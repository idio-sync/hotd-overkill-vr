// The stand-in XINPUT1_3.dll's exports. HOTD_NG.exe imports only ordinal 2 (XInputGetState);
// the rest forward to the system's xinput1_4.dll. With fake_pad=1 player 1 always reads as
// connected and scripted buttons/sticks from the command file are ORed in.
#include "common.h"

#include <mutex>

namespace hv {

static HMODULE real_xinput() {
    static HMODULE h = [] {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat_s(p, L"\\xinput1_4.dll");
        HMODULE m = LoadLibraryW(p);
        logf("real xinput %ls: %p", p, m);
        return m;
    }();
    return h;
}

template <class T>
static T real_fn(const char* name) {
    HMODULE m = real_xinput();
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

static std::mutex g_pad_mx;
static XINPUT_GAMEPAD g_pad{};

bool fake_pad_enabled() {
    static int on = ini_int(L"fake_pad", 1);
    return on != 0;
}

void pad_set(WORD buttons, SHORT lx, SHORT ly, SHORT rx, SHORT ry, BYTE lt, BYTE rt) {
    std::lock_guard lock(g_pad_mx);
    g_pad.wButtons = buttons;
    g_pad.sThumbLX = lx;
    g_pad.sThumbLY = ly;
    g_pad.sThumbRX = rx;
    g_pad.sThumbRY = ry;
    g_pad.bLeftTrigger = lt;
    g_pad.bRightTrigger = rt;
}

void pad_apply(XINPUT_STATE* s) {
    static DWORD packet = 0;
    std::lock_guard lock(g_pad_mx);
    s->Gamepad.wButtons |= g_pad.wButtons;
    if (g_pad.sThumbLX || g_pad.sThumbLY) {
        s->Gamepad.sThumbLX = g_pad.sThumbLX;
        s->Gamepad.sThumbLY = g_pad.sThumbLY;
    }
    if (g_pad.sThumbRX || g_pad.sThumbRY) {
        s->Gamepad.sThumbRX = g_pad.sThumbRX;
        s->Gamepad.sThumbRY = g_pad.sThumbRY;
    }
    if (g_pad.bLeftTrigger) s->Gamepad.bLeftTrigger = g_pad.bLeftTrigger;
    if (g_pad.bRightTrigger) s->Gamepad.bRightTrigger = g_pad.bRightTrigger;
    s->dwPacketNumber += ++packet;
}

}  // namespace hv

using namespace hv;

extern "C" DWORD WINAPI hv_XInputGetState(DWORD index, XINPUT_STATE* state) {
    using fn_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static fn_t f = real_fn<fn_t>("XInputGetState");
    DWORD r = f ? f(index, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (index == 0 && fake_pad_enabled()) {
        if (r != ERROR_SUCCESS) {
            memset(state, 0, sizeof *state);
            r = ERROR_SUCCESS;
        }
        pad_apply(state);
    }
    return r;
}

extern "C" DWORD WINAPI hv_XInputSetState(DWORD index, XINPUT_VIBRATION* vib) {
    using fn_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    static fn_t f = real_fn<fn_t>("XInputSetState");
    DWORD r = f ? f(index, vib) : ERROR_DEVICE_NOT_CONNECTED;
    return index == 0 && fake_pad_enabled() ? ERROR_SUCCESS : r;
}

extern "C" DWORD WINAPI hv_XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps) {
    using fn_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    static fn_t f = real_fn<fn_t>("XInputGetCapabilities");
    DWORD r = f ? f(index, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (index == 0 && fake_pad_enabled() && r != ERROR_SUCCESS) {
        memset(caps, 0, sizeof *caps);
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        r = ERROR_SUCCESS;
    }
    return r;
}

extern "C" void WINAPI hv_XInputEnable(BOOL enable) {
    using fn_t = void(WINAPI*)(BOOL);
    static fn_t f = real_fn<fn_t>("XInputEnable");
    if (f) f(enable);
}

#pragma comment(linker, "/EXPORT:XInputGetState=_hv_XInputGetState@8,@2")
#pragma comment(linker, "/EXPORT:XInputSetState=_hv_XInputSetState@8,@3")
#pragma comment(linker, "/EXPORT:XInputGetCapabilities=_hv_XInputGetCapabilities@12,@4")
#pragma comment(linker, "/EXPORT:XInputEnable=_hv_XInputEnable@4,@5")
