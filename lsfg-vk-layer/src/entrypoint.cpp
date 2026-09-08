/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "lsfg-vk-layer/isolated_swapchain.hpp"
#include "swapchain.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk::layer;

namespace {
    struct LayerInfo;
    struct InstanceInfo;

    // ---- dry-run timing ----------------------------------------------------
    static void dryRunSample(std::string_view name, double ms) {
        static bool enabled = !!std::getenv("LSFGVK_DRY_RUN");
        if (!enabled) return;
        static std::mutex m;
        static std::unordered_map<std::string, std::vector<double>> samples;
        std::unique_lock lock(m);
        samples[std::string(name)].push_back(ms);
    }

    static void dryRunPrint() {
        static bool enabled = !!std::getenv("LSFGVK_DRY_RUN");
        if (!enabled) return;
        static std::mutex m;
        static std::unordered_map<std::string, std::vector<double>> samples;
        std::unique_lock lock(m);
        for (const auto& [name, vals] : samples) {
            std::fprintf(stderr, "lsfg-vk-layer: [dry-run] %s n=%zu max=%.3f ms\n",
                name.c_str(), vals.size(), *std::max_element(vals.begin(), vals.end()));
            if (vals.size() > 1) {
                std::vector<double> sorted = vals;
                std::sort(sorted.begin(), sorted.end());
                const double p50 = sorted[sorted.size() / 2];
                const double p90 = sorted[sorted.size() * 9 / 10];
                std::fprintf(stderr, "lsfg-vk-layer: [dry-run]   p50=%.3f ms  p90=%.3f ms\n",
                    p50, p90);
            }
        }
        std::fprintf(stderr, "lsfg-vk-layer: [dry-run] entries=%zu\n", samples.size());
    }
    // ------------------------------------------------------------------------

    // global layer info initialized at layer negotiation
    struct LayerInfo {
        std::unordered_map<std::string, PFN_vkVoidFunction> map; //!< function pointer override map
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

        Root root;
    }* layer_info; // NOLINT (global variable)

    // instance-wide info initialized at instance creation(s)
    struct InstanceInfo {
        std::vector<VkInstance> handles; // there may be several instances
        vk::VulkanInstanceFuncs funcs;

        std::unordered_map<VkDevice, vk::Vulkan> devices;
        std::unordered_map<VkSwapchainKHR, ls::R<vk::Vulkan>> swapchains;
        std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchainInfos;
    }* instance_info; // NOLINT (global variable)

