# Mass Effect Legendary — one-way frame-doubling attempt (2026-08-30)

Topology: ME1 renders on the 9070 XT (headless), `lsfg-vk-app` doubles and
presents from the 9060 XT (DP-7 2560x1440 @ 239.97 Hz). Profile
`mass-effect-oneway` in `~/.config/lsfg-vk/conf.toml` (active_in =
MassEffect1/2/3.exe, scoped so the launcher does not grab the stream).

## What was verified working

- **Profile + layer activation under Proton.** The layer's wine detection
  (detection.cpp: `/proc/self/maps` last mapped `.exe` + ends_with matching)
  is the right mechanism for `MassEffect1.exe` inside a Proton prefix.
- **Device pinning.** `MESA_VK_DEVICE_SELECT=1002:7550` pins the wine/Vulkan
  process to the 9070 XT — confirmed in `mass-effect/game.log`:
  `device-select: ... for MESA_VK_DEVICE_SELECT selected 0` with
  `GPU 0: 1002:7550 "AMD Radeon RX 9070 XT"` first in the selectable list
  (the 9070 XT is also Mesa's default first discrete device, so even
  unpinned the game would render on the right card on this rig).
- **App side ready.** The doubler app reached
  `listening on /run/user/1000/lsfg-vk/app.sock (processing on 'AMD Radeon
  RX 9060 XT ...')` in every attempt and stayed healthy; the stream
  handshake boundary (layer → app socket) is where the game-side flow
  stopped, i.e. the game never created its swapchain because the game
  never launched.

## What blocked it (game/EA-side, not one-way-side)

1. **Direct Proton launch** (`mass-effect/`, GE-Proton 11.6, `proton run
   MassEffect1.exe` with the full layer env): the exe hands off to the
   standalone **EA Desktop** flow (`EADesktop.exe -updater_call
   -ls=LaunchHelper`, `origin2://game/launch/...`). EA Desktop's xalia UI
   (window 1920x1080 at (320,165), `ea-window-*.png`) opened and the
   process tree sat at the EA auth stage for >4 min with no further
   progress — an EA account session (or its re-login) is required, which
   needs manual GUI interaction. Also noted: the existing wine prefix
   (compatdata/1328670) was created by the CachyOS SLR Proton build, and
   GE-Proton logged `Prefix has an invalid version?!` while "upgrading"
   it — mixing Proton builds on one prefix is a side risk to avoid.
2. **Steam launch** (`mass-effect-steam/`, `steam
   steam://rungameid/1328670`): Steam launched the game via its EA bridge
   (`waitforexitandrun link2ea://launchgame/1328670?platform=steam&theme=...`
   under the CachyOS SLR Proton inside SteamLinuxRuntime_4). The bridge
   progressed through `xalia.exe` → `EADesktop.exe -ls=BackgroundService`
   and then stalled: the UI processes exited and the `link2ea` command
   waited indefinitely (10 min, no swapchain, no stream). Same class of
   blocker — the EA session inside the prefix needs interactive attention.

## What the user types to frame-double ME (the verified recipe)

From the Steam client (one-time, then just press Play):

1. Start the doubler first (same as the vkcube demo):
   `lsfg-vk-app --profile app-oneway --session x11` (or `--session wayland`).
2. Steam → Mass Effect Legendary Edition → Properties → Launch options:
   ```
   VK_LAYER_PATH=/tmp/opencode/layer-test VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation LSFGVK_CONFIG=/home/archerc/.config/lsfg-vk/conf.toml MESA_VK_DEVICE_SELECT=1002:7550 %command%
   ```
   (Steam converts the `VAR=value` tokens before `%command%` into the
   game process's environment; the profile matches `MassEffect1.exe` via
   the wine-exe detection.)
3. Play. Expected: `lsfg-vk: using profile with name
   'mass-effect-oneway' (identified via wine executable)` +
   `lsfg-vk: external presentation active (game on 'AMD Radeon RX 9070
   XT ...', app on socket)` in the game log, `lsfg-vk-app: stream from
   'AMD Radeon RX 9070 XT ...' <WxH> ...` in the app log, and the
   top-left HUD on the 9060 XT output.

Notes for that path:
- The Steam client must see the launch options in its UI; editing
  `config.vdf` on disk while the client is running is NOT picked up
  (verified 2026-08-30: on-disk `LaunchOptions` edit + `steam://run`
  produced a launch with none of the env vars applied).
- If the EA login wall appears, resolve the EA session (the EA Desktop
  window; it is visible once the app window is not covering it) and
  re-launch.
- Keep one Proton build per prefix (the prefix was built by the CachyOS
  SLR Proton; launch via Steam, which uses that same build, rather than
  a different Proton against the same prefix).