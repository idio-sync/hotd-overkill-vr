"""Cheap static peek at totdo.dll (SecuROM PA): sections + entropy, exports, imports."""
import sys, math, collections
import pefile
p = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Virtual\hotd-vr-game\totdo.dll"
pe = pefile.PE(p)
print("base %X entry %X" % (pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.AddressOfEntryPoint))
for s in pe.sections:
    d = s.get_data()
    c = collections.Counter(d)
    ent = -sum(v / len(d) * math.log2(v / len(d)) for v in c.values()) if d else 0
    print("%-10s va %08X vsz %08X raw %08X ent %.2f chr %08X" % (s.Name.rstrip(b'\0').decode('latin1'), s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData, ent, s.Characteristics))
if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        print("export", e.ordinal, e.name, hex(e.address))
for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    print("import", e.dll.decode(), [ (i.name.decode() if i.name else i.ordinal) for i in e.imports])
