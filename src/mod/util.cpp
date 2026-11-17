// Paths, log, import/vtable patching, inline detours.
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hv {

std::wstring g_game_dir;
std::wstring g_dir;

static FILE* g_log;
static std::mutex g_log_mx;
static ULONGLONG g_start;

void init_paths() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_game_dir = buf;
    g_game_dir.resize(g_game_dir.find_last_of(L"\\/"));
    g_dir = g_game_dir + L"\\hovr";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_start = GetTickCount64();
}

std::wstring path_in(const wchar_t* name) { return g_dir + L"\\" + name; }

int ini_int(const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(L"probe", key, def, path_in(L"probe.ini").c_str()));
}

void log_open() { g_log = _wfopen(path_in(L"probe.log").c_str(), L"w"); }

void logf(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard lock(g_log_mx);
    fprintf(g_log, "[%8.3f] ", (GetTickCount64() - g_start) / 1000.0);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool readable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    return addr && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

void* install_detour(uintptr_t image_va, const uint8_t* expect, size_t n, void* hook, const char* name) {
    auto target = reinterpret_cast<uint8_t*>(va(image_va));
    if (n < 5 || n > 16 || memcmp(target, expect, n) != 0) {
        logf("detour %s: unexpected bytes at %p (%02x %02x %02x %02x %02x); not installed", name, target, target[0], target[1],
             target[2], target[3], target[4]);
        return nullptr;
    }
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    memcpy(tramp, target, n);
    tramp[n] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + n + 1) = int32_t((target + n) - (tramp + n + 5));
    DWORD old;
    VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = int32_t(static_cast<uint8_t*>(hook) - (target + 5));
    for (size_t i = 5; i < n; ++i) target[i] = 0x90;
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    logf("detour %s installed at %p (trampoline %p)", name, target, tramp);
    return tramp;
}

uint64_t fnv64(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static bool write_ptr(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool patch_import(HMODULE module, const char* dll, const char* func, void* hook, void** orig) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    // The exe lists some DLLs more than once (three KERNEL32 descriptors), so keep looking.
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byname->Name), func) != 0) continue;
            auto slot = reinterpret_cast<void**>(&slots->u1.Function);
            *orig = *slot;
            bool ok = write_ptr(slot, hook);
            logf("import %s!%s %s (was %p)", dll, func, ok ? "hooked" : "PATCH FAILED", *orig);
            return ok;
        }
    }
    logf("import %s!%s not found", dll, func);
    return false;
}

bool patch_vtbl(void* object, int index, void* hook, void** orig) {
    auto vtbl = *reinterpret_cast<void***>(object);
    if (vtbl[index] == hook) return true;
    *orig = vtbl[index];
    return write_ptr(&vtbl[index], hook);
}

}  // namespace hv
