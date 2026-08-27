/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/ipc/socket.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ls::ipc {
    /// one accepted game stream: owns the two staging fds the layer hands off
    /// during handshake (dma-buf handles, one per STAGING message). the sync
    /// fds carried per FRAME are consumed and closed immediately, so they are
    /// not stored here. StreamState is move-only; its destructor closes every
    /// stored fd, which is what makes the accept loop leak-free on teardown.
    class StreamState {
    public:
        /// the two staging fds owned by this stream, or fewer if teardown hit
        /// before the handshake completed
        std::array<int, 2> stagingFds{-1, -1};
        /// how many staging fds were actually stored (0..2)
        size_t stagingCount{0};

        ~StreamState();
    };

    /// run one accepted connection to completion: HELLO -> NEGOTIATED -> two
    /// STAGING -> READY -> per-frame FRAME/RELEASE until the peer closes or an
    /// error (or a shutdown flag) ends the stream. the caller removes the
    /// stream from its registry once this returns.
    ///
    /// @param conn the accepted connection (its fd is the stream key)
    /// @param state mutable registry entry the handshake fills (its staging
    ///     fds); the caller erases it on return so the destructor closes those
    ///     fds
    /// @param stop shared shutdown flag; set by the SIGINT handler when true
    ///     the stream handler returns immediately instead of blocking
    /// @throws ls::error / ls::ipc::socket_error on protocol or socket failure
    void runStream(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const std::atomic<bool>& stop);
}
