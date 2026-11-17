// Logs the game's asset opens (CreateFileA/W on .pc files), to see what loads when.
#include "common.h"

namespace hv {

using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static CreateFileW_t o_CreateFileW;

static bool interesting(const char* name) {
    size_t n = name ? strlen(name) : 0;
    return n > 3 && _stricmp(name + n - 3, ".pc") == 0;
}

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (interesting(name)) logf("open %s%s", name, h == INVALID_HANDLE_VALUE ? " (failed)" : "");
    return h;
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (name) {
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, nullptr, nullptr);
        if (interesting(a)) logf("open %s%s", a, h == INVALID_HANDLE_VALUE ? " (failed)" : "");
    }
    return h;
}

void install_file_hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_import(exe, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(hk_CreateFileA), reinterpret_cast<void**>(&o_CreateFileA));
    patch_import(exe, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(hk_CreateFileW), reinterpret_cast<void**>(&o_CreateFileW));
    // The game opens its assets through the C runtime (fopen/_open), not its own imports.
    if (HMODULE crt = GetModuleHandleW(L"MSVCR100.dll")) {
        void* unused;
        patch_import(crt, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(hk_CreateFileA), &unused);
        patch_import(crt, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(hk_CreateFileW), &unused);
    }
}

}  // namespace hv
