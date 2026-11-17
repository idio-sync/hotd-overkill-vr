// The game's own weapon model in the gun hand (design: "The weapon model", milestone 3).
//
// What the game has (docs/findings-probe.md "The weapon model"): every weapon's hud pack
// (<level>_hud_<weapon>.pc) holds the first-person mesh GunArms_<Weapon> and its magazine
// GunArms_<Weapon>_Clip as BlitzTech actors (ACTR). At level start the game makes an entity for
// each, with an actor instance, for every equipped weapon - and never draws them. The weapon data
// block (cWeapon+4) lists those entities as (hash, 0, entity*) triples from +0x28.
//
// How we draw them (option 2 of the design: the engine's own GPU buffers, our own draw):
//   cWeapon+4 = data -> entity E (a triple) -> the actor instance I near E (vtable 0x80FB58,
//   I+0xF0 == E, I+0x8C = the actor resource ACTR) -> ACTR+0x28 node -> node+0xB0 mesh:
//   mesh+0 vertex count, +4 vertex buffer record {IDirect3DVertexBuffer9*, 0, count, stride},
//   +0x0C primitive groups {count, material, texture list} (the list pointer points at the first
//   texture record, its count is the dword before it),
//   +0x44 index count, +0x48 index buffer record {IDirect3DIndexBuffer9*, 0, count, 2, format}.
//   A texture record W has W+0x20 == W and W+0x24 = IDirect3DTexture9*; the list is diffuse,
//   normal map, specular (e.g. handgun_g_t, _n, _s).
// Every pointer is checked (exception-guarded reads, the D3D objects' own type and size) and the
// D3D objects are AddRef'd while we use them; everything is dropped when the level ends.
//
// Drawing: in stereo frames, once per frame, just before the game leaves the scene target after
// its opaque pass (stereo.cpp stereo_before_rt), into each eye's half with that eye's matrices and
// the scene's depth buffer - so the world occludes the gun and the game's post-processing (bloom)
// treats it like the rest of the scene. Our own shaders: the diffuse texture lit by a fixed
// view-space key light plus ambient, with the specular map; no normal map.
//
// Placement (src/vr/gunpose.h): each mesh is posed differently, so each weapon has a description
// (barrel direction, muzzle point, grip point, up, magazine drop direction; defaults measured from
// the meshes with tools/gun_mesh.py). The grip point sits at gun_grip in the controller's frame;
// the barrel is parallel to the controller's pointing axis (unless hold_<weapon> tilts it), and
// the shot ray is the barrel line itself (gun.cpp), so the barrel points exactly where shots go.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "../vr/gunpose.h"

struct ID3DXBuffer : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};

