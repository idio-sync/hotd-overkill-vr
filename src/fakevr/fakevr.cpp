// fakevr: the runtime core - the C entry points, IVRSystem, render models, the headset and
// controller model, scripting.
//
// Headset model (shaped like the Steam Frame as SteamVR would report it; values documented in
// README "Fake SteamVR runtime"): per-eye half-angles 52 deg outer, 45 deg inner, 46 deg up, 50 deg
// down (asymmetric both ways, like pancake-lens headsets of ~100-110 deg), IPD 64 mm (eye-to-head
// = +-32 mm along x, no cant unless `cant D`), recommended 2064x2016 per eye (2016 tall, the width
// in proportion to the tangents), 120 Hz; WaitGetPoses paces at that rate. The head starts at
// (0, 1.65, 0) facing -z (standing universe, the same numbers seated).
//
// GetProjectionMatrix is a standard y-up projection (clip x = +w at the right edge, y = +w at the
// top edge, w = -z) built from the tangents, as every OpenVR renderer uses it. GetProjectionRaw
// returns left, right, then top = -tan(up) and bottom = +tan(down) (SteamVR's raw values put top
// negative); the mod only logs it.
//
// Controllers: device 1 left, 2 right; render model "oculus_quest2_controller_<hand>", type
// "oculus_touch" (what Killer7 saw SteamVR report for the Steam Frame's controllers); the render
// model's "tip" component sits 1.5 cm below and 5 cm ahead of the raw pose, pointing 40 deg below
// the raw axis. Scripts give each controller's POINTING pose (the tip, where a wearer feels the
// controller points); the raw device pose is derived from it, so a mod that ignores the tip aims
// 40 deg off. Axes are not described (Prop_Axis*Type unknown) by default, as the Steam Frame's own
// driver does; `axes 1` describes them (joystick, trigger, trigger). Legacy buttons: trigger =
// k_EButton_SteamVR_Trigger + axis 1, grip = k_EButton_Grip + axis 2, a = k_EButton_A (A/X),
// b = k_EButton_ApplicationMenu (B/Y), stick = k_EButton_SteamVR_Touchpad (click), system.
//
// Scripting: hovr\fakevr_cmd.txt (tools\fakevr.ps1 writes it atomically; read every 50 ms from
// WaitGetPoses), the mod's `fakevr CMD` passthrough (hovr\cmd.txt) or the export fakevr_command.
// Lines run in order; `wait MS` pauses the queue. Commands (angles in degrees, metres):
//   head X Y Z YAW PITCH ROLL [MS]   head pose (YAW + turns right, PITCH + up, ROLL + right ear down),
//                                    moved there linearly over MS
//   look YAW PITCH ROLL [MS]         head orientation only
//   headpos X Y Z [MS]               head position only
//   ctl L|R X Y Z YAW PITCH ROLL [MS]  controller pointing pose (tracking space)
//   ctlrel L|R X Y Z YAW PITCH [MS]  controller relative to the head: offset in the head's frame
//                                    (x right, y up, z back), direction relative to the head's
//   aim L|R X Y Z                    point a controller at a tracking-space point
//   aimov L|R KEY PX PY              point a controller at texture pixel PX, PY of overlay KEY
//   btn L|R NAME [MS]                press (trigger, grip, a, b, stick, system) for MS (default 150)
//   hold L|R NAME / release L|R NAME|all
//   axis L|R N X Y                   raw axis values
//   connect L|R 0|1                  controller on/off
//   axes 0|1, tip 0|1                describe the axes / give the render model a tip component
//   ipd M, cant DEG, hz HZ, fov OUTER INNER UP DOWN (degrees)
//   capture NAME [truth 0|1] [scale S]  captures of the next complete frame (compositor.cpp)
//   state                            log the whole state; haptics: log the pulses since the last
//   wait MS, log TEXT
#include "fakevr.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <sstream>

namespace fv {

// --- math ---

M4 ident() {
    M4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
}
M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
M4 trans(double x, double y, double z) {
    M4 r = ident();
    r.m[0][3] = x, r.m[1][3] = y, r.m[2][3] = z;
    return r;
}
M4 rot_x(double a) {
    M4 r = ident();
    const double c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c, r.m[1][2] = -s, r.m[2][1] = s, r.m[2][2] = c;
    return r;
}
M4 rot_y(double a) {
    M4 r = ident();
    const double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c, r.m[0][2] = s, r.m[2][0] = -s, r.m[2][2] = c;
    return r;
}
M4 rot_z(double a) {
    M4 r = ident();
    const double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c, r.m[0][1] = -s, r.m[1][0] = s, r.m[1][1] = c;
    return r;
}
M4 rigid_inv(const M4& p) {
    M4 r = ident();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = p.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * p.m[0][3] + r.m[i][1] * p.m[1][3] + r.m[i][2] * p.m[2][3]);
    return r;
}
void apply_pt(const M4& p, const double in[3], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = p.m[i][0] * in[0] + p.m[i][1] * in[1] + p.m[i][2] * in[2] + p.m[i][3];
}
void apply_dir(const M4& p, const double in[3], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = p.m[i][0] * in[0] + p.m[i][1] * in[1] + p.m[i][2] * in[2];
}
vr::HmdMatrix34_t to34(const M4& p) {
    vr::HmdMatrix34_t r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = float(p.m[i][j]);
    return r;
}
M4 from34(const vr::HmdMatrix34_t& m) {
    M4 r = ident();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = m.m[i][j];
    return r;
}
M4 pose_matrix(const PoseP& p) {
    return mul(trans(p.x, p.y, p.z), mul(rot_y(-p.yaw * kDeg), mul(rot_x(p.pitch * kDeg), rot_z(-p.roll * kDeg))));
}
PoseP pose_params(const M4& m) {
    PoseP p;
    p.x = m.m[0][3], p.y = m.m[1][3], p.z = m.m[2][3];
    const double fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];  // forward = -z column
    p.yaw = std::atan2(fx, -fz) / kDeg;
    p.pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, fy))) / kDeg;
    p.roll = std::atan2(-m.m[1][0], m.m[1][1]) / kDeg;  // right.y = -sin(roll) cos(pitch), up.y = cos(roll) cos(pitch)
    return p;
}

