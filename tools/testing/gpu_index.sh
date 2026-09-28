#!/usr/bin/env bash
# Resolve a PCI address to a Vulkan enumeration index. Prints the index only.
#
#   RENDER=$(./tools/testing/gpu_index.sh 0000:04:00.0)   # 9070 XT
#   DOUBLER=$(./tools/testing/gpu_index.sh 0000:87:00.0)  # 9060 XT
#   furmark --gpu-index "$RENDER" ...
#
# Why not a literal index: --gpu-index is a POSITION in the loader's
# enumeration, not a device. That order is not guaranteed across reboots or
# driver reloads, and a launch aimed at the wrong card fails in confusing ways
# - a segfault on the doubler, or a benchmark attributed to the wrong GPU. The
# PCI address is the only stable identity, so resolve through it every run.
#
# Fails loudly rather than defaulting to 0: silently picking the wrong GPU is
# worse than not running at all.
set -uo pipefail

BDF="${1:?usage: $0 <pci-bdf e.g. 0000:04:00.0>}"

if [ ! -e "/sys/bus/pci/devices/$BDF" ]; then
    echo "$0: no PCI device at $BDF" >&2
    echo "  have: $(ls /sys/bus/pci/devices/ | grep -E '^0000:' | tr '\n' ' ')" >&2
    exit 1
fi

# normalise to lowercase 4:2:2.1 hex
_d="${BDF%%:*}"; _r="${BDF#*:}"; _b="${_r%%:*}"; _r2="${_r#*:}"
_v="${_r2%%.*}"; _f="${_r2#*.}"
WANT="$(printf '%04x:%02x:%02x.%x' "$((16#$_d))" "$((16#$_b))" "$((16#$_v))" "$((16#$_f))")"

# vulkaninfo - NOT --summary, which prints neither GPU id nor bus info.
#
# The two facts needed live in DIFFERENT sections of the dump: the device name
# under VkPhysicalDeviceProperties, the bus address under
# VkPhysicalDevicePCIBusInfoPropertiesEXT. A single streaming pass cannot pair
# them, so pair by POSITION - the Nth stanza of each section is the same device
# in the same enumeration order.
INFO="$(vulkaninfo 2>/dev/null)"
if [ -z "$INFO" ]; then
    echo "$0: could not read the Vulkan device list (is vulkaninfo working?)" >&2
    exit 1
fi

# fields are "name = value", so $1 is the key and $3 the value. Do NOT gsub the
# value: a zero-valued field came out empty that way.
mapfile -t NAMES < <(awk '$1 == "deviceName" { print $3 }' <<< "$INFO")
mapfile -t BUSES < <(awk '
    $1 == "pciDomain"   { D = $3 }
    $1 == "pciBus"      { B = $3 }
    $1 == "pciDevice"   { V = $3 }
    $1 == "pciFunction" { F = $3
                         printf("%04x:%02x:%02x.%x\n", D, B, V, F) }
' <<< "$INFO")

MAP=""
for (( i = 0; i < ${#NAMES[@]} && i < ${#BUSES[@]}; ++i )); do
    MAP+="$i ${BUSES[$i]} ${NAMES[$i]}"$'\n'
done

if [ -z "$MAP" ]; then
    echo "$0: could not read the Vulkan device list (is vulkaninfo working?)" >&2
    exit 1
fi

ID="$(awk -v w="$WANT" '$2 == w { print $1; exit }' <<< "$MAP")"

if [ -z "$ID" ]; then
    echo "$0: $BDF is not in the Vulkan enumeration." >&2
    echo "  available:" >&2
    awk '{ printf "    index %-3s %s  %s\n", $1, $2, $3 }' <<< "$MAP" >&2
    exit 1
fi

echo "$ID"
