# Draft: oneway-dualgpu (one-way "true" dual-GPU presentation)

## Status
- intent: clear
- review_required: TRUE (user: "schedule a momus multi-agent review")
- status: review in flight
## Decisions (addendum)
- D4 (user): FULL window-manager coverage required — app ships NATIVE X11 (xcb)
  AND NATIVE Wayland (xdg-shell) surface backends; XWayland demoted to
  compatibility note. Plan restructured: task 7 = SurfaceBackend iface + X11,
  task 10 = Wayland impl, task 11 gains both-backend session cells, docs task
  covers WM table. Structural check passed (14 tasks + F1/F2).

## Request (user, verbatim intent)
Current dual-GPU branch is two-way (render A → process B → copy back A → present A).
Wanted: Card A renders; Card B receives dma-buf, scales/doubles, PRESENTS to screen.
Fork from current branch; track origin/develop.
Additional directive: deep-research Windows LS dual-GPU internals to validate approach.

## RFC #550 + #159 ground truth (fetched Aug 25)
- PancakeTAS: true impl = "still shares external memory the same way, except it
  doesn't copy the images back and just presents from the VkDevice on that other
  GPU. It requires a bunch of emulation, and I've gotten it to work in vkcube &
  DXVK before, but there's a lot to go."
- PancakeTAS will NOT merge two-way AT ALL ("I just really prefer having a single-way
  approach, it's infinitely better"); user concurred on-thread.
- NVIDIA post-mmap import detail: "on the importing side the valid memory types are
  just 0".
- Issue #159 original body: "we cannot move the Vulkan swapchain to another GPU due
  to layer limitations … transfer all frames back and forth" (his old assumption,
  since evolved).

## Local grounding results (verified today)
- Hooks today: CreateInstance/CreateDevice/DestroyDevice/DestroyInstance/
  CreateSwapchainKHR/QueuePresentKHR/DestroySwapchainKHR (entrypoint.cpp map).
  NOT hooked (one-way must add): vkAcquireNextImageKHR (+AcquireNextImages2?),
  vkGetSwapchainImagesKHR.
- present() flow confirmed two-way: capture blit → scheduleFrames → per-dest
  Acquire+blit+QueuePresent on GAME device → final re-present of original image
  (swapchain.cpp lines 374-615).
- vk::Vulkan has (instance,device,physdev,funcs,devfuncs,graphical,setLoaderData)
  ctor → can wrap a layer-created secondary VkDevice; setLoaderData already captured
  (entrypoint.cpp:199) = fabricated-handle tagging mechanism.
- INSTANCE CONSTRAINT (critical): backend owns its own VkInstance; surfaces are
  instance-bound ⇒ B-side real swapchain requires a VkDevice created inside the
  GAME'S instance on the processing phdev (layer can via instance_info->funcs.
  CreateDevice). Backend stays untouched for generation; only LAYER gains a
  B-side wrapper.
- SwapchainInfo lacks VkSurfaceKHR today — thread createInfo.surface through
  createSwapchainContext for B-side creation.
- Root::probeDevice already enumerates phdev identity (name/ids/pci) with instance
  funcs — extend to UUID match vs backend.selectedDeviceUUID() and surface-support
  query on processing phdev.
- No scaling exists in lsfg-vk FG path (1:1 blits, extent passthrough); user's
  "scale/double" maps to FG-only. SCALING = out of scope unless requested later.

## One-way architecture (emerging, to validate against librarian reports)
A-side emulation: fake VkSwapchainKHR handle (SetDeviceLoaderData-tagged);
N exportable images on A (negotiated modifier); emulate Acquire ring +
GetSwapchainImagesKHR; QueuePresent hook runs capture→schedule as today but NO
game-device present of outputs.
B-side real: mirrored swapchain on game-instance-created B device (same surface,
format/colorSpace/extent/FIFO); loop = acquire B image ← done fd/source fd → blit →
QueuePresentKHR(B queue). Game frame presented on B from imported source too
(frame 0 of cycle).
Backpressure: B FIFO acquire throttles synchronous present() loop; ring exhaustion
throttles emulated acquire.
OOOLS: B OUT_OF_DATE/SUBOPTIMAL → recreate B chain (+ring reset on A); oldSwapchain
path rebuilds both.

