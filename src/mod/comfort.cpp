// Comfort for VR (milestone 4), sim/openvr only (mode=off installs none of it). Installed at the
// first Present, after SecuROM's start-up patching; each detour checks its bytes first.
//
// - The game's first-person arms ([vr] game_arms, default 0 = hidden). Some weapon/character
//   combinations play a head-locked reload animation (findings-probe.md: the SMG with Varla in
//   Naked Terror - skinned draws of the arms rig). Entities are drawn by 0x7697A0 (thiscall, one
//   argument, ret 4; vtable slot 0xA4 of cSkinnedAttachment and the other drawable entity
//   classes, called by the scene list walker 0x523FC0); the entity's actor instance is at
//   E+0x48 and its actor resource ACTR at I+0x8C, whose name is at [ACTR+0x6C]. A reload frame
//   (sim, Naked Terror, Varla, SMG) draws three entities that are not drawn otherwise: the arms rig
//   - a cPlayerHUDWeaponDisplay (vtable 0x800BCC; its actor is called "Motion") - and the weapon's
//   own GunArms_SMG and GunArms_SMG_Clip (entities of the static-mesh class, vtable 0x813768),
//   which the game never draws outside the animation. With game_arms=0 all of them are skipped:
//   every cPlayerHUDWeaponDisplay and every actor named GunArms_* (the mod draws the weapon in the
//   hand itself). The hands version can turn them back on and re-pose them.
//
// - Camera shake and smack ([vr] camera_shake, default 0 = off). cCamera's update 0x4DCA50 adds
//   two angle offsets to the camera's orientation: 0x4BF210 (the shake: spring-damped, driven by
//   the two shake slots at cCameraManager+0xA4 that CameraSetShakeSettings fills) and 0x529CA0
//   (the smack, clamped by cCameraManager+0xF4.. that CameraSetSmackSettings fills). Both are
//   thiscall (cCamera*, float out[3]) returning out, ret 4. The originals still run (their spring
//   state stays consistent); with camera_shake=0 their outputs are zeroed.
//
// - Head-bob ([vr] head_bob, default 0 = off). The moving head-bob in 0x4DCA50 adds
//   |sin| * cCameraManager+0x8C (up) and sin * +0x90 (sideways) to the camera position while the
//   rail moves faster than +0x98 (wavelength +0x94); CameraSetMovingHeadBob writes all four
//   (defaults 6.5, 7, 250, 10). With head_bob=0 the two amplitudes are held at 0 (re-applied each
//   frame: the manager is made per level and scripts may rewrite them); the game's values come back
//   when it is switched on live. The static head-bob values (+0x9C/+0xA0) are written by their
//   script function but nothing reads them in this build. Sim check (Naked Terror): with the game's
//   amplitudes restored the camera moved 1500 units along the rail at a constant height, so this
//   build does not seem to bob there at all; holding them at 0 is a safety net.
//
// - WiiView edge-pan: always off in VR. 0x55F5C0 (thiscall on cCameraManager, no arguments) says
//   whether the cursor at the screen edge pans the camera (byte +0x134 and cGameSettings+0x46 == 0);
//   in VR it answers 0, because the gun's cursor sits at the edge whenever it points off-screen.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace hv {

constexpr uintptr_t kCameraManager = 0x99B8F4;
constexpr uintptr_t kCamPos = 0x9C7E88;  // the engine's camera position (vec3), written with the view

static uintptr_t rd(uintptr_t addr) {
    uintptr_t v = 0;
    return safe_copy(&v, addr, 4) ? v : 0;
}

// --- first-person arms ---

using EntityDraw_t = void(__fastcall*)(void* entity, void* edx, int arg);
static EntityDraw_t o_entity_draw;
static std::atomic<bool> g_log_actors;
static std::atomic<long> g_arms_hidden, g_arms_drawn;
static std::mutex g_seen_mx;
static std::unordered_set<uintptr_t> g_seen;  // actor resources already logged
constexpr uintptr_t kHudWeaponDisplayVtbl = 0x800BCC;  // cPlayerHUDWeaponDisplay (RTTI)

static bool actor_name(uintptr_t entity, char* name, size_t n, uintptr_t* actr_out) {
    const uintptr_t inst = rd(entity + 0x48), actr = inst ? rd(inst + 0x8C) : 0, p = actr ? rd(actr + 0x6C) : 0;
    *actr_out = actr;
    if (!p || n < 2) return false;
    memset(name, 0, n);
    // Read in small pieces: a name near the end of a page must not fault the whole read.
    for (size_t off = 0; off + 8 <= n - 1; off += 8) {
        if (!safe_copy(name + off, p + off, 8)) break;
        if (memchr(name + off, 0, 8)) break;
    }
    name[n - 1] = 0;
    return name[0] != 0;
}

