"""Shared helpers for static analysis of HOTD_NG.exe (Typing of the Dead: Overkill).

Usage from other scripts:  from hotd import *
Loads the PE image as mapped in memory (VA-indexed), offers disassembly,
string search, xref search (absolute 4-byte refs and rel32 call/jmp targets),
and RTTI -> vtable resolution.
"""
import os, re, struct, sys, functools
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.environ.get("HOTD_EXE", r"C:\Users\Virtual\hotd-vr-game\HOTD_NG.exe")
_pe = pefile.PE(EXE, fast_load=False)
BASE = _pe.OPTIONAL_HEADER.ImageBase
IMG = bytearray(_pe.get_memory_mapped_image())
SECTIONS = [(s.Name.rstrip(b"\0").decode(), BASE + s.VirtualAddress, s.Misc_VirtualSize) for s in _pe.sections]
TEXT = [s for s in SECTIONS if s[0] == ".text"][0]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

IMPORTS = {}
for e in getattr(_pe, "DIRECTORY_ENTRY_IMPORT", []):
    for i in e.imports:
        nm = (i.name.decode() if i.name else "ord%d" % i.ordinal)
        IMPORTS[i.address] = e.dll.decode() + "!" + nm


def rva(va):
    return va - BASE


def u32(va):
    return struct.unpack_from("<I", IMG, va - BASE)[0]


def f32(va):
    return struct.unpack_from("<f", IMG, va - BASE)[0]


def f64(va):
    return struct.unpack_from("<d", IMG, va - BASE)[0]


def cstr(va, n=256):
    o = va - BASE
    e = IMG.find(b"\0", o, o + n)
    return IMG[o:e].decode("latin1")


def sec_of(va):
    for n, s, sz in SECTIONS:
        if s <= va < s + sz:
            return n
    return None


def in_text(va):
    return TEXT[1] <= va < TEXT[1] + TEXT[2]


def disasm(va, n=40, maxbytes=None):
    o = va - BASE
    code = bytes(IMG[o:o + (maxbytes or n * 16)])
    out = []
    for ins in md.disasm(code, va):
        out.append(ins)
        if len(out) >= n:
            break
    return out


def annotate(ins):
    s = "%08X  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str)
    for m in re.finditer(r"0x([0-9a-f]{6,8})", ins.op_str):
        v = int(m.group(1), 16)
        if v in IMPORTS:
            s += "   ; " + IMPORTS[v]
        elif sec_of(v) and not in_text(v):
            try:
                st = cstr(v, 80)
                if len(st) >= 3 and all(32 <= ord(c) < 127 for c in st):
                    s += '   ; "%s"' % st
                    continue
            except Exception:
                pass
            try:
                fv = f32(v)
                if 1e-6 < abs(fv) < 1e7:
                    s += "   ; f32=%g" % fv
            except Exception:
                pass
    return s


def show(va, n=40, out=sys.stdout):
    for ins in disasm(va, n):
        print(annotate(ins), file=out)


def find_bytes(pat, start=None, end=None):
    res = []
    o = 0 if start is None else start - BASE
    e = len(IMG) if end is None else end - BASE
    while True:
        i = IMG.find(pat, o, e)
        if i < 0:
            break
        res.append(BASE + i)
        o = i + 1
    return res


def find_str(s):
    return find_bytes(s.encode() + b"\0")


def xrefs_abs(va):
    """All places containing the 4-byte absolute VA (code operands or data pointers)."""
    return find_bytes(struct.pack("<I", va))


@functools.lru_cache(maxsize=1)
def _call_index():
    """Map target -> list of call/jmp rel32 sites in .text (byte scan, may include false positives)."""
    idx = {}
    s, sz = TEXT[1], TEXT[2]
    o0 = s - BASE
    data = IMG
    for o in range(o0, o0 + sz - 5):
        b = data[o]
        if b == 0xE8 or b == 0xE9:
            t = (BASE + o + 5 + struct.unpack_from("<i", data, o + 1)[0]) & 0xFFFFFFFF
            if s <= t < s + sz:
                idx.setdefault(t, []).append(BASE + o)
    return idx


def callers(va):
    return _call_index().get(va, [])


def func_start(va, maxback=0x4000):
    """Heuristic: walk back to a likely function start (after CC/C3 padding, or push ebp; mov ebp,esp)."""
    o = va - BASE
    for k in range(o, o - maxback, -1):
        if IMG[k - 1] in (0xCC, 0x90) and IMG[k - 2] in (0xCC, 0x90, 0xC3):
            return BASE + k
        if IMG[k:k + 3] == b"\x55\x8b\xec" and IMG[k - 1] in (0xCC, 0xC3, 0x90):
            return BASE + k
    return None


# ---- RTTI ----
def rtti_typedescs(substr=""):
    """Return {name: typedescriptor_va} for '.?AV...' names containing substr."""
    res = {}
    for m in re.finditer(rb"\.\?A[VU][\w@?$<>]+?@@\0", IMG):
        name = m.group(0)[:-1].decode()
        if substr in name:
            res[name] = BASE + m.start() - 8  # TypeDescriptor: pVFTable, spare, name
    return res


def col_for_td(td):
    """Complete Object Locators referencing typedescriptor td (x86: sig=0, off, cdOff, pTD, pCHD)."""
    cols = []
    for p in xrefs_abs(td):
        col = p - 12
        if u32(col) == 0 and sec_of(col) and not in_text(col):
            cols.append(col)
    return cols


def vtables_for(name):
    tds = rtti_typedescs(name)
    out = []
    for n, td in tds.items():
        if n != name:
            continue
        for col in col_for_td(td):
            off = u32(col + 4)
            for p in xrefs_abs(col):
                vt = p + 4
                out.append((col, off, vt))
    return out


def vtable_entries(vt, maxn=200):
    ents = []
    for i in range(maxn):
        f = u32(vt + 4 * i)
        if not in_text(f):
            break
        ents.append(f)
    return ents


def name_of_vtable(vt):
    col = u32(vt - 4)
    try:
        td = u32(col + 12)
        return cstr(td + 8)
    except Exception:
        return None
