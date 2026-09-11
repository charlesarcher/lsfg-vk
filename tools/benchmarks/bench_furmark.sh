#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# bench_furmark.sh - Automated FurMark A/B and Transport Benchmark for lsfg-vk.
#
# Measures:
#   1. Native rendering vs. one-way decoupled dual-GPU offload throughput.
#   2. Transport latency comparison:
#      - POSIX SHM (CPU copy: CopyHop pixel walk -> shm -> doubler memcpy, ~2-6ms latency)
#      - DMA-BUF (Zero-copy hardware DMA: parks ~28-38ms on PRIME implicit-sync)
#
# Topology:
#   render          = AMD Radeon RX 9070 XT (Vulkan gpu-index 1, 1002:7550)
#   process/present = AMD Radeon RX 9060 XT (lsfg-vk-app, 1002:7590)
#
# Usage:
#   ./bench_furmark.sh {baseline|doubled|ab|transport-compare} [seconds] [preset] [transport]
#
# Transports:
#   shm      POSIX shared memory CPU copy (default for live 2x doubling)
#   dmabuf   Zero-copy hardware DMA-BUF export (exposes PRIME implicit-sync parking)
#
# Presets:
#   bound    2560x1440 MSAA4 vsync0 (GPU-bound realism; target ~140-150 fps; default)
#   light    1280x720  MSAA1 vsync0 (lightweight; exposes presentation queue bottlenecks)
#   panel    2560x1440 MSAA1 vsync0
#   heavy    2560x1440 MSAA8 vsync0

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 {baseline|doubled|ab|transport-compare} [seconds] [preset] [transport]"
  echo ""
  echo "Modes:"
  echo "  baseline          - run unlayered native FurMark on render GPU"
  echo "  doubled           - run one-way decoupled frame doubling with lsfg-vk-app"
  echo "  ab                - run baseline vs doubled in selected transport"
  echo "  transport-compare - 3-way benchmark: baseline vs POSIX SHM (CPU copy) vs DMA-BUF (zero-copy)"
  echo ""
  echo "Presets:"
  echo "  bound             - 2560x1440 MSAA4 vsync0 (GPU-bound, ~140-150 fps; default)"
  echo "  light             - 1280x720  MSAA1 vsync0 (lightweight)"
  echo "  panel             - 2560x1440 MSAA1 vsync0"
  echo "  heavy             - 2560x1440 MSAA8 vsync0"
  echo ""
  echo "Transports:"
  echo "  shm               - POSIX SHM CPU copy (CopyHop, ~2.2-6.5ms latency; default)"
  echo "  dmabuf            - Zero-copy DMA-BUF (exposes ~30ms PRIME implicit sync wait)"
  exit 0
fi

REAL_SOURCE="$(readlink -f "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd "$(dirname "$REAL_SOURCE")/../.." && pwd)"
MODE=${1:-ab}
SECS=${2:-8}
PRESET=${3:-bound}
TRANSPORT=${4:-${LSFGVK_TRANSPORT:-${TRANSPORT:-shm}}}
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

FURMARK_VK_GPU="${RENDER_GPU_INDEX:-1}"

DEFAULT_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
if [[ ! -f "$DEFAULT_CONFIG" && -f "$REPO_ROOT/tools/conf.toml.example" ]]; then
  DEFAULT_CONFIG="$REPO_ROOT/tools/conf.toml.example"
