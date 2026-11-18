// The headset: OpenVR (mode=openvr) or a simulated one (mode=sim). Ported from k7-vr's
// src/vr/vr.cpp (OpenVR init, poses, yaw-only recentre anchor, panels as SteamVR overlays, black
// eyes for flat frames).
//
// Controllers (milestone 2): the gun hand's pose comes from the same WaitGetPoses call as the
// head, composed with the controller render model's "tip" (where SteamVR's own pointers come from;
// k7-vr: the Steam Frame controllers report as oculus_quest2 with the tip ~40 degrees below the raw
// axis) and [vr] hand_offset / hand_pitch. Buttons are read through the legacy controller state
// (GetControllerState): no action manifest, which would switch the app off legacy input. Sim mode
// has a scripted hand (position and direction relative to the head) and scripted buttons.
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
#include <mutex>

#include "../vr/aim.h"
#include "../vr/xform.h"

namespace hv {

static ::vr::IVRSystem* g_sys;
static bool g_openvr;
static vr::Mat4 g_anchor = vr::identity();  // right-handed, tracking space
static vr::Mat4 g_head = vr::identity();    // right-handed, tracking space
static vr::Mat4 g_eye_to_head[2] = {vr::identity(), vr::identity()};
static float g_tan[2][4];
static bool g_recentre = true;
static bool g_have_pose;  // openvr: WaitGetPoses has given a valid head pose
static constexpr float kPi = 3.14159265f;

// The gun hand (tip and hand_offset/hand_pitch applied), tracking space; read by the game thread.
static std::mutex g_hand_mx;
static vr::Mat4 g_hand = vr::identity();
static bool g_hand_valid;
static vr::Mat4 g_hand_anchor = vr::identity(), g_hand_head = vr::identity();  // the anchor/head with it

struct Controller {
    ::vr::TrackedDeviceIndex_t device = ::vr::k_unTrackedDeviceIndexInvalid;
    int trigger_axis = 1;
    CtlState state;
};
static Controller g_ctl[2];          // [0] gun hand, [1] the other
static CtlState g_sim_ctl[2];        // sim: scripted buttons
static vr::Mat4 g_tip = vr::identity();
static ::vr::TrackedDeviceIndex_t g_tip_device = ::vr::k_unTrackedDeviceIndexInvalid;

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

void vrsys_recentre(const char* why) {
    logf("vr: recentre (%s)%s", why, g_openvr ? "" : " - sim: the scripted head is the anchor");
    g_recentre = true;
}

static ::vr::ETrackedControllerRole role_of(bool gun) {
    const bool right = gun != g_vr.left_hand;
    return right ? ::vr::TrackedControllerRole_RightHand : ::vr::TrackedControllerRole_LeftHand;
}

// The pointing pose relative to the controller's raw pose: its render model's tip component.
static vr::Mat4 controller_tip(::vr::TrackedDeviceIndex_t device) {
    if (device == g_tip_device) return g_tip;
    g_tip_device = device;
    g_tip = vr::identity();
    char model[256] = {};
    g_sys->GetStringTrackedDeviceProperty(device, ::vr::Prop_RenderModelName_String, model, sizeof model);
    ::vr::IVRRenderModels* models = ::vr::VRRenderModels();
    ::vr::VRControllerState_t st{};
    ::vr::RenderModel_ControllerMode_State_t mode{};
    ::vr::RenderModel_ComponentState_t comp{};
    if (models && model[0] && models->GetComponentState(model, ::vr::k_pch_Controller_Component_Tip, &st, &mode, &comp)) {
        g_tip = vr::from_rows34(comp.mTrackingToComponentLocal.m);
        const float up = std::asin(std::fmax(-1.f, std::fmin(1.f, -g_tip.m[1][2]))) * 180 / kPi;
        logf("vr: gun controller model '%s': tip at %.3f %.3f %.3f m, pointing %.1f degrees %s the raw axis", model, g_tip.m[0][3],
             g_tip.m[1][3], g_tip.m[2][3], std::fabs(up), up >= 0 ? "above" : "below");
    } else {
        logf("vr: gun controller model '%s' has no tip component; aiming along its raw axis (hand_pitch adjusts)", model);
    }
    return g_tip;
}

static void find_trigger_axis(Controller& c, bool gun) {
    c.trigger_axis = -1;
    for (int i = 0; i < int(::vr::k_unControllerStateAxisCount); ++i) {
        const int32_t type = g_sys->GetInt32TrackedDeviceProperty(c.device, ::vr::ETrackedDeviceProperty(::vr::Prop_Axis0Type_Int32 + i));
        if (type == ::vr::k_eControllerAxis_Trigger && c.trigger_axis < 0) c.trigger_axis = i;
    }
    const bool assumed = c.trigger_axis < 0;
    if (assumed) c.trigger_axis = 1;  // SteamVR's legacy layout: stick/trackpad axis 0, trigger axis 1
    char name[128] = {};
    g_sys->GetStringTrackedDeviceProperty(c.device, ::vr::Prop_ControllerType_String, name, sizeof name);
    logf("vr: %s controller '%s' (device %u), trigger axis %d%s", gun ? "gun" : "other", name, c.device, c.trigger_axis,
         assumed ? " (assumed)" : "");
}

static void poll_controller(bool gun) {
    Controller& c = g_ctl[gun ? 0 : 1];
    const ::vr::TrackedDeviceIndex_t device = g_sys->GetTrackedDeviceIndexForControllerRole(role_of(gun));
    if (device != c.device) {
        c = Controller{};
        c.device = device;
        if (device != ::vr::k_unTrackedDeviceIndexInvalid) find_trigger_axis(c, gun);
    }
    c.state = CtlState{};
    if (device == ::vr::k_unTrackedDeviceIndexInvalid) return;
    ::vr::VRControllerState_t st;
    if (!g_sys->GetControllerState(device, &st, sizeof st)) return;
    auto pressed = [&](::vr::EVRButtonId id) { return (st.ulButtonPressed & ::vr::ButtonMaskFromId(id)) != 0; };
    c.state.present = true;
    c.state.trigger = st.rAxis[c.trigger_axis].x;
    if (pressed(::vr::k_EButton_SteamVR_Trigger)) c.state.trigger = (std::max)(c.state.trigger, 1.f);
    if (pressed(::vr::k_EButton_A)) c.state.buttons |= kBtnA;
    if (pressed(::vr::k_EButton_ApplicationMenu)) c.state.buttons |= kBtnB;
    if (pressed(::vr::k_EButton_Grip)) c.state.buttons |= kBtnGrip;
    if (pressed(::vr::k_EButton_SteamVR_Touchpad)) c.state.buttons |= kBtnStick;
    static uint64_t last[2];
    if (st.ulButtonPressed != last[gun ? 0 : 1]) {
        last[gun ? 0 : 1] = st.ulButtonPressed;
        logf("vr: %s controller buttons %016llx", gun ? "gun" : "other", st.ulButtonPressed);
    }
}

bool vrsys_controller(bool gun, CtlState* out) {
    *out = g_openvr ? g_ctl[gun ? 0 : 1].state : g_sim_ctl[gun ? 0 : 1];
    if (!g_openvr) out->present = g_vr.mode != VrMode::off;
    if (out->trigger > 0.5f) out->buttons |= kBtnTrigger;
    return out->present;
}

void vrsys_sim_buttons(bool gun, uint32_t buttons, float trigger) {
    g_sim_ctl[gun ? 0 : 1].buttons = buttons;
    g_sim_ctl[gun ? 0 : 1].trigger = trigger;
}

void vrsys_haptic(unsigned us) {
    if (us == 0) return;
    if (!g_openvr) {
        static int logged;
        if (logged++ < 20) logf("vr: (sim) haptic pulse %u us", us);
        return;
    }
    const ::vr::TrackedDeviceIndex_t d = g_ctl[0].device;
    if (d != ::vr::k_unTrackedDeviceIndexInvalid) g_sys->TriggerHapticPulse(d, 0, (unsigned short)((std::min)(us, 3999u)));
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
        // The scripted hand: position and direction relative to the head (no tip or hand_pitch:
        // those correct a physical controller).
        const vr::Mat4 hand = vr::mul(vr::mul(g_head, vr::translation(g_vr.sim_hand[0], g_vr.sim_hand[1], g_vr.sim_hand[2])),
                                      vr::rh_yaw_pitch(g_vr.sim_hand_yaw * kPi / 180, g_vr.sim_hand_pitch * kPi / 180));
        std::lock_guard lock(g_hand_mx);
        g_hand = hand;
        g_hand_valid = true;
        g_hand_anchor = g_anchor;
        g_hand_head = g_head;
        return;
    }
    ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];
    ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    const auto& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
    if (hmd.bPoseIsValid) g_head = vr::from_rows34(hmd.mDeviceToAbsoluteTracking.m);
    read_eyes();  // IPD can change while playing
    if (hmd.bPoseIsValid) g_have_pose = true;
    if (g_recentre && hmd.bPoseIsValid) {
        g_anchor = vr::yaw_only_rh(g_head);
        g_recentre = false;
        // Heading: + = turned right from the tracking space's -z (the anchor's forward is its -z column).
        logf("vr: recentred at %.2f %.2f %.2f m, heading %.1f deg", g_anchor.m[0][3], g_anchor.m[1][3], g_anchor.m[2][3],
             std::atan2(-g_anchor.m[0][2], g_anchor.m[2][2]) * 180 / kPi);
    }
    poll_controller(true);
    poll_controller(false);
    const ::vr::TrackedDeviceIndex_t gun = g_ctl[0].device;
    const bool valid = gun < ::vr::k_unMaxTrackedDeviceCount && poses[gun].bPoseIsValid;
    vr::Mat4 hand = vr::identity();
    if (valid)
        hand = vr::mul(vr::mul(vr::from_rows34(poses[gun].mDeviceToAbsoluteTracking.m), controller_tip(gun)),
                       vr::hand_adjust(g_vr.hand_offset, g_vr.hand_pitch));
    std::lock_guard lock(g_hand_mx);
    if (valid) g_hand = hand;
    g_hand_valid = valid;
    g_hand_anchor = g_anchor;
    g_hand_head = g_head;
}

