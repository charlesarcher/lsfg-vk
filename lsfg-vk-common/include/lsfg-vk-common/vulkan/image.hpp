/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"
#include "vulkan.hpp"

#include <optional>

#include <vulkan/vulkan_core.h>

namespace vk {
    /// tiling and external memory mode of an image
    enum class ImageMode : std::uint8_t {
        /// optimal tiling with OPAQUE_FD external memory (legacy single-device path)
        Opaque,
        /// linear-convention dma-buf exchange. NOTE(todo-17): created via the
        /// DRM_FORMAT_MODIFIER path with drmFormatModifier=0, NOT plain
        /// VK_IMAGE_TILING_LINEAR - RADV rejects plain-LINEAR + DMA_BUF for
        /// the exchange usages, while modifier-0-via-modifier-path works on
        /// RADV and ANV alike and yields the identical linear layout
        Linear,
        /// explicit drm modifier tiling with DMA_BUF external memory
        DrmModifier
    };

    /// layout parameters for dma-buf exchange images
    struct ImageLayout {
        /// tiling and external memory mode
        ImageMode mode = ImageMode::Opaque;
        /// drm format modifier (required for ImageMode::DrmModifier; forced
        /// to 0 for ImageMode::Linear regardless of this field's value)
        uint64_t drmModifier{};
        /// row pitch of plane 0 in bytes. required for explicit modifiers and
        /// for imports; may be omitted when CREATING a modifier-0 image, in
        /// which case the natural pitch (width * texel block size) is used
        /// and exportDmaBuf() reports it to importers verbatim
        uint32_t rowPitch{};
    };

    /// dma-buf export descriptor of an image
    struct ImageExport {
        /// file descriptor owned by the caller, must be close()d
        int fd{};
        /// size of the underlying memory allocation in bytes
        VkDeviceSize allocationSize{};
        /// row pitch of plane 0 in bytes
        uint32_t rowPitch{};
    };

    /// vulkan image
    class Image {
    public:
        /// create an image
        /// @param vk the vulkan instance
        /// @param extent extent of the image in pixels
        /// @param format vulkan format of the image
        /// @param usage usage flags
        /// @param importFd optional file descriptor for shared memory
        /// @param exportFd optional pointer to an integer where the file descriptor will be stored
        /// @param layout optional tiling/modifier layout for dma-buf exchange images
        /// @throws ls::vulkan_error on failure
        Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
            VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::optional<int> importFd = std::nullopt,
            std::optional<int*> exportFd = std::nullopt,
            const ImageLayout& layout = {});

        /// get the image handle
        /// @return the image handle
        [[nodiscard]] const auto& handle() const { return this->image.get(); }
        /// get the image view handle
        /// @return the image view handle
        [[nodiscard]] const auto& imageview() const { return this->view.get(); }

        /// get the extent of the image
        /// @return the extent of the image
        [[nodiscard]] VkExtent2D getExtent() const { return this->extent; }

        /// export the image memory as a dma-buf file descriptor
        /// @param vk the vulkan instance
        /// @return descriptor with an owned fd (caller must close() it), allocation size and row pitch
        /// @throws ls::vulkan_error on failure or if not created with a dma-buf exchange mode
        [[nodiscard]] ImageExport exportDmaBuf(const vk::Vulkan& vk) const;
    private:
        ls::owned_ptr<VkImage> image;
        ls::owned_ptr<VkDeviceMemory> memory;
        ls::owned_ptr<VkImageView> view;

        VkExtent2D extent{};
        ImageMode mode{};
        VkDeviceSize allocationSize{};
        uint32_t rowPitch{};
    };
}