// --- state, log, paths ---

static State g_state;
State& st() { return g_state; }

static FILE* g_log;
static std::mutex g_log_mx;
static double g_t0;
static std::wstring g_hovr;

double now_ms() {
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

static void init_paths(HMODULE self) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring s = p;
    s = s.substr(0, s.find_last_of(L"\\/"));
    g_hovr = s + L"\\hovr";
    CreateDirectoryW(g_hovr.c_str(), nullptr);
    g_log = _wfopen((g_hovr + L"\\fakevr.log").c_str(), L"w");
    g_t0 = now_ms();
}

std::wstring hovr_path(const wchar_t* name) { return g_hovr + L"\\" + name; }
std::wstring dir_path(const wchar_t* name) {
    const std::wstring d = g_hovr + L"\\fakevr";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\" + name;
}

void logf(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard lock(g_log_mx);
    fprintf(g_log, "[%9.3f] ", (now_ms() - g_t0) / 1000.0);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

void stub_called(const char* what) {
    static std::mutex mx;
    static std::vector<std::pair<std::string, int>> seen;
    std::lock_guard lock(mx);
    for (auto& s : seen)
        if (s.first == what) {
            if (++s.second <= 3) logf("stub %s (call %d)", what, s.second);
            return;
        }
    seen.push_back({what, 1});
    logf("stub %s (first call; not modelled, returns zero/empty)", what);
}

// --- headset and controllers ---

static void model_defaults(State& s) {
    s.tan_outer = std::tan(52 * kDeg);
    s.tan_inner = std::tan(45 * kDeg);
    s.tan_up = std::tan(46 * kDeg);
    s.tan_down = std::tan(50 * kDeg);
    s.rec_h = 2016;
    s.rec_w = uint32_t(std::lround(2016 * (s.tan_outer + s.tan_inner) / (s.tan_up + s.tan_down) / 2)) * 2;
    s.head = PoseP{0, 1.65, 0, 0, 0, 0};
    s.ctl[kRight].aim = PoseP{0.18, 1.35, -0.35, 0, 0, 0};
    s.ctl[kLeft].aim = PoseP{-0.18, 1.35, -0.35, 0, 0, 0};
    for (auto& m : s.frame_ctl) m = ident();
}

M4 head_matrix() { return pose_matrix(st().head); }

M4 eye_to_head(int eye) {
    const State& s = st();
    const double x = eye == 0 ? -s.ipd / 2 : s.ipd / 2;
    return mul(trans(x, 0, 0), rot_y((eye == 0 ? 1 : -1) * s.cant_deg * kDeg));
}

void eye_tangents(int eye, double* l, double* r, double* b, double* t) {
    const State& s = st();
    *l = -(eye == 0 ? s.tan_outer : s.tan_inner);
    *r = eye == 0 ? s.tan_inner : s.tan_outer;
    *b = -s.tan_down;
    *t = s.tan_up;
}

M4 tip_local() {
    const State& s = st();
    return mul(trans(s.tip_pos[0], s.tip_pos[1], s.tip_pos[2]), rot_x(s.tip_pitch_deg * kDeg));
}

M4 controller_aim(uint32_t d) { return pose_matrix(st().ctl[d].aim); }
M4 controller_raw(uint32_t d) {
    const M4 aim = controller_aim(d);
    return st().tip_component ? mul(aim, rigid_inv(tip_local())) : aim;
}

static PoseP lerp(const PoseP& a, const PoseP& b, double f) {
    PoseP p;
    p.x = a.x + (b.x - a.x) * f, p.y = a.y + (b.y - a.y) * f, p.z = a.z + (b.z - a.z) * f;
    p.yaw = a.yaw + (b.yaw - a.yaw) * f, p.pitch = a.pitch + (b.pitch - a.pitch) * f, p.roll = a.roll + (b.roll - a.roll) * f;
    return p;
}

static void step_anim(Anim& a, PoseP& p, double t) {
    if (!a.on) return;
    const double f = a.t1 > a.t0 ? std::fmin(1.0, std::fmax(0.0, (t - a.t0) / (a.t1 - a.t0))) : 1.0;
    p = lerp(a.from, a.to, f);
    if (f >= 1) a.on = false;
}

void advance_poses(double t) {
    State& s = st();
    step_anim(s.head_anim, s.head, t);
    for (auto& c : s.ctl) step_anim(c.anim, c.aim, t);
}

static bool device_ok(uint32_t d) { return d == kHmd || ((d == kLeft || d == kRight) && st().ctl[d].connected); }

void fill_pose(uint32_t d, vr::TrackedDevicePose_t* out) {
    *out = vr::TrackedDevicePose_t{};
    if (!device_ok(d)) return;
    out->mDeviceToAbsoluteTracking = to34(d == kHmd ? head_matrix() : controller_raw(d));
    out->eTrackingResult = vr::TrackingResult_Running_OK;
    out->bPoseIsValid = true;
    out->bDeviceIsConnected = true;
}

static uint64_t button_bit(const std::string& n) {
    using namespace vr;
    EVRButtonId id = n == "trigger" ? k_EButton_SteamVR_Trigger
                     : n == "grip"  ? k_EButton_Grip
                     : n == "a"     ? k_EButton_A
                     : n == "b"     ? k_EButton_ApplicationMenu
                     : n == "stick" ? k_EButton_SteamVR_Touchpad
                     : n == "system" ? k_EButton_System
                                     : k_EButton_Max;
    return id == k_EButton_Max ? 0 : ButtonMaskFromId(id);
}

static uint64_t pressed_mask(const Controller& c, double now) {
    uint64_t m = c.held;
    for (int i = 0; i < 64; ++i)
        if (now < c.until[i]) m |= 1ull << i;
    return m;
}

static void controller_state(uint32_t d, vr::VRControllerState_t* out) {
    *out = vr::VRControllerState_t{};
    Controller& c = st().ctl[d];
    const uint64_t m = pressed_mask(c, now_ms());
    if (m != c.last_pressed) {
        ++c.packet;
        logf("controller %s buttons %016llx", d == kLeft ? "left" : "right", m);
        c.last_pressed = m;
    }
    out->unPacketNum = c.packet;
    out->ulButtonPressed = m;
    out->ulButtonTouched = m;
    for (int i = 0; i < 5; ++i) out->rAxis[i] = {c.axis[i][0], c.axis[i][1]};
    if (m & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) out->rAxis[1].x = 1;
    if (m & vr::ButtonMaskFromId(vr::k_EButton_Grip)) out->rAxis[2].x = 1;
}

// --- IVRSystem ---

static const char* role_name(uint32_t d) { return d == kHmd ? "hmd" : d == kLeft ? "left" : d == kRight ? "right" : "?"; }

class System : public StubIVRSystem {
public:
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        std::lock_guard lock(st().mx);
        *w = st().rec_w, *h = st().rec_h;
        logf("GetRecommendedRenderTargetSize -> %ux%u", *w, *h);
    }
    vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye eye, float zn, float zf) override {
        double l, r, b, t;
        eye_tangents(eye, &l, &r, &b, &t);
        vr::HmdMatrix44_t m{};
        m.m[0][0] = float(2 / (r - l));
        m.m[0][2] = float((r + l) / (r - l));
        m.m[1][1] = float(2 / (t - b));
        m.m[1][2] = float((t + b) / (t - b));
        m.m[2][2] = zf / (zn - zf);
        m.m[2][3] = zf * zn / (zn - zf);
        m.m[3][2] = -1;
        static int logged;
        if (logged++ < 2) logf("GetProjectionMatrix(eye %d, %g, %g)", int(eye), zn, zf);
        return m;
    }
    void GetProjectionRaw(vr::EVREye eye, float* l, float* r, float* top, float* bottom) override {
        double L, R, B, T;
        eye_tangents(eye, &L, &R, &B, &T);
        *l = float(L), *r = float(R), *top = float(-T), *bottom = float(-B);
        logf("GetProjectionRaw(eye %d) -> l %.4f r %.4f top %.4f bottom %.4f", int(eye), *l, *r, *top, *bottom);
    }
    vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eye) override { return to34(eye_to_head(eye)); }
    bool GetTimeSinceLastVsync(float* s, uint64_t* frame) override {
        State& g = st();
        const double period = 1000 / g.hz, t = now_ms();
        const double k = g.vsync0 ? std::floor((t - g.vsync0) / period) : 0;
        if (s) *s = float((t - (g.vsync0 + k * period)) / 1000);
        if (frame) *frame = uint64_t(k);
        return true;
    }
    int32_t GetD3D9AdapterIndex() override {
        logf("GetD3D9AdapterIndex -> 0");
        return 0;
    }
    void GetDXGIOutputInfo(int32_t* adapter) override {
        logf("GetDXGIOutputInfo -> adapter 0");
        *adapter = 0;
    }
    bool IsDisplayOnDesktop() override { return false; }
    void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t* poses, uint32_t n) override {
        std::lock_guard lock(st().mx);
        for (uint32_t i = 0; i < n; ++i) {
            if (i < kDevices) fill_pose(i, &poses[i]);
            else poses[i] = vr::TrackedDevicePose_t{};
        }
    }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role) override {
        std::lock_guard lock(st().mx);
        const uint32_t d = role == vr::TrackedControllerRole_LeftHand ? kLeft : role == vr::TrackedControllerRole_RightHand ? kRight : vr::k_unTrackedDeviceIndexInvalid;
        return d != vr::k_unTrackedDeviceIndexInvalid && st().ctl[d].connected ? d : vr::k_unTrackedDeviceIndexInvalid;
    }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t d) override {
        return d == kLeft ? vr::TrackedControllerRole_LeftHand : d == kRight ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_Invalid;
    }
    vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t d) override {
        std::lock_guard lock(st().mx);
        if (!device_ok(d)) return vr::TrackedDeviceClass_Invalid;
        return d == kHmd ? vr::TrackedDeviceClass_HMD : vr::TrackedDeviceClass_Controller;
    }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t d) override {
        std::lock_guard lock(st().mx);
        return device_ok(d);
    }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, vr::ETrackedPropertyError* e) override {
        if (e) *e = device_ok(d) ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
        log_prop(d, p, "bool", "unknown");
        return false;
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, vr::ETrackedPropertyError* e) override {
        float v = 0;
        bool ok = false;
        if (d == kHmd) {
            if (p == vr::Prop_DisplayFrequency_Float) v = float(st().hz), ok = true;
            if (p == vr::Prop_UserIpdMeters_Float) v = float(st().ipd), ok = true;
            if (p == vr::Prop_SecondsFromVsyncToPhotons_Float) v = 0.011f, ok = true;
        }
        if (e) *e = !device_ok(d) ? vr::TrackedProp_InvalidDevice : ok ? vr::TrackedProp_Success : vr::TrackedProp_UnknownProperty;
        char b[32];
        snprintf(b, sizeof b, "%g", v);
        log_prop(d, p, "float", ok ? b : "unknown");
        return v;
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, vr::ETrackedPropertyError* e) override {
        int32_t v = 0;
        bool ok = false;
        if (d == kLeft || d == kRight) {
            if (p >= vr::Prop_Axis0Type_Int32 && p <= vr::Prop_Axis4Type_Int32 && st().axes_described) {
                static const int32_t types[5] = {vr::k_eControllerAxis_Joystick, vr::k_eControllerAxis_Trigger, vr::k_eControllerAxis_Trigger, 0, 0};
                v = types[p - vr::Prop_Axis0Type_Int32], ok = true;
            }
            if (p == vr::Prop_ControllerRoleHint_Int32) v = GetControllerRoleForTrackedDeviceIndex(d), ok = true;
        }
        if (p == vr::Prop_DeviceClass_Int32 && device_ok(d)) v = GetTrackedDeviceClass(d), ok = true;
        if (e) *e = !device_ok(d) ? vr::TrackedProp_InvalidDevice : ok ? vr::TrackedProp_Success : vr::TrackedProp_UnknownProperty;
        char b[32];
        snprintf(b, sizeof b, "%d", v);
        log_prop(d, p, "int32", ok ? b : "unknown");
        return v;
    }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, vr::ETrackedPropertyError* e) override {
        uint64_t v = 0;
        bool ok = false;
        if ((d == kLeft || d == kRight) && p == vr::Prop_SupportedButtons_Uint64) {
            for (const char* n : {"trigger", "grip", "a", "b", "stick", "system"}) v |= button_bit(n);
            ok = true;
        }
        if (e) *e = !device_ok(d) ? vr::TrackedProp_InvalidDevice : ok ? vr::TrackedProp_Success : vr::TrackedProp_UnknownProperty;
        log_prop(d, p, "uint64", ok ? "set" : "unknown");
        return v;
    }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, char* buf, uint32_t size,
                                            vr::ETrackedPropertyError* e) override {
        std::string v;
        const char* hand = d == kLeft ? "left" : "right";
        if (d == kHmd) {
            if (p == vr::Prop_TrackingSystemName_String) v = "fakevr";
            if (p == vr::Prop_ModelNumber_String) v = "fakevr Steam Frame model";
            if (p == vr::Prop_ManufacturerName_String) v = "fakevr";
            if (p == vr::Prop_SerialNumber_String) v = "FAKEVR-HMD";
            if (p == vr::Prop_RenderModelName_String) v = "generic_hmd";
        } else if (d == kLeft || d == kRight) {
            if (p == vr::Prop_TrackingSystemName_String) v = "oculus";
            if (p == vr::Prop_ModelNumber_String) v = std::string("Miramar (") + hand + " Controller)";
            if (p == vr::Prop_ManufacturerName_String) v = "Oculus";
            if (p == vr::Prop_SerialNumber_String) v = std::string("FAKEVR-") + hand;
            if (p == vr::Prop_RenderModelName_String) v = std::string("oculus_quest2_controller_") + hand;
            if (p == vr::Prop_ControllerType_String) v = "oculus_touch";
        }
        if (!device_ok(d)) {
            if (e) *e = vr::TrackedProp_InvalidDevice;
            if (buf && size) buf[0] = 0;
            return 0;
        }
        if (v.empty()) {
            if (e) *e = vr::TrackedProp_UnknownProperty;
            if (buf && size) buf[0] = 0;
            log_prop(d, p, "string", "unknown");
            return 0;
        }
        const uint32_t need = uint32_t(v.size() + 1);
        if (!buf || size < need) {
            if (e) *e = vr::TrackedProp_BufferTooSmall;
            return need;
        }
        memcpy(buf, v.c_str(), need);
        if (e) *e = vr::TrackedProp_Success;
        log_prop(d, p, "string", v.c_str());
        return need;
    }
    bool PollNextEvent(vr::VREvent_t*, uint32_t) override {
        static int n;
        if (n++ == 0) logf("PollNextEvent: first call (no events are generated)");
        return false;
    }
    bool GetControllerState(vr::TrackedDeviceIndex_t d, vr::VRControllerState_t* out, uint32_t size) override {
        std::lock_guard lock(st().mx);
        if (size != sizeof(vr::VRControllerState_t)) {
            logf("GetControllerState: size %u, expected %u", size, unsigned(sizeof(vr::VRControllerState_t)));
            return false;
        }
        if (d != kLeft && d != kRight) return false;
        if (!st().ctl[d].connected) return false;
        controller_state(d, out);
        return true;
    }
    bool GetControllerStateWithPose(vr::ETrackingUniverseOrigin, vr::TrackedDeviceIndex_t d, vr::VRControllerState_t* out, uint32_t size,
                                    vr::TrackedDevicePose_t* pose) override {
        if (!GetControllerState(d, out, size)) return false;
        std::lock_guard lock(st().mx);
        if (pose) fill_pose(d, pose);
        return true;
    }
    void TriggerHapticPulse(vr::TrackedDeviceIndex_t d, uint32_t axis, unsigned short us) override {
        std::lock_guard lock(st().mx);
        State& s = st();
        if (s.haptics.size() < 100000) s.haptics.push_back({now_ms() - g_t0, d, axis, us});
        static int logged;
        if (logged++ < 40) logf("TriggerHapticPulse(device %u %s, axis %u, %u us)", d, role_name(d), axis, unsigned(us));
    }
    const char* GetButtonIdNameFromEnum(vr::EVRButtonId id) override { return "button"; }
    const char* GetControllerAxisTypeNameFromEnum(vr::EVRControllerAxisType) override { return "axis"; }
    bool IsInputAvailable() override { return true; }
    bool IsSteamVRDrawingControllers() override { return true; }
    bool ShouldApplicationPause() override { return false; }
    bool ShouldApplicationReduceRenderingWork() override { return false; }
    void AcknowledgeQuit_Exiting() override { logf("AcknowledgeQuit_Exiting"); }
    const char* GetRuntimeVersion() override { return "fakevr 1"; }
    vr::EVRInitError SetSDKVersion(uint32_t a, uint32_t b, uint32_t c) override {
        logf("SetSDKVersion %u.%u.%u", a, b, c);
        return vr::VRInitError_None;
    }

