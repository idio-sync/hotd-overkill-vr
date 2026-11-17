// Same-frame stereo by per-draw duplication (design: "Rendering").
//
// Screen-sized render targets are created double-wide at eye resolution (width x 2*eye_w/1280,
// height x eye_h/720): the 1280x720 scene target and its depth, the bloom/blur chain (640x180,
// 320x180, 160x90, 640x360) and the composite target with its depth. The composite target's size
// follows the window (1291x807 for a 1536x807 back buffer): it is the colour/depth target whose
// height is the back buffer's and whose width is smaller (and wider than tall). Shadow maps and
// other targets keep their size. The game keeps thinking in its own sizes: viewports, scissor
// rects and rects it passes for those targets are scaled, and GetViewport/GetScissorRect answer in
// its sizes.
//
// Frames are either stereo (gameplay: gamestate.cpp says in a level, no cutscene, not paused, and
// the scene is drawn) or flat (everything else), decided at each Present for the next frame:
//
// Stereo frames: on the scene target (1280x720 class), every draw whose clip-space matrix is a
// perspective one is issued twice: the left eye into the left half, the right eye into the right
// half, each with cWVP_eye = C_eye * cWVP (C_eye = P_eye * E_eye * P_game^-1, src/vr/stereo.h) for
// each clip matrix the shader declares (cWVP, cProject, cObjectViewProject, cProj) and eyePos moved
// to the eye. Everything else (full-screen passes, orthographic draws, other targets) is drawn once
// over the whole double-wide target. P_game and the view come from the engine's globals at draw
// time: projection 0x9C7DC0 and view 0x9C7E48 (D3DX row-vector matrices); `vr check` compares them
// with cWVP * cObjectMatrix^-1 of the draws. Eye poses and projections come from vrsys.cpp.
//   The composite draw (pixel shader 8cf0f75e1213637b) turns the post-processed scene into the
// composite target; right after it, the composite target holds both eyes (copied to the shared
// eye texture), and everything the game then draws "on the composite target" - the HUD: 2D
// sprites and the 3D ammo rounds - goes to our own HUD texture at the game's mono size instead,
// with its own depth buffer, cleared to transparent black. Its alpha is forced to accumulate
// coverage (separate alpha blend ONE, INVSRCALPHA) so the HUD texture is premultiplied. The
// game's crosshair (sprites centred on player 1's cursor) is skipped unless crosshair=1.
//
// Flat frames: nothing is duplicated; every draw goes over the whole double-wide targets, so the
// game's own frame comes out in the back buffer as usual (the scene supersampled horizontally).
// At Present the part of the back buffer the game drew is copied to the flat-screen texture.
#include "common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../vr/stereo.h"

