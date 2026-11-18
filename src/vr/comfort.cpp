#include "comfort.h"

#include <cmath>

namespace hv::vr {

static constexpr double kPiD = 3.14159265358979323846;
static constexpr float kDeg = float(180.0 / kPiD);

static Quat normalized(Quat q) {
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-12) return {1, 0, 0, 0};
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

Quat quat_from_rotation(const Mat4& r) {
    const double m00 = r.m[0][0], m01 = r.m[0][1], m02 = r.m[0][2];
    const double m10 = r.m[1][0], m11 = r.m[1][1], m12 = r.m[1][2];
    const double m20 = r.m[2][0], m21 = r.m[2][1], m22 = r.m[2][2];
    const double tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    return normalized(q);
}

Mat4 rotation_from_quat(const Quat& qi) {
    const Quat q = normalized(qi);
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    Mat4 r = identity();
    r.m[0][0] = float(1 - 2 * (y * y + z * z)), r.m[0][1] = float(2 * (x * y - w * z)), r.m[0][2] = float(2 * (x * z + w * y));
    r.m[1][0] = float(2 * (x * y + w * z)), r.m[1][1] = float(1 - 2 * (x * x + z * z)), r.m[1][2] = float(2 * (y * z - w * x));
    r.m[2][0] = float(2 * (x * z - w * y)), r.m[2][1] = float(2 * (y * z + w * x)), r.m[2][2] = float(1 - 2 * (x * x + y * y));
    return r;
}

Quat quat_slerp(const Quat& a, const Quat& bi, double t) {
    Quat b = bi;
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) b = {-b.w, -b.x, -b.y, -b.z}, d = -d;
    if (d > 0.99995) {
        return normalized({a.w + t * (b.w - a.w), a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)});
    }
    const double th = std::acos(d), s = std::sin(th);
    const double ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
    return normalized({ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z});
}

double quat_angle(const Quat& a, const Quat& b) {
    // conj(a) * b
    const double w = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    const double x = a.w * b.x - b.w * a.x - (a.y * b.z - a.z * b.y);
    const double y = a.w * b.y - b.w * a.y - (a.z * b.x - a.x * b.z);
    const double z = a.w * b.z - b.w * a.z - (a.x * b.y - a.y * b.x);
    return 2 * std::atan2(std::sqrt(x * x + y * y + z * z), std::fabs(w));
}

float rotation_angle(const Mat4& a, const Mat4& b) { return float(quat_angle(quat_from_rotation(a), quat_from_rotation(b))); }

float forward_angle(const Mat4& a, const Mat4& b) {
    const double ax = a.m[0][2], ay = a.m[1][2], az = a.m[2][2], bx = b.m[0][2], by = b.m[1][2], bz = b.m[2][2];
    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    return float(std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz));
}

bool camera_pose_from_view(const Mat4& view, Mat4* cam_world) { return invert(view, cam_world); }

Mat4 level_rotation(const Mat4& r, int level) {
    if (level <= 0) return r;
    double fx = r.m[0][2], fy = r.m[1][2], fz = r.m[2][2];
    const double f = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (f < 1e-9) return r;
    fx /= f, fy /= f, fz /= f;
    const double h = std::sqrt(fx * fx + fz * fz);
    if (h < 0.0087) return r;  // looking (almost) straight up or down: no heading to keep
    // right = up x forward (horizontal), up' = forward x right.
    double rx = fz / h, rz = -fx / h;
    double ux, uy, uz;
    if (level >= 2) {
        fx /= h, fz /= h, fy = 0;
        ux = 0, uy = 1, uz = 0;
    } else {
        ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;
    }
    Mat4 o = r;
    o.m[0][0] = float(rx), o.m[1][0] = 0, o.m[2][0] = float(rz);
    o.m[0][1] = float(ux), o.m[1][1] = float(uy), o.m[2][1] = float(uz);
    o.m[0][2] = float(fx), o.m[1][2] = float(fy), o.m[2][2] = float(fz);
    return o;
}

float ramp(float v, float a, float b) {
    if (b <= a) return v >= a ? 1.f : 0.f;
    const float t = (v - a) / (b - a);
    return t < 0 ? 0.f : t > 1 ? 1.f : t;
}

float vignette_target(const RailComfortSettings& s, float turn_dps, float move_mps) {
    const float k = std::fmax(ramp(turn_dps, s.turn_dps[0], s.turn_dps[1]), ramp(move_mps, s.move_mps[0], s.move_mps[1]));
    const float v = s.vignette < 0 ? 0.f : s.vignette > 1 ? 1.f : s.vignette;
    return v * k;
}

