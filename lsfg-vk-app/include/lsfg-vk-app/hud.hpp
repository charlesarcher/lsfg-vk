/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace ls::hud {
    /// small "<game>/<presented>" fps indicator (e.g. "57/114") drawn in the
    /// top-left corner of the app's fullscreen window so the user can tell at a
    /// glance that one-way frame doubling is active.
    ///
    /// the text is rasterized on the CPU (seven-segment digits) into a small
    /// double-buffered image and uploaded via buffer-to-image copy. each update
    /// writes the slot the present loop is NOT reading, and the upload + present
    /// submits are queue-ordered, so the present loop never sees a half-updated
    /// image.
    class Hud {
    public:
        /// top-left inset (px) where the box is blitted onto the display
        static constexpr VkOffset2D ORIGIN = { 8, 8 };

        /// create the HUD. @p outputHeight selects the display scale so the
        /// text stays legible from 1080p to 4K. @p format must be the
        /// swapchain format the box is blitted into (B8G8R8A8 or A8B8G8R8).
        /// @throws ls::error for an unsupported format
        Hud(const vk::Vulkan& vk, uint32_t outputHeight, VkFormat format);

        /// re-rasterize @p text (e.g. "57/114") and upload it to the inactive
        /// slot. called at a 1 Hz cadence from the present loop thread.
        /// @throws ls::vulkan_error if the previous upload fence does not signal
        void update(std::string_view text);

        /// the image the present loop blits into the top-left
        [[nodiscard]] const vk::Image& image() const { return *this->slotImage[this->active]; }

        /// the last GPU access on the active image; the present loop's blit src
        /// barrier must declare it in srcAccessMask
        [[nodiscard]] VkAccessFlags lastAccess() const { return this->slotLastAccess[this->active]; }

        /// the pixel extent of the box rasterized into the image
        [[nodiscard]] VkExtent2D box() const { return this->boxExtent; }

        /// the present loop calls this after blitting the active image so the
        /// next upload barrier's srcAccessMask is exact
        void markRead() { this->slotLastAccess[this->active] = VK_ACCESS_TRANSFER_READ_BIT; }

    private:
        void rasterize(std::string_view text, std::vector<uint8_t>& out) const;

        const vk::Vulkan& vk;
        std::array<ls::lazy<vk::Image>, 2> slotImage;
        std::array<ls::lazy<vk::Fence>, 2> slotFence;
        std::array<VkAccessFlags, 2> slotLastAccess{ VK_ACCESS_NONE, VK_ACCESS_NONE };
        std::array<bool, 2> slotGeneral{ false, false };
        std::array<bool, 2> slotFenceSignaled{ false, false };
        uint8_t active{ 0 };
        VkFormat format;
        VkExtent2D boxExtent;
        uint32_t scale;
        vk::CommandBuffer cmdbuf;
    };
}