/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "extraction/dll_reader.hpp"
#include "extraction/shader_registry.hpp"
#include "helpers/limits.hpp"
#include "helpers/utils.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "shaderchains/alpha0.hpp"
#include "shaderchains/alpha1.hpp"
#include "shaderchains/beta0.hpp"
#include "shaderchains/beta1.hpp"
#include "shaderchains/delta0.hpp"
#include "shaderchains/delta1.hpp"
#include "shaderchains/gamma0.hpp"
#include "shaderchains/gamma1.hpp"
#include "shaderchains/generate.hpp"
#include "shaderchains/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan_core.h>

#ifdef LSFGVK_TESTING_RENDERDOC
#include <renderdoc_app.h>
#include <dlfcn.h>
#endif

using namespace lsfgvk;
using namespace lsfgvk::backend;

namespace lsfgvk::backend {
    error::error(const std::string& msg, const std::exception& inner)
        : std::runtime_error(msg + "\n- " + inner.what()) {}
    error::error(const std::string& msg)
        : std::runtime_error(msg) {}
    error::~error() = default;

    /// instance class
    class InstanceImpl {
    public:
        /// create an instance
        /// (see lsfg-vk documentation)
        InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision, bool enableDmaBufExtensions);

        /// get the Vulkan instance
        /// @return the Vulkan instance
        [[nodiscard]] const auto& getVulkan() const { return this->vk; }
        /// get the shader registry
        /// @return the shader registry
        [[nodiscard]] const auto& getShaderRegistry() const { return this->shaders; }
        /// get the device uuid of the selected physical device
        /// @return the 16-byte device uuid
        [[nodiscard]] std::array<uint8_t, 16> selectedDeviceUUID() const {
            return this->vk.deviceUUID();
        }
        /// check whether the selected physical device supports dma-buf external memory
        /// @return true if dma buf imports/exports are supported
        [[nodiscard]] bool selectedDeviceSupportsDmaBuf() const {
            return this->vk.supportsDmaBuf();
        }
        /// check whether the selected physical device supports drm modifier images
        /// @return true if drm modifier images are supported
        [[nodiscard]] bool selectedDeviceSupportsDrmModifierImages() const {
            return this->vk.supportsDrmModifierImages();
        }
        /// check whether this instance enables the dma-buf exchange extensions
        /// @return true if contexts may ingest dma-buf descriptors / run cross-device
        [[nodiscard]] bool dmaBufExtensionsEnabled() const {
            return this->vk.supportsDmaBuf() && this->vk.supportsDrmModifierImages();
        }
#ifdef LSFGVK_TESTING_RENDERDOC
        /// get the RenderDoc API
        /// @return the RenderDoc API
        [[nodiscard]] const auto& getRenderDocAPI() const { return this->renderdoc; }
#endif
        // Movable, non-copyable, custom destructor
        InstanceImpl(const InstanceImpl&) = delete;
        InstanceImpl& operator=(const InstanceImpl&) = delete;
        InstanceImpl(InstanceImpl&&) = default;
        InstanceImpl& operator=(InstanceImpl&&) = default;
        ~InstanceImpl();
    private:
        vk::Vulkan vk;
        ShaderRegistry shaders;

#ifdef LSFGVK_TESTING_RENDERDOC
        std::optional<RENDERDOC_API_1_6_0> renderdoc;
#endif
    };

    /// context class
    class ContextImpl {
    public:
        /// create a context
        /// (see lsfg-vk documentation)
        ContextImpl(const InstanceImpl& instance,
            std::span<const vk::ExchangeDescriptor> sources,
            std::span<const vk::ExchangeDescriptor> dests,
            std::array<uint8_t, 16> exporterDeviceUUID,
            int syncFd, VkExtent2D extent, bool hdr, float flow, bool perf);

        /// schedule frames
        /// (see lsfg-vk documentation)
        std::vector<int> scheduleFrames(int captureReadyFd = -1);

        /// check whether this context synchronizes across devices
        /// @return true if the context runs in cross-device mode
        [[nodiscard]] bool isCrossDevice() const { return this->crossDevice; }
    private:
        /// schedule frames in cross-device mode via sync-fd handshakes
        /// @param captureReadyFd sync fd signaled on capture completion (owned)
        /// @return per-generated-frame done sync fds
        std::vector<int> scheduleFramesCross(int captureReadyFd);

        std::pair<vk::Image, vk::Image> sourceImages;
        std::vector<vk::Image> destImages;
        vk::Image blackImage;

        std::optional<vk::TimelineSemaphore> syncSemaphore; // imported (same-device only)
        vk::TimelineSemaphore prepassSemaphore;
        bool crossDevice{false}; // exporter uuid != selected device uuid
        // cross-device handshake semaphores are FRESH PER CYCLE and are never
        // waited locally after an export: on this rig's RADV, a binary
        // semaphore that was exportFd()'d deadlocks any later local wait
        // (empirical law, todo-15/17 probes). retired generations are kept
        // one extra cycle and destroyed behind the cmdbuf fence gate, since
        // destroying while their signal/wait batches are still pending trips
        // VUID-vkDestroySemaphore-semaphore-05149 under validation
        std::optional<vk::Semaphore> captureWait; // consumed by this cycle's pre-pass submit
        std::optional<vk::Semaphore> retiredCaptureWait; // last cycle's; destroyed next entry
        std::vector<vk::Semaphore> doneSignals; // signaled by this cycle's main passes
        std::vector<vk::Semaphore> retiredDoneSignals; // last cycle's; destroyed next entry
        size_t idx{1};
        size_t fidx{0}; // real frame index

        std::vector<vk::CommandBuffer> cmdbufs;
        vk::Fence cmdbufFence;

        Ctx ctx;

        Mipmaps mipmaps;
        std::array<Alpha0, 7> alpha0;
        std::array<Alpha1, 7> alpha1;
        Beta0 beta0;
        Beta1 beta1;
        struct Pass {
            std::vector<Gamma0> gamma0;
            std::vector<Gamma1> gamma1;

            std::vector<Delta0> delta0;
            std::vector<Delta1> delta1;
            ls::lazy<Generate> generate;
        };
        std::vector<Pass> passes;
    };
}

