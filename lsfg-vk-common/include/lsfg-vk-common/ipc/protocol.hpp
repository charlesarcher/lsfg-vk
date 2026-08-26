/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ls::ipc {
    /// protocol version implemented by this module (HELLO.proto_version)
    inline constexpr uint32_t PROTO_VERSION = 1;

    /// leading magic of every message frame, the ASCII characters 'LSFG'
    inline constexpr uint32_t MAGIC = 0x4C534647;

    /// fixed length of the device_name field in HELLO, NUL-padded
    inline constexpr size_t DEVICE_NAME_LEN = 256;

    /// fixed length of the game_uuid field in HELLO (a VkUUID)
    inline constexpr size_t UUID_LEN = 16;

    /// upper bound for a single message payload; guards against absurd
    /// length prefixes from a broken/malicious peer. the largest legitimate
    /// payload is HELLO at 288 bytes, so this leaves ample headroom
    inline constexpr size_t MAX_PAYLOAD_LEN = 4096;

    /// message types on the wire (the u8 following the magic)
    enum class MsgType : uint8_t {
        Hello = 1,
        Negotiated = 2,
        Error = 3,
        Staging = 4,
        Ready = 5,
        Frame = 6,
        Release = 7
    };

    /// human-readable name of a message type, for error messages
    constexpr const char* nameOf(MsgType type) {
        switch (type) {
            case MsgType::Hello: return "HELLO";
            case MsgType::Negotiated: return "NEGOTIATED";
            case MsgType::Error: return "ERROR";
            case MsgType::Staging: return "STAGING";
            case MsgType::Ready: return "READY";
            case MsgType::Frame: return "FRAME";
            case MsgType::Release: return "RELEASE";
        }
        return "<unknown>";
    }

    /// C→A handshake offer: game identity and true swapchain properties.
    /// the app negotiates the exchange layout against its REAL processing
    /// device using these caps
    struct Hello {
        /// protocol version, must equal PROTO_VERSION
        uint32_t protoVersion;
        /// UUID of the game's Vulkan device (VkPhysicalDeviceProperties::deviceUUID)
        std::array<uint8_t, UUID_LEN> gameUuid;
        /// name of the game's Vulkan device, NUL-padded (may be truncated to fit)
        std::array<char, DEVICE_NAME_LEN> deviceName;
        /// VkFormat of the presented swapchain images
        uint32_t vkFormat;
        /// swapchain width in pixels
        uint32_t width;
        /// swapchain height in pixels
        uint32_t height;
    };

    /// A→C reply describing the layout staging images must be created with
    struct Negotiated {
        /// DRM modifier the staging images must use
        uint64_t modifier;
        /// row pitch of the staged image in bytes
        uint32_t rowPitch;
        /// size of the underlying memory allocation in bytes
        uint64_t allocationSize;
    };

    /// A→C refusal; the sender closes the connection afterwards.
    /// codes are advisory, the text is what gets surfaced to users
    struct ErrorMsg {
        /// sender-defined error code (0 = generic)
        uint32_t code;
        /// human-readable description
        std::string message;
    };

    /// generic failure code for ErrorMsg
    inline constexpr uint32_t ERROR_GENERIC = 0;

    /// C→A staging-image handoff; carries exactly one fd via SCM_RIGHTS.
    /// one message per fd, sent twice during handshake (ring depth is 2)
    struct Staging { };

    /// A→C acknowledgement; the stream is live after this point
    struct Ready { };

    /// C→A per-frame notification; carries exactly one sync fd (the capture
    /// blit's completion payload) alongside the staging slot it filled
    struct Frame {
        /// index of the staging slot this frame was captured into (0 or 1)
        uint32_t stagingIdx;
    };

    /// A→C backpressure ack; the app will not read this slot again until
    /// it is recaptured, so the layer may reuse it
    struct Release {
        /// index of the freed staging slot
        uint32_t stagingIdx;
    };

    /// any protocol message; STAGING/FRAME additionally carry one fd each,
    /// managed by the Connection (see socket.hpp), never inside the payload
    using Message = std::variant<Hello, Negotiated, ErrorMsg, Staging, Ready, Frame, Release>;

    /// message type associated with a concrete message struct
    template<typename T>
    constexpr MsgType messageTypeOf() {
        if constexpr (std::is_same_v<T, Hello>) return MsgType::Hello;
        else if constexpr (std::is_same_v<T, Negotiated>) return MsgType::Negotiated;
        else if constexpr (std::is_same_v<T, ErrorMsg>) return MsgType::Error;
        else if constexpr (std::is_same_v<T, Staging>) return MsgType::Staging;
        else if constexpr (std::is_same_v<T, Ready>) return MsgType::Ready;
        else if constexpr (std::is_same_v<T, Frame>) return MsgType::Frame;
        else if constexpr (std::is_same_v<T, Release>) return MsgType::Release;
        else static_assert(!sizeof(T), "not a protocol message type");
    }

    /// message type of a decoded message
    MsgType typeOf(const Message& msg);

    /// whether a message of this type carries exactly one SCM_RIGHTS fd
    constexpr bool carriesFd(MsgType type) {
        return type == MsgType::Staging || type == MsgType::Frame;
    }

    /// build a Hello with the device name safely truncated + NUL-padded
    /// @param protoVersion protocol version (usually PROTO_VERSION)
    /// @param gameUuid game device UUID
    /// @param deviceName game device name, truncated at 255 chars if longer
    Hello makeHello(uint32_t protoVersion, const std::array<uint8_t, UUID_LEN>& gameUuid,
        const std::string& deviceName, uint32_t vkFormat, uint32_t width, uint32_t height);

    /// extract the NUL-terminated device name from a Hello
    std::string helloDeviceName(const Hello& hello);

    // === serialization ===
    //
    // wire format of one message:
    //     u32 LE total-length | u32 LE magic | u8 msg_type | payload...
    // where total-length counts all bytes AFTER the length field itself
    // (i.e. 5 + payload.size()). all multi-byte fields are serialized as
    // explicit little-endian bytes; structs are NEVER memcpy'd raw.
    //
    // fds are not part of the byte stream; they travel out-of-band via
    // SCM_RIGHTS ancillary data on STAGING/FRAME messages only.

    /// encode a Hello into its payload bytes
    std::vector<std::byte> encodePayload(const Hello& hello);
    /// decode a Hello payload
    /// @throws ls::error if the payload has the wrong size
    Hello decodeHello(std::span<const std::byte> payload);

    /// encode a Negotiated into its payload bytes
    std::vector<std::byte> encodePayload(const Negotiated& negotiated);
    /// decode a Negotiated payload
    /// @throws ls::error if the payload has the wrong size
    Negotiated decodeNegotiated(std::span<const std::byte> payload);

    /// encode an ErrorMsg into its payload bytes
    std::vector<std::byte> encodePayload(const ErrorMsg& error);
    /// decode an ErrorMsg payload
    /// @throws ls::error if the payload is too short or over-long
    ErrorMsg decodeError(std::span<const std::byte> payload);

    /// encode an empty-payload message (Staging/Ready); returns empty vector
    std::vector<std::byte> encodePayload(const Staging& staging);
    /// encode an empty-payload message (Staging/Ready); returns empty vector
    std::vector<std::byte> encodePayload(const Ready& ready);
    /// validate an empty payload
    /// @throws ls::error if the payload is non-empty
    Staging decodeStaging(std::span<const std::byte> payload);
    /// validate an empty payload
    /// @throws ls::error if the payload is non-empty
    Ready decodeReady(std::span<const std::byte> payload);

    /// encode a Frame into its payload bytes
    std::vector<std::byte> encodePayload(const Frame& frame);
    /// decode a Frame payload
    /// @throws ls::error if the payload has the wrong size
    Frame decodeFrame(std::span<const std::byte> payload);

    /// encode a Release into its payload bytes
    std::vector<std::byte> encodePayload(const Release& release);
    /// decode a Release payload
    /// @throws ls::error if the payload has the wrong size
    Release decodeRelease(std::span<const std::byte> payload);

    /// dispatch-decode a payload by its wire message type
    /// @throws ls::error on unknown type or malformed payload
    Message decodeMessage(MsgType type, std::span<const std::byte> payload);
}