    // create instance
    VkResult myvkCreateInstance(
            const VkInstanceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkInstance* instance) {
        // apply layer chaining
        auto* layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                || layerInfo->function != VK_LAYER_LINK_INFO)) {
            layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "lsfg-vk: no layer info found in pNext chain, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* linkInfo = layerInfo->u.pLayerInfo;
        if (!linkInfo) {
            std::cerr << "lsfg-vk: link info is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layer_info->GetInstanceProcAddr = linkInfo->pfnNextGetInstanceProcAddr;
        if (!layer_info->GetInstanceProcAddr) {
            std::cerr << "lsfg-vk: next layer's vkGetInstanceProcAddr is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layerInfo->u.pLayerInfo = linkInfo->pNext; // advance for next layer

        // create instance
        auto* vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            layer_info->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!vkCreateInstance) {
            std::cerr << "lsfg-vk: failed to get next layer's vkCreateInstance, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        try {
            VkInstanceCreateInfo newInfo = *info;
            layer_info->root.modifyInstanceCreateInfo(newInfo,
                [=, newInfo = &newInfo]() {
                    auto res = vkCreateInstance(newInfo, alloc, instance);
                    if (res != VK_SUCCESS)
                        throw ls::vulkan_error(res, "vkCreateInstance() failed");
                }
            );

            if (!instance_info)
                instance_info = new InstanceInfo{ // NOLINT (memory management)
                    .funcs = vk::initVulkanInstanceFuncs(*instance,
                        layer_info->GetInstanceProcAddr, true),
                };

            instance_info->handles.push_back(*instance);

            return VK_SUCCESS;
        } catch (const ls::vulkan_error& e) {
            if (e.error() == VK_ERROR_EXTENSION_NOT_PRESENT)
                std::cerr << "lsfg-vk: required Vulkan instance extensions are not present. "
                    "Your GPU driver is not supported.\n";
            return e.error();
        }
    }

    // Wine's unix thunk for vkGetPastPresentationTimingEXT (winevulkan
    // thunk64 +0x2a2f5) does `mov (%rax)` on info->swapchain, treating
    // VkSwapchainKHR as a pointer to a wine object. Isolated handles are
    // 0x5af5xxxx integers, not wine objects → 0xc0000005 in the thunk
    // BEFORE our stub runs. Hide the extensions so DXVK never calls it.
    bool hideDeviceExt(const char* name) {
        return std::strcmp(name, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) == 0
            || std::strcmp(name, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0;
    }

    VkResult myvkEnumerateDeviceExtensionProperties(
            VkPhysicalDevice physdev, const char* layerName,
            uint32_t* pCount, VkExtensionProperties* pProperties) {
        if (!pCount || !instance_info || !instance_info->funcs.EnumerateDeviceExtensionProperties)
            return VK_ERROR_INITIALIZATION_FAILED;
        auto* next = instance_info->funcs.EnumerateDeviceExtensionProperties;
        if (layerName && layerName[0])
            return next(physdev, layerName, pCount, pProperties);

        uint32_t n = 0;
        VkResult r = next(physdev, nullptr, &n, nullptr);
        if (r != VK_SUCCESS)
            return r;
        std::vector<VkExtensionProperties> all(n);
        if (n > 0) {
            r = next(physdev, nullptr, &n, all.data());
            if (r != VK_SUCCESS && r != VK_INCOMPLETE)
                return r;
        }
        std::vector<VkExtensionProperties> keep;
        keep.reserve(all.size());
        for (const auto& e : all)
            if (!hideDeviceExt(e.extensionName))
                keep.push_back(e);

        if (!pProperties) {
            *pCount = static_cast<uint32_t>(keep.size());
            return VK_SUCCESS;
        }
        const uint32_t cap = *pCount;
        const uint32_t out = std::min(cap, static_cast<uint32_t>(keep.size()));
        for (uint32_t i = 0; i < out; ++i)
            pProperties[i] = keep[i];
        *pCount = out;
        return out < keep.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }

    // create device
    VkResult myvkCreateDevice(
            VkPhysicalDevice physdev,
            const VkDeviceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkDevice* device) {
        // apply layer chaining
        auto* layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || layerInfo->function != VK_LAYER_LINK_INFO)) {
            layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "lsfg-vk: no layer info found in pNext chain, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* linkInfo = layerInfo->u.pLayerInfo;
        if (!linkInfo) {
            std::cerr << "lsfg-vk: link info is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        instance_info->funcs.GetDeviceProcAddr = linkInfo->pfnNextGetDeviceProcAddr;
        if (!linkInfo->pfnNextGetDeviceProcAddr) {
            std::cerr << "lsfg-vk: next layer's vkGetDeviceProcAddr is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layerInfo->u.pLayerInfo = linkInfo->pNext; // advance for next layer

        // fetch device loader functions
        layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || layerInfo->function != VK_LOADER_DATA_CALLBACK)) {
            layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "lsfg-vk: no layer loader data found in pNext chain.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* setLoaderData = layerInfo->u.pfnSetDeviceLoaderData;
        if (!setLoaderData) {
            std::cerr << "lsfg-vk: instance loader data function is null.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // create device
        try {
            VkDeviceCreateInfo newInfo = *info;
            layer_info->root.modifyDeviceCreateInfo(instance_info->funcs, physdev, newInfo,
                [=, newInfo = &newInfo]() {
                    auto res = instance_info->funcs.CreateDevice(physdev, newInfo, alloc, device);
                    if (res != VK_SUCCESS)
                        throw ls::vulkan_error(res, "vkCreateDevice() failed");
                }
            );
        } catch (const ls::vulkan_error& e) {
            if (e.error() == VK_ERROR_EXTENSION_NOT_PRESENT)
                std::cerr << "lsfg-vk: required Vulkan device extensions are not present. "
                    "Your GPU driver is not supported.\n";
            return e.error();
        }

        // create layer instance
        // some applications create auxiliary devices without presentation
        // support (vkcube does); wrapping those with graphical=true throws
        // when the swapchain entry points are absent, so match the flag to
        // what the device actually enables
        const bool graphical = [&] {
            for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                if (std::string_view(info->ppEnabledExtensionNames[i])
                        == VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                    return true;
            return false;
        }();
        try {
            instance_info->devices.emplace(
                *device,
                vk::Vulkan(
                    instance_info->handles.front(), *device, physdev,
                    instance_info->funcs, vk::initVulkanDeviceFuncs(instance_info->funcs, *device,
                        graphical),
                    graphical, setLoaderData
                )
            );
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: something went wrong during lsfg-vk initialization:\n";
            std::cerr << "- " << e.what() << '\n';
        }

        return VK_SUCCESS;
    }

    // destroy device
    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
        // destroy layer instance
        auto it = instance_info->devices.find(device);
        if (it != instance_info->devices.end())
            instance_info->devices.erase(it);

        // destroy device
        auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkDestroyDevice"));
        if (!vkDestroyDevice) {
            std::cerr << "lsfg-vk: failed to get next layer's vkDestroyDevice, "
                "the previous layer does not follow spec\n";
            return;
        }

        vkDestroyDevice(device, alloc);
    }

    // destroy instance
    void myvkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
        // remove instance handle
        auto it = std::ranges::find(instance_info->handles, instance);
        if (it != instance_info->handles.end())
            instance_info->handles.erase(it);

        // destroy instance info if no handles remain
        if (instance_info->handles.empty()) {
            delete instance_info; // NOLINT (memory management)
            instance_info = nullptr;
        }

        // destroy instance
        auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            layer_info->GetInstanceProcAddr(instance, "vkDestroyInstance"));
        if (!vkDestroyInstance) {
            std::cerr << "lsfg-vk: failed to get next layer's vkDestroyInstance, "
                "the previous layer does not follow spec\n";
            return;
        }

        vkDestroyInstance(instance, alloc);
    }

    // get optional function pointer override
    PFN_vkVoidFunction getProcAddr(const std::string& name) {
        auto it = layer_info->map.find(name);
        if (it != layer_info->map.end())
            return it->second;
        return nullptr;
    }

    // get instance-level function pointers
    PFN_vkVoidFunction myvkGetInstanceProcAddr(VkInstance instance, const char* name) {
        if (!name) return nullptr;

        auto func = getProcAddr(name);
        if (func) return func;

        if (!layer_info->GetInstanceProcAddr) return nullptr;
        return layer_info->GetInstanceProcAddr(instance, name);
    }

    // get device-level function pointers
    PFN_vkVoidFunction myvkGetDeviceProcAddr(VkDevice device, const char* name) {
        if (!name) return nullptr;

        auto func = getProcAddr(name);
        if (func) return func;

        if (!instance_info->funcs.GetDeviceProcAddr) return nullptr;
        return instance_info->funcs.GetDeviceProcAddr(device, name);
    }
}

