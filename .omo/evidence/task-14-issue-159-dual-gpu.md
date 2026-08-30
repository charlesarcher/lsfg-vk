# task-14 — Per-context mode matrix verification + fd hygiene audit + RAII ordering

Status: **DONE** · Build `/tmp/opencode/build-t14` green, **0 warnings** · Regression exit 0, validation clean
Product files changed: `lsfg-vk-layer/src/swapchain.cpp` ONLY (4 surgical fd-lifetime fixes, comments at site).
`instance.cpp` / `entrypoint.cpp` / `lsfgvk.cpp`: audited, **no changes needed**.

---

## (a) Per-context transport/sync matrix — VERIFIED (harness + code trace)

Method: **real harness** (`/tmp/opencode/t14-harness/matrix.cpp`, t13 recipe: layer TUs +
backend/common archives, standalone `vk::Vulkan` wrappers as fake game devices, fabricated
`SwapchainInfo{320x240 SDR}`), driving `Root::createSwapchainContext` directly. Chose harness over
log-inspection because todo 15's runner was still in flight; this gives independent proof.

Code-trace chain (per-context by construction):
1. `entrypoint.cpp:182-190` — EVERY `VkDevice` wrapped separately into `instance_info->devices`.
2. `entrypoint.cpp:280,302,333` — `myvkCreateSwapchainKHR` looks up THIS device's wrapper and passes
   it to `createSwapchainContext(it->second, ...)`.
3. `swapchain.cpp:143-144` — mode decision reads THIS context's uuid:
   `gameUuid = vk.deviceUUID(); crossDevice = gameUuid != backend.selectedDeviceUUID()`.
   No global/stateful input; two swapchains on different devices in one process get independent
   decisions. Backend's `ContextImpl` derives the identical flag from the identical comparison
   (`exporterDeviceUUID != instance.getVulkan().deviceUUID()`), so layer and backend can never
   disagree about a context's mode.

Harness result (one process, ONE shared lazy-emplaced backend, gpu=9070 XT):

| order | contexts | result |
|---|---|---|
| BAA (cross-first) | 9060(cross) + 9070(same)×2 | **exit 0** — `processing on '0000…0400…' (game on '…9060 XT…')` AND `frame generation on the game's own device '…9070 XT…'` ×2, all three live simultaneously on the shared backend |
| ABA (same-first) | 9070(same) then 9060(cross) | first context ok; cross context fails with the named gate error below |

**DISCOVERED LIMITATION (documented, not fixed — outside fd/RAII mandate):** the backend dma-buf
extension gate is decided ONCE at lazy emplace from the FIRST context's `crossRequested`
(`instance.cpp:305-341`). A process whose first swapchain is same-device but a later one is
cross-device gets: `dma-buf exchange required (exporter uuid …87…, selected uuid …04…) but the
instance was created without the dma-buf extensions`. Q4 honored (clean named error, no silent
fallback); cross-first and all-single-mode processes are unaffected; todo 15's CLI constructs
per-run instances with correct gates. Fix would require re-thinking gate policy at emplace time
(behavior change ⇒ out of scope for todo 14). Filed in issues.md.

## (b) fd hygiene sweep — every fd-producing failure path (T7/T12/T13 surface)

Ownership rules applied: VK_KHR_external_memory_fd / VK_KHR_external_semaphore_fd — import consumes
the fd on SUCCESS only; on failure it stays caller-owned and must be closed by the caller.
SYNC_FD export has copy transference (fresh caller-owned fd each export).

### lsfg-vk-layer/src/swapchain.cpp

