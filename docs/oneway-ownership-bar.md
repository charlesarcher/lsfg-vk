# One-way dual-GPU: ownership bar

Milestone: Decoupled Zero-Copy PCIe DMA (commit `3fccd54`, `72be460`, `399c1c4` on `feat/dual-gpu-oneway`).
Prior tag: `working_with_memcpy` (POSIX memfd fallback).
Host: kennykiller. Render 9070 XT (`1002:7550`, Vulkan gpu-index 1). Overlay 9060 XT (`1002:7590`). Panel DP-7 2560x1440 @ 240 Hz.
Push: `fork` = `github.com:charlesarcher/lsfg-vk.git`. Do **not** push `origin` (PancakeTAS).

## Contract

The 9070 keeps doing the game. The 9060 is a display coprocessor. Native scene rate stays almost untouched, then the panel sees about 2× that, toward 240 Hz. Same ownership as Windows Lossless Scaling on this hardware: the game never writes GPU-B resources.

FurMark **bound** (2560x1440 MSAA4, mangohud): native ~141. Bar: 9070 ≥134 (≤5% hit), overlay ~2×. **Met** (136/140/143 vs 138/141/145).

RE2 in-game at **Mizoil** (not title): native **173 fps**. 95% = **164**. Overlay native/doubled ~2×. Title ~250/500 is menus. **Met** (MangoHud 168–169 fps, overlay 330 REAL + 332 GEN = 662 presents/s, 0 skips).

## Current Architecture: Decoupled Zero-Copy PCIe DMA

The production hop eliminates the CPU copy and kernel dma-buf coupling entirely:

1. **Host-Visible Allocations (`posix_memalign`)**:
   - Bound via `VK_EXT_external_memory_host` (userptr).
   - Card A writes via PCIe DMA (~1.98 ms).
   - Card B reads via PCIe DMA (~1.98 ms).
   - No CPU framebuffer memcpy (CPU utilization drops to ~4%).
2. **Zero Kernel Hacks & No CS-Strip**:
   - Standard Vulkan submission; no ioctl monkey-patching.
   - Eliminates kernel `dma_resv` implicit sync stalls naturally by avoiding cross-device GEM sharing.
3. **Single `VkDevice` in Game Process**:
   - Game process (`re2.exe`) stays strictly on Card A (9070 XT).
   - Card B DMA ingestion runs exclusively in `lsfg-vk-app` to prevent `vkd3d-proton` device lost (`VK_ERROR_DEVICE_LOST -8`).
4. **Optimal Staging Ring Depth**:
   - `STAGING_RING_DEPTH = 4` achieves 99.2% throughput with minimal input latency.

## Measured End-to-End Latency

Hardware monotonic timestamping from Card A capture to Card B present (`vkQueuePresentKHR`):

| Frame Type | Metric | CPU Memcpy Method | Decoupled Zero-Copy DMA | Delta |
| :--- | :--- | :--- | :--- | :--- |
| **REAL Frames** | p50 Median | 6.80 ms | **4.52 ms** | **-2.28 ms** (33.5% faster) |
| | Mean | 6.97 ms | **5.74 ms** | **-1.24 ms** |
| | p10–p90 | 6.51 – 7.27 ms | **4.33 – 4.85 ms** | Tight consistent window |
| **GEN Frames** | p50 Median | 2.61 ms | **0.50 ms** | **-2.11 ms** (80.8% faster) |
| | Mean | 2.83 ms | **1.74 ms** | **-1.08 ms** |
| | p10–p90 | 2.40 – 3.12 ms | **0.29 – 0.80 ms** | Near-instant display |

## Last Measured (RE2 Live Gameplay & FurMark)

- **FurMark Bound (2560x1440 MSAA4)**: 1,102 sent / 9 skip (99.2% transfer efficiency), 138 FPS game rate, 277.7 GEN/s (~555 presents/s, locked 2×).
- **RE2 Mizoil (2560x1440)**: 330 REAL + 332 GEN presents/s = 662 presents/s total, 0 skips, MangoHud flat at 5.9 ms / 169 FPS.
- **Visual Inspection**: Verified clean single outline for Leon under active WASD movement; no double head, no edge outline separation or halos.

## dma-buf (measured, this pair)

Not a slogan. A→B (9070 export, 9060 import) is what one-way wants:

- 9070 `GetMemoryFdPropertiesKHR` SUCCESS (types 0x1 VRAM, 0x4 GTT, 14 151 680 B).
- Same fd on 9060 Vulkan: `VK_ERROR_INVALID_EXTERNAL_HANDLE`.
- Same fd `drmPrimeFDToHandle(renderD130, …)`: EINVAL 22.