namespace hv {

constexpr uintptr_t kProjVa = 0x9C7DC0, kViewVa = 0x9C7E48;
constexpr uintptr_t kGameControl = 0x99B708;  // cGameControl; +0x24 = player 1 (cPlayer)
constexpr uint64_t kCompositePs = 0x8cf0f75e1213637bull;
constexpr float kPi = 3.14159265f;

enum Kind { kOther, kScene, kComposite };

struct Target {
    UINT ow, oh;  // the game's size
    UINT w, h;    // the real size
    Kind kind;
};

static std::mutex g_mx;
static std::unordered_map<IDirect3DSurface9*, Target> g_targets;
static UINT g_bb_w = 1536, g_bb_h = 807;

// Render target 0 (as really set) and the game's depth surface.
static IDirect3DSurface9* g_rt;
static Target g_rt_info;
static bool g_rt_resized, g_rt_scene;
static IDirect3DSurface9* g_scene_surface;
static IDirect3DSurface9* g_game_depth;

static D3DVIEWPORT9 g_game_vp;
static RECT g_game_scissor;
static bool g_scissor_on;

// Frame mode.
static bool g_frame_stereo;      // this frame is drawn in stereo
static int g_frames_since_scene = 1 << 20;
static bool g_level_was;
static const char* g_recentre_pending;  // why the next entry into gameplay recentres (null: it doesn't)
static GameState g_state;

// HUD redirection and the hand-off textures.
// The HUD is drawn into a private target (g_hudrt, also what the desktop mirror shows) and copied
// once per frame into the shared one, so D3D11 never reads a texture the game is still drawing.
static SharedTex g_eyetex{"eyes"}, g_hud{"hud"}, g_hudrt{"hud draw"}, g_screen{"screen"};
static IDirect3DSurface9* g_hud_depth;
static UINT g_hud_depth_w, g_hud_depth_h;
static IDirect3DSurface9* g_bb_resolve;  // non-multisampled copy of the back buffer (flat frames)
static IDirect3DDevice9* g_hud_dev;
static bool g_hud_active, g_hud_drawn;
static IDirect3DSurface9* g_comp_rt;
static IDirect3DSurface9* g_comp_depth;
static UINT g_comp_ow, g_comp_oh;  // the composite target's game size
static DWORD g_game_rs[4] = {FALSE, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLENDOP_ADD};  // separate alpha states as the game set them
static DWORD g_game_blend[2] = {D3DBLEND_ONE, D3DBLEND_ZERO};  // SRCBLEND, DESTBLEND as the game set them
static DWORD g_hud_alpha_dest = ~0u;  // DESTBLENDALPHA as we last set it during the HUD
static int g_crosshair_skips, g_last_crosshair_skips;
static RECT g_bb_rect;  // what the game drew on the back buffer this frame
static bool g_bb_drawn;
static RECT g_bb_comp_rect;  // where the game drew the composite target into the back buffer
static bool g_bb_comp;
static ID3D11Texture2D* g_own11[3];  // D3D11 copies: eyes, hud, screen
static RECT g_flat_rect;  // the back-buffer area the flat screen showed last
static bool g_flat_rect_ok;

// Per-frame stats.
static int g_stereo_draws, g_scene_mono_draws, g_overlay_draws, g_last_stereo, g_last_mono, g_last_overlay;
static int g_scene_persp;  // perspective draws on the scene target (stereo or not)
static int g_frames_stereo, g_frames_flat;

static char g_status[256];
static std::unordered_map<void*, bool> g_target_textures;
static bool g_check_requested, g_checking;

void stereo_set_backbuffer(UINT w, UINT h) {
    if (w && h) g_bb_w = w, g_bb_h = h;
}

static Kind kind_of(UINT w, UINT h, bool depth) {
    static const UINT colour[][2] = {{1280, 720}, {640, 180}, {320, 180}, {160, 90}, {640, 360}};
    if (w == 1280 && h == 720) return kScene;
    if (!depth)
        for (auto& s : colour)
            if (s[0] == w && s[1] == h) return kScene;  // the scene's post-processing chain
    // The composite target: as tall as the back buffer, narrower, wider than tall (1291x807 for a
    // 1536x807 back buffer); or, seen with a 1920x1200 back buffer, a smaller 16:10 one
    // (1680x1050): 16:10 within a few pixels, narrower than the back buffer (not the back buffer's
    // own 1920x1200 depth surface).
    if (h == g_bb_h && w < g_bb_w && w > h) return kComposite;
    if (h >= 400 && w < g_bb_w && h <= g_bb_h && std::abs(int(w) * 10 - int(h) * 16) <= 24) return kComposite;
    return kOther;
}

bool stereo_target_size(UINT w, UINT h, bool depth, UINT* out_w, UINT* out_h) {
    if (g_vr.mode == VrMode::off || kind_of(w, h, depth) == kOther) return false;
    *out_w = UINT(std::lround(double(w) * 2.0 * g_vr.eye_w / 1280.0));
    *out_h = UINT(std::lround(double(h) * g_vr.eye_h / 720.0));
    return true;
}

static void track(IDirect3DSurface9* s, UINT w, UINT h, bool depth) {
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    const Kind k = kind_of(w, h, depth);
    std::lock_guard lock(g_mx);
    g_targets[s] = {w, h, d.Width, d.Height, k};
    if (k == kComposite && !depth) g_comp_ow = w, g_comp_oh = h;
    logf("stereo target %p: %ux%u -> %ux%u%s", s, w, h, d.Width, d.Height, k == kComposite ? " (composite)" : "");
}

void stereo_track_texture(IDirect3DTexture9* tex, UINT w, UINT h) {
    IDirect3DSurface9* s = nullptr;
    D3DSURFACE_DESC d{};
    tex->GetLevelDesc(0, &d);
    {
        std::lock_guard lock(g_mx);
        g_target_textures[tex] = true;
    }
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s) {
        track(s, w, h, (d.Usage & D3DUSAGE_DEPTHSTENCIL) != 0);
        s->Release();  // the texture keeps its level alive; the pointer stays the same
    }
}

void stereo_track_surface(IDirect3DSurface9* s, UINT w, UINT h) {
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    track(s, w, h, (d.Usage & D3DUSAGE_DEPTHSTENCIL) != 0);
}

// A tracked surface, checked against its real size (a freed surface's address can be reused).
static bool lookup(IDirect3DSurface9* s, Target* out) {
    if (!s) return false;
    {
        std::lock_guard lock(g_mx);
        auto it = g_targets.find(s);
        if (it == g_targets.end()) return false;
        *out = it->second;
    }
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    return d.Width == out->w && d.Height == out->h;
}

void stereo_set_rt(IDirect3DSurface9* s) {
    g_rt = s;
    g_rt_resized = lookup(s, &g_rt_info);
    g_rt_scene = g_rt_resized && g_rt_info.ow == 1280 && g_rt_info.oh == 720;
    UINT w = 0, h = 0;
    if (g_rt_resized) {
        w = g_rt_info.ow, h = g_rt_info.oh;
    } else if (s) {
        D3DSURFACE_DESC d{};
        s->GetDesc(&d);
        w = d.Width, h = d.Height;
    }
    // SetRenderTarget resets the viewport and the scissor rect to the whole target.
    g_game_vp = {0, 0, w, h, 0.f, 1.f};
    g_game_scissor = {0, 0, LONG(w), LONG(h)};
}

void stereo_note_depth(IDirect3DSurface9* s) { g_game_depth = s; }
bool stereo_rt_resized() { return g_rt_resized; }

std::vector<IDirect3DSurface9*> stereo_targets() {
    std::vector<IDirect3DSurface9*> all, out;
    {
        std::lock_guard lock(g_mx);
        for (auto& [s, t] : g_targets) all.push_back(s);
    }
    for (auto* s : all) {
        Target t;
        if (lookup(s, &t)) out.push_back(s);
    }
    return out;
}
IDirect3DSurface9* stereo_scene_surface() { return g_scene_surface; }

void stereo_game_viewport(const D3DVIEWPORT9& vp) { g_game_vp = vp; }
const D3DVIEWPORT9& stereo_get_game_viewport() { return g_game_vp; }

