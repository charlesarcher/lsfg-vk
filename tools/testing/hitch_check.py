#!/usr/bin/env python3
# Frame-differenced hitch check.
#
# Consumes the raw grayscale frames ffmpeg already produced:
#   ffmpeg -y -i <clip> -vf "fps=<FPS>,format=gray,scale=320:-1" \
#          -f rawvideo <out.raw>
# and reports every frame whose mean absolute difference from its predecessor
# collapses relative to the median. A frozen or stalled frame shows up as a
# near-zero difference, which is the signature we are hunting.
#
# Decoding to a file rather than a pipe on purpose: piping 850 MB of rawvideo
# through subprocess stdout fails, and the clip itself is fine.
#
# Usage: hitch_check.py <frames.raw> <fps> [width] [height]
import statistics, sys

def main():
    if len(sys.argv) < 3:
        print("usage: hitch_check.py <frames.raw> <fps> [width] [height]",
              file=sys.stderr)
        return 2
    path = sys.argv[1]
    fps = float(eval(sys.argv[2]))
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 180

    fsz = w * h
    with open(path, "rb") as fh:
        data = fh.read()
    n = len(data) // fsz
    if n < 2:
        print(f"hitch_check: only {n} frame(s) decoded", file=sys.stderr)
        return 3

    prev = data[0:fsz]
    diffs = []
    for i in range(1, n):
        cur = data[i * fsz:(i + 1) * fsz]
        s = 0
        for a, b in zip(prev, cur):
            s += a - b if a > b else b - a
        diffs.append(s / fsz)
        prev = cur

    med = statistics.median(diffs)
    thresh = med * 0.25
    hitches = [(i, d) for i, d in enumerate(diffs) if d < thresh]

    print(f"frames={n} fps={fps} mean_abs_diff={med:.4f} threshold={thresh:.4f}")
    print(f"hitches={len(hitches)}")
    # Bucket by 10 s so the SHAPE is visible even with thousands of hits. A
    # real stall is one or two frames in the middle of a moving clip, not a
    # dense run at the start before the scene is animating.
    buckets = {}
    for i, d in hitches:
        b = int((i / fps) // 10) * 10
        buckets[b] = buckets.get(b, 0) + 1
    if buckets:
        print("per-10s-bucket:")
        for b in sorted(buckets):
            print(f"  {b:3d}-{b + 10:3d}s : {buckets[b]}")
    print("first 20 hits:")
    for i, d in hitches[:20]:
        print(f"  t={i / fps:8.3f}s  diff={d:7.4f}")
    return 0 if not hitches else 1

if __name__ == "__main__":
    sys.exit(main())