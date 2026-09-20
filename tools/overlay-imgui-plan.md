
# lsfg-vk overlay: dear imgui migration (design, S40+)

Goal: replace the CPU seven-segment HUD with a real dear imgui overlay that
looks/belaves like the desktop launcher (WinUI-3 dark + purple palette
already exist in gui.cpp), toggled while doubled, nice widgets, nice stats.

## Non-negotiables (standing)
- Layershell OVERLAY mode stays: never focusable, empty input region — the
  overlay CANNOT receive mouse/keyboard. Toggle is therefore NOT mouse-driven.
- Zero cost when hidden: no ImGui NewFrame, no RT draw, no blit; present
  path byte-identical to HUD-off.
- Keep `hud.cpp` as fallback minimal renderer (env: LSFGVK_HUD=minimal).

## Chosen defaults (recommended; ask for a change only if wrong)
1. TOGGLE: SIGUSR1 handler on lsfg-vk-app (atomic flip + fade-out); binds
   into KDE global shortcuts zero-protocol; a second delivery rides the
   app.sock control op so the desktop launcher button can drive it later.
2. STATS v1: numbers card (FPS game/presented, click->photon p50/p99 live
   from the dual-shm ledger — real values, not EMAs) + frametime sparkline
   (last ~180 game frames) + GEN-blend fraction. Pipeline row I/G/S/E stays.
3. RENDER PATH: offscreen ImGui RT (B8G8R8A8, sized w/4 x h/4, dynamic)
   rendered at ~15 Hz on the OUTPUT thread, blitted into the swapchain via
   the existing drawHud blit site (nearest for text crispness; linear for
   photos). ImGui_ImplVulkan for draw data; our own one-time renderpass on
   the RT only (swapchain untouched).
4. FONT: imgui's proggy? NO — load NotoSans/Roboto (mangohud-style) at
   outputHeight/45 px for legibility at 1440p+; StB TrueType already vendored.

## Implementation skeleton (phase A, one session)
- thirdparty: build `imgui_vk` static target (imgui*.cpp +
  backends/imgui_impl_vulkan.cpp; do NOT link glfw/gl into the app).
- app-side new files: src/imgui_hud.cpp/.hpp:
  * init(device, queue, RT size, font) once per PROCESS (survives stream
    teardown: device persists across streams in main.cpp).
  * per tick: NewFrame -> widgets -> RenderDrawData on RT (one-time RP),
    submit with per-frame fence (S40 fence discipline: every cb submits
    WITH its fence, waits infinite; no lost presents).
  * blit: same makeBlitBarrier pattern as hud.cpp; slot double-buffered
    (two RTs ping-pong) to keep uploads off the read slot.
- presentation.cpp: stats tick switches hud->update(text) for
  lui->tick(snapshot) when enabled; drawHud prefers imgui RT when present.
- toggle: signal(SIGUSR1) + app.sock op + LSFGVK_OVERLAY_HUD env default.
- acceptance: (a) toggled-off frames bit-identical to HUD-off path,
  (b) sparkline + p50/p99 update visibly, (c) 237 fps game stream sustains
  with HUD on (<= 0.5 ms present delta on MEASURED REAL rows), (d) blit-
  quality: no shimmer on text at 1440p.

## Phase B (later, gated on A)
- frametime histogram + GEN-blend preview corner (ledger + timestamps),
  drag/reposition via a transient FOCUSABLE mode (layer-shell rebuilt w/o
  empty input region) — explicitly behind a toggle, not default.
