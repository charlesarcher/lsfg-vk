#!/usr/bin/env bash
# lsfg-vk game launcher for Steam.
# Set Steam launch options to:
#   /home/archerc/code/lsfg-vk/launch_re.sh %command%
# Pins rendering to the 9070 XT (1002:7550), injects the lsfg-vk layer,
# mangohud on. Overlay is started here and KILLED when the game exits so
# exclusive layer-shell cannot keep DP-7 after Steam-stop.
set -eu
export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
export LSFGVK_APP_SOCK="$HOME/.local/state/lsfg-vk/app.sock"
APP_BIN=/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app
APP_PID=""
if ! pgrep -x lsfg-vk-app >/dev/null; then
    mkdir -p "$(dirname "$LSFGVK_APP_SOCK")"
    rm -f "$LSFGVK_APP_SOCK"
    # Ignore INT so a timeout/Steam-stop on the game does not tear overlay
    # down before the game process actually dies (isolated = black + hang).
    env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
        LSFGVK_APP_DBG=1 \
        LSFGVK_CONFIG="$LSFGVK_CONFIG" \
        LSFGVK_APP_SOCK="$LSFGVK_APP_SOCK" \
        setsid -f "$APP_BIN" --profile app-oneway --session wayland \
        >>/tmp/doubler.log 2>&1
    for _ in $(seq 1 50); do
        APP_PID=$(pgrep -n -x lsfg-vk-app || true)
        [[ -n "$APP_PID" && -S "$LSFGVK_APP_SOCK" ]] && break
        sleep 0.1
    done
fi
cleanup() {
    if [[ -n "${APP_PID:-}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
        kill -TERM "$APP_PID" 2>/dev/null || true
        sleep 0.2
        kill -KILL "$APP_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
export VK_LAYER_PATH=/home/archerc/code/lsfg-vk/build/lsfg-vk-layer
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export MESA_VK_DEVICE_SELECT=1002:7550
export LSFGVK_LAYER_DBG=1
export PROTON_LOG=1
env -u LD_LIBRARY_PATH mangohud --dlsym "$@"
