# Lossless Scaling (LSFG) UI Design & Architecture Guide

This document captures the visual layout, interaction paradigms, styling tokens, and control hierarchies of the official Lossless Scaling Windows UI (version 3.x series) as referenced from community operational guides and direct visual inspection of the interface. It serves as an authoritative design reference for implementing a native Linux / Wayland / X11 control interface (e.g. Qt, GTK4, or Slint) for `lsfg-vk`.

---

## 1. Design System & Theming Tokens

The interface follows modern Windows Fluent Design / WinUI 3 principles with a dark-mode palette and purple accent styling.

### 1.1 Color Palette
- **Window Background**: Dark Slate / Charcoal (`#1a1d24` to `#20242c`).
- **Surface / Card Background**: Elevated Slate (`#282c35`).
- **Input / Dropdown Fill**: Recessed Surface (`#21252d` with hover `#2d323c`).
- **Borders & Dividers**: Low-contrast subtle outlines (`rgba(255, 255, 255, 0.08)`).
- **Primary Text**: High-contrast white/off-white (`#f0f2f5`).
- **Secondary / Label Text**: Muted slate (`#9ba1b0`).
- **Accent Active / Focus**: Vivid Purple / Magenta (`#8a2be2` / `#9b30ff`).
  - Active toggle pills: Filled purple `#8a2be2` with crisp white thumb.
  - Inactive toggle pills: Muted dark grey `#383e4a` with soft white/grey thumb.
  - Slider track / thumb: Purple thumb with gradient or active fill along the track.

### 1.2 Typography & Spacing
- **Font Family**: Segoe UI Variable / Inter / Roboto Sans.
- **Card Titles**: Bold 16–18pt (`font-weight: 600`), white.
- **Control Labels**: Medium 13–14pt (`font-weight: 400`), muted grey, left-aligned within card.
- **Control Elements**: Right-aligned, fixed width per type (comboboxes ~140–180px, spinners ~80–100px).
- **Grid Structure**: Two equal-width columns (Left and Right), with stacked modular cards separated by a standard 12–16px gap.

---

## 2. Card Hierarchy & Control Specifications

```
+------------------------------------+------------------------------------+
| LEFT COLUMN                        | RIGHT COLUMN                       |
+------------------------------------+------------------------------------+
| [Card 1] Frame Generation          | [Card 5] Scaling                   |
| [Card 2] Capture                   | [Card 6] Rendering                 |
| [Card 3] Cursor                    | [Card 7] GPU & Display             |
| [Card 4] Crop Input                | [Card 8] Behavior                  |
+------------------------------------+------------------------------------+
```

---

### Column 1: Core Processing & Input

#### Card 1: Frame Generation
Primary optical flow and frame interpolation engine controls.
- **`Type`** *(Dropdown Combobox)*:
  - Options: `LSFG 3.x` (Default), `LSFG 2.x`, `LSFG 1.x`, `Off`.
  - Maps to: Pipeline shader generator version in `Lossless.dll`.
- **`Mode`** *(Dropdown Combobox)*:
  - Options: `Fixed` (Default), `Fractional`, `Adaptive`.
  - Description: Fixed maintains constant multiplier; Adaptive dynamically varies generated frames to hit monitor refresh rate without judder.
- **`Multiplier`** *(Numeric Spinner $\wedge / \vee$)*:
  - Values: `2`, `3`, `4` (Default: `2` or `3`).
  - Corresponds to: Target frame multiplication ratio (2x, 3x, 4x).
- **`Flow scale`** *(Horizontal Slider)*:
  - Range: Continuous scale with draggable purple circular thumb.
  - Description: Controls optical flow downsampling factor. Higher values reduce vector estimation artifacts at the cost of higher GPU compute overhead.
- **`Performance`** *(Toggle Switch)*:
  - States: `On` / `Off` (Default: `Off`).
  - Description: Switches to lightweight compute shaders with lower sample counts (critical for integrated GPUs like Intel ARL or lower-tier discrete GPUs).

#### Card 2: Capture
Frame ingestion mechanism from the rendering application.
- **`Capture API`** *(Dropdown Combobox)*:
  - Options: `WGC` (Windows Graphics Capture, Win11 24H2 default), `DXGI` (Desktop Duplication API).
  - Linux `lsfg-vk` equivalent: `Vulkan Layer Hook` (in-process `vkQueuePresentKHR`), `Wayland DMABUF / PipeWire`, `X11 Composite`.
- **`Queue target`** *(Numeric Spinner $\wedge / \vee$)*:
  - Values: `0` (Unbuffered / minimal latency), `1` (Balanced 1-frame staging ring), `2` (2-frame fallback).
  - Linux equivalent: `STAGING_RING_DEPTH` (our tuned default of 4 for decoupled DMA).

