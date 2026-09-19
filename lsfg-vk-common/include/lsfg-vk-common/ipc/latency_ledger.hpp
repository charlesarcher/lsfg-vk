#pragma once
// Session 40 latency campaign: click->photon through the doubled path.
//
// Contract (all CLOCK_MONOTONIC ns, absolute):
//   /dev/shm/lsfg-dbl-layer  —  LAYER writes one 16-byte row per captured frame:
//       [ uint64 captureTsNs, uint64 fidx ]  (single writer: the layer hook)
//   /dev/shm/lsfg-dbl-ledger —  APP writes one 32-byte row per REAL present:
//       [ uint64 captureTsNs, uint64 presentedNs, uint64 seq ] (seq = ring order)
//
// The click probe (tools/latency/probe_vk.c) reads both:
//   1. commits the magenta frame on click (t_click stamped at uinput write);
//   2. reads the newest lsfg-dbl-layer row -> captureTsTs of its frame;
//   3. scans lsfg-dbl-ledger rows for that captureTsNs -> presentedNs;
//   sample = presentedNs - t_click.
//
// Env-gated: both producers only map/write when LSFGVK_DBL_LEDGER=1.
// Files are opened O_CREAT|O_RDWR, ftruncate'd, mmap'd MAP_SHARED. Lockless:
// each file has exactly one writer; consumers tolerate torn last row by
// scanning the previous power-of-two bounded window (seq advances, never
// rewrites; NO overwrites needed at 240 Hz * 32B = 8 KB/s vs 128 KB file).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lsfgvk {
namespace ledger {

// ---- layout constants (probe reads these, keep byte-exact) ----
inline constexpr uint64_t LAYER_MAGIC  = 0x4c44424c41594552ULL;   // "LDBLAYER"
inline constexpr uint64_t LEDGER_MAGIC = 0x4c44424c45444745ULL;   // "LDBLEDGER"
inline constexpr size_t   LAYER_ROW    = 16;                      // captureTsNs, fidx
inline constexpr size_t   LEDGER_ROW   = 32;                      // captureTsNs, presentedNs, seq
inline constexpr size_t   LEDGER_CAP   = 4096;                    // rows in the ring
inline constexpr size_t   LEDGER_FILE  = LEDGER_ROW * LEDGER_CAP; // 128 KiB
inline constexpr size_t   LAYER_FILE   = 4096;                    // magic+count+head rows
// layer file map: [u64 magic][u64 headIndex][.. rows at 16 + head*16 ..] (one row used)

// ---- mapping helper: returns MAP_FAILED-safe unmapping wrapper ----
struct Shm {
    void*  map{nullptr};
    size_t size{0};
    int    fd{-1};

    bool open(const char* path, size_t sz, bool creat) {
        int flags = creat ? (O_RDWR | O_CREAT) : O_RDWR;
        fd = ::shm_open(path, flags, 0600);
        if (fd < 0) {
            std::fprintf(stderr, "lsfg-vk: shm_open(%s) failed errno=%d\n",
                path, errno);
            return false;
        }
        if (creat && ::ftruncate(fd, static_cast<off_t>(sz)) != 0) {
            std::fprintf(stderr, "lsfg-vk: ftruncate(%s) failed errno=%d\n",
                path, errno);
            return false;
        }
        void* m = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            std::fprintf(stderr, "lsfg-vk: mmap(%s) failed errno=%d\n",
                path, errno);
            return false;
        }
        map = m;
        size = sz;
        return true;
    }

    void close() {
        if (map && size) ::munmap(map, size);
        if (fd >= 0)    ::close(fd);
        map = nullptr; size = 0; fd = -1;
    }
};

inline bool ledgerEnabled() {
    const char* e = ::getenv("LSFGVK_DBL_LEDGER");
    return e != nullptr && e[0] != '0' && e[0] != '\0';
}

// ---- LAYER producer: one row per captured frame ----
struct LayerSink {
    Shm shm;
    bool ok{false};

    void init() {
        if (ok || !ledgerEnabled()) return;
        if (!shm.open("/lsfg-dbl-layer", LAYER_FILE, /*creat*/ true)) return;
        ::memset(shm.map, 0, LAYER_FILE);
        auto* magic = static_cast<uint64_t*>(shm.map);
        *magic = LAYER_MAGIC;
        ok = true;
        std::fprintf(stderr, "lsfg-vk: dbl-ledger layer sink armed (%s)\n",
            "/dev/shm/lsfg-dbl-layer");
    }

    void publish(uint64_t captureTsNs, uint64_t fidx) {
        if (!ok) return;
        auto* head = static_cast<uint64_t*>(shm.map) + 1;  // after magic
        uint64_t h = (*head)++;                     // single writer: plain ++
        size_t off = 16 + (h % 254) * LAYER_ROW;    // 253 rows, wrap
        uint64_t* row = reinterpret_cast<uint64_t*>(
            static_cast<char*>(shm.map) + off);
        row[0] = captureTsNs;
        row[1] = fidx;
        // observers read rows by scanning backwards from head; the seq field
        // is implicit (row slot) so no torn row matters — scan finds ts match.
    }
};

// ---- APP producer: one row per REAL present ----
struct LedgerSink {
    Shm shm;
    bool ok{false};
    uint64_t seq{0};

    void init() {
        if (ok || !ledgerEnabled()) return;
        if (!shm.open("/lsfg-dbl-ledger", LEDGER_FILE, /*creat*/ true)) return;
        auto* rows = static_cast<uint64_t*>(shm.map);
        if (rows[0] != LEDGER_MAGIC) {
            ::memset(shm.map, 0, LEDGER_FILE);
            rows[0] = LEDGER_MAGIC;      // first 8B: magic
            rows[1] = 1;                 // second 8B: next write slot (1-based,
                                         // row i written at slot i % LEDGER_CAP)
        }
        seq = 0;                          // fresh seq per run (scan by ts)
        ok = true;
        std::fprintf(stderr, "lsfg-vk: dbl-ledger app sink armed (%s)\n",
            "/dev/shm/lsfg-dbl-ledger");
    }

    void publish(uint64_t captureTsNs, uint64_t presentedNs) {
        if (!ok) return;
        auto* hdr = static_cast<uint64_t*>(shm.map);
        uint64_t slot = hdr[1]++;                       // next write index
        uint64_t* row = hdr + 2 + (slot % (LEDGER_CAP - 1)) * 4;
        row[0] = captureTsNs;                            // capture time of frame
        row[1] = presentedNs;                            // compositor latch
        row[2] = ++seq;                                  // monotonic row count
        row[3] = 0;                                      // reserved
    }
};

}  // namespace ledger
}  // namespace lsfgvk
