// Unit tests for src/vr/stereo (no game, no D3D). Run: ctest --test-dir out
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "../src/vr/stereo.h"
#include "../src/vr/aim.h"
#include "../src/vr/comfort.h"
#include "../src/vr/gunfit.h"
#include "../src/vr/gunpose.h"
#include "../src/vr/xform.h"
#include "../src/vr/harness.h"
#include "../src/vr/settings_file.h"
#include "hovr_default_ini.h"

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

static void test_stereo_target_real_size() {
    unsigned w, h;
    // The scene family: two eyes side by side at eye resolution (Steam Frame-like 2064x2016).
    stereo_target_real_size(false, 1280, 720, 2064, 2016, &w, &h);
    CHECK(w == 4128 && h == 2016);
    stereo_target_real_size(false, 640, 360, 2064, 2016, &w, &h);
    CHECK(w == 2064 && h == 1008);
    // The composite target holds exactly two eyes whatever its game size: 1680x1050 (16:10 at a
    // 1920x1200 back buffer) used to become 5418x2940 (2709x2940 per eye for a 2064x2016 eye).
    stereo_target_real_size(true, 1680, 1050, 2064, 2016, &w, &h);
    CHECK(w == 4128 && h == 2016);
    stereo_target_real_size(true, 1291, 807, 1280, 720, &w, &h);
    CHECK(w == 2560 && h == 720);
}

