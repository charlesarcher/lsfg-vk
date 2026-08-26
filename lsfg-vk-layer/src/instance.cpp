/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "swapchain.hpp"
#include "lsfg-vk-common/configuration/detection.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
    /// helper function to add required extensions
    std::vector<const char*> add_extensions(const char* const* existingExtensions, size_t count,
            const std::vector<const char*>& requiredExtensions) {
        std::vector<const char*> extensions(count);
        std::copy_n(existingExtensions, count, extensions.data());

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(extensions,
                [requiredExtension](const char* extension) {
                    return std::string(extension) == std::string(requiredExtension);
                });
            if (it == extensions.end())
                extensions.push_back(requiredExtension);
        }

        return extensions;
    }

    /// identity and dual-gpu exchange capabilities of a physical device,
    /// formatted exactly like the backend picker formats its candidates
    struct DeviceProbe {
        bool dmaBuf{};   // VK_EXT_external_memory_dma_buf present
        bool drmModifier{};  // VK_EXT_image_drm_format_modifier present

        std::string name;   // VkPhysicalDeviceProperties2::deviceName
        std::string ids;    // "0xVVVV:0xDDDD" (backend to_hex_id format)
        std::optional<std::string> pci;  // "bus:device.function" if VK_EXT_pci_bus_info present
    };

    /// format a vendor/device id exactly like backend::to_hex_id does ("0xXXXX")
    std::string to_hex_id(uint32_t id) {
        static constexpr std::array<char, 17> chars = std::to_array("0123456789ABCDEF");

        std::string result = "0x";
        result += chars.at((id >> 12) & 0xF);
        result += chars.at((id >> 8) & 0xF);
        result += chars.at((id >> 4) & 0xF);
        result += chars.at(id & 0xF);
        return result;
    }

    /// probe a physical device's identity and exchange extension support
    /// @param funcs instance function pointers of the instance owning physdev
    /// @param physdev physical device to probe
    DeviceProbe probeDevice(const vk::VulkanInstanceFuncs& funcs, VkPhysicalDevice physdev) {
        // enumerate the device's extensions (mirrors the backend's selection probing)
        uint32_t count{};
        funcs.EnumerateDeviceExtensionProperties(physdev, nullptr, &count, nullptr);

        std::vector<VkExtensionProperties> extensions(count);
        funcs.EnumerateDeviceExtensionProperties(physdev, nullptr, &count, extensions.data());

        DeviceProbe probe{};
        bool hasPciExt{false};
        for (const auto& ext : extensions) {
            const std::string name(std::to_array(ext.extensionName).data());
            if (name == "VK_EXT_external_memory_dma_buf")
                probe.dmaBuf = true;
            else if (name == "VK_EXT_image_drm_format_modifier")
                probe.drmModifier = true;
            else if (name == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)
                hasPciExt = true;
        }

        // then fetch all available properties
        VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = hasPciExt ? &pciInfo : nullptr
        };
        funcs.GetPhysicalDeviceProperties2(physdev, &props);

        std::array<char, 256> devname = std::to_array(props.properties.deviceName);
        devname.at(255) = '\0'; // ensure null-termination
        probe.name = std::string(devname.data());
        probe.ids = to_hex_id(props.properties.vendorID) + ":" + to_hex_id(props.properties.deviceID);
        // drivers may leave the chained struct unfilled on instances predating the
        // extension's struct-filling (observed: all zeros); treat that as unavailable
        // rather than matching a bogus "0:0.0" bus
        if (hasPciExt && (pciInfo.pciBus != 0 || pciInfo.pciDevice != 0
                || pciInfo.pciFunction != 0))
            probe.pci = std::to_string(pciInfo.pciBus) + ":" +
                std::to_string(pciInfo.pciDevice) + "." +
                std::to_string(pciInfo.pciFunction);

        return probe;
    }

    /// check whether a probed device matches a configured gpu selector
    /// (device name | "0xVVVV:0xDDDD" | pci bus id, as documented for the gpu option)
    bool matchesSelector(const DeviceProbe& dev, const std::string& gpu) {
        return dev.name == gpu
            || dev.ids == gpu
            || (dev.pci.has_value() && *dev.pci == gpu);
    }
}

Root::Root() {
    // find active profile
    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (!profile.has_value())
        return;

    this->active_profile = profile->second;

    std::cerr << "lsfg-vk: using profile with name '" << this->active_profile->name << "' ";
    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            std::cerr << "(identified via override)\n";
            break;
        case ls::IdentType::EXECUTABLE:
            std::cerr << "(identified via executable)\n";
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr << "(identified via wine executable)\n";
            break;
        case ls::IdentType::PROCESS_NAME:
            std::cerr << "(identified via process name)\n";
            break;
    }
}

