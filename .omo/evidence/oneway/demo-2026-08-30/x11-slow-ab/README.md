# Why is the X11 backend slower? — A/B evidence (2026-08-30)

Question: the 2026-08-30 demo showed 57 fps game / 114 fps presented with the
X11 app backend but 150 / 300 with the Wayland backend (same vkcube 500x500
game process, same GPUs, same monitor @ 240 Hz).

A/B (ab.sh): identical run with the X11 app window created WITHOUT the EWMH
fullscreen request (LSFGVK_APP_NO_FS=1, the new parity debug knob) vs WITH it:

| run | cycles | per-second stats |
|---|---|---|
| x11 with fullscreen | 2,006 | 57 / 114 (stable, 6 samples) |
| x11 without fullscreen | 2,020 | 57 / 115 (stable, 6 samples) |

=> the WM fullscreen handling of the X11 client is NOT the cost; the X11
result is unchanged with or without it. The cost is the X server / XWayland
round-trip of the app's own swapchain: every acquire/present/release of the
app window goes through the X server (DRI3 present, release event back via
XWayland), ~8.8 ms per present vs ~3.3 ms for the native wayland toplevel
(release arrives as a direct wl_buffer event, no X hop).

Because the staging ring is 2 slots deep and the layer's present hook waits
for a free slot (500 ms bound) before forwarding the game's present, the
app's per-cycle latency backpressures the game: the identical game process
runs at its natural ~150 fps when the app is Wayland and is throttled to
~57 fps when the app is X11. Practical consequence on this rig: the Wayland
backend is the higher-headroom session (present capacity ~300 fps vs ~115)
- e.g. a 60 fps game doubled needs 120 presented fps, which only the Wayland
app loop sustains here at multiplier 2.
