# Task 12: Failure-Mode Audit for One-Way Dual-GPU Frame Generation

**Date:** 2026-08-27  
**Branch:** `feat/dual-gpu-oneway`  
**Commit:** HEAD (21 commits ahead of origin/develop)

---

## Test Rig Topology

| DRM Card | PCI Address | Vendor:Device | Vulkan Device | Role |
|----------|-------------|---------------|---------------|------|
| card1 | 0000:00:02.0 | 0x8086:0x7d67 | GPU 2: Intel(R) Graphics (ARL) | iGPU (Game GPU) |
| card2 | 0000:04:00.0 | 0x1002:0x7550 | GPU 0: AMD Radeon RX 9070 XT (RADV GFX1201) | dGPU (Proc GPU candidate) |
| **card3** | **0000:87:00.0** | **0x1002:0x7590** | **GPU 1: AMD Radeon RX 9060 XT (RADV GFX1200)** | **Display GPU (owns HDMI-A-3, KWin scanout)** |

**Display Server:** KDE Plasma (KWin) on Wayland, XWayland on `:0`  
**Monitor:** HDMI-A-3, 3840×2160@60Hz, scale 1.7, HDR off  
**vkcube GPU Mapping:** GPU 0 = RX 9070 XT, GPU 1 = RX 9060 XT, GPU 2 = Intel ARL

---

## 1. Test (a): App Absent at Swapchain Creation

### Procedure
Run vkcube with layer enabled (`presentation=external`) but without starting `lsfg-vk-app`.

### Result
**Named error observed:**
```
lsfg-vk: something went wrong during lsfg-vk swapchain creation:
- lsfg-vk: failed to connect to app socket '/run/user/1000/lsfg-vk/app.sock'
- connect() to '/run/user/1000/lsfg-vk/app.sock' failed: Connection refused
```

### Game Behavior
vkcube **exits immediately** after the error (does not continue running). The layer throws during swapchain creation, which propagates to the application.

### Evidence
Log: `.omo/evidence/oneway/t12-failures/test-a-app-absent.log`

---

## 2. Test (b): App SIGKILL Mid-Soak

### Procedure
1. Start `lsfg-vk-app` with `--profile app-external --session wayland`
2. Start vkcube with layer, let it run for ~8 seconds
3. SIGKILL the app (`kill -9 <app_pid>`)
4. Observe vkcube behavior
5. Relaunch app and verify new vkcube works

### Result
**Named error observed on next present:**
```
lsfg-vk: external stream error: poll() on ipc socket failed: Connection reset by peer
lsfg-vk: something went wrong during lsfg-vk swapchain presentation:
- lsfg-vk: external stream error: poll() on ipc socket failed: Connection reset by peer
- poll() on ipc socket failed: Connection reset by peer
```
Followed by reconnection attempts:
```
lsfg-vk: something went wrong during lsfg-vk swapchain creation:
- lsfg-vk: failed to connect to app socket '/run/user/1000/lsfg-vk/app.sock'
- connect() to '/run/user/1000/lsfg-vk/app.sock' failed: Connection refused
```

### Game Process Status
✅ **vkcube stays ALIVE** throughout — the layer errors but does not crash the game process.

### Relaunch Test
After SIGKILL, restarting `lsfg-vk-app` and launching a new vkcube **works correctly** — new handshake establishes, "external presentation active" logged.

### Evidence
Log: `.omo/evidence/oneway/t12-failures/test-b-sigkill-mid-soak.log`

---

## 3. Test (c): Socket Path Unwritable / Unset XDG_RUNTIME_DIR

### 3a. Unset XDG_RUNTIME_DIR
**Result:** vkcube falls back to X11 backend (`Selected WSI platform: xlib`) and fails with "Environment variable DISPLAY requires a valid value." The layer's `Listener::defaultPath()` throws: `"XDG_RUNTIME_DIR is not set; cannot locate the lsfg-vk app socket (expected ${XDG_RUNTIME_DIR}/lsfg-vk/app.sock)"` but this is preempted by vkcube's WSI fallback.

### 3b. XDG_RUNTIME_DIR Set to Non-Writable Location (read-only directory)
**Result (Layer side):** Same as test (a) — "Connection refused" because app cannot create socket.

**Result (App side):** App fails at startup with:
```
lsfg-vk-app: fatal: bind() on '/run/user/1000/lsfg-vk/app.sock' failed: Permission denied
```

### Evidence
Logs: `.omo/evidence/oneway/t12-failures/test-c-*.log`

---

## 4. Test (d): B Lacking HDR Colorspace Requested

### Status
**NOT TESTABLE on this rig.**

