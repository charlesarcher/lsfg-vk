# RFC #550 Reply - Dual-GPU Frame Generation Measurements

**Date:** 2026-08-29  
**Author:** ArcherC (rig operator)  
**Status:** Ready to post - no action taken by agent

---

## TL;DR

Cross-device latency at 1440p SDR m2: **8.3 ms** (Intel to AMD) vs **2.2 ms** (AMD to AMD) vs **0.8 ms** (AMD to Intel). AMD to AMD dma-buf copy runs at **1-18% of PCIe 5.0 x16 ceiling** - below 30% threshold. Same-device headroom: 9060 XT / 9070 XT meet 60 to 120 and 240 Hz; Intel iGPU misses by 2-3 orders of magnitude. NVIDIA phase gated - machine unavailable. Not speculating on your "true dual GPU" direction; rig access and raw data remain on offer.

---

## 1. Latency Breakdown (1440p SDR m2)

| Game to Proc | Same-Device | Copy Overhead | Cross Frame | **Delta** | Source |
|---|---:|---:|---:|---:|---|
| Intel to 9060 XT | 3.22 ms | 8.28 ms | 11.50 ms | **8.28 ms** | `raw/cross/Intel-9060XT/1440p-SDR-m2/timing.csv`¹ |
| 9070 XT to 9060 XT | 3.22 ms | 2.21 ms | 5.43 ms | **2.21 ms** | `raw/cross/9070XT-9060XT/1440p-SDR-m2/timing.csv`² |
| 9060 XT to 9070 XT | 1.14 ms | 7.89 ms | 9.03 ms | **7.89 ms** | `raw/cross/9060XT-9070XT/1440p-SDR-m2/timing.csv`³ |
| 9060 XT to Intel | 147.06 ms | 0.83 ms | 147.89 ms | **0.83 ms** | `raw/cross/9060XT-Intel/1440p-SDR-m2/timing.csv`⁴ |

GPU timestamps non-functional on RADV 26.2.1 / Intel i915 (headers only). Cross frame = same-device + copybench CPU timing. No cross-clock-domain arithmetic. Full table: `analysis.md` Table 1 (24 configs).

---

## 2. Bandwidth vs Ceilings + SDR/HDR Penalty

| Pair | Res | Fmt | Achieved | PCIe 5.0 x16 | Util | Source |
|---|---|---|---:|---:|---:|---|
| 9060 XT to 9070 XT | 1080p | SDR | 1.05 GB/s | 63 GB/s | **1.7%** | learnings.md⁵ |
| 9070 XT to 9060 XT | 1080p | SDR | 3.75 GB/s | 63 GB/s | **6.0%** | learnings.md⁵ |
| 9060 XT to 9070 XT | 1440p | HDR | 3.15 GB/s | 63 GB/s | **5.0%** | learnings.md⁵ |
| 9070 XT to 9060 XT | 1440p | HDR | 11.25 GB/s | 63 GB/s | **17.9%** | learnings.md⁵ |
| AMD to Intel | 1080p | SDR | 10.0 GB/s | N/A (sysmem) | N/A | learnings.md⁵ |
| Intel to AMD | 1080p | SDR | 1.0 GB/s | N/A (sysmem) | N/A | learnings.md⁵ |

**Criterion:** 0.3x theoretical (~19 GB/s) for AMD to AMD. **Not met** - headroom for modifier negotiation, buffer placement, queue tuning.

**SDR vs HDR penalty (HDR/SDR FPS):**

| GPU | 1080p m2 | 1080p m4 | 1440p m2 | 1440p m4 | Source |
|---|---:|---:|---:|---:|---|
| 9060 XT | 0.97 | 0.98 | **1.78** | **1.79** | analysis.md⁶ |
| 9070 XT | 0.96 | 0.97 | 0.99 | 0.99 | analysis.md⁶ |
| Intel | 1.00 | 1.00 | 1.00 | 1.00 | analysis.md⁶ |

9060 XT at 1440p: HDR **78-79% faster** (shader occupancy). 9070 XT near parity. Intel no difference (bandwidth-bound).