// Maps the game's x range [x0, x1) on a target of game width ow into the real target: the whole
// width (eye < 0) or one eye's half.
static void map_x(const Target& t, int eye, double x0, double x1, double* o0, double* o1) {
    if (eye < 0) {
        const double s = double(t.w) / t.ow;
        *o0 = x0 * s, *o1 = x1 * s;
    } else {
        const double half = t.w / 2.0, s = half / t.ow;
        *o0 = eye * half + x0 * s, *o1 = eye * half + x1 * s;
    }
}

static D3DVIEWPORT9 eye_viewport(const Target& t, const D3DVIEWPORT9& g, int eye) {
    double x0, x1;
    map_x(t, eye, g.X, double(g.X) + g.Width, &x0, &x1);
    const double sy = double(t.h) / t.oh;
    D3DVIEWPORT9 v = g;
    v.X = DWORD(std::lround(x0));
    v.Width = DWORD(std::lround(x1)) - v.X;
    v.Y = DWORD(std::lround(g.Y * sy));
    v.Height = DWORD(std::lround((double(g.Y) + g.Height) * sy)) - v.Y;
    return v;
}

static RECT eye_rect(const Target& t, const RECT& r, int eye) {
    double x0, x1;
    map_x(t, eye, r.left, r.right, &x0, &x1);
    const double sy = double(t.h) / t.oh;
    return {LONG(std::lround(x0)), LONG(std::lround(r.top * sy)), LONG(std::lround(x1)), LONG(std::lround(r.bottom * sy))};
}

bool stereo_map_viewport(D3DVIEWPORT9* vp) {
    if (!g_rt_resized) return false;
    *vp = eye_viewport(g_rt_info, *vp, -1);
    return true;
}

bool stereo_map_rect(IDirect3DSurface9* s, RECT* r) {
    Target t;
    if (!lookup(s, &t)) return false;
    *r = eye_rect(t, *r, -1);
    return true;
}

void stereo_game_scissor(const RECT& r) { g_game_scissor = r; }
bool stereo_map_scissor(RECT* r) {
    if (!g_rt_resized) return false;
    *r = eye_rect(g_rt_info, *r, -1);
    return true;
}
void stereo_scissor_enable(bool on) { g_scissor_on = on; }

// --- per-eye matrices ---

struct Eyes {
    float proj_raw[16], view_raw[16];
    unsigned frame;
    bool valid;
    vr::Mat4 p_game, view;  // column form
    vr::Mat4 c[2];
    vr::Proj pe[2];
    float world_off[2][3];
};
static Eyes g_eyes;
static unsigned g_frame_no = 1;

static bool update_eyes() {
    const float* p = reinterpret_cast<const float*>(va(kProjVa));
    const float* v = reinterpret_cast<const float*>(va(kViewVa));
    Eyes& e = g_eyes;
    if (e.valid && e.frame == g_frame_no && !memcmp(e.proj_raw, p, 64) && !memcmp(e.view_raw, v, 64)) return true;
    memcpy(e.proj_raw, p, 64);
    memcpy(e.view_raw, v, 64);
    e.frame = g_frame_no;
    e.valid = false;
    e.p_game = vr::from_d3dx(p);
    e.view = vr::from_d3dx(v);
    vr::Proj pg;
    if (!vr::proj_from_matrix(e.p_game, &pg)) {
        static int warned;
        if (warned++ < 5) logf("stereo: engine projection is not a perspective one; drawing mono");
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        vr::Mat4 pose;
        float t[4];
        if (vrsys_eye(i, &pose, t)) e.pe[i] = vr::proj_from_tangents(t[0], t[1], t[2], t[3], pg.a, pg.b);
        else e.pe[i] = vr::sim_eye_proj(pg, float(g_vr.eye_w) / float(g_vr.eye_h));
        if (!vr::correction(e.p_game, e.pe[i], vr::eye_view(pose, g_vr.unit_m), &e.c[i])) return false;
        float off[3];
        vr::eye_offset_units(pose, g_vr.unit_m, off);
        for (int k = 0; k < 3; ++k)
            e.world_off[i][k] = off[0] * e.view.m[0][k] + off[1] * e.view.m[1][k] + off[2] * e.view.m[2][k];
    }
    e.valid = true;
    return true;
}

bool stereo_eye_proj(int eye, vr::Proj* out) {
    if (!g_eyes.valid) return false;
    *out = g_eyes.pe[eye & 1];
    return true;
}

void stereo_eye_half_angles(float game_hx, float game_hy, float* hx, float* hy) {
    if (!g_eyes.valid) {  // before the first stereo frame: the sim eye (the game's vertical FOV)
        *hy = game_hy;
        *hx = std::fmax(game_hx, std::atan(std::tan(game_hy) * float(g_vr.eye_w) / float(g_vr.eye_h)));
        return;
    }
    float mx = 0, my = 0;
    for (const vr::Proj& p : g_eyes.pe) {
        float t[4];
        vr::proj_tangents(p, t);
        mx = std::fmax(mx, std::fmax(-t[0], t[1]));
        my = std::fmax(my, std::fmax(-t[2], t[3]));
    }
    *hx = std::atan(mx);
    *hy = std::atan(my);
}

static vr::Mat4 load(int reg) {
    vr::Mat4 m;
    for (int i = 0; i < 4; ++i) memcpy(m.m[i], vs_shadow(UINT(reg + i)), 16);
    return m;
}