#### Card 3: Cursor
Mouse pointer confinement, scaling, and synchronization.
- **`Clip cursor`** *(Toggle Switch)*: `On` / `Off` — Confines the pointer inside the scaled output rectangle.
- **`Adjust cursor speed`** *(Toggle Switch)*: `On` / `Off` — Compensates mouse sensitivity when rendering at lower internal resolution.
- **`Hide cursor`** *(Toggle Switch)*: `On` / `Off` — Suppresses native pointer during scaling.
- **`Scale cursor`** *(Toggle Switch)*: `On` / `Off` — Upscales cursor bitmap when spatial scaling is active.

#### Card 4: Crop Input
Bounding box adjustment to exclude letterboxes, black bars, or extraneous UI elements.
- **Visual Crosshair Widget**: A central rectangular aperture with four directional numeric input fields:
  - **Top**: `[ 0 ] px`
  - **Bottom**: `[ 0 ] px`
  - **Left**: `[ 0 ] px`
  - **Right**: `[ 0 ] px`

---

### Column 2: Presentation, Scaling & Topology

#### Card 5: Scaling
Spatial upscaling configuration (applied before or after frame generation).
- **`Type`** *(Dropdown Combobox)*:
  - Options: `Off` (Default), `LS1` (Lossless Scaling proprietary), `AMD FSR`, `NVIDIA NIS`, `Integer`, `Bicubic`, `Bilinear`, `Anime4K`.
- **`Mode`** *(Dropdown Combobox)*:
  - Options: `Auto`, `Custom`.
- **`Sub-Mode / Aspect Ratio`** *(Dropdown Combobox)*:
  - Options: `Fullscreen`, `Aspect Ratio`.
  - When `Custom` is selected: Exposes **`Scale Factor`** spinner (e.g. `x1.20`, `x1.33`, `x1.50`).

#### Card 6: Rendering
Synchronization, frame pacing, and diagnostic overlay controls.
- **`Sync mode`** *(Dropdown Combobox)*:
  - Options: `Off (Allow tearing)` (Default for minimal latency), `VSync`, `Adaptive`.
  - Maps to: Vulkan `VkPresentModeKHR` (`IMMEDIATE`, `FIFO`, `MAILBOX`).
- **`Max frame latency`** *(Numeric Spinner $\wedge / \vee$)*:
  - Values: `1` to `10` (Default: `10` on Windows for buffer pacing headroom).
- **`HDR support`** *(Toggle Switch)*: `On` / `Off` — Enables wide color gamut / FP16 swapchain pass-through.
- **`G-Sync support`** *(Toggle Switch)*: `On` / `Off` (Default: `On` with purple active pill).
- **`Draw FPS`** *(Toggle Switch)*: `On` / `Off` (Default: `On`).
  - Visual output: Draws overlay in the top-left showing `Base FPS / Doubled FPS` (e.g. `144 / 288 FPS`).
  - Linux equivalent: `MangoHud` integration or native `lsfg-vk` overlay HUD.

#### Card 7: GPU & Display
Device topology and adapter routing (crucial for dual-GPU offload architectures).
- **`Preferred GPU`** *(Dropdown Combobox)*:
  - Options: `Auto`, or specific hardware adapters:
    - *Discrete GPU A* (e.g. `AMD Radeon RX 9070 XT`)
    - *Discrete GPU B* (e.g. `AMD Radeon RX 9060 XT`)
    - *Integrated GPU* (e.g. `Intel(R) Graphics (ARL)`)
  - Linux `lsfg-vk` equivalent: `gpu` selection in `~/.config/lsfg-vk/conf.toml` and PCI device selection.
- **`Output display`** *(Dropdown Combobox)*:
  - Options: `Auto`, `Display 1 (DP-1)`, `Display 2 (HDMI-1)`.
  - Controls which connector/CRTC receives the generated swapchain presentation.

#### Card 8: Behavior
Desktop environment and window management integration.
- **`Multi-display mode`** *(Toggle Switch)*: `On` / `Off` (Default: `Off`).
  - Controls whether presentation spans displays and how cursor confinement behaves across multi-monitor setups.

---

## 3. Mapping to `lsfg-vk` Configuration

For developers building a Linux frontend or CLI configuration, the UI elements map directly to `conf.toml` keys:

| UI Section | UI Control | `conf.toml` / CLI Equivalent |
| :--- | :--- | :--- |
| **Frame Generation** | `Type` | `dll` selection / shader model |
| **Frame Generation** | `Multiplier` | `multiplier = 2` (or 3, 4) |
| **Frame Generation** | `Performance` | `performance_mode = true` |
| **Frame Generation** | `Flow scale` | Optical flow resolution factor |
| **Capture** | `Queue target` | `STAGING_RING_DEPTH` (e.g. 4) |
| **Scaling** | `Type` | `scale_mode` (`none`, `fsr`, `nis`, `integer`) |
| **Rendering** | `Sync mode` | `present_mode` (`immediate`, `fifo`, `mailbox`) |
| **Rendering** | `Draw FPS` | `show_fps = true` / `MANGOHUD=1` |
| **GPU & Display** | `Preferred GPU` | `gpu = "AMD Radeon RX 9060 XT..."` or `"Intel(R) Graphics (ARL)"` |
| **GPU & Display** | `Output display` | Wayland `wl_output` / X11 screen assignment |
