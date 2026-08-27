/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-app/stream.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <atomic>

/// task 8: external presentation output. the app owns the swapchain on the
/// processing GPU's display output and presents both the backend-generated
/// frames and the real captured game frame there.
namespace ls::presentation {
    /// run the frame-generation PRESENT pipeline for one accepted stream.
    ///
    /// Creates ONE surface backend + a window, surface and FIFO swapchain on the
    /// transport (graphical) vk, then per accepted FRAME: schedules the backend
    /// for the captured frame, imports each destination image's done sync fd,
    /// acquires a swapchain image, blits the corresponding destination image in
    /// and presents it, and finally blits the latest captured game frame into a
    /// swapchain image and presents it. The stream socket is polled concurrently
    /// so a peer close ends the stream cleanly.
    ///
    /// Every WSI handle (window, surface, swapchain) is a LOCAL that is torn
    /// down on every return path (normal, stop, or throw); the vk::Vulkan, the
    /// backend instance and the stream's frame-generation context are owned by
    /// the caller and are NOT destroyed here.
    ///
    /// @param conn the accepted connection (drains RELEASE, reads FRAME)
    /// @param state the completed handshake state (source/destination images,
    ///     backend context, width/height)
    /// @param vk the processing device, built graphical so its df() carries the
    ///     swapchain function pointers
    /// @param backend the process-level frame-gen instance
    /// @param conf the selected profile (its optional output selects the display
    ///     connector to present on)
    /// @param stop shared shutdown flag; when true the loop stops and tears down
    /// @throws ls::error / ls::vulkan_error on surface/swapchain/present failure
    void runPresent(ls::ipc::Connection& conn, ls::ipc::StreamState& state,
        const vk::Vulkan& vk, lsfgvk::backend::Instance& backend,
        const ls::GameConf& conf, const std::atomic<bool>& stop);
}
