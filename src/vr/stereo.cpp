#include "stereo.h"

#include <cmath>
#include <utility>

namespace hv::vr {

Mat4 identity() {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += double(a.m[i][k]) * b.m[k][j];
            r.m[i][j] = float(s);
        }
    return r;
}

bool invert(const Mat4& a, Mat4* out) {
    // Gauss-Jordan with partial pivoting, in double.
    double m[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-30) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        const double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            if (f == 0) continue;
            for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out->m[i][j] = float(m[i][j + 4]);
    return true;
}

Vec4 apply(const Mat4& a, const Vec4& v) {
    const float in[4] = {v.x, v.y, v.z, v.w};
    float o[4];
    for (int i = 0; i < 4; ++i) o[i] = a.m[i][0] * in[0] + a.m[i][1] * in[1] + a.m[i][2] * in[2] + a.m[i][3] * in[3];
    return {o[0], o[1], o[2], o[3]};
}

Mat4 from_d3dx(const float rv[16]) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = rv[j * 4 + i];
    return r;
}

float max_abs_diff(const Mat4& a, const Mat4& b) {
    float d = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) d = std::fmax(d, std::fabs(a.m[i][j] - b.m[i][j]));
    return d;
}

Mat4 translation(float x, float y, float z) {
    Mat4 r = identity();
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}

Mat4 rotation_yaw_pitch(float yaw, float pitch) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    Mat4 ry = identity(), rx = identity();
    // Yaw: +z -> (sin, 0, cos), i.e. turning right in a left-handed y-up space.
    ry.m[0][0] = cy, ry.m[0][2] = sy, ry.m[2][0] = -sy, ry.m[2][2] = cy;
    // Pitch: +z -> (0, sin, cos), looking up.
    rx.m[1][1] = cp, rx.m[1][2] = sp, rx.m[2][1] = -sp, rx.m[2][2] = cp;
    return mul(ry, rx);
}

Mat4 rigid_inverse(const Mat4& p) {
    Mat4 r = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = p.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * p.m[0][3] + r.m[i][1] * p.m[1][3] + r.m[i][2] * p.m[2][3]);
    return r;
}

Mat4 proj_matrix(const Proj& p) {
    Mat4 r{};
    r.m[0][0] = p.sx, r.m[0][2] = p.cx;
    r.m[1][1] = p.sy, r.m[1][2] = p.cy;
    r.m[2][2] = p.a, r.m[2][3] = p.b;
    r.m[3][2] = 1;
    return r;
}

bool proj_from_matrix(const Mat4& m, Proj* out) {
    const float w = m.m[3][2];
    if (std::fabs(w) < 1e-12f) return false;
    const float eps = 1e-5f * std::fabs(w);
    if (std::fabs(m.m[3][0]) > eps || std::fabs(m.m[3][1]) > eps || std::fabs(m.m[3][3]) > eps) return false;
    const float s = 1 / w;
    *out = {m.m[0][0] * s, m.m[1][1] * s, m.m[0][2] * s, m.m[1][2] * s, m.m[2][2] * s, m.m[2][3] * s};
    return out->sx != 0 && out->sy != 0 && out->b != 0;
}

Proj proj_from_tangents(float l, float r, float b, float t, float a, float bz) {
    return {2 / (r - l), 2 / (t - b), -(r + l) / (r - l), -(t + b) / (t - b), a, bz};
}

Proj sim_eye_proj(const Proj& game, float eye_aspect) {
    return {game.sy / eye_aspect, game.sy, 0, 0, game.a, game.b};
}

void proj_tangents(const Proj& p, float out[4]) {
    out[0] = (-1 - p.cx) / p.sx;
    out[1] = (1 - p.cx) / p.sx;
    out[2] = (-1 - p.cy) / p.sy;
    out[3] = (1 - p.cy) / p.sy;
}

Mat4 eye_pose(const Mat4& head, float ipd_m, int eye) {
    return mul(head, translation(eye == 0 ? -ipd_m / 2 : ipd_m / 2, 0, 0));
}

static Mat4 in_units(const Mat4& pose_m, float unit_m) {
    Mat4 p = pose_m;
    for (int i = 0; i < 3; ++i) p.m[i][3] /= unit_m;
    return p;
}

Mat4 eye_view(const Mat4& eye_m, float unit_m) { return rigid_inverse(in_units(eye_m, unit_m)); }

void eye_offset_units(const Mat4& eye_m, float unit_m, float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = eye_m.m[i][3] / unit_m;
}

bool correction(const Mat4& p_game, const Proj& p_eye, const Mat4& e_view, Mat4* out) {
    Mat4 inv;
    if (!invert(p_game, &inv)) return false;
    *out = mul(proj_matrix(p_eye), mul(e_view, inv));
    return true;
}

float widened_half_angle(float eye_half, float cone, float margin) {
    const float limit = 89.0f * 3.14159265f / 180.0f;
    const float h = eye_half + cone + margin;
    return h < limit ? h : limit;
}

}  // namespace hv::vr
