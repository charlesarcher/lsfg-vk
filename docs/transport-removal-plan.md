# Removing every transport except DecoupledDma — research and plan

Date: 2026-10-12. Branch: `debug/ring-2026-10-10`. Status: **plan only, nothing deleted yet.**
Baseline to protect: journal S46 — 131.46 frames/s into the doubler, 262.95/s out
(7873 real + 7873 generated), app tick gap p50/p90/p99 = 3.566 / 5.670 / 6.13 ms,
180 s run holds 260.03/s, pixel stalls 0.045% in the steady window, amdgpu faults 9 -> 9.

## 1. Scope

Delete `PosixShm`, `DmaBuf`, `Udmabuf`, `UdmabufRing` from `ls::Transport` and every code
path that exists only for them. `DecoupledDma` stays byte-for-byte in behaviour: this is a
subtraction, not a re-engineering of the surviving path.

Non-goals for this pass: fixing the hardcoded second-device id (`capture_context.cpp:1106`
matches `deviceID == 0x7590`), and re-architecting the STAGING handshake beyond what the
deletion forces. Both are listed as follow-ups.

## 2. What must survive, and why it looks droppable

These are the traps. Each is load-bearing for `DecoupledDma` even though it is named or
shaped like doomed-transport code.

| Survives | Anchor | Why |
|---|---|---|
| STAGING x `STAGING_RING_DEPTH` + READY handshake | app `stream.cpp:455`, `:464`; layer `capture_context.cpp:842` loop | In decoupled the app still allocates `STAGING_RING_DEPTH` plain device images, exports each, and sends `Staging` with the fd; the layer receives all of them and closes the fd (`:874`, the log line "STAGING fd closed, transport is not udmabuf"). It is a pure barrier, but the layer blocks on it, so removing it without a replacement deadlocks startup. |
| `Release` message and `slotFree` | layer `capture_context.cpp:1505-1548` (`drainReleases`), `:1654-1657` (slot pick); app `presentation.cpp:3578` | The decoupled copy path sends `Release` after its GPU copy; that is what makes a slot reusable. Only the early "Release first" variant at `:2875` is already skipped for decoupled. |
| `Frame` + its sync fd, and `bExportFds` | layer `:549`, `:1144`, `:1215`, `:1250`, `:1275`, `:1376`, `:2562` | `bExportFds` is the decoupled VRAM dma-buf export table. The FRAME fd for slot 0 is a sync file, later fds carry the shared buffer (`presentation.cpp:3314-3318`). |
| `state.aImports` (app) | `stream.hpp:47`, used `presentation.cpp:3323-3350` | Named as if it were the p2p transport's, but decoupled imports the layer's VRAM dma-buf into it. Keep the member, drop the p2p branch that also writes it. |
| `hostImages` **on both sides, with different meanings** | layer `capture_context.hpp:119` = decoupled's own host allocation (KEEP). app `stream.hpp:73` = the udmabuf bounce image (DELETE). | Same identifier in two processes with opposite fates. This is the single most likely way to break the surviving path with a search-and-replace. |
| `bVk`, `bHostImages`, `bVramImages`, `bCbs`, `bFences`, the dual-host block | layer `:1095-1160`, submit at `:2398-2435` | This *is* decoupled. |

## 3. Blast radius, measured

Counts are lines mentioning a doomed transport, its exclusive symbols
(`CopyHop`, `UdmabufRing`, `udmaBuf*`, `hostBounce`, `posixShm`, `ringSlot*`), or both.

