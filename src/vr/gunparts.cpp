#include "gunparts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <numeric>
#include <unordered_map>

namespace hv::vr {

// --- pieces ---

namespace {

struct Dsu {
    std::vector<int> p;
    explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void unite(int a, int b) {
        a = find(a), b = find(b);
        if (a != b) p[a] = b;
    }
};

uint64_t cell_key(int x, int y, int z) {
    return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(z) & 0x1FFFFF);
}

float smooth(float t) {
    t = std::fmin(1.f, std::fmax(0.f, t));
    return t * t * (3 - 2 * t);
}

void normalize3(float v[3]) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0)
        for (int i = 0; i < 3; ++i) v[i] /= n;
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

}  // namespace

std::vector<int> mesh_pieces(const float* pos, size_t n, size_t stride, const uint32_t* idx, size_t nidx, float weld,
                             std::vector<MeshPiece>* pieces) {
    pieces->clear();
    std::vector<int> out(n, -1);
    if (!pos || n == 0) return out;
    Dsu d(n);
    std::vector<char> used(n, 0);
    for (size_t t = 0; t + 2 < nidx; t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= n || b >= n || c >= n) continue;
        d.unite(int(a), int(b)), d.unite(int(a), int(c));
        used[a] = used[b] = used[c] = 1;
    }
    auto p = [&](size_t i) { return pos + i * stride; };
    if (weld > 0) {
        const float inv = 1 / weld, w2 = weld * weld;
        std::unordered_map<uint64_t, std::vector<int>> grid;
        grid.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            const float* q = p(i);
            if (!used[i] || !std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) continue;
            const int cx = int(std::floor(q[0] * inv)), cy = int(std::floor(q[1] * inv)), cz = int(std::floor(q[2] * inv));
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        auto it = grid.find(cell_key(cx + dx, cy + dy, cz + dz));
                        if (it == grid.end()) continue;
                        for (int j : it->second) {
                            const float* r = p(size_t(j));
                            const float ex = q[0] - r[0], ey = q[1] - r[1], ez = q[2] - r[2];
                            if (ex * ex + ey * ey + ez * ez <= w2) d.unite(int(i), j);
                        }
                    }
            grid[cell_key(cx, cy, cz)].push_back(int(i));
        }
    }
    std::unordered_map<int, int> id_of_root;
    for (size_t i = 0; i < n; ++i) {
        if (!used[i]) continue;
        const int r = d.find(int(i));
        auto [it, fresh] = id_of_root.emplace(r, int(pieces->size()));
        if (fresh) pieces->emplace_back();
        out[i] = it->second;
        MeshPiece& m = (*pieces)[size_t(it->second)];
        ++m.verts;
        for (int k = 0; k < 3; ++k) m.lo[k] = std::fmin(m.lo[k], p(i)[k]), m.hi[k] = std::fmax(m.hi[k], p(i)[k]);
    }
    for (size_t t = 0; t + 2 < nidx; t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= n || b >= n || c >= n) continue;
        ++(*pieces)[size_t(out[a])].tris;
    }
    return out;
}

std::vector<float> rest_stream(size_t n) {
    std::vector<float> s(n * 2, 0.f);
    for (size_t i = 0; i < n; ++i) s[2 * i + 1] = 1.f;
    return s;
}

