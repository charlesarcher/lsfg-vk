# Plan: oneway-dualgpu

```yaml
slug: oneway-dualgpu
created: 2026-08-25
intent: clear
review_required: false
approved_by_user: yes ("Write the full plan")
branch_strategy: NEW branch feat/dual-gpu-oneway cut from v10-dual-gpu; track origin/develop
executor_contract: decision-complete; zero interview context assumed
related_plans: .omo/plans/dual-gpu-measurements.md stays UNSTARTED; revise post-build (decision D3)
```

## Goal

Implement ONE-WAY dual-GPU frame generation ("true dual GPU", RFC #550):

```
Game (GPU A, unmodified swapchain)
   └─ thin layer hooks vkQueuePresentKHR:
        blit presented image ──▶ exportable staging image (A-local, created once)
        export sync-fd of the blit ──▶ unix socket (SCM_RIGHTS) ──▶ lsfg-vk-app
Game's own present is FORWARDED unchanged (its window keeps presenting underneath)

lsfg-vk-app (own process, GPU B):
   import staging images once at handshake
   per frame: wait capture sync-fd → backend openContext/scheduleFrames (FG)
   → present ALL output frames from B's REAL WSI swapchain:
       [m-1 generated frames] then [the real frame]
   → ack staging slot back to layer (backpressure)
```

Properties (all binding):
- **Render-cadence capture** — the blit is recorded inside the present hook, pre-
  composition; no compositor/portal involvement ever.
- **Zero return PCIe traffic** — generated frames and the real frame are presented
  FROM B; nothing crosses B→A. Cross-device traffic = exactly 1 frame per cycle.
- **Zero WSI emulation** — the game's swapchain is never replaced or fabricated;
  no hooked acquire/get-images; the app creates an ordinary swapchain on B.
- **Window-manager coverage** — the app owns NATIVE X11 (xcb) AND NATIVE Wayland
  (libwayland-client + xdg-shell) surface backends behind one internal interface;
  frame doubling works under any WM/compositor (KWin, Mutter, wlroots, i3,
  Hyprland, Xfce…), with XWayland merely a compatibility note, never a requirement.
- **Backend library reused unmodified** — the app links `lsfg-vk-backend` and calls
  the existing descriptor-based `openContext`; the backend API stays frozen.
- Two-way dual-GPU remains selectable and is the DEFAULT (`presentation = "game"`).

## Background briefing (worker: you were not interviewed; this is your ground truth)

### Repository / branch / delivery state
- Repo `/home/archerc/code/lsfg-vk`. Resting checkout `feat/dual-gpu` (23-commit
  history + plan record). Do not modify it except reads.
- `v10-dual-gpu` = 10-commit stream (HEAD expected `ed79315`), product tree
  byte-identical to fully-tested state, tracks `PancakeTAS/lsfg-vk:develop`.
  **Record its SHA at start; F2 audits drift against it.**
- Fork remote `fork` → `github.com:charlesarcher/lsfg-vk.git` (user charlesarcher).
  Push policy force-with-lease only. **NO PR may be created; NO GitHub comments/
  issues/discussions may be posted by the agent — drafts only.**
- gh CLI appears logged out; SSH + REST work. Delegation-plane health unknown:
  fire one trivial canary task; on provider failure proceed directly and record
  deviation in `.omo/start-work/ledger.jsonl` (established precedent).
- Build tree `build/` green at 0 warnings; keep it that way (clang-tidy clean).
- FIRST ACTION: `git checkout -b feat/dual-gpu-oneway v10-dual-gpu` then
  `git branch --set-upstream-to=origin/develop` is NOT valid for a new local
  branch — instead configure tracking: `git branch -u origin/develop
  feat/dual-gpu-oneway` (matches user directive "fork from this branch, track
  origin/develop"). All commits land here only.

### Rig ground truth
| vulkaninfo # | Exact deviceName | DRM card |
|---|---|---|
| 0 | `AMD Radeon RX 9060 XT (RADV GFX1200)` | card3 |
| 1 | `AMD Radeon RX 9070 XT (RADV GFX1201)` | card2 |
| 2 | `Intel(R) Graphics (ARL)` | card1 |
- DLL: `/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll`
- Layer test rig: `/tmp/opencode/layer-test/{layer_json.json,liblsfg-vk-layer.so}`;
  env `VK_LAYER_PATH=/tmp/opencode/layer-test
  VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation`
- `vkcube --gpu_number {0=9060XT,1=9070XT,2=Intel} --present_mode fifo`
- Env-mode testing (bypasses conf): `LSFGVK_ENV=1 LSFGVK_GPU=<name>
  LSFGVK_DLL_PATH=<path>`; filter radv noise `grep -E 'Validation Error|VUID'`
- Success log line today: `lsfg-vk: processing on '<uuid>' (game on '<name>')`;
  external mode ADDS its own lines specified in task 4.
- opencode shells redirect `XDG_CONFIG_HOME`; prefer env-mode for tests.

### Code anchors (verified today)
- Hooks live in `lsfg-vk-layer/src/entrypoint.cpp`
  (`myvkCreateSwapchainKHR/myvkQueuePresentKHR/myvkDestroySwapchainKHR`,
  map registered in `vkNegotiateLoaderLayerInterfaceVersion`). NOT hooked and
  NOT needed: AcquireNextImage/GetSwapchainImages (zero-emulation property).
- `Root::createSwapchainContext` (instance.cpp:288) builds `Swapchain(vk, …)`
  contexts; `Root::modifySwapchainCreateInfo` applies
  `context_ModifySwapchainCreateInfo` (swapchain.cpp:121) — usage bumps +
  pacing minImageCount.
- Current two-way present flow: `Swapchain::present` (swapchain.cpp:374-615):
  capture blit → cross-device: fresh capture semaphore + `exportFd` +
  `scheduleFrames(ctx, captureFd)` returning per-frame done fds → per-dest
  acquire/blit/present ON GAME DEVICE → final re-present of original image.
  The external mode REPLACES this class for `presentation="external"` only;
  two-way paths stay byte-identical.
- `vk::Vulkan` wrapper: constructor overload `(VkInstance, VkDevice,
  VkPhysicalDevice, funcs, devfuncs, graphical, setLoaderData)`
  (vulkan.hpp:190) — the app uses the standalone-instance ctor (vulkan.hpp:172);
  the layer keeps its existing single wrapper.
- Backend contract (lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp):
  `Instance(devicePicker, dllPath, allowLowPrecision, enableDmaBufExtensions)`,
  `openContext(sourceDescs, destDescs, exporterDeviceUUID, negotiatedModifier,
  syncFd, w, h, hdr, flow, perf)`, `scheduleFrames(ctx, captureFd) →
  vector<int> doneFds` (SYNC_FD, ownership transferred), `isCrossDevice`.
  Descriptors carry `{fd, allocationSize, rowPitch, modifier, format, extent}`;
  modifiers: `EXCHANGE_MODIFIER_OPAQUE` or `vk::EXCHANGE_MODIFIER_LINEAR`(0) or
  explicit DRM modifier.
- Existing negotiation helper: `vk::exchangeCaps(format)` +
  `vk::negotiateExchangeLayout(gameCaps, processingCaps, format, usageNeeds)`
  (used with a LINEAR proxy today — see swapchain.cpp:159-204 comment). The APP
  holds a REAL processing-device wrapper and therefore negotiates with TRUE caps
  (this retires the proxy limitation for external mode; do NOT touch two-way's
  proxy).
- Reference pattern for the whole IPC shape: obs-vkcapture
  (github.com/nowrep/obs-vkcapture) — real-swapchain usage bump + blit to own
  exportable image + dmabuf fd over unix socket. Study, do not vendor code.

### Research receipts (why this design is sound)
Full citations in `.omo/drafts/oneway-dualgpu.md`. Headlines: Windows LS owns the
output surface on GPU B externally but pays post-composition capture; maintainer
confirms true impl = same external-memory sharing, present from other GPU's
VkDevice, "requires a bunch of emulation" IN-LAYER — our X topology achieves his
end-state with zero emulation; VkSurfaceKHR is instance-level and multi-device-
per-surface is legal (spec + Mesa src/vulkan/wsi); monitor-cabled-to-B ⇒ direct
scanout, monitor-on-A ⇒ compositor does an app-invisible copy.

## Config surface (exact, implement verbatim)

Profile-level TOML keys added to `ls::GameConf` (config.cpp/config.hpp):

```toml
[[profile]]
# ... existing keys ...
presentation = "external"   # default "game"; values: "game" | "external"
output = "DP-1"             # OPTIONAL, external mode only: output name for app
                            # window placement. X11: RandR connector name.
                            # Wayland: wl_output/xdg-output name.
                            # default = primary output of the current session
```

Validation rules (hard errors, Q4 policy — no silent fallbacks anywhere):
- unknown value → named parse error listing allowed values
- `presentation = "external"` without `gpu` → "external presentation requires
  'gpu' to select the processing device"
- layer cannot connect to the socket at context creation → named error naming
  the attempted path (game sees swapchain creation failure — accepted loud
  failure, consistent with missing-DLL behavior)
- `output` set while presentation="game" → ignored silently? NO: named warning
  line on stderr once ("output is only used by presentation='external'")

Socket path (both sides): `${XDG_RUNTIME_DIR}/lsfg-vk/app.sock`. Unset
XDG_RUNTIME_DIR ⇒ named error both sides.

## IPC protocol v1 (implement verbatim)

Transport: `AF_UNIX` `SOCK_STREAM`, all messages length-prefixed (u32 LE) with
leading magic `u32 = 0x4C534647 ('LSFG')` + `u8 msg_type`. fds travel via
SCM_RIGHTS ancillary data only on messages that carry them.

Handshake (per swapchain/stream), layer ⇄ app:
1. C→A `HELLO {u32 proto_version=1, u8[16] game_uuid, char[256] device_name,
   u32 vk_format, u32 width, u32 height}`
   (`staging_count` REMOVED from HELLO — see below: staging ring depth is 2,
   fixed by the backend's exactly-two-sources contract)
2. A→C `NEGOTIATED {u64 modifier, u32 row_pitch, u64 allocation_size}` OR
   `ERROR {u32 code, char[] message}` then close. Handshake recv MUST carry a
   2-second deadline (poll-based); expiry ⇒ named error aborting swapchain
   creation — never an indefinite hang inside `createSwapchainContext`'s
   writer mutex.
3. C→A `STAGING {fd via SCM_RIGHTS}` — ONE message PER fd (2 messages total;
   pins the 1-fd-per-message rule of the ipc module; kernel dups on receive,
   layer closes its originals once the stream reaches READY)
4. A→C `READY {}` — stream live

Steady state:
- C→A `FRAME {u32 staging_idx, fd sync_fd}` (capture-blit completion payload,
  exported from the capture semaphore right after enqueue per the proven
  copy-transference pattern, swapchain.cpp:456-460)
- A→C `RELEASE {u32 staging_idx}` — sent once the done-fds for the generated
  cycle THAT CONSUMED this slot have been delivered to the app's present loop
  (i.e., the backend will never read this slot again until it is recaptured)

Ring depth `staging_count = 2` — NOT a free choice: the frozen backend accepts
EXACTLY 2 source descriptors alternated between (lsfgvk.hpp:149 "Exactly 2"),
so the staging ring maps 1:1 onto the backend's curr/next alternation exactly
like today's `sourceImages.at(fidx % 2)` (swapchain.cpp:379). Backpressure
bound ⇒ ≤2 frames in flight.

Teardown: either side closes socket ⇒ layer tears down CaptureContext quietly
(next present re-attempts connect and surfaces named error), app drops stream
and destroys its swapchain cleanly. App killed mid-stream MUST NOT crash the
game (send()/recv() EPIPE handling → context marked dead → present hook throws
named error; entrypoint's existing catch converts to VK_ERROR_* result).

## Methodology constraints (binding)

1. Two-way behavior byte-identical when `presentation` absent or "game": no
   changes to `Swapchain` FG paths, `context_ModifySwapchainCreateInfo`
   semantics for the legacy path, or backend. Verified in F1 against recorded
   spot-cell expectations.
2. All new stderr log lines prefixed `lsfg-vk:` and listed in task 4/7 exactly;
   matrix runner greppable.
3. clang-tidy clean, GPL-3.0 headers, house style (imperative docs, named
   errors, RAII fd janitors like ExportFdJanitor/FdJanitor patterns).
4. Validation layers must be clean (`grep VUID`) on every E2E run.
5. No fabricated Vulkan handles anywhere; the only new WSI object is the app's
   genuine swapchain.
6. Commits: conventional, teaching-style messages consistent with v10 stream.

## Todos

- [x] 1. Display-topology discovery + session matrix plan
    - Recommended task executor category: quick
    - Task: Record to `.omo/notepads/oneway-dualgpu/rig-display.md`:
      `$XDG_SESSION_TYPE`, `xrandr --listactivemonitors` (or
      `kscreen-doctor -o` / `wayland-info` under Wayland), which DRM card drives
      the active monitor (sysfs `/sys/class/drm/card*-*/status` = enabled +
      mapper between cardN and vulkaninfo devices via
      `VK_LOADER_DEBUG=devices` or drm node ↔ pci matching), and the chosen
      E2E pairing implied (display-card must be PRESENTING side; pick game GPU
      among the other two). THEN lay out the WM-coverage test matrix: which of
      {app-X11-backend, app-Wayland-backend} cells are natively testable on
      this rig today (native session type decides Wayland-app cells; X-app
      cells always available via native X or XWayland), and record how the
      OTHER backend will be exercised (e.g. nested `weston --xwayland`? cage?
      a second tty? document the concrete method chosen — if a second-backend
      cell is physically impossible on this rig, mark it `[~]` with reason and
      cover it in docs as maintainer/community-verifiable).
    - Acceptance: doc names display card ↔ deviceName mapping, chosen primary
      pairs for task 10, and an explicit per-backend testability verdict with
      concrete invocation commands.
    - QA: cross-check mapping via `vkcube --gpu_number n` appearing on the
      expected monitor (visual) or `drm_info` connector↔driver match.
    - Commit: `chore(measure): record rig display topology for external mode`

- [x] 2. Config: `presentation` and `output` profile keys
    - Recommended task executor category: quick
    - References: `lsfg-vk-common/src/configuration/config.cpp` (`parseGameConf`
      around :122), `include/lsfg-vk-common/configuration/config.hpp`
      (`GameConf`), docs anchor points in `docs/Configuration.md`.
    - Task: Add enum `ls::Presentation { Game, External }` parsed from string
      key with validation rules above; add `std::optional<std::string> output`.
      Thread through `GameConf` consumers. Default `Game` when key absent.
      Implement the misplaced-`output` warning line. Update `lsfg-vk-cli
      validate` output to show the new fields (it prints profiles already —
      extend minimally).
    - Acceptance: validate CLI parses sample confs with all three states
      (absent/game/external±gpu/output-misplaced) producing exactly the errors/
      warnings specified above.
    - QA: unit-style checks via `lsfg-vk-cli validate -c <fixture>`; commit
      fixtures under `lsfg-vk-cli/tests/fixtures/presentation-*.toml` (or repo-
      consistent location if a tests dir exists — else `/tmp` evidence logged).
      Evidence: command transcripts appended to
      `.omo/evidence/oneway/t02-config.log`.
    - Commit: `feat(common): presentation and output profile options`

- [x] 3. IPC module (common): socket, framing, handshake, streams
    - Recommended task executor category: deep
    - References: new files `lsfg-vk-common/src/ipc/socket.cpp` +
      `include/lsfg-vk-common/ipc/socket.hpp` (+ protocol header
      `ipc/protocol.hpp`); RAII style of `helpers/pointers.hpp`; fd-janitor
      patterns from swapchain.cpp:76-118; obs-vkcapture's socket framing as
      reference only.
    - Task: Implement `ipc::Listener` (app side: bind/accept, one stream per
      connection) and `ipc::Connection` (layer side: connect/reconnect) over
      `$XDG_RUNTIME_DIR/lsfg-vk/app.sock`; message read/write helpers for the
      protocol structs in the exact wire format specified above incl.
      SCM_RIGHTS send/recv of 1 fd per message max; blocking semantics with
      `MSG_NOSIGNAL`; typed errors carrying errno. No threading: both sides
      integrate into existing loops (layer = present hook; app = poll loop
      combining socket + swapchain present — task 7).
    - Acceptance: a throwaway test driver (not shipped, e.g. under `/tmp`)
      exercises handshake + 1000 frame/release round-trips with zero fd leaks
      (`ls /proc/self/fd` count stable) and correct EPIPE/agg behavior on
      peer kill.
    - QA: run driver; `strace -f -e trace=sendmsg,recvmsg` excerpt showing
      SCM_RIGHTS; leak check before/after. Evidence:
      `.omo/evidence/oneway/t03-ipc.log`.
    - Commit: `feat(common): ipc transport for external presentation`

- [x] 4. Layer capture-mode: CaptureContext + forward-present passthrough
    - Recommended task executor category: deep
    - References: `lsfg-vk-layer/src/{entrypoint,instance,swapchain}.cpp`,
      headers in `lsfg-vk-layer/include/lsfg-vk-layer/`; staging image
      creation mirrors `vk::Image` exportable construction in
      swapchain.cpp:209-234 but with `TRANSFER_DST | TRANSFER_SRC? (no —
      TRANSFER_DST only… plus nothing else)` usages and negotiated layout;
      capture blit mirrors swapchain.cpp:401-430 barrier style.
    - Task: When `profile.presentation == External`:
      (a) `context_ModifySwapchainCreateInfo` gains the external branch — add
      ONLY `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (no minImageCount bump, no
      present-mode rewrite);
      (b) `createSwapchainContext` constructs `CaptureContext` instead of
      `Swapchain`: connects IPC (handshake per spec above, with the 2 s
      deadline; sending TRUE game caps so the APP negotiates with its real
      device), creates 2 exportable staging images on A at negotiated layout,
      exports + hands off, allocates capture `vk::CommandBuffer` + per-slot
      `vk::Semaphore` (sync-fd exportable) ring;
      (c) present hook path: record blit
      `info.images[imageIdx] → staging[slot]` waiting on the GAME's present
      wait-semaphores (consume them exactly like swapchain.cpp:451-454),
      signal that slot's capture semaphore, submit via `vk.queue()` as usual;
      export the slot semaphore's sync-fd immediately after enqueue;
      send FRAME; THEN forward the original present DOWN-CHAIN WITH THE WAIT-
      SEMAPHORE LIST REPLACED BY THE SLOT'S CAPTURE SEMAPHORE — the game's
      semaphores were consumed by the blit submit, so re-waiting them in the
      present would be an invalid double-wait; waiting the capture semaphore
      instead is spec-sound REGARDLESS of which queue the game presented on
      (`vk.queue()` is the wrapper's first-graphics-family choice, NOT
      necessarily the game's queue — cross-queue ordering requires a
      semaphore, and queue-discipline arguments are not spec guarantees).
      Copy transference makes simultaneous fd-export + present-wait of one
      binary semaphore legal. Cost: present waits ~blit duration (sub-ms).
      Forward everything else unmodified (swapchain, imageIdx, pNext chain
      incl. present-mode info); propagate results including SUBOPTIMAL
      passthrough. Precedent: obs-vkcapture strips/replaces waits identically;
      NOTE the pre-existing two-way hazard being inherited, not introduced:
      multi-swapchain presents pass ALL wait semaphores to EACH context
      (entrypoint.cpp:400-412) — record in task 12, do not fix here;
      (d) RELEASE handling frees slots (socket readable during present hook —
      drain non-blocking before selecting a slot);
      (e) log lines (exact):
      `lsfg-vk: external presentation active (game on '<name>', app on socket)`
      at context creation; `lsfg-vk: external stream error: <msg>` on failures.
      Two-way/Same-device branches untouched (verify by diff scope).
      TEARDOWN SAFETY: `CaptureContext` destructor drains in-flight capture
      work with a bounded fence wait (reuse the 150 ms renderFence pattern,
      swapchain.cpp:436-438) before destroying slot semaphores/images —
      destroying pending-work semaphores trips validation (VUID-05149 family;
      see the retiredCaptureSignal comment swapchain.cpp:441-446).
      HOT-RELOAD: external-mode contexts are NOT rebuilt by `Root::update()`'s
      rebuild loop — changing profile options while external is active logs
      the honest restart-required line once (precedent instance.cpp:167-171).
      Rationale: rebuild = full IPC handshake + staging re-export, and a
      rebuild under a concurrently blocked present widens an existing UAF
      window (escaped reference from getSwapchainContext's short-lived shared
      lock) from frame-scale to seconds.
    - Acceptance: with app running (task 5 skeleton answering NEGOTIATED with
      LINEAR + READY, dropping frames), vkcube runs indefinitely with game
      window visible AND socket traffic visible (`ss -xp | grep app.sock`);
      with app ABSENT, swapchain creation fails with the named socket error;
      game survives app SIGKILL mid-stream (next present → named error →
      entrypoint catch → generic-exception path returns VK_ERROR_UNKNOWN,
      matching the existing catch-all at entrypoint.cpp:420-423; document in
      code comment).
    - QA: validation-clean run; fd-leak check across 500 frames
      (`ls /proc/self/fd | wc -l` stable); two-way regression quick check
      (one debug-tool run still passes). Evidence:
      `.omo/evidence/oneway/t04-layer.log`.
    - Commits: `feat(layer): external presentation capture context` +
      `feat(layer): forward presents in external mode`

- [x] 5. App skeleton: instance, device, listener, stream registry
    - Recommended task executor category: deep
    - References: new top-level dir `lsfg-vk-app/` mirroring `lsfg-vk-cli/`
      structure (CMake target `lsfg-vk-app`, `src/main.cpp`,
      `src/{stream,wsi,output}.cpp`); instance creation via
      `vk::Vulkan(appName ctor, vulkan.hpp:172)`; DevicePicker equivalent
      reading SAME conf.toml (`findConfigurationFile`, `findProfile` — reuse
      common config; the app identifies "which profile am I" by explicit
      `--profile <name>` arg REQUIRED in v1, error if ambiguous); MUST
      `setenv("DISABLE_LSFGVK","1")` around anything that could nest-load the
      layer (mirror instance.cpp:318).
    - Task: Binary `lsfg-vk-app`: args `--profile <name>` [--output <name>]
      [-v]; builds instance/device on the configured processing GPU with the
      full exchange extension set + swapchain ext + WSI instance exts
      (VK_KHR_xcb_surface family); binds listener; accepts streams; maintains
      `map<stream_id, StreamState>`; clean shutdown on SIGINT (drain, destroy
      swapchains, close). Log lines (exact): startup banner
      `lsfg-vk-app: listening on <path> (processing on '<deviceName>')`,
      per-stream `lsfg-vk-app: stream from '<device_name>' <WxH fmt>`.
    - Acceptance: binary launches, prints banner, accepts and completes
      handshakes with task-4 layer (NEGOTIATED LINEAR + READY), tolerates
      stream churn (repeated vkcube restarts) without leaks.
    - QA: 10 rapid vkcube restart cycles; `ss -xp` shows single listener;
      valgrind-free check via fd/mem counters stable. Evidence:
      `.omo/evidence/oneway/t05-app-skeleton.log`.
    - Commit: `feat(app): lsfg-vk-app skeleton with ipc streams`

- [x] 6. App transport: true-caps negotiation, staging import, backend wiring
    - Recommended task executor category: deep
    - References: `vk::exchangeCaps` / `vk::negotiateExchangeLayout`
      (lsfg-vk-common/vulkan/exchange.*), backend `openContext` descriptor
      contract (lsfgvk.hpp:100-174), staging import mirrors backend-side
      import code paths; `usageNeeds` set copied from swapchain.cpp:179-183.
    - Task: On HELLO: compute app-device `exchangeCaps(format)`; negotiate
      modifier from the app's TRUE caps requiring the usageNeeds set
      (swapchain.cpp:179-183); reply NEGOTIATED. LAYER-SIDE VALIDATION before
      creating staging images (creation success alone proves nothing):
      query `VkDrmFormatModifierPropertiesListEXT` for the returned modifier
      on the GAME device and verify the feature flags cover usageNeeds —
      missing flags ⇒ named handshake error naming modifier + missing bits.
      Import the two socket-received staging fds as B-local `vk::Image`s
      (usages TRANSFER_SRC | SAMPLED): `dup()` each received fd — one copy
      for image import, one for the descriptor handed to openContext (the fd
      is just a dmabuf handle; NO device re-export roundtrip needed for
      sources). Create 2 B-local destination images (TRANSFER_SRC) sized
      identically; THESE need the self-export roundtrip to become descriptor
      fds (they are created natively, not received), wrapped with negotiated
      modifier + real pitch/size. Call `backend.openContext` with EXACTLY 2
      source descriptors (the imported stagings, alternating fidx%2) + 2 dest
      descriptors, `game_uuid_from_HELLO`, modifier, syncFd −1 (ignored in
      cross-device mode per lsfgvk.hpp:144), w, h, hdr=(format>57),
      1.0F/flow_scale from profile, perf from profile. Verify
      `isCrossDevice == true` and log compatibly.
    - Acceptance: end-to-end with layer: app logs context creation; backend
      init log appears ONCE per stream; no VUIDs.
    - QA: dump descriptor table at DEBUG log level; verify negotiated pitch
      against each staging import's actual rowPitch (mismatch ⇒ named error;
      rowPitch discipline per lsfgvk.hpp:117-129 ingestion contract).
      Evidence: `.omo/evidence/oneway/t06-transport.log`.
    - Commit: `feat(app): negotiate layout and wire backend contexts`

- [x] 7. App WSI abstraction + X11 (xcb) backend
    - Recommended task executor category: deep
    - References: new files `lsfg-vk-app/src/wsi/{surface_backend.hpp,backend_x11.cpp}`;
      Vulkan WSI: `vkCreateXcbSurfaceKHR`; xcb connect to `$DISPLAY` (XWayland
      fallback is a compatibility note, not the target); RandR for output
      enumeration/matching (`output` config key = RandR connector name);
      fullscreen-borderless = override-redirect-free borderless positioned on
      the chosen output's geometry with WM_HINTS input = False (the window
      must NEVER take keyboard focus — games pause on focus loss; verify the
      game retains focus with the overlay visible), NO focus-steal hacks —
      compositor-policy fallback documented if placement fights WM.
    - Task: Define `SurfaceBackend` interface consumed by task 8:
      `connect(session)`, `outputs() → list{name, geometry}`, `createWindow(
      output_name, extent, colorspace) → native_handle`, `createSurface(vk
      instance, native_handle) → VkSurfaceKHR`, `surfaceCaps()`,
      `processEvents(timeout)` (resize/close), `destroy()` — everything task 8
      needs, nothing more. Implement the X11/xcb side fully. Build deps added
      to CMake: `wayland-client`, `wayland-scanner` codegen for xdg-shell +
      xdg-output REQUIRED from this commit onward (task 13 fills the second
      implementation; interface ships complete now).
    - Acceptance: standalone smoke path (temporary main flag `--wsi-test`)
      opens borderless window on named/primary output under X, creates
      VkSurfaceKHR + swapchain, clears frames until closed; output-name match
      and mismatch error paths behave per spec.
    - QA: validation-clean; screenshot evidence `.omo/evidence/oneway/t07-x11/`.
    - Commit: `feat(app): wsi surface backend interface and x11 implementation`

- [x] 8. App presentation choreography (backend-agnostic over SurfaceBackend)
    - Recommended task executor category: deep
    - References: consumes `SurfaceBackend` (task 7; Wayland impl activates when
      task 10 lands without changes here); blit/barrier patterns from
      swapchain.cpp:508-535; importSyncFd pattern swapchain.cpp:60-73.
    - Task: Full present pipeline incl. HDR colorspace mirroring
      (`imageColorSpace` from HELLO passed through; if B lacks it → named
      error listing supported spaces), swapchain extent CLAMPED to surface caps
      (mismatch ⇒ scale-blit, never recreate-loop), FIFO present mode ALWAYS
      (pacing anchor), resize/OOOLS recovery via SurfaceBackend events, stream
      teardown on socket close (destroy swapchain/window/context), per-frame:
      `AcquireNextImageKHR(UINT64_MAX)` ← wait doneFd/sourceFd semaphores →
      blit imported staging or dest image → swapchain image → QueuePresentKHR;
      ORDER PER CYCLE: m-1 generated frames THEN real frame (replicates
      swapchain.cpp:489-611 sequencing), `-v` log hook printing frame kinds;
      present loop multiplexes socket POLLIN (RELEASE draining) with frame
      readiness.
    - Acceptance: vkcube + app (X11 backend): interpolated animation visible on
      B's monitor/output at multiplier cadence; `-v` log shows per-cycle
      `[gen×(m-1), real]` ordering; 30-minute soak without leak/error.
      Machine-checkable primary criteria: `-v` ordering lines present at m×
      game cadence; fdinfo shows B-side generation volume; screenshots are
      SUPPLEMENTARY evidence (agents capture, not judge).
    - QA: visual confirmation on rig + `vkcube` fps counter sanity; soak log
      evidence `.omo/evidence/oneway/t08-presentation.log`; screenshot for
      the guide (task 13 embeds path).
    - Commits: `feat(app): wsi output window and swapchain` +
      `feat(app): frame presentation choreography`

- [x] 9. Pacing, backpressure, stall policy
    - Recommended task executor category: unspecified-high
    - References: ring semantics from task 3/4; FIFO anchoring in task 8.
    - Task: Define + implement stall policy constants: capture-submit blocks
      while 0 free slots (ring depth 2 ⇒ ≤2 frames in flight) with a BOUNDED
      slot-acquisition poll — 500 ms deadline, expiry ⇒ named stream error
      (the 250 ms FRAME-send timeout alone does NOT cover this path: a SIGSTOP-
     ped app keeps the socket open and simply stops sending RELEASEs);
      handshake deadline 2 s (protocol section); app-side acquire timeout
      UINT64_MAX (FIFO paces everything); layer-side send timeout 250 ms on
      FRAME; app idle >5 s with no FRAME ⇒ keep window alive
      presenting last frame (media-player semantics), log once. Add
      `-v`-gated per-second stats line: `lsfg-vk-app: <fps> fps, slots
      <free>/<total>, queued <n>`.
    - Acceptance: kill -STOP the app for 3 s under load ⇒ game stalls ≤ ring
      bound then ERRORS NAMED (never hangs forever); resume ⇒ stream recovers
      WITHOUT context rebuild; stats line accurate vs manual count.
    - QA: scripted STOP/CONT + timing transcript. Evidence:
      `.omo/evidence/oneway/t09-pacing.log`.
    - Commit: `feat(app,layer): bounded backpressure and stall policy`

- [x] 10. App Wayland backend (xdg-shell) — full WM coverage
    - Recommended task executor category: deep
    - References: implements `SurfaceBackend` from task 7;
      libwayland-client + wayland-scanner-generated protocols: wl_compositor,
      xdg_wm_base (xdg-shell stable), zxdg_output_manager_v1 (xdg-output unstable v4
      acceptable — pin version in code comment) for output naming matching the
      `output` config key; wl_registry/wl_output for fallback naming when
      xdg-output unsupported; fullscreen-borderless via xdg_toplevel.set_
      fullscreen(wl_output) when output specified else plain toplevel on
      primary; resize via xdg_toplevel.configure ack; close via
      xdg_toplevel.close. Registry/error paths mirror house RAII.
    - Task: Implement the Wayland SurfaceBackend behind the SAME interface;
      wire `--session auto` detection (WAYLAND_DISPLAY vs DISPLAY env, explicit
      `--session {x11,wayland}` override); ensure swapchain creation uses
      VK_KHR_wayland_surface with matching colorspace support check as X11
      side. NO portals, NO layer-shell — plain toplevel only.
    - Acceptance: identical smoke path as task 8 under a NATIVE Wayland session
      (KWin/Mutter/wlroots — whatever the rig runs): window appears on chosen
      output, swapchain cycles frames, resize/OOOLS handled; output-name match/
      error parity with X11; `-v` logs name the backend in use.
    - QA: validation-clean run per session type reachable on rig (task 1's
      matrix); evidence `.omo/evidence/oneway/t10-wayland/`.
    - Commit: `feat(app): wayland wsi backend via xdg-shell`

- [x] 11. E2E verification matrix + traffic-shape proof
    - Recommended task executor category: unspecified-high
    - References: task 1's pairing doc + session matrix; prior conventions
      (.omo/plans/issue-159-dual-gpu.md); fdinfo technique from Dual-GPU-Guide.
    - Task: Cells (each: launch app + layered vkcube, 60 s soak, artifacts =
      stderr logs both sides + fdinfo snapshots + screenshot):
      (a) display-GPU-as-B pairing (from task 1; expect direct scanout),
      (b) Intel-game → 9070XT-B, (c) 9060XT-game → 9070XT-B, (d) m∈{2,3}
      SDR; HDR cell where colorspace supported (record honestly if skipped).
      SESSION COVERAGE per task 1's verdict: run the primary cell under BOTH
      reachable backends (app X11-native and app Wayland-native); any
      unreachable backend cell gets an explicit `[~]` + docs note per task 1's
      method. TRAFFIC-SHAPE PROOF per cell: fdinfo engine deltas over the soak
      ⇒ A shows ~1 blit/frame copy volume, ZERO large outbound copy beyond
      capture; B shows generation+present; compare against one TWO-WAY control
      cell (same GPUs via existing mode) demonstrating the eliminated return
      leg numerically in the report. Plus OOOLS drill: resize window during
      soak ⇒ recovery (both backends where reachable).
    - Acceptance: all cells validation-clean with complete artifacts;
      report `.omo/evidence/oneway/t11-matrix/report.md` incl. control-vs-
      external return-traffic table + backend coverage table.
    - QA: numbers recomputed twice; screenshots embedded. Evidence: the
      report + raw dirs.
    - Commit: `data(oneway): e2e matrix and traffic-shape proof`

- [x] 12. Failure-mode audit
    - Recommended task executor category: unspecified-high
    - Task: Drill + document each: (a) app absent at swapchain creation (named
      error, game's own behavior after — record), (b) app SIGKILL mid-soak
      (game gets named error on next present; verify process alive; relaunch
      app + new vkcube works), (c) socket path unwritable/unset XDG_RUNTIME_DIR,
      (d) B lacking HDR colorspace requested, (e) wrong `--profile` name,
      (f) unsupported `output` name per backend (RandR miss / wl_output miss),
      (g) focus behavior: confirm the GAME keeps keyboard focus while the app
      window covers it on both backends (X11 input-hint path; Wayland
      compositor policy — document actual observed behavior honestly),
      (h) game running EXCLUSIVE-fullscreen where reachable: app window cannot
      cover it — record the honest outcome (FG ineffective, game unharmed),
      (i) two games simultaneously (two streams, one app) — verify isolation,
      (j) multiplier change hot-reload while external active (expect honest
      restart-required line per task 4's hot-reload policy, and NO context
      rebuild), (k) pre-existing hazard documentation only: multi-swapchain
      presents sharing wait semaphores across contexts
      (entrypoint.cpp:400-412 double-wait when >1 swapchain in one present)
      — inherited from two-way, record observed behavior, do not fix.
      Write findings to `.omo/evidence/oneway/t12-failures.md`.
    - Acceptance: every scenario has recorded outcome + no crashes/hangs
      beyond designed behavior.
    - QA: transcripts per scenario. Evidence: the md + logs dir.
    - Commit: `test(oneway): failure-mode audit`

- [x] 13. Documentation
    - Recommended task executor category: writing
    - References: `docs/Configuration.md`, `docs/Dual-GPU-Guide.md`,
      `docs/Troubleshooting.md`; house style (imperative, honest caveats).
    - Task: Configuration.md: `presentation` + `output` reference entries
      (per-backend output-name semantics). Guide: new "One-way mode (external
      app)" section — concept diagram (text), setup steps (run app first,
      `--session` selection, cabling guidance display-on-B vs compositor-copy
      caveat), WM coverage table (native X11 + native Wayland = all major WMs;
      tested-cells table from task 11 incl. which backend was exercised where),
      screenshot(s). Troubleshooting: entries for socket-missing, app-died,
      colorspace mismatch, wrong-output-name, session-detection failures.
      README feature bullet one-liner.
    - Acceptance: every claim traces to t11/t12 artifacts; grep finds no
      undocumented option.
    - QA: link-check + style pass. Evidence: diff review in
      `.omo/evidence/oneway/t13-docs.diff` header note.
    - Commit: `docs: one-way external presentation mode`

- [x] 14. RFC #550 reply draft
    - Recommended task executor category: writing
    - References: `.omo/drafts/oneway-dualgpu.md` research receipts;
      t11 report; maintainer's exact words quoted in draft file.
    - Task: `measurements/rfc550-oneway-reply-draft.md` (≤500 words + tables):
      announces X architecture built on his confirmed transport ("same external
      memory sharing"), presents traffic-shape numbers proving return-leg
      elimination, answers his earlier question about our approach directly,
      asks whether the frozen backend descriptor transport + app topology
      interest upstream, offers rig data. NO speculation about his internals;
      NO commitments beyond offering data/access; explicitly notes two-way
      remains available locally but is not the proposed contribution.
    - Acceptance: word count ≤500 body; zero mismatched numbers vs t11;
      tone matches user's prior candid register.
    - QA: number audit vs report; spellcheck. Evidence: audit appended in
      file header comment.
    - Commit: `docs(measure): rfc 550 one-way reply draft`

## Final verification wave

- [x] F1. Regression + acceptance gate
    - Recommended task executor category: unspecified-high
    - Task: (a) TWO-WAY REGRESSION: default mode spot cells — debug-tool pair
      Intel→9070XT (wait-count + validation-clean) and one live vkcube
      cross-device cell — produce byte-pattern-identical log lines to
      pre-branch expectations (record baseline FIRST from v10-dual-gpu build
      before switching); same-device legacy path one cell. (b) FULL ACCEPTANCE:
      rerun best cell (display-GPU pairing, m2, primary backend) fresh: all
      artifacts regenerate, numbers within ±15% of t11. (c) Branch hygiene:
      `git log v10-dual-gpu..
      feat/dual-gpu-oneway` contains only this plan's commits; diff touches
      only {lsfg-vk-layer, lsfg-vk-app/, lsfg-vk-common(ipc/config additions),
      docs, .omo}; `git diff v10-dual-gpu..feat/dual-gpu-oneway --
      lsfg-vk-backend` is EMPTY (backend frozen proof). Write verdict
      `.omo/evidence/oneway/_final-f1.md`.
- [x] F2. Traceability + constraint audit
    - Recommended task executor category: unspecified-high
    - Task: Audit (a) every doc/RFC number resolves to committed artifact;
      (b) zero WSI-emulation code: grep branch diff for
      `AcquireNextImage|GetSwapchainImages|SetDeviceLoaderData` hits ONLY
      pre-existing lines (obs: none exist today ⇒ zero tolerance); (c)
      `presentation` default path untouched: config fixtures from t02 confirm
      absent-key == old parse; (d) branch SHAs: `v10-dual-gpu` unchanged from
      start-of-plan value, `feat/dual-gpu` unchanged, fork remote unchanged;
      (e) no PR exists (`gh api repos/charlesarcher/lsfg-vk/pulls?state=all`
      count unchanged); (f) zero-emulation grep SCOPED CORRECTLY: run the
      `AcquireNextImage|GetSwapchainImages|SetDeviceLoaderData` diff-grep
      against `lsfg-vk-layer/**` ONLY — the lsfg-vk-app legitimately calls
      AcquireNextImageKHR on its OWN genuine swapchain (task 8), which is WSI
      USE not emulation; the invariant under audit is that the LAYER grows no
      such calls and no fabricated handles anywhere. Verdict to
      `.omo/evidence/oneway/_final-f2.md`.

## Dependency matrix

```
1 ─┬─▶ 2 ─┐
2 ─┤      ├─▶ 4 ──(QA needs 5's skeleton)──┐
3 ─┴─▶ 5 ─▶ 6 ─▶ 7 ─▶ 8 ─▶ 9 ─▶ 10 ─▶ 11 ─▶ 12 ─▶ 13 ─▶ 14 ─▶ F1 ─▶ F2
              (7 feeds iface to 8; 10 needs only 7; both before 11)
```

## Risks & fallbacks

- Native Wayland backend depends on compositor behavior variance (xdg-output
  naming, fullscreen hints): task 1 records the rig's concrete WM; parity cells
  that cannot run on this rig are marked `[~]` and flagged in docs as community-
  verifiable — never silently claimed as tested.
- XWayland remains a compatibility note only; native backends are the contract.
- Self-export/import roundtrip for B-local dests costs trivial copies; future
  backend API cleanup candidate — do NOT unfreeze backend now.
- RADV/ANV sync-fd import quirks already survived in two-way (doneWait pattern)
  — reuse the exact proven primitives; do not invent new sync mechanisms.
- If borderless placement fights the WM (focus steal policies on either
  backend), fall back to documented manual placement instructions rather than
  override-focus hacks.
- Delegation plane down ⇒ direct execution + ledger deviation (precedent).