private:
    static void log_prop(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, const char* type, const char* value) {
        static std::mutex mx;
        static std::vector<std::pair<uint32_t, int>> seen;  // (device << 24 | prop) - once each
        std::lock_guard lock(mx);
        const uint32_t key = (d << 24) | uint32_t(p);
        for (auto& s : seen)
            if (s.first == key) return;
        seen.push_back({key, 1});
        logf("property %s device %u (%s) prop %d -> %s", type, d, role_name(d), int(p), value);
    }
};

class Chaperone : public StubIVRChaperone {
public:
    void ResetZeroPose(vr::ETrackingUniverseOrigin o) override { logf("RECENTRE-RELEVANT: IVRChaperone::ResetZeroPose(%d)", int(o)); }
    vr::ChaperoneCalibrationState GetCalibrationState() override { return vr::ChaperoneCalibrationState_OK; }
};

class RenderModels : public StubIVRRenderModels {
public:
    bool GetComponentState(const char* model, const char* component, const vr::VRControllerState_t*, const vr::RenderModel_ControllerMode_State_t*,
                           vr::RenderModel_ComponentState_t* out) override {
        std::lock_guard lock(st().mx);
        const std::string m = model ? model : "", c = component ? component : "";
        const bool known = m.rfind("oculus_quest2_controller_", 0) == 0;
        const bool ok = known && c == vr::k_pch_Controller_Component_Tip && st().tip_component;
        logf("GetComponentState('%s', '%s') -> %s", m.c_str(), c.c_str(), ok ? "the tip" : "false");
        if (!ok || !out) return false;
        *out = vr::RenderModel_ComponentState_t{};
        out->mTrackingToComponentLocal = to34(tip_local());
        out->mTrackingToComponentRenderModel = to34(tip_local());
        out->uProperties = vr::VRComponentProperty_IsVisible | vr::VRComponentProperty_IsStatic;
        return true;
    }
    uint32_t GetComponentCount(const char* model) override { return 0; }
};