| File | Hits | Size | Fate |
|---|---|---|---|
| `lsfg-vk-layer/src/capture_context.cpp` | 85 | 2677 | modify: `CopyHop` 393-540, ring setup 660-760, STAGING per-transport branches 859-905, ring submit/gate blocks near 1770-1900, `udmaBufs`/`udmaMmapFds` plumbing |
| `lsfg-vk-app/src/presentation.cpp` | 46 | 4042 | modify: source-selection ladder 3303-3590 (`hostImages` case, `ringSrc` case), `dmaHop`, udmabuf release-ordering branch 2865-2875 |
| `lsfg-vk-app/src/stream.cpp` | 55 | 769 | modify: `hostBounce`/`posixShm` 313-461, `udmaBufs` import 542-547, cross-device check 575+, ring receive block 652-710 |
| `lsfg-vk-common/src/ipc/udmabuf_ring.cpp` + `.hpp` | 27 | 164 + 67 | delete file |
| `lsfg-vk-common/src/ipc/control_block.cpp` + `.hpp` | 7 | 124 + 105 | delete file (only the ring uses it) |
| `lsfg-vk-common/include/.../configuration/config.hpp` | 13 | 193 | modify: enum 41-79 becomes one value, drop `ringSlots` 106 |
| `lsfg-vk-common/src/configuration/config.cpp` | 11 | 403 | modify: `transportFromString` 142-155, `resolveUnsetTransport` 21-37, `ring_slots` parse 186 |
| `lsfg-vk-common/include/.../ipc/protocol.hpp` | — | — | delete `RingSetup` 160, `RingSlotFd` 169, `RingCtrl` 177 |
| `lsfg-vk-common/src/ipc/socket.cpp` | — | — | delete the ring codecs and type names (`:354-413`) |
| `lsfg-vk-common/CMakeLists.txt` | 2 | — | drop lines 7-8 |
| `lsfg-vk-app/include/lsfg-vk-app/stream.hpp` | 11 | 178 | modify: drop `udmaFds`, `udmaBufs`, `hostImages`, `dmaMaps`, `shmMaps`, `shmSeq`, `shmSeen`, `shmBytes`, `hostPtrs`, `ringSlot*`, `state.transport` 140 |
| `lsfg-vk-layer/include/lsfg-vk-layer/capture_context.hpp` | 10 | 251 | modify: drop `ring` 167, `ringPending` 197, `ringSubmitQ` 198, `ringRecycleFence`/`ringInFlightFence`, `copyHop` 127, `udmaBufs` 117, `udmaMmapFds` 118, `shmMaps` 125, `shmSeq` 126, include of `udmabuf_ring.hpp` 9 |
| `lsfg-vk-layer/src/entrypoint.cpp` | 5 | 1569 | modify: three `== UdmabufRing` gates at 966-1035 collapse to unconditional |
| `lsfg-vk-layer/src/instance.hpp` | 1 | — | `transport()` 39-44 returns a single value or goes away |
| `lsfg-vk-app/src/gui.cpp` | 1 | 531 | delete the combo 423-429. `g_guiState.transportMode` (`gui.hpp:35`) has **no writer other than the combo and no reader** — the control is decorative today, so removing it costs nothing. |
| `tools/testing/test_control_block.cpp` | 81 | — | delete with the ring |
| `tools/testing/*udmabuf*` (8 probes) + `test_udmabuf.sh` | 3055 | — | decision needed, see 4.B |
| `tools/testing/profiles/furmark-decoupled.toml` | sets `transport = "udmabuf"` twice (20, 38) | — | must be migrated or deleted |
| `tools/testing/profiles/re2.toml` | sets `transport = "udmabuf"` twice (15, 29) | — | must be migrated or deleted |
| `docs/Configuration.md` 27-31, `docs/One-Way-Dual-GPU-Guide.md` (4), `docs/Dual-GPU-Guide.md` (1), `docs/Troubleshooting.md` (1), `tools/README.md` (6) | — | — | rewrite to one transport |

There is **no test suite to update**: `test/` does not exist and no `add_test`/`enable_testing`
is registered anywhere outside the build tree. `tools/testing/test_control_block.cpp` and
`test_udmabuf.sh` are hand-run tools, not CTest targets. So verification is compile plus
runtime A/B, never "tests are green".

Rough total: about 550 lines deleted as whole files, 150-200 lines removed from live files,
and 250-350 lines of branch-simplification edits. `capture_context.cpp` and
`presentation.cpp` should each end up a few hundred lines shorter.

## 4. Two decisions that change the work

**A. A profile that names a removed transport.** The default config file writes no
`transport` key at all (`config.cpp:39-95`, only a comment), so fresh installs are
unaffected. Existing files on this branch do name it. Two options:
1. **Hard error** — `transportFromString` throws `transport "dmabuf" was removed; decoupled is the only transport`. Explicit, and the reason upstream users would notice is stated in the message. Recommended.
2. Silent upgrade with a warning — nothing breaks, but a stale profile hides the fact that the knob is gone.

Either way `furmark-decoupled.toml` and `re2.toml` must stop setting `udmabuf`.

