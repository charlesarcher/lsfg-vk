/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "copybench.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/timestamps.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk::cli;
using namespace lsfgvk::cli::copybench;

namespace {
    std::string uuidToHex(const std::array<uint8_t, 16>& uuid) {
        static constexpr std::array<char, 16> hexDigits{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string hex{};
        hex.reserve(32);
        for (const uint8_t byte : uuid) {
            hex.push_back(hexDigits.at(byte >> 4));
            hex.push_back(hexDigits.at(byte & 0xF));
        }
        return hex;
    }

    std::string deviceName(const vk::Vulkan& vk) {
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);
        std::array<char, 256> devname = std::to_array(props.properties.deviceName);
        devname.at(255) = '\0';
        return std::string(devname.data());
    }

    double percentile(const std::vector<uint64_t>& values, double p) {
        if (values.empty()) return 0.0;
        size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size()))) - 1;
        idx = std::min(idx, values.size() - 1);
        return static_cast<double>(values[idx]);
    }

    void printCopySummary(const std::vector<uint64_t>& copyTimesNs,
                          uint32_t width, uint32_t height, bool hdr) {
        if (copyTimesNs.empty()) return;

        std::vector<uint64_t> sorted = copyTimesNs;
        std::sort(sorted.begin(), sorted.end());

        const uint32_t bytesPerPixel = hdr ? 8 : 4;
        const uint64_t bytesPerFrame = static_cast<uint64_t>(width) * height * bytesPerPixel;

        std::cerr << "\ncopy benchmark results:\n";
        std::cerr << "  iterations:       " << copyTimesNs.size() << "\n";
        std::cerr << "  image size:       " << width << "x" << height << " ("
                  << (hdr ? "HDR/R16G16B16A16_SFLOAT" : "SDR/R8G8B8A8_UNORM") << ")\n";
        std::cerr << "  bytes per frame:  " << bytesPerFrame << " bytes ("
                  << (bytesPerFrame / (1024.0 * 1024.0)) << " MiB)\n\n";

        std::cerr << "per-iteration timing (ns -> GB/s):\n";
        std::cerr << "  " << std::left << std::setw(8) << "iter"
                  << std::right << std::setw(14) << "time (ns)"
                  << std::setw(16) << "throughput (GB/s)" << "\n";
        std::cerr << "  " << std::string(38, '-') << "\n";

        for (size_t i = 0; i < copyTimesNs.size(); ++i) {
            const uint64_t ns = copyTimesNs[i];
            const double gb_per_s = (ns > 0) ? (static_cast<double>(bytesPerFrame) / static_cast<double>(ns)) : 0.0;
            std::cerr << "  " << std::left << std::setw(8) << i
                      << std::right << std::setw(14) << ns
                      << std::setw(16) << std::fixed << std::setprecision(2) << gb_per_s << "\n";
        }

        double p50 = percentile(sorted, 50.0);
        double p90 = percentile(sorted, 90.0);
        double p95 = percentile(sorted, 95.0);
        double p99 = percentile(sorted, 99.0);
        double min_ns = static_cast<double>(sorted.front());
        double max_ns = static_cast<double>(sorted.back());
        double sum_ns = 0.0;
        for (uint64_t v : sorted) sum_ns += static_cast<double>(v);
        double mean_ns = sum_ns / static_cast<double>(sorted.size());

        std::cerr << "\nsummary percentiles (ns -> GB/s):\n";
        auto printPct = [&](const char* label, double ns) {
            double gb_s = (ns > 0) ? (static_cast<double>(bytesPerFrame) / ns) : 0.0;
            std::cerr << "  " << std::left << std::setw(8) << label
                      << std::right << std::setw(14) << std::fixed << std::setprecision(2) << ns << " ns"
                      << std::setw(16) << gb_s << " GB/s\n";
        };
        printPct("min:", min_ns);
        printPct("p50:", p50);
        printPct("p90:", p90);
        printPct("p95:", p95);
        printPct("p99:", p99);
        printPct("max:", max_ns);
        printPct("mean:", mean_ns);
    }

    auto makeDeviceSelector(const std::string& gpuName) {
        return [gpuName](const vk::VulkanInstanceFuncs& fi,
                         const std::vector<VkPhysicalDevice>& devices) -> VkPhysicalDevice {
            for (const VkPhysicalDevice& device : devices) {
                VkPhysicalDeviceProperties2 props{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
                };
                fi.GetPhysicalDeviceProperties2(device, &props);
                auto& properties = props.properties;
                std::array<char, 256> devname = std::to_array(properties.deviceName);
                devname.at(255) = '\0';
                if (std::string(devname.data()) == gpuName)
                    return device;
            }
            throw ls::error("failed to find specified GPU: " + gpuName);
        };
    }
}