bool Root::update() {
    if (!this->config.update())
        return false;

    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (profile.has_value())
        this->active_profile = profile->second;
    else
        this->active_profile = std::nullopt;

    // external-mode contexts are NOT rebuilt by the hot-reload loop:
    // a rebuild would need a full IPC handshake + staging re-export, and a
    // rebuild under a concurrently blocked present widens the existing UAF
    // window (escaped reference from getSwapchainContext's short-lived shared
    // lock) from frame-scale to seconds. log once and suppress the reload
    // signal when any external context is live so the caller skips the loop.
    if (this->hasExternalContexts()) {
        // keep the gpu-change honesty line for the backend case, but also
        // surface a generic line for any external-mode profile change
        if (this->backend.has_value() && this->active_profile.has_value()
                && this->active_profile->gpu != this->backendGpuKey) {
            std::cerr << "lsfg-vk: gpu change requires restart to take effect\n";
            this->backendGpuKey = this->active_profile->gpu;
        } else if (this->active_profile.has_value()
                && this->active_profile->presentation == ls::Presentation::External) {
            // any hot-reload while external is active requires restart;
            // the config watcher already updated active_profile, but callers
            // must not rebuild the context
            std::cerr << "lsfg-vk: config change requires restart to take effect\n";
        }
        // suppress rebuild: external contexts stay as-is
        // still return false to the caller so the reload loop is skipped
        // for external contexts; game contexts would still be rebuilt if mixed,
        // but the caller handles that per-swapchain below. we return true
        // only to indicate the config did change; the caller decides per-context.
        // to keep the contract simple, return true and let the caller filter.
        return true;
    }

    // hot-reload honesty: the backend persists for the whole process lifetime
    // (vulkan loader bug workaround), so a changed gpu key cannot retarget the
    // processing device mid-run; report it instead of silently ignoring it
    if (this->backend.has_value() && this->active_profile.has_value()
            && this->active_profile->gpu != this->backendGpuKey) {
        std::cerr << "lsfg-vk: gpu change requires restart to take effect\n";
        this->backendGpuKey = this->active_profile->gpu;
    }

    return true;
}

void Root::modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_semaphore_capabilities"
        }
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    finish();
}

void Root::modifyDeviceCreateInfo(const vk::VulkanInstanceFuncs& funcs, VkPhysicalDevice physdev,
        VkDeviceCreateInfo& createInfo, const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;
    const auto& profile = *this->active_profile;

    // probe the game device; the dual-gpu exchange extensions are enabled only where
    // supported, while the legacy KHR set below stays unconditional (load-bearing for
    // the fd-based sharing path)
    const auto game = probeDevice(funcs, physdev);

    // Q4 policy (no silent fallback): a gpu other than this device was requested, but
    // the device cannot participate in dma-buf exchange -> fail with a named error
    // before vkCreateDevice rejects the injected extensions opaquely
    if ((!game.dmaBuf || !game.drmModifier)
            && profile.gpu.has_value() && !matchesSelector(game, *profile.gpu)) {
        std::string missing;
        if (!game.dmaBuf)
            missing = "VK_EXT_external_memory_dma_buf";
        if (!game.drmModifier)
            missing = missing.empty()
                ? std::string("VK_EXT_image_drm_format_modifier")
                : missing + " and VK_EXT_image_drm_format_modifier";

        throw ls::error("gpu '" + *profile.gpu + "' was requested for frame generation, but the"
            " game device '" + game.name + "' does not support " + missing + ", which is required"
            " for dual-gpu operation");
    }

    std::vector<const char*> requiredExtensions{
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_timeline_semaphore"
    };
    if (game.dmaBuf)
        requiredExtensions.push_back("VK_EXT_external_memory_dma_buf");
    if (game.drmModifier)
        requiredExtensions.push_back("VK_EXT_image_drm_format_modifier");

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        requiredExtensions
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool isFeatureEnabled = false;
    auto* featureInfo = reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));
    while (featureInfo) {
        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        } else if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        }

        featureInfo = const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = const_cast<void*>(createInfo.pNext),
        .timelineSemaphore = VK_TRUE
    };
    if (!isFeatureEnabled)
        createInfo.pNext = &timelineFeatures;

    finish();
}

void Root::modifySwapchainCreateInfo(const vk::Vulkan& vk, VkSwapchainCreateInfoKHR& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;

    VkSurfaceCapabilitiesKHR caps{}; // NOLINT (enum value 0)
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(), createInfo.surface, &caps);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");

    context_ModifySwapchainCreateInfo(*this->active_profile, caps.maxImageCount, createInfo);

    finish();
}

