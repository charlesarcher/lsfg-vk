/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "swapchain.hpp"

#include "lsfg-vk-layer/capture_context.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk::layer;

namespace {
    VkImageMemoryBarrier barrierHelper(VkImage handle,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout oldLayout,
            VkImageLayout newLayout) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = handle,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
    }
}

CaptureContext::CaptureContext(const vk::Vulkan& vk, ls::GameConf profile,
        SwapchainInfo info, const std::string& gameDeviceName)
        : profile(std::move(profile)), info(std::move(info)),
          gameDeviceName(gameDeviceName), vkPtr(&vk) {
    // only constructed for External presentation; caller guards this
    if (this->profile.presentation != ls::Presentation::External)
        throw ls::error("CaptureContext created for non-external presentation");

    // --- IPC handshake (2 s deadline on the NEGOTIATED reply) --------------
    std::filesystem::path sockPath;
    try {
        sockPath = ls::ipc::Listener::defaultPath();
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: external presentation active but XDG_RUNTIME_DIR is not set", e);
    }

    try {
        this->ipcConn.emplace(ls::ipc::Connection::connect(sockPath));
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: failed to connect to app socket '" + sockPath.string() + "'", e);
    }

    // send HELLO with true game caps
    const auto gameUuid = vk.deviceUUID();
    const auto hello = ls::ipc::makeHello(ls::ipc::PROTO_VERSION, gameUuid,
        gameDeviceName, static_cast<uint32_t>(this->info.format),
        this->info.extent.width, this->info.extent.height);
    try {
        this->ipcConn->send(hello);
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: failed to send HELLO", e);
    }

    // receive NEGOTIATED with 2 s deadline (poll-based, never hangs)
    ls::ipc::Negotiated negotiated{};
    try {
        auto msg = this->ipcConn->receive(std::chrono::milliseconds(2000));
        if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer refused: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer refused handshake: " + err->message);
        }
        if (!std::holds_alternative<ls::ipc::Negotiated>(msg)) {
            const auto got = ls::ipc::typeOf(msg);
            throw ls::error(std::string("lsfg-vk: external stream error: expected NEGOTIATED, got ")
                + ls::ipc::nameOf(got));
        }
        negotiated = std::get<ls::ipc::Negotiated>(std::move(msg));
    } catch (const ls::ipc::socket_error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: handshake deadline or socket failure", e);
    } catch (const ls::error& e) {
        // includes our explicit type-mismatch throws above
        if (std::string(e.what()).find("external stream error") != std::string::npos)
            throw;
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
    }

    // --- layer-side validation of the negotiated modifier on the GAME device ---
    constexpr VkFormatFeatureFlags2 usageNeeds =
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;

    try {
        const auto gameCaps = vk.exchangeCaps(this->info.format);
        auto it = gameCaps.find(this->info.format);
        if (it == gameCaps.end())
            throw ls::error("game device has no caps for format " + std::to_string(static_cast<int>(this->info.format)));

        const ls::ipc::Negotiated& neg = negotiated;
        const uint64_t mod = neg.modifier;
        bool found = false;
        VkFormatFeatureFlags2 foundBits = 0;
        for (const auto& entry : it->second) {
            if (entry.modifier == mod) {
                found = true;
                foundBits = entry.requiredUsageBits;
                break;
            }
        }
        if (!found)
            throw ls::error("negotiated modifier 0x" + std::to_string(mod) + " not advertised by game device");
        if ((foundBits & usageNeeds) != usageNeeds) {
            const auto missing = usageNeeds & ~foundBits;
            throw ls::error("negotiated modifier 0x" + std::to_string(mod)
                + " lacks required usage bits 0x" + std::to_string(missing));
        }
    } catch (const ls::error& e) {
        if (std::string(e.what()).find("negotiated modifier") != std::string::npos) {
            std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
            throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
        }
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw;
    }

    // --- create 2 exportable staging images on A at negotiated layout ------
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkExtent2D extent = this->info.extent;

    vk::ImageLayout layout{};
    if (negotiated.modifier == vk::EXCHANGE_MODIFIER_LINEAR) {
        layout.mode = vk::ImageMode::Linear;
        layout.rowPitch = negotiated.rowPitch;
        layout.drmModifier = 0;
    } else {
        layout.mode = vk::ImageMode::DrmModifier;
        layout.drmModifier = negotiated.modifier;
        layout.rowPitch = negotiated.rowPitch;
    }

    try {
        this->stagingImages.reserve(2);
        for (int i = 0; i < 2; ++i) {
            this->stagingImages.emplace_back(vk, extent, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                std::nullopt, std::nullopt, layout);
        }
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: failed to create staging images", e);
    }

    // export + hand off (one SCM_RIGHTS fd per STAGING message)
    try {
        for (auto& img : this->stagingImages) {
            const auto exp = img.exportDmaBuf(vk);
            // the allocationSize/rowPitch from export should match negotiated,
            // but we trust the negotiation; no hard check here
            (void)exp.allocationSize;
            this->ipcConn->attachFd(exp.fd);
            this->ipcConn->send(ls::ipc::Staging{});
            // attachFd ownership transferred to kernel; send closed our copy
        }
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: failed to export staging images", e);
    }

    // wait for READY (2 s deadline as well)
    try {
        auto msg = this->ipcConn->receive(std::chrono::milliseconds(2000));
        if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer refused: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer refused at READY: " + err->message);
        }
        if (!std::holds_alternative<ls::ipc::Ready>(msg)) {
            const auto got = ls::ipc::typeOf(msg);
            throw ls::error(std::string("lsfg-vk: external stream error: expected READY, got ")
                + ls::ipc::nameOf(got));
        }
    } catch (const ls::ipc::socket_error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: READY deadline or socket failure", e);
    } catch (const ls::error& e) {
        if (std::string(e.what()).find("external stream error") != std::string::npos)
            throw;
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
    }

    // --- capture command buffer + per-slot sync-fd semaphores ---------------
    try {
        this->captureCommandBuffer.emplace(vk);
        this->captureFence.emplace(vk);
        this->captureSemaphores.reserve(2);
        this->presentSemaphores.reserve(2);
        for (int i = 0; i < 2; ++i) {
            this->captureSemaphores.emplace_back(vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
            this->presentSemaphores.emplace_back(vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        }
        // FRAME sends should not block forever if app stops reading
        this->ipcConn->setSendTimeout(std::chrono::milliseconds(250));
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: failed to create capture semaphores", e);
    }

    std::cerr << "lsfg-vk: external presentation active (game on '" << gameDeviceName << "', app on socket)\n";
}

CaptureContext::CaptureContext(CaptureContext&&) noexcept = default;
CaptureContext& CaptureContext::operator=(CaptureContext&&) noexcept = default;

CaptureContext::~CaptureContext() {
    if (this->vkPtr && this->fenceSubmitted && this->captureFence.has_value()) {
        // bounded wait so destruction of pending-work semaphores does not trip VUID-05149
        // 150 ms matches swapchain.cpp:436-438 renderFence pattern
        try {
            (void)this->captureFence->wait(*this->vkPtr, 150ULL * 1000 * 1000);
        } catch (...) {
            // teardown must not throw
        }
    }
    // Connection and images/semaphores destroy via RAII; pending fds closed by Connection dtor
}

void CaptureContext::drainReleases() {
    if (!this->ipcConn.has_value()) return;
    // non-blocking drain: poll until no readable data
    while (true) {
        bool drained = false;
        try {
            drained = this->ipcConn->drained();
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
            throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
        }
        if (drained) break;

        ls::ipc::Message msg;
        try {
            // data is ready; receive without deadline (already poll-proven readable)
            msg = this->ipcConn->receive(std::nullopt);
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
            throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
        }

        if (auto* rel = std::get_if<ls::ipc::Release>(&msg)) {
            if (rel->stagingIdx < this->slotFree.size())
                this->slotFree.at(rel->stagingIdx) = true;
        } else if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer error: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer error: " + err->message);
        } else {
            // unexpected message in steady state - treat as stream error
            const auto got = ls::ipc::typeOf(msg);
            std::cerr << "lsfg-vk: external stream error: unexpected " << ls::ipc::nameOf(got) << " in steady state\n";
            throw ls::error(std::string("lsfg-vk: external stream error: unexpected ")
                + ls::ipc::nameOf(got));
        }
    }
}