namespace {
    VkResult myvkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* info,
            const VkAllocationCallbacks* alloc,
            VkSwapchainKHR* swapchain) {
        const auto& it = instance_info->devices.find(device);
        if (it == instance_info->devices.end())
            return VK_ERROR_INITIALIZATION_FAILED;

        try {
            // retire old swapchain
            if (info->oldSwapchain) {
                if (isIsolated(info->oldSwapchain))
                    destroyIsolated(it->second, info->oldSwapchain);
                const auto& info_mapping = instance_info->swapchainInfos.find(info->oldSwapchain);
                if (info_mapping != instance_info->swapchainInfos.end())
                    instance_info->swapchainInfos.erase(info_mapping);

                const auto& mapping = instance_info->swapchains.find(info->oldSwapchain);
                if (mapping != instance_info->swapchains.end())
                    instance_info->swapchains.erase(mapping);

                layer_info->root.removeSwapchainContext(info->oldSwapchain);
            }

            layer_info->root.update(); // ensure config is up to date

            const bool isolated = layer_info->root.externalPresentation()
                && isolatedSwapchainEnabled();

            VkSwapchainCreateInfoKHR newInfo = *info;
            if (isolated) {
                IsolatedSwapchain iso = createIsolated(it->second, newInfo);
                const VkSwapchainKHR handle = allocIsolatedHandle();
                std::vector<VkImage> handles = iso.handles;
                storeIsolated(handle, std::move(iso));
                try {
                    auto& sinfo = instance_info->swapchainInfos.emplace(handle, SwapchainInfo {
                        .images = std::move(handles),
                        .format = newInfo.imageFormat,
                        .colorSpace = newInfo.imageColorSpace,
                        .extent = newInfo.imageExtent,
                        .presentMode = newInfo.presentMode,
                        .fake = true
                    }).first->second;
                    layer_info->root.createSwapchainContext(it->second, handle, sinfo);
                    instance_info->swapchains.emplace(handle, ls::R<vk::Vulkan>(it->second));
                    *swapchain = handle;
                    return VK_SUCCESS;
                } catch (...) {
                    instance_info->swapchains.erase(handle);
                    instance_info->swapchainInfos.erase(handle);
                    layer_info->root.removeSwapchainContext(handle);
                    if (isIsolated(handle))
                        destroyIsolated(it->second, handle);
                    throw;
                }
            }

            // create swapchain
            layer_info->root.modifySwapchainCreateInfo(it->second, newInfo,
                [=, newInfo = &newInfo]() {
                    auto res = it->second.df().CreateSwapchainKHR(
                        device, newInfo, alloc, swapchain);
                    if (res != VK_SUCCESS)
                        throw ls::vulkan_error(res, "vkCreateSwapchainKHR() failed");
                }
            );

            // get all swapchain images
            uint32_t imageCount{};
            auto res = it->second.df().GetSwapchainImagesKHR(device, *swapchain,
                &imageCount, VK_NULL_HANDLE);
            if (res != VK_SUCCESS || imageCount == 0)
                throw ls::vulkan_error(res, "vkGetSwapchainImagesKHR() failed");

            std::vector<VkImage> swapchainImages(imageCount);
            res = it->second.df().GetSwapchainImagesKHR(device, *swapchain,
                &imageCount, swapchainImages.data());
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkGetSwapchainImagesKHR() failed");

            auto& info = instance_info->swapchainInfos.emplace(*swapchain, SwapchainInfo {
                .images = std::move(swapchainImages),
                .format = newInfo.imageFormat,
                .colorSpace = newInfo.imageColorSpace,
                .extent = newInfo.imageExtent,
                .presentMode = newInfo.presentMode
            }).first->second;

            // create lsfg-vk swapchain
            layer_info->root.createSwapchainContext(it->second, *swapchain, info);

            instance_info->swapchains.emplace(*swapchain,
                ls::R<vk::Vulkan>(it->second));

            return res;
        } catch (const ls::vulkan_error& e) {
            std::cerr << "lsfg-vk: something went wrong during lsfg-vk swapchain creation:\n";
            std::cerr << "- " << e.what() << '\n';
            return e.error();
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: something went wrong during lsfg-vk swapchain creation:\n";
            std::cerr << "- " << e.what() << '\n';
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    VkResult myvkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex) {
        const auto& swIt = instance_info->swapchains.find(swapchain);
        if (swIt == instance_info->swapchains.end())
            return VK_ERROR_INITIALIZATION_FAILED;

        static const bool dbgAcq = std::getenv("LSFGVK_LAYER_DBG") != nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        VkResult res = VK_SUCCESS;
        if (isIsolated(swapchain)) {
            auto& iso = isolatedAt(swapchain);
            const uint32_t idx = iso.next % static_cast<uint32_t>(iso.recycleFences.size());
            // Never UINT64_MAX: a stuck recycle fence under exclusive overlay
            // is an untabbable black screen. 8 ms then hand out the image.
            constexpr uint64_t kCapNs = 8ull * 1000ull * 1000ull;
            const uint64_t ns = (timeout == UINT64_MAX || timeout > kCapNs) ? kCapNs : timeout;
            (void)iso.recycleFences.at(idx).wait(swIt->second.get(), ns);
            iso.recycleFences.at(idx).reset(swIt->second.get());
            iso.next = idx + 1;
            *pImageIndex = idx;
            isolatedSignal(swIt->second.get(), iso.signalQueue,
                swIt->second.get().queue(),
                semaphore, fence);
            res = VK_SUCCESS;
        } else {
            res = swIt->second.get().df().AcquireNextImageKHR(
                device, swapchain, timeout, semaphore, fence, pImageIndex);
        }
        if (dbgAcq) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr,
                "lsfg-vk-layer: [dbg] acquire timeout=%llu result=%d wait=%lld ms\n",
                static_cast<unsigned long long>(timeout), res, static_cast<long long>(ms));
        }
        return res;
    }

    VkResult myvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        VkResult result = VK_SUCCESS;

        // ensure layer config is up to date
        bool reload{};
        try {
            reload = layer_info->root.update();
        } catch (const std::exception&) {
            reload = false; // ignore parse errors
        }

        if (reload) {
            try {
                for (const auto& [swapchain, vk] : instance_info->swapchains) {
                    // external-mode contexts are NOT rebuilt by hot-reload:
                    // rebuild would need a full IPC handshake + staging
                    // re-export, and doing it under a concurrently blocked
                    // present widens the existing UAF window (escaped
                    // reference from getSwapchainContext's short-lived shared
                    // lock) from frame-scale to seconds. skip them.
                    if (layer_info->root.isExternalContext(swapchain))
                        continue;

                    auto& info = instance_info->swapchainInfos.at(swapchain);

                    layer_info->root.removeSwapchainContext(swapchain);
                    layer_info->root.createSwapchainContext(vk, swapchain, info);
                }

                std::cerr << "lsfg-vk: updated lsfg-vk configuration\n";
            } catch (const std::exception& e) {
                std::cerr << "lsfg-vk: something went wrong during lsfg-vk configuration update:\n";
                std::cerr << "- " << e.what() << '\n';
            }
        }

        // present each swapchain
        for (size_t i = 0; i < info->swapchainCount; i++) {
            const auto& swapchain = info->pSwapchains[i];

            const auto& it = instance_info->swapchains.find(swapchain);
            if (it == instance_info->swapchains.end())
                return VK_ERROR_INITIALIZATION_FAILED;

            try {
                std::vector<VkSemaphore> waitSemaphores;
                waitSemaphores.reserve(info->waitSemaphoreCount);

                for (size_t j = 0; j < info->waitSemaphoreCount; j++)
                    waitSemaphores.push_back(info->pWaitSemaphores[j]);

                {
                    static const bool dbgPres{ std::getenv("LSFGVK_LAYER_DBG") != nullptr };
                    const auto t0 = std::chrono::steady_clock::now();
                    if (dbgPres) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        std::fprintf(stderr,
                            "lsfg-vk-layer: [dbg] present-enter swapchain=%p idx=%u @%lld ms\n",
                            (void*)swapchain,
                            static_cast<unsigned>(info->pImageIndices[i]),
                            static_cast<long long>(ms));
                    }
                    try {
                        result = layer_info->root.presentSwapchain(swapchain,
                            it->second, queue,
                            const_cast<void*>(info->pNext),
                            info->pImageIndices[i],
                            { waitSemaphores.begin(), waitSemaphores.end() }
                        );
                    } catch (const std::exception& e) {
                        // Overlay kicked the 1080 socket for a 1440 HELLO.
                        // Isolated has no scanout; still SUCCESS so DXVK
                        // present-fences do not latch.
                        if (!isIsolated(swapchain))
                            throw;
                        std::cerr << "lsfg-vk: isolated present skipped: " << e.what() << "\n";
                        result = VK_SUCCESS;
                    }
                    if (isIsolated(swapchain) && result == VK_SUCCESS) {
                        auto& iso = isolatedAt(swapchain);
                        const uint32_t idx = info->pImageIndices[i];
                        isolatedSignal(it->second.get(), iso.signalQueue, iso.signalQueue,
                            VK_NULL_HANDLE, iso.recycleFences.at(idx).handle());
                        // DXVK frame-latency latch: present fences in pNext must
                        // signal or the game waits forever (fullscreen freeze).
                        for (const auto* n = static_cast<const VkBaseInStructure*>(info->pNext);
                                n != nullptr;
                                n = static_cast<const VkBaseInStructure*>(n->pNext)) {
                            if (n->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR)
                                continue;
                            const auto* fi = reinterpret_cast<const VkSwapchainPresentFenceInfoKHR*>(n);
                            for (uint32_t k = 0; k < fi->swapchainCount; ++k) {
                                if (fi->pFences && fi->pFences[k] != VK_NULL_HANDLE)
                                    isolatedSignal(it->second.get(), iso.signalQueue, queue,
                                        VK_NULL_HANDLE, fi->pFences[k]);
                            }
                        }
                    }
                    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (dbgPres) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        std::fprintf(stderr,
                            "lsfg-vk-layer: [dbg] present-exit  swapchain=%p result=%d wait=%lld ms @%lld ms\n",
                            (void*)swapchain,
                            static_cast<int>(result),
                            static_cast<long long>(d),
                            static_cast<long long>(ms));
                    }
                }
            } catch (const ls::vulkan_error& e) {
                if (e.error() != VK_ERROR_OUT_OF_DATE_KHR) {
                    std::cerr << "lsfg-vk: something went wrong during lsfg-vk swapchain presentation:\n";
                    std::cerr << "- " << e.what() << '\n';
                } // silently swallow out-of-date errors

                result = e.error();
            } catch (const std::exception& e) {
                std::cerr << "lsfg-vk: something went wrong during lsfg-vk swapchain presentation:\n";
                std::cerr << "- " << e.what() << '\n';
                result = VK_ERROR_UNKNOWN;
            }

            if (result != VK_SUCCESS && info->pResults)
                info->pResults[i] = result;
        }

        return result;
