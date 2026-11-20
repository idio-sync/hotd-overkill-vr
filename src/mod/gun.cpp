// The controller is the gun (milestone 2). Every shot path of player 1 is fed the
// gun hand's ray instead of the camera's ray through the mouse cursor, so the game's own ray casts
// decide the hits from the hand's position (parallax included):
//
//   0x50D110 cWeapon::FireBullets(origin*, mode, travelled), thiscall, ret 0xC. Called by the
//            weapon's fire 0x52B090 (return address 0x52B164) with a copy of the player's aim origin
//            (player+0x1E0); it ray casts along cWeapon+0x7B4 (the aim direction, re-aimed each
//            frame by the game) scaled to the weapon's range, with spread for multi-pellet weapons.
//            Overridden: our own origin vector (the game's isn't written) and +0x7B4 (length kept).
//            Its recursive calls for bullets that go on through a hit (mode + 1, from 0x50D89B)
//            are left alone, as are other callers.
//   0x4E1330 the weapon-type-5 shot (a projectile; 0x52B090 when [weapon+4]+0x20 == 5): origin*,
//            thiscall, ret 4. It adds a camera-space offset (-5, -10, 0) to the origin (written
//            back through the pointer, so ours is a copy) and flies along player+0x1F0. Overridden:
//            the origin (minus that offset, so the projectile starts at the hand) and player+0x1F0
//            for the duration of the call.
//   0x42B900, 0x479250  cursor-driven sphere checks (pickups, targets; radius 80 / 160) with
//            player* (thiscall, ret 4): segment from ScreenToWorld(cursor) (a point on the near
//            plane) along normalize(that - player+0x1E0) * range. For the call, the cursor is moved
//            to where the hand's ray crosses the near plane and player+0x1E0 to a point behind it
//            on the same line, so the segment runs along the hand's ray.
//   0x5295F0 / 0x51FBD0  SetCursorX / SetCursorY (thiscall, float, ret 4; clamp to the screen):
//            player 1's cursor is set to the projection of the aim point (where the hand's ray meets
//            the scene: the game's ray cast 0x5514E0, or 10 m out), so the game's own aim (the
//            per-frame direction player+0x1F0, the crosshair, anything else reading the cursor)
//            agrees with the gun.
//
//   0x5514E0 the world ray cast (cdecl: 0, origin*, segment*, 0, result*). Development harness only:
//            hooked for the gun accuracy test's shot log (`vr shotlog 1`), FireBullets' call at
//            0x50D279 (return address 0x50D27E) logged per pellet; nothing is changed.
//
// The hand's world ray: the gun hand in the game camera's space (vrsys_hand_in_game, the same
// anchor transform as the eyes) through the inverse of the engine's view matrix (0x9C7E48) of the
// last frame drawn - the frame the player aimed at.
//
// The probe's dev overrides (`aimdir`, `aimorigin`) still apply on top.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include "../vr/aim.h"

#pragma intrinsic(_ReturnAddress)

