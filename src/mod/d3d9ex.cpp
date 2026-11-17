// The game's device as Direct3D 9Ex (design: "Rendering - Device"), so that our textures can be
// shared with a D3D11 device for OpenVR. Plain D3D9 refuses shared handles (the game's own device
// answered D3DERR_INVALIDCALL in the share test, 2026-11-17), so sim/openvr modes create the
// device with Direct3DCreate9Ex/CreateDeviceEx; mode=off leaves it a plain D3D9 device.
//
// D3D9Ex has no D3DPOOL_MANAGED, which the game uses for nearly all its textures (221 of 229 at
// the loadout screen) and some buffers. Emulation:
//  - vertex/index buffers: D3DPOOL_DEFAULT (lockable as before);
//  - textures and cube textures: D3DPOOL_DEFAULT, which cannot be locked. Locks on them (texture
//    LockRect, cube LockRect, and LockRect on their level surfaces) go to a system-memory staging
//    surface of the level's size; on unlock the locked rectangle is copied in with UpdateSurface
//    and the staging surface is freed. A lock therefore does not see the texture's old contents
//    (locks are counted and READONLY ones logged; the game only fills textures at load time).
//  - Present's S_PRESENT_OCCLUDED (a minimized window) is reported to the game as S_OK.
// Volume textures are counted and logged; none were seen.
#include "common.h"

#include <map>
#include <mutex>
#include <set>

namespace hv {

static bool g_ex;           // the device is a 9Ex device
static IDirect3D9Ex* g_d3d_ex;
static UINT g_adapter;
static std::mutex g_mx;
static std::set<void*> g_converted;  // textures and cube textures created DEFAULT for MANAGED

struct Staging {
    IDirect3DSurface9* sys;
    RECT rect;  // the locked rect in the level
    DWORD flags;
};
static std::map<std::pair<void*, UINT>, Staging> g_staging;  // (resource, face << 8 | level)
static long g_staged_locks, g_readonly_locks, g_failed_locks;

bool ex_enabled() { return g_ex; }

bool ex_adapter_luid(LUID* out) {
    if (!g_d3d_ex) return false;
    return SUCCEEDED(g_d3d_ex->GetAdapterLUID(g_adapter, out));
}

IDirect3D9* ex_create_d3d(UINT sdk) {
    using Create9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    HMODULE m = GetModuleHandleW(L"d3d9.dll");
    auto create = m ? reinterpret_cast<Create9Ex_t>(GetProcAddress(m, "Direct3DCreate9Ex")) : nullptr;
    IDirect3D9Ex* d3d = nullptr;
    HRESULT hr = create ? create(sdk, &d3d) : E_NOINTERFACE;
    logf("Direct3DCreate9Ex(%u) -> %08lx %p", sdk, hr, d3d);
    if (FAILED(hr) || !d3d) return nullptr;
    g_d3d_ex = d3d;
    return d3d;  // IDirect3D9Ex starts with IDirect3D9's methods
}

HRESULT ex_create_device(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    IDirect3DDevice9Ex* dev = nullptr;
    pp->Windowed = TRUE;  // VR modes are always windowed (and 9Ex fullscreen would need a display mode)
    pp->FullScreen_RefreshRateInHz = 0;
    HRESULT hr = g_d3d_ex->CreateDeviceEx(adapter, type, focus, behavior, pp, nullptr, &dev);
    logf("CreateDeviceEx -> %08lx %p", hr, dev);
    if (SUCCEEDED(hr)) {
        g_ex = true;
        g_adapter = adapter;
        *out = dev;
    }
    return hr;
}

D3DPOOL ex_pool(D3DPOOL pool, bool* converted) {
    *converted = g_ex && pool == D3DPOOL_MANAGED;
    return *converted ? D3DPOOL_DEFAULT : pool;
}

// --- locks on converted textures ---

using TexLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TexUnlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
using CubeLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using CubeUnlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
using SurfLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfUnlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
static TexLock_t o_TexLock;
static TexUnlock_t o_TexUnlock;
static CubeLock_t o_CubeLock;
static CubeUnlock_t o_CubeUnlock;
static SurfLock_t o_SurfLock;
static SurfUnlock_t o_SurfUnlock;

static bool converted(void* res) {
    std::lock_guard lock(g_mx);
    return g_converted.count(res) != 0;
}

// Locks `level_surface` (a DEFAULT-pool surface) through a staging copy.
static HRESULT stage_lock(void* res, UINT key, IDirect3DSurface9* level_surface, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
    D3DSURFACE_DESC d{};
    level_surface->GetDesc(&d);
    IDirect3DDevice9* dev = nullptr;
    level_surface->GetDevice(&dev);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = dev ? dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr) : E_FAIL;
    if (dev) dev->Release();
    if (FAILED(hr)) {
        if (g_failed_locks++ < 10) logf("d3d9ex: staging %ux%u fmt %d for a lock failed (%08lx)", d.Width, d.Height, int(d.Format), hr);
        return hr;
    }
    const DWORD sys_flags = flags & D3DLOCK_READONLY;
    hr = sys->LockRect(lr, r, sys_flags);
    if (FAILED(hr)) {
        sys->Release();
        return hr;
    }
    RECT rect = r ? *r : RECT{0, 0, LONG(d.Width), LONG(d.Height)};
    {
        std::lock_guard lock(g_mx);
        g_staging[{res, key}] = {sys, rect, flags};
    }
    ++g_staged_locks;
    if (flags & D3DLOCK_READONLY) {
        if (g_readonly_locks++ < 10) logf("d3d9ex: READONLY lock of a converted texture (contents not read back)");
    }
    return S_OK;
}

