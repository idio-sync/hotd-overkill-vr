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
//
// - Rail camera comfort ([vr] comfort_vignette, vignette_*, comfort_smooth_ms, comfort_level;
//   `vr comfort ...`): the rail camera's turns and moves can be fast and abrupt. The view is
//   placed on a comfort camera that follows the game camera's orientation through an exponential
//   filter (comfort_smooth_ms; lag capped at cone_deg/2 because culling only covers the cone, no
//   smoothing across camera cuts), with the camera's roll (or roll and pitch) removed
//   (comfort_level); the eyes and the gun hand both go through it, so shots still fly along the
//   barrel the player sees. Each eye's edges darken while the rail turns or moves fast
//   (comfort_vignette, driven by the camera's speed over ~100 ms; camera cuts don't count).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../vr/comfort.h"

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
    comfort_log_settings("vr comfort");
}

// --- rail camera comfort: smoothing, levelling, vignette (src/vr/comfort.h has the math) ---
//
// stereo.cpp calls comfort_rail_frame once per stereo frame (its first perspective scene draw)
// with the frame's view matrix; the filter's rotation D then goes into the eye poses and the gun
// hand (vrsys.cpp) for that frame and the game frame after it (the shots aim through the view of
// the last frame drawn). Flat frames don't update it: the first stereo frame after them starts
// afresh (snaps). The vignette is drawn into each eye's image at Present (stereo.cpp).

static std::mutex g_rail_mx;
static vr::RailComfort g_rail;
static vr::Mat4 g_rail_delta = vr::identity();
static float g_rail_vignette;
static LONGLONG g_rail_last_qpc;

struct RailSample {
    double t;           // seconds (QPC)
    float turn, move;   // smoothed speeds, deg/s and m/s
    float raw_turn, raw_move;
    float vignette;
    float lag;          // degrees between the view's and the game camera's forward axes
    float cam_roll, view_roll;  // degrees the right axis tilts from the horizon: rail camera, the view
    uint8_t cut, capped;
};
static std::vector<RailSample> g_rail_samples;  // ring buffer
static size_t g_rail_next;
static constexpr size_t kRailSamples = 1 << 15;  // ~4.5 min at 120 Hz

static double qpc_seconds(LONGLONG t) {
    static LONGLONG f;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    return double(t) / double(f);
}

static vr::RailComfortSettings rail_settings() {
    vr::RailComfortSettings s;
    s.enabled = g_vr.comfort_on;
    s.smooth_ms = g_vr.comfort_smooth_ms;
    s.level = g_vr.comfort_level;
    s.cone_deg = g_vr.cone_deg;
    s.vignette = g_vr.comfort_vignette;
    s.turn_dps[0] = g_vr.vignette_turn_dps[0], s.turn_dps[1] = g_vr.vignette_turn_dps[1];
    s.move_mps[0] = g_vr.vignette_move_mps[0], s.move_mps[1] = g_vr.vignette_move_mps[1];
    s.ease_ms = g_vr.vignette_ease_ms;
    return s;
}

