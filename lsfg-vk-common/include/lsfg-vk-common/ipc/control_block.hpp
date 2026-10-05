#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

/// Control block shared between the render process (the layer) and the doubler
/// (the app). One memfd, mapped by both sides, carrying one slot record per
/// udmabuf in the ring.
///
/// Design rules this structure exists to enforce:
///   - neither process ever waits for the other. There is no semaphore, no
///     fence wait across the socket, no condition variable. A slot is claimed
///     by CAS on `state`; if the CAS fails the frame is skipped.
///   - the CPU never touches frame data. Only the sequence/state words live
///     here, never pixels.
///   - slots are single-producer/single-consumer by ownership of the CAS, so
///     the atomics need no locks. seq is published with release ordering by
///     the writer and acquired by the reader.
namespace ls::ipc {

/// slot lifecycle. Transitions are CAS-only; a stale writer can never move a
/// slot backwards because it must observe the exact expected state.
enum class SlotState : std::uint32_t {
    Free = 0,      ///< nobody owns it; the producer may claim it
    Writing = 1,   ///< the producer has claimed it and is running a GPU copy
    Complete = 2,  ///< the copy is done and seq is valid; the consumer may take it
    Reading = 3,   ///< the consumer has claimed it and is running its GPU copy
};

/// One ring slot. 64 bytes so the ring is cache-line aligned per slot and K
/// slots never share a line between the two processes' writes.
struct alignas(64) RingSlot {
    std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(SlotState::Free)};
    /// frame sequence number, valid only while state == Complete
    std::atomic<std::uint64_t> seq{0};
    std::uint64_t captureTsNs{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t format{0};
    std::uint32_t reserved{0};
    char pad[24]{};
};
static_assert(sizeof(RingSlot) == 64, "RingSlot must be one cache line");

/// Header at the front of the control-block mapping. The app reads `count`
/// before touching any slot, so it must be published first.
struct RingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t count;    ///< number of slots that follow (K)
    std::uint32_t bytesPerSlot;
    std::uint64_t slotBytes; ///< udmabuf payload bytes per slot
};
static_assert(sizeof(RingHeader) == 24, "RingHeader layout is part of the ABI");

inline constexpr std::uint32_t RING_MAGIC = 0x4c534652;  // 'LSFR'
inline constexpr std::uint32_t RING_VERSION = 1;

/// A mapped control block: the header plus the K slots. Constructed in place
/// over an existing mapping; it never allocates and never copies pixels.
class ControlBlock {
    public:
        /// map an existing control-block region (shared, read/write).
        /// @param base the mapping
        /// @param bytes the mapping size
        /// @param forProducer true on the layer side, which initialises the
        ///        header; false on the app side, which only reads it
        ControlBlock(void* base, std::size_t bytes, bool forProducer);

        /// initialise the header and mark all slots Free (producer only).
        void init(std::uint32_t slotCount, std::uint64_t slotBytes,
            std::uint32_t width, std::uint32_t height, std::uint32_t format);

        [[nodiscard]] std::uint32_t count() const { return this->hdr->count; }
        [[nodiscard]] std::uint64_t slotBytes() const { return this->hdr->slotBytes; }
        [[nodiscard]] RingSlot* slot(std::uint32_t i) {
            return this->slots + i;
        }
        [[nodiscard]] RingHeader* header() const { return this->hdr; }

        /// Producer: claim a free slot and mark it Writing. Returns the index,
        /// or -1 when no slot is free. Never blocks.
        [[nodiscard]] int claimForWrite(std::uint64_t seq,
            std::uint64_t captureTsNs);

        /// Producer: publish a claimed slot as Complete. Never blocks.
        void publishComplete(std::uint32_t idx, std::uint64_t seq,
            std::uint64_t captureTsNs, std::uint32_t width,
            std::uint32_t height, std::uint32_t format);

        /// Consumer: take the newest Complete slot, marking it Reading.
        /// Returns the index, or -1 when nothing new is ready. Never blocks.
        [[nodiscard]] int claimNewestComplete(std::uint64_t* seqOut);

        /// Consumer: release a Reading slot back to Free. Never blocks.
        void release(std::uint32_t idx);

    private:
        RingHeader* hdr{nullptr};
        RingSlot* slots{nullptr};
        std::uint32_t n{0};
        std::uint32_t lastClaim{0};
};

}  // namespace ls::ipc