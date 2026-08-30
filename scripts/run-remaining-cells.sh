#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run remaining baseline campaign cells

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI_BIN="${REPO_ROOT}/build/lsfg-vk-cli/lsfg-vk-cli"
DLL_PATH="/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
OUT_ROOT="${REPO_ROOT}/measurements/raw/baseline"

declare -A GPU_NAMES=(
    ["9060XT"]="AMD Radeon RX 9060 XT (RADV GFX1200)"
    ["9070XT"]="AMD Radeon RX 9070 XT (RADV GFX1201)"
    ["Intel"]="Intel(R) Graphics (ARL)"
)

declare -A RESOLUTIONS=(
    ["1080p"]="1920 1080"
    ["1440p"]="2560 1440"
)

declare -A FORMATS=(
    ["SDR"]=""
    ["HDR"]="--hdr"
)

MULTIPLIERS=(2 4)
DURATION=30

COMMIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
MESA_VERSION="26.2.1-arch3.1"
DLL_HASH="$(sha256sum "${DLL_PATH}" | cut -d' ' -f1)"

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

    # Skip if already completed
    if [[ -f "${cell_json}" ]]; then
        echo "SKIP: ${gpu_short} ${res_name} ${fmt_name} m${mult} (already done)"
        return 0
    fi

    echo "=== Cell: ${gpu_short} ${res_name} ${fmt_name} m${mult} ==="

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

    echo "fdinfo capture not available for short-lived benchmark process" > "${fdinfo_before}"
    echo "fdinfo capture not available for short-lived benchmark process" > "${fdinfo_after}"

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

main() {
    echo "=== Running remaining baseline cells ==="

    local total=0
    local passed=0
    local failed=0

    for gpu_short in 9060XT 9070XT Intel; do
        for res_name in 1080p 1440p; do
            for fmt_name in SDR HDR; do
                for mult in "${MULTIPLIERS[@]}"; do
                    local cell_dir="${OUT_ROOT}/${gpu_short}/${res_name}-${fmt_name}-m${mult}"
                    if [[ -f "${cell_dir}/cell.json" ]]; then
                        continue
                    fi
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

    echo "=== Remaining Cells Summary ==="
    echo "Total: ${total}"
    echo "Passed: ${passed}"
    echo "Failed: ${failed}"

    if [[ ${failed} -gt 0 ]]; then
        exit 1
    fi
}

main "$@"