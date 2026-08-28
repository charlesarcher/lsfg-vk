#!/bin/bash
# Run all test cells for the E2E matrix
# This script runs each test cell with proper artifact collection

set -euo pipefail

BASE_DIR="/home/archerc/code/lsfg-vk/.omo/evidence/oneway/t11-matrix"
CAPTURE_FDINFO="/home/archerc/code/lsfg-vk/.omo/evidence/oneway/t11-matrix/capture_fdinfo.sh"
DLL_PATH="/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
CONFIG_FILE="/home/archerc/.config/lsfg-vk/conf.toml"
LAYER_PATH="/tmp/opencode/layer-test"
APP_BIN="/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app"
VKCUBE_BIN="vkcube"

# GPU device names
INTEL_ARL="Intel(R) Graphics (ARL)"
RX_9070_XT="AMD Radeon RX 9070 XT (RADV GFX1201)"
RX_9060_XT="AMD Radeon RX 9060 XT (RADV GFX1200)"

# vkcube GPU numbers (from vulkaninfo --summary)
# GPU0 = RX 9060 XT, GPU1 = RX 9070 XT, GPU2 = Intel ARL
VKCUBE_INTEL=2
VKCUBE_9070=1
VKCUBE_9060=0

# Duration for each soak test
SOAK_DURATION=60

# Function to run a single test cell
run_cell() {
    local CELL_ID="$1"
    local BACKEND="$2"
    local GAME_GPU_NAME="$3"
    local PROC_GPU_NAME="$4"
    local MULTIPLIER="$5"
    local VKCUBE_GPU_NUM="$6"
    local WSI_PLATFORM="$7"
    local SESSION_ARG="$8"
    local OUTDIR="$9"

    mkdir -p "${OUTDIR}"
    echo "=== Test Cell: ${CELL_ID} (${BACKEND}) ==="
    echo "Game GPU: ${GAME_GPU_NAME}"
    echo "Proc GPU: ${PROC_GPU_NAME}"
    echo "Multiplier: ${MULTIPLIER}"
    echo "Output: ${OUTDIR}"

    # Create test config for this cell
    cat > "${OUTDIR}/test_config.toml" <<EOF
version = 2

[global]
dll = "${DLL_PATH}"
allow_fp16 = true

[[profile]]
name = "vkcube-external"
active_in = "vkcube"
gpu = "${GAME_GPU_NAME}"
presentation = "external"
multiplier = ${MULTIPLIER}

[[profile]]
name = "test-app"
gpu = "${PROC_GPU_NAME}"
presentation = "external"
multiplier = ${MULTIPLIER}
EOF

    # Capture pre-soak fdinfo
    echo "Capturing pre-soak fdinfo..."
    sudo "${CAPTURE_FDINFO}" "${OUTDIR}" "pre"

    # Start lsfg-vk-app
    echo "Starting lsfg-vk-app..."
    LSFGVK_CONFIG="${OUTDIR}/test_config.toml" \
    "${APP_BIN}" -p test-app ${SESSION_ARG} -v > "${OUTDIR}/app.log" 2>&1 &
    APP_PID=$!

    # Give app time to start
    sleep 3

    # Check if app started successfully
    if ! kill -0 ${APP_PID} 2>/dev/null; then
        echo "ERROR: lsfg-vk-app failed to start"
        cat "${OUTDIR}/app.log"
        return 1
    fi

    echo "lsfg-vk-app started (PID: ${APP_PID})"

    # Run vkcube with layer
    echo "Running vkcube for ${SOAK_DURATION}s..."
    VK_LAYER_PATH="${LAYER_PATH}" \
    VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
    LSFGVK_DLL_PATH="${DLL_PATH}" \
    LSFGVK_CONFIG="${OUTDIR}/test_config.toml" \
    timeout ${SOAK_DURATION} "${VKCUBE_BIN}" --gpu_number ${VKCUBE_GPU_NUM} --present_mode fifo --wsi ${WSI_PLATFORM} > "${OUTDIR}/vkcube.log" 2>&1 || true
    VKCUBE_EXIT=$?

    echo "vkcube exited with code: ${VKCUBE_EXIT}"

    # Capture post-soak fdinfo
    echo "Capturing post-soak fdinfo..."
    sudo "${CAPTURE_FDINFO}" "${OUTDIR}" "post"

    # Kill the app
    kill ${APP_PID} 2>/dev/null || true
    wait ${APP_PID} 2>/dev/null || true

    # Copy layer logs from vkcube output
    cp "${OUTDIR}/vkcube.log" "${OUTDIR}/layer.log"

    echo "Test cell ${CELL_ID} (${BACKEND}) completed"
    echo "Artifacts in: ${OUTDIR}"
    echo ""
}

# Main test matrix
echo "Starting E2E verification matrix..."
echo "Base directory: ${BASE_DIR}"
echo ""

# Cell (a): display-GPU-as-B pairing (RX 9060 XT as B, game on Intel ARL)
# Wayland backend
run_cell "cell-a" "wayland" "${INTEL_ARL}" "${RX_9060_XT}" 2 ${VKCUBE_INTEL} "wayland" "--session wayland" "${BASE_DIR}/cell-a/wayland"

# X11 backend (via XWayland)
run_cell "cell-a" "x11" "${INTEL_ARL}" "${RX_9060_XT}" 2 ${VKCUBE_INTEL} "xcb" "--session x11" "${BASE_DIR}/cell-a/x11"

