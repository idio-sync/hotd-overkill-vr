// Direct3D 9 hooks: device creation (forced windowed), frame pacing stats, render target
// inventory, screenshots, and `frame` logs (every state change and draw of one frame, with
// vertex shader constants named from the shaders' CTAB).
#include "common.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace hv {

// IDirect3D9 / IDirect3DDevice9 vtable indices.
enum {
    kCreateDevice = 16,
    kReset = 16, kPresent = 17, kCreateTexture = 23, kCreateRenderTarget = 28, kCreateDepthStencilSurface = 29,
    kStretchRect = 34, kSetRenderTarget = 37, kSetDepthStencilSurface = 39, kClear = 43, kSetTransform = 44,
    kSetViewport = 47, kSetRenderState = 57, kSetTexture = 65, kSetScissorRect = 75, kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84, kCreateVertexShader = 91,
    kSetVertexShader = 92, kSetVertexShaderConstantF = 94, kCreatePixelShader = 106, kSetPixelShader = 107,
    kSetPixelShaderConstantF = 109,
};

static FILE* g_frame;      // non-null while logging a frame
static int g_frame_no;     // frames presented
static int g_draws;        // draws this frame
static int g_last_draws;
static IDirect3DVertexShader9* g_vs;
static IDirect3DPixelShader9* g_ps;
static float g_vsc[256][4];
struct Range { UINT start, count; };
static std::vector<Range> g_dirty;
static int g_cap_fps;
static LONGLONG g_qpf;

static void flog(const char* fmt, ...) {
    if (!g_frame) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_frame, fmt, a);
    va_end(a);
    fputc('\n', g_frame);
}

static const char* fmt_name(D3DFORMAT f) {
    static char buf[16];
    switch (f) {
        case D3DFMT_A8R8G8B8: return "ARGB8";
        case D3DFMT_X8R8G8B8: return "XRGB8";
        case D3DFMT_A16B16G16R16F: return "RGBA16F";
        case D3DFMT_A32B32G32R32F: return "RGBA32F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_G16R16F: return "RG16F";
        case D3DFMT_A2R10G10B10: return "A2RGB10";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D16: return "D16";
        case D3DFMT_UNKNOWN: return "UNKNOWN";
        default:
            if (f > 0xFFFF) {
                snprintf(buf, sizeof buf, "%.4s", reinterpret_cast<const char*>(&f));  // FOURCC (INTZ, NULL, RESZ...)
                return buf;
            }
            snprintf(buf, sizeof buf, "fmt%d", int(f));
            return buf;
    }
}

static uint64_t hash_of(void* shader) {
    auto i = shader ? shader_info(shader) : nullptr;
    return i ? i->hash : 0;
}

static std::string const_name(UINT reg) {
    auto info = g_vs ? shader_info(g_vs) : nullptr;
    if (info)
        for (auto& c : info->consts)
            if (c.set == 2 && reg >= UINT(c.index) && reg < UINT(c.index + c.count))
                return c.name + (c.count > 1 ? "[" + std::to_string(reg - c.index) + "]" : "");
    return "?";
}

static IDirect3DSurface9* g_rt0;
static D3DVIEWPORT9 g_vp;

static void log_draw(const char* kind, UINT prims) {
    if (!g_frame) return;
    D3DSURFACE_DESC d{};
    if (g_rt0) g_rt0->GetDesc(&d);
    flog("#%d %s prims=%u vs=%016llx ps=%016llx rt=%ux%u %s vp=%lu,%lu %lux%lu z=%.2f-%.2f", g_draws, kind, prims,
         hash_of(g_vs), hash_of(g_ps), d.Width, d.Height, fmt_name(d.Format), g_vp.X, g_vp.Y, g_vp.Width, g_vp.Height,
         g_vp.MinZ, g_vp.MaxZ);
    for (auto& r : g_dirty) {
        UINT n = (std::min)(r.count, 12u);
        for (UINT i = 0; i < n; ++i) {
            UINT c = r.start + i;
            if (c >= 256) break;
            flog("    c%-3u %-24s % .5g % .5g % .5g % .5g", c, const_name(c).c_str(), g_vsc[c][0], g_vsc[c][1], g_vsc[c][2], g_vsc[c][3]);
        }
        if (r.count > n) flog("    ... c%u..c%u not shown", r.start + n, r.start + r.count - 1);
    }
    g_dirty.clear();
}

// --- device hooks ---

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateSurf_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using StretchRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepth_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTransform_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetRenderState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using CreateVS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreatePS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

