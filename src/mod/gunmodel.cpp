// The game's own weapon model in the gun hand (milestone 3).
//
// What the game has (docs/findings-probe.md "The weapon model"): every weapon's hud pack
// (<level>_hud_<weapon>.pc) holds the first-person mesh GunArms_<Weapon> and its magazine
// GunArms_<Weapon>_Clip as BlitzTech actors (ACTR). At level start the game makes an entity for
// each, with an actor instance, for every equipped weapon - and never draws them. The weapon data
// block (cWeapon+4) lists those entities as (hash, 0, entity*) triples from +0x28.
//
// How we draw them (the engine's own GPU buffers, our own draw; rather than having the engine
// draw its model, or extracting the meshes offline, which could not ship):
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
// the meshes with tools/gun_mesh.py, the bores checked with the bore fit). The grip point sits at gun_grip in the controller's frame;
// the barrel is parallel to the controller's pointing axis (unless hold_<weapon> tilts it), and
// the shot ray is the barrel line itself (gun.cpp), so the barrel points exactly where shots go.
//
// Moving parts (gun feedback stage 1, src/vr/gunparts.h): when a model is resolved its vertex and
// index buffers are read back and split into pieces, and the pieces of each moving part (slide,
// hammer, trigger, ...; the hand cannon's cylinder and the minigun's barrels in their _Clip meshes)
// are found by their model-space boxes. Each vertex's part index and weight go into a second vertex
// stream the mod owns; the gun shader moves it by its part's matrix (model space) before the world
// matrix. The parts follow player 1's shots (gunmodel_shot), the ammo, the reload state and the
// controller's analog trigger. The shot ray never sees any of it: it is the un-moved barrel.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../vr/gunfit.h"
#include "../vr/gunparts.h"
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

// kClipPart: the _Clip is a part of the gun (the hand cannon's cylinder, the pump's forend; the
// minigun's barrel cluster with kClipFixed too): for those weapons any value but kClipNone draws it
// in place as a part (src/vr/gunparts.h).
enum ClipMode { kClipNone = 0, kClipDrop = 1, kClipFixed = 2, kClipPart = 3 };

struct WeaponDef {
    const char* actr;  // the actor's name (ACTR+0x6C)
    const char* key;   // ini suffix
    vr::GunModelDesc desc;
    int clip;  // ClipMode default
    bool show;
};

// Model units are centimetres. Measured with tools/gun_mesh.py (bounding boxes, slices along the
// barrel, side/top views): see docs/findings-probe.md "The weapon model". The muzzle points (and
// the rifle's barrel direction) of the magnum, gold magnum, hand cannon, pump, auto shotgun and
// rifle are the bore fit's (src/vr/gunfit.h, `vr gunmeshcheck`): the centre of the bore's ring at
// the muzzle face, so the shot line runs down the bore. Grip points are unchanged by that fit.
static const WeaponDef kDefs[] = {
    {"GunArms_Magnum", "magnum", {{1, 0, 0}, {0, 1, 0}, {23.436f, 9.433f, -0.87f}, {0.0f, 1.0f, -0.87f}, {-0.25f, -0.97f, 0}}, kClipDrop, true},
    {"GunArms_Gwendalin", "gold_magnum", {{0, 0, 1}, {0, 1, 0}, {-0.007f, 6.485f, 16.328f}, {0, -3.0f, -11.5f}, {0, -1, 0}}, kClipNone, true},
    {"GunArms_HandCannon", "handcannon", {{1, 0, 0}, {0, 1, 0}, {33.495f, 9.14f, -0.519f}, {-2.0f, 0.5f, -0.5f}, {0, -1, 0}}, kClipPart, true},
    {"GunArms_ShotgunPump", "pump", {{1, 0, 0}, {0, 1, 0}, {61.669f, 10.807f, -0.481f}, {-8.0f, 4.0f, -0.5f}, {0, -1, 0}}, kClipPart, true},
    {"GunArms_ShotGunAuto", "autoshotgun", {{1, 0, 0}, {0, 1, 0}, {73.283f, 12.4f, -0.749f}, {1.0f, 2.0f, -0.7f}, {0, -1, 0}}, kClipDrop, true},
    {"GunArms_SMG", "smg", {{1, 0, 0}, {0, 1, 0}, {28.3f, 11.5f, -0.72f}, {2.0f, 1.5f, -0.9f}, {0, -1, 0}}, kClipDrop, true},
    {"GunArms_AssaultRifle", "rifle", {{0.968961f, -0.110631f, -0.221077f}, {0, 1, 0}, {52.671f, 2.87f, -11.168f}, {-3.0f, -1.0f, 0.8f}, {0, -1, 0}}, kClipDrop, true},
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
    // Moving parts: per vertex its part index and weight (src/vr/gunparts.h), and our vertex
    // buffer of them (the shader's second stream; made at the first draw).
    std::vector<float> parts;
    IDirect3DVertexBuffer9* part_vb = nullptr;
    bool part_vb_failed = false;
};