void comfort_rail_frame(const vr::Mat4& view) {
    vr::Mat4 cam;
    if (!vr::camera_pose_from_view(view, &cam)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = g_rail_last_qpc ? float(qpc_seconds(now.QuadPart - g_rail_last_qpc)) : 0.f;
    g_rail_last_qpc = now.QuadPart;
    std::lock_guard lock(g_rail_mx);
    g_rail.update(cam, g_vr.unit_m, dt, rail_settings());
    g_rail_delta = g_rail.delta();
    g_rail_vignette = g_rail.vignette();
    static int cuts_logged, cut_followup;
    if (g_rail.cut() && cuts_logged++ < 50) {
        logf("comfort: camera cut (no smoothing across it, not counted as speed): turn %.0f deg/s, move %.1f m/s, vignette %.2f",
             g_rail.turn_dps(), g_rail.move_mps(), g_rail.vignette());
        cut_followup = 30;
    } else if (cut_followup > 0 && --cut_followup == 0) {
        logf("comfort: 30 frames after the cut: turn %.0f deg/s, move %.1f m/s, vignette %.2f", g_rail.turn_dps(), g_rail.move_mps(),
             g_rail.vignette());
    }
    static bool was_capped;
    if (g_rail.capped() != was_capped) {
        static int capped_logged;
        if (capped_logged++ < 50)
            logf(g_rail.capped() ? "comfort: smoothing lag reached its cap (%.1f deg = cone_deg/2)" : "comfort: smoothing lag back under its cap (%.1f deg)",
                 g_rail.lag_deg());
        was_capped = g_rail.capped();
    }
    // Roll: how far the right axis tilts out of the horizontal plane (+y up), the camera's and the view's.
    auto roll = [](const vr::Mat4& r) {
        const float n = std::sqrt(r.m[0][0] * r.m[0][0] + r.m[1][0] * r.m[1][0] + r.m[2][0] * r.m[2][0]);
        return n > 0 ? std::asin(std::fmax(-1.f, std::fmin(1.f, r.m[1][0] / n))) * 57.29578f : 0.f;
    };
    const vr::Mat4 view_rot = vr::mul(cam, g_rail_delta);
    if (g_rail_samples.size() < kRailSamples) g_rail_samples.resize(kRailSamples);
    g_rail_samples[g_rail_next % kRailSamples] = {qpc_seconds(now.QuadPart), g_rail.turn_dps(), g_rail.move_mps(), g_rail.raw_turn_dps(),
                                                  g_rail.raw_move_mps(), g_rail.vignette(), g_rail.lag_deg(), roll(cam), roll(view_rot),
                                                  uint8_t(g_rail.cut()), uint8_t(g_rail.capped())};
    ++g_rail_next;
}

vr::Mat4 comfort_rail_delta() {
    std::lock_guard lock(g_rail_mx);
    return g_vr.stereo ? g_rail_delta : vr::identity();
}

float comfort_vignette_now() {
    std::lock_guard lock(g_rail_mx);
    return g_rail_vignette;
}

void comfort_log_now(const char* why) {
    std::lock_guard lock(g_rail_mx);
    logf("%s: rail turn %.0f deg/s, move %.2f m/s, vignette %.2f, smoothing lag %.1f deg%s", why, g_rail.turn_dps(), g_rail.move_mps(),
         g_rail.vignette(), g_rail.lag_deg(), g_rail.capped() ? " (capped)" : "");
}

void comfort_log_settings(const char* why) {
    logf("comfort (%s): %s; vignette %.2f (turn %.0f..%.0f deg/s, move %.1f..%.1f m/s, ease %.0f ms), smooth %.0f ms (lag cap %.1f deg), "
         "level %d (%s)",
         why, g_vr.comfort_on ? "on" : "OFF (all of it)", g_vr.comfort_vignette, g_vr.vignette_turn_dps[0], g_vr.vignette_turn_dps[1],
         g_vr.vignette_move_mps[0], g_vr.vignette_move_mps[1], g_vr.vignette_ease_ms, g_vr.comfort_smooth_ms, g_vr.cone_deg / 2,
         g_vr.comfort_level, g_vr.comfort_level == 0 ? "as the game" : g_vr.comfort_level == 1 ? "roll removed" : "roll and pitch removed");
}

static float percentile(std::vector<float>& v, float p) {
    if (v.empty()) return 0;
    const size_t k = (std::min)(v.size() - 1, size_t(p * float(v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
    return v[k];
}

// `vr comfort stats [SECONDS]`: the rail camera's speeds over the last SECONDS (default 30).
static void rail_stats(float seconds) {
    std::vector<float> turn, move, raw_turn, raw_move;
    int frames = 0, active = 0, full = 0, cuts = 0, capped = 0;
    double vig_sum = 0, t0 = 1e300, t1 = 0;
    float lag_max = 0, cam_roll = 0, view_roll = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double tn = qpc_seconds(now.QuadPart);
    {
        std::lock_guard lock(g_rail_mx);
        const size_t n = (std::min)(g_rail_next, g_rail_samples.size());
        for (size_t i = 0; i < n; ++i) {
            const RailSample& s = g_rail_samples[(g_rail_next - 1 - i) % kRailSamples];
            if (tn - s.t > seconds) break;
            ++frames;
            t0 = (std::min)(t0, s.t), t1 = (std::max)(t1, s.t);
            if (s.cut) {
                ++cuts;
                continue;
            }
            turn.push_back(s.turn), move.push_back(s.move), raw_turn.push_back(s.raw_turn), raw_move.push_back(s.raw_move);
            if (s.vignette > 0.02f) ++active, vig_sum += s.vignette;
            if (s.vignette >= 0.95f * g_vr.comfort_vignette && g_vr.comfort_vignette > 0) ++full;
            if (s.capped) ++capped;
            lag_max = (std::max)(lag_max, s.lag);
            cam_roll = (std::max)(cam_roll, std::fabs(s.cam_roll));
            view_roll = (std::max)(view_roll, std::fabs(s.view_roll));
        }
    }
    if (!frames) {
        logf("comfort stats: no stereo frames in the last %.0f s", seconds);
        return;
    }
    logf("comfort stats: last %.0f s: %d stereo frames over %.1f s, %d camera cuts", seconds, frames, float(t1 - t0), cuts);
    logf("comfort stats: turn deg/s (smoothed 100 ms) p50 %.1f p75 %.1f p90 %.1f p95 %.1f p99 %.1f max %.1f; per frame max %.1f",
         percentile(turn, .5f), percentile(turn, .75f), percentile(turn, .9f), percentile(turn, .95f), percentile(turn, .99f), percentile(turn, 1),
         percentile(raw_turn, 1));
    logf("comfort stats: move m/s (smoothed 100 ms) p50 %.2f p75 %.2f p90 %.2f p95 %.2f p99 %.2f max %.2f; per frame max %.2f",
         percentile(move, .5f), percentile(move, .75f), percentile(move, .9f), percentile(move, .95f), percentile(move, .99f), percentile(move, 1),
         percentile(raw_move, 1));
    logf("comfort stats: vignette on %.1f%% of frames (mean strength %.2f while on), at full strength %.1f%%; smoothing lag max %.1f deg, "
         "capped %.1f%% of frames",
         100.0 * active / frames, active ? vig_sum / active : 0.0, 100.0 * full / frames, lag_max, 100.0 * capped / frames);
    logf("comfort stats: largest roll: rail camera %.2f deg, the view %.2f deg", cam_roll, view_roll);
    comfort_log_settings("stats");
}

void comfort_command(const std::string& sub, const std::vector<std::string>& args) {
    auto num = [&](size_t i, float* out) {
        if (i >= args.size()) return false;
        char* end = nullptr;
        const float v = strtof(args[i].c_str(), &end);
        if (end == args[i].c_str()) return false;
        *out = v;
        return true;
    };
    float a = 0, b = 0;
    if (sub == "stats") {
        rail_stats(num(0, &a) && a > 0 ? a : 30.f);
        return;
    }
    if (sub == "off" || sub == "on") {
        g_vr.comfort_on = sub == "on";
    } else if (sub == "vignette" && num(0, &a)) {
        g_vr.comfort_vignette = a < 0 ? 0.f : a > 1 ? 1.f : a;
    } else if (sub == "smooth" && num(0, &a)) {
        g_vr.comfort_smooth_ms = a < 0 ? 0.f : a;
    } else if (sub == "level" && num(0, &a)) {
        g_vr.comfort_level = a < 0 ? 0 : a > 2 ? 2 : int(a);
    } else if (sub == "turn" && num(0, &a) && num(1, &b)) {
        g_vr.vignette_turn_dps[0] = a, g_vr.vignette_turn_dps[1] = b;
    } else if (sub == "move" && num(0, &a) && num(1, &b)) {
        g_vr.vignette_move_mps[0] = a, g_vr.vignette_move_mps[1] = b;
    } else if (sub == "ease" && num(0, &a)) {
        g_vr.vignette_ease_ms = a < 0 ? 0.f : a;
    } else {
        logf("vr comfort: on | off | vignette X (0..1) | smooth MS | level 0|1|2 | turn A B (deg/s) | move A B (m/s) | ease MS | stats [S]");
        return;
    }
    std::string line = "vr comfort " + sub;
    for (const std::string& s : args) line += " " + s;
    comfort_log_settings(line.c_str());
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
