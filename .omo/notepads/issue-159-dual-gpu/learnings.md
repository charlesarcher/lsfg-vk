# Learnings — issue-159-dual-gpu

Conventions, patterns, and successful approaches discovered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## task-1 (2026-08-23)
- vk::Vulkan::deviceUUID()/driverUUID() work on BOTH ctor paths: they only touch instance_funcs + phys_dev, both set by standalone and wrapping ctors. No per-ctor special-casing needed.
- VkPhysicalDeviceProperties2 chain (Properties2 -> IDProperties) is the pattern; note GetPhysicalDeviceProperties2 takes NON-const ptr (const triggers -fpermissive error).
- RADV deviceUUID is not a real 16-byte GUID: bytes 4..7 encode PCI ids (9070 XT=0x04, 9060 XT=0x87 here); driverUUID is the ASCII string "AMD-MESA-DRV" and is IDENTICAL across both AMD cards (same Mesa install) - do not use driverUUID to distinguish devices.
- EMPIRICAL CORRECTION vs draft ledger: SYNC_FD semaphore features=0x3 AND SYNC_FD fence features=0x3 (EXPORTABLE|IMPORTABLE) on both RX 9070 XT and RX 9060 XT. Ledger claims (sem=0x657, fence=0x0) did NOT reproduce; 0x657 isn't even a valid ExternalSemaphoreFeatures mask. supportsSyncFdFenceExportImport() correctly returns TRUE on this rig.
- exportFromImportedHandleTypes measured: sem=0x11 (OPAQUE_FD|SYNC_FD), fence=0x9 (OPAQUE_FD|SYNC_FD).
- New instance funcs loaded unconditionally via ipa<> in initVulkanInstanceFuncs; both are core-1.1 entry points so no graphical-conditional needed (unlike SurfaceCapabilitiesKHR).

## Task 6 (Configuration.md rewrite) — 2026-08-23

- Doc voice pattern confirmed: option bullets are "**Display Name / `key`**: sentences." with no (Default:) marker on gpu; kept that shape. Multi-block bullet content (table + follow-up paragraph) works fine indented 2 spaces under a `-` item in CommonMark.
- Terminology locked for todo 16 consistency: "processing GPU" (the configured frame-gen device), "game's own GPU" / "render GPU" (where the app renders+presents), "dual-GPU mode". Reuse these exact strings.
- LSFGVK_GPU env-var entry never carried the same-GPU restriction, so it was left byte-identical (diff minimality). Todo 16 should NOT "fix" it either.
- Cost anchors as written: ~1.8 GB/s (1440p SDR m=2), ~14 GB/s (1440p@240 m=4) needing x8-class, ~3-5 ms added latency. Note: ASCII hyphen used in "3-5 ms" range per style rules (no en dashes).
- Grep gate `grep -ri "dual gpu is not supported\|must be the \*\*same gpu" docs/Configuration.md` is empty (exit=1); evidence at .omo/evidence/task-6-issue-159-dual-gpu.{md,grepout}.

## task-5 (CLI --render-gpu + matrix runner skeleton)
- `optional<string>::value_or(optional<string>)` does NOT compile (value_or needs a string-convertible arg); the semantic equivalent used in debug.cpp's exporter selector is `const std::optional<std::string> render_gpu{opts.render_gpu ? opts.render_gpu : opts.gpu};`.
- Rig Vulkan deviceNames carry RADV suffixes and selectors use EXACT `==` matching: "AMD Radeon RX 9070 XT (RADV GFX1201)", "AMD Radeon RX 9060 XT (RADV GFX1200)", "Intel(R) Graphics (ARL)". The plan's shorthand names ("AMD Radeon RX 9070 XT") FAIL device selection. run-matrix.sh defaults carry the full suffixed names.
- Baseline binary lives at build/lsfg-vk-cli/lsfg-vk-cli (build/lsfg-vk-cli is a directory).
- debug tool needs no display server; deterministic DDS test frames = 128-byte prefix ('DDS ' + 124 zero bytes) + W*H*4 payload; any content works, header is skipped.
- A/B methodology: baseline `-g X` vs new `-g X --render-gpu X` → byte-identical stdout/stderr, both exit 0. Negative proof of consumption: `-g <valid> --render-gpu <bogus>` errors naming the RENDER gpu.
- Concurrent-worker builds: lsfg-vk-common/image.cpp was mid-edit during verification; per-TU object builds (`make lsfg-vk-cli/CMakeFiles/lsfg-vk-cli.dir/src/tools/debug.cpp.o`) prove own-TU cleanliness without waiting on foreign files.
- shellcheck is not installed on this rig; bash -n only.

## task-4: exchange descriptors + pure negotiation core (2026-08-23)
- `negotiateExchangeLayout` semantics locked in: candidates = modifiers present in BOTH caps lists for the format whose intersected usage bits cover `usageNeeds`; FIRST common candidate in capsA order wins (deterministic). LINEAR fallback only fires when no pass-1 candidate exists AND modifier 0 is listed on both sides with sufficient bits — strict lookup, no "assumed supported" heuristic. Failure = `ls::error` (repo exception idiom), message names format + needed usage bits.
- `NegotiatedExchangeLayout.kind()` is DERIVED from `modifier == EXCHANGE_MODIFIER_LINEAR` — single source of truth, cannot disagree with the modifier field. `EXCHANGE_MODIFIER_LINEAR = 0` defined locally per DRM convention (no libdrm dependency).
- `DeviceExchangeCaps = std::map<VkFormat, std::vector<ExchangeModifierCaps>>`; entry field named `requiredUsageBits` per plan but semantically it's VkDrmFormatModifierPropertiesEXT::drmFormatModifierTilingFeatures (SUPPORTED usage) — todo 8 maps directly.
- Standalone test gotcha: `ls::error` has a virtual member (`inner()`), so its vtable lives in errors.cpp — unit harness must compile errors.cpp too or link fails. Also `VK_FORMAT_FEATURE_2_SAMPLED_BIT` does not exist; correct name is `VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT`.
- Parallel-worker hazard observed: full-tree cmake build transiently failed in image.cpp (todo 2 mid-edit header/cpp mismatch) while exchange.cpp.o compiled clean; retry after ~45s went green with 0 warnings. When a Wave-mate owns a file, treat their transient breakage as retry-not-fix.