static void test_composite_game_size() {
    unsigned w = 0, h = 0;
    // seen: 1680x1050 configured, 1920x1200 back buffer (fits as it is)
    CHECK(composite_game_size(1680, 1050, 1920, 1200, &w, &h) && w == 1680 && h == 1050);
    // seen: 1680x1050 configured, 1536x807 window -> 1291x807
    CHECK(composite_game_size(1680, 1050, 1536, 807, &w, &h) && w == 1291 && h == 807);
    // user report 2026-11-19: 1920x1080 at a 2560x1440 back buffer
    CHECK(composite_game_size(1920, 1080, 2560, 1440, &w, &h) && w == 1920 && h == 1080);
    // wider than the back buffer: width-bound
    CHECK(composite_game_size(2560, 1080, 1920, 1200, &w, &h) && w == 1920 && h == 810);
    CHECK(!composite_game_size(0, 0, 1920, 1200, &w, &h));
    CHECK(!composite_game_size(1920, 1080, 0, 0, &w, &h));

    const unsigned char ours[12] = {0x00, 0x01, 0x90, 0x06, 0x1A, 0x04};  // the test save's config
    CHECK(config_resolution(ours, sizeof ours, &w, &h) && w == 1680 && h == 1050);
    const unsigned char bad[12] = {0x00, 0x01, 0x10, 0x00, 0x10, 0x00};
    CHECK(!config_resolution(bad, sizeof bad, &w, &h));
    CHECK(!config_resolution(ours, 4, &w, &h));
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

// --- controller aiming (src/vr/aim.h) ---

static void test_rh_yaw_pitch_forward() {
    // Yaw right 30 degrees, pitch up 20: forward (-z) turns towards +x and +y.
    const float y = 30 * kPi / 180, p = 20 * kPi / 180;
    const Ray r = pose_ray_rh(rh_yaw_pitch(y, p));
    CHECK_NEAR(r.d[0], std::sin(y) * std::cos(p), 1e-5);
    CHECK_NEAR(r.d[1], std::sin(p), 1e-5);
    CHECK_NEAR(r.d[2], -std::cos(y) * std::cos(p), 1e-5);
    // The same pose in the game camera's space points the same way with z flipped (+z forward).
    const Ray l = pose_ray_lh(flip_z(rh_yaw_pitch(y, p)));
    CHECK_NEAR(l.d[0], r.d[0], 1e-5);
    CHECK_NEAR(l.d[1], r.d[1], 1e-5);
    CHECK_NEAR(l.d[2], -r.d[2], 1e-5);
    float yy, pp;
    yaw_pitch_towards(identity(), r.d, &yy, &pp);
    CHECK_NEAR(yy, y, 1e-5);
    CHECK_NEAR(pp, p, 1e-5);
}

static void test_hand_adjust() {
    // hand_pitch tilts the aim up in the controller's own frame; hand_offset moves the origin
    // along the controller's axes (here: 5 cm forward = -z).
    const float off[3] = {0, 0, -0.05f};
    const Mat4 ctl = mul(translation(0.3f, 1.0f, -0.4f), ry_rh(kPi / 2));  // controller turned to point at -x
    const Ray r = pose_ray_rh(mul(ctl, hand_adjust(off, 40)));
    CHECK_NEAR(r.o[0], 0.3 - 0.05, 1e-5);  // 5 cm along its forward (-x)
    CHECK_NEAR(r.o[1], 1.0, 1e-5);
    CHECK_NEAR(r.o[2], -0.4, 1e-5);
    CHECK_NEAR(r.d[0], -std::cos(40 * kPi / 180), 1e-5);
    CHECK_NEAR(r.d[1], std::sin(40 * kPi / 180), 1e-5);
    CHECK_NEAR(r.d[2], 0, 1e-5);
}

// A game camera at world (100, 200, 300) units, turned 90 degrees right (looking along +x),
// as the engine's view matrix (world -> view).
static Mat4 camera_world_turned_right() { return mul(translation(100, 200, 300), rotation_yaw_pitch(kPi / 2, 0)); }

static void test_hand_ray_to_world() {
    Mat4 view;
    CHECK(invert(camera_world_turned_right(), &view));
    // Hand 20 cm right, 30 cm down, 40 cm ahead of the head, pointing straight ahead; head at the
    // anchor, yaw 0: in the game camera's space the hand is at (0.2, -0.3, 0.4) m pointing +z.
    const Mat4 anchor = translation(0.5f, 1.2f, 0.1f);
    const Mat4 hand_rh = mul(anchor, translation(0.2f, -0.3f, -0.4f));
    const Ray cam = pose_ray_lh(pose_in_game(anchor, hand_rh));
    CHECK_NEAR(cam.o[0], 0.2, 1e-5);
    CHECK_NEAR(cam.o[1], -0.3, 1e-5);
    CHECK_NEAR(cam.o[2], 0.4, 1e-5);
    CHECK_NEAR(cam.d[2], 1, 1e-5);
    Ray w;
    CHECK(camera_ray_to_world(view, cam, 0.01f, &w));
    // Camera forward = world +x, camera right = world -z (left-handed, yaw 90 right), up = +y.
    CHECK_NEAR(w.o[0], 100 + 40, 1e-3);
    CHECK_NEAR(w.o[1], 200 - 30, 1e-3);
    CHECK_NEAR(w.o[2], 300 - 20, 1e-3);
    CHECK_NEAR(w.d[0], 1, 1e-5);
    CHECK_NEAR(w.d[1], 0, 1e-5);
    CHECK_NEAR(w.d[2], 0, 1e-5);
    // And back: a world point 5 m along the ray is 5 m ahead of the hand in the camera's space.
    const float p[3] = {w.o[0] + 500 * w.d[0], w.o[1] + 500 * w.d[1], w.o[2] + 500 * w.d[2]};
    float c[3];
    CHECK(world_point_to_camera(view, p, 0.01f, c));
    CHECK_NEAR(c[0], 0.2, 1e-4);
    CHECK_NEAR(c[1], -0.3, 1e-4);
    CHECK_NEAR(c[2], 5.4, 1e-4);
}

static void test_ray_panel_uv() {
    // The flat screen 3 m ahead of a turned anchor, 3.2 x 1.8 m.
    const Mat4 anchor = mul(translation(1, 1.2f, 2), ry_rh(0.7f));
    const Mat4 panel = panel_pose(anchor, 3.0f, 0.1f);
    static const float us[] = {0.1f, 0.5f, 0.83f}, vs[] = {0.05f, 0.5f, 0.9f};
    for (float u : us)
        for (float v : vs) {
            float target[3];
            panel_point(panel, 3.2f, 1.8f, u, v, target);
            // A hand 25 cm below and right of the anchor, pointing at that point.
            const Vec4 h = apply(anchor, {0.25f, -0.25f, -0.3f, 1});
            Ray r{{h.x, h.y, h.z}, {target[0] - h.x, target[1] - h.y, target[2] - h.z}};
            const float n = std::sqrt(r.d[0] * r.d[0] + r.d[1] * r.d[1] + r.d[2] * r.d[2]);
            for (float& c : r.d) c /= n;
            float uu, vv;
            CHECK(ray_panel_uv(panel, 3.2f, 1.8f, r, &uu, &vv));
            CHECK_NEAR(uu, u, 1e-4);
            CHECK_NEAR(vv, v, 1e-4);
            // Pointing away from the panel never hits it.
            Ray back = r;
            for (float& c : back.d) c = -c;
            CHECK(!ray_panel_uv(panel, 3.2f, 1.8f, back, &uu, &vv));
        }
    // Off the panel's edge: u > 1.
    float target[3];
    panel_point(panel, 3.2f, 1.8f, 1.4f, 0.5f, target);
    const Vec4 h = apply(anchor, {0, 0, 0, 1});
    Ray r{{h.x, h.y, h.z}, {target[0] - h.x, target[1] - h.y, target[2] - h.z}};
    float uu, vv;
    CHECK(ray_panel_uv(panel, 3.2f, 1.8f, r, &uu, &vv));
    CHECK_NEAR(uu, 1.4, 1e-4);
}

static void test_cursor_on_line() {
    // A near plane at z = 5 seen from a camera at the origin: cursor (x, y) -> (x/100, y/100, 5)
    // in some rotated, shifted frame (the game's ScreenToWorld is affine like this).
    const Mat4 m = mul(translation(10, 20, 30), rotation_yaw_pitch(0.3f, -0.2f));
    auto pt = [&](float x, float y, float out[3]) {
        const Vec4 p = apply(m, {x / 100, y / 100, 5, 1});
        out[0] = p.x, out[1] = p.y, out[2] = p.z;
    };
    float base[3], px[3], py[3], du[3], dv[3];
    pt(0, 0, base), pt(1, 0, px), pt(0, 1, py);
    for (int i = 0; i < 3; ++i) du[i] = px[i] - base[i], dv[i] = py[i] - base[i];
    // A line from a hand 30 cm ahead and to the side, through the world point of cursor (120, -45)
    // at 10 m: the cursor found is (120, -45) scaled to the near plane... i.e. the line's crossing.
    float target[3];
    {
        const Vec4 p = apply(m, {1.2f * 200, -0.45f * 200, 1000, 1});  // on the camera ray of cursor (120, -45), 10 m out
        target[0] = p.x, target[1] = p.y, target[2] = p.z;
    }
    const Vec4 cam = apply(m, {0, 0, 0, 1});
    const float o[3] = {cam.x, cam.y, cam.z};
    const float d[3] = {target[0] - o[0], target[1] - o[1], target[2] - o[2]};
    float x, y, p[3], t;
    CHECK(cursor_on_line(base, du, dv, o, d, &x, &y, p, &t));
    CHECK_NEAR(x, 120, 0.05);
    CHECK_NEAR(y, -45, 0.05);
    CHECK_NEAR(t, 0.005, 1e-5);  // the near plane is 5 of the 1000 units to the target
    // A line from elsewhere (a hand off to the side) still lands its crossing on the plane.
    const Vec4 hand = apply(m, {0.5f, -0.4f, 30, 1});
    const float o2[3] = {hand.x, hand.y, hand.z};
    const float d2[3] = {target[0] - o2[0], target[1] - o2[1], target[2] - o2[2]};
    CHECK(cursor_on_line(base, du, dv, o2, d2, &x, &y, p, &t));
    float q[3];
    pt(x, y, q);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(q[i], p[i], 1e-2);
    CHECK(t < 0);  // the hand is past the near plane: the crossing is behind it
    // Parallel to the plane: no answer.
    CHECK(!cursor_on_line(base, du, dv, o, du, &x, &y, p, &t));
}

// --- milestone 3: the gun mesh in the hand ---

static float dist_to_line(const float p[3], const Ray& r) {
    float v[3] = {p[0] - r.o[0], p[1] - r.o[1], p[2] - r.o[2]};
    const float t = v[0] * r.d[0] + v[1] * r.d[1] + v[2] * r.d[2];
    for (int i = 0; i < 3; ++i) v[i] -= t * r.d[i];
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The magnum's description (barrel along +x) and one toed-in like the assault rifle's mesh.
static const GunModelDesc kMagnum = {{1, 0, 0}, {0, 1, 0}, {23.436f, 9.433f, -0.87f}, {0, 1, -0.87f}, {-0.25f, -0.97f, 0}};
static const GunModelDesc kToedIn = {{0.95f, -0.07f, -0.22f}, {0, 1, 0}, {52.9f, 2.7f, -11.05f}, {-3, -1, 0.8f}, {0, -1, 0}};

static void test_model_to_gun() {
    for (const GunModelDesc& d : {kMagnum, kToedIn}) {
        const Mat4 m = model_to_gun(d, 0.01f);
        // The muzzle goes to the origin, points along the barrel to +z (scaled to metres).
        Vec4 o = apply(m, {d.muzzle[0], d.muzzle[1], d.muzzle[2], 1});
        CHECK_NEAR(o.x, 0, 1e-6);
        CHECK_NEAR(o.y, 0, 1e-6);
        CHECK_NEAR(o.z, 0, 1e-6);
        const float n = std::sqrt(d.barrel[0] * d.barrel[0] + d.barrel[1] * d.barrel[1] + d.barrel[2] * d.barrel[2]);
        Vec4 b = apply(m, {d.muzzle[0] - 30 * d.barrel[0] / n, d.muzzle[1] - 30 * d.barrel[1] / n, d.muzzle[2] - 30 * d.barrel[2] / n, 1});
        CHECK_NEAR(b.x, 0, 1e-5);
        CHECK_NEAR(b.y, 0, 1e-5);
        CHECK_NEAR(b.z, -0.30, 1e-5);
        // Up stays up (positive y, no sideways part), and the rotation is proper (no mirroring).
        Vec4 u = apply(m, {d.up[0], d.up[1], d.up[2], 0});
        CHECK(u.y > 0);
        CHECK_NEAR(u.x, 0, 1e-6);
        const float det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                          m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
        CHECK_NEAR(det, 1e-6, 1e-9);  // (0.01)^3
    }
    // Magnum: model -z (the mesh's left side, seen from behind the barrel) is the gun's right.
    const Mat4 m = model_to_gun(kMagnum, 0.01f);
    Vec4 side = apply(m, {0, 0, -1, 0});
    CHECK_NEAR(side.x, 0.01, 1e-7);
}

static void test_gun_in_hand_holds_grip_and_barrel() {
    const float grip[3] = {0.01f, -0.03f, -0.05f};
    for (const GunModelDesc& d : {kMagnum, kToedIn}) {
        const Mat4 g = gun_in_hand(d, 0.01f, grip, 0, 0);
        const Mat4 hm = mul(g, model_to_gun(d, 0.01f));
        // The model's grip point lands on the hand's grip point.
        Vec4 p = apply(hm, {d.grip[0], d.grip[1], d.grip[2], 1});
        CHECK_NEAR(p.x, grip[0], 1e-6);
        CHECK_NEAR(p.y, grip[1], 1e-6);
        CHECK_NEAR(p.z, grip[2], 1e-6);
        // No hold rotation: the barrel is parallel to the hand's pointing axis (+z).
        Ray r = barrel_ray(g);
        CHECK_NEAR(r.d[0], 0, 1e-6);
        CHECK_NEAR(r.d[1], 0, 1e-6);
        CHECK_NEAR(r.d[2], 1, 1e-6);
        // The ray starts at the model's muzzle.
        Vec4 mz = apply(hm, {d.muzzle[0], d.muzzle[1], d.muzzle[2], 1});
        CHECK_NEAR(r.o[0], mz.x, 1e-6);
        CHECK_NEAR(r.o[1], mz.y, 1e-6);
        CHECK_NEAR(r.o[2], mz.z, 1e-6);
    }
    // A hold rotation turns the barrel (pitch 10 up, yaw 5 right) about the grip point.
    const Mat4 g = gun_in_hand(kMagnum, 0.01f, grip, 5, 10);
    Ray r = barrel_ray(g);
    CHECK_NEAR(r.d[1], std::sin(10 * kPi / 180), 1e-5);
    CHECK(r.d[0] > 0);
    Vec4 p = apply(mul(g, model_to_gun(kMagnum, 0.01f)), {kMagnum.grip[0], kMagnum.grip[1], kMagnum.grip[2], 1});
    CHECK_NEAR(p.y, grip[1], 1e-6);
}

// The drawn barrel, taken to the world with the mesh's world matrix, is the shot ray the gun code
// uses (camera_ray_to_world of the barrel ray in camera space): the barrel points exactly along it.
static void test_drawn_barrel_is_the_shot_ray() {
    const Mat4 view = mul(rotation_yaw_pitch(0.4f, -0.1f), translation(-2766, -76, -6676));  // some camera
    const Mat4 hand = mul(translation(0.15f, -0.25f, 0.35f), rotation_yaw_pitch(-0.3f, 0.2f));
    const float grip[3] = {0, -0.03f, -0.05f};
    for (const GunModelDesc& d : {kMagnum, kToedIn})
        for (float hold : {0.f, 12.f}) {
            const Mat4 g = gun_in_hand(d, 0.01f, grip, hold * 0.5f, hold);
            Mat4 w;
            CHECK(gun_world_matrix(view, 0.01f, hand, g, model_to_gun(d, 0.01f), &w));
            Ray shot;
            CHECK(camera_ray_to_world(view, barrel_ray(mul(hand, g)), 0.01f, &shot));
            // Model points along the barrel (muzzle and 25 units behind it) lie on the shot ray.
            const float n = std::sqrt(d.barrel[0] * d.barrel[0] + d.barrel[1] * d.barrel[1] + d.barrel[2] * d.barrel[2]);
            for (float t : {0.f, -25.f}) {
                Vec4 q = apply(w, {d.muzzle[0] + t * d.barrel[0] / n, d.muzzle[1] + t * d.barrel[1] / n, d.muzzle[2] + t * d.barrel[2] / n, 1});
                const float p[3] = {q.x, q.y, q.z};
                CHECK(dist_to_line(p, shot) < 1e-2f);  // game units
            }
            // ... and the muzzle is the ray's origin; world scale is 1 unit = 1 model unit (cm).
            Vec4 m0 = apply(w, {d.muzzle[0], d.muzzle[1], d.muzzle[2], 1});
            CHECK_NEAR(m0.x, shot.o[0], 2e-2);
            CHECK_NEAR(m0.y, shot.o[1], 2e-2);
            CHECK_NEAR(m0.z, shot.o[2], 2e-2);
            Vec4 m1 = apply(w, {d.muzzle[0] - d.barrel[0] / n * 10, d.muzzle[1] - d.barrel[1] / n * 10, d.muzzle[2] - d.barrel[2] / n * 10, 1});
            CHECK_NEAR(std::sqrt((m1.x - m0.x) * (m1.x - m0.x) + (m1.y - m0.y) * (m1.y - m0.y) + (m1.z - m0.z) * (m1.z - m0.z)), 10, 1e-2);
        }
    // With no hold rotation the shot direction is the hand's own pointing direction (milestone 2).
    const Mat4 g = gun_in_hand(kToedIn, 0.01f, grip, 0, 0);
    Ray a, b;
    CHECK(camera_ray_to_world(view, barrel_ray(mul(hand, g)), 0.01f, &a));
    CHECK(camera_ray_to_world(view, pose_ray_lh(hand), 0.01f, &b));
    for (int i = 0; i < 3; ++i) CHECK_NEAR(a.d[i], b.d[i], 1e-5);
}

// Shots use the hand of the frame on screen while that record is recent, else the latest.
static void test_use_drawn_pose() {
    CHECK(use_drawn_pose(true, 100, 101));   // the usual case: fired one frame after the frame drawn
    CHECK(use_drawn_pose(true, 100, 100));   // same frame
    CHECK(use_drawn_pose(true, 100, 100 + kDrawnPoseMaxAge));
    CHECK(!use_drawn_pose(true, 100, 101 + kDrawnPoseMaxAge));  // stale: flat screen, menus
    CHECK(!use_drawn_pose(false, 100, 101));  // none recorded, or the hand was not tracked
    CHECK(!use_drawn_pose(true, 102, 101));   // from the future: not used
    CHECK(use_drawn_pose(true, 0xFFFFFFFFu, 1));  // across the counter's wrap
}

static void test_clip_out_fraction() {
    // Not reloading, nothing out: seated.
    CHECK_NEAR(clip_out_fraction(false, 0.5f, 3, 0), 0, 0);
    // Reload starts: drops out over 0.15 s even while the progress still reads 0.
    CHECK_NEAR(clip_out_fraction(true, 0, 0, 0), 0, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 0, 0.075f, 0), 0.5, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 0, 0.5f, 0), 1, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 0.4f, 0.5f, 0), 1, 1e-6);
    // Back in as the progress goes from 0.6 to 0.85: seated before the reload ends.
    const float mid = clip_out_fraction(true, 0.7f, 0.8f, 0);
    CHECK(mid > 0 && mid < 1);
    CHECK_NEAR(clip_out_fraction(true, 0.85f, 1, 0), 0, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 1.0f, 1, 0), 0, 1e-6);
    // The reload ended early (flag down) with the magazine still out: back in 0.15 s.
    CHECK_NEAR(clip_out_fraction(false, 0, 0, 0.8f), 0.8, 1e-6);
    CHECK_NEAR(clip_out_fraction(false, 0, 0.075f, 0.8f), 0.4, 1e-6);
    CHECK_NEAR(clip_out_fraction(false, 0, 0.2f, 0.8f), 0, 1e-6);
}