// `vr check`: the engine's P*V against cWVP * cObjectMatrix^-1 for the scene draws of a frame.
static float g_check_max;
static int g_check_draws;
static void check_draw(const ShaderInfo* info) {
    if (info->object_reg < 0 || info->clip_regs[0] != 0) return;
    vr::Mat4 w = vr::identity();
    for (int i = 0; i < 3; ++i) memcpy(w.m[i], vs_shadow(UINT(info->object_reg + i)), 16);
    vr::Mat4 wi;
    if (!vr::invert(w, &wi)) return;
    const vr::Mat4 vp = vr::mul(load(0), wi);
    const vr::Mat4 pv = vr::mul(g_eyes.p_game, g_eyes.view);
    // Translation terms are in game units (thousands): compare the rotation/scale part and the
    // translation column separately, relative to their size.
    float rot = 0, tr = 0, trs = 1;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) rot = std::fmax(rot, std::fabs(vp.m[i][j] - pv.m[i][j]));
        tr = std::fmax(tr, std::fabs(vp.m[i][3] - pv.m[i][3]));
        trs = std::fmax(trs, std::fabs(pv.m[i][3]));
    }
    const float err = std::fmax(rot, tr / trs);
    if (g_check_draws < 3 || err > 1e-3f)
        logf("vr check: draw vs=%016llx rot diff %.2g, translation diff %.3g (of %.0f)", info->hash, rot, tr, trs);
    g_check_max = std::fmax(g_check_max, err);
    ++g_check_draws;
}

// Textures bound per sampler stage that are resized targets (a pass reading a screen-sized image).
static bool g_stage_target[16];

void stereo_set_texture(DWORD stage, IDirect3DBaseTexture9* t) {
    if (stage >= 16 || g_vr.mode == VrMode::off) return;
    std::lock_guard lock(g_mx);
    g_stage_target[stage] = t && g_target_textures.count(t);
}

static bool reads_target() {
    for (bool b : g_stage_target)
        if (b) return true;
    return false;
}

// --- HUD redirection ---

SIZE stereo_hud_size() { return {LONG(g_hud.w ? g_hud.w : 1291), LONG(g_hud.h ? g_hud.h : 807)}; }

static const D3DRENDERSTATETYPE kSepAlpha[4] = {D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
                                                D3DRS_BLENDOPALPHA};

bool stereo_hud_rs(D3DRENDERSTATETYPE s, DWORD v) {
    if (s == D3DRS_SRCBLEND) g_game_blend[0] = v;
    if (s == D3DRS_DESTBLEND) g_game_blend[1] = v;
    for (int i = 0; i < 4; ++i)
        if (s == kSepAlpha[i]) {
            g_game_rs[i] = v;
            return g_hud_active;  // while the HUD is redirected, ours stay
        }
    return false;
}

static void read_cursor();

static void hud_begin(IDirect3DDevice9* dev) {
    const Target comp = g_rt_info;
    // The eyes: the composite target as it is now (both halves).
    if (!shared_ensure(g_eyetex, dev, comp.w, comp.h) || !shared_ensure(g_hud, dev, comp.ow, comp.oh) ||
        !shared_ensure(g_hudrt, dev, comp.ow, comp.oh, false))
        return;
    if (!g_hud_depth || g_hud_depth_w != comp.ow || g_hud_depth_h != comp.oh) {
        if (g_hud_depth) g_hud_depth->Release();
        g_hud_depth = nullptr;
        if (FAILED(raw_create_depth(dev, comp.ow, comp.oh, D3DFMT_D24S8, &g_hud_depth))) {
            logf("stereo: HUD depth %ux%u failed", comp.ow, comp.oh);
            return;
        }
        g_hud_depth_w = comp.ow, g_hud_depth_h = comp.oh;
    }
    raw_stretch(dev, g_rt, nullptr, g_eyetex.surf, nullptr, D3DTEXF_NONE);
    g_comp_rt = g_rt;
    g_comp_depth = g_game_depth;
    g_hud_dev = dev;
    const D3DVIEWPORT9 vp = g_game_vp;  // in the composite's game size = the HUD texture's size
    const RECT sc = g_game_scissor;
    raw_set_rt(dev, 0, g_hudrt.surf);
    raw_set_depth(dev, g_hud_depth);
    raw_clear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(0, 0, 0, 0), 1, 0);
    stereo_set_rt(g_hudrt.surf);
    g_game_vp = vp;
    g_game_scissor = sc;
    raw_set_viewport(dev, &vp);
    if (g_scissor_on) raw_set_scissor(dev, &sc);
    // Alpha accumulates coverage: with the colour's SRCALPHA/INVSRCALPHA blend the HUD texture
    // ends up premultiplied over transparent black.
    raw_set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    raw_set_rs(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_hud_alpha_dest = ~0u;  // set per draw (hud_alpha)
    g_hud_active = true;
    g_hud_drawn = true;
    read_cursor();
    frame_note("    stereo: HUD redirected to the HUD texture");
}

static void hud_end() {
    if (!g_hud_active) return;
    g_hud_active = false;
    for (int i = 0; i < 4; ++i) raw_set_rs(g_hud_dev, kSepAlpha[i], g_game_rs[i]);
}

IDirect3DSurface9* stereo_redirect_rt(IDirect3DSurface9* s) {
    if (!g_hud_active) return s;
    if (s == g_comp_rt) return g_hudrt.surf;
    hud_end();  // the game moved on (the back buffer copy)
    return s;
}

