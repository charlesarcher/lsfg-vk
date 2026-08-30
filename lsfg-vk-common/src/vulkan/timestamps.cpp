/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/timestamps.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
}

TimingRing::~TimingRing() = default;

void TimingRing::writeTimestamp(VkCommandBuffer cmdbuf, Stage stage, bool isStart) const {
    if (!enabled_) return;

    const uint32_t baseQuery = static_cast<uint32_t>(stage) * 2 + (isStart ? 0 : 1);
    vk_->df().CmdWriteTimestamp(cmdbuf,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        *pool_, baseQuery);
}

void TimingRing::resetFrame(VkCommandBuffer cmdbuf, uint64_t frameIdx) const {
    if (!enabled_) return;

    const uint32_t firstQuery = static_cast<uint32_t>(frameIdx % ringDepth_) * queriesPerFrame_;
    vk_->df().CmdResetQueryPool(cmdbuf, *pool_, firstQuery, queriesPerFrame_);
}

std::optional<TimingRing::FrameTiming> TimingRing::readFrame(uint64_t frameIdx) {
    if (!enabled_) return std::nullopt;

    const uint32_t firstQuery = static_cast<uint32_t>(frameIdx % ringDepth_) * queriesPerFrame_;

    // Read all queries for this frame - non-blocking to avoid hangs on some hardware/drivers
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    auto res = vk_->df().GetQueryPoolResults(
        vk_->dev(), *pool_, firstQuery, queriesPerFrame_,
        readbackBuffer_.size() * sizeof(uint64_t),
        readbackBuffer_.data(), sizeof(uint64_t), flags);

    if (res == VK_NOT_READY) {
        return std::nullopt;
    }
    if (res != VK_SUCCESS) {
        throw ls::vulkan_error(res, "vkGetQueryPoolResults() failed");
    }

    // Apply valid bits mask and convert to nanoseconds
    auto toNs = [this](uint64_t raw) -> uint64_t {
        return static_cast<uint64_t>((raw & timestampValidBitsMask_) * timestampPeriod_);
    };

    FrameTiming timing{};
    timing.frameIdx = frameIdx;

    // Backend stages (processing device)
    // CopyIn: stage 0-1
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::CopyInEnd) * 2 + 1) {
        uint64_t copyInStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::CopyInStart) * 2]);
        uint64_t copyInEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::CopyInEnd) * 2]);
        if (copyInEnd > copyInStart) timing.tCopyInNs = copyInEnd - copyInStart;
    }

    // Flow (mipmaps + alpha/beta/gamma/delta): stages 2-5
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::AlphaBetaGammaDeltaEnd) * 2 + 1) {
        uint64_t flowStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::MipmapsStart) * 2]);
        uint64_t flowEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::AlphaBetaGammaDeltaEnd) * 2]);
        if (flowEnd > flowStart) timing.tFlowNs = flowEnd - flowStart;
    }

    // Generate: stages 6-7
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::GenerateEnd) * 2 + 1) {
        uint64_t genStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GenerateStart) * 2]);
        uint64_t genEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GenerateEnd) * 2]);
        if (genEnd > genStart) timing.tGenerateNs = genEnd - genStart;
    }

    // CopyOut: stages 8-9
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::CopyOutEnd) * 2 + 1) {
        uint64_t copyOutStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::CopyOutStart) * 2]);
        uint64_t copyOutEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::CopyOutEnd) * 2]);
        if (copyOutEnd > copyOutStart) timing.tCopyOutNs = copyOutEnd - copyOutStart;
    }

    // Game side (layer device) stages
    // Game CopyIn: stages 10-11
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::GameCopyInEnd) * 2 + 1) {
        uint64_t gameInStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GameCopyInStart) * 2]);
        uint64_t gameInEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GameCopyInEnd) * 2]);
        if (gameInEnd > gameInStart) timing.tGameSideInNs = gameInEnd - gameInStart;
    }

    // Game CopyOut: stages 12-13
    if (queriesPerFrame_ > static_cast<uint32_t>(Stage::GameCopyOutEnd) * 2 + 1) {
        uint64_t gameOutStart = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GameCopyOutStart) * 2]);
        uint64_t gameOutEnd = toNs(readbackBuffer_[static_cast<uint32_t>(Stage::GameCopyOutEnd) * 2]);
        if (gameOutEnd > gameOutStart) timing.tGameSideOutNs = gameOutEnd - gameOutStart;
    }

    // Total = sum of all backend stages
    timing.tTotalNs = timing.tCopyInNs + timing.tFlowNs + timing.tGenerateNs + timing.tCopyOutNs;

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