## task-2 (dma-buf images) — 2026-08-23
- `vk::Image` ctor extended with ONE defaulted param `const ImageLayout& layout = {}`
  (`{mode, drmModifier, rowPitch}`) — every existing call site compiles unchanged; Opaque mode is
  byte-compatible with the old behavior (same OPAQUE_FD literals via a `handleType` variable,
  same pNext selection, same findMemoryTypeIndex call).
- New modes couple tiling+handle type BY DESIGN per the transport matrix: Linear/DrmModifier imply
  DMA_BUF external memory; allocation always chains VkExportMemoryAllocateInfo{DMA_BUF} so
  `exportDmaBuf(vk)` works lazily (returns {fd, allocationSize, rowPitch}; caller closes fd).
- IMPORT correctness trap handled: for dma-buf imports the type index comes from
  vkGetMemoryFdPropertiesKHR ∩ image requirements (device-local preferred, fallback to any common
  bit) — never plain findMemoryTypeIndex. Imported fd is close()d on every pre-success failure
  path (fd properties failure, empty intersection, AllocateMemory failure); after successful
  AllocateMemory the driver owns it (bind failure must NOT close).
- DrmModifier creation needs plane layouts: single-plane exchange formats ⇒ drmFormatModifierPlaneCount=1,
  VkSubresourceLayout{offset=0, rowPitch=from descriptor}; rowPitch==0 rejected early with a named error.
- Row pitch/allocation size queried post-bind via core vkGetImageSubresourceLayout (chosen over
  Layout2: single-plane formats, no extension dependency). Extra GetImageMemoryRequirements call
  in ctor is init-time only, negligible.
- CLI rig facts: `-g` matches EXACT deviceName ("AMD Radeon RX 9070 XT (RADV GFX1201)");
  dll must be passed via `-d` (LSFGVK_DLL_PATH only works with LSFGVK_ENV=1 through the config path);
  radv "not conformant" stderr warnings are normal. Benchmark baseline on Navi48: ~1204 gen-fps.
