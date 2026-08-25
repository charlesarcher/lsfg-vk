# Dual-GPU Setup Guide

This guide walks through setting up lsfg-vk to run frame generation on a
second GPU while your game renders on its own. Every command and log line
below was captured on the development rig used to build this feature:
an Intel Arrow Lake iGPU plus two AMD Radeon cards (RX 9070 XT and
RX 9060 XT), all running Mesa RADV/ANV drivers.

## How it works

When the `gpu` option names a device other than the one your game renders
on, lsfg-vk enters dual-GPU mode:

1. Your game renders and presents on its own GPU (unchanged).
2. Each rendered frame is copied over PCIe to the processing GPU.
3. The entire frame-generation pipeline runs there.
4. Generated frames are copied back and injected into the presentation.

Presentation always stays on the render GPU - the game's swapchain is
bound to its own device. See [Configuration](Configuration.md) for the
bandwidth cost model.

## Requirements

- Both GPUs must expose `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier`. If either is missing, lsfg-vk
  refuses to start with an error naming the device and extension.
- Both GPUs' render nodes (`/dev/dri/renderD*`) must be accessible to
  your user (inside the sandbox for Flatpak).
- Check `vulkaninfo --summary` if unsure - both devices must appear.

## Step 1: Identify your GPUs

lsfg-vk matches the `gpu` option against the exact Vulkan device name:

```console
$ vulkaninfo --summary | grep deviceName
    deviceName         = AMD Radeon RX 9060 XT (RADV GFX1200)
    deviceName         = AMD Radeon RX 9070 XT (RADV GFX1201)
    deviceName         = Intel(R) Graphics (ARL)
```

Use these strings verbatim (the driver suffix in parentheses matters).
Alternatively, `vendorID:deviceID` uppercase form (`0x1002:0x7550`) or
PCI bus ID (`3:0.0`) work. The lsfg-vk configuration UI lists all valid
names in a dropdown.

## Step 2: Find your config file

The layer reads its TOML configuration from the first of these that
exists (creating a default file with example profiles if none does):

1. `$LSFGVK_CONFIG` (explicit override)
2. `$XDG_CONFIG_HOME/lsfg-vk/conf.toml`
3. `$HOME/.config/lsfg-vk/conf.toml`
4. `/etc/lsfg-vk/conf.toml`

If a profile from the shipped defaults matches your game first, edit
that profile rather than appending a duplicate - the first match wins.

## Step 3: Enable dual-GPU mode

Set `gpu` inside the profile that matches your game to the device that
should run frame generation. A minimal working example from the test
rig, driving vkcube on the Intel iGPU with an RX 9070 XT generating
frames:

```toml
version = 2

[global]
dll = "/mnt/windows/Games/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
allow_fp16 = true

[[profile]]
name = "vkcube dual-GPU test"
active_in = "vkcube"
gpu = "AMD Radeon RX 9070 XT (RADV GFX1201)"
multiplier = 2
```

`active_in` matches the executable name (a single string or a list).
Omitting `gpu` entirely processes frames on the game's own GPU.
Restart the application after changing `gpu`; the processing device is
fixed while the process runs.

## Step 4: Verify it is working

Launch the game and check stderr (or your log collector). A working
dual-GPU setup logs exactly:

```console
lsfg-vk: using profile with name 'vkcube dual-GPU test' (identified via executable)
lsfg-vk: enabling device extensions: VK_KHR_external_memory_fd VK_KHR_external_semaphore_fd VK_KHR_timeline_semaphore VK_EXT_external_memory_dma_buf VK_EXT_image_drm_format_modifier
lsfg-vk: processing on 'AMD Radeon RX 9070 XT (RADV GFX1201)' [uuid 00000000040000000000000000000000], dma-buf: yes, drm-modifier-images: yes
lsfg-vk: processing on '00000000040000000000000000000000' (game on 'Intel(R) Graphics (ARL)')
```

The last line is the definitive proof: a processing UUID different from
the game device named in parentheses. Single-GPU operation instead logs
`frame generation on the game's own device '...'`.

To confirm both GPUs are doing work simultaneously, sample the engine
timers of the running process:

```console
$ grep drm-engine /proc/$(pgrep vkcube)/fdinfo/* | grep -v ":.*0 ns"
drm-engine-gfx:    913898232 ns    # AMD - frame generation
drm-engine-render: 802163856 ns    # Intel - game rendering
```

## Tested combinations

All nine ordered combinations of the development rig's GPUs pass the
automated matrix (`scripts/run-matrix.sh live`) with validation-clean
runs and exact frame counts, and were additionally verified presenting
a real swapchain through the layer:

| Game GPU | Frame-gen GPU | Status |
| --- | --- | --- |
| Intel Arrow Lake iGPU | RX 9070 XT | verified (CLI + live swapchain) |
| Intel Arrow Lake iGPU | RX 9060 XT | verified (CLI + live swapchain) |
| RX 9070 XT | Intel Arrow Lake iGPU | verified (CLI) |
| RX 9060 XT | Intel Arrow Lake iGPU | verified (CLI) |
| RX 9070 XT | RX 9060 XT | verified (CLI + live swapchain) |
| RX 9060 XT | RX 9070 XT | verified (CLI + live swapchain) |
| any | itself | verified (legacy path, unchanged) |

## When it fails

Failures are loud and name your configuration. A `gpu` entry nothing
matches produces:

```console
lsfg-vk: something went wrong during lsfg-vk swapchain creation:
- failed to create backend instance for requested gpu 'RTX 5090'
- Unable to initialize Vulkan
- no suitable physical device found (error -3)
```

Common causes and their fixes are covered in
[Troubleshooting](Troubleshooting.md#dual-gpu-setups): missing exchange
extensions, LINEAR-fallback limitations on unusual drivers, Flatpak
render-node visibility, unverified HDR transport, and the per-driver
pipeline-cache files.
