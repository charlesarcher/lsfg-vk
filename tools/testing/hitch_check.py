#!/usr/bin/env python3
# Frame-differenced hitch check.
#
# METHOD, and why it is this method:
# A hitch is a frame that is IDENTICAL to its predecessor while the clip is
# otherwise animating. The first version of this tool computed mean absolute
# difference on a 320x180 (later 640x360) grayscale downscale and reported
# "stalls" that did not exist: at that scale the animating region of a
# 2560x1440 frame quantizes to a constant, so 97%-moving frames registered as
# identical. Measured proof, for the worst reported event (t=25.104s):
#
#   full-res, 39 frames:   0 zero-diffs
#   full-res, tile match:  1-3% of 64x64 tiles identical, not 100%
#   8 consecutive PNGs at that timestamp: 8 distinct sha256
#
# So the metric is the PERCENTAGE OF PIXELS THAT CHANGED between consecutive
# frames, and a stall is a frame where that collapses to ~0 while its
# neighbours do not. Never trust a downscale; verify any stall at full
# resolution before believing it.
#
# Decode step (must write to a file; piping ~3 GB of rawvideo through a pipe
# fails, and the clip itself is fine):
#
#   ffmpeg -y -i <clip> -vf "fps=<FPS>,format=gray,scale=640:-1" \
#          -f rawvideo <out.raw>
#
# Usage: hitch_check.py <frames.raw> <fps> [w] [h] [start_s] [end_s]
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: hitch_check.py <frames.raw> <fps> [w] [h] [start_s] [end_s]",
              file=sys.stderr)
        return 2
    path = sys.argv[1]
    fps = float(eval(sys.argv[2]))
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 360
    start_s = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0
    end_s = float(sys.argv[6]) if len(sys.argv) > 6 else 1e9

    fsz = w * h
    with open(path, "rb") as fh:
        data = fh.read()
    n = len(data) // fsz
    if n < 2:
        print(f"hitch_check: only {n} frame(s) decoded", file=sys.stderr)
        return 3

    lo = int(start_s * fps)
    hi = min(n - 1, int(end_s * fps))
    if hi <= lo:
        print("hitch_check: empty analysis window", file=sys.stderr)
        return 3

    prev = data[lo * fsz:(lo + 1) * fsz]
    changed = []
    for i in range(lo + 1, hi + 1):
        cur = data[i * fsz:(i + 1) * fsz]
        ch = 0
        for a, b in zip(prev, cur):
            if a != b:
                ch += 1
        changed.append((100.0 * ch / fsz, i))
        prev = cur

    med = sorted(p for p, _ in changed)[len(changed) // 2]
    # 0.01% of pixels: far below real motion, far above downscale quantisation.
    STALL_PCT = 0.01
    stalls = sorted(((p, i) for p, i in changed if p <= STALL_PCT),
                    key=lambda r: r[1])

    # Group consecutive stalled frames into runs so a burst reads as one event.
    runs = []
    for _p, i in stalls:
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])

    print(f"frames={n} window={lo / fps:.1f}s..{hi / fps:.1f}s "
          f"analysed={len(changed)} median_changed_pct={med:.3f}")
    print(f"threshold_pct={STALL_PCT}")
    print(f"stalled_frames={len(stalls)}  stall_events={len(runs)}")
    for a, b in runs:
        nfr = b - a + 1
        print(f"  t={a / fps:7.3f}s  frames={nfr:3d}  {nfr / fps * 1000:6.1f} ms")
    total = sum(b - a + 1 for a, b in runs)
    print(f"total_stall_ms={total / fps * 1000:.1f}  "
          f"pct_of_window={100.0 * total / len(changed):.3f}%")
    return 0 if not runs else 1


if __name__ == "__main__":
    sys.exit(main())