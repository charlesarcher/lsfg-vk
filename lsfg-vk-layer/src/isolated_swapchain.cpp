/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-layer/isolated_swapchain.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

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

void destroyIsolated(const vk::Vulkan& /*vk*/, VkSwapchainKHR handle) {
    auto it = g_isolated.find(handle);
    if (it == g_isolated.end())
        return;
    g_isolated.erase(it);
    g_tombstones.insert(handle);
}

IsolatedSwapchain createIsolated(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& info) {
    IsolatedSwapchain iso;
    iso.format = info.imageFormat;
    iso.extent = info.imageExtent;
    const uint32_t count = std::max(3u, info.minImageCount + 1);
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
    for (uint32_t i = 0; i < count; ++i) {
        iso.images.emplace_back(vk, info.imageExtent, info.imageFormat, usage,
            std::nullopt, std::nullopt, vk::ImageLayout{}, sharing, shareFams);
        iso.handles.push_back(iso.images.back().handle());
        iso.recycleFences.emplace_back(vk, true); // SIGNALED: first acquires pass
    }

    VkQueue q{VK_NULL_HANDLE};
    if (g_signalNoted)
        vk.df().GetDeviceQueue(vk.dev(), g_signalFamily, g_signalIndex, &q);
    iso.signalQueue = q;
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