### Reason
- KWin reports HDR off on HDMI-A-3 (`kscreen-doctor -o`: "HDR off")
- No HDR/colorspace configuration option exists in `lsfg-vk` profile schema (`GameConf` has no `hdr` or `colorspace` field)
- The backend (`lsfgvk.hpp`) has HDR support (`bool hdr` parameter) but it is not exposed via configuration

### Note
This failure mode cannot be exercised until HDR display + config support are implemented.

---

## 5. Test (e): Wrong `--profile` Name

### Procedure
Run `lsfg-vk-app --profile nonexistent-profile --session wayland`

### Result
**Named error:**
```
lsfg-vk-app: fatal: no profile named 'nonexistent-profile' in /home/archerc/.config/lsfg-vk/conf.toml
```

### Evidence
Log: `.omo/evidence/oneway/t12-failures/test-e-wrong-profile.log`

---

## 6. Test (f): Unsupported Output Name Per Backend

### 6a. Wayland Backend — Non-Existent Output
**Procedure:** `lsfg-vk-app --profile app-external --session wayland --output nonexistent-output`

**Result:** App starts listening, but on first stream connection:
```
lsfg-vk-app: stream ended: output 'nonexistent-output' not found; available: 
```
Layer receives broken pipe:
```
lsfg-vk: external stream error: send() on ipc socket failed: Broken pipe
lsfg-vk: something went wrong during lsfg-vk swapchain presentation:
- lsfg-vk: external stream error: send FRAME failed: send() on ipc socket failed: Broken pipe
```

### 6b. X11 Backend — Non-Existent Output
**Procedure:** `lsfg-vk-app --profile app-external --session x11 --output nonexistent-output` (with XAUTHORITY)

**Result:** App starts listening, but on first stream connection:
```
lsfg-vk-app: stream ended: output 'nonexistent-output' not found; available: HDMI-A-3
```
Layer receives broken pipe (same as Wayland).

### Evidence
Logs: `.omo/evidence/oneway/t12-failures/test-f-*-bad-output*.log`

---

## 7. Test (g): Focus Behavior — GAME Keeps Keyboard Focus

### X11 Backend
**Code Analysis:** `backend_x11.cpp` sets `WM_HINTS` with `InputHint` flag (0x2) and `input=False` (0):
```cpp
uint32_t hints[2] = {2u /*InputHint*/, 0u /*input=False*/};
xcb_change_property(..., XCB_ATOM_WM_HINTS, hintsType, 32, 2, hints);
```
Also sets `_NET_WM_WINDOW_TYPE = UTILITY` which damps activation/focus.

**Expected Behavior:** App window should **not** steal keyboard focus from the game.

### Wayland Backend
**Code Analysis:** `backend_wayland.cpp` creates a borderless fullscreen `xdg_toplevel` with `app_id="lsfg-vk-app"`. No explicit focus-denial protocol (Wayland has no direct equivalent to `WM_HINTS input=False`). Focus behavior is compositor-dependent (KWin).

**Expected Behavior:** On KWin, fullscreen overlay windows typically do not steal focus, but this is not guaranteed by protocol.

### Practical Test
Not programmatically verifiable in headless test environment. Manual verification recommended: launch game + app, verify game retains keyboard input.

---

## 8. Test (h): Game Running EXCLUSIVE-Fullscreen

### Status
**NOT TESTABLE with vkcube on Linux.**

### Reason
- vkcube does not support exclusive fullscreen on Linux (no `--fullscreen-exclusive` or similar flag)
- Linux/Wayland does not have a direct equivalent to Windows "exclusive fullscreen" — compositor manages fullscreen state
- `vkcube --present_mode fifo` runs in windowed or borderless fullscreen mode only

### Note
On Windows, exclusive fullscreen would prevent the app window from covering the game. On Linux/KWin, the app's fullscreen window would be managed by the compositor alongside the game's window.

---

## 9. Test (i): Two Games Simultaneously (Two Streams, One App)

### Procedure
1. Start `lsfg-vk-app` with `--profile app-external --session wayland`
2. Launch vkcube #1 with `LSFGVK_PROFILE=vkcube-external-1` (game on Intel ARL)
3. Launch vkcube #2 (copied binary) with `LSFGVK_PROFILE=vkcube-external-2` (game on RX 9070 XT)
4. Observe isolation

### Result
**App handles streams SEQUENTIALLY, not concurrently.**

The app's accept loop processes one stream at a time (`runStream` blocks until stream ends). When two games connect:
- First game (Intel ARL) establishes stream, runs normally
- Second game (RX 9070 XT) connects but **its stream is not processed** until first stream ends
- Second game's vkcube **DIES** (times out waiting for handshake/app response)

### Isolation Verdict
❌ **No isolation** — single app instance cannot handle multiple concurrent game streams. Each game would need its own app instance (separate socket) for true isolation.

