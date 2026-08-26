/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/ipc/protocol.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>

namespace ls::ipc {
    class Connection;

    /// typed error for a failed socket syscall, carrying the errno code and
    /// the name/context of the failing operation alongside the usual
    /// human-readable ls::error text
    class [[gnu::visibility("default")]] socket_error : public ls::error {
    public:
        /// construct a socket_error around a failed syscall
        /// @param op description of the failing operation (e.g. "connect() to '<path>'")
        /// @param err the errno value the syscall reported
        explicit socket_error(const std::string& op, int err)
            : ls::error(op + " failed: " + std::strerror(err)), op(op), err(err) {}

        /// get the errno value associated with this failure
        [[nodiscard]] int errnoCode() const { return this->err; }

        /// get the description of the failing operation
        [[nodiscard]] const std::string& operation() const { return this->op; }
    private:
        std::string op;
        int err;
    };

    /// unix-socket listener for the app side: binds ${XDG_RUNTIME_DIR}/lsfg-vk/
    /// app.sock (or an explicit path) and hands out one Connection per accepted
    /// game stream. RAII: the destructor closes the listening socket and unlinks
    /// the socket file it created
    class Listener {
    public:
        /// default socket path per protocol v1: ${XDG_RUNTIME_DIR}/lsfg-vk/app.sock
        /// @return the socket path
        /// @throws ls::error if XDG_RUNTIME_DIR is unset or empty
        static std::filesystem::path defaultPath();

        /// create the listener: creates parent directories, removes a stale
        /// socket file left behind by a crashed previous instance, binds and
        /// listens. a live listener's socket being stolen this way is accepted
        /// for v1 (single app instance per runtime dir)
        /// @param path unix socket path to bind
        /// @throws ls::error if the path is too long for sockaddr_un
        /// @throws socket_error on socket/bind/listen failures
        explicit Listener(const std::filesystem::path& path);

        Listener(Listener&& other) noexcept;
        Listener& operator=(Listener&& other) noexcept;

        /// non-copyable: owns the listening fd and the bound socket file
        Listener(const Listener&) = delete;
        Listener& operator=(const Listener&) = delete;

        /// closes the listening fd and unlinks the socket file
        ~Listener();

        /// block until a game connects
        /// @return the accepted connection
        /// @throws socket_error on accept failures
        [[nodiscard]] Connection accept() const;

        /// close early and unlink the socket file; safe to call repeatedly,
        /// also invoked by the destructor
        void close();

        /// underlying listening fd, for integration into external poll loops
        [[nodiscard]] int fd() const noexcept { return this->listenFd; }
    private:
        Listener(int fd, std::filesystem::path path);

        int listenFd{-1};
        std::filesystem::path socketPath;
    };

    /// one game⇄app byte+fd stream over AF_UNIX SOCK_STREAM. blocking I/O with
    /// MSG_NOSIGNAL throughout; receive deadlines are enforced with poll.
    /// move-only RAII: closes its fd exactly once, and also closes any fd
    /// received but never picked up, so the module itself never leaks fds.
    ///
    /// fd discipline (STRICT one fd per message):
    /// - sending STAGING/FRAME requires an fd attached beforehand via
    ///   attachFd(); the send transfers ownership to the kernel
    /// - sending any other message with an fd still attached throws instead
    ///   of silently leaking it (detach it explicitly first)
    /// - receiving STAGING/FRAME stores the kernel-duplicated fd retrievable
    ///   via takeReceivedFd(); receiving another message while one is still
    ///   stored throws rather than leaking it
    class Connection {
        friend class Listener;
    public:
        /// connect to the app's socket (layer side)
        /// @param path unix socket path to connect to
        /// @return the established connection
        /// @throws socket_error naming the path on socket/connect failures
        static Connection connect(const std::filesystem::path& path);

        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;

        /// non-copyable: owns the connected fd
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        /// closes the fd and any unconsumed attached/received fds exactly once
        ~Connection();

