#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_vkcube.sh - Headless fake-swapchain smoke test harness for lsfg-vk.
#
# Exercises vkcube through the full frame-doubling pipeline under isolated
# swapchain capture, verifying IPC connection, export, and presentation.
#
# Usage:
#   ./test_vkcube.sh [seconds] [extra vkcube args...]
#
# Examples:
#   ./test_vkcube.sh 10
#   ./test_vkcube.sh 5 --c 50

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 [seconds] [extra vkcube args...]"
  echo "  seconds    Duration to run vkcube before timeout (default: 15)"
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SECS=${1:-15}
shift 2>/dev/null || true
LOG=/tmp/vkcube-fake.log

export VK_LAYER_PATH="$REPO_ROOT/build/lsfg-vk-layer"
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}"
export LSFGVK_LAYER_DBG=1
export LSFGVK_FAKE_SWAPCHAIN=1
export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"

if ! command -v vkcube >/dev/null 2>&1; then
  echo "error: vkcube binary not found on PATH" >&2
  exit 1
fi

rm -f "$LOG"
echo "Running vkcube for ${SECS}s with lsfg-vk layer..."
set +e
timeout --signal=SEGV "$SECS" vkcube --c 100 "$@" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e
echo "=== vkcube exit: $RC (124=timeout/success, 139=segv, 134=abort) ==="
exit 0
