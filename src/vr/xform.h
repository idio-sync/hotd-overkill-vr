#pragma once
// OpenVR <-> game-camera conversions and panel placement, free of D3D/OpenVR so they can be
// unit-tested (tests/test_stereo.cpp).
//
// OpenVR tracking space is right-handed: +x right, +y up, -z forward, metres. Its poses are 3x4
// row-major [R | t] for column vectors (HmdMatrix34_t), i.e. the first three rows of a Mat4 here.
// The game camera's space (stereo.h) is left-handed: +x right, +y up, +z forward. The two differ
// by a z flip S = diag(1, 1, -1): a pose P in one is S P S in the other (S is its own inverse).
// Translations stay in metres here; stereo.h's eye_view converts to game units.
#include "stereo.h"

namespace hv::vr {

// 3x4 row-major pose (HmdMatrix34_t layout) -> Mat4, and back.
Mat4 from_rows34(const float m[3][4]);
void to_rows34(const Mat4& p, float out[3][4]);

// Right-handed (OpenVR) <-> left-handed (game camera) pose. Self-inverse.
Mat4 flip_z(const Mat4& p);

// The rotation about +y (yaw) of a right-handed pose, with its translation: a recentre anchor
// that keeps the seated player's heading but not head tilt.
Mat4 yaw_only_rh(const Mat4& p);

// Head pose relative to the anchor, as a pose in the game camera's (left-handed) space, metres.
// The anchor is where the game's rail camera sits in the room.
Mat4 head_in_game(const Mat4& anchor_rh, const Mat4& head_rh);

// Eye pose in the game camera's space: head (left-handed, from head_in_game) composed with an
// OpenVR eye-to-head transform (right-handed).
Mat4 eye_in_game(const Mat4& head_lh, const Mat4& eye_to_head_rh);

// Half-angle tangents (left < 0 < right, bottom < 0 < top, y up) from an OpenVR projection matrix
// (GetProjectionMatrix: right-handed, w = -z). They are the same numbers in the game camera's
// left-handed space (tangent = x / forward distance). False if it isn't a perspective matrix.
bool tangents_from_openvr(const float p[4][4], float out[4]);

// A panel of width x height metres, centred at `pose_rh` (OpenVR: the panel lies in its local
// x-y plane, facing +z), seen by an eye at `eye_rh` (both in the same space, e.g. relative to the
// anchor) through a projection `eye` (game-camera convention). Writes clip-space x, y, w of the
// corners in the order top-left, top-right, bottom-right, bottom-left (as the panel's texture
// is laid out: u right, v down). False if any corner is behind the eye.
bool project_panel(const Mat4& eye_rh, const Mat4& pose_rh, float width, float height, const Proj& eye, float out[4][3]);

// Where a panel sits: `distance` metres in front of the anchor, `drop` metres below it.
Mat4 panel_pose(const Mat4& anchor_rh, float distance, float drop);

}  // namespace hv::vr
