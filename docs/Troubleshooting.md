# Troubleshooting
This page documents common issues, known incompatibilities and contains a guide to help you create a helpful bug report.

Before reporting a bug, please read through the following sections to see if your issue is already addressed.

### Basic Troubleshooting Steps
If lsfg-vk does not seem to be doing *anything*:
- Ensure the game you are trying to run is using Vulkan (not OpenGL).
- Ensure you are running a 64-bit game (try `PROTON_USE_WOW64=1`, but if it doesn't work then you're out of luck).
- Install `vulkan-tools` and run `vulkaninfo | grep -i VK_LAYER_LSFGVK_frame_generation`.
  - If there is no output revisit the installation steps.
- Launch the game with the environment variable `VK_LOADER_DEBUG=layer` set.
  - Look for lines mentioning `VK_LAYER_LSFGVK_frame_generation` inbetween `<Loader>` and `<Device>`.
  - If you can't find any, try again using `LSFGVK_ENV=1`.
    - If it still doesn't show up, you may be running in flatpak.
    - If it does show up, then the `active_in` property of your profile is likely misconfigured. Reconfigure it, then try again without `LSFGVK_ENV=1`.
- Check for warnings/errors from lsfg-vk in the terminal/log output. These will often give clues as to what is going wrong.
- If there are no errors/warnings and you have gone through all above steps, then move onto the next section.

If lsfg-vk is loaded, but frame generation is not working:
- (When using `pacing_mode = none`): Disable VRR.
- (When using `pacing_mode = none`): Explicitly enable V-Sync in your game settings.
- (When using `pacing_mode = none` on Gamescope/SteamDeck): Set `ENABLE_GAMESCOPE_WSI=0`.
- (When using `pacing_mode = none` on Wayland): Disable tearing control & direct passthrough in your compositor
- (When using `pacing_mode = none` on Wayland): Try running in windowed mode.
- Disable in-game upscaling options (e.g. DLSS, FSR, etc).
- Disable other Vulkan layers (e.g. VkBasalt, MangoHud)

If games do not open at all with lsfg-vk enabled for them (stuck at black screen):
- Check the log output for errors mentioning your configured `gpu`. A GPU that cannot be found or cannot participate in frame generation makes lsfg-vk fail with a named error quoting your configuration and the affected device, instead of silently falling back to another GPU (lsfg-vk-ui shows all valid names in a dropdown). Leaving `gpu` unset always works: frame generation then runs on the game's own GPU. See the Dual-GPU Setups section below for details.

Should none of the above help, please proceed to the bug reporting section.

### Performance Overlays
If you are using performance overlays like Steam's built-in overlay, there is a good chance that they will not show the correct framerate.

This is a known limitation of Vulkan layers and without directly working with the overlay developers, there is little that can be done to fix this.

### One-Way External Presentation Mode

If you are using `presentation = external` (one-way mode), the following
failure modes are specific to the `lsfg-vk-app` / layer handshake.

#### "failed to connect to app socket" / "Connection refused"

**Layer log:**
```
lsfg-vk: failed to connect to app socket '/run/user/1000/lsfg-vk/app.sock'
lsfg-vk: connect() to '/run/user/1000/lsfg-vk/app.sock' failed: Connection refused
```

**Cause:** The game started but `lsfg-vk-app` is not running, or the socket
path differs (e.g. `XDG_RUNTIME_DIR` mismatch between game and app).

**Fix:**
- Start `lsfg-vk-app` **before** launching the game.
- Ensure both processes share the same `XDG_RUNTIME_DIR` (default
  `/run/user/$(id -u)`). If running the game via Flatpak or a different
  user session, the socket will not be visible.
- The socket path is fixed at `$XDG_RUNTIME_DIR/lsfg-vk/app.sock`; it
  cannot be changed via configuration.

**Game behavior:** The game **exits immediately** — the layer throws during
swapchain creation and does not fall back.

#### "Connection reset by peer" (app died mid-stream)

**Layer log:**
```
lsfg-vk: external stream error: poll() on ipc socket failed: Connection reset by peer
lsfg-vk: something went wrong during lsfg-vk swapchain presentation:
- lsfg-vk: external stream error: poll() on ipc socket failed: Connection reset by peer
```

