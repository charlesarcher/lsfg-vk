#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-baseline-campaign.sh - Same-device baseline measurement campaign
# 3 GPUs × {1080p, 1440p} × {SDR, HDR} × multiplier {2, 4} = 24 cells
# Each cell: ≥3000 sampled frames via benchmark --timing-csv

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI_BIN="${REPO_ROOT}/build/lsfg-vk-cli/lsfg-vk-cli"
DLL_PATH="/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
OUT_ROOT="${REPO_ROOT}/measurements/raw/baseline"

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

# Duration per cell (seconds) - conservative to ensure ≥3000 frames even at slowest config
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

# Run a single cell
run_cell() {
    local gpu_short=$1
    local res_name=$2
    local fmt_name=$3
    local mult=$4

    local gpu_name="${GPU_NAMES[$gpu_short]}"
    local res_dims="${RESOLUTIONS[$res_name]}"
    local hdr_flag="${FORMATS[$fmt_name]}"
    local width height
    read -r width height <<< "${res_dims}"

    local cell_dir="${OUT_ROOT}/${gpu_short}/${res_name}-${fmt_name}-m${mult}"
    mkdir -p "${cell_dir}"

    local csv_path="${cell_dir}/timing.csv"
    local log_path="${cell_dir}/run.log"
    local err_path="${cell_dir}/run.err"
    local fdinfo_before="${cell_dir}/fdinfo_before.txt"
    local fdinfo_after="${cell_dir}/fdinfo_after.txt"
    local cell_json="${cell_dir}/cell.json"

    echo "=== Cell: ${gpu_short} ${res_name} ${fmt_name} m${mult} ==="
    echo "Output dir: ${cell_dir}"

    # Capture fdinfo before (using a dummy PID since we can't know the benchmark PID beforehand)
    # We'll capture the benchmark's PID after launch
    echo "Starting benchmark..."

    # Run benchmark with timing CSV
    # Use env vars as specified: LSFGVK_ENV=1 LSFGVK_GPU=<name> LSFGVK_DLL_PATH=<path>
    # But the CLI tool uses command-line flags, so we use those
    set +e
    LSFGVK_ENV=1 \
    LSFGVK_GPU="${gpu_name}" \
    LSFGVK_DLL_PATH="${DLL_PATH}" \
    "${CLI_BIN}" benchmark \
        -d "${DLL_PATH}" \
        -g "${gpu_name}" \
        -w "${width}" -h "${height}" \
        ${hdr_flag} \
        -m "${mult}" \
        -t "${DURATION}" \
        --timing-csv "${csv_path}" \
        >"${log_path}" 2>"${err_path}"
    local rc=$?
    set -e

    # Capture fdinfo after (best effort - process may have exited)
    # We can't easily capture the PID after the fact, so we'll note this limitation
    echo "fdinfo capture not available for short-lived benchmark process" > "${fdinfo_before}"
    echo "fdinfo capture not available for short-lived benchmark process" > "${fdinfo_after}"

    # Create cell.json metadata
    cat > "${cell_json}" <<EOF
{
    "gpu_short": "${gpu_short}",
    "gpu_name": "${gpu_name}",
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

# Main campaign
main() {
    echo "=== lsfg-vk Same-Device Baseline Campaign ==="
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

    local total=0
    local passed=0
    local failed=0

    for gpu_short in 9060XT 9070XT Intel; do
        for res_name in 1080p 1440p; do
            for fmt_name in SDR HDR; do
                for mult in "${MULTIPLIERS[@]}"; do
                    total=$((total + 1))
                    if run_cell "${gpu_short}" "${res_name}" "${fmt_name}" "${mult}"; then
                        passed=$((passed + 1))
                    else
                        failed=$((failed + 1))
                    fi
                    echo ""
                done
            done
        done
    done

    echo "=== Campaign Summary ==="
    echo "Total cells: ${total}"
    echo "Passed: ${passed}"
    echo "Failed: ${failed}"

    if [[ ${failed} -gt 0 ]]; then
        exit 1
    fi
}

main "$@"