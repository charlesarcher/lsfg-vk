#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# udmabuf WIRING CHECK (not a benchmark). vkcube 512x256, app-oneway profile,
# udmabuf transport, 30 s.
#
# Refuses to report unless:
#   * our layer .so is mapped in vkcube, and NOT in the app
#   * validation is mapped in both, unless LSFGVK_WIRING_VALIDATION=0
#   * the app is really listening on app.sock
# VK_INSTANCE_LAYERS is never exported. The app inherits this script's
# environment, and an exported layer list is how our layer leaked into it.
# Numbers here are wiring evidence only. Performance comes from FurMark
# fullscreen 1440p, and only after this passes.
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=build-framedbg
RENDER=$(env -u VK_INSTANCE_LAYERS -u LSFGVK_PROFILE \
        ./tools/testing/gpu_index.sh 0000:04:00.0) || exit 1
DOUBLER=$(env -u VK_INSTANCE_LAYERS -u LSFGVK_PROFILE \
        ./tools/testing/gpu_index.sh 0000:87:00.0) || exit 1
# The client registers no debug messenger, so the validation layer's own
# debug log is the only place its messages can land. Cleared here so we read
# this run's messages, not an earlier one's.
rm -f /tmp/vkval_client.log /tmp/t_cube.tty

echo "render 04:00.0 -> vulkan $RENDER ; doubler 87:00.0 -> vulkan $DOUBLER"

# VK_LAYER_PATH must ALSO see the system layer directory: restricting it to
# just our build dir made VK_LAYER_KHRONOS_validation unresolvable and
# vkCreateInstance failed -6 (VK_ERROR_LAYER_NOT_PRESENT).
export VK_LAYER_PATH="$PWD/$BUILD/lsfg-vk-layer:/usr/share/vulkan/explicit_layer.d"
# Never export VK_INSTANCE_LAYERS. The app inherits it and loads our layer.
unset VK_INSTANCE_LAYERS
export LSFGVK_PROFILE=app-oneway
# Default validation on. LSFGVK_WIRING_VALIDATION=0 is the parked-abort run:
# validation off, our layer on vkcube only.
VAL="${LSFGVK_WIRING_VALIDATION:-1}"
if [ "$VAL" = "1" ]; then
    export LSFGVK_VALIDATION=1
else
    unset LSFGVK_VALIDATION
    echo "validation OFF (LSFGVK_WIRING_VALIDATION=0)"
fi
export LSFGVK_DBG_BUDGET_BYTES=$((8*1024*1024))
export LSFGVK_LAYER_SHELL=1

# a stale socket inode can survive an exit; the path existing proves nothing
rm -f ~/.local/state/lsfg-vk/app.sock

env -u VK_INSTANCE_LAYERS \
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
if [ "$VAL" = "1" ]; then
    CUBE_LAYERS=VK_LAYER_LSFGVK_frame_generation:VK_LAYER_KHRONOS_validation
else
    CUBE_LAYERS=VK_LAYER_LSFGVK_frame_generation
fi
env -u LSFGVK_VALIDATION VK_INSTANCE_LAYERS="$CUBE_LAYERS" \
    vkcube --width 512 --height 256 --gpu_number "$RENDER" --wsi xcb \
    >/tmp/t_cube.log 2>&1 &
CUBE=$!

# --- validation, only when this run asked for it -------------------------
sleep 8
if [ "$VAL" = "1" ]; then
    for pair in "app:$APP" "client:$CUBE"; do
        what=${pair%%:*}; pid=${pair##*:}
        if grep -qa libVkLayer_khronos_validation "/proc/$pid/maps" 2>/dev/null; then
            echo "  validation: LOADED in $what (pid $pid)"
        else
            echo "  validation: NOT LOADED in $what (pid $pid) -> REFUSING TO RUN"
            kill $CUBE $APP 2>/dev/null; wait 2>/dev/null; exit 1
        fi
    done
else
    echo "  validation: not requested this run"
fi

# --- both on the same layer .so -----------------------------------------
# THE GAME is vkcube, so OUR LAYER must be mapped there - that is where frames
# are captured and pushed over the socket. Without it, nothing streams.
# The APP is not a game and has NO reason to load our layer; if it does, that
# is the VK_LAYER_PATH leak (the app exporting a layer path the client then
# inherits), which once made both processes silently run different builds.
# So: layer in the client, NOT in the app, validation in BOTH.
C=$(grep -ao '/[^ ]*liblsfg-vk-layer.so' "/proc/$CUBE/maps" 2>/dev/null | sort -u | head -1)
A=$(grep -ao '/[^ ]*liblsfg-vk-layer.so' "/proc/$APP/maps" 2>/dev/null | sort -u | head -1)
case "$C" in
    "$PWD/$BUILD/lsfg-vk-layer/"*)
        echo "  layer in vkcube (pid $CUBE): $C  OK" ;;
    "")
        echo "  layer MISSING in vkcube: no frame generation running, nothing can"
        echo "    stream. REFUSING TO REPORT."
        kill $CUBE $APP 2>/dev/null; exit 1 ;;
    *)
        echo "  layer in vkcube WRONG BUILD: $C -> REFUSING"; kill $CUBE $APP 2>/dev/null; exit 1 ;;
esac
if [ -n "$A" ]; then
    echo "  LAYER LEAK: the app also has our layer mapped ($A)."
    echo "    The app must not load it. REFUSING."
    kill $CUBE $APP 2>/dev/null; exit 1
else
    echo "  app has no layer mapped (correct)"
fi

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
echo "--- udmabuf byte check (past row 0, against the source copy) ---"
grep -a 'udmabuf-bytes' /tmp/t_cube.log /tmp/t_app.log 2>/dev/null | head -5
echo "--- SEGV (si_addr mapping + frame pointers) ---"
grep -a '\[SEGV\]' /tmp/t_app.log /tmp/t_cube.log 2>/dev/null | head -40
echo "--- validation messages ---"
grep -aiE "VUID|Validation Error" /tmp/t_app.log /tmp/t_cube.log 2>/dev/null | head -5
echo "--- card resets during this run ---"
journalctl -k --since "-45 seconds" --no-pager 2>/dev/null \
  | grep -iE 'amdgpu.*(reset begin|fault|timeout)' | head -5
echo "(empty above = no card reset)"
kill $CUBE $APP 2>/dev/null; wait 2>/dev/null
echo "done. THIS IS A WIRING CHECK, NOT A BENCHMARK."
