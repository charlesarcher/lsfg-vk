/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// load libvulkan.so.1 and return its handle
    void* get_vulkan_handle() {
        static void* handle{nullptr}; // NOLINT (const correctness)
        if (handle) return handle;

        handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!handle) handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            throw ls::vulkan_error("failed to load libvulkan.so.1");

        return handle;
    }

    /// get the main proc addr function
    PFN_vkGetInstanceProcAddr get_mpa() {
        static PFN_vkGetInstanceProcAddr mpa{nullptr};
        if (mpa) return mpa;

        mpa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(get_vulkan_handle(), "vkGetInstanceProcAddr"));
        if (!mpa)
            throw ls::vulkan_error("failed to get vkGetInstanceProcAddr symbol");

        return mpa;
    }
}

namespace {
    template<typename T>
    T ipa(PFN_vkGetInstanceProcAddr mpa, VkInstance instance, const char* name) {
        T func = reinterpret_cast<T>(
            mpa(instance, name));
        if (!func)
            throw ls::vulkan_error("failed to get instance proc addr for " + std::string(name));
        return func;
    }

    /// create a vulkan instance
    ls::owned_ptr<VkInstance> createInstance(
            const std::string& appName, version appVersion,
            const std::string& engineName, version engineVersion) {
        VkInstance handle{};

        auto vkCreateInstance =
            ipa<PFN_vkCreateInstance>(get_mpa(), VK_NULL_HANDLE, "vkCreateInstance");
        if (!vkCreateInstance)
            throw ls::vulkan_error("failed to get vkCreateInstance symbol");

        const VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = appName.c_str(),
            .applicationVersion = appVersion.into(),
            .pEngineName = engineName.c_str(),
            .engineVersion = engineVersion.into(),
            .apiVersion = VK_API_VERSION_1_2 // seems 1.2 is supported on all Vulkan-capable GPUs
        };
        const VkInstanceCreateInfo instanceInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &appInfo
        };
        auto res = vkCreateInstance(&instanceInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateInstance() failed");

        auto defunc =
            ipa<PFN_vkDestroyInstance>(get_mpa(), handle, "vkDestroyInstance");
        if (!defunc)
            throw ls::vulkan_error("failed to get vkDestroyInstance symbol");
        return ls::owned_ptr<VkInstance>(
            new VkInstance(handle),
            [defunc](VkInstance& instance) {
                defunc(instance, VK_NULL_HANDLE);
            }
        );
    }

    /// filter for a physical device
    VkPhysicalDevice findPhysicalDevice(const VulkanInstanceFuncs& fi,
            VkInstance instance,
            PhysicalDeviceSelector filter) {
        uint32_t phydevCount{};
        auto res = fi.EnumeratePhysicalDevices(instance, &phydevCount, VK_NULL_HANDLE);
        if (res != VK_SUCCESS || phydevCount == 0)
            throw ls::vulkan_error(res, "vkEnumeratePhysicalDevices() failed");

        std::vector<VkPhysicalDevice> phydevs(phydevCount);
        res = fi.EnumeratePhysicalDevices(instance, &phydevCount, phydevs.data());
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkEnumeratePhysicalDevices() failed");

        VkPhysicalDevice selected = filter(fi, phydevs);
        if (!selected)
            throw ls::vulkan_error("no suitable physical device found");

        return selected;
    }

    /// find the queue family index with given flags
    uint32_t findQFI(const VulkanInstanceFuncs& fi,
            VkPhysicalDevice physdev, VkQueueFlags flags) {
        uint32_t queueCount{};
        fi.GetPhysicalDeviceQueueFamilyProperties(physdev, &queueCount, VK_NULL_HANDLE);

        std::vector<VkQueueFamilyProperties> queues(queueCount);
        fi.GetPhysicalDeviceQueueFamilyProperties(physdev, &queueCount, queues.data());

        for (uint32_t i = 0; i < queueCount; ++i) {
            if ((queues.at(i).queueFlags & flags) == flags)
                return i;
        }

        throw ls::vulkan_error("no queue family with requested flags found");
    }

    /// check for fp16 support
    bool checkFP16(const VulkanInstanceFuncs& fi, VkPhysicalDevice physdev) {
        VkPhysicalDeviceVulkan12Features supportedFeaturesVulkan12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
        };
        VkPhysicalDeviceFeatures2 supportedFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &supportedFeaturesVulkan12
        };
        fi.GetPhysicalDeviceFeatures2(physdev, &supportedFeatures);
        return supportedFeaturesVulkan12.shaderFloat16 == VK_TRUE;
    }

    /// query the device and driver uuids of a physical device
    VkPhysicalDeviceIDProperties queryIDProperties(const VulkanInstanceFuncs& fi,
            VkPhysicalDevice physdev) {
        VkPhysicalDeviceIDProperties idProps{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
        };
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &idProps
        };
        fi.GetPhysicalDeviceProperties2(physdev, &props);
        return idProps;
    }

    /// check if a physical device exposes a given extension
    bool hasDeviceExtension(const VulkanInstanceFuncs& fi, VkPhysicalDevice physdev,
            std::string_view name) {
        uint32_t extCount{};
        auto res = fi.EnumerateDeviceExtensionProperties(physdev, VK_NULL_HANDLE,
            &extCount, VK_NULL_HANDLE);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkEnumerateDeviceExtensionProperties() failed");

        std::vector<VkExtensionProperties> extensions(extCount);
        res = fi.EnumerateDeviceExtensionProperties(physdev, VK_NULL_HANDLE,
            &extCount, extensions.data());
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkEnumerateDeviceExtensionProperties() failed");

        for (const auto& ext : extensions)
            if (std::string_view(ext.extensionName) == name) return true;
        return false;
    }

    template<typename T>
    T dpa(const VulkanInstanceFuncs& funcs, VkDevice device, const char* name) {
        T func = reinterpret_cast<T>(
            funcs.GetDeviceProcAddr(device, name));
        if (!func)
            throw ls::vulkan_error("failed to get device proc addr for " + std::string(name));
        return func;
    }

    /// query the name of a physical device
    std::string queryDeviceName(const VulkanInstanceFuncs& fi, VkPhysicalDevice physdev) {
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        fi.GetPhysicalDeviceProperties2(physdev, &props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay, modernize-return-braced-init-list)
        return std::string(props.properties.deviceName);
    }

    /// create a logical device
    ls::owned_ptr<VkDevice> createLogicalDevice(const VulkanInstanceFuncs& fi,
            VkPhysicalDevice physdev, uint32_t cfi, bool fp16,
            bool enableDmaBufExtensions) {
        VkDevice handle{};

        const float queuePriority{1.0F}; // highest priority
        const VkPhysicalDeviceVulkan12Features requestedFeaturesVulkan12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .shaderFloat16 = fp16,
            .timelineSemaphore = VK_TRUE
        };
        const VkDeviceQueueCreateInfo requestedQueueInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = cfi,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority
        };
        std::vector<const char*> requestedExtensions{
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME
        };
        if (enableDmaBufExtensions) {
            // two-stage policy: capability is requested best-effort here
            // (silent skip keeps same-device users on drivers without these
            // extensions fully working), while the hard Q4 error fires at
            // openContext only when cross-device exchange is actually needed
            // but unavailable. extensions are init-time capability bits with
            // zero per-frame cost, so over-requesting costs nothing.
            const bool hasDmaBuf = hasDeviceExtension(fi, physdev,
                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
            const bool hasDrmModifier = hasDeviceExtension(fi, physdev,
                VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            if (hasDmaBuf && hasDrmModifier) {
                requestedExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
                requestedExtensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            } else {
                std::cerr << "lsfg-vk: dma-buf extensions unavailable on '"
                    << queryDeviceName(fi, physdev) << "', dual-gpu mode disabled\n";
            }
        }
        std::cerr << "lsfg-vk: enabling device extensions:";
        for (const auto* ext : requestedExtensions)
            std::cerr << ' ' << ext;
        std::cerr << '\n';
        const VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &requestedFeaturesVulkan12,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &requestedQueueInfo,
            .enabledExtensionCount = static_cast<uint32_t>(requestedExtensions.size()),
            .ppEnabledExtensionNames = requestedExtensions.data()
        };
        auto res = fi.CreateDevice(physdev, &deviceInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateDevice() failed");

        auto defunc =
            dpa<PFN_vkDestroyDevice>(fi, handle, "vkDestroyDevice");
        if (!defunc)
            throw ls::vulkan_error("failed to get vkDestroyDevice symbol");
        return ls::owned_ptr<VkDevice>(
            new VkDevice(handle),
            [defunc](VkDevice& device) {
                defunc(device, VK_NULL_HANDLE);
            }
        );
    }

    /// get a queue from the logical device
    VkQueue getQueue(const VulkanDeviceFuncs& fd, VkDevice device,
            std::optional<PFN_vkSetDeviceLoaderData> setLoaderData,
            uint32_t cfi) {
        VkQueue queue{};

        fd.GetDeviceQueue(device, cfi, 0, &queue);

        if (setLoaderData) { // optionally set loader data
            auto res = (*setLoaderData)(device, queue);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkSetDeviceLoaderData() failed");
        }
        return queue;
    }

    /// create a command pool
    ls::owned_ptr<VkCommandPool> createCommandPool(const VulkanDeviceFuncs& fd,
            VkDevice device, uint32_t cfi) {
        VkCommandPool handle{};

        const VkCommandPoolCreateInfo cmdpoolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = cfi
        };
        auto res = fd.CreateCommandPool(device, &cmdpoolInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateCommandPool() failed");

        return ls::owned_ptr<VkCommandPool>(
            new VkCommandPool(handle),
            [dev = device, defunc = fd.DestroyCommandPool](VkCommandPool& pool) {
                defunc(dev, pool, VK_NULL_HANDLE);
            }
        );
    }

    /// try to read the pipeline cache from file
    void readCacheFile(const std::filesystem::path& cachefile, std::vector<uint8_t>& data) {
        std::ifstream file(cachefile, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return;

        const std::streamsize size = static_cast<std::streamsize>(file.tellg());
        data = std::vector<uint8_t>(static_cast<size_t>(size));

        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(data.data()), size))
            return;
    }

    /// create a pipeline cache
    ls::owned_ptr<VkPipelineCache> createPipelineCache(
            const VulkanDeviceFuncs& fd, VkDevice device,
            const std::optional<std::filesystem::path>& cachefile) {
        VkPipelineCache handle{};

        std::vector<uint8_t> cache{};
        if (cachefile && std::filesystem::exists(*cachefile))
            readCacheFile(*cachefile, cache);

        const VkPipelineCacheCreateInfo pipelineCacheInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = cache.size(),
            .pInitialData = cache.data()
        };
        auto res = fd.CreatePipelineCache(device, &pipelineCacheInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreatePipelineCache() failed");

        return ls::owned_ptr<VkPipelineCache>(
            new VkPipelineCache(handle),
            [dev = device, defunc = fd.DestroyPipelineCache](VkPipelineCache& cache) {
                defunc(dev, cache, VK_NULL_HANDLE);
            }
        );
    }
}

