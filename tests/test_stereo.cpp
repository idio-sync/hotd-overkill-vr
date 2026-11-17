// Unit tests for src/vr/stereo (no game, no D3D). Run: ctest --test-dir out
#include <cmath>
#include <cstdio>

#include "../src/vr/stereo.h"

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

int main() {
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
    std::printf("all stereo tests passed\n");
    return 0;
}
