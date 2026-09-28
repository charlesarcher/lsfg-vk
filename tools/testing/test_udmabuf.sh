#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# test_udmabuf.sh - Prove the /dev/udmabuf shared-buffer transport is usable.
#
# This is the GATE for the host-buffer bounce (see
# wiki/incidents/lsfg-vk-zero-copy-transport-20260927). The transport is:
#
#     render GPU --DMA--> udmabuf (system memory) --DMA--> doubler GPU
#
# i.e. shared/default backing memory, no point-to-point, no CPU copy. That is
# what kennykiller needs, because the two AMD cards have no usable cross-device
# page-table mapping (the p2p path faults with GCVM_L2_PROTECTION_FAULT).
#
# Two probes, run in order:
#   1. udmabuf_import_probe - one udmabuf imports as a VkImage on ONE GPU.
#   2. udmabuf_xgpu_probe   - the SAME udmabuf imports on TWO GPUs; render
#                             DMAs in, doubler DMAs out, pixels verified.
#
# Probe 2 is the one that matters. Probe 1 is cheap and localises failures.
#
# Requirements: /dev/udmabuf (CONFIG_UDMABUF=y), membership in the 'kvm' group.
#
# Usage:
#   ./test_udmabuf.sh

set -euo pipefail

cd "$(dirname "$0")"

fail() { echo "FAIL: $*" >&2; exit 1; }

if [[ ! -c /dev/udmabuf ]]; then
  fail "/dev/udmabuf not present. Need CONFIG_UDMABUF=y in the kernel."
fi
if [[ ! -r /usr/include/linux/udmabuf.h ]]; then
  fail "missing /usr/include/linux/udmabuf.h (install kernel headers)"
fi
if ! id -nG | tr ' ' '\n' | grep -qx kvm; then
  echo "ERROR: /dev/udmabuf is mode 660 root:kvm and you are not in 'kvm'." >&2
  echo "       Run: sudo usermod -aG kvm \$USER   (then log out/in)" >&2
  exit 1
fi

# size_limit_mb caps a single dmabuf. Our staging slot is 14.06 MiB, so this is
# only a warning, but it will bite anyone trying one giant buffer.
LIMIT=$(cat /sys/module/udmabuf/parameters/size_limit_mb 2>/dev/null || echo "?")
echo "== udmabuf size_limit_mb = ${LIMIT} (per-slot 14.06 MiB) =="

for probe in udmabuf_import_probe udmabuf_xgpu_probe; do
  echo "== building ${probe} =="
  g++ -O2 -std=c++17 -o "/tmp/${probe}" "${probe}.cpp" -lvulkan
done

echo
echo "== probe 1: single-GPU import =="
/tmp/udmabuf_import_probe

echo
echo "== probe 2: cross-GPU bounce (THE GATE) =="
/tmp/udmabuf_xgpu_probe

echo
echo "PASS: shared system-memory bounce is available on this platform."
echo "      render -> udmabuf -> doubler, no p2p, no CPU copy."