static bool stage_unlock(void* res, UINT key, IDirect3DSurface9* level_surface, HRESULT* out) {
    Staging s;
    {
        std::lock_guard lock(g_mx);
        auto it = g_staging.find({res, key});
        if (it == g_staging.end()) return false;
        s = it->second;
        g_staging.erase(it);
    }
    s.sys->UnlockRect();
    *out = S_OK;
    if (!(s.flags & D3DLOCK_READONLY) && level_surface) {
        IDirect3DDevice9* dev = nullptr;
        level_surface->GetDevice(&dev);
        POINT pt = {s.rect.left, s.rect.top};
        *out = dev ? dev->UpdateSurface(s.sys, &s.rect, level_surface, &pt) : E_FAIL;
        if (dev) dev->Release();
        static int errors;
        if (FAILED(*out) && errors++ < 10) logf("d3d9ex: UpdateSurface after a staged lock -> %08lx", *out);
    }
    s.sys->Release();
    return true;
}

static HRESULT STDMETHODCALLTYPE hk_TexLock(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
    if (!converted(t)) return o_TexLock(t, level, lr, r, flags);
    IDirect3DSurface9* s = nullptr;
    if (FAILED(t->GetSurfaceLevel(level, &s))) return D3DERR_INVALIDCALL;
    HRESULT hr = stage_lock(t, level, s, lr, r, flags);
    s->Release();
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_TexUnlock(IDirect3DTexture9* t, UINT level) {
    if (!converted(t)) return o_TexUnlock(t, level);
    IDirect3DSurface9* s = nullptr;
    t->GetSurfaceLevel(level, &s);
    HRESULT hr = D3DERR_INVALIDCALL;
    stage_unlock(t, level, s, &hr);
    if (s) s->Release();
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CubeLock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
    if (!converted(t)) return o_CubeLock(t, f, level, lr, r, flags);
    IDirect3DSurface9* s = nullptr;
    if (FAILED(t->GetCubeMapSurface(f, level, &s))) return D3DERR_INVALIDCALL;
    HRESULT hr = stage_lock(t, UINT(f) << 8 | level, s, lr, r, flags);
    s->Release();
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CubeUnlock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT level) {
    if (!converted(t)) return o_CubeUnlock(t, f, level);
    IDirect3DSurface9* s = nullptr;
    t->GetCubeMapSurface(f, level, &s);
    HRESULT hr = D3DERR_INVALIDCALL;
    stage_unlock(t, UINT(f) << 8 | level, s, &hr);
    if (s) s->Release();
    return hr;
}
// Level surfaces of converted textures (GetSurfaceLevel + LockRect): a lock that the runtime
// refuses on a plain DEFAULT-pool surface is staged.
static HRESULT STDMETHODCALLTYPE hk_SurfLock(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) {
    HRESULT hr = o_SurfLock(s, lr, r, flags);
    if (SUCCEEDED(hr)) return hr;
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    if (d.Pool != D3DPOOL_DEFAULT || (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC))) return hr;
    return stage_lock(s, 0, s, lr, r, flags);
}
static HRESULT STDMETHODCALLTYPE hk_SurfUnlock(IDirect3DSurface9* s) {
    HRESULT hr;
    if (stage_unlock(s, 0, s, &hr)) return hr;
    return o_SurfUnlock(s);
}

void ex_track_texture(IDirect3DTexture9* t) {
    {
        std::lock_guard lock(g_mx);
        g_converted.insert(t);
    }
    static bool hooked;
    if (!hooked) {
        hooked = true;
        patch_vtbl(t, 19, reinterpret_cast<void*>(hk_TexLock), reinterpret_cast<void**>(&o_TexLock));
        patch_vtbl(t, 20, reinterpret_cast<void*>(hk_TexUnlock), reinterpret_cast<void**>(&o_TexUnlock));
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(t->GetSurfaceLevel(0, &s)) && s) {
            patch_vtbl(s, 13, reinterpret_cast<void*>(hk_SurfLock), reinterpret_cast<void**>(&o_SurfLock));
            patch_vtbl(s, 14, reinterpret_cast<void*>(hk_SurfUnlock), reinterpret_cast<void**>(&o_SurfUnlock));
            s->Release();
        }
        logf("d3d9ex: texture lock hooks installed");
    }
}

void ex_track_cube(IDirect3DCubeTexture9* t) {
    {
        std::lock_guard lock(g_mx);
        g_converted.insert(t);
    }
    static bool hooked;
    if (!hooked) {
        hooked = true;
        patch_vtbl(t, 19, reinterpret_cast<void*>(hk_CubeLock), reinterpret_cast<void**>(&o_CubeLock));
        patch_vtbl(t, 20, reinterpret_cast<void*>(hk_CubeUnlock), reinterpret_cast<void**>(&o_CubeUnlock));
        logf("d3d9ex: cube texture lock hooks installed");
    }
}

// A converted texture's address can be reused by a later, unrelated texture: forget it when its
// last reference goes. (Called from the device's texture creation hook for every new texture.)
void ex_forget(void* res) {
    std::lock_guard lock(g_mx);
    g_converted.erase(res);
}

const char* ex_status() {
    static char buf[160];
    if (!g_ex) return "";
    size_t n;
    {
        std::lock_guard lock(g_mx);
        n = g_converted.size();
    }
    snprintf(buf, sizeof buf, ", d3d9ex: %zu converted textures, %ld staged locks", n, g_staged_locks);
    return buf;
}

}  // namespace hv
