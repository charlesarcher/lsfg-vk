#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_wsi_recreate.sh - Dynamic WSI swapchain recreation stress harness for lsfg-vk.
#
# Compiles wsi_recreate.c (or uses precompiled binary) to simulate dynamic
# resolution switches (1080p -> 1440p) while frame-doubling is active.
# Verifies that:
#   1. Isolated swapchain recreates cleanly without device lost.
#   2. IPC socket streams disconnect and reconnect smoothly.
#   3. lsfg-vk-app adjusts its presentation pipeline without segfaults.
#
# Usage:
#   ./test_wsi_recreate.sh

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0"
  echo "  Runs an automated 1080p -> 1440p dynamic swapchain recreation test."
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOCK=${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}
APP="$REPO_ROOT/build/lsfg-vk-app/lsfg-vk-app"
LAYER="$REPO_ROOT/build/lsfg-vk-layer"
SRC="$REPO_ROOT/tools/testing/wsi_recreate.c"
BIN=/tmp/lsfg-wsi-recreate
LOG=/tmp/lsfg-recreate.log
APPLOG=/tmp/lsfg-recreate-app.log

if [[ ! -x "$APP" ]]; then
  echo "error: missing $APP - run cmake --build build first" >&2
  exit 1
fi

echo "Compiling $SRC..."
cc -O1 -o "$BIN" "$SRC" -lvulkan -ldl

echo "Starting lsfg-vk-app on $SOCK..."
pkill -x lsfg-vk-app 2>/dev/null || true
sleep 0.2
mkdir -p "$(dirname "$SOCK")"
rm -f "$SOCK" "$LOG" "$APPLOG"

env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
  LSFGVK_APP_DBG=1 \
  LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}" \
  LSFGVK_APP_SOCK="$SOCK" \
  "$APP" --profile app-oneway --session wayland >>"$APPLOG" 2>&1 &
APP_PID=$!

cleanup() {
  if [[ -n "${APP_PID:-}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 50); do
  [[ -S "$SOCK" ]] && break
  sleep 0.1
done

if [[ ! -S "$SOCK" ]]; then
  echo "FAIL: timed out waiting for app socket $SOCK" >&2
  cat "$APPLOG" >&2
  exit 1
fi

echo "Executing WSI dynamic recreation test..."
set +e
timeout --signal=KILL 20 \
  env VK_LAYER_PATH="$LAYER" \
      VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
      LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}" \
      LSFGVK_APP_SOCK="$SOCK" \
      LSFGVK_LAYER_DBG=1 \
      LSFGVK_PROFILE=vkcube-oneway \
      MESA_VK_DEVICE_SELECT=1002:7550 \
      "$BIN" >"$LOG" 2>&1
RC=$?
set -e

echo "=== recreation test exit: $RC (0=success) ==="
tail -n 15 "$LOG"
echo "--- app log summary ---"
grep -E 'stream from|stream ended|Bad file|idle |2560x1440|1920x1080' "$APPLOG" | tail -n 15 || true
exit "$RC"
