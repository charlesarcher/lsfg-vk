/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// create a buffer
    ls::owned_ptr<VkBuffer> createBuffer(const vk::Vulkan& vk, size_t size,
            VkBufferUsageFlags usage) {
        VkBuffer handle{};

        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateBuffer(vk.dev(), &bufferInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateBuffer() failed");

        return ls::owned_ptr<VkBuffer>(
            new VkBuffer(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyBuffer](VkBuffer& buffer) {
                defunc(dev, buffer, VK_NULL_HANDLE);
            }
        );
    }
    /// allocate memory for a buffer
    ls::owned_ptr<VkDeviceMemory> allocateMemory(const vk::Vulkan& vk, VkBuffer buffer) {
        VkDeviceMemory handle{};

        VkMemoryRequirements reqs{};
        vk.df().GetBufferMemoryRequirements(vk.dev(), buffer, &reqs);

        auto mti = vk.findMemoryTypeIndex(
            reqs.memoryTypeBits,
            true
        );
        if (!mti.has_value())
            throw ls::vulkan_error("no suitable memory type found for buffer");

        const VkMemoryAllocateInfo memoryInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = *mti
        };
        auto res = vk.df().AllocateMemory(vk.dev(), &memoryInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkAllocateMemory() failed");

        res = vk.df().BindBufferMemory(vk.dev(), buffer, handle, 0);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkBindBufferMemory() failed");

        return ls::owned_ptr<VkDeviceMemory>(
            new VkDeviceMemory(handle),
            [dev = vk.dev(), defunc = vk.df().FreeMemory](VkDeviceMemory& memory) {
                defunc(dev, memory, VK_NULL_HANDLE);
            }
        );
    }
    /// copy data to a buffer
    void copyDataToBuffer(const vk::Vulkan& vk,
            VkDeviceMemory memory, const void* data, size_t size) {
        void* buf{};

        auto res = vk.df().MapMemory(vk.dev(), memory, 0, size, 0, &buf);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkMapMemory() failed");

        std::copy_n(
            reinterpret_cast<const uint8_t*>(data),
            size,
            reinterpret_cast<uint8_t*>(buf)
        );

        vk.df().UnmapMemory(vk.dev(), memory);
    }
}

Buffer::Buffer(const vk::Vulkan& vk, const void* data, size_t size, VkBufferUsageFlags usage) :
        buffer(createBuffer(vk, size, usage)),
        memory(allocateMemory(vk, *this->buffer)),
        size(size) {
    copyDataToBuffer(vk, *this->memory, data, size);
}

namespace {
    /// create a buffer that will be bound to an imported dma-buf
    ///
    /// VkExternalMemoryBufferCreateInfo is required: without it the bind is
    /// rejected (VUID-vkBindBufferMemory-memory-02985) because the buffer
    /// never declared the handle type it is being bound to.
    ls::owned_ptr<VkBuffer> createImportedBuffer(const vk::Vulkan& vk, size_t size,
            VkBufferUsageFlags usage) {
        VkBuffer handle{};
        const VkExternalMemoryBufferCreateInfo ext{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        };
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = &ext,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateBuffer(vk.dev(), &bufferInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateBuffer() (dma-buf import) failed");
        return ls::owned_ptr<VkBuffer>(
            new VkBuffer(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyBuffer](VkBuffer& buffer) {
                defunc(dev, buffer, VK_NULL_HANDLE);
            }
        );
    }
    /// import a dma-buf fd as buffer memory
    ///
    /// NO VkMemoryDedicatedAllocateInfo: dedicated imports are an image
    /// feature, and chaining one onto a buffer import segfaults radv inside
    /// vkBindBufferMemory.
    ls::owned_ptr<VkDeviceMemory> importDmaBuf(const vk::Vulkan& vk, VkBuffer buffer,
            int fd) {
        VkDeviceMemory handle{};

        VkMemoryRequirements reqs{};
        vk.df().GetBufferMemoryRequirements(vk.dev(), buffer, &reqs);

        // The loader does not export vkGetMemoryFdPropertiesKHR as a symbol,
        // so it comes from the device's own proc-addr table (the same way
        // lsfg-vk-layer does it).
        VkMemoryFdPropertiesKHR fdProps{
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
        };
        auto res = vk.df().GetMemoryFdPropertiesKHR(vk.dev(),
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkGetMemoryFdPropertiesKHR() failed");

        auto mti = vk.findMemoryTypeIndex(
            static_cast<uint32_t>(reqs.memoryTypeBits & fdProps.memoryTypeBits),
            true
        );
        if (!mti.has_value())
            throw ls::vulkan_error("no host-visible memory type for the dma-buf import");

        const VkImportMemoryFdInfoKHR import{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = fd
        };
        const VkMemoryAllocateInfo memoryInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import,
            .allocationSize = reqs.size,
            .memoryTypeIndex = *mti
        };
        res = vk.df().AllocateMemory(vk.dev(), &memoryInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkAllocateMemory() (dma-buf import) failed");

        res = vk.df().BindBufferMemory(vk.dev(), buffer, handle, 0);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkBindBufferMemory() (dma-buf import) failed");

        return ls::owned_ptr<VkDeviceMemory>(
            new VkDeviceMemory(handle),
            [dev = vk.dev(), defunc = vk.df().FreeMemory](VkDeviceMemory& memory) {
                defunc(dev, memory, VK_NULL_HANDLE);
            }
        );
    }
}

Buffer::Buffer(const vk::Vulkan& vk, int fd, size_t size, VkBufferUsageFlags usage) :
        buffer(createImportedBuffer(vk, size, usage)),
        memory(importDmaBuf(vk, *this->buffer, fd)),
        size(size) {
}
