// The headset: OpenVR (mode=openvr) or a simulated one (mode=sim). Ported from k7-vr's
// src/vr/vr.cpp (OpenVR init, poses, yaw-only recentre anchor, panels as SteamVR overlays, black
// eyes for flat frames).
//
// Spaces (src/vr/xform.h): OpenVR tracking space is right-handed, metres. The anchor is a
// yaw-only pose of the head taken at recentre (level start, `vr recentre`); the game's rail
// camera sits at the anchor, so the head relative to the anchor, converted to the left-handed
// game-camera space, is what stereo.cpp adds on top of the game camera. The HUD panel and the flat
// screen are world-locked to the anchor.
//
// Order per frame (d3d.cpp's Present): the game's frame is drawn -> vrsys_submit (eyes, or black
// eyes and the screen overlay) -> the desktop Present -> vrsys_wait_poses (WaitGetPoses: paces
// the game at the headset's rate and gives the head pose for the next frame).
#include "common.h"

#include <d3d11.h>
#include <openvr.h>

#include <cmath>

#include "../vr/xform.h"

namespace hv {

static ::vr::IVRSystem* g_sys;
static bool g_openvr;
static vr::Mat4 g_anchor = vr::identity();  // right-handed, tracking space
static vr::Mat4 g_head = vr::identity();    // right-handed, tracking space
static vr::Mat4 g_eye_to_head[2] = {vr::identity(), vr::identity()};
static float g_tan[2][4];
static bool g_recentre = true;
static constexpr float kPi = 3.14159265f;

bool vrsys_openvr() { return g_openvr; }

static int init_openvr_raw(::vr::IVRSystem** out) {
    ::vr::EVRInitError err = ::vr::VRInitError_None;
    __try {
        *out = ::vr::VR_Init(&err, ::vr::VRApplication_Scene);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;  // openvr_api.dll missing (delay-load failure)
    }
    return int(err);
}

static void read_eyes() {
    for (int eye = 0; eye < 2; ++eye) {
        const ::vr::HmdMatrix34_t m = g_sys->GetEyeToHeadTransform(::vr::EVREye(eye));
        g_eye_to_head[eye] = vr::from_rows34(m.m);
        const ::vr::HmdMatrix44_t p = g_sys->GetProjectionMatrix(::vr::EVREye(eye), 0.1f, 100.f);
        float t[4];
        if (vr::tangents_from_openvr(p.m, t))
            for (int i = 0; i < 4; ++i) g_tan[eye][i] = t[i];
    }
}

// At Direct3DCreate9 (the game's main thread, before any render target exists).
void vrsys_start() {
    if (g_vr.mode != VrMode::openvr) return;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        logf("vr: Shift held at start: VR off for this run");
        g_vr.mode = VrMode::off;
        g_vr.stereo = false;
        return;
    }
    const int err = init_openvr_raw(&g_sys);
    if (err != 0 || !g_sys) {
        logf(err < 0 ? "vr: openvr_api.dll could not be loaded; VR off" : "vr: VR_Init failed (%d); VR off", err);
        g_sys = nullptr;
        g_vr.mode = VrMode::off;
        g_vr.stereo = false;
        return;
    }
    g_openvr = true;
    for (int eye = 0; eye < 2; ++eye) {
        float l, r, t, b;
        g_sys->GetProjectionRaw(::vr::EVREye(eye), &l, &r, &t, &b);
        logf("vr: eye %d GetProjectionRaw l %.3f r %.3f t %.3f b %.3f", eye, l, r, t, b);
    }
    read_eyes();
    for (int eye = 0; eye < 2; ++eye)
        logf("vr: eye %d tangents l %.3f r %.3f b %.3f t %.3f, offset %.4f %.4f %.4f m", eye, g_tan[eye][0], g_tan[eye][1],
             g_tan[eye][2], g_tan[eye][3], g_eye_to_head[eye].m[0][3], g_eye_to_head[eye].m[1][3], g_eye_to_head[eye].m[2][3]);
    uint32_t w = 0, h = 0;
    g_sys->GetRecommendedRenderTargetSize(&w, &h);
    if (g_vr.eye_res_auto) {
        g_vr.eye_w = (std::max)(64, int(w * g_vr.eye_scale + 0.5f) & ~1);
        g_vr.eye_h = (std::max)(64, int(h * g_vr.eye_scale + 0.5f) & ~1);
    }
    logf("vr: OpenVR up, recommended %ux%u per eye; eye targets %dx%d (eye_scale %.2f)", w, h, g_vr.eye_w, g_vr.eye_h, g_vr.eye_scale);
    ::vr::VRCompositor()->SetTrackingSpace(::vr::TrackingUniverseStanding);
}

void vrsys_recentre() {
    g_recentre = true;
}

static vr::Mat4 sim_head() {
    // The scripted head, as a right-handed pose (the conversion is its own inverse).
    return vr::flip_z(vr::rotation_yaw_pitch(g_vr.sim_yaw_deg * kPi / 180, g_vr.sim_pitch_deg * kPi / 180));
}

