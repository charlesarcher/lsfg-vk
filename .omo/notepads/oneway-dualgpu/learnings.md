# Learnings — oneway-dualgpu

Conventions, patterns, and successful approaches discovered during work on this plan.

_Auto-scaffolded by /start-work. Append new entries below - never overwrite._

---

## 2026-08-26 t1 (rig topology + two-way baselines)
- Display card = card3 = RX 9060 XT (0x7590) via only enabled connector card3-HDMI-A-3 (4K@60, KDE Wayland/KWin, Xwayland on :0). card2=9070XT (0x7550), card1=Intel ARL (0x7d67). Matches plan's known-device table.
- WM matrix: BOTH backends natively testable — Wayland-native session + Xwayland for X11. No nested compositor needed (weston/cage not installed; fine).
- Native `lsfg-vk-cli debug` IGNORES LSFGVK_GPU env (layer-only); must pass `-g <deviceName>` to pin processing GPU. Default fell to GPU0 (9060XT). Documented in rig-display.md.
- Two-way cross-device works today at v10: debug CLI game=Intel/render=9070XT exit=0; vkcube gpu_number=2 (Intel) + layer proc=9070XT ran full 15s (exit=124=SIGINT timeout), success line present. VUID/validation-error counts: 0 in both logs.
- vkcube uuid convention: processing uuid encodes PCI bus (87→9060XT/card3, 04→9070XT/card2).