// Moving parts' animation state, per weapon object (gunmodel_draw).
struct Anim {
    bool init = false;
    bool reloading = false;  // the reloading flag (cWeapon+0x764) and its last change
    double flag_t = 0;
    float out0 = 0, out_last = 0;  // the magazine's / cylinder's way out (clip_out_fraction)
    double reload_end_t = -1e9;    // the flag's last fall (the pump racks)
    bool locked = false;           // slide lock: emptied by a shot, until the reload seats
    unsigned lock_checked = 0;     // the shot whose ammo was checked
    double unlock_t = -1e9;
    bool hammer_down = false;      // hand cannon: fell on a shot, until the trigger is let go
    unsigned hammer_shot = 0;
    float spin_speed = 0, spin_deg = 0;  // minigun
    double last_t = 0;
};

struct Model {
    bool ok = false;
    const WeaponDef* def = nullptr;
    Mesh gun, clip;
    bool has_clip = false;
    uintptr_t data = 0;  // cWeapon+4 when resolved
    const vr::WeaponParts* wp = nullptr;  // its moving parts (null: none)
    bool gun_parts_ok = false;  // the gun mesh's parts were found
    bool clip_ok = true;        // a clip_part weapon's _Clip is the expected mesh (else hidden, as before)
    Anim anim;
};

static void release(Mesh& m) {
    if (m.vb) m.vb->Release();
    if (m.ib) m.ib->Release();
    if (m.part_vb) m.part_vb->Release();
    for (auto*& t : m.tex)
        if (t) t->Release(), t = nullptr;
    m.vb = nullptr, m.ib = nullptr, m.part_vb = nullptr;
}

static std::map<uintptr_t, Model> g_models;  // by cWeapon
static bool g_level_was;

// Shots, per weapon object (gunmodel_shot; the game thread). Lock order: g_mx, then g_shot_mx.
struct ShotRec {
    double t = 0, t_prev = 0;
    unsigned n = 0;
};
static std::mutex g_shot_mx;
static std::map<uintptr_t, ShotRec> g_shots;

double gunmodel_now() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv;
}

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

static void build_parts(Model& m);

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
    if (m.ok) build_parts(m);
    return m;
}

static void release_all() {
    for (auto& [w, m] : g_models) release(m.gun), release(m.clip);
    if (!g_models.empty()) logf("gun model: level ended, %zu weapon model(s) released", g_models.size());
    g_models.clear();
    std::lock_guard lock(g_shot_mx);
    g_shots.clear();
}

static uintptr_t current_weapon() {
    const uintptr_t gc = *reinterpret_cast<const volatile uintptr_t*>(va(kGameControl));
    const uintptr_t p = gc ? rd(gc + 0x24) : 0;
    return p ? rd(p + 0x224) : 0;
}

// The current weapon's model (resolving it the first time); null if none. Call with g_mx held.
static Model* current_model(uintptr_t* weapon_out = nullptr) {
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

static std::atomic<bool> g_backdrop;
void gunmodel_enable(bool on) { g_enabled = on; }
void gunmodel_backdrop(bool on) { g_backdrop = on; }
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
    Model* model;
    uintptr_t weapon;
};

static bool placement(Placement* p) {
    if (g_vr.mode == VrMode::off || !g_vr.gun_model || !g_enabled) return false;
    uintptr_t weapon = 0;
    Model* m = current_model(&weapon);
    if (!m) return false;
    p->weapon = weapon;
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
static IDirect3DVertexBuffer9* g_rest_vb;  // part 0, weight 1 for kRestVerts vertices: a mesh without its own stream
static bool g_shader_failed;
constexpr UINT kRestVerts = 8192;

// The second stream (TEXCOORD1): the vertex's part and weight; c8.. hold each part's matrix (the
// first three rows, model space), applied before the model's world matrix and blended by the weight.
static_assert(vr::kMaxParts * 3 == 18, "cPart below holds 3 rows per part");
static const char kVs[] = R"(
float4 cClip[4] : register(c0);
float4 cView[4] : register(c4);
float4 cPart[18] : register(c8);
struct I { float4 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; float2 pw : TEXCOORD1; };
struct O { float4 p : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 v : TEXCOORD2; };
O main(I i) {
    O o;
    int k = (int)(i.pw.x + 0.5) * 3;
    float4 p0 = float4(i.p.xyz, 1);
    float3 q = float3(dot(p0, cPart[k]), dot(p0, cPart[k + 1]), dot(p0, cPart[k + 2]));
    float4 p = float4(lerp(i.p.xyz, q, i.pw.y), 1);
    float3 nq = float3(dot(i.n, cPart[k].xyz), dot(i.n, cPart[k + 1].xyz), dot(i.n, cPart[k + 2].xyz));
    float4 n = float4(lerp(i.n, nq, i.pw.y), 0);
    o.p = float4(dot(p, cClip[0]), dot(p, cClip[1]), dot(p, cClip[2]), dot(p, cClip[3]));
    o.v = float3(dot(p, cView[0]), dot(p, cView[1]), dot(p, cView[2]));
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

// Our vertex buffer of `n` floats (the part stream), filled once.
static IDirect3DVertexBuffer9* make_stream(IDirect3DDevice9* dev, const float* data, size_t n) {
    IDirect3DVertexBuffer9* vb = nullptr;
    const UINT bytes = UINT(n * sizeof(float));
    if (!bytes || FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr))) return nullptr;
    void* p = nullptr;
    if (FAILED(vb->Lock(0, bytes, &p, 0)) || !p) {
        vb->Release();
        return nullptr;
    }
    memcpy(p, data, bytes);
    vb->Unlock();
    return vb;
}

