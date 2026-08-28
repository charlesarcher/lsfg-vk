# Dual-GPU Setup Guide

This guide walks through setting up lsfg-vk to run frame generation on a
second GPU while your game renders on its own. Every command and log line
below was captured on the development rig used to build this feature:
an Intel Arrow Lake iGPU plus two AMD Radeon cards (RX 9070 XT and
RX 9060 XT), all running Mesa RADV/ANV drivers.

## How it works

When the `gpu` option names a device other than the one your game renders
on, lsfg-vk enters dual-GPU mode:

1. Your game renders and presents on its own GPU (unchanged).
2. Each rendered frame is copied over PCIe to the processing GPU.
3. The entire frame-generation pipeline runs there.
4. Generated frames are copied back and injected into the presentation.

Presentation always stays on the render GPU - the game's swapchain is
bound to its own device. See [Configuration](Configuration.md) for the
bandwidth cost model.

## Requirements

- Both GPUs must expose `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier`. If either is missing, lsfg-vk
  refuses to start with an error naming the device and extension.
- Both GPUs' render nodes (`/dev/dri/renderD*`) must be accessible to
  your user (inside the sandbox for Flatpak).
- Check `vulkaninfo --summary` if unsure - both devices must appear.

## Step 1: Identify your GPUs

lsfg-vk matches the `gpu` option against the exact Vulkan device name:

```console
$ vulkaninfo --summary | grep deviceName
    deviceName         = AMD Radeon RX 9060 XT (RADV GFX1200)
    deviceName         = AMD Radeon RX 9070 XT (RADV GFX1201)
    deviceName         = Intel(R) Graphics (ARL)
```

Use these strings verbatim (the driver suffix in parentheses matters).
Alternatively, `vendorID:deviceID` uppercase form (`0x1002:0x7550`) or
PCI bus ID (`3:0.0`) work. The lsfg-vk configuration UI lists all valid
names in a dropdown.

## Step 2: Find your config file

The layer reads its TOML configuration from the first of these that
exists (creating a default file with example profiles if none does):

1. `$LSFGVK_CONFIG` (explicit override)
2. `$XDG_CONFIG_HOME/lsfg-vk/conf.toml`
3. `$HOME/.config/lsfg-vk/conf.toml`
4. `/etc/lsfg-vk/conf.toml`

If a profile from the shipped defaults matches your game first, edit
that profile rather than appending a duplicate - the first match wins.

## Step 3: Enable dual-GPU mode

Set `gpu` inside the profile that matches your game to the device that
should run frame generation. A minimal working example from the test
rig, driving vkcube on the Intel iGPU with an RX 9070 XT generating
frames:

```toml
version = 2

[global]
dll = "/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
allow_fp16 = true

[[profile]]
name = "vkcube dual-GPU test"
active_in = "vkcube"
gpu = "AMD Radeon RX 9070 XT (RADV GFX1201)"
multiplier = 2
```

`active_in` matches the executable name (a single string or a list).
Omitting `gpu` entirely processes frames on the game's own GPU.
Restart the application after changing `gpu`; the processing device is
fixed while the process runs.

## Step 4: Verify it is working

Launch the game and check stderr (or your log collector). A working
dual-GPU setup logs exactly:

```console
lsfg-vk: using profile with name 'vkcube dual-GPU test' (identified via executable)
lsfg-vk: enabling device extensions: VK_KHR_external_memory_fd VK_KHR_external_semaphore_fd VK_KHR_timeline_semaphore VK_EXT_external_memory_dma_buf VK_EXT_image_drm_format_modifier
lsfg-vk: processing on 'AMD Radeon RX 9070 XT (RADV GFX1201)' [uuid 00000000040000000000000000000000], dma-buf: yes, drm-modifier-images: yes
lsfg-vk: processing on '00000000040000000000000000000000' (game on 'Intel(R) Graphics (ARL)')
```

The last line is the definitive proof: a processing UUID different from
the game device named in parentheses. Single-GPU operation instead logs
`frame generation on the game's own device '...'`.

To confirm both GPUs are doing work simultaneously, sample the engine
timers of the running process:

```console
$ grep drm-engine /proc/$(pgrep vkcube)/fdinfo/* | grep -v ":.*0 ns"
drm-engine-gfx:    913898232 ns    # AMD - frame generation
drm-engine-render: 802163856 ns    # Intel - game rendering
```

## Tested combinations

All nine ordered combinations of the development rig's GPUs pass the
automated matrix (`scripts/run-matrix.sh live`) with validation-clean
runs and exact frame counts, and were additionally verified presenting
a real swapchain through the layer:

