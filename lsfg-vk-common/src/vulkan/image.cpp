/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <bitset>
#include <cstdint>
#include <optional>

#include <unistd.h>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// create a image
    ls::owned_ptr<VkImage> createImage(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
            bool external) {
        VkImage handle{};

        const VkExternalMemoryImageCreateInfo externalInfo{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
        };
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = external ? &externalInfo : nullptr,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {
                .width = extent.width,
                .height = extent.height,
                .depth = 1
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateImage(vk.dev(), &imageInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImage() failed");

        return ls::owned_ptr<VkImage>(
            new VkImage(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImage](VkImage& image) {
                defunc(dev, image, VK_NULL_HANDLE);
            }
        );
    }
    /// bytes per texel block of the single-plane uncompressed exchange formats
    uint32_t exchangeTexelBlockSize(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
            default:
                throw ls::vulkan_error("unsupported dma-buf exchange format");
        }
    }
    /// create a dma-buf exchange image via the drm-format-modifier creation path.
    /// BOTH explicit modifiers and the linear convention travel this path:
    /// plain VK_IMAGE_TILING_LINEAR + DMA_BUF is rejected by RADV for the
    /// exchange usages (STORAGE-on-LINEAR exists only via the modifier path
    /// there), while modifier 0 through the modifier path creates the identical
    /// linear layout and is accepted by RADV and ANV alike (probe: 18/18 pairs)
    ls::owned_ptr<VkImage> createExchangeImage(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
            bool external, bool imported, const ImageLayout& layout) {
        // modifier 0 IS the linear layout; force it so a stray drmModifier
        // value can never turn ImageMode::Linear into an explicit layout
        const uint64_t drmModifier = layout.mode == ImageMode::Linear ?
            0 : layout.drmModifier;

        // exporters may omit the row pitch (unknowable before the image
        // exists): synthesize the natural pitch, which exportDmaBuf then
        // carries to importers verbatim. imports MUST know their pitch -
        // guessing would misinterpret the foreign memory's scanline stride.
        uint32_t rowPitch = layout.rowPitch;
        if (rowPitch == 0) {
            if (drmModifier != 0)
                throw ls::vulkan_error(
                    "drm modifier image creation requires an explicit row pitch");
            if (imported)
                throw ls::vulkan_error(
                    "imported dma-buf exchange image requires an explicit row pitch");
            // pad to a 256-byte alignment: drivers disagree on the minimum
            // linear pitch alignment (ANV accepts small alignments and reports
            // them back verbatim, RADV rejects modifier-0 plane layouts whose
            // pitch is not 256-aligned). requesting the padded pitch makes the
            // exported layout consumable by every current driver; the extra
            // padding bytes are never read.
            rowPitch = (extent.width * exchangeTexelBlockSize(format) + 255) / 256 * 256;
        }

        VkImage handle{};

        const VkExternalMemoryImageCreateInfo externalInfo{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        };
        const VkSubresourceLayout planeLayout{
            .offset = 0,
            .rowPitch = rowPitch
        };
        const VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .pNext = external ? &externalInfo : nullptr,
            .drmFormatModifier = drmModifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &planeLayout
        };
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &modifierInfo,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {
                .width = extent.width,
                .height = extent.height,
                .depth = 1
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateImage(vk.dev(), &imageInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImage() failed");

        return ls::owned_ptr<VkImage>(
            new VkImage(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImage](VkImage& image) {
                defunc(dev, image, VK_NULL_HANDLE);
            }
        );
    }
    /// find the memory type index for imported foreign dma-buf memory,
    /// preferring device-local types within the given valid bits
    uint32_t findImportMemoryTypeIndex(const vk::Vulkan& vk, uint32_t validTypes) {
        VkPhysicalDeviceMemoryProperties props{};
        vk.fi().GetPhysicalDeviceMemoryProperties(vk.physdev(), &props);

        std::optional<uint32_t> fallback{};
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if (!(validTypes & (1U << i)))
                continue;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
            if (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                return i;
            if (!fallback.has_value())
                fallback = i;
        }

        if (!fallback.has_value())
            throw ls::vulkan_error("no suitable memory type found for imported dma-buf");
        return *fallback;
    }
    /// allocate memory for a image
    ls::owned_ptr<VkDeviceMemory> allocateMemory(const vk::Vulkan& vk, VkImage image,
            std::optional<int> importFd, std::optional<int*> exportFd,
            const ImageLayout& layout) {
        VkDeviceMemory handle{};

        VkMemoryRequirements reqs{};
        vk.df().GetImageMemoryRequirements(vk.dev(), image, &reqs);

        const bool dmaBuf = layout.mode != ImageMode::Opaque;
        const auto handleType = dmaBuf ?
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT :
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

        std::optional<uint32_t> mti{};
        if (dmaBuf && importFd.has_value()) {
            // imported foreign memory reports its own valid memory type bits,
            // which must be intersected with the image's requirements
            VkMemoryFdPropertiesKHR fdProps{
                .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
            };
            auto res = vk.df().GetMemoryFdPropertiesKHR(vk.dev(),
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                importFd.value(), &fdProps);
            if (res != VK_SUCCESS) {
                close(importFd.value()); // ownership was not transferred on failure
                throw ls::vulkan_error(res, "vkGetMemoryFdPropertiesKHR() failed");
            }

            const auto candidates = reqs.memoryTypeBits & fdProps.memoryTypeBits;
            if (!candidates) {
                close(importFd.value());
                throw ls::vulkan_error(
                    "no common memory type between imported dma-buf and image requirements");
            }
            mti = findImportMemoryTypeIndex(vk, candidates);
        } else {
            mti = vk.findMemoryTypeIndex(
                reqs.memoryTypeBits,
                false
            );
        }
        if (!mti.has_value())
            throw ls::vulkan_error("no suitable memory type found for image");

        const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
            .image = image,
        };
        const VkImportMemoryFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = &dedicatedInfo,
            .handleType = handleType,
            .fd = importFd.value_or(-1)
        };
        const VkExportMemoryAllocateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicatedInfo,
            .handleTypes = handleType
        };
        const void* pNextAlloc{};
        if (importFd.has_value())
            pNextAlloc = &importInfo;
        else if (exportFd.has_value() || dmaBuf)
            pNextAlloc = &exportInfo;
        const VkMemoryAllocateInfo memoryInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = pNextAlloc,
            .allocationSize = reqs.size,
            .memoryTypeIndex = *mti
        };
        auto res = vk.df().AllocateMemory(vk.dev(), &memoryInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS) {
            if (dmaBuf && importFd.has_value())
                close(importFd.value()); // ownership was not transferred on failure
            throw ls::vulkan_error(res, "vkAllocateMemory() failed");
        }

        res = vk.df().BindImageMemory(vk.dev(), image, handle, 0);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkBindImageMemory() failed");

        if (exportFd.has_value()) {
            const VkMemoryGetFdInfoKHR fdInfo{
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                .memory = handle,
                .handleType = handleType
            };
            int fd{};
            res = vk.df().GetMemoryFdKHR(vk.dev(), &fdInfo, &fd);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkGetMemoryFdKHR() failed");
            **exportFd = fd;
        }

        return ls::owned_ptr<VkDeviceMemory>(
            new VkDeviceMemory(handle),
            [dev = vk.dev(), defunc = vk.df().FreeMemory](VkDeviceMemory& memory) {
                defunc(dev, memory, VK_NULL_HANDLE);
            }
        );
    }
    /// query the size of the memory requirements of an image
    VkDeviceSize queryAllocationSize(const vk::Vulkan& vk, VkImage image) {
        VkMemoryRequirements reqs{};
        vk.df().GetImageMemoryRequirements(vk.dev(), image, &reqs);
        return reqs.size;
    }
    /// query the row pitch of plane 0 of an image.
    /// drm-modifier images require the MEMORY_PLANE_0 aspect here: ANV
    /// reports zeros for COLOR_BIT on modifier images, RADV accepts both
    uint32_t queryRowPitch(const vk::Vulkan& vk, VkImage image, bool modifierTiled) {
        const VkImageSubresource subresource{
            .aspectMask = modifierTiled ?
                VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .arrayLayer = 0
        };
        VkSubresourceLayout layout{};
        vk.df().GetImageSubresourceLayout(vk.dev(), image, &subresource, &layout);
        return static_cast<uint32_t>(layout.rowPitch);
    }
    /// create an image view
    ls::owned_ptr<VkImageView> createImageView(const vk::Vulkan& vk,
            VkImage image, VkFormat format) {
        VkImageView handle{};

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        auto res = vk.df().CreateImageView(vk.dev(), &viewInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImageView() failed");

        return ls::owned_ptr<VkImageView>(
            new VkImageView(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImageView](VkImageView& view) {
                defunc(dev, view, VK_NULL_HANDLE);
            }
        );
    }
}

