#!/usr/bin/env bash
# Bisect the Steam environment: direct proton run WITH the Steam overlay
# LD_PRELOAD injected, everything else identical to debug_re2.sh.
# If fps collapses to ~36, the overlay preload is the killer.
set -eu
SECS=${1:-90}
GAME_DIR="/mnt/windows/Games/Steam/steamapps/common/RESIDENT EVIL 2"
LOG=/tmp/re2-direct.log
export STEAM_COMPAT_CLIENT_INSTALL_PATH='/usr/share/steam'
export STEAM_COMPAT_DATA_PATH='/mnt/windows/Games/Steam/steamapps/compatdata/883710'
export SteamAppId=883710
export SteamGameId=883710
export WINEDEBUG=-all
export VK_LAYER_PATH=/home/archerc/code/lsfg-vk/build/lsfg-vk-layer
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
export LSFGVK_APP_SOCK="$HOME/.local/state/lsfg-vk/app.sock"
export LSFGVK_LAYER_DBG=1
export LSFGVK_FAKE_SWAPCHAIN=1
export MESA_VK_DEVICE_SELECT=1002:7550
# THE VARIABLE: Steam overlay preload (64-bit)
export LD_PRELOAD="/home/archerc/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so"
rm -f "$LOG"
timeout --signal=INT "$SECS" \
    /usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton waitforexitandrun \
    "$GAME_DIR/re2.exe" >"$LOG" 2>&1
RC=$?
echo "=== exit: $RC ==="
echo "presents: $(grep -c 'fake present done' "$LOG")"
echo "overlay loaded ok: $(grep -c 'gameoverlayrenderer' "$LOG" || true)"
