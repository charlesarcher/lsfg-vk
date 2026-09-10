/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-layer/isolated_swapchain.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/ipc/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

namespace lsfgvk::layer {
namespace {
    std::atomic<uintptr_t> nextHandle{0x5AF50001};
    std::unordered_map<VkSwapchainKHR, IsolatedSwapchain> g_isolated;
    std::unordered_set<VkSwapchainKHR> g_tombstones;
    uint32_t g_signalFamily{~0u};
    uint32_t g_signalIndex{0};
    bool g_signalNoted{false};
}

void noteIsolatedSignalQueue(uint32_t family, uint32_t index) {
    if (std::getenv("LSFGVK_NO_EXTRA_Q") && std::getenv("LSFGVK_NO_EXTRA_Q")[0] == '1')
        return;
    g_signalFamily = family;
    g_signalIndex = index;
    g_signalNoted = true;
}

bool getIsolatedSignalQueue(uint32_t& family, uint32_t& index) {
    if (!g_signalNoted)
        return false;
    family = g_signalFamily;
    index = g_signalIndex;
    return true;
}

namespace {
int openRenderFdForDevice(const vk::Vulkan& vk) {
    VkPhysicalDeviceDrmPropertiesEXT drmProp{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT
    };
    VkPhysicalDeviceProperties2 props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drmProp
    };
    vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);
    if (!drmProp.hasRender)
        return -1;
    DIR* dir = ::opendir("/dev/dri");
    if (!dir)
        return -1;
    int out = -1;
    while (dirent* e = ::readdir(dir)) {
        if (std::strncmp(e->d_name, "renderD", 7) != 0)
            continue;
        const std::string path = std::string("/dev/dri/") + e->d_name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0)
            continue;
        if (static_cast<int>(gnu_dev_major(st.st_rdev)) == drmProp.renderMajor
                && static_cast<int>(gnu_dev_minor(st.st_rdev)) == drmProp.renderMinor) {
            out = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            break;
        }
    }
    ::closedir(dir);
    return out;
}

int allocExplicitVramDmaBuf(const vk::Vulkan& vk, uint64_t size) {
    static amdgpu_device_handle adev = nullptr;
    if (!adev) {
        const int dfd = openRenderFdForDevice(vk);
        if (dfd < 0)
            return -1;
        uint32_t maj = 0, min = 0;
        if (amdgpu_device_initialize(dfd, &maj, &min, &adev) != 0 || !adev) {
            ::close(dfd);
            adev = nullptr;
            return -1;
        }
    }
    amdgpu_bo_alloc_request req{};
    req.alloc_size = size;
    req.phys_alignment = 4096;
    req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
    req.flags = AMDGPU_GEM_CREATE_EXPLICIT_SYNC;
    amdgpu_bo_handle bo{};
    if (amdgpu_bo_alloc(adev, &req, &bo) != 0)
        return -1;
    uint32_t rawFd = 0;
    if (amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, &rawFd) != 0)
        return -1;
    return static_cast<int>(rawFd);
}
} // namespace

bool isolatedSwapchainEnabled() {
    const char* e = std::getenv("LSFGVK_ISOLATED_SWAPCHAIN");
    if (e && e[0] == '0' && e[1] == '\0')
        return false;
    return true; // default ON — this is the Windows-LS contract
}

VkSwapchainKHR allocIsolatedHandle() {
    return reinterpret_cast<VkSwapchainKHR>(nextHandle.fetch_add(1, std::memory_order_relaxed));
}

bool isIsolated(VkSwapchainKHR handle) {
    return g_isolated.find(handle) != g_isolated.end();
}

bool isIsolatedTombstone(VkSwapchainKHR handle) {
    return g_tombstones.find(handle) != g_tombstones.end();
}

IsolatedSwapchain& isolatedAt(VkSwapchainKHR handle) {
    return g_isolated.at(handle);
}

void destroyIsolated(const vk::Vulkan& vk, VkSwapchainKHR handle) {
    auto it = g_isolated.find(handle);
    if (it == g_isolated.end())
        return;
    if (it->second.icdAcqSem != VK_NULL_HANDLE)
        vk.df().DestroySemaphore(vk.dev(), it->second.icdAcqSem, nullptr);
    for (int fd : it->second.exportFds)
        if (fd >= 0)
            ::close(fd);
    g_isolated.erase(it);
    g_tombstones.insert(handle);
}

