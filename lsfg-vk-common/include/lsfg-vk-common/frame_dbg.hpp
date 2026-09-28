#pragma once

/// Shared by the layer and the app: both sit on the capture/present path.

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
#define LSFG_FRAME_DBG_PRINTF(...) ::fprintf(stderr, __VA_ARGS__)
/// Frame-path diagnostic. Compiled out entirely in a normal build.
#define LSFG_FRAME_DBG(...) LSFG_FRAME_DBG_PRINTF(__VA_ARGS__)
#else
#define LSFG_FRAME_DBG_PRINTF(...) ((void)0)
#define LSFG_FRAME_DBG(...) ((void)0)
#endif