IDirect3DSurface9* stereo_redirect_depth(IDirect3DSurface9* s) {
    return g_hud_active && s && s == g_comp_depth ? g_hud_depth : s;
}

// The HUD texture's alpha per draw: sprites blended "over" (DESTBLEND = INVSRCALPHA) add their
// coverage (alpha ONE, INVSRCALPHA); additive, multiplicative and other blends leave alpha as it is
// (ZERO, ONE) - additive light then shows as premultiplied colour with no coverage, and sprites
// that don't change the colour (black additive quads) don't punch opaque holes in the panel.
static void hud_alpha(IDirect3DDevice9* dev) {
    const bool over = g_game_blend[1] == D3DBLEND_INVSRCALPHA;
    const DWORD dest = over ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE;
    if (dest == g_hud_alpha_dest) return;
    raw_set_rs(dev, D3DRS_SRCBLENDALPHA, over ? D3DBLEND_ONE : D3DBLEND_ZERO);
    raw_set_rs(dev, D3DRS_DESTBLENDALPHA, dest);
    g_hud_alpha_dest = dest;
}

// Player 1's cursor: render-target pixels from the centre, +y up (findings-static.md). Read once
// per frame (at the HUD's start).
static bool g_cursor_ok;
static float g_cursor[2];
static void read_cursor() {
    const uintptr_t gc = *reinterpret_cast<const volatile uintptr_t*>(va(kGameControl));
    uintptr_t player = 0;
    g_cursor_ok = gc && safe_copy(&player, gc + 0x24, 4) && player && safe_copy(g_cursor, player + 0x1BC, 8);
}
static bool cursor_pos(float* x, float* y) {
    *x = g_cursor[0], *y = g_cursor[1];
    return g_cursor_ok;
}

// A HUD sprite drawn centred on the cursor: the crosshair. HUD sprites are DrawPrimitiveUP quads
// in local coordinates, placed by their cWVP (c0..c3); in clip space x * w/2, y * h/2 are pixels
// from the centre of the composite target, +y up - the cursor's space.
static bool is_crosshair(const void* up, UINT stride, UINT prims) {
    if (!up || stride < 12 || prims != 2) return false;
    float cx, cy;
    if (!cursor_pos(&cx, &cy)) return false;
    const vr::Mat4 m = load(0);
    const float hw = float(g_hudrt.w) / 2, hh = float(g_hudrt.h) / 2;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(up) + size_t(i) * stride);
        const vr::Vec4 c = vr::apply(m, {v[0], v[1], v[2], 1});
        if (std::fabs(c.w) < 1e-6f) return false;
        const float px = c.x / c.w * hw, py = c.y / c.w * hh;
        x0 = std::fmin(x0, px), x1 = std::fmax(x1, px), y0 = std::fmin(y0, py), y1 = std::fmax(y1, py);
    }
    const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    if (frame_logging()) {
        char line[160];
        snprintf(line, sizeof line, "    hud sprite centre %.1f %.1f size %.1f x %.1f (cursor %.1f %.1f) blend %lu/%lu", mx, my, x1 - x0,
                 y1 - y0, cx, cy, g_game_blend[0], g_game_blend[1]);
        frame_note(line);
    }
    return std::fabs(mx - cx) < 2 && std::fabs(my - cy) < 2 && x1 - x0 < 200 && y1 - y0 < 200;
}

