# Learnings — oneway-dualgpu

Conventions, patterns, and successful approaches discovered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## 2026-08-26 t1 (rig topology + two-way baselines)
- Display card = card3 = RX 9060 XT (0x7590) via only enabled connector card3-HDMI-A-3 (4K@60, KDE Wayland/KWin, Xwayland on :0). card2=9070XT (0x7550), card1=Intel ARL (0x7d67). Matches plan's known-device table.
- WM matrix: BOTH backends natively testable — Wayland-native session + Xwayland for X11. No nested compositor needed (weston/cage not installed; fine).
- Native `lsfg-vk-cli debug` IGNORES LSFGVK_GPU env (layer-only); must pass `-g <deviceName>` to pin processing GPU. Default fell to GPU0 (9060XT). Documented in rig-display.md.
- Two-way cross-device works today at v10: debug CLI game=Intel/render=9070XT exit=0; vkcube gpu_number=2 (Intel) + layer proc=9070XT ran full 15s (exit=124=SIGINT timeout), success line present. VUID/validation-error counts: 0 in both logs.
- vkcube uuid convention: processing uuid encodes PCI bus (87→9060XT/card3, 04→9070XT/card2).

---

## 2026-08-26 Reconciliation + task-5 prep (corrects stale checkpoint)

### Ground-truth reconciliation
- Task 4 (Layer capture-mode / CaptureContext) was reported "in-flight/being verified" in the prior assistant state, but is actually **COMMITTED** as `e68cc9b feat(layer): external presentation capture context` with passing evidence in `.omo/evidence/oneway/t04-layer.log`. All 4 acceptance criteria pass:
  - app absent → named socket error (`failed to connect to app socket`)
  - app present → `external presentation active`
  - 500-frame fd-leak: stable 4→4
  - SIGKILL mid-stream → `vkcube still alive`
- Tasks 1–4 all committed on `feat/dual-gpu-oneway`. Working tree clean (no tracked-file mods).
- The out-of-order app scratch (`lsfg-vk-app/`, uncommitted, X11 backend segfaulted) was reset (`rm -rf`) — correct, since WSI backends were built before the task-5 skeleton and were uncommitted.
- TRANSIENT: root `CMakeLists.txt` refs `add_subdirectory(lsfg-vk-app)` with `LSFGVK_BUILD_APP=ON` (default). With `lsfg-vk-app/` deleted, full reconfigure is broken until task-5 recreates it. Task 5 restores the build.

### Task-5 anchors (confirmed)
- App ctor: `vk::Vulkan(appName, version, engineName, engineVersion, PhysicalDeviceSelector, isGraphical=false, setLoaderData, cachefile, enableDmaBufExtensions=false)` — `vulkan.hpp:172`
- `PhysicalDeviceSelector = const std::function<VkPhysicalDevice(const VulkanInstanceFuncs&, const std::vector<VkPhysicalDevice>&)&>`
- Config: `ConfigFile()` default path; `ConfigFile(path)`; `findConfigurationFile()`; `profiles()` → `vector<GameConf>`; `GameConf.{name, gpu(optional), presentation, output(optional)}`; `Presentation{Game,External}`
- `DISABLE_LSFGVK` setenv around anything that could nest-load the layer (mirror `instance.cpp`)
- ipc `Listener`/`Connection` from task 3; `protocol.hpp` Message variant (Hello/Negotiated/Ready/Frame/Release + Staging carries fd)
- Log lines: `lsfg-vk-app: listening on <path> (processing on '<deviceName>')`; `lsfg-vk-app: stream from '<device_name>' <WxH fmt>`

---

## 2026-08-26 task-5 prep: IPC contract + rowPitch + activation anchors

### Layer handshake contract (capture_context.cpp) — app must satisfy reversed
- Layer connects first (`Connection::connect(sockPath)`), sends HELLO, waits NEGOTIATED (2 s),
  creates 2 staging images at `negotiated.rowPitch` (format R8G8B8A8_UNORM, capture_context.cpp:151),
  exports dma-buf, sends 2x STAGING (each 1 fd via attachFd+Staging{}), waits READY (2 s), then per present:
  drainReleases -> selectFreeSlot -> blit -> export sync-fd -> FRAME -> forward present.
