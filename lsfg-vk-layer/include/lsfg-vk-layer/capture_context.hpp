/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    struct SwapchainInfo;

    /// capture context for external presentation (one-way dual-GPU).
    /// owns two exportable staging images on the game device, a capture
    /// command buffer, per-slot sync-fd semaphores and the IPC stream to
    /// the companion app. blits the presented image into the current
    /// staging slot, exports the completion sync-fd over the socket, then
    /// forwards the original present waiting on that same semaphore.
    class CaptureContext {
    public:
        /// create a capture context: IPC handshake (2 s deadline), staging
        /// image creation at negotiated layout, export + handoff, semaphore ring.
        /// @param vk vulkan wrapper for the game device (layer's wrapper)
        /// @param profile active game profile (must have presentation == External)
        /// @param info swapchain image metadata (extent/format/images)
        /// @param gameDeviceName device name for log lines
        CaptureContext(const vk::Vulkan& vk, ls::GameConf profile,
            SwapchainInfo info, const std::string& gameDeviceName);

        CaptureContext(const CaptureContext&) = delete;
        CaptureContext& operator=(const CaptureContext&) = delete;
        CaptureContext(CaptureContext&&) noexcept;
        CaptureContext& operator=(CaptureContext&&) noexcept;

        /// teardown: bounded fence wait (150 ms) draining in-flight capture
        /// work before destroying semaphores/images
        ~CaptureContext();

        /// present hook for external mode: blit → export sync-fd → FRAME → forward present.
        /// @param vk vulkan wrapper for the game device
        /// @param queue presentation queue from vkQueuePresentKHR
        /// @param swapchain swapchain handle being presented
        /// @param next_chain pNext chain from VkPresentInfoKHR
        /// @param imageIdx index of the swapchain image being presented
        /// @param semaphores wait semaphores from the game's present info
        VkResult present(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain,
            void* next_chain, uint32_t imageIdx,
            const std::vector<VkSemaphore>& semaphores);
    private:
        void drainReleases();
        [[nodiscard]] size_t selectFreeSlot();

        ls::GameConf profile;
        SwapchainInfo info;
        std::string gameDeviceName;

        // vulkan objects (created on the game device)
        std::vector<vk::Image> stagingImages;
        std::vector<vk::Semaphore> captureSemaphores;
        std::vector<vk::Semaphore> presentSemaphores;
        ls::lazy<vk::CommandBuffer> captureCommandBuffer;
        ls::lazy<vk::Fence> captureFence;
        bool fenceSubmitted{false};

        // IPC stream (exactly 2 slots, maps to backend's two sources)
        std::optional<ls::ipc::Connection> ipcConn;

        // slot ring state
        std::array<bool, 2> slotFree{true, true};
        size_t nextSlot{0};
        uint64_t fidx{0};

        // for teardown fence wait (need vk + device functions)
        const vk::Vulkan* vkPtr{nullptr};
    };

}
