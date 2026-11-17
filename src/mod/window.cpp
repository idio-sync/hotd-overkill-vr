// Keeps the game window minimized and out of the way (minimized=1, the default) while the
// game believes it is active: activation/focus loss messages are rewritten, the client size
// of the minimized window is faked, and focus-stealing calls are dropped. Also scripted
// mouse/keyboard: a fake cursor position (GetCursorPos), posted button/key messages, and a
// fake GetAsyncKeyState table.
#include "common.h"

#include <atomic>

namespace hv {

static std::atomic<HWND> g_hwnd;
static WNDPROC g_orig_proc;
static LONG g_client_w = 1280, g_client_h = 720;
static std::atomic<int> g_cursor_x{-1}, g_cursor_y{-1};
static std::atomic<uint8_t> g_async[256];

static bool minimized_mode() {
    static int on = ini_int(L"minimized", 1);
    return on != 0;
}

HWND game_window() { return g_hwnd.load(); }

static LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (minimized_mode()) {
        switch (msg) {
            case WM_ACTIVATEAPP:
                if (!wp) wp = TRUE;
                break;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) wp = MAKEWPARAM(WA_ACTIVE, 0);
                break;
            case WM_NCACTIVATE:
                wp = TRUE;
                break;
            case WM_KILLFOCUS:
                return 0;
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) return 0;
                break;
        }
    }
    return CallWindowProcA(g_orig_proc, hwnd, msg, wp, lp);
}

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static CreateWindowExA_t o_CreateWindowExA;
static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND hwnd = o_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    logf("CreateWindowExA class=%s name=%s style=%08lx ex=%08lx %dx%d at %d,%d parent=%p -> %p",
         HIWORD(reinterpret_cast<uintptr_t>(cls)) ? cls : "#atom", name ? name : "", style, ex, w, h, x, y, parent, hwnd);
    if (hwnd && !parent && !(style & WS_CHILD) && !g_hwnd.load()) {
        g_hwnd = hwnd;
        g_orig_proc = reinterpret_cast<WNDPROC>(SetWindowLongA(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(hk_wndproc)));
        if (minimized_mode()) {
            PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
            PostMessageA(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            PostMessageA(hwnd, WM_SETFOCUS, 0, 0);
        }
    }
    return hwnd;
}

using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
static ShowWindow_t o_ShowWindow;
static BOOL WINAPI hk_ShowWindow(HWND hwnd, int cmd) {
    if (hwnd == g_hwnd.load() && minimized_mode() && cmd != SW_HIDE) {
        logf("ShowWindow(%d) -> SW_SHOWMINNOACTIVE", cmd);
        cmd = SW_SHOWMINNOACTIVE;
    }
    return o_ShowWindow(hwnd, cmd);
}

using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
static SetWindowPos_t o_SetWindowPos;
static BOOL WINAPI hk_SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
    if (hwnd == g_hwnd.load() && minimized_mode()) {
        logf("SetWindowPos %d,%d %dx%d flags %x (no activate/z-order)", x, y, w, h, flags);
        flags |= SWP_NOACTIVATE | SWP_NOZORDER;
        flags &= ~UINT(SWP_SHOWWINDOW);
    }
    return o_SetWindowPos(hwnd, after, x, y, w, h, flags);
}

using HwndBool_t = BOOL(WINAPI*)(HWND);
static HwndBool_t o_SetForegroundWindow, o_BringWindowToTop;
static BOOL WINAPI hk_SetForegroundWindow(HWND hwnd) {
    if (minimized_mode()) return TRUE;
    return o_SetForegroundWindow(hwnd);
}
static BOOL WINAPI hk_BringWindowToTop(HWND hwnd) {
    if (minimized_mode()) return TRUE;
    return o_BringWindowToTop(hwnd);
}

