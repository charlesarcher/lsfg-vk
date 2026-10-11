# udmabuf ring transport — progress

Format: commit | profile | result | log path
Rule: no CPU frame copies, no malloc/posix_memalign/external_memory_host, GPU DMA only.

## Baseline findings (step 1, BO-list scope)
- radv global BO list is per VkDevice: radv_amdgpu_winsys.h:57-64, created per device in
  radv_CreateDevice -> radv_create_winsys (radv_device.c:1460), so a copy-only VkDevice on
  the 9070 keeps the udmabufs off the game's submits. Quote recorded; no code.
- Host-pointer import of a memfd or udmabuf mapping is REJECTED on both AMD cards:
  VkResult=-1000072003 (VK_ERROR_INVALID_EXTERNAL_HANDLE) at
  src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c:820-822. Intel iGPU accepts the same
  memfd mapping. => dma-buf import only.
- Tooling: tools/testing/udmabuf_hostptr_probe.cpp (untracked, diagnostic).aa47c7a+ | ring setup/lifecycle | no GPU path yet | n/a
2f2bee0+ring37 | furmark-decoupled-bench | FAIL: transport=udmabuf_ring rejected by parser | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T203520
7a2710d | furmark-decoupled-bench | FAIL: stream ended 'lazy: value already present' (ring slot images re-emplaced on re-created ring) | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T203636
f5e5e95 | furmark-decoupled-bench | FAIL: app 'lazy: value already present' -> layer broken pipe | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T204145
4d16b23 | furmark-decoupled-bench | FAIL: import needs explicit row pitch (error -3); copy-only device + K+1 fds OK | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T204739
4d16b23 | furmark-decoupled-bench | ring hand-off WORKS (K=3 imported, consume ready) but 0 frames published; LSFGVK_RING_DBG stripped by env -i so no layer trace | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T204912
780e9d8 | furmark-decoupled-bench | 0 published: 1440 skip-IPC debug gate hid first 16 presents from the ring | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T205226
63e8bf9 | furmark-decoupled-bench | still 0 published: capture gated on staging slotFree which the ring never fills | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T205341
eb03799 | furmark-decoupled-bench | FAIL: copy-device selector queried the game's VkPhysicalDevice across instances -> loader ERROR, no ring setup | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T205947
fe4339d | furmark-decoupled-bench | FAIL: move ctor dropped ring/copyVk -> present sees ring=0 | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T210223
c09da53 | furmark-decoupled-bench | 0 published: stray forward-present block short-circuited the ring copy after 'copy begin' | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T211320
32915fe | furmark-decoupled-bench | FIRST end-to-end frame (claim/fence/publish/app-claim/release) but only 1 present reaches the layer; game stops presenting after frame 1, cause unknown | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T211419
ecb2130 | furmark-decoupled-bench | FAIL: game SIGSEGV in radv via forward-present on the scanout-less isolated swapchain (exit 139) | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T211638
24f46b2 | furmark-decoupled-bench | PROGRESS: 3 frames published+claimed (all K slots cycled), then game abort 134: capture submit failed vkQueueSubmit error -4 (device lost); app also SIGSEGV | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T212032
4350546 | furmark-decoupled-bench | ring healthy (skips not blocking) but app SIGSEGV in createSemaphore deleter during output-thread teardown; game aborts -4; release path still unreached | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T212525
e2ef15b+7859d37 | furmark-decoupled-bench | BLOCKED: 9070 XT dropped out of the Vulkan enumeration; only 9060 (index 0) and Intel (index 1) enumerate. reset_render_card.sh cannot run (rocm-smi sees no AMD device). Last good run before this: /tmp/lsfg-runs/furmark-decoupled-bench/20261004T212525
0a4eefe | furmark-decoupled-bench | PASS-CLEAN: 131.43 presents/s, gen=7886 real=7886, longest_gap=44.412ms, exit=0, 60.0s, dmesg 04:00.0 faults 11->11 (none) | /tmp/lsfg-runs/furmark-decoupled-bench/20261004T220930
52606b0 | re2 | VISIBLE at 30s (title screen + MangoHud + our overlay: game fps 123, doubled fps 242, frame 2361/4722), 3351 frames realized, 4 streams incl. both 1440p, 38.2s clip, dmesg 11->11, no crash | /tmp/lsfg-runs/re2/20261004T221100 + /home/archerc/.hermes/cache/scratch/dec_final_30.png
e02ed76+ | dp7-record (FurMark) | HITCH CHECK FAILS: 240fps/2560x1440/14755 frames; in-game 1-59s has 3 duplicate runs: t=1.967s 20.8ms, t=25.100s 25.0ms, t=56.008s 4.2ms. Plus 191.7ms freeze at t=0.075s (startup) and 37.5ms repeats at 60.1s+ (post-bench teardown, clip 61.48s vs elapsed 60.009s) | /tmp/lsfg-runs/dp7-record/20261004T221308 + tools/testing/hitch_check.py
2241f12 | re2 | RE2 visible+2x on DP-7 (base 123 / doubled 242, frame 6159/12318), 127s capture, 3 streams incl both 1440p, 14195 frames realized, dmesg 11->11. Hitch check: 40-110s steady state = ZERO stalls over 16800 frames; 30-120s = 11 frames/45.8ms (3 events, all in first 32s menu transition) | /tmp/lsfg-runs/re2/20261004T224140
c845979+be600c2 | dp7-record (FurMark 300s) | HITCHES CONFIRMED VISUALLY: 55 events / 246 stalled frames over 296s (median changed 39.921%). At t=41.30s full-res PNGs show 6 identical frames then 4 identical (byte-identical sha256). MangoHud p99=16.2ms vs p50=5.1ms. presents/s=129.58 gen=38875 real=38875 exit=0, dmesg 11->11 | /home/archerc/lsfg-work/runs/dp7-record/20261004T232353 + ~/lsfg-work/vis/