// Release vs development: the test copy (probe.ini) keeps the whole harness and its save redirect
// in every mode; a release install never redirects, never hides or mutes the window, never polls a
// command file, and with mode=off (or Shift held at start) hooks nothing at all.
static void test_harness_plan() {
    for (bool vr_mode : {false, true})
        for (bool shift : {false, true}) {
            HarnessPlan dev = plan_harness({true, false, vr_mode, shift});
            CHECK(dev.dev && dev.hooks && dev.redirect_save && dev.command_file && dev.dev_logging && dev.probe_ini_keys);
            CHECK(dev.minimized && dev.mute && dev.fake_pad);
            CHECK(dev.vr == (vr_mode && !shift));
            HarnessPlan key = plan_harness({false, true, vr_mode, shift});
            CHECK(key.dev && key.redirect_save && key.command_file);
            HarnessPlan rel = plan_harness({false, false, vr_mode, shift});
            CHECK(!rel.dev && !rel.redirect_save && !rel.command_file && !rel.dev_logging && !rel.probe_ini_keys);
            CHECK(!rel.minimized && !rel.mute && !rel.fake_pad);
            CHECK(rel.vr == (vr_mode && !shift));
            CHECK(rel.hooks == (vr_mode && !shift));  // mode=off or Shift: a plain XInput pass-through
            CHECK(rel.game_hooks == rel.hooks && dev.game_hooks);
            // A refused exe: release hooks nothing; development keeps only the harness's own hooks.
            HarnessPlan rel_bad = plan_harness({false, false, vr_mode, shift, false});
            CHECK(!rel_bad.hooks && !rel_bad.game_hooks && !rel_bad.vr);
            HarnessPlan dev_bad = plan_harness({true, false, vr_mode, shift, false});
            CHECK(dev_bad.dev && dev_bad.hooks && !dev_bad.game_hooks && !dev_bad.vr && !dev_bad.command_file);
            CHECK(dev_bad.redirect_save && dev_bad.minimized && dev_bad.mute);
        }
}

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Release install, first start: hovr\hovr.ini is written only when it is missing (src/vr/settings_file.h).
static void test_default_settings_written_only_when_missing() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("hovr_settings_test_" + std::to_string(fs::file_time_type::clock::now().time_since_epoch().count()));
    const fs::path game = root / "game";
    const fs::path hovr = game / "hovr";  // missing, as in a fresh install: created by the write
    std::string error;
    fs::create_directories(game);

    // The built-in text is tools/hovr.default.ini: mode=openvr in [vr], with its comments.
    std::string v;
    CHECK(ini_text_value(kDefaultIni, "vr", "mode", &v) && v == "openvr");
    CHECK(ini_text_value(kDefaultIni, "VR", "Eye_Scale", &v) && v == "1.0");
    CHECK(ini_text_value(kDefaultIni, "vr", "hand", &v) && v == "right");
    CHECK(!ini_text_value(kDefaultIni, "vr", "dev", &v));     // never the development harness
    CHECK(!ini_text_value(kDefaultIni, "probe", "mode", &v));
    CHECK(std::string(kDefaultIni).find("; ") != std::string::npos);

    // Missing: the folder and the file are created; CRLF line ends, otherwise the text as is.
    CHECK(write_settings_if_missing(hovr.wstring(), L"hovr.ini", kDefaultIni, &error) == SettingsWrite::written);
    std::string crlf;
    for (const char* c = kDefaultIni; *c; ++c) {
        if (*c == '\n') crlf += '\r';
        crlf += *c;
    }
    CHECK(read_file(hovr / "hovr.ini") == crlf);

    // Present (the player's own settings): never replaced, never touched.
    const std::string mine = "[vr]\r\nmode=off\r\nhand=left\r\n";
    {
        std::ofstream(hovr / "hovr.ini", std::ios::binary | std::ios::trunc) << mine;
    }
    CHECK(write_settings_if_missing(hovr.wstring(), L"hovr.ini", kDefaultIni, &error) == SettingsWrite::kept);
    CHECK(read_file(hovr / "hovr.ini") == mine);
    // An empty file is still the player's file.
    {
        std::ofstream(hovr / "hovr.ini", std::ios::binary | std::ios::trunc);
    }
    CHECK(write_settings_if_missing(hovr.wstring(), L"hovr.ini", kDefaultIni, &error) == SettingsWrite::kept);
    CHECK(fs::file_size(hovr / "hovr.ini") == 0);

    // Cannot be written (hovr is a file, not a folder): failed with a reason, nothing created.
    const fs::path blocked = root / "blocked";
    fs::create_directories(blocked);
    {
        std::ofstream(blocked / "hovr", std::ios::binary) << "x";
    }
    error.clear();
    CHECK(write_settings_if_missing((blocked / "hovr").wstring(), L"hovr.ini", kDefaultIni, &error) == SettingsWrite::failed);
    CHECK(!error.empty());
    CHECK(read_file(blocked / "hovr") == "x");

    // The parser: comments, other sections, spaces around '=' and the value.
    const char* text = "; mode=sim\n[probe]\nmode=sim\n[vr]\r\n  # x=1\r\n Mode = off \r\nsim_hand = 1, 2, 3\n[other]\nhand=left\n";
    CHECK(ini_text_value(text, "vr", "mode", &v) && v == "off");
    CHECK(ini_text_value(text, "vr", "sim_hand", &v) && v == "1, 2, 3");
    CHECK(!ini_text_value(text, "vr", "hand", &v));
    CHECK(!ini_text_value(text, "vr", "x", &v));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// --- rail camera comfort (src/vr/comfort.h) ---

