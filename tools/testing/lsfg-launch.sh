#!/usr/bin/env bash
# Isolated profile launch. Nothing is inherited from the caller shell: the
# first invocation re-execs under env -i, then the named profile adds the
# rest. Fresh-run steps live here only. Profile scripts call this file.
#
# Usage: tools/testing/lsfg-launch.sh <profile>
# Profiles live in tools/testing/profiles/<profile>.env
#
# Does not SIGKILL. Does not reset either card. Refuses to run if DP-7 is
# not the connected, enabled panel. Refuses a recorder command that names
# kmsgrab. Present target is DP-7.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PROFILE="${1:?usage: $0 <profile>}"
ENVFILE="$REPO/tools/testing/profiles/${PROFILE}.env"
if [[ ! -f "$ENVFILE" ]]; then
  echo "no profile $ENVFILE" >&2
  exit 1
fi

# Steam calls this with --game. Stay in Steam's environment so Proton
# keeps its compat path. Do not rebuild. Do not start or look up a doubler.
if [[ "${2:-}" == "--game" ]]; then
  # shellcheck disable=SC1090
  source "$ENVFILE"
  STEAM_ENV="${HOME}/.local/state/lsfg-vk/steam-game.env"
  if [[ -f "$STEAM_ENV" ]]; then
    now=$(date +%s)
    mtime=$(stat -c %Y "$STEAM_ENV")
    if (( now - mtime < 180 )); then
      # shellcheck disable=SC1090
      source "$STEAM_ENV"
    fi
  fi
  if [[ "${LAYER_ON:-1}" == "0" || "${DISABLE_LSFGVK:-}" == "1" ]]; then
    unset VK_INSTANCE_LAYERS
    unset VK_LAYER_PATH
    export DISABLE_LSFGVK=1
    export LSFGVK_MEASURE=0
  else
    export VK_LAYER_PATH="${VK_LAYER_PATH:-$REPO/build/lsfg-vk-layer}"
    export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_LSFGVK_frame_generation}"
    export LSFGVK_MEASURE="${LSFGVK_MEASURE:-0}"
    export LSFGVK_PROFILE="${LSFGVK_PROFILE:-re2-oneway}"
  fi
  export LSFGVK_CONFIG="${LSFGVK_CONFIG:-$REPO/tools/testing/profiles/${LSFG_CONFIG_NAME:-re2.toml}}"
  export LSFGVK_APP_SOCK="${LSFGVK_APP_SOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/lsfg-vk/app.sock}"
  export MESA_VK_DEVICE_SELECT="${MESA_VK_DEVICE_SELECT:-1002:7550}"
  export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"
  export VKD3D_DEBUG="${VKD3D_DEBUG:-warn}"
  export DISABLE_VK_LAYER_VALVE_steam_fossilize=1
  export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
  export DISABLE_VK_LAYER_VALVE_steam_overlay=1
  export DISABLE_VK_LAYER_VALVE_steam_overlay_1=1
  unset ENABLE_VK_LAYER_VALVE_steam_fossilize_1
  unset ENABLE_VK_LAYER_VALVE_steam_overlay_1
  echo "profile $PROFILE: exec game, no doubler start" >&2
  exec env -u LD_LIBRARY_PATH mangohud --dlsym "${@:3}"
fi

if [[ -z "${LSFG_LAUNCH_ISOLATED:-}" || -n "${LD_PRELOAD:-}${LD_LIBRARY_PATH:-}" ]]; then
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
    /bin/bash "$0" "$PROFILE" "${@:2}"
fi

# shellcheck disable=SC1090
source "$ENVFILE"
GAME_ARGS=("${@:2}")

GAME_KIND="${GAME_KIND:-furmark}"
LAYER_ON="${LAYER_ON:-1}"
LSFG_RECORD="${LSFG_RECORD:-0}"
LSFG_CONFIG_NAME="${LSFG_CONFIG_NAME:-furmark-decoupled.toml}"
if ! declare -p APP_ENV >/dev/null 2>&1; then APP_ENV=(); fi
if ! declare -p FM_ENV >/dev/null 2>&1; then FM_ENV=(); fi

RUN_ID="$(date +%Y%m%dT%H%M%S)"
OUT="${LSFG_RUN_DIR:-/tmp/lsfg-runs/${PROFILE}/${RUN_ID}}"
mkdir -p "$OUT"
LOG="$OUT/launch.log"
STEAM_ENV="$HOME/.local/state/lsfg-vk/steam-game.env"

