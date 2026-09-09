/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/timestamps.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    struct SwapchainInfo;
    struct CopyHop;

    /// capture context for external presentation (one-way dual-GPU).
    /// imports the two app-owned staging images (the app creates them in its
    /// own local VRAM and hands the dma-buf fds over the socket), owns a
    /// capture command buffer, per-slot sync-fd semaphores and the IPC stream
    /// to the companion app. blits the presented image into the current
    /// staging slot (a sequential A→B PCIe write), exports the completion
    /// sync-fd over the socket, then forwards the original present waiting on
    /// a separate semaphore.
    class CaptureContext {
    public:
        /// create a capture context: IPC handshake (2 s deadline), import of the
        /// app's two staging images at the negotiated layout, semaphore ring.
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
        /// non-blocking drain of RELEASE messages; @returns count applied
        int drainReleases();
        /// one round-robin probe. never waits on GPU B. nullopt = skip this capture
        [[nodiscard]] std::optional<size_t> trySelectFreeSlot();
        /// bitmap of currently free slots (bit i = slot i free); debug only
        [[nodiscard]] unsigned freeMask() const {
            unsigned m = 0;
            for (size_t i = 0; i < this->slotFree.size(); ++i)
                if (this->slotFree.at(i))
                    m |= 1u << i;
            return m;
        }

        ls::GameConf profile;
        SwapchainInfo info;
        std::string gameDeviceName;
        bool fake{false};               // fake swapchain: layer-owned images
        VkFence lastImageGateFence{VK_NULL_HANDLE}; // cb-reuse gate when fake

        // vulkan objects (created on the game device)
        std::vector<vk::Image> stagingImages; // imported B staging (two-way / IMPORT_STAGING)
        std::vector<vk::Image> localImages;   // 9070-owned capture dest; dma-buf exported on FRAME
        std::vector<vk::Image> hostImages;    // LINEAR images bound to host memory
        std::unique_ptr<vk::Vulkan> bVk;      // same-process 9060 device (LSFGVK_DUAL_HOST)
        std::vector<vk::Image> bHostImages;   // 9060 import of a separate malloc
        std::vector<vk::Image> bVramImages;   // 9060-local dma-buf export dest
        std::array<void*, ls::ipc::STAGING_RING_DEPTH> hostPtrsA{};
        std::array<void*, ls::ipc::STAGING_RING_DEPTH> hostPtrsB{};
        std::array<void*, ls::ipc::STAGING_RING_DEPTH> shmMaps{};
        std::array<uint32_t*, ls::ipc::STAGING_RING_DEPTH> shmSeq{};
        std::unique_ptr<CopyHop> copyHop;
        VkDeviceSize hostAllocSize{0};
        std::array<int, ls::ipc::STAGING_RING_DEPTH> bExportFds{};
        std::optional<vk::CommandBuffer> bEmptyCb;
        std::optional<vk::Fence> bEmptyFence;
        vk::ImageLayout exchangeLayout{};     // negotiated LINEAR/DRM layout for localImages
        std::array<int, ls::ipc::STAGING_RING_DEPTH> localExportFds{};
        std::array<bool, ls::ipc::STAGING_RING_DEPTH> dmaBufSent{};
        bool localCopyOnly{false};
        std::vector<vk::Semaphore> captureSemaphores; // recreated per cycle in present(), behind the fence gate
        std::vector<vk::Semaphore> leakCaptureSems;   // LSFGVK_LEAK_SEM=1: never DestroySemaphore
        std::vector<vk::Semaphore> presentSemaphores;
        // Session 13.17: capture cb RING - the render thread must never wait
        // on the previous blit. each ring slot owns a command buffer + fence;
        // a slot is reused only when its fence is ALREADY signaled (non-
        // blocking test, 0 timeout). ring is deep enough (STAGING_RING_DEPTH
        // + slack) that a busy GPU never makes the render thread wait.
        static constexpr size_t CAPTURE_RING_DEPTH = 6;
        std::vector<vk::CommandBuffer> captureCommandBuffers{};
        std::vector<vk::Fence> captureFences{};
        size_t captureRingIdx{0};
        // deprecated singles (kept for dtor compat, unused in fake path)
        ls::lazy<vk::CommandBuffer> captureCommandBuffer;
        ls::lazy<vk::Fence> captureFence;
        bool fenceSubmitted{false};
        // GPU timestamp instrumentation of the capture blit (LSFGVK_TIMING=1)
        vk::TimingRing timingRing;

        // IPC stream (STAGING_RING_DEPTH slots, maps to the backend's sources)
        std::optional<ls::ipc::Connection> ipcConn;

        // slot ring state
        std::array<bool, ls::ipc::STAGING_RING_DEPTH> slotFree{};
        size_t nextSlot{0};
        uint64_t fidx{0};
        uint64_t droppedCaptures{0};

        // for teardown fence wait (need vk + device functions)
        const vk::Vulkan* vkPtr{nullptr};
        VkCommandPool capturePool{VK_NULL_HANDLE};
        VkQueue captureQ{VK_NULL_HANDLE};
    };

}