static float deg(float rad) { return rad * 180 / kPi; }
static float rad(float d) { return d * kPi / 180; }

// A camera rotation (left-handed, +y up): yaw right, pitch up, then roll (right side down).
static Mat4 cam_rot(float yaw_deg, float pitch_deg, float roll_deg) {
    Mat4 rz = identity();
    const float c = std::cos(rad(roll_deg)), s = std::sin(rad(roll_deg));
    // Roll about +z (forward): +x (right) towards -y (down).
    rz.m[0][0] = c, rz.m[0][1] = s, rz.m[1][0] = -s, rz.m[1][1] = c;
    return mul(rotation_yaw_pitch(rad(yaw_deg), rad(pitch_deg)), rz);
}

static Mat4 cam_pose(float yaw_deg, float pitch_deg, float roll_deg, float x, float y, float z) {
    return mul(translation(x, y, z), cam_rot(yaw_deg, pitch_deg, roll_deg));
}

static RailComfortSettings plain_settings() {
    RailComfortSettings s;
    s.smooth_ms = 0;
    s.level = 0;
    s.vignette = 0;
    s.cone_deg = 90;
    return s;
}

static void test_comfort_quaternions() {
    const Mat4 r = cam_rot(37, -12, 8);
    const Mat4 back = rotation_from_quat(quat_from_rotation(r));
    CHECK(max_abs_diff(r, back) < 1e-5f);
    CHECK_NEAR(deg(rotation_angle(cam_rot(10, 0, 0), cam_rot(40, 0, 0))), 30, 1e-3);
    CHECK_NEAR(deg(rotation_angle(cam_rot(0, 0, 0), cam_rot(0, 0, 25))), 25, 1e-3);
    // Roll doesn't move the forward axis; yaw does.
    CHECK_NEAR(deg(forward_angle(cam_rot(0, 0, 0), cam_rot(0, 0, 25))), 0, 1e-3);
    CHECK_NEAR(deg(forward_angle(cam_rot(0, 0, 0), cam_rot(20, 0, 0))), 20, 1e-3);
    // Small per-frame angles are resolved (1 deg/s at 120 Hz).
    CHECK_NEAR(deg(rotation_angle(cam_rot(5, 0, 0), cam_rot(5 + 1.f / 120, 0, 0))), 1.0 / 120, 2e-4);
    // Slerp halfway.
    const Quat h = quat_slerp(quat_from_rotation(cam_rot(0, 0, 0)), quat_from_rotation(cam_rot(60, 0, 0)), 0.5);
    CHECK(max_abs_diff(rotation_from_quat(h), cam_rot(30, 0, 0)) < 1e-5f);
}