IsolatedSwapchain createIsolated(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& info) {
    IsolatedSwapchain iso;
    iso.format = info.imageFormat;
    iso.extent = info.imageExtent;
    uint32_t count = 5u;
    const bool exportIsolated = std::getenv("LSFGVK_EXPORT_ISOLATED")
        && std::getenv("LSFGVK_EXPORT_ISOLATED")[0] == '1';
    if (exportIsolated)
        count = static_cast<uint32_t>(ls::ipc::STAGING_RING_DEPTH);
    const VkImageUsageFlags usage = info.imageUsage
        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT;
    iso.images.reserve(count);
    iso.handles.reserve(count);
    iso.recycleFences.reserve(count);
    std::vector<uint32_t> shareFams{ vk.queueFamilyIndex() };
    if (g_signalNoted && g_signalFamily != vk.queueFamilyIndex())
        shareFams.push_back(g_signalFamily);
    const VkSharingMode sharing = shareFams.size() > 1
        ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    vk::ImageLayout lay{};
    if (exportIsolated) {
        lay.mode = vk::ImageMode::Linear;
        lay.hostVisible = true;
    }
    const uint32_t pitch = (info.imageExtent.width * 4u + 255u) / 256u * 256u;
    const uint64_t gemBytes =
        (static_cast<uint64_t>(pitch) * info.imageExtent.height + 4095ull) & ~4095ull;
    int nExplicit = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int gemFd = -1;
        if (!exportIsolated)
            gemFd = allocExplicitVramDmaBuf(vk, gemBytes);
        if (gemFd >= 0) {
            try {
                vk::ImageLayout expl{};
                expl.mode = vk::ImageMode::Linear;
                expl.rowPitch = pitch;
                const int imp = ::dup(gemFd);
                ::close(gemFd);
                gemFd = -1;
                if (imp < 0)
                    throw ls::error("dup isolated explicit fd failed");
                iso.images.emplace_back(vk, info.imageExtent, info.imageFormat, usage,
                    imp, std::nullopt, expl, sharing, shareFams);
                ++nExplicit;
            } catch (const std::exception& e) {
                if (gemFd >= 0)
                    ::close(gemFd);
                std::cerr << "lsfg-vk: isolated explicit-sync import failed: "
                    << e.what() << "\n";
                iso.images.emplace_back(vk, info.imageExtent, info.imageFormat, usage,
                    std::nullopt, std::nullopt, lay, sharing, shareFams);
            }
        } else {
            iso.images.emplace_back(vk, info.imageExtent, info.imageFormat, usage,
                std::nullopt, std::nullopt, lay, sharing, shareFams);
        }
        iso.handles.push_back(iso.images.back().handle());
        iso.recycleFences.emplace_back(vk, true);
        if (exportIsolated) {
            auto exp = iso.images.back().exportDmaBuf(vk);
            iso.exportFds.push_back(exp.fd);
        }
    }
    if (nExplicit > 0)
        std::cerr << "lsfg-vk: isolated explicit-sync images " << nExplicit
            << "/" << count << "\n";

    VkQueue q{VK_NULL_HANDLE};
    if (g_signalNoted)
        vk.df().GetDeviceQueue(vk.dev(), g_signalFamily, g_signalIndex, &q);
    iso.signalQueue = q;
    const VkSemaphoreCreateInfo sci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (vk.df().CreateSemaphore(vk.dev(), &sci, nullptr, &iso.icdAcqSem) != VK_SUCCESS)
        iso.icdAcqSem = VK_NULL_HANDLE;
    const bool dedicated = q != VK_NULL_HANDLE && q != vk.queue();
    if (!dedicated)
        std::cerr << "lsfg-vk: isolated swapchain " << count << " images "
                  << info.imageExtent.width << "x" << info.imageExtent.height
                  << " FALLBACK render queue (noted=" << g_signalNoted
                  << " fam=" << g_signalFamily << " idx=" << g_signalIndex << ")\n";
    else
        std::cerr << "lsfg-vk: isolated swapchain " << count << " images "
                  << info.imageExtent.width << "x" << info.imageExtent.height
                  << " DEDICATED fam=" << g_signalFamily << " idx=" << g_signalIndex << "\n";
    return iso;
}

void storeIsolated(VkSwapchainKHR handle, IsolatedSwapchain iso) {
    g_isolated.emplace(handle, std::move(iso));
}

void isolatedSignal(const vk::Vulkan& vk, VkQueue signalQueue, VkQueue fallback,
        VkSemaphore semaphore, VkFence fence) {
    VkQueue q = signalQueue != VK_NULL_HANDLE ? signalQueue : fallback;
    if (q == VK_NULL_HANDLE)
        throw ls::error("isolatedSignal: no queue");
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 0,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = semaphore != VK_NULL_HANDLE ? 1u : 0u,
        .pSignalSemaphores = semaphore != VK_NULL_HANDLE ? &semaphore : nullptr,
    };
    const auto res = vk.df().QueueSubmit(q, 1, &submit, fence);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "isolatedSignal vkQueueSubmit failed");
}

} // namespace lsfgvk::layer