**Cause:** `lsfg-vk-app` was killed (SIGKILL, crash, OOM) while a stream
was active.

**Game behavior:** The game **stays alive**. The layer errors on the next
present but does not crash the game process.

**Recovery:** Restart `lsfg-vk-app` and launch a new game instance. The
new handshake establishes cleanly ("external presentation active" logged).

#### "Permission denied" on socket bind (app side)

**App log:**
```
lsfg-vk-app: fatal: bind() on '/run/user/1000/lsfg-vk/app.sock' failed: Permission denied
```

**Cause:** `XDG_RUNTIME_DIR` points to a non-writable directory, or the
`lsfg-vk` subdirectory exists with wrong permissions.

**Fix:**
- Ensure `XDG_RUNTIME_DIR` is set and writable (typically
  `/run/user/$(id -u)`).
- Remove any stale socket file: `rm -f $XDG_RUNTIME_DIR/lsfg-vk/app.sock`.
- The app creates the `lsfg-vk` directory with mode 0700; if it already
  exists with different ownership/permissions, delete it.

#### "output 'X' not found; available: ..."

**App log (Wayland):**
```
lsfg-vk-app: stream ended: output 'nonexistent-output' not found; available: 
```

**App log (X11):**
```
lsfg-vk-app: stream ended: output 'nonexistent-output' not found; available: HDMI-A-3
```

**Cause:** The `output` option in the profile (or `--output` CLI flag)
does not match any connector enumerated by the backend.

**Fix:**
- Run `lsfg-vk-app --profile <name> --session wayland --output invalid`
  (or `--session x11`) to see the list of available output names.
- **Wayland:** Names come from `xdg_output` (logical names like
  `HDMI-A-3`, `DP-1`). If the list is empty, the compositor does not
  support `xdg-output`; the app falls back to `wl_output` make/model
  strings.
- **X11:** Names come from RandR (e.g. `HDMI-A-3`, `DP-1`).
- Omit `output` entirely to auto-select the primary/active output.

#### Colorspace mismatch (HDR requested but display is SDR)

**Status:** Not testable on current rig (KWin reports HDR off, no HDR
config option in `GameConf`).

**Expected failure:** If a future HDR config requests BT.2020/PQ but the
processing GPU's swapchain only supports `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`,
the layer/app will fail to create a compatible swapchain.

**Workaround:** Ensure the display supports HDR and the compositor has HDR
enabled before attempting HDR external presentation. Current code prefers
`SRGB_NONLINEAR` and falls back to the first supported colorspace.

#### Session detection failures (wrong backend selected)

**Symptoms:** App starts but game fails to connect, or app logs "could not
connect the surface backend for session: wayland" on an X11-only session.

**Cause:** `--session auto` (default) prefers Wayland when
`WAYLAND_DISPLAY` is set. On an X11-only session with `WAYLAND_DISPLAY`
erroneously set, or a Wayland session where XWayland is desired, the
wrong backend is chosen.

**Fix:** Explicitly set `--session wayland` or `--session x11` on the
`lsfg-vk-app` command line. The `session` option is not in the profile;
it is a CLI-only flag.

#### Two games, one app (no isolation)

**Symptom:** Second game dies waiting for handshake; first game runs
normally.

**Cause:** `lsfg-vk-app` processes streams **sequentially** (single-threaded
accept loop). It cannot handle multiple concurrent game streams.

**Fix:** Run a separate `lsfg-vk-app` instance per game (each needs its
own socket — currently not configurable, so only one app instance per
`XDG_RUNTIME_DIR` is possible). True multi-game isolation requires
separate user sessions or a future socket-per-profile feature.

#### Multiplier hot-reload "restart required" message not appearing

**Observation:** Changing `multiplier` in `conf.toml` while external
presentation is active does not log the expected "config change requires
restart" message, though no context rebuild occurs (correct behavior).

**Cause:** `WatchedConfig::update()` uses filesystem `last_write_time`
comparison, which may have timestamp resolution issues on some filesystems.