static void test_comfort_levelling() {
    const Mat4 r = cam_rot(30, 10, 20);
    // Level 1: forward kept, roll gone (right axis horizontal, up above the horizon), heading kept.
    const Mat4 l1 = level_rotation(r, 1);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(l1.m[i][2], r.m[i][2], 1e-5);
    CHECK_NEAR(l1.m[1][0], 0, 1e-6);
    CHECK(l1.m[1][1] > 0.9f);
    CHECK(max_abs_diff(l1, cam_rot(30, 10, 0)) < 1e-5f);
    // Level 2: yaw only.
    const Mat4 l2 = level_rotation(r, 2);
    CHECK(max_abs_diff(l2, cam_rot(30, 0, 0)) < 1e-5f);
    // Level 0 and a camera that is already level: unchanged.
    CHECK(max_abs_diff(level_rotation(r, 0), r) < 1e-7f);
    CHECK(max_abs_diff(level_rotation(cam_rot(-70, 5, 0), 1), cam_rot(-70, 5, 0)) < 1e-5f);
    // Rows stay orthonormal.
    const Mat4 p = mul(l1, rigid_inverse(l1));
    CHECK(max_abs_diff(p, identity()) < 1e-5f);
    // Straight down: no heading, left as it is.
    CHECK(max_abs_diff(level_rotation(cam_rot(10, -90, 30), 1), cam_rot(10, -90, 30)) < 1e-6f);

    // Through the filter: a rolled rail camera gives a level view (D = R_g^T R_s), yaw kept.
    RailComfortSettings s = plain_settings();
    s.level = 1;
    RailComfort rc;
    rc.update(cam_pose(30, 10, 20, 100, 200, 300), 0.01f, 1.f / 120, s);
    const Mat4 view_rot = mul(cam_rot(30, 10, 20), rc.delta());
    CHECK(max_abs_diff(view_rot, cam_rot(30, 10, 0)) < 1e-5f);
    s.level = 2;
    rc.update(cam_pose(30, 10, 20, 100, 200, 300), 0.01f, 1.f / 120, s);
    CHECK(max_abs_diff(mul(cam_rot(30, 10, 20), rc.delta()), cam_rot(30, 0, 0)) < 1e-5f);
    // Switched off: the game camera as it is.
    s.enabled = false;
    rc.update(cam_pose(30, 10, 20, 100, 200, 300), 0.01f, 1.f / 120, s);
    CHECK(max_abs_diff(rc.delta(), identity()) < 1e-6f);
}

static void test_comfort_smoothing_step() {
    RailComfortSettings s = plain_settings();
    s.smooth_ms = 150;
    RailComfort rc;
    const float dt = 1.f / 120;
    rc.update(cam_pose(0, 0, 0, 0, 0, 0), 0.01f, dt, s);
    CHECK(rc.fresh());
    CHECK(max_abs_diff(rc.delta(), identity()) < 1e-6f);  // starts on the camera
    // The rail camera turns 30 degrees in one frame (under the cut threshold) and stays there.
    // The view follows exponentially: after one time constant 1/e of the turn is left.
    int frames = 0;
    for (; frames < 18; ++frames) rc.update(cam_pose(30, 0, 0, 0, 0, 0), 0.01f, dt, s);  // 18 frames = 150 ms
    CHECK(!rc.cut());
    CHECK_NEAR(rc.lag_deg(), 30 * std::exp(-1.0), 0.05);
    // The view's orientation is the game camera's turned back by the lag, about +y.
    const Mat4 view_rot = mul(cam_rot(30, 0, 0), rc.delta());
    CHECK(max_abs_diff(view_rot, cam_rot(float(30 - 30 * std::exp(-1.0)), 0, 0)) < 1e-3f);
    for (; frames < 120; ++frames) rc.update(cam_pose(30, 0, 0, 0, 0, 0), 0.01f, dt, s);
    CHECK(rc.lag_deg() < 0.1f);
    // smooth_ms = 0: no lag at all.
    s.smooth_ms = 0;
    rc.update(cam_pose(50, 0, 0, 0, 0, 0), 0.01f, dt, s);
    CHECK(max_abs_diff(rc.delta(), identity()) < 1e-5f);
}

