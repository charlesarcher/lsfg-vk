#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# bench_furmark.sh - Automated FurMark A/B performance benchmark for lsfg-vk.
#
# Measures native rendering vs. one-way decoupled dual-GPU offload throughput,
# presented framerates, and frame latency.
#
# Default topology:
#   render          = AMD Radeon RX 9070 XT (Vulkan gpu-index 1, 1002:7550)
#   process/present = AMD Radeon RX 9060 XT (lsfg-vk-app, 1002:7590)
#
# Usage:
#   ./bench_furmark.sh {baseline|doubled|ab} [seconds] [preset]
#
# Presets:
#   bound  2560x1440 MSAA4 vsync0 (GPU-bound realism; target ~140-150 fps; default)
#   light  1280x720  MSAA1 vsync0 (lightweight; exposes present-path ceiling)
#   panel  2560x1440 MSAA1 vsync0 (native resolution, minimal MSAA)
#   heavy  2560x1440 MSAA8 vsync0 (heavy multisampling load)
#
# Environment flags:
#   MANGOHUD=0          Disable MangoHud for pure headless numeric A/B
#   RENDER_GPU_INDEX=N   Vulkan GPU index for FurMark (default: 1)
#   LSFGVK_PROFILE=name  Layer profile override (default: furmark-oneway)

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 {baseline|doubled|ab} [seconds] [preset]"
  echo ""
  echo "Modes:"
  echo "  baseline - run unlayered native FurMark on render GPU"
  echo "  doubled  - run one-way decoupled frame doubling with lsfg-vk-app"
  echo "  ab       - run baseline followed by doubled and calculate retention %"
  echo ""
  echo "Presets:"
  echo "  bound    - 2560x1440 MSAA4 vsync0 (GPU-bound, target ~140-150 fps; default)"
  echo "  light    - 1280x720  MSAA1 vsync0 (high native fps; present-path limit)"
  echo "  panel    - 2560x1440 MSAA1 vsync0 (panel match)"
  echo "  heavy    - 2560x1440 MSAA8 vsync0 (heavy multisampling)"
  exit 0
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE=${1:-ab}
SECS=${2:-8}
PRESET=${3:-bound}
SOCK=${LSFGVK_APP_SOCK:-$HOME/.local/state/lsfg-vk/app.sock}
APP="$REPO_ROOT/build/lsfg-vk-app/lsfg-vk-app"
LAYER_DIR="$REPO_ROOT/build/lsfg-vk-layer"
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

# FurMark Vulkan device list: 0=9060, 1=9070, 2=Intel (on standard tri-GPU rig)
FURMARK_VK_GPU="${RENDER_GPU_INDEX:-1}"

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
trap cleanup EXIT INT TERM

require_build() {
  if [[ ! -x "$APP" ]]; then
    echo "missing $APP - run cmake --build build first" >&2
    exit 1
  fi
  if [[ ! -f "$LAYER_DIR/liblsfg-vk-layer.so" ]]; then
    echo "missing layer .so in $LAYER_DIR - run cmake --build build first" >&2
    exit 1
  fi
  if ! command -v furmark >/dev/null 2>&1; then
    echo "missing furmark binary on PATH" >&2
    exit 1
  fi
}

start_app() {
  if pgrep -x lsfg-vk-app >/dev/null; then
    echo "lsfg-vk-app already running, using existing instance"
    STARTED_APP=0
    APP_PID=$(pgrep -n -x lsfg-vk-app)
    return
  fi
  mkdir -p "$(dirname "$SOCK")"
  rm -f "$SOCK"
  STARTED_APP=1
  # Launch lsfg-vk-app without inherited layer vars so it does not layer itself
  env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
    LSFGVK_APP_DBG=1 \
    LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}" \
    LSFGVK_APP_SOCK="$SOCK" \
    "$APP" --profile "${LSFGVK_APP_PROFILE:-app-oneway}" --session wayland >"$OUT/app.log" 2>&1 &
  APP_PID=$!
  for _ in $(seq 1 60); do
    if [[ -S "$SOCK" ]] && kill -0 "$APP_PID" 2>/dev/null; then
      return
    fi
    sleep 0.05
  done
  echo "timed out waiting for lsfg-vk-app socket $SOCK" >&2
  cat "$OUT/app.log" >&2
  exit 1
}