**Status:** The logic exists in code (`Root::update()` detects external
contexts and suppresses rebuild) but the notification message did not
trigger in testing. No functional impact — external presentation continues
uninterrupted.

---

### Dual-GPU Setups
A step-by-step setup guide with tested examples lives in the [Dual-GPU Setup Guide](Dual-GPU-Guide.md).

If you configured the `gpu` option to a device other than the one your game renders on, lsfg-vk enters dual-GPU mode: the game keeps rendering and presenting on its own GPU, while the entire frame generation pipeline runs on the selected processing GPU. Frames travel between both GPUs over PCIe, which costs bandwidth and adds latency (see the [Configuration](Configuration.md) documentation for the details).

You can verify which device ended up doing what in the log output:
- `lsfg-vk: frame generation on the game's own device '...'` means everything runs on a single GPU (either because `gpu` is unset or set to the game's own device).
- `lsfg-vk: processing on '...' (game on '...')` means dual-GPU mode is active. The first quoted value is the processing device's UUID in hexadecimal, the second is the game device's name. The line printed at startup (`lsfg-vk: processing on '<device name>' [uuid ...]`) maps names to UUIDs, so you can tell which of your cards is doing the processing.

Keep in mind that changing the `gpu` option only takes effect after restarting the application; lsfg-vk logs `gpu change requires restart to take effect` if it detects a changed setting mid-session.

If games fail to start or frame generation errors out in dual-GPU mode, check the following:
- **Both GPUs must support the exchange extensions.** Dual-GPU mode requires `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` on both the game's GPU and the processing GPU. If either device is missing one of them, lsfg-vk refuses to start with an error naming the device and the missing extension. On NVIDIA this mechanism is available since driver 515.43.04, but it remains unverified, as no NVIDIA test hardware was available for this project.
- **Cross-vendor pairs usually fall back to LINEAR tiling.** Both devices must agree on a memory layout (a DRM modifier) for the exchanged frames. GPUs from different vendors typically share no proper modifiers, so lsfg-vk exchanges frames as plain LINEAR images instead. This works on the hardware tested during development, where LINEAR memory accepts every operation frame generation needs (sampling, storage access and transfers), but whether a driver allows e.g. storage image access on LINEAR memory is driver-dependent: if your combination fails with an error about negotiating an exchange layout, your driver likely does not support the required usages on LINEAR.
- **Flatpak applications need access to both render nodes.** The sandbox must be able to see the render nodes (`/dev/dri/renderD*`) of both the game's GPU and the processing GPU, otherwise device selection or the frame exchange fails inside the container. See the [Flatpak Guide](Flatpak-Guide.md) for general Flatpak setup.
- **HDR in dual-GPU mode is unverified.** The automated test harness only exercises SDR frames, so transporting HDR content between two GPUs ships untested. If you combine HDR with dual-GPU mode and run into corruption or errors, please mention this in your bug report.
- **Pipeline caches are per-driver.** Compiled shaders are cached in files named `lsfg-vk_pipeline_cache_<driverUUID>.bin` inside your cache directory (`$XDG_CACHE_HOME`, falling back to `~/.cache`). GPUs sharing a driver (e.g. two AMD cards on the same Mesa install) share one cache file, while different drivers (e.g. Intel and AMD) get separate ones. The old unkeyed `lsfg-vk_pipeline_cache.bin` from previous versions is removed automatically, so there is nothing to clean up by hand.

### Opening a Bug Report
When opening a bug report, please include the following information to help us diagnose and fix the issue:
- A detailed description of the issue you are experiencing.
- What system you are running on (OS, GPU, drivers, etc).
- The game you are trying to run (and through what platform, e.g. Steam Proton, native Linux, etc).
- The relevant section of your lsfg-vk configuration file.

Ideally, also include a log file with the environment variables `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and `VK_LOADER_DEBUG=all` set. You might need to install the Vulkan validation layers package for your distribution to do this.

If you're running the game through Steam, the log file is located at `~/.steam/steam/logs/console-linux.txt`. Please clear it before launching the game to ensure it only contains relevant information.
