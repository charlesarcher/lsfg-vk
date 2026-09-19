# tools/latency — Session 40 instrumentation

## probe_latency — click→photon ground truth (per-click, human-run)

Measures evdev click time → wp_presentation latch on THIS desktop. All clocks
CLOCK_MONOTONIC (input device switched via EVIOCSCLOCKID; KWin presents in
clock_id=1). The compositor + present-path floor ONLY (not the in-game engine);
every layer/app delta you measure in-game adds ONTO this number.

Build (needs the app's generated protocol headers):
    cd ~/code/lsfg-vk
    gcc -O2 -o /tmp/probe_latency tools/latency/probe_latency.c \
        build/protocols/xdg-shell-protocol.c \
        build/protocols/presentation-time-protocol.c \
        -Ibuild/protocols $(pkg-config --cflags --libs wayland-client) -lpthread

Run (from a desktop session; click the mouse):
    WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 /tmp/probe_latency 320 180 60

Output: per-click p50/p99/min/max in ms. Sixty clicks ≈ 20 s.

## Latency HUD (app side)

Second HUD row (7-segment, under the fps row) shows `<ipc>+<solve>+<scan>ms`:
- ipc = capture→wire-recv hop (layer→app, segment E+F)
- solve = app schedule/submit window (segment G)
- scan = KWin compositor hold (submit→latch, segment H — wp_presentation v2)
Blank until each segment has measured data. The opt-in experience row
(click→photon p50/p99 + GEN adds) rides the same mechanism, fed by this probe
once calibrated; enabled via lsfg-vk-ui or conf.toml when wired.
