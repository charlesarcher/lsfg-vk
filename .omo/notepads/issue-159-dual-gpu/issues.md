# Issues — issue-159-dual-gpu

Problems and gotchas encountered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## task-1 (2026-08-23)
- Draft ledger SYNC_FD numbers were wrong: live probe shows semFeatures=0x3 / fenceFeatures=0x3 on both AMD cards (not sem=0x657/fence=0x0). Wave-2 workers relying on "fences not exportable" must re-check assumptions.

## task-5
- Transient build failures from concurrent todo-2 edits to lsfg-vk-common/image.cpp during verification window; resolved by wait+rebuild per brief. Own TUs verified clean in isolation meanwhile.
- Plan's rig GPU shorthand names ("AMD Radeon RX 9070 XT") do NOT match actual Vulkan deviceName ("... (RADV GFX1201)") — exact-match selectors reject them. Runner defaults use full suffixed names.

## task-14 (2026-08-24)
- Gate-ordering limitation: backend dma-buf extension gate fixed at lazy
  emplace from FIRST context's crossRequested; mixed multi-device processes
  (same-device swapchain created before a cross-device one) hit the named
  "reconstruct with enableDmaBufExtensions=true" error for the later context.
  Clean hard error (Q4), but dual-mode-in-one-process requires cross-first
  creation order. Fix needs gate-policy rework (behavior change) — out of
  todo-14's fd/RAII mandate; surfaced to F1/F2 reviewers.
- lsfg-vk-common semaphore imports don't close the fd on vkImportSemaphoreFdKHR
  failure despite "// closes the fd" comments (timeline_semaphore.cpp:47-49,
  semaphore.cpp:47-51) — violates VK_KHR_external_semaphore_fd ownership on
  the failure path. Outside task-14's allowed file set; near-unreachable
  in-process (fds come from our own successful exports). Upstream follow-up.
- Handed descriptor dups leak when openContext fails without consuming them
  (T13-documented residual): bounded ≤ multiplier+1 fds per failed context
  creation; unfixable layer-side without double-close risk. fdscan quantified:
  +3/attempt steady-state under induced gate-off throws.

## task-15 (2026-08-24)
- BLOCKER for cross pairs (backend): scheduleFramesCross cycle-2 recycle-wait
  on SYNC_FD-exported doneSlots deadlocks RADV GFX1200/GFX1201 inside
  vkQueueSubmit; validation flags the same wait VUID-03238. Standalone probe
  (no validation) reproduces: signal -> export SYNC_FD -> poll ok -> wait
  source = hang. Spec says copy transference; driver disagrees. Owner:
  backend (todo 14/F3). Repro: scripts/run-matrix.sh live, pair logs under
  .omo/evidence/task-15-issue-159-dual-gpu/per-pair/.
- BLOCKER for validation-clean cross runs (layer+common+backend): modifier 0
  is mapped to PLAIN linear tiling on both sides; RADV rejects plain-linear +
  DMA_BUF for the exchange usages (09860/00990/09861, FORMAT_NOT_SUPPORTED).
  STORAGE-on-LINEAR exists only via the drm-modifier path on this rig.
- vk::CommandBuffer::submit full overload crashes (back() on empty vector)
  when called without timeline semaphores — latent API trap, hit from CLI.
- CLI harness must enable dma-buf extensions on BOTH its own exporter device
  AND the backend instance; also must pass an exportFd out-pointer so the
  DMA_BUF external chain lands on exchange images (see learnings task-15).

## task-17 (2026-08-24)
- Brief deviation (sanctioned by its escape clause): "destroy immediately after
  export" replaced by one-cycle-deferred destruction behind fence gates -
  immediate destruction trips VUID-05149 under validation and would have failed
  the zero-validation-error matrix gate (probe evidence in task-17 log).
- Latent trap fixed defensively, not because a current call site passes empty
  wait+signal vectors today: CommandBuffer::submit full overload guarded with
  empty checks; layer cross path now passes game `semaphores` directly, which
  CAN be empty for some games' presents.
- validateExchangeDescriptors tightened: LINEAR descriptors previously exempt
  from the rowPitch>0 check; now any non-opaque descriptor requires it (all
  real producers always set it; importImage would otherwise synthesize a wrong
  natural pitch for foreign memory).
- Residual (out of scope, unchanged): dma-buf extension gate still decided once
  at lazy backend emplace from the FIRST context's crossRequested (task-14
  limitation); mixed same-device-first processes still hit the named error.
