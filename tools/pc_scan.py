"""Looks inside BlitzTech .pc asset packs for zlib streams and readable names.

    .venv\\Scripts\\python -I tools/pc_scan.py FILE.pc [...] [--grep REGEX]
"""
import argparse
import re
import struct
import zlib

NAME = re.compile(rb"[A-Za-z0-9_\-. /\\]{6,}")


def scan(path, grep):
    d = open(path, "rb").read()
    print(f"== {path} {len(d)} bytes, header u32 {struct.unpack_from('<12I', d, 0)}")
    streams, total, names = 0, 0, set()
    for m in re.finditer(rb"\x78[\x01\x5e\x9c\xda]", d):
        try:
            out = zlib.decompressobj().decompress(d[m.start():m.start() + 8_000_000])
        except zlib.error:
            continue
        if len(out) < 64:
            continue
        streams += 1
        total += len(out)
        names.update(s.group().decode() for s in NAME.finditer(out))
    raw = {s.group().decode() for s in NAME.finditer(d)}
    print(f"  zlib streams {streams}, {total} bytes inflated; {len(names)} names inside, {len(raw)} raw")
    pat = re.compile(grep, re.I)
    hits = sorted(n for n in names | raw if pat.search(n))
    for n in hits[:80]:
        print("   ", n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--grep", default=r"gun|pistol|magnum|shotgun|rifle|weap|bullet|crossbow|minigun|\.dds|\.tga|mesh")
    a = ap.parse_args()
    for f in a.files:
        scan(f, a.grep)


if __name__ == "__main__":
    main()
