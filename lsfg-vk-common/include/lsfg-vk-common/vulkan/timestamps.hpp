/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"
#include "vulkan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {

    /// GPU timestamp instrumentation ring buffer.
    /// Only active when LSFGVK_TIMING=1 environment variable is set.
    /// When inactive, all methods are no-ops with zero overhead.
    class TimingRing {
    public:
        /// Stage indices for timestamp pairs (start/end per stage)
        enum class Stage : uint32_t {
            // Backend (processing device) stages
            CopyInStart = 0,
            CopyInEnd = 1,
            MipmapsStart = 2,
            MipmapsEnd = 3,
            AlphaBetaGammaDeltaStart = 4,
            AlphaBetaGammaDeltaEnd = 5,
            GenerateStart = 6,
            GenerateEnd = 7,
            CopyOutStart = 8,
            CopyOutEnd = 9,

            // Layer (game device) stages
            GameCopyInStart = 10,
            GameCopyInEnd = 11,
            GameCopyOutStart = 12,
            GameCopyOutEnd = 13,

            Count = 14
        };

        /// Per-frame timing data in nanoseconds
        struct FrameTiming {
            uint64_t frameIdx = 0;
            uint64_t tCopyInNs = 0;
            uint64_t tFlowNs = 0;           // mipmaps + alpha/beta/gamma/delta
            uint64_t tGenerateNs = 0;
            uint64_t tCopyOutNs = 0;
            uint64_t tTotalNs = 0;
            uint64_t tGameSideInNs = 0;
            uint64_t tGameSideOutNs = 0;
        };

        /// Create a timing ring for a specific device side
        /// @param vk Vulkan instance
        /// @param side "backend" or "layer" - determines which stages are used
        /// @param ringDepth number of frames in the ring (default 8, min 8)
        TimingRing(const vk::Vulkan& vk, const std::string& side, size_t ringDepth = 8);

        ~TimingRing();

        TimingRing(const TimingRing&) = delete;
        TimingRing& operator=(const TimingRing&) = delete;
        TimingRing(TimingRing&&) = default;
        TimingRing& operator=(TimingRing&&) = default;

        /// Check if timing is enabled (LSFGVK_TIMING=1)
        [[nodiscard]] bool enabled() const { return enabled_; }

        /// Get the query pool handle (VK_NULL_HANDLE if disabled)
        [[nodiscard]] VkQueryPool pool() const { return pool_.get(); }

        /// Get the number of queries per frame (2 per stage: start + end)
        [[nodiscard]] uint32_t queriesPerFrame() const { return static_cast<uint32_t>(Stage::Count) * 2; }

        /// Get the ring depth (number of frames)
        [[nodiscard]] size_t ringDepth() const { return ringDepth_; }

        /// Write a timestamp at the given stage (start or end)
        /// @param cmdbuf command buffer to write into
        /// @param stage the stage to timestamp
        /// @param isStart true for start timestamp, false for end timestamp
        void writeTimestamp(VkCommandBuffer cmdbuf, Stage stage, bool isStart) const;

        /// Reset the query pool slots for a specific frame index
        /// @param cmdbuf command buffer to record reset into
        /// @param frameIdx frame index (modulo ringDepth)
        void resetFrame(VkCommandBuffer cmdbuf, uint64_t frameIdx) const;

        /// Read back timing data for frame N-4 (host readback while GPU works on N)
        /// @param frameIdx the frame index to read (should be currentFrame - 4)
        /// @return FrameTiming with nanosecond values, or nullopt if not ready
        std::optional<FrameTiming> readFrame(uint64_t frameIdx);

        /// Get the CSV output path if LSFGVK_TIMING_CSV is set
        [[nodiscard]] std::optional<std::string> csvPath() const { return csvPath_; }

        /// Write a CSV row for the given frame timing
        void writeCsvRow(const FrameTiming& timing) const;

    private:
        bool enabled_{false};
        std::string side_;
        size_t ringDepth_{8};
        uint32_t queriesPerFrame_{0};
        uint64_t timestampValidBitsMask_{UINT64_MAX};
        float timestampPeriod_{1.0f}; // nanoseconds per timestamp unit
        ls::owned_ptr<VkQueryPool> pool_;
        std::optional<std::string> csvPath_;
        mutable std::vector<uint64_t> readbackBuffer_; // reused buffer for vkGetQueryPoolResults
        const vk::Vulkan* vk_{nullptr}; // reference to Vulkan instance for device functions
    };

    /// RAII helper for writing a timestamp pair (start + end) around a region
    class ScopedTimestamp {
    public:
        ScopedTimestamp(const TimingRing* ring, VkCommandBuffer cmdbuf,
                        TimingRing::Stage stage)
            : ring_(ring), cmdbuf_(cmdbuf), stage_(stage) {
            if (ring_ && ring_->enabled()) {
                ring_->writeTimestamp(cmdbuf_, stage_, true);
            }
        }

        ~ScopedTimestamp() {
            if (ring_ && ring_->enabled()) {
                ring_->writeTimestamp(cmdbuf_, stage_, false);
            }
        }

    private:
        const TimingRing* ring_;
        VkCommandBuffer cmdbuf_;
        TimingRing::Stage stage_;
    };

} // namespace vk