size_t CaptureContext::selectFreeSlot() {
    // round-robin over free slots with 500 ms deadline polling
    constexpr auto deadline = std::chrono::milliseconds(500);
    const auto start = std::chrono::steady_clock::now();

    while (true) {
        for (size_t tries = 0; tries < this->slotFree.size(); ++tries) {
            const size_t idx = (this->nextSlot + tries) % this->slotFree.size();
            if (this->slotFree.at(idx)) {
                this->nextSlot = (idx + 1) % this->slotFree.size();
                return idx;
            }
        }

        // no free slot yet; check deadline
        if (std::chrono::steady_clock::now() - start >= deadline) {
            throw ls::error("lsfg-vk: external stream error: no free staging slots within 500 ms (app stalled)");
        }

        // drain any pending RELEASE messages before retrying
        try {
            this->drainReleases();
        } catch (const ls::error& e) {
            // already logged inside drainReleases; surface as stream error
            throw;
        }

        // brief sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

VkResult CaptureContext::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    // RELEASE handling frees slots draining non-blocking
    try {
        this->drainReleases();
    } catch (const ls::error& e) {
        // already logged inside drainReleases; surface as stream error
        throw;
    }

    size_t slot = 0;
    try {
        slot = this->selectFreeSlot();
    } catch (const ls::error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw;
    }

    if (imageIdx >= this->info.images.size())
        throw ls::error("swapchain image index out of range");

    const VkImage srcImage = this->info.images.at(imageIdx);
    const vk::Image& dstImage = this->stagingImages.at(slot);
    const vk::Semaphore& sigSem = this->captureSemaphores.at(slot);
    const vk::Semaphore& presentSem = this->presentSemaphores.at(slot);

    // bounded fence wait for previous capture work before reusing command buffer
    // (mirrors swapchain.cpp:436-438 150 ms pattern)
    if (this->fenceSubmitted) {
        if (!this->captureFence->wait(vk, 150ULL * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
        this->captureFence->reset(vk);
        this->fenceSubmitted = false;
    }

    // record blit info.images[imageIdx] -> staging[slot] waiting on game's
    // present wait-semaphores, signal slot's capture semaphore
    const auto& cmdbuf = *this->captureCommandBuffer;
    cmdbuf.begin(vk);
    cmdbuf.blitImage(vk,
        {
            barrierHelper(srcImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(dstImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { srcImage, dstImage.handle() },
        dstImage.getExtent(),
        {
            barrierHelper(srcImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );
    cmdbuf.end(vk);

    // submit the capture blit waiting on the game's present wait-semaphores
    // and signaling the slot's capture semaphore. use the game's present
    // queue (the queue param) for the submit so the wait/signal is on the
    // same queue that the subsequent QueuePresent will wait on; using
    // vk.queue() (first-graphics) can be a different queue handle and cause
    // the present wait to block forever on some drivers.
    try {
        std::vector<VkSemaphore> waitSems = semaphores;
        std::vector<VkSemaphore> signalSems = { sigSem.handle(), presentSem.handle() };
        std::vector<VkPipelineStageFlags> stages(waitSems.size(),
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        VkCommandBuffer rawBuf = cmdbuf.raw();
        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = static_cast<uint32_t>(waitSems.size()),
            .pWaitSemaphores = waitSems.data(),
            .pWaitDstStageMask = stages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &rawBuf,
            .signalSemaphoreCount = static_cast<uint32_t>(signalSems.size()),
            .pSignalSemaphores = signalSems.data()
        };
        auto res = vk.df().QueueSubmit(queue, 1, &submitInfo,
            this->captureFence->handle());
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkQueueSubmit() failed");
        this->fenceSubmitted = true;
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: capture submit failed: ") + e.what(), e);
    }

    // export sync-fd immediately after enqueue (copy transference)
    int syncFd = -1;
    try {
        syncFd = sigSem.exportFd(vk);
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: export sync fd failed: ") + e.what(), e);
    }

    // send FRAME (owns fd on success, closes on failure path via Connection)
    try {
        this->ipcConn->attachFd(syncFd);
        this->ipcConn->send(ls::ipc::Frame{ static_cast<uint32_t>(slot) });
        // ownership transferred to kernel; our copy closed by send()
        syncFd = -1;
    } catch (const std::exception& e) {
        if (syncFd >= 0) ::close(syncFd);
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        // send()/recv() EPIPE handling → context marked dead → present hook
        // throws named error; entrypoint's existing catch converts to VK_ERROR_*
        throw ls::error(std::string("lsfg-vk: external stream error: send FRAME failed: ") + e.what(), e);
    }

    // mark slot busy until RELEASE
    this->slotFree.at(slot) = false;
    this->fidx++;

    // forward original present down-chain WITH wait list REPLACED BY capture semaphore
    // game's semaphores were consumed by the blit submit, so re-waiting them would be
    // an invalid double-wait; waiting the capture semaphore instead is spec-sound
    // regardless of which queue the game presented on. simultaneous fd-export +
    // present-wait of one binary semaphore is legal via copy transference.
    VkSemaphore waitSem = presentSem.handle();
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = next_chain,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &waitSem,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };
    auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

    return res;
}