APP="$REPO/build/lsfg-vk-app/lsfg-vk-app"
LAYER="$REPO/build/lsfg-vk-layer"
LAYER_SO="$LAYER/liblsfg-vk-layer.so"
CFG="$REPO/tools/testing/profiles/$LSFG_CONFIG_NAME"
HUD="$REPO/tools/testing/profiles/MangoHud.conf"
SOCK="${XDG_RUNTIME_DIR}/lsfg-vk/app.sock"
mkdir -p "$(dirname "$SOCK")" "$(dirname "$STEAM_ENV")"

cards_ok() {
  /usr/bin/lspci -s 0000:04:00.0 | /usr/bin/grep -q 'Navi 48' \
    && /usr/bin/lspci -s 0000:87:00.0 | /usr/bin/grep -q 'Navi 44'
}
if ! cards_ok; then
  echo "CARD DROPPED before run" | /usr/bin/tee "$LOG"
  exit 2
fi

DP7_STATUS="$(/usr/bin/cat /sys/class/drm/card3-DP-7/status)"
DP7_ENABLED="$(/usr/bin/cat /sys/class/drm/card3-DP-7/enabled)"
if [[ "$DP7_STATUS" != "connected" || "$DP7_ENABLED" != "enabled" ]]; then
  echo "REFUSING: DP-7 is not the connected enabled panel. Not substituting HDMI-A-3." | /usr/bin/tee "$LOG"
  exit 2
fi

stop_comm() {
  local comm="$1" p
  for p in $(/usr/bin/pgrep -x "$comm" || true); do
    echo "stopping $comm pid=$p"
    /bin/kill -TERM "$p" 2>/dev/null || true
  done
}
echo "--- stop ---"
stop_comm lsfg-vk-app
stop_comm furmark
stop_comm re2.exe
for _ in $(seq 1 50); do
  if ! /usr/bin/pgrep -x lsfg-vk-app >/dev/null \
    && ! /usr/bin/pgrep -x furmark >/dev/null \
    && ! /usr/bin/pgrep -x re2.exe >/dev/null; then
    break
  fi
  sleep 0.2
done
if /usr/bin/pgrep -x lsfg-vk-app >/dev/null \
  || /usr/bin/pgrep -x furmark >/dev/null \
  || /usr/bin/pgrep -x re2.exe >/dev/null; then
  echo "REFUSING: old process still alive" | /usr/bin/tee "$LOG"
  /usr/bin/pgrep -a -x lsfg-vk-app || true
  /usr/bin/pgrep -a -x furmark || true
  /usr/bin/pgrep -a -x re2.exe || true
  exit 1
fi
echo "stop: clear"
rm -f "$SOCK"

echo "--- verify_build ---"
DIRTY_PATHS="$(git -C "$REPO" status --porcelain --untracked-files=no -- . ':!thirdparty')"
VERIFY_ARGS=()
if [[ -n "$DIRTY_PATHS" ]]; then
  VERIFY_ARGS=(--allow-dirty)
fi
if ! "$REPO/tools/testing/verify_build.sh" "${VERIFY_ARGS[@]}" >"$OUT/verify_build.log" 2>&1; then
  echo "REFUSING: verify_build failed (see $OUT/verify_build.log)" | /usr/bin/tee "$LOG"
  exit 1
fi

GIT_FULL="$(git -C "$REPO" rev-parse HEAD)"
if [[ -n "$DIRTY_PATHS" ]]; then
  DIRTY_FLAG=yes
else
  DIRTY_FLAG=no
fi
APP_HASH="$(/usr/bin/sha256sum "$APP" | /usr/bin/awk '{print $1}')"
LAYER_HASH="$(/usr/bin/sha256sum "$LAYER_SO" | /usr/bin/awk '{print $1}')"

{
  echo "--- hash block ---"
  echo "git $GIT_FULL"
  echo "dirty $DIRTY_FLAG"
  if [[ -n "$DIRTY_PATHS" ]]; then
    echo "dirty_paths:"
    printf '%s\n' "$DIRTY_PATHS" | sed 's/^/  /'
  fi
  echo "sha256 app $APP_HASH $APP"
  echo "sha256 layer $LAYER_HASH $LAYER_SO"
  echo "--- end hash block ---"
} > "$LOG"

exec > >(tee -a "$LOG") 2>&1

echo "profile=$PROFILE"
echo "run_dir=$OUT"
echo "repo=$REPO"
echo "game_kind=$GAME_KIND"
echo "layer_on=$LAYER_ON"
echo "--- cards ---"
/usr/bin/lspci -s 0000:04:00.0
/usr/bin/lspci -s 0000:87:00.0
echo "card3-DP-7 status=$DP7_STATUS enabled=$DP7_ENABLED"
echo "verify_build: see $OUT/verify_build.log"

