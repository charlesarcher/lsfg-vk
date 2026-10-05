#include "lsfg-vk-common/ipc/control_block.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"

namespace ls::ipc {

ControlBlock::ControlBlock(void* base, std::size_t bytes, bool forProducer) {
    if (base == nullptr)
        throw ls::error("control block: null mapping");
    this->hdr = static_cast<RingHeader*>(base);
    if (bytes < sizeof(RingHeader))
        throw ls::error("control block: mapping too small for the header");

    if (forProducer) {
        this->hdr->magic = RING_MAGIC;
        this->hdr->version = RING_VERSION;
        this->n = 0;
        this->slots = nullptr;
        return;
    }

    // Consumer: the producer published the header before any slot exists.
    if (this->hdr->magic != RING_MAGIC)
        throw ls::error("control block: bad magic");
    if (this->hdr->version != RING_VERSION)
        throw ls::error("control block: version mismatch");
    this->n = this->hdr->count;
    const std::size_t need = sizeof(RingHeader) + sizeof(RingSlot) * this->n;
    if (bytes < need)
        throw ls::error("control block: mapping smaller than the declared ring");
    this->slots = reinterpret_cast<RingSlot*>(
        static_cast<char*>(base) + sizeof(RingHeader));
}

void ControlBlock::init(std::uint32_t slotCount, std::uint64_t slotBytes,
    std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    if (slotCount == 0)
        throw ls::error("control block: ring needs at least one slot");
    this->hdr->slotBytes = slotBytes;
    this->hdr->count = slotCount;
    this->hdr->bytesPerSlot = sizeof(RingSlot);
    this->slots = reinterpret_cast<RingSlot*>(
        reinterpret_cast<char*>(this->hdr) + sizeof(RingHeader));
    for (std::uint32_t i = 0; i < slotCount; ++i) {
        this->slots[i].state.store(static_cast<std::uint32_t>(SlotState::Free),
            std::memory_order_relaxed);
        this->slots[i].seq.store(0, std::memory_order_relaxed);
        this->slots[i].captureTsNs = 0;
        this->slots[i].width = width;
        this->slots[i].height = height;
        this->slots[i].format = format;
    }
    // The ring is only usable once count is visible; release it last.
    std::atomic_thread_fence(std::memory_order_release);
    this->n = slotCount;
}

int ControlBlock::claimForWrite(std::uint64_t seq, std::uint64_t captureTsNs) {
    // Walk from a rotating offset so the producer and consumer do not contend
    // for the same slot index first. Still lock-free and still never blocks.
    for (std::uint32_t k = 0; k < this->n; ++k) {
        const std::uint32_t i = (this->lastClaim + 1 + k) % this->n;
        std::uint32_t expect = static_cast<std::uint32_t>(SlotState::Free);
        RingSlot& s = this->slots[i];
        if (s.state.compare_exchange_strong(expect,
                static_cast<std::uint32_t>(SlotState::Writing),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            s.captureTsNs = captureTsNs;
            s.seq.store(seq, std::memory_order_relaxed);
            this->lastClaim = i;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ControlBlock::publishComplete(std::uint32_t idx, std::uint64_t seq,
    std::uint64_t captureTsNs, std::uint32_t width, std::uint32_t height,
    std::uint32_t format) {
    RingSlot& s = this->slots[idx];
    s.seq.store(seq, std::memory_order_relaxed);
    s.captureTsNs = captureTsNs;
    s.width = width;
    s.height = height;
    s.format = format;
    // Release: the consumer must see the payload metadata before Complete.
    s.state.store(static_cast<std::uint32_t>(SlotState::Complete),
        std::memory_order_release);
}

int ControlBlock::claimNewestComplete(std::uint64_t* seqOut) {
    int best = -1;
    std::uint64_t bestSeq = 0;
    for (std::uint32_t i = 0; i < this->n; ++i) {
        if (this->slots[i].state.load(std::memory_order_acquire)
                != static_cast<std::uint32_t>(SlotState::Complete))
            continue;
        const std::uint64_t s = this->slots[i].seq.load(std::memory_order_relaxed);
        if (best < 0 || s > bestSeq) {
            best = static_cast<int>(i);
            bestSeq = s;
        }
    }
    if (best < 0)
        return -1;

    // Take it by CAS; if the producer or another pass got there first, give up
    // for this frame rather than wait.
    RingSlot& s = this->slots[static_cast<std::uint32_t>(best)];
    std::uint32_t expect = static_cast<std::uint32_t>(SlotState::Complete);
    if (!s.state.compare_exchange_strong(expect,
            static_cast<std::uint32_t>(SlotState::Reading),
            std::memory_order_acq_rel, std::memory_order_acquire))
        return -1;
    if (seqOut != nullptr)
        *seqOut = s.seq.load(std::memory_order_relaxed);
    return best;
}

void ControlBlock::release(std::uint32_t idx) {
    this->slots[idx].state.store(static_cast<std::uint32_t>(SlotState::Free),
        std::memory_order_release);
}

}  // namespace ls::ipc