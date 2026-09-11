#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-matrix.sh - dual-GPU test matrix runner for lsfg-vk (issue #159).
#
# live mode generates a deterministic DDS frame directory (kept out of git,
# under the output dir), executes the rig's GPU pairs through
# `lsfg-vk-cli debug` under VK_LAYER_KHRONOS_validation and asserts per pair:
#   - exit code 0 (negative control: nonzero, with the named selector error)
#   - exactly FRAMES*(MULTIPLIER-1) "wait ok" lines parsed from tool stdout
#   - zero validation errors ("Validation Error"/"VUID") on stderr; radv's
#     normal "not conformant" warnings are deliberately not matched
#   - cross pairs: the todo-13 context line's processing uuid is correlated
#     with the backend init line of the SAME run naming the -g device, while
#     the game side names the --render-gpu device - proving real dual-gpu
#     operation instead of a silent same-device fallback
# evidence lands under $LSFGVK_MATRIX_OUT/<pair>/{run.log,run.err}.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# configuration (env-overridable)
LSFGVK_DLL_PATH="${LSFGVK_DLL_PATH:-/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll}"
LSFGVK_CLI_BIN="${LSFGVK_CLI_BIN:-$REPO_ROOT/build/lsfg-vk-cli/lsfg-vk-cli}"
LSFGVK_MATRIX_OUT="${LSFGVK_MATRIX_OUT:-$REPO_ROOT/.omo/evidence/task-15-issue-159-dual-gpu}"

# rig facts (issue #159): card1=Intel iGPU 0x8086:0x7D67,
# card2=AMD Navi48 0x1002:0x7550, card3=AMD Navi44 0x1002:0x7590
# GPU names must equal VkPhysicalDeviceProperties.deviceName exactly
# (CLI selectors compare with string equality; RADV appends suffixes).
GPU_INTEL="${LSFGVK_GPU_INTEL:-Intel(R) Graphics (ARL)}"
GPU_NAVI48="${LSFGVK_GPU_NAVI48:-AMD Radeon RX 9070 XT (RADV GFX1201)}"

FRAMES="${LSFGVK_MATRIX_FRAMES:-8}"
WIDTH="${LSFGVK_MATRIX_WIDTH:-640}"
HEIGHT="${LSFGVK_MATRIX_HEIGHT:-360}"
MULTIPLIER="${LSFGVK_MATRIX_MULTIPLIER:-2}"
FLOW="${LSFGVK_MATRIX_FLOW:-0.85}"
PAIR_TIMEOUT="${LSFGVK_MATRIX_TIMEOUT:-120}"
EXPECTED_WAITS=$((FRAMES * (MULTIPLIER - 1)))

MODE="${1:-dry}"
FRAMES_DIR=""

usage() {
    echo "usage: $0 [dry|live]" >&2
}

