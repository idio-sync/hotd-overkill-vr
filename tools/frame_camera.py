"""Recovers the game camera from a probe frame log (hovr\\frame_N.txt).

    .venv\\Scripts\\python -I tools/frame_camera.py FRAME_LOG [--rt 1280x720]

For draws on the scene target that upload both cWVP (c0..c3) and cObjectMatrix (c4..c6),
VP = WVP * W^-1 (column-vector convention: the shaders do dp4 oPos.x, v0, c0). VP is then
split into a perspective projection and a rigid view: FOV, aspect, near/far, camera position
and axes. Prints one line per distinct VP so per-draw consistency is visible.
"""
import argparse
import math
import re

import numpy as np

DRAW = re.compile(r"^#(\d+) (\w+) prims=(\d+) vs=(\w+) ps=(\w+) rt=(\d+x\d+)")
CONST = re.compile(r"^\s+c(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)")


def parse_log(path):
    """The probe prints a draw line followed by the constants that were uploaded before it."""
    draws = []
    regs = {}
    cur = None
    with open(path) as f:
        for line in f:
            m = DRAW.match(line)
            if m:
                if cur:
                    draws.append(cur)
                cur = {"no": int(m.group(1)), "rt": m.group(6), "vs": m.group(4), "new": {}}
                continue
            m = CONST.match(line)
            if m and cur is not None:
                c = int(m.group(1))
                cur["new"][c] = (m.group(2), [float(m.group(i)) for i in range(3, 7)])
    if cur:
        draws.append(cur)
    # Running register file per vertex shader slot layout: apply uploads in order.
    for d in draws:
        for c, (name, vals) in d["new"].items():
            regs[c] = (name, vals)
        d["regs"] = dict(regs)
    return draws


def decompose(vp):
    f = vp[3, :3]
    fl = np.linalg.norm(f)
    fwd = f / fl
    r0 = vp[0, :3] / fl
    r1 = vp[1, :3] / fl
    cx = r0 @ fwd
    cy = r1 @ fwd
    right = r0 - cx * fwd
    up = r1 - cy * fwd
    sx = np.linalg.norm(right)
    sy = np.linalg.norm(up)
    right /= sx
    up /= sy
    # Camera position: the point where rows 0, 1, 3 are all zero (x = y = w = 0 at the eye).
    pos = np.linalg.solve(np.array([vp[0, :3], vp[1, :3], vp[3, :3]]), -np.array([vp[0, 3], vp[1, 3], vp[3, 3]]))
    # Depth: z_clip = A*z_view + B with w = z_view (after normalising by fl).
    zv = lambda p: (vp[3, :3] @ p + vp[3, 3]) / fl
    zc = lambda p: (vp[2, :3] @ p + vp[2, 3]) / fl
    p1 = pos + fwd * 100.0
    p2 = pos + fwd * 1000.0
    A = (zc(p2) - zc(p1)) / (zv(p2) - zv(p1))
    B = zc(p1) - A * zv(p1)
    near = -B / A if A else float("nan")      # z_clip = 0 at near (D3D)
    far = B / (1 - A) if A != 1 else float("inf")  # z_clip = w at far
    return {
        "hfov": math.degrees(2 * math.atan(1 / sx)),
        "vfov": math.degrees(2 * math.atan(1 / sy)),
        "aspect": sy / sx,
        "off": (cx, cy),
        "near": near,
        "far": far,
        "pos": pos,
        "fwd": fwd,
        "up": up,
        "right": right,
        "handed": float(np.cross(right, up) @ fwd),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--rt", default="1280x720")
    args = ap.parse_args()
    seen = {}
    for d in parse_log(args.log):
        if d["rt"] != args.rt:
            continue
        r = d["regs"]
        if not all(c in d["new"] for c in range(0, 4)):
            continue
        if not all(c in r and r[c][0].startswith("cObjectMatrix") for c in range(4, 7)):
            continue
        if not all(c in d["new"] for c in range(4, 7)):
            continue
        wvp = np.array([r[c][1] for c in range(0, 4)])
        w = np.vstack([np.array([r[c][1] for c in range(4, 7)]), [0, 0, 0, 1]])
        vp = wvp @ np.linalg.inv(w)
        key = tuple(np.round(vp.flatten(), 2))
        seen.setdefault(key, []).append(d["no"])
    for key, nos in seen.items():
        vp = np.array(key).reshape(4, 4)
        try:
            c = decompose(vp)
        except np.linalg.LinAlgError:
            continue
        print(f"draws {nos[:6]}{'...' if len(nos) > 6 else ''} ({len(nos)}): hfov {c['hfov']:.2f} vfov {c['vfov']:.2f} "
              f"aspect {c['aspect']:.3f} off {c['off'][0]:.3f},{c['off'][1]:.3f} near {c['near']:.2f} far {c['far']:.1f} "
              f"handed {c['handed']:+.0f}")
        print(f"    pos {np.round(c['pos'], 2)} fwd {np.round(c['fwd'], 3)} up {np.round(c['up'], 3)}")


if __name__ == "__main__":
    main()