namespace hv {

constexpr uintptr_t kGameControl = 0x99B708;  // +0x24 player 1
constexpr uintptr_t kViewVa = 0x9C7E48;
constexpr uintptr_t kInstanceVtbl = 0x80FB58;  // actor instance (engine class, no RTTI)

// --- weapons ---

enum ClipMode { kClipNone = 0, kClipDrop = 1, kClipFixed = 2 };

struct WeaponDef {
    const char* actr;  // the actor's name (ACTR+0x6C)
    const char* key;   // ini suffix
    vr::GunModelDesc desc;
    int clip;  // ClipMode default
    bool show;
};

// Model units are centimetres. Measured with tools/gun_mesh.py (bounding boxes, slices along the
// barrel, side/top views): see docs/findings-probe.md "The weapon model".
static const WeaponDef kDefs[] = {
    {"GunArms_Magnum", "magnum", {{1, 0, 0}, {0, 1, 0}, {23.4f, 8.4f, -0.87f}, {0.0f, 1.0f, -0.87f}, {-0.25f, -0.97f, 0}}, kClipDrop, true},
    {"GunArms_Gwendalin", "gold_magnum", {{0, 0, 1}, {0, 1, 0}, {0, 5.3f, 16.3f}, {0, -3.0f, -11.5f}, {0, -1, 0}}, kClipNone, true},
    {"GunArms_HandCannon", "handcannon", {{1, 0, 0}, {0, 1, 0}, {33.5f, 9.6f, -0.5f}, {-2.0f, 0.5f, -0.5f}, {0, -1, 0}}, kClipNone, true},
    {"GunArms_ShotgunPump", "pump", {{1, 0, 0}, {0, 1, 0}, {61.7f, 12.0f, -0.47f}, {-8.0f, 4.0f, -0.5f}, {0, -1, 0}}, kClipNone, true},
    {"GunArms_ShotGunAuto", "autoshotgun", {{1, 0, 0}, {0, 1, 0}, {73.3f, 12.0f, -0.75f}, {1.0f, 2.0f, -0.7f}, {0, -1, 0}}, kClipDrop, true},
    {"GunArms_SMG", "smg", {{1, 0, 0}, {0, 1, 0}, {28.3f, 11.5f, -0.72f}, {2.0f, 1.5f, -0.9f}, {0, -1, 0}}, kClipDrop, true},
    {"GunArms_AssaultRifle", "rifle", {{0.975f, -0.069f, -0.212f}, {0, 1, 0}, {52.9f, 2.7f, -11.05f}, {-3.0f, -1.0f, 0.8f}, {0, -1, 0}}, kClipDrop, true},
    {"GunArms_Minigun", "minigun", {{0, 0, 1}, {0, 1, 0}, {0, 0, 51.9f}, {0, 7.0f, 4.0f}, {0, -1, 0}}, kClipFixed, true},
    {"GunArms_CrossBow", "crossbow", {{1, 0, 0}, {0, 1, 0}, {46.7f, 7.2f, -0.9f}, {1.0f, 1.0f, -0.9f}, {0, -1, 0}}, kClipNone, true},
};

struct WeaponCfg {
    vr::GunModelDesc desc;
    float hold[2] = {0, 0};  // yaw, pitch (degrees)
    int clip;
    bool show;
};

static std::mutex g_mx;
static std::map<std::string, WeaponCfg> g_cfg;  // by key

static const WeaponDef* def_by_actr(const char* name) {
    for (const WeaponDef& d : kDefs)
        if (!_stricmp(d.actr, name)) return &d;
    return nullptr;
}

void gunmodel_load_config() {
    std::lock_guard lock(g_mx);
    g_cfg.clear();
    for (const WeaponDef& d : kDefs) {
        WeaponCfg c{d.desc, {0, 0}, d.clip, d.show};
        const std::string k = d.key;
        float v[3];
        if (vr_ini_floats(("show_" + k).c_str(), v, 1)) c.show = v[0] != 0;
        if (vr_ini_floats(("clip_" + k).c_str(), v, 1)) c.clip = int(v[0]);
        vr_ini_floats(("grip_" + k).c_str(), c.desc.grip, 3);
        vr_ini_floats(("barrel_" + k).c_str(), c.desc.barrel, 3);
        vr_ini_floats(("muzzle_" + k).c_str(), c.desc.muzzle, 3);
        vr_ini_floats(("hold_" + k).c_str(), c.hold, 2);
        g_cfg[k] = c;
    }
}

// --- the resident mesh ---

struct Mesh {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DTexture9* tex[3] = {};
    UINT nv = 0, stride = 0, nidx = 0;
    D3DFORMAT ifmt = D3DFMT_INDEX16;
    char name[64] = {};
    uintptr_t vb_rec = 0;  // the engine's vertex buffer record: still holding vb = still loaded
};

struct Model {
    bool ok = false;
    const WeaponDef* def = nullptr;
    Mesh gun, clip;
    bool has_clip = false;
    uintptr_t data = 0;  // cWeapon+4 when resolved
};

static void release(Mesh& m) {
    if (m.vb) m.vb->Release();
    if (m.ib) m.ib->Release();
    for (auto*& t : m.tex)
        if (t) t->Release(), t = nullptr;
    m.vb = nullptr, m.ib = nullptr;
}

static std::map<uintptr_t, Model> g_models;  // by cWeapon
static bool g_level_was;

static uintptr_t rd(uintptr_t a) {
    uintptr_t v = 0;
    return safe_copy(&v, a, 4) ? v : 0;
}

static bool heap_ptr(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF0000 && !(p & 3); }
// Pack data is not always aligned (the SMG's primitive groups sit at an odd offset).
static bool pack_ptr(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF0000; }

// D3D objects behind pointers read from game memory: checked through their own methods under an
// exception guard before we take a reference.
static bool seh_vb(IDirect3DVertexBuffer9* vb, D3DVERTEXBUFFER_DESC* d) {
    __try {
        return SUCCEEDED(vb->GetDesc(d)) && d->Type == D3DRTYPE_VERTEXBUFFER;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool seh_ib(IDirect3DIndexBuffer9* ib, D3DINDEXBUFFER_DESC* d) {
    __try {
        return SUCCEEDED(ib->GetDesc(d)) && d->Type == D3DRTYPE_INDEXBUFFER;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool seh_tex(IDirect3DTexture9* t, D3DSURFACE_DESC* d) {
    __try {
        return t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(t->GetLevelDesc(0, d));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const char* fmt4(D3DFORMAT f, char* buf) {
    if (f > 0xFFFF) snprintf(buf, 8, "%.4s", reinterpret_cast<const char*>(&f));
    else snprintf(buf, 8, "%d", int(f));
    return buf;
}

// ACTR -> its single mesh's buffers and textures.
static bool resolve_mesh(uintptr_t actr, Mesh* out) {
    uint32_t hdr[4] = {};
    if (!safe_copy(hdr, actr, 16) || hdr[1] != 0x10010000) return false;
    const uintptr_t name_p = rd(actr + 0x6C), node = rd(actr + 0x28);
    if (!safe_copy(out->name, name_p, sizeof out->name - 1)) return false;
    out->name[sizeof out->name - 1] = 0;
    const uintptr_t mesh = pack_ptr(node) ? rd(node + 0xB0) : 0;
    if (!pack_ptr(mesh)) return false;
    const uint32_t nv = uint32_t(rd(mesh)), nidx = uint32_t(rd(mesh + 0x44));
    const uintptr_t vbr = rd(mesh + 4), ibr = rd(mesh + 0x48), groups = rd(mesh + 0x0C);
    uint32_t vrec[4] = {}, irec[5] = {};
    if (!heap_ptr(vbr) || !heap_ptr(ibr) || !safe_copy(vrec, vbr, 16) || !safe_copy(irec, ibr, 20)) return false;
    if (vrec[2] != nv || irec[2] != nidx || nv == 0 || nidx < 3 || vrec[3] < 32 || vrec[3] > 128) {
        logf("gun model: %s: unexpected buffer records (verts %u/%u stride %u, indices %u/%u)", out->name, nv, vrec[2], vrec[3], nidx, irec[2]);
        return false;
    }
    auto vb = reinterpret_cast<IDirect3DVertexBuffer9*>(uintptr_t(vrec[0]));
    auto ib = reinterpret_cast<IDirect3DIndexBuffer9*>(uintptr_t(irec[0]));
    D3DVERTEXBUFFER_DESC vd{};
    D3DINDEXBUFFER_DESC id{};
    if (!seh_vb(vb, &vd) || !seh_ib(ib, &id) || vd.Size < nv * vrec[3] ||
        id.Size < nidx * (id.Format == D3DFMT_INDEX32 ? 4u : 2u)) {
        logf("gun model: %s: the buffers are not the expected D3D objects", out->name);
        return false;
    }
    out->vb_rec = vbr;
    out->vb = vb, out->ib = ib, out->nv = nv, out->stride = vrec[3], out->nidx = nidx, out->ifmt = id.Format;
    vb->AddRef();
    ib->AddRef();
    // Textures: diffuse, normal, specular.
    const uintptr_t tl = pack_ptr(groups) ? rd(groups + 8) : 0;
    const uint32_t nt = pack_ptr(tl) ? uint32_t(rd(tl - 4)) : 0;  // the list pointer points past its count
    char line[256];
    int len = snprintf(line, sizeof line, "gun model: %s: %u verts (stride %u), %u indices, %u textures:", out->name, nv, vrec[3], nidx, nt);
    for (uint32_t k = 0; k < nt && k < 3; ++k) {
        const uintptr_t w = rd(tl + 4 * k);
        if (!heap_ptr(w) || rd(w + 0x20) != w) continue;
        auto t = reinterpret_cast<IDirect3DTexture9*>(rd(w + 0x24));
        D3DSURFACE_DESC d{};
        if (!t || !seh_tex(t, &d)) continue;
        t->AddRef();
        out->tex[k] = t;
        char f[8];
        len += snprintf(line + len, sizeof line - len, " %ux%u %s", d.Width, d.Height, fmt4(d.Format, f));
    }
    logf("%s", line);
    return true;
}

// The actor instance of an entity: within the entity's allocation (seen at E+0x290).
static uintptr_t instance_of(uintptr_t entity) {
    static uint32_t buf[0x1000 / 4];
    if (!safe_copy(buf, entity, sizeof buf)) return 0;
    const uint32_t vt = uint32_t(va(kInstanceVtbl));
    for (size_t i = 0; i + 0x100 / 4 < sizeof buf / 4; ++i)
        if (buf[i] == vt && buf[i + 0xF0 / 4] == entity && heap_ptr(buf[i + 0x8C / 4]) && rd(buf[i + 0x8C / 4] + 8) == buf[i + 8 / 4])
            return entity + i * 4;
    return 0;
}

static Model resolve(uintptr_t weapon) {
    Model m;
    const uintptr_t data = rd(weapon + 4);
    m.data = data;
    if (!heap_ptr(data)) return m;
    for (uintptr_t off = 0x28; off <= 0xC0; off += 4) {
        const uintptr_t e = rd(data + off);
        if (!heap_ptr(e) || e == weapon) continue;
        const uintptr_t inst = instance_of(e);
        if (!inst) continue;
        const uintptr_t actr = rd(inst + 0x8C);
        char name[64] = {};
        if (!safe_copy(name, rd(actr + 0x6C), sizeof name - 1) || _strnicmp(name, "GunArms_", 8)) continue;
        const size_t n = strlen(name);
        const bool clip = n > 5 && !_stricmp(name + n - 5, "_Clip");
        Mesh& dst = clip ? m.clip : m.gun;
        if (dst.vb) continue;
        if (!resolve_mesh(actr, &dst)) continue;
        if (clip) m.has_clip = true;
        else m.def = def_by_actr(name);
    }
    m.ok = m.gun.vb != nullptr;
    if (m.ok && !m.def) logf("gun model: %s is not a known weapon model; not drawn", m.gun.name);
    if (!m.ok) logf("gun model: weapon %p (data %p) has no resident GunArms model; no model drawn", reinterpret_cast<void*>(weapon),
                    reinterpret_cast<void*>(data));
    m.ok = m.ok && m.def;
    return m;
}

static void release_all() {
    for (auto& [w, m] : g_models) release(m.gun), release(m.clip);
    if (!g_models.empty()) logf("gun model: level ended, %zu weapon model(s) released", g_models.size());
    g_models.clear();
}

static uintptr_t current_weapon() {
    const uintptr_t gc = *reinterpret_cast<const volatile uintptr_t*>(va(kGameControl));
    const uintptr_t p = gc ? rd(gc + 0x24) : 0;
    return p ? rd(p + 0x224) : 0;
}

// The current weapon's model (resolving it the first time); null if none. Call with g_mx held.
static const Model* current_model(uintptr_t* weapon_out = nullptr) {
    const GameState st = game_state();
    if (g_level_was && !st.level) release_all();
    g_level_was = st.level;
    if (!st.level) return nullptr;
    const uintptr_t w = current_weapon();
    if (weapon_out) *weapon_out = w;
    if (!heap_ptr(w)) return nullptr;
    auto it = g_models.find(w);
    // A cached model whose weapon or buffers changed (a new weapon object at an old address, or
    // the pack reloaded) is resolved again.
    if (it != g_models.end() && (rd(w + 4) != it->second.data || (it->second.ok && rd(it->second.gun.vb_rec) != uintptr_t(it->second.gun.vb)))) {
        release(it->second.gun), release(it->second.clip);
        g_models.erase(it);
        it = g_models.end();
    }
    if (it == g_models.end()) {
        it = g_models.emplace(w, resolve(w)).first;
        if (it->second.ok)
            logf("gun model: weapon %p -> %s (%s)%s", reinterpret_cast<void*>(w), it->second.gun.name, it->second.def->key,
                 it->second.has_clip ? " with its magazine" : "");
    }
    return it->second.ok ? &it->second : nullptr;
}

// Once per Present: models are dropped when the level ends (menus, quitting, the next level).
void gunmodel_frame() {
    if (g_vr.mode == VrMode::off) return;
    std::lock_guard lock(g_mx);
    const bool level = game_state().level;
    if (g_level_was && !level) release_all();
    g_level_was = level;
}

// --- placement ---

static std::atomic<bool> g_enabled{true};
static std::atomic<float> g_force_clip{-1};
static bool g_barrel_line;

void gunmodel_enable(bool on) { g_enabled = on; }
void gunmodel_force_clip(float f) { g_force_clip = f; }
void gunmodel_barrel_line(bool on) { g_barrel_line = on; }

static float g_metres_per_unit() { return 0.01f * g_vr.gun_scale; }

// The gun hand's grip point in the hand's (left-handed) frame: [vr] gun_grip is x right, y up,
// z back like hand_offset.
static void grip_in_hand(float out[3]) { out[0] = g_vr.gun_grip[0], out[1] = g_vr.gun_grip[1], out[2] = -g_vr.gun_grip[2]; }

struct Placement {
    vr::GunModelDesc desc;
    vr::Mat4 gun;   // gun frame in the hand frame
    vr::Mat4 m2g;   // model -> gun frame
    int clip;
    const Model* model;
};

static bool placement(Placement* p) {
    if (g_vr.mode == VrMode::off || !g_vr.gun_model || !g_enabled) return false;
    const Model* m = current_model();
    if (!m) return false;
    auto it = g_cfg.find(m->def->key);
    if (it == g_cfg.end() || !it->second.show) return false;
    const WeaponCfg& c = it->second;
    float grip[3];
    grip_in_hand(grip);
    p->desc = c.desc;
    p->m2g = vr::model_to_gun(c.desc, g_metres_per_unit());
    p->gun = vr::gun_in_hand(c.desc, g_metres_per_unit(), grip, c.hold[0], c.hold[1]);
    p->clip = m->has_clip ? c.clip : kClipNone;
    p->model = m;
    return true;
}

bool gunmodel_barrel(const vr::Mat4& hand_lh, vr::Ray* cam_ray) {
    if (!g_vr.aim_barrel) return false;
    std::lock_guard lock(g_mx);
    Placement p;
    if (!placement(&p)) return false;
    *cam_ray = vr::barrel_ray(vr::mul(hand_lh, p.gun));
    return true;
}

// --- drawing ---

static IDirect3DVertexShader9* g_vs;
static IDirect3DPixelShader9* g_ps;
static IDirect3DVertexShader9* g_vs_line;
static IDirect3DPixelShader9* g_ps_line;
static IDirect3DVertexDeclaration9* g_decl;
static IDirect3DVertexDeclaration9* g_decl_line;
static IDirect3DTexture9* g_white;
static IDirect3DStateBlock9* g_sb;
static bool g_shader_failed;

static const char kVs[] = R"(
float4 cClip[4] : register(c0);
float4 cView[4] : register(c4);
struct I { float4 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct O { float4 p : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 v : TEXCOORD2; };
O main(I i) {
    O o;
    float4 p = float4(i.p.xyz, 1);
    o.p = float4(dot(p, cClip[0]), dot(p, cClip[1]), dot(p, cClip[2]), dot(p, cClip[3]));
    o.v = float3(dot(p, cView[0]), dot(p, cView[1]), dot(p, cView[2]));
    float4 n = float4(i.n, 0);
    o.n = float3(dot(n, cView[0]), dot(n, cView[1]), dot(n, cView[2]));
    o.uv = i.uv;
    return o;
}
)";

static const char kPs[] = R"(
sampler2D sDiffuse : register(s0);
sampler2D sSpec : register(s1);
float4 cLight : register(c0);   // xyz: view-space direction to the key light
float4 cParams : register(c1);  // ambient, diffuse, specular, specular map on
float4 cFace : register(c2);    // x: sign that makes VFACE positive on front faces
float4 main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1, float3 v : TEXCOORD2, float face : VFACE) : COLOR {
    float3 N = normalize(n) * ((face * cFace.x) >= 0 ? 1 : -1);
    float3 L = normalize(cLight.xyz);
    float3 V = normalize(-v);
    float3 H = normalize(L + V);
    float4 t = tex2D(sDiffuse, uv);
    float3 s = cParams.w > 0 ? tex2D(sSpec, uv).rgb : float3(0.25, 0.25, 0.25);
    float d = saturate(dot(N, L));
    float r = 0.5 + 0.5 * saturate(dot(N, float3(0, 1, 0)));  // a little more light from above
    float3 c = t.rgb * (cParams.x * r + cParams.y * d) + s * cParams.z * pow(saturate(dot(N, H)), 24);
    return float4(c, 1);
}
)";

static const char kVsLine[] = R"(
float4 cClip[4] : register(c0);
float4 main(float4 p : POSITION) : POSITION {
    float4 q = float4(p.xyz, 1);
    return float4(dot(q, cClip[0]), dot(q, cClip[1]), dot(q, cClip[2]), dot(q, cClip[3]));
}
)";
static const char kPsLine[] = "float4 c : register(c0); float4 main() : COLOR { return c; }";

using Compile_t = HRESULT(WINAPI*)(LPCSTR, UINT, const void*, void*, LPCSTR, LPCSTR, DWORD, ID3DXBuffer**, ID3DXBuffer**, void**);

static ID3DXBuffer* compile(const char* src, const char* profile) {
    static Compile_t fn = [] {
        HMODULE m = GetModuleHandleW(L"d3dx9_43.dll");
        return m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DXCompileShader")) : nullptr;
    }();
    if (!fn) {
        logf("gun model: D3DXCompileShader not found");
        return nullptr;
    }
    ID3DXBuffer *code = nullptr, *err = nullptr;
    const HRESULT hr = fn(src, UINT(strlen(src)), nullptr, nullptr, "main", profile, 0, &code, &err, nullptr);
    if (FAILED(hr)) logf("gun model: shader compile (%s) failed %08lx: %s", profile, hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

static bool ensure_resources(IDirect3DDevice9* dev) {
    if (g_vs && g_ps && g_decl && g_white && g_sb && g_vs_line) return true;
    if (g_shader_failed) return false;
    g_shader_failed = true;
    ID3DXBuffer *vs = compile(kVs, "vs_3_0"), *ps = compile(kPs, "ps_3_0"), *vsl = compile(kVsLine, "vs_3_0"), *psl = compile(kPsLine, "ps_3_0");
    bool ok = vs && ps && vsl && psl;
    // (Created through the hooked device: the hook only hashes the bytecode and names its constants.)
    if (ok) ok = SUCCEEDED(dev->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &g_vs)) &&
                 SUCCEEDED(dev->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()), &g_ps)) &&
                 SUCCEEDED(dev->CreateVertexShader(static_cast<const DWORD*>(vsl->GetBufferPointer()), &g_vs_line)) &&
                 SUCCEEDED(dev->CreatePixelShader(static_cast<const DWORD*>(psl->GetBufferPointer()), &g_ps_line));
    for (ID3DXBuffer* b : {vs, ps, vsl, psl})
        if (b) b->Release();
    // vf_stock_staticsinglenormalmapped: position, normal, colour, uv, tangent (52 bytes).
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                    D3DDECL_END()};
    const D3DVERTEXELEMENT9 ell[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (ok) ok = SUCCEEDED(dev->CreateVertexDeclaration(el, &g_decl)) && SUCCEEDED(dev->CreateVertexDeclaration(ell, &g_decl_line));
    if (ok) {
        // 1x1 white (for a model without textures); DEFAULT pool, filled by a clear-free path:
        // a dynamic texture we lock (D3D9Ex has no MANAGED pool).
        ok = SUCCEEDED(raw_create_texture(dev, 1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_white, nullptr));
        D3DLOCKED_RECT lr;
        if (ok && SUCCEEDED(g_white->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
            *static_cast<DWORD*>(lr.pBits) = 0xFFB0B0B0;
            g_white->UnlockRect(0);
        }
    }
    if (ok) ok = SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb));
    logf("gun model: shaders and states %s", ok ? "ready" : "FAILED (no gun model)");
    g_shader_failed = !ok;
    return ok;
}

void gunmodel_reset() {
    std::lock_guard lock(g_mx);
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&g_vs), reinterpret_cast<IUnknown**>(&g_ps), reinterpret_cast<IUnknown**>(&g_vs_line),
                         reinterpret_cast<IUnknown**>(&g_ps_line), reinterpret_cast<IUnknown**>(&g_decl),
                         reinterpret_cast<IUnknown**>(&g_decl_line), reinterpret_cast<IUnknown**>(&g_white),
                         reinterpret_cast<IUnknown**>(&g_sb)})
        if (*p) (*p)->Release(), *p = nullptr;
    g_shader_failed = false;
}

