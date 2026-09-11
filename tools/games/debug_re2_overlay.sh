#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# debug_re2_overlay.sh - Bisect Steam overlay LD_PRELOAD overhead on Proton.
#
# Injects gameoverlayrenderer.so directly to measure compositor contention.
#
# Usage:
#   ./debug_re2_overlay.sh [seconds]

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 [seconds]"
  echo "  Bisects Steam overlay LD_PRELOAD overhead on RE2 under Proton."
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SECS=${1:-60}
GAME_DIR="${RE2_GAME_DIR:-/mnt/windows/Games/Steam/steamapps/common/RESIDENT EVIL 2  BIOHAZARD RE2}"
LOG=/tmp/re2-overlay-direct.log

export STEAM_COMPAT_CLIENT_INSTALL_PATH='/usr/share/steam'
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-/mnt/windows/Games/Steam/steamapps/compatdata/883710}"
export STEAM_COMPAT_TOOL_PATHS="${STEAM_COMPAT_TOOL_PATHS:-/usr/share/steam/compatibilitytools.d/proton-cachyos-slr}"
export SteamAppId=883710
export SteamGameId=883710
export WINEDEBUG=-all

export VK_LAYER_PATH="$REPO_ROOT/build/lsfg-vk-layer"
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}"
export LSFGVK_LAYER_DBG=1
export LSFGVK_FAKE_SWAPCHAIN=1
export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"

# Steam overlay preload (64-bit)
export LD_PRELOAD="${STEAM_OVERLAY_SO:-$HOME/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so}"

rm -f "$LOG"
echo "Launching RE2 with Steam overlay preload for ${SECS}s..."
timeout --signal=INT "$SECS" \
    "$STEAM_COMPAT_TOOL_PATHS/proton" waitforexitandrun \
    "$GAME_DIR/re2.exe" >"$LOG" 2>&1 || true

echo "=== execution complete, log saved in $LOG ==="
echo "presents: $(grep -c 'fake present done' "$LOG" || true)"
echo "overlay loaded ok: $(grep -c 'gameoverlayrenderer' "$LOG" || true)"
