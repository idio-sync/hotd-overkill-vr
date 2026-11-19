// When the game crashes, say where: hovr\hovr.log gets the exception, the module and offset of the
// faulting instruction (an exe offset is turned into the address in the supported build, as in
// docs/findings-static.md), the registers and the return addresses found on the stack, and
// hovr\crash.dmp a small minidump. A user's report then shows what crashed (2026-11-19: a report
// whose log just stopped). Installed at the first Present, after the game's and the CRT's own
// start-up; the filter that was there before is called afterwards, so the game's own crash
// handling is unchanged.
#include "common.h"

#include <dbghelp.h>

namespace hv {

static LPTOP_LEVEL_EXCEPTION_FILTER g_previous;
static volatile LONG g_entered;

// "HOTD_NG.exe+0x10d110 (0x50d110 in the supported build)" or "XINPUT1_3.dll+0x1234".
static std::string where(uintptr_t addr) {
    HMODULE m = nullptr;
    char buf[MAX_PATH + 64];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) ||
        !m) {
        snprintf(buf, sizeof buf, "%08x (no module)", unsigned(addr));
        return buf;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    const uintptr_t off = addr - reinterpret_cast<uintptr_t>(m);
    if (m == GetModuleHandleW(nullptr))
        snprintf(buf, sizeof buf, "%s+0x%x (0x%x in the supported build)", name, unsigned(off), unsigned(off + 0x400000));
    else
        snprintf(buf, sizeof buf, "%s+0x%x", name, unsigned(off));
    return buf;
}

static bool in_module_code(uintptr_t v) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi)) return false;
    return mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

// The game is a 32-bit process: 2 GB of address space, 4 GB with a Large Address Aware exe. Its
// own textures, the driver's mappings and the mod's eye-sized targets all live in it, so running
// out of it is a likely way for the game to crash at high headset resolutions.
std::string address_space_status() {
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    SIZE_T largest = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t p = 0x10000; VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi); ) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    char buf[128];
    snprintf(buf, sizeof buf, "address space %llu of %llu MB used, largest free block %llu MB",
             (ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20, ms.ullTotalVirtual >> 20,
             static_cast<unsigned long long>(largest >> 20));
    return buf;
}

static bool read_stack(const uintptr_t* p, uintptr_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void write_dump(EXCEPTION_POINTERS* ep) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    using Dump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto dump = dbghelp ? reinterpret_cast<Dump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!dump) {
        logf("crash: no dbghelp.dll MiniDumpWriteDump, no dump written");
        return;
    }
    const std::wstring path = path_in(L"crash.dmp");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        logf("crash: cannot write %ls (error %lu)", path.c_str(), GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    const BOOL ok = dump(GetCurrentProcess(), GetCurrentProcessId(), f,
                         MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                         &mei, nullptr, nullptr);
    CloseHandle(f);
    logf(ok ? "crash: wrote %ls - please attach it to the report with hovr.log" : "crash: writing %ls failed",
         path.c_str());
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_entered, 1) == 0) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const CONTEXT* c = ep->ContextRecord;
        const uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            logf("crash: access violation (%s of %08x) at %s", er->ExceptionInformation[0] == 0 ? "read"
                                                                : er->ExceptionInformation[0] == 8 ? "execute"
                                                                                                   : "write",
                 unsigned(er->ExceptionInformation[1]), where(at).c_str());
        else
            logf("crash: exception %08lx at %s", er->ExceptionCode, where(at).c_str());
        logf("crash: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx, thread %lu", c->Eax,
             c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, GetCurrentThreadId());
        logf("crash: %s", address_space_status().c_str());
        // Return addresses on the stack: values that point into a module's code.
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Esp);
        int found = 0;
        for (int i = 0; i < 1024 && found < 16; ++i) {
            uintptr_t v;
            if (!read_stack(sp + i, &v)) break;
            if (in_module_code(v)) {
                logf("crash: stack +%03x: %s", unsigned(i * 4), where(v).c_str());
                ++found;
            }
        }
        write_dump(ep);
    }
    return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_handler() {
    static bool done;
    if (done) return;
    done = true;
    g_previous = SetUnhandledExceptionFilter(on_crash);
    logf("crash handler installed (previous filter %p)", reinterpret_cast<void*>(g_previous));
}

}  // namespace hv
