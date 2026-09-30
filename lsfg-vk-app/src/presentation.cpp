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
#include "lsfg-vk-common/frame_dbg.hpp"

using Clock = std::chrono::steady_clock;
using Usec = std::chrono::microseconds;
static inline long long elapsedUs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<Usec>(b - a).count();
}
#include "gui.hpp"

#include "lsfg-vk-app/hud.hpp"
#include "lsfg-vk-app/imgui_hud.hpp"
#include <csignal>

/* S40+ Phase A: dear imgui overlay. picks up the same swapchain extent +
 * format as the legacy hud; ticked from the stats interval; toggled by
 * SIGUSR1 (fade). hidden = no NewFrame/no blit — present cost zero.
 * These live at file scope (signal handler + function-scope extern rules). */
static ls::hud::ImGuiHud* g_imguiHud{ nullptr };
static std::atomic<bool> g_imguiInitDone{ false };
/* S43b: the imgui card is ONE process-wide object but runPresent runs on one
   std::thread PER CONNECTION (the app's accept loop), and RE2 opens three in a
   single session. The S40 fix destroyed the card in every stream's teardown, so
   the first stream to end freed the card out from under its two siblings that
   were still inside renderCardOver / ImGui_ImplVulkan_NewFrame — the SIGSEGV in
   ImGui_ImplVulkan_Shutdown we kept hitting. Reference-count instead: register
   on entry, and only the LAST user out destroys it, when nobody can be inside. */
static std::atomic<int> g_imguiUsers{ 0 };
/* Extent/format the card was actually built with. RE2 changes resolution
   mid-session, so a later stream can find a card sized for the old one — the
   original S40 fault. Recorded so the mismatch is visible instead of silent. */
