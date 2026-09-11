/* SPDX-License-Identifier: GPL-3.0-or-later */

// task 8: external presentation output for lsfg-vk-app.
//
// The app is a headless receiver, so its display is the X server running under
// Xwayland. This builds a SurfaceBackend (X11/xcb), creates a borderless
// no-focus window on the selected output, a VkSurfaceKHR for it, and a FIFO
// color-attachment swapchain on the processing (graphical) transport vk. Then,
// per accepted FRAME, it drives the frame-gen backend and presents both the
// generated frames and the real captured game frame into that swapchain.
//
// The present choreography mirrors the layer's swapchain.cpp present loop
// (489-611): per FRAME, backend.scheduleFrames(ctx, captureFd) returns one sync
// fd per destination image; each is imported into a binary semaphore, waited on,
// then a swapchain image is acquired, the destination image is blitted into it
// and presented; finally the latest captured source image (the real game frame)
// is blitted into a swapchain image and presented. FIFO present mode is always
// used. All WSI handles are LOCALS torn down on every exit path.

// glibc keeps struct sigaction / sigemptyset behind __USE_POSIX; not needed here
// but harmless. We only need poll/errno/close/getenv which are exposed.
#include "lsfg-vk-app/presentation.hpp"
#include "gui.hpp"

#include "lsfg-vk-app/hud.hpp"
#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-app/swizzle_spv.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/descriptor_pool.hpp"
#include "lsfg-vk-common/vulkan/descriptor_set.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/sampler.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/shader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <time.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <drm/amdgpu_drm.h>
#include <vulkan/vulkan_core.h>

