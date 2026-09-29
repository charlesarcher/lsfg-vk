#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Reset ONLY the RX 9070 XT render card at 0000:04:00.0.
#
# CARD RULE (standing, 2026-09-28):
#   * We do not debug the driver. If a card drops off the bus, reset it,
#     rerun, and mark that run CONTAMINATED.
#   * NEVER reset the RX 9060 XT at 0000:87:00.0 - it drives the display and
#     resetting it ends the session. If the 9060 drops, STOP AND REPORT.
#   * Always print the target's PCI address and lspci name before resetting,
#     and refuse unless it is exactly 04:00.0.
set -euo pipefail

RENDER_BDF="0000:04:00.0"
DOUBLER_BDF="0000:87:00.0"

target="${1:-$RENDER_BDF}"

# ---- show what we are about to touch -------------------------------------
echo "reset target : $target"
echo "  lspci name : $(lspci -s "$target" | sed 's/^[0-9a-f:.]* //')"
echo "  vulkan id  : $(./tools/testing/gpu_index.sh "$target" 2>/dev/null || echo 'n/a')"
echo "  policy     : only $RENDER_BDF (RX 9070 XT) may be reset"

if [ "$target" = "$DOUBLER_BDF" ]; then
    echo "REFUSING: $DOUBLER_BDF is the RX 9060 XT. It drives the display;"
    echo "          resetting it ends the session. STOP AND REPORT instead."
    exit 2
fi

if [ "$target" != "$RENDER_BDF" ]; then
    echo "REFUSING: target is $target, not $RENDER_BDF."
    exit 2
fi

# ---- confirm the doubler is still present before we do anything ----------
if ! lspci -s "$DOUBLER_BDF" >/dev/null 2>&1; then
    echo "WARNING: $DOUBLER_BDF (RX 9060 XT, the display card) is NOT present."
    echo "         It may have dropped. Resetting the render card will not fix"
    echo "         that, and the session may not come back."
    echo "         STOP AND REPORT unless you intend to continue."
    read -r -p "         continue anyway? [y/N] " a
    [ "$a" = "y" ] || exit 3
fi

echo "--- rocm-smi --gpureset -d $(./tools/testing/gpu_index.sh "$RENDER_BDF" 2>/dev/null || echo 0) ---"
rocm-smi --gpureset -d "$(./tools/testing/gpu_index.sh "$RENDER_BDF")"

# ---- one log line per reset, with the PCI address ------------------------
printf '%s CARD-RESET pci=%s name=%s\n' \
    "$(date '+%Y-%m-%dT%H:%M:%S%z')" \
    "$target" \
    "$(lspci -s "$target" | sed 's/^[0-9a-f:.]* //')" \
    >> /home/archerc/wiki/incidents/lsfg-vk-card-resets.log
echo "logged to wiki/incidents/lsfg-vk-card-resets.log"
