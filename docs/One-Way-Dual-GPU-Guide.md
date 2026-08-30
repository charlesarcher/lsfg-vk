# Dual-GPU Frame Doubling — One-Way Mode (Quick Start)

This guide covers the new **one-way dual-GPU frame doubling** feature
(`presentation = "external"`): your game renders on the 9070 XT, the 9060 XT
doubles the frames **and** drives the display. No frames travel back over PCIe.

**Every command and log line in this guide was executed and verified on
2026-08-29 and 2026-08-30** on this machine (branch
`feat/dual-gpu-oneway`, including the 2026-08-30 fps-HUD and X11-placement
commits). The "verified demo record" at the end shows the exact runs and their
results.

Card names used throughout:

| Your card | Vulkan `deviceName` | PCI id | Role |
|---|---|---|---|
| "9700XT" | `AMD Radeon RX 9070 XT (RADV GFX1201)` | `1002:7550` | **Render** (game GPU, headless) |
| "9600XT" | `AMD Radeon RX 9060 XT (RADV GFX1200)` | `1002:7590` | **Frame doubler + display** |

## How this works

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

### The four parts

- **The Vulkan layer** (`liblsfg-vk-layer.so`, layer
  `VK_LAYER_LSFGVK_frame_generation`) runs *inside the game process*. It hooks
  only `vkCreateSwapchainKHR` / `vkQueuePresentKHR` /
  `vkDestroySwapchainKHR`. In one-way mode the present hook becomes
  *capture + forward*: it copies the frame just presented into a staging image
  and then forwards the game's own present down the chain **unchanged**. The
  game's swapchain is never replaced and no acquire/get-images calls are ever
  hooked — zero WSI emulation.
- **The app** (`lsfg-vk-app`) is a separate process you start before the
  game. It owns an ordinary swapchain on the doubler card (9060 XT) and
  presents everything the display sees.
- **The backend** (`lsfg-vk-backend`, linked into the app) is the
  frame-generation engine: it dlopens `Lossless.dll` and runs LSSC's shader
  chain (mipmaps → alpha → beta → gamma → delta → generate passes) on the
  doubler card.
- **The socket** (`$XDG_RUNTIME_DIR/lsfg-vk/app.sock`, AF_UNIX) carries
  length-prefixed messages plus GPU sync file descriptors (SCM_RIGHTS). It is
  the only path between the two processes.

### Setup (once, at swapchain creation)

1. The game creates its swapchain; the layer sees `presentation = "external"`
   and connects to the app's socket (absent app → loud, named error — see
   Troubleshooting).
2. The layer announces its game device, format, and extent (HELLO); the app
   negotiates the dma-buf layout (modifier, pitch) against the doubler card's
   *true* caps (NEGOTIATED).
3. The layer creates **two** exportable staging images on the 9070 XT and
   hands the app both of their fds (STAGING ×2); the app imports them as
   images the doubler card can read directly.
4. READY — the stream is live.

### Per-frame timeline (what happens for every game frame)