static bool ensure_resources(IDirect3DDevice9* dev) {
    if (g_vs && g_ps && g_decl && g_white && g_sb && g_vs_line && g_rest_vb) return true;
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
    // vf_stock_staticsinglenormalmapped: position, normal, colour, uv, tangent (52 bytes); stream 1:
    // our part index and weight.
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                    {1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
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
    if (ok) {
        const std::vector<float> rest = vr::rest_stream(kRestVerts);
        ok = (g_rest_vb = make_stream(dev, rest.data(), rest.size())) != nullptr;
    }
    logf("gun model: shaders and states %s", ok ? "ready" : "FAILED (no gun model)");
    g_shader_failed = !ok;
    return ok;
}

void gunmodel_reset() {
    std::lock_guard lock(g_mx);
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&g_vs), reinterpret_cast<IUnknown**>(&g_ps), reinterpret_cast<IUnknown**>(&g_vs_line),
                         reinterpret_cast<IUnknown**>(&g_ps_line), reinterpret_cast<IUnknown**>(&g_decl),
                         reinterpret_cast<IUnknown**>(&g_decl_line), reinterpret_cast<IUnknown**>(&g_white),
                         reinterpret_cast<IUnknown**>(&g_sb), reinterpret_cast<IUnknown**>(&g_rest_vb)})
        if (*p) (*p)->Release(), *p = nullptr;
    for (auto& [w, m] : g_models)
        for (Mesh* mesh : {&m.gun, &m.clip})
            if (mesh->part_vb) mesh->part_vb->Release(), mesh->part_vb = nullptr, mesh->part_vb_failed = false;
    g_shader_failed = false;
}

static void set_matrix(IDirect3DDevice9* dev, UINT reg, const vr::Mat4& m) { raw_set_vs_const(dev, reg, &m.m[0][0], 4); }

// The parts' matrices (c8..): the first three rows of each.
static void set_parts(IDirect3DDevice9* dev, const vr::Mat4* parts) {
    float rows[vr::kMaxParts * 12];
    for (int i = 0; i < vr::kMaxParts; ++i)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) rows[i * 12 + r * 4 + c] = parts[i].m[r][c];
    raw_set_vs_const(dev, 8, rows, vr::kMaxParts * 3);
}

// The mesh's part stream: its own (made at the first draw), else everything at rest.
static bool bind_parts(IDirect3DDevice9* dev, Mesh& m) {
    if (!m.part_vb && !m.part_vb_failed && m.parts.size() == size_t(m.nv) * 2) {
        m.part_vb = make_stream(dev, m.parts.data(), m.parts.size());
        if (!m.part_vb) m.part_vb_failed = true, logf("gun parts: %s: no vertex buffer for its parts; drawn still", m.name);
    }
    IDirect3DVertexBuffer9* vb = m.part_vb ? m.part_vb : (m.nv <= kRestVerts ? g_rest_vb : nullptr);
    if (!vb) return false;
    dev->SetStreamSource(1, vb, 0, 8);
    return true;
}

static void draw_mesh(IDirect3DDevice9* dev, Mesh& m) {
    if (!bind_parts(dev, m)) return;
    dev->SetStreamSource(0, m.vb, 0, m.stride);
    dev->SetIndices(m.ib);
    raw_set_texture(dev, 0, m.tex[0] ? m.tex[0] : g_white);
    raw_set_texture(dev, 1, m.tex[2] ? m.tex[2] : g_white);
    const float params[4] = {g_vr.gun_light * 0.45f, g_vr.gun_light * 0.75f, 0.6f, m.tex[2] ? 1.f : 0.f};
    dev->SetPixelShaderConstantF(1, params, 1);
    raw_dip(dev, D3DPT_TRIANGLELIST, 0, 0, m.nv, 0, m.nidx / 3);  // past the per-eye duplication: we draw each eye ourselves
}

static int g_draws_logged;