**B. The ring files.** Journal S45 records the standing rule: "The ring WIP is preserved in
a commit, not deleted." Deleting `udmabuf_ring.{hpp,cpp}` + `control_block.{hpp,cpp}` keeps
that true in the git sense (they stay reachable from `ac73740`), but they leave the working
tree. Options: delete from the tree (cleanest, matches "greatly simplify this branch"); or
keep the files compiled and unreachable (tree stays cluttered, no dead-code warning value).
Recommended: delete, and say so in the commit message with the commit that still holds them.

## 5. Execution order — the tree builds at the end of every step

The enum cannot be shrunk before its references go, because every target is one
compilation unit set: each step deletes one transport's enum value *and* all of its
references in the same commit, in order of decreasing isolation.

1. **`UdmabufRing`** — most isolated: enum value, `udmabuf_ring.{hpp,cpp}`,
   `control_block.{hpp,cpp}`, CMake 7-8, protocol `RingSetup`/`RingSlotFd`/`RingCtrl` +
   `socket.cpp` codecs, layer ring setup/submit/gates, `entrypoint.cpp` 966-1035,
   `ringSlots` + `ring_slots`, app `stream.cpp:652-710` + ring state members,
   `test_control_block.cpp`, ring probes.
2. **`Udmabuf`** — `hostBounce`, app `udmaFds`/`udmaBufs`/`dmaMaps`/`hostImages`(app),
   layer `udmaBufs`/`udmaMmapFds` and the STAGING import branch 859-868, the release
   ordering special case at `presentation.cpp:2865-2875`, the "STAGING fd closed" log line
   becomes the only STAGING behaviour.
3. **`PosixShm`** — `CopyHop` 393-540 and `copyHop` member, layer `shmMaps`/`shmSeq`/
   `shmMMap` receive branch 880-905, app shm staging 412-449, `hostPtrs`,
   `LSFGVK_POSIX_SHM`, `instance.hpp:39-44` default, the `sourceImages` case of the blit
   ladder.
4. **`DmaBuf`** — last, because it shares `aImports` with decoupled: delete the p2p import
   branch and `dmaBufSent`, keep the member and the decoupled writer. Also retire
   `LSFGVK_DUAL_HOST`, whose only purpose is toggling the dma-buf hop
   (`presentation.cpp:2870-2875`).
5. **Collapse** — `transportFromString` per decision A (or delete the key entirely),
   `resolveUnsetTransport` gone, `GameConf::transport`/`transportExplicit` gone or fixed,
   GUI combo gone, docs rewritten, stale comments corrected including the stale
   `DecoupledDma` enum comment (it claims the app maps the shared buffer; it does not —
   `stream.cpp:324-327` says so).

Optional step 6, only if the STAGING barrier is to be simplified: make the app send
`Staging` with no fd, drop the three dummy images at `stream.cpp:457-461`, and change the
layer to stop requiring `takeReceivedFd() >= 0` at `:855-857`. That is a protocol change on
a path that currently works, so it belongs in its own commit after the removal is proven.

## 6. Verification, per step

1. `bash tools/testing/verify_build.sh` must print all trees built, hashes unchanged for
   the profile files.
2. Runtime gate: `./tools/testing/lsfg-launch.sh furmark-decoupled-dma`, then compare
   against S46 — game within a few percent of 131/s, presents = 2x received within 0.2%,
   delivery at least 99.8%, tick p50/p90/p99 within about 10% of 3.57/5.67/6.13 ms,
   `furmark exit=0`, `cards still present`, `dmesg` amdgpu fault count unchanged at 9.
3. After step 5 only: the capture gate — `furmark-decoupled-dma-record.sh`, then
   `hitch_check.py` over 2-58 s must stay near the 0.045% baseline.
4. Startup is the specific thing most likely to break (the STAGING barrier and the
   non-blocking `drainReleases` poll, whose comment at `:1517-1527` records a previous
   first-present deadlock). Any hang at "waiting for STAGING" is a step-local revert, not a
   debug session.

Rough cost: 5 builds plus 5 x 70 s runs, about 20 minutes of machine time on top of edits.

## 7. Rollback and branch policy

One commit per transport, so `git revert` of a single step restores one transport. Keep this
on `debug/ring-2026-10-10`: `pr/oneway-dual-gpu` is the upstream-facing branch and
upstream still documents four transports; deleting them there would be a product decision,
not a simplification.

