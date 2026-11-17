"""Measures the first-person gun meshes in the game's own hud packs (development only; reads the
user's installed .pc files, writes nothing into the repo).

    .venv\\Scripts\\python -I tools/gun_mesh.py PACK.pc [--png OUTDIR]

For each actor (ACTR) in the pack: node and mesh layout, vertex/index counts, the bounding box,
the principal axes, and a guess of the barrel axis (the long axis; the muzzle end is the end
with the fewest vertices near it, the grip/hammer end being the busier one). --png renders
orthographic shaded views (front/side/top) for eyeballing. Pack layout notes: docs/findings-probe.md
"The weapon model".
"""
import argparse
import re
import struct
import zlib

import numpy as np

TAGS = {0: "VSHR", 4: "PSHR", 8: "TEXR", 0xC: "ACTR", 0x10: "MTRL", 0x14: "VTXD"}


def inflate(path):
    d = open(path, "rb").read()
    m = re.search(rb"\x78[\x01\x5e\x9c\xda]", d)
    return zlib.decompressobj().decompress(d[m.start():])


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e].decode("latin1")


def table(img):
    """Resource entries of 5 dwords from offset 0: (x, hash, size, name offset, type); the string
    table follows them, 16-byte aligned (name offsets are relative to it)."""
    ents = []
    o = 0
    while True:
        x, h, size, noff, typ = struct.unpack_from("<5I", img, o)
        if typ not in TAGS or noff > 0x10000 or h == 0:
            break
        ents.append((h, size, noff, typ))
        o += 20
    base = (o + 15) & ~15
    names = [cstr(img, base + e[2]) for e in ents]
    return [dict(hash=e[0], size=e[1], type=TAGS[e[3]], name=n) for e, n in zip(ents, names)], base


def find_actr(img, h, size):
    pat = struct.pack("<II", h, size)
    for m in re.finditer(re.escape(pat), img):
        a = m.start() - 8
        if a % 4 == 0 and u32(img, a + 4) == 0x00010000:
            return a
    return None


def mesh_of(img, a):
    node = a + u32(img, a + 0x28)
    mesh = a + u32(img, node + 0xB0)
    nv = u32(img, mesh)
    vtx = a + u32(img, mesh + 4)
    nidx = u32(img, mesh + 0x44)
    idx = a + u32(img, mesh + 0x48)
    groups = a + u32(img, mesh + 0x0C)
    return dict(node=node, mesh=mesh, nv=nv, vtx=vtx, nidx=nidx, idx=idx, groups=groups, vtxd=u32(img, mesh + 0x60))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--png")
    args = ap.parse_args()
    img = inflate(args.pack)
    ents, sbase = table(img)
    names = {e["hash"]: e["name"] for e in ents}
    print(f"{args.pack}: {len(img)} bytes, {len(ents)} resources, strings at {sbase:#x}")
    for e in ents:
        if e["type"] != "ACTR":
            continue
        a = find_actr(img, e["hash"], e["size"])
        print(f"ACTR {e['name']} hash {e['hash']:08x} size {e['size']:#x} at {a:#x}" if a is not None else f"ACTR {e['name']}: not found")
        if a is None:
            continue
        print(f"  nodes {u32(img, a + 0x40)}, name {cstr(img, a + u32(img, a + 0x6c))}")
        m = mesh_of(img, a)
        print(f"  node {m['node'] - a:#x} mesh {m['mesh'] - a:#x}: {m['nv']} verts at {m['vtx'] - a:#x}, {m['nidx']} indices at {m['idx'] - a:#x}, "
              f"vertex format {names.get(m['vtxd'], hex(m['vtxd']))}")
        g = m["groups"]
        ng = u32(img, g)
        tl = a + u32(img, g + 8)
        nt = u32(img, tl) if tl + 4 <= len(img) else 0
        texs = [names.get(u32(img, tl + 4 + 4 * k), "?") for k in range(nt if nt < 8 else 0)]
        print(f"  {ng} primitive group(s): material {names.get(u32(img, g + 4), hex(u32(img, g + 4)))}, textures {texs}; "
              f"group words {[hex(u32(img, tl + 4 + 4 * nt + 4 * k)) for k in range(4)] if nt < 8 else '?'}")
        stride = 52
        v = np.frombuffer(img, dtype="<f4", count=m["nv"] * 13, offset=m["vtx"]).reshape(-1, 13)
        pos, nrm, uv = v[:, 0:3], v[:, 3:6], v[:, 7:9]
        nl = np.linalg.norm(nrm, axis=1)
        idx = np.frombuffer(img, dtype="<u2", count=m["nidx"], offset=m["idx"]).astype(int)
        print(f"  normals |n| {nl.min():.3f}..{nl.max():.3f}; indices max {idx.max()} (of {m['nv']}); uv {uv.min(0)} .. {uv.max(0)}")
        lo, hi = pos.min(0), pos.max(0)
        print(f"  bbox min {np.round(lo, 2)} max {np.round(hi, 2)} size {np.round(hi - lo, 2)} centre {np.round((lo + hi) / 2, 2)}")
        c = pos.mean(0)
        if not np.isfinite(pos).all():
            print("  positions not finite: a different vertex layout (stride is not 52)")
            continue
        w, vec = np.linalg.eigh(np.cov((pos - c).T))
        order = np.argsort(w)[::-1]
        for k in order:
            print(f"  principal axis {np.round(vec[:, k], 3)} spread {np.sqrt(w[k]):.2f}")
        if args.png:
            render(pos, nrm, idx, f"{args.png}/{e['name']}")


def render(pos, nrm, idx, base):
    from PIL import Image, ImageDraw

    tris = idx[: len(idx) // 3 * 3].reshape(-1, 3)
    views = {"x": (2, 1, 0), "y": (0, 2, 1), "z": (0, 1, 2)}  # looking along -axis: (u, v, depth)
    for name, (ui, vi, di) in views.items():
        S = 600
        lo, hi = pos.min(0), pos.max(0)
        span = (hi - lo).max() * 1.1
        im = Image.new("RGB", (S, S), (40, 40, 60))
        dr = ImageDraw.Draw(im)
        depth = pos[tris, di].mean(1)
        for t in tris[np.argsort(depth)]:
            p = pos[t]
            n = np.cross(p[1] - p[0], p[2] - p[0])
            ln = np.linalg.norm(n)
            shade = abs(n[di]) / ln if ln > 0 else 0
            col = int(60 + 190 * shade)
            pts = [((q[ui] - (lo[ui] + hi[ui]) / 2) / span * S + S / 2, S / 2 - (q[vi] - (lo[vi] + hi[vi]) / 2) / span * S) for q in p]
            dr.polygon(pts, fill=(col, col, col))
        dr.text((5, 5), f"view along {name}: right = {'xyz'[ui]}, up = {'xyz'[vi]}", fill=(255, 255, 0))
        im.save(f"{base}_{name}.png")


if __name__ == "__main__":
    main()