// The gun hand through the same transform as the head (xform.h pose_in_game).
bool vrsys_hand_in_game(vr::Mat4* pose_lh) {
    std::lock_guard lock(g_hand_mx);
    *pose_lh = vr::pose_in_game(g_hand_anchor, g_hand);
    return g_hand_valid;
}

bool vrsys_hand_in_anchor(vr::Mat4* pose_rh) {
    std::lock_guard lock(g_hand_mx);
    *pose_rh = vr::mul(vr::rigid_inverse(g_hand_anchor), g_hand);
    return g_hand_valid;
}

bool vrsys_head_in_anchor(vr::Mat4* pose_rh) {
    std::lock_guard lock(g_hand_mx);
    *pose_rh = vr::mul(vr::rigid_inverse(g_hand_anchor), g_hand_head);
    return g_vr.mode != VrMode::off;
}

// Sim: turn the scripted hand towards a point given relative to the anchor (right-handed, metres).
// OpenVR with the fake runtime (tests, src/fakevr): the point in tracking space goes to its `aim`
// command instead, so `vr handat`, `vr handscreen` and `vr handseek` work there too.
void vrsys_sim_aim_at(const float p[3]) {
    if (g_openvr) {
        using FakeCmd = void(__cdecl*)(const char*);
        HMODULE m = GetModuleHandleW(L"openvr_api.dll");
        FakeCmd f = m ? reinterpret_cast<FakeCmd>(GetProcAddress(m, "fakevr_command")) : nullptr;
        if (!f || !dev_harness()) {
            logf("vr: aiming the hand by command needs the fake runtime (openvr mode)");
            return;
        }
        float t[3];
        for (int i = 0; i < 3; ++i) t[i] = g_anchor.m[i][0] * p[0] + g_anchor.m[i][1] * p[1] + g_anchor.m[i][2] * p[2] + g_anchor.m[i][3];
        char cmd[128];
        snprintf(cmd, sizeof cmd, "aim %s %.4f %.4f %.4f", g_vr.left_hand ? "L" : "R", t[0], t[1], t[2]);
        logf("vr: fake runtime %s (anchor-space point %.3f %.3f %.3f)", cmd, p[0], p[1], p[2]);
        f(cmd);
        return;
    }
    const vr::Mat4 head = sim_head();
    const vr::Mat4 at = vr::mul(head, vr::translation(g_vr.sim_hand[0], g_vr.sim_hand[1], g_vr.sim_hand[2]));
    const float dir[3] = {p[0] - at.m[0][3], p[1] - at.m[1][3], p[2] - at.m[2][3]};
    float yaw, pitch;
    vr::yaw_pitch_towards(head, dir, &yaw, &pitch);
    g_vr.sim_hand_yaw = yaw * 180 / kPi;
    g_vr.sim_hand_pitch = pitch * 180 / kPi;
    logf("vr: sim hand aimed at %.3f %.3f %.3f m: yaw %.2f pitch %.2f deg (hand at %.3f %.3f %.3f)", p[0], p[1], p[2], g_vr.sim_hand_yaw,
         g_vr.sim_hand_pitch, at.m[0][3], at.m[1][3], at.m[2][3]);
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
    // Nothing before the first pose: the frame was drawn without one, and the panels would be
    // placed at the tracking origin (the anchor is the first head pose).
    if (!g_openvr || !g_have_pose) return;
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