Image::Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            std::optional<int> importFd,
            std::optional<int*> exportFd,
            const ImageLayout& layout) :
        image(layout.mode == ImageMode::Opaque ?
            createImage(vk,
                extent, format, usage,
                importFd.has_value() || exportFd.has_value()
            ) : createExchangeImage(vk,
                extent, format, usage,
                importFd.has_value() || exportFd.has_value(),
                importFd.has_value(),
                layout
            )),
        memory(allocateMemory(vk,
            *this->image,
            importFd, exportFd, layout
        )),
        view(createImageView(vk,
            *this->image,
            format
        )),
        extent(extent),
        mode(layout.mode),
        allocationSize(queryAllocationSize(vk, *this->image)),
        rowPitch(layout.mode == ImageMode::Opaque ?
            0 : queryRowPitch(vk, *this->image, true)) {
}

ImageExport Image::exportDmaBuf(const vk::Vulkan& vk) const {
    if (this->mode == ImageMode::Opaque)
        throw ls::vulkan_error(
            "cannot export image as dma-buf: not created for dma-buf exchange");

    const VkMemoryGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = *this->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    int fd{};
    auto res = vk.df().GetMemoryFdKHR(vk.dev(), &fdInfo, &fd);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetMemoryFdKHR() failed");

    return {
        .fd = fd,
        .allocationSize = this->allocationSize,
        .rowPitch = this->rowPitch
    };
}
