// Direct3D 9 hooks: device creation (forced windowed), frame pacing stats, render target
// inventory, screenshots, and `frame` logs (every state change and draw of one frame, with
// vertex shader constants named from the shaders' CTAB). In sim/openvr mode the hooks also
// feed stereo.cpp: double-wide screen-sized targets, scaled viewports/rects, per-eye draws, the
// HUD redirection; the device is created as 9Ex (d3d9ex.cpp) for the D3D11 hand-off.
#include "common.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace hv {

// IDirect3D9 / IDirect3DDevice9 vtable indices.
enum {
    kCreateDevice = 16,
    kReset = 16, kPresent = 17, kCreateTexture = 23, kCreateVolumeTexture = 24, kCreateCubeTexture = 25, kCreateVertexBuffer = 26, kCreateIndexBuffer = 27, kCreateRenderTarget = 28, kCreateDepthStencilSurface = 29,
    kStretchRect = 34, kSetRenderTarget = 37, kSetDepthStencilSurface = 39, kClear = 43, kSetTransform = 44,
    kSetViewport = 47, kGetViewport = 48, kSetRenderState = 57, kSetTexture = 65, kSetScissorRect = 75,
    kGetScissorRect = 76, kDrawPrimitive = 81, kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83,
    kDrawIndexedPrimitiveUP = 84, kCreateVertexShader = 91, kSetVertexShader = 92, kSetVertexShaderConstantF = 94,
    kCreatePixelShader = 106, kSetPixelShader = 107, kSetPixelShaderConstantF = 109,
};

static FILE* g_frame;      // non-null while logging a frame
static bool g_frame_all;   // log every uploaded register
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

static void log_pools();
static void count_pool(int kind, D3DPOOL pool);

static void flog(const char* fmt, ...) {
    if (!g_frame) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_frame, fmt, a);
    va_end(a);
    fputc('\n', g_frame);
}

void frame_note(const char* text) { flog("%s", text); }
bool frame_logging() { return g_frame != nullptr; }
const float* vs_shadow(UINT reg) { return g_vsc[reg < 256 ? reg : 255]; }
IDirect3DVertexShader9* current_vs() { return g_vs; }
IDirect3DPixelShader9* current_ps() { return g_ps; }

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

static std::string const_name(UINT reg, bool pixel = false) {
    void* sh = pixel ? static_cast<void*>(g_ps) : static_cast<void*>(g_vs);
    auto info = sh ? shader_info(sh) : nullptr;
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
        UINT n = g_frame_all ? r.count : (std::min)(r.count, 12u);
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
using GetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DVIEWPORT9*);
using SetRenderState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetScissorRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using GetScissorRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, RECT*);
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
static GetViewport_t o_GetViewport;
static SetRenderState_t o_SetRenderState;
static SetTexture_t o_SetTexture;
static SetScissorRect_t o_SetScissorRect;
static GetScissorRect_t o_GetScissorRect;
static DrawPrimitive_t o_DrawPrimitive;
static DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
static DrawPrimitiveUP_t o_DrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
static CreateVS_t o_CreateVertexShader;
static SetVS_t o_SetVertexShader;
static SetVSConstF_t o_SetVertexShaderConstantF;
static CreatePS_t o_CreatePixelShader;
static SetPS_t o_SetPixelShader;

HRESULT raw_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    return o_SetVertexShaderConstantF(dev, start, v, count);
}
HRESULT raw_set_viewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) { return o_SetViewport(dev, vp); }
HRESULT raw_set_scissor(IDirect3DDevice9* dev, const RECT* r) { return o_SetScissorRect(dev, r); }
HRESULT raw_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s) { return o_SetRenderTarget(dev, index, s); }
HRESULT raw_set_depth(IDirect3DDevice9* dev, IDirect3DSurface9* s) { return o_SetDepthStencilSurface(dev, s); }
HRESULT raw_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) { return o_SetTexture(dev, stage, t); }
HRESULT raw_set_vs(IDirect3DDevice9* dev, IDirect3DVertexShader9* v) { return o_SetVertexShader(dev, v); }
HRESULT raw_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* p) { return o_SetPixelShader(dev, p); }
HRESULT raw_set_rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE st, DWORD v) { return o_SetRenderState(dev, st, v); }
HRESULT raw_dpup(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    return o_DrawPrimitiveUP(dev, t, n, v, stride);
}
HRESULT raw_clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st) {
    return o_Clear(dev, n, r, flags, c, z, st);
}
HRESULT raw_stretch(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                    D3DTEXTUREFILTERTYPE f) {
    return o_StretchRect(dev, src, sr, dst, dr, f);
}
HRESULT raw_create_texture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                           IDirect3DTexture9** tex, HANDLE* shared) {
    return o_CreateTexture(dev, w, h, levels, usage, f, pool, tex, shared);
}
HRESULT raw_create_depth(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface9** out) {
    return o_CreateDepthStencilSurface(dev, w, h, f, D3DMULTISAMPLE_NONE, 0, FALSE, out, nullptr);
}
HRESULT raw_create_rt(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface9** out) {
    return o_CreateRenderTarget(dev, w, h, f, D3DMULTISAMPLE_NONE, 0, FALSE, out, nullptr);
}

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