HRESULT stereo_draw(IDirect3DDevice9* dev, DrawFn draw, void* ctx, const void* up, UINT stride, UINT prims) {
    if (g_vr.mode == VrMode::off) return draw(ctx);
    if (g_hud_active && !g_vr.crosshair && !(g_dbg & 4) && is_crosshair(up, stride, prims)) {
        ++g_crosshair_skips;
        frame_note("    stereo: crosshair skipped");
        return S_OK;
    }
    if (g_hud_active) hud_alpha(dev);
    if (!g_rt_resized && g_rt) {  // the back buffer's drawn area, for the flat screen
        D3DSURFACE_DESC d{};
        g_rt->GetDesc(&d);
        if (d.Width == g_bb_w && d.Height == g_bb_h) {
            const RECT r = {LONG(g_game_vp.X), LONG(g_game_vp.Y), LONG(g_game_vp.X + g_game_vp.Width), LONG(g_game_vp.Y + g_game_vp.Height)};
            if (!g_bb_drawn) g_bb_rect = r;
            else UnionRect(&g_bb_rect, &g_bb_rect, &r);
            g_bb_drawn = true;
            if (reads_target()) g_bb_comp_rect = r, g_bb_comp = true;  // the composite blit (pillarboxed)
        }
    }
    if (!g_rt_scene) {
        HRESULT hr = draw(ctx);
        if (g_frame_stereo && !g_hud_active && !(g_dbg & 16) && g_rt_resized && g_rt_info.kind == kComposite) {
            IDirect3DPixelShader9* ps = current_ps();
            const ShaderInfo* pi = ps ? shader_info(ps) : nullptr;
            if (pi && pi->hash == kCompositePs) hud_begin(dev);
        }
        return hr;
    }
    IDirect3DVertexShader9* vs = current_vs();
    const ShaderInfo* info = vs ? shader_info(vs) : nullptr;
    bool correct = info && info->n_clip;
    if (correct) {
        const float* w = vs_shadow(UINT(info->clip_regs[0] + 3));
        if (std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) < 1e-7f) correct = false;  // orthographic
    }
    if (correct) ++g_scene_persp;
    if (!g_frame_stereo) return draw(ctx);
    if (correct && !update_eyes()) {
        ++g_scene_mono_draws;
        return draw(ctx);
    }
    if (!correct) {
        // Not a perspective draw. A pass that reads a screen-sized target (post-processing of the
        // scene) is drawn once across both halves; anything else is a screen overlay (blood on the
        // screen, flashes) and goes into each eye's half unchanged.
        if (reads_target()) {
            ++g_scene_mono_draws;
            return draw(ctx);
        }
        ++g_overlay_draws;
        frame_note("    stereo: overlay drawn into each eye's half");
    } else {
        if (g_checking) check_draw(info);
        g_scene_surface = g_rt;
        ++g_stereo_draws;
        frame_note("    stereo: drawn once per eye");
    }

    HRESULT hr = S_OK;
    for (int eye = 0; eye < 2; ++eye) {
        D3DVIEWPORT9 vp = eye_viewport(g_rt_info, g_game_vp, eye);
        raw_set_viewport(dev, &vp);
        if (g_scissor_on) {
            RECT r = eye_rect(g_rt_info, g_game_scissor, eye);
            raw_set_scissor(dev, &r);
        }
        if (!correct) {
            HRESULT r = draw(ctx);
            if (FAILED(r)) hr = r;
            continue;
        }
        for (int k = 0; k < info->n_clip; ++k) {
            const vr::Mat4 m = vr::mul(g_eyes.c[eye], load(info->clip_regs[k]));
            raw_set_vs_const(dev, UINT(info->clip_regs[k]), &m.m[0][0], 4);
        }
        if (info->eye_reg >= 0) {
            const float* p = vs_shadow(UINT(info->eye_reg));
            const float e[4] = {p[0] + g_eyes.world_off[eye][0], p[1] + g_eyes.world_off[eye][1], p[2] + g_eyes.world_off[eye][2], p[3]};
            raw_set_vs_const(dev, UINT(info->eye_reg), e, 1);
        }
        HRESULT r = draw(ctx);
        if (FAILED(r)) hr = r;
    }
    // Back to the game's state.
    if (correct) {
        for (int k = 0; k < info->n_clip; ++k) raw_set_vs_const(dev, UINT(info->clip_regs[k]), vs_shadow(UINT(info->clip_regs[k])), 4);
        if (info->eye_reg >= 0) raw_set_vs_const(dev, UINT(info->eye_reg), vs_shadow(UINT(info->eye_reg)), 1);
    }
    D3DVIEWPORT9 full = eye_viewport(g_rt_info, g_game_vp, -1);
    raw_set_viewport(dev, &full);
    if (g_scissor_on) {
        RECT r = eye_rect(g_rt_info, g_game_scissor, -1);
        raw_set_scissor(dev, &r);
    }
    return hr;
}

bool stereo_frame_active() { return g_frame_stereo; }

// --- extra geometry in the scene (the gun model) ---
// The scene target is drawn in two sessions per frame: the opaque pass (~300 draws per eye), then,
// after a one-draw detour to a 1280x180 target, transparent effects. The gun goes in when the game
// leaves the scene target after its opaque pass: depth-tested against the scene, before the
// effects and the post-processing.
static bool g_extra_drawn;

static bool scene_eye_setup(IDirect3DDevice9* dev, int eye, vr::Mat4* world_to_clip) {
    if (!update_eyes()) return false;
    D3DVIEWPORT9 vp = eye_viewport(g_rt_info, g_game_vp, eye);
    raw_set_viewport(dev, &vp);
    *world_to_clip = vr::mul(g_eyes.c[eye], vr::mul(g_eyes.p_game, g_eyes.view));
    return true;
}

void stereo_before_rt(IDirect3DDevice9* dev, IDirect3DSurface9* next) {
    if (!g_frame_stereo || !g_rt_scene || next == g_rt || g_extra_drawn || g_stereo_draws < 30 || g_hud_active) return;
    g_extra_drawn = true;
    const D3DVIEWPORT9 game_vp = g_game_vp;
    gunmodel_draw(dev, scene_eye_setup);
    D3DVIEWPORT9 full = eye_viewport(g_rt_info, game_vp, -1);
    raw_set_viewport(dev, &full);
    frame_note("    stereo: gun model drawn into the scene target");
}

// --- Present ---

// Flat frames: the game's picture in the back buffer -> the flat-screen texture (mono size).
static bool copy_flat(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    RECT r = g_bb_comp ? g_bb_comp_rect : g_bb_drawn ? g_bb_rect : RECT{0, 0, LONG(d.Width), LONG(d.Height)};
    r.left = (std::max)(r.left, 0L), r.top = (std::max)(r.top, 0L);
    r.right = (std::min)(r.right, LONG(d.Width)), r.bottom = (std::min)(r.bottom, LONG(d.Height));
    bool ok = r.right > r.left && r.bottom > r.top && shared_ensure(g_screen, dev, UINT(r.right - r.left), UINT(r.bottom - r.top));
    IDirect3DSurface9* src = bb;
    if (ok && d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // Resolve first (a multisampled source can't be stretched or cropped on every driver).
        D3DSURFACE_DESC rd{};
        if (g_bb_resolve) g_bb_resolve->GetDesc(&rd);
        if (!g_bb_resolve || rd.Width != d.Width || rd.Height != d.Height) {
            if (g_bb_resolve) g_bb_resolve->Release();
            g_bb_resolve = nullptr;
            ok = SUCCEEDED(raw_create_rt(dev, d.Width, d.Height, d.Format, &g_bb_resolve));
        }
        if (ok) ok = SUCCEEDED(raw_stretch(dev, bb, nullptr, g_bb_resolve, nullptr, D3DTEXF_NONE));
        src = g_bb_resolve;
    }
    if (ok) ok = SUCCEEDED(raw_stretch(dev, src, &r, g_screen.surf, nullptr, D3DTEXF_NONE));
    bb->Release();
    if (ok) g_flat_rect = r, g_flat_rect_ok = true;
    return ok;
}

