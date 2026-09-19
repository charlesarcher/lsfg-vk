# tools/latency — Session 40 instrumentation

## probe_latency — click→photon ground truth (per-click, human-run)

Measures evdev click time → wp_presentation latch on THIS desktop. All clocks
CLOCK_MONOTONIC (input device switched via EVIOCSCLOCKID; KWin presents in
clock_id=1). The compositor + present-path floor ONLY (not the in-game engine);
every layer/app delta you measure in-game adds ONTO this number.

Build (app-generated protocol headers live under build/lsfg-vk-app/protocols;
regenerate standalone copies safely if absent):
    cd ~/code/lsfg-vk
    mkdir -p /tmp/pt && cp build/lsfg-vk-app/protocols/{xdg-shell,presentation-time}-*.c /tmp/pt/ 2>/dev/null || \
      true
    ls build/lsfg-vk-app/protocols/xdg-shell-protocol.c >/dev/null 2>&1 || {
      wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /tmp/pt/xdg-shell-protocol.c
      wayland-scanner private-code /usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml /tmp/pt/presentation-time-protocol.c
    }
    gcc -O2 -o /tmp/probe_latency tools/latency/probe_latency.c \
        $(ls build/lsfg-vk-app/protocols/xdg-shell-protocol.c \
            build/lsfg-vk-app/protocols/presentation-time-protocol.c \
            2>/dev/null || printf '/tmp/pt/xdg-shell-protocol.c /tmp/pt/presentation-time-protocol.c') \
        -I$( [ -f build/lsfg-vk-app/protocols/xdg-shell-client-protocol.h ] && echo build/lsfg-vk-app/protocols || echo /tmp/pt ) \
        $(pkg-config --cflags --libs wayland-client) -lpthread

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
