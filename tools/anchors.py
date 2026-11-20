"""The anchor table: every place in HOTD_NG.exe that the mod depends on.

The mod patches and reads fixed addresses of one build of the game (Steam build 569304). Instead
of requiring the whole exe to be that file, the mod compares these anchors with the exe on disk
(src/mod/buildcheck.cpp, src/vr/exe_anchors.cpp): a copy changed by another patcher (a 4 GB / LAA
patch, a fix somewhere the mod doesn't use) still plays in VR; one that changed a place the mod
hooks or reads is refused, and the log names the anchor.

Anchor kinds (SPEC below):
  detour  the first bytes of a function the mod hooks with an inline detour (the same bytes the
          detour itself checks, src/mod/*.cpp install_detour calls)
  call    the first instructions of a game function the mod calls directly
  retaddr a call instruction whose return address the mod compares with
  global  an instruction in the game's code that references a fixed global the mod reads or
          writes (a moved global changes that instruction's address operand)
  vtable  an instruction that stores a vtable the mod compares objects with, or the vtable's own
          bytes (RTTI locator, first slot)

Bytes are compared as they are in the file: not relocated, so the absolute addresses in these
instructions are the image-base 0x400000 ones. Nothing in SecuROM's runtime-filled table
(0xA71000..0xA71844) or its zeroed .text runs is used.

usage:
  anchors.py --write [--exe KNOWN_EXE]   regenerate src/vr/exe_anchors_table.h from the known exe
                                         (needs pefile and capstone: the repo's .venv)
  anchors.py --check EXE                 check an exe against the table (standard library only);
                                         exit 0: known build or compatible copy, 1: refused, 2: error
  anchors.py --find VA [--exe KNOWN_EXE] list instructions in .text that reference VA (to pick a
                                         site for a new global or vtable anchor)

When the mod starts using a new address: add it to SPEC, run --write, rebuild (the unit test
checks the table's consistency and, where the game exe exists, the table against it).
"""
import argparse
import hashlib
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(REPO, "src", "vr", "exe_anchors_table.h")
DEFAULT_EXE = os.environ.get("HOTD_EXE", r"C:\Users\Virtual\hotd-vr-game\HOTD_NG.exe")
IMAGE_BASE = 0x400000
SECUROM_TABLE = (0xA71000, 0xA71844)  # runtime-filled jmp table: never an anchor
MAX_LEN = 16
KNOWN_SHA256 = "f9db505a3835af7cef136d4c2f9e2df819698bd1183a033af6b5a2f90275bb03"  # Steam build 569304

