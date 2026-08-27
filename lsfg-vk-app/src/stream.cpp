/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-app/stream.hpp"

#include "lsfg-vk-common/ipc/protocol.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"

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
void runStream(Connection& conn, StreamState& state, const std::atomic<bool>& stop) {
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

    // 2. NEGOTIATED: LINEAR modifier + 256-byte aligned pitch for the
    //    R8G8B8A8 (4 Bpp) staging format, matching image.cpp's rowPitch.
    const uint32_t rowPitch = ((hello->width * 4) + 255) / 256 * 256;
    const uint64_t allocationSize = static_cast<uint64_t>(rowPitch) * hello->height;
    conn.send(Negotiated{
        .modifier = vk::EXCHANGE_MODIFIER_LINEAR,
        .rowPitch = rowPitch,
        .allocationSize = allocationSize
    });

    // 3. two STAGING messages: import the dma-buf fds, keep them owned ------
    for (size_t i = 0; i < 2; ++i) {
        auto stagingMsg = recvStop(conn, stop);
        if (!stagingMsg)
            return;
        if (!std::holds_alternative<Staging>(*stagingMsg))
            throw ls::error(std::string("expected STAGING, got ") + nameOf(typeOf(*stagingMsg)));
        const int fd = conn.takeReceivedFd();
        if (fd < 0)
            throw ls::error("STAGING arrived without its fd");
        if (i < state.stagingFds.size())
            state.stagingFds.at(i) = fd;
    }
    state.stagingCount = 2;

    // 4. READY: stream is live -------------------------------------------
    conn.send(Ready{});

    // 5. steady state: per FRAME consume the sync fd and ack the slot ------
    while (!stop.load(std::memory_order_relaxed)) {
        auto frameMsg = recvStop(conn, stop);
        if (!frameMsg)
            break;
        const auto* frame = std::get_if<Frame>(&*frameMsg);
        if (!frame)
            throw ls::error(std::string("expected FRAME, got ") + nameOf(typeOf(*frameMsg)));

        // take the sync fd, close it (the skeleton does not re-import it),
        // and release the staging slot back to the layer for backpressure
        const int syncFd = conn.takeReceivedFd();
        if (syncFd >= 0)
            ::close(syncFd);
        conn.send(Release{ frame->stagingIdx });
    }
}
}  // namespace ls::ipc
