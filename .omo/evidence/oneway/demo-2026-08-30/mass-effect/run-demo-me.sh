#!/usr/bin/env bash
# Mass Effect 1 (Legendary Edition) one-way frame-doubling demo.
# Game renders on the 9070 XT (headless, pinned via MESA_VK_DEVICE_SELECT),
# lsfg-vk-app doubles the frames and presents from the 9060 XT (display).
# Usage: run-demo-me.sh
set -u
EV="$(dirname "$(readlink -f "$0")")"
APP=/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app
CONF=$HOME/.config/lsfg-vk/conf.toml
PROTON="$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-6-x86_64/proton"
GAMEDIR="/mnt/windows/Games/Steam/steamapps/common/Mass Effect Legendary Edition/Game/ME1/Binaries/Win64"

# 1. start the doubler app (X11 backend, verbose)
LSFGVK_CONFIG=$CONF XAUTHORITY=/run/user/1000/xauth_OKuvCG \
    nohup $APP --profile app-oneway --session x11 -v > "$EV/app.log" 2>&1 &
echo $! > "$EV/app.pid"
sleep 3
echo "--- app banner ---"; head -6 "$EV/app.log"

# 2. launch ME1 via Proton, pinned to the 9070 XT, with the layer
cd "$GAMEDIR"
DISPLAY=:0 XAUTHORITY=/run/user/1000/xauth_OKuvCG \
WINEDEBUG=-all \
STEAM_COMPAT_APPID=1328670 \
STEAM_COMPAT_DATA_PATH="$HOME/.local/share/Steam/steamapps/compatdata/1328670" \
STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
VK_LAYER_PATH=/tmp/opencode/layer-test \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_CONFIG=$CONF \
MESA_VK_DEVICE_SELECT=1002:7550 \
MESA_VK_DEVICE_SELECT_DEBUG=1 \
nohup "$PROTON" run MassEffect1.exe > "$EV/game.log" 2>&1 &
echo $! > "$EV/game.pid"

# 3. poll for the stream handshake (layer -> app) up to 5 min
for i in $(seq 1 60); do
    sleep 5
    if grep -q 'stream from' "$EV/app.log" 2>/dev/null; then
        echo "STREAM LIVE after ~$((i*5))s"
        break
    fi
    if ! kill -0 "$(cat "$EV/game.pid")" 2>/dev/null; then
        echo "GAME PROCESS DIED at ~$((i*5))s"; tail -25 "$EV/game.log"; break
    fi
done
echo "--- app log (stream section) ---"; grep -E 'stream from|context created|using.*backend|error' "$EV/app.log" | head
echo "--- game log: layer + device select ---"
grep -E 'lsfg-vk|MESA_VK_DEVICE_SELECT|Selected GPU|Validation Error|VUID' "$EV/game.log" | head -20

# 4. let it run ~90 s in-game, capture stills + engine busy
sleep 90
spectacle -b -n -o "$EV/shot-a.png"
echo "--- engine busy (doubler=card3, game=card2) ---"
echo "doubler_9060XT=$(cat /sys/class/drm/card3/device/gpu_busy_percent)% game_9070XT=$(cat /sys/class/drm/card2/device/gpu_busy_percent)%" \
    | tee "$EV/engine-midrun.txt"
sleep 30
spectacle -b -n -o "$EV/shot-b.png"
sleep 30
echo "--- stats summary ---"
echo "gen cycles: $(grep -c '\[gen x 1 + real\]' "$EV/app.log")"
grep 'fps game' "$EV/app.log" | tail -5
echo "vuid: $(grep -cE 'Validation Error|VUID' "$EV/app.log" "$EV/game.log" | tr '\n' ' ')"

# 5. teardown: SIGINT app, time exit; then kill game
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
with open(os.path.join(ev, "shutdown-ms.txt"), "w") as f:
    f.write(f"ME sigint->exit: {ms:.0f} ms\n")
print(f"sigint->exit: {ms:.0f} ms")
PYEOF
kill "$(cat "$EV/game.pid")" 2>/dev/null
sleep 3
echo "--- app log tail ---"; tail -4 "$EV/app.log"
echo "--- game log tail ---"; tail -8 "$EV/game.log"