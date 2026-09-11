/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "debug.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
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
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <unistd.h>

using namespace lsfgvk::cli;
using namespace lsfgvk::cli::debug;

namespace {
    /// formats a 16-byte device uuid as 32 lowercase hex characters
    /// (mirrors the layer's todo-13 log-line contract)
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

    /// queries the device name of the wrapped physical device
    std::string deviceName(const vk::Vulkan& vk) {
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);

        std::array<char, 256> devname = std::to_array(props.properties.deviceName);
        devname.at(255) = '\0'; // ensure null-termination
        // NOLINTNEXTLINE(modernize-return-braced-init-list, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        return std::string(devname.data());
    }

    /// uploads an image from a dds file
    void upload_image(const vk::Vulkan& vk,
            const vk::Image& image, const std::string& path) {
        // read image bytecode
        std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
        if (!file.is_open())
            throw ls::error("ifstream::ifstream() failed");

        std::streamsize size = static_cast<std::streamsize>(file.tellg());
        size -= 124 + 4; // dds header and magic bytes

        std::vector<char> code(static_cast<size_t>(size));
        file.seekg(124 + 4, std::ios::beg);
        if (!file.read(code.data(), size))
            throw ls::error("ifstream::read() failed");

        file.close();

        // upload to image
        const vk::Buffer stagingbuf{vk, code.data(), code.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT};

        const vk::CommandBuffer cmdbuf{vk};
        cmdbuf.begin(vk);
        cmdbuf.copyBufferToImage(vk, stagingbuf, image);
        cmdbuf.end(vk);

        const vk::TimelineSemaphore sema{vk, 0};
        cmdbuf.submit(vk); // synchronous: fences internally
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

int debug::run(const Options& opts) {
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
        const VkExtent2D extent{
            static_cast<uint32_t>(opts.width),
            static_cast<uint32_t>(opts.height)
        };
        const VkFormat format = opts.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        if (!std::filesystem::exists(opts.path))
            throw ls::error("debug path does not exist: " + opts.path.string());
        std::vector<std::filesystem::path> paths{};
        for (const auto& entry : std::filesystem::directory_iterator(opts.path))
            paths.push_back(entry.path());
        std::ranges::sort(paths, [](const std::filesystem::path& a, const std::filesystem::path& b) {
            auto fa = a.filename().string();
            auto fb = b.filename().string();

            auto norm_a = fa.find_first_of('.');
            if (norm_a == std::string::npos)
                throw ls::error("invalid debug file name: " + fa);
            auto norm_b = fb.find_first_of('.');
            if (norm_b == std::string::npos)
                throw ls::error("invalid debug file name: " + fb);

            return std::stoi(fa.substr(0, norm_a)) < std::stoi(fb.substr(0, norm_b));
        });

        // dma-buf extensions are needed on BOTH sides of a cross-device
        // exchange: the exporter (--render-gpu) must export dma-buf exchange
        // images, the processing device (-g) must import them; equal or unset
        // keeps both legacy extension sets byte-identical
        const bool needs_dma_buf =
            opts.render_gpu.has_value() && opts.render_gpu != opts.gpu;

        // create instance
        const vk::Vulkan vk{
            "lsfg-vk-debug", vk::version{2, 0, 0},
            "lsfg-vk-debug-engine", vk::version{2, 0, 0},
            [opts](const vk::VulkanInstanceFuncs fi,
                    const std::vector<VkPhysicalDevice>& devices) {
                // exporter/frame-source side: --render-gpu overrides --gpu
                const std::optional<std::string> render_gpu{
                    opts.render_gpu ? opts.render_gpu : opts.gpu};
                if (!render_gpu.has_value())
                    return devices.front();

                for (const VkPhysicalDevice& device : devices) {
                    VkPhysicalDeviceProperties2 props{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
                    };
                    fi.GetPhysicalDeviceProperties2(device, &props);

                    auto& properties = props.properties;
                    std::array<char, 256> devname = std::to_array(properties.deviceName);
                    devname.at(255) = '\0'; // ensure null-termination

                    if (std::string(devname.data()) == *render_gpu)
                        return device;
                }

                throw ls::error("failed to find specified GPU: " + *render_gpu);
            },
            false, std::nullopt, std::nullopt,
            needs_dma_buf
        };

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
            dll, opts.allow_fp16, needs_dma_buf
        };

        // dual-gpu detection: exporter (--render-gpu side) vs processing (-g side)
        const bool crossDevice = lsfgvk.selectedDeviceUUID() != vk.deviceUUID();

        // caller-side layout negotiation per the openContext contract. the
        // cli harness mirrors the layer's linear-only dual-gpu transport:
        // negotiate against a LINEAR proxy for the processing device and
        // require the LINEAR fallback (rig reality: Intel∩AMD proper-modifier
        // intersection is empty)
        uint64_t negotiatedModifier =
            lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE;
        if (crossDevice) {
            constexpr VkFormatFeatureFlags2 usageNeeds =
                VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
                | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
                | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
                | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
            const auto gameCaps = vk.exchangeCaps(format);
            const vk::DeviceExchangeCaps processingCaps{
                { format,
                    {{ vk::EXCHANGE_MODIFIER_LINEAR, usageNeeds }} }
            };
            const auto layout = vk::negotiateExchangeLayout(
                gameCaps, processingCaps,
                format, usageNeeds);
            if (layout.kind() != vk::ExchangeLayoutKind::LinearFallback)
                throw ls::error("debug tool's dual-gpu transport supports"
                    " linear exchange layouts only");
            negotiatedModifier = layout.modifier;
        }

        // same-device keeps the legacy optimal-tiled OPAQUE_FD path
        // byte-identically; cross-device creates LINEAR dma-buf exchange
        // images whose fds are exported into descriptors below
        const vk::ImageLayout exchangeLayout = crossDevice
            ? vk::ImageLayout{ .mode = vk::ImageMode::Linear }
            : vk::ImageLayout{};

        std::pair<int, int> srcfds{};
        // the exportFd out-points are load-bearing in dual-gpu mode too: they
        // force the DMA_BUF external-memory chain onto the exchange images at
        // creation time; in cross-device mode the allocation-time exports are
        // closed unused below, the descriptor fds are exported separately
        const vk::Image frame_0{vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &srcfds.first, exchangeLayout};
        const vk::Image frame_1{vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &srcfds.second, exchangeLayout};

        std::vector<vk::Image> destimgs{};
        std::vector<int> destfds{};
        for (int i = 0; i < (opts.multiplier - 1); i++) {
            int fd{};
            destimgs.emplace_back(vk,
                extent, format,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                std::nullopt,
                &fd,
                exchangeLayout
            );
            destfds.push_back(fd);
        }

        int syncfd{};
        const vk::TimelineSemaphore sync{vk, 0, std::nullopt, &syncfd};

        // exchange descriptors: same-device wraps the legacy opaque fds into
        // EXCHANGE_MODIFIER_OPAQUE sentinel descriptors (allocationSize and
        // rowPitch ignored); cross-device exports each linear exchange image
        // as dma-buf carrying the negotiated layout. exported fds are
        // CONSUMED by successful imports (transfer semantics), so they are
        // intentionally not closed here afterwards
        std::vector<vk::ExchangeDescriptor> srcDescs{};
        std::vector<vk::ExchangeDescriptor> destDescs{};
        srcDescs.reserve(2);
        destDescs.reserve(destfds.size());
        if (crossDevice) {
            // the allocation-time exports are superseded by the explicit
            // dma-buf exports below; close the scratch fds unused
            close(srcfds.first);
            close(srcfds.second);
            for (const int fd : destfds)
                close(fd);

            for (const vk::Image* img : { &frame_0, &frame_1 }) {
                const auto exp = img->exportDmaBuf(vk);
                srcDescs.push_back({ exp.fd, exp.allocationSize,
                    exp.rowPitch, negotiatedModifier,
                    format, extent });
            }
            for (const auto& img : destimgs) {
                const auto exp = img.exportDmaBuf(vk);
                destDescs.push_back({ exp.fd, exp.allocationSize,
                    exp.rowPitch, negotiatedModifier,
                    format, extent });
            }
        } else {
            srcDescs.push_back({ srcfds.first, 0, 0,
                lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                format, extent });
            srcDescs.push_back({ srcfds.second, 0, 0,
                lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                format, extent });
            for (const int fd : destfds)
                destDescs.push_back({ fd, 0, 0,
                    lsfgvk::backend::EXCHANGE_MODIFIER_OPAQUE,
                    format, extent });
        }

        lsfgvk::backend::Context& lsfgvk_ctx = lsfgvk.openContext(
            srcDescs, destDescs, vk.deviceUUID(),
            negotiatedModifier,
            syncfd, extent.width, extent.height,
            opts.hdr, 1.0F / opts.flow, opts.performance_mode
        );

        // mirror the layer's dual-gpu mode log lines (todo-13 contract) so
        // the e2e matrix runner can apply identical grep gates to
        // cli-harness runs, which do not load the layer itself
        if (lsfgvk.isCrossDevice(lsfgvk_ctx))
            std::cerr << "lsfg-vk: processing on '"
                << uuidToHex(lsfgvk.selectedDeviceUUID())
                << "' (game on '" << deviceName(vk) << "')\n";
        else
            std::cerr << "lsfg-vk: frame generation on the game's own device '"
                << deviceName(vk) << "'\n";

        // render destination images
        const bool ctxCrossDevice = lsfgvk.isCrossDevice(lsfgvk_ctx);
        size_t idx{1};
        size_t waits{};
        for (size_t j = 0; j < paths.size(); j++) {
            upload_image(vk,
                j % 2 == 0 ? frame_0 : frame_1,
                paths.at(j).string());

            if (!ctxCrossDevice) {
                sync.signal(vk, idx++);
                lsfgvk.scheduleFrames(lsfgvk_ctx);

                for (size_t i = 0; i < destimgs.size(); i++) {
                    auto success = sync.wait(vk, idx++);
                    if (!success)
                        throw ls::error("failed to wait for frame");
                    std::cout << "lsfg-vk-debug: wait ok " << ++waits << "\n";
                }
            } else {
                // cli-harness semantics for dual-gpu contexts: no blit
                // pipeline consumes generated frames here, so (a)
                // captureReadyFd=-1 reports the capture as already complete
                // (the backend emulates an already-signaled sync fd via an
                // empty submit; upload_image's synchronous submit provides
                // the ordering the real handshake would), and (b) each
                // returned done-fd is observed via poll(2) without consuming
                // its payload, then closed immediately - nothing imports it
                // and leaking would exhaust fds on long runs
                const auto doneFds = lsfgvk.scheduleFrames(lsfgvk_ctx, -1);
                for (const int fd : doneFds) {
                    // NOLINTNEXTLINE(misc-include-cleaner)
                    pollfd pfd{ .fd = fd, .events = POLLIN };
                    // NOLINTNEXTLINE(misc-include-cleaner)
                    if (poll(&pfd, 1, 30 * 1000) != 1
                            || !(pfd.revents & POLLIN))
                        throw ls::error("timeout waiting for a generated"
                            " frame's done-fd");
                    close(fd);
                    std::cout << "lsfg-vk-debug: wait ok " << ++waits << "\n";
                }
            }
        }

        // deinitialize lsfg-vk
        lsfgvk.closeContext(lsfgvk_ctx);

        // print timing summary if CSV was requested
        if (opts.timing_csv.has_value()) {
            printTimingSummary(*opts.timing_csv);
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