static std::atomic<uint32_t> g_imguiBuiltW{ 0 };
static std::atomic<uint32_t> g_imguiBuiltH{ 0 };
static volatile std::sig_atomic_t g_imguiToggleReq{ 0 };
static void installImguiToggle() {
std::signal(SIGUSR1, [](int) { g_imguiToggleReq = 1; });
}
#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/ipc/latency_ledger.hpp"
#include "lsfg-vk-app/swizzle_spv.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/descriptor_pool.hpp"
#include "lsfg-vk-common/vulkan/descriptor_set.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/sampler.hpp"
#include "lsfg-vk-common/helpers/env_flag.hpp"
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
#include <execinfo.h>
#include <fcntl.h>
#include <sys/ucontext.h>
#include <csignal>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
    /// Per-present stage times. Off unless LSFGVK_DBG is set to something
    /// other than "0". The off path is one flag check: no clock, no poll,
    /// no allocation. When on, the generation sync fd is polled on the CPU
    /// before import so that wait is a number instead of being buried in
    /// QueuePresent. That poll is not on the default path.
    struct StageTimes {
        bool on{ false };
        std::vector<int64_t> acquire, blit, genPresent, fenceWait, syncFd;
        std::atomic<uint64_t> pushed{ 0 }, dropped{ 0 };
        std::atomic<uint64_t> genCount{ 0 }, realCount{ 0 };
        Clock::time_point lastDump{};
        bool dumped{ false };

        StageTimes() {
            const char* e = std::getenv("LSFGVK_DBG");
            on = e && e[0] && std::strcmp(e, "0") != 0;
            if (!on)
                return;
            constexpr size_t cap = 200000;
            acquire.reserve(cap);
            blit.reserve(cap);
            genPresent.reserve(cap);
            fenceWait.reserve(cap);
            syncFd.reserve(cap);
            std::fprintf(stderr,
                "stage-time on (LSFGVK_DBG); sync-fd wait is a CPU poll before import\n");
        }

        void add(std::vector<int64_t>& v, int64_t us) {
            if (v.size() < v.capacity())
                v.push_back(us);
        }

        static void printOne(const char* name, std::vector<int64_t> v) {
            if (v.empty()) {
                std::fprintf(stderr, "stage-time %-12s n=0\n", name);
                return;
            }
            std::sort(v.begin(), v.end());
            const size_t n = v.size();
            const int64_t p50 = v[n / 2];
            const int64_t p99 = v[std::min(n - 1, (n * 99) / 100)];
            std::fprintf(stderr,
                "stage-time %-12s n=%zu p50=%lld p99=%lld max=%lld us\n",
                name, n, static_cast<long long>(p50),
                static_cast<long long>(p99), static_cast<long long>(v.back()));
        }

        void dump(const char* tag) {
            if (!on)
                return;
            std::fprintf(stderr,
                "stage-time %s pushed=%llu dropped=%llu gen=%llu real=%llu\n",
                tag,
                static_cast<unsigned long long>(pushed.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(dropped.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(genCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(realCount.load(std::memory_order_relaxed)));
            printOne("acquire", acquire);
            printOne("blit", blit);
            printOne("gen-present", genPresent);
            printOne("fence-wait", fenceWait);
            printOne("sync-fd-wait", syncFd);
            std::fflush(stderr);
            lastDump = Clock::now();
            dumped = true;
        }

        void maybeDump() {
            if (!on)
                return;
            if (!dumped) {
                lastDump = Clock::now();
                dumped = true;
                return;
            }
            if (elapsedUs(lastDump, Clock::now()) >= 10'000'000)
                dump("10s");
        }
    };

    StageTimes& stageTimes() {
        static StageTimes t;
        return t;
    }


    /// Session 40 dbl-ledger app-side sink: (capTsNs, presentedNs) per REAL
    /// present, consumed by tools/latency/probe_vk for click→photon through
    /// the doubled path. Env-gated: only mapped when LSFGVK_DBL_LEDGER=1.
    lsfgvk::ledger::LedgerSink g_ledgerApp;

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
            LSFG_FRAME_DBG("prime FDToHandle errno=%d fd=%d", errno, foreignFd);
            return -1;
        }
        int localFd = -1;
        if (drmPrimeHandleToFD(drmFd, handle, O_CLOEXEC | O_RDWR, &localFd) != 0) {
            LSFG_FRAME_DBG("prime HandleToFD errno=%d handle=%u", errno, handle);
            (void)drmCloseBufferHandle(drmFd, handle);
            return -1;
        }
        (void)drmCloseBufferHandle(drmFd, handle);
        LSFG_FRAME_DBG("prime reexport foreign=%d -> local=%d handle=%u", foreignFd, localFd, handle);
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
            LSFG_FRAME_DBG("pin gtt: no render node fd in this process");
            return;
        }
        drm_prime_handle ph{};
        ph.fd = dmaFd;
        if (::drmIoctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
            LSFG_FRAME_DBG("pin gtt: FD_TO_HANDLE errno=%d drm=%d", errno, drm);
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
        LSFG_FRAME_DBG("pin gtt drm=%d handle=%u set=%d errno=%d get=%d domains=0x%llx flags=0x%llx explicit=%d",
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
            LSFG_FRAME_DBG("gem flags: no render node fd in this process");
            return;
        }
        drm_prime_handle ph{};
        ph.fd = dmaFd;
        if (::drmIoctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
            LSFG_FRAME_DBG("gem flags: FD_TO_HANDLE errno=%d", errno);
            return;
        }
        drm_amdgpu_gem_create_in info{};
        drm_amdgpu_gem_op op{};
        op.handle = ph.handle;
        op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
        op.value = reinterpret_cast<uint64_t>(&info);
        const int ir = ::drmIoctl(drm, DRM_IOCTL_AMDGPU_GEM_OP, &op);
        LSFG_FRAME_DBG("gem flags rc=%d domains=0x%llx flags=0x%llx explicit=%d uncached=%d coherent=%d uswc=%d",
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
        static unsigned importFailures = 0;
        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fd
        };
        const auto res = vk.df().ImportSemaphoreFdKHR(vk.dev(), &importInfo);
        if (res != VK_SUCCESS) {
            /* S42g diagnostics: fd identity + per-fd rc; count instead
               of spamming (RE2 2026-09-20: the repeated -13 = handshake
               starvation; with the S42e cap the retry pump stops). */
            const unsigned n = importFailures++;
            if (n < 16 || (n % 256) == 0) {
                struct stat st{};
                char node[96] = "(stat failed)";
                if (::fstat(fd, &st) == 0)
                    std::snprintf(node, sizeof(node), "dev%lu inode%lu",
                        (unsigned long)st.st_dev, (unsigned long)st.st_ino);
                std::cerr << "lsfg-vk-app: importSyncFd failed rc=" << (int)res
                          << " fd=" << fd << " " << node
                          << " (n=" << n << ")\n";
            }
            close(fd);
            throw ls::vulkan_error(res, "vkImportSemaphoreFdKHR() failed");
        }
    }

    /// whether verbose per-cycle logging is requested (the -v hook: main sets
    /// LSFGVK_APP_VERBOSE when -v is passed, since runPresent carries no flag).
    bool verboseEnabled() {
        return envFlagOn("LSFGVK_APP_VERBOSE");
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
            LSFG_FRAME_DBG("dma-in probe %s: fence wait failed before submit", tag);
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
                LSFG_FRAME_DBG("dma-in probe %s timestamp read failed: %s", tag, e.what());
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
        LSFG_FRAME_DBG("dma-in probe %s wall %.3f ms exec %.3f ms park %.3f ms cal0=%d",
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
            LSFG_FRAME_DBG("dma-in probe local copy failed: %s", e.what());
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
                LSFG_FRAME_DBG("dma-in timestamps period %.3f ns/tick pool=%d cal=%d",
                    g_dmaHopTs.periodNs,
                    g_dmaHopTs.pool != VK_NULL_HANDLE,
                    g_dmaHopTs.getCal != nullptr);
            }
            LSFG_FRAME_DBG("dma-in device ready (no gfx)");
            return true;
        } catch (const std::exception& e) {
            ::unsetenv("DISABLE_VK_LAYER_LSFGVK_frame_generation");
            LSFG_FRAME_DBG("dma-in device failed: %s", e.what());
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
                LSFG_FRAME_DBG("dma-in import share failed: %s", e.what());
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
                LSFG_FRAME_DBG("dma-in vram dest failed: %s", e.what());
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
                LSFG_FRAME_DBG("dma-in timestamp read failed: %s", e.what());
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
            LSFG_FRAME_DBG("dma-in hop slot %u wall %.3f ms copy %.3f ms park %.3f ms "
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

// --- DIAGNOSTIC: SIGSEGV handler that reports the fault address ------------
// SEGV_ACCERR in std::__atomic_base<bool>::store has now happened twice, with
// no caller frames. Print which mapping si_addr lands in, and walk RBP. The
// app is built with -fno-omit-frame-pointer so that walk is the real stack.
// Do not "fix" the store from this. The mapping is the evidence.
namespace {
unsigned long lsfdgParseHex(const char* s, const char** end) {
    unsigned long v = 0;
    while (*s) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = static_cast<unsigned>(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = static_cast<unsigned>(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = static_cast<unsigned>(*s - 'A' + 10);
        else break;
        v = (v << 4) | d;
        ++s;
    }
    if (end) *end = s;
    return v;
}
void lsfdgWrite(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    if (n) (void) ::write(STDERR_FILENO, s, n);
}
} // namespace
extern "C" void lsfdgSegvHandler(int sig, siginfo_t* si, void* ctx) {
    static volatile sig_atomic_t inHandler;
    if (inHandler) {
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    inHandler = 1;
    const void* addr = si ? si->si_addr : nullptr;
    char buf[256];
    int n = ::snprintf(buf, sizeof(buf),
        "\n[SEGV] signal=%d code=%d si_addr=%p tid=%ld\n",
        sig, si ? si->si_code : 0, addr, static_cast<long>(::gettid()));
    if (n > 0) (void) ::write(STDERR_FILENO, buf, static_cast<size_t>(n));

    const int mfd = ::open("/proc/self/maps", O_RDONLY);
    if (mfd < 0) {
        lsfdgWrite("[SEGV] /proc/self/maps unreadable\n");
    } else {
        const unsigned long target = reinterpret_cast<unsigned long>(addr);
        char chunk[1024];
        char line[512];
        size_t llen = 0;
        int found = 0;
        ssize_t nr;
        while ((nr = ::read(mfd, chunk, sizeof(chunk))) > 0 && !found) {
            for (ssize_t i = 0; i < nr && !found; ++i) {
                if (chunk[i] != '\n') {
                    if (llen + 1 < sizeof(line))
                        line[llen++] = chunk[i];
                    continue;
                }
                line[llen] = '\0';
                const char* dash = nullptr;
                const unsigned long start = lsfdgParseHex(line, &dash);
                if (dash && *dash == '-') {
                    const unsigned long end = lsfdgParseHex(dash + 1, nullptr);
                    if (target >= start && target < end) {
                        lsfdgWrite("[SEGV] si_addr mapping: ");
                        (void) ::write(STDERR_FILENO, line, llen);
                        lsfdgWrite("\n");
                        found = 1;
                    }
                }
                llen = 0;
            }
        }
        ::close(mfd);
        if (!found)
            lsfdgWrite("[SEGV] si_addr is not in any mapping\n");
    }

    if (ctx) {
        auto* uc = static_cast<ucontext_t*>(ctx);
        const unsigned long rip = static_cast<unsigned long>(
            uc->uc_mcontext.gregs[REG_RIP]);
        unsigned long rbp = static_cast<unsigned long>(
            uc->uc_mcontext.gregs[REG_RBP]);
        n = ::snprintf(buf, sizeof(buf), "[SEGV] fp rip=%#lx rbp=%#lx\n", rip, rbp);
        if (n > 0) (void) ::write(STDERR_FILENO, buf, static_cast<size_t>(n));
        for (int i = 0; i < 32 && rbp > 0x10000 && (rbp & 7) == 0; ++i) {
            const unsigned long next = *reinterpret_cast<unsigned long*>(rbp);
            const unsigned long ret = *reinterpret_cast<unsigned long*>(rbp + 8);
            n = ::snprintf(buf, sizeof(buf), "[SEGV] fp[%d] %#lx\n", i, ret);
            if (n > 0) (void) ::write(STDERR_FILENO, buf, static_cast<size_t>(n));
            if (next <= rbp)
                break;
            rbp = next;
        }
    }

    lsfdgWrite("[SEGV] backtrace:\n");
    void* frames[64];
    const int cnt = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, cnt, STDERR_FILENO);
    n = ::snprintf(buf, sizeof(buf), "[SEGV] end tid=%ld (%d frames)\n",
        static_cast<long>(::gettid()), cnt);
    if (n > 0) (void) ::write(STDERR_FILENO, buf, static_cast<size_t>(n));
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
static void lsfdgInstallSegvHandler() {
    struct sigaction sa {};
    sa.sa_sigaction = lsfdgSegvHandler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
}


void runPresent(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const vk::Vulkan& vk, lsfgvk::backend::Instance& backend,
        const ls::GameConf& conf, std::string_view session,
        const std::atomic<bool>& stop) {
    lsfdgInstallSegvHandler();   // DIAGNOSTIC: report si_addr on SIGSEGV

    const uint32_t w = state.width, h = state.height;
    bool dropOverlay = false;
    /* S43b: claim a reference on the shared card for the life of this stream.
       Balanced release at the bottom of this function. */
    g_imguiUsers.fetch_add(1, std::memory_order_acq_rel);
    // Session 40: window handle for wp_presentation arming; set once when the
    // overlay WSI is built inside ensureOverlayWsi, read from the present
    // loops. X11 backends return false from armPresentFeedback — no arming.
    ls::wsi::WindowHandle fbHandle{ nullptr };

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
    fbHandle = handle;  // Session 40: publish to the present loops

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
    /* S40+: the imgui overlay card is semi-transparent (WindowBg alpha
       ~0.40); the RT keeps per-pixel alpha and the blit copies it into the
       swapchain — the compositor needs a premultiplied-alpha surface to
       blend the card over the game. PRE_MULTIPLIED now PREFERRED whenever
       supported (KWin layer-shell supports it); OPAQUE stays the legal
       fallback (card renders as its cpu-y 40%-brightness color there). */
    VkCompositeAlphaFlagBitsKHR compositingAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* S40+ card-over: in-buffer blend => OPAQUE ONLY (KWin must not
       blend card pixels against the desktop beneath the layer). */
    std::fprintf(stderr, "lsfg-vk-app: overlay compositeAlpha=0x%x (supported=0x%x)\n",
        (unsigned)compositingAlpha, (unsigned)caps.supportedCompositeAlpha);

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
        LSFG_FRAME_DBG("surface present modes (%u):%s%s%s%s",
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
    /* S40 next-steps lever: LSFGVK_OVERLAY_IMAGES forces the overlay buffer
       count (2 = shallowest legal: minImageCount floor). Shallow pool trims
       the REAL present's queued-ahead wait (the ~1-2 ms the traces chased);
       0/missing = driver default (deep pool). */
    uint32_t minImages = caps.minImageCount < 2 ? 3 : caps.minImageCount;
    if (const char* oi = getenv("LSFGVK_OVERLAY_IMAGES"); oi && *oi) {
        const uint32_t forced{ static_cast<uint32_t>(atoi(oi)) };
        if (forced >= 2 && forced <= 8)
            minImages = forced > caps.minImageCount ? forced : caps.minImageCount;
    }

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

    LSFG_FRAME_DBG("swapchain extent %ux%u fmt=%u mode=%s minImages=%u (stream %ux%u)",
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
        LSFG_FRAME_DBG("reusing overlay WSI %ux%u (stream %ux%u)",
            g_overlay.extent.width, g_overlay.extent.height, w, h);
    } else if (boot1080 || noWsi) {
        LSFG_FRAME_DBG("defer overlay WSI (boot1080=%d noWsi=%d stream %ux%u)",
            boot1080 ? 1 : 0, noWsi ? 1 : 0, w, h);
    } else {
        LSFG_FRAME_DBG("defer overlay WSI until first FRAME (stream %ux%u)", w, h);
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
                LSFG_FRAME_DBG("guard: keeping overlay WSI");
                return;
            }
            LSFG_FRAME_DBG("guard: dropping overlay WSI");
            if (g_overlay.swapchain != VK_NULL_HANDLE && vk != nullptr)
                vk->df().DestroySwapchainKHR(vk->dev(), g_overlay.swapchain, VK_NULL_HANDLE);
            g_overlay.swapchain = VK_NULL_HANDLE;
            g_overlay.swapImages.clear();
            if (g_overlay.wsi)
                g_overlay.wsi->destroy();
            g_overlay.wsi.reset();
            LSFG_FRAME_DBG("guard: overlay WSI dropped");
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
        // Session 40 latency ledger stamps (CLOCK_MONOTONIC steady_clock ns):
        // recvTsNs = FRAME pulled off the wire by the input thread;
        // schedDoneNs = scheduleFrames had submitted snapshot + frame-gen work
        // (the app's last queue touch for this frame).
        uint64_t recvTsNs{ 0 };
        uint64_t schedDoneNs{ 0 };
    };
    // Frames abandoned without being read. MUST stay 0: no frame may ever be
    // dropped. A non-zero value means a slot's blit did not signal in time and
    // we declined to read a possibly-torn slot - a visible hole in the stream,
    // counted rather than hidden. Static so the local Inbox (and the input
    // loop that owns it) can both reach them.
    static uint64_t g_framesDropped;
    static uint64_t g_framesOut;


    struct Inbox {
        std::mutex m;
        std::condition_variable cv;
        std::deque<PendingFrame> q;
        // Shutdown: without this, takeNewestWait() parks on the cv forever and
        // runPresent's join() hangs instead of reporting the error that caused
        // the teardown. Set stop() wakes every waiter.
        bool stopped{ false };
        void stop() {
            {
                std::lock_guard<std::mutex> lk(m);
                stopped = true;
            }
            cv.notify_all();
        }
        // Blocking variant: waits on the cv until a frame arrives or the
        // timeout expires (timeout serves WSI event pumping only; frame
        // arrival wakes instantly - zero poll latency). Returns nullopt on
        // timeout.
        std::optional<PendingFrame> takeNewestWait(unsigned timeoutMs) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                [this] { return !q.empty() || stopped; });
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
        // FIFO, oldest-first. NO FRAME MAY BE DROPPED.
        //
        // This used to take q.back() (the NEWEST) and then q.clear() the rest,
        // closing every queued frame's fds on the way out. That silently
        // discarded complete, already-captured frames - and because each
        // capture produces a GEN and a REAL, it discarded those in pairs, so
        // the doubler was left interpolating against frames that no longer
        // existed. Under load (any hiccup that let the queue build) this
        // silently threw away work the render card had already paid for.
        //
        // Taking the oldest and leaving the rest queued keeps the 1:1
        // GEN/REAL correspondence the backend's pairing depends on.
        std::optional<PendingFrame> takeNewestLocked() {
            if (q.empty())
                return std::nullopt;
            PendingFrame oldest = std::move(q.front());
            q.pop_front();
            if (g_framesDropped) {
                std::cerr << "lsfg-vk-app: frame-integrity out " << g_framesOut
                          << " backlog " << q.size()
                          << " dropped " << g_framesDropped
                          << " (must be 0)\n";
                g_framesOut = 0;
                g_framesDropped = 0;
            }
            return PendingFrame{ std::move(oldest) };
        }
        void push(PendingFrame f) {
            // Newest 2 only. The output thread cannot keep up with the copy
            // thread. An unbounded queue holds one sync_fd per frame that has
            // not been presented yet, until the process runs out of fds.
            // Close the fds of everything older than the newest two.
            std::vector<PendingFrame> dropped;
            if (stageTimes().on)
                stageTimes().pushed.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(m);
                q.push_back(std::move(f));
                while (q.size() > 2) {
                    dropped.push_back(std::move(q.front()));
                    q.pop_front();
                }
            }
            if (!dropped.empty() && stageTimes().on)
                stageTimes().dropped.fetch_add(dropped.size(), std::memory_order_relaxed);
            for (auto& old : dropped) {
                for (int d : old.doneFds)
                    if (d >= 0) ::close(d);
                if (old.snapFd >= 0) ::close(old.snapFd);
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
        // S40+ card-over: blend the premult card into the game frame.
        // coBlit chooses: co pass (new) vs proven blit (a3501c3) — env-co
        // behaves like: LSFGVK_CO_DISABLE=1 → blit.
        /* S41: blit is now the default (proven card); the in-buffer
           card-over stays opt-in via LSFGVK_CO_EXPERIMENT=1 while its
           present-time sampled-slot reads transparent (S41 C1-C7). */
        static const bool coDisable = [] {
            const char* e = std::getenv("LSFGVK_CO_EXPERIMENT");
            return !(e && e[0] == '1' && !e[1]);
        }();
        static bool logged = false;
        if (g_imguiHud && !coDisable) {
            if (!logged) { logged = true;
                std::cerr << "lsfg-vk-app: card-over experiment active\n"; }
            g_imguiHud->renderCardOver(cb, dstImage, imgExtent);
            return;
        }
        if (g_imguiHud && coDisable) {
            static bool logged2 = false;
            if (!logged2) { logged2 = true;
                std::cerr << "lsfg-vk-app: blit HUD active (card verified)\n"; }
        }
        if (g_imguiHud && ls::hud::ImGuiHud::drawing()) {
            const VkImage srcImg = g_imguiHud->rtImage();
            const VkExtent2D b = g_imguiHud->rtExtent();
            const auto o = g_imguiHud->origin();
            const VkImageMemoryBarrier sBar = makeBlitBarrier(srcImg,
                g_imguiHud->lastAccess(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            const VkImageMemoryBarrier dB = makeBlitBarrier(dstImage,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkImageMemoryBarrier bars[2] = { sBar, dB };
            vk.df().CmdPipelineBarrier(cb.raw(),
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                0, nullptr, 0, nullptr, 2, bars);
            /* S42d: blit ONLY the card subrect of the RT. The RT outside
               the imgui window = transparent-black premult (0,0,0,0);
               under the OPAQUE composite surface those pixels present
               as opaque black = the 650x366 "black box" seen in RE2
               play-test (pixel-verified edges x=1912, y=367). */
            const int32_t cx0 = 463, cy0 = 13, cx1 = 627, cy1 = 112;
            const VkImageBlit bl{
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0,0,1 },
                .srcOffsets = {
                    { cx0, cy0, 0 },
                    { cx1, cy1, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0,0,1 },
                .dstOffsets = {
                    { o.x + cx0, o.y + cy0, 0 },
                    { o.x + cx1, o.y + cy1, 1 } },
            };
            vk.df().CmdBlitImage(cb.raw(), srcImg,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dstImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            g_imguiHud->markRead();
        }
        if (!hud)
            return;   /* legacy seven-seg blits only in LSFGVK_HUD=minimal */
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
                /* S43: an unconditional print on the acquire-retry path.
                   It only fires when the swapchain runs dry, but it is still
                   the frame path and still formatted every 240 spins. The
                   loop now just pumps events and tries again; the stall is
                   visible on the card without writing to stderr. */
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
            uint64_t recvTsNs{ 0 };    // Session 40 ledger stamps (see PendingFrame)
            uint64_t schedDoneNs{ 0 };
        } cur;
        int lastShownStagingIdx{ -1 }; // newest real frame actually shown (HOLD-LAST)
        uint64_t presentIdx{ 0 };      // rolling index into the signal pool

        Clock::time_point statsLastTime = Clock::now();
        /* S43: lastReal/lastGen MUST be per-thread, exactly like
           statsLastTime. The app spawns one std::thread per accepted
           connection (main.cpp accept loop) and RE2 opens three in one
           session, so this closure runs concurrently three times over. As
           `static` they were ONE baseline shared by all three threads while
           the 250 ms gate stayed per-thread: whichever thread won the window
           took the full delta (~120) and the other two divided a partial
           delta by a full 250 ms and read near zero. The card then showed
           whichever thread wrote last — the 27/51/94/0 oscillation measured
           2026-09-25. Per-thread baselines fix it at the source: each thread
           measures the whole process-wide interval across its own full
           window, so all three compute the same correct value, and the data
           race on the shared statics disappears with them. */
        uint64_t lastReal = lsfgvk::gui::g_guiState.totalRealPresents.load();
        uint64_t lastGen = lsfgvk::gui::g_guiState.totalGenPresents.load();
        /* S42c: 4 Hz stats (was 1 s; the 1 Hz cadence ALSO paced the
           card tick = the RE2 play-test choppy-card complaint). */
        const auto statsInterval = std::chrono::milliseconds(250);
        auto maybeStats = [&](Clock::time_point now) {
            if (now - statsLastTime < statsInterval)
                return;
            const double dt = std::chrono::duration<double>(now - statsLastTime).count();
            /* S42l: fps from the PROCESS-WIDE present atomics, not the
               per-stream frameCount/presentedFrames. The game recreates
               its layer context repeatedly (RE2 connected 3× in one
               session; old streams never notice and never end), so a
               stale stream's maybeStats kept publishing 0/0 windows
               (the "oscillating 0/0 ↔ 120/240" report). The atomics are
               shared by every stream, so a stale publisher computes the
               same correct value as the live one. */
            const uint64_t nowReal = lsfgvk::gui::g_guiState.totalRealPresents.load();
            const uint64_t nowGen = lsfgvk::gui::g_guiState.totalGenPresents.load();
            const uint32_t gameFps = static_cast<uint32_t>(
                static_cast<double>(nowReal - lastReal) / dt + 0.5);
            /* S42q: the card's second row is the SCREEN rate = every
               present shown (REAL captures + GEN solves). The old
               value was totalGenPresents alone — at multiplier 2 the
               GEN rate equals the REAL rate BY STRUCTURE (the flow
               emits exactly multiplier-1 destination images per
               capture), so the rows pinned at parity (244/244,
               51/55) and the card's own "(2x)" tag (which compares
               presented > game) could never engage at steady state
               — the exact "244/244 means it didn't double" confusion.
               Keep the gen-only rate in totalGenPresents for the
               trace row; publish real+gen as the presented rate. */
            const uint32_t presentedFps = static_cast<uint32_t>(
                static_cast<double>((nowReal - lastReal)
                    + (nowGen - lastGen)) / dt + 0.5);
            lastReal = nowReal; lastGen = nowGen;
            // S42j: feed the ImGui card directly (the echo publish in
            // maybeHud re-publishes latest() and never carried these).
            ls::hud::ImGuiHud::publishFps(
                static_cast<float>(gameFps), static_cast<float>(presentedFps));
            /* S42k: stats-window tracer (throttled to 1 line/s) — proves the
               compute side of the fps pipeline independently of the card. */
            {
                static uint32_t lastG = 0, lastD = 0; static int logThrottle = 0;
                if ((gameFps != lastG || presentedFps != lastD) && logThrottle++ < 240)
                    LSFG_FRAME_DBG("stats win: gf=%u df=%u dt=%.3f", gameFps, presentedFps, dt);
                if (gameFps != lastG || presentedFps != lastD) { lastG = gameFps; lastD = presentedFps; }
            }
            lsfgvk::gui::g_guiState.currentFpsReal.store(static_cast<float>(gameFps));
            lsfgvk::gui::g_guiState.currentFpsGen.store(static_cast<float>(presentedFps));
            lsfgvk::gui::g_guiState.streamActive.store(true);
            // Session 40: scanout-derived latency. wp_presentation latch is
            // CLOCK_MONOTONIC (same clock as Clock::now); the comparison is
            // exact, no cross-clock math. When no feedback has arrived yet
            // the HUD fields simply stay at their last value.
            if (g_overlay.wsi) {
                const uint64_t latchNs = g_overlay.wsi->lastPresentLatchNs();
                if (latchNs > 0) {
                    // last queued REAL present: newest capture at submit time
                    const uint64_t nowNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now().time_since_epoch()).count());
                    if (latchNs < nowNs && nowNs - latchNs < 1'000'000'000ULL) {
                        // present->scanout headroom (queue drains + compositor hold)
                        const float scanMs = static_cast<float>(
                            static_cast<double>(nowNs - latchNs) / 1e6);
                        lsfgvk::gui::g_guiState.latencyScanMs.store(scanMs);
                        if (verboseEnabled())
                            std::cerr << "lsfg-vk-app: scanout lag " << scanMs
                                      << " ms (wp_presentation)\n";
                    }
                }
            }
            if (verboseEnabled())
                std::cerr << "lsfg-vk-app: " << gameFps << " fps game, "
                          << presentedFps << " fps presented\n";
            {
                // hud.update submits on the queue; serialize against the input thread.
                std::lock_guard<std::mutex> lk(submitMtx);
                try {
                    if (hud) {
                        // Session 40 latency row: ipc/solve/scan p50s (ms) —
                        // the always-on pipeline row (segments E..H, one line).
                        // The opt-in experience row (click→photon p50/p99 +
                        // GEN adds) arrives with the probe client (step 4) and
                        // will ride the same second row, gated by config.
                        char lat[64] = "";
                        const float ipc = lsfgvk::gui::g_guiState.latencyIpcMs.load();
                        const float solve = lsfgvk::gui::g_guiState.latencyGenSolveMs.load();
                        const float scan = lsfgvk::gui::g_guiState.latencyScanMs.load();
                        if (ipc > 0 || solve > 0 || scan > 0)
                            /* S40 labels: I=IPC(ms) G=GEN-solve S=SCAN */
                            std::snprintf(lat, sizeof(lat), "I%.1fG%.1fS%.1f",
                                static_cast<double>(ipc), static_cast<double>(solve),
                                static_cast<double>(scan));
                        // Opt-in experience row (LSFGVK_LATENCY_HUD=experience|all):
                        // GEN adds +X ms = GEN scanout minus REAL scanout EMA.
                        // Dashes until both anchors are live — never a guessed number.
                        static const bool expHud =
                            [] {
                                const char* e = std::getenv("LSFGVK_LATENCY_HUD");
                                return e && (std::strcmp(e, "experience") == 0
                                    || std::strcmp(e, "all") == 0);
                            }();
                        if (expHud) {
                            const float genExtra = lsfgvk::gui::g_guiState.
                                latencyGenExtraMs.load();
                            const float p50 = lsfgvk::gui::g_guiState.
                                inputLatencyP50Ms.load();
                            const float p99 = lsfgvk::gui::g_guiState.
                                inputLatencyP99Ms.load();
                            if (genExtra != 0.0f)
                                std::snprintf(lat, sizeof(lat), "I%.1fG%.1fS%.1f E%+.1f",
                                    static_cast<double>(ipc), static_cast<double>(solve),
                                    static_cast<double>(scan),
                                    static_cast<double>(genExtra));
                            else
                                std::snprintf(lat, sizeof(lat), "I%.1fG%.1fS%.1f E--",
                                    static_cast<double>(ipc), static_cast<double>(solve),
                                    static_cast<double>(scan));
                            (void)p50; (void)p99; // click→photon row rides step 4 calibration
                        }
                        /* S40: labeled rows — 'FPS a/b' = game/presented;
                           second row labels each segment (I=IPC G=GEN
                           S=SCAN) + 'E' = GEN adds, dashes until live. */
                        hud->update("FPS " + std::to_string(gameFps)
                            + "/" + std::to_string(presentedFps), lat);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "lsfg-vk-app: hud update failed: " << e.what() << "\n";
                }
            }
            try {
                if (g_imguiToggleReq != 0) {
                    g_imguiToggleReq = 0;
                    ls::hud::ImGuiHud::toggle();
                }
            } catch (const std::exception& e) {
                std::cerr << "lsfg-vk-app: imgui toggle failed: " << e.what() << "\n";
            }
            statsLastTime = now;
        };
        /* S42c: the overlay CARD repaint must run at present cadence, not
           the 1 Hz stats cadence (user-visible 1 Hz choppy card found in
           RE2 play-test). tick() self-paces via its own dt; the pub()
           numbers still refresh from maybeStats at 1 Hz. */
        auto maybeHud = [&](Clock::time_point now) {
            static Clock::time_point hudLast = now;
            const double hdt = std::chrono::duration<double>(now - hudLast).count();
            /* S43: no repaint ceiling. This was 20 Hz, so the card's frame
               index lagged the frame on screen by up to 50 ms (~7 captures
               at 132/s) and could not identify a frame in a capture. The
               HUD now repaints once per present, which is what the frame
               identity row needs to mean anything. */
            hudLast = now;
            if (!g_imguiHud || !g_imguiInitDone.load()) return;
            ls::hud::ImGuiHud::Stats st = ls::hud::ImGuiHud::latest();
            /* S43: frame identity for the card. captureIdx is the count of
               frames the layer has handed us, presentIdx the count of presents
               submitted. Both are plain atomic loads on the stats path, so
               this costs nothing per frame. */
            st.captureIdx = frameCount.load(std::memory_order_relaxed);
            st.presentIdx = presentedFrames.load(std::memory_order_relaxed);
            try {
                ls::hud::ImGuiHud::publish(st);
                if (g_imguiHud)
                    g_imguiHud->tick(static_cast<float>(hdt));
            } catch (const std::exception& e) {
                std::cerr << "lsfg-vk-app: imgui hud tick failed: " << e.what() << "\n";
            }
        };

        // present one swapchain image that blits the private snapshot of the
        // given real frame into it (used for REAL presents and HOLD-LAST).
        auto presentReal = [&](int stagingIdx, int snapFd, uint64_t capTsNs = 0) -> bool {
            uint32_t idx{};
            const auto tReal0 = Clock::now();
            Clock::time_point tAcq0{};
            if (stageTimes().on)
                tAcq0 = Clock::now();
            if (!acquireImage(idx)) {
                if (snapFd >= 0) ::close(snapFd);
                return false;
            }
            if (stageTimes().on)
                stageTimes().add(stageTimes().acquire, elapsedUs(tAcq0, Clock::now()));
            const auto tAcquire = Clock::now();
            const VkImage dstImage = swapImages.at(idx);
            auto& srcImage = state.genSources.at(stagingIdx);
            // wait for this command buffer's previous submit to complete
            Clock::time_point tFence0{};
            if (stageTimes().on)
                tFence0 = Clock::now();
            if (!cbFences.at(cbIdx).wait(vk, UINT64_MAX))
                throw ls::vulkan_error(VK_TIMEOUT, "cb fence wait failed");
            if (stageTimes().on)
                stageTimes().add(stageTimes().fenceWait, elapsedUs(tFence0, Clock::now()));
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
                LSFG_FRAME_DBG("output: REAL importSyncFd %lld us", elapsedUs(tImport0, tImport1));
            }
            Clock::time_point tBlit0{};
            if (stageTimes().on)
                tBlit0 = Clock::now();
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
            if (stageTimes().on)
                stageTimes().add(stageTimes().blit, elapsedUs(tBlit0, Clock::now()));
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
                LSFG_FRAME_DBG("output: REAL acquire %lld us blit %lld us lock %lld us submit %lld us (total %lld us)",
                    elapsedUs(tAcq0, tAcquire), elapsedUs(tAcquire, tBlitEnd),
                    elapsedUs(tLock0, tLock1), elapsedUs(tLock1, tSubmit1),
                    elapsedUs(tAcq0, tSubmit1));
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
            // Session 40: arm presented-feedback BEFORE the commit: the
            // feedback object associates with this surface's next commit
            // (the present below), and the compositor replies asynchronously
            // after scanning that buffer out (latch = photon side).
            g_overlay.wsi->armPresentFeedback(fbHandle);
            const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
            {
                static const bool countPresents = [] {
                    const char* e = std::getenv("LSFGVK_PRESENT_COUNT");
                    return e && e[0] == '1' && !e[1];
                }();
                if (countPresents) {
                    std::fprintf(stderr, "present-count kind=real rc=%d\n",
                        static_cast<int>(pres));
                    std::fflush(stderr);
                }
            }
            if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(pres, "QueuePresentKHR failed (real)");
            if (stageTimes().on)
                stageTimes().realCount.fetch_add(1, std::memory_order_relaxed);
            // Session 40: drain presented-feedback (needs the blocking
            // roundtrip; see drainPresentFeedback docs)
            g_overlay.wsi->drainPresentFeedback();
            {
                // REAL present→scanout: submit time vs the same commit's latch.
                // EMA (alpha 1/8) iInto latencyRealScanMs, used for the GEN adds
                // delta and the experience row.
                const uint64_t idx = presentIdx > 0 ? presentIdx - 1 : 0;
                (void)idx;
                const uint64_t latchNs = g_overlay.wsi->lastPresentLatchNs();
                if (latchNs > 0) {
                    const uint64_t submitNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            tReal0.time_since_epoch()).count());
                    // Session 40 dbl-ledger: row per decoded REAL present:
                    // (frame captureTsNs from the layer, compositor latch).
                    g_ledgerApp.init();
                    g_ledgerApp.publish(capTsNs, latchNs);
                    if (latchNs > submitNs && latchNs - submitNs < 100'000'000ULL) {
                        const float scanMs = static_cast<float>(
                            static_cast<double>(latchNs - submitNs) / 1e6);
                        const float prior = lsfgvk::gui::g_guiState.
                            latencyRealScanMs.load(std::memory_order_relaxed);
                        lsfgvk::gui::g_guiState.latencyRealScanMs.store(
                            prior == 0.0f ? scanMs : prior + (scanMs - prior) / 8.0f,
                            std::memory_order_relaxed);
                    }
                }
            }
            if (capTsNs > 0) {
                const uint64_t nowNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count());
                const double latMs = (nowNs > capTsNs) ? (nowNs - capTsNs) / 1e6 : 0.0;
                lsfgvk::gui::g_guiState.currentLatencyRealMs.store(static_cast<float>(latMs));
                ls::hud::ImGuiHud::pushLatencyMs(static_cast<float>(latMs));
                /* S40+ Phase A: feed the frametime ring (per-REAL-present dt).
                   static prevNs lives in this output loop scope; rings the
                   delta even between stats ticks so the sparkline is honest. */
                {
                    static thread_local uint64_t prevPresentNs{ 0 };
                    if (prevPresentNs != 0) {
                        const double dtN = static_cast<double>(nowNs > prevPresentNs
                            ? nowNs - prevPresentNs : 0);
                        if (dtN > 0.0 && dtN < 1e9)
                            ls::hud::ImGuiHud::pushFrameMs(
                                static_cast<float>(dtN / 1e6));
                    }
                    prevPresentNs = nowNs;
                }
                LSFG_FRAME_DBG("MEASURED LATENCY: REAL present slot %d latency %.2f ms", stagingIdx, latMs);
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
                        vk::Image dumpImg(vk, extent, VK_FORMAT_B8G8R8A8_UNORM,
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
                                rgb[i * 3 + 0] = px[i * 4 + 2];   /* R */
                                rgb[i * 3 + 1] = px[i * 4 + 1];   /* G */
                                rgb[i * 3 + 2] = px[i * 4 + 0];   /* B */
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
                stageTimes().maybeDump();
                if (stop.load(std::memory_order_relaxed)
                        || failed.load(std::memory_order_relaxed))
                    break;

                // realize a freshly-produced frame (drop-oldest/newest).
                // Session 13.21: when idle this BLOCKS on the inbox cv (wakes
                // instantly on arrival; 15 ms cap only pumps WSI events). The
                // taken frame is fully consumed into cur - fds never dropped.
                if (!cur.active) {
                    // Shutdown: never park here. takeNewestWait() returns
                    // nullopt once inbox.stop() is set, and the stop check keeps
                    // the loop from re-entering, so join() cannot hang.
                    if (stop.load(std::memory_order_relaxed)
                            || failed.load(std::memory_order_relaxed))
                        return false;
                    auto pf = inbox.takeNewest();
                    if (!pf)
                        pf = inbox.takeNewestWait(15);
                    if (!pf
                        && (stop.load(std::memory_order_relaxed)
                            || failed.load(std::memory_order_relaxed)))
                        return false;
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
                            /* S40+ default: imgui overlay shows; the legacy
                               seven-segment renders only when asked via
                               LSFGVK_HUD=minimal (or when imgui failed). */
                            static const int hudMode = [] {
                                const char* e = getenv("LSFGVK_HUD");
                                return (e && std::strcmp(e, "minimal") == 0) ? 1 : 0;
                            }();
                            if (hudMode == 1)
                                makeHud();
                            /* S40+ Phase A imgui overlay (per-process,
                               survives stream teardown): create once. */
                            if (!g_imguiInitDone.exchange(true)) {
                                try {
                                    g_imguiHud = new ls::hud::ImGuiHud{ vk,
                                        extent, static_cast<VkFormat>(
                                            g_overlay.imageFormat) };
                                    g_imguiBuiltW.store(extent.width,
                                        std::memory_order_relaxed);
                                    g_imguiBuiltH.store(extent.height,
                                        std::memory_order_relaxed);
                                    /* S41: buildCardOver crashes RADV on the
                                       clean-rebuilt tree (null dispatch in
                                       vkCreate chain). Default = don't build;
                                       opt-in via LSFGVK_CO_EXPERIMENT=1. */
                                    static const bool coExp = [] {
                                        const char* e = getenv("LSFGVK_CO_EXPERIMENT");
                                        return e && e[0] == '1' && !e[1];
                                    }();
                                    if (coExp) g_imguiHud->buildCardOver();
                                    installImguiToggle();
                                } catch (const std::exception& e) {
                                    g_imguiInitDone.store(false);
                                    std::cerr << "lsfg-vk-app: imgui overlay "
                                              << "init failed: " << e.what()
                                              << "\n";
                                }
                            }
                        }
                        cur.active = true;
                        cur.nextDest = 0;
                        cur.stagingIdx = pf->stagingIdx;
                        cur.doneFds = std::move(pf->doneFds);
                        cur.snapFd = pf->snapFd;  // -1 if no snapshot fd
                        cur.captureTsNs = pf->captureTsNs;
                        cur.recvTsNs = pf->recvTsNs;
                        cur.schedDoneNs = pf->schedDoneNs;
                    } else if (wantDropWsi.exchange(false, std::memory_order_relaxed)
                            && g_overlay.wsi) {
                        LSFG_FRAME_DBG("output: idle drop overlay WSI (keep IPC)");
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
                    uint32_t idx{};
                    Clock::time_point tAcq0{};
                    if (stageTimes().on)
                        tAcq0 = Clock::now();
                    if (!acquireImage(idx))
                        break;
                    if (stageTimes().on)
                        stageTimes().add(stageTimes().acquire, elapsedUs(tAcq0, Clock::now()));
                    const VkImage dstImage = swapImages.at(idx);
                    if (cur.doneFds.at(i) >= 0) {
                        if (stageTimes().on) {
                            struct pollfd pfd{
                                .fd = cur.doneFds.at(i),
                                .events = POLLIN,
                            };
                            const auto tSync0 = Clock::now();
                            ::poll(&pfd, 1, -1);
                            stageTimes().add(stageTimes().syncFd,
                                elapsedUs(tSync0, Clock::now()));
                        }
                        importSyncFd(vk, doneWaitSem.at(i).handle(), cur.doneFds.at(i));
                        cur.doneFds.at(i) = -1;   // import consumed the fd
                    }
                    // wait for this command buffer's previous submit to complete
                    Clock::time_point tFence0{};
                    if (stageTimes().on)
                        tFence0 = Clock::now();
                    if (!cbFences.at(cbIdx).wait(vk, UINT64_MAX))
                        throw ls::vulkan_error(VK_TIMEOUT, "cb fence wait failed");
                    if (stageTimes().on)
                        stageTimes().add(stageTimes().fenceWait, elapsedUs(tFence0, Clock::now()));
                    cbFences.at(cbIdx).reset(vk);
                    Clock::time_point tBlit0{};
                    if (stageTimes().on)
                        tBlit0 = Clock::now();
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
                    if (stageTimes().on)
                        stageTimes().add(stageTimes().blit,
                            elapsedUs(tBlit0, Clock::now()));
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
                    // Session 40: same as REAL — arm before the commit.
                    g_overlay.wsi->armPresentFeedback(fbHandle);
                    const auto tGenSubmit = Clock::now();
                    Clock::time_point tPres0{};
                    if (stageTimes().on)
                        tPres0 = Clock::now();
                    const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
                    {
                        static const bool countPresents = [] {
                            const char* e = std::getenv("LSFGVK_PRESENT_COUNT");
                            return e && e[0] == '1' && !e[1];
                        }();
                        if (countPresents) {
                            std::fprintf(stderr, "present-count kind=gen rc=%d\n",
                                static_cast<int>(pres));
                            std::fflush(stderr);
                        }
                    }
                    if (stageTimes().on) {
                        stageTimes().add(stageTimes().genPresent,
                            elapsedUs(tPres0, Clock::now()));
                        stageTimes().genCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                        throw ls::vulkan_error(pres, "QueuePresentKHR failed (generated)");
                    g_overlay.wsi->drainPresentFeedback();
                    {
                        // GEN present→scanout EMA + 'GEN adds' delta vs REAL.
                        const uint64_t latchNs = g_overlay.wsi->lastPresentLatchNs();
                        if (latchNs > 0) {
                            /* S43: log the GEN latch to the dbl-ledger. The REAL
                               half was already recorded and measured dead-tight
                               (8.33 ms p50, 8.34 p90 at ns resolution) while the
                               GEN half — the half a viewer actually perceives as
                               judder — had no numbers at all. Same clock, same
                               row layout, kind-tagged, invisible to the S40 click
                               probe (captureTs=0 is skipped as a stale slot). */
                            g_ledgerApp.init();
                            g_ledgerApp.publishGen(latchNs);
                                    const uint64_t submitNs = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    tGenSubmit.time_since_epoch()).count());
                            if (latchNs > submitNs && latchNs - submitNs < 100'000'000ULL) {
                                const float scanMs = static_cast<float>(
                                    static_cast<double>(latchNs - submitNs) / 1e6);
                                float prior = lsfgvk::gui::g_guiState.
                                    latencyGenScanMs.load(std::memory_order_relaxed);
                                lsfgvk::gui::g_guiState.latencyGenScanMs.store(
                                    prior == 0.0f ? scanMs : prior + (scanMs - prior) / 8.0f,
                                    std::memory_order_relaxed);
                                const float realScan = lsfgvk::gui::g_guiState.
                                    latencyRealScanMs.load(std::memory_order_relaxed);
                                if (realScan > 0.0f)
                                    lsfgvk::gui::g_guiState.latencyGenExtraMs.store(
                                        scanMs - realScan, std::memory_order_relaxed);
                            }
                        }
                    }
                    if (cur.captureTsNs > 0) {
                        const uint64_t nowNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now().time_since_epoch()).count());
                        const double latMs = (nowNs > cur.captureTsNs) ? (nowNs - cur.captureTsNs) / 1e6 : 0.0;
                        lsfgvk::gui::g_guiState.currentLatencyGenMs.store(static_cast<float>(latMs));
                        LSFG_FRAME_DBG("MEASURED LATENCY: GEN present slot %u latency %.2f ms", cur.stagingIdx, latMs);
                        // Session 40 ledger: IPC hop (capture→recv) + app solve
                        // (recv→schedDone excerpts the app's work window)
                        if (cur.recvTsNs > cur.captureTsNs) {
                            const float ipcMs = static_cast<float>(
                                (cur.recvTsNs - cur.captureTsNs) / 1e6);
                            lsfgvk::gui::g_guiState.latencyIpcMs.store(ipcMs);
                            if (cur.schedDoneNs > cur.recvTsNs)
                                lsfgvk::gui::g_guiState.latencyGenSolveMs.store(static_cast<float>(
                                    (cur.schedDoneNs - cur.recvTsNs) / 1e6));
                            static uint32_t ledgerLogged = 0;
                            if (ledgerLogged++ < 5)
                                LSFG_FRAME_DBG("ledger: ipc %.3f ms solve %.3f ms (slot %u)",
                                    ipcMs,
                                    (cur.schedDoneNs > cur.recvTsNs)
                                        ? (cur.schedDoneNs - cur.recvTsNs) / 1e6 : 0.0,
                                    cur.stagingIdx);
                        }
                    }
                    ++presentIdx;
                    ++presentedFrames;
                    ++cur.nextDest;
                    lsfgvk::gui::g_guiState.totalGenPresents.fetch_add(1);
                    lsfgvk::gui::g_guiState.totalPresents.fetch_add(1);
                    LSFG_FRAME_DBG("output: GEN present dest %zu/%zu (slot %u)",
                        i, destCount, cur.stagingIdx);
                } else if (cur.active) {
                    // --- REAL present: this frame's private snapshot -----------
                    std::fprintf(stderr, "[M3] f=%llu tid=%ld REAL present ATTEMPTED "
                        "slot=%u\n", (unsigned long long)cur.stagingIdx, ::gettid(),
                        cur.stagingIdx);
                    const bool presOk = presentReal(cur.stagingIdx,
                        cur.snapFd >= 0 ? cur.snapFd : -1, cur.captureTsNs);
                    std::fprintf(stderr, "[M4] f=%llu tid=%ld REAL present RESULT ok=%d\n",
                        (unsigned long long)cur.stagingIdx, ::gettid(), presOk ? 1 : 0);
                    if (!presOk)
                        break;
                    lastShownStagingIdx = static_cast<int>(cur.stagingIdx);
                    for (int d : cur.doneFds)   // close any never-imported gen fds
                        if (d >= 0)
                            ::close(d);
                    cur.active = false;
                    lsfgvk::gui::g_guiState.totalRealPresents.fetch_add(1);
                    lsfgvk::gui::g_guiState.totalPresents.fetch_add(1);
                    LSFG_FRAME_DBG("output: REAL present (slot %u)", cur.stagingIdx);
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
                maybeHud(Clock::now());

                // stop on a window resize/close (processEvents returns true).
                // Close ends the loop. Resize/configure must NOT — xdg_toplevel
                // sends extra configures after the first present (activated).
                // Treating that as fatal stops GEN/REAL after FRAME 0 and the
                // game's next blocking FRAME send never returns.
                // 1080 boot must not pump/present the 1440 overlay WSI.
                // Concurrent runPresent shares g_overlay; two output threads
                // on one swapchain is FRAME 0 then hang.
                if (!boot1080 && g_overlay.wsi && g_overlay.wsi->processEvents(0)) {
                    LSFG_FRAME_DBG("output: processEvents requested stop (close)");
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
            stageTimes().dump("final");
        } catch (...) {
            stageTimes().dump("catch");
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
        // Per-frame timing for the udmabuf transport. The two questions this
        // pass is meant to answer: does the 36.6 ms park survive on the buffer
        // bounce (step 3), and what does removing the blocking fence wait add
        // on top (step 4)? So measure the copy, the park, and the
        // present-to-present interval, and report p50/p99.
        struct UdmaTiming {
            std::vector<double> copyMs, parkMs, frameMs;
            std::chrono::steady_clock::time_point lastPresent{};
            bool haveLast{ false };
        } udmaT{};
        auto stat = [](std::vector<double>& v, const char* label) {
            if (v.empty()) return;
            std::sort(v.begin(), v.end());
            const double p50 = v[v.size() / 2];
            const double p99 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))];
            std::fprintf(stderr,
                "udmabuf-timing: %-8s n=%zu p50=%.2f p99=%.2f max=%.2f ms\n",
                label, v.size(), p50, p99, v.back());
        };
        auto dumpTiming = [&]() {
            stat(udmaT.copyMs, "copy");
            stat(udmaT.parkMs, "park");
            stat(udmaT.frameMs, "frame");
        };

        // The udmabuf bounce submits its own copy of the staging slot into
        // genSources. It MUST NOT reuse snapCbs/snapFences: the snapshot block
        // below already begin/end/submits snapCbs.at(sidx), and doing that to
        // the same command buffer twice loses the context.
        std::vector<vk::CommandBuffer> bounceCbs;
        std::vector<vk::Fence> bounceFences;
        for (size_t i = 0; i < ls::ipc::STAGING_RING_DEPTH; ++i) {
            snapCbs.emplace_back(vk, vk.transferCmdPoolHandle());
            computeCbs.emplace_back(vk, vk.transferCmdPoolHandle());
            snapFences.emplace_back(vk, true);
            bounceCbs.emplace_back(vk, vk.transferCmdPoolHandle());
            // signalled so the first use of each slot passes the reuse guard
            bounceFences.emplace_back(vk, true);
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
                const auto recvTs = Clock::now();
                auto msg = conn.receive(std::nullopt);
                const auto* frame = std::get_if<ls::ipc::Frame>(&msg);
                if (!frame)
                    continue;

                std::fprintf(stderr, "FRAME recv f=%llu slot=%u\n",
                    (unsigned long long)fidx, frame->stagingIdx);

                int captureFd = conn.takeReceivedFd();
                static bool loggedCap = false;
                if (!loggedCap && captureFd >= 0) {
                    loggedCap = true;
                    char link[64]{};
                    (void)::readlink(("/proc/self/fd/" + std::to_string(captureFd)).c_str(),
                        link, sizeof(link) - 1);
                    LSFG_FRAME_DBG("input: FRAME fd=%d link=%s", captureFd, link);
                }
                static const bool dropGen = std::getenv("LSFGVK_DROP_GEN")
                    && std::getenv("LSFGVK_DROP_GEN")[0] == '1';
                if (dropGen) {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    LSFG_FRAME_DBG("input: DROP_GEN Release (slot %u) (fidx %llu)",
                        frame->stagingIdx, (unsigned long long)fidx);
                    ++fidx;
                    continue;
                }

                // CPU copy / dual-host: render GPU already finished, so Release first is
                // safe. dma-buf still points at the render GPU buffer — Release first
                // lets the render GPU rewrite it while the secondary GPU copies.
                const uint32_t sidxEarly = frame->stagingIdx;
                const bool udmaSlot = sidxEarly < state.udmaFds.size()
                    && state.udmaFds.at(sidxEarly) >= 0;
                const bool dmaHop = (state.shmBytes == 0 && std::getenv("LSFGVK_DUAL_HOST") != nullptr
                    && std::getenv("LSFGVK_DUAL_HOST")[0] == '0');
                // udmabuf: the 9060 has not copied out yet. Releasing now lets
                // the 9070 overwrite the slot mid-copy.
                if (!dmaHop && !udmaSlot
                        && conf.transport != ls::Transport::DecoupledDma) {
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    LSFG_FRAME_DBG("input: Release first (slot %u) (fidx %llu)",
                        frame->stagingIdx, (unsigned long long)fidx);
                }

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
                        LSFG_FRAME_DBG("input: FRAME fd slot %u link='%s' needShare=%d",
                            sidxEarly, nlink > 0 ? link : "?", needShare);
                    }
                    // Protocol: the FIRST fd for a slot is the share; every
                    // later FRAME for that slot carries only the sync fd. Do
                    // NOT gate this on readlink: on the dma-buf path the fd
                    // can arrive as 'anon_inode:sync_file' or with an
                    // uninformative link, and a rejected share leaves dmaFds
                    // at -1 for the rest of the session - no import, no
                    // conversion blit, black FG content. (See the 2026-09-27
                    // black-screen investigation.)
                    const bool isShare = needShare && nlink >= 0;
                    if (needShare && isShare && !udmaSlot) {
                        int keepFd = ::dup(captureFd);
                        if (keepFd < 0)
                            throw ls::error("dup() failed before render dma-buf keep");
                        state.dmaFds.at(sidxEarly) = keepFd;
                        if (sidxEarly < state.aImports.size())
                            state.aImports.at(sidxEarly).reset();
                        LSFG_FRAME_DBG("input: keep render dma-buf slot %u link='%s'",
                            sidxEarly, nlink > 0 ? link : "?");
                        ::close(captureFd);
                        captureFd = -1;
                    }
                }
                // Later FRAMEs on this path also carry the 9060 dma-buf, not a
                // sync fd. The first fd is already kept. Close the dup.
                if (conf.transport == ls::Transport::DecoupledDma
                        && captureFd >= 0
                        && sidxEarly < state.dmaFds.size()
                        && state.dmaFds.at(sidxEarly) >= 0) {
                    ::close(captureFd);
                    captureFd = -1;
                }
                if (dmaHop && captureFd < 0) {
                    conn.send(ls::ipc::Release{ frame->stagingIdx });
                    LSFG_FRAME_DBG("input: dma-buf keep, hop on capture-done (slot %u)",
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
                    // Overlay did not sample this write. Return the slot, or the
                    // ring fills and the layer stops. The dmaHop path keeps the
                    // one slot a coprocessor copy is still reading.
                    if (!dmaHop
                            || static_cast<int>(frame->stagingIdx) != pendingDmaRelease) {
                        conn.send(ls::ipc::Release{ frame->stagingIdx });
                        std::fprintf(stderr,
                            "Release skip slot=%u f=%llu\n",
                            frame->stagingIdx, (unsigned long long)fidx);
                    }
                    LSFG_FRAME_DBG("input: snapshot skip (cb busy) (fidx %llu)",
                        (unsigned long long)fidx);
                } else {
                // The bounce path never submits this fence. Resetting it here
                // leaves it unsignaled, and every later frame skips. Reset only
                // in the branches that pass it to QueueSubmit.
                if (dmaHop && pendingDmaRelease >= 0) {
                    const uint32_t done = static_cast<uint32_t>(pendingDmaRelease);
                    conn.send(ls::ipc::Release{ done });
                    LSFG_FRAME_DBG("input: Release slot %u", done);
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
                        LSFG_FRAME_DBG("input: poll write-complete %d revents=%d wall %.3f ms",
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
                            LSFG_FRAME_DBG("input: Release immediate after dma-in slot %u", sidx);
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
                            LSFG_FRAME_DBG("input: import render dma-buf slot %u (copy)", sidx);
                        } catch (const std::exception& e) {
                            LSFG_FRAME_DBG("input: render dma-buf import failed slot %u: %s",
                                sidx, e.what());
                        }
                    }
                }
                // Host-buffer bounce: the render card DMAs into the shared
                // mapping (state.dmaMaps), and OUR GPU DMAs out of that same
                // mapping into genSources. Deliberately no CPU copy - the shm
                // branch below owns the memcpy and is skipped on this transport
                // because hostPtrs is null there.
                if (state.shmBytes && sidx < state.dmaMaps.size()
                        && state.dmaMaps.at(sidx) && sidx < snapCbs.size()) {
                    if (captureFd >= 0) {
                        // The render card's own sync_fd: signalled only once its
                        // capture blit into this slot has landed. A timeout
                        // abandons the slot rather than copying a torn frame.
                        //
                        // THIS is the park: the app is idle here waiting for
                        // the render GPU. If the 36.6 ms shows up in parkMs
                        // rather than copyMs, the transport is not the cost.
                        const auto parkT0 = std::chrono::steady_clock::now();
                        pollfd pfd{};
                        pfd.fd = captureFd;
                        pfd.events = POLLIN;
                        const int prReady = ::poll(&pfd, 1, 2000);
                        udmaT.parkMs.push_back(
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - parkT0).count());
                        if (prReady <= 0) {
                            ::close(captureFd);
                            captureFd = -1;
                            conn.send(ls::ipc::Release{ sidx });
                            ++g_framesDropped;
                            if (prReady == 0)
                                continue;
                            throw ls::error("poll() on bounce slot-ready failed");
                        }
                        ::close(captureFd);
                        captureFd = -1;
                    }
                    // The shared slot is a BUFFER, not an image. A
                    // dma-buf backed linear image import is not usable on
                    // this driver (queried correctly, every format comes
                    // back with compatibleHandleTypes == 0), and a buffer has
                    // no modifier or pitch for the two GPUs to disagree
                    // about. Proven both legs at 2560x1440, 200 iterations,
                    // no faults: tools/testing/udmabuf_transport_probe.cpp.
                    if (!state.udmaBufs.at(sidx).has_value()) {
                        const int ufd = state.udmaFds.at(sidx);
                        if (ufd < 0)
                            throw ls::error("udmabuf slot has no dma-buf fd");
                        state.udmaBufs.at(sidx).emplace(vk, ::dup(ufd),
                            static_cast<size_t>(state.shmBytes),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                          | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                        LSFG_FRAME_DBG("udmabuf: imported slot %u fd=%d as VkBuffer",
                            sidx, ufd);
                    }
                    auto& bcb = bounceCbs.at(sidx);
                    auto& bFence = bounceFences.at(sidx);
                    if (!bFence.wait(vk, 0)) {
                        // NO DROPS: a busy slot must wait for its own fence.
                        // Falling through here would silently substitute a
                        // CPU copy for a frame we promised to DMA.
                        bFence.wait(vk, UINT64_MAX);
                    }
                    bFence.reset(vk);
                    const auto copyT0 = std::chrono::steady_clock::now();
                    bcb.begin(vk);
                    bcb.copyBufferToImage(vk,
                        state.udmaBufs.at(sidx).value(),
                        *state.genSources.at(sidx),
                        state.width, state.height);
                    bcb.end(vk);
                    {
                        std::lock_guard<std::mutex> lk(submitMtx);
                        bcb.submit(vk, {}, VK_NULL_HANDLE, 0,
                            {}, copyDone.handle(), 0,
                            bFence.handle(), vk.dmaQueueHandle());
                    }
                    std::fprintf(stderr, "copy SUBMITTED f=%llu slot=%u\n",
                        (unsigned long long)fidx, sidx);
                    // The park: this blocking wait is the ~36.6 ms the whole
                    // investigation is chasing. It is KEPT for this pass on
                    // purpose, so udmabuf alone can be measured before the
                    // fence wait is touched.
                    const bool copySignalled = bFence.wait(vk, UINT64_MAX);
                    std::fprintf(stderr, "[M2] f=%llu tid=%ld copy FENCE signalled=%d\n",
                        (unsigned long long)fidx, ::gettid(), copySignalled ? 1 : 0);
                    if (!copySignalled)
                        throw ls::vulkan_error(VK_TIMEOUT,
                            "udmabuf bounce copy fence timed out");
                    conn.send(ls::ipc::Release{ sidx });
                    LSFG_FRAME_DBG("input: Release after udmabuf copy (slot %u)", sidx);
                    const auto copyT1 = std::chrono::steady_clock::now();
                    udmaT.copyMs.push_back(
                        std::chrono::duration<double, std::milli>(copyT1 - copyT0).count());
                    if (udmaT.haveLast)
                        udmaT.frameMs.push_back(
                            std::chrono::duration<double, std::milli>(copyT1 - udmaT.lastPresent).count());
                    udmaT.lastPresent = copyT1;
                    udmaT.haveLast = true;
                    LSFG_FRAME_DBG("udmabuf: GPU copy shared buffer -> genSources slot %u",
                        sidx);
                    if (udmaT.frameMs.size() == 200) {
                        dumpTiming();
                        std::fprintf(stderr,
                            "udmabuf-timing: 200 frames captured; "
                            "baseline p99 15.12 ms, doubled p99 14.68 ms\n");
                    }
                }

                if (state.shmBytes && sidx < state.shmMaps.size()
                        && state.shmMaps.at(sidx) && state.hostPtrs.at(sidx)) {
                    if (captureFd >= 0) {
                        // BLOCK until the render card says this slot's blit has
                        // actually landed, then read it.
                        //
                        // This used to be `::poll(&pfd, 1, 1)` with the result
                        // DISCARDED - a 1 ms wait that gave up silently and read
                        // the slot anyway. captureFd is the render card's
                        // slot-ready sync semaphore: it is signalled only when
                        // the capture blit into this slot has completed. Reading
                        // before that yields a TORN capture (part of frame N,
                        // part of frame N+1), and the doubler then interpolates
                        // against garbage. Nothing downstream can detect it.
                        //
                        // A large timeout is a FAILURE indication, not licence
                        // to read: on timeout we never touch the slot, hand it
                        // straight back, and surface the error.
                        pollfd pfd{};
                        pfd.fd = captureFd;
                        pfd.events = POLLIN;
                        const int prReady = ::poll(&pfd, 1, 2000);
                        if (prReady <= 0) {
                            // Slot abandoned unread - never a torn read.
                            ::close(captureFd);
                            captureFd = -1;
                            if (!dmaHop)
                                conn.send(ls::ipc::Release{ sidx });
                            ++g_framesDropped;   // never torn-read; counted
                            if (prReady == 0)
                                continue;
                            throw ls::error("poll() on capture slot-ready failed");
                        }
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
                    LSFG_FRAME_DBG("input: posix-shm memcpy slot %u", sidx);

                    // Session-40 click-response detector (LSFGVK_CODETECT=1):
                    // downsampled grid RMS-diff of the newest captured game frame
                    // vs the previous copy of THIS slot. A large content jump
                    // right after uclick's keystroke injection = the in-game
                    // visible response ⇒ candidate click→frame latency sample.
                    // Reads hostPtrs (CPU-side copy: shm path only), 1/64 pixels.
                    static const bool codetect =
                        std::getenv("LSFGVK_CODETECT") != nullptr
                        && std::getenv("LSFGVK_CODETECT")[0] == '1';
                    if (codetect && state.shmBytes) {
                        static std::vector<uint8_t> prevGrid{};
                        static uint32_t prevHotSlot = 0xffffffffu;
                        const auto* px = static_cast<const uint8_t*>(
                            state.hostPtrs.at(sidx));
                        const size_t tot = static_cast<size_t>(w) * h;
                        const size_t step = px ? (16 * 4) : 16; /* 16-px grid */
                        uint64_t sum = 0; size_t n = 0;
                        std::vector<uint8_t> grid;
                        grid.reserve((tot / 256) + 8);
                        const uint64_t recvNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                recvTs.time_since_epoch()).count());
                        for (size_t i = 0; i + 15 < tot; i += 256) {
                            // luminance-ish (G dominant) single byte
                            const uint8_t g = px[i * 4 + 1];
                            grid.push_back(g);
                            if (!prevGrid.empty() && n < prevGrid.size()) {
                                const int d =
                                    static_cast<int>(g) -
                                    static_cast<int>(prevGrid[n]);
                                sum += static_cast<uint64_t>(d * d);
                                ++n;
                            }
                        }
                        const double rms = n
                            ? std::sqrt(static_cast<double>(sum) / n) : 0.0;
                        if (rms > 24.0) {
                            lsfgvk::gui::g_guiState.inputLatencyValid.store(true);
                            FILE* f = std::fopen("/tmp/lsfg-codeetect.log", "a");
                            if (f) {
                                std::fprintf(f, "ACT %llu slot%u fidx%llu rms%.1f\n",
                                    (unsigned long long)recvNs, sidx,
                                    (unsigned long long)fidx, rms);
                                std::fclose(f);
                            }
                        }
                        prevGrid = std::move(grid);
                        (void)prevHotSlot;
                    }
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
                            LSFG_FRAME_DBG("dumped %s", path);
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
                        LSFG_FRAME_DBG("input: poll frame-ready %d revents=%d", pr, pfd.revents);
                    }
                    if (sidx < state.dmaFds.size() && state.dmaFds.at(sidx) >= 0) {
                        dma_buf_import_sync_file imp{};
                        imp.flags = DMA_BUF_SYNC_WRITE;
                        imp.fd = captureFd;
                        if (::ioctl(state.dmaFds.at(sidx), DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &imp) != 0) {
                            static int nImp = 0;
                            if (nImp < 4) {
                                LSFG_FRAME_DBG("input: IMPORT_SYNC_FILE WRITE errno=%d", errno);
                                ++nImp;
                            }
                        } else {
                            static bool impOk = false;
                            if (!impOk) {
                                LSFG_FRAME_DBG("input: IMPORT_SYNC_FILE WRITE ok slot %u", sidx);
                                impOk = true;
                            }
                        }
                    }
                    importSyncFd(vk, frameReadySem.handle(), captureFd);
                    captureFd = -1;
                    waitWriteDone = true;
                }
                // Source for the present-time blit. Three cases:
                //   aImports  - cross-device imported capture (p2p path)
                //   hostImages- udmabuf bounce: our own import of the shared
                //               system memory, which is the whole point of
                //               that transport
                //   sourceImages - POSIX-shm staging image
                // On the bounce path sourceImages is deliberately empty, so
                // falling back to it would throw "lazy: no value present".
                // On the udmabuf path the bounce above ALREADY copied the
                // shared buffer into genSources, so genSources is both the
                // bounce destination and the present source. There is no
                // separate host image to blit from, and re-blitting would
                // overwrite the frame we just DMA'd.
                // DecoupledDma: the FRAME fd is the layer's 9060 VRAM image.
                // Import it once, then before every copy export that dma-buf's
                // sync file and poll it. EXPLICIT_SYNC means the import itself
                // does not wait. Copy into genSources, then release the slot.
                bool decoupledFilled = false;
                if (conf.transport == ls::Transport::DecoupledDma
                        && sidx < state.dmaFds.size()
                        && state.dmaFds.at(sidx) >= 0) {
                    if (!state.aImports.at(sidx).has_value()) {
                        try {
                            const vk::ImageLayout aLayout{
                                .mode = vk::ImageMode::Linear,
                                .rowPitch = state.width * 4u,
                            };
                            int tryFd = ::dup(state.dmaFds.at(sidx));
                            if (tryFd < 0)
                                throw ls::error("dup() failed before 9060 dma-buf import");
                            state.aImports.at(sidx).emplace(vk,
                                VkExtent2D{ state.width, state.height },
                                VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                tryFd, std::nullopt, aLayout);
                            std::cerr << "decoupled: imported 9060 dma-buf slot="
                                << sidx << " fd=" << state.dmaFds.at(sidx) << "\n";
                        } catch (const std::exception& e) {
                            std::cerr << "decoupled: import failed slot=" << sidx
                                << " " << e.what() << "\n";
                        }
                    }
                    if (state.aImports.at(sidx).has_value()
                            && state.genSources.at(sidx).has_value()) {
                        const auto pollT0 = std::chrono::steady_clock::now();
                        int syncFd = -1;
                        int pr = -1;
                        dma_buf_export_sync_file exp{};
                        exp.flags = DMA_BUF_SYNC_READ;
                        exp.fd = -1;
                        if (::ioctl(state.dmaFds.at(sidx),
                                DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0)
                            syncFd = exp.fd;
                        if (syncFd >= 0) {
                            pollfd pfd{};
                            pfd.fd = syncFd;
                            pfd.events = POLLIN;
                            pr = ::poll(&pfd, 1, 2000);
                            ::close(syncFd);
                        } else {
                            std::cerr << "decoupled: EXPORT_SYNC_FILE failed slot="
                                << sidx << " errno=" << errno << "\n";
                        }
                        const double pollMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - pollT0).count();
                        const auto copyT0 = std::chrono::steady_clock::now();
                        if (!snapCbFence.wait(vk, 0))
                            snapCbFence.wait(vk, UINT64_MAX);
                        snapCbFence.reset(vk);
                        cb.begin(vk);
                        cb.copyImage(vk,
                            {
                                makeBlitBarrier(state.aImports.at(sidx).mut().handle(),
                                    VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                                makeBlitBarrier(state.genSources.at(sidx).mut().handle(),
                                    VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                            },
                            { state.aImports.at(sidx).mut().handle(),
                              state.genSources.at(sidx).mut().handle() },
                            imgExtent,
                            {
                                makeBlitBarrier(state.genSources.at(sidx).mut().handle(),
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                            });
                        cb.end(vk);
                        {
                            std::lock_guard<std::mutex> lk(submitMtx);
                            cb.submit(vk, {}, VK_NULL_HANDLE, 0,
                                {}, VK_NULL_HANDLE, 0,
                                snapCbFence.handle(),
                                vk.dmaQueueHandle());
                        }
                        snapCbFence.wait(vk, UINT64_MAX);
                        const double copyMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - copyT0).count();
                        std::cerr << "decoupled-copy slot=" << sidx
                            << " pollMs=" << pollMs
                            << " pr=" << pr
                            << " submitToFenceMs=" << copyMs << "\n";
                        conn.send(ls::ipc::Release{ sidx });
                        decoupledFilled = true;
                    }
                }
                const bool bounceSrc = state.udmaBufs.at(sidx).has_value()
                    || decoupledFilled;
                auto& srcLazy = bounceSrc
                    ? state.genSources.at(sidx)
                    : (state.aImports.at(sidx).has_value()
                        ? state.aImports.at(sidx)
                        : state.sourceImages.at(sidx));
                auto& snapLazy = state.genSources.at(sidx);
                const bool fromA = state.aImports.at(sidx).has_value() && !bounceSrc;
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
                if (bounceSrc) {
                    // genSources already holds this frame; the bounce fence
                    // orders it against the present. Nothing to snapshot.
                } else if (swizzleBlit) {
                    static bool loggedBlit = false;
                    if (!loggedBlit) {
                        loggedBlit = true;
                        LSFG_FRAME_DBG("input: dma-buf hop copy+compute %u -> RGBA",
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
                    snapCbFence.reset(vk);
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
                snapCbFence.reset(vk);
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
                // On the bounceSrc path there is NO submit above that signals
                // snapshotSem, and vk::Semaphore::exportFd() on a SYNC_FD
                // semaphore BLOCKS in the driver (drmSyncobjTimelineWait) until a
                // signal for it is submitted. Exporting it there hung the input
                // thread inside RADV on frame 0 (2026-09-28).
                //
                // We skip the export on that path. This is ONLY safe today
                // because bFence.wait(vk, UINT64_MAX) above blocks the host
                // until the bounce copy has fully retired, so presentReal()'s
                // blit is already ordered against it. snapFd stays -1, the
                // documented "no snapshot" value: PendingFrame initialises it to
                // -1 and presentReal() guards with `if (snapFd >= 0)`.
                //
                // !! When that blocking fence wait is replaced by
                // submit-and-signal, THIS PATH NEEDS A REAL SIGNAL AGAIN.
                // Removing the host-side wait removes the only thing ordering
                // the bounce copy against presentReal(); a submit signalling
                // snapshotSem (or an equivalent fence handed to the blit) must
                // be added here or the present will race the copy.
                //
                // The fd never crosses app.sock: it is app-local, consumed only
                // by presentReal(), so the layer is unaffected by skipping it.
                if (!bounceSrc) {
                    snapFd = snapshotSem.exportFd(vk);
                }
                LSFG_FRAME_DBG("input: snapshot submitted xferQ=%d sameAsGfx=%d (fidx %llu)",
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
                        LSFG_FRAME_DBG("input: EMPTY_%s QueueSubmit period=%d (fidx %llu)",
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
                LSFG_FRAME_DBG("input: scheduleFrames took %lld us (fidx %llu)",
                    elapsedUs(tSched0, Clock::now()), (unsigned long long)fidx);

                PendingFrame pf{ std::move(doneFds), snapFd,
                    frame->stagingIdx, frame->captureTsNs };
                pf.recvTsNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        recvTs.time_since_epoch()).count());
                pf.schedDoneNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count());
                inbox.push(std::move(pf));
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
    // Order matters: set stop, then wake the output thread's condvar wait, THEN
    // join. Without inbox.stop() a worker parked in takeNewestWait() never
    // returns and this hangs forever instead of surfacing the error that
    // caused the teardown (seen 2026-09-28: input thread stuck in exportFd,
    // runPresent stuck in join).
    inbox.stop();   // the output thread's cv wait must be woken, or join hangs
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
    /* S43b: the imgui card is shared by every CONCURRENT stream (one
       std::thread per connection; RE2 opens three in one session), so it must
       NOT be destroyed per stream — that freed it under its live siblings and
       produced the SIGSEGV in ImGui_ImplVulkan_Shutdown, which in turn left the
       game quit with nothing on the other end of its IPC socket. Only the LAST
       stream out destroys it, when no sibling can still be inside the card. */
    if (g_imguiUsers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lk(submitMtx);
        vk.df().DeviceWaitIdle(vk.dev());
        delete g_imguiHud;
        g_imguiHud = nullptr;
        g_imguiInitDone.store(false);
        g_imguiBuiltW.store(0, std::memory_order_relaxed);
        g_imguiBuiltH.store(0, std::memory_order_relaxed);
    }
    if (inputError)
        std::rethrow_exception(inputError);
    if (outputError)
        std::rethrow_exception(outputError);
}

void releaseOverlayWsi(const vk::Vulkan& vk) {
    if (!g_overlay.wsi)
        return;
    LSFG_FRAME_DBG("release overlay WSI (no live stream)");
    if (g_overlay.swapchain != VK_NULL_HANDLE)
        vk.df().DestroySwapchainKHR(vk.dev(), g_overlay.swapchain, VK_NULL_HANDLE);
    g_overlay.swapchain = VK_NULL_HANDLE;
    g_overlay.swapImages.clear();
    g_overlay.wsi->destroy();
    g_overlay.wsi.reset();
}

} // namespace ls::presentation
