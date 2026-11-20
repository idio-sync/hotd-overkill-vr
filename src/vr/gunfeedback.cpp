#include "gunfeedback.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace hv::vr {

namespace {

constexpr float kPi = 3.14159265f;

float clamp01(float v) { return std::fmin(1.f, std::fmax(0.f, v)); }

// 0..1 from a hash.
float unit(uint32_t h) { return float(h & 0xFFFFFF) / float(0xFFFFFF); }

uint32_t argb(float r, float g, float b) {
    auto c = [](float v) { return uint32_t(std::lround(clamp01(v) * 255)); };
    return 0xFF000000u | c(r) << 16 | c(g) << 8 | c(b);
}

// Hot white at full brightness, through yellow to a dim orange.
uint32_t flame(float b, float core) {
    b = clamp01(b), core = clamp01(core);
    return argb(b, b * (0.72f + 0.28f * core), b * (0.30f + 0.70f * core));
}

}  // namespace

const char* haptic_name(Haptic h) {
    static const char* const names[] = {"pistol", "handcannon", "shotgun", "rifle", "smg", "minigun", "crossbow", "hit", "dry", "seat", "forend"};
    static_assert(std::size(names) == size_t(Haptic::count));
    return size_t(h) < std::size(names) ? names[size_t(h)] : "?";
}

// --- per weapon ---
// Flash sizes (radius, model cm): big for the shotguns and the hand cannon, medium for the pistols
// and the rifle, small and flickering for the SMG and the minigun, none for the crossbow. Recoil:
// the magnum ~4 deg and 1.5 cm; the hand cannon and the pump the strongest; the SMG and minigun
// small and fast. Hit ticks come after the shot's own pattern (shotguns: after the second thump;
// the minigun: in the gap before its next shot, ~25 ms later).
static const FeedbackDef kFeedback[] = {
    {"magnum", 9.0f, 2.0f, false, 4.0f, 1.5f, 0.016f, 1.3f, Haptic::pistol, 50},
    {"gold_magnum", 9.0f, 2.0f, false, 4.0f, 1.5f, 0.016f, 1.3f, Haptic::pistol, 50},
    {"handcannon", 13.0f, 1.8f, false, 7.0f, 2.5f, 0.020f, 1.3f, Haptic::handcannon, 65},
    {"pump", 14.0f, 2.2f, false, 6.5f, 3.0f, 0.020f, 1.3f, Haptic::shotgun, 120},
    {"autoshotgun", 13.0f, 2.2f, false, 5.0f, 2.5f, 0.018f, 1.4f, Haptic::shotgun, 120},
    {"smg", 7.0f, 1.8f, true, 1.5f, 0.6f, 0.012f, 1.6f, Haptic::smg, 45},
    {"rifle", 8.0f, 2.4f, false, 2.0f, 1.0f, 0.014f, 1.6f, Haptic::rifle, 30},
    {"minigun", 8.0f, 2.0f, true, 0.8f, 0.4f, 0.010f, 1.6f, Haptic::minigun, 14},
    {"crossbow", 0.0f, 0.0f, false, 1.5f, 0.6f, 0.016f, 1.3f, Haptic::crossbow, 0},
};

const FeedbackDef& feedback_def(const char* key) {
    if (key)
        for (const FeedbackDef& d : kFeedback)
            if (!std::strcmp(d.key, key)) return d;
    return kFeedback[0];
}
int feedback_count() { return int(std::size(kFeedback)); }
const FeedbackDef& feedback_at(int i) { return kFeedback[size_t(i)]; }

// --- recoil ---

float recoil_kick(float t, float tau) {
    if (!(t >= 0) || !(tau > 0) || t > 10 * tau) return 0;
    const float x = t / tau;
    return x * std::exp(1 - x);
}

float recoil_amount(const float* since, int n, float tau, float cap) {
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += recoil_kick(since[i], tau);
    if (!(cap > 0)) return sum;
    const float knee = std::fmax(std::fmin(1.f, cap), 0.7f * cap);  // one kick is never squeezed
    if (sum <= knee || knee >= cap) return std::fmin(sum, cap);
    return knee + (cap - knee) * std::tanh((sum - knee) / (cap - knee));
}