// PNG of a render target surface (resolved first if multisampled).
static void save_surface(IDirect3DDevice9* dev, IDirect3DSurface9* src_rt, const std::string& name, bool alpha = false) {
    IDirect3DSurface9 *resolved = nullptr, *sys = nullptr;
    D3DSURFACE_DESC d{};
    do {
        if (!src_rt) break;
        src_rt->GetDesc(&d);
        if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) {
            logf("screenshot %s: format %s not supported", name.c_str(), fmt_name(d.Format));
            break;
        }
        IDirect3DSurface9* src = src_rt;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(o_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))) break;
            o_StretchRect(dev, src_rt, nullptr, resolved, nullptr, D3DTEXF_NONE);
            src = resolved;
        }
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) break;
        if (FAILED(dev->GetRenderTargetData(src, sys))) break;
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) break;
        std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4), alpha_of(size_t(d.Width) * d.Height);
        for (UINT y = 0; y < d.Height; ++y) {
            auto row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
            uint8_t* out = &px[size_t(y) * d.Width * 4];
            memcpy(out, row, d.Width * 4);
            for (UINT x = 0; x < d.Width; ++x) {
                alpha_of[size_t(y) * d.Width + x] = out[x * 4 + 3];
                out[x * 4 + 3] = 255;
            }
        }
        sys->UnlockRect();
        if (alpha) {  // NAME_a.png: the alpha channel as grey
            std::vector<uint8_t> a(px.size());
            for (size_t i = 0; i < px.size(); i += 4) {
                a[i] = a[i + 1] = a[i + 2] = alpha_of[i / 4];
                a[i + 3] = 255;
            }
            std::wstring wa = path_in((std::wstring(name.begin(), name.end()) + L"_a.png").c_str());
            save_png(wa, d.Width, d.Height, d.Width * 4, a.data());
        }
        std::wstring w = path_in((std::wstring(name.begin(), name.end()) + L".png").c_str());
        bool ok = save_png(w, d.Width, d.Height, d.Width * 4, px.data());
        logf("screenshot %s %ux%u %s", name.c_str(), d.Width, d.Height, ok ? "saved" : "FAILED");
    } while (false);
    if (sys) sys->Release();
    if (resolved) resolved->Release();
}

