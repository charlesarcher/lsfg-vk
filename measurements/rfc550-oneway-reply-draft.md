# RFC #550 Reply: One-Way Dual-GPU Frame Generation

## Architecture

We built **X** — a thin capture-only Vulkan layer + standalone presentation app (`lsfg-vk-app`). The layer hooks `vkQueuePresentKHR`, captures the game frame, exports it via dma-buf, and hands off to the app over a Unix socket. The app imports on the processing GPU, runs frame generation, and presents directly to the display.

This matches your confirmed transport: **"same external memory sharing"** — dma-buf fds cross processes at render cadence. The only emulation we needed was making the game's swapchain images exportable (proven pattern: `obs-vkcapture`). No WSI emulation anywhere.

## Traffic-Shape Proof (Task 11)

| Metric | Game GPU (Intel ARL) | Proc GPU (RX 9070 XT) |
|--------|---------------------|----------------------|
| Engine activity | rcs0 +5,923 ms (9.9%) | 40% GFX + 53% VCN |
| Outbound DMA/blit | **ZERO** (bcs0/vcs0 = 0) | Active (gen + present) |
| dma-buf role | Exporter only | Importer + presenter |
| Return traffic (B→A) | **NONE** | — |

**Two-way control (same hardware):** Game GPU rcs0 identical (+5,923 ms), but **return traffic ACTIVE** — every displayed frame copied back A←B. At multiplier *m*, return ≈ *m*× forward, serialized on the same link. Your "atrocious at high multipliers" is exactly this.

## Direct Answer to Your Question

> *"How does your approach differ from the two-way branch?"*

Two-way: render A → process B → copy back A → present A (bidirectional dma-buf).  
One-way X: render A → export → process B → **present B** (unidirectional dma-buf, A never imports).

The layer stays minimal; the app owns the B-side swapchain and presentation. Backend library (`liblsfg-vk`) is frozen — descriptor transport unchanged.

## Upstream Interest?

Two questions:

1. **Frozen backend descriptor transport** — the layer/app handshake (dma-buf fd + metadata over SCM_RIGHTS) is stable and versioned. Does this interest upstream as a reusable cross-process frame transport?

2. **App topology** — native X11 (xcb) + native Wayland (xdg-shell) backends in the app, XWayland demoted to compat note. Full WM coverage achieved. Worth standardizing?

## Rig Data Available

- KDE Plasma (KWin) on Wayland, XWayland on `:0`
- Display: RX 9060 XT (card3, HDMI-A-3, 4K@60)
- Game GPUs: Intel ARL (card1), RX 9070 XT (card2)
- All Task 11 artifacts: engine deltas, pm_info snapshots, app/layer logs, OOOLS resize recovery

## What We're Not Claiming

- HDR works (KWin reports HDR off; colorspace negotiation unvalidated)
- X11 backend fully automated (test harness needs `XAUTHORITY`; manual verification passes)
- Any speculation on your internals

## Two-Way Status

Two-way remains selectable locally via `presentation = "game"` (default unchanged). It is **not** the proposed contribution.

---

**Offer:** Happy to share full measurement artifacts, run additional cells, or spin up a test session on the rig. No commitments beyond data/access.