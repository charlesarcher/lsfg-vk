/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "lsfg-vk-common/vulkan/vulkan.hpp"

namespace ls::wsi {

    /// geometry + identity of one physical output of the display server
    struct OutputGeometry {
        std::string name;         // RandR connector name (matches the 'output' config key)
        uint32_t x{0};
        uint32_t y{0};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t refresh{0};      // mHz, 0 = unknown
    };

    /// opaque native window handle. For the X11 backend this is the XID (xcb_window_t),
    /// stored as a void* to keep the interface backend-agnostic (task 8 consumes it).
    using WindowHandle = void*;

    /// backend-agnostic presentation surface interface. Everything task 8 needs,
    /// nothing more: connect, enumerate outputs, create a borderless no-focus window,
    /// create the VkSurfaceKHR for it, query its caps for swapchain clamping, pump
    /// resize/close events, and tear everything down. One instance per backend type.
    class SurfaceBackend {
    public:
        /// @param session "x11" | "wayland" | "auto"
        [[nodiscard]] virtual bool connect(std::string_view session) = 0;
        [[nodiscard]] virtual std::vector<OutputGeometry> outputs() const = 0;
        /// @param output_name connector name to match; empty string => primary/active output
        /// @throws ls::error on unknown output name (mismatch error path)
        [[nodiscard]] virtual WindowHandle
            createWindow(std::string_view output_name, VkExtent2D extent, uint32_t colorspace) = 0;
        [[nodiscard]] virtual VkSurfaceKHR
            createSurface(const vk::Vulkan& vk, WindowHandle handle) = 0;
        /// fill @p caps (extent range) + @p colorspaces from the surface for swapchain clamp
        virtual void surfaceCaps(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR& caps,
                std::vector<VkColorSpaceKHR>& colorspaces) = 0;
        /// pump display/WM events for up to @p timeout_ms; return true if window resized or closed
        virtual bool processEvents(int timeout_ms) = 0;
        /// tear down window + surface + connection (idempotent)
        virtual void destroy() = 0;
    };

    /// factory: build the best-available backend (X11/xcb is the only implementation today)
    std::unique_ptr<SurfaceBackend> createX11SurfaceBackend();

    /// factory: build the Wayland backend (xdg-shell + xdg-output)
    std::unique_ptr<SurfaceBackend> createWaylandSurfaceBackend();
}
