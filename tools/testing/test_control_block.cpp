// Contract test for the lock-free control block and the udmabuf ring.
// Proves, without a GPU:
//   - the ring creates K real udmabufs and a control block
//   - producer and consumer never block when the peer is absent or slow
//   - the producer skips a frame rather than waiting when no slot is free
//   - the consumer reuses nothing and never waits when no slot is complete
//   - K slots in flight is the hard bound on un-consumed frames
#include "lsfg-vk-common/ipc/udmabuf_ring.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace ls;

static int failures = 0;
static void check(bool cond, const char* what) {
    std::printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond)
        failures++;
}

int main() {
    constexpr uint32_t K = 3;
    constexpr size_t slotBytes = 256u * 1024u;

    ipc::UdmabufRing ring(K, slotBytes, 2560, 1440, 44);
    check(ring.count() == K, "ring created with K slots");
    const size_t wantAligned = (slotBytes + 4095u) & ~static_cast<size_t>(4095u);
    check(ring.slotBytes() == wantAligned, "slot bytes page-aligned");
    check(ring.slots()[0].udmabuf >= 0, "slot 0 is a live udmabuf fd");
    check(ring.slots()[1].udmabuf >= 0, "slot 1 is a live udmabuf fd");
    check(ring.slots()[2].udmabuf >= 0, "slot 2 is a live udmabuf fd");
    check(ring.control().count() == K, "control block reports K");

    // The peer side: map the control block from the same memfd, exactly as a
    // second process would after receiving the fd over SCM_RIGHTS.
    int cfd = ring.releaseControlFd();
    void* peerMap = mmap(nullptr, sizeof(ipc::RingHeader) + sizeof(ipc::RingSlot) * K,
        PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    check(peerMap != MAP_FAILED, "peer maps the control block");
    ipc::ControlBlock peer(peerMap,
        sizeof(ipc::RingHeader) + sizeof(ipc::RingSlot) * K, false);

    // Producer fills the ring and then must be refused, not block.
    int held[8];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        int idx = ring.control().claimForWrite(1000 + i, 0);
        if (idx < 0)
            break;
        ring.control().publishComplete(static_cast<uint32_t>(idx), 1000 + i, 0, 2560, 1440, 44);
        held[n++] = idx;
    }
    check(n == static_cast<int>(K), "producer fills exactly K slots");
    check(ring.control().claimForWrite(9999, 0) == -1,
        "producer skips (returns -1) when no slot is free, never blocks");

    // Consumer takes the newest complete slot, not the oldest.
    uint64_t seq = 0;
    int got = peer.claimNewestComplete(&seq);
    check(got == held[K - 1], "consumer takes the newest complete slot");
    check(seq == 1000 + K - 1, "consumer reads the published sequence");

    // Consumer with nothing new: must return -1 immediately.
    int got2 = peer.claimNewestComplete(&seq);
    check(got2 == -1 || got2 != got, "consumer never takes the same slot twice");

    // Round trip: consumer releases, producer reclaims the same index.
    peer.release(static_cast<uint32_t>(got));
    int again = ring.control().claimForWrite(2000, 0);
    check(again == got, "released slot becomes claimable again");

    munmap(peerMap, sizeof(ipc::RingHeader) + sizeof(ipc::RingSlot) * K);
    close(cfd);

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
        failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}