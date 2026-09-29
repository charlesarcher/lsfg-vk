#pragma once

/// Shared by the layer and the app: both sit on the capture/present path.

#include <cstdio>
#include <cstdlib>

/// Per-frame debug printing, COMPILED OUT by default.
///
/// The frame path (capture, solve, present) runs a few hundred times a
/// second per stream. A runtime env gate is not good enough: the call site
/// still evaluates its arguments, still reads the clock, and still builds
/// the varargs. Any of those is real work on the critical path even when the
/// print never happens.
///
/// LSGV_FRAME_DBG must be a compile-time constant so `dbg(...)` collapses to
/// nothing at all. The arguments are not evaluated, the timestamps are not
/// taken, and the strings do not reach the binary. Turning debugging on is a
/// rebuild, which is the correct trade: a diagnostic that costs nothing in
/// production is worth a rebuild.
#ifdef LSGV_FRAME_DBG
#define LSGV_FRAME_DBG_ENABLED 1
#else
#define LSGV_FRAME_DBG_ENABLED 0
#endif

#if LSGV_FRAME_DBG_ENABLED
namespace lsfgdbg {
    /// Runtime byte budget for the frame-path log. The frame path runs a few
    /// hundred times a second per stream and every WaitForFences/acquire logs,
    /// so an unbounded fprintf filled /tmp (a tmpfs, i.e. RAM) to 1.2 GB in one
    /// run. Cap the volume but KEEP per-frame lines: once the budget is spent
    /// we print one "suppressed" line and then stay quiet, so the shape of the
    /// run is still visible instead of filling memory.
    inline long& budget() { static long b = [] {
        const char* s = ::getenv("LSFGVK_DBG_BUDGET_BYTES");
        return s ? ::atol(s) : 32L * 1024 * 1024;   // 32 MiB default
    }(); return b; }
    inline bool& announced() { static bool a = false; return a; }

    /// Returns true if the caller may print ~n bytes.
    inline bool allow(long n) {
        if (budget() <= 0) {
            if (!announced()) {
                announced() = true;
                ::fprintf(stderr, "[dbg] frame-log byte budget exhausted; "
                    "further frame-path lines suppressed\n");
            }
            return false;
        }
        budget() -= n;
        return true;
    }
}
#define LSFG_FRAME_DBG_PRINTF(...) \
    do { if (::lsfgdbg::allow(160)) ::fprintf(stderr, __VA_ARGS__); } while (0)
/// Frame-path diagnostic. Compiled out entirely in a normal build.
#define LSFG_FRAME_DBG(...) LSFG_FRAME_DBG_PRINTF(__VA_ARGS__)
#else
#define LSFG_FRAME_DBG_PRINTF(...) ((void)0)
#define LSFG_FRAME_DBG(...) ((void)0)
#endif
