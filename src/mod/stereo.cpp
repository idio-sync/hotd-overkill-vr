// Same-frame stereo by per-draw duplication (design: "Rendering").
//
// Screen-sized render targets (the 1280x720 scene target and its depth, the bloom/blur chain
// 640x180, 320x180, 160x90, 640x360, and the 1291x807 composite target with its depth) are created
// double-wide at eye resolution: width x 2*eye_w/1280, height x eye_h/720. Shadow maps and other
// targets keep their size. The game keeps thinking in its own sizes: viewports, scissor rects and
// rects it passes for those targets are scaled, and GetViewport/GetScissorRect answer in its sizes.
//
// On the scene target (1280x720 class), every draw whose clip-space matrix is a perspective one is
// issued twice: the left eye into the left half, the right eye into the right half, each with
// cWVP_eye = C_eye * cWVP (C_eye = P_eye * E_eye * P_game^-1, src/vr/stereo.h) for each clip
// matrix the shader declares (cWVP, cProject, cObjectViewProject, cProj) and eyePos moved to the
// eye. Everything else (full-screen passes, orthographic draws, other targets) is drawn once over
// the whole double-wide target. P_game and the view come from the engine's globals at draw time:
// projection 0x9C7DC0 and view 0x9C7E48 (D3DX row-vector matrices); `vr check` compares them
// with cWVP * cObjectMatrix^-1 of the draws.
#include "common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../vr/stereo.h"

namespace hv {

constexpr uintptr_t kProjVa = 0x9C7DC0, kViewVa = 0x9C7E48;
constexpr float kPi = 3.14159265f;

struct Target {
    UINT ow, oh;  // the game's size
    UINT w, h;    // the real size
};

static std::mutex g_mx;
static std::unordered_map<IDirect3DSurface9*, Target> g_targets;

// Render target 0.
static IDirect3DSurface9* g_rt;
static Target g_rt_info;
static bool g_rt_resized, g_rt_scene;
static IDirect3DSurface9* g_scene_surface;

static D3DVIEWPORT9 g_game_vp;
static RECT g_game_scissor;
static bool g_scissor_on;

// Per-frame stats.
static int g_stereo_draws, g_scene_mono_draws, g_overlay_draws, g_last_stereo, g_last_mono, g_last_overlay;
static char g_status[128];
static std::unordered_map<void*, bool> g_target_textures;
static bool g_check_requested, g_checking;

static bool screen_sized(UINT w, UINT h, bool depth) {
    static const UINT colour[][2] = {{1280, 720}, {640, 180}, {320, 180}, {160, 90}, {640, 360}, {1291, 807}};
    static const UINT depths[][2] = {{1280, 720}, {1291, 807}};
    if (depth) {
        for (auto& s : depths)
            if (s[0] == w && s[1] == h) return true;
        return false;
    }
    for (auto& s : colour)
        if (s[0] == w && s[1] == h) return true;
    return false;
}

bool stereo_target_size(UINT w, UINT h, bool depth, UINT* out_w, UINT* out_h) {
    if (g_vr.mode == VrMode::off || !screen_sized(w, h, depth)) return false;
    *out_w = UINT(std::lround(double(w) * 2.0 * g_vr.eye_w / 1280.0));
    *out_h = UINT(std::lround(double(h) * g_vr.eye_h / 720.0));
    return true;
}

static void track(IDirect3DSurface9* s, UINT w, UINT h) {
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    std::lock_guard lock(g_mx);
    g_targets[s] = {w, h, d.Width, d.Height};
    logf("stereo target %p: %ux%u -> %ux%u", s, w, h, d.Width, d.Height);
}

void stereo_track_texture(IDirect3DTexture9* tex, UINT w, UINT h) {
    IDirect3DSurface9* s = nullptr;
    {
        std::lock_guard lock(g_mx);
        g_target_textures[tex] = true;
    }
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s) {
        track(s, w, h);
        s->Release();  // the texture keeps its level alive; the pointer stays the same
    }
}