# (kind, name, va, length) - va: where the anchor's bytes are. length: for detour/call, at least
# that many bytes, rounded up to whole instructions; for retaddr/global/vtable stores, a number of
# whole instructions ("2i"); for vtable data, bytes. target: the address the mod uses, when the
# anchor sits elsewhere (globals, vtables, the return address).
SPEC = [
    # Inline detours (src/mod/gun.cpp, engine.cpp, comfort.cpp).
    ("detour", "FireBullets", 0x50D110, 9, None),
    ("detour", "projectile shot", 0x4E1330, 9, None),
    ("detour", "sphere check A", 0x42B900, 6, None),
    ("detour", "sphere check B", 0x479250, 6, None),
    ("detour", "SetCursorX", 0x5295F0, 6, None),
    ("detour", "SetCursorY", 0x51FBD0, 6, None),
    ("detour", "input actions", 0x426A20, 5, None),
    ("detour", "frustum A", 0x6DEDE0, 6, None),
    ("detour", "frustum B", 0x6DF810, 6, None),
    ("detour", "entity draw", 0x7697A0, 7, None),
    ("detour", "camera shake (and its global 0x9B01A8)", 0x4BF210, 8, None),
    ("detour", "camera smack", 0x529CA0, 6, None),
    ("detour", "wiiview edge-pan", 0x55F5C0, 7, None),
    # Also called by gun.cpp; detoured in the development harness only (the shot log).
    ("detour", "world ray cast", 0x5514E0, 9, None),
    # Game functions the mod calls (gun.cpp).
    ("call", "ScreenToWorld", 0x655620, 6, None),
    ("call", "ray result init", 0x4D6830, 6, None),
    ("call", "ray result entity", 0x48C8D0, 6, None),
    ("call", "quaternion to matrix", 0x6A44A0, 6, None),
    ("call", "matrix times vector", 0x6A2A50, 6, None),
    # FireBullets' return address in the weapon's fire (gun.cpp kFireRet 0x52B164).
    ("retaddr", "weapon fire calls FireBullets", 0x52B15F, "1i", 0x52B164),
    # FireBullets' world ray cast per pellet (gun.cpp kCastRet 0x50D27E; the shot log).
    ("retaddr", "FireBullets calls the world ray cast", 0x50D279, "1i", 0x50D27E),
    # Globals (engine.cpp, gamestate.cpp, gun.cpp, gunmodel.cpp, stereo.cpp, comfort.cpp).
    ("global", "app object, fps limit flag +0x48 (main loop)", 0x6FDF8D, "2i", 0x9F09E0),
    ("global", "cGameControl (input actions)", 0x426C98, "1i", 0x99B708),
    ("global", "cCameraManager (head-bob)", 0x4DCD01, "1i", 0x99B8F4),
    ("global", "cCutsceneControl, cutscene flag +8 (IsCutSceneRunning)", 0x57B420, "2i", 0x99B770),
    ("global", "cGameModeManager, paused flag +0x310", 0x58A520, "2i", 0x99B71C),
    ("global", "projection matrix", 0x655823, "1i", 0x9C7DC0),
    ("global", "view matrix (ScreenToWorld)", 0x655764, "1i", 0x9C7E48),
    ("global", "camera position", 0x64DDB0, "1i", 0x9C7E88),
    ("global", "FOV X and Y (written)", 0x66A940, "3i", 0x9C7F20),
    ("global", "half-FOV sin X (frustum A)", 0x6DEDE6, "1i", 0x9C7F28),
    ("global", "half-FOV cos X (frustum A)", 0x6DEDF2, "1i", 0x9C7F30),
    ("global", "half-FOV cos Y (frustum A)", 0x6DEE2B, "1i", 0x9C7F34),
    ("global", "half-FOV sin Y (frustum A)", 0x6DEE36, "1i", 0x9C7F2C),
    # Vtables (comfort.cpp kHudWeaponDisplayVtbl, gunmodel.cpp kInstanceVtbl).
    ("vtable", "cPlayerHUDWeaponDisplay vtable (constructor)", 0x54F292, "1i", 0x800BCC),
    ("vtable", "cPlayerHUDWeaponDisplay vtable (RTTI locator, slot 0)", 0x800BC8, 8, 0x800BCC),
    ("vtable", "actor instance vtable (constructor)", 0x6FA9CB, "1i", 0x80FB58),
    ("vtable", "actor instance vtable (slots 0-1)", 0x80FB58, 8, 0x80FB58),
]

# The FOV anchor also covers 0x9C7F24 (its third instruction).
EXTRA_TARGETS = {0x66A940: [0x9C7F24]}


# ---------------------------------------------------------------- PE (standard library only)

class Pe:
    def __init__(self, data):
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise ValueError("not an exe (no MZ header)")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.timestamp = struct.unpack_from("<I", data, pe + 8)[0]
        self.characteristics = struct.unpack_from("<H", data, pe + 22)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
        self.sections = []
        for i in range(nsec):
            o = opt + opt_size + 40 * i
            if o + 40 > len(data):
                raise ValueError("truncated section table")
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, IMAGE_BASE + va, vsize, raw, rawsize))

    def file_offset(self, va, n):
        """File offset of n bytes at image VA va, or None unless they all lie in one section's raw
        data (and inside its mapped size)."""
        for name, sva, vsize, raw, rawsize in self.sections:
            limit = min(rawsize, vsize) if vsize else rawsize
            if sva <= va and va + n <= sva + limit:
                off = raw + va - sva
                return off if off + n <= len(self.data) else None
        return None

    def regions(self):
        """(label, offset, size) of the headers, each section's raw data and the overlay."""
        out = [("PE headers", 0, min(self.size_of_headers, len(self.data)))]
        end = self.size_of_headers
        for name, sva, vsize, raw, rawsize in self.sections:
            out.append(("%s (0x%X)" % (name, sva), raw, rawsize))
            end = max(end, raw + rawsize)
        out.append(("signature/overlay", end, max(0, len(self.data) - end)))
        return out


def fnv64(b):
    """FNV-1a 64, as src/mod/util.cpp and src/vr/exe_anchors.cpp compute it."""
    h = 1469598103934665603
    for x in memoryview(b):
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------- table file

ROW = re.compile(r'\{AnchorKind::(\w+), "((?:[^"\\]|\\.)*)", 0x([0-9A-F]+), 0x([0-9A-F]+), (\d+), \{([^}]*)\}\}')
SEC = re.compile(r'\{"([^"]*)", 0x([0-9A-F]+), 0x([0-9A-F]+), 0x([0-9a-f]+)ull\}')