// The rest: every method a logging stub.
static System g_system;
static Chaperone g_chaperone;
static RenderModels g_models;
static StubIVRApplications g_apps;
static StubIVRSettings g_settings;
static StubIVRChaperoneSetup g_chaperone_setup;
static StubIVRHeadsetView g_headset_view;
static StubIVRNotifications g_notifications;
static StubIVROverlayView g_overlay_view;
static StubIVRExtendedDisplay g_extended;
static StubIVRTrackedCamera g_camera;
static StubIVRScreenshots g_screenshots;
static StubIVRResources g_resources;
static StubIVRDriverManager g_drivers;
static StubIVRInput g_input;
static StubIVRIOBuffer g_iobuffer;
static StubIVRSpatialAnchors g_anchors;
static StubIVRDebug g_debug;

static void* interface_for(const char* version) {
    struct Entry {
        const char* v;
        void* p;
    };
    const Entry table[] = {
        {vr::IVRSystem_Version, static_cast<vr::IVRSystem*>(&g_system)},
        {vr::IVRCompositor_Version, compositor()},
        {vr::IVROverlay_Version, overlay()},
        {vr::IVRRenderModels_Version, static_cast<vr::IVRRenderModels*>(&g_models)},
        {vr::IVRChaperone_Version, static_cast<vr::IVRChaperone*>(&g_chaperone)},
        {vr::IVRApplications_Version, static_cast<vr::IVRApplications*>(&g_apps)},
        {vr::IVRSettings_Version, static_cast<vr::IVRSettings*>(&g_settings)},
        {vr::IVRChaperoneSetup_Version, static_cast<vr::IVRChaperoneSetup*>(&g_chaperone_setup)},
        {vr::IVRHeadsetView_Version, static_cast<vr::IVRHeadsetView*>(&g_headset_view)},
        {vr::IVRNotifications_Version, static_cast<vr::IVRNotifications*>(&g_notifications)},
        {vr::IVROverlayView_Version, static_cast<vr::IVROverlayView*>(&g_overlay_view)},
        {vr::IVRExtendedDisplay_Version, static_cast<vr::IVRExtendedDisplay*>(&g_extended)},
        {vr::IVRTrackedCamera_Version, static_cast<vr::IVRTrackedCamera*>(&g_camera)},
        {vr::IVRScreenshots_Version, static_cast<vr::IVRScreenshots*>(&g_screenshots)},
        {vr::IVRResources_Version, static_cast<vr::IVRResources*>(&g_resources)},
        {vr::IVRDriverManager_Version, static_cast<vr::IVRDriverManager*>(&g_drivers)},
        {vr::IVRInput_Version, static_cast<vr::IVRInput*>(&g_input)},
        {vr::IVRIOBuffer_Version, static_cast<vr::IVRIOBuffer*>(&g_iobuffer)},
        {vr::IVRSpatialAnchors_Version, static_cast<vr::IVRSpatialAnchors*>(&g_anchors)},
        {vr::IVRDebug_Version, static_cast<vr::IVRDebug*>(&g_debug)},
    };
    for (const Entry& e : table)
        if (version && strcmp(version, e.v) == 0) return e.p;
    return nullptr;
}