static void save_backbuffer(IDirect3DDevice9* dev, const std::string& name) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        save_surface(dev, bb, name);
        bb->Release();
    }
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
    stereo_present(dev, req);  // VR: hand-off, desktop mirror, submit
    if (req.shot) save_backbuffer(dev, req.shot_name);  // the back buffer still holds this frame
    if (req.scene_shot) {
        if (stereo_scene_surface()) save_surface(dev, stereo_scene_surface(), req.scene_name);
        else logf("vr scene: no stereo scene target drawn yet");
    }
    if (req.check) stereo_request_check();
    if (req.dump_targets) {
        int i = 0;
        for (IDirect3DSurface9* s : stereo_targets()) {
            D3DSURFACE_DESC d{};
            s->GetDesc(&d);
            if (d.Format != D3DFMT_A8R8G8B8 || d.Usage & D3DUSAGE_DEPTHSTENCIL) continue;
            save_surface(dev, s, "rt" + std::to_string(i++) + "_" + std::to_string(d.Width) + "x" + std::to_string(d.Height), true);
        }
    }
    stereo_frame_end();  // decides the next frame's mode
    engine_frame(stereo_frame_active());

    HRESULT hr = o_Present(dev, a, b, w, r);
    if (hr == S_PRESENT_OCCLUDED || hr == S_PRESENT_MODE_CHANGED) hr = D3D_OK;  // 9Ex success codes the game doesn't know
    if (g_vr.mode != VrMode::off) vrsys_wait_poses();
    static HRESULT last_hr = S_OK;
    if (hr != last_hr) {
        logf("Present -> %08lx (frame %d, %d draws)", hr, g_frame_no, g_draws);
        last_hr = hr;
    }

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
        char status[300];
        snprintf(status, sizeof status, "frame %d: %.1f fps, dt %.2f-%.2f ms, %d draws last frame%s", g_frame_no,
                 frames_since / since, min_dt * 1000.0 / g_qpf, max_dt * 1000.0 / g_qpf, g_draws, stereo_status());
        logf("%s", status);
        write_status(status);
        static int reports;
        if (++reports % 12 == 1) log_pools();
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
        g_frame_all = req.log_frame_all;
        g_dirty.clear();
        flog("=== frame %d", g_frame_no);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    log_pp("Reset (game)", pp);
    fix_pp(pp);
    if (ex_enabled()) pp->Windowed = TRUE;
    stereo_reset();
    stereo_set_backbuffer(pp->BackBufferWidth, pp->BackBufferHeight);
    HRESULT hr = o_Reset(dev, pp);
    logf("Reset -> %08lx", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                                  D3DPOOL pool, IDirect3DTexture9** tex, HANDLE* shared) {
    count_pool(0, pool);
    UINT rw = w, rh = h;
    bool resize = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
                  stereo_target_size(w, h, (usage & D3DUSAGE_DEPTHSTENCIL) != 0, &rw, &rh);
    bool converted;
    pool = ex_pool(pool, &converted);
    HRESULT hr = o_CreateTexture(dev, rw, rh, levels, usage, f, pool, tex, shared);
    if (SUCCEEDED(hr) && ex_enabled()) {
        ex_forget(*tex);
        if (converted) ex_track_texture(*tex);
    }
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        logf("CreateTexture %ux%u %s %s levels=%u -> %p", w, h, fmt_name(f),
             usage & D3DUSAGE_RENDERTARGET ? "RT" : "DS", levels, SUCCEEDED(hr) ? *tex : nullptr);
    if (resize && SUCCEEDED(hr)) stereo_track_texture(*tex, w, h);
    return hr;
}

// Resource pools the game uses (D3D9Ex has no D3DPOOL_MANAGED): counts per pool, logged.
static long g_pool_count[4][4];  // [kind: tex, cube, vb, ib][pool]
static void count_pool(int kind, D3DPOOL pool) {
    if (unsigned(pool) < 4) ++g_pool_count[kind][pool];
}
static void log_pools() {
    static const char* kinds[] = {"textures", "cube textures", "vertex buffers", "index buffers"};
    for (int k = 0; k < 4; ++k)
        logf("pools: %s default %ld managed %ld systemmem %ld scratch %ld", kinds[k], g_pool_count[k][0], g_pool_count[k][1],
             g_pool_count[k][2], g_pool_count[k][3]);
}
using CreateCube_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
using CreateVB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateIB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
static CreateCube_t o_CreateCubeTexture;
static CreateVB_t o_CreateVertexBuffer;
static CreateIB_t o_CreateIndexBuffer;
static HRESULT STDMETHODCALLTYPE hk_CreateCubeTexture(IDirect3DDevice9* dev, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
                                                      IDirect3DCubeTexture9** t, HANDLE* s) {
    count_pool(1, p);
    bool converted;
    p = ex_pool(p, &converted);
    HRESULT hr = o_CreateCubeTexture(dev, e, l, u, f, p, t, s);
    if (SUCCEEDED(hr) && ex_enabled()) {
        ex_forget(*t);
        if (converted) ex_track_cube(*t);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateVertexBuffer(IDirect3DDevice9* dev, UINT n, DWORD u, DWORD fvf, D3DPOOL p,
                                                       IDirect3DVertexBuffer9** b, HANDLE* s) {
    count_pool(2, p);
    bool converted;
    return o_CreateVertexBuffer(dev, n, u, fvf, ex_pool(p, &converted), b, s);
}
static HRESULT STDMETHODCALLTYPE hk_CreateIndexBuffer(IDirect3DDevice9* dev, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p,
                                                      IDirect3DIndexBuffer9** b, HANDLE* s) {
    count_pool(3, p);
    bool converted;
    return o_CreateIndexBuffer(dev, n, u, f, ex_pool(p, &converted), b, s);
}

using CreateVolume_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DVolumeTexture9**, HANDLE*);
static CreateVolume_t o_CreateVolumeTexture;
static HRESULT STDMETHODCALLTYPE hk_CreateVolumeTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f,
                                                        D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* s) {
    bool converted;
    p = ex_pool(p, &converted);
    if (converted) logf("d3d9ex: a MANAGED volume texture %ux%ux%u became DEFAULT; its locks are not emulated", w, h, d);
    return o_CreateVolumeTexture(dev, w, h, d, l, u, f, p, t, s);
}