PartsResult assign_parts(const float* pos, size_t n, size_t stride, const uint32_t* idx, size_t nidx, const PartRule* rules, int nrules,
                         float weld) {
    PartsResult r;
    r.stream = rest_stream(n);
    std::vector<MeshPiece> pcs;
    const std::vector<int> piece = mesh_pieces(pos, n, stride, idx, nidx, weld, &pcs);
    r.pieces = int(pcs.size());
    std::vector<int> owner(pcs.size(), -1);
    constexpr float tol = 0.02f;
    bool ok = true;
    char buf[256];
    for (int k = 0; k < nrules; ++k) {
        const PartRule& R = rules[k];
        int count = 0, tris = 0;
        for (size_t i = 0; i < pcs.size(); ++i) {
            const MeshPiece& m = pcs[i];
            if (owner[i] >= 0 || m.tris < R.min_tris || m.tris > R.max_tris) continue;
            bool in = true;
            for (int a = 0; a < 3 && in; ++a) in = m.lo[a] >= R.lo[a] - tol && m.hi[a] <= R.hi[a] + tol;
            if (!in) continue;
            owner[i] = k, ++count, tris += m.tris;
        }
        const bool bad = count < R.min_count || count > R.max_count || R.part < 0 || R.part >= kMaxParts;
        snprintf(buf, sizeof buf, "%s%s %d piece%s (%d tris)%s", k ? ", " : "", R.name, count, count == 1 ? "" : "s", tris,
                 bad ? " - EXPECTED " : "");
        r.log += buf;
        if (bad) {
            snprintf(buf, sizeof buf, "%d..%d", R.min_count, R.max_count);
            r.log += buf;
            ok = false;
        }
    }
    if (!ok) return r;
    for (size_t v = 0; v < n; ++v) {
        if (piece[v] < 0 || owner[size_t(piece[v])] < 0) continue;
        const PartRule& R = rules[owner[size_t(piece[v])]];
        float w = 1;
        if (R.ramp_axis >= 0 && R.ramp_axis < 3 && R.ramp[1] != R.ramp[0])
            w = std::fmin(1.f, std::fmax(0.f, (pos[v * stride + size_t(R.ramp_axis)] - R.ramp[0]) / (R.ramp[1] - R.ramp[0])));
        r.stream[2 * v] = float(R.part), r.stream[2 * v + 1] = w;
        if (R.part != 0 && w > 0) ++r.vertices_moving;
    }
    r.ok = true;
    return r;
}

// --- per weapon ---
// Boxes in each mesh's model space (cm), from the recon of 2026-11-20 (the design's table): every
// box is the piece's own bounding box with a margin of 0.05-0.15, so only that piece fits. Pivots:
// a trigger turns about the top of its blade, the hammers about the pin at their foot.

static const PartRule kMagnumGun[] = {
    {"slide safeties", kPartSlide, {0.2f, 8.6f, -2.9f}, {3.0f, 10.1f, 1.1f}, 50, 200, 2, 2},
    {"slide", kPartSlide, {-1.8f, 6.1f, -2.6f}, {19.8f, 10.7f, 0.85f}, 100, 400, 1, 1},
    {"rear sight", kPartSlide, {0.8f, 10.35f, -1.2f}, {2.6f, 11.9f, -0.55f}, 8, 40, 1, 1},
    {"hammer", kPartHammer, {-2.35f, 6.75f, -1.4f}, {0.75f, 10.2f, -0.3f}, 20, 120, 1, 1},
    {"trigger", kPartTrigger, {5.35f, 3.05f, -1.2f}, {7.1f, 5.7f, -0.55f}, 20, 120, 1, 1},
};
static const PartRule kGoldGun[] = {
    {"slide", kPartSlide, {-2.0f, 2.6f, -4.4f}, {2.0f, 7.8f, 16.45f}, 100, 400, 1, 1},
    {"front sight", kPartSlide, {-0.45f, 7.6f, 14.3f}, {0.4f, 9.0f, 16.25f}, 8, 40, 1, 1},
    {"slide panel", kPartSlide, {-1.0f, 2.65f, -4.35f}, {1.0f, 5.45f, 12.2f}, 2, 20, 1, 1},
    {"trigger", kPartTrigger, {-0.35f, -0.9f, -4.7f}, {0.35f, 2.1f, -2.7f}, 20, 120, 1, 1},
};
static const PartRule kHandCannonGun[] = {
    {"hammer", kPartHammer, {0.7f, 4.8f, -1.3f}, {4.75f, 9.5f, 0.3f}, 60, 300, 1, 1},
    {"trigger", kPartTrigger, {5.9f, 1.2f, -1.0f}, {8.4f, 4.25f, 0.0f}, 20, 120, 1, 1},
};
static const PartRule kHandCannonClip[] = {
    {"crane", kClipBase, {1.3f, 1.4f, -0.75f}, {4.7f, 5.8f, 1.35f}, 30, 150, 1, 1},
    {"cylinder", kClipTurn, {-4.5f, 2.9f, -2.0f}, {1.55f, 7.6f, 2.6f}, 300, 800, 1, 1},
    {"ejector rod", kClipTurn, {2.9f, 4.85f, -0.1f}, {8.7f, 5.7f, 0.7f}, 4, 60, 0, 3},
};
static const PartRule kPumpGun[] = {
    {"trigger", kPartTrigger, {3.95f, 3.9f, -0.8f}, {5.7f, 6.65f, -0.1f}, 20, 120, 1, 1},
};
static const PartRule kPumpClip[] = {
    {"forend", kClipBase, {-2.5f, -1.6f, -2.9f}, {17.4f, 2.85f, 1.95f}, 60, 300, 1, 1},
};
static const PartRule kAutoShotgunGun[] = {
    {"charging knob", kPartSlide, {20.4f, 12.5f, -3.6f}, {21.2f, 13.35f, -2.0f}, 8, 40, 1, 1},
    {"trigger", kPartTrigger, {7.2f, 5.35f, -1.15f}, {9.65f, 8.55f, -0.35f}, 15, 80, 1, 1},
};
static const PartRule kSmgGun[] = {
    {"trigger", kPartTrigger, {8.1f, 4.4f, -1.4f}, {10.45f, 8.5f, -0.05f}, 30, 100, 1, 1},
};
static const PartRule kRifleGun[] = {
    {"charging handle", kPartSlide, {9.1f, 6.95f, -5.9f}, {17.85f, 10.1f, -3.4f}, 8, 100, 2, 2},
    {"trigger", kPartTrigger, {4.0f, 1.05f, -1.55f}, {6.9f, 4.7f, -0.35f}, 15, 80, 1, 1},
};
static const PartRule kMinigunClip[] = {
    {"sight block", kClipBase, {-1.1f, 4.8f, 17.7f}, {1.2f, 6.4f, 19.9f}, 10, 40, 1, 1},
    {"barrel cluster", kClipTurn, {-6.2f, -6.2f, -3.2f}, {6.2f, 6.2f, 52.2f}, 1, 2000, 30, 60},
};
// The string halves run from the nock (x 1.66, drawn) out to the cams (x 16.4-24); each bends: the
// weight is 1 at the nock and 0 from x 17 (where the string leaves the cam), so moving the nock
// 15.3 cm forward lays each half straight between its cam and x 17.
static const PartRule kCrossbowGun[] = {
    {"string", kPartSlide, {1.5f, 8.6f, -26.3f}, {24.1f, 9.1f, 24.6f}, 40, 200, 2, 2, 0, {17.0f, 1.66f}},
    {"trigger", kPartTrigger, {5.5f, 2.15f, -1.1f}, {7.7f, 5.9f, -0.45f}, 8, 60, 1, 1},
};

