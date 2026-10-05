#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "lsfg-vk-common/ipc/control_block.hpp"

/// A ring of udmabufs plus the shared control block that describes it.
///
/// Ownership rule for this transport: the RENDER side (the layer) creates
/// every buffer, because the frame data lands there. The doubler imports the
/// fds it is handed and never creates a buffer for this path.
///
/// Every allocation is a kernel udmabuf: memfd backing with F_SEAL_SHRINK and
/// UDMABUF_CREATE. No malloc, no posix_memalign, no
/// VK_EXT_external_memory_host anywhere in this path.
namespace ls::ipc {

/// One udmabuf slot and its backing memfd.
struct RingBuffer {
    int memfd{-1};      ///< the shared pages themselves
    int udmabuf{-1};    ///< UDMABUF_CREATE handle, the only GPU-importable form
    std::size_t bytes{0};
};

/// The whole ring: K buffers plus one control-block mapping.
class UdmabufRing {
    public:
        /// Create K udmabufs and the control block. Render side only.
        ///
        /// @param count number of slots (K); must be >= 1
        /// @param slotBytes payload bytes per slot, rounded up to 4096 here
        /// @param width frame width, recorded in the control block
        /// @param height frame height, recorded in the control block
        /// @param format VkFormat of the frame payload
        /// @throws ls::error on any kernel or allocation failure
        UdmabufRing(std::uint32_t count, std::size_t slotBytes,
            std::uint32_t width, std::uint32_t height, std::uint32_t format);

        ~UdmabufRing();
        UdmabufRing(const UdmabufRing&) = delete;
        UdmabufRing& operator=(const UdmabufRing&) = delete;
        UdmabufRing(UdmabufRing&& other) noexcept;
        UdmabufRing& operator=(UdmabufRing&& other) noexcept;

        [[nodiscard]] std::uint32_t count() const {
            return static_cast<std::uint32_t>(this->bufs.size());
        }
        [[nodiscard]] std::size_t slotBytes() const { return this->bytes; }
        [[nodiscard]] const std::vector<RingBuffer>& slots() const {
            return this->bufs;
        }
        /// the control-block mapping owned by this ring
        [[nodiscard]] ControlBlock& control() { return *this->cb; }
        /// hand the control-block fd to the peer (transfer of ownership)
        [[nodiscard]] int releaseControlFd();

    private:
        std::vector<RingBuffer> bufs;
        void* ctrlMap{nullptr};
        std::size_t ctrlBytes{0};
        int ctrlFd{-1};
        std::size_t bytes{0};
        std::unique_ptr<ControlBlock> cb;
};

}  // namespace ls::ipc