if [[ "$GAME_KIND" == "furmark" ]]; then
  IDX="$("$REPO/tools/testing/gpu_index.sh" 0000:04:00.0)"
  echo "gpu_index.sh 0000:04:00.0 -> $IDX"
  if [[ "$IDX" != "1" ]]; then
    echo "REFUSING: --gpu-index 1 is not PCI 0000:04:00.0 (resolved $IDX)"
    exit 1
  fi
fi

check_exe() {
  local pid="$1" want="$2" label="$3" got
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "HASH CHECK FAIL: $label pid $pid is not alive"
    exit 1
  fi
  got="$(/usr/bin/sha256sum "/proc/$pid/exe" | /usr/bin/awk '{print $1}')"
  if [[ "$got" != "$want" ]]; then
    echo "HASH CHECK FAIL: $label pid=$pid exe $got != $want"
    exit 1
  fi
  echo "hash check $label pid=$pid exe MATCH $got"
}

check_layer_mapped() {
  local pid="$1" i path got
  for i in $(seq 1 120); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "HASH CHECK FAIL: pid $pid exited before layer map check"
      exit 1
    fi
    path="$(/usr/bin/awk '/liblsfg-vk-layer\.so/ {print $6; exit}' "/proc/$pid/maps" 2>/dev/null || true)"
    if [[ -n "$path" ]]; then
      got="$(/usr/bin/sha256sum "$path" | /usr/bin/awk '{print $1}')"
      if [[ "$got" != "$LAYER_HASH" ]]; then
        echo "HASH CHECK FAIL: layer mapped $path $got != $LAYER_HASH"
        exit 1
      fi
      echo "hash check layer pid=$pid mapped MATCH $got $path"
      return 0
    fi
    sleep 0.25
  done
  echo "HASH CHECK FAIL: liblsfg-vk-layer.so not mapped in $pid"
  exit 1
}

check_layer_absent() {
  local pid="$1" path
  path="$(/usr/bin/awk '/liblsfg-vk-layer\.so/ {print $6; exit}' "/proc/$pid/maps" 2>/dev/null || true)"
  if [[ -n "$path" ]]; then
    echo "HASH CHECK FAIL: layer disabled but mapped $path in $pid"
    exit 1
  fi
  echo "hash check layer pid=$pid not mapped"
}

write_steam_env() {
  {
    echo "# generated $(date +%s)"
    printf 'export LSFGVK_APP_SOCK=%q\n' "$SOCK"
    printf 'export LSFGVK_CONFIG=%q\n' "$CFG"
    printf 'export MESA_VK_DEVICE_SELECT=%q\n' "${MESA_VK_DEVICE_SELECT:-1002:7550}"
    printf 'export LSFGVK_LAYER_DBG=%q\n' "${LSFGVK_LAYER_DBG:-0}"
    printf 'export PROTON_LOG=%q\n' "${PROTON_LOG:-0}"
    printf 'export DXVK_LOG_LEVEL=%q\n' "${DXVK_LOG_LEVEL:-info}"
    printf 'export VKD3D_DEBUG=%q\n' "${VKD3D_DEBUG:-warn}"
    echo "export DISABLE_VK_LAYER_VALVE_steam_fossilize=1"
    echo "export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1"
    echo "export DISABLE_VK_LAYER_VALVE_steam_overlay=1"
    echo "export DISABLE_VK_LAYER_VALVE_steam_overlay_1=1"
    if [[ "$LAYER_ON" == "1" ]]; then
      printf 'export VK_LAYER_PATH=%q\n' "$LAYER"
      echo "export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation"
      printf 'export LSFGVK_PROFILE=%q\n' "${LSFGVK_PROFILE:-re2-oneway}"
      printf 'export LSFGVK_MEASURE=%q\n' "${LSFGVK_MEASURE:-0}"
      echo "unset DISABLE_LSFGVK"
    else
      echo "unset VK_INSTANCE_LAYERS"
      echo "unset VK_LAYER_PATH"
      echo "export DISABLE_LSFGVK=1"
      echo "export LSFGVK_MEASURE=0"
    fi
  } > "$STEAM_ENV"
  echo "steam env=$STEAM_ENV"
}

APP_CMD=( "$APP" --profile app-oneway --session wayland --output DP-7 )
echo "--- app command ---"
printf '%q ' "${APP_CMD[@]}"; echo

rm -f "$SOCK"
if /usr/bin/pgrep -x lsfg-vk-app >/dev/null; then
  echo "REFUSING: doubler still alive, will not reuse"
  /usr/bin/pgrep -a -x lsfg-vk-app || true
  exit 1
