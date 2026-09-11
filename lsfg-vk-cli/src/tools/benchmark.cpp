/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "benchmark.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <time.h>
#include <bits/time.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk::cli;
using namespace lsfgvk::cli::benchmark;

namespace {
    // get current time in milliseconds
    uint64_t ms() {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);

        return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    }

    // compute percentile from sorted values
    double percentile(const std::vector<uint64_t>& values, double p) {
        if (values.empty()) return 0.0;
        size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size()))) - 1;
        idx = std::min(idx, values.size() - 1);
        return static_cast<double>(values[idx]);
    }

    // read timing CSV and compute per-stage percentiles
    void printTimingSummary(const std::string& csvPath) {
        std::ifstream file(csvPath);
        if (!file.is_open()) {
            std::cerr << "warning: could not open timing CSV for summary: " << csvPath << "\n";
            return;
        }

        std::string line;
        // skip header
        std::getline(file, line);

        std::vector<uint64_t> copyIn, flow, generate, copyOut, total, gameIn, gameOut;

        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string field;
            std::vector<std::string> fields;

            while (std::getline(ss, field, ',')) {
                fields.push_back(field);
            }

            if (fields.size() < 9) continue;

            try {
                copyIn.push_back(std::stoull(fields[2]));
                flow.push_back(std::stoull(fields[3]));
                generate.push_back(std::stoull(fields[4]));
                copyOut.push_back(std::stoull(fields[5]));
                total.push_back(std::stoull(fields[6]));
                gameIn.push_back(std::stoull(fields[7]));
                gameOut.push_back(std::stoull(fields[8]));
            } catch (...) {
                continue;
            }
        }

        auto sortVec = [](std::vector<uint64_t>& v) {
            std::sort(v.begin(), v.end());
        };
        sortVec(copyIn);
        sortVec(flow);
        sortVec(generate);
        sortVec(copyOut);
        sortVec(total);
        sortVec(gameIn);
        sortVec(gameOut);

        auto printStage = [](const char* name, const std::vector<uint64_t>& v) {
            if (v.empty()) return;
            double p50 = percentile(v, 50.0);
            double p95 = percentile(v, 95.0);
            std::cerr << "  " << std::left << std::setw(16) << name
                      << "p50: " << std::right << std::setw(10) << std::fixed << std::setprecision(2) << (p50 / 1e6) << " ms"
                      << "  p95: " << std::setw(10) << (p95 / 1e6) << " ms\n";
        };

        std::cerr << "\ntiming summary (percentiles in ms):\n";
        printStage("copy_in:", copyIn);
        printStage("flow:", flow);
        printStage("generate:", generate);
        printStage("copy_out:", copyOut);
        printStage("total:", total);
        printStage("game_copy_in:", gameIn);
        printStage("game_copy_out:", gameOut);
    }
}