cleanup() {
    # reap any straggler a timed-out pair might have left behind
    jobs -p | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT

generate_frames() {
    local dir="$1"
    if [[ -d "$dir" ]] && [[ "$(find "$dir" -maxdepth 1 -name '*.dds' | wc -l)" -eq "$FRAMES" ]]; then
        echo "frames: reusing existing $dir"
        return 0
    fi
    mkdir -p "$dir"
    echo "frames: generating $FRAMES deterministic ${WIDTH}x${HEIGHT} RGBA8 DDS frames into $dir"
    python3 - "$dir" "$FRAMES" "$WIDTH" "$HEIGHT" <<'PYEOF'
import os, sys

outdir, nframes, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
header = b'DDS ' + bytes(124)  # magic + header; the debug tool skips all 128 bytes
base = bytes((o * 31 + 17 + (o >> 2)) & 0xFF for o in range(w * h * 4))
for i in range(nframes):
    shift = (i * 409603) % len(base)
    payload = base[shift:] + base[:shift]  # deterministic distinct frames
    with open(os.path.join(outdir, f"{i}.dds"), "wb") as f:
        f.write(header + payload)
PYEOF
}

note() { echo "   $*"; }

gate_cross_pair() {
    local err_log="$1" render_gpu="$2" proc_gpu="$3"

    # backend init line of THIS run maps processing name -> uuid:
    #   lsfg-vk: processing on '<name>' [uuid <32hex>], dma-buf: ...
    local init_uuid
    init_uuid="$(grep -oP "^lsfg-vk: processing on '\Q${proc_gpu}\E' \[uuid \K[0-9a-f]{32}" "$err_log" | head -n1 || true)"
    if [[ -z "$init_uuid" ]]; then
        note "cross-gate FAIL: no backend init line naming '$proc_gpu'"
        return 1
    fi

    # todo-13 context line proves cross mode with named game side:
    #   lsfg-vk: processing on '<32hex>' (game on '<name>')
    local ctx_uuid game_name
    ctx_uuid="$(grep -oP "^lsfg-vk: processing on '\K[0-9a-f]{32}(?=' \(game on )" "$err_log" | head -n1 || true)"
    game_name="$(grep -oP "^lsfg-vk: processing on '[0-9a-f]{32}' \(game on '\K[^']+" "$err_log" | head -n1 || true)"
    if [[ -z "$ctx_uuid" ]]; then
        note "cross-gate FAIL: no cross-device context line"
        return 1
    fi
    if [[ "$game_name" != "$render_gpu" ]]; then
        note "cross-gate FAIL: game side '$game_name' != --render-gpu '$render_gpu'"
        return 1
    fi
    if [[ "$ctx_uuid" != "$init_uuid" ]]; then
        note "cross-gate FAIL: context uuid ${ctx_uuid:0:8}.. != init uuid ${init_uuid:0:8}.. of '$proc_gpu'"
        return 1
    fi
    note "cross-gate OK: processing uuid ${ctx_uuid:0:8}.. == '$proc_gpu' != game '$game_name'"
    return 0
}

run_pair_live() {
    local name="$1" render_gpu="$2" proc_gpu="$3" expect="$4" # expect: same|cross|invalid

    local logdir="$LSFGVK_MATRIX_OUT/per-pair/$name"
    mkdir -p "$logdir"
    local run_log="$logdir/run.log" err_log="$logdir/run.err"

    echo "== pair '$name': --render-gpu '$render_gpu' -g '$proc_gpu' (expect=$expect)"

    set +e
    timeout "$PAIR_TIMEOUT" env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
        "$LSFGVK_CLI_BIN" debug \
            -d "$LSFGVK_DLL_PATH" \
            -g "$proc_gpu" \
            --render-gpu "$render_gpu" \
            -w "$WIDTH" -h "$HEIGHT" -m "$MULTIPLIER" -f "$FLOW" \
            "$FRAMES_DIR" \
        >"$run_log" 2>"$err_log"
    local rc=$?
    set -e

    local status="PASS"

    # assertion 1: exit code
    if [[ "$expect" == invalid ]]; then
        if [[ $rc -eq 0 ]]; then
            status="FAIL(exit=0-on-invalid-pair)"
        elif [[ $rc -eq 124 ]]; then
            status="FAIL(timeout)"
        fi
    elif [[ $rc -ne 0 ]]; then
        status="FAIL(exit=$rc)"
    fi

    # assertion 2: wait count parsed from real tool stdout
    local waits
    waits="$(grep -c '^lsfg-vk-debug: wait ok' "$run_log" || true)"
    if [[ "$expect" == invalid ]]; then
        if [[ "$waits" -ne 0 ]]; then
            status="FAIL(waits=$waits-on-invalid-pair)"
        fi
    elif [[ "$waits" -ne "$EXPECTED_WAITS" ]]; then
        status="FAIL(waits=$waits want=$EXPECTED_WAITS)"
    fi

    # assertion 3: validation stderr empty
    if grep -qE 'Validation Error|VUID' "$err_log"; then
        status="FAIL(validation-errors-present)"
    fi

    # assertion 4: mode gates
    case "$expect" in
        cross)
            if ! gate_cross_pair "$err_log" "$render_gpu" "$proc_gpu"; then
                status="FAIL(cross-gate)"
            fi
            ;;
        same)
            if ! grep -qF "frame generation on the game's own device '$proc_gpu'" "$err_log"; then
                status="FAIL(same-device-line-missing)"
            fi
            if grep -q "(game on '" "$err_log"; then
                status="FAIL(unexpected-cross-line)"
            fi
            ;;
        invalid)
            if ! grep -qF "failed to find specified GPU: $render_gpu" "$err_log"; then
                status="FAIL(named-error-missing)"
            fi
            if grep -q "^lsfg-vk: processing on '" "$err_log"; then
                status="FAIL(vulkan-work-before-error)"
            fi
            ;;
    esac

    echo "   exit=$rc waits=$waits/$EXPECTED_WAITS -> $status"
    SUMMARY+=("$name|$status")
    if [[ "$status" != PASS ]]; then
        FAILURES=$((FAILURES + 1))
    fi
}

