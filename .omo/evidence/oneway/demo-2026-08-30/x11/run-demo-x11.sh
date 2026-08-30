#!/usr/bin/env bash
# X11-backend one-way frame-doubling demo (9070 XT render / 9060 XT doubler+display).
# Usage: run-demo-x11.sh
set -u
EV="$(dirname "$(readlink -f "$0")")"
APP=/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app
CONF=$HOME/.config/lsfg-vk/conf.toml
XAUTH=/run/user/1000/xauth_OKuvCG

# 1. start the doubler app (X11 backend, verbose)
LSFGVK_CONFIG=$CONF XAUTHORITY=$XAUTH nohup $APP --profile app-oneway --session x11 -v \
    > "$EV/app.log" 2>&1 &
echo $! > "$EV/app.pid"
sleep 3
echo "--- app banner ---"; head -8 "$EV/app.log"
echo "--- socket ---"; ss -lxp | grep app.sock || echo "NO SOCKET"

# 2. start the game on the 9070 XT (headless) with the layer
VK_LAYER_PATH=/tmp/opencode/layer-test \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_CONFIG=$CONF \
MESA_VK_DEVICE_SELECT=1002:7550 \
nohup vkcube --present_mode fifo --wsi xcb --suppress_popups > "$EV/game.log" 2>&1 &
echo $! > "$EV/game.pid"
sleep 5
echo "--- game log (first lines) ---"; head -12 "$EV/game.log"

# 3. mid-run artifacts
sleep 20
spectacle -b -n -o "$EV/shot-t25.png"
echo "--- engine busy (doubler=card3, game=card2) ---"
echo "doubler_9060XT=$(cat /sys/class/drm/card3/device/gpu_busy_percent)% game_9070XT=$(cat /sys/class/drm/card2/device/gpu_busy_percent)%" \
    | tee "$EV/engine-midrun.txt"

# 4. geometry of the app window (EWMH: FULLSCREEN|ABOVE at (0,0) 2560x1440)
{
    echo "timestamp: $(date -Is)"
    /tmp/opencode/geom
} > "$EV/geom.txt" 2>&1
cat "$EV/geom.txt"
spectacle -b -n -o "$EV/shot-t40.png"

# 5. SIGINT the app and time the exit
python3 - "$EV" <<'PYEOF'
import os, sys, time, signal
ev = sys.argv[1]
pid = int(open(os.path.join(ev, "app.pid")).read().strip())
t0 = time.perf_counter()
os.kill(pid, signal.SIGINT)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass
ms = (time.perf_counter() - t0) * 1000
with open(os.path.join(ev, "sigint-start"), "w") as f:
    f.write(f"{t0:.9f}\n")
with open(os.path.join(ev, "sigint-end"), "w") as f:
    f.write(f"{time.perf_counter():.9f}\n")
with open(os.path.join(ev, "shutdown-ms.txt"), "w") as f:
    f.write(f"X11 sigint->exit: {ms:.0f} ms\n")
print(f"sigint->exit: {ms:.0f} ms")
PYEOF
tail -3 "$EV/app.log"

# 6. stop the game
kill "$(cat "$EV/game.pid")" 2>/dev/null
sleep 1
echo "--- game log tail ---"; tail -5 "$EV/game.log"
echo "--- cycle + stats summary ---"
echo "gen cycles: $(grep -c '\[gen x 1 + real\]' "$EV/app.log")"
grep 'fps game' "$EV/app.log" | tail -5
echo "vuid/validation: $(grep -cE 'Validation Error|VUID' "$EV/app.log" "$EV/game.log" | tr '\n' ' ')"