Instance::Instance(
        const DevicePicker& devicePicker,
        const std::filesystem::path& shaderDllPath,
        bool allowLowPrecision, bool enableDmaBufExtensions) {
    const auto selectFunc = [&devicePicker](const vk::VulkanInstanceFuncs funcs,
            const std::vector<VkPhysicalDevice>& devices) {
        for (const auto& device : devices) {
            // check if the physical device supports VK_EXT_pci_bus_info
            uint32_t ext_count{};
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, VK_NULL_HANDLE);

            std::vector<VkExtensionProperties> extensions(ext_count);
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, extensions.data());

            const bool has_pci_ext = std::ranges::find_if(extensions,
                [](const VkExtensionProperties& ext) {
                    return std::string(std::to_array(ext.extensionName).data())
                        == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME;
                }) != extensions.end();

            // then fetch all available properties
            VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT
            };
            VkPhysicalDeviceProperties2 props{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = has_pci_ext ? &pciInfo : nullptr
            };
            funcs.GetPhysicalDeviceProperties2(device, &props);

            std::array<char, 256> devname = std::to_array(props.properties.deviceName);
            devname.at(255) = '\0'; // ensure null-termination

            if (devicePicker(
                std::string(devname.data()),
                { backend::to_hex_id(props.properties.vendorID),
                  backend::to_hex_id(props.properties.deviceID) },
                has_pci_ext ? std::optional<std::string>{
                    std::to_string(pciInfo.pciBus) + ":" +
                    std::to_string(pciInfo.pciDevice) + "." +
                    std::to_string(pciInfo.pciFunction)
                } : std::nullopt
            ))
                return device;
        }

        throw ls::vulkan_error("no suitable physical device found");
    };

    this->m_impl = std::make_unique<InstanceImpl>(
        selectFunc, shaderDllPath, allowLowPrecision, enableDmaBufExtensions
    );
}