## 2026-10-10 session: ring faults root-caused, frames finally published

Everything below is measured, not inferred. Card ended off the bus; needs reboot.

### The faults (three separate causes, found in this order)

1. **Image import is impossible on this driver.** The probe reports
   `handles=0x0` for EVERY candidate format on the 9070:
   `no linear+dma-buf format for render dst`. An image import is ACCEPTED at
   creation and then faults the GPU page tables on first use. That was the
   original MAPPING_ERROR / -4 cascade. Fix: import slots as BUFFERS
   (7dbb93c), which is what the working udmabuf transport always did.
   NOTE: the pre-existing ring probe's BUFFER_ONLY mode forces DOUBLER_ONLY
   (`#if BUFFER_ONLY && !DOUBLER_ONLY -> #define DOUBLER_ONLY 1`), so its
   "buffers proven" result never covered the render card. That misled three
   separate attempts.

2. **Nested submit during the present hook.** 21 faults. The layer's own
   comment (entrypoint.cpp:1010) says it: "Nested QueueSubmit during
   QueuePresent races vkd3d's QueueSubmit2." Fault count per run:
   in-present 21 -> worker thread 3 (shared VkDevice, no external sync
   possible) -> acquire-driven 0.

3. **The isolated recycle-fence signal was a SECOND nested submit.**
   isolatedSignal is a bare empty QueueSubmit that only signals a fence, and
   a submit can signal exactly ONE fence - so the ring's copy submit now
   carries the recycle fence instead (42e7d0d). Faults 19 -> 3 per run.

### The deadlock (separate from the faults)

The ring forces skipIpc false so every present reaches presentSwapchain, but
the deferred work that CONSUMES the game's present wait-semaphores is gated
on skipIpc and flushed from vkQueueSubmit2. Nothing consumed them, so
vkd3d's present never completed and the game stopped after ONE frame per
swapchain. The ring now consumes them in its in-present submit.

### The silent no-submit

`ringSubmitQ` was set during ring setup, but captureQ is not assigned until
later in the constructor and vk.queue() was null there -> every submit went
to VK_NULL_HANDLE and did nothing, with no error. Found by an ungated
`q=(nil)` print (resolve the queue at submit time, as the decoupled path
does).

### The doubler never released slots

The ring branch in the input thread claims a slot and `continue`s, so it
never reached the wire path's buffer->image copy and Release block
(presentation.cpp:2890-3670). Nothing copied the slot and nothing released
it, so the ring filled after K frames and the producer skipped forever
(`ring claim seq=4 -> slot -1`). Fixed in 0219bba: the ring now does its own
copy (slot buffer -> genSources) on the app device with its own fence, then
releases before handing the frame to the output thread.

### Where it stands

- Submits work, frames publish to the control block (9), faults flat
  (77 -> 77 across the last runs), and the doubler realizes a frame.
- Remaining failure observed: `stream ended: failed to open backend context
  - backend context is not cross-device`, with the app reporting
  `stream from 'AMD Radeon RX 9060 XT'` - i.e. the GAME had fallen back to
  the 9060 because the 9070 was already off the bus, making the context
  same-device. That error is a SYMPTOM of the missing card, not a code
  defect (an earlier claim that skipping the STAGING exchange caused it was
  wrong and is retracted).
- To resume: reboot (the 9070 will not re-enumerate after resets; the 9060
  also wedged a compute queue). Then confirm the game lands on the 9070
  (`stream from 'AMD Radeon RX 9070 XT'`) before reading anything else.
