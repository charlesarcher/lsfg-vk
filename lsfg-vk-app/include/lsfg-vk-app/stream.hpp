/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"

#include <array>
#include <atomic>
#include <cstdint>
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
        /// the two B-local source images: created natively on this device at
        /// the negotiated layout, exported to the layer as the staging
        /// handoff (STAGING messages), and handed to the backend as the
        /// source descriptors (dups of the same exports). empty until the
        /// staging phase completes; the vk::Image members RAII their handles.
        std::array<ls::lazy<vk::Image>, 2> sourceImages{};

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
        /// motion-flow factor handed to openContext (1/flow_scale)
        float flow{1.0F};
        /// performance-mode flag handed to openContext
        bool perf{false};

        // move-only: user-declared move ops keep std::map<int,StreamState>
        // emplace/erase by value working (copy stays implicitly deleted).
        StreamState() = default;
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