// --- scripting ---

static std::deque<std::string> g_queue;
static double g_wait_until;

static uint32_t hand_of(const std::string& s) { return (s == "L" || s == "l" || s == "left") ? kLeft : kRight; }

static void aim_at(uint32_t d, const double p[3]) {
    Controller& c = st().ctl[d];
    const double dx = p[0] - c.aim.x, dy = p[1] - c.aim.y, dz = p[2] - c.aim.z;
    const double h = std::sqrt(dx * dx + dz * dz);
    c.anim.on = false;
    c.aim.yaw = std::atan2(dx, -dz) / kDeg;
    c.aim.pitch = std::atan2(dy, h) / kDeg;
    c.aim.roll = 0;
    logf("aim %s at %.3f %.3f %.3f: yaw %.2f pitch %.2f from %.3f %.3f %.3f", role_name(d), p[0], p[1], p[2], c.aim.yaw, c.aim.pitch, c.aim.x,
         c.aim.y, c.aim.z);
}

static void start_anim(Anim& a, PoseP& cur, const PoseP& to, double ms) {
    if (ms <= 0) {
        a.on = false;
        cur = to;
        return;
    }
    a.from = cur, a.to = to, a.t0 = now_ms(), a.t1 = a.t0 + ms, a.on = true;
}