namespace hv {

constexpr uintptr_t kGameControl = 0x99B708;  // cGameControl; +0x24 player 1; +0x64 camera
constexpr uintptr_t kViewVa = 0x9C7E48;
constexpr uintptr_t kFireRet = 0x52B164;      // FireBullets' return address in the weapon's fire
constexpr float kMissDistance = 1000;         // units (10 m): the aim point when the ray hits nothing

using Fire_t = void(__fastcall*)(void* self, void* edx, float* origin, int mode, float travelled);
using FireProj_t = void(__fastcall*)(void* self, void* edx, float* origin);
using Sphere_t = unsigned(__fastcall*)(void* self, void* edx, uint8_t* player);
using SetCursor_t = void(__fastcall*)(uint8_t* player, void* edx, float v);
using Unproject_t = void(__cdecl*)(float x, float y, float* out);
using RayCast_t = int(__cdecl*)(int, const float* origin, const float* seg, int, void* result);
using RayResult_t = void(__fastcall*)(void* self, void* edx);
using QuatToMat_t = void(__cdecl*)(float* mat, const float* quat);
using MatVec_t = void(__cdecl*)(float* out, const float* mat, const float* vec);

static Fire_t o_fire;
static FireProj_t o_fire_proj;
static Sphere_t o_sphere_a, o_sphere_b;
static SetCursor_t o_setx, o_sety;

// --- dev overrides (`aimdir`, `aimorigin`) ---
static std::mutex g_dev_mx;
static bool g_dev_dir, g_dev_origin, g_capture_next;
static float g_dev_aim_dir[3], g_dev_aim_origin[3];

void gun_capture_next() {
    std::lock_guard lock(g_dev_mx);
    g_capture_next = true;
    g_dev_dir = false;
}

void gun_set_dir(bool on, float x, float y, float z) {
    std::lock_guard lock(g_dev_mx);
    g_dev_dir = on;
    const float n = std::sqrt(x * x + y * y + z * z);
    if (n > 0) g_dev_aim_dir[0] = x / n, g_dev_aim_dir[1] = y / n, g_dev_aim_dir[2] = z / n;
}

void gun_set_origin(bool on, float x, float y, float z) {
    std::lock_guard lock(g_dev_mx);
    g_dev_origin = on;
    g_dev_aim_origin[0] = x, g_dev_aim_origin[1] = y, g_dev_aim_origin[2] = z;
}

// --- game reads and calls (game thread) ---

static uintptr_t read_u32(uintptr_t addr) {
    uintptr_t v = 0;
    return safe_copy(&v, addr, 4) ? v : 0;
}

static uintptr_t player1() {
    const uintptr_t gc = *reinterpret_cast<const volatile uintptr_t*>(va(kGameControl));
    return gc ? read_u32(gc + 0x24) : 0;
}

static bool read_view(vr::Mat4* view) {
    float v[16];
    if (!safe_copy(v, va(kViewVa), sizeof v)) return false;
    *view = vr::from_d3dx(v);
    return true;
}

// The current weapon's range (units) and data block ([player+0x224] = cWeapon, +4 = its data).
static float weapon_range(uintptr_t player, uintptr_t* data_out = nullptr) {
    const uintptr_t weapon = read_u32(player + 0x224), data = weapon ? read_u32(weapon + 4) : 0;
    if (data_out) *data_out = data;
    float r = 0;
    if (data && safe_copy(&r, data + 0x204, 4) && r > 10 && r < 1e6f) return r;
    return 20000;
}

static bool seh_unproject(float x, float y, float out[4]) {
    alignas(16) float buf[8] = {};
    __try {
        reinterpret_cast<Unproject_t>(va(0x655620))(x, y, buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    memcpy(out, buf, 16);
    return true;
}

// ScreenToWorld is affine in the cursor: base + x du + y dv.
static bool unproject_basis(float base[3], float du[3], float dv[3]) {
    float a[4], b[4], c[4];
    if (!seh_unproject(0, 0, a) || !seh_unproject(100, 0, b) || !seh_unproject(0, 100, c)) return false;
    for (int i = 0; i < 3; ++i) base[i] = a[i], du[i] = (b[i] - a[i]) / 100, dv[i] = (c[i] - a[i]) / 100;
    return true;
}

static int seh_raycast(const float* org, const float* seg, uint8_t* res) {
    __try {
        reinterpret_cast<RayResult_t>(va(0x4D6830))(res, nullptr);
        return reinterpret_cast<RayCast_t>(va(0x5514E0))(0, org, seg, 0, res);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// What a ray cast hit (FireBullets: 0x48C8D0 on the result gives the entity, its vtable slot
// 0x154 says whether it takes shots - zombies, props).
struct HitInfo {
    uintptr_t entity = 0, vtable = 0;
    bool shootable = false;
};
static void seh_entity(uint8_t* res, HitInfo* out) {
    __try {
        using Entity_t = uintptr_t(__fastcall*)(void* res, void* edx);
        using Bool_t = bool(__fastcall*)(void* self, void* edx);
        out->entity = reinterpret_cast<Entity_t>(va(0x48C8D0))(res, nullptr);
        if (!out->entity) return;
        out->vtable = *reinterpret_cast<const uintptr_t*>(out->entity);
        out->shootable = reinterpret_cast<Bool_t>(*reinterpret_cast<const uintptr_t*>(out->vtable + 0x154))(reinterpret_cast<void*>(out->entity), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->shootable = false;
    }
}

// The game's own world ray cast (as FireBullets uses it): the first hit along o + d * range.
static bool cast_ray(const float o[3], const float d[3], float range, float hit[3], HitInfo* info = nullptr) {
    alignas(16) float org[4] = {o[0], o[1], o[2], 1};
    alignas(16) float seg[4] = {d[0] * range, d[1] * range, d[2] * range, 1};
    alignas(16) uint8_t res[0x80] = {};
    const int r = seh_raycast(org, seg, res);
    if (r < 0) {
        static int warned;
        if (warned++ < 3) logf("gun: ray cast raised an exception");
        return false;
    }
    if (!r) return false;
    memcpy(hit, res, 12);
    if (info) seh_entity(res, info);
    return true;
}

// --- the aim ---

static std::atomic<unsigned> g_frame{1};
void gun_frame() { ++g_frame; }
unsigned gun_frame_no() { return g_frame.load(); }

struct Aim {
    unsigned frame = 0;
    bool valid = false;  // the controller aims this frame
    vr::Ray ray{};       // the hand's world ray
    float point[3] = {};
    bool hit = false;
    float cursor[2] = {};
    bool cursor_ok = false;
};
static std::mutex g_aim_mx;
static Aim g_aim;
static std::atomic<bool> g_handat_pending, g_seek_pending;
static float g_handat[2];
static std::atomic<int> g_hooks_installed;
static std::atomic<bool> g_god;  // dev `vr god 1`: player 1's health (player+0x20C, 10 = full) kept full
void gun_set_god(bool on) { g_god = on; }
static std::atomic<int> g_hits;  // player 1's health drops seen
int gun_hits() { return g_hits.load(); }

bool gun_aim_active() {
    if (g_vr.mode == VrMode::off || !g_vr.aim || !g_hooks_installed) return false;
    vr::Mat4 h;
    return vrsys_hand_in_game(&h) && game_state().gameplay;
}

// The shot ray: the barrel line of the gun model in the hand (muzzle origin, barrel direction) when
// one is shown, otherwise the controller's own pointing ray.
static bool hand_world_ray(vr::Ray* out) {
    vr::Mat4 hand, view;
    if (!vrsys_hand_in_game(&hand) || !read_view(&view)) return false;
    vr::Ray cam;
    if (!gunmodel_barrel(hand, &cam)) cam = vr::pose_ray_lh(hand);
    return vr::camera_ray_to_world(view, cam, g_vr.unit_m, out);
}

// Sim `vr handat X Y`: what the camera sees through cursor (X, Y) -> point the hand at it.
static void do_handat(uintptr_t player) {
    float base[3], du[3], dv[3], o[4];
    vr::Mat4 view;
    if (!unproject_basis(base, du, dv) || !safe_copy(o, player + 0x1E0, 16) || !read_view(&view)) {
        logf("vr handat: no camera");
        return;
    }
    float p[3], d[3];
    for (int i = 0; i < 3; ++i) p[i] = base[i] + g_handat[0] * du[i] + g_handat[1] * dv[i], d[i] = p[i] - o[i];
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (float& c : d) c /= n;
    float hit[3];
    const bool ok = cast_ray(o, d, weapon_range(player), hit);
    if (!ok)
        for (int i = 0; i < 3; ++i) hit[i] = o[i] + d[i] * kMissDistance;
    float cam[3];
    if (!vr::world_point_to_camera(view, hit, g_vr.unit_m, cam)) return;
    // Camera space (LH) -> anchor space (RH): undo the rail comfort rotation the hand goes through
    // (vrsys_hand_in_game), then flip z.
    const vr::Mat4 dl = comfort_rail_delta();
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = dl.m[0][i] * cam[0] + dl.m[1][i] * cam[1] + dl.m[2][i] * cam[2];
    const float anchor_rh[3] = {c[0], c[1], -c[2]};
    logf("vr handat %.1f %.1f: camera ray %s at world %.1f %.1f %.1f (%.2f m ahead)", g_handat[0], g_handat[1], ok ? "hits" : "misses",
         hit[0], hit[1], hit[2], cam[2]);
    vrsys_sim_aim_at(anchor_rh);
}

void gun_request_handat(float x, float y) {
    g_handat[0] = x, g_handat[1] = y;
    g_handat_pending = true;
}

// Sim `vr handseek`: scan the camera's view (a grid of cursors) with the game's ray cast, take the
// shootable entity nearest the screen centre, and aim the hand at the middle of its hit cells.
static void do_handseek(uintptr_t player) {
    float base[3], du[3], dv[3], o[4];
    if (!unproject_basis(base, du, dv) || !safe_copy(o, player + 0x1E0, 16)) return;
    const float range = weapon_range(player);
    struct Cell {
        uintptr_t entity;
        float x, y;
    };
    std::vector<Cell> cells;
    for (float y = -360; y <= 360; y += 30)
        for (float x = -600; x <= 600; x += 30) {
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = base[i] + x * du[i] + y * dv[i] - o[i];
            const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (float& c : d) c /= n;
            float hit[3];
            HitInfo info;
            if (cast_ray(o, d, range, hit, &info) && info.shootable) cells.push_back({info.entity, x, y});
        }
    uintptr_t best = 0;
    float best_r = 1e9f;
    for (const Cell& c : cells)
        if (c.x * c.x + c.y * c.y < best_r) best_r = c.x * c.x + c.y * c.y, best = c.entity;
    if (!best) {
        logf("vr handseek: nothing shootable in view (%zu cells hit)", cells.size());
        return;
    }
    float sx = 0, sy = 0;
    int n = 0;
    for (const Cell& c : cells)
        if (c.entity == best) sx += c.x, sy += c.y, ++n;
    logf("vr handseek: entity %p (%d grid cells) centred at cursor %.0f %.0f", reinterpret_cast<void*>(best), n, sx / n, sy / n);
    g_handat[0] = sx / n, g_handat[1] = sy / n;
    do_handat(player);
}

void gun_request_handseek() { g_seek_pending = true; }

// Dev `vr measure` (world-scale check): from the camera (the engine's camera position 0x9C7E88),
// the game's ray cast straight down (floor), up (ceiling) and level in 8 directions (walls); then a
// fine grid of the camera's view finds the shootable entity nearest the centre and reports the
// vertical extent of its hit points (a standing zombie: about its height) and its distance.
static std::atomic<bool> g_measure_pending;
void gun_request_measure() { g_measure_pending = true; }

static void do_measure(uintptr_t player) {
    float cam[3];
    if (!safe_copy(cam, va(0x9C7E88), 12)) return;
    const float u = g_vr.unit_m;
    auto dist = [&](float dx, float dy, float dz) {
        const float d[3] = {dx, dy, dz};
        float hit[3];
        if (!cast_ray(cam, d, 5000, hit)) return -1.f;
        return std::sqrt((hit[0] - cam[0]) * (hit[0] - cam[0]) + (hit[1] - cam[1]) * (hit[1] - cam[1]) + (hit[2] - cam[2]) * (hit[2] - cam[2]));
    };
    const float down = dist(0, -1, 0), up = dist(0, 1, 0);
    float walls[8];
    for (int i = 0; i < 8; ++i) walls[i] = dist(std::cos(i * 0.7853982f), 0, std::sin(i * 0.7853982f));
    logf("vr measure: camera at %.1f %.1f %.1f; floor %.1f units below (%.2f m), ceiling %.1f above (%.2f m) -> room height %.2f m", cam[0], cam[1],
         cam[2], down, down * u, up, up * u, down > 0 && up > 0 ? (down + up) * u : -1.f);
    logf("vr measure: level distances to walls (8 directions, m): %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f", walls[0] * u, walls[1] * u, walls[2] * u,
         walls[3] * u, walls[4] * u, walls[5] * u, walls[6] * u, walls[7] * u);
    // Zombies: every shootable entity in a fine grid of the view; extent of its hit points.
    float base[3], du[3], dv[3], o[4];
    if (!unproject_basis(base, du, dv) || !safe_copy(o, player + 0x1E0, 16)) return;
    struct Ent {
        uintptr_t e;
        float ymin, ymax, dsum, cx, cy;
        int n;
    };
    std::vector<Ent> ents;
    for (float y = -360; y <= 360; y += 6)
        for (float x = -640; x <= 640; x += 12) {
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = base[i] + x * du[i] + y * dv[i] - o[i];
            const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (float& c : d) c /= n;
            float hit[3];
            HitInfo info;
            if (!cast_ray(o, d, 5000, hit, &info) || !info.shootable) continue;
            const float dd = std::sqrt((hit[0] - o[0]) * (hit[0] - o[0]) + (hit[1] - o[1]) * (hit[1] - o[1]) + (hit[2] - o[2]) * (hit[2] - o[2]));
            Ent* f = nullptr;
            for (Ent& e : ents)
                if (e.e == info.entity) f = &e;
            if (!f) ents.push_back({info.entity, hit[1], hit[1], 0, 0, 0, 0}), f = &ents.back();
            f->ymin = (std::min)(f->ymin, hit[1]), f->ymax = (std::max)(f->ymax, hit[1]);
            f->dsum += dd, f->cx += x, f->cy += y, ++f->n;
        }
    for (const Ent& e : ents)
        logf("vr measure: shootable %p: %d cells around cursor %.0f %.0f, %.2f m away; hit points span y %.1f..%.1f = %.2f m (top %.2f m above "
             "the floor under the camera)",
             reinterpret_cast<void*>(e.e), e.n, e.cx / e.n, e.cy / e.n, e.dsum / e.n * u, e.ymin, e.ymax, (e.ymax - e.ymin) * u,
             down > 0 ? (e.ymax - (cam[1] - down)) * u : -1.f);
    if (ents.empty()) logf("vr measure: nothing shootable in view");
}

// Once per game frame (lazily, from the cursor setters): the hand's ray, where it meets the scene,
// and the cursor that projects there.
static void aim_update(uintptr_t player) {
    const unsigned frame = g_frame.load();
    {
        std::lock_guard lock(g_aim_mx);
        if (g_aim.frame == frame) return;
    }
    if (g_handat_pending.exchange(false)) do_handat(player);
    if (g_seek_pending.exchange(false)) do_handseek(player);
    if (g_measure_pending.exchange(false)) do_measure(player);
    Aim a;
    a.frame = frame;
    if (gun_aim_active() && hand_world_ray(&a.ray)) {
        a.valid = true;
        a.hit = cast_ray(a.ray.o, a.ray.d, weapon_range(player), a.point);
        if (!a.hit)
            for (int i = 0; i < 3; ++i) a.point[i] = a.ray.o[i] + a.ray.d[i] * kMissDistance;
        // The cursor whose camera ray (from the game's aim origin) passes through the aim point.
        float base[3], du[3], dv[3], o[4], d[3], p[3], t;
        if (unproject_basis(base, du, dv) && safe_copy(o, player + 0x1E0, 16)) {
            for (int i = 0; i < 3; ++i) d[i] = a.point[i] - o[i];
            a.cursor_ok = vr::cursor_on_line(base, du, dv, o, d, &a.cursor[0], &a.cursor[1], p, &t) && t > 0;
        }
    }
    std::lock_guard lock(g_aim_mx);
    g_aim = a;
}

bool gun_aim_point(float out[3], bool* hit) {
    std::lock_guard lock(g_aim_mx);
    if (!g_aim.valid || g_frame.load() - g_aim.frame > 30) return false;
    memcpy(out, g_aim.point, 12);
    *hit = g_aim.hit;
    return true;
}

// --- cursor setters ---

static bool cursor_override(uint8_t* player, int axis, float* v) {
    if (reinterpret_cast<uintptr_t>(player) != player1()) return false;
    {
        // Hits on player 1 (dev: `vr waithit` waits for one; god mode refills the health after it).
        auto health = reinterpret_cast<volatile int32_t*>(player + 0x20C);
        static int32_t last = -1;
        const int32_t h = *health;
        if (last >= 0 && h < last) {
            ++g_hits;
            logf("player 1 hit: health %d -> %d", int(last), int(h));
        }
        if (g_god && h > 0 && h < 10) *health = 10;
        last = *health;
    }
    aim_update(reinterpret_cast<uintptr_t>(player));
    std::lock_guard lock(g_aim_mx);
    if (!g_aim.valid || !g_aim.cursor_ok) return false;
    *v = g_aim.cursor[axis];
    return true;
}

static void __fastcall hk_setx(uint8_t* player, void* edx, float x) {
    cursor_override(player, 0, &x);
    o_setx(player, edx, x);
}

static void __fastcall hk_sety(uint8_t* player, void* edx, float y) {
    cursor_override(player, 1, &y);
    o_sety(player, edx, y);
}

// --- shots ---

static void shot_feedback(uintptr_t player) {
    uintptr_t data = 0;
    weapon_range(player, &data);
    uint32_t pellets = 1, type = 0, sub = 0;
    if (data) safe_copy(&pellets, data + 0x1E0, 4), safe_copy(&type, data + 0x20, 4), safe_copy(&sub, data + 0x24, 4);
    // Shotguns fire several pellets; the hand cannon is weapon kind 0xB (see gun_log_state).
    const bool strong = pellets > 1 || sub == 0xB;
    input_shot(strong ? 2.f : 1.f);
    static uintptr_t last_data;
    if (data != last_data) {
        last_data = data;
        logf("gun: weapon data %p: type %u kind %u pellets %u range %.0f -> %s haptics", reinterpret_cast<void*>(data), type, sub, pellets,
             weapon_range(player), strong ? "strong" : "normal");
    }
}

// --- the gun accuracy test's shot log (dev `vr shotlog 1`; tools/gun_accuracy.py) ---
// Lines (all `shotlog ...`, key=value, vectors as x,y,z, world positions in game units):
//   shot   once per player-1 FireBullets call from the weapon's fire: the ray the mod set (o, d:
//          what FireBullets got, spread-free), the barrel line hand_world_ray gave at fire time
//          (fire_o, fire_d) and the hand pose then, and the barrel of the last frame drawn before
//          the shot (drawn_*: gunmodel.cpp, from the world matrix the gun was drawn with) with
//          its frame number and the rail camera's turn speed.
//   pellet every ray cast FireBullets makes for that shot (0x5514E0, called from 0x50D279), the
//          game's own origin, direction (after its spread) and result; `depth` = FireBullets'
//          mode (0 the shot, 1+ its recursive calls for bullets that carry on through a hit).
//   proj   a projectile weapon's launch (0x4E1330): the origin the game wrote back (after its own
//          camera-space offset) and the direction it flies along (player+0x1F0 during the call).
//   tag    `vr shotlog tag NAME`: a scenario marker; on/off: the switch, with unit_m.
// Each FireBullets call for player 1 pushes a context (thread-local; FireBullets recurses), so
// pellets join their shot by id.
constexpr uintptr_t kCastRet = 0x50D27E;  // FireBullets' world ray cast (call at 0x50D279) returns here
using RayCastHook_t = int(__cdecl*)(int, const float*, const float*, int, void*);
static RayCastHook_t o_raycast;
static std::atomic<bool> g_shotlog;
static unsigned g_shot_id;  // game thread
struct ShotCtx {
    unsigned id;
    uintptr_t weapon;
};
static thread_local ShotCtx t_ctx[16];
static thread_local int t_ctx_n;

void gun_shotlog(bool on) {
    if (on && !o_raycast) logf("shotlog: the ray cast hook is not installed (development harness only): shots and projectiles only");
    g_shotlog = on;
    logf("shotlog %s frame=%u unit_m=%.5f", on ? "on" : "off", g_frame.load(), g_vr.unit_m);
}
bool gun_shotlog_on() { return g_shotlog; }
void gun_shotlog_tag(const std::string& name) { logf("shotlog tag name=%s frame=%u", name.c_str(), g_frame.load()); }

// The weapon's names and data for a log line: "weapon=... key=... type=... kind=... pellets=...".
static void weapon_fields(uintptr_t weapon, char* out, size_t n) {
    char name[64] = "?", key[16] = "?";
    gunmodel_weapon_name(weapon, name, key);
    const uintptr_t data = read_u32(weapon + 4);
    uint32_t type = 0, kind = 0, pellets = 0;
    if (data) safe_copy(&type, data + 0x20, 4), safe_copy(&kind, data + 0x24, 4), safe_copy(&pellets, data + 0x1E0, 4);
    snprintf(out, n, "weapon=%s key=%s data=%p type=%u kind=%u pellets=%u", name, key, reinterpret_cast<void*>(data), type, kind, pellets);
}

// The fire-time barrel (hand_world_ray, if the hand aims), the hand pose then, the barrel of the
// last frame drawn, and the rail camera's turn speed now.
static void barrel_fields(const vr::Ray* fire, char* out, size_t n) {
    int len = 0;
    if (fire) len += snprintf(out + len, n - len, "fire_o=%.3f,%.3f,%.3f fire_d=%.6f,%.6f,%.6f", fire->o[0], fire->o[1], fire->o[2], fire->d[0],
                              fire->d[1], fire->d[2]);
    else len += snprintf(out + len, n - len, "fire_o=- fire_d=-");
    vr::Mat4 h;
    if (vrsys_hand_in_game(&h))
        len += snprintf(out + len, n - len, " fire_hand=%.4f,%.4f,%.4f,%.5f,%.5f,%.5f", h.m[0][3], h.m[1][3], h.m[2][3], h.m[0][2], h.m[1][2], h.m[2][2]);
    float turn, turn_frame;
    comfort_rail_turn(&turn, &turn_frame);
    len += snprintf(out + len, n - len, " fire_turn=%.1f", turn);
    DrawnBarrel d;
    if (gunmodel_last_drawn(&d))
        snprintf(out + len, n - len,
                 " drawn=1 drawn_frame=%u drawn_key=%s drawn_o=%.3f,%.3f,%.3f drawn_d=%.6f,%.6f,%.6f drawn_hand=%.4f,%.4f,%.4f,%.5f,%.5f,%.5f "
                 "turn=%.1f turn_frame=%.1f",
                 d.frame, d.key, d.muzzle[0], d.muzzle[1], d.muzzle[2], d.dir[0], d.dir[1], d.dir[2], d.hand[0], d.hand[1], d.hand[2], d.hand[3],
                 d.hand[4], d.hand[5], d.turn, d.turn_frame);
    else snprintf(out + len, n - len, " drawn=0");
}

static void log_pellet(const float* org, const float* seg, uint8_t* res, int hit) {
    const ShotCtx& c = t_ctx[t_ctx_n - 1];
    const uintptr_t r = reinterpret_cast<uintptr_t>(res);
    // FireBullets' frame (its ebp = res + 0xEC): pellet index [ebp-0x150], pellet count [ebp-0x14C],
    // mode (byte) [ebp+0xC].
    uint32_t pellet = 0, count = 0;
    uint8_t mode = 0;
    safe_copy(&pellet, r - 0x64, 4), safe_copy(&count, r - 0x60, 4), safe_copy(&mode, r + 0xF8, 1);
    const float l = std::sqrt(seg[0] * seg[0] + seg[1] * seg[1] + seg[2] * seg[2]);
    const float il = l > 0 ? 1 / l : 0;
    HitInfo info;
    float p[3] = {};
    if (hit) memcpy(p, res, 12), seh_entity(res, &info);
    logf("shotlog pellet id=%u frame=%u pellet=%u of=%u depth=%u o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f len=%.1f hit=%d p=%.3f,%.3f,%.3f ent=%p shoot=%d",
         c.id, g_frame.load(), pellet, count, unsigned(mode), org[0], org[1], org[2], seg[0] * il, seg[1] * il, seg[2] * il, l, hit ? 1 : 0, p[0],
         p[1], p[2], reinterpret_cast<void*>(info.entity), int(info.shootable));
}

static int __cdecl hk_raycast(int a, const float* org, const float* seg, int b, void* res) {
    const int r = o_raycast(a, org, seg, b, res);
    if (g_shotlog && t_ctx_n > 0 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == va(kCastRet) && org && seg && res)
        log_pellet(org, seg, static_cast<uint8_t*>(res), r);
    return r;
}

static void __fastcall hk_fire(void* self, void* edx, float* origin, int mode, float travelled) {
    auto w = static_cast<uint8_t*>(self);
    auto dir = reinterpret_cast<float*>(w + 0x7B4);
    const uintptr_t owner = read_u32(reinterpret_cast<uintptr_t>(w + 0x794)), p1 = player1();
    const bool player_shot = (mode & 0xFF) == 0 && owner && owner == p1 &&
                             reinterpret_cast<uintptr_t>(_ReturnAddress()) == va(kFireRet);
    // Shot log: a new shot, or a bullet carrying on (FireBullets' recursion) inside a logged one.
    bool shotlog = false;
    unsigned shot_id = 0;
    if (g_shotlog && owner && owner == p1 && t_ctx_n < int(std::size(t_ctx))) {
        if (player_shot) shotlog = true, shot_id = ++g_shot_id;
        else if (t_ctx_n > 0) shotlog = true, shot_id = t_ctx[t_ctx_n - 1].id;
    }
    bool fire_ok = false;
    vr::Ray fire_ray{};
    alignas(16) float own[4] = {origin[0], origin[1], origin[2], origin[3]};
    float* use_origin = origin;
    static int logged;
    vr::Ray r;
    if (player_shot && gun_aim_active() && hand_world_ray(&r)) {
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len <= 0) len = 1;
        if (logged < 400) {
            // What the game's ray cast will meet along the hand's ray (before spread).
            float hit[3];
            HitInfo info;
            const bool h = cast_ray(r.o, r.d, weapon_range(owner), hit, &info);
            const float dist = h ? std::sqrt((hit[0] - r.o[0]) * (hit[0] - r.o[0]) + (hit[1] - r.o[1]) * (hit[1] - r.o[1]) +
                                             (hit[2] - r.o[2]) * (hit[2] - r.o[2]))
                                 : 0;
            logf("shot: game origin %.1f %.1f %.1f dir %.4f %.4f %.4f -> hand origin %.1f %.1f %.1f dir %.4f %.4f %.4f; ray %s", origin[0],
                 origin[1], origin[2], dir[0] / len, dir[1] / len, dir[2] / len, r.o[0], r.o[1], r.o[2], r.d[0], r.d[1], r.d[2],
                 !h ? "hits nothing" : info.shootable ? "hits a shootable entity" : info.entity ? "hits an entity (not shootable)" : "hits the scene");
            if (h)
                logf("  hit at %.1f %.1f %.1f (%.2f m from the hand), entity %p vtable %p", hit[0], hit[1], hit[2], dist * g_vr.unit_m,
                     reinterpret_cast<void*>(info.entity), reinterpret_cast<void*>(info.vtable));
        }
        for (int i = 0; i < 3; ++i) dir[i] = r.d[i] * len, own[i] = r.o[i];
        use_origin = own;
        fire_ok = true, fire_ray = r;
    } else if (logged < 400) {
        logf("FireBullets weapon=%p mode=%d travelled=%g origin %.1f %.1f %.1f dir %.4f %.4f %.4f (%s)", self, mode & 0xFF, travelled,
             origin[0], origin[1], origin[2], dir[0], dir[1], dir[2], player_shot ? "player 1, mouse aim" : "not overridden");
    }
    ++logged;
    {
        std::lock_guard lock(g_dev_mx);
        if (g_capture_next) {
            g_capture_next = false;
            const float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            for (int i = 0; i < 3; ++i) g_dev_aim_dir[i] = n > 0 ? dir[i] / n : 0;
            g_dev_dir = true;
            logf("  -> direction captured for later shots");
        } else if (g_dev_dir) {
            float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len <= 0) len = 1;
            for (int i = 0; i < 3; ++i) dir[i] = g_dev_aim_dir[i] * len;
            logf("  -> dir overridden %.4f %.4f %.4f", dir[0], dir[1], dir[2]);
        }
        if (g_dev_origin) {
            for (int i = 0; i < 3; ++i) own[i] = g_dev_aim_origin[i];
            use_origin = own;
            logf("  -> origin overridden %.1f %.1f %.1f", own[0], own[1], own[2]);
        }
    }
    if (player_shot) shot_feedback(owner);
    if (shotlog && player_shot) {
        char wf[160], bf[640];
        weapon_fields(reinterpret_cast<uintptr_t>(self), wf, sizeof wf);
        barrel_fields(fire_ok ? &fire_ray : nullptr, bf, sizeof bf);
        const float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), in = n > 0 ? 1 / n : 0;
        logf("shotlog shot id=%u frame=%u %s range=%.0f aim=%s o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f %s", shot_id, g_frame.load(), wf,
             weapon_range(owner), fire_ok ? "hand" : "mouse", use_origin[0], use_origin[1], use_origin[2], dir[0] * in, dir[1] * in, dir[2] * in,
             bf);
    }
    if (shotlog) t_ctx[t_ctx_n++] = {shot_id, reinterpret_cast<uintptr_t>(self)};
    o_fire(self, edx, use_origin, mode, travelled);
    if (shotlog) --t_ctx_n;
}

// The camera-space offset 0x4E1330 adds to a projectile's origin, as the game computes it.
static bool seh_proj_offset(float out[4]) {
    __try {
        const uintptr_t gc = *reinterpret_cast<const volatile uintptr_t*>(va(kGameControl));
        const uintptr_t cam = *reinterpret_cast<const uintptr_t*>(gc + 0x64);
        const uintptr_t node = *reinterpret_cast<const uintptr_t*>(cam + 0xDC);
        alignas(16) float mat[16];
        alignas(16) const float v[4] = {-5, -10, 0, 0};
        alignas(16) float r[4] = {};
        reinterpret_cast<QuatToMat_t>(va(0x6A44A0))(mat, reinterpret_cast<const float*>(node + 0x14));
        reinterpret_cast<MatVec_t>(va(0x6A2A50))(r, mat, v);
        memcpy(out, r, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Shot log: a projectile's launch. 0x4E1330 writes the launch point (origin + its offset) back
// through the origin pointer and into the projectile, and launches it along player+0x1F0 at the
// weapon's speed (data+0x258); when it has no projectile to launch it returns early and the
// origin is unchanged (`launched=0`). Where it lands is not logged (no collision hook).
static void log_projectile(uintptr_t weapon, const float* launch, const float before[3], const float* dir, const vr::Ray* fire) {
    char wf[160], bf[640];
    weapon_fields(weapon, wf, sizeof wf);
    barrel_fields(fire, bf, sizeof bf);
    float speed = 0;
    const uintptr_t data = read_u32(weapon + 4);
    if (data) safe_copy(&speed, data + 0x258, 4);
    const bool launched = launch[0] != before[0] || launch[1] != before[1] || launch[2] != before[2];
    const float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), in = n > 0 ? 1 / n : 0;
    logf("shotlog proj id=%u frame=%u %s aim=%s launched=%d speed=%.1f o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f %s", ++g_shot_id, g_frame.load(), wf,
         fire ? "hand" : "mouse", int(launched), speed, launch[0], launch[1], launch[2], dir[0] * in, dir[1] * in, dir[2] * in, bf);
}

static void __fastcall hk_fire_proj(void* self, void* edx, float* origin) {
    auto w = static_cast<uint8_t*>(self);
    const uintptr_t owner = read_u32(reinterpret_cast<uintptr_t>(w + 0x794));
    vr::Ray r;
    float off[4];
    if (owner && owner == player1() && gun_aim_active() && hand_world_ray(&r) && seh_proj_offset(off)) {
        auto pdir = reinterpret_cast<float*>(owner + 0x1F0);
        float saved[3];
        memcpy(saved, pdir, 12);
        memcpy(pdir, r.d, 12);
        alignas(16) float own[4] = {r.o[0] - off[0], r.o[1] - off[1], r.o[2] - off[2], origin[3]};
        logf("shot (projectile): hand origin %.1f %.1f %.1f dir %.4f %.4f %.4f (offset %.1f %.1f %.1f)", r.o[0], r.o[1], r.o[2], r.d[0],
             r.d[1], r.d[2], off[0], off[1], off[2]);
        shot_feedback(owner);
        const float before[3] = {own[0], own[1], own[2]};
        o_fire_proj(self, edx, own);
        if (g_shotlog) log_projectile(reinterpret_cast<uintptr_t>(self), own, before, pdir, &r);
        memcpy(pdir, saved, 12);
        return;
    }
    const float before[3] = {origin[0], origin[1], origin[2]};
    o_fire_proj(self, edx, origin);
    if (g_shotlog && owner && owner == player1())
        log_projectile(reinterpret_cast<uintptr_t>(self), origin, before, reinterpret_cast<float*>(owner + 0x1F0), nullptr);
}

// Sphere checks: the cursor and aim origin moved onto the hand's ray for the call.
struct SphereOverride {
    uint8_t* p = nullptr;
    float cursor[2], origin[3];
    SphereOverride(uint8_t* player, const char* name) {
        if (!player || reinterpret_cast<uintptr_t>(player) != player1() || !gun_aim_active()) return;
        vr::Ray r;
        float base[3], du[3], dv[3], x, y, pt[3], t;
        if (!hand_world_ray(&r) || !unproject_basis(base, du, dv) || !vr::cursor_on_line(base, du, dv, r.o, r.d, &x, &y, pt, &t))
            return;
        if (std::fabs(t) > 1000) return;  // the hand's ray nearly parallel to the near plane
        p = player;
        auto c = reinterpret_cast<float*>(p + 0x1BC);
        auto o = reinterpret_cast<float*>(p + 0x1E0);
        memcpy(cursor, c, 8);
        memcpy(origin, o, 12);
        c[0] = x, c[1] = y;
        for (int i = 0; i < 3; ++i) o[i] = pt[i] - r.d[i] * 100;
        static int logged;
        if (logged++ < 10) {
            float q[4];
            seh_unproject(x, y, q);
            logf("gun: %s on the hand's ray: cursor %.1f %.1f (was %.1f %.1f), near-plane point %.2f %.2f %.2f (check %.2f %.2f %.2f)",
                 name, x, y, cursor[0], cursor[1], pt[0], pt[1], pt[2], q[0], q[1], q[2]);
        }
    }
    ~SphereOverride() {
        if (!p) return;
        memcpy(p + 0x1BC, cursor, 8);
        memcpy(p + 0x1E0, origin, 12);
    }
};

static unsigned __fastcall hk_sphere_a(void* self, void* edx, uint8_t* player) {
    SphereOverride o(player, "sphere check 0x42B900");
    return o_sphere_a(self, edx, player);
}

static unsigned __fastcall hk_sphere_b(void* self, void* edx, uint8_t* player) {
    SphereOverride o(player, "sphere check 0x479250");
    return o_sphere_b(self, edx, player);
}

// --- input actions (diagnostics) ---
// 0x426A20 (thiscall input, player*, player index, bool; ret 0xC) turns the input map's action
// values into player actions: 3 fire (0x511650; left click), 4 grenade (0x40DE20; right click),
// 5 reload (0x551C20 -> weapon+0x78F; middle click; also the motion "shake" gesture via 0x47D190),
// 0xB next weapon (0x485060 -> 0x4F9A50; TAB), 0xC previous weapon (0x4E9C10 -> 0x5564E0); 24/25
// the mouse cursor in client pixels (seen in tests 2026-11-17). Action value objects:
// [[input + 0x2C + index*4] + 0x6C][action] + 0x2C (int; the bound inputs' sum, clamped to +0x34).
using Actions_t = void(__fastcall*)(void* input, void* edx, void* player, int index, int flag);
static Actions_t o_actions;
static std::atomic<bool> g_log_actions;
void gun_log_actions(bool on) { g_log_actions = on; }

static void __fastcall hk_actions(void* input, void* edx, void* player, int index, int flag) {
    if (g_log_actions && index >= 0 && index < 4) {
        static int last[0x20];
        const uintptr_t set = read_u32(reinterpret_cast<uintptr_t>(input) + 0x2C + index * 4);
        const uintptr_t arr = set ? read_u32(set + 0x6C) : 0;
        for (int a = 0; arr && a < 0x20; ++a) {
            const uintptr_t obj = read_u32(arr + a * 4);
            int v = 0;
            if (obj && safe_copy(&v, obj + 0x2C, 4) && v != last[a]) {
                logf("input action %d (player %d): %d -> %d", a, index, last[a], v);
                last[a] = v;
            }
        }
    }
    o_actions(input, edx, player, index, flag);
}

// --- `vr gun` ---

void gun_log_state() {
    const uintptr_t p = player1();
    const uintptr_t weapon = p ? read_u32(p + 0x224) : 0;
    uintptr_t data = 0;
    const float range = p ? weapon_range(p, &data) : 0;
    uint32_t ammo = 0, type = 0, sub = 0, pellets = 0, clip = 0;
    uint8_t flags[0x24] = {};
    if (weapon) safe_copy(&ammo, weapon + 0x760, 4), safe_copy(flags, weapon + 0x78C, sizeof flags);
    if (data) safe_copy(&type, data + 0x20, 4), safe_copy(&sub, data + 0x24, 4), safe_copy(&pellets, data + 0x1E0, 4), safe_copy(&clip, data + 0x1C4, 4);
    float cur[2] = {}, org[3] = {}, dir[3] = {};
    if (p) safe_copy(cur, p + 0x1BC, 8), safe_copy(org, p + 0x1E0, 12), safe_copy(dir, p + 0x1F0, 12);
    logf("vr gun: player %p weapon %p data %p type %u kind %u pellets %u clip %u ammo %u range %.0f; flags +78C %02x %02x %02x %02x %02x "
         "+7A0 %02x; cursor %.1f %.1f; aim origin %.1f %.1f %.1f dir %.4f %.4f %.4f",
         reinterpret_cast<void*>(p), reinterpret_cast<void*>(weapon), reinterpret_cast<void*>(data), type, sub, pellets, clip, ammo, range,
         flags[0], flags[1], flags[2], flags[3], flags[4], flags[0x14], cur[0], cur[1], org[0], org[1], org[2], dir[0], dir[1], dir[2]);
    Aim a;
    {
        std::lock_guard lock(g_aim_mx);
        a = g_aim;
    }
    // The sphere checks' override for the current hand ray: the cursor whose near-plane point lies
    // on the ray, and how far the game's ScreenToWorld puts that cursor from it (should be ~0).
    vr::Ray r;
    float base[3], du[3], dv[3], x, y, pt[3], t, q[4];
    if (hand_world_ray(&r) && unproject_basis(base, du, dv) && vr::cursor_on_line(base, du, dv, r.o, r.d, &x, &y, pt, &t) &&
        seh_unproject(x, y, q)) {
        const float e = std::sqrt((q[0] - pt[0]) * (q[0] - pt[0]) + (q[1] - pt[1]) * (q[1] - pt[1]) + (q[2] - pt[2]) * (q[2] - pt[2]));
        logf("vr gun: sphere-check cursor for the hand ray %.1f %.1f, near-plane point %.2f %.2f %.2f (%.1f units behind the hand), "
             "ScreenToWorld of it off by %.4f units",
             x, y, pt[0], pt[1], pt[2], -t, e);
    }
    logf("vr gun: aim %s (frame %u, now %u): hand %.1f %.1f %.1f dir %.4f %.4f %.4f -> %s %.1f %.1f %.1f, cursor %s %.1f %.1f",
         a.valid ? "on" : "off", a.frame, g_frame.load(), a.ray.o[0], a.ray.o[1], a.ray.o[2], a.ray.d[0], a.ray.d[1], a.ray.d[2],
         a.hit ? "hits" : "misses, 10 m:", a.point[0], a.point[1], a.point[2], a.cursor_ok ? "" : "(none)", a.cursor[0], a.cursor[1]);
}

void install_gun_hooks() {
    static bool done;
    if (done) return;
    done = true;
    static const uint8_t fire[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x74, 0x03, 0x00, 0x00};
    static const uint8_t proj[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00};
    static const uint8_t sphere_a[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x70};
    static const uint8_t sphere_b[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x78};
    static const uint8_t setter[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    o_fire = reinterpret_cast<Fire_t>(install_detour(0x50D110, fire, sizeof fire, reinterpret_cast<void*>(hk_fire), "FireBullets 0x50D110"));
    // The gun accuracy test's per-bullet log (`vr shotlog 1`): the world ray cast, development
    // harness only (a release install never hooks it). Off, it only passes the call on.
    static const uint8_t cast[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xD4, 0x01, 0x00, 0x00};
    if (dev_harness())
        o_raycast = reinterpret_cast<RayCastHook_t>(
            install_detour(0x5514E0, cast, sizeof cast, reinterpret_cast<void*>(hk_raycast), "world ray cast 0x5514E0 (shot log)"));
    if (g_vr.mode == VrMode::off) return;  // mode=off: only the probe's logging FireBullets hook
    o_fire_proj = reinterpret_cast<FireProj_t>(
        install_detour(0x4E1330, proj, sizeof proj, reinterpret_cast<void*>(hk_fire_proj), "projectile shot 0x4E1330"));
    o_sphere_a = reinterpret_cast<Sphere_t>(
        install_detour(0x42B900, sphere_a, sizeof sphere_a, reinterpret_cast<void*>(hk_sphere_a), "sphere check 0x42B900"));
    o_sphere_b = reinterpret_cast<Sphere_t>(
        install_detour(0x479250, sphere_b, sizeof sphere_b, reinterpret_cast<void*>(hk_sphere_b), "sphere check 0x479250"));
    o_setx = reinterpret_cast<SetCursor_t>(install_detour(0x5295F0, setter, sizeof setter, reinterpret_cast<void*>(hk_setx), "SetCursorX 0x5295F0"));
    o_sety = reinterpret_cast<SetCursor_t>(install_detour(0x51FBD0, setter, sizeof setter, reinterpret_cast<void*>(hk_sety), "SetCursorY 0x51FBD0"));
    static const uint8_t actions[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    o_actions = reinterpret_cast<Actions_t>(install_detour(0x426A20, actions, sizeof actions, reinterpret_cast<void*>(hk_actions), "input actions 0x426A20"));
    // The aim needs all of them; a detour that didn't go in (bytes differ) turns controller aiming off.
    const bool all = o_fire && o_fire_proj && o_sphere_a && o_sphere_b && o_setx && o_sety;
    g_hooks_installed = all;
    logf("gun: controller aim %s", all ? "ready" : "OFF (a shot-path detour could not be installed)");
}

}  // namespace hv