static WeaponParts make(const char* key, float bx, float by, float bz) {
    WeaponParts w;
    w.key = key;
    w.barrel[0] = bx, w.barrel[1] = by, w.barrel[2] = bz;
    w.up[0] = 0, w.up[1] = 1, w.up[2] = 0;
    return w;
}

static void set3(float* d, float x, float y, float z) { d[0] = x, d[1] = y, d[2] = z; }

static std::vector<WeaponParts> build_table() {
    std::vector<WeaponParts> t;
    {
        // Slide (with its safeties and rear sight) back 3 cm; the hammer, in its path, is pushed back.
        WeaponParts w = make("magnum", 1, 0, 0);
        w.gun = kMagnumGun, w.n_gun = int(std::size(kMagnumGun));
        w.slide_travel = 3.0f, set3(w.slide_dir, -1, 0, 0), w.slide_lock = true;
        set3(w.hammer_pivot, -0.3f, 7.0f, -0.85f), w.hammer_deg = 55;
        set3(w.trigger_pivot, 6.2f, 5.5f, -0.87f), w.trigger_deg = 16;
        t.push_back(w);
    }
    {
        // Barrel and slide are one piece: it moves with its front sight and side panels.
        WeaponParts w = make("gold_magnum", 0, 0, 1);
        w.gun = kGoldGun, w.n_gun = int(std::size(kGoldGun));
        w.slide_travel = 2.6f, set3(w.slide_dir, 0, 0, -1), w.slide_lock = true;
        set3(w.trigger_pivot, 0, 1.9f, -3.7f), w.trigger_deg = 16;
        t.push_back(w);
    }
    {
        // The _Clip is the cylinder (with crane and ejector rod): in the frame's window with the
        // top chamber on the bore; it turns 60 degrees per shot about its own axis.
        WeaponParts w = make("handcannon", 1, 0, 0);
        w.gun = kHandCannonGun, w.n_gun = int(std::size(kHandCannonGun));
        w.clip = kHandCannonClip, w.n_clip = int(std::size(kHandCannonClip));
        w.clip_part = true, set3(w.clip_rest, 10.30f, 2.34f, -0.81f);
        set3(w.hammer_pivot, 3.8f, 5.0f, -0.5f), w.hammer_deg = 32;
        set3(w.trigger_pivot, 7.18f, 4.05f, -0.5f), w.trigger_deg = 16;
        set3(w.turn_point, 0, 5.259f, 0.288f), w.turn_sign = 1;
        set3(w.swing_point, 0, 1.9f, 1.19f), w.swing_deg = 85, w.swing_sign = 1;
        t.push_back(w);
    }
    {
        // The _Clip is the forend, on the magazine tube; it racks back 8.5 cm after each shot.
        WeaponParts w = make("pump", 1, 0, 0);
        w.gun = kPumpGun, w.n_gun = int(std::size(kPumpGun));
        w.clip = kPumpClip, w.n_clip = int(std::size(kPumpClip));
        w.clip_part = true, set3(w.clip_rest, 32.5f, 7.0f, 0);
        w.rack_travel = 8.5f;
        set3(w.trigger_pivot, 4.8f, 6.45f, -0.48f), w.trigger_deg = 16;
        t.push_back(w);
    }
    {
        WeaponParts w = make("autoshotgun", 1, 0, 0);
        w.gun = kAutoShotgunGun, w.n_gun = int(std::size(kAutoShotgunGun));
        w.slide_travel = 4.0f, set3(w.slide_dir, -1, 0, 0);
        set3(w.trigger_pivot, 8.43f, 8.35f, -0.73f), w.trigger_deg = 16;
        t.push_back(w);
    }
    {
        WeaponParts w = make("smg", 1, 0, 0);
        w.gun = kSmgGun, w.n_gun = int(std::size(kSmgGun));
        set3(w.trigger_pivot, 9.27f, 8.3f, -0.72f), w.trigger_deg = 16;
        t.push_back(w);
    }
    {
        // Toed in: the charging handle runs back along the bore.
        WeaponParts w = make("rifle", 0.968961f, -0.110631f, -0.221077f);
        w.gun = kRifleGun, w.n_gun = int(std::size(kRifleGun));
        w.slide_travel = 4.0f, set3(w.slide_dir, -0.968961f, 0.110631f, 0.221077f);
        set3(w.trigger_pivot, 5.46f, 4.5f, -0.93f), w.trigger_deg = 16;
        w.kick_back = 0.02f, w.kick_home = 0.05f;  // 3-round bursts
        t.push_back(w);
    }
    {
        // The _Clip is the barrel cluster: it spins about the bore axis (+z), all but the sight block.
        WeaponParts w = make("minigun", 0, 0, 1);
        w.clip = kMinigunClip, w.n_clip = int(std::size(kMinigunClip));
        w.clip_part = true;
        set3(w.turn_point, 0.04f, 0.01f, 0), w.turn_sign = -1;
        t.push_back(w);
    }
    {
        WeaponParts w = make("crossbow", 1, 0, 0);
        w.gun = kCrossbowGun, w.n_gun = int(std::size(kCrossbowGun));
        w.slide_travel = 15.34f, set3(w.slide_dir, 1, 0, 0), w.slide_is_string = true;
        set3(w.trigger_pivot, 6.6f, 5.7f, -0.77f), w.trigger_deg = 16;
        t.push_back(w);
    }
    return t;
}

