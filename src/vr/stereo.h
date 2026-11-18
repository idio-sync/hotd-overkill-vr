#pragma once
// Stereo math for the VR view, free of D3D/OpenVR so it can be unit-tested (tests/test_stereo.cpp).
//
// Conventions (the game's, D3DX left-handed): view space +x right, +y up, +z forward; clip
// space D3D (x, y in -w..w, z in 0..w). Matrices are row-major for COLUMN vectors: v' = M v,
// m[r][c]. The game's vertex shaders compute oPos.i = dp4(v, c[i]), so the four constant
// registers of cWVP are exactly the rows of M here. D3DX matrices in memory (row-vector form,
// v' = v M) are the transpose: use from_d3dx().
//
// Head and eye poses are rigid transforms of the game camera's space, translation in metres.
// Yaw > 0 turns the head right (+z towards +x), pitch > 0 looks up (+z towards +y).

namespace hv::vr {

struct Mat4 {
    float m[4][4];
};

struct Vec4 {
    float x, y, z, w;
};

Mat4 identity();
Mat4 mul(const Mat4& a, const Mat4& b);  // a * b (apply b, then a)
bool invert(const Mat4& a, Mat4* out);   // general inverse (computed in double); false if singular
Vec4 apply(const Mat4& a, const Vec4& v);
Mat4 from_d3dx(const float rowvec[16]);  // D3DX row-vector matrix in memory -> column form (transpose)
float max_abs_diff(const Mat4& a, const Mat4& b);

// Rigid transforms.
Mat4 translation(float x, float y, float z);
Mat4 rotation_yaw_pitch(float yaw_rad, float pitch_rad);  // yaw about +y, then pitch about +x (head frame)
Mat4 rigid_inverse(const Mat4& p);

// Perspective projections, LH, D3D depth: clip = (sx x + cx z, sy y + cy z, a z + b, z).
struct Proj {
    float sx, sy, cx, cy, a, b;
};
Mat4 proj_matrix(const Proj& p);
// Reads a column-form perspective matrix; false if it is not one (e.g. orthographic).
bool proj_from_matrix(const Mat4& m, Proj* out);
// Off-centre frustum from tangents of the half-angles (y up: left, bottom < 0 < right, top),
// keeping depth terms a, b (so all eyes share the game's near/far).
Proj proj_from_tangents(float left, float right, float bottom, float top, float a, float b);
// Sim-mode eye: symmetric, the game's vertical FOV, the eye image's aspect (width / height).
Proj sim_eye_proj(const Proj& game, float eye_aspect);
// Half-angle tangents of a projection: left (<0), right, bottom (<0), top.
void proj_tangents(const Proj& p, float out[4]);

// Eye pose in the game camera's space (metres): head pose, then half the IPD along the head's
// x axis (eye 0 = left = -ipd/2, eye 1 = right = +ipd/2).
Mat4 eye_pose(const Mat4& head_in_camera, float ipd_m, int eye);
// View transform of an eye: maps a point in the game camera's view space (game units) to the
// eye's view space (game units). unit_m = metres per game unit.
Mat4 eye_view(const Mat4& eye_in_camera_m, float unit_m);
// Eye position in the game camera's view space, game units.
void eye_offset_units(const Mat4& eye_in_camera_m, float unit_m, float out[3]);

// Per-eye clip-space correction: C = P_eye * E * P_game^-1, so that cWVP_eye = C * cWVP.
bool correction(const Mat4& p_game, const Proj& p_eye, const Mat4& e_view, Mat4* out);

// Culling: the half-angle the game camera's frustum needs so that an eye with half-angle
// `eye_half` (radians, relative to the camera's forward) still sees everything when the head
// turns up to `cone` radians; plus `margin`, clamped below 90 degrees.
float widened_half_angle(float eye_half, float cone, float margin);

// The real size of a screen-sized render target created double-wide for stereo. The scene's
// 1280x720 family (scene, depth, bloom/blur chain) scales with the eye size relative to 1280x720,
// so the scene target holds exactly two eye_w x eye_h eyes. The composite target (the window's
// size or a 16:10 one, e.g. 1680x1050) holds exactly two eyes too: the game's composite draw
// stretches the scene over it, so any other size only resamples the eyes (non-square pixels, more
// pixels to post-process, copy and submit than the headset asked for).
void stereo_target_real_size(bool composite, unsigned w, unsigned h, int eye_w, int eye_h, unsigned* out_w, unsigned* out_h);

}  // namespace hv::vr