static void test_comfort_cut_snaps() {
    RailComfortSettings s = plain_settings();
    s.smooth_ms = 150;
    s.vignette = 1;
    const float dt = 1.f / 120;
    RailComfort rc;
    for (int i = 0; i < 60; ++i) rc.update(cam_pose(0, 0, 0, 0, 0, 0), 0.01f, dt, s);
    // A cut: 90 degrees in one frame. No lag behind it, no speed, no vignette.
    rc.update(cam_pose(90, 0, 0, 0, 0, 0), 0.01f, dt, s);
    CHECK(rc.cut());
    CHECK(max_abs_diff(rc.delta(), identity()) < 1e-5f);
    CHECK_NEAR(rc.turn_dps(), 0, 1e-6);
    for (int i = 0; i < 60; ++i) {
        rc.update(cam_pose(90, 0, 0, 0, 0, 0), 0.01f, dt, s);
        CHECK(rc.vignette() == 0);
    }
    // A jump of 2 m (200 units) in one frame: a cut too.
    rc.update(cam_pose(90, 0, 0, 200, 0, 0), 0.01f, dt, s);
    CHECK(rc.cut());
    CHECK_NEAR(rc.move_mps(), 0, 1e-6);
    // 20 cm in a frame (24 m/s) is motion, not a cut.
    rc.update(cam_pose(90, 0, 0, 220, 0, 0), 0.01f, dt, s);
    CHECK(!rc.cut());
    CHECK(rc.move_mps() > 1);
    // A gap (flat frames between) starts afresh: on the camera, no speed, no vignette.
    rc.update(cam_pose(0, 0, 0, 0, 0, 0), 0.01f, 0.5f, s);
    CHECK(rc.fresh());
    CHECK(max_abs_diff(rc.delta(), identity()) < 1e-5f);
    CHECK(rc.vignette() == 0);
}

static void test_comfort_lag_cap() {
    RailComfortSettings s = plain_settings();
    s.smooth_ms = 150;
    s.cone_deg = 35;
    const float dt = 1.f / 120;
    RailComfort rc;
    // A steady 200 deg/s turn: the exponential lag would settle at 200 * 0.15 = 30 degrees; it is
    // held at cone_deg / 2.
    float yaw = 0;
    bool capped = false;
    for (int i = 0; i < 120; ++i) {
        yaw += 200 * dt;
        rc.update(cam_pose(yaw, 0, 0, 0, 0, 0), 0.01f, dt, s);
        CHECK(rc.lag_deg() <= 17.5f + 0.01f);
        capped = capped || rc.capped();
    }
    CHECK(capped);
    CHECK_NEAR(rc.lag_deg(), 17.5, 0.05);
    // A slow 60 deg/s turn stays under the cap: lag ~ 60 * 0.15 = 9 degrees.
    RailComfort slow;
    yaw = 0;
    for (int i = 0; i < 240; ++i) {
        yaw += 60 * dt;
        slow.update(cam_pose(yaw, 0, 0, 0, 0, 0), 0.01f, dt, s);
    }
    CHECK(!slow.capped());
    CHECK_NEAR(slow.lag_deg(), 8.75, 0.2);  // discrete steps: 0.5 deg/frame * a / (1 - a), a = exp(-dt/tau)
    // Its speed reads 60 deg/s.
    CHECK_NEAR(slow.turn_dps(), 60, 0.5);
    // A rolled, pitched camera whipping round at 400 deg/s: capped, and the view stays level.
    s.level = 1;
    RailComfort rolled;
    yaw = 0;
    for (int i = 0; i < 60; ++i) {
        yaw += 400 * dt;
        rolled.update(cam_pose(yaw, -15, 10, 0, 0, 0), 0.01f, dt, s);
        const Mat4 v = mul(cam_rot(yaw, -15, 10), rolled.delta());
        CHECK_NEAR(v.m[1][0], 0, 1e-4);  // right axis horizontal
        CHECK(rolled.lag_deg() <= 17.5f + 0.01f);
    }
    CHECK(rolled.capped());
}

static void test_comfort_vignette_mapping() {
    RailComfortSettings s;
    s.vignette = 0.6f;
    CHECK_NEAR(vignette_target(s, 0, 0), 0, 0);
    CHECK_NEAR(vignette_target(s, 30, 2), 0, 1e-6);
    CHECK_NEAR(vignette_target(s, 75, 0), 0.3, 1e-5);   // halfway on the turn ramp
    CHECK_NEAR(vignette_target(s, 0, 4), 0.3, 1e-5);    // halfway on the move ramp
    CHECK_NEAR(vignette_target(s, 75, 6), 0.6, 1e-5);   // the larger of the two
    CHECK_NEAR(vignette_target(s, 500, 0), 0.6, 1e-6);  // capped at the strength
    s.vignette = 0;
    CHECK_NEAR(vignette_target(s, 500, 50), 0, 0);
    // The shape: clear inside, opacity e outside, monotonic between; stronger = smaller clear area.
    CHECK_NEAR(vignette_alpha(0.6f, 10), 0, 0);
    CHECK_NEAR(vignette_alpha(0.6f, vignette_inner_deg(0.6f)), 0, 1e-6);
    CHECK_NEAR(vignette_alpha(0.6f, vignette_inner_deg(0.6f) + kVignetteFalloffDeg), 0.96, 1e-5);
    CHECK_NEAR(vignette_alpha(0.6f, 85), 0.96, 1e-5);
    CHECK_NEAR(vignette_inner_deg(0.6f), 35, 1e-4);
    CHECK_NEAR(vignette_alpha(1, 40), 1, 1e-6);   // full strength: black beyond 40 degrees
    CHECK_NEAR(vignette_alpha(0.3f, 85), 0.48, 1e-5);
    // At the default strength the eye's edges (45-52 degrees) are clearly dark, the middle clear.
    CHECK(vignette_alpha(0.6f, 45) > 0.6f);
    CHECK_NEAR(vignette_alpha(0.6f, 30), 0, 0);
    float last = 0;
    for (float a = 0; a <= 90; a += 1) {
        const float v = vignette_alpha(0.6f, a);
        CHECK(v >= last - 1e-7f);
        last = v;
    }
    CHECK(vignette_inner_deg(1) < vignette_inner_deg(0.3f));
    CHECK_NEAR(vignette_alpha(0, 80), 0, 0);
    // Through the filter: a still camera, then a fast turn: the vignette eases in over ease_ms,
    // and out again when the camera stops.
    RailComfortSettings f = plain_settings();
    f.vignette = 0.6f;
    f.ease_ms = 200;
    RailComfort rc;
    const float dt = 1.f / 120;
    for (int i = 0; i < 30; ++i) rc.update(cam_pose(0, 0, 0, 0, 0, 0), 0.01f, dt, f);
    CHECK(rc.vignette() == 0);
    float yaw = 0;
    for (int i = 0; i < 12; ++i) {  // 100 ms at 180 deg/s
        yaw += 180 * dt;
        rc.update(cam_pose(yaw, 0, 0, 0, 0, 0), 0.01f, dt, f);
    }
    CHECK(rc.vignette() > 0.1f && rc.vignette() < 0.6f);  // on its way in, eased
    for (int i = 0; i < 60; ++i) {
        yaw += 180 * dt;
        rc.update(cam_pose(yaw, 0, 0, 0, 0, 0), 0.01f, dt, f);
    }
    CHECK_NEAR(rc.vignette(), 0.6, 1e-4);
    CHECK_NEAR(rc.turn_dps(), 180, 1);
    for (int i = 0; i < 60; ++i) rc.update(cam_pose(yaw, 0, 0, 0, 0, 0), 0.01f, dt, f);
    CHECK_NEAR(rc.vignette(), 0, 1e-6);
    // Fast forward motion (5 m/s) at 120 Hz.
    float z = 0;
    for (int i = 0; i < 120; ++i) {
        z += 500 * dt;
        rc.update(cam_pose(yaw, 0, 0, 0, 0, z), 0.01f, dt, f);
    }
    CHECK_NEAR(rc.move_mps(), 5, 0.01);
    CHECK_NEAR(rc.vignette(), 0.6f * 0.75f, 1e-3);
}

