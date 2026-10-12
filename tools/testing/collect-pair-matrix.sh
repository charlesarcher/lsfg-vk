#!/usr/bin/env bash
# Card-pair matrix from tools/testing/lsfg-launch.sh pair-* run dirs. Two logs carry
# same-named gen/real counts: LSFGVK-BENCH in furmark.log is what the game rendered and
# the layer captured; `present-count kind=gen|real` in app.log is what the doubler
# presented. gen == real on either side is exact 2x; their ratio is render-card lag.
# recorder.log's `damage fps` is how often the *screen* changed, so it saturates at the
# panel rate and cannot tell a 264 fps stream from an 11 fps one - shown for completeness.
set -uo pipefail

RUNS=${LSFGVK_RUNS_DIR:-$HOME/lsfg-work/runs}
PAIRS=${*:-9070-to-9060 9060-to-9070 9070-to-igpu 9060-to-igpu igpu-to-9060 igpu-to-9070}

hud_of() {
  local L=$1 f
  if ! ls "$L"/*.png >/dev/null 2>&1 && [ -f "$L/cap.mp4" ]; then
    for t in 60 90 25; do
      /usr/bin/ffmpeg -nostdin -v error -ss $t -i "$L/cap.mp4" -frames:v 1 -y "$L/hud$t.png" 2>/dev/null
    done
  fi
  for f in frame.png hud60.png hud90.png mid.png hud45.png hud30.png hud20.png; do
    [ -f "$L/$f" ] || continue
    for psm in 11 6; do
      local r
      r=$(timeout 90 tesseract "$L/$f" - --psm "$psm" 2>/dev/null | tr '\n' ' ' \
          | grep -oE '(game|doubled)[ _]?fps[ _]?[0-9]+' | grep -oE '[0-9]+' | paste -sd/ -)
      [ -n "${r:-}" ] && { printf '%s (%s) ' "$r" "${f%.png}"; return 0; }
    done
  done
  printf '-'
}

damage_of() {
  [ -f "$1/recorder.log" ] || { printf '-'; return; }
  grep -aoE 'damage fps: [0-9]+' "$1/recorder.log" | awk '{print $3}' | sort -n \
    | awk '{a[NR]=$1} END {if (!NR) {print "-"; exit} printf "%s-%s", a[1], a[NR]}'
}

printf '%-15s %-8s %-12s %-12s %-8s %-9s %-7s %-9s %s\n' \
  pair game-fps captured presented ratio out-fps damage gap_ms hud
for p in $PAIRS; do
  L=$(ls -1dt "$RUNS/pair-$p"/*/ 2>/dev/null | head -1)
  [ -n "${L:-}" ] || L=$(ls -1dt "$RUNS/$p"/*/ 2>/dev/null | head -1)
  [ -n "${L:-}" ] || { printf '%-15s NO RUN\n' "$p"; continue; }

  BENCH=$(grep -hao 'LSFGVK-BENCH.*' "$L/furmark.log" "$L/launch.log" 2>/dev/null | tail -1)
  GFPS=$(printf '%s' "$BENCH" | sed -n 's/.*presents\/s=\([0-9.]*\).*/\1/p')
  CAP=$(printf '%s' "$BENCH" | sed -n 's/.*gen=\([0-9]*\) real=\([0-9]*\).*/\1\/\2/p')
  GAP=$(printf '%s' "$BENCH" | sed -n 's/.*longest_gap_ms=\([0-9.]*\).*/\1/p')
  E=$(printf '%s' "$BENCH" | sed -n 's/.*elapsed_s=\([0-9.]*\).*/\1/p')

  AG=$(grep -ac 'present-count kind=gen' "$L/app.log" 2>/dev/null)
  AR=$(grep -ac 'present-count kind=real' "$L/app.log" 2>/dev/null)
  # (gen + real) / real, so 2.0000 means every captured frame made exactly one generated frame
  RATIO=$(awk -v g="$AG" -v r="$AR" 'BEGIN { if (r+0 > 0) printf "%.4f", (g + r) / r; else printf "-" }')
  OUTF=$(awk -v g="$AG" -v e="${E:-0}" 'BEGIN { if (e+0 > 0) { printf "%.1f", 2 * g / e; exit } printf "-" }')

  ABORT=$(grep -c 'vkWaitForFences() failed' "$L/furmark.log" 2>/dev/null)
  VERDICT=""
  [ "${ABORT:-0}" -gt 0 ] && VERDICT="  <- game aborted: VK_ERROR_DEVICE_LOST on frame 1"
  # a 1/1 pair would still divide out to 2.0000, so a pair that never got past frame 1
  # reports no ratio at all rather than a passing-looking number
  [ "${ABORT:-0}" -gt 0 ] && RATIO=-

  printf '%-15s %-8s %-12s %-12s %-8s %-9s %-7s %-9s %s%s\n' \
    "$p" "${GFPS:--}" "${CAP:--}" "$AG/$AR" "$RATIO" "$OUTF" \
    "$(damage_of "$L")" "${GAP:--}" "$(hud_of "$L")" "$VERDICT"
done
