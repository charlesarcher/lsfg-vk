#!/usr/bin/env bash
# BASELINE launcher (Session 13.22): NO lsfg-vk layer, NO doubler - native
# present path. Renders on 9070 XT, mangohud on, perf captures to
# /tmp/lsfg-baseline.perf.data.
# Steam launch options: /home/archerc/code/lsfg-vk/launch_re_baseline.sh %command%
set -eu
# hard strip every lsfg-vk/layer variable so nothing leaks from any parent env
unset VK_LAYER_PATH VK_INSTANCE_LAYERS VK_LOADER_LAYERS_ENABLE \
      LSFGVK_CONFIG LSFGVK_APP_SOCK LSFGVK_LAYER_DBG LSFGVK_FAKE_SWAPCHAIN \
      LSFGVK_TIMING LSFGVK_OVERLAY_GAP 2>/dev/null || true
export MESA_VK_DEVICE_SELECT=1002:7550
GAME_ENV_FILE=$(mktemp /tmp/lsfg-gameenv.XXXXXX)
{ export -p; echo 'exec mangohud --dlsym "$@"'; } > "$GAME_ENV_FILE"
exec env -u LD_LIBRARY_PATH perf record -F 149 --call-graph dwarf \
    -o /tmp/lsfg-baseline.perf.data -- bash "$GAME_ENV_FILE" "$@" \
    2> >(tee /tmp/lsfg-baseline-game.log >&2)