static void __fastcall hk_entity_draw(void* entity, void* edx, int arg) {
    char name[64];
    uintptr_t actr = 0;
    const bool named = actor_name(reinterpret_cast<uintptr_t>(entity), name, sizeof name, &actr);
    const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
    const bool rig = rd(e) == va(kHudWeaponDisplayVtbl);
    const bool arms = rig || (named && _strnicmp(name, "GunArms_", 8) == 0);
    const bool hide = arms && g_vr.stereo && !g_vr.game_arms;
    if (named && (arms || g_log_actors.load())) {
        bool fresh;
        {
            std::lock_guard lock(g_seen_mx);
            fresh = g_seen.insert(actr).second;
        }
        if (fresh)
            logf("game arms: entity %p (vtable %p) draws actor %s%s%s", entity, reinterpret_cast<void*>(rd(e)), name,
                 rig ? " - a player's first-person weapon display (cPlayerHUDWeaponDisplay: the arms)" : "",
                 arms ? (hide ? " - first-person view model, hidden (game_arms=0)" : " - first-person view model, drawn (game_arms=1)") : "");
    }
    if (arms) ++(hide ? g_arms_hidden : g_arms_drawn);
    if (hide) return;
    o_entity_draw(entity, edx, arg);
}

// --- camera shake / smack ---

using Offset_t = float*(__fastcall*)(void* camera, void* edx, float* out);
static Offset_t o_shake, o_smack;
static std::atomic<bool> g_log_shake;
static std::atomic<int> g_shake_lines;
static float g_max_shake, g_max_smack;  // largest offset magnitude since the last report (radians)
static std::mutex g_shake_mx;

static float* offset_common(const char* what, float* out, float* max_seen) {
    if (!out) return out;
    const float m = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    {
        std::lock_guard lock(g_shake_mx);
        if (m > *max_seen) *max_seen = m;
    }
    if (m > 1e-5f && g_log_shake.load() && g_shake_lines++ < 400)
        logf("camera %s offset %.5f %.5f %.5f (%.3f deg)%s", what, out[0], out[1], out[2], m * 57.29578f,
             g_vr.camera_shake ? "" : " -> zeroed");
    if (!g_vr.camera_shake) out[0] = out[1] = out[2] = 0;
    return out;
}

static float* __fastcall hk_shake(void* camera, void* edx, float* out) {
    float* r = o_shake(camera, edx, out);
    offset_common("shake", out, &g_max_shake);
    return r;
}

static float* __fastcall hk_smack(void* camera, void* edx, float* out) {
    float* r = o_smack(camera, edx, out);
    offset_common("smack", out, &g_max_smack);
    return r;
}

// --- WiiView edge-pan ---

using Bool_t = unsigned(__fastcall*)(void* self, void* edx);
static Bool_t o_wiiview;
static std::atomic<long> g_wiiview_asked, g_wiiview_would;
static std::atomic<bool> g_wiiview_allow;  // dev `vr wiiview 1`
void comfort_allow_wiiview(bool on) { g_wiiview_allow = on; }

static unsigned __fastcall hk_wiiview(void* manager, void* edx) {
    const unsigned game = o_wiiview(manager, edx);
    ++g_wiiview_asked;
    if (game) {
        if (g_wiiview_would++ == 0) logf("wiiview: the game asked for the edge-pan (cCameraManager+0x134 set); kept off in VR");
    }
    return g_wiiview_allow.load() ? game : 0;
}

// --- head-bob ---

struct BobState {
    uintptr_t manager = 0;
    float saved[2] = {6.5f, 7.0f};  // the game's amplitudes (ctor defaults until read)
    bool zeroed = false;
};
static BobState g_bob;

static void bob_frame() {
    const uintptr_t m = *reinterpret_cast<const volatile uintptr_t*>(va(kCameraManager));
    if (m != g_bob.manager) g_bob = BobState{m};
    if (!m) return;
    float amp[2];
    if (!safe_copy(amp, m + 0x8C, 8)) return;
    auto field = reinterpret_cast<float*>(m + 0x8C);
    if (!g_vr.head_bob) {
        if (amp[0] != 0 || amp[1] != 0) {
            g_bob.saved[0] = amp[0], g_bob.saved[1] = amp[1];
            field[0] = field[1] = 0;
            logf("head-bob: off (the game's amplitudes %.2f up, %.2f sideways held at 0)", amp[0], amp[1]);
        }
        g_bob.zeroed = true;
    } else if (g_bob.zeroed) {
        field[0] = g_bob.saved[0], field[1] = g_bob.saved[1];
        g_bob.zeroed = false;
        logf("head-bob: on (amplitudes %.2f, %.2f restored)", g_bob.saved[0], g_bob.saved[1]);
    }
}

