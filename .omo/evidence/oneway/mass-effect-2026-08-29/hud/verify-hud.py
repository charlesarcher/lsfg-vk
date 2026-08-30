#!/usr/bin/env python3
"""Verify the app's top-left HUD against captured stills.

Renders the expected seven-segment text with the EXACT algorithm of
lsfg-vk-app/src/hud.cpp (same glyphs, scale rule, box size, colors) and
compares the bright-pixel mask + box color against the crop of the
frame at the HUD origin (8, 8).
"""
import re, sys
import numpy as np
from PIL import Image

SCALE = 6                      # 1440 / 240, clamped [3,10]
BOX_W, BOX_H = 34 * SCALE, 11 * SCALE   # 204 x 66
OX, OY = 8, 8
BOX = (14, 16, 22)             # rgb
TXT = (240, 245, 250)

SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G = 1,2,4,8,16,32,64
DIGIT = [
 SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,
 SEG_B|SEG_C,
 SEG_A|SEG_B|SEG_G|SEG_E|SEG_D,
 SEG_A|SEG_B|SEG_G|SEG_C|SEG_D,
 SEG_F|SEG_G|SEG_B|SEG_C,
 SEG_A|SEG_F|SEG_G|SEG_C|SEG_D,
 SEG_A|SEG_F|SEG_G|SEG_E|SEG_C|SEG_D,
 SEG_A|SEG_B|SEG_C,
 SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,
 SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,
]
def seg_pixel(seg, x, y):
    if seg == SEG_A: return y == 0 and 1 <= x <= 3
    if seg == SEG_B: return x == 3 and y in (1,2)
    if seg == SEG_C: return x == 3 and y in (4,5)
    if seg == SEG_D: return y == 6 and 1 <= x <= 3
    if seg == SEG_E: return x == 1 and y in (4,5)
    if seg == SEG_F: return x == 1 and y in (1,2)
    if seg == SEG_G: return y == 3 and 1 <= x <= 3
    return False
def slash(x, y):
    return ((y, x) in [(0,4),(1,3),(1,4),(2,3),(3,2),(4,1),(5,0)])
def glyph(c, x, y):
    if c.isdigit():
        segs = DIGIT[int(c)]
        return any(seg_pixel(s, x, y) for s in (SEG_A,SEG_B,SEG_C,SEG_D,SEG_E,SEG_F,SEG_G) if segs & s)
    if c == '/':
        return slash(x, y)
    return False

def render(text):
    """mirror Hud::rasterize -> (box_h x box_w x 3) rgb"""
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
                if not glyph(ch, gx, gy): continue
                for sx in range(se):
                    for sy in range(se):
                        img[y0 + gy*se + sy, x0 + c*5*se + gx*se + sx] = TXT
    return img

def bright_mask(img):
    return (np.abs(img.astype(int) - np.array(TXT)).sum(axis=2) <= 24)

def stats_values(app_log):
    vals = []
    for m in re.finditer(r"lsfg-vk-app: (\d+) fps game, (\d+) fps presented", open(app_log).read()):
        vals.append(f"{m.group(1)}/{m.group(2)}")
    return vals

def check(label, still, app_log):
    full = np.asarray(Image.open(still).convert("RGB"))
    crop = full[OY:OY+BOX_H, OX:OX+BOX_W].astype(int)
    mb = bright_mask(crop)
    n_bright = int(mb.sum())
    box_mean = crop[~mb].mean(axis=0) if (~mb).any() else None
    vals = stats_values(app_log)
    best = (None, -1.0)
    for v in sorted(set(vals)):
        exp = render(v)
        me = bright_mask(exp)
        iou = (mb & me).sum() / max((mb | me).sum(), 1)
        if iou > best[1]:
            best = (v, float(iou))
    v, iou = best
    ok = n_bright > 50 and box_mean is not None and abs(box_mean[0]-BOX[0]) < 10 and abs(box_mean[1]-BOX[1]) < 10 and abs(box_mean[2]-BOX[2]) < 10 and iou > 0.6
    print(f"== {label}")
    print(f"   still={still}  crop=({OX},{OY}) {BOX_W}x{BOX_H}")
    print(f"   bright px={n_bright}  box mean RGB=({box_mean[0]:.0f},{box_mean[1]:.0f},{box_mean[2]:.0f}) vs expected {BOX}")
    print(f"   candidates={len(set(vals))} stats values; best match text={v!r} IoU={iou:.3f}")
    print(f"   VERDICT: {'PASS' if ok else 'FAIL'}")
    # save the crop for visual inspection
    Image.fromarray(crop.astype(np.uint8)).save(still.replace(".png", "-hud-crop.png"))
    return ok

if __name__ == "__main__":
    ok1 = check("X11 backend ", sys.argv[1], sys.argv[2])
    ok2 = check("Wayland backend", sys.argv[3], sys.argv[4])
    sys.exit(0 if (ok1 and ok2) else 1)