static HRESULT STDMETHODCALLTYPE hk_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                       DWORD q, BOOL lockable, IDirect3DSurface9** s, HANDLE* shared) {
    UINT rw = w, rh = h;
    bool resize = stereo_target_size(w, h, false, &rw, &rh);
    HRESULT hr = o_CreateRenderTarget(dev, rw, rh, f, ms, q, lockable, s, shared);
    logf("CreateRenderTarget %ux%u %s ms=%d -> %p", w, h, fmt_name(f), int(ms), SUCCEEDED(hr) ? *s : nullptr);
    if (resize && SUCCEEDED(hr)) stereo_track_surface(*s, w, h);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                                              DWORD q, BOOL discard, IDirect3DSurface9** s, HANDLE* shared) {
    UINT rw = w, rh = h;
    bool resize = stereo_target_size(w, h, true, &rw, &rh);
    HRESULT hr = o_CreateDepthStencilSurface(dev, rw, rh, f, ms, q, discard, s, shared);
    logf("CreateDepthStencilSurface %ux%u %s ms=%d -> %p", w, h, fmt_name(f), int(ms), SUCCEEDED(hr) ? *s : nullptr);
    if (resize && SUCCEEDED(hr)) stereo_track_surface(*s, w, h);
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
    RECT s2, d2;
    if (sr) {
        s2 = *sr;
        if (stereo_map_rect(src, &s2)) sr = &s2;
    }
    if (dr) {
        d2 = *dr;
        if (stereo_map_rect(dst, &d2)) dr = &d2;
    }
    return o_StretchRect(dev, src, sr, dst, dr, f);
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s) {
    if (index == 0) s = stereo_redirect_rt(s);  // HUD redirection (stereo.cpp)
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
        stereo_set_rt(s);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (g_frame) {
        D3DSURFACE_DESC d{};
        if (s) s->GetDesc(&d);
        flog("SetDepthStencil %p %ux%u %s", s, d.Width, d.Height, fmt_name(d.Format));
    }
    stereo_note_depth(s);
    return o_SetDepthStencilSurface(dev, stereo_redirect_depth(s));
}

static HRESULT STDMETHODCALLTYPE hk_Clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    flog("Clear flags=%lx color=%08lx z=%g stencil=%lu rects=%lu", flags, c, z, s, n);
    if (n && r && stereo_rt_resized()) {
        std::vector<D3DRECT> rects(r, r + n);
        for (auto& x : rects) {
            RECT q = {x.x1, x.y1, x.x2, x.y2};
            stereo_map_rect(g_rt0, &q);
            x = {q.left, q.top, q.right, q.bottom};
        }
        return o_Clear(dev, n, rects.data(), flags, c, z, s);
    }
    return o_Clear(dev, n, r, flags, c, z, s);
}

static HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
    if (g_frame && m)
        flog("SetTransform %d [% .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g | % .4g % .4g % .4g % .4g]", int(t),
             m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
    return o_SetTransform(dev, t, m);
}

static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (!vp) return o_SetViewport(dev, vp);
    g_vp = *vp;
    stereo_game_viewport(*vp);
    D3DVIEWPORT9 real = *vp;
    if (stereo_map_viewport(&real)) return o_SetViewport(dev, &real);
    return o_SetViewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE hk_GetViewport(IDirect3DDevice9* dev, D3DVIEWPORT9* vp) {
    HRESULT hr = o_GetViewport(dev, vp);
    if (SUCCEEDED(hr) && vp && stereo_rt_resized()) *vp = stereo_get_game_viewport();  // in the game's sizes
    return hr;
}

