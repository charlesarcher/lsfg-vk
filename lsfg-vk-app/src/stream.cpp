/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-app/stream.hpp"
#include "lsfg-vk-app/presentation.hpp"

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/protocol.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <atomic>
#include <vector>
#include <span>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ls::ipc;

namespace {
    /// bound the blocking recv() so a SIGINT (EINTR) or a silent peer can never
    /// hang the accept loop: SO_RCVTIMEO makes recvmsg return within this span
    /// when no data is ready.
    constexpr std::chrono::milliseconds RECV_TIMEOUT{500};
    /// deadline handed to Connection::receive so a single message cannot block
    /// longer than this; checked against the shutdown flag after any failure.
    constexpr std::chrono::milliseconds RECEIVE_DEADLINE{2000};
    /// poll backstop between messages (also wakes the loop to re-check stop)
    constexpr int IDLE_POLL_MS{500};

    /// format a VkFormat as a readable name; unknown formats fall back to the
    /// integer value rather than guessing (matches the "tiny switch" contract)
    std::string formatName(uint32_t format) {
        switch (static_cast<VkFormat>(format)) {
            case VK_FORMAT_R8G8B8A8_UNORM:   return "R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB:    return "R8G8B8A8_SRGB";
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT_PACK32";
            default:
                return "VkFormat(" + std::to_string(format) + ")";
        }
    }

    /// receive the next message, breaking out cleanly when the shutdown flag is
    /// set or the peer has gone away. poll() first so an idle loop re-checks the
    /// flag (and detects POLLHUP); a blocking receive is bounded by SO_RCVTIMEO
    /// (set on the socket) and the receive deadline.
    /// @return the message, or std::nullopt to end the stream (SIGINT / peer close)
    std::optional<Message> recvStop(Connection& conn, const std::atomic<bool>& stop) {
        while (true) {
            if (stop.load(std::memory_order_relaxed))
                return std::nullopt;

            pollfd pfd{};
            pfd.fd = conn.fd();
            pfd.events = POLLIN;
            const int r = ::poll(&pfd, 1, IDLE_POLL_MS);
            if (r < 0) {
                if (errno == EINTR) continue;
                throw socket_error("poll() on stream", errno);
            }
            if (r == 0) continue;                                   // backstop timeout
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
                return std::nullopt;                                // peer gone

            try {
                return conn.receive(RECEIVE_DEADLINE);
            } catch (const std::exception&) {
                if (stop.load(std::memory_order_relaxed))
                    return std::nullopt;                            // SIGINT / timeout
                throw;
            }
        }
    }

    /// set SO_RCVTIMEO on a socket fd
    void setRecvTimeout(int fd, std::chrono::milliseconds timeout) {
        timeval tv{};
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
        tv.tv_sec = static_cast<time_t>(micros / 1'000'000);
        tv.tv_usec = static_cast<suseconds_t>(micros % 1'000'000);
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            throw socket_error("setsockopt(SO_RCVTIMEO) on stream", errno);
    }
}

StreamState::~StreamState() {
    for (int& fd : this->stagingFds)
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
}

