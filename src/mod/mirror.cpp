// The desktop window in sim/openvr modes (design: "the desktop window shows a mirror"):
//  - gameplay frames: left eye | right eye side by side at the eye images' aspect, each with the
//    HUD panel drawn where the headset shows it (projected through that eye; premultiplied alpha);
//  - flat frames: the flat-screen texture (the game's mono frame as sent to the headset),
//    letterboxed to the window.
// Drawn with the fixed-function pipeline into the back buffer just before Present, past our own
// hooks (raw_*), with the game's state saved in a state block and restored after.
#include "common.h"

#include <cmath>

#include "../vr/xform.h"

namespace hv {

static IDirect3DStateBlock9* g_sb;

void mirror_release() {
    if (g_sb) g_sb->Release();
    g_sb = nullptr;
}

struct Vtx {
    float x, y, z, rhw, u, v;
};

struct Saved {
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* ds = nullptr;
};

static bool begin(IDirect3DDevice9* dev, Saved* s, IDirect3DSurface9** bb, D3DSURFACE_DESC* bd) {
    if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) return false;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, bb))) return false;
    (*bb)->GetDesc(bd);
    g_sb->Capture();
    dev->GetRenderTarget(0, &s->rt);
    dev->GetDepthStencilSurface(&s->ds);
    raw_set_rt(dev, 0, *bb);
    raw_set_depth(dev, nullptr);
    D3DVIEWPORT9 vp = {0, 0, bd->Width, bd->Height, 0, 1};
    raw_set_viewport(dev, &vp);
    raw_clear(dev, 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1, 0);
    raw_set_vs(dev, nullptr);
    raw_set_ps(dev, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    const struct { D3DRENDERSTATETYPE s; DWORD v; } rs[] = {
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_LIGHTING, FALSE},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
    for (auto& r : rs) raw_set_rs(dev, r.s, r.v);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    return true;
}

static void end(IDirect3DDevice9* dev, Saved* s, IDirect3DSurface9* bb) {
    raw_set_rt(dev, 0, s->rt);
    raw_set_depth(dev, s->ds);
    g_sb->Apply();  // shaders, textures, states, viewport, scissor
    if (s->rt) s->rt->Release();
    if (s->ds) s->ds->Release();
    bb->Release();
}

static void quad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    // -0.5: D3D9 pixel centres.
    const Vtx v[4] = {{x0 - .5f, y0 - .5f, .5f, 1, u0, v0}, {x1 - .5f, y0 - .5f, .5f, 1, u1, v0},
                      {x0 - .5f, y1 - .5f, .5f, 1, u0, v1}, {x1 - .5f, y1 - .5f, .5f, 1, u1, v1}};
    raw_dpup(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

void mirror_stereo(IDirect3DDevice9* dev, IDirect3DTexture9* eyes, IDirect3DTexture9* hud) {
    Saved s;
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd{};
    if (!begin(dev, &s, &bb, &bd)) return;
    const float W = float(bd.Width), H = float(bd.Height);
    const float ew = W / 2, eh = ew * float(g_vr.eye_h) / float(g_vr.eye_w), top = (H - eh) / 2;
    for (int eye = 0; eye < 2; ++eye) {
        const float x0 = eye * ew;
        raw_set_texture(dev, 0, eyes);
        raw_set_rs(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        quad(dev, x0, top, x0 + ew, top + eh, eye * 0.5f, 0, eye * 0.5f + 0.5f, 1);
        vr::Proj p;
        if (!hud || !stereo_eye_proj(eye, &p)) continue;
        // The HUD panel as this eye sees it: perspective-correct (rhw = 1/w).
        const float aspect = float(stereo_hud_size().cy) / float(stereo_hud_size().cx);
        float c[4][3];
        if (!vr::project_panel(vrsys_eye_in_anchor(eye), vrsys_panel_in_anchor(false), g_vr.hud_width, g_vr.hud_width * aspect, p, c))
            continue;
        static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        Vtx v[4];
        for (int i = 0; i < 4; ++i) {
            const float w = c[i][2];
            v[i] = {x0 + (c[i][0] / w + 1) * 0.5f * ew - .5f, top + (1 - c[i][1] / w) * 0.5f * eh - .5f, .5f, 1 / w, uv[i][0], uv[i][1]};
        }
        const Vtx fan[4] = {v[0], v[1], v[3], v[2]};  // as a strip: TL, TR, BL, BR
        RECT clip = {LONG(x0), LONG(top), LONG(x0 + ew), LONG(top + eh)};
        raw_set_scissor(dev, &clip);
        raw_set_rs(dev, D3DRS_SCISSORTESTENABLE, TRUE);
        raw_set_rs(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        raw_set_rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
        raw_set_rs(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        raw_set_rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        raw_set_texture(dev, 0, hud);
        raw_dpup(dev, D3DPT_TRIANGLESTRIP, 2, fan, sizeof(Vtx));
        raw_set_rs(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    }
    raw_set_texture(dev, 0, nullptr);
    end(dev, &s, bb);
}

// Small filled squares-ish dots (8-sided) drawn over `target` at pixel centres xy; the game's
// state is saved and restored as for the mirror.
void draw_dots(IDirect3DDevice9* dev, IDirect3DSurface9* target, const float (*xy)[2], int n, float radius, D3DCOLOR colour) {
    if (!target || n <= 0) return;
    if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) return;
    D3DSURFACE_DESC d{};
    target->GetDesc(&d);
    g_sb->Capture();
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    raw_set_rt(dev, 0, target);
    raw_set_depth(dev, nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0, 1};
    raw_set_viewport(dev, &vp);
    raw_set_vs(dev, nullptr);
    raw_set_ps(dev, nullptr);
    raw_set_texture(dev, 0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    const struct { D3DRENDERSTATETYPE s; DWORD v; } rs[] = {
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_LIGHTING, FALSE},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
    for (auto& r : rs) raw_set_rs(dev, r.s, r.v);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    struct CV {
        float x, y, z, rhw;
        D3DCOLOR c;
    };
    for (int i = 0; i < n; ++i) {
        CV fan[10];
        fan[0] = {xy[i][0] - .5f, xy[i][1] - .5f, .5f, 1, colour};
        for (int k = 0; k <= 8; ++k) {
            const float a = k * 3.14159265f / 4;
            fan[k + 1] = {xy[i][0] + radius * std::cos(a) - .5f, xy[i][1] + radius * std::sin(a) - .5f, .5f, 1, colour};
        }
        raw_dpup(dev, D3DPT_TRIANGLEFAN, 8, fan, sizeof(CV));
    }
    raw_set_rt(dev, 0, rt);
    raw_set_depth(dev, ds);
    g_sb->Apply();
    if (rt) rt->Release();
    if (ds) ds->Release();
}

void mirror_flat(IDirect3DDevice9* dev, IDirect3DTexture9* screen) {
    if (!screen) return;
    Saved s;
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd{}, sd{};
    if (!begin(dev, &s, &bb, &bd)) return;
    screen->GetLevelDesc(0, &sd);
    const float W = float(bd.Width), H = float(bd.Height), a = float(sd.Width) / float(sd.Height);
    float w = W, h = W / a;
    if (h > H) h = H, w = H * a;
    raw_set_texture(dev, 0, screen);
    quad(dev, (W - w) / 2, (H - h) / 2, (W + w) / 2, (H + h) / 2, 0, 0, 1, 1);
    raw_set_texture(dev, 0, nullptr);
    end(dev, &s, bb);
}

}  // namespace hv
