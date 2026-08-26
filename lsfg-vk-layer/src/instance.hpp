/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "swapchain.hpp"
#include "lsfg-vk-layer/capture_context.hpp"

#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <variant>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// root context of the lsfg-vk layer
    class Root {
    public:
        /// create the lsfg-vk root context
        /// @throws ls::error on failure
        Root();

        /// check if the layer is active
        /// @return true if active
        [[nodiscard]] bool active() const { return this->active_profile.has_value(); }

        /// ensure the layer is up-to-date
        /// @return true if the configuration was updated
        bool update();

        /// modify instance create info
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
            const std::function<void(void)>& finish) const;
        /// modify device create info
        /// @param funcs instance function pointers of the instance owning physdev
        /// @param physdev the game physical device the logical device is created from
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifyDeviceCreateInfo(const vk::VulkanInstanceFuncs& funcs, VkPhysicalDevice physdev,
            VkDeviceCreateInfo& createInfo, const std::function<void(void)>& finish) const;

        /// modify swapchain create info
        /// @param vk vulkan instance
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifySwapchainCreateInfo(const vk::Vulkan& vk, VkSwapchainCreateInfoKHR& createInfo,
            const std::function<void(void)>& finish) const;
        /// create swapchain context
        /// @param vk vulkan instance
        /// @param swapchain swapchain handle
        /// @param info swapchain info
        /// @throws ls::error on failure
        void createSwapchainContext(const vk::Vulkan& vk, VkSwapchainKHR swapchain,
            const SwapchainInfo& info);
        /// get swapchain context
        /// @param swapchain swapchain handle
        /// @return swapchain context
        /// @throws ls::error if not found
        /// @note thread-safe: takes a shared lock for the lookup; the returned
        /// reference stays valid because unordered_map nodes are stable and
        /// apps must not destroy a swapchain while presenting it (Vulkan
        /// external-synchronization rules)
        using ContextVariant = std::variant<Swapchain, CaptureContext>;

        [[nodiscard]] ContextVariant& getSwapchainContext(VkSwapchainKHR swapchain) {
            const std::shared_lock lock(this->mutex);
            const auto& it = this->swapchains.find(swapchain);
            if (it == this->swapchains.end())
                throw ls::error("swapchain context not found");

            return it->second;
        }
        /// dispatch present to the stored context (external or game)
        VkResult presentSwapchain(VkSwapchainKHR swapchain,
            const vk::Vulkan& vk, VkQueue queue,
            void* next_chain, uint32_t imageIdx,
            const std::vector<VkSemaphore>& semaphores);
        /// whether a swapchain is an external capture context
        [[nodiscard]] bool isExternalContext(VkSwapchainKHR swapchain) const;
        /// whether any external contexts are currently live
        [[nodiscard]] bool hasExternalContexts() const;
        /// remove swapchain context
        /// @param swapchain swapchain handle
        void removeSwapchainContext(VkSwapchainKHR swapchain);
    private:
        ls::WatchedConfig config;
        std::optional<ls::GameConf> active_profile;

        ls::lazy<backend::Instance> backend;
        std::optional<std::string> backendGpuKey; // gpu key the backend was created with
        std::unordered_map<VkSwapchainKHR, ContextVariant> swapchains;

        /// guards backend/swapchains against concurrent present threads.
        /// writers: lazy backend emplace + context create/remove (cold paths);
        /// readers: the per-present swapchain lookup (hot path - shared lock
        /// only, tens of ns uncontended vs ~800us frame budget)
        mutable std::shared_mutex mutex;
    };

}
