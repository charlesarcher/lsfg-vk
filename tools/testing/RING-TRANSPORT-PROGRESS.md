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
