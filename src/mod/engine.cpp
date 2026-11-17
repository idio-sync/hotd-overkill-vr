// Engine patches for VR (sim/openvr only; mode=off leaves the game alone):
//
// - Frame limiter: the main loop (0x6FDEA0) caps at 60 Hz unless `[[0x9F09E0]+0x48]` (the
//   `disable-fps-limit` option, a dword) is 0. Cleared while the scene is drawn in stereo (see
//   set_limiter). Game time uses a variable timestep, so it stays real-time (findings-probe.md).
//
// - Culling: the camera frustum planes are built by 0x6DEDE0 and 0x6DF810 (both thiscall, 3 args,
//   ret 0xC) from the half-FOV sin/cos globals that 0x66A8E0 computes (sin/cos of half the X FOV
//   at 0x9C7F28/0x9C7F30, of half the Y FOV at 0x9C7F2C/0x9C7F34; full FOVs in radians at
//   0x9C7F20/0x9C7F24). Both are wrapped: for the duration of the call those four globals hold a
//   wider frustum (eye FOV + cone_deg on each side), then the originals are restored, so only
//   culling sees the wider view; the projection, cursor unprojection and LOD keep the game's FOV.
#include "common.h"

#include <cmath>

#include "../vr/stereo.h"

namespace hv {

constexpr uintptr_t kAppPtr = 0x9F09E0, kFpsLimitOff = 0x48;
constexpr uintptr_t kFovX = 0x9C7F20, kFovY = 0x9C7F24;
constexpr uintptr_t kSinX = 0x9C7F28, kSinY = 0x9C7F2C, kCosX = 0x9C7F30, kCosY = 0x9C7F34;
constexpr float kPi = 3.14159265f;

static float& gf(uintptr_t image_va) { return *reinterpret_cast<float*>(va(image_va)); }

// --- culling ---

using Frustum_t = void(__fastcall*)(void* self, void* edx, void* a, void* b, void* c);
static Frustum_t o_frustum_a, o_frustum_b;
static long g_widened_calls;

struct WidenFrustum {
    float saved[4];
    bool on = false;
    WidenFrustum() {
        if (!g_vr.stereo) return;
        const float hx = gf(kFovX) * 0.5f, hy = gf(kFovY) * 0.5f;
        if (!(hx > 0 && hx < kPi / 2 && hy > 0 && hy < kPi / 2)) return;
        // Sim eye: the game's vertical FOV at the eye image's aspect.
        const float eye_hy = hy;
        const float eye_hx = std::fmax(hx, std::atan(std::tan(hy) * float(g_vr.eye_w) / float(g_vr.eye_h)));
        const float cone = g_vr.cone_deg * kPi / 180, margin = 3 * kPi / 180;
        const float wx = vr::widened_half_angle(eye_hx, cone, margin), wy = vr::widened_half_angle(eye_hy, cone, margin);
        saved[0] = gf(kSinX), saved[1] = gf(kCosX), saved[2] = gf(kSinY), saved[3] = gf(kCosY);
        gf(kSinX) = std::sin(wx), gf(kCosX) = std::cos(wx), gf(kSinY) = std::sin(wy), gf(kCosY) = std::cos(wy);
        on = true;
        if (g_widened_calls++ == 0)
            logf("culling widened: half-FOV %.1f x %.1f -> %.1f x %.1f deg", hx * 180 / kPi, hy * 180 / kPi, wx * 180 / kPi,
                 wy * 180 / kPi);
    }
    ~WidenFrustum() {
        if (on) gf(kSinX) = saved[0], gf(kCosX) = saved[1], gf(kSinY) = saved[2], gf(kCosY) = saved[3];
    }
};

static void __fastcall hk_frustum_a(void* self, void* edx, void* a, void* b, void* c) {
    WidenFrustum w;
    o_frustum_a(self, edx, a, b, c);
}
static void __fastcall hk_frustum_b(void* self, void* edx, void* a, void* b, void* c) {
    WidenFrustum w;
    o_frustum_b(self, edx, a, b, c);
}

static void install_culling_hooks() {
    // push ebp; mov ebp, esp; sub esp, imm8 (the next instruction holds an absolute address).
    static const uint8_t a[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    static const uint8_t b[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x5C};
    o_frustum_a = reinterpret_cast<Frustum_t>(install_detour(0x6DEDE0, a, sizeof a, reinterpret_cast<void*>(hk_frustum_a), "frustum 0x6DEDE0"));
    o_frustum_b = reinterpret_cast<Frustum_t>(install_detour(0x6DF810, b, sizeof b, reinterpret_cast<void*>(hk_frustum_b), "frustum 0x6DF810"));
}

// --- frame limiter ---

// Uncapped only while the scene is being drawn in stereo (gameplay and in-engine cutscenes); the
// game's own cap comes back elsewhere (boot, menus, loading, video cutscenes - where it was never
// tested uncapped, and where two early sim runs lost the D3D device).
static void set_limiter(bool want_uncapped) {
    static DWORD game_value = 1;
    static bool cleared;
    uintptr_t app = *reinterpret_cast<uintptr_t*>(va(kAppPtr));
    if (!app || !readable(app + kFpsLimitOff)) return;
    auto flag = reinterpret_cast<volatile DWORD*>(app + kFpsLimitOff);
    if (want_uncapped && *flag != 0) {
        game_value = *flag;
        *flag = 0;
        if (!cleared) logf("frame limiter: off (app %p +0x48 was %lu)", reinterpret_cast<void*>(app), DWORD(game_value));
        cleared = true;
    } else if (!want_uncapped && cleared) {
        *flag = game_value;
        cleared = false;
        logf("frame limiter: restored (%lu)", DWORD(game_value));
    }
}

void engine_frame(bool stereo_last_frame) {
    if (g_vr.mode == VrMode::off) return;
    static bool installed;
    if (!installed) {  // at the first Present: after SecuROM's start-up patching
        installed = true;
        install_culling_hooks();
    }
    // A short debounce so a frame without scene draws (a load hitch) doesn't toggle it.
    static int mono_frames = 1 << 20;  // untouched until the first stereo frame
    mono_frames = stereo_last_frame ? 0 : (std::min)(mono_frames + 1, 1 << 20);
    set_limiter(g_vr.uncap && g_vr.stereo && mono_frames < 30);
}

}  // namespace hv
