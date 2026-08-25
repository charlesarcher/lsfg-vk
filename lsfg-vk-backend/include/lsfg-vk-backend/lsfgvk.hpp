/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lsfg-vk-common/vulkan/exchange.hpp"

namespace lsfgvk::backend {

    /// sentinel DRM modifier meaning "import as legacy OPAQUE_FD external memory".
    /// modifier convention for ExchangeDescriptor ingestion (see openContext):
    /// - EXCHANGE_MODIFIER_OPAQUE: legacy opaque-fd import; the descriptor's
    ///   allocationSize and rowPitch fields are ignored
    /// - vk::EXCHANGE_MODIFIER_LINEAR (0): modifier-0 dma-buf import via the
    ///   DRM_FORMAT_MODIFIER tiling path (rowPitch required)
    /// - any other value: explicit drm-modifier dma-buf import (rowPitch required)
    inline constexpr uint64_t EXCHANGE_MODIFIER_OPAQUE =
        std::numeric_limits<uint64_t>::max();

    class [[gnu::visibility("default")]] ContextImpl;
    class [[gnu::visibility("default")]] InstanceImpl;

    using Context = ContextImpl;

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class [[gnu::visibility("default")]] error : public std::runtime_error {
    public:
        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        /// @param inner Inner exception.
        ///
        explicit error(const std::string &msg, const std::exception &inner);

        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        ///
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    /// Function type for picking a device based on its name and IDs
    using DevicePicker = std::function<bool(
        const std::string& deviceName,
        std::pair<const std::string&, const std::string&> ids, // (vendor ID, device ID) 0xXXXX format
        const std::optional<std::string>& pci // (bus:slot.func) if available, no padded zeros
    )>;

    ///
    /// Main entry point of the library
    ///
    class [[gnu::visibility("default")]] Instance {
    public:
        ///
        /// Create a lsfg-vk instance
        ///
        /// @param devicePicker Function that picks a physical device based on some identifiers.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        /// @param enableDmaBufExtensions Whether to additionally enable the dma-buf
        ///        exchange extensions (VK_EXT_external_memory_dma_buf,
        ///        VK_EXT_image_drm_format_modifier) on the selected device's logical
        ///        device. Must be true for any instance that will open cross-device
        ///        contexts or ingest non-opaque exchange descriptors; requesting it
        ///        on a device lacking either extension is a hard error naming the
        ///        device and extension. Defaults to off (legacy extension set).
        /// @throws backend::error on failure
        ///
        Instance(
            const DevicePicker& devicePicker,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision,
            bool enableDmaBufExtensions = false
        );

        ///
        /// Open a frame generation context from exchange descriptors.
        ///
        /// == Exchange protocol ==
        ///
        /// The VkFormat of the exchanged images is inferred from whether hdr is true or false:
        /// - false: VK_FORMAT_R8G8B8A8_UNORM
        /// - true: VK_FORMAT_R16G16B16A16_SFLOAT
        ///
        /// The application and library must keep track of the frame index. When the next frame
        /// is ready, signal the syncFd with one increment (with the first trigger being 1).
        /// Each generated frame will increment the semaphore by one:
        /// - Application signals 1 -> Start generating with (curr, next) source images
        /// - Library signals 1 -> First frame between (curr, next) is ready
        /// - Library signals N -> N-th frame between (curr, next) is ready
        /// - Application signals N+1 -> Start generating with (next, curr) source images
        ///
        /// == Descriptor ingestion contract ==
        ///
        /// The backend TRUSTS the negotiated layout it receives: layout negotiation
        /// happens caller-side (e.g. via vk::negotiateExchangeLayout over both
        /// devices' vk::exchangeCaps); the backend never renegotiates. Every
        /// descriptor in both spans must carry the negotiated modifier:
        /// - EXCHANGE_MODIFIER_OPAQUE: legacy OPAQUE_FD import (allocationSize and
        ///   rowPitch ignored; byte-identical to the legacy fd-pair overload)
        /// - vk::EXCHANGE_MODIFIER_LINEAR: modifier-0 dma-buf import via the
        ///   DRM_FORMAT_MODIFIER tiling path (rowPitch required)
        /// - any other value: explicit drm-modifier dma-buf import (rowPitch required)
        ///
        /// Each descriptor's extent must equal (width, height) and its format must
        /// equal the hdr-inferred format; violations throw a descriptive error before
        /// any file descriptor is touched.
        ///
        /// File descriptor ownership: each descriptor's fd is CONSUMED by a successful
        /// image import (VK_KHR_external_memory_fd transfer semantics). On import
        /// failure the failing fd is closed by the importer; on validation failure
        /// (thrown before any import) all fds remain owned by the caller.
        ///
        /// == Device-aware operation ==
        ///
        /// Cross-device mode is selected by comparing exporterDeviceUUID against the
        /// selected device's UUID (selectedDeviceUUID()):
        /// - equal: same-device mode, the timeline choreography documented above
        ///   applies unchanged (syncFd honored).
        /// - different: cross-device mode, the shared timeline semaphore is replaced
        ///   by SYNC_FD binary semaphore handshakes replicating the exact
        ///   choreography (see scheduleFrames for the fd flow); syncFd is ignored.
        ///   Requires this instance to have been created with enableDmaBufExtensions
        ///   set (hard error otherwise), which in turn hard-errors at construction
        ///   time if the selected device lacks either exchange extension.
        ///
        /// @param sources Exactly 2 descriptors for the source images alternated between.
        /// @param dests At least 1 descriptor for the output images to generate into.
        /// @param exporterDeviceUUID Device UUID of the device that exported (created)
        ///        the exchanged memory; also supersedes the legacy overload's optional
        ///        gameDeviceUUID for cross-device detection.
        /// @param negotiatedModifier Result of the caller-side layout negotiation;
        ///        must match every descriptor's modifier field.
        /// @param syncFd File descriptor for the timeline semaphore used for
        ///        synchronization (same-device mode only).
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::span<const vk::ExchangeDescriptor> sources,
            std::span<const vk::ExchangeDescriptor> dests,
            std::array<uint8_t, 16> exporterDeviceUUID,
            uint64_t negotiatedModifier,
            int syncFd,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf
        );
        ///
        /// Legacy fd-pair entry point, kept for the current layer call site.
        ///
        /// NOTE(todo-13): the layer must migrate to the descriptor overload above:
        /// create the two source and (multiplier-1) destination images with the
        /// negotiated tiling, export each as dma-buf into an ExchangeDescriptor
        /// carrying {fd, allocationSize, rowPitch, negotiated modifier, format,
        /// extent}, then pass those spans plus the game device's UUID as
        /// exporterDeviceUUID and the negotiated modifier. Until then this overload
        /// wraps the raw fds into EXCHANGE_MODIFIER_OPAQUE descriptors and assumes
        /// same-device operation when gameDeviceUUID is nullopt.
        ///
        /// @param sourceFds Pair of file descriptors for the source images alternated between.
        /// @param destFds Vector with file descriptors to import output images from.
        /// @param syncFd File descriptor for the timeline semaphore used for synchronization.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        /// @param gameDeviceUUID Device UUID of the exporting (game) device, compared
        ///                       against the selected device to detect cross-device
        ///                       operation. nullopt assumes same-device operation.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<int, int> sourceFds,
            const std::vector<int>& destFds,
            int syncFd,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf,
            std::optional<std::array<uint8_t, 16>> gameDeviceUUID = std::nullopt
        );