namespace {
    /// find the cache file path for a given driver uuid
    std::filesystem::path findCacheFilePath(std::array<uint8_t, 16> driverUuid) {
        constexpr std::array<char, 16> hexDigits{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string filename{"lsfg-vk_pipeline_cache_"};
        for (const auto& byte : driverUuid) {
            filename += hexDigits.at(byte >> 4);
            filename += hexDigits.at(byte & 0xF);
        }
        filename += ".bin";

        std::filesystem::path dir{};
        const char* xdgCacheHome = std::getenv("XDG_CACHE_HOME");
        if (xdgCacheHome && *xdgCacheHome != '\0') {
            dir = std::filesystem::path(xdgCacheHome);
        } else {
            const char* home = std::getenv("HOME");
            if (home && *home != '\0')
                dir = std::filesystem::path(home) / ".cache";
            else
                dir = "/tmp";
        }

        // best-effort prune of the legacy unkeyed cache file; errors (e.g. enoent) are intentional to ignore
        std::error_code ec{};
        std::filesystem::remove(dir / "lsfg-vk_pipeline_cache.bin", ec);

        return dir / filename;
    }
    /// create a Vulkan instance
    vk::Vulkan createVulkanInstance(vk::PhysicalDeviceSelector selectPhysicalDevice,
            bool enableDmaBufExtensions) {
        try {
            // the cache path is keyed by the selected device's driver uuid; resolve it
            // during device selection. vk::Vulkan initializes phys_dev before the
            // pipeline cache members ([class.base.init] declaration order), so the
            // assignment below is observed when the cache is created/persisted.
            std::optional<std::filesystem::path> cachefile{};
            const auto cacheKeyedSelector =
                [&selectPhysicalDevice, &cachefile](
                        const vk::VulkanInstanceFuncs& funcs,
                        const std::vector<VkPhysicalDevice>& devices) {
                    auto* const device = selectPhysicalDevice(funcs, devices);

                    VkPhysicalDeviceIDProperties idProps{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
                    };
                    VkPhysicalDeviceProperties2 props{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                        .pNext = &idProps
                    };
                    funcs.GetPhysicalDeviceProperties2(device, &props);

                    cachefile = findCacheFilePath(std::to_array(idProps.driverUUID));
                    return device;
                };

            return{
                "lsfg-vk", vk::version{2, 0, 0},
                "lsfg-vk-engine", vk::version{2, 0, 0},
                cacheKeyedSelector,
                false, std::nullopt,
                cachefile, enableDmaBufExtensions
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to initialize Vulkan", e);
        }
    }
    /// build a shader registry
    ShaderRegistry createShaderRegistry(vk::Vulkan& vk,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision) {
        std::unordered_map<uint32_t, std::vector<uint8_t>> resources{};

        try {
            resources = backend::extractResourcesFromDLL(shaderDllPath);
        } catch (const std::exception& e) {
            throw backend::error("Unable to parse Lossless Scaling DLL", e);
        }

        try {
            return backend::buildShaderRegistry(
                vk, allowLowPrecision && vk.supportsFP16(),
                resources
            );
        } catch (const std::exception& e) {
            throw backend::error("Unable to build shader registry", e);
        }
    }
#ifdef LSFGVK_TESTING_RENDERDOC
    /// load RenderDoc integration
    std::optional<RENDERDOC_API_1_6_0> loadRenderDocIntegration() {
        void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (!module)
            return std::nullopt;

        auto renderdocGetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(
            dlsym(module, "RENDERDOC_GetAPI"));
        if (!renderdocGetAPI)
            return std::nullopt;

        RENDERDOC_API_1_6_0* api{};
        renderdocGetAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api));
        if (!api)
            return std::nullopt;

        return *api;
    }
#endif
}

namespace {
    /// format a 16-byte uuid as a hex string
    std::string uuidToHex(const std::array<uint8_t, 16>& uuid) {
        constexpr std::array<char, 16> hexDigits{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string out{};
        out.reserve(2 * uuid.size());
        for (const auto& byte : uuid) {
            out += hexDigits.at(byte >> 4);
            out += hexDigits.at(byte & 0xF);
        }
        return out;
    }
    /// validate exchange descriptors against the context parameters.
    /// runs BEFORE any image import so that validation failures leave all
    /// descriptor fds untouched (ownership stays with the caller).
    void validateExchangeDescriptors(const InstanceImpl& instance,
            std::span<const vk::ExchangeDescriptor> sources,
            std::span<const vk::ExchangeDescriptor> dests,
            std::array<uint8_t, 16> exporterDeviceUUID,
            uint64_t negotiatedModifier,
            VkExtent2D extent, VkFormat format) {
        if (sources.size() != 2)
            throw backend::error("context requires exactly 2 source descriptors, got "
                + std::to_string(sources.size()));
        if (dests.empty())
            throw backend::error("context requires at least one destination descriptor");

        const bool crossDevice =
            exporterDeviceUUID != instance.getVulkan().deviceUUID();
        const bool needsDmaBuf =
            crossDevice || negotiatedModifier != EXCHANGE_MODIFIER_OPAQUE;
        // capability-based check (not the requested-gate flag): with best-effort
        // gating the extensions are enabled iff the device supports them, so the
        // todo-1 probes are the ground truth for what this instance can do
        const bool dmaBufCapable =
            instance.getVulkan().supportsDmaBuf()
            && instance.getVulkan().supportsDrmModifierImages();
        if (needsDmaBuf && !dmaBufCapable)
            throw backend::error(
                "dma-buf exchange required (exporter uuid " + uuidToHex(exporterDeviceUUID)
                + ", selected uuid " + uuidToHex(instance.selectedDeviceUUID())
                + ") but the selected device does not support the required"
                " VK_EXT_external_memory_dma_buf / VK_EXT_image_drm_format_modifier"
                " extensions");

        const auto checkDescriptor = [&](const vk::ExchangeDescriptor& desc,
                const char* kind, size_t index) {
            if (desc.extent.width != extent.width || desc.extent.height != extent.height)
                throw backend::error(std::string(kind) + " descriptor " +
                    std::to_string(index) + " extent " +
                    std::to_string(desc.extent.width) + "x" +
                    std::to_string(desc.extent.height) +
                    " does not match context extent " +
                    std::to_string(extent.width) + "x" +
                    std::to_string(extent.height));
            if (desc.format != format)
                throw backend::error(std::string(kind) + " descriptor " +
                    std::to_string(index) + " format " +
                    std::to_string(desc.format) +
                    " does not match context format " +
                    std::to_string(format));
            if (desc.modifier != negotiatedModifier)
                throw backend::error(std::string(kind) + " descriptor " +
                    std::to_string(index) + " modifier " +
                    std::to_string(desc.modifier) +
                    " does not match negotiated modifier " +
                    std::to_string(negotiatedModifier));
            if (desc.modifier != EXCHANGE_MODIFIER_OPAQUE && desc.rowPitch == 0)
                throw backend::error(std::string(kind) + " descriptor " +
                    std::to_string(index) +
                    " uses drm-modifier tiling and requires a non-zero row pitch");
        };

        for (size_t i = 0; i < sources.size(); ++i)
            checkDescriptor(  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                sources[i], "source", i);
        for (size_t i = 0; i < dests.size(); ++i)
            checkDescriptor(  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                dests[i], "destination", i);
    }
}

InstanceImpl::InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision, bool enableDmaBufExtensions)
        : vk(createVulkanInstance(selectPhysicalDevice, enableDmaBufExtensions)),
        shaders(createShaderRegistry(this->vk, shaderDllPath,
            allowLowPrecision && vk.supportsFP16())) {
#ifdef LSFGVK_TESTING_RENDERDOC
    this->renderdoc = loadRenderDocIntegration();
#endif
    vk.persistPipelineCache(); // will silently fail

    // log the selected device identity and dma-buf capability for dual-gpu diagnostics
    VkPhysicalDeviceProperties2 props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
    };
    this->vk.fi().GetPhysicalDeviceProperties2(this->vk.physdev(), &props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    std::cerr << "lsfg-vk: processing on '" << props.properties.deviceName << "'"
        << " [uuid " << uuidToHex(this->selectedDeviceUUID()) << "]"
        << ", dma-buf: "
        << (this->selectedDeviceSupportsDmaBuf() ? "yes" : "no")
        << ", drm-modifier-images: "
        << (this->selectedDeviceSupportsDrmModifierImages() ? "yes" : "no")
        << '\n';
}

