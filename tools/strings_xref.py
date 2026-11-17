"""Find ASCII strings matching a regex and list code references to each.

usage: strings_xref.py <regex> [--min N] [--noxref]
Prints  VA  RVA  "string"  -> xref sites (with containing function guess)
"""
import re, sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from hotd import *

pat = re.compile(sys.argv[1].encode(), re.I)
minlen = 4
noxref = "--noxref" in sys.argv
for m in re.finditer(rb"[\x20-\x7e]{%d,}\0" % minlen, IMG):
    s = m.group(0)[:-1]
    if not pat.search(s):
        continue
    va = BASE + m.start()
    if in_text(va):
        continue
    line = '%08X rva %06X "%s"' % (va, va - BASE, s.decode())
    if not noxref:
        xs = [x for x in xrefs_abs(va)]
        line += "  xrefs: " + ", ".join(
            "%08X%s" % (x, "(f%08X)" % (func_start(x) or 0) if in_text(x) else "[d]") for x in xs[:8])
    print(line)
