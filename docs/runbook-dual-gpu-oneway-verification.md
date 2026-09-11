# Dual-GPU Frame Generation: Performance & Functional Verification Runbook

This runbook specifies the architecture, measurement protocols, failure modes, and pass/fail criteria required for any agent to reproduce the dual-GPU frame-doubling pipeline and verify both performance and functional correctness.

---

### 1. Architectural Architecture & Invariants

```
[ Game / App (Card A: RX 9070 XT) ]
      │  (Single VkDevice only; never instantiate Card B in game process)
      ▼  vkQueuePresentKHR hook
[ Host-Visible Buffer ] (posix_memalign, 4K page-aligned)
      │  Bound via VK_EXT_external_memory_host (userptr)
      ▼  PCIe DMA Write (~1.98 ms)
[ Staging Ring ] (STAGING_RING_DEPTH = 4)
      │  Decoupled; NO cross-device dma-buf GEM objects
      ▼  PCIe DMA Read (~1.98 ms)
[ Doubler App (Card B: RX 9060 XT) ]
      │  Optical flow + generation shaders
      ▼  WSI Swapchain Presentation
[ High-Refresh Display (DP-7 @ 240 Hz) ]
```

#### Core Invariants
- **Zero Kernel Hacks & No CS-Strip**: No kernel patches, no `LD_PRELOAD` ioctl interception, and no stripping of DRM submission BO lists.
- **Single `VkDevice` in Game Process**: Direct3D 12 translation layers (`vkd3d-proton`) crash with `VK_ERROR_DEVICE_LOST (-8)` if a secondary `VkDevice` is created in the game process. Card B must be controlled strictly inside `lsfg-vk-app`.
- **Decoupled Userptr Memory**: Both GPUs independently map the same physical host allocations. Avoiding cross-device dma-buf sharing prevents kernel `dma_resv` implicit read-fence stalls.
- **Ring Depth Tuning**: Ring depth 2 provides insufficient buffering headroom against Card B compute variance (causes sawtooth drops); depth 8 adds excessive queue buffering latency (~39 ms). **Depth 4** is the optimal operating point (4.52 ms p50 latency, 99.2% throughput).

---

### 2. Instrumentation & Latency Measurement

Click-to-display latency is measured using hardware monotonic timestamps:
1. **Timestamping**: Capture `clock_gettime(CLOCK_MONOTONIC_RAW)` at the exact point of hardware capture on Card A (`capture_context.cpp`) and propagate `captureTsNs` through the IPC frame header.
2. **Measurement Point**: Compute $\Delta t = t_{\text{present}} - t_{\text{capture}}$ inside `presentation.cpp` immediately prior to `vkQueuePresentKHR`.
3. **Statistical Breakdown**:
   - **Real Frames**: Measures input-to-photon delivery of the original rendered frame.
   - **Generated Frames**: Measures latency of the interpolated intermediate frame.
   - **Pass Criteria**:
     - REAL Frame Latency: $p_{50} \le 5.0\text{ ms}$ (Decoupled DMA achieves $\sim 4.52\text{ ms}$).
     - GEN Frame Latency: $p_{50} \le 1.0\text{ ms}$ (Decoupled DMA achieves $\sim 0.50\text{ ms}$).

---

### 3. Execution & Verification Workflow

#### Phase A: Synthetic Benchmark (FurMark 1440p)
Run the automated harness to verify throughput and pacing without display server or Wine overhead:
```bash
cd /home/archerc/code/lsfg-vk
./test_furmark.sh doubled 8 bound
killall -9 lsfg-vk-app 2>/dev/null || true
```
**Verification Checks**:
- Native Render Rate (9070 XT): $\ge 134\text{ FPS}$ (Target $\sim 138\text{--}142\text{ FPS}$).
- Presentation Rate (9060 XT): Locked $2\times$ rate ($\ge 276\text{ presents/s}$).
- Transfer Efficiency: $\ge 99\%$ (zero or near-zero skips).
- MangoHud Graph: Completely flat horizontal baseline ($\sim 3.8\text{ ms}$).

#### Phase B: Complex Game Pipeline (Resident Evil 2 via Steam)
Verify DirectX 12 / vkd3d-proton compatibility, resolution transitions, and optical flow quality:
1. **Launch via Wrapper**:
   ```bash
   DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 steam steam://rungameid/883710
   ```
2. **Handle Swapchain Reconfiguration**:
   - RE2 boots in a 1920×1080 splash window. The doubler must defer fullscreen overlay creation (`boot1080=1`) to prevent blocking input focus.
   - Upon clicking into Story/Continue, the swapchain recreates at 2560×1440. The layer must retain the isolated swapchain context across the transition without triggering `vkd3d` device loss.
3. **In-Game Movement (Mizoil Gas Station)**:
   - Exercise WASD motion while capturing frame dumps (`LSFGVK_DUMP_PRESENT=1`).
   - Sample 1.0s presentation rates: Verify $\sim 330\text{ REAL} + 332\text{ GEN} = 662\text{ presents/s}$ with 0 skips.

---

### 4. Visual Artifact Inspection Protocol

When inspecting frame captures (`vision_analyze` on dumped PPM/PNG images):
1. **Double Silhouette / Ghosting**: Inspect high-contrast character boundaries (Leon's head, hair, and shoulders against dark backgrounds). There must be exactly one outline; optical flow misprediction manifests as a secondary translucent skull or trailing ghost.
2. **Edge Halos / Warping**: Look for blur or pixel tearing along depth discontinuities between foreground characters and background geometry.
3. **HUD Stability**: Verify that 2D static elements (interaction prompts, reticle, MangoHud) remain crisp and are not distorted by motion vector estimation.

---

### 5. Troubleshooting & Triage Rules

| Symptom | Root Cause | Remediation |
| :--- | :--- | :--- |
| `vkQueueSubmit vr -8` / Device Lost in Wine | Secondary `VkDevice` running inside the game client process | Remove secondary device from layer; execute Card B DMA inside `lsfg-vk-app` only. |
| Transfer locked to ~33 ms / ~30 presents/s | Foreign `dma_resv` read-fence implicit synchronization on shared dma-buf GEMs | Use decoupled host-visible memory import (`VK_EXT_external_memory_host`) instead of dma-buf fd exchange. |
| Alternating frame drops / sawtooth MangoHud | Insufficient ring buffering for Card B's generation pipeline | Increase `STAGING_RING_DEPTH` from 2 to 4 and ensure per-slot command buffers are asynchronous. |
| Black screen on 1080p game boot | Exclusive fullscreen Wayland overlay covering the splash window before input focus | Defer WSI overlay creation until native resolution (1440p) stream initializes. |
