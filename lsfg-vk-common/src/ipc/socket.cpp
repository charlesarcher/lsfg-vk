/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/ipc/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// misc-include-cleaner (LLVM 22) cannot map glibc's public socket/poll/time
// API surface back to public headers: it demands internal bits/* headers
// instead of <sys/socket.h>, <sys/uio.h>, <sys/poll.h> & co. suppressed for
// the whole file rather than littering every syscall line
// NOLINTBEGIN(misc-include-cleaner)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ls::ipc {
    namespace {
        /// size of the fixed frame header: length prefix + magic + msg type
        constexpr size_t HEADER_LEN = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);

        /// append a u32 as explicit little-endian bytes
        void putU32(std::vector<std::byte>& out, uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }

        /// append a u64 as explicit little-endian bytes
        void putU64(std::vector<std::byte>& out, uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }

        /// read a u32 from little-endian bytes
        /// @throws ls::error if fewer than 4 bytes remain
        uint32_t getU32(std::span<const std::byte>& in) {
            if (in.size() < 4) throw ls::error("ipc payload truncated (u32)");
            uint32_t value{};
            for (unsigned i = 0; i < 4; ++i) {
                value |= static_cast<uint32_t>(std::to_integer<uint8_t>(in.front())) << (8 * i);
                in = in.subspan(1);
            }
            return value;
        }

        /// read a u64 from little-endian bytes
        /// @throws ls::error if fewer than 8 bytes remain
        uint64_t getU64(std::span<const std::byte>& in) {
            if (in.size() < 8) throw ls::error("ipc payload truncated (u64)");
            uint64_t value{};
            for (unsigned i = 0; i < 8; ++i) {
                value |= static_cast<uint64_t>(std::to_integer<uint8_t>(in.front())) << (8 * i);
                in = in.subspan(1);
            }
            return value;
        }

        /// pop one payload byte, bounds-checked
        /// @throws ls::error if no bytes remain
        uint8_t getU8(std::span<const std::byte>& in) {
            if (in.empty()) throw ls::error("ipc payload truncated (u8)");
            const auto value = std::to_integer<uint8_t>(in.front());
            in = in.subspan(1);
            return value;
        }

        /// wait until the fd becomes readable or the deadline expires.
        /// the deadline is anchored once at call start so partial reads
        /// cannot extend the total budget
        /// @return true if readable, false if the deadline expired
        /// @throws socket_error on poll failures
        bool pollReadable(int fd, const std::optional<std::chrono::milliseconds>& deadline) {
            std::optional<std::chrono::steady_clock::time_point> endAt;
            if (deadline)
                endAt = std::chrono::steady_clock::now() + *deadline;

            while (true) {
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;

                int timeoutMs = -1;
                if (endAt) {
                    const auto remaining = *endAt - std::chrono::steady_clock::now();
                    if (remaining <= std::chrono::milliseconds::zero()) return false;
                    timeoutMs = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
                }

                const int res = poll(&pfd, 1, timeoutMs);
                if (res < 0) {
                    if (errno == EINTR) continue;
                    throw socket_error("poll() on ipc socket", errno);
                }
                if (res == 0) return false;
                if (pfd.revents & (POLLERR | POLLNVAL))
                    throw socket_error("poll() on ipc socket", ECONNRESET);
                return true; // POLLIN or POLLHUP: data and/or EOF is readable
            }
        }

        /// receive exactly `len` bytes, honoring the deadline across partial
        /// reads. ancillary data is collected along the way; SCM_RIGHTS fds
        /// are appended to `fds` whenever the kernel delivers them
        /// @return true on success, false on clean EOF before any byte of
        ///     this call's first read (i.e. peer closed between messages)
        /// @throws ls::error on EOF mid-message or more than one received fd
        /// @throws socket_error on recv failures or deadline expiry
        bool recvFull(int fd, std::byte* buf, size_t len,
                const std::optional<std::chrono::milliseconds>& deadline,
                std::vector<int>& fds) {
            std::span<std::byte> remaining{buf, len};
            while (!remaining.empty()) {
                if (!pollReadable(fd, deadline)) {
                    if (remaining.size() == len)
                        throw socket_error("recvmsg() on ipc socket (deadline expired)",
                            ETIMEDOUT);
                    throw ls::error("ipc deadline expired mid-message");
                }

                // control buffer on every call: we cannot know which recvmsg
                // consumes the final byte of the message, and plain read()
                // would silently discard the SCM_RIGHTS cmsg
                std::array<std::byte, CMSG_SPACE(sizeof(int))> ctrl{};
                iovec iov{};
                iov.iov_base = remaining.data();
                iov.iov_len = remaining.size();
                msghdr mh{};
                mh.msg_iov = &iov;
                mh.msg_iovlen = 1;
                mh.msg_control = ctrl.data();
                mh.msg_controllen = ctrl.size();

                const ssize_t n = recvmsg(fd, &mh, MSG_NOSIGNAL | MSG_CMSG_CLOEXEC);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    throw socket_error("recvmsg() on ipc socket", errno);
                }
                if (n == 0) {
                    if (remaining.size() == len) return false;
                    throw ls::error("ipc peer closed connection mid-message");
                }

                for (cmsghdr* cmsg = CMSG_FIRSTHDR(&mh); cmsg; cmsg = CMSG_NXTHDR(&mh, cmsg)) {
                    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                        continue;
                    const size_t payloadLen = cmsg->cmsg_len - CMSG_LEN(0);
                    if (payloadLen != sizeof(int))
                        throw ls::error("ipc peer sent an unexpected number of fds in one message");
                    int dupFd{-1};
                    std::memcpy(&dupFd, CMSG_DATA(cmsg), sizeof(int));
                    fds.push_back(dupFd);
                }

                remaining = remaining.subspan(static_cast<size_t>(n));
            }
            return true;
        }
        /// close an fd if it is open, used for strict cleanup paths
        void closeQuiet(int& fd) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    }

    // === protocol.hpp ===

    MsgType typeOf(const Message& msg) {
        return std::visit([](const auto& m) { return messageTypeOf<std::decay_t<decltype(m)>>(); },
            msg);
    }

    Hello makeHello(uint32_t protoVersion, const std::array<uint8_t, UUID_LEN>& gameUuid,
        const std::string& deviceName, uint32_t vkFormat, uint32_t width, uint32_t height) {
        Hello hello{};
        hello.protoVersion = protoVersion;
        hello.gameUuid = gameUuid;
        // truncate at 255 so the final byte stays a NUL terminator
        const size_t nameLen = std::min(deviceName.size(), DEVICE_NAME_LEN - 1);
        deviceName.copy(hello.deviceName.data(), nameLen);
        for (size_t i = nameLen; i < DEVICE_NAME_LEN; ++i) hello.deviceName.at(i) = '\0';
        hello.vkFormat = vkFormat;
        hello.width = width;
        hello.height = height;
        return hello;
    }

    std::string helloDeviceName(const Hello& hello) {
        size_t len = 0;
        while (len < DEVICE_NAME_LEN && hello.deviceName.at(len) != '\0') ++len;
        return {hello.deviceName.data(), len};
    }

    std::vector<std::byte> encodePayload(const Hello& hello) {
        std::vector<std::byte> out{};
        out.reserve(4 + UUID_LEN + DEVICE_NAME_LEN + 12);
        putU32(out, hello.protoVersion);
        for (const uint8_t b : hello.gameUuid) out.push_back(static_cast<std::byte>(b));
        for (const char c : hello.deviceName) out.push_back(static_cast<std::byte>(c));
        putU32(out, hello.vkFormat);
        putU32(out, hello.width);
        putU32(out, hello.height);
        return out;
    }

    Hello decodeHello(const std::span<const std::byte> payload) {
        if (payload.size() != 4 + UUID_LEN + DEVICE_NAME_LEN + 12)
            throw ls::error("malformed HELLO payload: wrong size (" + std::to_string(payload.size())
                + " bytes)");
        std::span<const std::byte> cur = payload;
        Hello hello{};
        hello.protoVersion = getU32(cur);
        for (uint8_t& b : hello.gameUuid) b = getU8(cur);
        for (char& c : hello.deviceName) c = static_cast<char>(getU8(cur));
        hello.vkFormat = getU32(cur);
        hello.width = getU32(cur);
        hello.height = getU32(cur);
        return hello;
    }

    std::vector<std::byte> encodePayload(const Negotiated& negotiated) {
        std::vector<std::byte> out{};
        out.reserve(20);
        putU64(out, negotiated.modifier);
        putU32(out, negotiated.rowPitch);
        putU64(out, negotiated.allocationSize);
        return out;
    }

    Negotiated decodeNegotiated(const std::span<const std::byte> payload) {
        if (payload.size() != 20)
            throw ls::error("malformed NEGOTIATED payload: wrong size ("
                + std::to_string(payload.size()) + " bytes)");
        std::span<const std::byte> cur = payload;
        Negotiated negotiated{};
        negotiated.modifier = getU64(cur);
        negotiated.rowPitch = getU32(cur);
        negotiated.allocationSize = getU64(cur);
        return negotiated;
    }

    std::vector<std::byte> encodePayload(const ErrorMsg& error) {
        std::vector<std::byte> out{};
        out.reserve(4 + error.message.size());
        putU32(out, error.code);
        for (const char c : error.message) out.push_back(static_cast<std::byte>(c));
        return out;
    }

    ErrorMsg decodeError(const std::span<const std::byte> payload) {
        if (payload.size() < 4)
            throw ls::error("malformed ERROR payload: missing code");
        std::span<const std::byte> cur = payload;
        ErrorMsg error{};
        error.code = getU32(cur);
        error.message.reserve(cur.size());
        for (const std::byte b : cur)
            error.message.push_back(static_cast<char>(std::to_integer<uint8_t>(b)));
        return error;
    }

    std::vector<std::byte> encodePayload([[maybe_unused]] const Staging& staging) { return {}; }
    std::vector<std::byte> encodePayload([[maybe_unused]] const Ready& ready) { return {}; }

    Staging decodeStaging(const std::span<const std::byte> payload) {
        if (!payload.empty())
            throw ls::error("malformed STAGING payload: expected empty");
        return {};
    }

    Ready decodeReady(const std::span<const std::byte> payload) {
        if (!payload.empty())
            throw ls::error("malformed READY payload: expected empty");
        return {};
    }

    std::vector<std::byte> encodePayload(const Frame& frame) {
        std::vector<std::byte> out{};
        out.reserve(4);
        putU32(out, frame.stagingIdx);
        return out;
    }

    Frame decodeFrame(const std::span<const std::byte> payload) {
        if (payload.size() != 4)
            throw ls::error("malformed FRAME payload: wrong size ("
                + std::to_string(payload.size()) + " bytes)");
        std::span<const std::byte> cur = payload;
        Frame frame{};
        frame.stagingIdx = getU32(cur);
        return frame;
    }

    std::vector<std::byte> encodePayload(const Release& release) {
        std::vector<std::byte> out{};
        out.reserve(4);
        putU32(out, release.stagingIdx);
        return out;
    }

    Release decodeRelease(const std::span<const std::byte> payload) {
        if (payload.size() != 4)
            throw ls::error("malformed RELEASE payload: wrong size ("
                + std::to_string(payload.size()) + " bytes)");
        std::span<const std::byte> cur = payload;
        Release release{};
        release.stagingIdx = getU32(cur);
        return release;
    }

    Message decodeMessage(const MsgType type, const std::span<const std::byte> payload) {
        switch (type) {
            case MsgType::Hello: return decodeHello(payload);
            case MsgType::Negotiated: return decodeNegotiated(payload);
            case MsgType::Error: return decodeError(payload);
            case MsgType::Staging: return decodeStaging(payload);
            case MsgType::Ready: return decodeReady(payload);
            case MsgType::Frame: return decodeFrame(payload);
            case MsgType::Release: return decodeRelease(payload);
        }
        throw ls::error("unknown ipc message type " + std::to_string(static_cast<int>(type)));
    }

    namespace {
        /// create, bind and listen the unix socket for a listener; all
        /// failure paths clean up their own resources before throwing
        /// @throws ls::error if the path is too long for sockaddr_un
        /// @throws socket_error on socket/bind/listen failures
        int createListenerSocket(const std::filesystem::path& path) {
            std::error_code ec{};
            std::filesystem::create_directories(path.parent_path(), ec);

            // remove a stale socket left behind by a crashed previous
            // instance; ENOENT is the normal case
            ::unlink(path.c_str());

            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                throw socket_error("socket(AF_UNIX) for listener", errno);

            const std::string pathStr = path.string();
            if (pathStr.size() >= sizeof(sockaddr_un::sun_path)) {
                ::close(fd);
                throw ls::error("ipc socket path too long for sockaddr_un (" + pathStr + ")");
            }

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            pathStr.copy(static_cast<char*>(addr.sun_path), pathStr.size());

            if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
                const int err = errno;
                ::close(fd);
                throw socket_error("bind() on '" + pathStr + "'", err);
            }

            if (::listen(fd, 8) < 0) {
                const int err = errno;
                ::close(fd);
                ::unlink(path.c_str());
                throw socket_error("listen() on '" + pathStr + "'", err);
            }

            return fd;
        }
    }

    // === Listener ===

    std::filesystem::path Listener::defaultPath() {
        // LSFGVK_APP_SOCK overrides the path for BOTH the app and the layer.
        // Needed for Steam/Proton games: the SteamLinuxRuntime container gives
        // the game its OWN /run/user/UID, so the host's XDG_RUNTIME_DIR socket
        // is invisible to the layer (connect() ENOENT). Point both processes
        // at a path under a directory the container shares (e.g. $HOME).
        const char* sockOverride = std::getenv("LSFGVK_APP_SOCK");
        if (sockOverride && *sockOverride != '\0')
            return std::filesystem::path(sockOverride);
        const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtimeDir || *runtimeDir == '\0')
            throw ls::error("XDG_RUNTIME_DIR is not set; cannot locate the lsfg-vk app socket "
                "(expected ${XDG_RUNTIME_DIR}/lsfg-vk/app.sock)");
        return std::filesystem::path(runtimeDir) / "lsfg-vk" / "app.sock";
    }

    Listener::Listener(const std::filesystem::path& path)
        : listenFd(createListenerSocket(path)), socketPath(path) {}

    Listener::Listener(int fd, std::filesystem::path path)
        : listenFd(fd), socketPath(std::move(path)) {}

    Listener::Listener(Listener&& other) noexcept
        : listenFd(std::exchange(other.listenFd, -1)),
          socketPath(std::move(other.socketPath)) {}

    Listener& Listener::operator=(Listener&& other) noexcept {
        if (this != &other) {
            this->close();
            this->listenFd = std::exchange(other.listenFd, -1);
            this->socketPath = std::move(other.socketPath);
        }
        return *this;
    }

    Listener::~Listener() {
        this->close();
    }

    void Listener::close() {
        if (this->listenFd < 0) return;
        ::close(this->listenFd);
        this->listenFd = -1;
        if (!this->socketPath.empty()) {
            ::unlink(this->socketPath.c_str());
            this->socketPath.clear();
        }
    }

    Connection Listener::accept() const {
        while (true) {
            const int fd = ::accept4(this->listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) return Connection(fd);
            if (errno == EINTR) continue;
            throw socket_error("accept() on ipc listener", errno);
        }
    }

    // === Connection ===

    Connection::Connection(int fd) : sockFd(fd) {}

    Connection Connection::connect(const std::filesystem::path& path) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw socket_error("socket(AF_UNIX) for connection", errno);

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string pathStr = path.string();
        if (pathStr.size() >= sizeof(sockaddr_un::sun_path)) {
            ::close(fd);
            throw ls::error("ipc socket path too long for sockaddr_un (" + pathStr + ")");
        }
        pathStr.copy(static_cast<char*>(addr.sun_path), pathStr.size());

        // EINTR may leave the connect attempt in progress; a retry then fails
        // with EISCONN once it completed, which means success
        while (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            const int err = errno;
            if (err == EINTR) continue;
            if (err == EISCONN) break;
            ::close(fd);
            throw socket_error("connect() to '" + pathStr + "'", err);
        }

        return Connection(fd);
    }

    Connection::Connection(Connection&& other) noexcept
        : sockFd(std::exchange(other.sockFd, -1)),
          attachedFd(std::exchange(other.attachedFd, -1)),
          receivedFd(std::exchange(other.receivedFd, -1)) {}

    Connection& Connection::operator=(Connection&& other) noexcept {
        if (this != &other) {
            this->close();
            this->sockFd = std::exchange(other.sockFd, -1);
            this->attachedFd = std::exchange(other.attachedFd, -1);
            this->receivedFd = std::exchange(other.receivedFd, -1);
        }
        return *this;
    }

    Connection::~Connection() {
        this->close();
    }

    void Connection::close() {
        closeQuiet(this->sockFd);
        closeQuiet(this->attachedFd);
        closeQuiet(this->receivedFd);
    }

    void Connection::send(const Message& msg) {
        if (this->sockFd < 0) throw ls::error("send on closed ipc connection");

        const MsgType type = typeOf(msg);
        const bool needFd = carriesFd(type);

        // strict fd rules: never let an fd vanish silently
        if (this->attachedFd >= 0 && !needFd)
            throw ls::error(std::string("ipc send of ") + nameOf(type)
                + " with an fd still attached (it carries no fd; detach it first)");
        if (needFd && this->attachedFd < 0)
            throw ls::error(std::string("ipc send of ") + nameOf(type)
                + " without an attached fd");

        std::vector<std::byte> frame{};
        const std::vector<std::byte> payload = std::visit(
            [](const auto& m) { return encodePayload(m); }, msg);
        if (payload.size() > MAX_PAYLOAD_LEN)
            throw ls::error("ipc payload exceeds maximum size");
        putU32(frame, static_cast<uint32_t>(HEADER_LEN + payload.size()));
        putU32(frame, MAGIC);
        frame.push_back(static_cast<std::byte>(type));
        frame.insert(frame.end(), payload.begin(), payload.end());

        // first write goes out via sendmsg so the attached fd travels as
        // SCM_RIGHTS ancillary data alongside the first byte; any remainder
        // continues with plain sends (the kernel consumed the cmsg already)
        iovec iov{};
        iov.iov_base = frame.data();
        iov.iov_len = frame.size();
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

        std::array<std::byte, CMSG_SPACE(sizeof(int))> ctrl{};
        if (needFd) {
            mh.msg_control = ctrl.data();
            mh.msg_controllen = ctrl.size();
            cmsghdr* cmsg = CMSG_FIRSTHDR(&mh);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &this->attachedFd, sizeof(int));
        }

        size_t sent = 0;
        bool firstCall = true;
        std::span<const std::byte> remaining{frame};
        while (!remaining.empty()) {
            ssize_t n = 0;
            if (firstCall) {
                n = ::sendmsg(this->sockFd, &mh, MSG_NOSIGNAL);
                firstCall = false;
            } else {
                n = ::send(this->sockFd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
            }

            if (n < 0) {
                if (errno == EINTR) continue;
                const int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) {
                    // SO_SNDTIMEO fired; a partial frame leaves the stream
                    // framing-corrupted, which must be surfaced loudly
                    if (sent > 0)
                        throw ls::error("ipc send timed out after a partial frame; "
                            "stream is unusable");
                    throw socket_error("send() on ipc socket (timed out)", err);
                }
                throw socket_error("send() on ipc socket", err);
            }
            if (n == 0)
                throw ls::error("ipc send made no progress");

            // the kernel duplicated the fd into the receiver's queue together
            // with the first byte; SCM_RIGHTS does NOT consume the sender's
            // descriptor, so close our reference now to complete the
            // ownership transfer promised by attachFd()
            if (needFd && sent == 0) {
                ::close(this->attachedFd);
                this->attachedFd = -1;
            }
            sent += static_cast<size_t>(n);
            remaining = remaining.subspan(static_cast<size_t>(n));
        }
    }

    Message Connection::receive(const std::optional<std::chrono::milliseconds>& deadline) {
        if (this->sockFd < 0) throw ls::error("receive on closed ipc connection");
        if (this->receivedFd >= 0)
            throw ls::error("ipc receive with an unconsumed fd from a previous STAGING/FRAME "
                "(takeReceivedFd() it first)");

        // header: length prefix + magic + type
        std::array<std::byte, HEADER_LEN> header{};
        std::vector<int> fds{};
        if (!recvFull(this->sockFd, header.data(), header.size(), deadline, fds))
            throw ls::error("ipc connection closed by peer");

        std::span<const std::byte> cur = header;
        const uint32_t totalLen = getU32(cur);
        const uint32_t magic = getU32(cur);
        const auto type = static_cast<MsgType>(getU8(cur));

        if (magic != MAGIC)
            throw ls::error("ipc frame has bad magic (stream desync?)");
        if (totalLen < HEADER_LEN || totalLen > HEADER_LEN + MAX_PAYLOAD_LEN)
            throw ls::error("ipc frame declares absurd length " + std::to_string(totalLen));

        std::vector<std::byte> payload(totalLen - HEADER_LEN);
        if (!payload.empty()
                && !recvFull(this->sockFd, payload.data(), payload.size(), deadline, fds))
            throw ls::error("ipc peer closed connection mid-payload");

        if (fds.size() > 1) {
            for (const int fd : fds) ::close(fd);
            throw ls::error("ipc message carried more than one fd (protocol violation)");
        }

        // strict one-fd-per-message rules on the receive side too
        if (carriesFd(type) && fds.empty()) {
            throw ls::error(std::string(nameOf(type)) + " arrived without its fd");
        }
        if (!carriesFd(type) && !fds.empty()) {
            ::close(fds.at(0));
            throw ls::error(std::string(nameOf(type)) + " arrived with an unexpected fd");
        }
        if (carriesFd(type))
            this->receivedFd = fds.at(0);

        return decodeMessage(type, payload);
    }

    void Connection::attachFd(const int fd) {
        if (this->attachedFd >= 0)
            throw ls::error("ipc attachFd with another fd still attached (detach it first)");
        this->attachedFd = fd;
    }

    int Connection::detachFd() {
        return std::exchange(this->attachedFd, -1);
    }

    int Connection::takeReceivedFd() {
        return std::exchange(this->receivedFd, -1);
    }

    bool Connection::drained() const {
        if (this->sockFd < 0) return true;

        // zero-timeout poll: pure readiness check. deliberately NOT a
        // MSG_PEEK recv — peeking without consuming can interact badly with
        // queued SCM_RIGHTS ancillary data, and poll answers the same
        // question without touching the receive queue
        pollfd pfd{};
        pfd.fd = this->sockFd;
        pfd.events = POLLIN;
        const int res = ::poll(&pfd, 1, 0);
        if (res < 0) {
            if (errno == EINTR) return false; // conservative: assume pending
            throw socket_error("poll() on ipc socket", errno);
        }
        return res == 0;
    }

    void Connection::setSendTimeout(const std::optional<std::chrono::milliseconds>& timeout) const {
        if (this->sockFd < 0) throw ls::error("setSendTimeout on closed ipc connection");

        timeval tv{};
        if (timeout) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(*timeout);
            tv.tv_sec = static_cast<time_t>(micros.count() / 1'000'000);
            tv.tv_usec = static_cast<suseconds_t>(micros.count() % 1'000'000);
        }

        if (::setsockopt(this->sockFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
            throw socket_error("setsockopt(SO_SNDTIMEO) on ipc socket", errno);
    }
}

// NOLINTEND(misc-include-cleaner)
