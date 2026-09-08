#!/usr/bin/env bash
# Agent-runnable 1080→1440 recreate through isolated+external. No Steam.
# Usage: ./test_recreate.sh
set -euo pipefail
ROOT=/home/archerc/code/lsfg-vk
SOCK=${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}
APP=$ROOT/build/lsfg-vk-app/lsfg-vk-app
LAYER=$ROOT/build/lsfg-vk-layer
BIN=/tmp/lsfg-recreate
LOG=/tmp/lsfg-recreate.log
APPLOG=/tmp/lsfg-recreate-app.log

cc -O1 -o "$BIN" "$ROOT/test_recreate.c" -lvulkan -ldl

killall -q lsfg-vk-app 2>/dev/null || true
sleep 0.2
mkdir -p "$(dirname "$SOCK")"
rm -f "$SOCK" "$LOG" "$APPLOG"
env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
  LSFGVK_APP_DBG=1 \
  LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml" \
  LSFGVK_APP_SOCK="$SOCK" \
  "$APP" --profile app-oneway --session wayland >>"$APPLOG" 2>&1 &
APP_PID=$!
for i in $(seq 1 50); do
  [[ -S "$SOCK" ]] && break
  sleep 0.1
done
[[ -S "$SOCK" ]] || { echo FAIL no sock; kill "$APP_PID" 2>/dev/null || true; exit 1; }

set +e
timeout --signal=KILL 20 \
  env VK_LAYER_PATH="$LAYER" \
      VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
      LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml" \
      LSFGVK_APP_SOCK="$SOCK" \
      LSFGVK_LAYER_DBG=1 \
      LSFGVK_PROFILE=vkcube-oneway \
      MESA_VK_DEVICE_SELECT=1002:7550 \
      "$BIN" >"$LOG" 2>&1
RC=$?
set -e
kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true
pgrep -x lsfg-vk-app >/dev/null && killall -9 lsfg-vk-app 2>/dev/null || true

echo "=== recreate exit=$RC ==="
tail -20 "$LOG"
echo "--- overlay ---"
grep -E 'stream from|stream ended|Bad file|idle ' "$APPLOG" | tail -20
echo "1440 HELLO: $(grep -c '2560x1440' "$APPLOG" || true)"
echo "1080 HELLO: $(grep -c '1920x1080' "$APPLOG" || true)"
exit $RC