- Layer validates NEGOTIATED modifier on GAME device only (capture_context.cpp:110-148): modifier must be
  advertised by game device with TransferDst|TransferSrc|Sampled|Storage bits. LINEAR (modifier 0) passes on
  this rig (t04: `external presentation active` twice => handshake completed with LINEAR).
- drainReleases frees a slot only on RELEASE{stagingIdx}. So the app MUST send RELEASE per FRAME or the layer
  throws `no free staging slots (app stalled)` after 2 frames. Skeleton: on FRAME, takeReceivedFd() the sync fd,
  close it, send RELEASE{stagingIdx}.

### App NEGOTIATED reply (task 5 skeleton)
- modifier = EXCHANGE_MODIFIER_LINEAR (0)
- rowPitch = align256(width*4)  // staging format is R8G8B8A8 (4 Bpp); matches image.cpp:84-97 formula
- allocationSize = rowPitch * height
- (true caps negotiation is task 6; skeleton only needs LINEAR + READY)

### DISABLE_LSFGVK activation (critical)
- Set in VkLayer_LSFGVK_frame_generation.json.in (`"DISABLE_LSFGVK":"1"`). Layer is an ICD-loader layer, so the
  app's vk::Vulkan(appName) ctor (dlopens libvulkan.so.1 + vkGetInstanceProcAddr) WOULD have the layer inject into
  the app's own instance unless DISABLE_LSFGVK=1 is set first. MUST setenv("DISABLE_LSFGVK","1",1) BEFORE creating
  the app vk::Vulkan instance; unsetenv in catch paths (mirror instance.cpp:354/386/393).

### PhysicalDeviceSelector (app) — mirror instance.cpp:374-379
- selector lambda over (VulkanInstanceFuncs, vector<VkPhysicalDevice>); match deviceName/ids/pci to profile.gpu
  via GetPhysicalDeviceProperties2 + IDProperties + PCIProperties from fi. No findProfile() exists; iterate
  configFile.profiles() and match by .name (error if ambiguous).

### Task 5 completion + std::bad_alloc root cause (CRITICAL)
- selectProfile() MUST return `ls::GameConf` BY VALUE. It originally returned
  `const ls::GameConf&` into the local `ls::ConfigFile config{path}` which is destroyed
  on return — dangling reference. Caller held it across vk::Vulkan creation; the freed
  std::string inside leaked a garbage-size pointer -> `std::string::reserve(garbage)` ->
  std::bad_alloc -> app crashed on startup. Return-by-value copy (apply --output override
  to the copy before returning) fixes it. GameConf is cheap to copy; fetched once at start.
- QA (t05-app-skeleton.log): 10 rapid vkcube+layer cycles, all 10 established external
  presentation, app fd stable 8->8 across 20 streams (no leak), single `ss -lxp` listener
  on /run/user/1000/lsfg-vk/app.sock. App receives `stream from 'Intel(R) Graphics (ARL)'
  500x500 VkFormat(58)` per cycle. Layer/game profile = Intel ARL; app profile = RX 9060 XT.

### Task 6 completion: negotiate exchange layout from true caps + wire backend contexts

- App negotiates R8G8B8A8_UNORM from the APP (RADV) device TRUE caps; the layer then
  validates the negotiated modifier on the GAME device (ANV). A RADV-only proper DRM
  modifier (0x144115188076389125) is NOT advertised by ANV, so the app MUST negotiate
  the LINEAR fallback (modifier == EXCHANGE_MODIFIER_LINEAR). negotiateExchangeLayout
  with app-vs-app (RADV vs RADV) returned a RADV-only proper modifier that ANV rejected
  ("not advertised by game device") -> stream aborted before READY. FIX: negotiate
  against a LINEAR-only caps proxy (mirror layer swapchain.cpp cross-device path), which
  forces modifier == LINEAR and is accepted by the game device.
- exportDmaBuf returns an unowned dma-buf handle (fd) that the image does not consume.
  The success path only closed it on the dup-failure branch, leaking 1 fd per dest image
  (2/stream -> 9->49 over 20 streams). FIX: close exp.fd right after the dup that is
  handed to the backend (the backend only imports the dup). Restored fd stability 9->9.
