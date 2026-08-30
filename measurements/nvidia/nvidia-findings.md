# NVIDIA RTX 5090 + 9070 XT Cross-Device Findings

**Date:** 2026-08-29  
**Status:** `[~] GATED — Machine unavailable`  
**Reason:** No NVIDIA GPU detected on current measurement rig. Current rig has AMD Radeon RX 9060 XT, AMD Radeon RX 9070 XT, and Intel ARL iGPU only.

---

## Preconditions Check

| Precondition | Status | Details |
|---|---|---|
| RTX 5090 + 9070 XT machine physically accessible | ❌ FAIL | No NVIDIA GPU present (`lspci | grep -i nvidia` → no output; `nvidia-smi` → not found) |
| Proprietary driver installed (≥610 series) | ❌ N/A | No NVIDIA GPU to query |
| Lossless.dll reachable on that machine | ❌ N/A | No target machine |
| Build toolchain present | ❌ N/A | No target machine |

---

## Per-Direction Verdicts

| Direction | Verdict | Stage | Error / Notes |
|---|---|---|---|
| 5090 → 9070 XT | **UNTESTED** | — | Machine unavailable |
| 9070 XT → 5090 | **UNTESTED** | — | Machine unavailable |

---

## Required Captures (All Missing — Machine Unavailable)

| Artifact | Path | Status |
|---|---|---|
| `vulkaninfo` JSON (both GPUs) | `measurements/nvidia/vulkaninfo-5090.json`, `vulkaninfo-9070xt.json` | ❌ Missing |
| `vulkaninfo` extension check (VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier) | — | ❌ Missing |
| EXTERNAL_MEMORY_FEATURE_IMPORTABLE bits for dma-buf handle type | — | ❌ Missing |
| copybench runs (all directions, 1080p SDR) | `measurements/nvidia/copybench-*.log` | ❌ Missing |
| Exact error strings on failure | — | ❌ Missing |
| Driver version record | — | ❌ Missing |
| Reduced live matrix (success escalation) | — | ❌ Missing |

---

## Failure Taxonomy Mapping (Maintainer's Claims)

| Claim | Applicable? | Notes |
|---|---|---|
| "NVIDIA↔anything failed without GBM allocation (incredibly slow)" | Unknown | Cannot test without hardware |
| Unmerged DMA-BUF-less copy code in Discord | Unknown | Cannot verify |
| 610-series driver mmap-for-exported-dma-bufs improvement | Unknown | Driver version unknown |

---

## Next Steps

1. **Acquire access** to RTX 5090 + 9070 XT machine with proprietary NVIDIA driver ≥610
2. **Record driver version** via `nvidia-smi` and note whether ≥610 series
3. **Run vulkaninfo** on both GPUs, capture JSON, verify:
   - `VK_EXT_external_memory_dma_buf` present
   - `VK_EXT_image_drm_format_modifier` present
   - `VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT` for `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`
4. **Run copybench** in both directions at 1080p SDR:
   ```bash
   lsfg-vk-cli copybench --render-gpu "NVIDIA GeForce RTX 5090" --gpu "AMD Radeon RX 9070 XT" --width 1920 --height 1080 --iters 2000
   lsfg-vk-cli copybench --render-gpu "AMD Radeon RX 9070 XT" --gpu "NVIDIA GeForce RTX 5090" --width 1920 --height 1080 --iters 2000
   ```
5. **On failure**: Capture EXACT error strings + vulkaninfo JSON + driver version into `measurements/nvidia/`
6. **On success**: Escalate to reduced live matrix (both orders × 1080p/1440p × SDR × m2) with standard cell artifacts

---

## Traceability

All claims in this document are traceable to the precondition checks above. No measurements were performed due to hardware unavailability.