- VulkanDeviceFuncs grew GetImageSubresourceLayout (core block tail) + GetMemoryFdPropertiesKHR
  (after GetMemoryFdKHR); designated-initializer order kept consistent between struct and table.
  Same availability class as the already-loaded GetMemoryFdKHR (loader returns ext trampolines for
  supported devices even when app didn't enable them — existing pattern relies on this too).

## task-3: SYNC_FD binary semaphores (2026-08-23)

- `vkSignalSemaphoreKHR` is timeline-only IN PRACTICE on this rig: RADV GFX1201 and ANV ARL both return
  `VK_ERROR_DEVICE_LOST` when host-signaling a BINARY semaphore (exportable or not). To emulate the spec's
  "-1 fd == already-signaled sync fd" semantic, signal via an EMPTY queue submit (0 cmd buffers +
  signalSemaphoreCount=1, spec-legal) followed by `DeviceWaitIdle`. Do not "simplify" back to SignalSemaphoreKHR.
- SYNC_FD has COPY transference: export snapshots the CURRENT payload, so binary semaphores must be signaled
  BEFORE `exportFd()`; each export yields a fresh fd. OPAQUE_FD has REFERENCE transference (one-shot move).
  This is why binary Semaphore got an `exportFd(vk)` method instead of TimelineSemaphore's ctor out-param style.
- SYNC_FD imports MUST set `VK_SEMAPHORE_IMPORT_TEMPORARY_BIT` (payload reverts after next wait observes it);
  OPAQUE_FD imports stay permanent (flags=0) — existing behavior preserved byte-identically.
- Creation must attach `VkExportSemaphoreCreateInfo{SYNC_FD}` whenever the semaphore will be exported OR
  imported as SYNC_FD (copy-transference VU requires it at creation time).
- `VkImportSemaphoreFdInfoKHR` field order differs across vulkan header revisions (this box: semaphore BEFORE
  flags); designated initializers must match the LOCAL `/usr/include/vulkan/vulkan_core.h`, not memory/docs.
- Smoke rig proof: Intel ARL -> RX 9070 XT and reverse SYNC_FD roundtrips complete in <5s with poll()-readable
  fds; -1 sentinel imports cleanly; default-constructed local binary semaphores unchanged.

## task-8: DeviceExchangeCaps query adapter (2026-08-23)

- `vkGetPhysicalDeviceFormatProperties2` was NOT in the todo-1 instance table; added as an unconditional
  `ipa<>` entry (core-1.1, same availability class as GetPhysicalDeviceProperties2). Signature is
  `(physdev, VkFormat, VkFormatProperties2*)` — the format is a VALUE arg, easy to forget when copying
  the Properties2 pNext-chain pattern.
- Two-call DRMOD enumeration works exactly like the other count/array patterns: chain
  `VkDrmFormatModifierPropertiesListEXT{sType}` (count=0, null array) onto `VkFormatProperties2.pNext`,
  allocate `drmFormatModifierCount` entries, re-point `pDrmFormatModifierProperties`, re-query.
  Driver-reported order preserved verbatim into the caps vector (negotiation relies on capsA order).
- Devices WITHOUT VK_EXT_image_drm_format_modifier are core-safe: the unknown pNext struct is ignored,
  count stays 0 → empty caps list returned naturally (no extension check needed in the adapter).
- Rig caps snapshot (2026-08-23): Intel ARL iGPU reports 5 modifiers/format (LINEAR + 4 Intel CC
  0x10000000000000{1,9,d,f}); both RADV Navi48/Navi44 report 9 modifiers/format (LINEAR + 8 GFX1201
  0x2000... modifiers). LINEAR usage bits = 0x1dd83 (SAMPLED|STORAGE|TRANSFER_*) for BOTH formats on ALL
  three GPUs; Intel HDR drops one modifier (0x...f → 0x15d83 bits) but LINEAR stays full.
- Cross-vendor assertion re-verified empirically: Intel∩AMD proper-modifier intersection = ∅ for SDR AND
  HDR ⇒ any Intel↔AMD pair negotiates LINEAR (which satisfies all needed usages here). AMD↔AMD pairs have
  8 shared proper modifiers each.
- Standalone throwaway linking recipe that worked: compile dump.cpp + src/helpers/errors.cpp +
  liblsfg-vk-common.a -ldl (errors.cpp TU included explicitly per known vtable gotcha; archive alone
  would also pull it via vulkan.o, belt-and-braces kept).

## task-11: driver-uuid keyed pipeline cache (2026-08-23)
- Ordering trap: the cache path is a vk::Vulkan CTOR ARG evaluated before device selection, but
  vulkan.hpp declares phys_dev BEFORE pipelineCache/cachefile members, so a selector wrapper can
  resolve the keyed path mid-construction: capture `std::optional<path>` by ref, assign it inside
  the selector after querying VkPhysicalDeviceIDProperties, pass the same optional as the cachefile
  arg. Guaranteed by [class.base.init] declaration-order init — documented in a comment at the site;
  do NOT reorder vulkan.hpp members without revisiting this.
- Rig UUIDs: RADV driverUUID hex = 414d442d4d4553412d44525600000000 ("AMD-MESA-DRV"+4 NULs), shared
  by BOTH AMD cards; Intel ARL = 186bb135fb15df24d2242d934ac847ae. Verified empirically: AMD↔AMD
  selections share ONE cache file, Intel gets a second — keying is per-DRIVER as planned.
- Prune pattern: std::filesystem::remove(dir / "lsfg-vk_pipeline_cache.bin", ec) with error_code —
  silently no-ops on ENOENT/unwritable dir; runs during path resolution (before cache read).
- Fallback semantics preserved exactly: XDG_CACHE_HOME honored whenever set+non-empty even if
  UNWRITABLE (no fallthrough to HOME — same as before; all cache I/O stays best-effort, run exits 0).
  HOME leg and /tmp leg verified via env -u.

## task-7: portable sync-fd handshake (2026-08-23)

- EXPORT-AFTER-ENQUEUE IS LIVE ON THIS RIG: exporting a SYNC_FD binary semaphore
  immediately after enqueueing its signal submit (NO host wait) yields a working
  fd on both RADV GFX1201 and ANV ARL — copy transference wraps the PENDING
  payload. Verified 5 cycles each direction via fenced waits (t7-smoke). Export
  BEFORE enqueue would be a dead fd; never do that.
- Binary semaphore recycling without extra submissions: capture(c) waits slot
  [(c-1)%2] in the same submit that signals slot[c%2] (different slots ⇒ legal);
  backend prepass(c+1) consumes all doneSlots(c). Temporary imports self-revert
  after the observing wait, so import rings need no explicit reset.
- vkSignalSemaphoreKHR stays unusable for binaries (DEVICE_LOST RADV/ANV); the
  -1 "already signaled" fallback is an empty QueueSubmit + DeviceWaitIdle.
- CommandBuffer::submit() already covers mixed binary+timeline shapes (timeline
  args are optional handles/values) — todo 7 needed NO command_buffer changes.
- ctx.vk inside ContextImpl is ls::R<const vk::Vulkan> (reference_wrapper):
  implicit conversion works for function ARGS but member access needs .get().
- Same-device zero-regression recipe: branch INSIDE present()/scheduleFrames on
  the mode flag; legacy text keeps its exact submits; only benign host-side
  reordering (present-mode walk, cmdbuf recording) was hoisted above the branch.
- openContext/scheduleFrames evolution for todo 12 to formalize: trailing
  defaulted `gameDeviceUUID` param, scheduleFrames returns done-fds + takes
  captureReadyFd (ownership: library consumes it unconditionally), plus
  Instance::isCrossDevice(context) query. All marked // NOTE(todo-12):.

## task-9: backend conditional dma-buf extensions + selected-device accessors (2026-08-23)
- Gate design landed as ONE trailing defaulted param `bool enableDmaBufExtensions = false` on the
  standalone vk::Vulkan ctor (after cachefile); createLogicalDevice takes it non-defaulted (internal).
  Default-off leaves requestedExtensions byte-identical to legacy {external_memory_fd,
  external_semaphore_fd, timeline_semaphore} — proven via new always-on stderr line
  "lsfg-vk: enabling device extensions: ..." printed from createLogicalDevice (only standalone ctor
  calls it; layer path never hits this function, so no extra noise in game processes).
- Hard-error policy implemented with hasDeviceExtension() probes BEFORE CreateDevice; error message
  names device ('<deviceName>') + missing extension. New tiny helper queryDeviceName() in vulkan.cpp.
  Negative live test not possible on this rig (all 3 GPUs support both exts) — covered by code review only.
- Backend accessors: InstanceImpl got thin delegating getters (vk.deviceUUID()/supportsDmaBuf()/
  supportsDrmModifierImages()); public backend::Instance methods forward via m_impl. Compile-only
  static_assert TU verified exact member-fn signatures for todo 13 consumption.
- Init log line lives at END of InstanceImpl ctor (after persistPipelineCache): "lsfg-vk: processing on
  '<name>' [uuid <32hex>], dma-buf: yes/no, drm-modifier-images: yes/no". uuidToHex() local helper
  mirrors findCacheFilePath's hexDigits pattern. Terminology "processing on" matches task-6 locked terms.
- Harness gotcha #2 for throwaway binaries: PhysicalDeviceSelector is a const std::function REFERENCE —
  returning a lambda temporary from a factory fn dangles (segfault). Store the lambda in a named static
  SelectorFn and return it by reference.
- Rig proof: default-off CLI debug run exit 0, ext list unchanged, init line prints RX 9070 XT uuid
  00000000040000... (byte4=0x04 per task-1 PCI-id encoding) with both booleans yes; gate-on harness on
  RX 9060 XT appends exactly the two EXT exts after the legacy three.

## task-10: layer conditional injection + game-device default picker (2026-08-23)
- modifyDeviceCreateInfo CANNOT probe the game device without receiving it: Root is passive,
  entrypoint.cpp owns physdev + instance_info->funcs, and a temp VkInstance cannot correlate
  handles across instances. Threaded (const vk::VulkanInstanceFuncs&, VkPhysicalDevice) through
  the signature; entrypoint call-site diff = ONE line. Deviation from "don't touch entrypoint"
  documented in evidence log; entrypoint was unowned in Wave 2.
- DevicePicker (backend-owned, frozen) exposes ONLY name/"0xVVVV:0xDDDD"/pci strings — no UUID
  channel. "UUID pinning" realized as identity-tuple equality: layer captures the game device's
  identity via the SAME queries/format the backend uses (Properties2 + to_hex_id mirror + pci
  format `bus:device.function` decimal no-padding), picker compares tuples. Byte-format parity
  with backend::to_hex_id (4 UPPERCASE hex digits, low-16 mask) is load-bearing.
- EMPIRICAL: RADV leaves VkPhysicalDevicePCIBusInfoPropertiesEXT all-ZERO when chained from an
  instance with NULL appInfo (ancient negotiated API version) while filling it fine on modern
  instances — same card, two instances, pci "0:0.0" vs "4:0.0". probeDevice treats zero-triple
  as unavailable (lenient skip; name+ids still match). Harness/game-like instances must set
  apiVersion >= 1.3 for pci to fill.
- Backend enumeration order differs per-instance (harness saw 9070-first, backend 9060-first in
  the same process) — live reproduction of the Troubleshooting.md:30 first-enumerated hazard the
  identity-pinned picker eliminates.
- Injection policy landed: legacy KHR five stay UNCONDITIONAL (load-bearing); dma-buf/modifier
  EXTs injected iff probed present; absent + gpu-set-and-differs => ls::error naming gpu string +
  device name + missing extension(s), thrown BEFORE finish() so vkCreateDevice never fails
  opaquely. gpu-unset or gpu==game-device => silent skip (legacy path needs neither).
- createSwapchainContext catch enriched: backend failures now carry "for requested gpu '<str>'"
  so nonexistent-gpu configs produce an error CONTAINING the configured string even when the
  injection hard-error can't fire (rig devices all support both EXTs).
- Harness recipe that worked: compile instance.cpp+swapchain.cpp directly against
  liblsfg-vk-{backend,common}.a (link order: backend THEN common), -lvulkan -ldl -lpthread;
  LSFGVK_ENV=1 activates env profile; fake VulkanInstanceFuncs (two fn pointers) unit-tests the
  injection paths driver-independently; real wrapped vk::Vulkan exercises the picker end-to-end.
  Fabricated SwapchainInfo segfaults inside Swapchain ctor AFTER selection — fixture-inherent
  (valid-gpu control crashes identically); treat post-selection exit 139 as out-of-scope.
- The injected extension list lives only during finish() (function-local vector captured by
  pointer in create-info) — snapshot it INSIDE the finish callback; reading after return is UB.

## task-12: backend descriptor ingestion + device-aware context api (2026-08-24)
- Modifier convention LOCKED in lsfgvk.hpp: `backend::EXCHANGE_MODIFIER_OPAQUE = UINT64_MAX`
  (legacy OPAQUE_FD import; allocationSize/rowPitch ignored) vs `vk::EXCHANGE_MODIFIER_LINEAR = 0`
  (LINEAR dma-buf) vs any other value (explicit drm modifier, rowPitch required). Backend maps
  opaque-sentinel → the exact pre-todo-12 `vk::Image(vk, extent, fmt, usage, fd)` call.
- openContext is now TWO overloads: primary `{span<const ExchangeDescriptor> sources,
  span<const ExchangeDescriptor> dests, array<uint8_t,16> exporterDeviceUUID,
  uint64_t negotiatedModifier, syncFd, w, h, hdr, flow, perf}` + legacy fd-pair overload
  (todo-7 shape incl. optional gameDeviceUUID) that wraps fds into opaque descriptors and
  delegates. exporterDeviceUUID SUPERSEDES gameDeviceUUID (no optional: exporter uuid is
  intrinsic to dma-buf descriptors); legacy overload defaults it to selectedDeviceUUID()
  when gameDeviceUUID is nullopt → byte-identical same-device behavior.
- Gate resolution (the "logical device exists by openContext time" trap): trailing defaulted
  `bool enableDmaBufExtensions = false` on backend::Instance ctor threads into todo-9's
  vk::Vulkan param; InstanceImpl stores it and openContext hard-errors when a context needs
  dma-buf (cross-device OR non-opaque negotiated modifier) on a gate-off instance — message
  names both uuids + tells caller to reconstruct with enableDmaBufExtensions=true.
  Todo-9's construction-time hard error covers requested-but-missing exts.
- CLI gate condition landed as `opts.render_gpu.has_value() && opts.render_gpu != opts.gpu`
  (unset gpu + set render_gpu conservatively gates ON since devices.front() may differ).
- Validation runs BEFORE any import (in Instance::openContext, not ContextImpl ctor — member
  init lists can't run pre-checks): exactly-2-sources, ≥1 dest, per-descriptor extent/format/
  modifier==negotiated/rowPitch>0-for-modifier checks with named errors; validation failure
  leaves ALL fds caller-owned, import failures follow todo-2 close-on-failure per image.
- CLI debug/benchmark now ingest OPAQUE-equivalent descriptors + pass vk.deviceUUID() as
  exporter. debug.cpp guards cross-device configs with a clean named error AFTER instance
  construction (proves gated init) because its timeline-driven loop can't drive the SYNC_FD
  handshake — TODO 15 OWNERS: the debug tool needs a cross-device run loop (capture-ready
  export + done-fd waits) before the matrix can execute cross pairs; today they exit 1 cleanly.
- Gotcha: anonymous-namespace helpers must be declared ABOVE Instance::openContext in
  lsfgvk.cpp (validation helper initially sat below → undeclared error); uuidToHex block is
  the right anchor point.
- Rig proof: plain debug exit 0 (3-ext legacy list), equal -g/--render-gpu exit 0 gate OFF,
  benchmark 3s exit 0 (~2370 gen-fps at 320x240 m=2), cross pair shows exactly the two EXT
  exts appended + processing-on-9060 + clean guard error exit 1. Build t12: 0 warnings;
  layer .so links unchanged (legacy overload keeps swapchain.cpp compiling untouched).

## task-13: layer negotiation wiring + descriptor production + reload honesty (2026-08-24)
- Dup-based fd handoff is THE safe janitor pattern for openContext: blindly closing
  exported fds after an openContext throw would double-close fds already consumed by
  successful imports (VK_KHR_external_memory_fd transfer semantics make them
  driver-owned). Layer now dup()s every exported fd into the descriptors and keeps
  the originals under an ExportFdJanitor; originals close exactly once on ANY throw,
  on success they close after the backend consumed the duplicates. Residual: on
  validation failure the dups leak (backend contract leaves them caller-owned) —
  impossible by construction since descriptors are assembled uniformly in one place.
- Negotiation proxy design (sanctioned simplification, documented at site): capsB =
  single {LINEAR, usageNeeds} entry keyed under the active format. With todo 4's pure
  function this reduces to "game-side LINEAR bits ⊇ union of all four usage sets"
  (game src TRANSFER_DST|SAMPLED, game dst TRANSFER_SRC|SAMPLED, backend imports
  STORAGE|SAMPLED per lsfgvk.cpp:653/:655). Result kind() is LinearFallback by
  construction; a defensive non-LINEAR guard throws anyway.
- Gate flip heuristic lives OUTSIDE the lazy block: crossRequested = gpu set &&
  !matchesSelector(gameProbe, gpu); threads as 4th backend ctor arg. probeDevice was
  hoisted above the lazy emplace so every context gets game.name for mode logging.
- Log-line contract as shipped (todo 15 greps these EXACT shapes):
  - same-device: `lsfg-vk: frame generation on the game's own device '<name>'`
  - cross-device: `lsfg-vk: processing on '<uuid32hex>' (game on '<name>')` — uuid
    NOT name because backend::Instance has no device-name accessor (frozen); the
    backend's own init line (~lsfgvk.cpp:434) carries the name↔uuid mapping.
  - hot-reload: `lsfg-vk: gpu change requires restart to take effect`, fired from
    Root::update() when active_profile->gpu != backendGpuKey && backend exists;
    key stored at emplace and updated after logging (fires once per change).
- Harness recipe that worked END-TO-END (better than t10's): link layer TUs +
  backend/common archives, drive Root::createSwapchainContext with a standalone
  vk::Vulkan as the "game" device + fabricated SwapchainInfo{images={}, 320x240 SDR}.
  Swapchain ctor COMPLETES against real devices — full descriptor→openContext→
  closeContext roundtrip, exit 0. Cross-pair run created a REAL cross-device context
  (negotiation + LINEAR dma-buf export/import RADV↔RADV + SYNC_FD pools) — only the
  frame LOOP remains for todo 15.
- Harness teardown gotcha #3: a standalone vk::Vulkan used as the fake game device
  must be deliberately leaked (`new`, never deleted) — the standalone ctor owns its
  VkInstance and destroying it trips the loader bug makeLeaking exists for; the real
  layer never hits this (wrapping ctor, app-owned handles). Symptom was exit 139
  AFTER all work completed cleanly.
- WatchedConfig::update() returns false forever under LSFGVK_ENV=1 → hot-reload
  tests MUST use a file config (LSFGVK_CONFIG + LSFGVK_PROFILE override for matching);
  first update() always returns true (last_timestamp starts default) — harmless.
- CLI debug/benchmark unchanged this todo: they exercise the backend path only; the
  layer paths are covered by the harness above.

## task-16: Troubleshooting dual-GPU docs (2026-08-24)
- Plan's log-line paraphrase was WRONG vs landed code: dual-GPU line prints the processing device's UUID HEX, not its name (`swapchain.cpp:148`: `processing on '<uuidToHex(...)>' (game on '<name>')`). Documented the real strings + pointed at lsfgvk.cpp:434's startup line (`processing on '<name>' [uuid ...]`) as the name-to-uuid map. Always grep the shipped source before quoting log lines in docs.
- Flatpak-Guide.md has NO render-node guidance (only config-dir/steamapps overrides) despite plan/inherited-wisdom claims. Requirement stated in Troubleshooting.md (:51 both /dev/dri/renderD* nodes visible in sandbox) with a general-setup cross-link; did not fabricate override commands.
- Grep gate `grep -ri "dual gpu is not supported\|must be the \*\*same gpu" docs/` empty repo-wide post-edit; evidence at .omo/evidence/task-16-issue-159-dual-gpu.{md,grepout}. NVIDIA wording mirrors Configuration.md:32 substance (dma-buf since 515.43.04, unverified, no test hardware).
- Cache doc facts locked: `lsfg-vk_pipeline_cache_<driverUUID>.bin` under XDG_CACHE_HOME/~/.cache; same-driver shares one file (rig: AMD+AMD via "AMD-MESA-DRV"), Intel vs AMD separate; legacy unkeyed file auto-pruned (lsfgvk.cpp:255-257).

## task-14: per-context matrix + fd hygiene audit + RAII ordering (2026-08-24)
- Janitor-hoisting pattern that fixed all ctor fd windows at once: declare
  `exportedFds` + `ExportFdJanitor` ABOVE the export loops, push each fd as
  produced (reserve capacity up front so push_backs can't throw), and retire
  per-vector janitors (`sourceFds`/`destinationFds`, init to -1 NOT 0 — a
  value-initialized 0 would alias stdin) once fds have a single canonical
  owner. Exactly-once close on every path then falls out of unwind order.
- syncFd ownership resolution: layer's `crossDevice` (gameUuid !=
  selectedDeviceUUID) is provably identical to backend ContextImpl's
  (exporterDeviceUUID != deviceUUID — same inputs, same comparison), so
  cross-device contexts can hand the ORIGINAL syncFd to openContext (backend
  never consumes it; janitor owns it throughout). Same-device must hand a DUP:
  whether a throw split the ctor before/after the timeline import is
  undecidable from the layer. fdscan proved: prefix +4/attempt vs fixed
  +3/attempt steady-state leak on induced gate-off throws.
- Inducible mid-negotiation throw WITHOUT test hooks: construct
  backend::Instance with enableDmaBufExtensions=false + Swapchain against a
  different-uuid game device -> openContext hard-errors inside
  validateExchangeDescriptors AFTER all fds exist. Perfect fdscan trigger.
- DISCOVERED LIMITATION (documented, unfixed): the dma-buf extension gate is
  decided ONCE at lazy backend emplace from the FIRST context's
  crossRequested (instance.cpp:305-341). Same-device-first + later
  cross-device swapchain in one process => named gate error for the later
  context. Cross-first or single-mode processes unaffected; Q4 honored.
  Harness orders: BAA exit 0 (both branches share one backend), ABA exit 1.
- Phase-B fdscan control trick: run the success roundtrip TWICE — delta after
  round 1 that stays flat in round 2 = one-time process residual (here: +1,
  identical pre/post-fix); growing delta = real per-context leak.
- RAII ordering facts worth keeping: swapchain.hpp declares `instance`
  BEFORE `ctx`, so reverse destruction runs ctx's closeContext deleter while
  the backend ref is still alive; hot-reload rebuild never leaves a reachable
  half-built Swapchain because getSwapchainContext throws on missing keys and
  myvkQueuePresentKHR catches per-swapchain.
- Harness build recipe refinement: prefix variants come free via
  `git show HEAD:<file> > /tmp/...` compiled alongside worktree TUs — no
  stash needed while other workers hold dirty files.

## task-15: cross-device e2e matrix execution (2026-08-24)

- Runner realized (`scripts/run-matrix.sh live`): deterministic DDS generator
  (128-byte 'DDS ' prefix + rotated 640x360x4 payload, 8 frames under the OUT
  dir, out of git), per-pair `timeout 120` + stdout/stderr split into
  `per-pair/<name>/{run.log,run.err}`, console tee'd to `matrix.log`, summary
  table + nonzero exit on any FAIL. Assertions are REAL-output driven: exit rc,
  `grep -c '^lsfg-vk-debug: wait ok'` == FRAMES*(MULTIPLIER-1), validation
  emptiness via `grep -E 'Validation Error|VUID'` (radv "not conformant" noise
  correctly ignored), negative control asserts the named selector error AND
  that no backend init line appeared (failure precedes Vulkan work).
- Debug tool loop upgraded for BOTH modes: same-device keeps
  signal/scheduleFrames/wait timeline choreography byte-identically; cross-device
  passes captureReadyFd=-1, polls each returned done-fd (POLLIN, 30s) then
  closes it. The tool now also emits the todo-13 mode log lines itself
  (`processing on '<uuid32hex>' (game on '<name>')` / same-device variant) —
  the CLI does NOT load the layer, so without this the runner's grep gate had
  no line to grep. uuidToHex + a deviceName() helper (via fi().GetPhysicalDeviceProperties2)
  live in debug.cpp's anonymous namespace.
- HARNESS GOTCHA THAT COST A DEBUG CYCLE: vk::CommandBuffer::submit's FULL
  overload does `waitValues.back()` on possibly-empty vectors -> SIGSEGV when
  called with no semaphores. The SIMPLE submit(vk) overload already fences +
  waits internally, so upload_image was ALWAYS host-synchronous — no extra
  fence needed for the captureReadyFd=-1 ordering argument.
- EXPORTER SIDE NEEDS THE DMA-BUF EXTENSIONS TOO: first live run segfaulted
  because only the backend instance got enableDmaBufExtensions=true; the CLI's
  own exporter vk::Vulkan still had the legacy 3-ext set, so LINEAR exchange
  images were created without external-memory support. Fix: one shared
  `needs_dma_buf = render_gpu set && render_gpu != gpu` gates BOTH ctors.
  Diagnostic telltale: TWO "enabling device extensions" lines differing.
- Second harness fix: passing the exportFd out-pointer is LOAD-BEARING in
  cross mode — createExchangeImage only chains VkExternalMemoryImageCreateInfo{
  DMA_BUF} when importFd||exportFd is present; with both nullopt (layer-style)
  the memory gets VkExportMemoryAllocateInfo{DMA_BUF} but the image has no
  external chain -> VUID-vkBindImageMemory-memory-02728. Allocation-time fds
  are closed unused; descriptor fds come from exportDmaBuf().
- FINDING #1 (BLOCKER, backend, NOT fixed from here): multi-cycle
  scheduleFramesCross deadlocks on this rig at cycle 2. Cycle-1 exports
  doneSlots[i] as SYNC_FD; cycle-2's prepass recycle-wait
  (lsfgvk.cpp:940-943) re-waits those slots and RADV GFX1200/1201 hang inside
  vkQueueSubmit. Isolated probe (/tmp/opencode/syncfd-export-probe.cpp, NO
  validation layers): empty-submit signal -> GetSemaphoreFdKHR(SYNC_FD) ->
  poll(fd)=POLLIN -> queue-wait on the SOURCE semaphore = infinite hang in
  vkQueueSubmit. So on RADV, SYNC_FD export does NOT leave the source binary
  semaphore in a waitable state (spec says copy transference; driver reality
  differs), and validation independently flags exactly that wait with
  VUID-vkQueueSubmit-pWaitSemaphores-03238 ("no way to be signaled"). The real
  layer would hit the identical hang at its second present(). Fix belongs in
  lsfg-vk-backend (e.g. don't recycle exported slots — rotate a larger done-slot
  ring, or re-signal via empty submits instead of recycling waits).
- FINDING #2 (validation-clean blocker, layer+common+backend protocol): the
  modifier convention maps EXCHANGE_MODIFIER_LINEAR(0) to PLAIN
  VK_IMAGE_TILING_LINEAR on both sides (swapchain.cpp ImageMode::Linear,
  importImage lsfgvk.cpp:512-517). RADV rejects plain-LINEAR + DMA_BUF for the
  exchange usages per caps query: exporter side VUID-VkExportMemoryAllocateInfo-
  handleTypes-09860 (+02728 pre-fix), importer side VUID-VkImageCreateInfo-
  pNext-00990 + VUID-...-09861 (LINEAR+STORAGE+DMA_BUF = FORMAT_NOT_SUPPORTED).
  ANV accepts linear both ways (its sides were clean once chained). Task-8's
  caps show STORAGE-on-LINEAR is advertised only via the DRM-MODIFIER path —
  so the fix direction is DrmModifier-mode images with modifier=0 end-to-end
  (needs a protocol way to say "modifier-explicit 0" vs plain linear). Until
  then cross pairs cannot be validation-clean on RADV.
- Matrix result as shipped: navi48-to-navi48 PASS (exit 0, 8/8 waits,
  validation-clean, same-device log line); invalid-renderer PASS (exit 1,
  named error "failed to find specified GPU:", zero Vulkan work); both cross
  pairs pass exit/gate mechanics ONLY up to frame 1 (cross-gate OK:
  processing uuid correlates with -g device != game device) then hit Finding
  #1 (exit 124 timeout, waits 1/8) with Finding #2 visible in their run.err.
  Evidence: .omo/evidence/task-15-issue-159-dual-gpu/{matrix.log,per-pair/*}.
  F3 note: after the backend fix lands, RE-RUN THIS RUNNER UNCHANGED — it is
  built to go fully green when the two findings are fixed.

## task-17: cross-device sync-fd deadlock + modifier-zero fix (2026-08-24)
- EMPIRICAL LAW RE-CONFIRMED AND GENERALIZED (RADV GFX1200/GFX1201): a binary
  semaphore that has been exportFd()'d must NEVER be waited locally afterward
  (hang inside vkQueueSubmit; validation independently flags VUID-03238). The
  law applies to BOTH edges: backend done-slot recycle-waits AND the layer's
  prev-capture-slot recycle-wait in present() - both were deleted.
- NEW SUB-LAW DISCOVERED BY PROBE BISECTION: binary semaphore payloads are
  consumed by their single waiter - two concurrent waits on one binary sem
  hang RADV's vkQueueSubmit too (probe main[0]+main[1] both waiting a chain
  sem). The backend's multi-waiter fan-out is only legal because
  prepassSemaphore is a TIMELINE sem (same-value multi-wait is fine). Probe
  emulation must chain one-shot binaries instead (1:1 waiter property).
- DESTRUCTION TIMING (validation-proven): destroying a semaphore whose signal/
  wait batch is still pending trips VUID-vkDestroySemaphore-semaphore-05149
  under VK_LAYER_KHRONOS_validation (10 hits in 100-cycle probe). The brief's
  literal "destroy immediately after export" is functional but NOT validation-
  clean. Fix: fresh-per-cycle creation kept, destruction deferred one cycle
  behind cmdbufFence (backend) / renderFence (layer) gates, which prove the
  referencing batches completed. Deferred variant: 0 validation errors.
- TEMP-IMPORT CHOREOGRAPHY THAT WORKS ON RADV (probe T4-T7): empty-submit
  signal -> DeviceWaitIdle -> exportFd -> destroy exporter -> TEMPORARY import
  into fresh SYNC_FD-capable sem -> submit{wait imported, signal other} ->
  downstream waits: all fine. The capture-fd path was never actually exercised
  before task-17 (CLI always passes -1; old runs died at frame 1 first).
- MODIFIER-0 MUST RIDE THE DRM-MODIFIER PATH: vk::Image ImageMode::Linear now
  creates via VkImageDrmFormatModifierExplicitCreateInfoEXT with
  drmFormatModifier=0, planeCount=1, explicit rowPitch (plain LINEAR+DMA_BUF
  rejected by RADV for exchange usages). Exporters may omit rowPitch:
  image.cpp synthesizes width*texelBlock (R8G8B8A8=4, RGBA16F=8) and
  exportDmaBuf carries it to importers verbatim; imports REQUIRE pitch now
  (validateExchangeDescriptors tightened: any non-opaque descriptor needs
  rowPitch>0). ANV accepts modifier-0-via-modifier-path equally (18/18 pairs).
- GetImageSubresourceLayout ASPECT TRAP: for DRM-modifier images ANV returns
  ZEROS for VK_IMAGE_ASPECT_COLOR_BIT; must use
  VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT (RADV tolerates both). image.cpp
  queryRowPitch switched for all exchange images. Probe-verified pitch 2560
  consistent across every ordered rig pair.
- Ordering formerly provided by the deleted recycle-waits is guaranteed by:
  layer renderFence gate at top of cross present() (covers everything through
  previous present's final dest pass => WAR-safe source/dest reuse across
  cycles), backend cmdbufFence gate (orders backend cycles + retires
  generations), and the 1:1 waiter property of fresh fds. Monotonic counters
  idx/fidx unchanged; dead handshakeIdx/captureSlots/doneSlots members removed
  (both sides) to keep -Wunused-private-field quiet.
- Matrix runner UNCHANGED and fully green on first post-fix run: all four
  pairs PASS with 8/8 waits and zero Validation/VUID lines (evidence:
  .omo/evidence/task-17-issue-159-dual-gpu.{log,matrix.log,per-pair/}).
  Same-device debug byte-identical pre/post; benchmark within noise.

## F3 final verification (2026-08-24)
- F3 VERDICT: APPROVE — independent rerun (fresh Release build /tmp/opencode/build-f3 @ 4fdfcdd): all 4 matrix pairs PASS (8/8 waits, zero Validation/VUID, cross uuid↔init-line correlation both directions), invalid-renderer named-error-before-Vulkan, benchmark 1241.33 gen-fps in-band, explicit-validation debug exit 0 clean. Evidence: .omo/evidence/f3-final-qa/{VERDICT.md,matrix.log,per-pair/,benchmark.log,debug-validation.*}.
task-18 (f2fix): moved janitor.consumed above the importSyncFd block in Swapchain::present (importSyncFd takes unconditional fd ownership — closes on failure, consumes on success — so janitor must stop tracking first, killing the double-close window); wrapped createSyncFdSemaphore in scheduleFramesCross with try/catch that closes captureReadyFd before rethrowing so the early-throw path honors 'consumed regardless of success'. Build /tmp/opencode/build-f2fix green 0 warnings; same-device debug 640x360 exit 0 validation-clean; full matrix live ALL 4 pairs PASS (.omo/evidence/f2fix/). Commit 2ae5bea.

## task-19: follow-up fixes (2026-08-24)
- std::span has NO .at() in C++20 — indexed span access stays operator[] with NOLINT after explicit size validation.
- clang-tidy misc-include-cleaner cannot map POSIX pollfd/POLLIN/poll to <poll.h> on this toolchain — NOLINT at use sites.
- bugprone-unchecked-optional-access here flags .value() too (strict mode); NOLINT with branch-invariant justification.
- CTAD gotcha: `std::array hexDigits{"0123..."}` deduces array<const char*,1>, NOT chars — spell out std::array<char,16>.
- Best-effort extension gating pattern: request capability when config implies need, enable iff supported (silent skip + stderr note), enforce hard error at point-of-use keyed on real capability probes — removes ordering limitations with zero runtime cost.

## task-20: live-app testing (2026-08-24)
- THE gap: CLI/harness greens never execute the layer's present() loop. First real-swapchain run (vkcube on Wayland) found 2 bugs instantly.
- Cross-vendor LINEAR pitch contract: ANV honors requested pitches >= its min alignment and reports them back verbatim; RADV rejects modifier-0 explicit plane layouts whose rowPitch is not 256-byte aligned. Exporter must pad synthesized pitch to 256B (extra bytes never read). 640x360 RGBA8 = 2560 = naturally aligned, which is why the test frames masked this.
- vkcube creates an auxiliary device WITHOUT VK_KHR_swapchain before its real one; layer device-wrap must derive `graphical` from the device's enabled extensions or it throws and silently unwraps that device.
- Loader quirks: VK_LOADER_DEBUG=layers is not a recognized flag value (use all); relative library_path in explicit-layer manifests found via VK_LAYER_PATH was NOT resolved against the manifest dir on this loader - use absolute paths in test manifests.
- GPU activity proof without perf counters: /proc/<pid>/fdinfo drm-engine-* deltas per render-node fd distinguish per-GPU work (amdgpu=drm-engine-gfx, Xe=drm-engine-render).

## delivery state (2026-08-24)
- Fork delivered: charlesarcher/lsfg-vk, branch feat/dual-gpu = 21-commit squashed stream off PancakeTAS/develop, NO PR (user decision pending).
- Local branches: feat/dual-gpu = original 23-commit history + completed plan record (20/20); rewrite/dual-gpu = mirror of the pushed stream, tracks origin/develop for future rebasing.
- History-rewrite artifact: .omo/plans/issue-159-dual-gpu.md exists only on feat/dual-gpu (e794c89); rewrite/dual-gpu intentionally carries no .omo state. Resting checkout = feat/dual-gpu so the completion record is present.
- Future PR flow: git checkout rewrite/dual-gpu && git fetch origin && git rebase origin/develop && git push fork feat/dual-gpu --force-with-lease; then open PR against PancakeTAS:develop.

## resting-branch correction (2026-08-24)
- v10-dual-gpu (the pushed 9-commit kernel-style stream) intentionally carries NO .omo state - it is based on origin/develop which never had the plan file. The boulder hook reads the working tree, so resting on that branch looks like "no plan".
- Resting checkout is therefore feat/dual-gpu (plan record 20/20 present). The upstreamable stream lives on v10-dual-gpu locally and feat/dual-gpu on the fork; rebase flow documented in the delivery-state entry above.