// The aim stays exact under smoothing and levelling: the world point the gun's ray reaches is
// where the eyes see the barrel pointing - the same eye-relative position as the tracked barrel.
static void test_comfort_aim_matches_eyes() {
    RailComfortSettings s = plain_settings();
    s.smooth_ms = 150;
    s.level = 1;
    s.cone_deg = 35;
    RailComfort rc;
    const float dt = 1.f / 120;
    float yaw = 10;
    for (int i = 0; i < 40; ++i) {  // a rolled, pitched camera turning at 90 deg/s: D is far from identity
        yaw += 90 * dt;
        rc.update(cam_pose(yaw, -8, 12, 2766, 160, 6676), 0.01f, dt, s);
    }
    const Mat4 d = rc.delta();
    CHECK(rotation_angle(d, identity()) > rad(10));
    Mat4 view;
    CHECK(invert(cam_pose(yaw, -8, 12, 2766, 160, 6676), &view));
    // Tracked poses (right-handed, relative to the anchor): the head and the gun hand.
    const Mat4 anchor = translation(0.3f, 1.6f, -0.2f);
    const Mat4 head_rh = mul(anchor, mul(translation(0.05f, 0.02f, 0.03f), rh_yaw_pitch(rad(15), rad(-5))));
    const Mat4 hand_rh = mul(anchor, mul(translation(0.25f, -0.3f, -0.35f), rh_yaw_pitch(rad(-20), rad(8))));
    const float ipd = 0.064f;
    for (int eye = 0; eye < 2; ++eye) {
        const Mat4 eye_to_head = translation(eye ? ipd / 2 : -ipd / 2, 0, 0);
        // As the mod does it (vrsys.cpp): D on top of the eye pose and on top of the hand pose.
        const Mat4 eye_cam = mul(d, eye_in_game(head_in_game(anchor, head_rh), eye_to_head));
        const Mat4 hand_cam = mul(d, pose_in_game(anchor, hand_rh));
        Ray w;
        CHECK(camera_ray_to_world(view, pose_ray_lh(hand_cam), 0.01f, &w));
        const float p[3] = {w.o[0] + 500 * w.d[0], w.o[1] + 500 * w.d[1], w.o[2] + 500 * w.d[2]};  // 5 m out
        // The eye sees that world point at (eye view of the camera-space point) ...
        const Mat4 to_eye = mul(eye_view(eye_cam, 0.01f), view);
        const Vec4 seen = apply(to_eye, {p[0], p[1], p[2], 1});
        // ... which must be where the tracked barrel's point 5 m out is relative to the tracked eye.
        const Ray r = pose_ray_rh(hand_rh);
        const float q[3] = {r.o[0] + 5 * r.d[0], r.o[1] + 5 * r.d[1], r.o[2] + 5 * r.d[2]};
        const Mat4 eye_rh = mul(head_rh, eye_to_head);
        const Vec4 e = apply(rigid_inverse(eye_rh), {q[0], q[1], q[2], 1});
        // Eye space: game units, left-handed (z forward); tracked: metres, right-handed.
        CHECK_NEAR(seen.x * 0.01f, e.x, 1e-3);
        CHECK_NEAR(seen.y * 0.01f, e.y, 1e-3);
        CHECK_NEAR(seen.z * 0.01f, -e.z, 1e-3);
        // Sanity: had the hand skipped D, the shot would land elsewhere.
        Ray w2;
        CHECK(camera_ray_to_world(view, pose_ray_lh(pose_in_game(anchor, hand_rh)), 0.01f, &w2));
        const float p2[3] = {w2.o[0] + 500 * w2.d[0], w2.o[1] + 500 * w2.d[1], w2.o[2] + 500 * w2.d[2]};
        const Vec4 seen2 = apply(to_eye, {p2[0], p2[1], p2[2], 1});
        CHECK(std::fabs(seen2.x * 0.01f - e.x) + std::fabs(seen2.y * 0.01f - e.y) > 0.3f);
    }
}