fi
CONFIG_FILE="${LSFGVK_CONFIG:-$DEFAULT_CONFIG}"

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
  local logname="${1:-app.log}"
  local shm_flag="${2:-1}"
  local dual_host_flag="${3:-0}"
  if pgrep -x lsfg-vk-app >/dev/null; then
    echo "lsfg-vk-app already running, terminating to apply fresh profile/transport..."
    pkill -x lsfg-vk-app 2>/dev/null || true
    for _ in $(seq 1 30); do
      if ! pgrep -x lsfg-vk-app >/dev/null; then break; fi
      sleep 0.1
    done
  fi
  mkdir -p "$(dirname "$SOCK")"
  rm -f "$SOCK"
  STARTED_APP=1

  env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u LSFGVK_LAYER_DBG \
    LSFGVK_APP_DBG=1 \
    LSFGVK_POSIX_SHM="$shm_flag" \
    LSFGVK_DUAL_HOST="$dual_host_flag" \
    LSFGVK_CONFIG="$CONFIG_FILE" \
    LSFGVK_APP_SOCK="$SOCK" \
    "$APP" --profile "${LSFGVK_APP_PROFILE:-app-oneway}" --session wayland >"$OUT/$logname" 2>&1 &
  APP_PID=$!
  for _ in $(seq 1 60); do
    if [[ -S "$SOCK" ]] && kill -0 "$APP_PID" 2>/dev/null; then
      return
    fi
    sleep 0.05
  done
  echo "timed out waiting for lsfg-vk-app socket $SOCK" >&2
  cat "$OUT/$logname" >&2
  exit 1
}

run_furmark_cmd() {
  local logfile="$1"
  shift
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
  local suffix="${1:-}"
  local log_app="app${suffix}.log"
  local log_doubled="doubled${suffix}.log"
  require_build

  local shm_flag="1"
  local dual_host_flag="0"
  if [[ "$TRANSPORT" == "dmabuf" || "$TRANSPORT" == "dma" ]]; then
    shm_flag="0"
    dual_host_flag="0"
  elif [[ "$TRANSPORT" == "decoupled" || "$TRANSPORT" == "decoupled_dma" || "$TRANSPORT" == "zero_copy" ]]; then
    shm_flag="0"
    dual_host_flag="1"
  fi

  start_app "$log_app" "$shm_flag" "$dual_host_flag"

  echo "--- running doubled [transport=$TRANSPORT, shm=$shm_flag, dual_host=$dual_host_flag] (${SECS}s, ${W}x${H} MSAA${MSAA}) ---"
  local rc=0
  local layer_env=(
    VK_LAYER_PATH="$LAYER_DIR"
    VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
    LSFGVK_PROFILE="${LSFGVK_PROFILE:-furmark-oneway}"
    LSFGVK_CONFIG="$CONFIG_FILE"
    LSFGVK_APP_SOCK="$SOCK"
    LSFGVK_POSIX_SHM="$shm_flag"
    LSFGVK_DUAL_HOST="$dual_host_flag"
    LSFGVK_LAYER_DBG=1
  )
  if [[ "${MANGOHUD:-1}" == "1" ]] && command -v mangohud >/dev/null 2>&1; then
    run_furmark_cmd "$OUT/$log_doubled" "${layer_env[@]}" mangohud --dlsym || rc=$?
  else
    run_furmark_cmd "$OUT/$log_doubled" "${layer_env[@]}" || rc=$?
  fi
  parse_stats "$OUT/$log_doubled" "doubled (one-way decoupled [${TRANSPORT}])"
  if [[ -f "$OUT/$log_app" ]]; then
    echo "  app presented sample:"
    grep -E 'fps game|latency|wait_import' "$OUT/$log_app" | tail -n 4 | sed 's/^/    /' || true
  fi
  return $rc
}

