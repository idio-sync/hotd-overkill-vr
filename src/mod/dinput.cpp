// DirectInput 8 hooks: the game reads mouse buttons (and probably the keyboard) through
// DInput. Devices are told apart by the GUID they were created with. In minimized mode the
// real devices are ignored (the user is working in other windows) and the scripted state
// from the command file is reported instead; both immediate (GetDeviceState) and buffered
// (GetDeviceData) reads are covered.
#define DIRECTINPUT_VERSION 0x0800
#include "common.h"

#include <dinput.h>

#include <deque>
#include <mutex>
#include <unordered_map>

namespace hv {

enum class Kind { mouse, keyboard, other };
struct DevInfo {
    Kind kind = Kind::other;
    DWORD data_size = 0;
    bool logged_state = false, logged_data = false;
};

static std::mutex g_mx;
static std::unordered_map<void*, DevInfo> g_devs;
static uint8_t g_keys[256];     // DIK_ codes, 0x80 = down
static uint8_t g_buttons[8];    // mouse buttons
static LONG g_dx, g_dy, g_dz;   // pending relative mouse motion and wheel
static std::deque<DIDEVICEOBJECTDATA> g_key_events, g_mouse_events;
static DWORD g_seq;

static bool ignore_real() {
    static int on = ini_int(L"minimized", 1);
    return on != 0;
}

void di_key(UINT vk, bool down) {
    UINT dik = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) dik |= 0x80;  // extended keys
    std::lock_guard lock(g_mx);
    g_keys[dik & 0xFF] = down ? 0x80 : 0;
    DIDEVICEOBJECTDATA e{};
    e.dwOfs = dik & 0xFF;
    e.dwData = down ? 0x80 : 0;
    e.dwTimeStamp = GetTickCount();
    e.dwSequence = ++g_seq;
    g_key_events.push_back(e);
}

void di_mouse_button(int button, bool down) {
    std::lock_guard lock(g_mx);
    g_buttons[button & 7] = down ? 0x80 : 0;
    DIDEVICEOBJECTDATA e{};
    e.dwOfs = DWORD(DIMOFS_BUTTON0 + button);
    e.dwData = down ? 0x80 : 0;
    e.dwTimeStamp = GetTickCount();
    e.dwSequence = ++g_seq;
    g_mouse_events.push_back(e);
}

void di_mouse_move(LONG dx, LONG dy) {
    std::lock_guard lock(g_mx);
    g_dx += dx;
    g_dy += dy;
}

void di_mouse_wheel(LONG dz) {
    std::lock_guard lock(g_mx);
    g_dz += dz;
    DIDEVICEOBJECTDATA e{};
    e.dwOfs = DIMOFS_Z;
    e.dwData = DWORD(dz);
    e.dwTimeStamp = GetTickCount();
    e.dwSequence = ++g_seq;
    g_mouse_events.push_back(e);
}

// The real mouse is ignored in the minimized test window, and in VR modes once the controllers
// drive the pointer (vrinput.cpp); the real keyboard only in the minimized test window.
static bool ignore_real_dev(Kind k) { return ignore_real() || (k == Kind::mouse && input_owns_pointer()); }

// IDirectInputDevice8A
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using SetDataFormat_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, LPCDIDATAFORMAT);
using SetCoop_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, HWND, DWORD);
using Acquire_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*);
static GetDeviceState_t o_GetDeviceState;
static GetDeviceData_t o_GetDeviceData;
static SetDataFormat_t o_SetDataFormat;
static SetCoop_t o_SetCooperativeLevel;
static Acquire_t o_Acquire;

static DevInfo* info_of(void* dev) {
    auto it = g_devs.find(dev);
    return it == g_devs.end() ? nullptr : &it->second;
}