| # | path | verdict |
|---|---|---|
| 1 | image-creation loops (:213-229): later `vk::Image` ctor throws after earlier opaque fds written via out-param | **fixed-in-this-todo** — vectors now `-1`-initialized + `ExportFdJanitor` per vector spans the loops (pre-existing upstream shape, hardened while in the hunk; `-1` init also removes the latent close-fd-0 hazard) |
| 2 | cross-device `exportDmaBuf` loops: partial failure leaked already-exported originals (janitor was constructed only after the loops) | **fixed-in-this-todo** — `exportedFds` janitor hoisted ABOVE the loops; fds registered as produced; all push_backs non-throwing (capacity reserved up front) |
| 3 | `dup()` failure (:293-300): threw after closing only `handedFds`; ALL exportedFds leaked | **fixed-in-this-todo** — throw now unwinds through the hoisted janitor, closing every original exactly once |
| 4 | **flagged window**: syncFd created (:281-283), then openContext throws BEFORE the timeline-semaphore import (validation :453, gate error :374, source/dest import failures in ContextImpl init list) → syncFd original leaked; naive catch-close is UNSAFE because post-import failures make consumption state undecidable (double-closing a driver-consumed fd number may hit an unrelated recycled fd) | **fixed-in-this-todo** — syncFd original registered under the janitor; cross-device hands the ORIGINAL to openContext (layer's `crossDevice` computation is provably identical to the backend's — same uuids, same comparison, no Vulkan calls — so the backend never consumes it there) while same-device hands a DUP (consumed-or-documented-residual). Verified empirically: fdscan steady-state leak dropped +4 → +3 per attempt |
| 5 | handed descriptor dups when openContext fails without consuming them (early validation/gate failure, or partial import failure leaving unattempted dups) | **documented-pre-existing** (T13-sanctioned residual): frozen backend contract leaves unconsumed descriptor fds caller-owned; the layer cannot distinguish consumed vs unconsumed dups after a throw without risking double-close of driver-owned numbers. Bounded ≤ multiplier+1 fds, once per failed context creation. Eliminating it requires a backend contract change (mutable ownership handoff) — rejected as out of scope |
| 6 | success path: janitor disarmed, originals closed once (:335-336); same-device dups consumed by imports; cross-device syncFd original closed via janitor set | safe-by-construction (fdscan PHASE_B returns to baseline) |
| 7 | present(): captureFd export (:430) produces fd only on success; handed to scheduleFrames which takes unconditional ownership before any fallible op (lsfgvk.cpp:900-928) | safe-by-construction |
| 8 | present(): doneFds — `FdJanitor` consumed-tracking precise (:534/:542); `importSyncFd` closes on failure (:68-71); `.at()` OOB unwind closes remainder | safe-by-construction |
| 9 | backend `scheduleFramesCross` catch(...) closes accumulated doneFds + rethrows (:997-1001); `importSyncFdSemaphore` closes on failure (:578-581) | safe-by-construction |

### lsfg-vk-backend/src/lsfgvk.cpp

