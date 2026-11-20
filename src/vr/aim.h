#pragma once
// Controller aiming math, free of D3D/OpenVR so it can be unit-tested (tests/test_stereo.cpp).
//
// Spaces (see xform.h): OpenVR tracking/anchor space is right-handed (+x right, +y up, -z
// forward, metres); the game camera's space is left-handed (+z forward). A controller points
// along its -z axis (right-handed), i.e. along +z once converted to the game camera's space.
//
// The hand's ray goes to the game's world through the same transform the eyes use: the tracked
// pose relative to the recentre anchor (xform.h pose_in_game), then the game camera's world
// transform (the inverse of the engine's view matrix) with metres converted to game units.
#include "stereo.h"

namespace hv::vr {

struct Ray {
    float o[3];  // origin
    float d[3];  // unit direction
};

// A right-handed rotation that turns the forward axis (-z) right by `yaw` and up by `pitch`
// (radians), the same convention as the sim head (rotation_yaw_pitch, converted with flip_z).
Mat4 rh_yaw_pitch(float yaw, float pitch);

// [vr] hand_offset / hand_pitch: the grip-to-aim correction in the controller's own
// (right-handed) frame: move by `offset_m` (x right, y up, z back), then tilt the aim up by
// `pitch_deg` degrees.
Mat4 hand_adjust(const float offset_m[3], float pitch_deg);

// The pointing ray of a pose in the game camera's (left-handed) space: its origin and its +z axis.
Ray pose_ray_lh(const Mat4& pose_lh);
// The pointing ray of a right-handed pose: its origin and its -z axis.
Ray pose_ray_rh(const Mat4& pose_rh);

// A ray in the game camera's space (metres) as a world ray in game units. `view` is the engine's
// view matrix in column form (world -> camera view space, game units). False if it can't be
// inverted.
bool camera_ray_to_world(const Mat4& view, const Ray& cam_m, float unit_m, Ray* world);
// A world point (game units) in the game camera's space (metres).
bool world_point_to_camera(const Mat4& view, const float p_world[3], float unit_m, float out_m[3]);

// Where a ray (right-handed, same space as the panel) meets a panel of width x height metres
// centred at `panel_rh` (lying in its local x-y plane, seen from its +z side, as SteamVR
// overlays are): u from 0 (left) to 1 (right), v from 0 (top) to 1 (bottom). False if the ray
// points away from the panel or comes from behind it; u, v may lie outside 0..1 (missed it).
bool ray_panel_uv(const Mat4& panel_rh, float width, float height, const Ray& ray_rh, float* u, float* v);
// The point (right-handed) of a panel at u, v.
void panel_point(const Mat4& panel_rh, float width, float height, float u, float v, float out[3]);

// The game's ScreenToWorld (0x655620) maps a cursor (x, y) to a point on the camera's near plane,
// affinely: base + x * du + y * dv. Given those three vectors, the cursor whose near-plane point
// lies on the line o + t d (and that point, and t). False if the line runs parallel to the plane.
bool cursor_on_line(const float base[3], const float du[3], const float dv[3], const float o[3], const float d[3], float* x,
                    float* y, float point[3], float* t);

// Yaw and pitch (radians, rh_yaw_pitch convention) that point a right-handed frame `frame_rh`'s
// forward axis along `dir_rh` (a direction in the frame's parent space).
void yaw_pitch_towards(const Mat4& frame_rh, const float dir_rh[3], float* yaw, float* pitch);

// Shots aim with the gun hand of the frame on screen (the pose its gun and eyes were drawn with),
// recorded at frame `drawn_frame`. At frame `now` that record is still the one to use if one was
// made and it is at most `max_age` frames old (a shot normally comes one frame after the frame
// drawn); otherwise (flat screens, menus, the first frames, no stereo frame for a while) the
// latest pose is used.
constexpr unsigned kDrawnPoseMaxAge = 3;
bool use_drawn_pose(bool recorded, unsigned drawn_frame, unsigned now, unsigned max_age = kDrawnPoseMaxAge);

}  // namespace hv::vr
