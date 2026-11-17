"""Find functions whose instructions match ALL given regexes (uses out/text.asm).

usage: funcgrep.py <regex1> [<regex2> ...]
Function boundaries: lines after int3 runs (approximate).
"""
import re, sys, os
asm = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "text.asm")
pats = [re.compile(p) for p in sys.argv[1:]]
cur = None
hits = {}
lines = []
prev_int3 = True


def flush():
    if cur is None:
        return
    found = [None] * len(pats)
    for l in lines:
        for i, p in enumerate(pats):
            if found[i] is None and p.search(l):
                found[i] = l
    if all(found):
        print("func %s  (%d insns)" % (cur, len(lines)))
        for f in found:
            print("    " + f)


with open(asm) as fh:
    for l in fh:
        l = l.rstrip("\n")
        is_int3 = " int3" in l
        if is_int3:
            prev_int3 = True
            continue
        if prev_int3:
            flush()
            cur = l[:8]
            lines = []
            prev_int3 = False
        lines.append(l)
flush()