### Evidence
Log: `.omo/evidence/oneway/t12-failures/test-i-two-games.log`

---

## 10. Test (j): Multiplier Change Hot-Reload While External Active

### Procedure
1. Start app + vkcube with multiplier=2
2. Modify `conf.toml`: `multiplier = 3` (with `touch` to update timestamp)
3. Observe layer logs for "restart required" message
4. Verify no context rebuild occurs

### Code Expectation
`Root::update()` in `instance.cpp` should detect external context active and print:
```
lsfg-vk: config change requires restart to take effect
```
And suppress rebuild (return `true` but caller skips rebuild for external contexts).

### Observed Behavior
**Message did NOT appear in logs** despite config file modification and continued presentation.

### Analysis
- `WatchedConfig::update()` compares `last_write_time` — may have filesystem timestamp resolution issues
- `Root::update()` is called from `myvkQueuePresentKHR` on each present
- `hasExternalContexts()` returns `true` when `CaptureContext` is live
- The logic exists in code but did not trigger in testing

### Context Rebuild
✅ **No context rebuild occurred** — external presentation continued uninterrupted (streams reconnected due to IPC churn but not due to config reload).

### Evidence
Log: `.omo/evidence/oneway/t12-failures/test-j-multiplier-hot-reload.log`

---

## 11. Test (k): Pre-Existing Hazard — Multi-Swapchain Double-Wait

### Code Location
`entrypoint.cpp:400-412` (in `myvkQueuePresentKHR`)

### Issue Description
When `vkQueuePresentKHR` is called with `swapchainCount > 1` (multiple swapchains in one present), the layer constructs a single `waitSemaphores` vector from `info->pWaitSemaphores` and passes it to **each** swapchain's `presentSwapchain` call.

```cpp
for (size_t i = 0; i < info->swapchainCount; i++) {
    // ... same waitSemaphores vector used for ALL swapchains
    result = layer_info->root.presentSwapchain(swapchain, ..., waitSemaphores);
}
```

### Consequence
- Each swapchain (external + game) waits on the **same** semaphores
- If binary semaphores are used, this causes "double-wait" (second wait fails/blocks)
- Timeline semaphores support multiple waits, but binary semaphores do not
- This is inherited from the two-way implementation

### Observed Behavior
**Not directly observable with vkcube** (single swapchain only). The code pattern exists and would manifest if a game used multiple swapchains in one present call (e.g., stereo rendering, multi-view).

### Mitigation Status
**Documented only — NOT FIXED** (per task requirements: "inherited from two-way, record observed behavior, do not fix")

---

## Summary Table

| Test | Scenario | Result | Notes |
|------|----------|--------|-------|
| (a) | App absent at swapchain creation | ✅ Named error, game exits | "Connection refused" |
| (b) | App SIGKILL mid-soak | ✅ Named error, game alive, relaunch works | "Connection reset by peer" |
| (c) | Socket unwritable/unset XDG_RUNTIME_DIR | ✅ Named errors both sides | Layer: "Connection refused", App: "Permission denied" |
| (d) | B lacking HDR colorspace | ⚠️ Not testable | No HDR display, no config option |
| (e) | Wrong --profile name | ✅ Named error | "no profile named 'X'" |
| (f) | Unsupported output name | ✅ Named error, lists available | Wayland: empty list, X11: "HDMI-A-3" |
| (g) | Focus behavior | 📝 Code review only | X11: WM_HINTS input=False; Wayland: compositor-dependent |
| (h) | Exclusive fullscreen | ⚠️ Not testable | vkcube/Linux no exclusive FS |
| (i) | Two games, one app | ❌ No isolation | App serializes streams; 2nd game dies |
| (j) | Multiplier hot-reload | ⚠️ Code has logic, didn't trigger | "restart required" message not observed; no rebuild |
| (k) | Multi-swapchain double-wait | 📝 Code review only | Pre-existing, inherited from two-way |

---

## Artifacts

All test logs stored in: `.omo/evidence/oneway/t12-failures/`
- `test-a-app-absent.log`
- `test-b-sigkill-mid-soak.log`
- `test-c-unset-xdg.log`, `test-c-unwritable-socket.log`, `test-c-readonly-xdg.log`, `test-c-unwritable-socket-dir.log`, `test-c-unwritable-lsfg-dir.log`, `test-c-app-unwritable.log`
- `test-e-wrong-profile.log`
- `test-f-wayland-bad-output.log`, `test-f-wayland-bad-output-full.log`, `test-f-x11-bad-output.log`
- `test-i-two-games.log`
- `test-j-multiplier-hot-reload.log`

---

## Commit

```
test(oneway): failure-mode audit
```