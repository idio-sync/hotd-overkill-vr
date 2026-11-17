"""Disassemble around a VA (or RVA with r prefix) with annotations.

usage: dis.py <va|rNNNN> [count] [--func]  (--func: start at heuristic function start
and stop at int3 padding after the last reachable ret)
"""
import sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from hotd import *

a = sys.argv[1]
va = BASE + int(a[1:], 16) if a.startswith("r") else int(a, 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 60
if "--func" in sys.argv:
    va = func_start(va) or va
    print("; function %08X (rva %06X) callers: %s" % (va, va - BASE, " ".join("%08X" % c for c in callers(va)[:20])))
    # linear sweep until we hit int3 padding, tracking max branch target
    maxt = va
    for ins in disasm(va, 20000):
        print(annotate(ins))
        if ins.mnemonic.startswith("j") and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16)
            if t > maxt and t - va < 0x10000:
                maxt = t
        if ins.mnemonic == "int3" and ins.address > maxt:
            break
else:
    show(va, n)
