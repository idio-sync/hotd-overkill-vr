// Unit tests for src/vr/stereo (no game, no D3D). Run: ctest --test-dir out
#include <cmath>
#include <cstdio>

#include "../src/vr/stereo.h"
#include "../src/vr/xform.h"

using namespace hv::vr;

static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        const double va = (a), vb = (b);                                                           \
        if (!(std::fabs(va - vb) <= (tol))) {                                                      \
            std::printf("%s:%d: %s = %.9g, expected %s = %.9g\n", __FILE__, __LINE__, #a, va, #b, vb); \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static const float kPi = 3.14159265f;

// The game's projection as it sits in memory at VA 0x9C7DC0 in gameplay (D3DX row-vector form,
// probe 2026-11-17): 100 x 68 degrees, 16:9, near 5, far 18000.
static const float kGameD3dx[16] = {0.8391f, 0, 0, 0, 0, 1.49173f, 0, 0, 0, 0, 1.00028f, 1, 0, 0, -5.00139f, 0};

static Mat4 game_proj() { return from_d3dx(kGameD3dx); }

// NDC x, y of a view-space point (game units) through C * P_game.
static void eye_ndc(const Mat4& c, const Vec4& view, float* x, float* y) {
    Vec4 clip = apply(mul(c, game_proj()), view);
    *x = clip.x / clip.w;
    *y = clip.y / clip.w;
}

static Mat4 correction_for(const Mat4& head, float ipd, int eye, float unit_m) {
    Proj pg;
    proj_from_matrix(game_proj(), &pg);
    Proj pe = sim_eye_proj(pg, 16.0f / 9.0f);
    Mat4 c;
    correction(game_proj(), pe, eye_view(eye_pose(head, ipd, eye), unit_m), &c);
    return c;
}

static void test_from_d3dx_is_transpose() {
    Mat4 p = game_proj();
    CHECK_NEAR(p.m[0][0], 0.8391, 1e-6);
    CHECK_NEAR(p.m[3][2], 1, 1e-6);       // w = z
    CHECK_NEAR(p.m[2][3], -5.00139, 1e-6);  // z_clip = a z + b
    Proj pr;
    CHECK(proj_from_matrix(p, &pr));
    CHECK_NEAR(pr.sy, 1.49173, 1e-6);
    CHECK_NEAR(pr.a, 1.00028, 1e-6);
    CHECK_NEAR(pr.b, -5.00139, 1e-6);
    // An orthographic matrix is not a perspective one.
    Mat4 ortho = identity();
    CHECK(!proj_from_matrix(ortho, &pr));
}

static void test_game_projection_round_trip() {
    Mat4 p = game_proj(), inv;
    CHECK(invert(p, &inv));
    CHECK(max_abs_diff(mul(inv, p), identity()) < 1e-5f);
    // A view-space point survives projection and unprojection (after the homogeneous divide).
    const Vec4 v = {-120, 35, 900, 1};
    Vec4 clip = apply(p, v);
    CHECK_NEAR(clip.w, 900, 1e-3);
    CHECK_NEAR(clip.z / clip.w, (1.00028 * 900 - 5.00139) / 900, 1e-6);
    Vec4 back = apply(inv, clip);
    CHECK_NEAR(back.x / back.w, -120, 1e-2);
    CHECK_NEAR(back.y / back.w, 35, 1e-2);
    CHECK_NEAR(back.z / back.w, 900, 1e-1);
    // Near and far planes map to depth 0 and 1.
    Vec4 n = apply(p, {0, 0, 5, 1}), f = apply(p, {0, 0, 18000, 1});
    CHECK_NEAR(n.z / n.w, 0, 1e-4);
    CHECK_NEAR(f.z / f.w, 1, 1e-3);
}

static void test_identity_when_eye_is_game_camera() {
    Proj pg;
    proj_from_matrix(game_proj(), &pg);
    Mat4 c;
    CHECK(correction(game_proj(), pg, identity(), &c));
    CHECK(max_abs_diff(c, identity()) < 1e-5f);
    // The sim eye at the game's aspect is the game's projection, so zero IPD and no head turn
    // is the identity too.
    Mat4 c2 = correction_for(identity(), 0, 0, 0.01f);
    CHECK(max_abs_diff(c2, identity()) < 1e-4f);
}

static void test_left_eye_sees_point_ahead_to_the_right() {
    const Vec4 ahead = {0, 0, 500, 1};  // 5 m ahead at 1 cm per unit
    float lx, ly, rx, ry;
    eye_ndc(correction_for(identity(), 0.064f, 0, 0.01f), ahead, &lx, &ly);
    eye_ndc(correction_for(identity(), 0.064f, 1, 0.01f), ahead, &rx, &ry);
    CHECK(lx > 0);
    CHECK(rx < 0);
    // The left eye sits 3.2 units left of the camera: x_eye = +3.2 at z = 500.
    CHECK_NEAR(lx, 0.8391 * 3.2 / 500, 1e-5);
    CHECK_NEAR(rx, -0.8391 * 3.2 / 500, 1e-5);
    CHECK_NEAR(ly, 0, 1e-6);
    // Parallax shrinks with distance.
    float fx, fy;
    eye_ndc(correction_for(identity(), 0.064f, 0, 0.01f), {0, 0, 5000, 1}, &fx, &fy);
    CHECK(fx > 0 && fx < lx);
    // Eye offset for eyePos: left eye 3.2 units to the camera's left.
    float off[3];
    eye_offset_units(eye_pose(identity(), 0.064f, 0), 0.01f, off);
    CHECK_NEAR(off[0], -3.2, 1e-5);
    CHECK_NEAR(off[1], 0, 1e-6);
    CHECK_NEAR(off[2], 0, 1e-6);
}

static void test_yawed_head_moves_point_left() {
    const float yaw = 30 * kPi / 180;
    Mat4 head = rotation_yaw_pitch(yaw, 0);
    float x, y;
    // A point straight ahead of the rail camera: head turned right, so it moves left.
    eye_ndc(correction_for(head, 0, 0, 0.01f), {0, 0, 500, 1}, &x, &y);
    CHECK(x < 0);
    CHECK_NEAR(x, -0.8391 * std::tan(yaw), 1e-4);
    // A point 30 degrees to the right is now in the middle.
    eye_ndc(correction_for(head, 0, 0, 0.01f), {500 * std::sin(yaw), 0, 500 * std::cos(yaw), 1}, &x, &y);
    CHECK_NEAR(x, 0, 1e-5);
    CHECK_NEAR(y, 0, 1e-5);
    // Pitching up moves a point ahead down.
    eye_ndc(correction_for(rotation_yaw_pitch(0, 10 * kPi / 180), 0, 0, 0.01f), {0, 0, 500, 1}, &x, &y);
    CHECK(y < 0);
    CHECK_NEAR(y, -1.49173 * std::tan(10 * kPi / 180), 1e-4);
    // The rotation keeps depth in range: a point on the turned eye's axis keeps its distance.
    Vec4 clip = apply(mul(correction_for(head, 0, 0, 0.01f), game_proj()), {500 * std::sin(yaw), 0, 500 * std::cos(yaw), 1});
    CHECK_NEAR(clip.w, 500, 1e-2);
}

static void test_widened_game_projection_is_undone() {
    // If the game's projection were wider than the eye's, C re-frames to the eye's FOV.
    Proj wide = proj_from_tangents(-3, 3, -2, 2, 1.00028f, -5.00139f);
    Proj pg;
    proj_from_matrix(game_proj(), &pg);
    Mat4 c;
    CHECK(correction(proj_matrix(wide), pg, identity(), &c));
    const Vec4 v = {100, -40, 700, 1};
    Vec4 a = apply(mul(c, proj_matrix(wide)), v), b = apply(game_proj(), v);
    CHECK_NEAR(a.x / a.w, b.x / b.w, 1e-5);
    CHECK_NEAR(a.y / a.w, b.y / b.w, 1e-5);
    CHECK_NEAR(a.z / a.w, b.z / b.w, 1e-5);
}

static void test_tangents_round_trip() {
    Proj p = proj_from_tangents(-1.2f, 0.9f, -1.1f, 1.0f, 1, -5);
    float t[4];
    proj_tangents(p, t);
    CHECK_NEAR(t[0], -1.2, 1e-6);
    CHECK_NEAR(t[1], 0.9, 1e-6);
    CHECK_NEAR(t[2], -1.1, 1e-6);
    CHECK_NEAR(t[3], 1.0, 1e-6);
    // The right edge direction lands on ndc x = +1.
    Vec4 c = apply(proj_matrix(p), {0.9f * 10, 0, 10, 1});
    CHECK_NEAR(c.x / c.w, 1, 1e-6);
}

static void test_widened_half_angle() {
    const float d = kPi / 180;
    CHECK_NEAR(widened_half_angle(34 * d, 35 * d, 2 * d), 71 * d, 1e-6);
    CHECK_NEAR(widened_half_angle(50 * d, 35 * d, 10 * d), 89 * d, 1e-6);  // clamped below 90
}

static void test_rigid_inverse() {
    Mat4 a = mul(translation(1, 2, 3), rotation_yaw_pitch(0.7f, -0.3f));
    CHECK(max_abs_diff(mul(a, rigid_inverse(a)), identity()) < 1e-5f);
}

// --- OpenVR <-> game camera (xform.h) ---

static Mat4 ry_rh(float a) {  // right-handed rotation about +y by a (a > 0: forward -z turns towards -x)
    Mat4 r = identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c, r.m[0][2] = s, r.m[2][0] = -s, r.m[2][2] = c;
    return r;
}
static Mat4 rx_rh(float a) {  // a > 0: forward -z turns towards +y (looking up)
    Mat4 r = identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c, r.m[1][2] = -s, r.m[2][1] = s, r.m[2][2] = c;
    return r;
}

static void test_flip_z_conversions() {
    const float a = 25 * kPi / 180;
    // flip_z is its own inverse.
    Mat4 p = mul(translation(0.1f, -0.2f, 0.3f), mul(ry_rh(0.4f), rx_rh(-0.2f)));
    CHECK(max_abs_diff(flip_z(flip_z(p)), p) < 1e-6f);
    // OpenVR head turned right (forward -z -> +x) = the game camera's yaw right (+z -> +x).
    Mat4 right = ry_rh(-a);
    Vec4 fwd = apply(right, {0, 0, -1, 0});
    CHECK(fwd.x > 0);
    CHECK(max_abs_diff(head_in_game(identity(), right), rotation_yaw_pitch(a, 0)) < 1e-6f);
    // Looking up.
    CHECK(max_abs_diff(head_in_game(identity(), rx_rh(a)), rotation_yaw_pitch(0, a)) < 1e-6f);
    // Leaning forward 10 cm in the room (-z) = +z in the game camera's space; right and up stay.
    Mat4 lean = head_in_game(identity(), translation(0.05f, 0.02f, -0.1f));
    CHECK_NEAR(lean.m[0][3], 0.05, 1e-6);
    CHECK_NEAR(lean.m[1][3], 0.02, 1e-6);
    CHECK_NEAR(lean.m[2][3], 0.1, 1e-6);
    // Relative to an anchor: the anchor itself is the identity, whatever its yaw and position.
    Mat4 anchor = mul(translation(1, 1.2f, -0.5f), ry_rh(2.0f));
    CHECK(max_abs_diff(head_in_game(anchor, anchor), identity()) < 1e-5f);
    // A head turned 25 degrees right of the anchor's heading is a 25-degree yaw in the game.
    CHECK(max_abs_diff(head_in_game(anchor, mul(anchor, right)), rotation_yaw_pitch(a, 0)) < 1e-5f);
    // Eye-to-head: the left eye 3.2 cm to the left stays to the left.
    Mat4 eye = eye_in_game(identity(), translation(-0.032f, 0, 0.01f));
    CHECK_NEAR(eye.m[0][3], -0.032, 1e-7);
    CHECK_NEAR(eye.m[2][3], -0.01, 1e-7);  // 1 cm behind the head's centre (OpenVR +z = backwards)
    // rows34 round trip.
    float rows[3][4];
    to_rows34(p, rows);
    CHECK(max_abs_diff(from_rows34(rows), p) < 1e-7f);
}

static void test_yaw_only_anchor() {
    const float yaw = 0.9f, pitch = 0.35f;
    Mat4 head = mul(translation(0.2f, 1.1f, 0.3f), mul(ry_rh(yaw), rx_rh(pitch)));
    Mat4 a = yaw_only_rh(head);
    Mat4 expect = mul(translation(0.2f, 1.1f, 0.3f), ry_rh(yaw));
    CHECK(max_abs_diff(a, expect) < 1e-5f);
    // Recentred there, the head keeps its pitch only.
    CHECK(max_abs_diff(head_in_game(a, head), rotation_yaw_pitch(0, pitch)) < 1e-5f);
}

// OpenVR's own projection from raw tangents (openvr_api ComputeProjection; GetProjectionRaw's
// "top" is the lower edge in y-up terms).
static void openvr_matrix(float l, float r, float t, float b, float out[4][4]) {
    const float n = 0.1f, f = 100, idx = 1 / (r - l), idy = 1 / (b - t), idz = 1 / (f - n);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = 0;
    out[0][0] = 2 * idx, out[0][2] = (r + l) * idx;
    out[1][1] = 2 * idy, out[1][2] = (b + t) * idy;
    out[2][2] = -f * idz, out[2][3] = -f * n * idz;
    out[3][2] = -1;
}

static void test_tangents_from_openvr() {
    float p[4][4], t[4];
    openvr_matrix(-1.25f, 1.05f, -1.1f, 1.3f, p);
    CHECK(tangents_from_openvr(p, t));
    CHECK_NEAR(t[0], -1.25, 1e-5);
    CHECK_NEAR(t[1], 1.05, 1e-5);
    // In y-up terms the image's lower edge is GetProjectionRaw's "top" and the upper edge its "bottom".
    CHECK_NEAR(t[2], -1.1, 1e-5);
    CHECK_NEAR(t[3], 1.3, 1e-5);
    // A direction on the upper edge lands on the top of the image (ndc y = +1) through OpenVR's matrix...
    const float dir[4] = {0, 1.3f, -1, 0};
    float cy = 0, cw = 0;
    for (int k = 0; k < 4; ++k) cy += p[1][k] * dir[k], cw += p[3][k] * dir[k];
    CHECK_NEAR(cy / cw, 1, 1e-5);
    // ... and through the game-convention projection built from the tangents.
    Vec4 c = apply(proj_matrix(proj_from_tangents(t[0], t[1], t[2], t[3], 1, -5)), {0, 1.3f, 1, 1});
    CHECK_NEAR(c.y / c.w, 1, 1e-5);
    // Not a perspective matrix.
    float o[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    CHECK(!tangents_from_openvr(o, t));
}

static void test_panel_projection() {
    const Proj eye = proj_from_tangents(-1, 1, -1, 1, 1, -5);
    const Mat4 pose = panel_pose(identity(), 1.5f, 0);
    float c[4][3];
    CHECK(project_panel(identity(), pose, 1.6f, 1.0f, eye, c));
    // Top-left, top-right, bottom-right, bottom-left; 1.5 m away.
    CHECK_NEAR(c[0][0] / c[0][2], -0.8 / 1.5, 1e-5);
    CHECK_NEAR(c[0][1] / c[0][2], 0.5 / 1.5, 1e-5);
    CHECK_NEAR(c[2][0] / c[2][2], 0.8 / 1.5, 1e-5);
    CHECK_NEAR(c[2][1] / c[2][2], -0.5 / 1.5, 1e-5);
    CHECK_NEAR(c[0][2], 1.5, 1e-5);
    // Dropped 0.2 m: the centre is below the middle.
    float d[4][3];
    project_panel(identity(), panel_pose(identity(), 1.5f, 0.2f), 1.6f, 1.0f, eye, d);
    CHECK_NEAR((d[0][1] / d[0][2] + d[3][1] / d[3][2]) / 2, -0.2 / 1.5, 1e-5);
    // Head turned right: the world-locked panel moves left.
    float y[4][3];
    project_panel(ry_rh(-0.3f), pose, 1.6f, 1.0f, eye, y);
    CHECK((y[0][0] / y[0][2] + y[1][0] / y[1][2]) / 2 < -0.1f);
    // Left eye (3.2 cm left): the panel appears shifted right, the right eye the opposite.
    float l[4][3], r[4][3];
    project_panel(translation(-0.032f, 0, 0), pose, 1.6f, 1.0f, eye, l);
    project_panel(translation(0.032f, 0, 0), pose, 1.6f, 1.0f, eye, r);
    CHECK_NEAR(l[0][0] / l[0][2] - c[0][0] / c[0][2], 0.032 / 1.5, 1e-5);
    CHECK_NEAR(r[0][0] / r[0][2] - c[0][0] / c[0][2], -0.032 / 1.5, 1e-5);
    // Behind the head: not drawable.
    CHECK(!project_panel(ry_rh(kPi), pose, 1.6f, 1.0f, eye, y));
    // The anchor carries the panel: an anchor turned 90 degrees left puts it on the left.
    Mat4 left_anchor = ry_rh(kPi / 2);
    Mat4 lp = panel_pose(left_anchor, 1.5f, 0);
    CHECK_NEAR(lp.m[0][3], -1.5, 1e-5);
    CHECK_NEAR(lp.m[2][3], 0, 1e-5);
}

int main() {
    test_flip_z_conversions();
    test_yaw_only_anchor();
    test_tangents_from_openvr();
    test_panel_projection();
    test_from_d3dx_is_transpose();
    test_game_projection_round_trip();
    test_identity_when_eye_is_game_camera();
    test_left_eye_sees_point_ahead_to_the_right();
    test_yawed_head_moves_point_left();
    test_widened_game_projection_is_undone();
    test_tangents_round_trip();
    test_widened_half_angle();
    test_rigid_inverse();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all stereo and xform tests passed\n");
    return 0;
}