void stereo_track_surface(IDirect3DSurface9* s, UINT w, UINT h) { track(s, w, h); }

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
    float yaw, pitch, ipd, unit;
    int eye_w, eye_h;
    bool valid;
    vr::Mat4 p_game, view;  // column form
    vr::Mat4 c[2];
    float world_off[2][3];
};
static Eyes g_eyes;

static bool update_eyes() {
    const float* p = reinterpret_cast<const float*>(va(kProjVa));
    const float* v = reinterpret_cast<const float*>(va(kViewVa));
    Eyes& e = g_eyes;
    if (e.valid && !memcmp(e.proj_raw, p, 64) && !memcmp(e.view_raw, v, 64) && e.yaw == g_vr.sim_yaw_deg &&
        e.pitch == g_vr.sim_pitch_deg && e.ipd == g_vr.ipd_m && e.unit == g_vr.unit_m && e.eye_w == g_vr.eye_w &&
        e.eye_h == g_vr.eye_h)
        return true;
    memcpy(e.proj_raw, p, 64);
    memcpy(e.view_raw, v, 64);
    e.yaw = g_vr.sim_yaw_deg, e.pitch = g_vr.sim_pitch_deg, e.ipd = g_vr.ipd_m, e.unit = g_vr.unit_m;
    e.eye_w = g_vr.eye_w, e.eye_h = g_vr.eye_h;
    e.valid = false;
    e.p_game = vr::from_d3dx(p);
    e.view = vr::from_d3dx(v);
    vr::Proj pg;
    if (!vr::proj_from_matrix(e.p_game, &pg)) {
        static int warned;
        if (warned++ < 5) logf("stereo: engine projection is not a perspective one; drawing mono");
        return false;
    }
    const vr::Proj pe = vr::sim_eye_proj(pg, float(g_vr.eye_w) / float(g_vr.eye_h));
    const vr::Mat4 head = vr::rotation_yaw_pitch(g_vr.sim_yaw_deg * kPi / 180, g_vr.sim_pitch_deg * kPi / 180);
    for (int i = 0; i < 2; ++i) {
        const vr::Mat4 pose = vr::eye_pose(head, g_vr.ipd_m, i);
        if (!vr::correction(e.p_game, pe, vr::eye_view(pose, g_vr.unit_m), &e.c[i])) return false;
        float off[3];
        vr::eye_offset_units(pose, g_vr.unit_m, off);
        for (int k = 0; k < 3; ++k)
            e.world_off[i][k] = off[0] * e.view.m[0][k] + off[1] * e.view.m[1][k] + off[2] * e.view.m[2][k];
    }
    e.valid = true;
    return true;
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
    float scale = 0;
    for (auto& r : pv.m)
        for (float x : r) scale = std::fmax(scale, std::fabs(x));
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

HRESULT stereo_draw(IDirect3DDevice9* dev, DrawFn draw, void* ctx) {
    if (!g_vr.stereo || !g_rt_scene) return draw(ctx);
    IDirect3DVertexShader9* vs = current_vs();
    const ShaderInfo* info = vs ? shader_info(vs) : nullptr;
    bool correct = info && info->n_clip;
    if (correct) {
        const float* w = vs_shadow(UINT(info->clip_regs[0] + 3));
        if (std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) < 1e-7f) correct = false;  // orthographic
    }
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

void stereo_request_check() { g_check_requested = true; }

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
    g_last_stereo = g_stereo_draws;
    g_last_mono = g_scene_mono_draws;
    g_last_overlay = g_overlay_draws;
    g_stereo_draws = g_scene_mono_draws = g_overlay_draws = 0;
}

const char* stereo_status() {
    if (g_vr.mode == VrMode::off) return "";
    snprintf(g_status, sizeof g_status, ", stereo %s: %d scene draws x2, %d overlays x2, %d passes once", g_vr.stereo ? "on" : "off",
             g_last_stereo, g_last_overlay, g_last_mono);
    return g_status;
}

}  // namespace hv
