#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# launch_app.sh - Standalone launcher for lsfg-vk-app presentation daemon.
#
# Usage:
#   ./launch_app.sh [--ui] [--profile profile_name] [--session wayland|x11]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_BIN="$REPO_ROOT/build/lsfg-vk-app/lsfg-vk-app"

if [[ ! -x "$APP_BIN" ]]; then
  echo "error: missing $APP_BIN - run cmake --build build first" >&2
  exit 1
fi

export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}"
export LSFGVK_APP_DBG="${LSFGVK_APP_DBG:-1}"

exec env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
  "$APP_BIN" --profile "${1:-app-oneway}" --session "${2:-wayland}" "${@:3}"
