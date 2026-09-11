/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ls::ipc {
    /// one accepted game stream. the sync fds carried per FRAME are consumed
    /// and closed immediately, so they are not stored here. StreamState is
    /// move-only; its members RAII their own handles, which is what makes the
    /// accept loop leak-free on teardown.
    ///
    /// From task 6 onward a completed stream also owns its transport: the two
    /// B-local staging/source images created natively (and exported to the
    /// layer as the STAGING handoff), the (multiplier-1) B-local destination
    /// images created natively (and self-exported to the backend), the backend
    /// frame-generation context, and the handshake parameters kept for the
    /// later presentation tasks. All of these are move-only, so the
    /// std::map registry can still emplace/erase StreamState by value.
    class StreamState {
    public:
        /// the STAGING_RING_DEPTH B-local source images: created natively on this
        /// device at the negotiated layout, exported to the layer as the staging
        /// handoff (STAGING messages), and handed to the backend as the
        /// source descriptors (dups of the same exports). empty until the
        /// staging phase completes; the vk::Image members RAII their handles.
        std::array<ls::lazy<vk::Image>, STAGING_RING_DEPTH> sourceImages{};

        /// B-local copies the backend samples. Snapshot copies the imported
        /// 9070-owned capture (aImports) → genSources, then Release, then gen.
        std::array<ls::lazy<vk::Image>, STAGING_RING_DEPTH> genSources{};
        /// 9070-owned capture images imported on B for the snapshot only.
        std::array<ls::lazy<vk::Image>, STAGING_RING_DEPTH> aImports{};
        /// Offload DMA-in device (no gfx). 9070 share is imported here only.
        std::unique_ptr<vk::Vulkan> dmaVk;
        std::array<std::optional<vk::CommandBuffer>, STAGING_RING_DEPTH> dmaCbs{};
        std::array<std::optional<vk::Fence>, STAGING_RING_DEPTH> dmaFences{};
        std::array<std::optional<vk::Image>, STAGING_RING_DEPTH> dmaSrc{};
        std::array<std::optional<vk::Image>, STAGING_RING_DEPTH> dmaDst{};
        std::array<int, STAGING_RING_DEPTH> dmaDstFds{};
        uint32_t rowPitch{0};
        /// POSIX memfd maps (not Vulkan). Layer memcpy capture here; we memcpy
        /// into hostPtrs (9060 malloc import) then GPU-copy to genSources.
        std::array<void*, STAGING_RING_DEPTH> shmMaps{};
        std::array<void*, STAGING_RING_DEPTH> hostPtrs{};
        std::array<ls::lazy<vk::Image>, STAGING_RING_DEPTH> hostImages{};
        std::array<void*, STAGING_RING_DEPTH> dmaMaps{};
        std::array<int, STAGING_RING_DEPTH> dmaFds{};
        std::array<uint32_t*, STAGING_RING_DEPTH> shmSeq{};
        std::array<uint32_t, STAGING_RING_DEPTH> shmSeen{};
        size_t shmBytes{0};

        /// the (multiplier-1) B-local destination images, created natively on this
        /// device and self-exported to become the backend's destination
        /// descriptors. the backend infers the generation multiplier from the
        /// destination count (multiplier = dests + 1, lsfgvk.hpp:150).
        std::vector<ls::lazy<vk::Image>> destinationImages;

        /// the backend frame-generation context; closed through the backend's
        /// closeContext on erase (mirrors the layer's swapchain.cpp ctx janitor).
        /// default-null until openContext succeeds.
        ls::owned_ptr<ls::R<lsfgvk::backend::Context>> context;

        /// owning process-level backend instance (owned by main for the whole
        /// run); points here so closeContext can be reached from the dtor.
        lsfgvk::backend::Instance* backend{nullptr};

        // --- handshake parameters kept for the presentation tasks (7/8) ------
        /// game device UUID carried over from HELLO (exporterDeviceUUID)
        std::array<uint8_t, 16> gameUuid{};
        /// negotiated drm modifier of the exchange layout
        uint64_t negotiatedModifier{0};
        /// swapchain width/height in pixels
        uint32_t width{0};
        uint32_t height{0};
        /// pixel format of the B-local source (staging) images, kept so the
        /// presentation task can create a same-format snapshot image for the
        /// early-release path (the real present reads the snapshot, not the
        /// live source, so the slot can be released before the display present)
        VkFormat sourceFormat{VK_FORMAT_R8G8B8A8_UNORM};
        /// game swapchain format (HELLO). 9070 dma-buf import must match this.
        VkFormat captureFormat{VK_FORMAT_R8G8B8A8_UNORM};
        /// motion-flow factor handed to openContext (1/flow_scale)
        float flow{1.0F};
        /// performance-mode flag handed to openContext
        bool perf{false};

        // move-only: user-declared move ops keep std::map<int,StreamState>
        // emplace/erase by value working (copy stays implicitly deleted).
        StreamState() { dmaFds.fill(-1); dmaDstFds.fill(-1); }
        ~StreamState();
        StreamState(StreamState&&) = default;
        StreamState& operator=(StreamState&&) = default;
    };

/// run one accepted connection to completion: HELLO -> NEGOTIATED -> two
///     STAGING (app → layer staging handoff) -> READY -> per-frame FRAME/
///     RELEASE until the peer closes or an error (or a shutdown flag) ends
///     the stream. the caller removes the stream from its registry once this
///     returns.
///
/// @param conn the accepted connection (its fd is the stream key)
/// @param state mutable registry entry the handshake fills (its staging
///     source images); the caller erases it on return
/// @param stop shared shutdown flag; set by the SIGINT handler when true
///     the stream handler returns immediately instead of blocking
/// @param session WSI backend: "x11" | "wayland" | "auto"
/// @throws ls::error / ls::ipc::socket_error on protocol or socket failure
    void runStream(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const std::atomic<bool>& stop, const vk::Vulkan& vk,
        lsfgvk::backend::Instance& backend, const ls::GameConf& conf,
        std::string_view session);
}