static Present_t o_Present;
static Reset_t o_Reset;
static CreateTexture_t o_CreateTexture;
static CreateSurf_t o_CreateRenderTarget, o_CreateDepthStencilSurface;
static StretchRect_t o_StretchRect;
static SetRenderTarget_t o_SetRenderTarget;
static SetDepth_t o_SetDepthStencilSurface;
static Clear_t o_Clear;
static SetTransform_t o_SetTransform;
static SetViewport_t o_SetViewport;
static SetRenderState_t o_SetRenderState;
static SetTexture_t o_SetTexture;
static DrawPrimitive_t o_DrawPrimitive;
static DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
static DrawPrimitiveUP_t o_DrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
static CreateVS_t o_CreateVertexShader;
static SetVS_t o_SetVertexShader;
static SetVSConstF_t o_SetVertexShaderConstantF;
static CreatePS_t o_CreatePixelShader;
static SetPS_t o_SetPixelShader;

static void log_pp(const char* what, const D3DPRESENT_PARAMETERS* pp) {
    logf("%s: %ux%u %s x%u ms=%d swap=%d hwnd=%p windowed=%d autodepth=%d %s flags=%lx refresh=%u interval=%x", what,
         pp->BackBufferWidth, pp->BackBufferHeight, fmt_name(pp->BackBufferFormat), pp->BackBufferCount,
         int(pp->MultiSampleType), int(pp->SwapEffect), pp->hDeviceWindow, pp->Windowed, pp->EnableAutoDepthStencil,
         fmt_name(pp->AutoDepthStencilFormat), pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

static void fix_pp(D3DPRESENT_PARAMETERS* pp) {
    if (ini_int(L"windowed", 1)) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (!ini_int(L"vsync", 1)) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

static void save_backbuffer(IDirect3DDevice9* dev, const std::string& name) {
    IDirect3DSurface9 *bb = nullptr, *resolved = nullptr, *sys = nullptr;
    D3DSURFACE_DESC d{};
    do {
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) break;
        bb->GetDesc(&d);
        IDirect3DSurface9* src = bb;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(o_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))) break;
            o_StretchRect(dev, bb, nullptr, resolved, nullptr, D3DTEXF_NONE);
            src = resolved;
        }
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) break;
        if (FAILED(dev->GetRenderTargetData(src, sys))) break;
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) break;
        std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4);
        for (UINT y = 0; y < d.Height; ++y) {
            auto row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
            uint8_t* out = &px[size_t(y) * d.Width * 4];
            memcpy(out, row, d.Width * 4);
            for (UINT x = 0; x < d.Width; ++x) out[x * 4 + 3] = 255;
        }
        sys->UnlockRect();
        std::wstring w = path_in((std::wstring(name.begin(), name.end()) + L".png").c_str());
        bool ok = save_png(w, d.Width, d.Height, d.Width * 4, px.data());
        logf("screenshot %s %ux%u %s", name.c_str(), d.Width, d.Height, ok ? "saved" : "FAILED");
    } while (false);
    if (sys) sys->Release();
    if (resolved) resolved->Release();
    if (bb) bb->Release();
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    static LONGLONG last_report, last_present, min_dt = LLONG_MAX, max_dt, frames_since;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_frame) {
        flog("=== Present (frame %d, %d draws)", g_frame_no, g_draws);
        fclose(g_frame);
        g_frame = nullptr;
        logf("frame log written (frame %d)", g_frame_no);
    }
    if (ini_int(L"gun_hook", 1)) install_gun_hook();  // once; after SecuROM's start-up patching
    FrameRequests req = control_tick();
    if (req.shot) save_backbuffer(dev, req.shot_name);  // the back buffer still holds this frame

    HRESULT hr = o_Present(dev, a, b, w, r);

    // Frame pacing stats, every 5 s.
    if (last_present) {
        LONGLONG dt = now.QuadPart - last_present;
        min_dt = (std::min)(min_dt, dt);
        max_dt = (std::max)(max_dt, dt);
    }
    last_present = now.QuadPart;
    ++frames_since;
    if (!last_report) last_report = now.QuadPart;
    double since = double(now.QuadPart - last_report) / g_qpf;
    if (since >= 5.0) {
        char status[200];
        snprintf(status, sizeof status, "frame %d: %.1f fps, dt %.2f-%.2f ms, %d draws last frame", g_frame_no,
                 frames_since / since, min_dt * 1000.0 / g_qpf, max_dt * 1000.0 / g_qpf, g_draws);
        logf("%s", status);
        write_status(status);
        last_report = now.QuadPart;
        frames_since = 0;
        min_dt = LLONG_MAX;
        max_dt = 0;
    }

    // Optional frame cap (cap_fps=0: off).
    if (g_cap_fps > 0) {
        static LONGLONG next;
        LONGLONG period = g_qpf / g_cap_fps;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!next || t.QuadPart - next > period) next = t.QuadPart;
        next += period;
        while (true) {
            QueryPerformanceCounter(&t);
            LONGLONG left = next - t.QuadPart;
            if (left <= 0) break;
            if (left * 1000 / g_qpf > 2) Sleep(1);
        }
    }

    ++g_frame_no;
    g_last_draws = g_draws;
    g_draws = 0;
    if (req.log_frame) {
        wchar_t name[64];
        swprintf_s(name, L"frame_%d.txt", g_frame_no);
        g_frame = _wfopen(path_in(name).c_str(), L"w");
        g_dirty.clear();
        flog("=== frame %d", g_frame_no);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    log_pp("Reset (game)", pp);
    fix_pp(pp);
    HRESULT hr = o_Reset(dev, pp);
    logf("Reset -> %08lx", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                                  D3DPOOL pool, IDirect3DTexture9** tex, HANDLE* shared) {
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, f, pool, tex, shared);
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        logf("CreateTexture %ux%u %s %s levels=%u -> %p", w, h, fmt_name(f),
             usage & D3DUSAGE_RENDERTARGET ? "RT" : "DS", levels, SUCCEEDED(hr) ? *tex : nullptr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                       DWORD q, BOOL lockable, IDirect3DSurface9** s, HANDLE* shared) {
    HRESULT hr = o_CreateRenderTarget(dev, w, h, f, ms, q, lockable, s, shared);
    logf("CreateRenderTarget %ux%u %s ms=%d -> %p", w, h, fmt_name(f), int(ms), SUCCEEDED(hr) ? *s : nullptr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                              DWORD q, BOOL discard, IDirect3DSurface9** s, HANDLE* shared) {
    HRESULT hr = o_CreateDepthStencilSurface(dev, w, h, f, ms, q, discard, s, shared);
    logf("CreateDepthStencilSurface %ux%u %s ms=%d -> %p", w, h, fmt_name(f), int(ms), SUCCEEDED(hr) ? *s : nullptr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                                const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    if (g_frame) {
        D3DSURFACE_DESC a{}, b{};
        if (src) src->GetDesc(&a);
        if (dst) dst->GetDesc(&b);
        flog("StretchRect %p %ux%u %s -> %p %ux%u %s", src, a.Width, a.Height, fmt_name(a.Format), dst, b.Width, b.Height, fmt_name(b.Format));
    }
    return o_StretchRect(dev, src, sr, dst, dr, f);
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s) {
    if (index == 0) g_rt0 = s;
    if (g_frame) {
        D3DSURFACE_DESC d{};
        if (s) s->GetDesc(&d);
        flog("SetRenderTarget %lu %p %ux%u %s", index, s, d.Width, d.Height, fmt_name(d.Format));
    }
    HRESULT hr = o_SetRenderTarget(dev, index, s);
    if (index == 0 && SUCCEEDED(hr) && s) {
        // SetRenderTarget(0) resets the viewport to the full target.
        D3DSURFACE_DESC d{};
        s->GetDesc(&d);
        g_vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (g_frame) {
        D3DSURFACE_DESC d{};
        if (s) s->GetDesc(&d);
        flog("SetDepthStencil %p %ux%u %s", s, d.Width, d.Height, fmt_name(d.Format));
    }
    return o_SetDepthStencilSurface(dev, s);
}

static HRESULT STDMETHODCALLTYPE hk_Clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    flog("Clear flags=%lx color=%08lx z=%g stencil=%lu", flags, c, z, s);
    return o_Clear(dev, n, r, flags, c, z, s);
}

static HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
    if (g_frame && m)
        flog("SetTransform %d [% .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g]", int(t),
             m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
    return o_SetTransform(dev, t, m);
}

static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (vp) g_vp = *vp;
    return o_SetViewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_frame && (s == D3DRS_ZENABLE || s == D3DRS_ZWRITEENABLE || s == D3DRS_ZFUNC || s == D3DRS_ALPHABLENDENABLE ||
                    s == D3DRS_CULLMODE || s == D3DRS_SCISSORTESTENABLE || s == D3DRS_COLORWRITEENABLE || s == D3DRS_STENCILENABLE))
        flog("RS %d = %lu", int(s), v);
    return o_SetRenderState(dev, s, v);
}

static HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) {
    if (g_frame && t && t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
        if (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
            flog("SetTexture %lu = RT %p %ux%u %s", stage, t, d.Width, d.Height, fmt_name(d.Format));
    }
    return o_SetTexture(dev, stage, t);
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    log_draw("DP", n);
    ++g_draws;
    return o_DrawPrimitive(dev, t, start, n);
}
static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT si, UINT n) {
    log_draw("DIP", n);
    ++g_draws;
    return o_DrawIndexedPrimitive(dev, t, base, minv, nv, si, n);
}
static HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    log_draw("DPUP", n);
    ++g_draws;
    return o_DrawPrimitiveUP(dev, t, n, v, stride);
}
static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minv, UINT nv, UINT n,
                                                           const void* idx, D3DFORMAT f, const void* v, UINT stride) {
    log_draw("DIPUP", n);
    ++g_draws;
    return o_DrawIndexedPrimitiveUP(dev, t, minv, nv, n, idx, f, v, stride);
}

static HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** s) {
    HRESULT hr = o_CreateVertexShader(dev, code, s);
    if (SUCCEEDED(hr) && code) shader_created(*s, code, true);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* s) {
    g_vs = s;
    return o_SetVertexShader(dev, s);
}
static HRESULT STDMETHODCALLTYPE hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (v && start < 256) {
        UINT n = (std::min)(count, 256 - start);
        memcpy(g_vsc[start], v, n * 16);
        if (g_frame) g_dirty.push_back({start, n});
    }
    return o_SetVertexShaderConstantF(dev, start, v, count);
}
static HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** s) {
    HRESULT hr = o_CreatePixelShader(dev, code, s);
    if (SUCCEEDED(hr) && code) shader_created(*s, code, false);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* s) {
    g_ps = s;
    return o_SetPixelShader(dev, s);
}

static void hook_device(IDirect3DDevice9* dev) {
#define HOOK(idx, name) patch_vtbl(dev, idx, reinterpret_cast<void*>(hk_##name), reinterpret_cast<void**>(&o_##name))
    HOOK(kReset, Reset);
    HOOK(kPresent, Present);
    HOOK(kCreateTexture, CreateTexture);
    HOOK(kCreateRenderTarget, CreateRenderTarget);
    HOOK(kCreateDepthStencilSurface, CreateDepthStencilSurface);
    HOOK(kStretchRect, StretchRect);
    HOOK(kSetRenderTarget, SetRenderTarget);
    HOOK(kSetDepthStencilSurface, SetDepthStencilSurface);
    HOOK(kClear, Clear);
    HOOK(kSetTransform, SetTransform);
    HOOK(kSetViewport, SetViewport);
    HOOK(kSetRenderState, SetRenderState);
    HOOK(kSetTexture, SetTexture);
    HOOK(kDrawPrimitive, DrawPrimitive);
    HOOK(kDrawIndexedPrimitive, DrawIndexedPrimitive);
    HOOK(kDrawPrimitiveUP, DrawPrimitiveUP);
    HOOK(kDrawIndexedPrimitiveUP, DrawIndexedPrimitiveUP);
    HOOK(kCreateVertexShader, CreateVertexShader);
    HOOK(kSetVertexShader, SetVertexShader);
    HOOK(kSetVertexShaderConstantF, SetVertexShaderConstantF);
    HOOK(kCreatePixelShader, CreatePixelShader);
    HOOK(kSetPixelShader, SetPixelShader);
#undef HOOK
    logf("device %p hooked", dev);
}

// --- IDirect3D9 ---

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t o_CreateDevice;

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev) {
    log_pp("CreateDevice (game)", pp);
    logf("  adapter=%u type=%d focus=%p behavior=%lx", adapter, int(type), focus, behavior);
    fix_pp(pp);
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, behavior, pp, dev);
    logf("CreateDevice -> %08lx", hr);
    if (SUCCEEDED(hr)) hook_device(*dev);
    return hr;
}

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
static Direct3DCreate9_t o_Direct3DCreate9;

static IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = o_Direct3DCreate9(sdk);
    logf("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (d3d) patch_vtbl(d3d, kCreateDevice, reinterpret_cast<void*>(hk_CreateDevice), reinterpret_cast<void**>(&o_CreateDevice));
    return d3d;
}

void install_d3d_hooks() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_cap_fps = ini_int(L"cap_fps", 0);
    patch_import(GetModuleHandleW(nullptr), "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(hk_Direct3DCreate9),
                 reinterpret_cast<void**>(&o_Direct3DCreate9));
}

}  // namespace hv
