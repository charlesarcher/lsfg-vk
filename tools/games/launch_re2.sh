#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# launch_re2.sh - Resident Evil 2 one-way dual-GPU Steam launch wrapper for lsfg-vk.
#
# Configure Steam Launch Options:
#   /path/to/lsfg-vk/tools/games/launch_re2.sh %command%
#
# Topology:
#   Pins game rendering to 9070 XT (1002:7550), injects lsfg-vk layer,
#   supervises lsfg-vk-app display process, and tears down cleanly upon game exit.

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 %command% [game arguments...]"
  echo "  Steam launch wrapper for Resident Evil 2."
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}"
APP_BIN="$REPO_ROOT/build/lsfg-vk-app/lsfg-vk-app"
APP_PID=""

if ! pgrep -x lsfg-vk-app >/dev/null; then
    mkdir -p "$(dirname "$LSFGVK_APP_SOCK")"
    rm -f "$LSFGVK_APP_SOCK"
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
trap cleanup EXIT INT TERM

export VK_LAYER_PATH="$REPO_ROOT/build/lsfg-vk-layer"
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"
export LSFGVK_LAYER_DBG=1
export PROTON_LOG=1
# Valve Steam layer compatibility flags
export DISABLE_VK_LAYER_VALVE_steam_fossilize=1
export DISABLE_VK_LAYER_VALVE_steam_overlay=1

exec env -u LD_LIBRARY_PATH mangohud --dlsym "$@"
