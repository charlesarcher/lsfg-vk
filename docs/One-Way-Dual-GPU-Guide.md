# Dual-GPU Frame Doubling — One-Way Mode (Quick Start)

This guide covers the new **one-way dual-GPU frame doubling** feature
(`presentation = "external"`): your game renders on the 9070 XT, the 9060 XT
doubles the frames **and** drives the display. No frames travel back over PCIe.

**Every command and log line in this guide was executed and verified on 2026-08-29**
on this machine (branch `feat/dual-gpu-oneway`). The "verified demo record" at the
end shows the exact runs and their results.

Card names used throughout:

| Your card | Vulkan `deviceName` | PCI id | Role |
|---|---|---|---|
| "9700XT" | `AMD Radeon RX 9070 XT (RADV GFX1201)` | `1002:7550` | **Render** (game GPU, headless) |
| "9600XT" | `AMD Radeon RX 9060 XT (RADV GFX1200)` | `1002:7590` | **Frame doubler + display** |

## How it works

```
Game (9070 XT, unchanged swapchain)
   └─ layer hooks vkQueuePresentKHR:
        blit presented frame → A-local staging image
        export via dma-buf ──► unix socket ──► lsfg-vk-app
        (game's own present is forwarded unchanged underneath)

lsfg-vk-app (own process, 9060 XT):
   import staging images once at handshake
   per frame: wait capture fd → run frame generation on 9060 XT
    → present ALL output frames from the 9060 XT's real swapchain:
         (multiplier-1) generated + 1 real   e.g. [generated, real] at multiplier 2
    → ack the staging slot back to the layer (backpressure)
 ```

Consequences:

- Cross-GPU PCIe traffic is exactly **one frame per cycle, one direction**.
- The display shows **`multiplier` frames for every 1 frame the game renders**
  (1 real + `multiplier - 1` generated; see "Frame-rate semantics" below).
- The `lsfg-vk-app` window is a **fullscreen** window on the 9060 XT's output; the
  captured game frame is blit-scaled to the full output resolution and presented
  from the 9060 XT's real swapchain.

## Cabling: which GPU does the display plug into?

**The display cable goes into the 9060 XT (the doubler card).** The 9070 XT is
headless in this topology. This is the defining difference from two-way mode
(`presentation = "game"`), where the display must be on the render GPU instead.

Verified topology on this machine (2026-08-29):

| Role | Card | PCI | Render node | DRM card | Connectors |
|---|---|---|---|---|---|
| Render (game) | RX 9070 XT | `04:00.0` | `/dev/dri/renderD129` | `card2` | none connected (headless) |
| Doubler + display | RX 9060 XT | `87:00.0` | `/dev/dri/renderD130` | `card3` | `DP-7` (2560×1440, **active monitor**), `HDMI-A-3` (4K, connected) |
| iGPU (unused) | Intel ARL | `00:02.0` | `/dev/dri/renderD128` | `card1` | all disconnected |

Re-check the topology any time:

```bash
# which Vulkan device names exist (copy one verbatim for conf.toml)
vulkaninfo --summary | grep deviceName

# which PCI device each render node belongs to
ls -la /dev/dri/by-path/

# which DRM connectors are connected and driving a display
for o in /sys/class/drm/card*-*; do
    [ -e "$o/status" ] && echo "$o: $(cat $o/status)"
done
```

## Requirements

- Both GPUs must expose `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier` (verified present on both cards here; the
  app's startup log prints `dma-buf: yes, drm-modifier-images: yes`).
- Both GPUs' render nodes (`/dev/dri/renderD*`) must be accessible to your user.
- Lossless Scaling installed (the `Lossless.dll` shader blob); on this machine:
  `/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll`.
- One `lsfg-vk-app` process per `XDG_RUNTIME_DIR`; it handles **one game stream
  at a time** (no multi-game isolation).
- X11 path: `XAUTHORITY` pointing at your X server's cookie file (required for
  XWayland). Find it with `ls /run/user/$(id -u)/ | grep xauth`.

## Step 1: Build / install the binaries

See [Building-From-Source](Building-From-Source.md) for dependencies. The
binaries needed for this feature: `lsfg-vk-layer` (the Vulkan layer),
`lsfg-vk-app` (the doubler/presenter), `lsfg-vk-cli` (validation).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### ⚠️ The layer manifest needs an absolute `library_path`