float vignette_inner_deg(float e) {
    e = e < 0 ? 0.f : e > 1 ? 1.f : e;
    return 50.f - 25.f * e;
}

float vignette_max_alpha(float e) {
    const float a = 1.6f * e;
    return a < 0 ? 0.f : a > 1 ? 1.f : a;
}

float vignette_alpha(float e, float angle_deg) {
    if (e <= 0) return 0;
    if (e > 1) e = 1;
    const float t = ramp(angle_deg, vignette_inner_deg(e), vignette_inner_deg(e) + kVignetteFalloffDeg);
    return vignette_max_alpha(e) * t * t * (3 - 2 * t);
}

void RailComfort::update(const Mat4& cam_world, float unit_m, float dt, const RailComfortSettings& s) {
    const Quat qg = quat_from_rotation(cam_world);
    const Mat4 rg = rotation_from_quat(qg);
    const float p[3] = {cam_world.m[0][3] * unit_m, cam_world.m[1][3] * unit_m, cam_world.m[2][3] * unit_m};
    const Quat target = quat_from_rotation(level_rotation(rg, s.enabled ? s.level : 0));
    const Quat q_prev = quat_from_rotation(prev_);
    fresh_ = !have_ || !(dt > 0) || dt > kGapSeconds;
    cut_ = false;
    raw_turn_ = raw_move_ = 0;
    if (fresh_) {
        smooth_ = target;
        turn_ = move_ = vig_ = 0;
    } else {
        const float turn = float(quat_angle(q_prev, qg)) * kDeg;
        const float dx = p[0] - prev_.m[0][3], dy = p[1] - prev_.m[1][3], dz = p[2] - prev_.m[2][3];
        const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
        cut_ = turn > kCutDegrees || move > kCutMetres;
        if (cut_) {
            smooth_ = target;  // a new shot: no smoothing across it, and it doesn't count as speed
        } else {
            raw_turn_ = turn / dt, raw_move_ = move / dt;
            const float b = 1 - std::exp(-dt / kSpeedTauSeconds);
            turn_ += b * (raw_turn_ - turn_);
            move_ += b * (raw_move_ - move_);
            if (s.enabled && s.smooth_ms > 0) smooth_ = quat_slerp(smooth_, target, 1 - std::exp(-double(dt) * 1000 / s.smooth_ms));
            else smooth_ = target;
        }
    }
    // The lag cap: culling covers cone_deg around the game camera's forward axis; keep the view's
    // forward axis within half of it (the head can still turn the other half).
    const float cap = (s.cone_deg > 0 ? s.cone_deg : 0) * 0.5f / kDeg;
    Mat4 rs = rotation_from_quat(smooth_);
    capped_ = false;
    if (forward_angle(rg, rs) > cap + 1e-6f) {
        // Along the slerp from the game camera to the filter's orientation the forward axis turns
        // steadily away: bisect for the furthest point within the cap.
        double lo = 0, hi = 1;
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2;
            if (forward_angle(rg, rotation_from_quat(quat_slerp(qg, smooth_, mid))) <= cap) lo = mid;
            else hi = mid;
        }
        smooth_ = quat_slerp(qg, smooth_, lo);
        rs = rotation_from_quat(smooth_);
        capped_ = true;
    }
    // Slerps (the filter, the cap towards a rolled camera) can bring back a little roll: level
    // again, keeping the forward axis (so the cap still holds).
    if (s.enabled && s.level >= 1) {
        rs = level_rotation(rs, 1);
        smooth_ = quat_from_rotation(rs);
        rs = rotation_from_quat(smooth_);
    }
    lag_deg_ = forward_angle(rg, rs) * kDeg;
    // D = R_g^T R_s.
    Mat4 rgt = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rgt.m[i][j] = rg.m[j][i];
    delta_ = mul(rgt, rs);
    for (int i = 0; i < 3; ++i) delta_.m[i][3] = delta_.m[3][i] = 0;
    delta_.m[3][3] = 1;
    // The vignette eases towards what the speeds call for.
    if (!fresh_) {
        const float want = s.enabled ? vignette_target(s, turn_, move_) : 0.f;
        const float step = s.ease_ms > 0 ? dt * 1000 / s.ease_ms : 1.f;
        const float d = want - vig_;
        vig_ += d > step ? step : d < -step ? -step : d;
    }
    prev_ = rg;
    for (int i = 0; i < 3; ++i) prev_.m[i][3] = p[i];
    have_ = true;
}

}  // namespace hv::vr