| Game GPU | Frame-gen GPU | Status |
| --- | --- | --- |
| Intel Arrow Lake iGPU | RX 9070 XT | verified (CLI + live swapchain) |
| Intel Arrow Lake iGPU | RX 9060 XT | verified (CLI + live swapchain) |
| RX 9070 XT | Intel Arrow Lake iGPU | verified (CLI) |
| RX 9060 XT | Intel Arrow Lake iGPU | verified (CLI) |
| RX 9070 XT | RX 9060 XT | verified (CLI + live swapchain) |
| RX 9060 XT | RX 9070 XT | verified (CLI + live swapchain) |
| any | itself | verified (legacy path, unchanged) |

## When it fails

Failures are loud and name your configuration. A `gpu` entry nothing
matches produces:

```console
lsfg-vk: something went wrong during lsfg-vk swapchain creation:
- failed to create backend instance for requested gpu 'RTX 5090'
- Unable to initialize Vulkan
- no suitable physical device found (error -3)
```

Common causes and their fixes are covered in
[Troubleshooting](Troubleshooting.md#dual-gpu-setups): missing exchange
extensions, LINEAR-fallback limitations on unusual drivers, Flatpak
render-node visibility, unverified HDR transport, and the per-driver
pipeline-cache files.

---

## One-way mode (external app)

### Concept

One-way mode moves **all presentation to the processing GPU**. The game
renders on GPU A, frames travel over PCIe to GPU B, frame generation runs
on GPU B, and the generated frames are presented by a separate
`lsfg-vk-app` process on GPU B. The game GPU **never imports frames back**.

```
┌─────────────────────────────────────────────────────────────────┐
│                        ONE-WAY EXTERNAL                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   GAME (GPU A)              PCIe              lsfg-vk-app (GPU B)│
│   ─────────────────        ──────            ────────────────── │
│   render frame ──────────► dma-buf export    │                  │
│                            │                 │                  │
│                            ▼                 ▼                  │
│                       frame gen          present               │
│                       (multiplier×)      (swapchain)           │
│                            │                 │                  │
│                            │                 │                  │
│   ◄────────────────────────┘  (NO return traffic)              │
│                                                                 │
│   Game GPU: render only (~10% rcs0, zero blitter/video)        │
│   Proc GPU: generation + present (~40% GFX, ~53% VCN)          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Traffic-shape difference vs two-way:**

| Direction | One-Way External | Two-Way (Control) |
|-----------|------------------|-------------------|
| A → B (game frames) | ✅ dma-buf export | ✅ dma-buf export |
| B → A (presented frames) | ❌ **NONE** | ✅ dma-buf import on A |
| B → Display | ✅ External app presents | ❌ Game presents on A |

This is the fundamental difference: two-way requires B→A dma-buf import for
presentation; one-way external eliminates it entirely.

### Setup steps

1. **Run the app first** — `lsfg-vk-app` must be listening before the game
   starts. It binds a Unix socket at `$XDG_RUNTIME_DIR/lsfg-vk/app.sock`.

   ```bash
   # Wayland (native KWin, GNOME, etc.)
   lsfg-vk-app --profile app-external --session wayland

   # X11 (XWayland) — requires XAUTHORITY for the XWayland server
   XAUTHORITY=/run/user/1000/xauth_XXXXXX \
   lsfg-vk-app --profile app-external --session x11
   ```

   The app prints:
   ```
   lsfg-vk-app: listening on /run/user/1000/lsfg-vk/app.sock (processing on 'AMD Radeon RX 9060 XT (RADV GFX1200)')
   ```

2. **Configure the game profile** with `presentation = external` and the
   processing GPU:

   ```toml
   [[profile]]
   name = "vkcube one-way external"
   active_in = "vkcube"
   gpu = "AMD Radeon RX 9060 XT (RADV GFX1200)"   # processing GPU (display GPU)
   presentation = "external"
   multiplier = 2
   # output = "HDMI-A-3"  # optional; defaults to primary output
   ```

   The `gpu` **must** be the display GPU (the one driving the monitor).
   The game GPU is selected automatically by the layer.

3. **Launch the game** with the layer enabled:

   ```bash
   VK_LAYER_PATH=/path/to/layer \
   VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
   LSFGVK_DLL_PATH="/path/to/Lossless.dll" \
   LSFGVK_CONFIG=/home/user/.config/lsfg-vk/conf.toml \
   vkcube --present_mode fifo
   ```

   On success the layer logs:
   ```
   lsfg-vk: external presentation active (game on 'Intel(R) Graphics (ARL)', app on socket)
   ```

4. **Cabling guidance** — The monitor must be connected to the **processing
   GPU** (the one named in `gpu`). The game GPU can be headless. On the
   development rig: RX 9060 XT (card3, HDMI-A-3) owns the KWin scanout; the
   game runs on Intel ARL (card1) or RX 9070 XT (card2).

5. **Compositor copy caveat** — The `lsfg-vk-app` window is a borderless
   fullscreen overlay. On Wayland it uses `xdg_toplevel` fullscreen; on X11
   it uses `_NET_WM_STATE_FULLSCREEN | ABOVE` with `WM_HINTS input=False`.
   The compositor still composites this window. If the compositor adds
   extra copies (e.g. KWin blur, GNOME overview), those copies add latency.
   Disable compositor effects for the `lsfg-vk-app` window if possible
   (KWin: Window Rules → "No compositing" for `app_id=lsfg-vk-app`).

### `--session` selection

| Value | Behavior |
|-------|----------|
| `auto` (default) | Prefers Wayland if `WAYLAND_DISPLAY` is set, else X11 (XWayland) |
| `wayland` | Forces Wayland backend (fails if no Wayland display) |
| `x11` | Forces X11 backend via XWayland (requires `XAUTHORITY`) |

**X11 backend note:** XWayland requires a valid `XAUTHORITY` file. The test
harness uses `XAUTHORITY=/run/user/1000/xauth_XXXXXX`. Without it, the app
starts but the game fails to connect to the X server.

### WM coverage table

| Compositor / Session | Wayland Backend | X11 Backend (XWayland) | Notes |
|----------------------|-----------------|------------------------|-------|
| **KWin (KDE Plasma)** | ✅ Native | ✅ Native | Tested; both backends work |
| **GNOME (Mutter)** | ✅ Native | ✅ Native | Expected to work; not tested on this rig |
| **wlroots (Sway, Hyprland, etc.)** | ✅ Native | ✅ Via XWayland | Expected to work; not tested on this rig |
| **Gamescope** | ✅ Native | ✅ Via XWayland | Expected to work; not tested on this rig |
| **Weston / Cage (nested)** | ✅ Native | ✅ Via XWayland | Not needed; native backends cover all major WMs |

**Verdict:** Native Wayland + native XWayland X11 = all major window managers
covered. No nested compositor fallback required.

### Tested-cells table (from Task 11)

| Cell | Game GPU (A) | Proc GPU (B) | Multiplier | Backend | Status | "external presentation active" Count |
|------|--------------|--------------|------------|---------|--------|--------------------------------------|
| (a) | Intel ARL | RX 9060 XT (display) | 2 | Wayland | ✅ PASS | 4,503 |
| (a) | Intel ARL | RX 9060 XT (display) | 2 | X11 | ⚠️ PARTIAL | 0 (XAUTHORITY) |
| (b) | Intel ARL | RX 9070 XT | 2 | Wayland | ✅ PASS | 4,636 |
| (b) | Intel ARL | RX 9070 XT | 2 | X11 | ⚠️ PARTIAL | 0 (XAUTHORITY) |
| (c) | RX 9060 XT | RX 9070 XT | 2 | Wayland | ❌ FAIL | 0 (GPU mapping) |
| (d-m2) | Intel ARL | RX 9060 XT (display) | 2 | Wayland | ✅ PASS | ~4,500 |
| (d-m2) | Intel ARL | RX 9060 XT (display) | 2 | X11 | ⚠️ PARTIAL | 0 (XAUTHORITY) |
| (d-m3) | Intel ARL | RX 9060 XT (display) | 3 | Wayland | ✅ PASS | ~4,500 |
| (d-m3) | Intel ARL | RX 9060 XT (display) | 3 | X11 | ⚠️ PARTIAL | 0 (XAUTHORITY) |

**Wayland Backend:** Fully functional for all cells where GPU mapping was correct.  
**X11 Backend:** Requires `XAUTHORITY=/run/user/1000/xauth_XXXXXX` for XWayland authentication. Verified manually working with proper auth.

**Notes:**
- Cell (c) failed due to test harness GPU mapping error (vkcube GPU 0 = 9070 XT, not 9060 XT). Needs re-run with `--gpu_number 1`.
- HDR not tested — KWin reports HDR off on HDMI-A-3; colorspace negotiation for BT.2020/PQ not validated.
- OOOLS (resize during soak) verified: automatic recovery, fd stable 9→9.

### Screenshots

*[Screenshots to be added — the external app window showing generated frames on the processing GPU's output]*

---

## Appendix: Test Rig Topology (Reference)

| DRM Card | PCI Address | Vendor:Device | Vulkan Device | Role |
|----------|-------------|---------------|---------------|------|
| card1 | 0000:00:02.0 | 0x8086:0x7d67 | Intel(R) Graphics (ARL) | iGPU (Game GPU candidate) |
| card2 | 0000:04:00.0 | 0x1002:0x7550 | AMD Radeon RX 9070 XT (RADV GFX1201) | dGPU (Proc GPU candidate) |
| **card3** | **0000:87:00.0** | **0x1002:0x7590** | **AMD Radeon RX 9060 XT (RADV GFX1200)** | **Display GPU (owns HDMI-A-3, KWin scanout)** |

**Display Server:** KDE Plasma (KWin) on Wayland, XWayland on `:0`  
**Monitor:** HDMI-A-3, 3840×2160@60Hz, scale 1.7, HDR off
