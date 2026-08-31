/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/timestamps.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {

    /// Check if timing is enabled via environment variable
    bool isTimingEnabled() {
        const char* env = std::getenv("LSFGVK_TIMING");
        return env && *env != '\0' && std::string(env) == "1";
    }

    /// Get CSV output path from environment variable
    std::optional<std::string> getCsvPath() {
        const char* env = std::getenv("LSFGVK_TIMING_CSV");
        if (env && *env != '\0') {
            return std::string(env);
        }
        return std::nullopt;
    }

    /// Query timestamp properties from physical device
    void queryTimestampProperties(const vk::Vulkan& vk,
                                  uint64_t& validBitsMask,
                                  float& period) {
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);

        // timestampValidBits is per queue family - query the compute queue family
        uint32_t queueFamilyCount = 0;
        vk.fi().GetPhysicalDeviceQueueFamilyProperties(vk.physdev(), &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vk.fi().GetPhysicalDeviceQueueFamilyProperties(vk.physdev(), &queueFamilyCount, queueFamilies.data());

        // Use the compute queue family (same as what Vulkan class uses)
        uint32_t validBits = 64;
        if (vk.queueFamilyIndex() < queueFamilies.size()) {
            validBits = queueFamilies[vk.queueFamilyIndex()].timestampValidBits;
        }
        if (validBits == 0) validBits = 64;
        if (validBits > 64) validBits = 64;

        validBitsMask = (validBits == 64) ? UINT64_MAX : ((UINT64_C(1) << validBits) - 1);
        period = props.properties.limits.timestampPeriod; // nanoseconds per timestamp unit
    }

    /// Create a timestamp query pool
    ls::owned_ptr<VkQueryPool> createQueryPool(const vk::Vulkan& vk, uint32_t queryCount) {
        const VkQueryPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = queryCount,
            .pipelineStatistics = 0
        };

        VkQueryPool pool{};
        auto res = vk.df().CreateQueryPool(vk.dev(), &poolInfo, VK_NULL_HANDLE, &pool);
        if (res != VK_SUCCESS) {
            throw ls::vulkan_error(res, "vkCreateQueryPool() failed");
        }

        return ls::owned_ptr<VkQueryPool>(
            new VkQueryPool(pool),
            [dev = vk.dev(), defunc = vk.df().DestroyQueryPool](VkQueryPool& p) {
                defunc(dev, p, VK_NULL_HANDLE);
            }
        );
    }

} // namespace

TimingRing::TimingRing(const vk::Vulkan& vk, const std::string& side, size_t ringDepth)
    : enabled_(isTimingEnabled())
    , side_(side)
    , ringDepth_(std::max(ringDepth, size_t(8)))
    , queriesPerFrame_(static_cast<uint32_t>(Stage::Count) * 2)
    , csvPath_(getCsvPath())
    , vk_(&vk) {

    if (!enabled_) {
        // Timing disabled - all methods become no-ops
        return;
    }

    queryTimestampProperties(vk, timestampValidBitsMask_, timestampPeriod_);

    const uint32_t totalQueries = queriesPerFrame_ * static_cast<uint32_t>(ringDepth_);
    pool_ = createQueryPool(vk, totalQueries);

    // Pre-allocate readback buffer
    readbackBuffer_.resize(queriesPerFrame_);

    if (csvPath_) {
        // Write CSV header
        std::ofstream csv(*csvPath_, std::ios::trunc);
        if (csv.is_open()) {
            csv << "frame_idx,side,t_copyin_ns,t_flow_ns,t_generate_ns,t_copyout_ns,t_total_ns,t_gameside_in_ns,t_gameside_out_ns\n";
        }
    }

    if (std::getenv("LSFGVK_TIMING_DBG")) {
        std::fprintf(stderr, "[timing] %s: period=%g ns/unit validBitsMask=0x%llx\n",
            side_.c_str(), timestampPeriod_,
            static_cast<unsigned long long>(timestampValidBitsMask_));
    }
}

TimingRing::~TimingRing() = default;

void TimingRing::writeTimestamp(VkCommandBuffer cmdbuf, Stage stage, bool isStart) const {
    if (!enabled_) return;

    // per-frame query offset so each frame's timestamps live in its own ring
    // slot (set by resetFrame, which is always issued once per frame before its
    // writes); a fixed stage offset would let every frame overwrite the same
    // queries and readback would never be coherent.
    const uint32_t baseQuery = currentFrameSlot_ * queriesPerFrame_
        + static_cast<uint32_t>(stage) * 2 + (isStart ? 0 : 1);
    vk_->df().CmdWriteTimestamp(cmdbuf,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        *pool_, baseQuery);
}