case "$MODE" in
    dry)
        echo "== lsfg-vk dual-GPU matrix (dry list, nothing executed) =="
        echo "dll:      $LSFGVK_DLL_PATH"
        echo "cli bin:  $LSFGVK_CLI_BIN"
        echo "out root: $LSFGVK_MATRIX_OUT"
        echo "frames:   $FRAMES x ${WIDTH}x${HEIGHT}, multiplier $MULTIPLIER -> $EXPECTED_WAITS waits/pair"
        echo
        run_pair_dry() {
            echo "pair: $1"
            echo "  renderer/exporter (--render-gpu): $2"
            echo "  processor (-g):                   $3"
            echo "  log dir:                          $LSFGVK_MATRIX_OUT/$1"
            echo
        }
        run_pair_dry "navi48-to-navi48" "$GPU_NAVI48" "$GPU_NAVI48"
        run_pair_dry "intel-to-navi48"  "$GPU_INTEL"  "$GPU_NAVI48"
        run_pair_dry "navi48-to-intel"  "$GPU_NAVI48" "$GPU_INTEL"
        run_pair_dry "invalid-renderer" "LSFGVK-Matrix-Bogus-GPU" "$GPU_NAVI48"
        ;;
    live)
        if [[ ! -x "$LSFGVK_CLI_BIN" ]]; then
            echo "error: cli binary not found/executable: $LSFGVK_CLI_BIN" >&2
            exit 1
        fi
        if [[ ! -f "$LSFGVK_DLL_PATH" ]]; then
            echo "error: Lossless.dll not found: $LSFGVK_DLL_PATH" >&2
            exit 1
        fi
        if ! command -v python3 >/dev/null 2>&1; then
            echo "error: python3 required for deterministic frame generation" >&2
            exit 1
        fi

        mkdir -p "$LSFGVK_MATRIX_OUT"
        FRAMES_DIR="$LSFGVK_MATRIX_OUT/frames-${WIDTH}x${HEIGHT}"
        generate_frames "$FRAMES_DIR"

        # full console (summary + per-pair verdicts) becomes matrix.log evidence
        exec > >(tee "$LSFGVK_MATRIX_OUT/matrix.log") 2>&1

        echo "== lsfg-vk dual-GPU matrix (live) =="
        echo "dll:      $LSFGVK_DLL_PATH"
        echo "cli bin:  $LSFGVK_CLI_BIN"
        echo "out root: $LSFGVK_MATRIX_OUT"
        echo "frames:   $FRAMES x ${WIDTH}x${HEIGHT}, multiplier $MULTIPLIER -> $EXPECTED_WAITS waits/pair"
        echo

        SUMMARY=()
        FAILURES=0
        run_pair_live "navi48-to-navi48" "$GPU_NAVI48" "$GPU_NAVI48" same
        run_pair_live "intel-to-navi48"  "$GPU_INTEL"  "$GPU_NAVI48" cross
        run_pair_live "navi48-to-intel"  "$GPU_NAVI48" "$GPU_INTEL"  cross
        run_pair_live "invalid-renderer" "LSFGVK-Matrix-Bogus-GPU" "$GPU_NAVI48" invalid

        echo
        echo "== matrix summary =="
        local_summary() {
            echo "  pair                          result"
            local entry
            for entry in "${SUMMARY[@]}"; do
                printf '  %-28s %s\n' "${entry%%|*}" "${entry##*|}"
            done
        }
        local_summary
        echo
        if [[ "$FAILURES" -ne 0 ]]; then
            echo "matrix RESULT: FAIL ($FAILURES pair(s) failed)"
            exit 1
        fi
        echo "matrix RESULT: PASS (all pairs green)"
        ;;
    *)
        usage
        exit 1
        ;;
esac