static const std::vector<WeaponParts>& table() {
    static const std::vector<WeaponParts> t = build_table();
    return t;
}

const WeaponParts* weapon_parts(const char* key) {
    if (!key) return nullptr;
    for (const WeaponParts& w : table())
        if (!std::strcmp(w.key, key)) return &w;
    return nullptr;
}
int weapon_parts_count() { return int(table().size()); }
const WeaponParts& weapon_parts_at(int i) { return table()[size_t(i)]; }

Mat4 rotation_about(const float axis[3], const float point[3], float deg) {
    float k[3] = {axis[0], axis[1], axis[2]};
    normalize3(k);
    const float a = deg * 3.14159265f / 180, c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat4 r = identity();
    r.m[0][0] = c + t * k[0] * k[0], r.m[0][1] = t * k[0] * k[1] - s * k[2], r.m[0][2] = t * k[0] * k[2] + s * k[1];
    r.m[1][0] = t * k[1] * k[0] + s * k[2], r.m[1][1] = c + t * k[1] * k[1], r.m[1][2] = t * k[1] * k[2] - s * k[0];
    r.m[2][0] = t * k[2] * k[0] - s * k[1], r.m[2][1] = t * k[2] * k[1] + s * k[0], r.m[2][2] = c + t * k[2] * k[2];
    return mul(mul(translation(point[0], point[1], point[2]), r), translation(-point[0], -point[1], -point[2]));
}