// The mesh check's bore fit (src/vr/gunfit.h) on a synthetic gun: a barrel (outer circle r 1.2,
// bore r 0.45, chamfer rings at the muzzle) along model +x from x = -2 to 10, a front sight block on
// top at the muzzle, a box frame behind, and a revolver cylinder (round, but 2.2 below the bore);
// the whole mesh turned by yaw 3 / pitch -2 degrees and moved. The hint is plain +x.
static void test_bore_fit() {
    std::vector<float> v;  // x, y, z per vertex, model space before the pose
    auto ring = [&](float x, float cy, float r, int segs, float phase) {
        for (int k = 0; k < segs; ++k) {
            const float a = phase + 2 * kPi * float(k) / float(segs);
            v.insert(v.end(), {x, cy + r * std::cos(a), r * std::sin(a)});
            v.insert(v.end(), {x, cy + r * std::cos(a), r * std::sin(a)});  // duplicate (another normal)
        }
    };
    for (float x : {-2.f, 2.f, 6.f, 9.6f, 10.f}) ring(x, 0, 1.2f, 16, 0.1f);
    ring(10.f, 0, 0.45f, 8, 0.3f);   // the bore at the muzzle face
    ring(9.8f, 0, 0.6f, 12, 0.0f);   // chamfer
    ring(4.f, 0, 0.45f, 8, 0.3f);    // the bore further in
    for (float x : {-1.f, -4.f}) ring(x, -2.2f, 1.5f, 12, 0);  // cylinder: round, off the bore axis
    for (float x : {9.0f, 10.0f})  // front sight block
        for (float y : {1.15f, 1.9f})
            for (float z : {-0.15f, 0.15f}) v.insert(v.end(), {x, y, z});
    for (float x : {-8.f, -3.f})  // frame
        for (float y : {-4.f, 1.f})
            for (float z : {-1.f, 1.f}) v.insert(v.end(), {x, y, z});
    const float yaw = 3 * kPi / 180, pitch = -2 * kPi / 180, off[3] = {23.f, 8.f, -0.9f};
    // Pose: pitch about z (x towards y), then yaw about y (x towards -z).
    auto pose = [&](const float p[3], float out[3]) {
        const float x1 = p[0] * std::cos(pitch) - p[1] * std::sin(pitch), y1 = p[0] * std::sin(pitch) + p[1] * std::cos(pitch), z1 = p[2];
        out[0] = x1 * std::cos(yaw) + z1 * std::sin(yaw) + off[0];
        out[1] = y1 + off[1];
        out[2] = -x1 * std::sin(yaw) + z1 * std::cos(yaw) + off[2];
    };
    std::vector<float> posed(v.size());
    for (size_t i = 0; i < v.size(); i += 3) pose(&v[i], &posed[i]);
    const float hint[3] = {1, 0, 0};
    const BoreFit f = fit_bore(posed.data(), posed.size() / 3, 3, hint);
    CHECK(f.axis_ok);
    CHECK(f.muzzle_ok);
    float true_dir[3], o[3], m[3];
    const float zero[3] = {0, 0, 0}, ten[3] = {10, 0, 0};
    pose(ten, m), pose(zero, o);
    for (int i = 0; i < 3; ++i) true_dir[i] = (m[i] - o[i]) / 10;
    float ang, perp, along;
    compare_bore(f, true_dir, m, &ang, &perp, &along);
    CHECK_NEAR(ang, 0, 0.01);
    CHECK_NEAR(perp, 0, 0.01);
    CHECK_NEAR(along, 0, 0.01);
    CHECK_NEAR(f.radius, 1.2, 0.01);
    CHECK(f.rings >= 4);
    CHECK(f.rings_round > f.rings);  // the cylinder's rings were round but dropped
    // A description 1 degree off and with its muzzle 0.5 up is reported as such.
    float side[3] = {true_dir[1], -true_dir[0], 0};  // across the barrel (true_dir x +z)
    const float sn = std::sqrt(side[0] * side[0] + side[1] * side[1]);
    float d1[3];
    for (int i = 0; i < 3; ++i) d1[i] = std::cos(kPi / 180) * true_dir[i] + std::sin(kPi / 180) * side[i] / sn;
    const float m1[3] = {m[0] + 0.5f * side[0] / sn, m[1] + 0.5f * side[1] / sn, m[2]};
    compare_bore(f, d1, m1, &ang, &perp, &along);
    CHECK_NEAR(ang, 1, 0.01);
    CHECK_NEAR(perp, 0.5, 0.01);
    CHECK_NEAR(along, 0, 0.01);
    CHECK(std::string(f.method) == "rings");
    // Nothing round: no axis, and a reason.
    const float flat[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1};
    const BoreFit g = fit_bore(flat, 6, 3, hint);
    CHECK(!g.axis_ok && !g.muzzle_ok && g.why[0]);

    // A slide pistol (as the magnum): the only round part is the bore's ring at the muzzle face
    // (r 0.7 around y 9.4); the slide is a box with its long edges along the barrel, here turned
    // 1 degree up. The axis comes from the edges, through the ring's centre.
    {
        std::vector<float> s;
        const float pitch1 = kPi / 180;
        auto put = [&](float x, float y, float z) {  // turned about the bore's centre at the muzzle face
            const float dx = x - 23.4f, dy = y - 9.4f;
            s.insert(s.end(), {23.4f + dx * std::cos(pitch1) - dy * std::sin(pitch1), 9.4f + dx * std::sin(pitch1) + dy * std::cos(pitch1), z});
        };
        for (int k = 0; k < 8; ++k) put(23.4f, 9.4f + 0.7f * std::cos(2 * kPi * k / 8), -0.87f + 0.7f * std::sin(2 * kPi * k / 8));
        for (float x : {10.f, 13.f, 16.f, 19.f, 23.4f})
            for (float y : {7.6f, 10.4f, 11.5f})
                for (float z : {-2.4f, 0.7f}) put(x, y, z);
        const BoreFit e = fit_bore(s.data(), s.size() / 3, 3, hint);
        CHECK(e.axis_ok);
        CHECK(std::string(e.method) == "edges");
        CHECK_NEAR(std::atan2(e.dir[1], e.dir[0]) * 180 / kPi, 1.0, 0.01);
        const float d0[3] = {1, 0, 0}, m0[3] = {23.4f, 8.4f, -0.87f};
        compare_bore(e, d0, m0, &ang, &perp, &along);
        CHECK_NEAR(ang, 1.0, 0.01);
        CHECK_NEAR(perp, 1.0, 0.01);  // the description's muzzle 1 below the bore
    }
    // An SMG-like barrel (r 0.68 at x 24.5 and 28.3, axis y 11.5) with a nut (r 1.85) at x 24.5
    // whose centre is 0.1 higher: the nut must not tilt the axis.
    {
        std::vector<float> s;
        auto circle = [&](float x, float cy, float r, int segs) {
            for (int k = 0; k < segs; ++k) s.insert(s.end(), {x, cy + r * std::cos(2 * kPi * k / segs), -0.72f + r * std::sin(2 * kPi * k / segs)});
        };
        circle(24.48f, 11.5f, 0.68f, 8);
        circle(28.33f, 11.5f, 0.68f, 8);
        circle(24.51f, 11.6f, 1.85f, 12);
        const BoreFit e = fit_bore(s.data(), s.size() / 3, 3, hint);
        CHECK(e.axis_ok);
        CHECK(std::string(e.method) == "rings");
        const float d0[3] = {1, 0, 0}, m0[3] = {28.33f, 11.5f, -0.72f};
        compare_bore(e, d0, m0, &ang, &perp, &along);
        CHECK_NEAR(ang, 0, 0.01);
        CHECK_NEAR(perp, 0, 0.01);
        CHECK_NEAR(along, 0, 0.01);
    }
}

int main() {
    test_bore_fit();
    test_comfort_quaternions();
    test_comfort_levelling();
    test_comfort_smoothing_step();
    test_comfort_cut_snaps();
    test_comfort_lag_cap();
    test_comfort_vignette_mapping();
    test_comfort_aim_matches_eyes();
    test_harness_plan();
    test_default_settings_written_only_when_missing();
    test_model_to_gun();
    test_gun_in_hand_holds_grip_and_barrel();
    test_drawn_barrel_is_the_shot_ray();
    test_clip_out_fraction();
    test_use_drawn_pose();
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
    test_stereo_target_real_size();
    test_composite_game_size();
    test_rigid_inverse();
    test_rh_yaw_pitch_forward();
    test_hand_adjust();
    test_hand_ray_to_world();
    test_ray_panel_uv();
    test_cursor_on_line();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all stereo, xform, aim, comfort, harness and settings-file tests passed\n");
    return 0;
}
