#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# qa-check-campaign.sh - Verify baseline campaign artifacts

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="${REPO_ROOT}/measurements/raw/baseline"
LOG_FILE="${OUT_ROOT}/_campaign-check.log"

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

# Required artifacts per cell
REQUIRED_FILES=("timing.csv" "run.log" "run.err" "fdinfo_before.txt" "fdinfo_after.txt" "cell.json")

echo "=== QA Check: Baseline Campaign ===" | tee "${LOG_FILE}"
echo "Timestamp: $(date -Iseconds)" | tee -a "${LOG_FILE}"
echo "Output root: ${OUT_ROOT}" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

total_cells=0
passed_cells=0
failed_cells=0

for gpu_short in 9060XT 9070XT Intel; do
    for res_name in 1080p 1440p; do
        for fmt_name in SDR HDR; do
            for mult in "${MULTIPLIERS[@]}"; do
                total_cells=$((total_cells + 1))
                cell_dir="${OUT_ROOT}/${gpu_short}/${res_name}-${fmt_name}-m${mult}"
                cell_passed=true

                echo "Checking: ${gpu_short} ${res_name} ${fmt_name} m${mult}" | tee -a "${LOG_FILE}"

                # Check directory exists
                if [[ ! -d "${cell_dir}" ]]; then
                    echo "  FAIL: Directory does not exist" | tee -a "${LOG_FILE}"
                    cell_passed=false
                fi

                # Check each required file
                for file in "${REQUIRED_FILES[@]}"; do
                    file_path="${cell_dir}/${file}"
                    if [[ ! -f "${file_path}" ]]; then
                        echo "  FAIL: Missing ${file}" | tee -a "${LOG_FILE}"
                        cell_passed=false
                    else
                        echo "  OK: ${file} exists" | tee -a "${LOG_FILE}"
                    fi
                done

                # Check cell.json validity and exit_code
                if [[ -f "${cell_dir}/cell.json" ]]; then
                    exit_code=$(jq -r '.exit_code' "${cell_dir}/cell.json" 2>/dev/null || echo "parse_error")
                    if [[ "${exit_code}" != "0" ]]; then
                        echo "  FAIL: cell.json exit_code = ${exit_code}" | tee -a "${LOG_FILE}"
                        cell_passed=false
                    else
                        echo "  OK: cell.json exit_code = 0" | tee -a "${LOG_FILE}"
                    fi

                    # Check CSV row count (excluding header)
                    if [[ -f "${cell_dir}/timing.csv" ]]; then
                        csv_rows=$(($(wc -l < "${cell_dir}/timing.csv") - 1))
                        echo "  INFO: timing.csv rows (excl header): ${csv_rows}" | tee -a "${LOG_FILE}"
                        if [[ ${csv_rows} -lt 3000 ]]; then
                            echo "  WARN: timing.csv has <3000 rows (GPU timestamps not functional on this hardware)" | tee -a "${LOG_FILE}"
                        fi
                    fi

                    # Check generated frames from run.err
                    if [[ -f "${cell_dir}/run.err" ]]; then
                        gen_frames=$(grep -oP "generated frames:\s+\K[0-9]+" "${cell_dir}/run.err" | tail -1 || echo "0")
                        echo "  INFO: generated frames: ${gen_frames}" | tee -a "${LOG_FILE}"
                        if [[ ${gen_frames} -lt 3000 ]]; then
                            echo "  WARN: generated frames <3000" | tee -a "${LOG_FILE}"
                        fi
                    fi
                fi

                if [[ "${cell_passed}" == "true" ]]; then
                    passed_cells=$((passed_cells + 1))
                    echo "  RESULT: PASS" | tee -a "${LOG_FILE}"
                else
                    failed_cells=$((failed_cells + 1))
                    echo "  RESULT: FAIL" | tee -a "${LOG_FILE}"
                fi
                echo "" | tee -a "${LOG_FILE}"
            done
        done
    done
done

echo "=== QA Summary ===" | tee -a "${LOG_FILE}"
echo "Total cells: ${total_cells}" | tee -a "${LOG_FILE}"
echo "Passed: ${passed_cells}" | tee -a "${LOG_FILE}"
echo "Failed: ${failed_cells}" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

if [[ ${failed_cells} -gt 0 ]]; then
    echo "QA RESULT: FAIL" | tee -a "${LOG_FILE}"
    exit 1
else
    echo "QA RESULT: PASS (all 24 cells have required artifacts)" | tee -a "${LOG_FILE}"
    exit 0
fi