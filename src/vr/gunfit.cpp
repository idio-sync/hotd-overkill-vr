#include "gunfit.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace hv::vr {

namespace {

struct V3 {
    double x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 unit(V3 a) {
    const double n = len(a);
    return n > 0 ? a * (1 / n) : a;
}

struct P2 {
    double u, v;
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kRingGap = 0.3;       // units along the hint between rings
constexpr double kRingLength = 0.6;    // a ring's longest extent along the hint
constexpr double kInlier = 0.02;       // units: a point on a candidate circle
constexpr double kRadiusGap = 0.08;    // units between two concentric circles' radii
// A circle: 6+ points going most of the way round (no gap over 75 degrees). Four corners of a
// box section are on a circle too, so 4 are not enough.
constexpr size_t kCirclePoints = 6;
constexpr double kMaxArcGap = 75;
constexpr double kRmsMax = 0.05, kResidualMax = 0.1;
constexpr double kRadiusMin = 0.2, kRadiusMax = 6;
constexpr double kOnLine = 0.03;       // units across the axis: a ring centre on it
constexpr double kMaxTiltDeg = 5;      // an axis this far from the hint pairs unrelated parts
constexpr double kNearHint = 3;        // units across the described barrel line: a ring may be the bore
constexpr double kSpanMin = 1.0;       // units between the first and last ring

// Small dense linear solve (Gaussian elimination, partial pivoting), n <= 16.
bool solve(std::vector<double>& a, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(a[r * n + c]) > std::fabs(a[p * n + c])) p = r;
        if (std::fabs(a[p * n + c]) < 1e-12) return false;
        if (p != c) {
            for (int k = 0; k < n; ++k) std::swap(a[c * n + k], a[p * n + k]);
            std::swap(b[c], b[p]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            for (int k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int c = n - 1; c >= 0; --c) {
        for (int k = c + 1; k < n; ++k) b[c] -= a[c * n + k] * b[k];
        b[c] /= a[c * n + c];
    }
    return true;
}

// The circle through three points; false if they are (nearly) collinear.
bool circle3(P2 a, P2 b, P2 c, P2* centre, double* r) {
    const double d = 2 * (a.u * (b.v - c.v) + b.u * (c.v - a.v) + c.u * (a.v - b.v));
    if (std::fabs(d) < 1e-9) return false;
    const double a2 = a.u * a.u + a.v * a.v, b2 = b.u * b.u + b.v * b.v, c2 = c.u * c.u + c.v * c.v;
    centre->u = (a2 * (b.v - c.v) + b2 * (c.v - a.v) + c2 * (a.v - b.v)) / d;
    centre->v = (a2 * (c.u - b.u) + b2 * (a.u - c.u) + c2 * (b.u - a.u)) / d;
    *r = std::hypot(a.u - centre->u, a.v - centre->v);
    return true;
}

// Deterministic RANSAC over point triples: the centre of the circle with the most points on it.
bool ransac_centre(const std::vector<P2>& p, P2* best) {
    const size_t n = p.size();
    std::vector<size_t> sample;
    const size_t m = (std::min)(n, size_t(40));
    for (size_t i = 0; i < m; ++i) sample.push_back(i * n / m);
    int best_count = 0;
    double best_err = 1e30;
    for (size_t i = 0; i < m; ++i)
        for (size_t j = i + 1; j < m; ++j)
            for (size_t k = j + 1; k < m; ++k) {
                P2 c;
                double r;
                if (!circle3(p[sample[i]], p[sample[j]], p[sample[k]], &c, &r) || r < kRadiusMin || r > kRadiusMax) continue;
                int count = 0;
                double err = 0;
                for (const P2& q : p) {
                    const double e = std::fabs(std::hypot(q.u - c.u, q.v - c.v) - r);
                    if (e <= kInlier) ++count, err += e;
                }
                if (count > best_count || (count == best_count && err < best_err)) best_count = count, best_err = err, *best = c;
            }
    return best_count >= int(kCirclePoints);
}

// The largest angular gap (degrees) between points around a centre.
double max_arc_gap(const std::vector<P2>& p, const std::vector<int>& idx, P2 c) {
    std::vector<double> a;
    for (int i : idx) a.push_back(std::atan2(p[i].v - c.v, p[i].u - c.u) * 180 / kPi);
    std::sort(a.begin(), a.end());
    double gap = a.front() + 360 - a.back();
    for (size_t i = 1; i < a.size(); ++i) gap = (std::max)(gap, a[i] - a[i - 1]);
    return gap;
}

// Concentric circles around c: groups of points by distance, each of 4+ points going most of the
// way round. Returns the groups (indices into p).
std::vector<std::vector<int>> circle_groups(const std::vector<P2>& p, P2 c) {
    std::vector<int> order(p.size());
    for (size_t i = 0; i < p.size(); ++i) order[i] = int(i);
    std::vector<double> r(p.size());
    for (size_t i = 0; i < p.size(); ++i) r[i] = std::hypot(p[i].u - c.u, p[i].v - c.v);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return r[a] < r[b]; });
    std::vector<std::vector<int>> groups, out;
    for (size_t k = 0; k < order.size(); ++k) {
        if (k == 0 || r[order[k]] - r[order[k - 1]] > kRadiusGap) groups.emplace_back();
        groups.back().push_back(order[k]);
    }
    for (auto& g0 : groups) {
        // Only the points on the circle itself (within kInlier of the group's median radius): a
        // sight's base that touches the barrel is near the radius, but not on it.
        const double rg = r[size_t(g0[g0.size() / 2])];
        std::vector<int> g;
        for (int i : g0)
            if (std::fabs(r[size_t(i)] - rg) <= kInlier) g.push_back(i);
        if (g.size() >= kCirclePoints && rg >= kRadiusMin && rg <= kRadiusMax && max_arc_gap(p, g, c) <= kMaxArcGap) out.push_back(g);
    }
    return out;
}

// Geometric fit of concentric circles (one radius per group): Gauss-Newton on |p - c| - r_g.
void fit_concentric(const std::vector<P2>& p, const std::vector<std::vector<int>>& groups, P2* c, std::vector<double>* radii) {
    const int g = int(groups.size()), n = 2 + g;
    radii->assign(size_t(g), 0);
    for (int k = 0; k < g; ++k) {
        double s = 0;
        for (int i : groups[size_t(k)]) s += std::hypot(p[size_t(i)].u - c->u, p[size_t(i)].v - c->v);
        (*radii)[size_t(k)] = s / double(groups[size_t(k)].size());
    }
    for (int it = 0; it < 20; ++it) {
        std::vector<double> a(size_t(n * n), 0), b(size_t(n), 0);
        for (int k = 0; k < g; ++k)
            for (int i : groups[size_t(k)]) {
                const double du = p[size_t(i)].u - c->u, dv = p[size_t(i)].v - c->v, d = std::hypot(du, dv);
                if (d < 1e-12) continue;
                std::vector<double> j(size_t(n), 0);
                j[0] = -du / d, j[1] = -dv / d, j[size_t(2 + k)] = -1;
                const double e = d - (*radii)[size_t(k)];
                for (int x = 0; x < n; ++x) {
                    b[size_t(x)] -= j[size_t(x)] * e;
                    for (int y = 0; y < n; ++y) a[size_t(x * n + y)] += j[size_t(x)] * j[size_t(y)];
                }
            }
        if (!solve(a, b, n)) return;
        c->u += b[0], c->v += b[1];
        for (int k = 0; k < g; ++k) (*radii)[size_t(k)] += b[size_t(2 + k)];
        if (std::fabs(b[0]) + std::fabs(b[1]) < 1e-9) break;
    }
}

struct Circle {
    P2 c;
    double rmin, rmax, rms;
    int used;
};

// The round parts of one ring of points (across the hint): circles with their concentric
// companions, strongest first. A ring can hold parts on different axes (an SMG's barrel nut sits
// 0.1 off its barrel's axis), so after each circle set its points are taken out and the rest
// searched again.
std::vector<Circle> extract_circles(std::vector<P2> p) {
    std::vector<Circle> out;
    for (int round = 0; round < 4 && p.size() >= kCirclePoints; ++round) {
        P2 c;
        if (!ransac_centre(p, &c)) break;
        std::vector<std::vector<int>> groups;
        std::vector<double> radii;
        for (int pass = 0; pass < 2; ++pass) {
            groups = circle_groups(p, c);
            if (groups.empty()) break;
            fit_concentric(p, groups, &c, &radii);
        }
        if (groups.empty()) break;
        double s2 = 0, worst = 0, rmin = 1e30, rmax = 0;
        int n = 0;
        for (size_t k = 0; k < groups.size(); ++k) {
            rmin = (std::min)(rmin, radii[k]), rmax = (std::max)(rmax, radii[k]);
            for (int i : groups[k]) {
                const double e = std::hypot(p[size_t(i)].u - c.u, p[size_t(i)].v - c.v) - radii[k];
                s2 += e * e, worst = (std::max)(worst, std::fabs(e)), ++n;
            }
        }
        const double rms = std::sqrt(s2 / n);
        if (rms <= kRmsMax && worst <= kResidualMax && rmin >= kRadiusMin && rmax <= kRadiusMax) out.push_back({c, rmin, rmax, rms, n});
        std::vector<P2> rest;
        for (const P2& q : p) {
            const double d = std::hypot(q.u - c.u, q.v - c.v);
            bool on = false;
            for (double r : radii) on = on || std::fabs(d - r) <= kInlier;
            if (!on) rest.push_back(q);
        }
        if (rest.size() == p.size()) break;
        p.swap(rest);
    }
    return out;
}

// Fallback for a bore with a single round ring (a muzzle face on a flat-sided slide): the
// dominant direction of vertex pairs nearly along the hint. Modelled guns are full of edges
// parallel to the barrel (slide, rib, frame), which line up exactly; other pairs scatter. Pairs
// at least 2 units apart along the hint and within 5 degrees of it are binned by their slopes
// across it (0.0005 rad bins); the fullest bin, refined by the pairs within one bin of it, wins.
bool edge_direction(const std::vector<V3>& s, V3 h, V3 u, V3 v, V3* dir, int* support) {
    constexpr double kBin = 0.0005, kCone = 0.0875;  // tan(5 deg)
    std::vector<std::pair<double, double>> slopes;
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = i + 1; j < s.size(); ++j) {
            V3 d = s[j] - s[i];
            double t = dot(d, h);
            if (std::fabs(t) < 2) continue;
            if (t < 0) d = d * -1, t = -t;
            const double a = dot(d, u) / t, b = dot(d, v) / t;
            if (std::hypot(a, b) <= kCone) slopes.push_back({a, b});
        }
    if (slopes.empty()) return false;
    std::vector<std::pair<long long, long long>> keys;
    keys.reserve(slopes.size());
    for (auto& [a, b] : slopes) keys.push_back({std::llround(a / kBin), std::llround(b / kBin)});
    std::vector<std::pair<long long, long long>> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    std::pair<long long, long long> best{};
    size_t best_n = 0;
    for (size_t i = 0; i < sorted.size();) {
        size_t j = i;
        while (j < sorted.size() && sorted[j] == sorted[i]) ++j;
        if (j - i > best_n) best_n = j - i, best = sorted[i];
        i = j;
    }
    double sa = 0, sb = 0;
    int n = 0;
    for (size_t k = 0; k < slopes.size(); ++k)
        if (std::llabs(keys[k].first - best.first) <= 1 && std::llabs(keys[k].second - best.second) <= 1) sa += slopes[k].first, sb += slopes[k].second, ++n;
    *support = n;
    if (n < 6) return false;
    *dir = unit(h + u * (sa / n) + v * (sb / n));
    return true;
}

struct Ring {
    V3 centre;
    double t, rmin, rmax, rms;
    int points;
};

}  // namespace