Context& Instance::openContext(
        std::span<const vk::ExchangeDescriptor> sources,
        std::span<const vk::ExchangeDescriptor> dests,
        std::array<uint8_t, 16> exporterDeviceUUID,
        uint64_t negotiatedModifier,
        int syncFd, uint32_t width, uint32_t height,
        bool hdr, float flow, bool perf) {
    // validate before constructing the context: a throw here leaves every
    // descriptor fd untouched, while import failures below follow todo-2's
    // close-on-failure discipline
    validateExchangeDescriptors(*this->m_impl, sources, dests,
        exporterDeviceUUID, negotiatedModifier,
        VkExtent2D{ width, height },
        hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);

    return *this->m_contexts.emplace_back(std::make_unique<ContextImpl>(*this->m_impl,
        sources, dests, exporterDeviceUUID,
        syncFd, VkExtent2D{ width, height }, hdr, flow, perf
    )).get();
}

Context& Instance::openContext(std::pair<int, int> sourceFds, const std::vector<int>& destFds,
        int syncFd, uint32_t width, uint32_t height,
        bool hdr, float flow, bool perf,
        std::optional<std::array<uint8_t, 16>> gameDeviceUUID) {
    const VkExtent2D extent{ width, height };
    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    // wrap the raw fds into opaque-fd-equivalent descriptors; the import path
    // then behaves byte-identically to the pre-descriptor implementation
    const std::array<vk::ExchangeDescriptor, 2> sources{{
        { sourceFds.first, 0, 0, EXCHANGE_MODIFIER_OPAQUE, format, extent },
        { sourceFds.second, 0, 0, EXCHANGE_MODIFIER_OPAQUE, format, extent }
    }};
    std::vector<vk::ExchangeDescriptor> dests{};
    dests.reserve(destFds.size());
    for (const int fd : destFds)
        dests.push_back({ fd, 0, 0, EXCHANGE_MODIFIER_OPAQUE, format, extent });

    return this->openContext(sources, dests,
        gameDeviceUUID.value_or(this->m_impl->selectedDeviceUUID()),
        EXCHANGE_MODIFIER_OPAQUE,
        syncFd, width, height, hdr, flow, perf);
}

std::array<uint8_t, 16> Instance::selectedDeviceUUID() const {
    return this->m_impl->selectedDeviceUUID();
}

bool Instance::selectedDeviceSupportsDmaBuf() const {
    return this->m_impl->selectedDeviceSupportsDmaBuf();
}

bool Instance::selectedDeviceSupportsDrmModifierImages() const {
    return this->m_impl->selectedDeviceSupportsDrmModifierImages();
}

