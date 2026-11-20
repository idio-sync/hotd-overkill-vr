#include "aim.h"

#include <cmath>

#include "xform.h"

namespace hv::vr {

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void normalize3(float v[3]) {
    const float n = std::sqrt(dot3(v, v));
    if (n > 0)
        for (int i = 0; i < 3; ++i) v[i] /= n;
}

Mat4 rh_yaw_pitch(float yaw, float pitch) { return flip_z(rotation_yaw_pitch(yaw, pitch)); }

Mat4 hand_adjust(const float offset_m[3], float pitch_deg) {
    return mul(translation(offset_m[0], offset_m[1], offset_m[2]), rh_yaw_pitch(0, pitch_deg * 3.14159265f / 180));
}

Ray pose_ray_lh(const Mat4& p) {
    Ray r{};
    for (int i = 0; i < 3; ++i) r.o[i] = p.m[i][3], r.d[i] = p.m[i][2];
    normalize3(r.d);
    return r;
}

Ray pose_ray_rh(const Mat4& p) {
    Ray r{};
    for (int i = 0; i < 3; ++i) r.o[i] = p.m[i][3], r.d[i] = -p.m[i][2];
    normalize3(r.d);
    return r;
}

bool camera_ray_to_world(const Mat4& view, const Ray& cam, float unit_m, Ray* world) {
    Mat4 cw;
    if (unit_m <= 0 || !invert(view, &cw)) return false;
    const Vec4 o = apply(cw, {cam.o[0] / unit_m, cam.o[1] / unit_m, cam.o[2] / unit_m, 1});
    const Vec4 d = apply(cw, {cam.d[0], cam.d[1], cam.d[2], 0});
    if (std::fabs(o.w) < 1e-12f) return false;
    world->o[0] = o.x / o.w, world->o[1] = o.y / o.w, world->o[2] = o.z / o.w;
    world->d[0] = d.x, world->d[1] = d.y, world->d[2] = d.z;
    normalize3(world->d);
    return true;
}

bool world_point_to_camera(const Mat4& view, const float p[3], float unit_m, float out[3]) {
    const Vec4 c = apply(view, {p[0], p[1], p[2], 1});
    if (std::fabs(c.w) < 1e-12f) return false;
    out[0] = c.x / c.w * unit_m, out[1] = c.y / c.w * unit_m, out[2] = c.z / c.w * unit_m;
    return true;
}

bool ray_panel_uv(const Mat4& panel, float width, float height, const Ray& ray, float* u, float* v) {
    const Mat4 inv = rigid_inverse(panel);
    const Vec4 o = apply(inv, {ray.o[0], ray.o[1], ray.o[2], 1});
    const Vec4 d = apply(inv, {ray.d[0], ray.d[1], ray.d[2], 0});
    if (o.z <= 0 || d.z >= -1e-6f) return false;  // behind the panel, or not pointing at its plane
    const float t = -o.z / d.z;
    const float x = o.x + t * d.x, y = o.y + t * d.y;
    *u = x / width + 0.5f;
    *v = 0.5f - y / height;
    return true;
}

void panel_point(const Mat4& panel, float width, float height, float u, float v, float out[3]) {
    const Vec4 p = apply(panel, {(u - 0.5f) * width, (0.5f - v) * height, 0, 1});
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}

bool cursor_on_line(const float base[3], const float du[3], const float dv[3], const float o[3], const float d[3], float* x,
                    float* y, float point[3], float* t) {
    const float n[3] = {du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2], du[0] * dv[1] - du[1] * dv[0]};
    const float nn = std::sqrt(dot3(n, n)), dl = std::sqrt(dot3(d, d));
    const float denom = dot3(d, n);
    if (nn == 0 || dl == 0 || std::fabs(denom) < 1e-6f * nn * dl) return false;
    const float bo[3] = {base[0] - o[0], base[1] - o[1], base[2] - o[2]};
    *t = dot3(bo, n) / denom;
    float r[3];
    for (int i = 0; i < 3; ++i) point[i] = o[i] + *t * d[i], r[i] = point[i] - base[i];
    // r = x du + y dv: least squares (exact, r lies in the plane).
    const double a = dot3(du, du), b = dot3(du, dv), c = dot3(dv, dv), p = dot3(r, du), q = dot3(r, dv);
    const double det = a * c - b * b;
    if (std::fabs(det) < 1e-20) return false;
    *x = float((p * c - q * b) / det);
    *y = float((q * a - p * b) / det);
    return true;
}

void yaw_pitch_towards(const Mat4& frame, const float dir[3], float* yaw, float* pitch) {
    // The direction in the frame's own axes (rotation part transposed).
    float l[3];
    for (int i = 0; i < 3; ++i) l[i] = frame.m[0][i] * dir[0] + frame.m[1][i] * dir[1] + frame.m[2][i] * dir[2];
    normalize3(l);
    // rh_yaw_pitch's forward: (sin y cos p, sin p, -cos y cos p).
    *pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, l[1])));
    *yaw = std::atan2(l[0], -l[2]);
}

bool use_drawn_pose(bool recorded, unsigned drawn_frame, unsigned now, unsigned max_age) {
    // Unsigned: a record from the future (it can't be) reads as very old.
    return recorded && now - drawn_frame <= max_age;
}

}  // namespace hv::vr
