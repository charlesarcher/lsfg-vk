#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# qa-check-cross-campaign.sh - Verify cross-device campaign artifacts

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="${REPO_ROOT}/measurements/raw/cross"
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

# Ordered pairs
declare -A PAIRS=(
    ["9060XT->9070XT"]="9060XT 9070XT"
    ["9060XT->Intel"]="9060XT Intel"
    ["9070XT->9060XT"]="9070XT 9060XT"
    ["9070XT->Intel"]="9070XT Intel"
    ["Intel->9060XT"]="Intel 9060XT"
    ["Intel->9070XT"]="Intel 9070XT"
)

# Required artifacts per cell
REQUIRED_FILES=("timing.csv" "run.log" "run.err" "fdinfo_before.txt" "fdinfo_after.txt" "cell.json")

echo "=== QA Check: Cross-Device Campaign ===" | tee "${LOG_FILE}"
echo "Timestamp: $(date -Iseconds)" | tee -a "${LOG_FILE}"
echo "Output root: ${OUT_ROOT}" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

total_cells=0
passed_cells=0
failed_cells=0

# Track CSV row counts for uniqueness check
declare -A CSV_ROW_COUNTS

for pair_key in "${!PAIRS[@]}"; do
    read -r game_short proc_short <<< "${PAIRS[$pair_key]}"
    for res_name in 1080p 1440p; do
        for fmt_name in SDR HDR; do
            for mult in "${MULTIPLIERS[@]}"; do
                total_cells=$((total_cells + 1))
                pair_dir="${game_short}-${proc_short}"
                cell_dir="${OUT_ROOT}/${pair_dir}/${res_name}-${fmt_name}-m${mult}"
                cell_passed=true

                echo "Checking: ${game_short}->${proc_short} ${res_name} ${fmt_name} m${mult}" | tee -a "${LOG_FILE}"

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
                        CSV_ROW_COUNTS["${pair_dir}/${res_name}-${fmt_name}-m${mult}"]="${csv_rows}"
                        if [[ ${csv_rows} -lt 1 ]]; then
                            echo "  WARN: timing.csv has <1 rows (GPU timestamps not functional on this hardware)" | tee -a "${LOG_FILE}"
                        fi
                    fi

                    # Check cross-device log line in run.err
                    if [[ -f "${cell_dir}/run.err" ]]; then
                        if grep -q "processing on '.*' (game on '" "${cell_dir}/run.err"; then
                            echo "  OK: Cross-device context line found in run.err" | tee -a "${LOG_FILE}"
                        else
                            echo "  WARN: Cross-device context line NOT found in run.err" | tee -a "${LOG_FILE}"
                        fi

                        # Check for VUID errors
                        if grep -qE 'Validation Error|VUID' "${cell_dir}/run.err"; then
                            echo "  FAIL: VUID/Validation errors found in run.err" | tee -a "${LOG_FILE}"
                            cell_passed=false
                        else
                            echo "  OK: No VUID/Validation errors in run.err" | tee -a "${LOG_FILE}"
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

# Check spot-checks
echo "=== Checking Spot-Checks ===" | tee -a "${LOG_FILE}"
spot_dir="${OUT_ROOT}/spotcheck"
spot_total=0
spot_passed=0
spot_failed=0