// --- camera log (dev `vr camlog N`): the camera position per frame, for head-bob/shake checks ---

static std::atomic<int> g_camlog;
static void camlog_frame() {
    if (g_camlog.load() <= 0) return;
    float p[3], v[16];
    if (!safe_copy(p, va(kCamPos), 12) || !safe_copy(v, va(0x9C7E48), sizeof v)) return;
    // View (D3DX row-vector): the camera's forward axis is its third column.
    logf("camlog %d: pos %.2f %.2f %.2f fwd %.4f %.4f %.4f right-y %.4f", g_camlog.load(), p[0], p[1], p[2], v[2], v[6], v[10], v[1]);
    --g_camlog;
}

void comfort_camlog(int frames) { g_camlog = frames; }
void comfort_log_shake(bool on) {
    g_log_shake = on;
    g_shake_lines = 0;
}
void comfort_log_actors(bool on) {
    g_log_actors = on;
    std::lock_guard lock(g_seen_mx);
    g_seen.clear();
}

void comfort_log() {
    float shake, smack;
    {
        std::lock_guard lock(g_shake_mx);
        shake = g_max_shake, smack = g_max_smack;
        g_max_shake = g_max_smack = 0;
    }
    float amp[2] = {};
    const uintptr_t m = *reinterpret_cast<const volatile uintptr_t*>(va(kCameraManager));
    uint8_t pan = 0;
    if (m) safe_copy(amp, m + 0x8C, 8), safe_copy(&pan, m + 0x134, 1);
    logf("comfort: game_arms=%d (GunArms draws: %ld hidden, %ld drawn), camera_shake=%d (largest offsets since last: shake %.3f deg, "
         "smack %.3f deg), head_bob=%d (amplitudes now %.2f %.2f), wiiview edge-pan off (asked %ld, game wanted it %ld times, "
         "manager+0x134 = %u)",
         int(g_vr.game_arms), g_arms_hidden.load(), g_arms_drawn.load(), int(g_vr.camera_shake), shake * 57.29578f, smack * 57.29578f,
         int(g_vr.head_bob), amp[0], amp[1], g_wiiview_asked.load(), g_wiiview_would.load(), unsigned(pan));
}

static void install() {
    static const uint8_t entity_draw[7] = {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1};  // push ebp; mov ebp,esp; push ebx; push esi; mov esi,ecx
    static const uint8_t smack[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};             // push ebp; mov ebp,esp; sub esp,0x18
    static const uint8_t wiiview[7] = {0x80, 0xB9, 0x34, 0x01, 0x00, 0x00, 0x00};      // cmp byte [ecx+0x134],0
    // push ebp; mov ebp,esp; mov eax,[0x9B01A8] (an absolute address: relocated with the image)
    uint8_t shake[8] = {0x55, 0x8B, 0xEC, 0xA1};
    const uint32_t abs = uint32_t(va(0x9B01A8));
    memcpy(shake + 4, &abs, 4);
    o_entity_draw = reinterpret_cast<EntityDraw_t>(install_detour(0x7697A0, entity_draw, sizeof entity_draw, reinterpret_cast<void*>(hk_entity_draw), "entity draw 0x7697A0"));
    o_shake = reinterpret_cast<Offset_t>(install_detour(0x4BF210, shake, sizeof shake, reinterpret_cast<void*>(hk_shake), "camera shake 0x4BF210"));
    o_smack = reinterpret_cast<Offset_t>(install_detour(0x529CA0, smack, sizeof smack, reinterpret_cast<void*>(hk_smack), "camera smack 0x529CA0"));
    o_wiiview = reinterpret_cast<Bool_t>(install_detour(0x55F5C0, wiiview, sizeof wiiview, reinterpret_cast<void*>(hk_wiiview), "wiiview edge-pan 0x55F5C0"));
    logf("comfort: game_arms=%d camera_shake=%d head_bob=%d; wiiview edge-pan off", int(g_vr.game_arms), int(g_vr.camera_shake), int(g_vr.head_bob));
}

void comfort_frame() {
    if (g_vr.mode == VrMode::off) return;
    static bool installed;
    if (!installed) {
        installed = true;
        install();
    }
    bob_frame();
    camlog_frame();
}

}  // namespace hv
