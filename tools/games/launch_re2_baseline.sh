#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# launch_re2_baseline.sh - Unlayered native baseline launcher with perf recording for RE2.
#
# Steam launch options:
#   /path/to/lsfg-vk/tools/games/launch_re2_baseline.sh %command%

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 %command% [game arguments...]"
  echo "  Unlayered native baseline launcher for RE2 with perf recording."
  exit 0
fi

# Hard-strip every lsfg-vk/layer variable so nothing leaks from parent environment
unset VK_LAYER_PATH VK_INSTANCE_LAYERS VK_LOADER_LAYERS_ENABLE \
      LSFGVK_CONFIG LSFGVK_APP_SOCK LSFGVK_LAYER_DBG LSFGVK_FAKE_SWAPCHAIN \
      LSFGVK_TIMING LSFGVK_OVERLAY_GAP 2>/dev/null || true

export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"
GAME_ENV_FILE=$(mktemp /tmp/lsfg-gameenv.XXXXXX)
{ export -p; echo 'exec mangohud --dlsym "$@"'; } > "$GAME_ENV_FILE"

if command -v perf >/dev/null 2>&1; then
  exec env -u LD_LIBRARY_PATH perf record -F 149 --call-graph dwarf \
      -o /tmp/lsfg-baseline.perf.data -- bash "$GAME_ENV_FILE" "$@" \
      2> >(tee /tmp/lsfg-baseline-game.log >&2)
else
  exec env -u LD_LIBRARY_PATH bash "$GAME_ENV_FILE" "$@" \
      2> >(tee /tmp/lsfg-baseline-game.log >&2)
fi