void pose_weapon(const WeaponParts* w, const GunMotion& m, GunPose* out) {
    for (int i = 0; i < kMaxParts; ++i) out->gun[i] = out->clip[i] = identity();
    if (!w) return;
    float b[3] = {w->barrel[0], w->barrel[1], w->barrel[2]}, u[3] = {w->up[0], w->up[1], w->up[2]};
    normalize3(b);
    const float ub = u[0] * b[0] + u[1] * b[1] + u[2] * b[2];
    for (int i = 0; i < 3; ++i) u[i] -= ub * b[i];
    normalize3(u);
    float top_back[3], bottom_back[3];
    cross3(b, u, top_back);     // turning about b x u moves +up towards -barrel
    cross3(u, b, bottom_back);  // about u x b: -up towards -barrel
    if (w->slide_travel != 0) {
        const float s = w->slide_travel * (w->slide_is_string ? m.string : m.kick);
        out->gun[kPartSlide] = translation(w->slide_dir[0] * s, w->slide_dir[1] * s, w->slide_dir[2] * s);
    }
    if (w->hammer_deg != 0) out->gun[kPartHammer] = rotation_about(top_back, w->hammer_pivot, w->hammer_deg * m.hammer);
    if (w->trigger_deg != 0) out->gun[kPartTrigger] = rotation_about(bottom_back, w->trigger_pivot, w->trigger_deg * m.trigger);
    if (w->clip_part) {
        const float r = w->rack_travel * m.rack;
        Mat4 base = translation(w->clip_rest[0] - b[0] * r, w->clip_rest[1] - b[1] * r, w->clip_rest[2] - b[2] * r);
        if (w->swing_deg != 0) base = mul(base, rotation_about(b, w->swing_point, w->swing_sign * w->swing_deg * m.swing));
        out->clip[kClipBase] = base;
        out->clip[kClipTurn] = mul(base, rotation_about(b, w->turn_point, w->turn_sign * (m.cyl_deg + m.spin_deg)));
    }
}

// --- curves ---

float kick_curve(float t, float back, float home) {
    if (!(t >= 0) || t >= back + home) return 0;
    if (t < back) {
        const float x = 1 - t / back;
        return 1 - x * x;
    }
    return 1 - smooth((t - back) / home);
}

float cylinder_turn(float t) { return t <= kHammerFall ? 0.f : smooth((t - kHammerFall) / 0.08f); }

float rack_curve(float t, float start, float back, float hold, float fwd) {
    if (!(t >= start)) return 0;
    t -= start;
    if (t < back) return smooth(t / back);
    t -= back;
    if (t < hold) return 1;
    t -= hold;
    return t < fwd ? 1 - smooth(t / fwd) : 0;
}

float string_curve(float t, float snap, float draw) {
    constexpr float pause = 0.05f;
    if (!(t >= 0)) return 0;
    if (t < snap) {
        const float x = 1 - t / snap;
        return 1 - x * x;
    }
    t -= snap;
    if (t < pause) return 1;
    t -= pause;
    return t < draw ? 1 - smooth(t / draw) : 0;
}

void spin_step(bool spinning, float dt, float* speed, float* angle) {
    dt = std::fmin(0.1f, std::fmax(0.f, dt));
    if (spinning) *speed = std::fmin(kSpinSpeed, *speed + kSpinSpeed / kSpinUp * dt);
    else *speed = std::fmax(0.f, *speed - kSpinSpeed / kSpinDown * dt);
    *angle = std::fmod(*angle + std::fmin(*speed * dt, kSpinMaxStep), 360.f);
}

}  // namespace hv::vr