#pragma clang diagnostic pop
    }

    VkResult myvkAcquireNextImage2KHR(VkDevice device,
            const VkAcquireNextImageInfoKHR* info, uint32_t* pImageIndex) {
        if (!info) return VK_ERROR_INITIALIZATION_FAILED;
        return myvkAcquireNextImageKHR(device, info->swapchain, info->timeout,
            info->semaphore, info->fence, pImageIndex);
    }

    VkResult myvkWaitForPresent2KHR(VkDevice, VkSwapchainKHR, const VkPresentWait2InfoKHR*) {
        return VK_SUCCESS;
    }

    void myvkSetHdrMetadataEXT(VkDevice, uint32_t, const VkSwapchainKHR*, const VkHdrMetadataEXT*) {}

    VkResult myvkReleaseSwapchainImagesKHR(VkDevice, const void*) { return VK_SUCCESS; }
    VkResult myvkReleaseSwapchainImagesEXT(VkDevice, const void*) { return VK_SUCCESS; }
    VkResult myvkGetPastPresentationTimingGOOGLE(VkDevice, VkSwapchainKHR,
            uint32_t* count, void*) {
        if (count) *count = 0;
        return VK_SUCCESS;
    }
    VkResult myvkGetPastPresentationTimingEXT(VkDevice, const void*, void* out) {
        if (out) {
            auto* p = static_cast<VkPastPresentationTimingPropertiesEXT*>(out);
            p->timingPropertiesCounter = 0;
            p->timeDomainsCounter = 0;
            p->presentationTimingCount = 0;
        }
        return VK_SUCCESS;
    }

    void myvkGetLatencyTimingsNV(VkDevice, VkSwapchainKHR, void* info) {
        if (info) {
            auto* p = static_cast<VkGetLatencyMarkerInfoNV*>(info);
            p->timingCount = 0;
        }
    }
    VkResult myvkSetLatencySleepModeNV(VkDevice, VkSwapchainKHR, const void*) {
        return VK_SUCCESS;
    }
    VkResult myvkLatencySleepNV(VkDevice, VkSwapchainKHR, const void*) {
        return VK_SUCCESS;
    }
    void myvkSetLatencyMarkerNV(VkDevice, VkSwapchainKHR, const void*) {}
    VkResult myvkCreateSharedSwapchainsKHR(VkDevice, uint32_t, const void*,
            const VkAllocationCallbacks*, VkSwapchainKHR*) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult myvkSetSwapchainPresentTimingQueueSizeEXT(VkDevice, VkSwapchainKHR, uint32_t) {
        return VK_SUCCESS;
    }
    VkResult myvkGetSwapchainTimingPropertiesEXT(VkDevice, VkSwapchainKHR,
            VkSwapchainTimingPropertiesEXT* pProps, uint64_t* pCounter) {
        if (pProps) *pProps = {};
        if (pCounter) *pCounter = 0;
        return VK_SUCCESS;
    }
    VkResult myvkGetSwapchainTimeDomainPropertiesEXT(VkDevice, VkSwapchainKHR,
            VkSwapchainTimeDomainPropertiesEXT* pProps, uint64_t* pCounter) {
        if (pProps) *pProps = {};
        if (pCounter) *pCounter = 0;
        return VK_SUCCESS;
    }
    VkResult myvkGetRefreshCycleDurationGOOGLE(VkDevice, VkSwapchainKHR,
            VkRefreshCycleDurationGOOGLE* p) {
        if (p) *p = {};
        return VK_SUCCESS;
    }
    VkResult myvkGetSwapchainCounterEXT(VkDevice, VkSwapchainKHR, VkSurfaceCounterFlagBitsEXT, uint64_t* v) {
        if (v) *v = 0;
        return VK_SUCCESS;
    }
    void myvkSetLocalDimmingAMD(VkDevice, VkSwapchainKHR, VkBool32) {}

    VkResult myvkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
            uint32_t* pCount, VkImage* pImages) {
        if (isIsolatedTombstone(swapchain)) {
            if (pCount) *pCount = 0;
            return VK_SUCCESS;
        }
        if (isIsolated(swapchain)) {
            auto& iso = isolatedAt(swapchain);
            const uint32_t n = static_cast<uint32_t>(iso.handles.size());
            if (pImages == nullptr) {
                *pCount = n;
                return VK_SUCCESS;
            }
            if (*pCount < n) {
                *pCount = n;
                return VK_INCOMPLETE;
            }
            for (uint32_t i = 0; i < n; ++i)
                pImages[i] = iso.handles[i];
            *pCount = n;
            return VK_SUCCESS;
        }
        const auto& it = instance_info->devices.find(device);
        if (it == instance_info->devices.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        return it->second.df().GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    }

    VkResult myvkWaitForPresentKHR(VkDevice, VkSwapchainKHR, uint64_t, uint64_t) {
        return VK_SUCCESS; // isolated presents complete when QueuePresent returns
    }

    VkResult myvkGetSwapchainStatusKHR(VkDevice, VkSwapchainKHR) {
        return VK_SUCCESS;
    }

    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* alloc) {
        const auto& it = instance_info->devices.find(device);
        if (it == instance_info->devices.end())
            return;

        const auto& info_mapping = instance_info->swapchainInfos.find(swapchain);
        if (info_mapping != instance_info->swapchainInfos.end())
            instance_info->swapchainInfos.erase(info_mapping);

        const auto& mapping = instance_info->swapchains.find(swapchain);
        if (mapping != instance_info->swapchains.end())
            instance_info->swapchains.erase(mapping);

        layer_info->root.removeSwapchainContext(swapchain);

        if (isIsolated(swapchain) || isIsolatedTombstone(swapchain)) {
            if (isIsolated(swapchain))
                destroyIsolated(it->second, swapchain);
            return;
        }

        // destroy swapchain
        it->second.df().DestroySwapchainKHR(device, swapchain, alloc);
    }
}