int copybench::run(const Options& opts) {
    try {
        if (opts.timing_csv.has_value()) {
            ::setenv("LSFGVK_TIMING", "1", 1);
            ::setenv("LSFGVK_TIMING_CSV", opts.timing_csv->c_str(), 1);
        }

        if (opts.width <= 0 || opts.height <= 0)
            throw ls::error("width and height must be positive integers");
        if (opts.iters <= 0)
            throw ls::error("iterations must be a positive integer");
        if (opts.render_gpu.empty())
            throw ls::error("--render-gpu is required");
        if (opts.gpu.empty())
            throw ls::error("--gpu is required");

        const VkExtent2D extent{
            static_cast<uint32_t>(opts.width),
            static_cast<uint32_t>(opts.height)
        };
        const VkFormat format = opts.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

        const vk::Vulkan renderVk{
            "lsfg-vk-copybench", vk::version{2, 0, 0},
            "lsfg-vk-copybench-engine", vk::version{2, 0, 0},
            makeDeviceSelector(opts.render_gpu),
            false, std::nullopt, std::nullopt, true
        };

        const vk::Vulkan processVk{
            "lsfg-vk-copybench", vk::version{2, 0, 0},
            "lsfg-vk-copybench-engine", vk::version{2, 0, 0},
            makeDeviceSelector(opts.gpu),
            false, std::nullopt, std::nullopt, true
        };

        const std::string renderDevName = deviceName(renderVk);
        const std::string processDevName = deviceName(processVk);
        const auto renderUUID = renderVk.deviceUUID();
        const auto processUUID = processVk.deviceUUID();

        std::cerr << "render GPU (exporter): " << renderDevName << " [" << uuidToHex(renderUUID) << "]\n";
        std::cerr << "process GPU (importer): " << processDevName << " [" << uuidToHex(processUUID) << "]\n";
        std::cerr << "format: " << (opts.hdr ? "HDR (R16G16B16A16_SFLOAT)" : "SDR (R8G8B8A8_UNORM)") << "\n";
        std::cerr << "extent: " << extent.width << "x" << extent.height << "\n";
        std::cerr << "iterations: " << opts.iters << "\n";

        const vk::DeviceExchangeCaps renderCaps = renderVk.exchangeCaps(format);
        const vk::DeviceExchangeCaps processCaps = processVk.exchangeCaps(format);

        constexpr VkFormatFeatureFlags2 usageNeeds =
            VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
            | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
            | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
            | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;

        const vk::DeviceExchangeCaps processingCapsLinear{
            { format, {{ vk::EXCHANGE_MODIFIER_LINEAR, usageNeeds }} }
        };
        const auto layout = vk::negotiateExchangeLayout(renderCaps, processingCapsLinear, format, usageNeeds);
        if (layout.kind() != vk::ExchangeLayoutKind::LinearFallback)
            throw ls::error("copybench cross-device transport supports LINEAR exchange layouts only");
        const uint64_t negotiatedModifier = layout.modifier;

        std::cerr << "negotiated modifier: 0x" << std::hex << negotiatedModifier << std::dec
                  << " (LINEAR fallback)\n";

        const vk::ImageLayout exportLayout{
            .mode = vk::ImageMode::Linear,
            .drmModifier = negotiatedModifier,
            .rowPitch = 0
        };

        int exportFd = -1;
        const vk::Image exportImage{renderVk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            std::nullopt, &exportFd, exportLayout};

        if (exportFd < 0)
            throw ls::error("failed to export dma-buf from render device");

        const auto exportDesc = exportImage.exportDmaBuf(renderVk);
        std::cerr << "exported dma-buf: fd=" << exportDesc.fd
                  << ", size=" << exportDesc.allocationSize
                  << ", rowPitch=" << exportDesc.rowPitch << "\n";

        const vk::ImageLayout importLayout{
            .mode = vk::ImageMode::Linear,
            .drmModifier = negotiatedModifier,
            .rowPitch = exportDesc.rowPitch
        };

        const vk::Image importImage{processVk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            exportDesc.fd, std::nullopt, importLayout};

        const vk::Image destImage{processVk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            std::nullopt, std::nullopt, vk::ImageLayout{}};

        const vk::CommandBuffer cmdbuf{processVk};
        vk::TimingRing timingRing{processVk, "copybench", 8};

        std::vector<uint64_t> copyTimesNs;
        copyTimesNs.reserve(opts.iters);

        const bool useGpuTiming = timingRing.enabled();
        (void)useGpuTiming; // GPU timing reserved for future use

        for (int iter = 0; iter < opts.iters; ++iter) {
            const auto cpuStart = std::chrono::steady_clock::now();

            cmdbuf.begin(processVk);

            if (useGpuTiming) {
                timingRing.resetFrame(cmdbuf.handle(), static_cast<uint64_t>(iter));
            }

            VkImageMemoryBarrier barriers[2] = {
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = (iter == 0) ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .oldLayout = (iter == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = importImage.handle(),
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1
                    }
                },
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = (iter == 0) ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = (iter == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = destImage.handle(),
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1
                    }
                }
            };
            processVk.df().CmdPipelineBarrier(
                cmdbuf.handle(),
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 2, barriers
            );

            if (useGpuTiming) {
                const uint32_t queryIdx = static_cast<uint32_t>(vk::TimingRing::Stage::CopyInStart) * 2;
                processVk.df().CmdWriteTimestamp(cmdbuf.handle(),
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    timingRing.pool(), queryIdx);
            }

            VkImageCopy copyRegion{
                .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .srcOffset = {0, 0, 0},
                .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .dstOffset = {0, 0, 0},
                .extent = {extent.width, extent.height, 1}
            };

            processVk.df().CmdCopyImage(
                cmdbuf.handle(),
                importImage.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                destImage.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copyRegion
            );

            if (useGpuTiming) {
                const uint32_t queryIdx = static_cast<uint32_t>(vk::TimingRing::Stage::CopyInStart) * 2 + 1;
                processVk.df().CmdWriteTimestamp(cmdbuf.handle(),
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    timingRing.pool(), queryIdx);
            }

            cmdbuf.end(processVk);
            cmdbuf.submit(processVk);
            processVk.df().DeviceWaitIdle(processVk.dev());

            const auto cpuEnd = std::chrono::steady_clock::now();
            const uint64_t cpuNs = std::chrono::duration_cast<std::chrono::nanoseconds>(cpuEnd - cpuStart).count();
            copyTimesNs.push_back(cpuNs);
        }

        std::cerr << "\nCPU timing (submit + wait):\n";
        printCopySummary(copyTimesNs, extent.width, extent.height, opts.hdr);

        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}