static HRESULT STDMETHODCALLTYPE hk_Acquire(IDirectInputDevice8A* dev) {
    HRESULT hr = o_Acquire(dev);
    return ignore_real() && FAILED(hr) ? DI_OK : hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetCooperativeLevel(IDirectInputDevice8A* dev, HWND hwnd, DWORD flags) {
    logf("DInput SetCooperativeLevel %p flags=%lx", dev, flags);
    if (ignore_real()) flags = DISCL_BACKGROUND | DISCL_NONEXCLUSIVE;  // never grab the user's mouse/keyboard
    return o_SetCooperativeLevel(dev, hwnd, flags);
}

static HRESULT STDMETHODCALLTYPE hk_SetDataFormat(IDirectInputDevice8A* dev, LPCDIDATAFORMAT fmt) {
    {
        std::lock_guard lock(g_mx);
        if (auto i = info_of(dev)) i->data_size = fmt ? fmt->dwDataSize : 0;
    }
    logf("DInput SetDataFormat %p size=%lu objects=%lu", dev, fmt ? fmt->dwDataSize : 0, fmt ? fmt->dwNumObjs : 0);
    return o_SetDataFormat(dev, fmt);
}

static HRESULT STDMETHODCALLTYPE hk_GetDeviceState(IDirectInputDevice8A* dev, DWORD size, LPVOID data) {
    HRESULT hr = o_GetDeviceState(dev, size, data);
    std::lock_guard lock(g_mx);
    DevInfo* i = info_of(dev);
    if (!i || i->kind == Kind::other) return hr;
    if (!i->logged_state) {
        i->logged_state = true;
        logf("DInput GetDeviceState %s size=%lu -> %08lx", i->kind == Kind::mouse ? "mouse" : "keyboard", size, hr);
    }
    if (ignore_real_dev(i->kind) || FAILED(hr)) {
        memset(data, 0, size);
        hr = DI_OK;
    }
    if (i->kind == Kind::keyboard) {
        auto k = static_cast<uint8_t*>(data);
        for (DWORD n = 0; n < size && n < 256; ++n) k[n] |= g_keys[n];
    } else if (size >= sizeof(DIMOUSESTATE)) {
        auto m = static_cast<DIMOUSESTATE*>(data);
        m->lX += g_dx;
        m->lY += g_dy;
        m->lZ += g_dz;
        g_dx = g_dy = g_dz = 0;
        for (DWORD b = 0; b < 4; ++b) m->rgbButtons[b] |= g_buttons[b];
        if (size >= sizeof(DIMOUSESTATE2))
            for (DWORD b = 4; b < 8; ++b) static_cast<DIMOUSESTATE2*>(data)->rgbButtons[b] |= g_buttons[b];
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDeviceData(IDirectInputDevice8A* dev, DWORD obj_size, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                                  DWORD flags) {
    std::lock_guard lock(g_mx);
    DevInfo* i = info_of(dev);
    if (!i || i->kind == Kind::other) return o_GetDeviceData(dev, obj_size, data, count, flags);
    DWORD want = count ? *count : 0;
    HRESULT hr;
    if (ignore_real_dev(i->kind)) {
        // Drain the real buffer so it never overflows, and report nothing from it.
        DWORD n = INFINITE;
        o_GetDeviceData(dev, obj_size, nullptr, &n, 0);
        hr = DI_OK;
        if (count) *count = 0;
    } else {
        hr = o_GetDeviceData(dev, obj_size, data, count, flags);
        if (FAILED(hr)) {
            hr = DI_OK;
            if (count) *count = 0;
        }
    }
    if (!i->logged_data) {
        i->logged_data = true;
        logf("DInput GetDeviceData %s obj_size=%lu want=%lu flags=%lx", i->kind == Kind::mouse ? "mouse" : "keyboard", obj_size, want, flags);
    }
    auto& q = i->kind == Kind::mouse ? g_mouse_events : g_key_events;
    if (count && data && !(flags & DIGDD_PEEK)) {
        DWORD got = *count;
        while (got < want && !q.empty()) {
            memcpy(reinterpret_cast<uint8_t*>(data) + size_t(got) * obj_size, &q.front(), (std::min)(DWORD(sizeof(DIDEVICEOBJECTDATA)), obj_size));
            q.pop_front();
            ++got;
        }
        *count = got;
    }
    return hr;
}

// IDirectInput8A::CreateDevice
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
static CreateDevice_t o_CreateDevice;

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN outer) {
    HRESULT hr = o_CreateDevice(di, guid, dev, outer);
    if (FAILED(hr) || !dev || !*dev) return hr;
    Kind k = IsEqualGUID(guid, GUID_SysMouse) ? Kind::mouse : IsEqualGUID(guid, GUID_SysKeyboard) ? Kind::keyboard : Kind::other;
    logf("DInput CreateDevice %s -> %p", k == Kind::mouse ? "mouse" : k == Kind::keyboard ? "keyboard" : "other", *dev);
    {
        std::lock_guard lock(g_mx);
        g_devs[*dev].kind = k;
    }
    patch_vtbl(*dev, 7, reinterpret_cast<void*>(hk_Acquire), reinterpret_cast<void**>(&o_Acquire));
    patch_vtbl(*dev, 9, reinterpret_cast<void*>(hk_GetDeviceState), reinterpret_cast<void**>(&o_GetDeviceState));
    patch_vtbl(*dev, 10, reinterpret_cast<void*>(hk_GetDeviceData), reinterpret_cast<void**>(&o_GetDeviceData));
    patch_vtbl(*dev, 11, reinterpret_cast<void*>(hk_SetDataFormat), reinterpret_cast<void**>(&o_SetDataFormat));
    patch_vtbl(*dev, 13, reinterpret_cast<void*>(hk_SetCooperativeLevel), reinterpret_cast<void**>(&o_SetCooperativeLevel));
    return hr;
}

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t o_DirectInput8Create;

static HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = o_DirectInput8Create(inst, ver, iid, out, outer);
    bool ansi = IsEqualIID(iid, IID_IDirectInput8A);
    logf("DirectInput8Create ver=%lx %s -> %08lx", ver, ansi ? "A" : "W/other", hr);
    if (SUCCEEDED(hr) && ansi && out && *out)
        patch_vtbl(*out, 3, reinterpret_cast<void*>(hk_CreateDevice), reinterpret_cast<void**>(&o_CreateDevice));
    return hr;
}

void install_dinput_hooks() {
    patch_import(GetModuleHandleW(nullptr), "DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(hk_DirectInput8Create),
                 reinterpret_cast<void**>(&o_DirectInput8Create));
}

}  // namespace hv