## In-flight research → RESULTS (both landed)

### Windows LS internals (librarian, cited report)
- CONFIRMED: LS is EXTERNAL capture-and-represent overlay. NO injection/hooking.
  Captures composed frames (DXGI DD / WGC / GDI), processes on "Preferred GPU",
  presents its OWN borderless window/swapchain on chosen "Output Display".
  Game's swapchain keeps presenting underneath, covered/superseded.
- CONFIRMED: display MUST be cabled to secondary for benefit; else every displayed
  frame copies back (guide: "heavily loading PCIe bandwidth and GPU memory controllers").
- Transfer = per-frame copies (copy-engine saturation evidence: Bus Interface/Copy
  100% while 3D idle). Shared-handle/D3DKMT zero-copy: UNKNOWN/unproven publicly.
- ~3-5ms = guide maintainers' claim for the copy hop; CptTombstone OSLTT chart =
  cited measurement source. GN independent: end-to-end latency dominated by
  SECONDARY GPU compute power (95.6ms GTX1060 vs 45.1ms RTX3060 @4x).
- Traffic asymmetry analysis: one-way forward = real frames only; two-way return =
  EVERY displayed frame incl. generated ⇒ at multiplier m, return ≈ m× forward,
  serialized on same link. Matches maintainer's "atrocious at high multipliers".
- Failure modes: AMD-render+NVIDIA-secondary launch failures; headless render GPU
  crashes (apps need active display target); RTX HDR/DLDSR lost via non-NVIDIA
  output; OpenGL games ignore GPU prefs; Arc needs ≥8 lanes; undervolt instability;
  CPU copy tax 5-15% all-core.
- KEY VALIDATION: Windows achieves one-way precisely because LS owns the output
  surface on B and never touches the game's swapchain.

### Vulkan WSI cross-GPU feasibility (librarian, cited report)
- CONFIRMED (spec + Mesa src/vulkan/wsi): VkSurfaceKHR instance-level, NOT bound to
  a physical device; multiple devices may hold swapchains on the SAME surface;
  Mesa wsi_common keeps per-device swapchain state, shared per-window-system
  connection caches explicitly designed for multi-device use.
- Scanout truth table: monitor-on-B(presenting GPU) ⇒ direct scanout, zero copy;
  monitor-on-A ⇒ compositor/X-server performs cross-GPU import copy at composition
  time (app-invisible, fixed cost).
- Fabricated handles: VkSwapchainKHR non-dispatchable ⇒ no SetDeviceLoaderData
  requirement (that's dispatchable-only); BUT layer owning a fabricated handle must
  intercept EVERY entry point that takes it (acquire/get-images/present/destroy/
  recreate) and never forward it down-chain.
- Precedents: gamescope ships a full WSI layer (src/wsi_layer) fabricating
  swapchains; obs-vkcapture hooks acquire/present around the REAL swapchain +
  blits into own exportable image (closest analog to our capture path).
- Acquire-emulation contract: timeout semantics (UINT64_MAX block, 0⇒NOT_READY),
  unsignaled-semaphore obligations, OUT_OF_DATE sticky/SUBOPTIMAL advisory, resize
  recreation flow.
- Cross-swapchain ordering on one surface: spec SILENT; X11 Present / Wayland
  commit serialize per-window in submission order ⇒ global ordering PROBABLY holds
  (PLAUSIBLE — must be spike-tested if relied upon).
- Surface caps: capabilities largely window-system-derived (same across GPUs);
  format/colorSpace lists differ per DRIVER (HDR colorspaces gated per-driver) ⇒
  explicit cross-device caps comparison required before committing.

## RESOLVED DESIGN SPACE — the fork
Option E (full emulation): fake swapchain on A (plain exportable images, emulated
acquire/get-images), ALL output (real+m-1 generated) presented via real B-swapchain.
Faithful to Windows LS + maintainer description. Highest emulation burden.
Option H (hybrid dual-swapchain): game's REAL swapchain on A untouched (today's
capture path unchanged, real frame re-presented on A exactly as now); ADDED B-side
swapchain receives ONLY the m-1 generated frames (import done fds → blit →
QueuePresentKHR on B). Kills ~(m-1)/m of return traffic; minimal emulation; relies
on cross-swapchain interleaving (spike-testable).
Option C: phased — H as milestone 1 (proves B-WSI + biggest win fast), evolve to E
as milestone 2 on same branch.

