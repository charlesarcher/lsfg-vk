#!/usr/bin/env python3
"""Panel-aware verification of the app's top-left HUD against captured stills.

Renders the expected seven-segment text with the EXACT algorithm of
lsfg-vk-app/src/hud.cpp (same glyphs, scale rule, box size, colors) and
IoU-matches it against the still. Improvements over the 2026-08-30
first-pass verify-hud.py:

- the HUD box ORIGIN is auto-detected by scanning the top-left region
  (the box is blitted at window (8,8); under Wayland KWin keeps its
  Plasma panels above the fullscreen toplevel, occluding the top-left
  of the window, so the visible box origin shifts)
- the text-color tolerance admits the slightly darkened composite color
  observed under Wayland compositing ((219,226,232) vs (240,245,250))
- candidates come from the app log's per-second
  "N fps game, M fps presented" stats (run the app with -v)

Usage: verify-hud.py <still.png> <app.log> [<still2.png> <app2.log> ...]
Exit 0 iff all stills PASS.
"""
import sys
import re
import numpy as np
from PIL import Image

SCALE = 6                      # 1440 / 240, clamped [3,10] (see hud.cpp)
BOX_W, BOX_H = 34 * SCALE, 11 * SCALE   # 204 x 66
BOX = (14, 16, 22)             # rgb
TXT = (240, 245, 250)
TXT_TOL = 90                   # sum-of-abs-diff tolerance (admits the
                               # wayland-composited (219,226,232) text)
BOX_TOL = 45
ORIGIN_SEARCH = (48, 48)       # scan the top-left 48x48 for the box origin

SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G = 1, 2, 4, 8, 16, 32, 64
DIGIT = [
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,
    SEG_F | SEG_G | SEG_B | SEG_C,
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
]


def seg_pixel(seg, x, y):
    if seg == SEG_A:
        return y == 0 and 1 <= x <= 3
    if seg == SEG_B:
        return x == 3 and y in (1, 2)
    if seg == SEG_C:
        return x == 3 and y in (4, 5)
    if seg == SEG_D:
        return y == 6 and 1 <= x <= 3
    if seg == SEG_E:
        return x == 1 and y in (4, 5)
    if seg == SEG_F:
        return x == 1 and y in (1, 2)
    if seg == SEG_G:
        return y == 3 and 1 <= x <= 3
    return False


def slash(x, y):
    return (y, x) in [(0, 4), (1, 3), (1, 4), (2, 3), (3, 2), (4, 1), (5, 0)]


def glyph(c, x, y):
    if c.isdigit():
        segs = DIGIT[int(c)]
        return any(seg_pixel(s, x, y) for s in
                   (SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G)
                   if segs & s)
    if c == '/':
        return slash(x, y)
    return False


def render(text):
    """mirror Hud::rasterize -> (BOX_H, BOX_W, 3) rgb"""
    img = np.zeros((BOX_H, BOX_W, 3), np.uint8)
    img[:] = BOX
    L = max(len(text), 1)
    se = (SCALE * 6) // L
    se = min(se, SCALE) or 1
    tw, th = 5 * L * se, 7 * se
    x0, y0 = (BOX_W - tw) // 2, (BOX_H - th) // 2
    for c, ch in enumerate(text):
        for gx in range(5):
            for gy in range(7):
                if not glyph(ch, gx, gy):
                    continue
                for sx in range(se):
                    for sy in range(se):
                        img[y0 + gy * se + sy,
                            x0 + c * 5 * se + gx * se + sx] = TXT
    return img


def stats_values(app_log):
    vals = []
    for m in re.finditer(
            r"lsfg-vk-app: (\d+) fps game, (\d+) fps presented",
            open(app_log).read()):
        vals.append(f"{m.group(1)}/{m.group(2)}")
    return vals


def text_mask(crop):
    return (np.abs(crop - np.array(TXT)).sum(axis=2) <= TXT_TOL)


def box_mask(crop):
    return (np.abs(crop - np.array(BOX)).sum(axis=2) <= BOX_TOL)


def find_origin(full):
    """scan the top-left region for the box origin (max box+text coverage)."""
    sx, sy = ORIGIN_SEARCH
    best = (8, 8, -1)
    for oy in range(0, sy, 2):
        for ox in range(0, sx, 2):
            if oy + BOX_H > full.shape[0] or ox + BOX_W > full.shape[1]:
                continue
            crop = full[oy:oy + BOX_H, ox:ox + BOX_W]
            score = int(box_mask(crop).sum() + text_mask(crop).sum())
            if score > best[2]:
                best = (ox, oy, score)
    return best[0], best[1], best[2]


def check(label, still, app_log):
    full = np.asarray(Image.open(still).convert("RGB")).astype(int)
    ox, oy, score = find_origin(full)
    crop = full[oy:oy + BOX_H, ox:ox + BOX_W]
    mb = text_mask(crop)
    nb, nbx = int(mb.sum()), int(box_mask(crop).sum())
    box_mean = crop[~mb].mean(axis=0) if (~mb).any() else None
    vals = stats_values(app_log)
    best = (None, -1.0)
    for v in sorted(set(vals)):
        me = text_mask(render(v))
        iou = (mb & me).sum() / max((mb | me).sum(), 1)
        if iou > best[1]:
            best = (v, float(iou))
    v, iou = best
    box_ok = box_mean is not None and all(
        abs(box_mean[i] - BOX[i]) < 15 for i in range(3))
    ok = (nb > 50 and nbx > 5000 and box_ok and iou > 0.6)
    print(f"== {label}")
    print(f"   still={still}")
    print(f"   origin=({ox},{oy})  box px={nbx}  text px={nb}  "
          f"box mean RGB=({box_mean[0]:.0f},{box_mean[1]:.0f},"
          f"{box_mean[2]:.0f}) vs expected {BOX}")
    print(f"   candidates={len(set(vals))} stats values; "
          f"best match text={v!r} IoU={iou:.3f}")
    print(f"   VERDICT: {'PASS' if ok else 'FAIL'}")
    Image.fromarray(crop.astype(np.uint8)).save(
        still.replace(".png", "-hud-crop.png"))
    return ok


if __name__ == "__main__":
    allok = True
    args = sys.argv[1:]
    for i in range(0, len(args), 2):
        allok &= check(f"still {i // 2 + 1} ", args[i], args[i + 1])
    sys.exit(0 if allok else 1)