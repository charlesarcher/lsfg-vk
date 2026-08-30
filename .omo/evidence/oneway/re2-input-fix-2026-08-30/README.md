# One-way input handling — fix + validation (2026-08-30)

Context: the one-way dual-GPU app (`lsfg-vk-app`) covers the game with its own
window presenting the doubled frames. That window must **not** eat the
game's input. Two bugs were reported while running RE2 (`--session wayland`):

1. Mouse/keyboard input not reaching the game.
2. App window vs game window "flashy/glitchy" stacking/focus fight until the
   user clicked.

This directory holds the validation evidence for both the Wayland fix (the
active path) and the X11 fallback behaviour.

---

## 1. Wayland fix (active path) — verified

The app window is now created as a **layer-shell overlay** surface (fallback:
`xdg_toplevel` for compositors without `zwlr_layer_shell_v1`). Two independent
mechanisms fix the two reported bugs:

### Pointer → empty input region
`wl_surface_set_input_region` with an **empty** region (a `wl_region` with no
`add_box`). The compositor's pointer hit-test then skips the app surface and
delivers the pointer to the window beneath (the game). This is the protocol's
click-through mechanism (the same pattern GTK uses for non-interactive
surfaces); Wayland has no X11 `WM_HINTS input=False`.

**Evidence** (`/tmp/opencode/wl-verify/app.log`, `WAYLAND_DEBUG=1`):
```
wl_compositor#4.create_region(new id wl_region#14)
wl_surface#11.set_input_region(wl_region#14)     <- no add_box => empty region
```
and the input probe recorded **0** pointer/keyboard events reaching the app
surface over the run.

### Keyboard/focus + stacking → layer-shell overlay
The overlay layer (a) always stacks above toplevels and (b) is never a focus
candidate, with `keyboard_interactivity = NONE` set explicitly (the protocol
default is `exclusive`, which would steal the keyboard). This removes the app
window from the compositor's toplevel focus/stacking cycle.

**Root cause of the "flashy/glitchy" fight:** as an `xdg_toplevel`, KWin
re-activated/stacked the two fullscreen toplevels at ~20 Hz (toplevel configure
states cycling at the compositor's refresh rate), re-focusing the app window and
re-stacking on every cycle — the visible flicker, and the thing that only a
click used to pin. As a layer-shell overlay the app window is outside that cycle.

**Evidence** (`/tmp/opencode/wl-verify/app.log`):
```
-> zwlr_layer_shell_v1#8.get_layer_surface(... wl_output#12, 3, "lsfg-vk")  (3 = LAYER_OVERLAY)
-> zwlr_layer_surface_v1#15.set_anchor(15)              (top|bottom|left|right)
-> zwlr_layer_surface_v1#15.set_exclusive_zone(-1)      (do not claim space)
-> zwlr_layer_surface_v1#15.set_keyboard_interactivity(0) (NONE)
zwlr_layer_surface_v1#15.configure(441309, 2560, 1440)
-> zwlr_layer_surface_v1#15.ack_configure(441309)
```
- **toplevel configures: 0** (was ~1,166 / 20 s pre-fix) — the ping-pong is gone.
- Present loop intact: **2,993** `[gen x 1 + real]` cycles @ ~149/300 fps, **0 VUIDs**,
  clean teardown before the stream re-create.

---

## 2. X11 fallback — `WM_HINTS input=False` is keyboard-only (validated)

The X11 backend (`backend_x11.cpp`) makes the app window a normal `InputOutput`
window with `WM_HINTS input=False` + `_NET_WM_WINDOW_TYPE_UTILITY` + post-map
`_NET_WM_STATE = FULLSCREEN | ABOVE`.

`input=False` is a **keyboard-focus hint only**: it tells the WM to never give
the app window keyboard focus (so the game keeps the keys). It does **not** make
the window pointer-transparent — a normal `InputOutput` window stacked above
still participates in pointer hit-testing and captures the pointer.

### Empirical proof (`x11-input-test.log`, `xptr.c`)

Pointer-only XTest probe (no keypress, restores the user's pointer). Game window
1280×720; overlay = fullscreen `input=False` window on top (mirrors
`backend_x11.cpp` chrome). Only variable between the two runs is the overlay.

```
### BASELINE (control: game window only, no overlay)
mode=baseline  GAME received: button=1 motion=0 | OVERLAY received: button=0

### OVERLAY (fullscreen input=False window on top)
mode=overlay   GAME received: button=0 motion=0 | OVERLAY received: button=1
```

- **Baseline**: the control passes — the game receives the pointer (button=1).
- **Overlay**: the **overlay captures the pointer** (overlay button=1) and the
  **game receives nothing** (button=0, motion=0). `input=False` did not let the
  pointer through.

### Conclusion for the X11 path

| Input | X11 `input=False` | Wayland (this fix) |
|---|---|---|
| Keyboard | ✅ app never takes focus → game keeps keys | ✅ overlay `keyboard_interactivity=NONE`, never focused |
| Pointer/mouse | ❌ app window (on top) captures the pointer | ✅ empty input region → hit-test skips the app |

So the X11 one-way backend's `input=False` gives **keyboard** pass-through but
**not mouse** pass-through. This is the same keyboard/pointer split the Wayland
fix had to address with two separate mechanisms (empty input region for the
pointer, layer-shell for the keyboard/focus).

> **Implication:** the X11 one-way path likely still drops *mouse* input to the
> game (the app window is on top and `InputOutput`). It is not addressed by
> `input=False`; a true X11 pointer click-through for a *drawing* window has no
> clean built-in equivalent (an `InputOnly` window would be transparent but
> could not present frames). Flagged here for awareness; the active path is
> Wayland.

### Why the isolated Xvfb environment could not be used

`Xvfb :99` was tried as a side-effect-free environment, but its baseline
control returns **0 events**: the XTest requests are accepted (`XTestFake*` →
ret=1) and the pointer warps correctly (`XQueryPointer` confirms the target),
yet no events are delivered to any client window (verified via `strace` on the
X socket and a `select()`-based event loop). Per the X server source
(`xorg-server-21.1.24/Xext/xtest.c`, `ProcXTestFakeInput`), core button/motion
events are routed through `PickPointer(client)` → `mieqProcessDeviceEvent`,
which requires a working virtual pointer delivering to the target window —
this Xvfb build does not do that. The live Xwayland display `:0` is the
valid environment and is what the table above was measured on.