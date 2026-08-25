/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"

#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// check whether the intersection of both devices' usage bits covers the need
    bool coversUsage(VkFormatFeatureFlags2 bitsA, VkFormatFeatureFlags2 bitsB,
            VkFormatFeatureFlags2 usageNeeded) {
        return (bitsA & bitsB & usageNeeded) == usageNeeded;
    }

    /// find the LINEAR entry of a modifier list, or null if absent
    const ExchangeModifierCaps* findLinear(const std::vector<ExchangeModifierCaps>& list) {
        for (const auto& entry : list)
            if (entry.modifier == EXCHANGE_MODIFIER_LINEAR) return &entry;
        return nullptr;
    }
}

NegotiatedExchangeLayout vk::negotiateExchangeLayout(const DeviceExchangeCaps& capsA,
        const DeviceExchangeCaps& capsB, VkFormat format,
        VkFormatFeatureFlags2 usageNeeded) {
    const auto itA = capsA.find(format);
    const auto itB = capsB.find(format);

    // first common candidate in capsA order wins, keeping negotiation deterministic
    if (itA != capsA.end() && itB != capsB.end()) {
        for (const auto& entryA : itA->second) {
            for (const auto& entryB : itB->second) {
                if (entryA.modifier != entryB.modifier) continue;
                if (!coversUsage(entryA.requiredUsageBits,
                        entryB.requiredUsageBits, usageNeeded)) continue;
                return NegotiatedExchangeLayout{
                    .modifier = entryA.modifier
                };
            }
        }
    }

    // fall back to LINEAR only when both sides support it with sufficient usage bits
    const ExchangeModifierCaps* linearA =
        itA == capsA.end() ? nullptr : findLinear(itA->second);
    const ExchangeModifierCaps* linearB =
        itB == capsB.end() ? nullptr : findLinear(itB->second);

    if (linearA && linearB &&
            coversUsage(linearA->requiredUsageBits, linearB->requiredUsageBits, usageNeeded))
        return NegotiatedExchangeLayout{
            .modifier = EXCHANGE_MODIFIER_LINEAR
        };

    throw ls::error("no usable exchange layout for format "
        + std::to_string(static_cast<int>(format)) + " with needed usage bits 0x"
        + std::to_string(usageNeeded)
        + ": no common DRM modifier between devices and LINEAR tiling unsupported");
}