// --- the gun accuracy test (dev `vr shotlog 1`) ---
// Each frame the gun is drawn: its muzzle point and barrel direction in the world, from the very
// world matrix it is drawn with (the description's muzzle and barrel through it), the hand pose,
// and the rail camera's turn speed. gun.cpp's shot log takes the last one at each shot: the frame
// on screen when the trigger was read.
static std::mutex g_drawn_mx;
static DrawnBarrel g_drawn[64];
static unsigned g_drawn_next;

static void record_drawn(const vr::Mat4& world, const vr::Mat4& hand, const Placement& p) {
    DrawnBarrel d;
    d.frame = gun_frame_no();
    const vr::Vec4 m = vr::apply(world, {p.desc.muzzle[0], p.desc.muzzle[1], p.desc.muzzle[2], 1});
    const vr::Vec4 b = vr::apply(world, {p.desc.barrel[0], p.desc.barrel[1], p.desc.barrel[2], 0});
    const float n = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (!(n > 0) || !(m.w != 0)) return;
    d.muzzle[0] = m.x / m.w, d.muzzle[1] = m.y / m.w, d.muzzle[2] = m.z / m.w;
    d.dir[0] = b.x / n, d.dir[1] = b.y / n, d.dir[2] = b.z / n;
    for (int i = 0; i < 3; ++i) d.hand[i] = hand.m[i][3], d.hand[3 + i] = hand.m[i][2];
    comfort_rail_turn(&d.turn, &d.turn_frame);
    snprintf(d.key, sizeof d.key, "%s", p.model->def->key);
    // Angular speeds since the previous frame drawn (the hand sweeping; the barrel in the world).
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now), QueryPerformanceFrequency(&freq);
    std::lock_guard lock(g_drawn_mx);
    static LARGE_INTEGER prev_t;
    static float prev_hand[3], prev_dir[3];
    const double dt = prev_t.QuadPart ? double(now.QuadPart - prev_t.QuadPart) / double(freq.QuadPart) : 0;
    auto deg_between = [](const float* a, const float* b) {
        const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        return std::atan2(std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * 57.29578f;
    };
    if (dt > 0 && dt < 0.25) {
        d.hand_turn = float(deg_between(prev_hand, d.hand + 3) / dt);
        d.barrel_turn = float(deg_between(prev_dir, d.dir) / dt);
    }
    prev_t = now;
    memcpy(prev_hand, d.hand + 3, sizeof prev_hand), memcpy(prev_dir, d.dir, sizeof prev_dir);
    g_drawn[g_drawn_next++ % std::size(g_drawn)] = d;
}

bool gunmodel_last_drawn(DrawnBarrel* out) {
    std::lock_guard lock(g_drawn_mx);
    if (!g_drawn_next) return false;
    *out = g_drawn[(g_drawn_next - 1) % std::size(g_drawn)];
    return true;
}

bool gunmodel_weapon_name(uintptr_t weapon, char name[64], char key[16]) {
    std::lock_guard lock(g_mx);
    auto it = g_models.find(weapon);
    if (it == g_models.end() || !it->second.ok) return false;
    snprintf(name, 64, "%s", it->second.gun.name);
    snprintf(key, 16, "%s", it->second.def->key);
    return true;
}