        /// serialize and send one message. STAGING/FRAME consume the attached
        /// fd via SCM_RIGHTS; all fields are little-endian on the wire
        /// @param msg the message (any protocol struct converts implicitly)
        /// @throws ls::error if the fd-carrying rules above are violated
        /// @throws socket_error on send failures (incl. EPIPE after peer death)
        void send(const Message& msg);

        /// receive one full message, decoding its payload. blocks until the
        /// message completes or the deadline expires; an fd on STAGING/FRAME
        /// is stored for takeReceivedFd()
        /// @param deadline max time to wait for the message; nullopt = forever
        /// @return the decoded message
        /// @throws ls::error on malformed frames, unknown types, fd-rule
        ///     violations, or unconsumed previously received fds
        /// @throws socket_error on recv failures or deadline expiry
        Message receive(const std::optional<std::chrono::milliseconds>& deadline = std::nullopt);

        /// receive one message and require it to be of a specific type.
        /// an incoming ERROR message is surfaced as an ls::error carrying the
        /// peer's text instead of returning the wrong type
        /// @tparam T expected message struct (e.g. Ready)
        /// @param deadline max time to wait; nullopt = forever
        /// @return the decoded message of exactly type T
        /// @throws ls::error on type mismatch (incl. peer ERROR replies),
        ///     see receive() for the remaining failure modes
        template<typename T>
        T expect(const std::optional<std::chrono::milliseconds>& deadline = std::nullopt) {
            Message msg = this->receive(deadline);
            if (!std::holds_alternative<T>(msg)) {
                if (auto* err = std::get_if<ErrorMsg>(&msg))
                    throw ls::error("peer refused: " + err->message);
                const MsgType got = std::visit(
                    [](const auto& m) { return messageTypeOf<std::decay_t<decltype(m)>>(); }, msg);
                throw ls::error(std::string("expected ")
                    + nameOf(messageTypeOf<T>()) + ", got " + nameOf(got));
            }
            return std::get<T>(std::move(msg));
        }

        /// hand an fd to the connection for the next STAGING/FRAME send.
        /// ownership moves here immediately: a successful send closes the
        /// connection's reference (the kernel duplicates the descriptor into
        /// the receiver), while any failure path before that keeps it
        /// retrievable via detachFd() or closed by the destructor
        /// @param fd the fd to own and send
        /// @throws ls::error if another fd is already attached
        void attachFd(int fd);

        /// take back a not-yet-sent attached fd (ownership passes to caller)
        /// @return the fd, or -1 if none is attached
        [[nodiscard]] int detachFd();

        /// take ownership of the fd carried by the most recently received
        /// STAGING/FRAME message (the kernel duplicated it on receive)
        /// @return the fd, or -1 if none was received/consumed yet
        [[nodiscard]] int takeReceivedFd();

        /// non-blocking readiness check: true when no data is currently
        /// pending. deliberately does not peek the queue (a MSG_PEEK recv can
        /// interact badly with queued SCM_RIGHTS ancillary data); a false
        /// result includes EOF, where the next receive() surfaces closure.
        /// does NOT guarantee the peer is alive while true
        /// @return true if the socket has no readable data right now
        /// @throws socket_error on poll failures
        [[nodiscard]] bool drained() const;

        /// configure SO_SNDTIMEO for sends on this connection (default: none)
        /// @param timeout max blocking time per send attempt, nullopt disables
        /// @throws socket_error if setsockopt fails
        void setSendTimeout(const std::optional<std::chrono::milliseconds>& timeout) const;

        /// underlying socket fd, for integration into external poll loops
        [[nodiscard]] int fd() const noexcept { return this->sockFd; }

        /// close early; safe to call multiple times, also invoked by the dtor
        void close();
    private:
        explicit Connection(int fd);

        int sockFd{-1};
        int attachedFd{-1};
        int receivedFd{-1};
    };
}