## 8. Follow-ups this plan deliberately does not include

- `selectB` hardcoded `0x7590` (`capture_context.cpp:1106`): once decoupled is the only
  path, a machine whose second card is not a 9060 XT silently loses the dual-host path and
  logs only `dual-host: no known dual-device`. It should select "the device that is not the
  game's", and fail loudly when there is none.
- The ~44.6 ms `longest_gap_ms` present-counter gap recorded in S46.
- The `Frame.stagingIdx` field, which only means something when the staging slots are real
  buffers; after the removal it is a slot token in a protocol that no longer shares
  buffers. Clean it up only after the runtime gate is green.

## Step 1 status (2026-10-12) - COMPLETE, both targets build

Ring is gone from `lsfg-vk-common`, `lsfg-vk-layer` and `lsfg-vk-app`; `lsfg-vk-layer` and
`lsfg-vk-app` compile with zero errors, grep finds no ring symbol anywhere in those three
trees. Line counts removed: ~150 layer setup block + 312 layer present-path + 60 header
members + ~230 app (staging branch, 82-line fd handshake, 20-member block, 147-line consume
loop, release path) + protocol/socket/config.

Tooling lesson that cost three broken builds: never delete by line number across several
sites in one pass. Two braces were eaten that way - the `}` that closed the `ErrorMsg` branch
in the layer's `drainReleases`, and the `{` of the app's `shmBytes` guard - and each produced
a cascade of 40+ unrelated "not declared in this scope" errors. Delete by anchor, re-derive
block extents from the modified file, and run the brace scan after every file.

Still to do for this transport (non-blocking): the tools/testing ring probes
(`udmabuf_ring_probe.cpp`) are not in any CMake target, so they linger as dead files until
the tools pass.

## Step 2 (Udmabuf host bounce) - exact cut list, measured 2026-10-12

Layer (`capture_context.cpp`, numbering at the ring-cut checkpoint):
- `:708-718` the STAGING import branch (`dup` the app fd, `udmaBufs.emplace(vk, fd, bytes)`,
  keep `mapFd` in the array). Removing it routes every transport through the existing
  close-and-continue path, so the STAGING/READY barrier still holds - the fd is closed for
  all transports and `imported` still reaches `STAGING_RING_DEPTH`.
- `:550` `udmaMmapFds.fill(-1)`, `:1042-1043` move-ctor initialisers, `:1090` `fill(-1)`,
  `:1141` the loop closing `udmaMmapFds`.
- `:1635-1692` the render-side `if (!emptyFrame && !exportIsolatedOn() && udmaBufs.at(slot))`
  branch: 57 lines of `CmdCopyImageToBuffer` plus its two image barriers.
- `:1893-1895` the `finishUdmaByteRef` call, its definition at `:153`, and helpers used only
  by it.
- Header: `udmaBufs` `capture_context.hpp:110`, `udmaMmapFds` `:111`.

App: `stream.cpp:321-331` `hostBounce`/`posixShm` selection, `:356-431` the bounce staging
creation and udmabuf export-and-send, `Ready`'s `dmaBufFds` handling, `presentation.cpp:2861`
the `shmBytes`/`dmaMaps` snapshot branch, and the bounce-only members `hostPtrs`, `udmaFds`,
`udmaBufs`, `dmaMaps` in `stream.hpp`. `shmBytes`, `aImports`, `dmaFds`, `dmaDstFds` stay for
PosixShm/p2p until steps 3 and 4.

**Finding that corrects step 2 of the execution order**: the layer's bounce buffers are
referenced from the *shared* present path (`:1635`, `:1893`), not only from transport-specific
code. They are inert for `DecoupledDma` only because `udmaBufs.at(slot)` is filled exclusively
by the STAGING import branch. So the cut deletes guarded branches in place; it cannot drop
whole functions, and `copyToStagingAndSignal` must keep its shape.

## Step 2 COMPLETE, step 3 code half done (2026-10-12)

Step 2 (`Udmabuf` host bounce) is gated: full build 0 errors; FurMark
`presents/s=131.45 gen=7888 real=7888 longest_gap_ms=45.349 elapsed_s=60.006` against S46
`131.30 / 7879 / 45.501` - +0.1% with `gen == real`, so nothing is dropped. The `LSFGVK-BENCH`
lines land in the run dir's `launch.log`, not `app.log`.

