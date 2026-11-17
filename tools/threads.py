"""Where are the game's threads? A poor man's stack dump for a hung HOTD_NG.exe (no debugger needed).

    .venv\\Scripts\\python -I tools/threads.py PID [--scan N]

Briefly suspends each thread of the (32-bit) process, prints EIP and the EBP frame chain, plus
(--scan) the first N stack words that point into a loaded module (likely return addresses),
as module+offset; HOTD_NG.exe addresses also as image VAs (base 0x400000).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

TH32CS_SNAPTHREAD = 4
THREAD_ALL = 0x001F03FF
PROCESS_ALL = 0x1F0FFF


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD), ("EntryPoint", ctypes.c_void_p)]


def modules(hp):
    arr = (ctypes.c_void_p * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(hp, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = []
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(arr[i]), name, 260)
        mi = MODULEINFO()
        psapi.GetModuleInformation(hp, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
        if mi.lpBaseOfDll and mi.lpBaseOfDll < 1 << 32:
            out.append((mi.lpBaseOfDll, mi.SizeOfImage, name.value))
    return sorted(out)


def where(mods, a):
    for base, size, name in mods:
        if base <= a < base + size:
            s = "%s+%x" % (name, a - base)
            if name.lower() == "hotd_ng.exe":
                s += " (va %08X)" % (a - base + 0x400000)
            return s
    return None


def read(hp, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return buf.raw[: got.value]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("--scan", type=int, default=12)
    args = ap.parse_args()
    hp = k32.OpenProcess(PROCESS_ALL, False, args.pid)
    mods = modules(hp)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == args.pid:
            ht = k32.OpenThread(THREAD_ALL, False, te.th32ThreadID)
            ctx = ctypes.create_string_buffer(716)
            struct.pack_into("<I", ctx, 0, 0x10007)
            k32.Wow64SuspendThread(ht)
            if k32.Wow64GetThreadContext(ht, ctx):
                ebp, eip, esp = struct.unpack_from("<I", ctx, 180)[0], struct.unpack_from("<I", ctx, 184)[0], struct.unpack_from("<I", ctx, 196)[0]
                print("thread %d: eip %08x %s" % (te.th32ThreadID, eip, where(mods, eip) or "?"))
                chain = []
                fp = ebp
                for _ in range(16):
                    d = read(hp, fp, 8)
                    if len(d) < 8:
                        break
                    nfp, ret = struct.unpack("<II", d)
                    chain.append(where(mods, ret) or "%08x" % ret)
                    if nfp <= fp:
                        break
                    fp = nfp
                print("   ebp chain: " + " <- ".join(chain[:10]))
                st = read(hp, esp, 4096)
                hits = []
                for i in range(0, len(st) - 3, 4):
                    v = struct.unpack_from("<I", st, i)[0]
                    w = where(mods, v)
                    if w and not w.lower().startswith(("hotd_ng.exe+0 ",)):
                        hits.append(w)
                    if len(hits) >= args.scan:
                        break
                print("   stack: " + " | ".join(hits))
            k32.ResumeThread(ht)
            k32.CloseHandle(ht)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)


if __name__ == "__main__":
    main()