def read_table(path=TABLE):
    text = open(path, encoding="utf-8").read()
    t = {
        "size": int(re.search(r"kKnownExeSize = (\d+)", text).group(1)),
        "fnv64": int(re.search(r"kKnownExeFnv64 = 0x([0-9a-f]+)ull", text).group(1), 16),
        "timestamp": int(re.search(r"kKnownExeTimestamp = 0x([0-9A-F]+)", text).group(1), 16),
        "sha256": re.search(r'kKnownExeSha256\[\] = "([0-9a-f]+)"', text).group(1),
        "anchors": [],
        "sections": [],
    }
    for m in ROW.finditer(text):
        bs = bytes(int(x, 16) for x in m.group(6).split(","))[:int(m.group(5))]
        t["anchors"].append({"kind": m.group(1), "name": m.group(2), "va": int(m.group(3), 16),
                             "target": int(m.group(4), 16), "bytes": bs})
    for m in SEC.finditer(text):
        t["sections"].append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)))
    return t


def check(path, table=None, out=sys.stdout):
    t = table or read_table()
    data = open(path, "rb").read()
    h = fnv64(data)
    known = len(data) == t["size"] and h == t["fnv64"]
    try:
        pe = Pe(data)
    except ValueError as e:
        print("%s: %s - refused" % (path, e), file=out)
        return 1
    print("%s: %d bytes, fnv64 %016x, sha256 %s, PE timestamp %08x, large-address-aware %s" %
          (path, len(data), h, hashlib.sha256(data).hexdigest(), pe.timestamp,
           "on" if pe.characteristics & 0x20 else "off"), file=out)
    if known:
        print("known build (Steam 569304): all %d anchors match" % len(t["anchors"]), file=out)
        return 0
    fails = []
    if pe.image_base != IMAGE_BASE:
        fails.append("PE image base: expected %08X, found %08X" % (IMAGE_BASE, pe.image_base))
    for a in t["anchors"]:
        n = len(a["bytes"])
        off = pe.file_offset(a["va"], n)
        found = data[off:off + n] if off is not None else None
        if found != a["bytes"]:
            fails.append('anchor "%s" (%s) at 0x%X: expected %s, found %s' %
                         (a["name"], a["kind"], a["va"], a["bytes"].hex(" "),
                          found.hex(" ") if found is not None else "nothing (outside the file's sections)"))
    changed = []
    regions = pe.regions()
    if [(r[1], r[2]) for r in regions] != [(s[1], s[2]) for s in t["sections"]]:
        changed.append("section layout")
    else:
        for (label, off, size), (_, _, _, kh) in zip(regions, t["sections"]):
            if fnv64(data[off:off + size]) != kh:
                changed.append(label)
    for f in fails:
        print(f, file=out)
    print("differs from the known build in: %s" % (", ".join(changed) or "nothing"), file=out)
    if fails:
        same = pe.timestamp == t["timestamp"]
        print("refused: %d of %d anchors differ (%s)" %
              (len(fails), len(t["anchors"]), "a changed copy of the supported build" if same else "a different build"),
              file=out)
        return 1
    print("compatible: a patched copy of the supported build; all %d anchors match" % len(t["anchors"]), file=out)
    return 0


# ---------------------------------------------------------------- generator (pefile + capstone)

def _decode(md, data, pe, va, n_ins):
    off = pe.file_offset(va, 16)
    ins = list(md.disasm(data[off:off + 32], va))[:n_ins]
    if len(ins) < n_ins or any(i.address != va + sum(j.size for j in ins[:k]) for k, i in enumerate(ins)):
        raise SystemExit("0x%X: cannot decode %d instructions" % (va, n_ins))
    return ins


