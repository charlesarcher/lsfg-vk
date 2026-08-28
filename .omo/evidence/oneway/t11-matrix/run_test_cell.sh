#!/bin/bash
# Run a single test cell for the E2E matrix
# Usage: run_test_cell.sh <cell_id> <backend> <game_gpu> <proc_gpu> <multiplier> <duration> <output_dir>

set -euo pipefail

CELL_ID="$1"
BACKEND="$2"
GAME_GPU="$3"
PROC_GPU="$4"
MULTIPLIER="$5"
DURATION="$6"
OUTDIR="$7"

mkdir -p "${OUTDIR}"

echo "=== Test Cell: ${CELL_ID} ==="
echo "Backend: ${BACKEND}"
echo "Game GPU: ${GAME_GPU}"
echo "Proc GPU: ${PROC_GPU}"
echo "Multiplier: ${MULTIPLIER}"
echo "Duration: ${DURATION}s"
echo "Output: ${OUTDIR}"

# Capture pre-soak fdinfo
sudo /home/archerc/code/lsfg-vk/.omo/evidence/oneway/t11-matrix/capture_fdinfo.sh "${OUTDIR}" "pre"

# Set up environment
export VK_LAYER_PATH=/tmp/opencode/layer-test
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_ENV=1
export LSFGVK_DLL_PATH="/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
export XDG_CONFIG_HOME=/home/archerc/.config

# Backend-specific settings
if [[ "${BACKEND}" == "x11" ]]; then
    export DISPLAY=:0
    SESSION_ARG="--session x11"
    WSI_PLATFORM="x11"
else
    unset DISPLAY
    SESSION_ARG="--session wayland"
    WSI_PLATFORM="wayland"
fi

# Start lsfg-vk-app in background
echo "Starting lsfg-vk-app with profile 'app-external' (proc GPU: ${PROC_GPU})..."
# The app-external profile uses RX 9060 XT as the presentation GPU
# We need to override the profile's GPU to use the specified proc GPU
# For now, we use the app-external profile which targets RX 9060 XT
# But for cells (b) and (c), the proc GPU is 9070 XT, so we need a different approach

# Actually, the app profile selects the presentation GPU. For one-way:
# - Cell (a): game on Intel, proc on 9060 XT (display GPU) -> app-external profile works
# - Cell (b): game on Intel, proc on 9070 XT -> need app profile with 9070 XT
# - Cell (c): game on 9060 XT, proc on 9070 XT -> need app profile with 9070 XT

# Let's create a temporary config for the app
cat > /tmp/lsfg_test_conf.toml <<EOF
version = 2

[global]
dll = "/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
allow_fp16 = true

[[profile]]
name = "test-app"
gpu = "${PROC_GPU}"
presentation = "external"
multiplier = ${MULTIPLIER}
EOF

export XDG_CONFIG_HOME=/tmp/lsfg_test_config
mkdir -p "${XDG_CONFIG_HOME}"
cp /tmp/lsfg_test_conf.toml "${XDG_CONFIG_HOME}/lsfg-vk/conf.toml"

# Start the app
/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app -p test-app ${SESSION_ARG} -v > "${OUTDIR}/app.log" 2>&1 &
APP_PID=$!

# Give app time to start and create socket
sleep 3

# Check if app started successfully
if ! kill -0 ${APP_PID} 2>/dev/null; then
    echo "ERROR: lsfg-vk-app failed to start"
    cat "${OUTDIR}/app.log"
    exit 1
fi

echo "lsfg-vk-app started (PID: ${APP_PID})"

# Run vkcube with layer
echo "Running vkcube with layer (game GPU: ${GAME_GPU})..."

# Determine vkcube GPU number
# GPU0 = 9060 XT, GPU1 = 9070 XT, GPU2 = Intel ARL (from vulkaninfo --summary)
if [[ "${GAME_GPU}" == "Intel(R) Graphics (ARL)" ]]; then
    VKCUBE_GPU=2
elif [[ "${GAME_GPU}" == "AMD Radeon RX 9070 XT (RADV GFX1201)" ]]; then
    VKCUBE_GPU=1
elif [[ "${GAME_GPU}" == "AMD Radeon RX 9060 XT (RADV GFX1200)" ]]; then
    VKCUBE_GPU=0
else
    echo "Unknown game GPU: ${GAME_GPU}"
    exit 1
fi

# Run vkcube with timeout
timeout ${DURATION} vkcube --gpu_number ${VKCUBE_GPU} --present_mode fifo --wsi ${WSI_PLATFORM} > "${OUTDIR}/vkcube.log" 2>&1 || true
VKCUBE_EXIT=$?

echo "vkcube exited with code: ${VKCUBE_EXIT}"

# Capture mid-soak screenshot (at ~30s mark)
# We'll capture after the run for now
# TODO: capture during soak

# Capture post-soak fdinfo
sudo /home/archerc/code/lsfg-vk/.omo/evidence/oneway/t11-matrix/capture_fdinfo.sh "${OUTDIR}" "post"

# Kill the app
kill ${APP_PID} 2>/dev/null || true
wait ${APP_PID} 2>/dev/null || true

echo "Test cell ${CELL_ID} completed"
echo "Artifacts in: ${OUTDIR}"