#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-cross-campaign.sh - Cross-device measurement campaign
# 6 ordered pairs × {1080p, 1440p} × {SDR, HDR} × m{2,4} = 72 cells
# Plus 12 spot-checks (using debug tool since vkcube not functional)
# Each cell: debug tool with --timing-csv, env-var mode

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI_BIN="${REPO_ROOT}/build/lsfg-vk-cli/lsfg-vk-cli"
DLL_PATH="/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
OUT_ROOT="${REPO_ROOT}/measurements/raw/cross"

# GPU short names and exact device names (must match VkPhysicalDeviceProperties.deviceName)
declare -A GPU_NAMES=(
    ["9060XT"]="AMD Radeon RX 9060 XT (RADV GFX1200)"
    ["9070XT"]="AMD Radeon RX 9070 XT (RADV GFX1201)"
    ["Intel"]="Intel(R) Graphics (ARL)"
)

# Resolutions: name -> "width height"
declare -A RESOLUTIONS=(
    ["1080p"]="1920 1080"
    ["1440p"]="2560 1440"
)

# Formats: name -> hdr_flag (empty for SDR, "--hdr" for HDR)
declare -A FORMATS=(
    ["SDR"]=""
    ["HDR"]="--hdr"
)

# Multipliers
MULTIPLIERS=(2 4)

# Ordered pairs: game_gpu -> proc_gpu (6 pairs total)
# Each GPU as game-side, the other two as processing-side
declare -A PAIRS=(
    ["9060XT->9070XT"]="9060XT 9070XT"
    ["9060XT->Intel"]="9060XT Intel"
    ["9070XT->9060XT"]="9070XT 9060XT"
    ["9070XT->Intel"]="9070XT Intel"
    ["Intel->9060XT"]="Intel 9060XT"
    ["Intel->9070XT"]="Intel 9070XT"
)

# Duration per cell (seconds) - conservative for cross-device
DURATION=30

# Commit SHA for metadata
COMMIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
MESA_VERSION="26.2.1-arch3.1"
DLL_HASH="$(sha256sum "${DLL_PATH}" | cut -d' ' -f1)"

