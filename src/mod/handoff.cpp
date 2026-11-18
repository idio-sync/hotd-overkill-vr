// D3D9 -> D3D11 hand-off (part 2 of milestone 1).
//
// The eye images, the HUD and the flat screen are rendered by the game's D3D9 device into our own
// D3DPOOL_DEFAULT render-target textures. With a 9Ex device (d3d9ex.cpp) those are created with
// shared handles and opened on a D3D11 device on the same adapter (matched by LUID), which copies
// them into its own textures for IVRCompositor::Submit and the overlays (vrsys.cpp).
//
// Options weighed (findings: docs/findings-probe.md "Hand-off"):
//  (a) 9Ex + shared DEFAULT textures - chosen; managed resources are emulated (d3d9ex.cpp).
//  (b) shared handles on the plain D3D9 device - the runtime refuses: CreateTexture with a
//      shared handle on the game's device returned D3DERR_INVALIDCALL (0x8876086c).
//  (c) CPU readback - two eye images per frame through system memory; only a fallback.
//
// Synchronisation: D3D9 has no keyed mutex. After the D3D9 copies of a frame, an event query is
// issued and (handoff_sync=1, the default) waited on before the D3D11 device reads the textures.
#include "common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>

namespace hv {

static ID3D11Device* g_d11;
static ID3D11DeviceContext* g_ctx;
static IDirect3DQuery9* g_query;
static bool g_tried;
static double g_wait_ms_sum;
static int g_wait_frames;
static LONGLONG g_qpf;

ID3D11Device* handoff_d11() { return g_d11; }
ID3D11DeviceContext* handoff_ctx() { return g_ctx; }
bool handoff_ready() { return g_d11 != nullptr; }

bool handoff_init(IDirect3DDevice9* dev) {
    if (g_tried) return g_d11 != nullptr;
    g_tried = true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    LUID luid{};
    if (!ex_enabled() || !ex_adapter_luid(&luid)) {
        logf("handoff: no 9Ex device; textures stay D3D9-only");
        return false;
    }
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* match = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
                logf("handoff: D3D9 adapter is DXGI adapter %u (%ls)", i, d.Description);
                match = a;
                break;
            }
            a->Release();
        }
        factory->Release();
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(match, match ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &g_d11, nullptr, &g_ctx);
    if (match) match->Release();
    logf("handoff: D3D11CreateDevice -> %08lx", hr);
    if (FAILED(hr)) {
        g_d11 = nullptr;
        g_ctx = nullptr;
        return false;
    }
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query);
    return true;
}

void handoff_release_d3d9() {
    if (g_query) g_query->Release();
    g_query = nullptr;
}

bool shared_ensure(SharedTex& s, IDirect3DDevice9* dev, UINT w, UINT h, bool share) {
    if (s.tex && s.w == w && s.h == h) return true;
    shared_release(s);
    HANDLE handle = nullptr;
    HRESULT hr = raw_create_texture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.tex,
                                    g_d11 && share ? &handle : nullptr);
    if (FAILED(hr)) {
        logf("handoff: %s texture %ux%u -> %08lx", s.name, w, h, hr);
        s.tex = nullptr;
        return false;
    }
    s.tex->GetSurfaceLevel(0, &s.surf);
    s.w = w, s.h = h;
    s.handle = handle;
    if (g_d11 && handle) {
        hr = g_d11->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&s.t11));
        if (FAILED(hr)) {
            logf("handoff: OpenSharedResource %s -> %08lx", s.name, hr);
            s.t11 = nullptr;
        }
    }
    logf("handoff: %s texture %ux%u%s", s.name, w, h, s.t11 ? " shared with D3D11" : "");
    return true;
}

void shared_release(SharedTex& s) {
    if (s.t11) s.t11->Release();
    if (s.surf) s.surf->Release();
    if (s.tex) s.tex->Release();
    s.t11 = nullptr;
    s.surf = nullptr;
    s.tex = nullptr;
    s.handle = nullptr;
    s.w = s.h = 0;
}

// Makes the D3D9 work submitted so far visible to D3D11 (flush; wait when handoff_sync=1).
void handoff_sync(IDirect3DDevice9* dev) {
    if (!g_query) return;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_query->Issue(D3DISSUE_END);
    if (g_vr.handoff_sync) {
        while (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) YieldProcessor();
    } else {
        g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    }
    QueryPerformanceCounter(&b);
    g_wait_ms_sum += double(b.QuadPart - a.QuadPart) * 1000.0 / g_qpf;
    ++g_wait_frames;
}

const char* handoff_status() {
    static char buf[96];
    if (!g_d11) return "";
    snprintf(buf, sizeof buf, ", handoff sync %.2f ms/frame", g_wait_frames ? g_wait_ms_sum / g_wait_frames : 0.0);
    g_wait_ms_sum = 0;
    g_wait_frames = 0;
    return buf;
}

// An own D3D11 texture shaped like `src`, holding a copy of it (recreated when the shape changes).
ID3D11Texture2D* handoff_copy(ID3D11Texture2D*& own, ID3D11Texture2D* src) {
    if (!src || !g_d11) return nullptr;
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (own) {
        D3D11_TEXTURE2D_DESC have;
        own->GetDesc(&have);
        if (have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
            own->Release();
            own = nullptr;
        }
    }
    if (!own) {
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        if (FAILED(g_d11->CreateTexture2D(&d, nullptr, &own))) return nullptr;
        logf("handoff: D3D11 texture %ux%u format %d", d.Width, d.Height, int(d.Format));
    }
    g_ctx->CopyResource(own, src);
    return own;
}

ID3D11Texture2D* handoff_black() {
    static ID3D11Texture2D* black;
    if (!black && g_d11) {
        static const uint32_t pixels[16 * 16] = {};
        D3D11_TEXTURE2D_DESC d{16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{pixels, 16 * 4, 0};
        g_d11->CreateTexture2D(&d, &init, &black);
    }
    return black;
}

// `vr handoff NAME`: what the D3D11 device holds, as PNGs (NAME.png, alpha in NAME_a.png).
void handoff_save(ID3D11Texture2D* tex, const std::string& name) {
    if (!tex || !g_d11) {
        logf("vr handoff: %s: no D3D11 texture", name.c_str());
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        logf("vr handoff: %s: format %d not saved", name.c_str(), int(d.Format));
        return;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_d11->CreateTexture2D(&d, nullptr, &st))) return;
    g_ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4), a(px.size());
        for (UINT y = 0; y < d.Height; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            uint8_t* o = &px[size_t(y) * d.Width * 4];
            memcpy(o, row, d.Width * 4);
            for (UINT x = 0; x < d.Width; ++x) {
                uint8_t* q = &a[(size_t(y) * d.Width + x) * 4];
                q[0] = q[1] = q[2] = o[x * 4 + 3];
                q[3] = 255;
                o[x * 4 + 3] = 255;
            }
        }
        g_ctx->Unmap(st, 0);
        std::wstring w(name.begin(), name.end());
        save_png(path_in((w + L".png").c_str()), d.Width, d.Height, d.Width * 4, px.data());
        save_png(path_in((w + L"_a.png").c_str()), d.Width, d.Height, d.Width * 4, a.data());
        logf("vr handoff: %s %ux%u saved from the D3D11 side", name.c_str(), d.Width, d.Height);
    }
    st->Release();
}

}  // namespace hv