def generate(exe):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    data = open(exe, "rb").read()
    if hashlib.sha256(data).hexdigest() != KNOWN_SHA256:
        raise SystemExit("%s is not the known build (SHA-256 %s): the table is generated from that one" % (exe, KNOWN_SHA256))
    pe = Pe(data)
    rows = []
    for kind, name, va, length, target in SPEC:
        target = target or va
        if isinstance(length, str):  # whole instructions
            ins = _decode(md, data, pe, va, int(length[:-1]))
            n = sum(i.size for i in ins)
        elif kind in ("detour", "call"):  # at least `length` bytes, whole instructions
            n = 0
            for i in md.disasm(data[pe.file_offset(va, 16):pe.file_offset(va, 16) + 32], va):
                if i.address != va + n:
                    raise SystemExit("0x%X: decode error" % va)
                n += i.size
                if n >= length:
                    break
        else:
            n = length
        if not 1 <= n <= MAX_LEN:
            raise SystemExit("%s: length %d" % (name, n))
        off = pe.file_offset(va, n)
        if off is None:
            raise SystemExit("%s: 0x%X not in the file" % (name, va))
        if va < SECUROM_TABLE[1] and va + n > SECUROM_TABLE[0]:
            raise SystemExit("%s: inside SecuROM's runtime table" % name)
        b = data[off:off + n]
        if kind in ("global", "vtable") and isinstance(length, str):
            for t in [target] + EXTRA_TARGETS.get(va, []):
                if struct.pack("<I", t) not in b:
                    raise SystemExit("%s: the instruction at 0x%X does not reference 0x%X" % (name, va, t))
        if kind == "retaddr":
            if b[0] != 0xE8 or va + n != target:
                raise SystemExit("%s: not a call returning to 0x%X" % (name, target))
        rows.append((kind, name, va, target, b))
    regions = pe.regions()
    tsha = hashlib.sha256(data).hexdigest()
    lines = [
        "// Generated by tools/anchors.py --write from HOTD_NG.exe (Steam build 569304) - do not edit;",
        "// change SPEC in tools/anchors.py and run it again. The places in the exe the mod depends on",
        "// (src/vr/exe_anchors.h), as bytes in the file (not relocated: image base 0x400000).",
        "#pragma once",
        "",
        '#include "exe_anchors.h"',
        "",
        "namespace hv::vr {",
        "",
        "inline constexpr uint64_t kKnownExeSize = %d;" % len(data),
        "inline constexpr uint64_t kKnownExeFnv64 = 0x%016xull;" % fnv64(data),
        "inline constexpr uint32_t kKnownExeTimestamp = 0x%08X;" % pe.timestamp,
        'inline constexpr char kKnownExeSha256[] = "%s";' % tsha,
        "",
        "// The known exe's regions (label, file offset, size, FNV-1a 64): which ones a patched copy changed.",
        "inline constexpr KnownRegion kKnownRegions[] = {",
    ]
    for label, off, size in regions:
        lines.append('    {"%s", 0x%X, 0x%X, 0x%016xull},' % (label, off, size, fnv64(data[off:off + size])))
    lines += ["};", "", "inline constexpr Anchor kAnchors[] = {"]
    for kind, name, va, target, b in rows:
        lines.append('    {AnchorKind::%s, "%s", 0x%X, 0x%X, %d, {%s}},' %
                     (kind, name, va, target, len(b), ", ".join("0x%02X" % x for x in b)))
    lines += ["};", "", "}  // namespace hv::vr", ""]
    with open(TABLE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    kinds = {}
    for r in rows:
        kinds[r[0]] = kinds.get(r[0], 0) + 1
    print("wrote %s: %d anchors (%s), %d regions" %
          (TABLE, len(rows), ", ".join("%d %s" % (v, k) for k, v in kinds.items()), len(regions)))


def find(exe, target):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    data = open(exe, "rb").read()
    pe = Pe(data)
    text = [s for s in pe.sections if s[0] == ".text"][0]
    _, tva, tvsize, traw, trawsize = text
    pat = struct.pack("<I", target)
    i = data.find(pat, traw, traw + trawsize)
    while i >= 0:
        p = tva + i - traw
        for back in range(10, 0, -1):  # longest instruction that covers the operand
            st = p - back
            o = traw + st - tva
            ins = next(md.disasm(data[o:o + 16], st), None)
            if ins and st + ins.size >= p + 4 and ("0x%x" % target) in ins.op_str:
                print("%08X  %-30s %s %s" % (st, bytes(ins.bytes).hex(" "), ins.mnemonic, ins.op_str))
                break
        i = data.find(pat, i + 1, traw + trawsize)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", metavar="EXE")
    ap.add_argument("--find", metavar="VA")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="the known exe (--write, --find)")
    a = ap.parse_args()
    if a.write:
        generate(a.exe)
    elif a.check:
        try:
            sys.exit(check(a.check))
        except OSError as e:
            print(e)
            sys.exit(2)
    elif a.find:
        find(a.exe, int(a.find, 16))
    else:
        ap.print_help()
        sys.exit(2)


if __name__ == "__main__":
    main()
