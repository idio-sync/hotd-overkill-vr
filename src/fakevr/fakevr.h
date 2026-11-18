#pragma once
// fakevr: a fake SteamVR runtime for tests (openvr_api.dll stand-in, built as out\fakevr\openvr_api.dll).
// It never loads or starts the real runtime (no vrclient.dll, no vrserver): it answers the
// OpenVR C entry points itself and hands out interface objects whose vtables come from the very
// headers the mod is compiled with (stubs.gen.h derives from them), so every method sits in the
// slot the mod calls. Design notes and the headset model: fakevr.cpp; compositor and captures:
// compositor.cpp.
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "stubs.gen.h"

namespace fv {

// --- math (its own, independent of the mod's src/vr, so the "truth" the fake draws in captures
// is not the mod's arithmetic checking itself). Right-handed tracking space: +x right, +y up,
// -z forward, metres; column vectors, m[row][col].
struct M4 {
    double m[4][4];
};
M4 ident();
M4 mul(const M4& a, const M4& b);
M4 trans(double x, double y, double z);
M4 rot_x(double rad);
M4 rot_y(double rad);
M4 rot_z(double rad);
M4 rigid_inv(const M4& p);
void apply_pt(const M4& p, const double in[3], double out[3]);
void apply_dir(const M4& p, const double in[3], double out[3]);
vr::HmdMatrix34_t to34(const M4& p);
M4 from34(const vr::HmdMatrix34_t& m);
constexpr double kDeg = 3.14159265358979323846 / 180;

// A pose by parameters: position (metres, tracking space) and yaw (+: turned right), pitch (+:
// looking up), roll (+: right ear towards the right shoulder), degrees.
// R = Ry(-yaw) * Rx(pitch) * Rz(-roll).
struct PoseP {
    double x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
};
M4 pose_matrix(const PoseP& p);
PoseP pose_params(const M4& m);

struct Anim {
    PoseP from, to;
    double t0 = 0, t1 = 0;
    bool on = false;
};

enum : uint32_t { kHmd = 0, kLeft = 1, kRight = 2, kDevices = 3 };

struct Controller {
    bool connected = true;
    PoseP aim;  // the pointing pose (the render model's tip), tracking space
    Anim anim;
    uint64_t held = 0;       // `hold`
    double until[64] = {};   // timed presses: pressed while now < until[button id]
    float axis[5][2] = {};   // `axis` (raw values; the trigger/grip buttons also drive axes 1/2)
    uint32_t packet = 0;
    uint64_t last_pressed = 0;
};

struct Overlay {
    std::string key, name;
    bool visible = false;
    float width = 1, alpha = 1, texel_aspect = 1;
    uint32_t sort = 0;
    uint64_t flags = 0;  // VROverlayFlags masks
    ID3D11Texture2D* tex = nullptr;  // AddRef'd
    vr::VRTextureBounds_t bounds{0, 0, 1, 1};
    int transform = 0;  // 0 none, 1 absolute, 2 device-relative (not composited)
    vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
    M4 xf = ident();
    int texture_sets = 0, transform_sets = 0, shows = 0, hides = 0;
    bool destroyed = false;
};

struct SubmitRec {
    ID3D11Texture2D* tex = nullptr;  // AddRef'd
    vr::VRTextureBounds_t bounds{0, 0, 1, 1};
    bool has_bounds = false;
    uint64_t frame = 0;
    int error = 0;
};

struct Haptic {
    double t_ms;
    uint32_t device;
    uint32_t axis;
    unsigned us;
};

struct State {
    std::recursive_mutex mx;
    // Headset model (fakevr.cpp: the Steam Frame-like defaults).
    double tan_outer = 0, tan_inner = 0, tan_up = 0, tan_down = 0;
    double ipd = 0.064, cant_deg = 0;
    uint32_t rec_w = 0, rec_h = 0;
    double hz = 120;
    PoseP head;
    Anim head_anim;
    Controller ctl[kDevices];  // [kLeft], [kRight]
    bool axes_described = false;
    bool tip_component = true;
    double tip_pitch_deg = -40;  // the tip's forward relative to the raw axis (- = below)
    double tip_pos[3] = {0, -0.015, -0.05};
    // Compositor.
    vr::ETrackingUniverseOrigin universe = vr::TrackingUniverseSeated;
    uint64_t frame = 0;          // WaitGetPoses count
    bool waited = false;
    PoseP frame_head;            // the head pose WaitGetPoses last returned (what the submits were drawn for)
    M4 frame_ctl[kDevices];      // and the controllers' pointing poses
    bool frame_ctl_ok[kDevices] = {};
    SubmitRec submit[2];
    bool submitted_this_frame[2] = {};
    std::vector<Overlay> overlays;
    std::vector<Haptic> haptics;
    size_t haptics_reported = 0;
    // Stats for the once-a-second log line.
    double stat_t0 = 0, last_wait_ms = 0, frame_ms_sum = 0, frame_ms_max = 0, wait_ms_sum = 0;
    int stat_waits = 0, stat_submits[2] = {}, stat_errors = 0, stat_missed = 0, stat_overlay_tex = 0;
    int total_errors = 0;
    std::string last_error;
    // Pacing.
    double vsync0 = 0;
    long long last_vsync_k = -1;
    // Captures.
    std::string capture_name;
    bool capture_truth = true;
    double capture_scale = 1;
};
State& st();

double now_ms();
void logf(const char* fmt, ...);
std::wstring dir_path(const wchar_t* name);  // <game>\hovr\fakevr\NAME (created on first use)
std::wstring hovr_path(const wchar_t* name); // <game>\hovr\NAME

// fakevr.cpp
void advance_poses(double t_ms);  // animations up to time t
M4 head_matrix();
M4 eye_to_head(int eye);
void eye_tangents(int eye, double* l, double* r, double* b, double* t);  // b < 0 < t (y up)
M4 controller_aim(uint32_t device);  // pointing pose
M4 controller_raw(uint32_t device);  // what the device pose reports (aim * tip^-1)
M4 tip_local();
void fill_pose(uint32_t device, vr::TrackedDevicePose_t* out);
void poll_command_file();
void run_command(const std::string& line);
void tick_script(double now);

// compositor.cpp
vr::IVRCompositor* compositor();
vr::IVROverlay* overlay();
void capture_now();  // st().capture_name set: writes the captures of the last frame
void stats_tick(double now);
const char* compositor_error_name(int e);
const char* overlay_error_name(int e);
Overlay* find_overlay(const std::string& key);

}  // namespace fv