// After the desktop Present: the head pose for the next frame (OpenVR: waits for it).
void vrsys_wait_poses() {
    if (!g_openvr) {
        g_head = sim_head();
        g_anchor = vr::identity();
        for (int eye = 0; eye < 2; ++eye) g_eye_to_head[eye] = vr::translation(eye == 0 ? -g_vr.ipd_m / 2 : g_vr.ipd_m / 2, 0, 0);
        g_recentre = false;
        return;
    }
    ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];
    ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    const auto& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
    if (hmd.bPoseIsValid) g_head = vr::from_rows34(hmd.mDeviceToAbsoluteTracking.m);
    read_eyes();  // IPD can change while playing
    if (g_recentre && hmd.bPoseIsValid) {
        g_anchor = vr::yaw_only_rh(g_head);
        g_recentre = false;
        logf("vr: recentred at %.2f %.2f %.2f m", g_anchor.m[0][3], g_anchor.m[1][3], g_anchor.m[2][3]);
    }
}

// Eye `eye` relative to the anchor (right-handed, metres).
vr::Mat4 vrsys_eye_in_anchor(int eye) { return vr::mul(vr::mul(vr::rigid_inverse(g_anchor), g_head), g_eye_to_head[eye]); }

// Eye pose in the game camera's space (left-handed, metres) and, in openvr mode, its tangents.
bool vrsys_eye(int eye, vr::Mat4* pose_lh, float tan[4]) {
    *pose_lh = vr::eye_in_game(vr::head_in_game(g_anchor, g_head), g_eye_to_head[eye]);
    if (!g_openvr) return false;
    for (int i = 0; i < 4; ++i) tan[i] = g_tan[eye][i];
    return true;
}

// Panels, world-locked to the anchor.
vr::Mat4 vrsys_panel_in_anchor(bool screen) {
    return screen ? vr::panel_pose(vr::identity(), g_vr.screen_distance, g_vr.screen_drop)
                  : vr::panel_pose(vr::identity(), g_vr.hud_distance, g_vr.hud_drop);
}

struct Overlay {
    const char* key;
    const char* name;
    bool screen;
    ::vr::VROverlayHandle_t handle = ::vr::k_ulOverlayHandleInvalid;
    bool shown = false;
};
static Overlay g_hud_overlay{"hovr.hud", "House of the Dead VR HUD", false};
static Overlay g_screen_overlay{"hovr.screen", "House of the Dead VR screen", true};

static void update_overlay(Overlay& o, ID3D11Texture2D* tex) {
    auto overlay = ::vr::VROverlay();
    if (!overlay) return;
    if (!tex) {
        if (o.shown) overlay->HideOverlay(o.handle), o.shown = false;
        return;
    }
    if (o.handle == ::vr::k_ulOverlayHandleInvalid) {
        const ::vr::EVROverlayError e = overlay->CreateOverlay(o.key, o.name, &o.handle);
        if (e != ::vr::VROverlayError_None) {
            static int errors;
            if (errors++ < 5) logf("vr: CreateOverlay %s -> %d", o.key, int(e));
            o.handle = ::vr::k_ulOverlayHandleInvalid;
            return;
        }
        if (o.screen)
            overlay->SetOverlayFlag(o.handle, ::vr::VROverlayFlags_IgnoreTextureAlpha, true);
        else  // the HUD texture's colour is already multiplied by its alpha (stereo.cpp)
            overlay->SetOverlayFlag(o.handle, ::vr::VROverlayFlags_IsPremultiplied, true);
    }
    overlay->SetOverlayWidthInMeters(o.handle, o.screen ? g_vr.screen_width : g_vr.hud_width);
    ::vr::Texture_t t{tex, ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
    overlay->SetOverlayTexture(o.handle, &t);
    ::vr::HmdMatrix34_t m;
    vr::to_rows34(vr::mul(g_anchor, vrsys_panel_in_anchor(o.screen)), m.m);
    overlay->SetOverlayTransformAbsolute(o.handle, ::vr::VRCompositor()->GetTrackingSpace(), &m);
    if (!o.shown) overlay->ShowOverlay(o.handle), o.shown = true;
}

// Eyes: the double-wide eye texture (left half, right half), or null for black eyes (flat
// frames). hud / screen: the panels' textures, or null to hide them.
void vrsys_submit(ID3D11Texture2D* eyes, ID3D11Texture2D* hud, ID3D11Texture2D* screen) {
    if (!g_openvr) return;
    for (int eye = 0; eye < 2; ++eye) {
        ::vr::Texture_t t{eyes ? eyes : handoff_black(), ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
        const ::vr::VRTextureBounds_t half{eye ? 0.5f : 0.f, 0.f, eye ? 1.f : 0.5f, 1.f};
        const ::vr::EVRCompositorError e = ::vr::VRCompositor()->Submit(::vr::EVREye(eye), &t, eyes ? &half : nullptr);
        static int errors;
        if (e != ::vr::VRCompositorError_None && errors++ < 10) logf("vr: Submit eye %d -> %d", eye, int(e));
    }
    update_overlay(g_hud_overlay, hud);
    update_overlay(g_screen_overlay, screen);
}

}  // namespace hv
