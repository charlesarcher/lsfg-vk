#!/usr/bin/env bash
# Direct RE2 launch via proton (no Steam UI, no mangohud, no perf) for
# debugging the fake-swapchain startup crash. Everything plain: log to file.
# Usage: ./debug_re2.sh [seconds]
set -eu
SECS=${1:-45}
LOG=/tmp/re2-direct.log

GAME_DIR='/mnt/windows/Games/Steam/steamapps/common/RESIDENT EVIL 2  BIOHAZARD RE2'
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/steam"
export STEAM_COMPAT_DATA_PATH='/mnt/windows/Games/Steam/steamapps/compatdata/883710'
export STEAM_COMPAT_TOOL_PATHS='/usr/share/steam/compatibilitytools.d/proton-cachyos-slr'
export WINEPREFIX="$STEAM_COMPAT_DATA_PATH/pfx"

export VK_LAYER_PATH=/home/archerc/code/lsfg-vk/build/lsfg-vk-layer
export VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation
export LSFGVK_CONFIG="$HOME/.config/lsfg-vk/conf.toml"
export LSFGVK_APP_SOCK="$HOME/.local/state/lsfg-vk/app.sock"
export LSFGVK_LAYER_DBG=1
export LSFGVK_FAKE_SWAPCHAIN=1
export MESA_VK_DEVICE_SELECT=1002:7550
export SteamAppId=883710
export WINEDEBUG=+err
export SteamGameId=883710
export SteamEnv=1
export WINEDLLOVERRIDES='dd,dxgi,nvapi=n'
true

rm -f "$LOG"
timeout --signal=INT "$SECS" mangohud --dlsym \
    /usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton waitforexitandrun \
    "$GAME_DIR/re2.exe" >"$LOG" 2>&1
RC=$?
echo "=== exit: $RC ==="
echo "--- last lines ---"
tail -12 "$LOG"
echo "--- markers ---"
echo "fake created: $(grep -c 'fake swapchain created' "$LOG" || true)"
echo "acquires: $(grep -c 'FAKE acquire' "$LOG" || true)"
echo "presents: $(grep -c 'fake present done' "$LOG" || true)"
grep -E "invalid|abort|Segmentation|stream error" "$LOG" | head -5 || true