static void log_state() {
    State& s = st();
    const PoseP& h = s.head;
    logf("state: head %.3f %.3f %.3f yaw %.2f pitch %.2f roll %.2f; universe %d; frame %llu", h.x, h.y, h.z, h.yaw, h.pitch, h.roll,
         int(s.universe), s.frame);
    for (uint32_t d : {kLeft, kRight}) {
        const Controller& c = s.ctl[d];
        logf("state: %s %s aim %.3f %.3f %.3f yaw %.2f pitch %.2f roll %.2f, held %016llx", role_name(d), c.connected ? "connected" : "OFF",
             c.aim.x, c.aim.y, c.aim.z, c.aim.yaw, c.aim.pitch, c.aim.roll, c.held);
    }
    for (const Overlay& o : s.overlays)
        if (!o.destroyed)
            logf("state: overlay %s visible %d width %.2f transform %d at %.3f %.3f %.3f, %d texture sets", o.key.c_str(), int(o.visible), o.width,
                 o.transform, o.xf.m[0][3], o.xf.m[1][3], o.xf.m[2][3], o.texture_sets);
}

void run_command(const std::string& line) {
    std::lock_guard lock(st().mx);
    std::stringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd) || cmd[0] == '#') return;
    if (cmd == "wait") {
        double ms = 0;
        ss >> ms;
        g_wait_until = now_ms() + ms;
        return;
    }
    logf("cmd: %s", line.c_str());
    State& s = st();
    if (cmd == "head" || cmd == "look" || cmd == "headpos") {
        PoseP to = s.head;
        double ms = 0;
        if (cmd == "head") ss >> to.x >> to.y >> to.z >> to.yaw >> to.pitch >> to.roll >> ms;
        if (cmd == "look") ss >> to.yaw >> to.pitch >> to.roll >> ms;
        if (cmd == "headpos") ss >> to.x >> to.y >> to.z >> ms;
        start_anim(s.head_anim, s.head, to, ms);
    } else if (cmd == "ctl" || cmd == "ctlrel") {
        std::string h;
        ss >> h;
        Controller& c = s.ctl[hand_of(h)];
        PoseP to = c.aim;
        double ms = 0;
        if (cmd == "ctl") {
            ss >> to.x >> to.y >> to.z >> to.yaw >> to.pitch >> to.roll >> ms;
        } else {
            double x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
            ss >> x >> y >> z >> yaw >> pitch >> ms;
            const M4 rel = mul(head_matrix(), mul(trans(x, y, z), mul(rot_y(-yaw * kDeg), rot_x(pitch * kDeg))));
            to = pose_params(rel);
        }
        start_anim(c.anim, c.aim, to, ms);
    } else if (cmd == "aim") {
        std::string h;
        double p[3] = {};
        ss >> h >> p[0] >> p[1] >> p[2];
        aim_at(hand_of(h), p);
    } else if (cmd == "aimov") {
        std::string h, key;
        double px = 0, py = 0;
        ss >> h >> key >> px >> py;
        Overlay* o = find_overlay(key);
        D3D11_TEXTURE2D_DESC d{};
        if (!o || !o->tex || o->transform != 1) {
            logf("aimov: no overlay %s with a texture and an absolute transform", key.c_str());
            return;
        }
        o->tex->GetDesc(&d);
        const double tw = d.Width * (o->bounds.uMax - o->bounds.uMin), th = d.Height * (o->bounds.vMax - o->bounds.vMin);
        const double w = o->width, hgt = w * th / tw / o->texel_aspect;
        const double local[3] = {((px + 0.5) / tw - 0.5) * w, (0.5 - (py + 0.5) / th) * hgt, 0};
        double p[3];
        apply_pt(o->xf, local, p);
        aim_at(hand_of(h), p);
    } else if (cmd == "btn" || cmd == "hold" || cmd == "release") {
        std::string h, n;
        double ms = 150;
        ss >> h >> n >> ms;
        Controller& c = s.ctl[hand_of(h)];
        const uint64_t bit = button_bit(n);
        if (cmd == "release" && n == "all") {
            c.held = 0;
            for (double& u : c.until) u = 0;
            return;
        }
        if (!bit) {
            logf("unknown button %s", n.c_str());
            return;
        }
        int idx = 0;
        while (!((bit >> idx) & 1)) ++idx;
        if (cmd == "btn") c.until[idx] = now_ms() + ms;
        if (cmd == "hold") c.held |= bit;
        if (cmd == "release") c.held &= ~bit, c.until[idx] = 0;
    } else if (cmd == "axis") {
        std::string h;
        int n = 0;
        float x = 0, y = 0;
        ss >> h >> n >> x >> y;
        if (n >= 0 && n < 5) s.ctl[hand_of(h)].axis[n][0] = x, s.ctl[hand_of(h)].axis[n][1] = y;
    } else if (cmd == "connect") {
        std::string h;
        int on = 1;
        ss >> h >> on;
        s.ctl[hand_of(h)].connected = on != 0;
    } else if (cmd == "axes") {
        int on = 0;
        ss >> on;
        s.axes_described = on != 0;
    } else if (cmd == "tip") {
        int on = 1;
        ss >> on;
        s.tip_component = on != 0;
    } else if (cmd == "ipd") {
        ss >> s.ipd;
    } else if (cmd == "cant") {
        ss >> s.cant_deg;
    } else if (cmd == "hz") {
        ss >> s.hz;
        s.vsync0 = 0;
    } else if (cmd == "fov") {
        double o = 52, i = 45, u = 46, d = 50;
        ss >> o >> i >> u >> d;
        s.tan_outer = std::tan(o * kDeg), s.tan_inner = std::tan(i * kDeg), s.tan_up = std::tan(u * kDeg), s.tan_down = std::tan(d * kDeg);
    } else if (cmd == "capture") {
        std::string name, k;
        ss >> name;
        s.capture_name = name.empty() ? "capture" : name;
        s.capture_truth = true;
        s.capture_scale = 1;
        while (ss >> k) {
            if (k == "truth") ss >> s.capture_truth;
            if (k == "scale") ss >> s.capture_scale;
        }
    } else if (cmd == "state") {
        log_state();
    } else if (cmd == "haptics") {
        for (size_t i = s.haptics_reported; i < s.haptics.size(); ++i)
            logf("haptic %.3f s device %u axis %u %u us", s.haptics[i].t_ms / 1000, s.haptics[i].device, s.haptics[i].axis, s.haptics[i].us);
        logf("haptics: %zu pulses since the last report (%zu in all)", s.haptics.size() - s.haptics_reported, s.haptics.size());
        s.haptics_reported = s.haptics.size();
    } else if (cmd == "log") {
        logf("log: %s", line.size() > 4 ? line.c_str() + 4 : "");
    } else {
        logf("unknown command: %s", line.c_str());
    }
}