namespace {
    /// import a single exchanged image described by a descriptor.
    /// opaque-sentinel modifiers take the legacy OPAQUE_FD path byte-identically;
    /// LINEAR / drm-modifier descriptors create dma-buf exchange images whose
    /// imported memory type is selected from the fd properties. NOTE(todo-17):
    /// ImageMode::Linear maps to modifier 0 via the DRM_FORMAT_MODIFIER
    /// creation path inside vk::Image (plain-LINEAR + DMA_BUF is unusable on
    /// RADV), so linear imports must carry their real row pitch.
    /// on success the fd is consumed by the import, on failure it is closed
    /// by the importer (todo-2 semantics).
    vk::Image importImage(const vk::Vulkan& vk,
            const vk::ExchangeDescriptor& desc, VkImageUsageFlags usage) {
        if (desc.modifier == EXCHANGE_MODIFIER_OPAQUE)
            return {vk, desc.extent, desc.format, usage, desc.fd};

        const vk::ImageLayout layout{
            .mode = desc.modifier == vk::EXCHANGE_MODIFIER_LINEAR ?
                vk::ImageMode::Linear : vk::ImageMode::DrmModifier,
            .drmModifier = desc.modifier,
            .rowPitch = desc.rowPitch
        };
        return {vk, desc.extent, desc.format, usage, desc.fd,
            std::nullopt, layout};
    }
    /// import source images
    std::pair<vk::Image, vk::Image> importSourceImages(const vk::Vulkan& vk,
            std::span<const vk::ExchangeDescriptor> sources, VkImageUsageFlags usage) {
        try {
            // both descriptors validated above (exactly 2 sources required)
            return {
                importImage(vk, sources[0], usage),  // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                importImage(vk, sources[1], usage)   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to import source images", e);
        }
    }
    /// import destination images
    std::vector<vk::Image> importDestImages(const vk::Vulkan& vk,
            std::span<const vk::ExchangeDescriptor> dests, VkImageUsageFlags usage) {
        try {
            std::vector<vk::Image> destImages;
            destImages.reserve(dests.size());

            for (const auto& desc : dests)
                destImages.emplace_back(importImage(vk, desc, usage));

            return destImages;
        } catch (const std::exception& e) {
            throw backend::error("Unable to import destination images", e);
        }
    }
    /// create a black image
    vk::Image createBlackImage(const vk::Vulkan& vk) {
        try {
            return{vk,
                { .width = 4, .height = 4 }
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create black image", e);
        }
    }
    /// import timeline semaphore
    vk::TimelineSemaphore importTimelineSemaphore(const vk::Vulkan& vk, int syncFd) {
        try {
            return{vk, 0, syncFd};
        } catch (const std::exception& e) {
            throw backend::error("Unable to import timeline semaphore", e);
        }
    }
    /// import a sync fd into a binary semaphore as a temporary payload.
    /// on success the fd is consumed by the implementation, on failure it is
    /// closed here before throwing, so ownership never leaks either way.
    void importSyncFdSemaphore(const vk::Vulkan& vk, VkSemaphore semaphore, int fd) {
        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fd
        };
        const auto res = vk.df().ImportSemaphoreFdKHR(vk.dev(), &importInfo);
        if (res != VK_SUCCESS) {
            close(fd);
            throw ls::vulkan_error(res, "vkImportSemaphoreFdKHR() failed");
        }
    }
    /// create prepass semaphores
    vk::TimelineSemaphore createPrepassSemaphore(const vk::Vulkan& vk) {
        try {
            return{vk, 0};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create prepass semaphore", e);
        }
    }
    /// create a fresh binary semaphore usable as a sync-fd handshake endpoint
    vk::Semaphore createSyncFdSemaphore(const vk::Vulkan& vk) {
        try {
            return {vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create sync-fd semaphore", e);
        }
    }
    /// create command buffers
    std::vector<vk::CommandBuffer> createCommandBuffers(const vk::Vulkan& vk, size_t count) {
        try {
            std::vector<vk::CommandBuffer> cmdbufs;
            cmdbufs.reserve(count);

            for (size_t i = 0; i < count; ++i)
                cmdbufs.emplace_back(vk);

            return cmdbufs;
        } catch (const std::exception& e) {
            throw backend::error("Unable to create command buffers", e);
        }
    }
    /// create context data
    Ctx createCtx(const InstanceImpl& instance, VkExtent2D extent,
            bool hdr, float flow, bool perf, size_t count) {
        const auto& vk = instance.getVulkan();
        const auto& shaders = instance.getShaderRegistry();

        try {
            std::vector<vk::Buffer> constantBuffers{};
            constantBuffers.reserve(count);

            for (size_t i = 0; i < count; ++i)
                constantBuffers.emplace_back(vk,
                    backend::getDefaultConstantBuffer(
                        i, count, flow
                    )
                );

            return {
                .vk = std::ref(vk),
                .shaders = std::ref(shaders),
                .pool{vk, backend::calculateDescriptorPoolLimits(count, perf)},
                .constantBuffer{vk, backend::getDefaultConstantBuffer(0, 1, flow)},
                .constantBuffers{std::move(constantBuffers)},
                .bnbSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false},
                .bnwSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true},
                .eabSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false},
                .sourceExtent = extent,
                .flowExtent = VkExtent2D {
                    .width = static_cast<uint32_t>(static_cast<float>(extent.width) / flow),
                    .height = static_cast<uint32_t>(static_cast<float>(extent.height) / flow)
                },
                .hdr = hdr,
                .flow = flow,
                .perf = perf,
                .count = count
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create context", e);
        }
    }
}