The generated manifest defaults to a **relative** `library_path`
(`liblsfg-vk-layer.so`). The Mesa Vulkan loader (tested on 1.4.357) does **not**
resolve it against the manifest directory; you get:

```
[Vulkan Loader] ERROR: liblsfg-vk-layer.so: cannot open shared object file: No such file or directory
[Vulkan Loader] INFO | LAYER: Requested layer "VK_LAYER_LSFGVK_frame_generation" failed to load.
```

**Fix (verified):** configure with an absolute path —

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DLSFGVK_LAYER_LIBRARY_PATH="$PWD/build/lsfg-vk-layer/liblsfg-vk-layer.so"
```

or keep the verified test rig in `/tmp/opencode/layer-test/` (manifest with
absolute `library_path` + symlink `liblsfg-vk-layer.so -> build/`, so it always
tracks fresh builds) and use `VK_LAYER_PATH=/tmp/opencode/layer-test`.

## Step 2: Configure

Config file location (first hit wins, **no fallback**):

1. `$LSFGVK_CONFIG` (explicit env var — recommended for testing)
2. `$XDG_CONFIG_HOME/lsfg-vk/conf.toml`
3. `$HOME/.config/lsfg-vk/conf.toml`
4. `/etc/lsfg-vk/conf.toml`

> **Gotcha (verified):** if `XDG_CONFIG_HOME` is set, it wins outright and
> `$HOME/.config` is *not* checked. Some development tools set it in their shell
> environments; in that case either put the file at
> `$XDG_CONFIG_HOME/lsfg-vk/conf.toml` or always launch with
> `LSFGVK_CONFIG=/home/$USER/.config/lsfg-vk/conf.toml`.

The full config for this topology (verified with `lsfg-vk-cli validate`):

```toml
version = 2

[global]
dll = "/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
allow_fp16 = true

# ── Game profile ──────────────────────────────────────────────────────────
# active_in = the game's executable name (this profile auto-applies to it).
# gpu       = the PROCESSING GPU — must be the display GPU (9060 XT).
#             Exact vulkaninfo deviceName, or uppercase 0xVVVV:0xDDDD,
#             or a PCI bus id. The game's own GPU is selected automatically.
[[profile]]
name = "vkcube-oneway"
active_in = "vkcube"
gpu = "AMD Radeon RX 9060 XT (RADV GFX1200)"
presentation = "external"
multiplier = 2

# ── App profile ───────────────────────────────────────────────────────────
# The lsfg-vk-app selects its processing GPU from this profile.
# Must name the SAME GPU as the game profile.
[[profile]]
name = "app-oneway"
gpu = "AMD Radeon RX 9060 XT (RADV GFX1200)"
presentation = "external"
multiplier = 2
```

Validation rules (hard errors, verified):

- `presentation = "external"` **requires** `gpu` →
  `external presentation requires 'gpu' to select the processing device`
- unknown `presentation` value → named parse error listing allowed values
- `output` set with `presentation = "game"` → named warning
- `multiplier` must be ≥ 2

```bash
./build/lsfg-vk-cli/lsfg-vk-cli validate -c /home/$USER/.config/lsfg-vk/conf.toml
# → vkcube-oneway: presentation=external, output=-
#   app-oneway:    presentation=external, output=-
#   Validation success
```

The `gpu` value can also be the PCI id (verified — the app resolved it to the
9060 XT): `gpu = "0x1002:0x7590"`.

## Step 3: What you type to frame-double a game

**Order matters: the app first, the game second.** The layer connects to the
app's socket at swapchain creation; if the app isn't listening, swapchain
creation fails loudly (see Troubleshooting).

### 3.1 Start the doubler app (on the 9060 XT)

```bash
# X11 backend — verified working on this rig (see "Known issues" for Wayland)
XAUTHORITY=/run/user/1000/xauth_OKuvCG \
    lsfg-vk-app --profile app-oneway --session x11