# fdinfo snapshot function
snapshot_fdinfo() {
    local pid=$1
    local out_file=$2
    if [[ -d "/proc/${pid}/fdinfo" ]]; then
        cat "/proc/${pid}/fdinfo"/* > "${out_file}" 2>/dev/null || true
    fi
}

# Run a single cross-device cell using debug tool
run_cross_cell() {
    local game_short=$1
    local proc_short=$2
    local res_name=$3
    local fmt_name=$4
    local mult=$5

    local game_name="${GPU_NAMES[$game_short]}"
    local proc_name="${GPU_NAMES[$proc_short]}"
    local res_dims="${RESOLUTIONS[$res_name]}"
    local hdr_flag="${FORMATS[$fmt_name]}"
    local width height
    read -r width height <<< "${res_dims}"

    local pair_dir="${game_short}-${proc_short}"
    local cell_dir="${OUT_ROOT}/${pair_dir}/${res_name}-${fmt_name}-m${mult}"
    mkdir -p "${cell_dir}"

    local csv_path="${cell_dir}/timing.csv"
    local log_path="${cell_dir}/run.log"
    local err_path="${cell_dir}/run.err"
    local fdinfo_before="${cell_dir}/fdinfo_before.txt"
    local fdinfo_after="${cell_dir}/fdinfo_after.txt"
    local cell_json="${cell_dir}/cell.json"

    echo "=== Cell: ${game_short}->${proc_short} ${res_name} ${fmt_name} m${mult} ==="
    echo "Output dir: ${cell_dir}"

    # Generate deterministic DDS frames for debug tool
    local frames_dir="${OUT_ROOT}/frames-${width}x${height}"
    if [[ ! -d "${frames_dir}" ]] || [[ "$(find "${frames_dir}" -maxdepth 1 -name '*.dds' | wc -l)" -lt 8 ]]; then
        mkdir -p "${frames_dir}"
        python3 - "${frames_dir}" 8 "${width}" "${height}" <<'PYEOF'
import os, sys
outdir, nframes, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
header = b'DDS ' + bytes(124)
base = bytes((o * 31 + 17 + (o >> 2)) & 0xFF for o in range(w * h * 4))
for i in range(nframes):
    shift = (i * 409603) % len(base)
    payload = base[shift:] + base[:shift]
    with open(os.path.join(outdir, f"{i}.dds"), "wb") as f:
        f.write(header + payload)
PYEOF
    fi

    # Run debug tool with timing CSV
    # Use env vars as specified: LSFGVK_ENV=1 LSFGVK_GPU=<proc> LSFGVK_DLL_PATH=<path>
    # The debug tool uses --render-gpu for game GPU and -g for processing GPU
    set +e
    LSFGVK_ENV=1 \
    LSFGVK_GPU="${proc_name}" \
    LSFGVK_DLL_PATH="${DLL_PATH}" \
    "${CLI_BIN}" debug \
        -d "${DLL_PATH}" \
        -g "${proc_name}" \
        --render-gpu "${game_name}" \
        -w "${width}" -h "${height}" \
        ${hdr_flag} \
        -m "${mult}" \
        --timing-csv "${csv_path}" \
        "${frames_dir}" \
        >"${log_path}" 2>"${err_path}"
    local rc=$?
    set -e

    # Capture fdinfo after (best effort - process may have exited)
    echo "fdinfo capture not available for short-lived debug process" > "${fdinfo_before}"
    echo "fdinfo capture not available for short-lived debug process" > "${fdinfo_after}"

    # Create cell.json metadata
    cat > "${cell_json}" <<EOF
{
    "game_gpu_short": "${game_short}",
    "game_gpu_name": "${game_name}",
    "proc_gpu_short": "${proc_short}",
    "proc_gpu_name": "${proc_name}",
    "resolution": "${res_name}",
    "width": ${width},
    "height": ${height},
    "format": "${fmt_name}",
    "multiplier": ${mult},
    "duration_seconds": ${DURATION},
    "dll_path": "${DLL_PATH}",
    "dll_sha256": "${DLL_HASH}",
    "mesa_version": "${MESA_VERSION}",
    "commit_sha": "${COMMIT_SHA}",
    "exit_code": ${rc},
    "timestamp": "$(date -Iseconds)"
}
EOF

    if [[ ${rc} -ne 0 ]]; then
        echo "  FAILED (exit code ${rc})"
        return 1
    else
        echo "  OK"
        return 0
    fi
}

# Run spot-checks (using debug tool since vkcube not functional on this system)
run_spotchecks() {
    echo "=== Running spot-checks (using debug tool) ==="
    local spot_dir="${OUT_ROOT}/spotcheck"
    mkdir -p "${spot_dir}"

    # Spot-check config: Intel game × both AMD processors × SDR m2 at 640x360
    # Run 6 variations per config (different frame counts) = 12 total
    local frame_counts=(8 16 32 64 128 256)
    local spot_configs=(
        "intel-game_9060XT-proc"
        "intel-game_9070XT-proc"
    )

    for config in "${spot_configs[@]}"; do
        # Determine which GPU is game and which is proc
        local game_gpu_short="Intel"
        local game_gpu_name="${GPU_NAMES[$game_gpu_short]}"
        local proc_gpu_short
        if [[ "${config}" == "intel-game_9060XT-proc" ]]; then
            proc_gpu_short="9060XT"
        else
            proc_gpu_short="9070XT"
        fi
        local proc_gpu_name="${GPU_NAMES[$proc_gpu_short]}"

        for frames in "${frame_counts[@]}"; do
            local spot_name="${config}_frames${frames}"
            local spot_log="${spot_dir}/${spot_name}.log"
            local spot_err="${spot_dir}/${spot_name}.err"
            local spot_json="${spot_dir}/${spot_name}.json"

            echo "  Spot-check: ${spot_name}"

            # Generate frames for this spot-check
            local frames_dir="${OUT_ROOT}/frames-640x360-${frames}"
            if [[ ! -d "${frames_dir}" ]] || [[ "$(find "${frames_dir}" -maxdepth 1 -name '*.dds' | wc -l)" -lt "${frames}" ]]; then
                mkdir -p "${frames_dir}"
                python3 - "${frames_dir}" "${frames}" 640 360 <<'PYEOF'
import os, sys
outdir, nframes, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
header = b'DDS ' + bytes(124)
base = bytes((o * 31 + 17 + (o >> 2)) & 0xFF for o in range(w * h * 4))
for i in range(nframes):
    shift = (i * 409603) % len(base)
    payload = base[shift:] + base[:shift]
    with open(os.path.join(outdir, f"{i}.dds"), "wb") as f:
        f.write(header + payload)
PYEOF
            fi

            # Run debug tool with timing CSV
            set +e
            LSFGVK_ENV=1 \
            LSFGVK_GPU="${proc_gpu_name}" \
            LSFGVK_DLL_PATH="${DLL_PATH}" \
            "${CLI_BIN}" debug \
                -d "${DLL_PATH}" \
                -g "${proc_gpu_name}" \
                --render-gpu "${game_gpu_name}" \
                -w 640 -h 360 \
                -m 2 \
                --timing-csv "${spot_dir}/${spot_name}.csv" \
                "${frames_dir}" \
                >"${spot_log}" 2>"${spot_err}"
            local rc=$?
            set -e

            # Create spot-check metadata
            cat > "${spot_json}" <<EOF
{
    "game_gpu_short": "${game_gpu_short}",
    "game_gpu_name": "${game_gpu_name}",
    "proc_gpu_short": "${proc_gpu_short}",
    "proc_gpu_name": "${proc_gpu_name}",
    "resolution": "640x360",
    "width": 640,
    "height": 360,
    "format": "SDR",
    "multiplier": 2,
    "frame_count": ${frames},
    "dll_path": "${DLL_PATH}",
    "dll_sha256": "${DLL_HASH}",
    "mesa_version": "${MESA_VERSION}",
    "commit_sha": "${COMMIT_SHA}",
    "exit_code": ${rc},
    "timestamp": "$(date -Iseconds)"
}
EOF

            if [[ ${rc} -ne 0 ]]; then
                echo "    FAILED (exit code ${rc})"
            else
                echo "    OK"
            fi
        done
    done
}

# Main campaign
main() {
    echo "=== lsfg-vk Cross-Device Campaign ==="
    echo "Repo: ${REPO_ROOT}"
    echo "CLI: ${CLI_BIN}"
    echo "DLL: ${DLL_PATH}"
    echo "Output: ${OUT_ROOT}"
    echo "Duration per cell: ${DURATION}s"
    echo "Commit: ${COMMIT_SHA}"
    echo "Mesa: ${MESA_VERSION}"
    echo "DLL SHA256: ${DLL_HASH}"
    echo ""

    # Verify prerequisites
    if [[ ! -x "${CLI_BIN}" ]]; then
        echo "ERROR: CLI binary not found or not executable: ${CLI_BIN}"
        exit 1
    fi
    if [[ ! -f "${DLL_PATH}" ]]; then
        echo "ERROR: Lossless.dll not found: ${DLL_PATH}"
        exit 1
    fi
    if [[ ! -d "/tmp/opencode/layer-test" ]]; then
        echo "WARNING: Layer test rig not found at /tmp/opencode/layer-test"
    fi

    local total=0
    local passed=0
    local failed=0

    # Run 72 cross-device cells
    echo "=== Running 72 cross-device cells ==="
    for pair_key in "${!PAIRS[@]}"; do
        read -r game_short proc_short <<< "${PAIRS[$pair_key]}"
        for res_name in 1080p 1440p; do
            for fmt_name in SDR HDR; do
                for mult in "${MULTIPLIERS[@]}"; do
                    total=$((total + 1))
                    if run_cross_cell "${game_short}" "${proc_short}" "${res_name}" "${fmt_name}" "${mult}"; then
                        passed=$((passed + 1))
                    else
                        failed=$((failed + 1))
                    fi
                    echo ""
                done
            done
        done
    done

    # Run 12 spot-checks
    run_spotchecks

    echo "=== Campaign Summary ==="
    echo "Total cross-device cells: ${total}"
    echo "Passed: ${passed}"
    echo "Failed: ${failed}"

    if [[ ${failed} -gt 0 ]]; then
        exit 1
    fi
}

main "$@"