ContextImpl::ContextImpl(const InstanceImpl& instance,
        std::span<const vk::ExchangeDescriptor> sources,
        std::span<const vk::ExchangeDescriptor> dests,
        std::array<uint8_t, 16> exporterDeviceUUID,
        int syncFd, VkExtent2D extent, bool hdr, float flow, bool perf) :
        sourceImages(importSourceImages(instance.getVulkan(), sources,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)),
        destImages(importDestImages(instance.getVulkan(), dests,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)),
        blackImage(createBlackImage(instance.getVulkan())),
        prepassSemaphore(createPrepassSemaphore(instance.getVulkan())),
        crossDevice(exporterDeviceUUID != instance.getVulkan().deviceUUID()),
        cmdbufs(createCommandBuffers(instance.getVulkan(), dests.size() + 1)),
        cmdbufFence(instance.getVulkan()),
        ctx(createCtx(instance, extent, hdr, flow, perf, dests.size())),
        mipmaps(ctx, sourceImages),
        alpha0{
            Alpha0(ctx, mipmaps.getImages().at(0)),
            Alpha0(ctx, mipmaps.getImages().at(1)),
            Alpha0(ctx, mipmaps.getImages().at(2)),
            Alpha0(ctx, mipmaps.getImages().at(3)),
            Alpha0(ctx, mipmaps.getImages().at(4)),
            Alpha0(ctx, mipmaps.getImages().at(5)),
            Alpha0(ctx, mipmaps.getImages().at(6))
        },
        alpha1{
            Alpha1(ctx, 3, alpha0.at(0).getImages()),
            Alpha1(ctx, 2, alpha0.at(1).getImages()),
            Alpha1(ctx, 2, alpha0.at(2).getImages()),
            Alpha1(ctx, 2, alpha0.at(3).getImages()),
            Alpha1(ctx, 2, alpha0.at(4).getImages()),
            Alpha1(ctx, 2, alpha0.at(5).getImages()),
            Alpha1(ctx, 2, alpha0.at(6).getImages())
        },
        beta0(ctx, alpha1.at(0).getImages()),
        beta1(ctx, beta0.getImages()) {
    // cross-device contexts synchronize via fresh-per-cycle sync-fd binary
    // semaphores (created on demand in scheduleFramesCross); same-device
    // contexts keep the imported timeline protocol untouched
    if (!this->crossDevice)
        this->syncSemaphore.emplace(importTimelineSemaphore(instance.getVulkan(), syncFd));

    // build main passes
    for (size_t i = 0; i < destImages.size(); ++i) {
        auto& pass = this->passes.emplace_back();

        pass.gamma0.reserve(7);
        pass.gamma1.reserve(7);
        pass.delta0.reserve(3);
        pass.delta1.reserve(3);
        for (size_t j = 0; j < 7; j++) {
            if (j == 0) { // first pass has no prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    this->blackImage,
                    this->beta1.getImages().at(5)
                );
            } else { // other passes use prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j - 1).getImage(),
                    this->beta1.getImages().at(6 - j)
                );
            }

            if (j == 4) { // first special pass has no prior data
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage,
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    this->blackImage,
                    this->beta1.getImages().at(6 - j),
                    this->blackImage
                );
            } else if (j > 4) { // further passes do
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.delta1.at(j - 5).getImage0(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    pass.delta1.at(j - 5).getImage0(),
                    this->beta1.getImages().at(6 - j),
                    pass.delta1.at(j - 5).getImage1()
                );
            }
        }

        pass.generate.emplace(ctx, i,
            this->sourceImages,
            pass.gamma1.at(6).getImage(),
            pass.delta1.at(2).getImage0(),
            pass.delta1.at(2).getImage1(),
            this->destImages.at(i)
        );
    }

    // initialize all images
    std::vector<VkImage> images{};
    images.push_back(this->blackImage.handle());
    mipmaps.prepare(images);
    for (size_t i = 0; i < 7; ++i) {
        alpha0.at(i).prepare(images);
        alpha1.at(i).prepare(images);
    }
    beta0.prepare(images);
    beta1.prepare(images);
    for (const auto& pass : this->passes) {
        for (size_t i = 0; i < 7; ++i) {
            pass.gamma0.at(i).prepare(images);
            pass.gamma1.at(i).prepare(images);

            if (i < 4) continue;
            pass.delta0.at(i - 4).prepare(images);
            pass.delta1.at(i - 4).prepare(images);
        }
    }

    std::vector<vk::Barrier> barriers{};
    barriers.reserve(images.size());

    for (const auto& image : images) {
        barriers.emplace_back(vk::Barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });
    }

    const vk::CommandBuffer cmdbuf{ctx.vk};
    cmdbuf.begin(ctx.vk);
    cmdbuf.insertBarriers(ctx.vk, barriers);
    cmdbuf.end(ctx.vk);
    cmdbuf.submit(ctx.vk); // wait for completion
}