namespace ls::ipc {
void runStream(Connection& conn, StreamState& state, const std::atomic<bool>& stop,
    const vk::Vulkan& vk, lsfgvk::backend::Instance& backend, const ls::GameConf& conf,
    std::string_view session) {
    // own the backend for this stream's lifetime (used by the context janitor
    // below on erase); points to the process-level instance in main.cpp, so it
    // outlives this stream.
    state.backend = &backend;
    // bound blocking recv so SIGINT/EINTR cannot hang the stream forever
    setRecvTimeout(conn.fd(), RECV_TIMEOUT);

    // 1. HELLO: game identity + swapchain size/format --------------------
    auto helloMsg = recvStop(conn, stop);
    if (!helloMsg)
        return;
    const auto* hello = std::get_if<Hello>(&*helloMsg);
    if (!hello)
        throw ls::error(std::string("expected HELLO, got ") + nameOf(typeOf(*helloMsg)));
    if (hello->protoVersion != PROTO_VERSION)
        throw ls::error("unsupported protocol version " + std::to_string(hello->protoVersion)
            + " (this app speaks " + std::to_string(PROTO_VERSION) + ")");

    std::cerr << "lsfg-vk-app: stream from '" << helloDeviceName(*hello) << "' "
              << hello->width << "x" << hello->height << " "
              << formatName(hello->vkFormat) << "\n";

    // 2. NEGOTIATED: the staging images the layer hands off are ALWAYS
    //    R8G8B8A8_UNORM (capture_context.cpp:151), 4 Bpp. negotiate the exchange
    //    layout from the app device's TRUE caps and reply with the negotiated
    //    modifier + 256-byte-aligned pitch + allocation size.
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t w = hello->width, h = hello->height;
    const uint32_t rowPitch = ((w * 4u) + 255u) / 256u * 256u;
    const auto appCaps = vk.exchangeCaps(fmt);
    const VkFormatFeatureFlags2 usageNeeds =
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    // cross-device exchange shares no proper DRM modifier; negotiate against a
    // LINEAR-only proxy (swapchain.cpp pattern) to force modifier == LINEAR.
    const vk::DeviceExchangeCaps gameProxyCaps{
        { fmt, {{ vk::EXCHANGE_MODIFIER_LINEAR, usageNeeds }} }
    };
    const auto neg = vk::negotiateExchangeLayout(appCaps, gameProxyCaps, fmt, usageNeeds);
    state.negotiatedModifier = neg.modifier;
    state.width = w; state.height = h; state.gameUuid = hello->gameUuid;
    conn.send(ls::ipc::Negotiated{
        .modifier = neg.modifier,
        .rowPitch = rowPitch,
        .allocationSize = static_cast<uint64_t>(rowPitch) * h
    });

    const vk::ImageLayout layout{
        .mode = (neg.modifier == vk::EXCHANGE_MODIFIER_LINEAR) ? vk::ImageMode::Linear
                                                               : vk::ImageMode::DrmModifier,
        .drmModifier = neg.modifier,
        .rowPitch = rowPitch
    };
    const VkImageUsageFlags imgUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // 3. two STAGING messages: import the dma-buf fds into B-local source
    //    images. the received fd is consumed by the import; we dup() it once so
    //    one copy is imported (image lives in the app) and one copy is handed to
    //    the backend as the source descriptor fd (the backend imports the dup;
    //    the kernel duplicated the fd on receive, no re-export roundtrip).
    std::vector<vk::ExchangeDescriptor> sourceDescs;
    std::vector<int> handedSourceFds;
    for (size_t i = 0; i < 2; ++i) {
        auto stagingMsg = recvStop(conn, stop);
        if (!stagingMsg)
            return;
        if (!std::holds_alternative<Staging>(*stagingMsg))
            throw ls::error(std::string("expected STAGING, got ")
                + nameOf(typeOf(*stagingMsg)));
        const int received = conn.takeReceivedFd();
        if (received < 0)
            throw ls::error("STAGING arrived without its fd");
        const int dupForDescriptor = ::dup(received);
        if (dupForDescriptor < 0) {
            ::close(received);
            throw ls::error("dup() failed for staging descriptor fd");
        }
        // import consumes the received fd on success (closed by the image); on
        // failure the image closes it for us.
        state.sourceImages.at(i).emplace(vk, VkExtent2D{ w, h }, fmt, imgUsage,
            received /*importFd*/, std::nullopt /*exportFd*/, layout);
        handedSourceFds.push_back(dupForDescriptor);
        sourceDescs.push_back({
            .fd = dupForDescriptor,
            .allocationSize = static_cast<VkDeviceSize>(rowPitch) * h,
            .rowPitch = rowPitch,
            .modifier = neg.modifier,
            .format = fmt,
            .extent = VkExtent2D{ w, h }
        });
    }
    state.stagingCount = 2;

    // 4. create two B-local destination images natively, self-export each, and
    //    hand the backend a dup of the export fd as the destination descriptor.
    std::vector<vk::ExchangeDescriptor> destDescs;
    std::vector<int> handedDestFds;
    for (size_t i = 0; i < 2; ++i) {
        state.destinationImages.at(i).emplace(vk, VkExtent2D{ w, h }, fmt, imgUsage,
            std::nullopt /*importFd*/, std::nullopt /*exportFd*/, layout);
        auto exp = state.destinationImages.at(i).mut().exportDmaBuf(vk);
        const int dupForDescriptor = ::dup(exp.fd);
        if (dupForDescriptor < 0) {
            ::close(exp.fd);
            throw ls::error("dup() failed for destination descriptor fd");
        }
        // exp.fd is unowned (image made with exportFd=nullopt) and the backend
        // only consumes the dup: closing it here avoids a 1-fd/stream leak.
        ::close(exp.fd);
        handedDestFds.push_back(dupForDescriptor);
        destDescs.push_back({
            .fd = dupForDescriptor,
            .allocationSize = exp.allocationSize,
            .rowPitch = exp.rowPitch,
            .modifier = neg.modifier,
            .format = fmt,
            .extent = VkExtent2D{ w, h }
        });
    }

    // 5. open the cross-device frame-gen context with EXACTLY 2 source + 2
    //    descriptors. hdr=false because the exchange images are R8G8B8A8_UNORM
    //    (lsfgvk.hpp:103-105: false => R8G8B8A8_UNORM, true => RGBA16F). the
    //    backend infers the format from hdr, so a wrong hdr would reject the
    //    source descriptors' format. syncFd is ignored on the cross-device path.
    //    on any throw before a successful import, close the handed dups (the
    //    backend has consumed none yet).
    try {
        auto& ctx = backend.openContext(
            std::span<const vk::ExchangeDescriptor>(sourceDescs),
            std::span<const vk::ExchangeDescriptor>(destDescs),
            state.gameUuid, neg.modifier, -1 /*syncFd ignored cross-device*/,
            w, h, false /*hdr: R8G8B8A8 staging, never format>57*/,
            1.0F / conf.flow_scale, conf.performance_mode);
        if (!backend.isCrossDevice(ctx))
            throw ls::error("backend context is not cross-device");
        state.context = ls::owned_ptr<ls::R<lsfgvk::backend::Context>>(
            new ls::R<lsfgvk::backend::Context>(ctx),
            [backend = &backend](ls::R<lsfgvk::backend::Context>& c) {
                backend->closeContext(c);
            });
        std::cerr << "lsfg-vk-app: context created on '"
                  << (backend.selectedDeviceSupportsDmaBuf() ? "dma-buf" : "?")
                  << "' cross-device=" << backend.isCrossDevice(ctx) << "\n";
    } catch (const std::exception& e) {
        for (int fd : handedSourceFds) if (fd >= 0) ::close(fd);
        for (int fd : handedDestFds) if (fd >= 0) ::close(fd);
        throw ls::error("failed to open backend context", e);
    }

    // 6. READY: stream is live.
    conn.send(ls::ipc::Ready{});

    // 7. steady state: present the backend-generated + real captured frames on
    //    the output swapchain (task 8). Hand off to runPresent, which owns the
    //    window, surface and swapchain for this stream and tears them all down
    //    on every return path. The handshake above already opened the backend
    //    context that runPresent drives via backend.scheduleFrames.
    ls::presentation::runPresent(conn, state, vk, backend, conf, session, stop);
}
}  // namespace ls::ipc