run_ab() {
  echo "=========================================================="
  echo "  lsfg-vk A/B Benchmark (Preset: $PRESET, ${W}x${H} MSAA${MSAA}, Transport: $TRANSPORT)"
  echo "=========================================================="
  run_baseline || true
  echo ""
  sleep 1
  run_doubled || true
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

run_transport_compare() {
  echo "================================================================================"
  echo "  lsfg-vk Transport Comparison: 3 Forward FG Mechanisms vs Native Baseline"
  echo "  Preset: $PRESET (${W}x${H} MSAA${MSAA}), Duration: ${SECS}s each"
  echo "================================================================================\n"
  
  echo "=== 1. Native Baseline (Direct Swapchain, No Layer) ==="
  run_baseline || true
  echo ""
  sleep 1

  echo "=== 2. Mechanism 1: POSIX SHM (CPU Copy: CopyHop -> memfd -> doubler memcpy) ==="
  TRANSPORT="shm"
  run_doubled "-shm" || true
  echo ""
  sleep 1

  echo "=== 3. Mechanism 2: Direct DMA-BUF (Zero-Copy with Kernel Implicit-Sync Parking) ==="
  TRANSPORT="dmabuf"
  run_doubled "-dmabuf" || true
  echo ""
  sleep 1

  echo "=== 4. Mechanism 3: Decoupled Zero-Copy DMA (WITHOUT Kernel Implicit Sync) ==="
  TRANSPORT="decoupled"
  run_doubled "-decoupled" || true
  echo ""

  echo "================================================================================"
  echo "  Transport Latency & Performance Summary (3 Forward FG Mechanisms)"
  echo "================================================================================"
  local base_avg shm_avg dma_avg dec_avg
  base_avg=$(grep "FPS (min/avg/max)" "$OUT/baseline.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)
  shm_avg=$(grep "FPS (min/avg/max)" "$OUT/doubled-shm.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)
  dma_avg=$(grep "FPS (min/avg/max)" "$OUT/doubled-dmabuf.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)
  dec_avg=$(grep "FPS (min/avg/max)" "$OUT/doubled-decoupled.log" | awk -F'/' '{gsub(/ /,"",$4); print $4}' || true)

  local shm_real shm_gen dma_real dma_gen dec_real dec_gen
  shm_real=$(grep "MEASURED LATENCY: REAL present slot" "$OUT/app-shm.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)
  shm_gen=$(grep "MEASURED LATENCY: GEN present slot" "$OUT/app-shm.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)
  dma_real=$(grep "MEASURED LATENCY: REAL present slot" "$OUT/app-dmabuf.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)
  dma_gen=$(grep "MEASURED LATENCY: GEN present slot" "$OUT/app-dmabuf.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)
  dec_real=$(grep "MEASURED LATENCY: REAL present slot" "$OUT/app-decoupled.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)
  dec_gen=$(grep "MEASURED LATENCY: GEN present slot" "$OUT/app-decoupled.log" | tail -n 1 | sed -E 's/.*latency ([0-9\.]+) ms.*/\1/' || true)

  printf "  %-24s | %-10s | %-12s | %-12s | %s\n" "Mechanism" "Render FPS" "Real Latency" "Gen Latency" "Characteristics"
  echo "  -------------------------+------------+--------------+--------------+--------------------------------"
  printf "  %-24s | %-10s | %-12s | %-12s | %s\n" "Native Baseline (Ref)" "${base_avg:-0} FPS" "N/A" "N/A" "Direct swapchain, zero layer overhead"
  printf "  %-24s | %-10s | %-12s | %-12s | %s\n" "1. POSIX SHM (CPU copy)" "${shm_avg:-0} FPS" "${shm_real:-0} ms" "${shm_gen:-0} ms" "Working 2x path; CPU walk + memcpy (~2ms)"
  printf "  %-24s | %-10s | %-12s | %-12s | %s\n" "2. DMA-BUF (Implicit)" "${dma_avg:-0} FPS" "${dma_real:-0} ms" "${dma_gen:-0} ms" "Zero-copy DMA; parks ~30ms on implicit sync"
  printf "  %-24s | %-10s | %-12s | %-12s | %s\n" "3. Decoupled DMA (No-Sync)" "${dec_avg:-0} FPS" "${dec_real:-0} ms" "${dec_gen:-0} ms" "Zero-copy DMA; no cross-GPU GEM resv"
  echo ""
  if [[ -n "${dma_real:-}" && -n "${shm_real:-}" ]]; then
    local delta
    delta=$(awk "BEGIN { printf \"%.2f\", $dma_real - $shm_real }")
    echo "  Implicit-sync parking penalty: +${delta} ms latency over CPU copy path."
  fi
  echo "  Artifact directory: $OUT"
}

case "$MODE" in
  baseline)          run_baseline ;;
  doubled)           run_doubled ;;
  ab)                run_ab ;;
  transport-compare) run_transport_compare ;;
  *) echo "unknown mode: $MODE (baseline|doubled|ab|transport-compare)" >&2; exit 2 ;;
esac
