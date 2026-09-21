#!/usr/bin/env bash
# S42r leak-clean harness — ASan/LSan pair soak for lsfg-vk.
# Usage: tools/s42r-leak-check.sh [stream-seconds]
# Requires: build-asan/ (cmake -B build-asan with -fsanitize=address,leak)
#           probe compiled as /tmp/probe_vk_asan (see journal S42r)
set -euo pipefail
# valgrind/memcheck on CachyOS aborts at startup when
# DEBUGINFOD_URLS is unset (its memcmp-redirect init path needs the
# debuginfod client initialized first). Export any URL:
export DEBUGINFOD_URLS="${DEBUGINFOD_URLS:-https://debuginfod.cachyos.org}"
SECS=${1:-30}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
rm -f ~/.local/state/lsfg-vk/app.sock /dev/shm/lsfg-dbl-* /tmp/s42r-lsan-*
echo '== launch ASan app'
env -u LSFGVK_APP_DBG LSFGVK_DUMP_PRESENT=0 \
    ASAN_OPTIONS="detect_leaks=1:log_path=/tmp/s42r-lsan-app:halt_on_error=0" \
    LSAN_OPTIONS="suppressions=$ROOT/tools/lsan-suppress.txt" \
    "$ROOT/build-asan/lsfg-vk-app/lsfg-vk-app" \
    --profile app-oneway --session wayland 2>&1 | tee /tmp/s42r-app.log &
APP=$!; APP=$(jobs -p | tail -1)
sleep 9
echo '== stream ASan probe (the plain probe binary must be ASan-built)'
LSFGVK_PROFILE=app-oneway LSFGVK_CONFIG=~/.config/lsfg-vk/conf.toml \
LSFGVK_APP_SOCK=~/.local/state/lsfg-vk/app.sock LSFGVK_DBL_LEDGER=1 \
LSFGVK_LAYER_SHELL=1 LSFGVK_PROBE_SIZE=2560x1440 MESA_VK_DEVICE_SELECT=1002:7590 \
VK_LAYER_PATH="$ROOT/build/lsfg-vk-layer" \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
ASAN_OPTIONS="detect_leaks=1" \
/tmp/probe_vk_asan "$SECS" 2>&1 | tee /tmp/s42r-probe.log
sleep 3
echo '== SIGINT the app so LSan runs at exit'
kill -INT "$APP" 2>/dev/null || true
for _ in $(seq 1 30); do kill -0 "$APP" 2>/dev/null || break; sleep 1; done
kill -0 "$APP" 2>/dev/null && { echo 'app did not exit'; kill -9 "$APP"; }
echo '== app LSan: (suppress libvulkan_radeon + libwayland-client only)'
tail -14 /tmp/s42r-lsan-app.* 2>/dev/null | grep -vE 'RUNNING UNDER|==[0-9]+==Registered|==[0-9]+==Unregistered|==[0-9]+==AddressSanitizer: failed to intercept'
echo '== probe LSan rows (in s42r-probe.log stderr):'
grep -E 'LeakSanitizer|SUMMARY: ' /tmp/s42r-probe.log | head -6 || echo '(none — leak-free)'