## Decisions ledger
- D1 (user): Architecture = X — thin capture-only layer + standalone app.
  User's insight validated: dma-buf fds CAN hand frames cross-process at render
  cadence; only blocker is swapchain images being non-exportable ⇒ minimal
  present-hook layer produces them (obs-vkcapture-proven pattern). No WSI
  emulation anywhere. External app rejected NOT for feasibility but because
  post-composition capture degrades FG pairing; X keeps render-cadence capture
  AND eliminates emulation.
- D2 (user): two-way stays selectable via new config key; default unchanged.
- D3 (user): measurements campaign DEFERRED; approved dual-gpu-measurements plan
  will be revised post-build to cover both modes (two-way numbers = baseline).
- Adopted defaults: branch `feat/dual-gpu-oneway` off `v10-dual-gpu`, tracking
  origin/develop; app binary named `lsfg-vk-app` (house naming: cli/ui/app);
  config key `presentation = "game"(default) | "external"`; socket at
  `$XDG_RUNTIME_DIR/lsfg-vk/app.sock`; reference impl obs-vkcapture; dests stay
  B-local (never exported back); backend library reused unmodified (frozen API).

## Review outcome (dual high-accuracy, completed this session)
- Momus (bg_d7da60a1): APPROVE-WITH-NOTES trajectory. Verified every cited code
  anchor exact (swapchain.cpp :121/:159-204/:374-615/:451-454/:489-611 etc.,
  instance.cpp :288/:318, entrypoint hook map, backend contract). Found:
  F2(b) grep-vs-task-8 contradiction; semaphore double-consume wording ambiguity;
  unbounded slot-wait vs task 9's own acceptance; VK_ERROR code nit; dangling
  256B citation; matrix nit (4←5).
- Oracle (bg_832e6e70): SOUND-WITH-RISKS. Deep findings: (1) CRITICAL queue
  mismatch — vk.queue() is wrapper's first-graphics-family choice ≠ game's
  present queue ⇒ empty-wait forwarding unsound cross-queue; same-queue ordering
  not a spec guarantee either; fix = forwarded present WAITS slot capture
  semaphore (copy transference makes simultaneous fd-export legal). (2)
  staging_count=3 vs backend EXACTLY-2-sources contract mismatch ⇒ ring=2.
  (3) handshake recv under writer mutex needs deadline. (4) hot-reload rebuild
  amplification + UAF window widening ⇒ skip rebuild, restart-required line.
  (5) modifier validation by creation insufficient ⇒ format-feature check.
  (6) dup() beats re-export roundtrip for sources. (7) SCM_RIGHTS count pin.
  (8) focus-steal → games pause on focus loss ⇒ X11 input-hint false + test.
  (9) teardown pending-work drain. (10) visual-QA autonomy proxies. Plus
  inherited multi-swapchain double-wait and exclusive-fullscreen caveats.
- ALL amendments applied to .omo/plans/oneway-dualgpu.md (16 edits); structural
  check re-passed (14 tasks + F1/F2 intact).
- Convergence: round 1 of ≤5; both verdicts ≥ approve-with-notes after fixes;
  no unresolved blockers.

## Approval gate
- APPROVED by user ("Write the full plan")
- plan written: .omo/plans/oneway-dualgpu.md (14 impl + F1/F2 after WM upgrade)
- dual review round 1 applied; status: ready for $start-work