static RECT g_game_scissor;
static HRESULT STDMETHODCALLTYPE hk_SetScissorRect(IDirect3DDevice9* dev, const RECT* r) {
    if (!r) return o_SetScissorRect(dev, r);
    g_game_scissor = *r;
    stereo_game_scissor(*r);
    RECT real = *r;
    if (stereo_map_scissor(&real)) return o_SetScissorRect(dev, &real);
    return o_SetScissorRect(dev, r);
}

static HRESULT STDMETHODCALLTYPE hk_GetScissorRect(IDirect3DDevice9* dev, RECT* r) {
    HRESULT hr = o_GetScissorRect(dev, r);
    if (SUCCEEDED(hr) && r && stereo_rt_resized()) *r = g_game_scissor;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_frame && (s == D3DRS_ZENABLE || s == D3DRS_ZWRITEENABLE || s == D3DRS_ZFUNC || s == D3DRS_ALPHABLENDENABLE ||
                    s == D3DRS_CULLMODE || s == D3DRS_SCISSORTESTENABLE || s == D3DRS_COLORWRITEENABLE || s == D3DRS_STENCILENABLE))
        flog("RS %d = %lu", int(s), v);
    if (s == D3DRS_SCISSORTESTENABLE) stereo_scissor_enable(v != 0);
    if (stereo_hud_rs(s, v)) return D3D_OK;
    return o_SetRenderState(dev, s, v);
}

static HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) {
    if (g_frame && t && t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
        flog("SetTexture %lu = %s %p %ux%u %s", stage,
             d.Usage & D3DUSAGE_RENDERTARGET ? "RT" : d.Usage & D3DUSAGE_DEPTHSTENCIL ? "DS" : "tex", t, d.Width, d.Height, fmt_name(d.Format));
    }
    stereo_set_texture(stage, t);
    return o_SetTexture(dev, stage, t);
}

using SetPSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
static SetPSConstF_t o_SetPixelShaderConstantF;
static HRESULT STDMETHODCALLTYPE hk_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (g_frame && v)
        for (UINT i = 0; i < count && i < 24; ++i)
            flog("    ps c%-3u %-22s % .5g % .5g % .5g % .5g", start + i, const_name(start + i, true).c_str(), v[i * 4], v[i * 4 + 1],
                 v[i * 4 + 2], v[i * 4 + 3]);
    return o_SetPixelShaderConstantF(dev, start, v, count);
}

// Draws go through stereo_draw, which calls back once (mono) or once per eye.
struct DpArgs { IDirect3DDevice9* dev; D3DPRIMITIVETYPE t; UINT start, n; };
struct DipArgs { IDirect3DDevice9* dev; D3DPRIMITIVETYPE t; INT base; UINT minv, nv, si, n; };
struct DpupArgs { IDirect3DDevice9* dev; D3DPRIMITIVETYPE t; UINT n; const void* v; UINT stride; };
struct DipupArgs { IDirect3DDevice9* dev; D3DPRIMITIVETYPE t; UINT minv, nv, n; const void* idx; D3DFORMAT f; const void* v; UINT stride; };