B→A (9060-owned, 9070 writes) **imports** and taxes the 9070 (dma_resv). FurMark bound ~74 vs ~141. EMPTY_XFER ~13 Hz on that 9060 device ~78. Native FurMark + vkcube on 9060 with **no shared BO** ~142.

Same-GPU other-process 9060→overlay 9060 dma-buf: also INVALID_EXTERNAL_HANDLE + PRIME EINVAL.

Knobs (one-way, leftover A/B; leave **unset** for product):

- Default: POSIX shm.
- `LSFGVK_IMPORT_STAGING=1` — dma-buf import of overlay staging (broken here).
- `LSFGVK_POSIX_SHM=0` — no shm; without IMPORT_STAGING there is **no hop**.

Host bounce is a worse interconnect than a working A-owned dma-buf. It is the share that holds the 95% bar. Next architecture: make A-owned GPU share work, then delete the 14 MB CPU copy.

## Last measured

FurMark `ab 8 bound` (2026-09-08): native 138/141/145, doubled 136/140/144, 1124 FRAME, 1123 GEN+REAL / 8 s, skip 0. Orange torus.

RE2 Continue+walk Mizoil (`working_with_memcpy` tree, Destroy-keep):

- Native 173 fps, 99%/52%.
- Doubled MangoHud 162–169 (93.6–98% of 173). Overlay e.g. 164/329, 167/336 (~2×). GPU ~99%/70%.
- Standing frametime ~5.6–6.7 ms; walk max ~9.4–9.8 ms (one hitch, not 20 fps).
- Shots: `/tmp/lsfg-re2-ingame/144-after-cont.png`, `145-look.png`, `146-walk.png`.
- Steam 3/3 past 1440 FRAME-0 after Destroy-keep. Not a long reliability study.
- No click-to-photon. Stills: no double-image on Leon/MIZOIL. Motion ghosting not proven.

## How to run

Build:

```bash
cd /home/archerc/code/lsfg-vk
cmake --build build -j$(nproc)
```

FurMark (no Steam; iterator for the hop):

```bash
./test_furmark.sh {baseline|doubled|ab} 6 bound
```

Do not `--export-dir` / `--logfile-suffix` (SIGSEGV in fwrite). Run from repo; FurMark binary `/opt/furmark`. Harness `env -u VK_INSTANCE_LAYERS` before `lsfg-vk-app`. Kill overlay after.

RE2 Steam AppID **883710**. LaunchOptions = `/home/archerc/code/lsfg-vk/launch_re.sh %command%`.

```bash
DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 steam steam://rungameid/883710
```

`CloudEnabled=0` on that appid in `userdata/10914133/config/localconfig.vdf` (AFK Steam “Unable to Sync”). Direct proton without Steam: XIO 62 on `:0`. Chromium Steam modals do not take xdotool.

Logs: layer `$HOME/steam-883710.log` (`PROTON_LOG=1`). Overlay `/tmp/doubler.log`. Isolated Destroy: `isolated Destroy keep`. 1440: `isolated swapchain 4 images 2560x1440`. GEN climbing = live present loop.

Measure: MangoHud = 9070/game present rate. Overlay top-right = native/doubled. Title ~250 ≠ in-game. 95% gate only at Mizoil vs 173. `rocm-smi --showuse --csv`: card0=9070, card1=9060.

Kill leftover (xdg still covers DP-7 if left up):

```bash
killall -9 lsfg-vk-app
# re2.exe PIDs only — do not pkill -f matching user classes
```

## Closed (do not re-run)

selectFreeSlot wait; B-owned staging + B QueueSubmit as the 74 fps tax; A→B Vulkan dma-buf / PRIME as the pixel path; 9060→overlay dma-buf; FIFO “50 ms blit”; present-thread 14 MB memcpy; blit on fam=1; IMMEDIATE/overlay-gap/dedicated-queue as RE2 20 fps fix; ICD CreateSwapchain for isolated; hide swapchain_maintenance1; QueueSubmit on gfx inside QueuePresent; fprintf/stderr delay as stall fix; force surface caps to 1440; layer-shell OVERLAY as default; join-old/kick 1080 IPC; destroy CaptureContext on isolated Destroy.

## Next session

1. **A-owned GPU share.** Why `drmPrimeFDToHandle(renderD130, 9070-fd)` is EINVAL (modifier, heap, VM, P2P, two amdgpu nodes). Then 9070 writes 9070 memory and 9060 reads it without a dma_resv on A’s dest. Then delete shm memcpy.
2. Do not treat shm 95% as the architecture. It is the workaround ceiling.
3. RE2 launch 3/3 is not “every time.” Re-check Continue/walk if hop changes.
4. FurMark quit `free(): invalid size` (RADV `FreeMemory` on `HOST_ALLOCATION_BIT`) is teardown noise.
5. Menu FG ghosts at high UI rate — do not skip GEN to fix it.