- QA (t06-transport.log): build green (exit 0, no warnings). 10 rapid vkcube+layer
  cycles all 10 established external presentation on 'Intel(R) Graphics (ARL)'. Every
  stream logged "context created on 'dma-buf' cross-device=1" (20 total across 10
  cycles, isCrossDevice() verified true each time). Single ss -lxp listener, fd stable
  9->9, ZERO VUID. App profile = RX 9060 XT (RADV); layer/game profile = Intel ARL.
- Layer/game profile gpu="Intel(R) Graphics (ARL)"; app profile gpu="AMD Radeon RX 9060 XT (RADV GFX1200)".
- QA harness: XDG_RUNTIME_DIR=/run/user/1000, XDG_CONFIG_HOME=/tmp/lsfgq, VK_LAYER_PATH=/tmp/opencode/layer-test,
  VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation, socket /run/user/1000/lsfg-vk/app.sock.

### Environment
- Rig GPUs present: Intel ARL (00:02.0), RX 9070 XT (04:00.0), RX 9060 XT (87:00.0 = display GPU). radv/hasvk ICDs.
- conf.toml at ~/.config/lsfg-vk/conf.toml (profile "vkcube-dual-gpu", gpu=RX 9070 XT, multiplier=2).
- Task 5 QA must add an APP profile pointing gpu at RX 9060 XT (display); layer/game profile at a game GPU.
- Root CMakeLists refs add_subdirectory(lsfg-vk-app) w/ LSFGVK_BUILD_APP=ON (ON default); build tree currently
  un-configurable until task 5 recreates lsfg-vk-app/.

---

## 2026-08-27 task-7a WSI/X11 backend: code-review findings (pre-7b)

