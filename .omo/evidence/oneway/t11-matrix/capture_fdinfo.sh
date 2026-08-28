#!/bin/bash
# Capture fdinfo/engine stats for all three GPUs
# Usage: capture_fdinfo.sh <output_dir> <label>

set -euo pipefail

OUTDIR="$1"
LABEL="$2"

mkdir -p "${OUTDIR}"

# Intel ARL (card1, 0000:00:02.0)
sudo cat /sys/kernel/debug/dri/0000:00:02.0/i915_engine_info > "${OUTDIR}/intel_${LABEL}.txt" 2>&1 || true

# RX 9070 XT (card2, 0000:04:00.0) - amdgpu ring files
sudo cat /sys/kernel/debug/dri/0000:04:00.0/amdgpu_ring_gfx_0.0.0 > "${OUTDIR}/amd9070_gfx_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:04:00.0/amdgpu_ring_sdma0 > "${OUTDIR}/amd9070_sdma0_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:04:00.0/amdgpu_ring_sdma1 > "${OUTDIR}/amd9070_sdma1_${LABEL}.txt" 2>&1 || true

# RX 9060 XT (card3, 0000:87:00.0) - amdgpu ring files
sudo cat /sys/kernel/debug/dri/0000:87:00.0/amdgpu_ring_gfx_0.0.0 > "${OUTDIR}/amd9060_gfx_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:87:00.0/amdgpu_ring_sdma0 > "${OUTDIR}/amd9060_sdma0_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:87:00.0/amdgpu_ring_sdma1 > "${OUTDIR}/amd9060_sdma1_${LABEL}.txt" 2>&1 || true

# Also capture gem_names for fd tracking
sudo cat /sys/kernel/debug/dri/0000:00:02.0/gem_names > "${OUTDIR}/intel_gem_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:04:00.0/gem_names > "${OUTDIR}/amd9070_gem_${LABEL}.txt" 2>&1 || true
sudo cat /sys/kernel/debug/dri/0000:87:00.0/gem_names > "${OUTDIR}/amd9060_gem_${LABEL}.txt" 2>&1 || true

echo "Captured fdinfo for ${LABEL} to ${OUTDIR}"