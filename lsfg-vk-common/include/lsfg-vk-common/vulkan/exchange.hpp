/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {
    /// sentinel DRM modifier for linear tiling
    /// defined locally as 0 following the DRM convention (DRM_FORMAT_MOD_LINEAR);
    /// drivers report this modifier in VkDrmFormatModifierPropertiesEXT when
    /// plain VK_IMAGE_TILING_LINEAR images are supported for a format
    inline constexpr uint64_t EXCHANGE_MODIFIER_LINEAR = 0;

    /// descriptor of an image exchanged between two devices (e.g. via dma-buf)
    /// kept aggregate/simple so it can be passed across the backend API as-is
    struct ExchangeDescriptor {
        /// file descriptor of the shared memory object
        int fd;
        /// size of the underlying memory allocation
        VkDeviceSize allocationSize;
        /// row pitch of the image in bytes
        uint32_t rowPitch;
        /// DRM modifier of the image (EXCHANGE_MODIFIER_LINEAR for linear)
        uint64_t modifier;
        /// vulkan format of the image
        VkFormat format;
        /// extent of the image in pixels
        VkExtent2D extent;
    };

    /// capability of a single DRM modifier on one device for one format,
    /// mirroring VkDrmFormatModifierPropertiesEXT::drmFormatModifierTilingFeatures:
    /// the usage bits images with this modifier support on the owning device.
    /// negotiation requires the INTERSECTION of both devices' bits to cover usageNeeds
    struct ExchangeModifierCaps {
        /// DRM modifier this entry describes
        uint64_t modifier;
        /// usage bits supported for this modifier
        VkFormatFeatureFlags2 requiredUsageBits;
    };

    /// per-device exchange capabilities, keyed by format.
    /// only RGBA8 SDR / RGBA16F HDR are negotiated today; filled from
    /// VkDrmFormatModifierPropertiesListEXT by the caps-query adapter
    using DeviceExchangeCaps = std::map<VkFormat, std::vector<ExchangeModifierCaps>>;

    /// kind of negotiated exchange layout
    enum class ExchangeLayoutKind : std::uint8_t {
        Modifier,       /// a proper (vendor-specific) DRM modifier was picked
        LinearFallback  /// fell back to LINEAR tiling (modifier == EXCHANGE_MODIFIER_LINEAR)
    };

    /// result of a successful layout negotiation.
    /// `kind` is derived from `modifier` and always consistent with it;
    /// failure is signalled by throwing ls::error, matching the repo's
    /// exception-based error idiom (callers surface it as a hard named error)
    struct NegotiatedExchangeLayout {
        /// negotiated DRM modifier, EXCHANGE_MODIFIER_LINEAR for the LINEAR fallback
        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
        uint64_t modifier;

        /// whether this result is the LINEAR fallback rather than a proper modifier
        [[nodiscard]] ExchangeLayoutKind kind() const {
            return this->modifier == EXCHANGE_MODIFIER_LINEAR
                ? ExchangeLayoutKind::LinearFallback : ExchangeLayoutKind::Modifier;
        }
    };

    /// negotiate the image layout used to exchange frames between two devices.
    /// PURE function: deterministic, total for valid inputs, no I/O, no Vulkan calls.
    ///
    /// candidates are the modifiers present in BOTH caps lists for `format`
    /// whose intersected usage bits cover `usageNeeds`; the first such candidate
    /// in capsA order wins. if no candidate exists, LINEAR is used only if it
    /// appears in both lists with sufficient bits; otherwise ls::error is thrown.
    ///
    /// @param capsA capabilities of the exporting device
    /// @param capsB capabilities of the importing device
    /// @param format format to negotiate for (RGBA8 SDR or RGBA16F HDR)
    /// @param usageNeeded usage bits the negotiated layout must support on BOTH sides
    /// @return the negotiated layout
    /// @throws ls::error if no common layout satisfies the usage requirements
    NegotiatedExchangeLayout negotiateExchangeLayout(const DeviceExchangeCaps& capsA,
        const DeviceExchangeCaps& capsB, VkFormat format,
        VkFormatFeatureFlags2 usageNeeded);
}
