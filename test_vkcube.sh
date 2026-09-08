#!/usr/bin/env bash
# vkcube debug harness (Session 13.10): run vkcube through the full doubler
# pipeline with the fake swapchain, capture all output, kill after N seconds.
# Usage: ./test_vkcube.sh [seconds] [extra vkcube args...]
set -eu
SECS=${1:-15}
shift 2>/dev/null || true
LOG=/tmp/vkcube-fake.log

export VK_LAYER_PATH=/home/archerc/code/lsfg-vk/build/lsfg-vk-layer
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
export LSFGVK_APP_SOCK="$HOME/.local/state/lsfg-vk/app.sock"
export LSFGVK_LAYER_DBG=1
export LSFGVK_FAKE_SWAPCHAIN=1
export MESA_VK_DEVICE_SELECT=1002:7550   # render on the 9070 XT

rm -f "$LOG"
set +e
timeout --signal=SEGV "$SECS" vkcube --c 100 "$@" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e
echo "=== vkcube exit: $RC (124=timeout ok, 139=segv, 134=abort) ==="
