/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

/// Scanout-isolated swapchain: layer-owned images, no WSI present.
/// The game renders and "presents"; we capture. KWin never sees these images.
/// Default on for external presentation; LSFGVK_ISOLATED_SWAPCHAIN=0 disables.

bool isolatedSwapchainEnabled();

struct IsolatedSwapchain {
    std::vector<vk::Image> images;
    std::vector<VkImage> handles;
    std::vector<vk::Fence> recycleFences; // SIGNALED at create; acquire waits
    uint32_t next{};
    VkQueue signalQueue{VK_NULL_HANDLE};  // dedicated; empty submits only
    VkFormat format{};
    VkExtent2D extent{};
};

VkSwapchainKHR allocIsolatedHandle();
bool isIsolated(VkSwapchainKHR handle);
bool isIsolatedTombstone(VkSwapchainKHR handle);

IsolatedSwapchain& isolatedAt(VkSwapchainKHR handle);
void destroyIsolated(const vk::Vulkan& vk, VkSwapchainKHR handle);

/// create layer-owned images + recycle fences. never calls the driver swapchain.
IsolatedSwapchain createIsolated(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& info);
void storeIsolated(VkSwapchainKHR handle, IsolatedSwapchain iso);

/// empty submit on signalQueue (falls back to `queue`) to signal a binary semaphore
/// and/or a fence. used for acquire-complete and DXVK present-fence release.
void isolatedSignal(const vk::Vulkan& vk, VkQueue signalQueue, VkQueue fallback,
    VkSemaphore semaphore, VkFence fence);

/// remembered extra queue requested in modifyDeviceCreateInfo (gfx q1 or compute q0)
void noteIsolatedSignalQueue(uint32_t family, uint32_t index);
/// true if modifyDeviceCreateInfo requested an extra queue
bool getIsolatedSignalQueue(uint32_t& family, uint32_t& index);

} // namespace lsfgvk::layer
