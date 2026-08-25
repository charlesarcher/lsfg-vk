/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"
#include "vulkan.hpp"

#include <optional>

#include <vulkan/vulkan_core.h>

namespace vk {
    /// vulkan semaphore
    class Semaphore {
    public:
        /// create a semaphore
        /// @param vk the vulkan instance
        /// @param fd optional file descriptor to import the semaphore from.
        ///           an fd of -1 is treated as an already-signaled semaphore
        ///           per spec (the import is skipped and the semaphore is
        ///           signaled via an empty queue submission instead)
        /// @param handleType external handle type used for import/export.
        ///                   sync_fd imports are always temporary (copy
        ///                   transference); opaque_fd imports are permanent
        /// @throws ls::vulkan_error on failure
        Semaphore(const vk::Vulkan& vk, std::optional<int> fd = std::nullopt,
            VkExternalSemaphoreHandleTypeFlagBits handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);

        /// export the semaphore payload to a file descriptor.
        /// requires the semaphore to have been created with a matching
        /// external handle type. for sync_fd this snapshots the current
        /// payload (copy transference), so signal before exporting.
        /// @param vk the vulkan instance
        /// @return the file descriptor, owned by the caller; importing it
        ///         consumes (closes) it, otherwise close(2) it manually
        /// @throws ls::vulkan_error on failure
        [[nodiscard]] int exportFd(const vk::Vulkan& vk) const;

        /// get the underlying VkSemaphore handle
        /// @return the VkSemaphore handle
        [[nodiscard]] const auto& handle() const { return *this->semaphore; }
    private:
        ls::owned_ptr<VkSemaphore> semaphore;
        VkExternalSemaphoreHandleTypeFlagBits handleType;
    };
}
