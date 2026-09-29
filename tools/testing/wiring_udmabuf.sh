#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# udmabuf WIRING CHECK (not a benchmark). vkcube 512x256, app-oneway profile,
# udmabuf transport, 30 s.
#
# Refuses to report unless:
#   * the validation layer is actually mapped into BOTH processes
#   * app and client loaded the SAME layer .so
#   * the app is really listening on app.sock
# Numbers here are wiring evidence only. Performance comes from FurMark
# fullscreen 1440p, and only after this passes.
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=build-framedbg
RENDER=$(env -u VK_INSTANCE_LAYERS -u LSFGVK_PROFILE \
        ./tools/testing/gpu_index.sh 0000:04:00.0) || exit 1
DOUBLER=$(env -u VK_INSTANCE_LAYERS -u LSFGVK_PROFILE \
        ./tools/testing/gpu_index.sh 0000:87:00.0) || exit 1
echo "render 04:00.0 -> vulkan $RENDER ; doubler 87:00.0 -> vulkan $DOUBLER"

# VK_LAYER_PATH must ALSO see the system layer directory: restricting it to
# just our build dir made VK_LAYER_KHRONOS_validation unresolvable and
# vkCreateInstance failed -6 (VK_ERROR_LAYER_NOT_PRESENT).
export VK_LAYER_PATH="$PWD/$BUILD/lsfg-vk-layer:/usr/share/vulkan/explicit_layer.d"
# NOTE: validation is requested by the app itself via LSFGVK_VALIDATION=1
# (ppEnabledLayerNames in VkInstanceCreateInfo). Listing it ALSO in
# VK_INSTANCE_LAYERS double-requests it and vkCreateInstance fails -6
# (VK_ERROR_LAYER_NOT_PRESENT). Keep it in one place only.
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_PROFILE=app-oneway
export LSFGVK_VALIDATION=1          # makes the APP request validation explicitly
export LSFGVK_DBG_BUDGET_BYTES=$((8*1024*1024))
export LSFGVK_LAYER_SHELL=1

# a stale socket inode can survive an exit; the path existing proves nothing
rm -f ~/.local/state/lsfg-vk/app.sock

"$BUILD/lsfg-vk-app/lsfg-vk-app" --profile app-oneway --session wayland \
    >/tmp/t_app.log 2>&1 &
APP=$!
sleep 6
ss -xl 2>/dev/null | grep -q app.sock \
    || { echo "REFUSING: app is not listening on app.sock"; kill $APP; exit 1; }
echo "app pid=$APP listening"

# vkcube is an external app and gets BOTH layers from the loader env: the
# frame-generation layer (it is the capture side) and validation. Overriding
# this with validation ALONE silently dropped frame generation - the client ran
# with no layer at all and streamed nothing. Both names must be present.
env VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation:VK_LAYER_KHRONOS_validation \
    vkcube --width 512 --height 256 --gpu_number "$RENDER" --wsi xcb \
    >/tmp/t_cube.log 2>&1 &
CUBE=$!

# --- validation must be REALLY loaded, in BOTH processes -----------------
sleep 8
for pair in "app:$APP" "client:$CUBE"; do
    what=${pair%%:*}; pid=${pair##*:}
    if grep -qa libVkLayer_khronos_validation "/proc/$pid/maps" 2>/dev/null; then
        echo "  validation: LOADED in $what (pid $pid)"
    else
        echo "  validation: NOT LOADED in $what (pid $pid) -> REFUSING TO RUN"
        kill $CUBE $APP 2>/dev/null; wait 2>/dev/null; exit 1
    fi
done

# --- both on the same layer .so -----------------------------------------
# vkcube IS the game process here. Our layer MUST be mapped into it: that is
# where frames are captured and pushed over the socket. "Client has no layer"
# means frame generation never ran and nothing can stream - that is a FAILED
# run, not a pass. Both processes must have our layer AND validation.
for pair in "app:$APP" "client:$CUBE"; do
    what=${pair%%:*}; pid=${pair##*:}
    L=$(grep -ao '/[^ ]*liblsfg-vk-layer.so' "/proc/$pid/maps" 2>/dev/null | sort -u | head -1)
    case "$L" in
        "$PWD/$BUILD/lsfg-vk-layer/"*)
            echo "  layer in $what (pid $pid): $L  OK" ;;
        "")
            echo "  layer MISSING in $what (pid $pid): no liblsfg-vk-layer.so mapped"
            echo "    -> frame generation is NOT running there. REFUSING TO REPORT."
            kill $CUBE $APP 2>/dev/null; wait 2>/dev/null; exit 1 ;;
        *)
            echo "  layer in $what WRONG BUILD: $L -> REFUSING"
            kill $CUBE $APP 2>/dev/null; wait 2>/dev/null; exit 1 ;;
    esac
done

# --- 30 s, sampled ------------------------------------------------------
for t in 10 20 30; do
    sleep 10
    FR=$(grep -ac 'input: FRAME' /tmp/t_app.log 2>/dev/null)
    GC=$(grep -ac 'copy SUBMITTED' /tmp/t_app.log 2>/dev/null)
    M2=$(grep -ac 'copy FENCE signalled=1' /tmp/t_app.log 2>/dev/null)
    M3=$(grep -ac 'REAL present ATTEMPTED' /tmp/t_app.log 2>/dev/null)
    M4=$(grep -ac 'REAL present RESULT ok=1' /tmp/t_app.log 2>/dev/null)
    echo "t=${t}s  FRAME=$FR  copySUB=$GC  fenceOK=$M2  presentATT=$M3  presentOK=$M4"
done

echo "--- first frame where a marker stops, and the last marker printed ---"
for m in M1 M2 M3 M4; do
    last=$(grep -ao "\[$m\] f=[0-9]*" /tmp/t_app.log 2>/dev/null | tail -1)
    echo "  $m last: ${last:-<never printed>}"
done
echo "--- validation messages ---"
grep -aiE "VUID|Validation Error" /tmp/t_app.log /tmp/t_cube.log 2>/dev/null | head -5
echo "--- card resets during this run ---"
journalctl -k --since "-45 seconds" --no-pager 2>/dev/null \
  | grep -iE 'amdgpu.*(reset begin|fault|timeout)' | head -5
echo "(empty above = no card reset)"
kill $CUBE $APP 2>/dev/null; wait 2>/dev/null
echo "done. THIS IS A WIRING CHECK, NOT A BENCHMARK."