int benchmark::run(const Options& opts) {
    try {
        // Set timing environment variables if requested
        if (opts.timing_csv.has_value()) {
            ::setenv("LSFGVK_TIMING", "1", 1);
            ::setenv("LSFGVK_TIMING_CSV", opts.timing_csv->c_str(), 1);
        }

        // parse options
        if (opts.flow < 0.25F || opts.flow > 1.0F)
            throw ls::error("flow scale must be between 0.25 and 1.0");
        if (opts.multiplier < 2)
            throw ls::error("multiplier must be 2 or greater");
        if (opts.width <= 0 || opts.height <= 0)
            throw ls::error("width and height must be positive integers");
        if (opts.duration <= 0)
            throw ls::error("duration must be a positive integer");
        const VkExtent2D extent{
            static_cast<uint32_t>(opts.width),
            static_cast<uint32_t>(opts.height)
        };
        const VkFormat format = opts.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

        // create instance
        const vk::Vulkan vk{
            "lsfg-vk-debug", vk::version{2, 0, 0},
            "lsfg-vk-debug-engine", vk::version{2, 0, 0},
            [opts](const vk::VulkanInstanceFuncs fi,
                    const std::vector<VkPhysicalDevice>& devices) {
                if (!opts.gpu.has_value())
                    return devices.front();

                for (const VkPhysicalDevice& device : devices) {
                    VkPhysicalDeviceProperties2 props{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
                    };
                    fi.GetPhysicalDeviceProperties2(device, &props);

                    auto& properties = props.properties;
                    std::array<char, 256> devname = std::to_array(properties.deviceName);
                    devname.at(255) = '\0'; // ensure null-termination

                    if (std::string(devname.data()) == *opts.gpu)
                        return device;
                }

                throw ls::error("failed to find specified GPU: " + *opts.gpu);
            }
        };

        std::pair<int, int> srcfds{};
        const vk::Image frame_0{vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &srcfds.first};
        const vk::Image frame_1{vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &srcfds.second};

        std::vector<vk::Image> destimgs{};
        std::vector<int> destfds{};
        for (int i = 0; i < (opts.multiplier - 1); i++) {
            int fd{};
            destimgs.emplace_back(vk,
                extent, format,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                std::nullopt,
                &fd
            );
            destfds.push_back(fd);
        }

        int syncfd{};
        const vk::TimelineSemaphore sync{vk, 0, std::nullopt, &syncfd};

        // initialize backend
        std::string dll{};
        if (opts.dll.has_value())
            dll = *opts.dll;
        else
            dll = ls::findShaderDll();

        lsfgvk::backend::Instance lsfgvk{
            [opts](
                const std::string& gpu_name,
                std::pair<const std::string&, const std::string&>,
                const std::optional<std::string>&
            ) {
                return opts.gpu.value_or(gpu_name) == gpu_name;
            },
            dll, opts.allow_fp16
        };

        // opaque-fd-equivalent descriptors: modifier sentinel marks legacy
        // OPAQUE_FD imports, allocationSize/rowPitch are ignored for those;
        // exporter and processing device coincide here, so this stays same-device
        const std::array<vk::ExchangeDescriptor, 2> srcDescs{{
            { srcfds.first, 0, 0, lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                format, extent },
            { srcfds.second, 0, 0, lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                format, extent }
        }};
        std::vector<vk::ExchangeDescriptor> destDescs{};
        destDescs.reserve(destfds.size());
        for (const int fd : destfds)
            destDescs.push_back({ fd, 0, 0, lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                format, extent });

        lsfgvk::backend::Context& lsfgvk_ctx = lsfgvk.openContext(
            srcDescs, destDescs, vk.deviceUUID(),
            lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
            syncfd, extent.width, extent.height,
            opts.hdr, 1.0F / opts.flow, opts.performance_mode
        );

        // run the benchmark
        size_t iterations{0};
        size_t generated_frames{0};
        size_t total_frames{1};

        uint64_t print_time = ms() + 1000ULL;
        const uint64_t end_time = ms() + static_cast<uint64_t>(opts.duration) * 1000ULL;
        while (ms() < end_time) {
            sync.signal(vk, total_frames++);
            lsfgvk.scheduleFrames(lsfgvk_ctx);

            for (size_t i = 0; i < destimgs.size(); i++) {
                auto success = sync.wait(vk, total_frames++);
                if (!success)
                    throw ls::error("failed to wait for frame");

                generated_frames++;
            }

            iterations++;

            if (ms() >= print_time) {
                print_time += 1000ULL;
                std::cerr << "." << std::flush;
            }
        }

        // output results

        std::cerr << (opts.duration < 40 ? "\r" : "\n");
        std::cerr << "benchmark results (ran for " << opts.duration << " seconds):\n";
        std::cerr << "  iterations:       " << iterations << "\n";
        std::cerr << "  generated frames: " << generated_frames << "\n";
        std::cerr << "  total frames:     " << total_frames << "\n";
        const auto time = static_cast<double>(opts.duration);
        const double fps_generated = static_cast<double>(generated_frames) / time;
        const double fps_total = static_cast<double>(total_frames) / time;
        std::cerr << std::setprecision(2) << std::fixed;
        std::cerr << "  fps (generated):  " << fps_generated << "fps\n";
        std::cerr << "  fps (total):      " << fps_total << "fps\n";

        // print timing summary if CSV was requested
        if (opts.timing_csv.has_value()) {
            printTimingSummary(*opts.timing_csv);
        }

        // deinitialize lsfg-vk
        lsfgvk.closeContext(lsfgvk_ctx);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
