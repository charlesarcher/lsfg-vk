#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# trace_offcpu.sh - eBPF off-CPU scheduling stall profiler for lsfg-vk.
#
# Pairs raw_syscalls enter/exit per thread and sums blocked time by syscall ID.
# Pinpoints scheduler stalls, fence waits, and compositor sync overheads:
#   - futex          => thread synchronization (present thread handshake)
#   - poll / ppoll   => X11 / Wayland socket wait (compositor feedback)
#   - ioctl (id 16)  => GPU command submission (amdgpu / drm) or ntsync
#   - nanosleep etc. => engine frame pacing
#
# Usage:
#   sudo ./trace_offcpu.sh [--ioctls] [seconds] [process_name]
#
# Examples:
#   sudo ./trace_offcpu.sh 60 re2.exe
#   sudo ./trace_offcpu.sh --ioctls 60 re2.exe
#   sudo ./trace_offcpu.sh 30 furmark

set -euo pipefail

IOCTLS_MODE=0
if [[ "${1:-}" == "--ioctls" ]]; then
  IOCTLS_MODE=1
  shift
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  echo "Usage: sudo $0 [--ioctls] [seconds] [process_name]"
  echo ""
  echo "Options:"
  echo "  --ioctls       Attribute blocked time per thread and break down ioctl request codes"
  echo "  seconds        Duration to trace in seconds (default: 60)"
  echo "  process_name   Target process comm name (default: re2.exe)"
  exit 0
fi

DUR=${1:-60}
COMM=${2:-re2.exe}
OUT=/tmp/lsfg-offcpu-${COMM}.txt

if ! command -v bpftrace >/dev/null 2>&1; then
  echo "error: bpftrace is required for eBPF off-CPU profiling" >&2
  exit 1
fi

echo "watcher: waiting for process '$COMM'..." | tee "$OUT"
while true; do
  PIDS=$(ps -eo pid,comm | awk -v c="$COMM" '$2==c {print $1}' || true)
  [[ -n "$PIDS" ]] && break
  sleep 0.5
done
echo "watcher: found '$COMM' (PIDs: $PIDS) - tracing for ${DUR}s (ioctls=${IOCTLS_MODE})" | tee -a "$OUT"

if [[ "$IOCTLS_MODE" == "1" ]]; then
  # Per-thread syscall block census + ioctl request code breakdown
  timeout --signal=INT "$DUR" bpftrace -e "
tracepoint:raw_syscalls:sys_enter
/comm == \"$COMM\"/
{
    @start[tid] = nsecs;
    @which[tid] = args->id;
    if (args->id == 16) { @ioctlcmd[tid] = args->args[1]; }
}

tracepoint:raw_syscalls:sys_exit
/comm == \"$COMM\" && @start[tid]/
{
    @[tid, @which[tid]] = sum(nsecs - @start[tid]);
    @cnt[tid, @which[tid]] = count();
    if (@which[tid] == 16) {
        @iocmd[@ioctlcmd[tid]] = sum(nsecs - @start[tid]);
        @iocmdcnt[@ioctlcmd[tid]] = count();
    }
    delete(@start[tid]);
    delete(@which[tid]);
    delete(@ioctlcmd[tid]);
}

interval:s:15 {
    print(@); clear(@);
    print(@cnt); clear(@cnt);
    print(@iocmd); clear(@iocmd);
    print(@iocmdcnt); clear(@iocmdcnt);
}
" 2>&1 | tee -a "$OUT"
else
  # Aggregated process-wide syscall census
  timeout --signal=INT "$DUR" bpftrace -e "
tracepoint:raw_syscalls:sys_enter
/comm == \"$COMM\"/
{ @start[tid] = nsecs; @which[tid] = args->id; }

tracepoint:raw_syscalls:sys_exit
/comm == \"$COMM\" && @start[tid]/
{
    @[comm, @which[tid]] = sum(nsecs - @start[tid]);
    delete(@start[tid]);
    delete(@which[tid]);
}

interval:s:10 { print(@); clear(@); }
" 2>&1 | tee -a "$OUT"
fi

echo "watcher: profiling complete, results saved in $OUT"