void poll_command_file() {
    static double last;
    const double t = now_ms();
    if (t - last < 50) return;
    last = t;
    const std::wstring path = hovr_path(L"fakevr_cmd.txt");
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char buf[1024];
    int n = 0;
    std::lock_guard lock(st().mx);
    while (fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.empty()) g_queue.push_back(l), ++n;
    }
    fclose(f);
    DeleteFileW(path.c_str());
    logf("fakevr_cmd.txt: %d lines queued", n);
}

void tick_script(double t) {
    std::lock_guard lock(st().mx);
    while (!g_queue.empty() && t >= g_wait_until) {
        const std::string l = g_queue.front();
        g_queue.pop_front();
        run_command(l);
    }
}

}  // namespace fv

// --- the C entry points (openvr.h declares them; VR_API_EXPORT makes them exports) ---

using namespace fv;

static uint32_t g_token;
static bool g_initialised;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init_paths(h);
        model_defaults(st());
        logf("fakevr: the fake SteamVR runtime (tests only; never loads vrclient or starts SteamVR), loaded by pid %lu",
             GetCurrentProcessId());
        double l, r, b, t;
        for (int eye = 0; eye < 2; ++eye) {
            eye_tangents(eye, &l, &r, &b, &t);
            logf("fakevr: eye %d tangents l %.4f r %.4f b %.4f t %.4f (%.1f/%.1f/%.1f/%.1f deg), eye at x %+.4f m", eye, l, r, b, t,
                 std::atan(-l) / kDeg, std::atan(r) / kDeg, std::atan(-b) / kDeg, std::atan(t) / kDeg, eye_to_head(eye).m[0][3]);
        }
        logf("fakevr: recommended %ux%u per eye, %.0f Hz", st().rec_w, st().rec_h, st().hz);
    }
    return TRUE;
}