namespace ls::presentation {
namespace {
    /// TEMP DEBUG: elapsed-ms probe (app start) for stall localization. gated
/// on LSFGVK_APP_DBG so the default stream stays clean.
const std::chrono::steady_clock::time_point g_dbgT0 = std::chrono::steady_clock::now();
bool dbgEnabled() {
    return std::getenv("LSFGVK_APP_DBG") != nullptr;
}
using Clock = std::chrono::steady_clock;
using Usec = std::chrono::microseconds;
static inline long long elapsedUs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<Usec>(b - a).count();
}
void dbg(const char* fmt, ...) {
    if (!dbgEnabled())
        return;
    char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_dbgT0).count();
        std::fprintf(stderr, "lsfg-vk-app: [dbg] %s (t+%lld ms)\n", buf, ms);
    }
    /// clamp v into [lo, hi] (mirrors the main.cpp swapchain-clamp helper).
    uint32_t clamp32(uint32_t v, uint32_t lo, uint32_t hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// build a single VkImageMemoryBarrier for a blit pass: a layout transition
    /// on a single-color image (mirrors the layer's barrierHelper).
    VkImageMemoryBarrier makeBlitBarrier(VkImage image, VkAccessFlags oldAccess,
            VkImageLayout oldLayout, VkAccessFlags newAccess, VkImageLayout newLayout,
            uint32_t srcQueueFamily = VK_QUEUE_FAMILY_IGNORED,
            uint32_t dstQueueFamily = VK_QUEUE_FAMILY_IGNORED) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = oldAccess;
        b.dstAccessMask = newAccess;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = srcQueueFamily;
        b.dstQueueFamilyIndex = dstQueueFamily;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        return b;
    }

    int existingRenderFd();

    /// Fallback if Vulkan import of the render GPU dma-buf fails.
    int primeReexportOnOffload(int foreignFd) {
        static int drmFd = -2;
        if (drmFd == -2)
            drmFd = existingRenderFd();
        if (drmFd < 0 || foreignFd < 0)
            return -1;
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(drmFd, foreignFd, &handle) != 0) {
            dbg("prime FDToHandle errno=%d fd=%d", errno, foreignFd);
            return -1;
        }
        int localFd = -1;
        if (drmPrimeHandleToFD(drmFd, handle, O_CLOEXEC | O_RDWR, &localFd) != 0) {
            dbg("prime HandleToFD errno=%d handle=%u", errno, handle);
            (void)drmCloseBufferHandle(drmFd, handle);
            return -1;
        }
        (void)drmCloseBufferHandle(drmFd, handle);
        dbg("prime reexport foreign=%d -> local=%d handle=%u", foreignFd, localFd, handle);
        return localFd;
    }

    int existingRenderFd() {
        DIR* dir = ::opendir("/proc/self/fd");
        if (!dir)
            return -1;
        int found = -1;
        while (dirent* e = ::readdir(dir)) {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
            char link[128]{};
            const ssize_t n = ::readlink(path, link, sizeof(link) - 1);
            if (n > 0 && std::strstr(link, "renderD") != nullptr) {
                found = std::atoi(e->d_name);
                if (found > 2)
                    break;
            }
        }
        ::closedir(dir);
        return found;
    }

    // Keep the offload import in GTT so each DMA does not re-place the buffer.
    // Uses this process's existing render node (same GEM the Vulkan device imported).
    void pinImportedGtt(int dmaFd) {
        if (dmaFd < 0)
            return;
        const int drm = existingRenderFd();
        if (drm < 0) {
            dbg("pin gtt: no render node fd in this process");
            return;
        }
        drm_prime_handle ph{};
        ph.fd = dmaFd;
        if (::drmIoctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
            dbg("pin gtt: FD_TO_HANDLE errno=%d drm=%d", errno, drm);
            return;
        }
        drm_amdgpu_gem_op op{};
        op.handle = ph.handle;
        op.op = AMDGPU_GEM_OP_SET_PLACEMENT;
        op.value = AMDGPU_GEM_DOMAIN_GTT;
        const int pr = ::drmIoctl(drm, DRM_IOCTL_AMDGPU_GEM_OP, &op);
        drm_amdgpu_gem_create_in info{};
        drm_amdgpu_gem_op q{};
        q.handle = ph.handle;
        q.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
        q.value = reinterpret_cast<uint64_t>(&info);
        const int ir = ::drmIoctl(drm, DRM_IOCTL_AMDGPU_GEM_OP, &q);
        dbg("pin gtt drm=%d handle=%u set=%d errno=%d get=%d domains=0x%llx flags=0x%llx explicit=%d",
            drm, ph.handle, pr, pr != 0 ? errno : 0, ir,
            static_cast<unsigned long long>(info.domains),
            static_cast<unsigned long long>(info.domain_flags),
            !!(info.domain_flags & AMDGPU_GEM_CREATE_EXPLICIT_SYNC));
        // Do not GEM_CLOSE: this handle is RADV's.
    }

    void logDmaGemFlags(int dmaFd) {
        static bool once{false};
        if (once || dmaFd < 0)
            return;
        once = true;
        const int drm = existingRenderFd();
        if (drm < 0) {
            dbg("gem flags: no render node fd in this process");
            return;
        }
        drm_prime_handle ph{};
        ph.fd = dmaFd;
        if (::drmIoctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
            dbg("gem flags: FD_TO_HANDLE errno=%d", errno);
            return;
        }
        drm_amdgpu_gem_create_in info{};
        drm_amdgpu_gem_op op{};
        op.handle = ph.handle;
        op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
        op.value = reinterpret_cast<uint64_t>(&info);
        const int ir = ::drmIoctl(drm, DRM_IOCTL_AMDGPU_GEM_OP, &op);
        dbg("gem flags rc=%d domains=0x%llx flags=0x%llx explicit=%d uncached=%d coherent=%d uswc=%d",
            ir,
            static_cast<unsigned long long>(info.domains),
            static_cast<unsigned long long>(info.domain_flags),
            !!(info.domain_flags & AMDGPU_GEM_CREATE_EXPLICIT_SYNC),
            !!(info.domain_flags & AMDGPU_GEM_CREATE_UNCACHED),
            !!(info.domain_flags & AMDGPU_GEM_CREATE_COHERENT),
            !!(info.domain_flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC));
        // Do not GEM_CLOSE or close(drm): this is the Vulkan device's fd.
    }

    /// import a sync fd into a binary semaphore as a temporary payload. on
    /// success the fd is consumed by the implementation, on failure it is closed
    /// before throwing (exact body of the layer's swapchain.cpp:57-73 importSyncFd).
    void importSyncFd(const vk::Vulkan& vk, VkSemaphore semaphore, int fd) {
        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fd
        };
        const auto res = vk.df().ImportSemaphoreFdKHR(vk.dev(), &importInfo);
        if (res != VK_SUCCESS) {
            close(fd);
            throw ls::vulkan_error(res, "vkImportSemaphoreFdKHR() failed");
        }
    }

    /// whether verbose per-cycle logging is requested (the -v hook: main sets
    /// LSFGVK_APP_VERBOSE when -v is passed, since runPresent carries no flag).
    bool verboseEnabled() {
        return std::getenv("LSFGVK_APP_VERBOSE") != nullptr;
    }

    struct DmaHopTs {
        VkQueryPool pool{VK_NULL_HANDLE};
        float periodNs{1.0f};
        PFN_vkGetCalibratedTimestampsEXT getCal{nullptr};
    };
    DmaHopTs g_dmaHopTs{};

    bool sampleCalDevice(const vk::Vulkan& dvk, uint64_t& deviceTs) {
        if (!g_dmaHopTs.getCal)
            return false;
        const VkCalibratedTimestampInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
            .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT,
        };
        uint64_t ts = 0;
        uint64_t maxDev = 0;
        const VkResult r = g_dmaHopTs.getCal(dvk.dev(), 1, &info, &ts, &maxDev);
        if (r != VK_SUCCESS)
            return false;
        deviceTs = ts;
        return true;
    }

    /// Record TOP/BOTTOM timestamps around `record`, submit on the dma-in
    /// queue, log wall / copy / park. Fence must be signaled on entry.
    template<typename Record>
    void submitDmaTimed(ls::ipc::StreamState& state, const char* tag, Record&& record) {
        auto& dvk = *state.dmaVk;
        auto& cb = *state.dmaCbs.at(0);
        auto& fence = *state.dmaFences.at(0);
        if (!fence.wait(dvk, 50ULL * 1000 * 1000)) {
            dbg("dma-in probe %s: fence wait failed before submit", tag);
            return;
        }
        fence.reset(dvk);
        cb.begin(dvk);
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            cb.resetQueryPool(dvk, g_dmaHopTs.pool, 0, 2);
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            dvk.df().CmdWriteTimestamp(cb.handle(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_dmaHopTs.pool, 0);
        record(dvk, cb);
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            dvk.df().CmdWriteTimestamp(cb.handle(),
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_dmaHopTs.pool, 1);
        cb.end(dvk);
        VkQueue q = dvk.hasTransferQueue() ? dvk.transferQueueHandle() : dvk.queue();
        cb.submit(dvk, {}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0,
            fence.handle(), q);
        const auto t0 = Clock::now();
        uint64_t calDev0 = 0;
        const bool haveCal0 = sampleCalDevice(dvk, calDev0);
        (void)fence.wait(dvk, 50ULL * 1000 * 1000);
        const auto t1 = Clock::now();
        uint64_t qv[2] = {0, 0};
        bool haveQ = false;
        if (g_dmaHopTs.pool != VK_NULL_HANDLE) {
            try {
                haveQ = cb.getQueryPoolResults(dvk, g_dmaHopTs.pool, 0, 2, qv, true);
            } catch (const std::exception& e) {
                dbg("dma-in probe %s timestamp read failed: %s", tag, e.what());
            }
        }
        const double wallMs = elapsedUs(t0, t1) / 1000.0;
        double copyMs = -1.0;
        double parkMs = -1.0;
        if (haveQ) {
            copyMs = (static_cast<double>(qv[1]) - static_cast<double>(qv[0]))
                * static_cast<double>(g_dmaHopTs.periodNs) / 1.0e6;
            if (haveCal0) {
                parkMs = (static_cast<double>(qv[0]) - static_cast<double>(calDev0))
                    * static_cast<double>(g_dmaHopTs.periodNs) / 1.0e6;
            } else {
                parkMs = wallMs - copyMs;
            }
        }
        dbg("dma-in probe %s wall %.3f ms exec %.3f ms park %.3f ms cal0=%d",
            tag, wallMs, copyMs, parkMs, haveCal0);
    }

    /// First-use discriminator on ctx=3: no imported dma-buf. Empty IB vs
    /// local vkCmdCopyImage. If empty parks ~33 ms, the hop delay is not
    /// the share. If empty is fast and local copy is fast, the share is.
    void probeDmaInQueue(ls::ipc::StreamState& state, VkExtent2D ext, VkFormat fmt) {
        auto& dvk = *state.dmaVk;
        const uint32_t dstQ = dvk.hasTransferQueue()
            ? dvk.transferQueueFamilyIndex() : dvk.queueFamilyIndex();
        auto empty = [](const vk::Vulkan&, vk::CommandBuffer&) {};
        submitDmaTimed(state, "empty-1", empty);
        submitDmaTimed(state, "empty-2", empty);
        submitDmaTimed(state, "empty-3", empty);
        try {
            const vk::ImageLayout lay{};
            vk::Image src(dvk, {1, 1}, fmt,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                std::nullopt, std::nullopt, lay);
            vk::Image dst(dvk, {1, 1}, fmt,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                std::nullopt, std::nullopt, lay);
            auto copy1 = [&](const vk::Vulkan& vk, vk::CommandBuffer& cb) {
                cb.copyImage(vk,
                    {
                        makeBlitBarrier(src.handle(),
                            VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                        makeBlitBarrier(dst.handle(),
                            VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                    },
                    { src.handle(), dst.handle() },
                    {1, 1},
                    {});
                (void)dstQ;
            };
            submitDmaTimed(state, "local-1x1-copy-1", copy1);
            submitDmaTimed(state, "local-1x1-copy-2", copy1);
            vk::Image srcF(dvk, ext, fmt,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                std::nullopt, std::nullopt, lay);
            vk::Image dstF(dvk, ext, fmt,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                std::nullopt, std::nullopt, lay);
            auto copyF = [&](const vk::Vulkan& vk, vk::CommandBuffer& cb) {
                cb.copyImage(vk,
                    {
                        makeBlitBarrier(srcF.handle(),
                            VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                        makeBlitBarrier(dstF.handle(),
                            VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                    },
                    { srcF.handle(), dstF.handle() },
                    ext,
                    {});
            };
            submitDmaTimed(state, "local-full-copy-1", copyF);
            submitDmaTimed(state, "local-full-copy-2", copyF);
            submitDmaTimed(state, "empty-after-local", empty);
        } catch (const std::exception& e) {
            dbg("dma-in probe local copy failed: %s", e.what());
        }
    }

    bool ensureDmaIn(ls::ipc::StreamState& state, const vk::Vulkan& presentVk) {
        if (state.dmaVk)
            return true;
        try {
            const auto want = presentVk.deviceUUID();
            auto select = [want](const vk::VulkanInstanceFuncs& fi,
                    const std::vector<VkPhysicalDevice>& devs) -> VkPhysicalDevice {
                for (auto pd : devs) {
                    VkPhysicalDeviceIDProperties id{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
                    };
                    VkPhysicalDeviceProperties2 p{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                        .pNext = &id
                    };
                    fi.GetPhysicalDeviceProperties2(pd, &p);
                    std::array<uint8_t, 16> uuid{};
                    std::memcpy(uuid.data(), id.deviceUUID, 16);
                    if (uuid == want)
                        return pd;
                }
                throw ls::error("dma-in device: offload GPU not found");
            };
            ::setenv("DISABLE_VK_LAYER_LSFGVK_frame_generation", "1", 1);
            state.dmaVk = std::make_unique<vk::Vulkan>(
                "lsfg-dma-in", vk::version{2, 0, 0},
                "lsfg-dma-in", vk::version{2, 0, 0},
                select, false, std::nullopt, std::nullopt, true, true);
            ::unsetenv("DISABLE_VK_LAYER_LSFGVK_frame_generation");
            auto& dvk = *state.dmaVk;
            const VkCommandPool pool = dvk.hasTransferQueue()
                ? dvk.transferCmdPoolHandle() : VK_NULL_HANDLE;
            for (size_t i = 0; i < ls::ipc::STAGING_RING_DEPTH; ++i) {
                state.dmaCbs.at(i).emplace(dvk, pool);
                state.dmaFences.at(i).emplace(dvk, true);
            }
            {
                VkPhysicalDeviceProperties2 props{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                };
                dvk.fi().GetPhysicalDeviceProperties2(dvk.physdev(), &props);
                g_dmaHopTs.periodNs = props.properties.limits.timestampPeriod;
                const VkQueryPoolCreateInfo qi{
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = 2,
                };
                const auto qres = dvk.df().CreateQueryPool(dvk.dev(), &qi,
                    VK_NULL_HANDLE, &g_dmaHopTs.pool);
                if (qres != VK_SUCCESS)
                    g_dmaHopTs.pool = VK_NULL_HANDLE;
                g_dmaHopTs.getCal = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                    dvk.fi().GetDeviceProcAddr(dvk.dev(), "vkGetCalibratedTimestampsEXT"));
                dbg("dma-in timestamps period %.3f ns/tick pool=%d cal=%d",
                    g_dmaHopTs.periodNs,
                    g_dmaHopTs.pool != VK_NULL_HANDLE,
                    g_dmaHopTs.getCal != nullptr);
            }
            dbg("dma-in device ready (no gfx)");
            return true;
        } catch (const std::exception& e) {
            ::unsetenv("DISABLE_VK_LAYER_LSFGVK_frame_generation");
            dbg("dma-in device failed: %s", e.what());
            state.dmaVk.reset();
            return false;
        }
    }

    /// primary render GPU share → offload VRAM on a device with no gfx. returns dma-buf of
    /// that VRAM image for the present device to sample. -1 on failure.
    int hopShareToOffload(ls::ipc::StreamState& state, uint32_t sidx, int shareFd) {
        if (!state.dmaVk || shareFd < 0)
            return -1;
        logDmaGemFlags(shareFd);
        // Capture-done fd was already polled by the caller. Do not also poll
        // DMA_BUF_IOCTL_EXPORT_SYNC_FILE WRITE here: that wait is extra gfx
        // fences on the share (~31 ms) and couples DMA-in to render-GPU gfx.
        auto& dvk = *state.dmaVk;
        const VkExtent2D ext{ state.width, state.height };
        static bool probed = false;
        if (!probed) {
            probed = true;
            probeDmaInQueue(state, ext, state.captureFormat);
        }
        const vk::ImageLayout shareLay{
            .mode = (state.negotiatedModifier == vk::EXCHANGE_MODIFIER_LINEAR)
                ? vk::ImageMode::Linear : vk::ImageMode::DrmModifier,
            .drmModifier = state.negotiatedModifier,
            .rowPitch = state.rowPitch,
        };
        if (!state.dmaSrc.at(sidx)) {
            const int imp = ::dup(shareFd);
            if (imp < 0)
                return -1;
            try {
                state.dmaSrc.at(sidx).emplace(dvk, ext, state.captureFormat,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, imp, std::nullopt, shareLay);
            } catch (const std::exception& e) {
                dbg("dma-in import share failed: %s", e.what());
                return -1;
            }
        }
        if (!state.dmaDst.at(sidx)) {
            try {
                const vk::ImageLayout vramLay{ .mode = vk::ImageMode::Linear };
                state.dmaDst.at(sidx).emplace(dvk, ext, state.captureFormat,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    std::nullopt, std::nullopt, vramLay);
                auto exp = state.dmaDst.at(sidx)->exportDmaBuf(dvk);
                state.dmaDstFds.at(sidx) = exp.fd;
            } catch (const std::exception& e) {
                dbg("dma-in vram dest failed: %s", e.what());
                return -1;
            }
        }
        if (!state.dmaCbs.at(sidx) || !state.dmaFences.at(sidx))
            return -1;
        auto& cb = *state.dmaCbs.at(sidx);
        auto& fence = *state.dmaFences.at(sidx);
        if (!fence.wait(dvk, 0))
            return -1;
        fence.reset(dvk);
        const uint32_t dstQ = dvk.hasTransferQueue()
            ? dvk.transferQueueFamilyIndex() : dvk.queueFamilyIndex();
        cb.begin(dvk);
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            cb.resetQueryPool(dvk, g_dmaHopTs.pool, 0, 2);
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            dvk.df().CmdWriteTimestamp(cb.handle(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_dmaHopTs.pool, 0);
        cb.copyImage(dvk,
            {
                makeBlitBarrier(state.dmaSrc.at(sidx)->handle(),
                    VK_ACCESS_NONE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_EXTERNAL, dstQ),
                makeBlitBarrier(state.dmaDst.at(sidx)->handle(),
                    VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
            },
            { state.dmaSrc.at(sidx)->handle(), state.dmaDst.at(sidx)->handle() },
            ext,
            {});
        if (g_dmaHopTs.pool != VK_NULL_HANDLE)
            dvk.df().CmdWriteTimestamp(cb.handle(),
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_dmaHopTs.pool, 1);
        cb.end(dvk);
        VkQueue q = dvk.hasTransferQueue() ? dvk.transferQueueHandle() : dvk.queue();
        cb.submit(dvk, {}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0,
            fence.handle(), q);
        const auto t0 = Clock::now();
        uint64_t calDev0 = 0;
        const bool haveCal0 = sampleCalDevice(dvk, calDev0);
        (void)fence.wait(dvk, 50ULL * 1000 * 1000);
        const auto t1 = Clock::now();
        uint64_t calDev1 = 0;
        const bool haveCal1 = sampleCalDevice(dvk, calDev1);
        uint64_t qv[2] = {0, 0};
        bool haveQ = false;
        if (g_dmaHopTs.pool != VK_NULL_HANDLE) {
            try {
                haveQ = cb.getQueryPoolResults(dvk, g_dmaHopTs.pool, 0, 2, qv, true);
            } catch (const std::exception& e) {
                dbg("dma-in timestamp read failed: %s", e.what());
            }
        }
        static int nHop = 0;
        if (nHop < 24) {
            const double wallMs = elapsedUs(t0, t1) / 1000.0;
            double copyMs = -1.0;
            double parkMs = -1.0;
            if (haveQ) {
                copyMs = (static_cast<double>(qv[1]) - static_cast<double>(qv[0]))
                    * static_cast<double>(g_dmaHopTs.periodNs) / 1.0e6;
                if (haveCal0) {
                    parkMs = (static_cast<double>(qv[0]) - static_cast<double>(calDev0))
                        * static_cast<double>(g_dmaHopTs.periodNs) / 1.0e6;
                } else {
                    parkMs = wallMs - copyMs;
                }
            }
            dbg("dma-in hop slot %u wall %.3f ms copy %.3f ms park %.3f ms "
                "q0=%llu q1=%llu cal0=%d cal1=%d",
                sidx, wallMs, copyMs, parkMs,
                static_cast<unsigned long long>(qv[0]),
                static_cast<unsigned long long>(qv[1]),
                haveCal0, haveCal1);
            ++nHop;
        }
        return state.dmaDstFds.at(sidx);
    }

    // Process-lifetime overlay. Recreating exclusive layer-shell on every
    // HELLO (RE2 1080→1080→1440) closes Xwayland :0 (XIO EBADF) and the
    // isolated game goes black. Keep WSI across streams; drop only on idle.
    struct OverlayDisplay {
        std::unique_ptr<ls::wsi::SurfaceBackend> wsi;
        VkSurfaceKHR surface{VK_NULL_HANDLE};
        VkSwapchainKHR swapchain{VK_NULL_HANDLE};
        VkExtent2D extent{};
        uint32_t imageFormat{};
        std::vector<VkImage> swapImages;
        const char* modeName{"MAILBOX"};
    };
    OverlayDisplay g_overlay;
} // namespace

void runPresent(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const vk::Vulkan& vk, lsfgvk::backend::Instance& backend,
        const ls::GameConf& conf, std::string_view session,
        const std::atomic<bool>& stop) {
    const uint32_t w = state.width, h = state.height;
    bool dropOverlay = false;

    auto ensureOverlayWsi = [&] {
    if (g_overlay.wsi)
        return;
    // --- surface backend + window/surface on the transport vk ----------------
    if (session == "wayland") {
        g_overlay.wsi = ls::wsi::createWaylandSurfaceBackend();
    } else {
        // default to X11 for "x11" or "auto" (XWayland)
        g_overlay.wsi = ls::wsi::createX11SurfaceBackend();
    }
    if (!g_overlay.wsi->connect(session))
        throw ls::error("could not connect the surface backend for session: " + std::string(session));

    if (verboseEnabled())
        std::cerr << "lsfg-vk-app: using " << (session == "wayland" ? "Wayland" : "X11") << " surface backend\n";

    // --- resolve the target output: an explicit output must match a connector
    //     exactly; absent/empty selects the primary/active output.
    const auto outputs = g_overlay.wsi->outputs();
    if (conf.output.has_value()) {
        bool found{false};
        for (const auto& out : outputs)
            if (out.name == *conf.output) { found = true; break; }
        if (!found) {
            std::string avail;
            for (const auto& out : outputs)
                avail += (avail.empty() ? std::string() : ", ") + out.name;
            throw ls::error("output '" + *conf.output + "' not found; available: " + avail);
        }
    }
    const std::string outputName = conf.output.has_value() ? *conf.output : std::string{};

    const auto handle = g_overlay.wsi->createWindow(outputName, VkExtent2D{ w, h }, 0u);
    VkSurfaceKHR surface = g_overlay.wsi->createSurface(vk, handle);

    // --- query caps, then pick an extent the surface will accept (> 0x0) -----
    VkSurfaceCapabilitiesKHR caps{};
    std::vector<VkColorSpaceKHR> colorspaces;
    g_overlay.wsi->surfaceCaps(surface, caps, colorspaces);

    // match the swapchain to the window size the compositor reports: on
    // Wayland a presented buffer smaller than the window implicitly resizes
    // the window every frame, which would tear the stream down. the stream
    // images are blit-scaled into the swapchain instead. 0x0 (no size report
    // yet) falls back to the stream size.
    VkExtent2D extent = g_overlay.wsi->windowExtent();
    if (extent.width == 0 || extent.height == 0)
        extent = VkExtent2D{ w, h };
    // only clamp to caps when the reported range is sane (min<=max, max>0);
    // RADV/XCB under XWayland can report a degenerate 0x0 max, so fall back to
    // the requested extent clamped to a sane [1, 16384] band (main.cpp:361-377).
    if (caps.minImageExtent.width <= caps.maxImageExtent.width
            && caps.maxImageExtent.width > 0 && caps.maxImageExtent.height > 0) {
        extent.width = clamp32(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp32(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    } else {
        extent.width = clamp32(extent.width, 1u, 16384u);
        extent.height = clamp32(extent.height, 1u, 16384u);
    }
    if (extent.width == 0 || extent.height == 0)
        throw ls::error("surface supported extent is 0x0");

    // --- derive the swapchain colorspace from the surface. The frozen Hello
    //     struct carries no colorspace, so we prefer SRGB_NONLINEAR (as the
    //     WSI smoke path hard-codes), else the first supported colorspace.
    VkColorSpaceKHR colorSpace{ VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    bool foundSrgb{false};
    for (const auto& cs : colorspaces)
        if (cs == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { foundSrgb = true; break; }
    if (!foundSrgb)
        colorSpace = colorspaces.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                                         : colorspaces.front();

    // --- pick a compositing alpha: OPAQUE, else PRE/POST_MULTIPLIED.
    VkCompositeAlphaFlagBitsKHR compositingAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            compositingAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            compositingAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }

    // Overlay present must not be FIFO-paced at compositor-throttled ~20 Hz.
    // MAILBOX (else IMMEDIATE) matches Windows LS: the secondary GPU presents as fast
    // as GEN+REAL are ready, toward high refresh rates. FIFO stays the fallback.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    const char* modeName = "MAILBOX";
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getModes =
        vk.fi().GetPhysicalDeviceSurfacePresentModesKHR;
    if (getModes) {
        uint32_t n = 0;
        getModes(vk.physdev(), surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        if (n > 0)
            getModes(vk.physdev(), surface, &n, modes.data());
        auto has = [&](VkPresentModeKHR m) {
            return std::find(modes.begin(), modes.end(), m) != modes.end();
        };
        dbg("surface present modes (%u):%s%s%s%s",
            n,
            has(VK_PRESENT_MODE_FIFO_KHR) ? " FIFO" : "",
            has(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? " FIFO_RELAXED" : "",
            has(VK_PRESENT_MODE_MAILBOX_KHR) ? " MAILBOX" : "",
            has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? " IMMEDIATE" : "");
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            modeName = "MAILBOX";
        } else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            modeName = "IMMEDIATE";
        }
    }
    const uint32_t minImages = caps.minImageCount < 2 ? 3 : caps.minImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = minImages;
    ci.imageFormat = g_overlay.wsi->swapchainFormat(surface);
    ci.imageColorSpace = colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.pQueueFamilyIndices = nullptr;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = compositingAlpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    auto createRes = vk.df().CreateSwapchainKHR(vk.dev(), &ci, VK_NULL_HANDLE, &swapchain);
    if (createRes != VK_SUCCESS && presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        modeName = "FIFO-fallback";
        createRes = vk.df().CreateSwapchainKHR(vk.dev(), &ci, VK_NULL_HANDLE, &swapchain);
    }
    if (createRes != VK_SUCCESS)
        throw ls::vulkan_error(createRes, "CreateSwapchainKHR failed");

    dbg("swapchain extent %ux%u fmt=%u mode=%s minImages=%u (stream %ux%u)",
        extent.width, extent.height, ci.imageFormat, modeName, minImages, w, h);

    uint32_t imageCount{};
    if (vk.df().GetSwapchainImagesKHR(vk.dev(), swapchain, &imageCount, nullptr) != VK_SUCCESS)
        throw ls::vulkan_error("failed to enumerate swapchain images");
    std::vector<VkImage> swapImages(imageCount);
    if (vk.df().GetSwapchainImagesKHR(vk.dev(), swapchain, &imageCount, swapImages.data())
            != VK_SUCCESS)
        throw ls::vulkan_error("failed to enumerate swapchain images");
    g_overlay.surface = surface;
    g_overlay.swapchain = swapchain;
    g_overlay.extent = extent;
    g_overlay.imageFormat = ci.imageFormat;
    g_overlay.swapImages = std::move(swapImages);
    g_overlay.modeName = modeName;
    }; // ensureOverlayWsi
    const bool noFs = std::getenv("LSFGVK_APP_NO_FS") != nullptr;
    const bool boot1080 = (w == 1920 && h == 1080) && !noFs;
    const bool noWsi = std::getenv("LSFGVK_NO_OVERLAY_WSI")
        && std::getenv("LSFGVK_NO_OVERLAY_WSI")[0] == '1';
    if (g_overlay.wsi) {
        dbg("reusing overlay WSI %ux%u (stream %ux%u)",
            g_overlay.extent.width, g_overlay.extent.height, w, h);
    } else if (boot1080 || noWsi) {
        dbg("defer overlay WSI (boot1080=%d noWsi=%d stream %ux%u)",
            boot1080 ? 1 : 0, noWsi ? 1 : 0, w, h);
    } else {
        dbg("defer overlay WSI until first FRAME (stream %ux%u)", w, h);
    }
    VkSwapchainKHR swapchain = g_overlay.swapchain;
    auto& swapImages = g_overlay.swapImages;
    VkExtent2D extent = g_overlay.extent;

    // --- shared size: destination count used by the two-thread split below ----
    // per-frame work objects (command buffer, acquire/signal/done semaphores,
    // fences) are created inside the INPUT and OUTPUT thread scopes in the
    // Stage 2/3 block further down.
    const size_t destCount = state.destinationImages.size();

    // --- early-release snapshot path -----------------------------------------
    vk::Semaphore snapshotSem{vk, std::nullopt,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    vk::Semaphore frameReadySem{vk, std::nullopt,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    vk::Semaphore copyDone{ vk };

    std::unique_ptr<ls::hud::Hud> hud;
    auto makeHud = [&] {
        if (!hud && g_overlay.extent.height > 0)
            hud = std::make_unique<ls::hud::Hud>(vk, g_overlay.extent.height,
                static_cast<VkFormat>(g_overlay.imageFormat));
    };

    struct SwapchainGuard {
        const vk::Vulkan* vk;
        bool* drop;
        ~SwapchainGuard() {
            if (drop == nullptr || !*drop) {
                dbg("guard: keeping overlay WSI");
                return;
            }
            dbg("guard: dropping overlay WSI");
            if (g_overlay.swapchain != VK_NULL_HANDLE && vk != nullptr)
                vk->df().DestroySwapchainKHR(vk->dev(), g_overlay.swapchain, VK_NULL_HANDLE);
            g_overlay.swapchain = VK_NULL_HANDLE;
            g_overlay.swapImages.clear();
            if (g_overlay.wsi)
                g_overlay.wsi->destroy();
            g_overlay.wsi.reset();
            dbg("guard: overlay WSI dropped");
        }
    };
    SwapchainGuard guard{ &vk, &dropOverlay };

    // =========================================================================
    // Stage 2/3: split frame-PRODUCE (INPUT) from display-PRESENT (OUTPUT) onto
    // two threads so a game frame is never blocked on the app's display cadence
    // (FIFO/vblank backpressure), which was the ~31 ms selectFreeSlot stall.
    //
    //   INPUT : receive FRAME -> dup capture fd -> scheduleFrames -> snapshot
    //           copy -> wait snapshotSem (non-blocking poll) -> poll last doneFd -> send Release
    //           -> enqueue {doneFds, stagingIdx}. Owns the socket. Never touches
    //           WSI. NEVER waits on the OUTPUT (no display cadence coupling).
    //   OUTPUT: the swapchain present path. Per dequeued frame a [GEN]+[REAL]
    //           phase machine, one present per vblank (FIFO acquire paces it),
    //           drop-oldest/present-newest inbox, HOLD-LAST real when idle.
    //           Owns all WSI.
    //   Cross-thread: a bounded SPSC inbox (mutex+condvar+deque of depth 3-4),
    //   a shared submit mutex (VkQueue is externally synchronized — ALL queue
    //   submissions, including the backend's scheduleFrames submits, run under
    //   it, which orders dest[i] reads before the next write on the same queue
    //   and prevents concurrent vkQueueSubmit), and atomic frame/present
    //   counters. Signal sems are a fixed pool indexed by a rolling present
    //   counter (>= the FIFO in-flight depth), so reuse is always safe.
    //   De-coupling note: the INPUT must not wait on any fence the OUTPUT
    //   signals, or its Release rate (== the game's unblock rate) collapses to
    //   the output's present cadence (GEN+REAL = 2 vblanks/frame -> 30 fps at
    //   60 Hz), re-throttling the game. The fixed destinationImages ring gives
    //   the output "the newest generated content" (inherent), which at the 120 Hz
    //   target (matched 60 fps input/output) is exactly the per-frame gen.
    // =========================================================================

    // --- shared cross-thread state -------------------------------------------
    std::atomic<bool> failed{ false };
    std::atomic<bool> wantDropWsi{ false };
    std::exception_ptr inputError, outputError;

    // bounded SPSC inbox: INPUT pushes each fully-produced frame; OUTPUT pops
    // the newest (dropping older un-shown frames and closing their done fds).
    struct PendingFrame {
        std::vector<int> doneFds;   // one per destination (already produced)
        int snapFd{ -1 };           // sync_fd semaphore for the snapshot copy; -1 if no snapshot
        uint32_t stagingIdx{ 0 };
        uint64_t captureTsNs{ 0 };
    };
    struct Inbox {
        std::mutex m;
        std::condition_variable cv;
        std::deque<PendingFrame> q;
        // Blocking variant: waits on the cv until a frame arrives or the
        // timeout expires (timeout serves WSI event pumping only; frame
        // arrival wakes instantly - zero poll latency). Returns nullopt on
        // timeout.
        std::optional<PendingFrame> takeNewestWait(unsigned timeoutMs) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                [this] { return !q.empty(); });
            if (q.empty())
                return std::nullopt;
            return takeNewestLocked();
        }
        std::optional<PendingFrame> takeNewest() {
            std::lock_guard<std::mutex> lk(m);
            if (q.empty())
                return std::nullopt;
            return takeNewestLocked();
        }
        std::optional<PendingFrame> takeNewestLocked() {
            if (q.empty())
                return std::nullopt;
            PendingFrame newest = std::move(q.back());
            q.pop_back();
            for (auto& f : q) {
                for (int d : f.doneFds)
                    if (d >= 0)
                        ::close(d);
                if (f.snapFd >= 0)
                    ::close(f.snapFd);
            }
            q.clear();
            return PendingFrame{ std::move(newest) };
        }
        void push(PendingFrame f) {
            {
                std::lock_guard<std::mutex> lk(m);
                q.push_back(std::move(f));
            }
            cv.notify_one();
        }
    } inbox;

    // VkQueue must be externally synchronized across the two submit threads.
    // ALL submissions run under this lock, including the backend's
    // scheduleFrames submits, so dest[i] reads/writes on the same queue are
    // strictly ordered and never submitted concurrently from two threads.
    std::mutex submitMtx;
    std::atomic<uint64_t> frameCount{ 0 };
    std::atomic<uint64_t> presentedFrames{ 0 };
    using Clock = std::chrono::steady_clock;
    using Usec = std::chrono::microseconds;

    // WSI events must be pumped before blocking Vulkan calls (Wayland release /
    // configure events only reach RADV through display dispatch). output-only.
    auto processWsiEvents = [&](int timeoutMs = 0) {
        if (g_overlay.wsi)
            g_overlay.wsi->processEvents(timeoutMs);
    };

    const VkExtent2D imgExtent{ w, h };

    // blit the HUD box into the top-right of a just-filled swapchain image.
    // records into @cb (the caller owns the submit); @dstImage must be in
    // TRANSFER_DST_OPTIMAL with in-flight write access, and is left in that same
    // layout so the caller's post barrier transitions it to PRESENT_SRC.
    auto drawHud = [&](vk::CommandBuffer& cb, VkImage dstImage) {
        if (!hud)
            return;
        const VkImage hudImage = hud->image().handle();
        const VkImageMemoryBarrier hudBarrier = makeBlitBarrier(hudImage,
            hud->lastAccess(), VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        const VkImageMemoryBarrier dstBarrier = makeBlitBarrier(dstImage,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageMemoryBarrier barriers[2] = { hudBarrier, dstBarrier };
        vk.df().CmdPipelineBarrier(cb.raw(),
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 2, barriers);
        const VkExtent2D b = hud->box();
        // top-right corner: ORIGIN is the inset from the right and top edges
        const VkOffset2D o{
            static_cast<int32_t>(extent.width) - static_cast<int32_t>(b.width)
                - ls::hud::Hud::ORIGIN.x,
            ls::hud::Hud::ORIGIN.y,
        };
        const VkImageBlit blit{
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1
            },
            .srcOffsets = {
                { 0, 0, 0 },
                { static_cast<int32_t>(b.width), static_cast<int32_t>(b.height), 1 }
            },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1
            },
            .dstOffsets = {
                { static_cast<int32_t>(o.x), static_cast<int32_t>(o.y), 0 },
                { static_cast<int32_t>(o.x + b.width), static_cast<int32_t>(o.y + b.height), 1 }
            }
        };
        vk.df().CmdBlitImage(cb.raw(), hudImage, VK_IMAGE_LAYOUT_GENERAL,
            dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
            VK_FILTER_NEAREST);
        hud->markRead();
    };

    // --- OUTPUT thread: the whole swapchain present path ----------------------
    auto outputLoop = [&] {
        // command buffer ring: one per in-flight present (destCount + 1)
        // each has its own fence to know when the GPU is done with it
        const size_t cbRingSize = destCount + 2;
        std::vector<vk::CommandBuffer> cbs;
        std::vector<vk::Fence> cbFences;
        cbs.reserve(cbRingSize);
        cbFences.reserve(cbRingSize);
        for (size_t i = 0; i < cbRingSize; ++i) {
            cbs.emplace_back(vk);
            cbFences.emplace_back(vk, true);   // signaled: the first wait passes
        }
        size_t cbIdx = 0;
        vk::Semaphore acquireSem{ vk };   // never signaled; reused for every acquire
        std::vector<vk::Semaphore> doneWaitSem;   // destCount + 1 (extra for snapshot)
        doneWaitSem.reserve(destCount + 1);
        for (size_t i = 0; i < destCount + 1; ++i)
            doneWaitSem.emplace_back(vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        const size_t presentCount = destCount + 1;   // gen + real per frame
        const size_t signalPool = std::max<size_t>(presentCount + 1, 4);
        std::vector<vk::Semaphore> signalSem;        // rolling present pool
        signalSem.reserve(signalPool);
        for (size_t i = 0; i < signalPool; ++i)
            signalSem.emplace_back(vk);

        // FIFO acquire paces the thread at one present per vblank.
        constexpr uint64_t acquireTimeoutNs = 200ULL * 1000 * 1000;   // 200 ms
        auto acquireImage = [&](uint32_t& outIdx) -> bool {
            for (;;) {
                if (stop.load(std::memory_order_relaxed)
                        || failed.load(std::memory_order_relaxed))
                    return false;
                const auto res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                    acquireTimeoutNs, acquireSem.handle(), VK_NULL_HANDLE, &outIdx);
                if (res != VK_TIMEOUT) {
                    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                        throw ls::vulkan_error(res, "AcquireNextImageKHR failed");
                    return true;
                }
                processWsiEvents(0);   // may deliver the wl_buffer release
            }
        };

        // the frame the output is currently realizing on the display.
        struct Cur {
            bool active{ false };
            size_t nextDest{ 0 };      // 0..destCount: gen presents in flight
            uint32_t stagingIdx{ 0 };
            std::vector<int> doneFds;  // produced gen fds; -1 once imported
            int snapFd{ -1 };          // snapshot sync_fd sem for REAL blit
            uint64_t captureTsNs{ 0 };
        } cur;
        int lastShownStagingIdx{ -1 }; // newest real frame actually shown (HOLD-LAST)
        uint64_t presentIdx{ 0 };      // rolling index into the signal pool

        Clock::time_point statsLastTime = Clock::now();
        const auto statsInterval = std::chrono::seconds(1);
        // 1 Hz HUD/stats update from the output thread.
        auto maybeStats = [&](Clock::time_point now) {
            if (now - statsLastTime < statsInterval)
                return;
            const double dt = std::chrono::duration<double>(now - statsLastTime).count();
            const uint32_t gameFps = static_cast<uint32_t>(frameCount.exchange(0) / dt);
            const uint32_t presentedFps = static_cast<uint32_t>(presentedFrames.exchange(0) / dt);
            lsfgvk::gui::g_guiState.currentFpsReal.store(static_cast<float>(gameFps));
            lsfgvk::gui::g_guiState.currentFpsGen.store(static_cast<float>(presentedFps));
            lsfgvk::gui::g_guiState.streamActive.store(true);
            if (verboseEnabled())
                std::cerr << "lsfg-vk-app: " << gameFps << " fps game, "
                          << presentedFps << " fps presented\n";
            {
                // hud.update submits on the queue; serialize against the input thread.
                std::lock_guard<std::mutex> lk(submitMtx);
                try {
                    if (hud)
                        hud->update(std::to_string(gameFps) + "/" + std::to_string(presentedFps));
                } catch (const std::exception& e) {
                    std::cerr << "lsfg-vk-app: hud update failed: " << e.what() << "\n";
                }
            }
            statsLastTime = now;
        };

        // present one swapchain image that blits the private snapshot of the
        // given real frame into it (used for REAL presents and HOLD-LAST).
        auto presentReal = [&](int stagingIdx, int snapFd, uint64_t capTsNs = 0) -> bool {
            uint32_t idx{};
            const auto tReal0 = Clock::now();
            if (!acquireImage(idx)) {
                if (snapFd >= 0) ::close(snapFd);
                return false;
            }
            const auto tAcquire = Clock::now();
            const VkImage dstImage = swapImages.at(idx);
            auto& srcImage = state.genSources.at(stagingIdx);
            // wait for this command buffer's previous submit to complete
            if (!cbFences.at(cbIdx).wait(vk, UINT64_MAX))
                throw ls::vulkan_error(VK_TIMEOUT, "cb fence wait failed");
            cbFences.at(cbIdx).reset(vk);
            cbs.at(cbIdx).begin(vk);
            // wait on the snapshot sync_fd semaphore (ensures copyImage done)
            std::vector<VkSemaphore> realWaits{ acquireSem.handle() };
            if (snapFd >= 0) {
                const auto tImport0 = Clock::now();
                // import consumes the fd on success (ownership transfers) - do NOT close
                importSyncFd(vk, doneWaitSem.at(destCount).handle(), snapFd);
                const auto tImport1 = Clock::now();
                realWaits.push_back(doneWaitSem.at(destCount).handle());
                dbg("output: REAL importSyncFd %lld us", elapsedUs(tImport0, tImport1));
            }
            cbs.at(cbIdx).blitImage(vk,
                {
                    makeBlitBarrier(srcImage.mut().handle(),
                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                },
                { srcImage.mut().handle(), dstImage },
                extent,
                {
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                },
                imgExtent,
                VK_FILTER_LINEAR
            );
            drawHud(cbs.at(cbIdx), dstImage);
            const auto tBlitEnd = Clock::now();
            cbs.at(cbIdx).end(vk);
            {
                const auto tLock0 = Clock::now();
                std::lock_guard<std::mutex> lk(submitMtx);
                const auto tLock1 = Clock::now();
                cbs.at(cbIdx).submit(vk,
                    realWaits, VK_NULL_HANDLE, 0,
                    { signalSem.at(presentIdx % signalPool).handle() }, VK_NULL_HANDLE, 0,
                    cbFences.at(cbIdx).handle());
                const auto tSubmit1 = Clock::now();
                dbg("output: REAL acquire %lld us blit %lld us lock %lld us submit %lld us (total %lld us)",
                    elapsedUs(tReal0, tAcquire), elapsedUs(tAcquire, tBlitEnd),
                    elapsedUs(tLock0, tLock1), elapsedUs(tLock1, tSubmit1),
                    elapsedUs(tReal0, tSubmit1));
            }
            cbIdx = (cbIdx + 1) % cbRingSize;
            processWsiEvents(0);
            const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &signalSem.at(presentIdx % signalPool).handle(),
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &idx,
            };
            const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
            if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(pres, "QueuePresentKHR failed (real)");
            if (capTsNs > 0) {
                const uint64_t nowNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count());
                const double latMs = (nowNs > capTsNs) ? (nowNs - capTsNs) / 1e6 : 0.0;
                lsfgvk::gui::g_guiState.currentLatencyRealMs.store(static_cast<float>(latMs));
                dbg("MEASURED LATENCY: REAL present slot %d latency %.2f ms", stagingIdx, latMs);
            }
            ++presentIdx;
            ++presentedFrames;
            static uint32_t totalPresentCount = 0;
            ++totalPresentCount;
            static bool dumpedPresent = false;
            static uint32_t lastDumpedSec = 0;
            if (std::getenv("LSFGVK_DUMP_PRESENT")) {
                const uint32_t curSec = static_cast<uint32_t>(totalPresentCount / 200);
                if (curSec != lastDumpedSec && totalPresentCount >= 100) {
                    lastDumpedSec = curSec;
                    dumpedPresent = true;
                cbFences.at((cbIdx + cbRingSize - 1) % cbRingSize).wait(vk, UINT64_MAX);
                const size_t nb = static_cast<size_t>(extent.width) * extent.height * 4;
                void* p = nullptr;
                if (::posix_memalign(&p, 4096, nb) == 0) {
                    try {
                        vk::Image dumpImg(vk, extent, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            p, nb);
                        vk::CommandBuffer dcb(vk);
                        dcb.begin(vk);
                        dcb.copyImage(vk,
                            {
                                makeBlitBarrier(dstImage,
                                    VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                                makeBlitBarrier(dumpImg.handle(),
                                    VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                            },
                            { dstImage, dumpImg.handle() },
                            extent,
                            {
                                makeBlitBarrier(dstImage,
                                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                            });
                        dcb.end(vk);
                        vk::Fence df(vk, true);
                        df.reset(vk);
                        dcb.submit(vk, {}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0, df.handle());
                        df.wait(vk, UINT64_MAX);
                        if (FILE* out = std::fopen("/tmp/lsfg-doubler-presentation.ppm", "wb")) {
                            std::fprintf(out, "P6\n%u %u\n255\n", extent.width, extent.height);
                            const auto* px = static_cast<const uint8_t*>(p);
                            const size_t npx = static_cast<size_t>(extent.width) * extent.height;
                            std::vector<uint8_t> rgb(npx * 3);
                            for (size_t i = 0; i < npx; ++i) {
                                rgb[i * 3 + 0] = px[i * 4 + 0];
                                rgb[i * 3 + 1] = px[i * 4 + 1];
                                rgb[i * 3 + 2] = px[i * 4 + 2];
                            }
                            std::fwrite(rgb.data(), 1, rgb.size(), out);
                            std::fclose(out);
                            char named[128];
                            std::snprintf(named, sizeof(named), "/tmp/lsfg-doubler-%u.ppm", lastDumpedSec);
                            if (FILE* out2 = std::fopen(named, "wb")) {
                                std::fprintf(out2, "P6\n%u %u\n255\n", extent.width, extent.height);
                                std::fwrite(rgb.data(), 1, rgb.size(), out2);
                                std::fclose(out2);
                            }
                            std::cerr << "lsfg-vk-app: dumped full presentation swapchain image with HUD to /tmp/lsfg-doubler-presentation.ppm\n";
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "lsfg-vk-app: present dump failed: " << e.what() << "\n";
                    }
                    ::free(p);
                }
                }
            }
            return true;
        };

        try {
            for (;;) {
                if (stop.load(std::memory_order_relaxed)
                        || failed.load(std::memory_order_relaxed))
                    break;

                // realize a freshly-produced frame (drop-oldest/newest).
                // Session 13.21: when idle this BLOCKS on the inbox cv (wakes
                // instantly on arrival; 15 ms cap only pumps WSI events). The
                // taken frame is fully consumed into cur - fds never dropped.
                if (!cur.active) {
                    auto pf = inbox.takeNewest();
                    if (!pf)
                        pf = inbox.takeNewestWait(15);
                    if (pf) {
                        wantDropWsi.store(false, std::memory_order_relaxed);
                        // RE2 boot is 1920x1080. Exclusive overlay on that
                        // stream is XIO on Xwayland :0. LSFGVK_APP_NO_FS: take
                        // WSI on 1080 as a window so the game never creates
                        // 1440 (FRAME-0 ntsync).
                        const bool noFs = std::getenv("LSFGVK_APP_NO_FS") != nullptr;
                        const bool boot1080 = (w == 1920 && h == 1080) && !noFs;
                        if (!boot1080 && !noWsi) {
                            ensureOverlayWsi();
                            swapchain = g_overlay.swapchain;
                            extent = g_overlay.extent;
                            makeHud();
                        }
                        cur.active = true;
                        cur.nextDest = 0;
                        cur.stagingIdx = pf->stagingIdx;
                        cur.doneFds = std::move(pf->doneFds);
                        cur.snapFd = pf->snapFd;  // -1 if no snapshot fd
                        cur.captureTsNs = pf->captureTsNs;
                    } else if (wantDropWsi.exchange(false, std::memory_order_relaxed)
                            && g_overlay.wsi) {
                        dbg("output: idle drop overlay WSI (keep IPC)");
                        {
                            std::lock_guard<std::mutex> lk(submitMtx);
                            vk.df().DeviceWaitIdle(vk.dev());
                        }
                        if (g_overlay.swapchain != VK_NULL_HANDLE)
                            vk.df().DestroySwapchainKHR(vk.dev(), g_overlay.swapchain,
                                VK_NULL_HANDLE);
                        g_overlay.swapchain = VK_NULL_HANDLE;
                        g_overlay.swapImages.clear();
                        g_overlay.wsi->destroy();
                        g_overlay.wsi.reset();
                        swapchain = VK_NULL_HANDLE;
                    }
                }

                // 1080 boot: no exclusive overlay. Consume the frame so
                // Release already sent by input; do not Acquire/Present.
                if (cur.active && swapchain == VK_NULL_HANDLE) {
                    for (int d : cur.doneFds)
                        if (d >= 0)
                            ::close(d);
                    if (cur.snapFd >= 0)
                        ::close(cur.snapFd);
                    cur.doneFds.clear();
                    cur.snapFd = -1;
                    cur.active = false;
                    continue;
                }

                // exactly one present this vblank: GEN, REAL, or HOLD-LAST.
                if (cur.active && cur.nextDest < destCount) {
                    // --- GEN present for destination images[cur.nextDest] ------
                    const size_t i = cur.nextDest;
                    const auto tGen0 = Clock::now();
                    uint32_t idx{};
                    if (!acquireImage(idx))
                        break;
                    const auto tAcquire = Clock::now();
                    const VkImage dstImage = swapImages.at(idx);
                    if (cur.doneFds.at(i) >= 0) {
                        importSyncFd(vk, doneWaitSem.at(i).handle(), cur.doneFds.at(i));
                        cur.doneFds.at(i) = -1;   // import consumed the fd
                    }
                    // wait for this command buffer's previous submit to complete
                    if (!cbFences.at(cbIdx).wait(vk, UINT64_MAX))
                        throw ls::vulkan_error(VK_TIMEOUT, "cb fence wait failed");
                    cbFences.at(cbIdx).reset(vk);
                    cbs.at(cbIdx).begin(vk);
                    cbs.at(cbIdx).blitImage(vk,
                        {
                            makeBlitBarrier(state.destinationImages.at(i).mut().handle(),
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                            makeBlitBarrier(dstImage,
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                        },
                        { state.destinationImages.at(i).mut().handle(), dstImage },
                        extent,
                        {
                            makeBlitBarrier(dstImage,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                        },
                        imgExtent,
                        VK_FILTER_LINEAR
                    );
                    drawHud(cbs.at(cbIdx), dstImage);
                    const auto tBlitEnd = Clock::now();
                    cbs.at(cbIdx).end(vk);
                    {
                        std::lock_guard<std::mutex> lk(submitMtx);
                        cbs.at(cbIdx).submit(vk,
                            { acquireSem.handle(), doneWaitSem.at(i).handle() },
                            VK_NULL_HANDLE, 0,
                            { signalSem.at(presentIdx % signalPool).handle() }, VK_NULL_HANDLE, 0,
                            cbFences.at(cbIdx).handle());
                    }
                    processWsiEvents(0);
                    const VkPresentInfoKHR presentInfo{
                        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                        .waitSemaphoreCount = 1,
                        .pWaitSemaphores = &signalSem.at(presentIdx % signalPool).handle(),
                        .swapchainCount = 1,
                        .pSwapchains = &swapchain,
                        .pImageIndices = &idx,
                    };
                    const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
                    if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                        throw ls::vulkan_error(pres, "QueuePresentKHR failed (generated)");
                    if (cur.captureTsNs > 0) {
                        const uint64_t nowNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now().time_since_epoch()).count());
                        const double latMs = (nowNs > cur.captureTsNs) ? (nowNs - cur.captureTsNs) / 1e6 : 0.0;
                        lsfgvk::gui::g_guiState.currentLatencyGenMs.store(static_cast<float>(latMs));
                        dbg("MEASURED LATENCY: GEN present slot %u latency %.2f ms", cur.stagingIdx, latMs);
                    }
                    ++presentIdx;
                    ++presentedFrames;
                    ++cur.nextDest;
                    lsfgvk::gui::g_guiState.totalGenPresents.fetch_add(1);
                    lsfgvk::gui::g_guiState.totalPresents.fetch_add(1);
                    dbg("output: GEN present dest %zu/%zu (slot %u)",
                        i, destCount, cur.stagingIdx);
                } else if (cur.active) {
                    // --- REAL present: this frame's private snapshot -----------
                    if (!presentReal(cur.stagingIdx, cur.snapFd >= 0 ? cur.snapFd : -1, cur.captureTsNs))
                        break;
                    lastShownStagingIdx = static_cast<int>(cur.stagingIdx);
                    for (int d : cur.doneFds)   // close any never-imported gen fds
                        if (d >= 0)
                            ::close(d);
                    cur.active = false;
                    lsfgvk::gui::g_guiState.totalRealPresents.fetch_add(1);
                    lsfgvk::gui::g_guiState.totalPresents.fetch_add(1);
                    dbg("output: REAL present (slot %u)", cur.stagingIdx);
                } else if (lastShownStagingIdx >= 0) {
                    // --- HOLD-LAST: nothing newer to show ---------------------
                    // The last REAL present is still on screen; re-blitting
                    // repeatedly burns GPU compute unnecessarily. Do NOT consume
                    // anything here - the loop top's blocking take fills cur the
                    // instant a frame arrives. Just pump WSI events non-blockingly and continue.
                } else {
                    // first frame not shown yet: loop top's takeNewestWait
                    // blocks for it; nothing to do here.
                }

                maybeStats(Clock::now());

                // stop on a window resize/close (processEvents returns true).
                // Close ends the loop. Resize/configure must NOT — xdg_toplevel
                // sends extra configures after the first present (activated).
                // Treating that as fatal stops GEN/REAL after FRAME 0 and the
                // game's next blocking FRAME send never returns.
                // 1080 boot must not pump/present the 1440 overlay WSI.
                // Concurrent runPresent shares g_overlay; two output threads
                // on one swapchain is FRAME 0 then hang.
                if (!boot1080 && g_overlay.wsi && g_overlay.wsi->processEvents(0)) {
                    dbg("output: processEvents requested stop (close)");
                    break;
                }
            }

            // drain frames left in the inbox / cur so no fd leaks at teardown.
            while (auto pf = inbox.takeNewest()) {
                for (int d : pf->doneFds)
                    if (d >= 0)
                        ::close(d);
                if (pf->snapFd >= 0)
                    ::close(pf->snapFd);
            }
            for (int d : cur.doneFds)
                if (d >= 0)
                    ::close(d);
            if (cur.snapFd >= 0)
                ::close(cur.snapFd);
        } catch (...) {
            if (cur.snapFd >= 0) ::close(cur.snapFd);
            if (!outputError)
                outputError = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    };

    // --- INPUT thread: receive -> schedule -> snapshot -> release -> enqueue --
    auto inputLoop = [&] {
        std::vector<vk::CommandBuffer> snapCbs;
        std::vector<vk::CommandBuffer> computeCbs;
        std::vector<vk::Fence> snapFences;
        for (size_t i = 0; i < ls::ipc::STAGING_RING_DEPTH; ++i) {
            snapCbs.emplace_back(vk, vk.transferCmdPoolHandle());
            computeCbs.emplace_back(vk, vk.transferCmdPoolHandle());
            snapFences.emplace_back(vk, true);
        }
        vk::CommandBuffer gfxCb{ vk };
        std::optional<vk::Shader> swizzleShader;
        std::optional<vk::Sampler> swizzleSampler;
        std::optional<vk::DescriptorPool> swizzlePool;
        std::array<std::optional<vk::DescriptorSet>, ls::ipc::STAGING_RING_DEPTH> swizzleSets;
        std::array<std::optional<vk::Image>, ls::ipc::STAGING_RING_DEPTH> swizzleMid;
        int pendingDmaRelease = -1;
        vk::CommandBuffer emptyGenCb{ vk };
        vk::Fence emptyGenFence{ vk, true };
        vk::CommandBuffer emptyXferCb{ vk, vk.transferCmdPoolHandle() };
        vk::Fence emptyXferFence{ vk, true };
        VkQueryPool tsPool = VK_NULL_HANDLE;
        float tsPeriod = 1.0f;
        {
            VkPhysicalDeviceProperties2 props{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            };
            vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);
            tsPeriod = props.properties.limits.timestampPeriod;
            const VkQueryPoolCreateInfo qi{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2,
            };
            (void)vk.df().CreateQueryPool(vk.dev(), &qi, VK_NULL_HANDLE, &tsPool);
        }
        uint32_t tsLogged{ 0 };
        uint64_t fidx{ 0 };
        uint64_t lastDumpFidx{ 0 };
        auto lastFrameAt = Clock::now();
        const auto streamStart = lastFrameAt;
        bool gotFrame{ false };
        try {
            for (;;) {
                if (stop.load(std::memory_order_relaxed)
                        || failed.load(std::memory_order_relaxed))
                    break;
                pollfd pfd{};
                pfd.fd = conn.fd();
                pfd.events = POLLIN;
                const int pr = ::poll(&pfd, 1, 200);
                if (pr < 0) {
                    if (errno == EINTR)
                        continue;
                    throw ls::ipc::socket_error("poll() on input thread", errno);
                }
                if (stop.load(std::memory_order_relaxed)
                        || failed.load(std::memory_order_relaxed))
                    break;
                if (pr == 0 || !(pfd.revents & POLLIN)) {
                    // Do NOT destroy overlay WSI while the game is alive —
                    // tearing exclusive layer-shell is XIO on Xwayland :0
                    // and the isolated game dies. Screen is released when
                    // streams.empty() in main.cpp after the game exits.
                    continue;
                }
                lastFrameAt = Clock::now();
                gotFrame = true;
                auto msg = conn.receive(std::nullopt);
                const auto* frame = std::get_if<ls::ipc::Frame>(&msg);
                if (!frame)
                    continue;

                int captureFd = conn.takeReceivedFd();
                static bool loggedCap = false;
                if (!loggedCap && captureFd >= 0) {
                    loggedCap = true;
                    char link[64]{};
                    (void)::readlink(("/proc/self/fd/" + std::to_string(captureFd)).c_str(),
                        link, sizeof(link) - 1);
                    dbg("input: FRAME fd=%d link=%s", captureFd, link);
                }
                static const bool dropGen = std::getenv("LSFGVK_DROP_GEN")
                    && std::getenv("LSFGVK_DROP_GEN")[0] == '1';
                if (dropGen) {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    dbg("input: DROP_GEN Release (slot %u) (fidx %llu)",
                        frame->stagingIdx, (unsigned long long)fidx);
                    ++fidx;
                    continue;
                }

                // CPU copy / dual-host: render GPU already finished, so Release first is
                // safe. dma-buf still points at the render GPU buffer — Release first
                // lets the render GPU rewrite it while the secondary GPU copies.
                const bool dmaHop = (state.shmBytes == 0 && std::getenv("LSFGVK_DUAL_HOST") != nullptr
                    && std::getenv("LSFGVK_DUAL_HOST")[0] == '0');
                if (!dmaHop) {
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    dbg("input: Release first (slot %u) (fidx %llu)",
                        frame->stagingIdx, (unsigned long long)fidx);
                }

                const uint32_t sidxEarly = frame->stagingIdx;
                if (sidxEarly < ls::ipc::STAGING_RING_DEPTH && captureFd >= 0) {
                    char link[64]{};
                    char path[64]{};
                    std::snprintf(path, sizeof(path), "/proc/self/fd/%d", captureFd);
                    const ssize_t nlink = ::readlink(path, link, sizeof(link) - 1);
                    if (nlink > 0)
                        link[nlink] = '\0';
                    // Protocol: first fd for a slot is the share. Do not require
                    // the word "dmabuf" in readlink (that miss skipped the hop).
                    const bool needShare = state.dmaFds.at(sidxEarly) < 0;
                    static uint32_t fdKindLogged{ 0 };
                    if (fdKindLogged < 8) {
                        ++fdKindLogged;
                        dbg("input: FRAME fd slot %u link='%s' needShare=%d",
                            sidxEarly, nlink > 0 ? link : "?", needShare);
                    }
                    const bool isShare = nlink > 0 && std::strstr(link, "dmabuf") != nullptr;
                    if (needShare && isShare) {
                        int keepFd = ::dup(captureFd);
                        if (keepFd < 0)
                            throw ls::error("dup() failed before render dma-buf keep");
                        state.dmaFds.at(sidxEarly) = keepFd;
                        if (sidxEarly < state.aImports.size())
                            state.aImports.at(sidxEarly).reset();
                        dbg("input: keep render dma-buf slot %u link='%s'",
                            sidxEarly, nlink > 0 ? link : "?");
                        ::close(captureFd);
                        captureFd = -1;
                    }
                }
                if (dmaHop && captureFd < 0) {
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    dbg("input: dma-buf keep, hop on capture-done (slot %u)",
                        frame->stagingIdx);
                    ++fidx;
                    continue;
                }

                static const bool skipSnap = (std::getenv("LSFGVK_SKIP_SNAP")
                    && std::getenv("LSFGVK_SKIP_SNAP")[0] == '1')
                    || (std::getenv("LSFGVK_EMPTY_GEN")
                        && std::getenv("LSFGVK_EMPTY_GEN")[0] == '1')
                    || (std::getenv("LSFGVK_EMPTY_XFER")
                        && std::getenv("LSFGVK_EMPTY_XFER")[0] == '1');
                int snapFd = -1;
                if (!skipSnap) {
                const uint32_t sidx = frame->stagingIdx;
                if (sidx >= ls::ipc::STAGING_RING_DEPTH)
                    throw ls::error("FRAME stagingIdx out of range");
                auto& snapCbFence = snapFences.at(sidx);
                auto& cb = snapCbs.at(sidx);
                auto& computeCb = computeCbs.at(sidx);
                if (!snapCbFence.wait(vk, 0)) {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    // Overlay did not sample this write. Free it unless it is
                    // the slot the in-flight coprocessor copy is still reading.
                    if (dmaHop
                            && static_cast<int>(frame->stagingIdx) != pendingDmaRelease)
                        conn.send(ls::ipc::Release{ frame->stagingIdx });
                    dbg("input: snapshot skip (cb busy) (fidx %llu)",
                        (unsigned long long)fidx);
                } else {
                snapCbFence.reset(vk);
                if (dmaHop && pendingDmaRelease >= 0) {
                    const uint32_t done = static_cast<uint32_t>(pendingDmaRelease);
                    conn.send(ls::ipc::Release{ done });
                    dbg("input: Release slot %u", done);
                    pendingDmaRelease = -1;
                }
                bool waitWriteDone = false;
                if (captureFd >= 0 && !state.shmBytes) {
                    pollfd pfd{};
                    pfd.fd = captureFd;
                    pfd.events = POLLIN;
                    const auto tPoll0 = Clock::now();
                    const int pr = ::poll(&pfd, 1, 50);
                    static uint32_t pollLogged{ 0 };
                    if (pollLogged < 8) {
                        ++pollLogged;
                        dbg("input: poll write-complete %d revents=%d wall %.3f ms",
                            pr, pfd.revents, elapsedUs(tPoll0, Clock::now()) / 1000.0);
                    }
                    ::close(captureFd);
                    captureFd = -1;
                    waitWriteDone = false;
                }
                if (dmaHop) {
                    int srcFd = state.dmaFds.at(sidx);
                    static const bool noHop = std::getenv("LSFGVK_NO_HOP")
                        && std::getenv("LSFGVK_NO_HOP")[0] == '1';
                    if (srcFd >= 0 && !noHop && ensureDmaIn(state, vk)) {
                        const int hopFd = hopShareToOffload(state, sidx, srcFd);
                        if (hopFd >= 0) {
                            srcFd = hopFd;
                            // Dest data is safely in offload VRAM. Release render
                            // slot immediately so the game can capture next frame.
                            conn.send(ls::ipc::Release{ sidx });
                            dbg("input: Release immediate after dma-in slot %u", sidx);
                        }
                    }
                    if (srcFd >= 0 && !state.aImports.at(sidx).has_value()) {
                        try {
                            const vk::ImageLayout aLayout{
                                .mode = (state.negotiatedModifier == vk::EXCHANGE_MODIFIER_LINEAR)
                                    ? vk::ImageMode::Linear : vk::ImageMode::DrmModifier,
                                .drmModifier = state.negotiatedModifier,
                                .rowPitch = state.rowPitch,
                            };
                            const std::vector<uint32_t> families{
                                vk.transferQueueFamilyIndex()
                            };
                            int tryFd = ::dup(srcFd);
                            if (tryFd < 0)
                                throw ls::error("dup() failed before render dma-buf import");
                            state.aImports.at(sidx).emplace(vk,
                                VkExtent2D{ state.width, state.height },
                                state.captureFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                tryFd, std::nullopt, aLayout,
                                VK_SHARING_MODE_EXCLUSIVE, families);
                            dbg("input: import render dma-buf slot %u (copy)", sidx);
                        } catch (const std::exception& e) {
                            dbg("input: render dma-buf import failed slot %u: %s",
                                sidx, e.what());
                        }
                    }
                }
                if (state.shmBytes && sidx < state.shmMaps.size()
                        && state.shmMaps.at(sidx) && state.hostPtrs.at(sidx)) {
                    if (captureFd >= 0) {
                        pollfd pfd{};
                        pfd.fd = captureFd;
                        pfd.events = POLLIN;
                        ::poll(&pfd, 1, 1);
                        ::close(captureFd);
                        captureFd = -1;
                    }
                    if (state.shmSeq.at(sidx)) {
                        for (int waits = 0; waits < 3; ++waits) {
                            const uint32_t s = __atomic_load_n(
                                state.shmSeq.at(sidx), __ATOMIC_ACQUIRE);
                            if (s != state.shmSeen.at(sidx)) {
                                state.shmSeen.at(sidx) = s;
                                break;
                            }
                            if (waits < 2)
                                ::poll(nullptr, 0, 1);
                        }
                    }
                    std::memcpy(state.hostPtrs.at(sidx), state.shmMaps.at(sidx),
                        state.shmBytes);
                    dbg("input: posix-shm memcpy slot %u", sidx);
                    if (false && w >= 2560 && state.hostPtrs.at(sidx)
                            && (fidx == 80 || fidx == 250 || (fidx > 0 && fidx % 400 == 0))) {
                        char path[128];
                        std::snprintf(path, sizeof(path),
                            "/tmp/lsfg-re2-ingame/dump-fidx%llu.ppm",
                            (unsigned long long)fidx);
                        if (FILE* out = std::fopen(path, "wb")) {
                            std::fprintf(out, "P6\n%u %u\n255\n", w, h);
                            const auto* px = static_cast<const uint8_t*>(state.hostPtrs.at(sidx));
                            const size_t npx = static_cast<size_t>(w) * h;
                            std::vector<uint8_t> rgb(npx * 3);
                            for (size_t i = 0; i < npx; ++i) {
                                rgb[i * 3 + 0] = px[i * 4 + 0];
                                rgb[i * 3 + 1] = px[i * 4 + 1];
                                rgb[i * 3 + 2] = px[i * 4 + 2];
                            }
                            std::fwrite(rgb.data(), 1, rgb.size(), out);
                            std::fclose(out);
                            dbg("dumped %s", path);
                        }
                    }
                } else if (captureFd >= 0) {
                    pollfd pfd{};
                    pfd.fd = captureFd;
                    pfd.events = POLLIN;
                    const int pr = ::poll(&pfd, 1, 0);
                    static uint32_t pollLogged{ 0 };
                    if (pollLogged < 8) {
                        ++pollLogged;
                        dbg("input: poll frame-ready %d revents=%d", pr, pfd.revents);
                    }
                    if (sidx < state.dmaFds.size() && state.dmaFds.at(sidx) >= 0) {
                        dma_buf_import_sync_file imp{};
                        imp.flags = DMA_BUF_SYNC_WRITE;
                        imp.fd = captureFd;
                        if (::ioctl(state.dmaFds.at(sidx), DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &imp) != 0) {
                            static int nImp = 0;
                            if (nImp < 4) {
                                dbg("input: IMPORT_SYNC_FILE WRITE errno=%d", errno);
                                ++nImp;
                            }
                        } else {
                            static bool impOk = false;
                            if (!impOk) {
                                dbg("input: IMPORT_SYNC_FILE WRITE ok slot %u", sidx);
                                impOk = true;
                            }
                        }
                    }
                    importSyncFd(vk, frameReadySem.handle(), captureFd);
                    captureFd = -1;
                    waitWriteDone = true;
                }
                auto& srcLazy = state.aImports.at(sidx).has_value()
                    ? state.aImports.at(sidx)
                    : state.sourceImages.at(sidx);
                auto& snapLazy = state.genSources.at(sidx);
                const bool fromA = state.aImports.at(sidx).has_value();
                const bool swizzleBlit = fromA
                    && state.captureFormat != state.sourceFormat
                    && state.sourceImages.at(sidx).has_value();
                const bool hostDma = sidx < state.dmaMaps.size()
                    && state.dmaMaps.at(sidx) != nullptr;
                const VkImageLayout srcOld = hostDma
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : (fromA
                    ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                    : (state.shmBytes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED));
                const VkImageLayout srcNew = hostDma
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : (fromA
                    ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                    : (state.shmBytes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
                const uint32_t srcQ = (fromA && !hostDma) ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
                const uint32_t dstQ = (fromA && !hostDma) ? vk.transferQueueFamilyIndex() : VK_QUEUE_FAMILY_IGNORED;
                if (swizzleBlit) {
                    static bool loggedBlit = false;
                    if (!loggedBlit) {
                        loggedBlit = true;
                        dbg("input: dma-buf hop copy+compute %u -> RGBA",
                            static_cast<unsigned>(state.captureFormat));
                    }
                    if (!swizzleMid.at(sidx)) {
                        swizzleMid.at(sidx).emplace(vk, imgExtent, state.captureFormat,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
                    }
                    auto& mid = *swizzleMid.at(sidx);
                    if (!swizzleShader) {
                        swizzleShader.emplace(vk, ls::swizzleSpv(), 1, 1, 0, 1);
                        swizzleSampler.emplace(vk,
                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                            VK_COMPARE_OP_NEVER, false);
                        swizzlePool.emplace(vk, vk::Limits{
                            .sets = 8,
                            .uniform_buffers = 1,
                            .samplers = 8,
                            .sampled_images = 8,
                            .storage_images = 8,
                        });
                    }
                    if (!swizzleSets.at(sidx)) {
                        swizzleSets.at(sidx).emplace(vk, *swizzlePool, *swizzleShader,
                            std::vector<ls::R<const vk::Image>>{ std::cref(mid) },
                            std::vector<ls::R<const vk::Image>>{ std::cref(snapLazy.mut()) },
                            std::vector<ls::R<const vk::Sampler>>{ std::cref(*swizzleSampler) },
                            std::vector<ls::R<const vk::Buffer>>{});
                    }
                    cb.begin(vk);
                    if (tsPool != VK_NULL_HANDLE) {
                        cb.resetQueryPool(vk, tsPool, 0, 2);
                        cb.writeTimestamp(vk, tsPool, 0);
                    }
                    cb.copyImage(vk,
                        {
                            makeBlitBarrier(srcLazy.mut().handle(),
                                VK_ACCESS_NONE, srcOld,
                                VK_ACCESS_TRANSFER_READ_BIT, srcNew,
                                srcQ, dstQ),
                            makeBlitBarrier(mid.handle(),
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                        },
                        { srcLazy.mut().handle(), mid.handle() },
                        imgExtent,
                        {
                            makeBlitBarrier(mid.handle(),
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                        });
                    if (tsPool != VK_NULL_HANDLE)
                        cb.writeTimestamp(vk, tsPool, 1);
                    cb.end(vk);
                    computeCb.begin(vk);
                    computeCb.pipelineBarrier(vk,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        {
                            makeBlitBarrier(mid.handle(),
                                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                            makeBlitBarrier(snapLazy.mut().handle(),
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
                        });
                    const uint32_t gx = (w + 15u) / 16u;
                    const uint32_t gy = (h + 15u) / 16u;
                    computeCb.dispatch(vk, *swizzleShader, *swizzleSets.at(sidx),
                        {}, gx, gy, 1);
                    computeCb.pipelineBarrier(vk,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        {
                            makeBlitBarrier(snapLazy.mut().handle(),
                                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                        });
                    computeCb.end(vk);
                    {
                        std::lock_guard<std::mutex> lk(submitMtx);
                        cb.submit(vk,
                            waitWriteDone
                                ? std::vector<VkSemaphore>{ frameReadySem.handle() }
                                : std::vector<VkSemaphore>{}, VK_NULL_HANDLE, 0,
                            {}, copyDone.handle(), 0,
                            snapCbFence.handle(),
                            vk.dmaQueueHandle());
                        computeCb.submit(vk,
                            std::vector<VkSemaphore>{ copyDone.handle() }, VK_NULL_HANDLE, 0,
                            {}, snapshotSem.handle(), 0,
                            VK_NULL_HANDLE,
                            vk.transferQueueHandle());
                    }
                } else {
                cb.begin(vk);
                cb.copyImage(vk,
                    {
                        makeBlitBarrier(srcLazy.mut().handle(),
                            VK_ACCESS_NONE, srcOld,
                            VK_ACCESS_TRANSFER_READ_BIT, srcNew,
                            srcQ, dstQ),
                        makeBlitBarrier(snapLazy.mut().handle(),
                            VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                    },
                    { srcLazy.mut().handle(), snapLazy.mut().handle() },
                    imgExtent,
                    {
                        makeBlitBarrier(snapLazy.mut().handle(),
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                    }
                );
                cb.end(vk);
                {
                    std::lock_guard<std::mutex> lk(submitMtx);
                    cb.submit(vk,
                        waitWriteDone
                            ? std::vector<VkSemaphore>{ frameReadySem.handle() }
                            : std::vector<VkSemaphore>{}, VK_NULL_HANDLE, 0,
                        {}, snapshotSem.handle(), 0,
                        snapCbFence.handle(),
                        vk.dmaQueueHandle());
                }
                }
                snapFd = snapshotSem.exportFd(vk);
                dbg("input: snapshot submitted xferQ=%d sameAsGfx=%d (fidx %llu)",
                    vk.transferQueueHandle() != VK_NULL_HANDLE,
                    vk.transferQueueHandle() == vk.queue() ? 1 : 0,
                    (unsigned long long)fidx);
                if (dmaHop) {
                    /* Release was already sent immediately after hopShareToOffload */
                    pendingDmaRelease = -1;
                }
                if (fidx >= 20 && fidx - lastDumpFidx >= 40
                        && state.genSources.at(sidx).has_value()
                        && std::getenv("LSFGVK_DUMP_PPM")
                        && std::getenv("LSFGVK_DUMP_PPM")[0] == '1') {
                    lastDumpFidx = fidx;
                    (void)snapCbFence.wait(vk, 50ULL * 1000 * 1000);
                    const size_t npx = static_cast<size_t>(w) * h;
                    const size_t nb = npx * 4;
                    void* p = nullptr;
                    if (::posix_memalign(&p, 4096, nb) == 0) {
                        std::memset(p, 0, nb);
                        try {
                            vk::Image dumpImg(vk, imgExtent, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                p, nb);
                            cb.begin(vk);
                            cb.copyImage(vk,
                                {
                                    makeBlitBarrier(state.genSources.at(sidx).mut().handle(),
                                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                                    makeBlitBarrier(dumpImg.handle(),
                                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                                },
                                { state.genSources.at(sidx).mut().handle(), dumpImg.handle() },
                                imgExtent,
                                {});
                            cb.end(vk);
                            snapCbFence.reset(vk);
                            cb.submit(vk,
                                std::vector<VkSemaphore>{}, VK_NULL_HANDLE, 0,
                                {}, VK_NULL_HANDLE, 0,
                                snapCbFence.handle(),
                                vk.transferQueueHandle());
                            (void)snapCbFence.wait(vk, 50ULL * 1000 * 1000);
                            uint64_t rs = 0, gs = 0, bs = 0;
                            auto* px = static_cast<const uint8_t*>(p);
                            char named[128];
                            std::snprintf(named, sizeof(named),
                                "/tmp/lsfg-dump-fidx%llu.ppm",
                                (unsigned long long)fidx);
                            if (FILE* out = std::fopen("/tmp/lsfg-capture.ppm", "wb")) {
                                std::fprintf(out, "P6\n%u %u\n255\n", w, h);
                                std::vector<uint8_t> rgb(npx * 3);
                                for (size_t i = 0; i < npx; ++i) {
                                    rgb[i * 3 + 0] = px[i * 4 + 0];
                                    rgb[i * 3 + 1] = px[i * 4 + 1];
                                    rgb[i * 3 + 2] = px[i * 4 + 2];
                                    rs += px[i * 4 + 0];
                                    gs += px[i * 4 + 1];
                                    bs += px[i * 4 + 2];
                                }
                                std::fwrite(rgb.data(), 1, rgb.size(), out);
                                std::fclose(out);
                                if (FILE* out2 = std::fopen(named, "wb")) {
                                    std::fprintf(out2, "P6\n%u %u\n255\n", w, h);
                                    std::fwrite(rgb.data(), 1, rgb.size(), out2);
                                    std::fclose(out2);
                                }
                            }
                            std::cerr << "lsfg-vk-app: dump /tmp/lsfg-capture.ppm mean RGB "
                                << (rs / npx) << " " << (gs / npx) << " " << (bs / npx) << "\n";
                        } catch (const std::exception& e) {
                            std::cerr << "lsfg-vk-app: dump fail " << e.what() << "\n";
                        }
                        ::free(p);
                    }
                }
                }
                } else {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    if (dmaHop)
                        conn.send(ls::ipc::Release{ frame->stagingIdx });
                }

                static const bool emptyGen = std::getenv("LSFGVK_EMPTY_GEN")
                    && std::getenv("LSFGVK_EMPTY_GEN")[0] == '1';
                static const bool emptyXfer = std::getenv("LSFGVK_EMPTY_XFER")
                    && std::getenv("LSFGVK_EMPTY_XFER")[0] == '1';
                if (emptyGen || emptyXfer) {
                    auto& eCb = emptyXfer ? emptyXferCb : emptyGenCb;
                    auto& eFence = emptyXfer ? emptyXferFence : emptyGenFence;
                    VkQueue q = emptyXfer ? vk.transferQueueHandle() : vk.queue();
                    static int periodMs = [] {
                        const char* p = std::getenv("LSFGVK_EMPTY_PERIOD_MS");
                        return p ? std::atoi(p) : 0;
                    }();
                    static auto lastSubmit = Clock::now()
                        - std::chrono::milliseconds(1000);
                    const auto now = Clock::now();
                    const bool due = periodMs <= 0
                        || std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - lastSubmit).count() >= periodMs;
                    if (due && q != VK_NULL_HANDLE && eFence.wait(vk, 0)) {
                        eFence.reset(vk);
                        eCb.begin(vk);
                        eCb.end(vk);
                        VkCommandBuffer rawBuf = eCb.raw();
                        std::lock_guard<std::mutex> lk(submitMtx);
                        const VkSubmitInfo si{
                            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &rawBuf,
                        };
                        auto res = vk.df().QueueSubmit(q, 1, &si, eFence.handle());
                        if (res != VK_SUCCESS)
                            throw ls::vulkan_error(res, "empty gen QueueSubmit failed");
                        lastSubmit = now;
                        dbg("input: EMPTY_%s QueueSubmit period=%d (fidx %llu)",
                            emptyXfer ? "XFER" : "GEN", periodMs,
                            (unsigned long long)fidx);
                    }
                    ++fidx;
                    continue;
                }

                const auto tSched0 = Clock::now();
                std::vector<int> doneFds;
                try {
                    std::lock_guard<std::mutex> lk(submitMtx);
                    doneFds = backend.scheduleFrames(*state.context, -1);
                } catch (const std::exception& e) {
                    if (snapFd >= 0) ::close(snapFd);
                    throw ls::error("failed to schedule frames", e);
                }
                if (doneFds.size() != destCount)
                    throw ls::error("backend returned " + std::to_string(doneFds.size())
                        + " done fds, expected " + std::to_string(destCount));
                dbg("input: scheduleFrames took %lld us (fidx %llu)",
                    elapsedUs(tSched0, Clock::now()), (unsigned long long)fidx);

                inbox.push(PendingFrame{ std::move(doneFds), snapFd, frame->stagingIdx, frame->captureTsNs });
                ++frameCount;
                ++fidx;
            }
        } catch (...) {
            if (!inputError)
                inputError = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
        if (tsPool != VK_NULL_HANDLE)
            vk.df().DestroyQueryPool(vk.dev(), tsPool, VK_NULL_HANDLE);
    };

    // --- run both until stop/failure, then join and tear down cleanly --------
    std::thread inputThread(inputLoop);
    std::thread outputThread(outputLoop);
    inputThread.join();
    outputThread.join();

    // close any fds still in the inbox (the input may have pushed after the
    // output's drain) and idle the device before the RAII guard destroys WSI.
    while (auto pf = inbox.takeNewest())
        for (int d : pf->doneFds)
            if (d >= 0)
                ::close(d);
    {
        std::lock_guard<std::mutex> lk(submitMtx);
        vk.df().DeviceWaitIdle(vk.dev());
    }
    if (inputError)
        std::rethrow_exception(inputError);
    if (outputError)
        std::rethrow_exception(outputError);
}

void releaseOverlayWsi(const vk::Vulkan& vk) {
    if (!g_overlay.wsi)
        return;
    dbg("release overlay WSI (no live stream)");
    if (g_overlay.swapchain != VK_NULL_HANDLE)
        vk.df().DestroySwapchainKHR(vk.dev(), g_overlay.swapchain, VK_NULL_HANDLE);
    g_overlay.swapchain = VK_NULL_HANDLE;
    g_overlay.swapImages.clear();
    g_overlay.wsi->destroy();
    g_overlay.wsi.reset();
}

} // namespace ls::presentation
