"""Horizontal disparity between the eyes of a side-by-side capture (vr scene / vr targets).

    .venv\\Scripts\\python -I tools/disparity.py SCENE.png [--grid 8x4] [--patch 48] [--range 60]

For a grid of patches in the left half, finds the horizontal shift (pixels, right-eye x minus
left-eye x) of the best match in the right half (sum of absolute differences, also searching
+-2 rows), and prints the shifts as a grid, with the match quality. Near things should have
negative values (they appear further left in the right eye), far things values near 0.
"""
import argparse

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--grid", default="8x4")
    ap.add_argument("--patch", type=int, default=48)
    ap.add_argument("--range", type=int, default=60)
    a = ap.parse_args()
    im = np.asarray(Image.open(a.image).convert("L"), dtype=np.float32)
    h, w2 = im.shape
    w = w2 // 2
    left, right = im[:, :w], im[:, w:]
    gx, gy = (int(v) for v in a.grid.split("x"))
    p, r = a.patch, a.range
    for j in range(gy):
        row = []
        for i in range(gx):
            cx = int((i + 0.5) * w / gx)
            cy = int((j + 0.5) * h / gy)
            x0, y0 = cx - p // 2, cy - p // 2
            ref = left[y0 : y0 + p, x0 : x0 + p]
            if ref.std() < 4:  # featureless
                row.append("   .   ")
                continue
            best = (1e18, 0, 0)
            for dy in range(-2, 3):
                for dx in range(-r, r + 1):
                    xs, ys = x0 + dx, y0 + dy
                    if xs < 0 or xs + p > w or ys < 0 or ys + p > h:
                        continue
                    sad = np.abs(right[ys : ys + p, xs : xs + p] - ref).mean()
                    if sad < best[0]:
                        best = (sad, dx, dy)
            row.append("%+4d/%-2d" % (best[1], int(best[0])))
        print(" ".join(row))


if __name__ == "__main__":
    main()
