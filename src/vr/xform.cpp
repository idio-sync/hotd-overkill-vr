#include "xform.h"

#include <cmath>

namespace hv::vr {

Mat4 from_rows34(const float m[3][4]) {
    Mat4 r = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = m[i][j];
    return r;
}

void to_rows34(const Mat4& p, float out[3][4]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = p.m[i][j];
}

Mat4 flip_z(const Mat4& p) {
    // S p S with S = diag(1, 1, -1, 1): negate row 2 and column 2 (the corner stays).
    Mat4 r = p;
    for (int j = 0; j < 4; ++j) r.m[2][j] = -r.m[2][j];
    for (int i = 0; i < 4; ++i) r.m[i][2] = -r.m[i][2];
    return r;
}

Mat4 yaw_only_rh(const Mat4& p) {
    // Forward is -z: the third column of R, negated. Its heading in the x-z plane.
    const float fx = -p.m[0][2], fz = -p.m[2][2];
    const float n = std::sqrt(fx * fx + fz * fz);
    Mat4 r = identity();
    if (n > 1e-6f) {
        // R_y(a) maps -z to (-sin a, 0, -cos a); match (fx, fz) / n.
        const float s = -fx / n, c = -fz / n;
        r.m[0][0] = c, r.m[0][2] = s;
        r.m[2][0] = -s, r.m[2][2] = c;
    }
    for (int i = 0; i < 3; ++i) r.m[i][3] = p.m[i][3];
    return r;
}

Mat4 pose_in_game(const Mat4& anchor_rh, const Mat4& pose_rh) { return flip_z(mul(rigid_inverse(anchor_rh), pose_rh)); }

Mat4 eye_in_game(const Mat4& head_lh, const Mat4& eye_to_head_rh) { return mul(head_lh, flip_z(eye_to_head_rh)); }

bool tangents_from_openvr(const float p[4][4], float out[4]) {
    // Direction (T, 0, -1): clip x = p00 T - p02, w = -p32 (p32 = -1 -> w = 1).
    if (std::fabs(p[3][2] + 1) > 1e-3f || p[0][0] == 0 || p[1][1] == 0) return false;
    out[0] = (-1 + p[0][2]) / p[0][0];
    out[1] = (1 + p[0][2]) / p[0][0];
    out[2] = (-1 + p[1][2]) / p[1][1];
    out[3] = (1 + p[1][2]) / p[1][1];
    return out[0] < out[1] && out[2] < out[3];
}

bool project_panel(const Mat4& eye_rh, const Mat4& pose_rh, float width, float height, const Proj& eye, float out[4][3]) {
    // Panel corners in its own space (OpenVR: x right, y up), in texture order.
    const float hw = width / 2, hh = height / 2;
    const float corners[4][2] = {{-hw, hh}, {hw, hh}, {hw, -hh}, {-hw, -hh}};
    // Panel space -> eye space (right-handed) -> game camera convention (left-handed).
    const Mat4 to_eye = flip_z(mul(rigid_inverse(eye_rh), pose_rh));
    const Mat4 clip = mul(proj_matrix(eye), to_eye);
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        const Vec4 c = apply(clip, {corners[i][0], corners[i][1], 0, 1});
        out[i][0] = c.x, out[i][1] = c.y, out[i][2] = c.w;
        if (c.w <= 1e-4f) ok = false;
    }
    return ok;
}

Mat4 panel_pose(const Mat4& anchor_rh, float distance, float drop) { return mul(anchor_rh, translation(0, -drop, -distance)); }

}  // namespace hv::vr