        ///
        /// Schedule a new set of generated frames.
        ///
        /// In cross-device mode the timeline choreography travels via SYNC_FD binary
        /// semaphores instead:
        /// - captureReadyFd carries a sync fd that is signaled once the current
        ///   frame has been captured into the source images ("application signals
        ///   1" above). Ownership is transferred to the library and the fd is
        ///   consumed regardless of success. -1 means the capture already completed.
        /// - the returned vector carries one sync fd per generated frame, signaled
        ///   when that frame is done ("library signals N" above). Ownership stays
        ///   with the caller, which must close(2) or import each fd. Empty in
        ///   same-device mode.
        ///
        /// @param context Context to use.
        /// @param captureReadyFd Sync fd signaled on capture completion (cross-device mode).
        /// @return Per-generated-frame done sync fds (cross-device mode only).
        /// @throws backend::error on failure
        ///
        std::vector<int> scheduleFrames(Context& context, int captureReadyFd = -1);

        ///
        /// Check whether a context synchronizes across devices using SYNC_FD handshakes.
        ///
        /// @param context Context to query.
        /// @return true if the context runs in cross-device mode.
        ///
        /// @throws backend::error on unknown context
        ///
        [[nodiscard]] bool isCrossDevice(const Context& context) const;

        ///
        /// Get the device UUID of the physical device selected by this instance
        /// (the device that runs the frame generation pipeline).
        ///
        /// @return The 16-byte device UUID.
        ///
        [[nodiscard]] std::array<uint8_t, 16> selectedDeviceUUID() const;

        ///
        /// Check whether the selected physical device supports dma-buf
        /// external memory (VK_EXT_external_memory_dma_buf).
        ///
        /// @return true if dma-buf imports/exports are supported.
        ///
        [[nodiscard]] bool selectedDeviceSupportsDmaBuf() const;

        ///
        /// Check whether the selected physical device supports drm modifier
        /// images (VK_EXT_image_drm_format_modifier).
        ///
        /// @return true if drm modifier images are supported.
        ///
        [[nodiscard]] bool selectedDeviceSupportsDrmModifierImages() const;

        ///
        /// Close a frame generation context
        ///
        /// @param context Context to close.
        ///
        void closeContext(const Context& context);

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
    };

    ///
    /// Make all lsfg-vk instances leaking.
    /// This is to workaround a bug in the Vulkan loader, which
    /// makes it impossible to destroy Vulkan instances and devices.
    ///
    void makeLeaking();

}