using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
static GetClientRect_t o_GetClientRect;
static BOOL WINAPI hk_GetClientRect(HWND hwnd, LPRECT r) {
    BOOL ok = o_GetClientRect(hwnd, r);
    if (hwnd == g_hwnd.load()) {
        if (IsIconic(hwnd)) {
            r->left = r->top = 0;
            r->right = g_client_w;
            r->bottom = g_client_h;
        } else if (ok && r->right > 0) {
            g_client_w = r->right;
            g_client_h = r->bottom;
        }
    }
    return ok;
}

using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
static GetCursorPos_t o_GetCursorPos;
static BOOL WINAPI hk_GetCursorPos(LPPOINT p) {
    int x = g_cursor_x.load();
    HWND hwnd = g_hwnd.load();
    if (x >= 0 && hwnd) {
        p->x = x;
        p->y = g_cursor_y.load();
        ClientToScreen(hwnd, p);  // the game converts back with ScreenToClient
        return TRUE;
    }
    return o_GetCursorPos(p);
}

using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
static GetAsyncKeyState_t o_GetAsyncKeyState;
static SHORT WINAPI hk_GetAsyncKeyState(int vk) {
    SHORT real = o_GetAsyncKeyState(vk);
    if (vk >= 0 && vk < 256 && g_async[vk].load()) return SHORT(-32767);  // 0x8001: down, and pressed since the last call
    // A minimized test window never has the user's real keys.
    if (minimized_mode()) return 0;
    return real;
}

void set_fake_cursor(int x, int y) {
    g_cursor_x = x;
    g_cursor_y = y;
    HWND hwnd = g_hwnd.load();
    if (hwnd && x >= 0) PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
}

void post_mouse_button(bool right, bool down) {
    g_async[right ? VK_RBUTTON : VK_LBUTTON] = down;
    di_mouse_button(right ? 1 : 0, down);
    HWND hwnd = g_hwnd.load();
    if (!hwnd) return;
    int x = (std::max)(0, g_cursor_x.load()), y = (std::max)(0, g_cursor_y.load());
    UINT msg = right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    PostMessageA(hwnd, msg, down ? (right ? MK_RBUTTON : MK_LBUTTON) : 0, MAKELPARAM(x, y));
}

void post_key(UINT vk, bool down) {
    g_async[vk & 0xFF] = down;
    di_key(vk, down);
    HWND hwnd = g_hwnd.load();
    if (!hwnd) return;
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (LPARAM(scan) << 16) | (down ? 0 : (LPARAM(3) << 30));
    PostMessageA(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

void install_window_hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto hook = [&](const char* name, void* fn, void** orig) { patch_import(exe, "USER32.dll", name, fn, orig); };
    hook("CreateWindowExA", reinterpret_cast<void*>(hk_CreateWindowExA), reinterpret_cast<void**>(&o_CreateWindowExA));
    hook("ShowWindow", reinterpret_cast<void*>(hk_ShowWindow), reinterpret_cast<void**>(&o_ShowWindow));
    hook("SetWindowPos", reinterpret_cast<void*>(hk_SetWindowPos), reinterpret_cast<void**>(&o_SetWindowPos));
    hook("SetForegroundWindow", reinterpret_cast<void*>(hk_SetForegroundWindow), reinterpret_cast<void**>(&o_SetForegroundWindow));
    hook("BringWindowToTop", reinterpret_cast<void*>(hk_BringWindowToTop), reinterpret_cast<void**>(&o_BringWindowToTop));
    hook("GetClientRect", reinterpret_cast<void*>(hk_GetClientRect), reinterpret_cast<void**>(&o_GetClientRect));
    hook("GetCursorPos", reinterpret_cast<void*>(hk_GetCursorPos), reinterpret_cast<void**>(&o_GetCursorPos));
    hook("GetAsyncKeyState", reinterpret_cast<void*>(hk_GetAsyncKeyState), reinterpret_cast<void**>(&o_GetAsyncKeyState));
}

}  // namespace hv