static HRESULT run_dp(void* p) {
    auto a = static_cast<DpArgs*>(p);
    return o_DrawPrimitive(a->dev, a->t, a->start, a->n);
}
static HRESULT run_dip(void* p) {
    auto a = static_cast<DipArgs*>(p);
    return o_DrawIndexedPrimitive(a->dev, a->t, a->base, a->minv, a->nv, a->si, a->n);
}
static HRESULT run_dpup(void* p) {
    auto a = static_cast<DpupArgs*>(p);
    return o_DrawPrimitiveUP(a->dev, a->t, a->n, a->v, a->stride);
}
static HRESULT run_dipup(void* p) {
    auto a = static_cast<DipupArgs*>(p);
    return o_DrawIndexedPrimitiveUP(a->dev, a->t, a->minv, a->nv, a->n, a->idx, a->f, a->v, a->stride);
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    log_draw("DP", n);
    ++g_draws;
    DpArgs a{dev, t, start, n};
    return stereo_draw(dev, run_dp, &a);
}
static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT si, UINT n) {
    log_draw("DIP", n);
    ++g_draws;
    DipArgs a{dev, t, base, minv, nv, si, n};
    return stereo_draw(dev, run_dip, &a);
}
static HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    log_draw("DPUP", n);
    if (g_frame && v && stride >= 12) {
        auto f = static_cast<const float*>(v);
        flog("    type %d stride %u v0 % .2f % .2f % .3f", int(t), stride, f[0], f[1], f[2]);
    }
    ++g_draws;
    DpupArgs a{dev, t, n, v, stride};
    return stereo_draw(dev, run_dpup, &a, v, stride, n);
}
static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minv, UINT nv, UINT n,
                                                           const void* idx, D3DFORMAT f, const void* v, UINT stride) {
    log_draw("DIPUP", n);
    ++g_draws;
    DipupArgs a{dev, t, minv, nv, n, idx, f, v, stride};
    return stereo_draw(dev, run_dipup, &a);
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
    HOOK(kCreateVolumeTexture, CreateVolumeTexture);
    HOOK(kCreateCubeTexture, CreateCubeTexture);
    HOOK(kCreateVertexBuffer, CreateVertexBuffer);
    HOOK(kCreateIndexBuffer, CreateIndexBuffer);
    HOOK(kCreateRenderTarget, CreateRenderTarget);
    HOOK(kCreateDepthStencilSurface, CreateDepthStencilSurface);
    HOOK(kStretchRect, StretchRect);
    HOOK(kSetRenderTarget, SetRenderTarget);
    HOOK(kSetDepthStencilSurface, SetDepthStencilSurface);
    HOOK(kClear, Clear);
    HOOK(kSetTransform, SetTransform);
    HOOK(kSetViewport, SetViewport);
    HOOK(kGetViewport, GetViewport);
    HOOK(kSetRenderState, SetRenderState);
    HOOK(kSetTexture, SetTexture);
    HOOK(kSetScissorRect, SetScissorRect);
    HOOK(kGetScissorRect, GetScissorRect);
    HOOK(kDrawPrimitive, DrawPrimitive);
    HOOK(kDrawIndexedPrimitive, DrawIndexedPrimitive);
    HOOK(kDrawPrimitiveUP, DrawPrimitiveUP);
    HOOK(kDrawIndexedPrimitiveUP, DrawIndexedPrimitiveUP);
    HOOK(kCreateVertexShader, CreateVertexShader);
    HOOK(kSetVertexShader, SetVertexShader);
    HOOK(kSetVertexShaderConstantF, SetVertexShaderConstantF);
    HOOK(kCreatePixelShader, CreatePixelShader);
    HOOK(kSetPixelShader, SetPixelShader);
    HOOK(kSetPixelShaderConstantF, SetPixelShaderConstantF);
#undef HOOK
    logf("device %p hooked", dev);
}

// --- IDirect3D9 ---

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t o_CreateDevice;

static bool g_want_ex;

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev) {
    log_pp("CreateDevice (game)", pp);
    logf("  adapter=%u type=%d focus=%p behavior=%lx", adapter, int(type), focus, behavior);
    fix_pp(pp);
    stereo_set_backbuffer(pp->BackBufferWidth, pp->BackBufferHeight);
    HRESULT hr = g_want_ex ? ex_create_device(adapter, type, focus, behavior, pp, dev)
                           : o_CreateDevice(d3d, adapter, type, focus, behavior, pp, dev);
    logf("CreateDevice -> %08lx", hr);
    if (SUCCEEDED(hr)) hook_device(*dev);
    if (SUCCEEDED(hr) && ex_enabled()) handoff_init(*dev);
    return hr;
}

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
static Direct3DCreate9_t o_Direct3DCreate9;

static IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
    static bool started;
    if (!started) {
        started = true;
        vrsys_start();  // OpenVR (mode=openvr) before any render target is sized; failure -> off
    }
    // VR modes: a 9Ex device for the D3D11 hand-off (sim: unless handoff=0).
    IDirect3D9* d3d = nullptr;
    g_want_ex = g_vr.mode == VrMode::openvr || (g_vr.mode == VrMode::sim && g_vr.handoff);
    if (g_want_ex) d3d = ex_create_d3d(sdk);
    if (!d3d) {
        g_want_ex = false;
        d3d = o_Direct3DCreate9(sdk);
    }
    logf("Direct3DCreate9(%u) -> %p%s", sdk, d3d, g_want_ex ? " (9Ex)" : "");
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