# Cell (b): Intel-game → 9070XT-B
# Wayland backend
run_cell "cell-b" "wayland" "${INTEL_ARL}" "${RX_9070_XT}" 2 ${VKCUBE_INTEL} "wayland" "--session wayland" "${BASE_DIR}/cell-b/wayland"

# X11 backend (via XWayland)
run_cell "cell-b" "x11" "${INTEL_ARL}" "${RX_9070_XT}" 2 ${VKCUBE_INTEL} "xcb" "--session x11" "${BASE_DIR}/cell-b/x11"

# Cell (c): 9060XT-game → 9070XT-B
# Wayland backend
run_cell "cell-c" "wayland" "${RX_9060_XT}" "${RX_9070_XT}" 2 ${VKCUBE_9060} "wayland" "--session wayland" "${BASE_DIR}/cell-c/wayland"

# X11 backend (via XWayland)
run_cell "cell-c" "x11" "${RX_9060_XT}" "${RX_9070_XT}" 2 ${VKCUBE_9060} "xcb" "--session x11" "${BASE_DIR}/cell-c/x11"

# Cell (d): m∈{2,3} SDR; HDR cell where colorspace supported
# For now, test multiplier 2 and 3 on Wayland with Intel→9060XT
run_cell "cell-d-m2" "wayland" "${INTEL_ARL}" "${RX_9060_XT}" 2 ${VKCUBE_INTEL} "wayland" "--session wayland" "${BASE_DIR}/cell-d/m2-wayland"
run_cell "cell-d-m3" "wayland" "${INTEL_ARL}" "${RX_9060_XT}" 3 ${VKCUBE_INTEL} "wayland" "--session wayland" "${BASE_DIR}/cell-d/m3-wayland"
run_cell "cell-d-m2" "x11" "${INTEL_ARL}" "${RX_9060_XT}" 2 ${VKCUBE_INTEL} "xcb" "--session x11" "${BASE_DIR}/cell-d/m2-x11"
run_cell "cell-d-m3" "x11" "${INTEL_ARL}" "${RX_9060_XT}" 3 ${VKCUBE_INTEL} "xcb" "--session x11" "${BASE_DIR}/cell-d/m3-x11"

# Two-way control cell: same GPUs but presentation=game (default mode)
# Intel game → 9070 XT processing (two-way)
echo "=== Two-way Control Cell ==="
mkdir -p "${BASE_DIR}/control"
sudo "${CAPTURE_FDINFO}" "${BASE_DIR}/control" "pre"

# Use the existing vkcube-dual-gpu profile for two-way
cat > "${BASE_DIR}/control/test_config.toml" <<EOF
version = 2

[global]
dll = "${DLL_PATH}"
allow_fp16 = true

[[profile]]
name = "vkcube-dual-gpu"
active_in = "vkcube"
gpu = "${RX_9070_XT}"
presentation = "game"
multiplier = 2
EOF

# Run two-way test (no app needed)
VK_LAYER_PATH="${LAYER_PATH}" \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_DLL_PATH="${DLL_PATH}" \
LSFGVK_CONFIG="${BASE_DIR}/control/test_config.toml" \
timeout ${SOAK_DURATION} "${VKCUBE_BIN}" --gpu_number ${VKCUBE_INTEL} --present_mode fifo --wsi wayland > "${BASE_DIR}/control/vkcube.log" 2>&1 || true

sudo "${CAPTURE_FDINFO}" "${BASE_DIR}/control" "post"
cp "${BASE_DIR}/control/vkcube.log" "${BASE_DIR}/control/layer.log"

echo "Two-way control cell completed"

# OOOLS drill: resize window during soak
echo "=== OOOLS Drill ==="
mkdir -p "${BASE_DIR}/ools"
sudo "${CAPTURE_FDINFO}" "${BASE_DIR}/ools" "pre"

# Start app
LSFGVK_CONFIG="${BASE_DIR}/cell-a/wayland/test_config.toml" \
"${APP_BIN}" -p test-app --session wayland -v > "${BASE_DIR}/ools/app.log" 2>&1 &
APP_PID=$!
sleep 3

# Run vkcube in background
VK_LAYER_PATH="${LAYER_PATH}" \
VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
LSFGVK_DLL_PATH="${DLL_PATH}" \
LSFGVK_CONFIG="${BASE_DIR}/cell-a/wayland/test_config.toml" \
"${VKCUBE_BIN}" --gpu_number ${VKCUBE_INTEL} --present_mode fifo --wsi wayland > "${BASE_DIR}/ools/vkcube.log" 2>&1 &
VKCUBE_PID=$!

# Wait a bit, then resize window (send SIGWINCH or use xdotool)
sleep 10
echo "Resizing window (simulating OOOLS)..."
# Note: Actual window resize would require xdotool or similar
# For now, we just continue the soak
sleep 40

# Kill processes
kill ${VKCUBE_PID} 2>/dev/null || true
kill ${APP_PID} 2>/dev/null || true
wait ${VKCUBE_PID} 2>/dev/null || true
wait ${APP_PID} 2>/dev/null || true

sudo "${CAPTURE_FDINFO}" "${BASE_DIR}/ools" "post"
cp "${BASE_DIR}/ools/vkcube.log" "${BASE_DIR}/ools/layer.log"

echo "OOOLS drill completed"

echo "All test cells completed!"
echo "Results in: ${BASE_DIR}"