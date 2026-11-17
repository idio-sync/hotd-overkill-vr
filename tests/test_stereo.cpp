// Unit tests for src/vr/stereo (no game, no D3D). Run: ctest --test-dir out
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../src/vr/stereo.h"
#include "../src/vr/aim.h"
#include "../src/vr/gunpose.h"
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
static const GunModelDesc kMagnum = {{1, 0, 0}, {0, 1, 0}, {23.4f, 8.4f, -0.87f}, {0, 1, -0.87f}, {-0.25f, -0.97f, 0}};
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

static void test_clip_out_fraction() {
    CHECK_NEAR(clip_out_fraction(false, 0.5f), 0, 0);
    CHECK_NEAR(clip_out_fraction(true, 0), 0, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 0.1f), 0.5, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 0.4f), 1, 0);
    CHECK(clip_out_fraction(true, 0.7f) > 0 && clip_out_fraction(true, 0.7f) < 1);
    CHECK_NEAR(clip_out_fraction(true, 0.85f), 0, 1e-6);
    CHECK_NEAR(clip_out_fraction(true, 1.0f), 0, 1e-6);
}

int main() {
    test_model_to_gun();
    test_gun_in_hand_holds_grip_and_barrel();
    test_drawn_barrel_is_the_shot_ray();
    test_clip_out_fraction();
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
    test_rh_yaw_pitch_forward();
    test_hand_adjust();
    test_hand_ray_to_world();
    test_ray_panel_uv();
    test_cursor_on_line();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all stereo, xform and aim tests passed\n");
    return 0;
}