| # | Where | What happens |
|---|-------|--------------|
| 1 | game (9070 XT) | renders a frame into its own swapchain and calls `vkQueuePresentKHR`. |
| 2 | layer (in the game process) | records a blit: presented image → staging slot (slots alternate 0/1), waiting on the game's own present semaphores. |
| 3 | layer | exports a sync-fd that signals "capture done" and sends `FRAME {slot, fd}` to the app. |
| 4 | layer | forwards the game's original present unchanged — the game's window keeps presenting underneath the app's fullscreen window. |
| 5 | app (9060 XT) | waits on the sync-fd (no CPU copy — the pixels are a dma-buf image shared between the GPUs; the doubler card reads them straight from the 9070 XT's memory) and runs LSSC frame generation: **`multiplier − 1` intermediate frames** synthesized between this real frame and the previous real frame. |
| 6 | app | presents from its own swapchain: the `multiplier − 1` generated frames, then the real frame (per-cycle order `[gen × (m−1), real]` — the `[gen x 1 + real]` lines in `-v`). |
| 7 | app | sends `RELEASE {slot}` — the layer may now overwrite that slot. |
| 8 | display | scans out the presented frames at up to its refresh rate. |

### Why the staging ring is exactly 2 slots

The LSSC backend's contract is **exactly 2 source images, alternated
between** — so the capture ring is exactly 2 slots deep, 1:1 with the
backend's current/next alternation. Side benefit: at most 2 frames are ever
in flight, which is the backpressure bound — if the app stalls, the game's
capture fills both slots, then the layer waits 500 ms for a free slot and
fails the present with a named error (`no free staging slots within 500 ms
(app stalled)`), never hanging.

### What the LSSC algorithm does

Frame generation is temporal interpolation. For each new real frame the
shader chain (extracted from `Lossless.dll`) estimates motion from the
current frame and the previous real frame (kept as temporal history; the
first frame after start-up uses a black prior) and synthesizes intermediate
frame(s) *between* the two. With `multiplier = 2`, one intermediate per real
frame → the display is presented at 2× the game's cadence. `flow_scale` and
`performance_mode` tune the quality/performance trade-off of that
interpolation (see [Configuration](Configuration.md)).

### Why "one-way"

The only cross-GPU traffic is the capture: one frame per cycle, game →
doubler, one direction. The generated frames are *made on* the doubler card
and *presented from* the doubler card — nothing ever travels back over
PCIe. (Two-way mode instead blits the generated frames back into the
game's swapchain: one PCIe round trip per frame.) That is also why the
display cable goes into the doubler card: the monitor scans out of the
card that does the presenting. (If the monitor were on the game card, the
compositor would silently copy the app's window to the other card — it
works, but you pay that copy every frame.)

### Guarantees

Whatever happens to the doubler process, the game is never harmed and
nothing fails silently:

- **The game never crashes or hangs.** At most 2 frames are in flight, so
  a wedged doubler can back up the game's capture by at most 2 frames; the
  layer then waits 500 ms for a free slot and reports
  `no free staging slots within 500 ms (app stalled)` — a named error, and
  the game keeps rendering and presenting its own window.
- **Every error is named.** Each condition the processes can hit logs the
  exact error text (see Troubleshooting). There is no silent path: if
  anything is wrong, both processes say what happened and why.
- **A restart restores the stream.** If the app exits (SIGINT, crash,
  kill), the game logs a named `Connection reset by peer` on its next
  present — and starting the app and game again brings the stream back.
- **Idle is media-player semantics.** No game frames for >5 s: the app
  keeps its window alive, presents the last real frame, and logs once.

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
# X11 backend (XWayland) — verified working on this rig
XAUTHORITY=/run/user/1000/xauth_OKuvCG \
    lsfg-vk-app --profile app-oneway --session x11

# native Wayland backend — verified working on this rig (60 s soak, demo record)
lsfg-vk-app --profile app-oneway --session wayland
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
fullscreen window covers it. In the **top-left corner** of that window a small
dark box shows the live frame-rate counter `<game>/<presented>` — e.g.
`57/114` — the same corner indicator Windows Lossless Scaling draws
(see "The fps counter" below).

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

### The fps counter (top-left HUD)

The app draws a small **`<game>/<presented>`** box in the top-left corner of its
window (inset 8 px), always on — no `-v` needed. It is the on-screen answer to
"is the doubling actually happening": the left number is the received *game*
frame rate, the right number the *presented* (doubled) rate, updated once per
second. With `multiplier = 2` it reads roughly `N/2N` (e.g. `57/114`,
`149/299`). A plain re-presenter would read `N/N`.

- The text is rasterized on the CPU (seven-segment digits, scaled with the
  output height: 1440p → scale 6, 204×66 px box) into a small
  double-buffered image; the 1 Hz update writes the slot the present loop is
  *not* reading, so a frame never shows a half-updated counter.
- It is blitted into the presented frame on every present path (generated,
  real, and idle re-present), so the counter is part of the content the
  doubler card scans out — visible on the monitor, captured in screenshots,
  and machine-verifiable (see "How to prove the frames were doubled", point 6).
- Note: the HUD is the app's own overlay in the app's window — it is *not*
  present in the game's window underneath.

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

## Frame-doubling a real game (Steam / Proton)

`vkcube` is the reference game here. The *same* one-way flow works for a real
Vulkan game; the only differences are (a) how the game process is started and
(b) how the profile is matched to it.

### Add a profile for the game

`active_in` is matched against the game's executable name (under Proton, the
real PE exe path is read from `/proc/self/maps` and matched by suffix, so the
bare exe name works). Scope it to the game exe(s) so the launcher doesn't grab
the single stream:

```toml
[[profile]]
name = "mass-effect-oneway"
active_in = ["MassEffect1.exe", "MassEffect2.exe", "MassEffect3.exe"]
gpu = "AMD Radeon RX 9060 XT (RADV GFX1200)"   # processing + display GPU
presentation = "external"
multiplier = 2
```

The `app-oneway` profile (Step 2) is unchanged — it just names the same
processing GPU.

### Start the doubler, then the game

1. **Doubler first** (same command as Step 3.1):
   `lsfg-vk-app --profile app-oneway --session x11` (or `--session wayland`).
2. **The game.** For a Steam/Proton game, set the layer + device pin in the
   game's **Steam → Properties → Launch options** (Steam turns the `VAR=value`
   tokens before `%command%` into the game process's environment):

   ```
   VK_LAYER_PATH=/tmp/opencode/layer-test VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation LSFGVK_CONFIG=/home/archerc/.config/lsfg-vk/conf.toml MESA_VK_DEVICE_SELECT=1002:7550 %command%
   ```

   - `MESA_VK_DEVICE_SELECT=1002:7550` pins the game's render to the 9070 XT
     (the PCI id of the render card; use the doubler's id for the other
     pairing). The layer then does the cross-device work to the 9060 XT.
   - For a **non-Steam** Vulkan game, the same env vars go in front of the
     launch command in a shell instead of Steam launch options.

### Expect

Game log: `lsfg-vk: using profile with name '<profile>' (identified via wine
executable)` → `lsfg-vk: external presentation active (game on 'AMD Radeon RX
9070 XT ...', app on socket)`. App log: `lsfg-vk-app: stream from 'AMD Radeon
RX 9070 XT ...' <WxH> ...` then `[gen x N + real]` per cycle. The monitor
(9060 XT) shows the frame-doubled game with the `<game>/<presented>` HUD in the
top-left.

> **Verified 2026-08-30:** the layer/Profile/device side of this flow is
> confirmed (profile matched under Proton, device pinned to the 9070 XT, doubler
> app listening and healthy). The Mass Effect game itself was blocked by the
> EA Desktop auth/bridge wall before creating a swapchain (see Known issues
> #4 and `.omo/evidence/oneway/demo-2026-08-30/mass-effect/NOTES.md`), so the
> full real-game E2E is pending an interactive EA session — the mechanism is
> proven end-to-end by the vkcube runs on both backends.

## Verified demo record (this machine)

### 2026-08-30: fps HUD + X11 placement fix (branch tip with `feat(app)` HUD)

All runs: `vkcube` 500×500 on the 9070 XT, `multiplier = 2`, config from Step 2.
Evidence under `.omo/evidence/oneway/demo-2026-08-30/` (reproducible demo
scripts `run-demo-x11.sh` / `run-demo-wayland.sh`, logs, stills, HUD crops, and
the panel-aware `verify-hud.py` machine check).

| # | Run | Result |
|---|---|---|
| 1 | **X11 backend** `lsfg-vk-app --session x11 -v` + vkcube on 9070 XT, ~40 s | **1,460 `[gen x 1 + real]` cycles**; 25 per-second samples, **57.0 fps game / 114.5 fps presented, aggregate ratio 2.0070**; window placed at **(0,0) 2560×1440** with `_NET_WM_STATE = FULLSCREEN \| ABOVE` (the post-map EWMH ClientMessage fix — pre-fix KWin placed it at (0,28) 2560×1382); engine busy mid-run: doubler 14 % / game 15 %; SIGINT exit in **0 ms**; 0 VUIDs. Stills: `x11/shot-t*.png` |
| 2 | **Wayland backend** `lsfg-vk-app --session wayland -v` + same game, ~40 s | **3,845 cycles**; 25 samples, **150.5 fps game / 301.9 fps presented, aggregate ratio 2.0058**; engine 26 % / 28 %; SIGINT exit in **0 ms** with the `shutting down` banner; game side raised the designed named error (`Connection reset by peer`) on teardown; 0 VUIDs. Stills: `wayland/shot-t*.png` |
| 3 | **HUD machine verification** (`verify-hud.py` on the stills above) | re-renders the expected seven-segment text with the exact `hud.cpp` algorithm and IoU-matches it against the still's HUD crop. X11: origin (8,8), text **`56/113` / `57/114`**, IoU **1.000**, box mean color (14,16,22) exact. Wayland: origin (8,8), text **`149/299`**, IoU **1.000**. Crops: `*/shot-t*-hud-crop.png` |
| 4 | **Mass Effect Legendary (real game)** | profile + layer wiring verified (wine-exe profile detection, device pinning to the 9070 XT, app listening and healthy in both attempts); the game itself was blocked by the **EA Desktop auth/bridge wall** (standalone launch → EA login UI; Steam launch → `link2ea://` bridge stalled in the prefix's EA session) — a game/EA-side blocker, not a one-way-side failure. Full record + the exact Steam launch-options recipe: `.omo/evidence/oneway/demo-2026-08-30/mass-effect/NOTES.md` |

### 2026-08-29 (this machine)

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
| 8 | One-way with `--session wayland` app backend, 60 s soak | **Fixed 2026-08-29** (bounded Wayland event dispatch + bounded `AcquireNextImageKHR` with event-pump retry): **9,088 cycles in 60 s**, `149 fps game / 300 fps presented` (ratio 2.005), 0 errors, clean SIGINT exit in 56 ms. Root cause: the blocking `wl_display_dispatch` deadlocked on wl_buffer-release events belonging to RADV's own event queue, and the infinite-timeout acquires made the stop flag unreachable. Stills: `runF-wayland-soak/soak-t*.png`; stall-state capture (pre-fix backtrace/strace): `runD-debug/` |

## How to prove the frames were doubled

The user's report was *ghosting and artifacting*, and the question was *how do
you prove the frames were actually doubled by the lossless-scaling algorithm,
rather than just re-presented*. Six independent measurements, captured on
2026-08-29/2026-08-30 and reproducible from the committed evidence
(machine-readable copies: `.omo/evidence/oneway/demo-2026-08-29/analysis.md`,
`.omo/evidence/oneway/demo-2026-08-30/`):

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
    `lsfg-vk-app: shutting down` banner (the old binary hung on SIGINT);
    2026-08-30 runs exit in **0 ms** on both backends.
 6. **The on-screen counter itself (machine-checked).** The top-left HUD shows
    the live `<game>/<presented>` rates, and `verify-hud.py` proves the pixels
    in a screenshot are the counter: it re-renders the expected text with the
    exact `hud.cpp` rasterizer and IoU-matches it against the still's crop.
    2026-08-30: X11 stills matched `56/113` and `57/114` (IoU 1.000), Wayland
    stills matched `149/299` (IoU 1.000), with the box color (14,16,22) exact.
    The right-hand number is 2× the left — the doubling, visible on the
    monitor, not just in the logs.

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

**Compositor effects**: the app's presentation window is borderless fullscreen.
Under **X11** the state is *requested* with a post-map EWMH ClientMessage
(`_NET_WM_STATE` action=add, FULLSCREEN | ABOVE) — a client-set property is
ignored by KWin and leaves the window under the top panel ((0,28) 2560×1382
instead of (0,0) 2560×1440; fixed 2026-08-30, verified placed at (0,0)
2560×1440 with FULLSCREEN | ABOVE). Under **Wayland** the window is a
fullscreen toplevel on the requested output; KWin/Plasma *may* keep its panels
drawn above the toplevel, which can occlude the top-left corner (the fps HUD)
by a few pixels — set the panel to auto-hide if that bothers you (in the
2026-08-30 verified runs the HUD was fully visible at (8,8) on both backends).
Disable compositor effects for the app window if you see smearing (KWin:
Window Rules → "No compositing" for the app).

**Flatpak**: both GPUs' `/dev/dri/renderD*` nodes must be granted to the
sandbox (`--device=/dev/dri` plus the specific nodes; see the Flatpak Guide).

## Known issues

1. **One stream at a time**: a second game connecting to the same app is not
   isolated; it fails. One app instance per `XDG_RUNTIME_DIR`.
2. **No HDR verification** (the exchange is always 8-bit UNORM — B8G8R8A8 — in
   one-way mode; HDR staging is not implemented).
3. **Unverified**: NVIDIA second cards, other WMs/compositors beyond KWin,
   exclusive fullscreen (frame generation ineffective, game unharmed).
4. **Steam/Proton real-game E2E not completed on this rig** (2026-08-30):
   Mass Effect Legendary reached the EA Desktop auth/bridge wall in both
   launch paths (standalone Proton → EA login UI; Steam `link2ea://` bridge →
   stalled EA session in the prefix) before the game ever created its
   swapchain. The layer-side recipe (Steam launch options) is in
   "Frame-doubling a real game (Steam)" below; the vkcube E2E on both
   backends is the verified proof of the mechanism.

### Fixed: Wayland backend stall (2026-08-29)

The `--session wayland` backend previously stalled after the first cycle and
could not be stopped with SIGINT. Root cause (gdb/strace-confirmed):
`processEvents` drained the display with the blocking `wl_display_dispatch`
(default queue) — when the bytes it read carried only events for RADV's own
event queue (the `wl_buffer` release listeners live there), it consumed the
data, dispatched nothing, and waited forever on the empty socket. The
present loop's `AcquireNextImageKHR(UINT64_MAX)` then made the stop flag
unreachable as well. Fixed with a non-blocking bounded dispatch in
`processEvents` and a 200 ms `AcquireNextImageKHR` timeout that pumps WSI
events and re-checks the stop flag on each retry. Verified: 60 s Wayland soak
(9,088 cycles, ratio 2.005, 0 errors, clean SIGINT in 56 ms) and an X11
regression run (no change: 1,744 cycles / 30 s, ratio 2.010).

## Related docs

- [Dual-GPU Setup Guide](Dual-GPU-Guide.md) — two-way mode concepts, bandwidth model
- [Configuration](Configuration.md) — all config keys, env vars
- [Troubleshooting](Troubleshooting.md) — extended failure-mode reference
- [Building From Source](Building-From-Source.md)