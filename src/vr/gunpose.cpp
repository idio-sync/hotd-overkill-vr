#include "gunpose.h"

#include <cmath>

namespace hv::vr {

static void normalize3(float v[3]) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0)
        for (int i = 0; i < 3; ++i) v[i] /= n;
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

Mat4 model_to_gun(const GunModelDesc& d, float s) {
    float f[3] = {d.barrel[0], d.barrel[1], d.barrel[2]}, u[3] = {d.up[0], d.up[1], d.up[2]}, r[3];
    normalize3(f);
    const float uf = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
    for (int i = 0; i < 3; ++i) u[i] -= uf * f[i];
    normalize3(u);
    cross3(u, f, r);  // x = y cross z: a proper rotation in either handedness
    Mat4 m = identity();
    for (int i = 0; i < 3; ++i) m.m[0][i] = s * r[i], m.m[1][i] = s * u[i], m.m[2][i] = s * f[i];
    for (int row = 0; row < 3; ++row)
        m.m[row][3] = -(m.m[row][0] * d.muzzle[0] + m.m[row][1] * d.muzzle[1] + m.m[row][2] * d.muzzle[2]);
    return m;
}

Mat4 gun_in_hand(const GunModelDesc& d, float s, const float grip_in_hand[3], float yaw_deg, float pitch_deg) {
    const Mat4 m2g = model_to_gun(d, s);
    const Vec4 g = apply(m2g, {d.grip[0], d.grip[1], d.grip[2], 1});
    const float k = 3.14159265f / 180;
    return mul(mul(translation(grip_in_hand[0], grip_in_hand[1], grip_in_hand[2]), rotation_yaw_pitch(yaw_deg * k, pitch_deg * k)),
               translation(-g.x, -g.y, -g.z));
}

Ray barrel_ray(const Mat4& gun) { return pose_ray_lh(gun); }

bool gun_world_matrix(const Mat4& view, float unit_m, const Mat4& hand, const Mat4& g, const Mat4& m2g, Mat4* out) {
    Mat4 cam_to_world;
    if (unit_m <= 0 || !invert(view, &cam_to_world)) return false;
    Mat4 to_units = identity();
    for (int i = 0; i < 3; ++i) to_units.m[i][i] = 1 / unit_m;
    *out = mul(mul(mul(mul(cam_to_world, to_units), hand), g), m2g);
    return true;
}

static float smooth(float t) {
    t = std::fmin(1.f, std::fmax(0.f, t));
    return t * t * (3 - 2 * t);
}

float clip_out_fraction(bool reloading, float p, float since, float out0) {
    const float k = smooth(since / 0.15f);
    if (!reloading) return out0 * (1 - k);
    float o = out0 + (1 - out0) * k;
    if (p > 0.6f) o *= 1 - smooth((p - 0.6f) / 0.25f);
    return o;
}

}  // namespace hv::vr