Mat4 recoil_in_gun(const float g[3], float deg, float back_m) {
    // About +x, a positive angle turns +z towards -y (x cross z = -y): the muzzle up is -deg.
    const float a = -deg * kPi / 180, c = std::cos(a), s = std::sin(a);
    Mat4 r = identity();
    r.m[1][1] = c, r.m[1][2] = -s;
    r.m[2][1] = s, r.m[2][2] = c;
    const Mat4 about = mul(mul(translation(g[0], g[1], g[2]), r), translation(-g[0], -g[1], -g[2]));
    return mul(translation(0, 0, -back_m), about);
}

// --- muzzle flash ---

uint32_t flash_hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du;
    x ^= x >> 15, x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

FlashFrame flash_frame(int frame, uint32_t seed, const FeedbackDef& d, uint32_t jitter) {
    FlashFrame f;
    if (!(d.flash_cm > 0) || frame < 0 || frame >= kFlashFrames) return f;
    f.on = true, f.frame = frame;
    // The first frame full, the second bigger and fading.
    f.scale = frame == 0 ? 1.0f : 1.15f;
    f.intensity = frame == 0 ? 1.0f : 0.5f;
    f.rot_deg = unit(flash_hash(seed)) * 360;
    f.scale *= 0.9f + 0.2f * unit(flash_hash(seed ^ 0x9E3779B9u));
    if (d.flicker) {
        // Every frame its own size and turn: a flicker, never the same star twice in a row.
        f.scale *= 0.7f + 0.45f * unit(flash_hash(jitter));
        f.rot_deg = std::fmod(f.rot_deg + 72 + unit(flash_hash(jitter ^ 0x51ED27u)) * 216, 360.f);
    }
    return f;
}

FlashFrame flash_step(FlashState* s, unsigned shot_count, const FeedbackDef& d) {
    if (shot_count != s->shot) {
        s->shot = shot_count;
        s->frame = 0;
        s->seed = flash_hash(shot_count * 2654435761u + 12345u);
    } else if (s->frame < 1000) {
        ++s->frame;
    }
    return flash_frame(s->frame, s->seed, d, flash_hash(s->seed + uint32_t(s->frame) * 7919u));
}

std::vector<uint32_t> flash_star_image(int size) {
    std::vector<uint32_t> px(size_t(size) * size, 0xFF000000u);
    // Five spikes at uneven angles and lengths (fixed: the per-shot rotation and size vary it).
    static const float spike_deg[] = {8, 83, 151, 218, 287};
    static const float spike_len[] = {0.98f, 0.78f, 0.92f, 0.72f, 0.86f};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1;
            const float r = std::sqrt(dx * dx + dy * dy), a = std::atan2(dy, dx);
            const float core = std::exp(-(r / 0.22f) * (r / 0.22f));
            const float glow = 0.8f * std::exp(-(r / 0.5f) * (r / 0.5f));
            float spike = 0;
            for (int k = 0; k < 5; ++k) {
                const float L = spike_len[k];
                if (r >= L) continue;
                float da = std::fabs(std::remainder(a - spike_deg[k] * kPi / 180, 2 * kPi));
                const float across = da * r;                  // distance from the spike's centre line
                const float w = 0.15f * (1 - r / L) + 0.012f;  // tapering to a point
                spike = std::fmax(spike, std::exp(-(across / w) * (across / w)) * std::pow(1 - r / L, 0.8f));
            }
            const float edge = clamp01((1 - r) / 0.12f);  // nothing at the quad's edge
            const float b = (core + glow + 1.1f * spike) * edge;
            px[size_t(y) * size + x] = flame(b, core + 0.4f * spike);
        }
    return px;
}