Step 3 (`PosixShm`) code half is in, and the whole project still builds (0 errors):
- Layer: the STAGING loop no longer mmaps the peer memfd - it validates the message, takes the
  fd, closes it and continues, which still satisfies the STAGING_RING_DEPTH barrier the app
  blocks on. The dual-host host-malloc capture destination and the `dual-host skipped` branch
  are gone; the hop job lost its shm arm, so `CopyHop` is now fed only by raw re-export.
- App: the `posixShm` staging branch (memfd, mmap, mlock, `posix_memalign` host buffer, the
  `Staging` fd hand-off) is gone and the decoupled staging creation after it is untouched.
  `StreamState`'s destructor body, which opened with `if (shmBytes == 0) return;` and so never
  ran for decoupled, is gone with it. `shmMaps`, `shmSeq`, `shmSeen`, `shmBytes`, `hostPtrs`
  and `hostImages` no longer exist in `StreamState`.

Remaining in step 3: the layer's write-nothing `shmMaps`/`shmSeq` members and the
`} else if (this->shmMaps.at(0))` arm of the FRAME fd chain; after that `CopyHop` can go, which
belongs with the raw re-export path in step 4. `Transport::PosixShm` itself stays until the
config collapse.

## Step 4 (DmaBuf p2p) - measured, and it is smaller than the plan assumed

`Transport::DmaBuf` no longer appears anywhere in the layer or the app: four `Transport::`
comparisons survive, three of them `== DecoupledDma` (layer `capture_context.cpp:541` for the
second-card device, app `presentation.cpp:2744` and `:2905`) and one `!= DecoupledDma`
(`presentation.cpp:2697-2702`, which is now dead because decoupled is the only transport).

What is actually left of the p2p era is the `LSFGVK_DUAL_HOST=0` "hop" experiment:
- `DmaHopTs g_dmaHopTs` (`presentation.cpp:516`) with its query-pool timing helpers,
  `ensureDmaIn()` (655-721) and the second timing block around 760-850 that returns
  `state.dmaDstFds.at(sidx)`.
- `dmaHop` declared at 2693-2694 and used at 2697, 2751-2757, 2778-2779, 2791-2796, 2814-283x,
  3357-3360, 3439; `pendingDmaRelease` declared at 2572 feeds those.
- `StreamState::dmaDstFds` (`stream.hpp:53`) exists only for that hop.

Do NOT remove `dmaFds` or `aImports`: 2704-2750 is the decoupled path keeping the render
card's dma-buf per slot (`state.dmaFds.at(slot) = dup(captureFd)`, invalidating `aImports`),
and 2744 closes later FRAMEs' duplicate fds on that same path. That is how DecoupledDma
receives the 9060 buffer.

So step 4 = delete the hop experiment (about 320 lines in `presentation.cpp` plus the member),
and step 5 = the enum, `transportFromString`, the GUI transport combo, the profiles and the docs.

## Step 1 status (superseded earlier note)

Done and compiling: `lsfg-vk-common` — ring files deleted, CMake lines gone, protocol
`RingSetup`/`RingSlotFd`/`RingCtrl` plus codecs removed, `Message` variant trimmed, enum
value, `ringSlots` and `ring_slots` gone. Layer header lost its 60-line ring member block.

The tree does NOT build right now. Remaining ring sites, as reported by the compiler:

- `capture_context.cpp`: ring setup 665-814 (ends at a `} else {`), the `!= UdmabufRing`
  guard at 842 with its closing brace at 935, move-ctor initialisers 1191-1198, an
  `&& transport == UdmabufRing` block at 1549-1560, `flushRingCopy` 1567 onward, the log
  argument at 1675, `ringPath` at 1781-1782 with the publish loop 1787-1796, `ringSeq` claim
  path 1808-1825, the copy-submit machinery 1969-2035 and 2131-2142.
- `entrypoint.cpp`: call sites 819 and 991, plus the three `== UdmabufRing` gates 966-1035.
- `instance.hpp` 113/116 and `instance.cpp` 555-583: `Root::flushRingCopy` and
  `Root::setRingRecycleFence` and their fan-out to every capture context.
- Then the app side: `stream.cpp` 329-345 and 652-710, `stream.hpp` ring members,
  `presentation.cpp` 1530-1537, 2633-2703, 2768, 3041, 3585-3600.
