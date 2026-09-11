#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# trace_wake.sh - Cross-thread scheduler wake latency profiler for lsfg-vk.
#
# Records:
#   1) Wakeup events and latency on target game threads.
#   2) Slow ioctl waits (>8ms) to correlate engine wake events with display flips.
#
# Usage:
#   sudo ./trace_wake.sh [seconds] [process_name]

set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: sudo $0 [seconds] [process_name]"
  echo "  seconds        Duration to trace in seconds (default: 60)"
  echo "  process_name   Target process comm name (default: re2.exe)"
  exit 0
fi

DUR=${1:-60}
COMM=${2:-re2.exe}
OUT=/tmp/lsfg-wake-${COMM}.txt

if ! command -v bpftrace >/dev/null 2>&1; then
  echo "error: bpftrace is required for wake latency profiling" >&2
  exit 1
fi

echo "watcher: waiting for process '$COMM'..." | tee "$OUT"
while true; do
  PIDS=$(ps -eo pid,comm | awk -v c="$COMM" '$2==c {print $1}' || true)
  [[ -n "$PIDS" ]] && break
  sleep 0.5
done
echo "watcher: found '$COMM' (PIDs: $PIDS) - tracing wake latency for ${DUR}s" | tee -a "$OUT"

timeout --signal=INT "$DUR" bpftrace -e "
#include <linux/sched.h>
tracepoint:raw_syscalls:sys_enter
/comm == \"$COMM\" && args->id == 16/
{
    @ioctl_start[tid] = nsecs;
}
tracepoint:raw_syscalls:sys_exit
/comm == \"$COMM\" && @ioctl_start[tid]/
{
    \$d = (nsecs - @ioctl_start[tid]) / 1000000;
    if (\$d > 8) {
        @slowioctl[tid] = count();
        @slowioctl_ms[tid] = sum(\$d);
    }
    delete(@ioctl_start[tid]);
}
tracepoint:sched:sched_wakeup
/comm == \"$COMM\"/
{
    @wk[comm, tid] = count();
}
" 2>&1 | tee -a "$OUT"

echo "watcher: done, output saved in $OUT"