if [[ -d "${spot_dir}" ]]; then
    for spot_json in "${spot_dir}"/*.json; do
        [[ -f "${spot_json}" ]] || continue
        spot_total=$((spot_total + 1))
        spot_name=$(basename "${spot_json}" .json)
        echo "Checking spot-check: ${spot_name}" | tee -a "${LOG_FILE}"

        spot_passed_flag=true

        # Check required files
        for ext in log err json csv; do
            if [[ ! -f "${spot_dir}/${spot_name}.${ext}" ]]; then
                echo "  FAIL: Missing ${spot_name}.${ext}" | tee -a "${LOG_FILE}"
                spot_passed_flag=false
            else
                echo "  OK: ${spot_name}.${ext} exists" | tee -a "${LOG_FILE}"
            fi
        done

        # Check exit code
        if [[ -f "${spot_json}" ]]; then
            exit_code=$(jq -r '.exit_code' "${spot_json}" 2>/dev/null || echo "parse_error")
            if [[ "${exit_code}" != "0" ]]; then
                echo "  FAIL: exit_code = ${exit_code}" | tee -a "${LOG_FILE}"
                spot_passed_flag=false
            else
                echo "  OK: exit_code = 0" | tee -a "${LOG_FILE}"
            fi

            # Check cross-device context line in err log
            if [[ -f "${spot_dir}/${spot_name}.err" ]]; then
                if grep -q "processing on '.*' (game on '" "${spot_dir}/${spot_name}.err"; then
                    echo "  OK: Cross-device context line found" | tee -a "${LOG_FILE}"
                else
                    echo "  WARN: Cross-device context line NOT found" | tee -a "${LOG_FILE}"
                fi

                # Check for VUID errors
                if grep -qE 'Validation Error|VUID' "${spot_dir}/${spot_name}.err"; then
                    echo "  FAIL: VUID/Validation errors found" | tee -a "${LOG_FILE}"
                    spot_passed_flag=false
                else
                    echo "  OK: No VUID/Validation errors" | tee -a "${LOG_FILE}"
                fi
            fi
        fi

        if [[ "${spot_passed_flag}" == "true" ]]; then
            spot_passed=$((spot_passed + 1))
            echo "  RESULT: PASS" | tee -a "${LOG_FILE}"
        else
            spot_failed=$((spot_failed + 1))
            echo "  RESULT: FAIL" | tee -a "${LOG_FILE}"
        fi
        echo "" | tee -a "${LOG_FILE}"
    done
else
    echo "WARNING: Spot-check directory does not exist: ${spot_dir}" | tee -a "${LOG_FILE}"
fi

# CSV row-count uniqueness check
echo "=== CSV Row-Count Uniqueness Check ===" | tee -a "${LOG_FILE}"
declare -A ROW_COUNT_SEEN
duplicate_found=false
for key in "${!CSV_ROW_COUNTS[@]}"; do
    count="${CSV_ROW_COUNTS[$key]}"
    if [[ -n "${ROW_COUNT_SEEN[$count]:-}" ]]; then
        echo "  FAIL: Duplicate row count ${count} for ${key} (also seen for ${ROW_COUNT_SEEN[$count]})" | tee -a "${LOG_FILE}"
        duplicate_found=true
    else
        ROW_COUNT_SEEN[$count]="${key}"
        echo "  OK: ${key} -> ${count} rows (unique)" | tee -a "${LOG_FILE}"
    fi
done
if [[ "${duplicate_found}" == "false" ]]; then
    echo "  All CSV row counts are unique" | tee -a "${LOG_FILE}"
fi
echo "" | tee -a "${LOG_FILE}"

echo "=== QA Summary ===" | tee -a "${LOG_FILE}"
echo "Cross-device cells - Total: ${total_cells}, Passed: ${passed_cells}, Failed: ${failed_cells}" | tee -a "${LOG_FILE}"
echo "Spot-checks - Total: ${spot_total}, Passed: ${spot_passed}, Failed: ${spot_failed}" | tee -a "${LOG_FILE}"
echo "" | tee -a "${LOG_FILE}"

overall_failed=$((failed_cells + spot_failed))
if [[ ${overall_failed} -gt 0 ]]; then
    echo "QA RESULT: FAIL" | tee -a "${LOG_FILE}"
    exit 1
else
    echo "QA RESULT: PASS (all ${total_cells} cross-device cells + ${spot_total} spot-checks have required artifacts)" | tee -a "${LOG_FILE}"
    exit 0
fi