static void set_matrix(IDirect3DDevice9* dev, UINT reg, const vr::Mat4& m) { raw_set_vs_const(dev, reg, &m.m[0][0], 4); }

static void draw_mesh(IDirect3DDevice9* dev, const Mesh& m) {
    dev->SetStreamSource(0, m.vb, 0, m.stride);
    dev->SetIndices(m.ib);
    raw_set_texture(dev, 0, m.tex[0] ? m.tex[0] : g_white);
    raw_set_texture(dev, 1, m.tex[2] ? m.tex[2] : g_white);
    const float params[4] = {g_vr.gun_light * 0.45f, g_vr.gun_light * 0.75f, 0.6f, m.tex[2] ? 1.f : 0.f};
    dev->SetPixelShaderConstantF(1, params, 1);
    raw_dip(dev, D3DPT_TRIANGLELIST, 0, 0, m.nv, 0, m.nidx / 3);  // past the per-eye duplication: we draw each eye ourselves
}

static int g_draws_logged;

// Called by stereo.cpp with the scene target and its depth bound, in a stereo frame. eye_clip(eye)
// gives world -> clip for that eye and sets its viewport.
void gunmodel_draw(IDirect3DDevice9* dev, bool (*eye_setup)(IDirect3DDevice9*, int, vr::Mat4*)) {
    std::lock_guard lock(g_mx);
    Placement p;
    vr::Mat4 hand, view;
    float vraw[16];
    if (!placement(&p) || !vrsys_hand_in_game(&hand) || !safe_copy(vraw, va(kViewVa), sizeof vraw)) return;
    view = vr::from_d3dx(vraw);
    if (!ensure_resources(dev)) return;
    vr::Mat4 world;
    if (!vr::gun_world_matrix(view, g_vr.unit_m, hand, p.gun, p.m2g, &world)) return;
    // The magazine: along clip_dir (model space) while reloading.
    vr::Mat4 clip_world = world;
    bool draw_clip = p.clip == kClipFixed;
    if (p.clip == kClipDrop) {
        float out = 0;
        const uintptr_t w = current_weapon();
        uint8_t reloading = 0;
        float progress = 0;
        if (w) safe_copy(&reloading, w + 0x764, 1), safe_copy(&progress, w + 0x76C, 4);
        out = g_vr.clip_anim ? vr::clip_out_fraction(reloading != 0, progress) : 0;
        if (g_force_clip >= 0) out = g_force_clip;
        float d[3] = {p.desc.clip_dir[0], p.desc.clip_dir[1], p.desc.clip_dir[2]};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float dist = out * g_vr.clip_drop / g_metres_per_unit();  // model units
        if (n > 0) clip_world = vr::mul(world, vr::translation(d[0] / n * dist, d[1] / n * dist, d[2] / n * dist));
        draw_clip = true;
    }
    const vr::Mat4 wv = vr::mul(view, world);
    g_sb->Capture();
    raw_set_vs(dev, g_vs);
    raw_set_ps(dev, g_ps);
    dev->SetVertexDeclaration(g_decl);
    const struct { D3DRENDERSTATETYPE s; DWORD v; } rs[] = {
        {D3DRS_ZENABLE, TRUE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_ZFUNC, D3DCMP_LESSEQUAL}, {D3DRS_ALPHABLENDENABLE, FALSE},
        {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}};
    for (auto& r : rs) raw_set_rs(dev, r.s, r.v);
    for (DWORD s = 0; s < 2; ++s) {
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    // Key light from above, a little left and from behind the viewer (view space, +z ahead).
    const float light[4] = {-0.35f, 0.8f, -0.5f, 0}, face[4] = {g_vr.gun_face_sign, 0, 0, 0};
    dev->SetPixelShaderConstantF(0, light, 1);
    dev->SetPixelShaderConstantF(2, face, 1);
    set_matrix(dev, 4, wv);
    for (int eye = 0; eye < 2; ++eye) {
        vr::Mat4 clip;
        if (!eye_setup(dev, eye, &clip)) continue;
        set_matrix(dev, 0, vr::mul(clip, world));
        draw_mesh(dev, p.model->gun);
        if (draw_clip) {
            set_matrix(dev, 0, vr::mul(clip, clip_world));
            set_matrix(dev, 4, vr::mul(view, clip_world));
            draw_mesh(dev, p.model->clip);
            set_matrix(dev, 4, wv);
        }
    }
    // Development: the barrel line from the muzzle to the aim point.
    float aim[3];
    bool hit;
    if (g_barrel_line && gun_aim_point(aim, &hit)) {
        vr::Ray r;
        if (vr::camera_ray_to_world(view, vr::barrel_ray(vr::mul(hand, p.gun)), g_vr.unit_m, &r)) {
            const float pts[2][3] = {{r.o[0], r.o[1], r.o[2]}, {aim[0], aim[1], aim[2]}};
            raw_set_vs(dev, g_vs_line);
            raw_set_ps(dev, g_ps_line);
            dev->SetVertexDeclaration(g_decl_line);
            raw_set_rs(dev, D3DRS_ZENABLE, FALSE);
            const float col[4] = {1, 0.1f, 0.1f, 1};
            dev->SetPixelShaderConstantF(0, col, 1);
            for (int eye = 0; eye < 2; ++eye) {
                vr::Mat4 clip;
                if (!eye_setup(dev, eye, &clip)) continue;
                set_matrix(dev, 0, clip);
                raw_dpup(dev, D3DPT_LINELIST, 1, pts, 12);
            }
        }
    }
    g_sb->Apply();
    if (g_draws_logged++ == 0) logf("gun model: first draw (%s)", p.model->gun.name);
}

void gunmodel_log() {
    std::lock_guard lock(g_mx);
    uintptr_t w = 0;
    const Model* m = current_model(&w);
    logf("vr gunmodel: enabled %d (gun_model=%d), weapon %p -> %s", int(g_enabled.load()), int(g_vr.gun_model), reinterpret_cast<void*>(w),
         m ? m->gun.name : "(no model)");
    for (auto& [k, c] : g_cfg)
        logf("  %-12s show %d clip %d grip %.2f,%.2f,%.2f barrel %.3f,%.3f,%.3f muzzle %.2f,%.2f,%.2f hold %.1f,%.1f", k.c_str(), int(c.show),
             c.clip, c.desc.grip[0], c.desc.grip[1], c.desc.grip[2], c.desc.barrel[0], c.desc.barrel[1], c.desc.barrel[2], c.desc.muzzle[0],
             c.desc.muzzle[1], c.desc.muzzle[2], c.hold[0], c.hold[1]);
    vr::Mat4 hand;
    Placement p;
    if (m && vrsys_hand_in_game(&hand) && placement(&p)) {
        const vr::Ray r = vr::barrel_ray(vr::mul(hand, p.gun));
        logf("  barrel (camera space, m): muzzle %.3f %.3f %.3f dir %.4f %.4f %.4f; hand at %.3f %.3f %.3f dir %.4f %.4f %.4f", r.o[0], r.o[1],
             r.o[2], r.d[0], r.d[1], r.d[2], hand.m[0][3], hand.m[1][3], hand.m[2][3], hand.m[0][2], hand.m[1][2], hand.m[2][2]);
    }
}

// `vr gunset KEY FIELD V...`: live per-weapon tuning (show, clip, grip, barrel, muzzle, hold).
void gunmodel_set(const std::string& key, const std::string& field, const float* v, int n) {
    std::lock_guard lock(g_mx);
    auto it = g_cfg.find(key);
    if (it == g_cfg.end()) {
        logf("vr gunset: unknown weapon '%s'", key.c_str());
        return;
    }
    WeaponCfg& c = it->second;
    if (field == "show" && n >= 1) c.show = v[0] != 0;
    else if (field == "clip" && n >= 1) c.clip = int(v[0]);
    else if (field == "grip" && n >= 3) memcpy(c.desc.grip, v, 12);
    else if (field == "barrel" && n >= 3) memcpy(c.desc.barrel, v, 12);
    else if (field == "muzzle" && n >= 3) memcpy(c.desc.muzzle, v, 12);
    else if (field == "clipdir" && n >= 3) memcpy(c.desc.clip_dir, v, 12);
    else if (field == "hold" && n >= 2) c.hold[0] = v[0], c.hold[1] = v[1];
    else logf("vr gunset: bad field/values '%s'", field.c_str());
}

}  // namespace hv