// Reads a vertex buffer's bytes (a locked read; exception-guarded). The mod makes the device
// Direct3D 9Ex and creates the game's MANAGED buffers in the DEFAULT pool (d3d9ex.cpp), where a
// vertex buffer can still be locked; a READONLY lock reads it back (the driver copies it if it
// lives in video memory).
static HRESULT seh_read_vb(IDirect3DVertexBuffer9* vb, UINT bytes, void* out) {
    void* p = nullptr;
    HRESULT hr = E_FAIL;
    __try {
        hr = vb->Lock(0, bytes, &p, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            memcpy(out, p, bytes);
            vb->Unlock();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_POINTER;
    }
    return hr;
}

static HRESULT seh_read_ib(IDirect3DIndexBuffer9* ib, UINT bytes, void* out) {
    void* p = nullptr;
    HRESULT hr = E_FAIL;
    __try {
        hr = ib->Lock(0, bytes, &p, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            memcpy(out, p, bytes);
            ib->Unlock();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_POINTER;
    }
    return hr;
}

// A mesh's positions (its whole vertices, stride/4 floats each) and indices, read back.
static bool read_mesh(const Mesh& g, std::vector<float>* pos, std::vector<uint32_t>* idx, HRESULT* hr) {
    const UINT bytes = g.nv * g.stride;
    pos->assign((bytes + 3) / 4, 0.f);
    if (FAILED(*hr = seh_read_vb(g.vb, bytes, pos->data()))) return false;
    const UINT isz = g.ifmt == D3DFMT_INDEX32 ? 4 : 2;
    std::vector<uint8_t> raw(size_t(g.nidx) * isz);
    if (FAILED(*hr = seh_read_ib(g.ib, g.nidx * isz, raw.data()))) return false;
    idx->resize(g.nidx);
    for (UINT i = 0; i < g.nidx; ++i) {
        if (isz == 4) memcpy(&(*idx)[i], &raw[size_t(i) * 4], 4);
        else (*idx)[i] = uint32_t(raw[size_t(i) * 2]) | uint32_t(raw[size_t(i) * 2 + 1]) << 8;
    }
    return true;
}

// The moving parts of a model just resolved (src/vr/gunparts.h): its gun mesh's and, for the hand
// cannon, pump and minigun, its _Clip mesh's. Anything unexpected leaves that mesh still (logged);
// a clip_part weapon's _Clip that isn't the expected mesh is hidden, as it was before.
static void build_parts(Model& m) {
    m.gun.parts = vr::rest_stream(m.gun.nv);
    m.clip.parts = vr::rest_stream(m.clip.nv);
    m.wp = vr::weapon_parts(m.def->key);
    if (!m.wp) return;
    auto run = [&](Mesh& mesh, const vr::PartRule* rules, int n, const char* what) {
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        HRESULT hr = S_OK;
        if (!read_mesh(mesh, &pos, &idx, &hr)) {
            logf("gun parts: %s %s: reading the mesh back failed (%08lx); it stays still", m.def->key, what, hr);
            return false;
        }
        const vr::PartsResult r = vr::assign_parts(pos.data(), mesh.nv, mesh.stride / 4, idx.data(), idx.size(), rules, n);
        logf("gun parts: %s %s (%s): %d pieces; %s%s", m.def->key, what, mesh.name, r.pieces, r.log.c_str(),
             r.ok ? "" : " - not the mesh expected: it stays still");
        if (r.ok) mesh.parts = r.stream;
        return r.ok;
    };
    if (m.wp->n_gun > 0) m.gun_parts_ok = run(m.gun, m.wp->gun, m.wp->n_gun, "gun");
    if (m.wp->clip_part) {
        if (!m.has_clip) {
            m.clip_ok = false;
            logf("gun parts: %s: no _Clip mesh resident", m.def->key);
        } else {
            m.clip_ok = run(m.clip, m.wp->clip, m.wp->n_clip, "clip");
            if (!m.clip_ok) logf("gun parts: %s: its _Clip is hidden (as before)", m.def->key);
        }
    }
}

// --- shots and the parts' animation ---

void gunmodel_shot(uintptr_t weapon) {
    const double t = gunmodel_now();
    ShotRec r;
    {
        std::lock_guard lock(g_shot_mx);
        ShotRec& s = g_shots[weapon];
        s.t_prev = s.t, s.t = t, ++s.n;
        r = s;
    }
    if (r.n <= 40) {  // the first 40 shots of each weapon object (the minigun fires ~40 a second)
        char name[64] = "?", key[16] = "?";
        gunmodel_weapon_name(weapon, name, key);
        if (r.n > 1) logf("gun anim: shot %u of %s (%p), %.0f ms after the one before", r.n, key, reinterpret_cast<void*>(weapon), (r.t - r.t_prev) * 1000);
        else logf("gun anim: shot %u of %s (%p)", r.n, key, reinterpret_cast<void*>(weapon));
    }
}

bool gunmodel_last_shot(uintptr_t weapon, double* t, unsigned* count) {
    std::lock_guard lock(g_shot_mx);
    auto it = g_shots.find(weapon);
    if (it == g_shots.end() || !it->second.n) return false;
    *t = it->second.t, *count = it->second.n;
    return true;
}

// dev `vr gunanim`: the current weapon's parts held as they are T ms after a shot, and/or the
// slide lock, the reload's way out, the time since a reload ended, the trigger, the minigun's angle.
struct Freeze {
    bool on = false;
    std::string key;  // a kDefs key or "any"
    float shot_ms = NAN, reload_end_ms = NAN, out = NAN, trigger = NAN, spin = NAN;
    int lock = -1;
};
static Freeze g_freeze;

void gunmodel_anim_command(const std::vector<std::string>& a) {
    std::lock_guard lock(g_mx);
    if (a.empty() || a[0] == "off") {
        g_freeze = Freeze{};
        logf("vr gunanim: live");
        return;
    }
    Freeze f;
    f.on = true, f.key = a[0];
    bool ok = a.size() >= 2;
    for (size_t i = 1; i < a.size() && ok;) {
        const std::string& field = a[i];
        char* end = nullptr;
        const float t = strtof(field.c_str(), &end);
        if (end != field.c_str() && *end == 0) {  // `vr gunanim W T`: T ms after a shot
            f.shot_ms = t;
            ++i;
            continue;
        }
        if (i + 1 >= a.size()) {
            ok = false;
            break;
        }
        const float v = strtof(a[i + 1].c_str(), nullptr);
        i += 2;
        if (field == "shot") f.shot_ms = v;
        else if (field == "lock") f.lock = v != 0;
        else if (field == "reloadend") f.reload_end_ms = v;
        else if (field == "out") f.out = v;
        else if (field == "trigger") f.trigger = v;
        else if (field == "spin") f.spin = v;
        else ok = false;
    }
    if (!ok) {
        logf("vr gunanim: usage: off | WEAPON|any [T] [shot MS] [lock 0|1] [reloadend MS] [out F] [trigger F] [spin DEG]");
        return;
    }
    g_freeze = f;
    logf("vr gunanim: %s frozen: shot %.0f ms, lock %d, reload end %.0f ms ago, out %.2f, trigger %.2f, spin %.1f deg", f.key.c_str(), f.shot_ms,
         f.lock, f.reload_end_ms, f.out, f.trigger, f.spin);
}

static float smooth01(float t) {
    t = std::fmin(1.f, std::fmax(0.f, t));
    return t * t * (3 - 2 * t);
}

// This frame's motion of the current weapon's parts, and how far its magazine (or the hand
// cannon's cylinder) is out for a reload. Call with g_mx held.
static vr::GunMotion animate(Model& m, uintptr_t w, float* clip_out) {
    Anim& a = m.anim;
    const double now = gunmodel_now();
    ShotRec s;
    {
        std::lock_guard lock(g_shot_mx);
        auto it = g_shots.find(w);
        if (it != g_shots.end()) s = it->second;
    }
    uint32_t ammo = 1;
    uint8_t reloading = 0;
    float progress = 0;
    safe_copy(&ammo, w + 0x760, 4), safe_copy(&reloading, w + 0x764, 1), safe_copy(&progress, w + 0x76C, 4);
    if (!(progress >= 0 && progress <= 2)) progress = 0;
    if (!a.init) {
        a.init = true, a.reloading = reloading != 0, a.flag_t = now - 10, a.last_t = now;
        a.lock_checked = a.hammer_shot = s.n;
    }
    // The reloading flag's last change, and how far out the magazine was then (milestone 4).
    if ((reloading != 0) != a.reloading) {
        if (a.reloading) a.reload_end_t = now;
        a.reloading = reloading != 0, a.flag_t = now, a.out0 = a.out_last;
    }
    a.out_last = vr::clip_out_fraction(a.reloading, progress, float(now - a.flag_t), a.out0);
    *clip_out = g_vr.clip_anim ? a.out_last : 0;
    if (g_force_clip >= 0) *clip_out = g_force_clip;
    const float dt = float(now - a.last_t);
    a.last_t = now;

    const vr::WeaponParts* wp = m.wp;
    const Freeze& fz = g_freeze;
    const bool frozen = fz.on && (fz.key == "any" || fz.key == m.def->key);
    float since = s.n ? float(now - s.t) : 1e9f, since_prev = s.n > 1 ? float(now - s.t_prev) : 1e9f;
    float since_reload = float(now - a.reload_end_t);
    CtlState ctl;
    float trigger = vrsys_controller(true, &ctl) ? std::fmin(1.f, std::fmax(0.f, ctl.trigger)) : 0.f;
    unsigned shots = s.n;
    if (frozen) {
        if (!std::isnan(fz.shot_ms)) since = fz.shot_ms / 1000, since_prev = 1e9f, shots = (std::max)(shots, 1u);
        if (!std::isnan(fz.reload_end_ms)) since_reload = fz.reload_end_ms / 1000;
        if (!std::isnan(fz.trigger)) trigger = fz.trigger;
        if (!std::isnan(fz.out)) *clip_out = fz.out;
    }
    vr::GunMotion mo;
    if (!wp || !g_vr.gun_parts) return mo;
    mo.trigger = trigger;
    mo.kick = (std::max)(vr::kick_curve(since, wp->kick_back, wp->kick_home), vr::kick_curve(since_prev, wp->kick_back, wp->kick_home));
    if (wp->slide_lock) {
        // A shot that emptied the magazine: the slide stays back until the reload seats the new one.
        if (s.n != a.lock_checked && since >= wp->kick_back) {
            a.lock_checked = s.n;
            if (ammo == 0) a.locked = true;
        }
        if (a.locked && (ammo > 0 || (a.reloading && progress >= 0.85f))) a.locked = false, a.unlock_t = now;
        const bool locked = frozen && fz.lock >= 0 ? fz.lock != 0 : a.locked;
        if (locked) mo.kick = 1;
        else if (!frozen) mo.kick = (std::max)(mo.kick, 1 - smooth01(float(now - a.unlock_t) / wp->kick_home));
    }
    mo.hammer = mo.kick;  // the magnum's hammer is pushed back by the slide
    if (!strcmp(m.def->key, "handcannon")) {
        // Double action: the trigger cocks the hammer, the shot drops it; it stays down until the
        // trigger is let go. Then the cylinder turns to the next chamber.
        if (s.n != a.hammer_shot) a.hammer_shot = s.n, a.hammer_down = true;
        if (a.hammer_down && trigger < 0.25f) a.hammer_down = false;
        float cock = a.hammer_down ? 0 : std::fmin(1.f, trigger / 0.5f);
        if (since < vr::kHammerFall) cock = 1 - since / vr::kHammerFall;
        if (frozen && !std::isnan(fz.shot_ms)) cock = since < 0 ? 1 : since < vr::kHammerFall ? 1 - since / vr::kHammerFall : 0;
        mo.hammer = cock;
        const int ch = wp->chambers > 0 ? wp->chambers : 6;
        mo.cyl_deg = 360.f / float(ch) * std::fmod(float(shots ? shots - 1 : 0) + vr::cylinder_turn(since), float(ch));
        mo.swing = *clip_out;
    }
    if (wp->rack_travel != 0)
        mo.rack = (std::max)(vr::rack_curve(since, wp->rack_start, wp->rack_back, wp->rack_hold, wp->rack_fwd),
                             vr::rack_curve(since_reload, 0, wp->rack_back, wp->rack_hold, wp->rack_fwd));
    if (!strcmp(m.def->key, "minigun")) {
        vr::spin_step(trigger > 0.5f || since < 0.15f, dt, &a.spin_speed, &a.spin_deg);
        mo.spin_deg = frozen && !std::isnan(fz.spin) ? fz.spin : a.spin_deg;
    }
    if (wp->slide_is_string) mo.string = vr::string_curve(since, wp->string_snap, wp->string_draw);
    return mo;
}

static void log_mesh_compare(const char* source, const Model& m, const vr::BoreFit& f, const vr::GunModelDesc& d) {
    float ang, perp, along;
    vr::compare_bore(f, d.barrel, d.muzzle, &ang, &perp, &along);
    const float bn = std::sqrt(d.barrel[0] * d.barrel[0] + d.barrel[1] * d.barrel[1] + d.barrel[2] * d.barrel[2]);
    logf("shotlog mesh weapon=%s key=%s source=%s axis=%d muzzle=%d method=%s rings=%d round_rings=%d edge_support=%d slab_points=%d radius_cm=%.3f "
         "rms_cm=%.4f span_cm=%.2f fit_dir=%.6f,%.6f,%.6f fit_muzzle=%.3f,%.3f,%.3f def_dir=%.6f,%.6f,%.6f def_muzzle=%.3f,%.3f,%.3f "
         "angle_deg=%.3f perp_cm=%.3f along_cm=%.3f why=\"%s\"",
         m.gun.name, m.def->key, source, int(f.axis_ok), int(f.muzzle_ok), f.method, f.rings, f.rings_round, f.edge_support, f.slab_points, f.radius, f.rms, f.span,
         f.dir[0], f.dir[1],
         f.dir[2], f.muzzle[0], f.muzzle[1], f.muzzle[2], d.barrel[0] / bn, d.barrel[1] / bn, d.barrel[2] / bn, d.muzzle[0], d.muzzle[1],
         d.muzzle[2], ang, perp, along, f.why);
}

// `vr gunmeshcheck`: the current weapon's bore and muzzle fitted from its live vertex buffer
// (src/vr/gunfit.h: round rings near the muzzle end, a line through their centres) against the
// built-in description (kDefs). A hovr.ini override (barrel_/muzzle_/grip_/hold_<weapon>) is logged
// as such, with its own comparison, and is not a failure.
void gunmodel_meshcheck() {
    std::lock_guard lock(g_mx);
    const Model* m = current_model();
    if (!m) {
        logf("vr gunmeshcheck: no weapon model (not in a level, or no model for this weapon)");
        return;
    }
    const Mesh& g = m->gun;
    D3DVERTEXBUFFER_DESC vd{};
    if (!seh_vb(g.vb, &vd)) {
        logf("vr gunmeshcheck: %s: the vertex buffer is gone", g.name);
        return;
    }
    const UINT bytes = g.nv * g.stride;
    std::vector<float> data((bytes + 3) / 4);
    const HRESULT hr = seh_read_vb(g.vb, bytes, data.data());
    if (FAILED(hr)) {
        logf("vr gunmeshcheck: %s: reading the vertex buffer failed (%08lx; pool %d usage %08lx)", g.name, hr, int(vd.Pool), vd.Usage);
        return;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const size_t sf = g.stride / 4;
    int finite = 0;
    for (UINT i = 0; i < g.nv; ++i) {
        const float* q = &data[i * sf];
        if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) continue;
        ++finite;
        for (int k = 0; k < 3; ++k) lo[k] = (std::min)(lo[k], q[k]), hi[k] = (std::max)(hi[k], q[k]);
    }
    logf("vr gunmeshcheck: %s: %u vertices read back (%d finite; stride %u, pool %d, usage %08lx), bbox %.2f %.2f %.2f .. %.2f %.2f %.2f cm", g.name,
         g.nv, finite, g.stride, int(vd.Pool), vd.Usage, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    const vr::GunModelDesc& def = m->def->desc;
    const vr::BoreFit f = vr::fit_bore(data.data(), g.nv, sf, def.barrel, def.muzzle);
    for (int i = 0; i < f.n_info; ++i)
        logf("vr gunmeshcheck: %s: round part at %.2f along the barrel, centre %.3f %.3f %.3f, radius %.3f..%.3f cm, %d points%s", g.name,
             f.info[i].t, f.info[i].centre[0], f.info[i].centre[1], f.info[i].centre[2], f.info[i].rmin, f.info[i].rmax, f.info[i].points,
             f.info[i].used ? " (on the fitted axis)" : "");
    log_mesh_compare("builtin", *m, f, def);
    // Overrides from hovr.ini (or `vr gunset`): what is in effect differs from the built-in.
    auto it = g_cfg.find(m->def->key);
    if (it == g_cfg.end()) return;
    const WeaponCfg& c = it->second;
    std::string fields;
    auto differs = [](const float* a, const float* b, int n) {
        for (int i = 0; i < n; ++i)
            if (a[i] != b[i]) return true;
        return false;
    };
    if (differs(c.desc.barrel, def.barrel, 3)) fields += "barrel,";
    if (differs(c.desc.muzzle, def.muzzle, 3)) fields += "muzzle,";
    if (differs(c.desc.grip, def.grip, 3)) fields += "grip,";
    if (c.hold[0] != 0 || c.hold[1] != 0) fields += "hold,";
    if (fields.empty()) return;
    fields.pop_back();
    logf("shotlog mesh_override weapon=%s key=%s fields=%s barrel=%.4f,%.4f,%.4f muzzle=%.3f,%.3f,%.3f grip=%.3f,%.3f,%.3f hold=%.2f,%.2f",
         g.name, m->def->key, fields.c_str(), c.desc.barrel[0], c.desc.barrel[1], c.desc.barrel[2], c.desc.muzzle[0], c.desc.muzzle[1],
         c.desc.muzzle[2], c.desc.grip[0], c.desc.grip[1], c.desc.grip[2], c.hold[0], c.hold[1]);
    if (fields.find("barrel") != std::string::npos || fields.find("muzzle") != std::string::npos) log_mesh_compare("override", *m, f, c.desc);
}

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
    if (gun_shotlog_on()) record_drawn(world, hand, p);
    // The moving parts (drawing only: the shot ray above is the un-moved barrel), and the clip mesh:
    // a part of the gun (cylinder, forend, barrel cluster), or a magazine that drops along clip_dir
    // (model space) while reloading, or fixed.
    Model& m = *p.model;
    float out = 0;
    const vr::GunMotion mo = animate(m, p.weapon, &out);
    vr::GunPose pose;
    vr::pose_weapon(m.wp, mo, &pose);
    vr::Mat4 clip_parts[vr::kMaxParts];
    for (vr::Mat4& c : clip_parts) c = vr::identity();
    bool draw_clip = false;
    if (m.wp && m.wp->clip_part) {
        draw_clip = p.clip != kClipNone && m.clip_ok;
        for (int i = 0; i < vr::kMaxParts; ++i) clip_parts[i] = pose.clip[i];
    } else if (p.clip == kClipFixed) {
        draw_clip = true;
    } else if (p.clip == kClipDrop) {
        float d[3] = {p.desc.clip_dir[0], p.desc.clip_dir[1], p.desc.clip_dir[2]};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float dist = out * g_vr.clip_drop / g_metres_per_unit();  // model units
        if (n > 0) clip_parts[0] = vr::translation(d[0] / n * dist, d[1] / n * dist, d[2] / n * dist);
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
        // dev `vr gunbackdrop 1`: the gun on plain grey (captures of its moving parts).
        if (g_backdrop) raw_clear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(120, 124, 132), 1.0f, 0);
        set_matrix(dev, 0, vr::mul(clip, world));
        set_parts(dev, pose.gun);
        draw_mesh(dev, m.gun);
        if (draw_clip) {
            set_parts(dev, clip_parts);
            draw_mesh(dev, m.clip);
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
    if (m)
        logf("  moving parts (gun_parts=%d): %s; gun mesh %s, clip mesh %s; slide %s", int(g_vr.gun_parts), m->wp ? "described" : "none",
             !m->wp || !m->wp->n_gun ? "-" : m->gun_parts_ok ? "split" : "still (unexpected pieces)", !m->wp || !m->wp->clip_part ? "-" : m->clip_ok ? "in place" : "hidden",
             m->anim.locked ? "locked back" : "free");
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