static BoreFit fit_once(const float* pos, size_t n, size_t stride, const float hint_in[3], const float* hint_point, float slab);

// Rings that are close together along a tilted axis (a chamfer just behind the muzzle face) land in
// one group and are not concentric across the hint: so the fit is repeated with its own axis as
// the hint until it settles.
BoreFit fit_bore(const float* pos, size_t n, size_t stride, const float hint[3], const float* hint_point, float slab) {
    BoreFit f = fit_once(pos, n, stride, hint, hint_point, slab);
    for (int it = 0; it < 4 && f.axis_ok; ++it) {
        const float h[3] = {f.dir[0], f.dir[1], f.dir[2]};
        const BoreFit g = fit_once(pos, n, stride, h, hint_point, slab);
        if (!g.axis_ok) break;
        const double c = double(g.dir[0]) * f.dir[0] + double(g.dir[1]) * f.dir[1] + double(g.dir[2]) * f.dir[2];
        f = g;
        if (c > std::cos(1e-4 * kPi / 180)) break;
    }
    return f;
}

static BoreFit fit_once(const float* pos, size_t n, size_t stride, const float hint_in[3], const float* hint_point, float slab) {
    BoreFit f;
    const V3 h = unit({hint_in[0], hint_in[1], hint_in[2]});
    if (len(h) == 0 || n == 0) {
        f.why = "no vertices or no hint";
        return f;
    }
    // A basis across the hint.
    const V3 seed = std::fabs(h.y) < 0.9 ? V3{0, 1, 0} : V3{1, 0, 0};
    const V3 u = unit(cross(seed, h)), v = cross(h, u);
    // Distinct vertices (duplicates differ only in normal/uv).
    std::vector<V3> pts;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const float* q = pos + i * stride;
        if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) continue;
        pts.push_back({q[0], q[1], q[2]});
    }
    std::sort(pts.begin(), pts.end(), [](const V3& a, const V3& b) { return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z; });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const V3& a, const V3& b) { return len(a - b) < 1e-4; }), pts.end());
    if (pts.empty()) {
        f.why = "no finite vertices";
        return f;
    }
    double tmax = -1e30;
    for (const V3& p : pts) tmax = (std::max)(tmax, dot(p, h));
    std::vector<std::pair<double, V3>> s;  // (t, point) in the slab
    for (const V3& p : pts)
        if (dot(p, h) >= tmax - slab) s.push_back({dot(p, h), p});
    std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    f.slab_points = int(s.size());
    // Rings along the hint, each fitted across it.
    std::vector<Ring> rings;
    for (size_t i = 0; i < s.size();) {
        size_t j = i + 1;
        while (j < s.size() && s[j].first - s[j - 1].first <= kRingGap && s[j].first - s[i].first <= kRingLength) ++j;
        std::vector<P2> p2;
        double tsum = 0;
        for (size_t k = i; k < j; ++k) p2.push_back({dot(s[k].second, u), dot(s[k].second, v)}), tsum += s[k].first;
        const double t = tsum / double(j - i);
        for (const Circle& c : extract_circles(p2)) rings.push_back({h * t + u * c.c.u + v * c.c.v, t, c.rmin, c.rmax, c.rms, c.used});
        i = j;
    }
    f.rings_round = int(rings.size());
    // Only round parts near the described barrel line: a pump's magazine tube, a crossbow's limb
    // pins or a minigun's side parts are round too.
    if (hint_point) {
        const V3 hp{hint_point[0], hint_point[1], hint_point[2]};
        std::vector<Ring> near;
        for (const Ring& r : rings) {
            const V3 w = r.centre - hp;
            if (len(w - h * dot(w, h)) <= kNearHint) near.push_back(r);
        }
        rings.swap(near);
    }
    for (const Ring& r : rings) {
        if (f.n_info >= int(std::size(f.info))) break;
        BoreFit::RingInfo& ri = f.info[f.n_info++];
        ri.t = float(r.t), ri.centre[0] = float(r.centre.x), ri.centre[1] = float(r.centre.y), ri.centre[2] = float(r.centre.z);
        ri.rmin = float(r.rmin), ri.rmax = float(r.rmax), ri.points = r.points;
    }
    if (rings.empty()) {
        f.why = hint_point && f.rings_round ? "no round rings near the described barrel line" : "no round rings near the muzzle end";
        return f;
    }
    // The front ring (the muzzle face; of several there, the innermost: the bore), and the line
    // through it and another ring at least kSpanMin behind that has the most ring centres on it
    // (within kOnLine across it); of equals, the one whose rings are smallest (the bore or barrel,
    // not a shroud around it).
    size_t fi = 0;
    for (size_t k = 1; k < rings.size(); ++k)
        if (rings[k].t > rings[fi].t + 0.05 || (std::fabs(rings[k].t - rings[fi].t) <= 0.05 && rings[k].rmin < rings[fi].rmin)) fi = k;
    const Ring front = rings[fi];
    std::vector<size_t> best;
    double best_r = 1e30;
    for (size_t j = 0; j < rings.size(); ++j) {
        if (j == fi || front.t - rings[j].t < kSpanMin) continue;
        const V3 d = unit(front.centre - rings[j].centre);
        if (dot(d, h) < std::cos(kMaxTiltDeg * kPi / 180)) continue;  // two unrelated round parts
        std::vector<size_t> on;
        double rsum = 0;
        for (size_t k = 0; k < rings.size(); ++k) {
            const V3 w = rings[k].centre - front.centre;
            if (len(w - d * dot(w, d)) <= kOnLine) on.push_back(k), rsum += rings[k].rmin;
        }
        const double rmean = rsum / double(on.size());
        if (on.size() > best.size() || (on.size() == best.size() && rmean < best_r)) best = on, best_r = rmean;
    }
    std::vector<Ring> on;
    if (best.empty()) on.push_back(front);
    for (size_t k : best) on.push_back(rings[k]);
    if (best.empty()) best.push_back(fi);
    for (size_t k : best)
        if (k < std::size(f.info)) f.info[k].used = true;
    f.rings = int(on.size());
    // The axis: weighted least-squares line through the centres.
    double w = 0;
    V3 m{0, 0, 0};
    for (const Ring& r : on) m = m + r.centre * r.points, w += r.points;
    m = m * (1 / w);
    double cov[3][3] = {};
    for (const Ring& r : on) {
        const V3 d = r.centre - m;
        const double c[3] = {d.x, d.y, d.z};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a][b] += r.points * c[a] * c[b];
    }
    V3 dir = h;
    for (int it = 0; it < 100; ++it) {  // power iteration from the hint
        const V3 nd{cov[0][0] * dir.x + cov[0][1] * dir.y + cov[0][2] * dir.z, cov[1][0] * dir.x + cov[1][1] * dir.y + cov[1][2] * dir.z,
                    cov[2][0] * dir.x + cov[2][1] * dir.y + cov[2][2] * dir.z};
        if (len(nd) == 0) break;
        dir = unit(nd);
    }
    if (dot(dir, h) < 0) dir = dir * -1;
    double tlo = 1e30, thi = -1e30, rmax = 0, rms = 0;
    for (const Ring& r : on) {
        const double t = dot(r.centre - m, dir);
        tlo = (std::min)(tlo, t), thi = (std::max)(thi, t), rmax = (std::max)(rmax, r.rmax), rms = (std::max)(rms, r.rms);
    }
    f.span = float(on.size() > 1 ? thi - tlo : 0);
    f.radius = float(rmax), f.rms = float(rms);
    f.axis_ok = on.size() >= 2 && f.span >= kSpanMin;
    if (f.axis_ok) {
        f.method = "rings";
    } else {
        // One round ring (the muzzle face): the axis through its centre, along the mesh's edges.
        m = front.centre;
        std::vector<V3> sp;
        for (const auto& [t, p] : s) sp.push_back(p);
        if (edge_direction(sp, h, u, v, &dir, &f.edge_support)) {
            f.axis_ok = true, f.method = "edges";
            f.why = "one round ring: the axis through its centre along the mesh's edges";
        } else {
            dir = h, f.method = "none";
            f.why = "one round ring and no edge direction: muzzle only, along the hint";
        }
    }
    // The muzzle: the axis point level with the front-most vertex near the axis.
    double tf = -1e30;
    for (const V3& p : pts) {
        const V3 d = p - m;
        const double t = dot(d, dir);
        if (len(d - dir * t) <= rmax + 0.3) tf = (std::max)(tf, t);
    }
    const V3 mz = m + dir * tf;
    f.muzzle_ok = true;
    f.dir[0] = float(dir.x), f.dir[1] = float(dir.y), f.dir[2] = float(dir.z);
    f.muzzle[0] = float(mz.x), f.muzzle[1] = float(mz.y), f.muzzle[2] = float(mz.z);
    return f;
}

void compare_bore(const BoreFit& f, const float dd[3], const float dm[3], float* angle_deg, float* perp, float* along) {
    const V3 d = unit({dd[0], dd[1], dd[2]}), a = {f.dir[0], f.dir[1], f.dir[2]};
    *angle_deg = f.axis_ok ? float(std::acos((std::min)(1.0, (std::max)(-1.0, dot(d, a)))) * 180 / kPi) : -1.f;
    const V3 w = V3{dm[0], dm[1], dm[2]} - V3{f.muzzle[0], f.muzzle[1], f.muzzle[2]};
    const double t = dot(w, a);
    *along = float(t);
    *perp = float(len(w - a * t));
}

}  // namespace hv::vr
