"""Checks a fake-runtime capture (tools\\fakevr_shot.ps1 NAME) against the ground truth:
the mod's hit marker (marker=1: a red dot where the shot will land) is found in both submitted eye
images, triangulated with the eyes' real poses and projections, and compared with the scripted
controller's pointing ray: the offset across the ray (metres, in the controller's frame: + right,
+ up) and the angle seen from the controller's tip. A marker on the ray at any distance means the
eyes, the head/hand transforms and the aim all agree; an offset that grows with distance is an
angular aim error; a constant offset of a few cm is the barrel sitting beside the pointing axis.

    .venv\\Scripts\\python.exe -I tools\\fakevr_check.py C:\\Users\\Virtual\\hotd-vr-game\\hovr\\fakevr\\NAME [--hand right]
"""
import argparse
import math
import re
import sys

import numpy as np
from PIL import Image


def parse(txt):
    d = {"eyes": {}, "ctl": {}}
    for line in open(txt, encoding="ascii", errors="replace"):
        m = re.match(r"head (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+) roll (\S+)", line)
        if m:
            d["head"] = [float(v) for v in m.groups()]
        m = re.match(r"(left|right) controller aim \(tip\) (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+) roll (\S+)", line)
        if m:
            d["ctl"][m.group(1)] = [float(v) for v in m.groups()[1:]]
        m = re.match(r"eye (\d): pose (\S+) (\S+) (\S+), tangents l (\S+) r (\S+) b (\S+) t (\S+); view (\d+)x(\d+)", line)
        if m:
            g = m.groups()
            d["eyes"][int(g[0])] = {"pos": np.array([float(v) for v in g[1:4]]), "tan": [float(v) for v in g[4:8]]}
    return d


def rot(yaw, pitch, roll):
    """R = Ry(-yaw) Rx(pitch) Rz(-roll) (fakevr's convention), degrees."""
    def ry(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])

    def rx(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])

    def rz(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])

    k = math.pi / 180
    return ry(-yaw * k) @ rx(pitch * k) @ rz(-roll * k)


def find_marker(png):
    a = np.asarray(Image.open(png).convert("RGB")).astype(int)
    mask = (a[:, :, 0] >= 250) & (np.abs(a[:, :, 1] - 40) <= 6) & (np.abs(a[:, :, 2] - 40) <= 6)
    ys, xs = np.nonzero(mask)
    if len(xs) < 4:
        return None, a.shape[1], a.shape[0]
    return (xs.mean() + 0.5, ys.mean() + 0.5, len(xs)), a.shape[1], a.shape[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", help="path without suffix, e.g. ...\\hovr\\fakevr\\g_neutral")
    ap.add_argument("--hand", default="right")
    args = ap.parse_args()
    d = parse(args.capture + ".txt")
    R_head = rot(*d["head"][3:6])
    rays = []
    for eye in (0, 1):
        m, w, h = find_marker(f"{args.capture}_submit{eye}.png")
        if not m:
            print(f"eye {eye}: no marker found")
            return 1
        l, r, b, t = d["eyes"][eye]["tan"]
        tx = l + m[0] / w * (r - l)
        ty = t - m[1] / h * (t - b)
        dirv = R_head @ np.array([tx, ty, -1.0])
        rays.append((d["eyes"][eye]["pos"], dirv / np.linalg.norm(dirv)))
        print(f"eye {eye}: marker at pixel {m[0]:.1f} {m[1]:.1f} of {w}x{h} ({m[2]} px)")
    # Closest point between the two eye rays.
    (p1, d1), (p2, d2) = rays
    w0 = p1 - p2
    a, bb, c, dd, e = d1 @ d1, d1 @ d2, d2 @ d2, d1 @ w0, d2 @ w0
    den = a * c - bb * bb
    s, u = (bb * e - c * dd) / den, (a * e - bb * dd) / den
    q1, q2 = p1 + s * d1, p2 + u * d2
    P = (q1 + q2) / 2
    print(f"marker triangulated at {P[0]:.3f} {P[1]:.3f} {P[2]:.3f} m (eye rays miss each other by {np.linalg.norm(q1 - q2) * 1000:.1f} mm), {s:.2f} m from the left eye")
    cx, cy, cz, yaw, pitch, roll = d["ctl"][args.hand]
    R = rot(yaw, pitch, roll)
    o = np.array([cx, cy, cz])
    f = R @ np.array([0, 0, -1.0])
    v = P - o
    along = v @ f
    perp = v - along * f
    right, up = R @ np.array([1.0, 0, 0]), R @ np.array([0, 1.0, 0])
    ang = math.degrees(math.acos(max(-1, min(1, (v @ f) / np.linalg.norm(v)))))
    print(f"{args.hand} controller ray: the marker is {along:.3f} m along it, {np.linalg.norm(perp) * 100:.1f} cm off it "
          f"({perp @ right * 100:+.1f} cm right, {perp @ up * 100:+.1f} cm up), {ang:.2f} deg off as seen from the tip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