bool stereo_flat_rect(RECT* r) {
    *r = g_flat_rect;
    return g_flat_rect_ok;
}

// A world point as an eye-texture pixel (both eyes side by side in g_eyetex), through the eye's
// projection and the frame's game view: what the headset shows there.
bool stereo_world_to_eye(const float p[3], int eye, float* x, float* y) {
    if (!g_eyes.valid || !g_eyetex.w) return false;
    const vr::Mat4 m = vr::mul(g_eyes.c[eye], vr::mul(g_eyes.p_game, g_eyes.view));
    const vr::Vec4 c = vr::apply(m, {p[0], p[1], p[2], 1});
    if (c.w <= 1e-3f) return false;
    const float half = float(g_eyetex.w) / 2;
    *x = eye * half + (c.x / c.w + 1) * 0.5f * half;
    *y = (1 - c.y / c.w) * 0.5f * float(g_eyetex.h);
    return *x >= eye * half && *x < (eye + 1) * half && *y >= 0 && *y < float(g_eyetex.h);
}

// [vr] marker: a small dot in each eye where the shot will land (the aim point).
static void draw_marker(IDirect3DDevice9* dev) {
    float p[3];
    bool hit;
    if (!g_vr.marker || !gun_aim_point(p, &hit)) return;
    float xy[2][2];
    int n = 0;
    for (int eye = 0; eye < 2; ++eye)
        if (stereo_world_to_eye(p, eye, &xy[n][0], &xy[n][1])) ++n;
    const float r = (std::max)(2.f, 3.f * float(g_eyetex.h) / 807.f);
    if (n) draw_dots(dev, g_eyetex.surf, xy, n, r, hit ? D3DCOLOR_XRGB(255, 40, 40) : D3DCOLOR_XRGB(255, 200, 40));
}

// Time spent in stereo_present per frame (status line), and `vr dbg BITS` switches for bisecting
// costs: 1 no mirror, 2 no D3D11 copies, 4 no crosshair check, 8 no culling widening, 16 no HUD
// redirection.
int g_dbg;
static double g_present_ms, g_ms_stereo, g_ms_flat;
static int g_present_n;
static double now_ms() {
    static LONGLONG f;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f);
}

void stereo_present(IDirect3DDevice9* dev, const FrameRequests& req) {
    if (g_vr.mode == VrMode::off) return;
    const double t0 = now_ms();
    struct Timer {
        double t0;
        ~Timer() { g_present_ms += now_ms() - t0, ++g_present_n; }
    } timer{t0};
    hud_end();
    const bool stereo = g_frame_stereo && g_hud_drawn;
    const bool flat = !stereo && copy_flat(dev);
    if (stereo) raw_stretch(dev, g_hudrt.surf, nullptr, g_hud.surf, nullptr, D3DTEXF_NONE);
    if (stereo) draw_marker(dev);
    if (g_vr.mirror && !(g_dbg & 1)) {
        if (stereo) mirror_stereo(dev, g_eyetex.tex, g_hudrt.tex);
        else if (flat) mirror_flat(dev, g_screen.tex);
    }
    if (handoff_ready()) {
        handoff_sync(dev);
        const bool copy = !(g_dbg & 2);
        ID3D11Texture2D* eyes = stereo && copy ? handoff_copy(g_own11[0], g_eyetex.t11) : nullptr;
        ID3D11Texture2D* hud = stereo && copy ? handoff_copy(g_own11[1], g_hud.t11) : nullptr;
        ID3D11Texture2D* screen = flat && copy ? handoff_copy(g_own11[2], g_screen.t11) : nullptr;
        vrsys_submit(eyes, hud, screen);
        if (req.handoff_shot) {
            if (stereo) {
                handoff_save(eyes, req.handoff_name + "_eyes");
                handoff_save(hud, req.handoff_name + "_hud");
            } else {
                handoff_save(screen, req.handoff_name + "_screen");
            }
        }
    } else if (req.handoff_shot) {
        logf("vr handoff: no D3D11 hand-off in this run (handoff=0 or no 9Ex device)");
    }
}

void stereo_reset() {
    hud_end();
    shared_release(g_eyetex);
    shared_release(g_hud);
    shared_release(g_hudrt);
    shared_release(g_screen);
    if (g_hud_depth) g_hud_depth->Release();
    if (g_bb_resolve) g_bb_resolve->Release();
    g_hud_depth = g_bb_resolve = nullptr;
    mirror_release();
    gunmodel_reset();
    handoff_release_d3d9();
}

void stereo_request_check() { g_check_requested = true; }
int stereo_draws_this_frame() { return g_stereo_draws; }

