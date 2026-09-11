#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# trace_ntsync.sh - Proton/Wine ntsync event wait latency profiler for lsfg-vk.
#
# Measures per-thread wait durations on Windows NT synchronization objects
# and logs timestamp deltas between successive wait releases on the frame-loop thread.
# Detects presentation feedback timer gates (>20ms).
#
# Usage:
#   sudo ./trace_ntsync.sh [seconds] [process_name]
#
# Examples:
#   sudo ./trace_ntsync.sh 60 re2.exe

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: sudo $0 [seconds] [process_name]"
  echo "  seconds        Duration to trace in seconds (default: 60)"
  echo "  process_name   Target process comm name (default: re2.exe)"
  exit 0
fi

DUR=${1:-60}
COMM=${2:-re2.exe}
OUT=/tmp/lsfg-ntsync-${COMM}.txt

if ! command -v bpftrace >/dev/null 2>&1; then
  echo "error: bpftrace is required for ntsync latency profiling" >&2
  exit 1
fi

echo "watcher: waiting for process '$COMM'..." | tee "$OUT"
while true; do
  PIDS=$(ps -eo pid,comm | awk -v c="$COMM" '$2==c {print $1}' || true)
  [[ -n "$PIDS" ]] && break
  sleep 0.5
done
echo "watcher: found '$COMM' (PIDs: $PIDS) - tracing ntsync waits for ${DUR}s" | tee -a "$OUT"

# 0xc0284e82 = NTSYNC_IOC_WAIT_ANY ioctl request code
timeout --signal=INT "$DUR" bpftrace -e "
#include <linux/sched.h>
tracepoint:raw_syscalls:sys_enter
/comm == \"$COMM\" && args->id == 16 && args->args[1] == 0xc0284e82/
{
    @start[tid] = nsecs;
}
tracepoint:raw_syscalls:sys_exit
/comm == \"$COMM\" && @start[tid]/
{
    \$d = nsecs - @start[tid];
    @dur_hist = hist(\$d / 1000);
    @per_tid[tid] = hist(\$d / 1000);
    if (\$d > 20000000) {
        printf(\"LONG WAIT tid=%d dur=%dms\\n\", tid, \$d / 1000000);
    }
    delete(@start[tid]);
}
interval:s:10 { print(@dur_hist); clear(@dur_hist); }
" 2>&1 | tee -a "$OUT"

echo "watcher: done, output saved in $OUT"