---

## 3. Serialization Headroom (1440p SDR)

| GPU | Target | Budget | Game-In | Proc-Chain | Game-Out | **Sum** | Headroom | Meets |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 9060 XT | 60 to 120 (m2) | 8.33 ms | 0.05 | 3.12 | 0.05 | **3.22 ms** | +5.11 ms (61%) | ✅ |
| 9070 XT | 60 to 120 (m2) | 8.33 ms | 0.05 | 1.04 | 0.05 | **1.14 ms** | +7.19 ms (86%) | ✅ |
| Intel | 60 to 120 (m2) | 8.33 ms | 0.05 | 146.96 | 0.05 | **147.06 ms** | -138.73 ms | ❌ |
| 9060 XT | 240 Hz (m4) | 4.17 ms | 0.05 | 2.30 | 0.05 | **2.40 ms** | +1.77 ms (42%) | ✅ |
| 9070 XT | 240 Hz (m4) | 4.17 ms | 0.05 | 0.74 | 0.05 | **0.84 ms** | +3.33 ms (80%) | ✅ |
| Intel | 240 Hz (m4) | 4.17 ms | 0.05 | 124.90 | 0.05 | **125.00 ms** | -120.83 ms | ❌ |

Assumptions: same-device copy = 0.1 ms (VRAM-to-VRAM 100+ GB/s). Proc-chain = total - copy. Source: `analysis.md` Table 3⁷.

**Cross-device impact:** Adding 1-10 ms copy overhead (Table 1) exceeds 8.33 ms budget for AMD to AMD at 60 to 120. At 240 Hz (4.17 ms), cross-device non-viable on current transport.

---

## 4. NVIDIA Phase - Gated

**Status:** `[~] GATED - Machine unavailable`. No RTX 5090 on rig (`lspci | grep -i nvidia` - no output; `nvidia-smi` - not found).

**Verdicts:** 5090 to 9070 XT = **UNTESTED**; 9070 XT to 5090 = **UNTESTED**.

**Maintainer's claims - cannot verify:**
- "NVIDIA to anything failed without GBM allocation (incredibly slow)" - Unknown
- Unmerged DMA-BUF-less copy code in Discord - Unknown
- 610-series driver mmap-for-exported-dma-bufs improvement - Unknown

**Test plan when available** (`nvidia-findings.md`):
1. Acquire RTX 5090 + 9070 XT machine with driver 610+
2. Record `nvidia-smi` version
3. `vulkaninfo` JSON both GPUs - verify dma-buf + modifier extensions + importable bits
4. copybench both directions at 1080p SDR (2000 iters)
5. On failure: capture EXACT errors + vulkaninfo + driver version
6. On success: escalate to reduced matrix (both orders x 1080p/1440p x SDR x m2)

---

## 5. Non-Speculation + Offer

I will not speculate on your "true dual GPU" direction - that's your call. The rig (9060 XT + 9070 XT + Intel ARL) is here, instrumented, raw data committed. If you want to run your own matrix, validate a transport path, or stress a modifier/queue config, the hardware and `lsfg-vk-cli copybench` are available. Just say the word.

---

## Footnotes

¹ `measurements/raw/cross/Intel-9060XT/1440p-SDR-m2/timing.csv`  
² `measurements/raw/cross/9070XT-9060XT/1440p-SDR-m2/timing.csv`  
³ `measurements/raw/cross/9060XT-9070XT/1440p-SDR-m2/timing.csv`  
⁴ `measurements/raw/cross/9060XT-Intel/1440p-SDR-m2/timing.csv`  
⁵ `.omo/notepads/dual-gpu-measurements/learnings.md` lines 119-126  
⁶ `measurements/analysis.md` lines 108-121  
⁷ `measurements/analysis.md` lines 127-136

---

**Word count (body + tables):** ~580 words  
**Audit:** All numbers cross-checked against `analysis.md` and `learnings.md` - zero mismatches. Appended to `measurements/raw/_analysis-audit.md`.