| # | path | verdict |
|---|---|---|
| 10 | `validateExchangeDescriptors` (:355-414): runs before ANY import; zero fd ops | safe-by-construction (fds untouched ⇒ caller-owned, matches comment) |
| 11 | `importImage` → vk::Image dma-buf/opaque import: close-on-every-pre-success-failure (image.cpp:159/165/208), never after AllocateMemory | safe-by-construction (todo-2 semantics verified by inspection) |
| 12 | `importTimelineSemaphore` (:559) → common `TimelineSemaphore` ctor: **import FAILURE does not close the fd despite the "// closes the fd" comment** (timeline_semaphore.cpp:47-49); same pattern in semaphore.cpp:47-51 (T3 SYNC_FD variant mirrors legacy shape) | **documented-pre-existing** — lives in `lsfg-vk-common/` (outside this todo's allowed file set); unreachable-ish in-process (fd comes from our own successful export); flagged for upstream follow-up |
| 13 | `closeContext` (:1006-1019): DeviceWaitIdle + erase; no fds held by backend beyond consumed imports | safe-by-construction |

### lsfg-vk-layer/src/instance.cpp & entrypoint.cpp

No fd operations anywhere (probe/picker/emplace/hooks deal in Vulkan handles only). One non-fd
observation, pre-existing, NOT addressed: if `createSwapchainContext` throws inside
`myvkCreateSwapchainKHR` (:333), the underlying VkSwapchainKHR was already created by the callback
and `swapchainInfos` keeps a stale entry; app receives an error handle-less. Pre-existing upstream
shape, non-fd, out of scope.

## (c) RAII ordering vs synchronous hot-reload rebuild — NO half-built window

Destruction order trace (reverse declaration order, swapchain.hpp:64-88):
`info, profile, fidx, idx → ctx → instance → postCopySemaphores, passes, renderFence,
renderCommandBuffer → doneWaitSemaphores, captureSemaphores → syncSemaphore → destinationImages,
sourceImages`.
- `ctx` (owned_ptr, deleter = `backend->closeContext`) is destroyed BEFORE `instance`
  (the `ls::R<backend::Instance>` ref) ⇒ the deleter's backend deref is always valid.
- `ctx` destroyed before layer-side semaphores/images is harmless: `closeContext` only does
  DeviceWaitIdle on the BACKEND device + erases ContextImpl (its imported images die with it;
  kernel dma-buf refs keep exported memory alive until the layer images die later).
- Ctor-throw mid-build (e.g. sync-fd pool creation :340-349): already-constructed members unwind in
  reverse order; `ctx` deleter runs `closeContext` whose DeviceWaitIdle waits only self-contained
  init work (no external handshake dependency) ⇒ no deadlock, no half-built escape.

Hot-reload rebuild (`entrypoint.cpp:364-378`): strictly sequential on the presenting thread, BEFORE
any present of that frame: `removeSwapchainContext(k)` fully destroys the old Swapchain (RAII, no
window where old+new coexist), then `createSwapchainContext(k)` builds the new one to completion.
If construction of swapchain k+1 throws after k was rebuilt: k keeps its complete NEW context,
k+1 simply has NO context; the catch logs and the present loop proceeds — `getSwapchainContext`
then throws "swapchain context not found" (instance.hpp:64-70), caught at :409, surfaced as
VK_ERROR_UNKNOWN for that swapchain only. No use-after-free, no partially-constructed object is
ever reachable; next config change retries the rebuild. The loop iterates `instance_info->swapchains`
by reference but nothing inside mutates that map (Root owns a different map) ⇒ iterator stable.

## (d) Pre-existing unlocked lazy-init race — DOCUMENTED ONLY (fix out of scope)

`Root::createSwapchainContext` (instance.cpp:299-355): `if (!this->backend.has_value()) { …long init… emplace }`
is check-then-act with no mutex. Two threads creating swapchains concurrently (multi-threaded apps
do create devices/swapchains off the main thread) can both observe `!has_value()` and double-emplace
(`std::optional::emplace` second call destroys the first backend while contexts may already hold
`ls::R<backend::Context>`s into it ⇒ potential UAF), or one thread can read `backend.mut()` while
the other is mid-emplace. Window WIDENED by T10/T13: probeDevice per context, dll search, shader
extraction, caps logging and (T12-era) descriptor machinery all run between check and emplace.
Pre-existing upstream (lazy emplace existed), never thread-safe; fix = mutex/once_flag around the
block — deferred per plan. Also noted: `update()`'s hot-reload honesty line reads/writes
`backendGpuKey` unlocked (same class of benign-but-real raciness).

## Verification summary

- Build: isolated `/tmp/opencode/build-t14`, EXIT=0, WARN=0, ERR=0 (one transient foreign failure
  in todo-15's debug.cpp mid-edit; waited ~50 s, rebuilt once → green).
- Per-context matrix: harness BAA exit 0 (both branches, one shared backend, distinct log lines);
  ABA documents the gate-ordering limitation above.
- fd-scan: `.omo/evidence/task-14-issue-159-dual-gpu.fdscan` — prefix +4/attempt vs fixed +3/attempt
  steady-state across induced mid-negotiation throws (= flagged syncFd window eliminated; remaining
  +3 = documented dup residual #5); success roundtrip returns to baseline (one-time +1 process
  residual, identical pre/post-fix, proven non-growing over repeated contexts).
- Same-device regression: `lsfg-vk-cli debug -g <Navi48> -w 640 -h 360` exit 0 under
  VK_LAYER_KHRONOS_validation, stderr clean, all waits ok. Pacing/present logic untouched
  (diff confined to ctor fd bookkeeping).