void TimingRing::resetFrame(VkCommandBuffer cmdbuf, uint64_t frameIdx) const {
    if (!enabled_) return;

    currentFrameSlot_ = static_cast<uint32_t>(frameIdx % ringDepth_);
    const uint32_t firstQuery = currentFrameSlot_ * queriesPerFrame_;
    vk_->df().CmdResetQueryPool(cmdbuf, *pool_, firstQuery, queriesPerFrame_);
}

std::optional<TimingRing::FrameTiming> TimingRing::readFrame(uint64_t frameIdx) {
    if (!enabled_) return std::nullopt;

    const uint32_t baseQuery = static_cast<uint32_t>(frameIdx % ringDepth_) * queriesPerFrame_;
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;

    // Each metric spans a start timestamp (written at stage S, query S*2+0) and
    // an end timestamp (written at stage E, query E*2+1) - non-contiguous, so
    // read the two queries separately. A whole-frame range read would stay
    // VK_NOT_READY whenever any query in the span was never issued (the backend
    // timestamps only 8 of the 28, the layer only the game-side stages).
    // non-blocking (no WAIT bit) to avoid hangs on some hardware/drivers.
    // return the masked RAW timestamp (unscaled); scaling is done on the
    // start/end difference, not on the huge absolute values, because a float
    // period applied to ~1e14-magnitude absolutes loses all low-order bits.
    auto readQuery = [this, baseQuery, flags](uint32_t query) -> std::optional<uint64_t> {
        uint64_t raw{};
        auto res = vk_->df().GetQueryPoolResults(
            vk_->dev(), *pool_, baseQuery + query, 1,
            sizeof(raw), &raw, sizeof(uint64_t), flags);
        if (res == VK_NOT_READY) return std::nullopt;
        if (res != VK_SUCCESS) throw ls::vulkan_error(res, "vkGetQueryPoolResults() failed");
        static thread_local int dbgCount = 0;
        if (std::getenv("LSFGVK_TIMING_DBG") && dbgCount < 12) {
            std::fprintf(stderr, "[timing] readQuery idx=%u res=%d raw=%llu\n",
                baseQuery + query, res, static_cast<unsigned long long>(raw & timestampValidBitsMask_));
            dbgCount++;
        }
        return raw & timestampValidBitsMask_;
    };
    // metric = (end - start) scaled to ns; subtract in the raw integer domain
    // first so the small delta is not lost to float precision on the absolutes.
    auto metric = [&, this](Stage startStage, Stage endStage) -> uint64_t {
        const auto s = readQuery(static_cast<uint32_t>(startStage) * 2);
        const auto e = readQuery(static_cast<uint32_t>(endStage) * 2 + 1);
        if (!s || !e || *e <= *s) return 0;
        return static_cast<uint64_t>((static_cast<double>(*e) - static_cast<double>(*s)) * timestampPeriod_);
    };

    FrameTiming timing{};
    timing.frameIdx = frameIdx;

    timing.tCopyInNs = metric(Stage::CopyInStart, Stage::CopyInEnd);
    timing.tFlowNs = metric(Stage::MipmapsStart, Stage::AlphaBetaGammaDeltaEnd);
    timing.tGenerateNs = metric(Stage::GenerateStart, Stage::GenerateEnd);
    timing.tCopyOutNs = metric(Stage::CopyOutStart, Stage::CopyOutEnd);
    timing.tGameSideInNs = metric(Stage::GameCopyInStart, Stage::GameCopyInEnd);
    timing.tGameSideOutNs = metric(Stage::GameCopyOutStart, Stage::GameCopyOutEnd);

    timing.tTotalNs = timing.tCopyInNs + timing.tFlowNs + timing.tGenerateNs + timing.tCopyOutNs;

    if (std::getenv("LSFGVK_TIMING_DBG") && (frameIdx % 64) < 4) {
        std::fprintf(stderr, "[timing] frame %llu: copyIn=%llu flow=%llu gen=%llu copyOut=%llu total=%llu\n",
            static_cast<unsigned long long>(frameIdx),
            static_cast<unsigned long long>(timing.tCopyInNs),
            static_cast<unsigned long long>(timing.tFlowNs),
            static_cast<unsigned long long>(timing.tGenerateNs),
            static_cast<unsigned long long>(timing.tCopyOutNs),
            static_cast<unsigned long long>(timing.tTotalNs));
    }
    return timing;
}

void TimingRing::writeCsvRow(const FrameTiming& timing) const {
    if (!csvPath_) return;

    std::ofstream csv(*csvPath_, std::ios::app);
    if (!csv.is_open()) return;

    csv << timing.frameIdx << ','
        << side_ << ','
        << timing.tCopyInNs << ','
        << timing.tFlowNs << ','
        << timing.tGenerateNs << ','
        << timing.tCopyOutNs << ','
        << timing.tTotalNs << ','
        << timing.tGameSideInNs << ','
        << timing.tGameSideOutNs << '\n';
}