run_furmark_cmd() {
  local logfile="$1"
  shift
  # Unset any stray wine/layer vars, then apply invocation args
  env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
    "$@" \
    /usr/bin/furmark \
      --demo furmark-vk \
      --gpu-index "$FURMARK_VK_GPU" \
      --width "$W" --height "$H" \
      --msaa "$MSAA" \
      --vsync 0 \
      --no-resize \
      --disable-demo-options \
      --print-render-speed \
      --no-score-box \
      --max-time "$SECS" \
      --no-osi \
      >"$logfile" 2>&1 &
  FURMARK_PID=$!
  set +e
  wait "$FURMARK_PID"
  local rc=$?
  set -e
  FURMARK_PID=""
  return $rc
}

parse_stats() {
  local logfile="$1"
  local label="$2"
  local fps_line
  fps_line=$(grep "FPS (min/avg/max)" "$logfile" || true)
  local renderer
  renderer=$(grep "renderer" "$logfile" | head -n1 || true)
  local frames
  frames=$(grep "frames  " "$logfile" | head -n1 || true)
  echo "=== $label ==="
  echo "  log: $logfile"
  [[ -n "$renderer" ]] && echo "  $renderer"
  [[ -n "$frames" ]]   && echo "  $frames"
  if [[ -n "$fps_line" ]]; then
    echo "  $fps_line"
  else
    local last_fps
    last_fps=$(grep "frame " "$logfile" | tail -n1 || true)
    echo "  no summary line; last: $last_fps"
  fi
}

run_baseline() {
  require_build
  echo "--- running baseline (${SECS}s, ${W}x${H} MSAA${MSAA}) ---"
  local rc=0
  if [[ "${MANGOHUD:-1}" == "1" ]] && command -v mangohud >/dev/null 2>&1; then
    run_furmark_cmd "$OUT/baseline.log" mangohud --dlsym || rc=$?
  else
    run_furmark_cmd "$OUT/baseline.log" || rc=$?
  fi
  parse_stats "$OUT/baseline.log" "baseline (native, no layer)"
  return $rc
}

run_doubled() {
  require_build
  start_app
  echo "--- running doubled (${SECS}s, ${W}x${H} MSAA${MSAA}) ---"
  local rc=0
  local layer_env=(
    VK_LAYER_PATH="$LAYER_DIR"
    VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
    LSFGVK_PROFILE="${LSFGVK_PROFILE:-furmark-oneway}"
    LSFGVK_CONFIG="${LSFGVK_CONFIG:-$HOME/.config/lsfg-vk/conf.toml}"
    LSFGVK_APP_SOCK="$SOCK"
    LSFGVK_LAYER_DBG=1
  )
  if [[ "${MANGOHUD:-1}" == "1" ]] && command -v mangohud >/dev/null 2>&1; then
    run_furmark_cmd "$OUT/doubled.log" "${layer_env[@]}" mangohud --dlsym || rc=$?
  else
    run_furmark_cmd "$OUT/doubled.log" "${layer_env[@]}" || rc=$?
  fi
  parse_stats "$OUT/doubled.log" "doubled (one-way decoupled layer)"
  if [[ -f "$OUT/app.log" ]]; then
    echo "  app presented sample:"
    grep -E 'fps game|latency|wait_import' "$OUT/app.log" | tail -n 4 | sed 's/^/    /' || true
  fi
  return $rc
}

run_ab() {
  echo "=========================================================="
  echo "  lsfg-vk A/B Benchmark (Preset: $PRESET, ${W}x${H} MSAA${MSAA}, ${SECS}s each)"
  echo "=========================================================="
  run_baseline
  echo ""
  sleep 1
  run_doubled
  echo ""
  echo "=========================================================="
  echo "  A/B Summary Comparison"
  echo "=========================================================="
  local base_avg doubler_avg
  base_avg=$(grep "FPS (min/avg/max)" "$OUT/baseline.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)
  doubler_avg=$(grep "FPS (min/avg/max)" "$OUT/doubled.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)
  if [[ -n "$base_avg" && -n "$doubler_avg" && "$base_avg" != "0" ]]; then
    local pct
    pct=$(awk "BEGIN { printf \"%.2f\", ($doubler_avg / $base_avg) * 100 }")
    echo "  Baseline Native Rate:  ${base_avg} FPS"
    echo "  Doubled Game Rate:     ${doubler_avg} FPS"
    echo "  Throughput Retention:  ${pct}% (target: >= 95.00%)"
  fi
  echo "  Artifact directory: $OUT"
}

case "$MODE" in
  baseline) run_baseline ;;
  doubled)  run_doubled ;;
  ab)       run_ab ;;
  *) echo "unknown mode: $MODE (baseline|doubled|ab)" >&2; exit 2 ;;
esac