# native Wayland backend
# lsfg-vk-app --profile app-oneway --session wayland
```

`--session auto` (default) prefers Wayland when `WAYLAND_DISPLAY` is set, else
X11. `--output <name>` optionally pins the display connector (e.g. `DP-7`);
default = primary output. Add `-v` for the per-cycle ordering log
(`[gen x N + real]`) and the per-second `N fps game, M fps presented` stats.

Expected (verified verbatim):

```
lsfg-vk: processing on 'AMD Radeon RX 9060 XT (RADV GFX1200)' [uuid 00000000870000000000000000000000], dma-buf: yes, drm-modifier-images: yes
lsfg-vk-app: listening on /run/user/1000/lsfg-vk/app.sock (processing on 'AMD Radeon RX 9060 XT (RADV GFX1200)')
```

Confirm the socket: `ss -lxp | grep app.sock`

### 3.2 Launch the game on the 9070 XT with the layer

The game must render on the 9070 XT (the headless card). Two verified ways:

- vkcube: `--gpu_number 1` (its index in `vulkaninfo --summary`)
- any Vulkan game: Mesa's device-select layer via PCI id
  (`MESA_VK_DEVICE_SELECT=1002:7550`; verified to pin vkcube to the 9070 XT
  without `--gpu_number`. Debug with `MESA_VK_DEVICE_SELECT_DEBUG=1`.)

```bash
VK_LAYER_PATH=/tmp/opencode/layer-test \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_CONFIG=/home/archerc/.config/lsfg-vk/conf.toml \
MESA_VK_DEVICE_SELECT=1002:7550 \
vkcube --present_mode fifo --wsi xcb
```

For a real game, replace the `vkcube ...` invocation with your game's launch
command (the `active_in` profile entry must match its executable name, e.g.
`active_in = "MyGame.exe"`).

### 3.3 Confirm it's working

Game process log (verified verbatim):

```
lsfg-vk: using profile with name 'vkcube-oneway' (identified via executable)
Selected GPU 0: AMD Radeon RX 9070 XT (RADV GFX1201), ...
lsfg-vk: external presentation active (game on 'AMD Radeon RX 9070 XT (RADV GFX1201)', app on socket)
```

App process log with `-v` (verified):

```
lsfg-vk-app: stream from 'AMD Radeon RX 9070 XT (RADV GFX1201)' 500x500 VkFormat(44)
lsfg-vk-app: context created on 'dma-buf' cross-device=1
lsfg-vk-app: using X11 surface backend
[gen x 1 + real]      # repeated once per captured game frame (N = multiplier-1)
lsfg-vk-app: 57 fps game, 114 fps presented   # per-second stats (verbose)
```

`VkFormat(44)` is `VK_FORMAT_B8G8R8A8_UNORM` — the exchange format negotiated at
handshake (it matches the X visual's native pixel layout, so the presented
swapchain uses the same format; a mismatched format would present with red and
blue swapped — see "How to prove the frames were doubled", point 4).

On the monitor (9060 XT → `DP-7`) you see the `lsfg-vk-app` **fullscreen**
window showing the frame-doubled output. The game keeps rendering on the 9070 XT
headless; under X11 its own window still presents underneath, but the app's
fullscreen window covers it.

### Frame-rate semantics (read this before judging the result)

- **One game frame → `multiplier` presented frames** (1 real + `multiplier - 1`
  generated), honoring the profile's `multiplier` key. With `multiplier = 2` a
  57 fps game drives ~114 fps into the display — verified: `57 fps game,
  114 fps presented` and an aggregate presented/game ratio of **2.009** across
  36 per-second samples (see the proof record below).
- The display is of course capped at its refresh rate; on a 60 Hz monitor most
  generated frames are vblank-discarded. To actually *see* the doubling you
  want a high-refresh (120/144/240 Hz) monitor — this rig's `DP-7` runs
  2560×1440 @ 239.97 Hz, so the doubled output is genuinely visible.
- In **two-way** mode `multiplier` behaves the same (2 = true doubling:
  1 generated + 1 real per game frame).

## Two-way mode (the alternative)

`presentation = "game"` (or the key omitted) keeps presentation on the **game's
own GPU**: frames are dma-buf-exported to the processing GPU, generated there,
and blitted back into the game's swapchain. The `lsfg-vk-app` is not involved.

**Cabling difference: the display must be plugged into the render/game GPU.**
On this rig that means monitor → 9070 XT, which is not how the machine is
cabled, so the only testable two-way cell here is the reverse pairing
(game on the 9060 XT, frame generation on the 9070 XT). Verified:

```toml
[[profile]]
name = "vkcube-twoway"
active_in = "vkcube"
gpu = "AMD Radeon RX 9070 XT (RADV GFX1201)"   # processing GPU (headless here)
multiplier = 2                                  # honored in two-way mode
```

```bash
VK_LAYER_PATH=/tmp/opencode/layer-test \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_CONFIG=/tmp/conf-twoway.toml \
vkcube --gpu_number 0 --present_mode fifo --wsi xcb
```

Verified log lines:

```
lsfg-vk: processing on 'AMD Radeon RX 9070 XT (RADV GFX1201)' [uuid 00000000040000000000000000000000], dma-buf: yes, drm-modifier-images: yes
lsfg-vk: processing on '00000000040000000000000000000000' (game on 'AMD Radeon RX 9060 XT (RADV GFX1200)')
```

Two-way costs a return PCIe trip per frame (see the bandwidth table in
[Configuration](Configuration.md)); one-way eliminates it entirely — which is
why one-way is the mode for the 9700XT-render / 9600XT-display topology.

## Verified demo record (2026-08-29, this machine)

All runs: branch `feat/dual-gpu-oneway` (build from the branch tip — the
`fix(app)` commit adds the clean SIGINT shutdown and the live-stream
`N fps game, M fps presented` stats these numbers come from), `vkcube`
500×500 as the game, config from Step 2, `multiplier = 2`. Evidence is captured
under
`.omo/evidence/oneway/demo-2026-08-29/` (logs, stills, and the reproducible
`analyze.py` → `analysis.md` proof record).

| # | Run | Result |
|---|---|---|
| 1 | `lsfg-vk-cli validate -c conf.toml` | `Validation success` |
| 2 | **Old** `lsfg-vk-app` (`--session x11`) + vkcube on 9070 XT | two ~300×300 animating cubes visible at once (the game window **and** the old 500×500 overlay) = the reported **ghosting**; green-leaning color = the reported **artifacting**. Stills: `runA-old/shot-old-artifact*.png` |
| 3 | **New** `lsfg-vk-app --session x11 -v` + vkcube on 9070 XT, 45 s | one fullscreen animating image; `stream from '... 9070 XT ...' 500x500 VkFormat(44)` (B8G8R8A8); **2,144 `[gen x 1 + real]` cycles**; per-second stats **57 fps game / 114 fps presented** (ratio 2.009); clean SIGINT exit in 54 ms. Stills: `runB-new/shot-new-*.png`, burst `burst-new-*.png` |
| 4 | Standalone `vkcube` (no layer, same GPU) | ground-truth cube color for the fidelity check (`ref-vkcube-standalone/ref-*.png`) |
| 5 | Mid-run engine capture | doubler card (9060 XT) `gpu_busy_percent` 4 % idle → 13–16 % during the run (`runB-new/engine-midrun.txt`) |
| 6 | Failure: one-way with app **not** running | game: `failed to connect to app socket '/run/user/1000/lsfg-vk/app.sock'` / `connect() ... failed: Connection refused` — loud, named failure at swapchain creation |
| 7 | Failure: app started with `--output BOGUS-OUT` | app: `stream ended: output 'BOGUS-OUT' not found; available: DP-7` |
| 8 | One-way with `--session wayland` app backend | **stalls after ~1 cycle** (reproduced 2026-08-29 with the current build) — see Known issues |

## How to prove the frames were doubled

The user's report was *ghosting and artifacting*, and the question was *how do
you prove the frames were actually doubled by the lossless-scaling algorithm,
rather than just re-presented*. Five independent measurements, all captured on
2026-08-29 and reproducible from the committed evidence (machine-readable copy:
`.omo/evidence/oneway/demo-2026-08-29/analysis.md`):

1. **Cadence ratio (the primary proof).** With `-v`, the app prints the
   received *game* frame rate and the *presented* frame rate once per second.
   Presented = generated + real, and real = the measured game rate, so the
   ratio is the observable multiplier. Measured: **57.14 fps game, 114.81 fps
   presented → ratio 2.0092** across 36 per-second samples; 2,144 `[gen x 1 +
   real]` cycles; presented − game = 57.7 fps of generated frames. A mere
   re-presenter would show a ratio of 1 — the 2.0× ratio is the doubling.
2. **Engine activity on the doubler card.** `gpu_busy_percent` on the 9060 XT
   (display card) rose from 4 % idle to 13–16 % *during* the run, matching the
   9070 XT game card. Frame generation was running on the doubler, not just
   copying frames across PCIe.
3. **Visual: one image (new) vs two cubes (old).** A two-frame pixel-diff of
   the old run shows **two distinct ~300×300 animating regions** (the game
   window and the old 500×500 overlay) — that is the **ghosting**. The new
   run's burst shows a single rotating cube filling the output (motion bbox ≈
   66 % × 67 % of the 2560×1440 frame, whole-frame bright fraction 1.0): the
   overlay was replaced by a fullscreen scaled blit, so only one image is seen.
4. **Color fidelity (no channel swap).** Isolating the moving cube pixels and
   comparing against a standalone `vkcube` capture (no layer): standalone
   R/G/B (60, 77, 79), new code (67, 80, 82) — a close match, blue-dominant in
   both. The old code read (70, 77, 69), green-leaning with R−B ≈ 0 — the
   R8G8B8A8-vs-B8G8R8A8 format mismatch, i.e. the **artifacting**. The new
   native B8G8R8A8 swapchain fixes it.
5. **Clean shutdown.** SIGINT exits the app in 54 ms with the
   `lsfg-vk-app: shutting down` banner (the old binary hung on SIGINT).

## Troubleshooting

Every entry below was reproduced on this machine.

**Game fails to start with a socket error** (app not running):

```
lsfg-vk: something went wrong during lsfg-vk swapchain creation:
- lsfg-vk: failed to connect to app socket '/run/user/1000/lsfg-vk/app.sock'
- connect() to '/run/user/1000/lsfg-vk/app.sock' failed: Connection refused
```
→ Start `lsfg-vk-app` **before** the game. Both processes must share
`XDG_RUNTIME_DIR` (the socket path is fixed and unconfigurable).

**Layer silently not applied / "failed to load"**:
`VK_LAYER_PATH` points at a manifest with a relative `library_path`
(Step 1 fix), or the layer name in `VK_INSTANCE_LAYERS` is misspelled. Check
with `VK_LAYER_PATH=... vulkaninfo | grep -A3 "Layers:"` — the layer must be
listed.

**"unable to parse configuration / File could not be opened"**:
`XDG_CONFIG_HOME` is set (Step 2 gotcha). Use `LSFGVK_CONFIG` or move the file.

**App reports an unknown output**:
`lsfg-vk-app: stream ended: output 'X' not found; available: DP-7` — pass one
of the listed names to `--output` (or omit `--output` for the primary).

**Game KILLed while streaming (app still up)**: the layer logs
`external stream error: poll() on ipc socket failed: Connection reset by peer`
and the game's swapchain presentation fails; start a fresh app + game.
Verified that app SIGKILL mid-stream surfaces the same named error.

**Changing `gpu` / `presentation`**: requires restarting the game (and the app
for its profile); the layer logs `gpu change requires restart to take effect`.

**Compositor effects**: the app's presentation window is borderless fullscreen
(X11: `_NET_WM_STATE_FULLSCREEN | ABOVE`, input disabled). Disable compositor
effects for it if you see smearing (KWin: Window Rules → "No compositing" for
the app).

**Flatpak**: both GPUs' `/dev/dri/renderD*` nodes must be granted to the
sandbox (`--device=/dev/dri` plus the specific nodes; see the Flatpak Guide).

## Known issues

1. **Wayland app backend stalls after ~1 cycle (reproduced 2026-08-29 with the
   current build).** With `--session wayland`, the app presents its buffer
   burst and then blocks in `AcquireNextImageKHR(UINT64_MAX)` — the compositor
   (KWin) releases only the displayed buffer(s), and with no further frame
   callbacks the app waits forever. While blocked in that call the SIGINT
   handler cannot flip the stop flag the loop checks (the loop is not at its
   poll), so `kill -INT` does not bring it down either; `kill -TERM`/`-9` are
   required. *Workaround: run the app with `--session x11` (verified stable for
   45+ s soaks — see the cadence proof above). The game's own WSI is
   unaffected.*
   Likely fixes (not yet implemented): finite-timeout
   `AcquireNextImageKHR` + event-pumping retry loop, and/or pacing commits so
   at most one buffer is committed ahead of the last frame callback.
2. **One stream at a time**: a second game connecting to the same app is not
   isolated; it fails. One app instance per `XDG_RUNTIME_DIR`.
3. **No HDR verification** (the exchange is always 8-bit UNORM — B8G8R8A8 — in
   one-way mode; HDR staging is not implemented).
4. **Unverified**: NVIDIA second cards, other WMs/compositors beyond KWin,
   exclusive fullscreen (frame generation ineffective, game unharmed).

## Related docs

- [Dual-GPU Setup Guide](Dual-GPU-Guide.md) — two-way mode concepts, bandwidth model
- [Configuration](Configuration.md) — all config keys, env vars
- [Troubleshooting](Troubleshooting.md) — extended failure-mode reference
- [Building From Source](Building-From-Source.md)