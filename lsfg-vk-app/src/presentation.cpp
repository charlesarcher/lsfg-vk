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

#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

namespace ls::presentation {
namespace {
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
} // namespace

void runPresent(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const vk::Vulkan& vk, lsfgvk::backend::Instance& backend,
        const ls::GameConf& conf, std::string_view session,
        const std::atomic<bool>& stop) {
    const uint32_t w = state.width, h = state.height;

    // --- surface backend + window/surface on the transport vk ----------------
    std::unique_ptr<ls::wsi::SurfaceBackend> wsi;
    if (session == "wayland") {
        wsi = ls::wsi::createWaylandSurfaceBackend();
    } else {
        // default to X11 for "x11" or "auto" (XWayland)
        wsi = ls::wsi::createX11SurfaceBackend();
    }
    if (!wsi->connect(session))
        throw ls::error("could not connect the surface backend for session: " + std::string(session));

    if (verboseEnabled())
        std::cerr << "lsfg-vk-app: using " << (session == "wayland" ? "Wayland" : "X11") << " surface backend\n";

    // --- resolve the target output: an explicit output must match a connector
    //     exactly; absent/empty selects the primary/active output.
    const auto outputs = wsi->outputs();
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

    const auto handle = wsi->createWindow(outputName, VkExtent2D{ w, h }, 0u);
    VkSurfaceKHR surface = wsi->createSurface(vk, handle);

    // --- query caps, then pick an extent the surface will accept (> 0x0) -----
    VkSurfaceCapabilitiesKHR caps{};
    std::vector<VkColorSpaceKHR> colorspaces;
    wsi->surfaceCaps(surface, caps, colorspaces);

    VkExtent2D extent{ w, h };
    // only clamp to caps when the reported range is sane (min<=max, max>0);
    // RADV/XCB under XWayland can report a degenerate 0x0 max, so fall back to
    // the requested extent clamped to a sane [1, 16384] band (main.cpp:361-377).
    if (caps.minImageExtent.width <= caps.maxImageExtent.width
            && caps.maxImageExtent.width > 0 && caps.maxImageExtent.height > 0) {
        extent.width = clamp32(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp32(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    } else {
        extent.width = clamp32(w, 1u, 16384u);
        extent.height = clamp32(h, 1u, 16384u);
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

    // --- create a FIFO color-attachment swapchain ----------------------------
    // minImageCount >= 3: each frame presents (destCount generated + 1 real)
    // images, so we keep at least 3 to avoid FIFO backpressure collapsing.
    const uint32_t minImages = caps.minImageCount < 2 ? 3 : caps.minImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = minImages;
    ci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    ci.imageColorSpace = colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.pQueueFamilyIndices = nullptr;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = compositingAlpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    const auto createRes = vk.df().CreateSwapchainKHR(vk.dev(), &ci, VK_NULL_HANDLE, &swapchain);
    if (createRes != VK_SUCCESS)
        throw ls::vulkan_error(createRes, "CreateSwapchainKHR failed");

    uint32_t imageCount{};
    if (vk.df().GetSwapchainImagesKHR(vk.dev(), swapchain, &imageCount, nullptr) != VK_SUCCESS)
        throw ls::vulkan_error("failed to enumerate swapchain images");
    std::vector<VkImage> swapImages(imageCount);
    if (vk.df().GetSwapchainImagesKHR(vk.dev(), swapchain, &imageCount, swapImages.data())
            != VK_SUCCESS)
        throw ls::vulkan_error("failed to enumerate swapchain images");

    // --- per-frame work objects, pooled for the loop's lifetime --------------
    // acquireSem is never signaled, so it is always unsignaled and safe to reuse
    // for every AcquireNextImageKHR; the signal-semaphore ring holds one entry
    // per present (destCount generated + 1 real) and is consumed by present().
    vk::CommandBuffer cmdbuf{vk};
    vk::Semaphore acquireSem{vk};
    vk::Fence fence{vk};
    std::vector<vk::Semaphore> doneWaitSem;                 // one per destination
    for (const auto& dst : state.destinationImages)
        doneWaitSem.emplace_back(vk, std::nullopt,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
    const size_t destCount = state.destinationImages.size();
    const size_t presentCount = destCount + 1;
    std::vector<vk::Semaphore> signalSem;   // one present per entry, non-copyable
    signalSem.reserve(presentCount);
    for (size_t i = 0; i < presentCount; ++i)
        signalSem.emplace_back(vk);

    // RAII guard: tear down swapchain + surface + connection on EVERY exit path
    // (normal return, stop, or throw) so no WSI handle leaks. Declared last so
    // it is destroyed first, before the vk:: objects below.
    struct SwapchainGuard {
        const vk::Vulkan* vk;
        VkSwapchainKHR swapchain;
        ls::wsi::SurfaceBackend* wsi;
        ~SwapchainGuard() {
            if (swapchain != VK_NULL_HANDLE && vk != nullptr)
                vk->df().DestroySwapchainKHR(vk->dev(), swapchain, VK_NULL_HANDLE);
            if (wsi != nullptr)
                wsi->destroy();
        }
    };
    SwapchainGuard guard{ &vk, swapchain, wsi.get() };

    // --- pacing / backpressure / stall policy state ---------------------------
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastFrameTime = Clock::now();
    Clock::time_point statsLastTime = Clock::now();
    uint64_t frameCount = 0;
    bool idleLogged = false;
    uint32_t lastFrameStagingIdx = 0;
    const auto idleThreshold = std::chrono::seconds(5);
    const auto statsInterval = std::chrono::seconds(1);

    // --- the present loop ----------------------------------------------------
    size_t fidx{ 0 };
    while (!stop.load(std::memory_order_relaxed)) {
        // poll the socket first so a pending FRAME is read without a blocking
        // receive, and we stay looped to send backpressure RELEASE each cycle.
        // a spurious wake (or the idle timeout) simply re-polls.
        pollfd pfd{};
        pfd.fd = conn.fd();
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            throw ls::ipc::socket_error("poll() on present loop", errno);
        }

        const auto now = Clock::now();

        // idle detection: if no FRAME for >5 s, present the last real frame once
        // and log a single notification. the window stays alive.
        if (pr == 0 || !(pfd.revents & POLLIN)) {
            if (!idleLogged && now - lastFrameTime >= idleThreshold) {
                // present the last captured game frame to keep the window alive
                uint32_t idx{};
                const auto aq = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                    UINT64_MAX, acquireSem.handle(), VK_NULL_HANDLE, &idx);
                if (aq == VK_SUCCESS || aq == VK_SUBOPTIMAL_KHR) {
                    const VkImage dstImage = swapImages.at(idx);
                    auto& srcImage = state.sourceImages.at(lastFrameStagingIdx);
                    cmdbuf.begin(vk);
                    cmdbuf.blitImage(vk,
                        {
                            makeBlitBarrier(srcImage.mut().handle(),
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                            makeBlitBarrier(dstImage,
                                VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                        },
                        { srcImage.mut().handle(), dstImage },
                        VkExtent2D{ w, h },
                        {
                            makeBlitBarrier(dstImage,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                        }
                    );
                    cmdbuf.end(vk);
                    cmdbuf.submit(vk,
                        { acquireSem.handle() }, VK_NULL_HANDLE, 0,
                        { signalSem.at(destCount).handle() }, VK_NULL_HANDLE, 0,
                        VK_NULL_HANDLE
                    );
                    const VkPresentInfoKHR presentInfo{
                        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                        .waitSemaphoreCount = 1,
                        .pWaitSemaphores = &signalSem.at(destCount).handle(),
                        .swapchainCount = 1,
                        .pSwapchains = &swapchain,
                        .pImageIndices = &idx,
                    };
                    (void)vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
                }
                std::cerr << "lsfg-vk-app: idle >5 s, presenting last frame (slot " << lastFrameStagingIdx << ")\n";
                idleLogged = true;
            }
            // per-second stats when verbose
            if (verboseEnabled() && now - statsLastTime >= statsInterval) {
                const double fps = static_cast<double>(frameCount) /
                    std::chrono::duration<double>(now - statsLastTime).count();
                std::cerr << "lsfg-vk-app: " << static_cast<uint32_t>(fps) << " fps, slots "
                          << "2/2, queued 0\n";
                frameCount = 0;
                statsLastTime = now;
            }
            continue;
        }

        // receive is bounded by SO_RCVTIMEO (set on the socket by runStream).
        auto msg = conn.receive(std::nullopt);
        const auto* frame = std::get_if<ls::ipc::Frame>(&msg);
        if (!frame)
            // steady state is one FRAME per staging slot per generation cycle;
            // anything else (a stray message) is silently drained.
            continue;

        // reset idle state on new frame
        idleLogged = false;
        lastFrameTime = now;
        lastFrameStagingIdx = frame->stagingIdx;
        ++frameCount;

        // the FRAME carries the capture sync fd; scheduleFrames consumes it.
        const int captureFd = conn.takeReceivedFd();

        // cross-frame gate: the previous frame's last submit signaled the
        // fence; wait for it before reusing the command buffer / re-reading the
        // source images (mirrors the layer's renderFence gate).
        if (fidx && !fence.wait(vk, 150ULL * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
        fence.reset(vk);

        std::vector<int> doneFds;
        try {
            doneFds = backend.scheduleFrames(*state.context, captureFd);
        } catch (const std::exception& e) {
            throw ls::error("failed to schedule frames", e);
        }
        if (doneFds.size() != destCount)
            throw ls::error("backend returned " + std::to_string(doneFds.size())
                + " done fds, expected " + std::to_string(destCount));

        const VkExtent2D imgExtent{ w, h };

        // --- generated presents: one per destination image -----------------
        for (size_t i = 0; i < destCount; ++i) {
            // acquire a swapchain image (wait: acquireSem is always unsignaled).
            uint32_t idx{};
            const auto aq = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                UINT64_MAX, acquireSem.handle(), VK_NULL_HANDLE, &idx);
            if (aq != VK_SUCCESS && aq != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(aq, "AcquireNextImageKHR failed");
            const VkImage dstImage = swapImages.at(idx);

            // wait for this generated frame via its sync fd.
            const int dFd = doneFds.at(i);
            if (dFd >= 0)
                importSyncFd(vk, doneWaitSem.at(i).handle(), dFd);

            // blit destinationImage[i] -> swapchain image -> present.
            cmdbuf.begin(vk);
            cmdbuf.blitImage(vk,
                {
                    makeBlitBarrier(state.destinationImages.at(i).mut().handle(),
                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                },
                { state.destinationImages.at(i).mut().handle(), dstImage },
                imgExtent,
                {
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                }
            );
            cmdbuf.end(vk);
            cmdbuf.submit(vk,
                { acquireSem.handle(), doneWaitSem.at(i).handle() },
                VK_NULL_HANDLE, 0,
                { signalSem.at(i).handle() }, VK_NULL_HANDLE, 0,
                VK_NULL_HANDLE
            );

            const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &signalSem.at(i).handle(),
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &idx,
            };
            const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
            if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(pres, "QueuePresentKHR failed (generated)");
        }

        // --- ONE real frame: blit the latest captured game frame -----------
        {
            uint32_t idx{};
            const auto aq = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                UINT64_MAX, acquireSem.handle(), VK_NULL_HANDLE, &idx);
            if (aq != VK_SUCCESS && aq != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(aq, "AcquireNextImageKHR failed (real)");
            const VkImage dstImage = swapImages.at(idx);

            auto& srcImage = state.sourceImages.at(frame->stagingIdx);
            cmdbuf.begin(vk);
            cmdbuf.blitImage(vk,
                {
                    makeBlitBarrier(srcImage.mut().handle(),
                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                },
                { srcImage.mut().handle(), dstImage },
                imgExtent,
                {
                    makeBlitBarrier(dstImage,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                }
            );
            cmdbuf.end(vk);
            // the real present is the last submit of the frame: signal the fence
            // so the next frame's gate waits for all of this frame's work.
            cmdbuf.submit(vk,
                { acquireSem.handle() }, VK_NULL_HANDLE, 0,
                { signalSem.at(destCount).handle() }, VK_NULL_HANDLE, 0,
                fence.handle()
            );

            const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &signalSem.at(destCount).handle(),
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &idx,
            };
            const auto pres = vk.df().QueuePresentKHR(vk.queue(), &presentInfo);
            if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(pres, "QueuePresentKHR failed (real)");
        }

        if (verboseEnabled())
            std::cerr << "[gen gen real]\n";

        // backpressure: tell the layer this slot is free so it can recapture
        // into it (the ring is 2 deep, so the layer will not reuse it until a
        // cycle later by which point this read is long done).
        conn.send(ls::ipc::Release{ frame->stagingIdx });

        ++fidx;

        // stop on a window resize/close (processEvents returns true for both);
        // teardown happens via the guard on return.
        if (wsi->processEvents(16))
            break;
    }

    // guard tears down the swapchain + surface + connection on return.
}

} // namespace ls::presentation
