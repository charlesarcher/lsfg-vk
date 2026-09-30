#!/usr/bin/env bash
# Isolated FurMark + lsfg-vk-app launch. Nothing is inherited from the caller
# shell: the first invocation re-execs under env -i with the variables named
# below, then the named profile file adds the rest. Prints the resolved
# environment and both command lines to the run log before starting anything.
#
# Usage: tools/testing/lsfg-launch.sh <profile>
# Profiles live in tools/testing/profiles/<profile>.env
#
# Does not SIGKILL. Does not reset either card. Refuses to run if DP-7 is
# not the connected, enabled panel.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PROFILE="${1:?usage: $0 <profile>}"
ENVFILE="$REPO/tools/testing/profiles/${PROFILE}.env"
if [[ ! -f "$ENVFILE" ]]; then
  echo "no profile $ENVFILE" >&2
  exit 1
fi

if [[ -z "${LSFG_LAUNCH_ISOLATED:-}" ]]; then
  UID_N="$(id -u)"
  RUNTIME="/run/user/${UID_N}"
  WAYLAND="wayland-0"
  if [[ ! -S "${RUNTIME}/${WAYLAND}" ]]; then
    echo "no ${RUNTIME}/${WAYLAND}" >&2
    exit 1
  fi
  XAUTH=""
  for f in "${RUNTIME}"/xauth_*; do
    if [[ -f "$f" ]]; then
      XAUTH="$f"
      break
    fi
  done
  exec /usr/bin/env -i \
    LSFG_LAUNCH_ISOLATED=1 \
    LSFG_PROFILE="$PROFILE" \
    HOME="$HOME" \
    USER="${USER:-archerc}" \
    LOGNAME="${USER:-archerc}" \
    PATH="/usr/bin:/bin" \
    LANG="${LANG:-C.UTF-8}" \
    XDG_RUNTIME_DIR="$RUNTIME" \
    WAYLAND_DISPLAY="$WAYLAND" \
    DISPLAY=":0" \
    XAUTHORITY="$XAUTH" \
    /bin/bash "$0" "$PROFILE"
fi

# shellcheck disable=SC1090
source "$ENVFILE"

RUN_ID="$(date +%Y%m%dT%H%M%S)"
OUT="${LSFG_RUN_DIR:-/tmp/lsfg-runs/${PROFILE}/${RUN_ID}}"
mkdir -p "$OUT"
LOG="$OUT/launch.log"
exec > >(tee -a "$LOG") 2>&1

echo "profile=$PROFILE"
echo "run_dir=$OUT"
echo "repo=$REPO"
echo "--- resolved environment ---"
/usr/bin/env | /usr/bin/sort

APP="$REPO/build/lsfg-vk-app/lsfg-vk-app"
LAYER="$REPO/build/lsfg-vk-layer"
CFG="$REPO/tools/testing/profiles/furmark-decoupled.toml"
HUD="$REPO/tools/testing/profiles/MangoHud.conf"
SOCK="${XDG_RUNTIME_DIR}/lsfg-vk/app.sock"
mkdir -p "$(dirname "$SOCK")"

cards_ok() {
  /usr/bin/lspci -s 0000:04:00.0 | /usr/bin/grep -q 'Navi 48' \
    && /usr/bin/lspci -s 0000:87:00.0 | /usr/bin/grep -q 'Navi 44'
}
if ! cards_ok; then
  echo "CARD DROPPED before run"
  exit 2
fi
echo "--- cards ---"
/usr/bin/lspci -s 0000:04:00.0
/usr/bin/lspci -s 0000:87:00.0

DP7_STATUS="$(/usr/bin/cat /sys/class/drm/card3-DP-7/status)"
DP7_ENABLED="$(/usr/bin/cat /sys/class/drm/card3-DP-7/enabled)"
echo "card3-DP-7 status=$DP7_STATUS enabled=$DP7_ENABLED"
if [[ "$DP7_STATUS" != "connected" || "$DP7_ENABLED" != "enabled" ]]; then
  echo "REFUSING: DP-7 is not the connected enabled panel. Not substituting HDMI-A-3."
  exit 2
fi

