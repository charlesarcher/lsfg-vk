/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <optional>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// create a semaphore.
    /// sync_fd uses copy transference: exports snapshot the current payload
    /// and imports must be temporary. an incoming fd of -1 is specified to
    /// behave like an already-signaled sync fd; we skip the driver roundtrip
    /// and queue-signal instead so the semantic holds without relying on
    /// driver support for literal -1 imports.
    ls::owned_ptr<VkSemaphore> createSemaphore(const vk::Vulkan& vk, std::optional<int> fd,
            VkExternalSemaphoreHandleTypeFlagBits handleType) {
        VkSemaphore handle{};

        const VkExportSemaphoreCreateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = handleType
        };
        const bool external = fd.has_value()
            || handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        const VkSemaphoreCreateInfo semaphoreInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = external ? &exportInfo : nullptr
        };
        auto res = vk.df().CreateSemaphore(vk.dev(), &semaphoreInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateSemaphore() failed");

        if (fd.has_value() && *fd >= 0) {
            // import semaphore from fd; sync_fd imports must be temporary
            const VkImportSemaphoreFdInfoKHR importInfo{
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore = handle,
                .flags = handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT ?
                    VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : VkSemaphoreImportFlags{},
                .handleType = handleType,
                .fd = *fd // closes the fd on success; we close it on failure below
            };
            res = vk.df().ImportSemaphoreFdKHR(vk.dev(), &importInfo);
            if (res != VK_SUCCESS) {
                close(*fd); // ownership was not transferred on failure
                throw ls::vulkan_error(res, "vkImportSemaphoreFdKHR() failed");
            }
        } else if (fd.has_value()) {
            // fd == -1: per spec this behaves like an already-signaled sync
            // fd. host-signaling is timeline-only, so signal via an empty
            // queue submission and drain the queue so the payload is
            // observably signaled once construction returns
            const VkSubmitInfo submitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &handle
            };
            res = vk.df().QueueSubmit(vk.queue(), 1, &submitInfo, VK_NULL_HANDLE);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkQueueSubmit() failed");

            res = vk.df().DeviceWaitIdle(vk.dev());
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkDeviceWaitIdle() failed");
        }

        return ls::owned_ptr<VkSemaphore>(
            new VkSemaphore(handle),
            [dev = vk.dev(), defunc = vk.df().DestroySemaphore](VkSemaphore& semaphore) {
                defunc(dev, semaphore, VK_NULL_HANDLE);
            }
        );
    }
}

Semaphore::Semaphore(const vk::Vulkan& vk, std::optional<int> fd,
    VkExternalSemaphoreHandleTypeFlagBits handleType)
    : semaphore(createSemaphore(vk, fd, handleType)), handleType(handleType) {}

int Semaphore::exportFd(const vk::Vulkan& vk) const {
    const VkSemaphoreGetFdInfoKHR getFdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = *this->semaphore,
        .handleType = this->handleType
    };
    int fd{};
    auto res = vk.df().GetSemaphoreFdKHR(vk.dev(), &getFdInfo, &fd);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetSemaphoreFdKHR() failed");

    return fd;
}