std::vector<uint32_t> flash_plume_image(int w, int h) {
    std::vector<uint32_t> px(size_t(w) * h, 0xFF000000u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h * 2 - 1;
            // Narrow at the muzzle, widest a third of the way out, closing to a point.
            const float hw = 0.16f + 0.74f * std::sin(kPi * std::pow(u, 0.75f));
            const float across = v / std::fmax(0.05f, hw);
            const float along = std::pow(clamp01(1 - u), 1.4f) * (0.65f + 0.35f * std::exp(-(u / 0.18f) * (u / 0.18f)));
            const float start = clamp01(u / 0.04f);  // no hard edge at the muzzle end
            const float b = std::exp(-3.2f * across * across) * along * start * clamp01((1 - std::fabs(v)) / 0.1f);
            px[size_t(y) * w + x] = flame(b * 1.1f, std::exp(-6 * across * across) * (1 - u));
        }
    return px;
}

// --- haptics ---
// Legacy pulses: strength is felt by length (at most 3999 us) and a frame gives at most one, so a
// thump is a run of long pulses over a few frames (3-4 at 120 Hz).

static const HapticStep kPistol[] = {{0, 22, 3999}, {22, 16, 1800}};
static const HapticStep kHandCannon[] = {{0, 35, 3999}, {35, 20, 2000}};
static const HapticStep kShotgun[] = {{0, 35, 3999}, {60, 30, 3999}, {90, 20, 1500}};
static const HapticStep kRifle[] = {{0, 12, 3200}, {12, 8, 1400}};
static const HapticStep kSmg[] = {{0, 25, 2600}};
static const HapticStep kMinigun[] = {{0, 10, 2000}};
static const HapticStep kCrossbow[] = {{0, 15, 3000}, {15, 20, 1200}};
static const HapticStep kHit[] = {{0, 0, 1300}};
static const HapticStep kDry[] = {{0, 0, 700}};
static const HapticStep kSeat[] = {{0, 0, 1500}};
static const HapticStep kForend[] = {{0, 0, 1500}};

const HapticStep* haptic_steps(Haptic h, int* n) {
    struct P {
        const HapticStep* s;
        int n;
    };
#define HV_P(a) {a, int(std::size(a))}
    static const P table[] = {HV_P(kPistol), HV_P(kHandCannon), HV_P(kShotgun), HV_P(kRifle), HV_P(kSmg), HV_P(kMinigun),
                              HV_P(kCrossbow), HV_P(kHit), HV_P(kDry), HV_P(kSeat), HV_P(kForend)};
#undef HV_P
    static_assert(std::size(table) == size_t(Haptic::count));
    if (size_t(h) >= std::size(table)) {
        *n = 0;
        return nullptr;
    }
    *n = table[size_t(h)].n;
    return table[size_t(h)].s;
}

void HapticPlayer::play(Haptic h, double now, float delay_ms) {
    int n = 0;
    const HapticStep* s = haptic_steps(h, &n);
    if (!s || n <= 0 || n > 32) return;
    if (active_.size() >= 32) active_.erase(active_.begin());
    active_.push_back({now, std::fmax(0.f, delay_ms) / 1000.0, false, s, n, 0});
}

unsigned HapticPlayer::tick(double now, float scale) {
    unsigned best = 0;
    for (size_t i = 0; i < active_.size();) {
        Entry& e = active_[i];
        // A pattern's clock starts at the first frame after it was queued (shots are queued mid-frame,
        // so its first step always gets that frame), its delay counted from then.
        if (!e.started) e.started = true, e.start = (std::max)(now, e.start) + e.delay;
        const float el = float((now - e.start) * 1000);
        bool done = true;
        for (int k = 0; k < e.n; ++k) {
            const HapticStep& st = e.steps[k];
            if (el < st.at_ms) {
                done = false;
                continue;
            }
            const uint32_t bit = 1u << k;
            const bool running = el < st.at_ms + st.dur_ms;
            if (!(e.fired & bit) || running) best = (std::max)(best, unsigned(st.us));
            e.fired |= bit;
            if (running) done = false;
        }
        if (done) active_.erase(active_.begin() + ptrdiff_t(i));
        else ++i;
    }
    if (!(scale > 0) || !best) return 0;
    const float us = std::fmin(float(kMaxPulseUs), std::round(float(best) * scale));
    return us >= 1 ? unsigned(us) : 0;
}

}  // namespace hv::vr
