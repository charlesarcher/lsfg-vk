/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"
#include "vulkan.hpp"

#include <cstddef>

#include <vulkan/vulkan_core.h>

namespace vk {
    /// vulkan buffer
    class Buffer {
    public:
        /// create a buffer
        /// @param vk the vulkan instance
        /// @param data initial data uploaded to the buffer
        /// @param usage usage flags for the buffer
        /// @throws ls::vulkan_error on failure
        template<typename T>
        Buffer(const vk::Vulkan& vk, const T& data,
                VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
            : Buffer(vk, reinterpret_cast<const void*>(&data), sizeof(T), usage) {}

        /// create a buffer
        /// @param vk the vulkan instance
        /// @param data initial data uploaded to the buffer
        /// @param size size of the buffer in bytes
        /// @param usage usage flags for the buffer
        /// @throws ls::vulkan_error on failure
        Buffer(const vk::Vulkan& vk, const void* data, size_t size,
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        /// import a dma-buf fd as a buffer
        ///
        /// This is the form the udmabuf transport needs. A dma-buf backed
        /// LINEAR IMAGE import is not usable on this driver: queried correctly
        /// (VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT with
        /// DRM_FORMAT_MOD_LINEAR) every format comes back with
        /// compatibleHandleTypes == 0. A buffer import of the same udmabuf is
        /// clean at 14.06 MiB, 200 iterations, both legs
        /// (tools/testing/udmabuf_transport_probe.cpp), and a buffer has no
        /// modifier or pitch for the two GPUs to disagree about.
        ///
        /// Note there is deliberately no VkMemoryDedicatedAllocateInfo here:
        /// dedicated imports are an image feature, and chaining one onto a
        /// buffer import segfaults radv inside vkBindBufferMemory.
        /// @param vk the vulkan instance
        /// @param fd an exported dma-buf fd (its offset is used as the
        ///        allocation offset)
        /// @param size size of the buffer in bytes
        /// @param usage usage flags for the buffer
        /// @throws ls::vulkan_error on failure
        Buffer(const vk::Vulkan& vk, int fd, size_t size,
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        /// get the buffer size in bytes
        ///
        /// The udmabuf transport asserts on this at context-open: a buffer
        /// that is not exactly the staging slot's size means the two ends
        /// disagree about the transfer, and the copy reads or writes past the
        /// end rather than failing.
        /// @return the buffer size in bytes
        [[nodiscard]] size_t byteSize() const { return this->size; }

        /// get the buffer handle
        /// @return the buffer handle
        [[nodiscard]] const auto& handle() const { return this->buffer.get(); }
        /// get the size of the buffer
        /// @return the size of the buffer in bytes
        [[nodiscard]] size_t length() const { return this->size; }
    private:
        ls::owned_ptr<VkBuffer> buffer;
        ls::owned_ptr<VkDeviceMemory> memory;
        size_t size;
    };
}
