"""Scan .text for SecuROM PA artefacts: long zero runs (removed code) and
jmp [slot] stubs that go through the runtime-filled table in the 0xA71000 section.
usage: securom_scan.py [minrun]
"""
import sys, re, struct
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from hotd import *
minrun = int(sys.argv[1]) if len(sys.argv) > 1 else 64
s, sz = TEXT[1], TEXT[2]
data = bytes(IMG[s - BASE:s - BASE + sz])
runs = [(m.start() + s, len(m.group(0))) for m in re.finditer(rb"\x00{%d,}" % minrun, data)]
print("zero runs >= %d bytes in .text: %d, total %d bytes" % (minrun, len(runs), sum(r[1] for r in runs)))
for a, n in runs[:40]:
    print("  %08X rva %06X len %d" % (a, a - BASE, n))
# jmp dword ptr [abs] = FF 25 imm32 into 0xA71000..0xA72000
stubs = [m.start() + s for m in re.finditer(rb"\xff\x25(?=(....))", data, re.S)
         if 0xA71000 <= struct.unpack("<I", data[m.start() + 2:m.start() + 6])[0] < 0xA72000]
calls = [m.start() + s for m in re.finditer(rb"\xff\x15(?=(....))", data, re.S)
         if 0xA71000 <= struct.unpack("<I", data[m.start() + 2:m.start() + 6])[0] < 0xA72000]
print("jmp [0xA71xxx] stubs:", len(stubs), " call [0xA71xxx]:", len(calls))
slots = sorted(set(u32(a + 2) for a in stubs))
print("distinct slots:", len(slots), "range %X-%X" % (slots[0], slots[-1]))

# who refers to code in the stub regions (functions whose bodies were replaced)
print("\nreferences into stub areas (function -> owner):")
seen = set()
for a in stubs:
    f = a
    refs = callers(a) + [x for x in xrefs_abs(a)]
    for r in refs:
        if in_text(r):
            fs = func_start(r) or r
            key = fs
            if key in seen:
                continue
            seen.add(key)
            # find vtable/data refs to the calling function
            d = [x for x in xrefs_abs(fs) if not in_text(x)]
            owner = ""
            for x in d:
                p = x
                while in_text(u32(p - 4)):
                    p -= 4
                nm = name_of_vtable(p)
                owner += " vt %08X[%d] %s" % (p, (x - p) // 4, nm)
            print("  stub %08X <- %08X in func %08X%s" % (a, r, fs, owner))
        else:
            p = r
            while in_text(u32(p - 4)):
                p -= 4
            print("  stub %08X <- data %08X (vt %08X[%d] %s)" % (a, r, p, (r - p) // 4, name_of_vtable(p)))