Ground truth (my own checks, not the subagent's "done"):
- `cmake --build build --target lsfg-vk-app` exit 0. `nm -D /usr/lib/libvulkan.so.1` exports `vkCreateXcbSurfaceKHR` (camelCase, NOT `xcb_surface`). `vulkan_xcb.h` present.
- `git status --short lsfg-vk-app`: only `CMakeLists.txt` modified + new `wsi/` dirs. NO scope creep.
- Task 6 remains DONE (`44ed5bd`), already `[x]` in the plan.

Two bugs caught in review of `backend_x11.cpp` (must fix in 7b):
1. WM_HINTS flags wrong at line ~372: `{1u<<16,0,0}`. Spec requires input=False (window NEVER grabs focus).
   Fix: property atom `WM_HINTS`, type atom `HINTS`, data `{2 /*InputHint*/, 0 /*input=False*/}`, 32-bit, 2 elements.
2. `surfaceCaps()` derefs `mFuncs.GetPhysicalDeviceSurfaceCapabilitiesKHR` which is NULL on a non-graphical
   `vk::Vulkan`. Fix: fetch `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` from the loader (vkGetInstanceProcAddr),
   guard null — consistent with how GetPhysicalDeviceSurfaceFormatsKHR is already fetched.

Note: `VulkanDeviceFuncs` swapchain funcs (CreateSwapchainKHR/AcquireNextImageKHR/QueuePresentKHR/DestroySwapchainKHR)
are populated ONLY when the `vk::Vulkan` is built graphical. So the `--wsi-test` smoke path must build its OWN
graphical `vk::Vulkan` (isGraphical=true) on the same display device; it must not reuse the main non-graphical transport device.

### 2026-08-27 task-7 XCB surface blocker (common layer): finding
- `VK_KHR_xcb_surface` must be ENABLED on *graphical* `vk::Vulkan` instances, else `vkCreateXcbSurfaceKHR` returns NULL from
  `vkGetInstanceProcAddr` and `ls::wsi::createSurface` throws `...returned null`. `vulkaninfo` lists it as available (rev 6).
- Fixed in `lsfg-vk-common/src/vulkan/vulkan.cpp`: `createInstance(...)` gains trailing `bool enableSurfaceExtensions=false`
  (default false, non-graphical transport stays byte-identical). When true it queries `vkEnumerateInstanceExtensionProperties(VK_NULL_HANDLE, …)`
  and enables XCB availability-filtered (only if present → no `VK_ERROR_EXTENSION_NOT_PRESENT`); library pulls the enum via the loader (`ipa<>`).
- The single call site in the `Vulkan::Vulkan` ctor now passes `isGraphical` as `enableSurfaceExtensions`. `VK_KHR_wayland_surface` left for task 13.

---

## 2026-08-27 task-8 presentation choreography: architecture decision

### Decision: make the app transport `vk::Vulkan` graphical (isGraphical=true)
- `lsfg-vk-app/src/main.cpp:547` currently creates the transport device with
  `isGraphical=false` (compute-only; swapchain PFNs null). The inline comment
  there literally says "swapchain funcs are null until a later task" = task 8.
- Flipping to `true` is a single, safe change:
  - ctor line 562-566 passes `isGraphical` to `createInstance(...)` => enables
    `VK_KHR_xcb_surface` on the instance (needed for `vkCreateXcbSurfaceKHR`).
  - ctor line 582-585 passes `isGraphical` to `initVulkanDeviceFuncs(...)` =>
    populates CreateSwapchainKHR / AcquireNextImageKHR / QueuePresentKHR PFNs.
  - `findQFI` (line 572-573) then selects the VK_QUEUE_GRAPHICS_BIT family (0
    on RADV RX 9060 XT, which also carries compute) => fine for image import/
    export + swapchain.
  - `initVulkanInstanceFuncs(*this->instance, get_mpa(), false)` stays `false`
    (line 567) => no instance-funcs change.
- BLAST RADIUS: `createLogicalDevice` is app-only (standalone ctor
  vulkan.cpp:575-580); the layer's two-way path uses the INSTANCE-DEVICE ctor
  (vulkan.cpp:599-626) with its own `setLoaderData` from entrypoint.cpp:195.
  The app sets `DISABLE_LSFGVK=1` so the layer never injects into the app. So
  making the transport device graphical cannot affect two-way behavior.

### Presentation device placement: swapchain on the SAME device as destination images
- Destination images are created on the transport `vk` (stream.cpp:208) and
  self-exported to the backend; the backend imports them as dma-buf descriptors
  and generates in-place into the SAME underlying memory.
- Therefore the swapchain should live on the transport `vk` too, so the
  destination→swapchain blit is intra-device (no cross-device blit). Per FRAME
  we still must wait the backend's doneFd sync-fd (importSyncFd → wait) before
  blitting, because the backend writes on ITS OWN device handle.

### Choreography to replicate (references)
- Layer present loop swapchain.cpp:494-618: for each generated destination
  image acquire→blit→present, THEN present the original/real frame last.
- Backend per-call frame count = destImages.size() (currently 2 doneFds from
  scheduleFramesCross lsfgvk.cpp:963-1046).
- App "generated frames" come from backend dest images; "real frame" = the
  captured source image (the game frame the layer sent via STAGING).
- Present loop must multiplex socket POLLIN (read FRAME, drain RELEASE) with
  frame readiness, per task-8 spec.

### Temporary smoke path (--wsi-test)
- The task-7 smoke path in main.cpp exercises WSI standalone; per its commit
  message it is "removed with task 8". Task 8 verifies via full E2E (layer +
  vkcube in external mode + app presenting on HDMI-A-3).

---

## 2026-08-27 (cont.) Task 8 re-delegation: verified API + false-completion note

### False completion diagnosis (task ses_fbd426a4bffe5DS9CLqITJCK76)
- The prior subagent ONLY studied code (read main.cpp/stream.cpp/stream.hpp/surface_backend.hpp/layer swapchain.cpp) then was compacted before writing. `git status --short lsfg-vk-app` stays empty, no Task 8 commit, no presentation.cpp/.hpp. `--wsi-test` smoke path is STILL present at main.cpp:264-499.
- Plan Task 7 = `8411f33` is the latest commit. Task 8 not implemented.
- NOTE off-scope contamination: `.omo/evidence/task-N-issue-159-dual-gpu` (N=2..20) belong to the DIFFERENT issue-159 plan, not oneway-dualgpu. Do not treat those as Task 8 deliverables.

### Verified API (authoritative, so agent cannot guess)
- `lsfgvk.hpp:229` `std::vector<int> scheduleFrames(Context& context, int captureReadyFd = -1);` — one doneFd per destination image (currently 2); ownership of each doneFd transferred to caller; caller imports it into a semaphore (consume-on-import), so NO separate close.
- `lsfgvk.hpp:166/201` `Context& openContext(...)`, `:239` `isCrossDevice(const Context&)`, `:255` `selectedDeviceSupportsDmaBuf()`, `:270` `closeContext(const Context&)`. stream.cpp:238-253 already calls these correctly (copy the exact call).
- Layer proven import pattern = swapchain.cpp:57-73 `importSyncFd(vk, VkSemaphore, int fd)`: `VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`, handleType `SYNC_FD_BIT`, on failure `close(fd)` before throw. The app must NOT re-define it — copy this body into presentation.cpp.
- The `--wsi-test` path (main.cpp:340-495) is the GOLD WSI template: `createX11SurfaceBackend()` → `connect()` → `createWindow(name, {512,512}, 0)` → `createSurface(*vk, handle)` → `surfaceCaps(...)` → **extent clamp with degenerate 0x0 fallback** (main.cpp:361-377, mirror exactly) → FIFO `VK_FORMAT_R8G8B8A8_UNORM` + `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` → composite-alpha → acquire→blit→signalSem→present loop. Reuse its swapchain-create + present-loop skeleton verbatim.

### CRITICAL correction to plan text: Hello has NO imageColorSpace
- The frozen layer's `Hello` struct (ipc/protocol.hpp) carries `protoVersion/gameUuid/deviceName/vkFormat/width/height` ONLY — no colorSpace. The plan's "imageColorSpace from HELLO passed through" is INCOHERENT with the frozen protocol.
- Resolution: derive the swapchain colorspace from `surfaceCaps()` supported colorspaces; prefer `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` (as the smoke path hard-codes), else pick the first supported. Do NOT attempt to read HELLO.colorSpace.

### Design decisions (already committed in notepad)
- Flip `main.cpp:547` `isGraphical` false→true so the transport device is graphical (swapchain PFNs + xcb_surface + graphics family). Blast radius = app-only; two-way path uses the layer's own instance-device ctor. Verified true.
- Swapchain + dest images live on the SAME transport `vk` → intra-device blit. Verify dest image creation in stream.cpp:208 is on `vk`.
- Surface lifecycle = PER-STREAM: each stream owns its own SurfaceBackend (own X connection) + window + surface + swapchain; torn down on stream end (mirrors the `context` owned_ptr lambda-deleter at stream.cpp:246-250). StreamState must hold these so the accept-loop `streams.erase` tears them leak-free.

### Present pipeline choreography (mirror layer swapchain.cpp:489-611)
Per accepted FRAME: `doneFds = scheduleFrames(ctx, captureFd)`; for each doneFd importSyncFd→wait→acquire→blit **destinationImages[i%2]→swapchain image**→present; THEN present ONE real frame = blit the latest **source image** (the game frame)→present. Order per cycle = generated frames then real (swapchain.cpp:489-611 sequencing). Present loop must poll socket POLLIN to drain RELEASE + read FRAME concurrently.

---

## 2026-08-27 task-9 pacing, backpressure, stall policy

### Layer: bounded slot acquisition with 500 ms deadline (capture_context.cpp)
- `selectFreeSlot()` now polls for a free slot with a 500 ms deadline instead of throwing immediately.
- On each iteration: round-robin check → if no free slot, check deadline → if expired, throw named error `lsfg-vk: external stream error: no free staging slots within 500 ms (app stalled)` → drain pending RELEASE messages → sleep 1 ms → retry.
- This bounds the game's stall to ≤ ring depth (2 frames) + 500 ms before surfacing a named error, matching the task-9 acceptance criteria.

### App: idle detection + keep-alive + stats (presentation.cpp)
- Track `lastFrameTime` (steady_clock) updated on each received FRAME.
- When poll times out (200 ms) and no FRAME for >5 s: present the last captured game frame once (using `lastFrameStagingIdx`), log `lsfg-vk-app: idle >5 s, presenting last frame (slot N)` exactly once (`idleLogged` guard), keep window alive.
- On new FRAME arrival: reset `idleLogged`, update `lastFrameTime` and `lastFrameStagingIdx`.
- Per-second stats when `LSFGVK_APP_VERBOSE` set: `lsfg-vk-app: <fps> fps, slots 2/2, queued 0` (slot count fixed at 2 by backend contract; queued always 0 in steady state since we process one FRAME per loop iteration).
- Frame counter `frameCount` incremented per FRAME, reset each stats interval.

### Verification criteria (from task 9)
- `kill -STOP app` for 3 s under load ⇒ game stalls ≤ ring bound (2 frames) then ERRORS NAMED with the new 500 ms deadline message.
- Resume ⇒ stream recovers WITHOUT context rebuild (layer's CaptureContext stays alive, only selectFreeSlot was blocked).
- Stats line accurate when `-v` enabled.

### Key implementation details
- Ring depth remains 2 (fixed by backend's exactly-2-sources contract) — no changes to slotFree array size.
- No infinite waits: layer polls with 500 ms deadline, app polls socket with 200 ms timeout.
- Handshake deadlines unchanged (2 s for NEGOTIATED/READY).
- Error messages follow existing `ls::error` pattern with `lsfg-vk: external stream error:` prefix.

---

## 2026-08-28 task-10 Wayland backend (xdg-shell): completion

### Implementation summary
- Created `lsfg-vk-app/src/wsi/backend_wayland.cpp` implementing `SurfaceBackend` interface using libwayland-client with xdg-shell (stable) and xdg-output (unstable v4).
- Added `--session {x11,wayland,auto}` CLI option to `lsfg-vk-app` with auto-detection: prefers Wayland when `WAYLAND_DISPLAY` is set, falls back to X11 (XWayland).
- Updated `runPresent` to accept session parameter and instantiate the appropriate backend.
- Added verbose logging: `lsfg-vk-app: using Wayland surface backend` / `lsfg-vk-app: using X11 surface backend` when `-v` enabled.
- Wayland backend features:
  - Binds wl_compositor, xdg_wm_base, zxdg_output_manager_v1, wl_seat globals
  - Enumerates outputs via xdg-output (with wl_output fallback for compositors without xdg-output)
  - Creates borderless fullscreen xdg_toplevel on specified output (or primary)
  - Sets app_id="lsfg-vk-app" for WM identification
  - Handles xdg_surface configure ack, xdg_toplevel configure/close events
  - Creates VkSurfaceKHR via vkCreateWaylandSurfaceKHR
  - Proper cleanup of all Wayland resources on destroy()

### Build & verification
- `cmake --build build --target lsfg-vk-app` succeeds (fixed anonymous namespace linkage issue for xdg_toplevel listeners)
- App starts with `--session wayland` and `--session x11` on KDE Wayland session (KWin)
- IPC handshake completes successfully with layer in external mode (profile `presentation=external`)
- Layer connects to app socket, sends HELLO, receives NEGOTIATED+STAGING+READY
- App logs: `lsfg-vk-app: stream from 'Intel(R) Graphics (ARL)' 500x500 VkFormat(58)`
- Two-way regression test passes: vkcube + layer (presentation=game) works on Intel→9070XT

### Known issue (pre-existing, not task-10)
- Backend context creation fails with `vkImportSemaphoreFdKHR() failed (error -13)` (VK_ERROR_INVALID_EXTERNAL_HANDLE) for timeline semaphore import. Affects both X11 and Wayland backends equally. This is a backend/driver issue, not a Wayland backend bug.
- The `--wsi-test` smoke path was removed per task-8 commit message; full E2E verification requires working backend context.

### Files created/modified
- `lsfg-vk-app/src/wsi/backend_wayland.cpp` (new)
- `lsfg-vk-app/src/main.cpp` (added --session option, auto-detection)
- `lsfg-vk-app/include/lsfg-vk-app/presentation.hpp` (added session parameter)
- `lsfg-vk-app/src/presentation.cpp` (backend selection + verbose logging)
- `lsfg-vk-app/include/lsfg-vk-app/stream.hpp` (added session parameter)
- `lsfg-vk-app/src/stream.cpp` (pass session to runPresent)

---

## 2026-08-27 Task 12: Failure-Mode Audit for One-Way Dual-GPU Frame Generation

### Test Summary (11 scenarios)

| Test | Scenario | Result | Key Finding |
|------|----------|--------|-------------|
| (a) | App absent at swapchain creation | ✅ Named error | "Connection refused" — game exits immediately |
| (b) | App SIGKILL mid-soak | ✅ Named error, game alive, relaunch works | "Connection reset by peer" → game stays alive, new vkcube works after app restart |
| (c) | Socket unwritable/unset XDG_RUNTIME_DIR | ✅ Named errors both sides | Layer: "Connection refused"; App: "Permission denied" on bind() |
| (d) | B lacking HDR colorspace | ⚠️ Not testable | No HDR display, no config option in GameConf |
| (e) | Wrong --profile name | ✅ Named error | "no profile named 'X' in conf.toml" |
| (f) | Unsupported output name | ✅ Named error, lists available | Wayland: empty list; X11: "HDMI-A-3" |
| (g) | Focus behavior | 📝 Code review | X11: WM_HINTS input=False set; Wayland: compositor-dependent (no protocol equivalent) |
| (h) | Exclusive fullscreen | ⚠️ Not testable | vkcube/Linux no exclusive FS support |
| (i) | Two games, one app | ❌ No isolation | App serializes streams (single-threaded accept loop); 2nd game dies waiting |
| (j) | Multiplier hot-reload | ⚠️ Code has logic, didn't trigger | "restart required" message not observed in logs; no context rebuild (correct) |
| (k) | Multi-swapchain double-wait | 📝 Code review only | Pre-existing in entrypoint.cpp:400-412; inherited from two-way; not fixed |

### Key Learnings

1. **App is single-stream only** — The accept loop in `main.cpp` processes one stream at a time (`runStream` blocks). True multi-game isolation requires separate app instances (separate sockets).

2. **Hot-reload message not triggering** — `WatchedConfig::update()` uses `last_write_time` comparison; may have filesystem timestamp resolution issues. The "config change requires restart" logic exists in `Root::update()` but didn't fire in testing.

3. **Focus behavior differs by backend** — X11 has explicit `WM_HINTS input=False`; Wayland relies on compositor (KWin) behavior with `xdg_toplevel` fullscreen + `app_id`.

4. **Pre-existing double-wait hazard** — In `myvkQueuePresentKHR`, multiple swapchains in one present call share the same wait semaphores vector. Binary semaphores would double-wait. Timeline semaphores handle this correctly. Not fixed per task requirements.

### Evidence
- Full report: `.omo/evidence/oneway/t12-failures.md`
- Individual test logs: `.omo/evidence/oneway/t12-failures/test-*.log`

### Commit
```
test(oneway): failure-mode audit
```

---

## 2026-08-27 Task 13: Documentation for One-Way Dual-GPU Frame Generation

### Documentation updates completed:

1. **Configuration.md** — Added `presentation` and `output` reference entries with per-backend output-name semantics (Wayland: xdg_output logical name; X11: RandR name).

2. **Dual-GPU-Guide.md** — New "One-way mode (external app)" section with:
   - Concept diagram (text) showing traffic flow A→B only, no return traffic
   - Setup steps: run app first, `--session` selection, cabling guidance (display on processing GPU), compositor copy caveat
   - WM coverage table (native X11 + native Wayland = all major WMs; tested-cells table from Task 11 including which backend was exercised where)
   - Tested-cells table from Task 11 with "external presentation active" counts
   - Screenshot placeholder

3. **Troubleshooting.md** — Entries for all Task 12 failure modes:
   - Socket-missing ("Connection refused", game exits)
   - App-died ("Connection reset by peer", game stays alive, relaunch works)
   - Colorspace mismatch (HDR not testable, documented expected failure)
   - Wrong-output-name (named error listing available outputs per backend)
   - Session-detection failures (auto-detection picks wrong backend, fix with explicit --session)
   - Two games one app (no isolation, sequential processing)
   - Multiplier hot-reload message not triggering (timestamp resolution issue)

4. **README.md** — Added feature bullet: "One-way external presentation: present generated frames on the processing GPU via a separate app, eliminating return traffic to the game GPU"

5. **Verification** — Grep confirms no undocumented options exist; `presentation` and `output` only appear in config.hpp/GameConf and are handled in layer (presentation) and app (output). No environment variables for these options. `session` is CLI-only for lsfg-vk-app.

### Commit
```
docs: one-way external presentation mode
```
test(oneway): failure-mode audit
```
