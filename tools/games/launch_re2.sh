#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# launch_re2.sh - Resident Evil 2 one-way dual-GPU Steam launch wrapper for lsfg-vk.
#
# Configure Steam Launch Options:
#   /path/to/lsfg-vk/tools/games/launch_re2.sh %command%
#
# Topology:
#   Pins game rendering to 9070 XT (1002:7550) and injects the lsfg-vk
#   layer. Does not start lsfg-vk-app. The doubler must already be
#   listening; otherwise this script exits 1 and does not launch the game.

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 %command% [game arguments...]"
  echo "  Steam launch wrapper for Resident Evil 2."
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Steam's launch option is this file. tools/testing/lsfg-launch.sh starts
# the appid, which makes Steam call this script. A fresh env file from
# that launcher supplies the checked-in profile. A bare Steam click with
# no fresh file keeps the defaults below.
STEAM_ENV="${HOME}/.local/state/lsfg-vk/steam-game.env"
if [[ -f "$STEAM_ENV" ]]; then
  now=$(date +%s)
  mtime=$(stat -c %Y "$STEAM_ENV")
  if (( now - mtime < 180 )); then
    # shellcheck disable=SC1090
    source "$STEAM_ENV"
  fi
fi
if [[ "${DISABLE_LSFGVK:-}" == "1" ]]; then
  unset VK_INSTANCE_LAYERS
  unset VK_LAYER_PATH
else
  export VK_LAYER_PATH="${VK_LAYER_PATH:-$REPO_ROOT/build/lsfg-vk-layer}"
  export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_LSFGVK_frame_generation}"
fi
export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}"
export LSFGVK_MEASURE="${LSFGVK_MEASURE:-0}"
export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"
# The doubler is started by hand before this script. This script must
# not start a second lsfg-vk-app: two presenters on one overlay is a
# black screen, and the script's old start path raced the one already
# listening. Refuse if it is not already up.
LIVE_APP=""
for p in $(pgrep -x lsfg-vk-app 2>/dev/null); do
    st=$(ps -o stat= -p "$p" 2>/dev/null | head -c2)
    [[ "$st" != Z* ]] && LIVE_APP="$p" && break
done
if [[ -z "$LIVE_APP" || ! -S "$LSFGVK_APP_SOCK" ]]; then
    echo "launch_re2: lsfg-vk-app is not listening on $LSFGVK_APP_SOCK" >&2
    echo "launch_re2: start the doubler first. This script will not start one." >&2
    exit 1
fi
echo "launch_re2: using existing doubler pid $LIVE_APP" >&2

export LSFGVK_LAYER_DBG="${LSFGVK_LAYER_DBG:-0}"
export PROTON_LOG="${PROTON_LOG:-0}"
# Valve's implicit-layer JSON keys off the _1 suffix. The unsuffixed
# DISABLE_ names do not match, and Steam sets ENABLE_*_1=1, so Fossilize
# stays in the chain (this run logged Fossilize INFO).
export DISABLE_VK_LAYER_VALVE_steam_fossilize=1
export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
export DISABLE_VK_LAYER_VALVE_steam_overlay=1
export DISABLE_VK_LAYER_VALVE_steam_overlay_1=1
unset ENABLE_VK_LAYER_VALVE_steam_fossilize_1
unset ENABLE_VK_LAYER_VALVE_steam_overlay_1

exec env -u LD_LIBRARY_PATH mangohud --dlsym "$@"
