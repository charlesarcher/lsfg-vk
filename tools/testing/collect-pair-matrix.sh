#!/usr/bin/env bash
# Card-pair matrix from tools/testing/lsfg-launch.sh pair-* run dirs. Two logs carry
# same-named gen/real counts: LSFGVK-BENCH in furmark.log is what the game rendered and
# the layer captured; `present-count kind=gen|real` in app.log is what the doubler
# presented. gen == real on either side is exact 2x; their ratio is render-card lag.
set -uo pipefail

RUNS=${LSFGVK_RUNS_DIR:-$HOME/lsfg-work/runs}
PAIRS=${*:-9070-to-9060 9060-to-9070 9070-to-igpu 9060-to-igpu igpu-to-9060 igpu-to-9070}

printf '%-16s %-9s %-9s %-11s %-14s %-10s %-9s %s\n' \
  pair game-fps game-g/r app-g/r app-out-fps gap_ms hud verdict
for p in $PAIRS; do
  L=$(ls -1dt "$RUNS/pair-$p"/*/ 2>/dev/null | head -1)
  [ -n "${L:-}" ] || { printf '%-16s NO RUN\n' "$p"; continue; }

  BENCH=$(grep -hao 'LSFGVK-BENCH.*' "$L/furmark.log" "$L/launch.log" 2>/dev/null | tail -1)
  GFPS=$(printf '%s' "$BENCH" | sed -n 's/.*presents\/s=\([0-9.]*\).*/\1/p')
  GGR=$(printf '%s' "$BENCH" | sed -n 's/.*gen=\([0-9]*\) real=\([0-9]*\).*/\1\/\2/p')
  GAP=$(printf '%s' "$BENCH" | sed -n 's/.*longest_gap_ms=\([0-9.]*\).*/\1/p')
  ELAPSED=$(printf '%s' "$BENCH" | sed -n 's/.*elapsed_s=\([0-9.]*\).*/\1/p')

  AG=$(grep -ac 'present-count kind=gen' "$L/app.log" 2>/dev/null)
  AR=$(grep -ac 'present-count kind=real' "$L/app.log" 2>/dev/null)
  AGR="$AG/$AR"
  if [ -n "${ELAPSED:-}" ] && [ "${AG:-0}" -gt 0 ]; then
    OUTF=$(awk -v g="$AG" -v r="$AR" -v e="$ELAPSED" 'BEGIN { printf "%.1f", (g + r) / e }')
  else
    OUTF=-
  fi

  HUD=$( [ -f "$L/frame.png" ] && timeout 90 tesseract "$L/frame.png" - --psm 11 2>/dev/null \
          | tr '\n' ' ' | grep -oE '(game|doubled) fps [0-9]+' | tr '\n' ' ' )
  [ -n "${HUD// /}" ] || HUD=$( [ -f "$L/mid.png" ] && timeout 90 tesseract "$L/mid.png" - --psm 6 2>/dev/null \
          | tr '\n' ' ' | grep -oE 'ame fps[0-9]+|oubled fps[0-9]+' | tr '\n' ' ' )
  HUD=${HUD:--}

  ABORT=$(grep -c 'vkWaitForFences() failed' "$L/furmark.log" 2>/dev/null)
  SOCK=$(grep -c 'app socket never appeared' "$L/launch.log" 2>/dev/null)
  if [ "${ABORT:-0}" -gt 0 ]; then VERDICT="game aborted: VK_ERROR_DEVICE_LOST"
  elif [ "${SOCK:-0}" -gt 0 ]; then VERDICT="app never came up in the readiness window"
  elif [ "$GGR" = "$AG" ] 2>/dev/null || [ "$GGR" = "$AGR" ]; then VERDICT="full rate, exact 2x"
  elif [ -n "${GFPS:-}" ]; then VERDICT="2x on what it presented, render card is the limit"
  else VERDICT="incomplete"; fi

  printf '%-16s %-9s %-9s %-11s %-14s %-10s %-9s %s\n' \
    "$p" "${GFPS:--}" "${GGR:--}" "$AGR" "$OUTF" "${GAP:--}" "${HUD:0:44}" "$VERDICT"
done