void Root::createSwapchainContext(const vk::Vulkan& vk,
        VkSwapchainKHR swapchain, const SwapchainInfo& info) {
    // writer lock: guards the lazy backend emplace and the swapchains map
    // against concurrent present threads (multi-device processes). cold path
    // only - swapchain creation/recreation - so blocking readers here is
    // acceptable and strictly better than the previous data race
    const std::unique_lock lock(this->mutex);
    if (!this->active_profile.has_value())
        throw ls::error("attempted to create swapchain context while layer is inactive");
    const auto& profile = *this->active_profile;

    // capture the game device's identity so an unset gpu pins frame generation to
    // the game's own physical device instead of the first-enumerated one; the
    // backend-owned DevicePicker signature carries only name/ids/pci strings, so
    // device-uuid equality is expressed through this identity tuple.
    // probed per swapchain (not only on backend emplace) so every context can
    // log its dual-gpu mode against the real game device
    const auto game = probeDevice(vk.fi(), vk.physdev());

    // external presentation: thin capture path, no backend instance involved
    if (profile.presentation == ls::Presentation::External) {
        this->swapchains.emplace(swapchain,
            CaptureContext(vk, profile, info, game.name));
        return;
    }

    // any configured gpu requests dma-buf capability on the backend device
    // (best-effort: the extensions are enabled there iff supported). whether a
    // given context actually crosses devices is decided per-context against
    // its own game device - multi-device processes can wrap several game
    // devices, so gating on this first context's match result broke later
    // cross-device contexts (T14 finding)
    const bool gpuConfigured = profile.gpu.has_value();

    if (!this->backend.has_value()) { // emplace backend late, due to loader bug
        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::string dll{};
            if (global.dll.has_value())
                dll = *global.dll;
            else
                dll = ls::findShaderDll();

            if (!profile.gpu.has_value())
                std::cerr << "lsfg-vk: no gpu configured, running frame generation on the"
                    " game's own device '" << game.name << "'\n";

            this->backend.emplace(
                [gpu = profile.gpu, game](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci
                ) {
                    if (!gpu) // pin selection to the game's own device
                        return deviceName == game.name
                            && (ids.first + ":" + ids.second) == game.ids
                            && (!game.pci.has_value()
                                || (pci.has_value() && *pci == *game.pci));

                    return (deviceName == *gpu)
                        || (ids.first + ":" + ids.second == *gpu)
                        || (pci && *pci == *gpu);
                },
                dll, global.allow_fp16, gpuConfigured
            );
        } catch (const std::exception& e) {
            unsetenv("DISABLE_LSFGVK");
            throw ls::error("failed to create backend instance" +
                (profile.gpu.has_value()
                    ? " for requested gpu '" + *profile.gpu + "'"
                    : ""), e);
        }

        unsetenv("DISABLE_LSFGVK");

        // remember which gpu key the persistent backend was constructed with,
        // so hot-reloaded gpu changes can be reported honestly in update()
        this->backendGpuKey = profile.gpu;
    }

    this->swapchains.emplace(swapchain,
        Swapchain(vk, this->backend.mut(), profile, info, game.name));
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    const std::unique_lock lock(this->mutex); // writer: map mutation (cold path)
    this->swapchains.erase(swapchain);
}

VkResult Root::presentSwapchain(VkSwapchainKHR swapchain,
        const vk::Vulkan& vk, VkQueue queue,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    // thread-safe lookup with shared lock then dispatch via variant
    // the unordered_map node stability guarantees the reference stays valid
    // per Vulkan external-synchronization rules (no concurrent destroy)
    ContextVariant* ctx = nullptr;
    {
        const std::shared_lock lock(this->mutex);
        const auto it = this->swapchains.find(swapchain);
        if (it == this->swapchains.end())
            throw ls::error("swapchain context not found");
        ctx = const_cast<ContextVariant*>(&it->second);
    }
    return std::visit([&](auto& c) -> VkResult {
        return c.present(vk, queue, swapchain, next_chain, imageIdx, semaphores);
    }, *ctx);
}

bool Root::isExternalContext(VkSwapchainKHR swapchain) const {
    const std::shared_lock lock(this->mutex);
    const auto it = this->swapchains.find(swapchain);
    if (it == this->swapchains.end()) return false;
    return std::holds_alternative<CaptureContext>(it->second);
}

bool Root::hasExternalContexts() const {
    const std::shared_lock lock(this->mutex);
    for (const auto& [_, ctx] : this->swapchains)
        if (std::holds_alternative<CaptureContext>(ctx)) return true;
    return false;
}