fi
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
check_exe "$APP_PID" "$APP_HASH" app

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
if [[ "$LSFG_RECORD" == "1" ]]; then
  REC_CMD=( /usr/bin/gpu-screen-recorder -w DP-7 -f 240 -c mp4 -k h264 -q very_high -o "$OUT/cap.mp4" )
  if printf '%s\n' "${REC_CMD[@]}" | /usr/bin/grep -q kmsgrab; then
    echo "REFUSING: kmsgrab in recorder command"
    exit 1
  fi
  echo "--- recorder command ---"
  printf '%q ' "${REC_CMD[@]}"; echo
  "${REC_CMD[@]}" >"$OUT/recorder.log" 2>&1 &
  REC_PID=$!
  echo "recorder pid=$REC_PID"
fi

GAME_PID=""
if [[ "$GAME_KIND" == "furmark" ]]; then
  FM_CMD=( /usr/bin/mangohud --dlsym /usr/bin/furmark
    --demo furmark-vk --gpu-index 1
    --width 2560 --height 1440 --fullscreen
    --msaa 4 --vsync 0 --no-resize --disable-demo-options
    --print-render-speed --no-score-box --max-time "$LSFG_MAX_TIME" --no-osi )
  echo "--- furmark command ---"
  printf '%q ' "${FM_CMD[@]}"; echo
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
  GAME_PID=$!
  echo "furmark pid=$GAME_PID"
  if [[ "$LAYER_ON" == "1" ]]; then
    check_layer_mapped "$GAME_PID"
  else
    check_layer_absent "$GAME_PID"
  fi
  set +e
  wait "$GAME_PID"
  echo "furmark exit=$?"
  set -e
elif [[ "$GAME_KIND" == "steam" ]]; then
  write_steam_env
  echo "Steam launch option calls $REPO/tools/games/launch_re2.sh, which calls the re2 profile"
  echo "game start: /usr/bin/steam steam://rungameid/${STEAM_APPID}"
  CONSOLE="$HOME/.local/share/Steam/logs/console-linux.txt"
  CONSOLE_MARK=0
  if [[ -f "$CONSOLE" ]]; then
    CONSOLE_MARK="$(/usr/bin/wc -l < "$CONSOLE")"
  fi
  echo "console_mark=$CONSOLE_MARK"
  timeout 20 /usr/bin/steam "steam://rungameid/${STEAM_APPID}" || true
  echo "steam cli returned"
  GAME_PID=""
  for _ in $(seq 1 60); do
    GAME_PID="$(/usr/bin/ps -eo pid,comm | /usr/bin/awk -v c="$GAME_COMM" '$2==c {print $1; exit}')"
    if [[ -n "$GAME_PID" ]]; then
      break
    fi
    sleep 1
  done
  if [[ -z "$GAME_PID" ]]; then
    echo "HASH CHECK FAIL: $GAME_COMM did not start"
    exit 1
  fi
  echo "game pid=$GAME_PID comm=$GAME_COMM"
  if [[ "$LAYER_ON" == "1" ]]; then
    check_layer_mapped "$GAME_PID"
  else
    sleep 8
    check_layer_absent "$GAME_PID"
  fi
  if [[ -n "${LSFG_MAX_TIME:-}" ]]; then
    sleep "$LSFG_MAX_TIME"
  fi
  CONSOLE="$HOME/.local/share/Steam/logs/console-linux.txt"
  if [[ -f "$CONSOLE" ]]; then
    /usr/bin/awk -v n="$CONSOLE_MARK" 'NR>n && /lsfg-vk-measure:|DXVK:|vkd3d|Game: re2/' "$CONSOLE" \
      > "$OUT/game-console.txt" || true
    echo "game-console lines=$(/usr/bin/wc -l < "$OUT/game-console.txt")"
  fi
  if kill -0 "$GAME_PID" 2>/dev/null; then
    /bin/kill -TERM "$GAME_PID" 2>/dev/null || true
  fi
else
  echo "REFUSING: unknown GAME_KIND=$GAME_KIND"
  exit 1
fi

if [[ -n "$REC_PID" ]] && kill -0 "$REC_PID" 2>/dev/null; then
  /bin/kill -INT "$REC_PID" 2>/dev/null || true
  set +e
  wait "$REC_PID"
  echo "recorder wait=$?"
  set -e
fi
if [[ -f "$OUT/cap.mp4" ]]; then
  /usr/bin/ffmpeg -y -ss 2 -i "$OUT/cap.mp4" -frames:v 1 "$OUT/frame.png" \
    >"$OUT/frame-extract.log" 2>&1 || echo "frame extract failed"
fi

sleep 1
if kill -0 "$APP_PID" 2>/dev/null; then
  /bin/kill -TERM "$APP_PID" 2>/dev/null || true
fi
set +e
wait "$APP_PID"
echo "app wait=$?"
set -e
rm -f "$STEAM_ENV"

if ! cards_ok; then
  echo "CARD DROPPED during run — discard"
  exit 2
fi
echo "cards still present"
echo "log=$LOG"
echo done
