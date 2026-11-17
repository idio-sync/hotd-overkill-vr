"""Linear-sweep disassembly of all of .text into out/text.asm (one instruction per line,
annotated with import names / strings / float constants) for grepping.

Linear sweep resyncs at int3/nop padding; jump tables inside .text can cause a few
garbage lines, which is fine for grep-driven work.
"""
import sys, os
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from hotd import *

out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "text.asm")
os.makedirs(os.path.dirname(out), exist_ok=True)
s, sz = TEXT[1], TEXT[2]
with open(out, "w") as f:
    va = s
    end = s + sz
    while va < end:
        chunk = bytes(IMG[va - BASE: min(end, va + 0x10000) - BASE])
        last = va
        for ins in md.disasm(chunk, va):
            f.write(annotate(ins) + "\n")
            last = ins.address + ins.size
        if last == va:  # undecodable byte
            f.write("%08X  db 0x%02x\n" % (va, IMG[va - BASE]))
            last = va + 1
        va = last
print(out)