/// initialize vulkan instance function pointers
VulkanInstanceFuncs vk::initVulkanInstanceFuncs(VkInstance i, PFN_vkGetInstanceProcAddr mpa,
        bool graphical) {
    return {
        .DestroyInstance = ipa<PFN_vkDestroyInstance>(mpa, i, "vkDestroyInstance"),
        .EnumeratePhysicalDevices = ipa<PFN_vkEnumeratePhysicalDevices>(mpa, i,
            "vkEnumeratePhysicalDevices"),
        .EnumerateDeviceExtensionProperties = ipa<PFN_vkEnumerateDeviceExtensionProperties>(mpa, i,
            "vkEnumerateDeviceExtensionProperties"),
        .GetPhysicalDeviceProperties2 = ipa<PFN_vkGetPhysicalDeviceProperties2>(mpa, i,
            "vkGetPhysicalDeviceProperties2"),
        .GetPhysicalDeviceFormatProperties2 =
            ipa<PFN_vkGetPhysicalDeviceFormatProperties2>(mpa, i,
                "vkGetPhysicalDeviceFormatProperties2"),
        .GetPhysicalDeviceExternalSemaphoreProperties =
            ipa<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(mpa, i,
                "vkGetPhysicalDeviceExternalSemaphoreProperties"),
        .GetPhysicalDeviceExternalFenceProperties =
            ipa<PFN_vkGetPhysicalDeviceExternalFenceProperties>(mpa, i,
                "vkGetPhysicalDeviceExternalFenceProperties"),
        .GetPhysicalDeviceQueueFamilyProperties =
            ipa<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(mpa, i,
                "vkGetPhysicalDeviceQueueFamilyProperties"),
        .GetPhysicalDeviceFeatures2 = graphical ?
            nullptr : ipa<PFN_vkGetPhysicalDeviceFeatures2>(mpa, i, "vkGetPhysicalDeviceFeatures2"),
        .GetPhysicalDeviceMemoryProperties = ipa<PFN_vkGetPhysicalDeviceMemoryProperties>(mpa, i,
            "vkGetPhysicalDeviceMemoryProperties"),
        .CreateDevice = ipa<PFN_vkCreateDevice>(mpa, i, "vkCreateDevice"),
        .GetDeviceProcAddr = ipa<PFN_vkGetDeviceProcAddr>(mpa, i, "vkGetDeviceProcAddr"),

        .GetPhysicalDeviceSurfaceCapabilitiesKHR = graphical ?
            ipa<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(mpa, i,
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") : nullptr
    };
}

/// initialize vulkan device function pointers
VulkanDeviceFuncs vk::initVulkanDeviceFuncs(const VulkanInstanceFuncs& f, VkDevice d,
        bool graphical) {
    return {
        .GetDeviceQueue = dpa<PFN_vkGetDeviceQueue>(f, d, "vkGetDeviceQueue"),
        .DeviceWaitIdle = dpa<PFN_vkDeviceWaitIdle>(f, d, "vkDeviceWaitIdle"),
        .CreateCommandPool = dpa<PFN_vkCreateCommandPool>(f, d, "vkCreateCommandPool"),
        .DestroyCommandPool = dpa<PFN_vkDestroyCommandPool>(f, d, "vkDestroyCommandPool"),
        .CreateDescriptorPool = dpa<PFN_vkCreateDescriptorPool>(f, d, "vkCreateDescriptorPool"),
        .DestroyDescriptorPool = dpa<PFN_vkDestroyDescriptorPool>(f, d, "vkDestroyDescriptorPool"),
        .CreateBuffer = dpa<PFN_vkCreateBuffer>(f, d, "vkCreateBuffer"),
        .DestroyBuffer = dpa<PFN_vkDestroyBuffer>(f, d, "vkDestroyBuffer"),
        .GetBufferMemoryRequirements = dpa<PFN_vkGetBufferMemoryRequirements>(f, d,
            "vkGetBufferMemoryRequirements"),
        .AllocateMemory = dpa<PFN_vkAllocateMemory>(f, d, "vkAllocateMemory"),
        .FreeMemory = dpa<PFN_vkFreeMemory>(f, d, "vkFreeMemory"),
        .BindBufferMemory = dpa<PFN_vkBindBufferMemory>(f, d, "vkBindBufferMemory"),
        .MapMemory = dpa<PFN_vkMapMemory>(f, d, "vkMapMemory"),
        .UnmapMemory = dpa<PFN_vkUnmapMemory>(f, d, "vkUnmapMemory"),
        .AllocateCommandBuffers = dpa<PFN_vkAllocateCommandBuffers>(f, d,
            "vkAllocateCommandBuffers"),
        .FreeCommandBuffers = dpa<PFN_vkFreeCommandBuffers>(f, d, "vkFreeCommandBuffers"),
        .BeginCommandBuffer = dpa<PFN_vkBeginCommandBuffer>(f, d, "vkBeginCommandBuffer"),
        .EndCommandBuffer = dpa<PFN_vkEndCommandBuffer>(f, d, "vkEndCommandBuffer"),
        .CmdPipelineBarrier = dpa<PFN_vkCmdPipelineBarrier>(f, d, "vkCmdPipelineBarrier"),
        .CmdBlitImage = dpa<PFN_vkCmdBlitImage>(f, d, "vkCmdBlitImage"),
        .CmdClearColorImage = dpa<PFN_vkCmdClearColorImage>(f, d, "vkCmdClearColorImage"),
        .CmdBindPipeline = dpa<PFN_vkCmdBindPipeline>(f, d, "vkCmdBindPipeline"),
        .CmdBindDescriptorSets = dpa<PFN_vkCmdBindDescriptorSets>(f, d, "vkCmdBindDescriptorSets"),
        .CmdDispatch = dpa<PFN_vkCmdDispatch>(f, d, "vkCmdDispatch"),
        .CmdCopyBufferToImage = dpa<PFN_vkCmdCopyBufferToImage>(f, d, "vkCmdCopyBufferToImage"),
        .QueueSubmit = dpa<PFN_vkQueueSubmit>(f, d, "vkQueueSubmit"),
        .AllocateDescriptorSets = dpa<PFN_vkAllocateDescriptorSets>(f, d,
            "vkAllocateDescriptorSets"),
        .FreeDescriptorSets = dpa<PFN_vkFreeDescriptorSets>(f, d, "vkFreeDescriptorSets"),
        .UpdateDescriptorSets = dpa<PFN_vkUpdateDescriptorSets>(f, d, "vkUpdateDescriptorSets"),
        .CreateFence = dpa<PFN_vkCreateFence>(f, d, "vkCreateFence"),
        .DestroyFence = dpa<PFN_vkDestroyFence>(f, d, "vkDestroyFence"),
        .ResetFences = dpa<PFN_vkResetFences>(f, d, "vkResetFences"),
        .WaitForFences = dpa<PFN_vkWaitForFences>(f, d, "vkWaitForFences"),
        .CreateImage = dpa<PFN_vkCreateImage>(f, d, "vkCreateImage"),
        .DestroyImage = dpa<PFN_vkDestroyImage>(f, d, "vkDestroyImage"),
        .GetImageMemoryRequirements = dpa<PFN_vkGetImageMemoryRequirements>(f, d,
            "vkGetImageMemoryRequirements"),
        .BindImageMemory = dpa<PFN_vkBindImageMemory>(f, d, "vkBindImageMemory"),
        .CreateImageView = dpa<PFN_vkCreateImageView>(f, d, "vkCreateImageView"),
        .DestroyImageView = dpa<PFN_vkDestroyImageView>(f, d, "vkDestroyImageView"),
        .CreateSampler = dpa<PFN_vkCreateSampler>(f, d, "vkCreateSampler"),
        .DestroySampler = dpa<PFN_vkDestroySampler>(f, d, "vkDestroySampler"),
        .CreateSemaphore = dpa<PFN_vkCreateSemaphore>(f, d, "vkCreateSemaphore"),
        .DestroySemaphore = dpa<PFN_vkDestroySemaphore>(f, d, "vkDestroySemaphore"),
        .CreateShaderModule = dpa<PFN_vkCreateShaderModule>(f, d, "vkCreateShaderModule"),
        .DestroyShaderModule = dpa<PFN_vkDestroyShaderModule>(f, d, "vkDestroyShaderModule"),
        .CreateDescriptorSetLayout = dpa<PFN_vkCreateDescriptorSetLayout>(f, d,
            "vkCreateDescriptorSetLayout"),
        .DestroyDescriptorSetLayout = dpa<PFN_vkDestroyDescriptorSetLayout>(f, d,
            "vkDestroyDescriptorSetLayout"),
        .CreatePipelineLayout = dpa<PFN_vkCreatePipelineLayout>(f, d, "vkCreatePipelineLayout"),
        .DestroyPipelineLayout = dpa<PFN_vkDestroyPipelineLayout>(f, d, "vkDestroyPipelineLayout"),
        .CreatePipelineCache = dpa<PFN_vkCreatePipelineCache>(f, d, "vkCreatePipelineCache"),
        .DestroyPipelineCache = dpa<PFN_vkDestroyPipelineCache>(f, d, "vkDestroyPipelineCache"),
        .GetPipelineCacheData = dpa<PFN_vkGetPipelineCacheData>(f, d, "vkGetPipelineCacheData"),
        .CreateComputePipelines = dpa<PFN_vkCreateComputePipelines>(f, d, "vkCreateComputePipelines"),
        .DestroyPipeline = dpa<PFN_vkDestroyPipeline>(f, d, "vkDestroyPipeline"),
        .GetImageSubresourceLayout = dpa<PFN_vkGetImageSubresourceLayout>(f, d,
            "vkGetImageSubresourceLayout"),

        .SignalSemaphoreKHR = dpa<PFN_vkSignalSemaphoreKHR>(f, d, "vkSignalSemaphoreKHR"),
        .WaitSemaphoresKHR = dpa<PFN_vkWaitSemaphoresKHR>(f, d, "vkWaitSemaphoresKHR"),
        .GetMemoryFdKHR = dpa<PFN_vkGetMemoryFdKHR>(f, d, "vkGetMemoryFdKHR"),
        .GetMemoryFdPropertiesKHR = dpa<PFN_vkGetMemoryFdPropertiesKHR>(f, d,
            "vkGetMemoryFdPropertiesKHR"),
        .ImportSemaphoreFdKHR = dpa<PFN_vkImportSemaphoreFdKHR>(f, d, "vkImportSemaphoreFdKHR"),
        .GetSemaphoreFdKHR = dpa<PFN_vkGetSemaphoreFdKHR>(f, d, "vkGetSemaphoreFdKHR"),

        .CreateSwapchainKHR = graphical ?
            dpa<PFN_vkCreateSwapchainKHR>(f, d, "vkCreateSwapchainKHR") : nullptr,
        .GetSwapchainImagesKHR = graphical ?
            dpa<PFN_vkGetSwapchainImagesKHR>(f, d, "vkGetSwapchainImagesKHR") : nullptr,
        .AcquireNextImageKHR = graphical ?
            dpa<PFN_vkAcquireNextImageKHR>(f, d, "vkAcquireNextImageKHR") : nullptr,
        .QueuePresentKHR = graphical ?
            dpa<PFN_vkQueuePresentKHR>(f, d, "vkQueuePresentKHR") : nullptr,
        .DestroySwapchainKHR = graphical ?
            dpa<PFN_vkDestroySwapchainKHR>(f, d, "vkDestroySwapchainKHR") : nullptr
    };
}

Vulkan::Vulkan(const std::string& appName, version appVersion,
        const std::string& engineName, version engineVersion,
        PhysicalDeviceSelector selectPhysicalDevice,
        bool isGraphical,
        std::optional<PFN_vkSetDeviceLoaderData> setLoaderData,
        const std::optional<std::filesystem::path>& cachefile,
        bool enableDmaBufExtensions) :
    instance(createInstance(
        appName, appVersion,
        engineName, engineVersion
    )),
    instance_funcs(initVulkanInstanceFuncs(*this->instance, get_mpa(), false)),
    phys_dev(findPhysicalDevice(this->instance_funcs,
        *this->instance,
        selectPhysicalDevice
    )),
    queueFamilyIdx(findQFI(this->instance_funcs, this->phys_dev,
        isGraphical ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT)),
    fp16(checkFP16(this->instance_funcs, this->phys_dev)),
    device(createLogicalDevice(this->instance_funcs,
        this->phys_dev,
        this->queueFamilyIdx,
        this->fp16,
        enableDmaBufExtensions
    )),
    setLoaderData(setLoaderData),
    device_funcs(initVulkanDeviceFuncs(
        this->instance_funcs,
        *this->device, false
    )),
    computeQueue(getQueue(this->device_funcs, *this->device,
        this->setLoaderData,
        this->queueFamilyIdx)),
    cmdPool(createCommandPool(this->device_funcs,
        *this->device,
        this->queueFamilyIdx
    )),
    pipelineCache(createPipelineCache(this->device_funcs,
        *this->device, cachefile
    )),
    cachefile(cachefile) {
}

Vulkan::Vulkan(VkInstance instance, VkDevice device,
        VkPhysicalDevice physdev,
        VulkanInstanceFuncs instanceFuncs,
        VulkanDeviceFuncs deviceFuncs,
        bool isGraphical,
        std::optional<PFN_vkSetDeviceLoaderData> setLoaderData,
        const std::optional<std::filesystem::path>& cachefile) :
    instance(new VkInstance(instance)),
    instance_funcs(instanceFuncs),
    phys_dev(physdev),
    queueFamilyIdx(findQFI(this->instance_funcs, this->phys_dev,
        isGraphical ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT)),
    fp16(false),
    device(new VkDevice(device)),
    setLoaderData(setLoaderData),
    device_funcs(deviceFuncs),
    computeQueue(getQueue(this->device_funcs, *this->device,
        this->setLoaderData,
        this->queueFamilyIdx)),
    cmdPool(createCommandPool(this->device_funcs,
        *this->device,
        this->queueFamilyIdx
    )),
    pipelineCache(createPipelineCache(this->device_funcs,
        *this->device, cachefile
    )),
    cachefile(cachefile) {
}

std::optional<uint32_t> Vulkan::findMemoryTypeIndex(
        std::bitset<32> validTypes, bool hostVisibility) const {
    const VkMemoryPropertyFlags desiredProps = hostVisibility ?
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkPhysicalDeviceMemoryProperties props;
    this->instance_funcs.GetPhysicalDeviceMemoryProperties(this->phys_dev, &props);

    std::array<VkMemoryType, 32> memTypes = std::to_array(props.memoryTypes);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if (validTypes.test(i) && (memTypes.at(i).propertyFlags & desiredProps) == desiredProps)
            return i;

    return std::nullopt;
}

void Vulkan::persistPipelineCache() const noexcept {
    if (!this->cachefile)
        return;

    size_t cacheSize{};
    auto res = this->device_funcs.GetPipelineCacheData(*this->device,
        *this->pipelineCache,
        &cacheSize, nullptr);
    if (res != VK_SUCCESS)
        return;

    std::vector<uint8_t> cacheData(cacheSize);
    res = this->device_funcs.GetPipelineCacheData(*this->device,
        *this->pipelineCache,
        &cacheSize, cacheData.data());
    if (res != VK_SUCCESS)
        return;

    std::ofstream file(*this->cachefile, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return;

    file.write(reinterpret_cast<const char*>(cacheData.data()),
        static_cast<std::streamsize>(cacheData.size()));
    if (!file.good())
        return;
}

std::array<uint8_t, 16> Vulkan::deviceUUID() const {
    return std::to_array(queryIDProperties(this->instance_funcs, this->phys_dev).deviceUUID);
}

std::array<uint8_t, 16> Vulkan::driverUUID() const {
    return std::to_array(queryIDProperties(this->instance_funcs, this->phys_dev).driverUUID);
}

bool Vulkan::supportsDmaBuf() const {
    return hasDeviceExtension(this->instance_funcs, this->phys_dev,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
}

bool Vulkan::supportsDrmModifierImages() const {
    return hasDeviceExtension(this->instance_funcs, this->phys_dev,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

DeviceExchangeCaps Vulkan::exchangeCaps(VkFormat format) const {
    VkDrmFormatModifierPropertiesListEXT modList{
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
    };
    VkFormatProperties2 props{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modList
    };
    this->instance_funcs.GetPhysicalDeviceFormatProperties2(this->phys_dev, format, &props);

    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(
        modList.drmFormatModifierCount);
    modList.pDrmFormatModifierProperties = modifiers.data();
    this->instance_funcs.GetPhysicalDeviceFormatProperties2(this->phys_dev, format, &props);

    std::vector<ExchangeModifierCaps> caps{};
    caps.reserve(modifiers.size());
    for (const auto& mod : modifiers)
        caps.push_back(ExchangeModifierCaps{
            .modifier = mod.drmFormatModifier,
            .requiredUsageBits = mod.drmFormatModifierTilingFeatures
        });

    return {{format, std::move(caps)}};
}

bool Vulkan::supportsSyncFdSemaphoreExportImport() const {
    const VkPhysicalDeviceExternalSemaphoreInfo info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
    };
    VkExternalSemaphoreProperties props{};
    this->instance_funcs.GetPhysicalDeviceExternalSemaphoreProperties(this->phys_dev,
        &info, &props);
    return (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)
        && (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
}

bool Vulkan::supportsSyncFdFenceExportImport() const {
    const VkPhysicalDeviceExternalFenceInfo info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    };
    VkExternalFenceProperties props{};
    this->instance_funcs.GetPhysicalDeviceExternalFenceProperties(this->phys_dev,
        &info, &props);
    return (props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)
        && (props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT);
}