/// Vulkan layer entrypoint
__attribute__((visibility("default")))
VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    // ensure loader compatibility
    if (!pVersionStruct
        || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT
        || pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    // if the layer has already been initialized, skip
    if (layer_info) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
        pVersionStruct->pfnGetDeviceProcAddr = myvkGetDeviceProcAddr;
        pVersionStruct->pfnGetInstanceProcAddr = myvkGetInstanceProcAddr;
        return VK_SUCCESS;
    }

    // load the layer configuration
    try {
        layer_info = new LayerInfo { // NOLINT (memory management)
            .map = {
#define VKPTR(name) reinterpret_cast<PFN_vkVoidFunction>(name)
                { "vkCreateInstance", VKPTR(myvkCreateInstance) },
                { "vkCreateDevice", VKPTR(myvkCreateDevice) },
                { "vkEnumerateDeviceExtensionProperties", VKPTR(myvkEnumerateDeviceExtensionProperties) },
                { "vkDestroyDevice", VKPTR(myvkDestroyDevice) },
                { "vkDestroyInstance", VKPTR(myvkDestroyInstance) },
                { "vkCreateSwapchainKHR", VKPTR(myvkCreateSwapchainKHR) },
                { "vkGetSwapchainImagesKHR", VKPTR(myvkGetSwapchainImagesKHR) },
                { "vkAcquireNextImageKHR", VKPTR(myvkAcquireNextImageKHR) },
                { "vkAcquireNextImage2KHR", VKPTR(myvkAcquireNextImage2KHR) },
                { "vkWaitForPresentKHR", VKPTR(myvkWaitForPresentKHR) },
                { "vkWaitForPresent2KHR", VKPTR(myvkWaitForPresent2KHR) },
                { "vkGetSwapchainStatusKHR", VKPTR(myvkGetSwapchainStatusKHR) },
                { "vkSetHdrMetadataEXT", VKPTR(myvkSetHdrMetadataEXT) },
                { "vkReleaseSwapchainImagesKHR", VKPTR(myvkReleaseSwapchainImagesKHR) },
                { "vkReleaseSwapchainImagesEXT", VKPTR(myvkReleaseSwapchainImagesEXT) },
                { "vkGetPastPresentationTimingGOOGLE", VKPTR(myvkGetPastPresentationTimingGOOGLE) },
                { "vkGetPastPresentationTimingEXT", VKPTR(myvkGetPastPresentationTimingEXT) },
                { "vkSetSwapchainPresentTimingQueueSizeEXT", VKPTR(myvkSetSwapchainPresentTimingQueueSizeEXT) },
                { "vkGetSwapchainTimingPropertiesEXT", VKPTR(myvkGetSwapchainTimingPropertiesEXT) },
                { "vkGetSwapchainTimeDomainPropertiesEXT", VKPTR(myvkGetSwapchainTimeDomainPropertiesEXT) },
                { "vkGetRefreshCycleDurationGOOGLE", VKPTR(myvkGetRefreshCycleDurationGOOGLE) },
                { "vkGetSwapchainCounterEXT", VKPTR(myvkGetSwapchainCounterEXT) },
                { "vkSetLocalDimmingAMD", VKPTR(myvkSetLocalDimmingAMD) },
                { "vkGetLatencyTimingsNV", VKPTR(myvkGetLatencyTimingsNV) },
                { "vkSetLatencySleepModeNV", VKPTR(myvkSetLatencySleepModeNV) },
                { "vkLatencySleepNV", VKPTR(myvkLatencySleepNV) },
                { "vkSetLatencyMarkerNV", VKPTR(myvkSetLatencyMarkerNV) },
                { "vkCreateSharedSwapchainsKHR", VKPTR(myvkCreateSharedSwapchainsKHR) },
                { "vkQueuePresentKHR", VKPTR(myvkQueuePresentKHR) },
                { "vkDestroySwapchainKHR", VKPTR(myvkDestroySwapchainKHR) }
#undef VKPTR
            },
            .root = Root()
        };

        if (!layer_info->root.active()) { // skip inactive
            delete layer_info; // NOLINT (memory management)
            layer_info = nullptr;

            return VK_ERROR_INITIALIZATION_FAILED;
        }
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: something went wrong during lsfg-vk layer initialization:\n";
        std::cerr << "- " << e.what() << '\n';

        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // emplace function pointers/version
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    pVersionStruct->pfnGetDeviceProcAddr = myvkGetDeviceProcAddr;
    pVersionStruct->pfnGetInstanceProcAddr = myvkGetInstanceProcAddr;
    std::atexit(dryRunPrint);
    return VK_SUCCESS;
}