uint32_t VR_CALLTYPE vr::VR_InitInternal2(EVRInitError* err, EVRApplicationType type, const char* startup) {
    logf("VR_InitInternal2(type %d, startup '%s')", int(type), startup ? startup : "");
    g_initialised = true;
    st().universe = type == VRApplication_Overlay ? TrackingUniverseStanding : TrackingUniverseSeated;
    if (err) *err = VRInitError_None;
    return ++g_token;
}

extern "C" __declspec(dllexport) uint32_t VR_CALLTYPE VR_InitInternal(vr::EVRInitError* err, vr::EVRApplicationType type) {
    return vr::VR_InitInternal2(err, type, nullptr);
}

void VR_CALLTYPE vr::VR_ShutdownInternal() {
    logf("VR_ShutdownInternal");
    g_initialised = false;
}

bool VR_CALLTYPE vr::VR_IsHmdPresent() { return true; }
bool VR_CALLTYPE vr::VR_IsRuntimeInstalled() { return true; }

bool vr::VR_GetRuntimePath(char* buf, uint32_t size, uint32_t* need) {
    static const char path[] = "C:\\fakevr (no runtime)";
    if (need) *need = sizeof path;
    if (!buf || size < sizeof path) return false;
    memcpy(buf, path, sizeof path);
    return true;
}

extern "C" __declspec(dllexport) const char* VR_CALLTYPE VR_RuntimePath() { return "C:\\fakevr (no runtime)"; }

const char* VR_CALLTYPE vr::VR_GetVRInitErrorAsSymbol(EVRInitError e) { return e == VRInitError_None ? "VRInitError_None" : "VRInitError_(fakevr)"; }
const char* VR_CALLTYPE vr::VR_GetVRInitErrorAsEnglishDescription(EVRInitError e) { return e == VRInitError_None ? "No Error (0)" : "fakevr error"; }
extern "C" __declspec(dllexport) const char* VR_CALLTYPE VR_GetStringForHmdError(vr::EVRInitError e) { return vr::VR_GetVRInitErrorAsEnglishDescription(e); }

void* VR_CALLTYPE vr::VR_GetGenericInterface(const char* version, EVRInitError* err) {
    void* p = interface_for(version);
    logf("VR_GetGenericInterface('%s') -> %s", version ? version : "", p ? "ok" : "NOT AVAILABLE (FnTable or unknown version)");
    if (err) *err = p ? VRInitError_None : VRInitError_Init_InterfaceNotFound;
    return p;
}

bool VR_CALLTYPE vr::VR_IsInterfaceVersionValid(const char* version) {
    const bool ok = interface_for(version) != nullptr;
    logf("VR_IsInterfaceVersionValid('%s') -> %d", version ? version : "", int(ok));
    return ok;
}

uint32_t VR_CALLTYPE vr::VR_GetInitToken() { return g_token; }

// Test hooks (not in OpenVR): the mod's `fakevr CMD` passthrough and an identity check.
extern "C" __declspec(dllexport) void __cdecl fakevr_command(const char* line) {
    if (line) fv::run_command(line);
}
extern "C" __declspec(dllexport) const char* __cdecl fakevr_version() { return "fakevr 1"; }
