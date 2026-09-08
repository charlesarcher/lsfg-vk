# One-way dual-GPU: ownership bar (2026-09-07)

FurMark iterator on kennykiller: 9070 XT render, 9060 XT overlay, DP-7 2560x1440 @ 240 Hz.
Config: `furmark-oneway` / `app-oneway`, `presentation = "external"`, multiplier 2.

## Contract

The 9070 keeps doing the game. The 9060 is a display coprocessor. Native scene
rate stays almost untouched, then the panel sees about 2× that, toward 240 Hz.

On FurMark **bound** (2560x1440 MSAA4, mangohud): native ~141. Bar: 9070 ≥134
(≤5% hit), overlay ~2× toward 240. Same ownership as Windows Lossless Scaling
on this hardware: the game never writes GPU-B resources.

RE2 (~200 native) is the product gate. Steam is the wrong iterator until the
FurMark 9070 half is real **with honest pixels**.

## Last measured (FurMark doubled bound)

- 135 / 140 / 144 on the 9070 (min on the ≥134 bar)
- Overlay MAILBOX, ~837 GEN + 837 REAL / 6 s ≈ 2× presents
- Dump mean RGB ~116/68/16 (orange FurMark torus). VkFormat(44) is
  B8G8R8A8_UNORM; hop dest is RGBA — CopyHop swizzles R/B
- Overlay not left running after `test_furmark.sh`

## How we got here (elimination, not FG cleverness)

B-owned staging exported to the 9070, then any QueueSubmit on that 9060 device
(even an empty CB at ~13 Hz) contended the BOs. That looked like “B is slow.”
It was A waiting on B’s lock of A’s destination.

| Probe | FurMark bound min/avg/max |
| --- | --- |
| copy, no FRAME | ~134 |
| DROP_GEN (FRAME+Release, no gen) | 132–135 |
| EMPTY_XFER 0 Hz | ~134 |
| EMPTY_XFER ~13 Hz on the 9060 | ~78 |
| product FG on B-owned staging | 63–77 |
| native FurMark + vkcube on 9060 (no shared BO) | ~142 |

Copy GPU time is hundreds of microseconds. Unix-socket FRAME is not the stall.

Working path:

1. Isolated swapchain (default-on for External). Skip-not-wait, ring 2.
2. Capture `copyImage` on extra compute queue fam=1 idx=0, submit fence NULL.
   `vkCmdBlitImage` on that queue DEVICE_LOSTs.
3. Dest is 9070-owned host malloc. Do not import B staging
   (`LSFGVK_IMPORT_STAGING=1` is A/B only).
4. A→B dma-buf and 9060→overlay 9060 dma-buf: `INVALID_EXTERNAL_HANDLE` /
   PRIME EINVAL. Dead on this 9070 XT + 9060 XT pair.
5. Cross-process share: POSIX memfd, raw mmap+memcpy (not Vulkan host import
   of the file map). Overlay memcpy shm → 9060 malloc, generate B-local,
   present overlay only. Default-on for External; `LSFGVK_POSIX_SHM=0` disables.
6. CopyHop: wait dup of capture sync-fd, memcpy+BGRA swizzle, seq bump.
   Never send IPC. FRAME stays on the game present thread.
7. Overlay present MAILBOX (not FIFO). FIFO acquire was the fake ~50 ms “REAL blit”.

## How to run (no Steam)

```bash
./test_furmark.sh {baseline|doubled|ab} 6 bound
```

Do not pass `--export-dir`. Do not default `LSFGVK_TIMING`. Harness
`env -u VK_INSTANCE_LAYERS` before starting `lsfg-vk-app`. Isolated overlay
covers the output; kill `lsfg-vk-app` if a run freezes (no Alt+Tab).

Steam RE2: `/home/archerc/code/lsfg-vk/launch_re.sh %command%` (starts overlay
if missing). Proton log `$HOME/steam-883710.log`. App `/tmp/doubler.log`.

## Open

- RE2 in-game still the product gate (historical isolated ~20 fps on the old
  handshake). Menus ~600 fps + ring-2 skip + FG interp ghost UI.
- FurMark quit-time `free(): invalid size`: RADV `FreeMemory` on
  `HOST_ALLOCATION_BIT`. dma-buf mmap and driver HOST_VISIBLE map both failed
  as replacements (not CPU-visible / stale CPU reads).
- Isolated RE2 first swapchains are 1920x1080; 2560x1440 comes after recreate.
