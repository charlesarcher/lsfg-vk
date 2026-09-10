#!/usr/bin/env bash
# Agent-runnable FurMark A/B for the one-way doubler. No Steam.
#
# Topology (binding): render = 9070 XT (Vulkan gpu-index 1, 1002:7550)
#                     process/present = 9060 XT (lsfg-vk-app, 1002:7590)
#
# Usage: ./test_furmark.sh {baseline|doubled|ab} [seconds] [preset]
#   presets:
#     light  1280x720  MSAA1  vsync0   high native fps; exposes present-path throttle
#     bound  2560x1440 MSAA4  vsync0   GPU-bound; target slightly under 200 like RE2
#     panel  2560x1440 MSAA1  vsync0   match DP-7; default
#     heavy  2560x1440 MSAA8  vsync0   GPU-bound; should NOT look like the 20fps gate
# Mangohud ON by default (mangohud --dlsym). Do not set MANGOHUD=0 unless
# bisecting overlay cost.
#
# OpenGL demos are refused: the layer is Vulkan-only.
# Kills only PIDs this script spawned.
# MangoHud is ON by default (mangohud --dlsym, same as launch_re.sh).
# Set MANGOHUD=0 to skip for a clean numeric A/B (~20 fps overlay cost).
set -euo pipefail

ROOT=/home/archerc/code/lsfg-vk
MODE=${1:-ab}
SECS=${2:-8}
PRESET=${3:-panel}
SOCK=${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}
APP="$ROOT/build/lsfg-vk-app/lsfg-vk-app"
LAYER_DIR="$ROOT/build/lsfg-vk-layer"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=/tmp/lsfg-furmark-$STAMP
mkdir -p "$OUT"

case "$PRESET" in
  light) W=1280; H=720;  MSAA=1 ;;
  bound) W=2560; H=1440; MSAA=4 ;;
  panel) W=2560; H=1440; MSAA=1 ;;
  heavy) W=2560; H=1440; MSAA=8 ;;
  *) echo "unknown preset: $PRESET (light|bound|panel|heavy)" >&2; exit 2 ;;
esac

# FurMark Vulkan device list (NOT the GpuMonitor list): 0=9060, 1=9070, 2=Intel
FURMARK_VK_GPU_9070="${RENDER_GPU_INDEX:-1}"

export DISPLAY=${DISPLAY:-:0}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}

STARTED_APP=0
APP_PID=""
FURMARK_PID=""