void stereo_frame_end() {
    if (g_checking) {
        logf("vr check: %d scene draws with cObjectMatrix compared, max relative difference %.3g %s", g_check_draws, g_check_max,
             g_check_draws && g_check_max < 1e-3f ? "(engine globals match the draws)" : "");
        if (g_eyes.valid) {
            vr::Proj pg;
            vr::proj_from_matrix(g_eyes.p_game, &pg);
            logf("vr check: P_game sx %.5f sy %.5f (fov %.2f x %.2f deg) a %.6f b %.4f; camera at %.1f %.1f %.1f", pg.sx, pg.sy,
                 2 * std::atan(1 / pg.sx) * 180 / kPi, 2 * std::atan(1 / pg.sy) * 180 / kPi, pg.a, pg.b,
                 -(g_eyes.view.m[0][3] * g_eyes.view.m[0][0] + g_eyes.view.m[1][3] * g_eyes.view.m[1][0] + g_eyes.view.m[2][3] * g_eyes.view.m[2][0]),
                 -(g_eyes.view.m[0][3] * g_eyes.view.m[0][1] + g_eyes.view.m[1][3] * g_eyes.view.m[1][1] + g_eyes.view.m[2][3] * g_eyes.view.m[2][1]),
                 -(g_eyes.view.m[0][3] * g_eyes.view.m[0][2] + g_eyes.view.m[1][3] * g_eyes.view.m[1][2] + g_eyes.view.m[2][3] * g_eyes.view.m[2][2]));
        }
        g_checking = false;
    }
    if (g_check_requested) {
        g_check_requested = false;
        g_checking = true;
        g_check_max = 0;
        g_check_draws = 0;
    }
    // Frame times per mode (Present to Present), for the status line.
    {
        static double last;
        const double t = now_ms();
        if (last > 0) (g_frame_stereo && g_hud_drawn ? g_ms_stereo : g_ms_flat) += t - last;
        last = t;
    }
    if (g_frame_stereo && g_hud_drawn) ++g_frames_stereo;
    else ++g_frames_flat;
    // The next frame's mode.
    const GameState prev = g_state;
    g_state = game_state();
    if (g_vr.mode != VrMode::off && (prev.level != g_state.level || prev.cutscene != g_state.cutscene || prev.paused != g_state.paused))
        logf("game state: level %d cutscene %d paused %d", int(g_state.level), int(g_state.cutscene), int(g_state.paused));
    g_frames_since_scene = g_scene_persp ? 0 : (std::min)(g_frames_since_scene + 1, 1 << 20);
    if (g_state.level && !g_level_was) g_recentre_pending = "level start";
    g_level_was = g_state.level;
    // Entering gameplay waits 0.5 s of steady gameplay state: at a level's start the cutscene flag
    // is raised a few frames after the level appears (seen: 0.3 s of stereo before the intro).
    static ULONGLONG gameplay_since;
    const bool ok = g_vr.mode != VrMode::off && g_vr.stereo && g_state.gameplay && g_frames_since_scene < 30;
    const ULONGLONG now = GetTickCount64();
    if (!ok) gameplay_since = 0;
    else if (!gameplay_since) gameplay_since = now;
    const bool next = ok && (g_frame_stereo || now - gameplay_since >= 500);
    if (next != g_frame_stereo) {
        logf("stereo: %s (level %d cutscene %d paused %d, scene %s)", next ? "gameplay - stereo eyes + HUD panel" : "flat screen",
             int(g_state.level), int(g_state.cutscene), int(g_state.paused), g_frames_since_scene < 30 ? "drawn" : "not drawn");
        if (next && g_recentre_pending) {
            vrsys_recentre(g_recentre_pending);
            g_recentre_pending = nullptr;
        }
        // Leaving gameplay (pause, death screen, a cutscene, a loading screen): the flat screen is
        // world-locked where the view was; recentre when gameplay comes back (continue, restart, the
        // next chapter), so the rail camera's direction is ahead of wherever the player now faces.
        if (!next && g_vr.recentre_resume && !g_recentre_pending) g_recentre_pending = "gameplay resumed after a flat screen";
    }
    g_frame_stereo = next;
    g_extra_drawn = false;
    g_hud_drawn = false;
    g_bb_drawn = false;
    g_bb_comp = false;
    g_last_stereo = g_stereo_draws;
    g_last_mono = g_scene_mono_draws;
    g_last_overlay = g_overlay_draws;
    g_last_crosshair_skips = g_crosshair_skips;
    g_stereo_draws = g_scene_mono_draws = g_overlay_draws = g_scene_persp = g_crosshair_skips = 0;
    ++g_frame_no;
}

const char* stereo_status() {
    if (g_vr.mode == VrMode::off) return "";
    snprintf(g_status, sizeof g_status,
             ", stereo %s: %d scene draws x2, %d overlays x2, %d passes once, %d crosshair skipped; frames %d stereo (%.1f fps) %d flat (%.1f fps); vr present %.2f ms%s%s",
             g_vr.stereo ? "on" : "off", g_last_stereo, g_last_overlay, g_last_mono, g_last_crosshair_skips, g_frames_stereo,
             g_ms_stereo > 0 ? g_frames_stereo * 1000.0 / g_ms_stereo : 0.0, g_frames_flat, g_ms_flat > 0 ? g_frames_flat * 1000.0 / g_ms_flat : 0.0,
             g_present_n ? g_present_ms / g_present_n : 0.0, ex_status(), handoff_status());
    g_present_ms = g_ms_stereo = g_ms_flat = 0;
    g_present_n = 0;
    g_frames_stereo = g_frames_flat = 0;
    return g_status;
}

}  // namespace hv
