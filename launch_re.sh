#!/usr/bin/env bash
# lsfg-vk game launcher for Steam (693f459 baseline, Session 13.26).
# Set Steam launch options to:
#   /home/archerc/code/lsfg-vk/launch_re.sh %command%
# Pins rendering to the 9070 XT (1002:7550), injects the lsfg-vk layer,
# mangohud on, perf capture to /tmp/lsfg-re2.perf.data, layer stderr teed
# to /tmp/lsfg-layer-game.log.
set -eu
export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
export LSFGVK_APP_SOCK="$HOME/.local/state/lsfg-vk/app.sock"
# Isolated swapchain has no game scanout. If the overlay is not listening,
# CreateSwapchain dies with Connection refused and the game "crashes".
APP_BIN=/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app
if ! pgrep -x lsfg-vk-app >/dev/null; then
    mkdir -p "$(dirname "$LSFGVK_APP_SOCK")"
    rm -f "$LSFGVK_APP_SOCK"
    env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
        LSFGVK_APP_DBG=1 \
        LSFGVK_CONFIG="$LSFGVK_CONFIG" \
        LSFGVK_APP_SOCK="$LSFGVK_APP_SOCK" \
        "$APP_BIN" --profile app-oneway --session wayland \
        >>/tmp/doubler.log 2>&1 &
    disown || true
    for _ in $(seq 1 50); do
        [[ -S "$LSFGVK_APP_SOCK" ]] && break
        sleep 0.1
    done
fi
export VK_LAYER_PATH=/home/archerc/code/lsfg-vk/build/lsfg-vk-layer
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export MESA_VK_DEVICE_SELECT=1002:7550
export LSFGVK_LAYER_DBG=1
export PROTON_LOG=1
GAME_ENV_FILE=$(mktemp /tmp/lsfg-gameenv.XXXXXX)
{ export -p; echo 'exec mangohud --dlsym "$@"'; } > "$GAME_ENV_FILE"
exec env -u LD_LIBRARY_PATH perf record -F 149 --call-graph dwarf \
    -o /tmp/lsfg-re2.perf.data -- bash "$GAME_ENV_FILE" "$@" \
    2> >(tee /tmp/lsfg-layer-game.log >&2)
