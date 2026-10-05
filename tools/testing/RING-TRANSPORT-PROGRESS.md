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