IDX="$("$REPO/tools/testing/gpu_index.sh" 0000:04:00.0)"
echo "gpu_index.sh 0000:04:00.0 -> $IDX"
if [[ "$IDX" != "1" ]]; then
  echo "REFUSING: --gpu-index 1 is not PCI 0000:04:00.0 (resolved $IDX)"
  exit 1
fi

if /usr/bin/pgrep -x furmark >/dev/null || /usr/bin/pgrep -x lsfg-vk-app >/dev/null; then
  echo "REFUSING: furmark or lsfg-vk-app already running"
  exit 1
fi

APP_CMD=( "$APP" --profile app-oneway --session wayland --output DP-7 )
FM_CMD=( /usr/bin/mangohud --dlsym /usr/bin/furmark
  --demo furmark-vk --gpu-index 1
  --width 2560 --height 1440 --fullscreen
  --msaa 4 --vsync 0 --no-resize --disable-demo-options
  --print-render-speed --no-score-box --max-time "$LSFG_MAX_TIME" --no-osi )

echo "--- app command ---"
printf '%q ' "${APP_CMD[@]}"; echo
echo "--- furmark command ---"
printf '%q ' "${FM_CMD[@]}"; echo

rm -f "$SOCK"
/usr/bin/env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH \
  -u LSFGVK_POSIX_SHM -u LSFGVK_WAIT_COPY -u LSFGVK_COPY_ON_GFX \
  -u LSFGVK_CAPTURE_PRIO -u LSFGVK_CO_EXPERIMENT \
  LSFGVK_CONFIG="$CFG" \
  LSFGVK_APP_SOCK="$SOCK" \
  LSFGVK_LAYER_SHELL=1 \
  LSFGVK_HUD=imgui \
  "${APP_ENV[@]}" \
  "${APP_CMD[@]}" >"$OUT/app.log" 2>&1 &
APP_PID=$!
echo "app pid=$APP_PID"

for _ in $(seq 1 80); do
  if [[ -S "$SOCK" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    break
  fi
  sleep 0.05
done
if [[ ! -S "$SOCK" ]]; then
  echo "app socket never appeared"
  wait "$APP_PID" 2>/dev/null || true
  exit 1
fi

REC_PID=""
if [[ "${LSFG_RECORD:-0}" == "1" ]]; then
  REC_CMD=( /usr/bin/gpu-screen-recorder -w screen -f 240 -c mp4 -k h264 -q very_high -o "$OUT/dp7.mp4" )
  echo "--- recorder command ---"
  printf '%q ' "${REC_CMD[@]}"; echo
  "${REC_CMD[@]}" >"$OUT/recorder.log" 2>&1 &
  REC_PID=$!
  echo "recorder pid=$REC_PID"
fi

/usr/bin/env -u LSFGVK_POSIX_SHM -u LSFGVK_WAIT_COPY -u LSFGVK_DUAL_HOST \
  -u LSFGVK_COPY_ON_GFX -u LSFGVK_CAPTURE_PRIO \
  VK_LAYER_PATH="$LAYER" \
  VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
  VK_LOADER_DEBUG=error,warn,info,layer \
  LSFGVK_PROFILE=furmark-oneway \
  LSFGVK_CAPTURE_WxH=2560x1440 \
  LSFGVK_CONFIG="$CFG" \
  LSFGVK_APP_SOCK="$SOCK" \
  MANGOHUD=1 \
  MANGOHUD_CONFIGFILE="$HUD" \
  "${FM_ENV[@]}" \
  "${FM_CMD[@]}" >"$OUT/furmark.log" 2>&1 &
FM_PID=$!
echo "furmark pid=$FM_PID"

set +e
wait "$FM_PID"
echo "furmark exit=$?"
set -e

if [[ -n "$REC_PID" ]] && kill -0 "$REC_PID" 2>/dev/null; then
  kill -INT "$REC_PID" 2>/dev/null || true
  set +e
  wait "$REC_PID"
  echo "recorder wait=$?"
  set -e
fi

sleep 1
if kill -0 "$APP_PID" 2>/dev/null; then
  kill -TERM "$APP_PID" 2>/dev/null || true
fi
set +e
wait "$APP_PID"
echo "app wait=$?"
set -e

if ! cards_ok; then
  echo "CARD DROPPED during run — discard"
  exit 2
fi
echo "cards still present"
echo "log=$LOG"
echo done