cleanup() {
  if [[ -n "${FURMARK_PID}" ]] && kill -0 "$FURMARK_PID" 2>/dev/null; then
    kill "$FURMARK_PID" 2>/dev/null || true
    wait "$FURMARK_PID" 2>/dev/null || true
  fi
  if [[ "$STARTED_APP" == 1 && -n "${APP_PID}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

die() { echo "FAIL: $*" >&2; exit 1; }

assert_binaries() {
  [[ -x "$APP" ]] || die "missing $APP — build first"
  [[ -f "$LAYER_DIR/liblsfg-vk-layer.so" ]] || die "missing layer .so"
  [[ -x /usr/bin/furmark ]] || die "furmark not installed"
}

start_app() {
  if pgrep -x lsfg-vk-app >/dev/null; then
    echo "reusing already-running lsfg-vk-app pid=$(pgrep -x lsfg-vk-app | tr '\n' ' ')"
    STARTED_APP=0
    return
  fi
  mkdir -p "$(dirname "$SOCK")"
  rm -f "$SOCK"
  # The app must NOT load the game layer. Inherited VK_INSTANCE_LAYERS from a
  # doubled FurMark run intercepts the overlay swapchain, opens a second
  # stream on the 9060, and kills IPC (timeline import -13 / FRAME EPIPE).
  env -u VK_LAYER_PATH -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_ENABLE \
    -u MESA_VK_DEVICE_SELECT -u LD_PRELOAD \
    LSFGVK_APP_DBG=1 LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml" \
    LSFGVK_APP_SOCK="$SOCK" \
    LSFGVK_SKIP_SNAP="${LSFGVK_SKIP_SNAP:-}" \
    LSFGVK_DROP_GEN="${LSFGVK_DROP_GEN:-}" \
    LSFGVK_EMPTY_GEN="${LSFGVK_EMPTY_GEN:-}" \
    LSFGVK_EMPTY_XFER="${LSFGVK_EMPTY_XFER:-}" \
    LSFGVK_EMPTY_PERIOD_MS="${LSFGVK_EMPTY_PERIOD_MS:-}" \
    LSFGVK_NO_CAPTURE_WAIT="${LSFGVK_NO_CAPTURE_WAIT:-}" \
    LSFGVK_NO_HOP="${LSFGVK_NO_HOP:-}" \
    LSFGVK_APP_VERBOSE="${LSFGVK_APP_VERBOSE:-}" \
    LSFGVK_POSIX_SHM="${LSFGVK_POSIX_SHM:-}" \
    LSFGVK_OVERLAY_GAP="${LSFGVK_OVERLAY_GAP:-}" \
    LSFGVK_DUMP_PPM="${LSFGVK_DUMP_PPM:-}" \
    LSFGVK_DUMP_PRESENT="${LSFGVK_DUMP_PRESENT:-}" \
    "$APP" --profile "${APP_PROFILE:-app-oneway}" --session wayland \
    >"$OUT/app.log" 2>&1 &
  APP_PID=$!
  STARTED_APP=1
  echo "started lsfg-vk-app pid=$APP_PID"
  local i
  for i in $(seq 1 50); do
    if kill -0 "$APP_PID" 2>/dev/null && [[ -S "$SOCK" ]]; then
      return
    fi
    sleep 0.1
  done
  die "lsfg-vk-app did not listen on $SOCK (see $OUT/app.log)"
}

run_furmark() {
  local tag=$1
  local log="$OUT/${tag}.log"

  # Strip every layer/lsfg var so baseline cannot leak from the parent shell.
  unset VK_LAYER_PATH VK_INSTANCE_LAYERS VK_LOADER_LAYERS_ENABLE \
        LSFGVK_CONFIG LSFGVK_APP_SOCK LSFGVK_LAYER_DBG LSFGVK_FAKE_SWAPCHAIN \
        LSFGVK_TIMING LSFGVK_OVERLAY_GAP LSFGVK_ENV LSFGVK_PROFILE \
        MESA_VK_DEVICE_SELECT 2>/dev/null || true

  if [[ "$tag" == doubled ]]; then
    export VK_LAYER_PATH="$LAYER_DIR"
    export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
    export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
    export LSFGVK_APP_SOCK="$SOCK"
    export LSFGVK_LAYER_DBG=1
    export LSFGVK_PROFILE="${GAME_PROFILE:-furmark-oneway}"
    # LSFGVK_TIMING is opt-in: CmdResetQueryPool on the copy CB was left
    # on from session 13.53 and may stall the 9070. Do not default it.
    # Do NOT set MESA_VK_DEVICE_SELECT here: it collapses FurMark's Vulkan
    # device list so --gpu-index 1 is no longer the 9070 XT.
  fi

  # Must run from /opt/furmark (g.dz assets). Do not pass --export-dir /
  # --logfile-suffix — those SIGSEGV in fwrite during gm_gxl_init.
  local wrap=()
  if [[ "${MANGOHUD:-1}" != 0 ]]; then
    wrap=(mangohud --dlsym)
  fi
  (
    cd /opt/furmark
    "${wrap[@]}" /usr/bin/furmark \
      --demo furmark-vk \
      --gpu-index "$FURMARK_VK_GPU_9070" \
      --width "$W" --height "$H" \
      --msaa "$MSAA" \
      --vsync 0 \
      --no-resize \
      --disable-demo-options \
      --print-render-speed \
      --no-score-box \
      --max-time "$SECS" \
      --no-osi
  ) >"$log" 2>&1 &
  FURMARK_PID=$!
  local rc=0
  wait "$FURMARK_PID" || rc=$?
  FURMARK_PID=""
  echo "$rc" >"$OUT/${tag}.exit"
  echo "$log"
}

summarize() {
  local tag=$1
  local log="$OUT/${tag}.log"
  echo "----- $tag -----"
  echo "log: $log  exit=$(cat "$OUT/${tag}.exit")"
  local renderer fps
  renderer=$(awk -F: '/renderer/{sub(/^ +/,"",$2); print $2; exit}' "$log" || true)
  fps=$(awk -F: '/FPS \(min\/avg\/max\)/{sub(/^ +/,"",$2); print $2; exit}' "$log" || true)
  echo "renderer: ${renderer:-MISSING}"
  echo "fps min/avg/max: ${fps:-MISSING}"
  if [[ "$renderer" != *"9070 XT"* ]]; then
    echo "FAIL: $tag did not render on 9070 XT" >&2
    return 1
  fi
  if [[ "$tag" == doubled ]]; then
    local presents gaps
    presents=$(grep -cE 'present enter|QueuePresent|FRAME-sent|external presentation' "$log" || true)
    echo "layer markers in furmark log: $presents"
    if [[ -f "$OUT/app.log" ]]; then
      echo "app stream lines:"
      grep -E 'stream from|listening|error|lost' "$OUT/app.log" | head -20 || true
    fi
  fi
  echo "$fps" >"$OUT/${tag}.fps"
}

assert_binaries
echo "out=$OUT mode=$MODE preset=$PRESET ${W}x${H} msaa=$MSAA secs=$SECS mangohud=${MANGOHUD:-1}"
echo "render=9070XT(vk-index $FURMARK_VK_GPU_9070)  present=${APP_PROFILE:-9060XT(app-oneway)}"

case "$MODE" in
  baseline)
    run_furmark baseline
    summarize baseline
    ;;
  doubled)
    start_app
    run_furmark doubled
    summarize doubled
    ;;
  ab)
    run_furmark baseline
    summarize baseline
    start_app
    run_furmark doubled
    summarize doubled
    echo "----- A/B -----"
    echo "baseline fps: $(cat "$OUT/baseline.fps")"
    echo "doubled  fps: $(cat "$OUT/doubled.fps")"
    echo "native FurMark fps is GAME rate. Doubler HUD presented ≈ 2× that if healthy."
    ;;
  *)
    die "mode must be baseline|doubled|ab"
    ;;
esac

echo "artifacts: $OUT"