std::vector<int> Instance::scheduleFrames(Context& context, int captureReadyFd) { // NOLINT (static)
#ifdef LSFGVK_TESTING_RENDERDOC
    const auto& impl = this->m_impl;
    if (impl->getRenderDocAPI()) {
        impl->getRenderDocAPI()->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
    try {
        return context.scheduleFrames(captureReadyFd);
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
#ifdef LSFGVK_TESTING_RENDERDOC
    if (impl->getRenderDocAPI()) {
        impl->getVulkan().df().DeviceWaitIdle(impl->getVulkan().dev());
        impl->getRenderDocAPI()->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
}

std::vector<int> Context::scheduleFrames(int captureReadyFd) {
    // cross-device mode replicates the timeline choreography via sync-fd
    // binary handshakes; the same-device path below stays byte-identical
    if (this->crossDevice)
        return this->scheduleFramesCross(captureReadyFd);

    // wait for previous pre-pass to complete
    if (this->fidx && !this->cmdbufFence.wait(this->ctx.vk))
        throw backend::error("Timeout waiting for previous frame to complete");
    this->cmdbufFence.reset(this->ctx.vk);

    // schedule pre-pass
    const auto& cmdbuf = this->cmdbufs.at(0);
    cmdbuf.begin(ctx.vk);

    this->mipmaps.render(ctx.vk, cmdbuf, this->fidx);
    for (size_t i = 0; i < 7; ++i) {
        this->alpha0.at(6 - i).render(ctx.vk, cmdbuf);
        this->alpha1.at(6 - i).render(ctx.vk, cmdbuf, this->fidx);
    }
    this->beta0.render(ctx.vk, cmdbuf, this->fidx);
    this->beta1.render(ctx.vk, cmdbuf);

    cmdbuf.end(ctx.vk);
    cmdbuf.submit(this->ctx.vk,
        // same-device branch: the optional is guaranteed engaged by the ctor
        {}, this->syncSemaphore.value().handle(),  // NOLINT(bugprone-unchecked-optional-access)
        this->idx,
        {}, this->prepassSemaphore.handle(), this->idx
    );

    this->idx++;

    // schedule main passes
    for (size_t i = 0; i < this->destImages.size(); i++) {
        const auto& cmdbuf = this->cmdbufs.at(i + 1);
        cmdbuf.begin(ctx.vk);

        const auto& pass = this->passes.at(i);
        for (size_t j = 0; j < 7; j++) {
            pass.gamma0.at(j).render(ctx.vk, cmdbuf, this->fidx);
            pass.gamma1.at(j).render(ctx.vk, cmdbuf);

            if (j < 4) continue;
            pass.delta0.at(j - 4).render(ctx.vk, cmdbuf, this->fidx);
            pass.delta1.at(j - 4).render(ctx.vk, cmdbuf);
        }
        pass.generate->render(ctx.vk, cmdbuf, this->fidx);

        cmdbuf.end(ctx.vk);
        cmdbuf.submit(this->ctx.vk,
            {}, this->prepassSemaphore.handle(), this->idx - 1,
            // same-device branch: the optional is guaranteed engaged by the ctor
            {}, this->syncSemaphore.value().handle(),  // NOLINT(bugprone-unchecked-optional-access)
            this->idx + i,
            i == this->destImages.size() - 1 ? this->cmdbufFence.handle() : VK_NULL_HANDLE
        );
    }

    this->idx += this->destImages.size();
    this->fidx++;
    return {};
}

std::vector<int> ContextImpl::scheduleFramesCross(int captureReadyFd) {
    // fresh capture-completion semaphore for THIS cycle. consuming the fd
    // up front keeps the "consumed regardless of success" contract even if
    // semaphore creation or the fence gate below throws; on the creation
    // path nothing was submitted against it yet, so closing the caller's
    // fd is the only cleanup owed.
    // an fd of -1 means the capture already completed; per spec that behaves
    // like an already-signaled sync fd, emulated by queue-signaling directly
    // (host signaling of binary semaphores is unsupported).
    auto captureSem = [&]() {
        try {
            return createSyncFdSemaphore(this->ctx.vk);
        } catch (...) {
            if (captureReadyFd >= 0) close(captureReadyFd);
            throw;
        }
    }();
    if (captureReadyFd >= 0) {
        importSyncFdSemaphore(this->ctx.vk, captureSem.handle(), captureReadyFd);
    } else {
        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &captureSem.handle()
        };
        auto res = this->ctx.vk.get().df().QueueSubmit(
            this->ctx.vk.get().queue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkQueueSubmit() failed");

        res = this->ctx.vk.get().df().DeviceWaitIdle(this->ctx.vk.get().dev());
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkDeviceWaitIdle() failed");
    }

    std::vector<int> doneFds{};
    try {
        // wait for previous frame's passes to complete; this also proves the
        // retired generation's signal/wait batches finished, so they can be
        // destroyed here without tripping VUID-05149 under validation
        if (this->fidx && !this->cmdbufFence.wait(this->ctx.vk))
            throw backend::error("Timeout waiting for previous frame to complete");
        this->retiredCaptureWait = std::move(this->captureWait);
        this->retiredDoneSignals = std::move(this->doneSignals);
        this->captureWait = std::move(captureSem);
        this->cmdbufFence.reset(this->ctx.vk);

        // the pre-pass waits ONLY this cycle's capture semaphore: cross-cycle
        // ordering comes from the fences (cmdbufFence here, the layer's
        // renderFence on the game device), never from re-waiting exported
        // slots - that recycle-wait deadlocked RADV inside vkQueueSubmit
        const auto& captureWaitHandle = this->captureWait->handle();
        const std::vector<VkSemaphore> prepassWaits{ captureWaitHandle };

        // schedule pre-pass
        const auto& cmdbuf = this->cmdbufs.at(0);
        cmdbuf.begin(ctx.vk);

        this->mipmaps.render(ctx.vk, cmdbuf, this->fidx);
        for (size_t i = 0; i < 7; ++i) {
            this->alpha0.at(6 - i).render(ctx.vk, cmdbuf);
            this->alpha1.at(6 - i).render(ctx.vk, cmdbuf, this->fidx);
        }
        this->beta0.render(ctx.vk, cmdbuf, this->fidx);
        this->beta1.render(ctx.vk, cmdbuf);

        cmdbuf.end(ctx.vk);
        cmdbuf.submit(this->ctx.vk,
            prepassWaits, VK_NULL_HANDLE, 0,
            {}, this->prepassSemaphore.handle(), this->idx
        );

        this->idx++;

        // schedule main passes
        for (size_t i = 0; i < this->destImages.size(); i++) {
            const auto& cmdbuf = this->cmdbufs.at(i + 1);
            cmdbuf.begin(ctx.vk);

            const auto& pass = this->passes.at(i);
            for (size_t j = 0; j < 7; j++) {
                pass.gamma0.at(j).render(ctx.vk, cmdbuf, this->fidx);
                pass.gamma1.at(j).render(ctx.vk, cmdbuf);

                if (j < 4) continue;
                pass.delta0.at(j - 4).render(ctx.vk, cmdbuf, this->fidx);
                pass.delta1.at(j - 4).render(ctx.vk, cmdbuf);
            }
            pass.generate->render(ctx.vk, cmdbuf, this->fidx);

            // fresh done semaphore per generated frame per cycle: signaled by
            // exactly this submit and never waited on locally afterwards
            this->doneSignals.push_back(createSyncFdSemaphore(this->ctx.vk));
            const auto& doneHandle = this->doneSignals.back().handle();

            cmdbuf.end(ctx.vk);
            cmdbuf.submit(this->ctx.vk,
                {}, this->prepassSemaphore.handle(), this->idx - 1,
                { doneHandle }, VK_NULL_HANDLE, 0,
                i == this->destImages.size() - 1 ? this->cmdbufFence.handle() : VK_NULL_HANDLE
            );

            // export right after enqueueing the signal: sync fds have copy
            // transference, so the fd snapshots the pending payload and is
            // signaled once the pass completes on the device. the semaphore
            // object is retired next cycle behind the fence gate instead of
            // being destroyed now (pending batches) or re-waited (RADV hang)
            doneFds.push_back(this->doneSignals.back().exportFd(this->ctx.vk));
        }

        this->idx += this->destImages.size();
        this->fidx++;
    } catch (...) {
        for (const auto& fd : doneFds)
            if (fd >= 0) close(fd);
        throw;
    }

    return doneFds;
}

void Instance::closeContext(const Context& context) {
    auto it = std::ranges::find_if(this->m_contexts,
        [context = &context](const std::unique_ptr<ContextImpl>& ctx) {
            return ctx.get() == context;
        });
    if (it == this->m_contexts.end())
        throw backend::error("attempted to close unknown context",
            std::runtime_error("no such context"));

    const auto& vk = this->m_impl->getVulkan();
    vk.df().DeviceWaitIdle(vk.dev());

    this->m_contexts.erase(it);
}

bool Instance::isCrossDevice(const Context& context) const {
    const auto it = std::ranges::find_if(this->m_contexts,
        [context = &context](const std::unique_ptr<ContextImpl>& ctx) {
            return ctx.get() == context;
        });
    if (it == this->m_contexts.end())
        throw backend::error("attempted to query unknown context",
            std::runtime_error("no such context"));

    return (*it)->isCrossDevice();
}

Instance::~Instance() = default;

// leaking shenanigans

namespace {
    bool leaking{false}; // NOLINT (global variable)
}

InstanceImpl::~InstanceImpl() {
    if (!leaking) return;

    try {
        new vk::Vulkan(std::move(this->vk));
    } catch (...) {
        std::cerr << "lsfg-vk: failed to leak Vulkan instance\n";
    }

}

void backend::makeLeaking() {
    leaking = true;
}
