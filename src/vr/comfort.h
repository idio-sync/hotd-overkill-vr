#pragma once
// Comfort for the rail camera's motion, free of D3D/OpenVR so it can be unit-tested
// (tests/test_stereo.cpp): rotation smoothing, horizon levelling and the speed-driven vignette.
//
// The VR view sits on the game's rail camera: a tracked pose relative to the recentre anchor
// (xform.h pose_in_game) is a pose in the game camera's space, and the engine's view matrix takes
// it into the world. Comfort puts a rotation D (`delta`) in between: the view is placed on a
// "comfort camera" at the game camera's position whose orientation R_s follows the game camera's
// R_g through a filter (and with its roll, or roll and pitch, removed), so
//     pose in the game camera's space = D * pose_in_game(anchor, pose),   D = R_g^T R_s.
// The eyes and the gun hand both go through D, so the shot still flies along the barrel the
// player sees. D is a pure rotation about the camera's origin: position is not smoothed.
//
// Spaces: the game's world and camera spaces are left-handed, +y up (stereo.h). Rotations are the
// upper 3x3 of a Mat4 (column vectors); a camera's rotation has its right, up and forward axes as
// columns (the camera -> world transform, the inverse of the view matrix).
#include "stereo.h"

namespace hv::vr {

struct Quat {
    double w, x, y, z;
};
Quat quat_from_rotation(const Mat4& r);  // a proper rotation (det +1); normalised
Mat4 rotation_from_quat(const Quat& q);  // no translation
Quat quat_slerp(const Quat& a, const Quat& b, double t);  // shortest path
double quat_angle(const Quat& a, const Quat& b);          // radians, angle of the rotation a^-1 b
float rotation_angle(const Mat4& a, const Mat4& b);       // the same for two rotation matrices
float forward_angle(const Mat4& a, const Mat4& b);        // radians between the +z (forward) axes

// The camera's pose in the world from the engine's view matrix (column form, world -> camera view
// space): rotation (axes as columns) and position (game units). False if it can't be inverted.
bool camera_pose_from_view(const Mat4& view, Mat4* cam_world);

// Horizon levelling of a camera rotation (+y up): 0 unchanged; 1 roll removed (forward kept,
// the right axis made horizontal); 2 roll and pitch removed (forward laid onto the horizon, yaw
// only). Unchanged when the forward axis is within ~0.5 degrees of vertical (no heading).
Mat4 level_rotation(const Mat4& r, int level);

// [vr] comfort_* and vignette_* (config.cpp; docs/SETTINGS.md).
struct RailComfortSettings {
    bool enabled = true;         // `vr comfort off` switches all of it off live
    float smooth_ms = 150;       // rotation smoothing time constant (0 = off)
    int level = 1;               // horizon levelling: 0 off, 1 roll, 2 roll and pitch
    float cone_deg = 35;         // the lag (forward-axis angle) is capped at half of this
    float vignette = 0.6f;       // the vignette's strength at full speed (0 = off)
    float turn_dps[2] = {30, 120};  // rail turn speed where the vignette starts / is full, deg/s
    float move_mps[2] = {2, 6};     // rail speed where it starts / is full, m/s
    float ease_ms = 200;         // the vignette fades in and out over this
};
constexpr float kCutMetres = 1.0f;       // a camera jump this far in one frame is a cut
constexpr float kCutDegrees = 45.0f;     // ... or a turn this large
constexpr float kGapSeconds = 0.25f;     // no update for this long (flat frames between): start afresh
constexpr float kSpeedTauSeconds = 0.1f;  // the speeds are smoothed over ~100 ms

// 0 below a, 1 above b, linear between (a step at a when b <= a).
float ramp(float v, float a, float b);
// The vignette strength the rail speeds call for (0..s.vignette).
float vignette_target(const RailComfortSettings& s, float turn_dps, float move_mps);

// The vignette's shape for a strength e (0..1), by the angle from the eye's forward axis: clear
// inside vignette_inner_deg(e), darkening smoothly to vignette_max_alpha(e) kVignetteFalloffDeg
// further out. Stronger means darker and a smaller clear area: e = 0.6 is 96% dark beyond 50
// degrees (the edges of a ~100-degree eye) and clear inside 35; e = 1 is black beyond 40.
constexpr float kVignetteFalloffDeg = 15.0f;
float vignette_inner_deg(float e);  // 50 - 25 e
float vignette_max_alpha(float e);  // min(1, 1.6 e)
float vignette_alpha(float e, float angle_deg);

// The rail camera's filter, speeds and vignette. update() once per stereo frame with the game
// camera's world pose (camera_pose_from_view), metres per game unit and seconds since the last
// update; delta() is D above.
class RailComfort {
public:
    void update(const Mat4& cam_world, float unit_m, float dt, const RailComfortSettings& s);
    void reset() { have_ = false; }  // the next update starts afresh (snaps)
    const Mat4& delta() const { return delta_; }
    float turn_dps() const { return turn_; }      // smoothed rail turn speed
    float move_mps() const { return move_; }      // smoothed rail speed
    float raw_turn_dps() const { return raw_turn_; }  // this frame's (0 on a cut or a fresh start)
    float raw_move_mps() const { return raw_move_; }
    float vignette() const { return vig_; }       // the vignette strength to draw now
    bool cut() const { return cut_; }             // the last update was a camera cut
    bool fresh() const { return fresh_; }         // the last update started afresh
    bool capped() const { return capped_; }       // the last update hit the lag cap
    float lag_deg() const { return lag_deg_; }    // forward-axis angle between the view and the game camera

private:
    bool have_ = false, cut_ = false, fresh_ = false, capped_ = false;
    Mat4 prev_ = identity();
    Quat smooth_{1, 0, 0, 0};
    Mat4 delta_ = identity();
    float turn_ = 0, move_ = 0, raw_turn_ = 0, raw_move_ = 0, vig_ = 0, lag_deg_ = 0;
};

}  // namespace hv::vr
