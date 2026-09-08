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

#include "lsfg-vk-app/hud.hpp"
#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"

#include <algorithm>
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
#include <cerrno>
#include <xf86drm.h>
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
            VkImageLayout oldLayout, VkAccessFlags newAccess, VkImageLayout newLayout) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = oldAccess;
        b.dstAccessMask = newAccess;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        return b;
    }

    /// 9060 Vulkan rejects 9070 dma-bufs (GetMemoryFdPropertiesKHR
    /// INVALID_EXTERNAL_HANDLE). Re-export through the 9060 DRM node so
    /// Vulkan sees a local GEM handle.
    int primeReexportOn9060(int foreignFd) {
        static int drmFd = -2;
        if (drmFd == -2) {
            drmFd = ::open("/dev/dri/renderD130", O_RDWR | O_CLOEXEC);
            dbg("prime open renderD130 fd=%d errno=%d", drmFd, drmFd < 0 ? errno : 0);
        }
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
    // MAILBOX (else IMMEDIATE) matches Windows LS: the 9060 presents as fast
    // as GEN+REAL are ready, toward 240 Hz. FIFO stays the fallback.
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
    };
    if (g_overlay.wsi) {
        dbg("reusing overlay WSI %ux%u (stream %ux%u)",
            g_overlay.extent.width, g_overlay.extent.height, w, h);
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
        auto presentReal = [&](int stagingIdx, int snapFd) -> bool {
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
            ++presentIdx;
            ++presentedFrames;
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
                        // stream is XIO on Xwayland :0 (isolated game dies
                        // before 1440). Drain FRAMEs, no WSI, until 1440.
                        const bool boot1080 = (w == 1920 && h == 1080);
                        if (!boot1080) {
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
                    ++presentIdx;
                    ++presentedFrames;
                    ++cur.nextDest;
                    dbg("output: GEN present dest %zu/%zu (slot %u)",
                        i, destCount, cur.stagingIdx);
                } else if (cur.active) {
                    // --- REAL present: this frame's private snapshot -----------
                    if (!presentReal(cur.stagingIdx, cur.snapFd >= 0 ? cur.snapFd : -1))
                        break;
                    lastShownStagingIdx = static_cast<int>(cur.stagingIdx);
                    for (int d : cur.doneFds)   // close any never-imported gen fds
                        if (d >= 0)
                            ::close(d);
                    cur.active = false;
                    dbg("output: REAL present (slot %u)", cur.stagingIdx);
                } else if (lastShownStagingIdx >= 0) {
                    // --- HOLD-LAST: nothing newer to show ---------------------
                    // Session 13.13: the last REAL present is still on screen;
                    // re-blitting at 240 Hz burns the 9060 XT. Session 13.21:
                    // do NOT consume anything here - the loop top's blocking
                    // take fills cur the instant a frame arrives. Just pump
                    // WSI events non-blockingly and continue.
                } else {
                    // first frame not shown yet: loop top's takeNewestWait
                    // blocks for it; nothing to do here.
                }

                maybeStats(Clock::now());

                // stop on a window resize/close (processEvents returns true).
                if (g_overlay.wsi && g_overlay.wsi->processEvents(0))
                    break;
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
        vk::CommandBuffer cb{ vk, vk.transferCmdPoolHandle() };
        vk::Fence snapCbFence{ vk, true };
        vk::CommandBuffer emptyGenCb{ vk };
        vk::Fence emptyGenFence{ vk, true };
        vk::CommandBuffer emptyXferCb{ vk, vk.transferCmdPoolHandle() };
        vk::Fence emptyXferFence{ vk, true };
        uint64_t fidx{ 0 };
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

                conn.send(ls::ipc::Release{ frame->stagingIdx });
                dbg("input: Release first (slot %u) (fidx %llu)",
                    frame->stagingIdx, (unsigned long long)fidx);

                static const bool skipSnap = (std::getenv("LSFGVK_SKIP_SNAP")
                    && std::getenv("LSFGVK_SKIP_SNAP")[0] == '1')
                    || (std::getenv("LSFGVK_EMPTY_GEN")
                        && std::getenv("LSFGVK_EMPTY_GEN")[0] == '1')
                    || (std::getenv("LSFGVK_EMPTY_XFER")
                        && std::getenv("LSFGVK_EMPTY_XFER")[0] == '1');
                int snapFd = -1;
                if (!skipSnap) {
                if (!snapCbFence.wait(vk, 0)) {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    dbg("input: snapshot skip (cb busy) (fidx %llu)",
                        (unsigned long long)fidx);
                } else {
                snapCbFence.reset(vk);
                const uint32_t sidx = frame->stagingIdx;
                if (sidx >= ls::ipc::STAGING_RING_DEPTH)
                    throw ls::error("FRAME stagingIdx out of range");
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
                } else if (captureFd >= 0 && !state.aImports.at(sidx).has_value()) {
                    try {
                        const vk::ImageLayout aLayout{
                            .mode = (state.negotiatedModifier == vk::EXCHANGE_MODIFIER_LINEAR)
                                ? vk::ImageMode::Linear : vk::ImageMode::DrmModifier,
                            .drmModifier = state.negotiatedModifier,
                            .rowPitch = state.rowPitch,
                        };
                        const std::vector<uint32_t> families{
                            vk.queueFamilyIndex(), vk.transferQueueFamilyIndex()
                        };
                        const VkSharingMode share = vk.hasTransferQueue()
                            ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
                        const VkImageUsageFlags aUsage =
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
                        auto importOne = [&](int fd) {
                            state.aImports.at(sidx).emplace(vk,
                                VkExtent2D{ state.width, state.height },
                                state.sourceFormat, aUsage,
                                fd, std::nullopt, aLayout, share, families);
                        };
                        int tryFd = ::dup(captureFd);
                        if (tryFd < 0)
                            throw ls::error("dup() failed before 9070 import");
                        try {
                            importOne(tryFd);
                            tryFd = -1;
                            ::close(captureFd); captureFd = -1;
                            dbg("input: imported 9070 capture slot %u", sidx);
                        } catch (const std::exception& e1) {
                            dbg("input: direct import failed slot %u: %s",
                                sidx, e1.what());
                            const int localFd = primeReexportOn9060(captureFd);
                            if (localFd < 0)
                                throw;
                            try {
                                importOne(localFd);
                                dbg("input: PRIME-imported 9070 capture slot %u", sidx);
                            } catch (...) {
                                ::close(localFd);
                                throw;
                            }
                            if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                        }
                    } catch (const std::exception& e) {
                        dbg("input: 9070 dma-buf import failed slot %u: %s",
                            sidx, e.what());
                        if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
                    }
                } else if (captureFd >= 0) {
                    ::close(captureFd);
                    captureFd = -1;
                }
                auto& srcLazy = state.aImports.at(sidx).has_value()
                    ? state.aImports.at(sidx)
                    : state.sourceImages.at(sidx);
                auto& snapLazy = state.genSources.at(sidx);
                const VkImageLayout srcOld = state.shmBytes
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
                const VkImageLayout srcNew = state.shmBytes
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                cb.begin(vk);
                cb.copyImage(vk,
                    {
                        makeBlitBarrier(srcLazy.mut().handle(),
                            VK_ACCESS_NONE, srcOld,
                            VK_ACCESS_TRANSFER_READ_BIT, srcNew),
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
                        std::vector<VkSemaphore>{}, VK_NULL_HANDLE, 0,
                        {}, snapshotSem.handle(), 0,
                        snapCbFence.handle(),
                        vk.transferQueueHandle());
                }
                snapFd = snapshotSem.exportFd(vk);
                dbg("input: snapshot submitted xferQ=%d (fidx %llu)",
                    vk.transferQueueHandle() != VK_NULL_HANDLE,
                    (unsigned long long)fidx);
                }
                } else {
                    if (captureFd >= 0) { ::close(captureFd); captureFd = -1; }
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

                inbox.push(PendingFrame{ std::move(doneFds), snapFd, frame->stagingIdx });
                ++frameCount;
                ++fidx;
            }
        } catch (...) {
            if (!inputError)
                inputError = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
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
