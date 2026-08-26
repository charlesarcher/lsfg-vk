# Rig Display Topology — feat/dual-gpu-oneway task 1

Captured: 2026-08-26 · branch `feat/dual-gpu-oneway` · product tree at tested v10 state (zero product changes)

## Session / Display Server

| Item | Value |
|---|---|
| `$XDG_SESSION_TYPE` | `wayland` |
| `$WAYLAND_DISPLAY` | `wayland-0` |
| `$DISPLAY` | `:0` (Xwayland) |
| Desktop | KDE Plasma (`XDG_SESSION_DESKTOP=KDE`, KWin compositor) |

## Monitor Enumeration

- `kscreen-doctor -o`: Output 1 `HDMI-A-3`, enabled, connected, priority 0, mode 3840x2160@60.00*, scale 1.7, HDR off, VRR never.
- `xrandr --listactivemonitors` (against Xwayland): 1 monitor, `HDMI-A-3` 3840x2160+0+0.

## DRM card → Vulkan deviceName mapping

Only one connector is enabled: `/sys/class/drm/card3-HDMI-A-3/status = connected`. All card1/card2 connectors disconnected.

| DRM card | PCI vendor:device | Vulkan # | deviceName | Role |
|---|---|---|---|---|
| card1 (`0000:00:02.0`) | `0x8086:0x7d67` | GPU2 | Intel(R) Graphics (ARL) | iGPU |
| card2 (`0000:04:00.0`) | `0x1002:0x7550` | GPU1 | AMD Radeon RX 9070 XT (RADV GFX1201) | dGPU |
| **card3** (`0000:87:00.0`) | `0x1002:0x7590` | GPU0 | AMD Radeon RX 9060 XT (RADV GFX1200) | **display card** |

Mapping method: `/sys/class/drm/cardN/device/{vendor,device}` cross-checked against `vulkaninfo --summary` vendorID/deviceID. Matches the plan's known-device table exactly.

## Chosen E2E pairing

- **Display-card / PRESENTING side (GPU B) = RX 9060 XT (card3, HDMI-A-3)** — owns the KWin scanout.
- **Game GPUs (candidates for GPU A): RX 9070 XT (card2)** and **Intel ARL (card1)**.
- Primary two-way cell exercised in baselines below: game on **Intel ARL**, processing on **9070 XT** (both non-display cards; display card fully out of the processing path).

## WM-coverage test matrix verdict

| Cell | Verdict | Method |
|---|---|---|
| app-X11-backend | ✅ natively testable today | Run under Xwayland (`DISPLAY=:0`) on KWin; verified working via `xrandr` against Xwayland. Command shape: `DISPLAY=:0 VK_LAYER_PATH=/tmp/opencode/layer-test VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation LSFGVK_ENV=1 LSFGVK_GPU=<proc> LSFGVK_DLL_PATH=<dll> <app>`. |
| app-Wayland-backend | ✅ natively testable today | The session itself is KDE Wayland; Baseline 2 (`vkcube --present_mode fifo`, "Selected WSI platform: wayland") proves the Wayland path live. |
| nested weston / cage fallback | ☐ not needed | Neither `weston` nor `cage` installed on this rig (noted per instructions, nothing installed). Only relevant if KWin-specific quirks appear later; a second TTY with a bare wl-roots compositor would be the manual fallback. |

## Two-way baselines (plan F1a) — evidence

Logs: `.omo/evidence/oneway/baseline-twoway-debug.log`, `.omo/evidence/oneway/baseline-twoway-vkcube.log`

### Baseline 1 — debug-tool cross-device
- Binary confirmed at `./build/lsfg-vk-cli/lsfg-vk-cli` (727 KB, built Aug 25).
- CLI syntax matched spec (`debug -d -w -h -m -r <folder>`); `--help` unsupported → used usage text.
- **Finding:** native CLI ignores `LSFGVK_GPU` (layer-only env). Run 1 defaulted to GPU0 (9060 XT); rerun pinned intended pairing via `-g "AMD Radeon RX 9070 XT (RADV GFX1201)"`. Both appended to the log.
- Success lines present:
  - `processing on '00000000870000000000000000000000' (game on 'Intel(R) Graphics (ARL)')` (run 1, 9060XT)
  - `processing on '00000000040000000000000000000000' (game on 'Intel(R) Graphics (ARL)')` (run 2, 9070XT)
- exit=0 both runs · dma-buf: yes · drm-modifier-images: yes · frames wait ok 1–4.

### Baseline 2 — live vkcube cross-device (~15 s)
- `vkcube --gpu_number 2 --present_mode fifo` + layer env; WSI platform: wayland; game GPU = Intel ARL.
- Success line: `lsfg-vk: processing on '00000000040000000000000000000000' (game on 'Intel(R) Graphics (ARL)')`.
- exit=124 (= timeout SIGINT after 15 s, expected for a live app) · extensions enabled incl. `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`.

### Validation noise counts (`grep -cE 'Validation Error|VUID'`)
| Log | Count |
|---|---|
| baseline-twoway-debug.log | **0** |
| baseline-twoway-vkcube.log | **0** |

(Benign `radv is not a conformant Vulkan implementation` warnings present in vkcube log — not validation errors.)
