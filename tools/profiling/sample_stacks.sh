#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# sample_stacks.sh - Hot-path thread stack sampler for lsfg-vk.
#
# Attaches non-invasively to target process (using eu-stack or gdb batch mode)
# and captures full thread backtraces during active frame doubling.
#
# Usage:
#   ./sample_stacks.sh [process_name] [warmup_seconds] [samples] [interval_seconds]
#
# Examples:
#   ./sample_stacks.sh re2.exe 45 5 8
#   ./sample_stacks.sh furmark 5 3 2

set -euo pipefail

COMM=${1:-re2.exe}
WARMUP=${2:-30}
SAMPLES=${3:-5}
INTERVAL=${4:-5}
OUT=/tmp/lsfg-stacks-${COMM}.txt

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: $0 [process_name] [warmup_seconds] [samples] [interval_seconds]"
  echo "  process_name      Process comm to attach to (default: re2.exe)"
  echo "  warmup_seconds    Seconds to wait for gameplay before sampling (default: 30)"
  echo "  samples           Number of stack snapshots to take (default: 5)"
  echo "  interval_seconds  Seconds between snapshots (default: 5)"
  exit 0
fi

echo "watcher: waiting for '$COMM'..." | tee "$OUT"
while true; do
  PID=$(ps -eo pid,comm | awk -v c="$COMM" '$2==c {print $1; exit}' || true)
  [[ -n "$PID" ]] && break
  sleep 0.5
done
echo "watcher: found '$COMM' PID $PID - waiting ${WARMUP}s for steady-state gameplay..." | tee -a "$OUT"
sleep "$WARMUP"

for i in $(seq 1 "$SAMPLES"); do
  echo "===== sample $i/$SAMPLES (t+${i}x${INTERVAL}s) =====" >> "$OUT"
  if command -v eu-stack >/dev/null 2>&1; then
    sudo -n timeout 15 eu-stack -p "$PID" >> "$OUT" 2>&1 || true
  elif command -v gdb >/dev/null 2>&1; then
    sudo -n timeout 15 gdb -p "$PID" -batch -ex "thread apply all bt 12" >> "$OUT" 2>&1 || true
  else
    echo "neither eu-stack nor gdb available" >> "$OUT"
    break
  fi
  sleep "$INTERVAL"
done

echo "watcher: sampling complete, backtraces written to $OUT"
