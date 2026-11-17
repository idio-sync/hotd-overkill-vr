// Keeps the probe away from the player's real settings and save: the game finds its
// folder with SHGetFolderPathA(CSIDL_APPDATA) (%APPDATA%\sega\typing of the dead overkill,
// synced by Steam Auto-Cloud). Every user folder it asks for is redirected into hovr\.
#include "common.h"

#include <shlobj.h>

namespace hv {

using SHGetFolderPathA_t = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
static SHGetFolderPathA_t o_SHGetFolderPathA;

static HRESULT WINAPI hk_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path) {
    const char* sub = nullptr;
    switch (csidl & 0xFF) {
        case CSIDL_APPDATA: sub = "AppData"; break;
        case CSIDL_LOCAL_APPDATA: sub = "LocalAppData"; break;
        case CSIDL_PERSONAL: sub = "Documents"; break;
        case CSIDL_COMMON_APPDATA: sub = "ProgramData"; break;
    }
    if (!sub) {
        HRESULT hr = o_SHGetFolderPathA(hwnd, csidl, token, flags, path);
        logf("SHGetFolderPathA(0x%x) -> %s (not redirected)", csidl, SUCCEEDED(hr) ? path : "error");
        return hr;
    }
    std::string dir;
    for (wchar_t c : g_dir) dir += char(c);  // the test copy's path is plain ASCII
    dir += "\\";
    dir += sub;
    CreateDirectoryA(dir.c_str(), nullptr);
    strcpy_s(path, MAX_PATH, dir.c_str());
    logf("SHGetFolderPathA(0x%x) -> %s", csidl, path);
    return S_OK;
}

void install_folder_hook() {
    if (!patch_import(GetModuleHandleW(nullptr), "SHELL32.dll", "SHGetFolderPathA",
                      reinterpret_cast<void*>(hk_SHGetFolderPathA), reinterpret_cast<void**>(&o_SHGetFolderPathA))) {
        // Without the redirect the game would write to the real save. Refuse to run.
        MessageBoxW(nullptr, L"hovr probe: could not redirect the save folder; exiting.", L"hovr", MB_OK);
        ExitProcess(1);
    }
}

}  // namespace hv
