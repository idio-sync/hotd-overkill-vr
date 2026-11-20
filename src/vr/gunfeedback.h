#pragma once
// Gun feedback stage 2 (docs/DEVELOPMENT.md, "Muzzle flash, recoil, haptic patterns"): the muzzle
// flash, the visual recoil and the haptic patterns, free of D3D/OpenVR so they can be unit-tested
// (tests/test_stereo.cpp). gunmodel.cpp draws the flash and kicks the drawn gun; vrinput.cpp plays
// the haptic patterns on the gun hand with legacy pulses.
//
// Nothing here touches the aim: the recoil moves the drawn gun only (the shot ray, the hit marker
// and the cursor stay on the un-kicked barrel), and the flash is drawn at the drawn muzzle.
#include <cstdint>
#include <vector>

#include "stereo.h"

namespace hv::vr {

// --- per weapon ---

enum class Haptic : uint8_t {
    // A shot of a weapon (one pattern per shot).
    pistol,      // magnum, gold magnum: a single strong thump
    handcannon,  // a bigger thump
    shotgun,     // pump, auto shotgun: a longer double thump
    rifle,       // a short thump per round of a burst
    smg,         // a short buzz per shot: a buzz at the fire rate while the trigger is held
    minigun,     // a very short buzz per shot (~40 shots a second): a continuous buzz
    crossbow,    // the string's release
    // Events.
    hit,     // the shot's ray hits a shootable entity: a light tick after the shot's pattern
    dry,     // the trigger pulled with an empty weapon (not reloading): a dry click
    seat,    // the magazine seats (or the hand cannon's cylinder swings back in) on a reload
    forend,  // the pump's forend closes (after each shot's rack and the reload's)
    count
};
const char* haptic_name(Haptic h);

struct FeedbackDef {
    const char* key;      // kDefs key (gunmodel.cpp)
    float flash_cm;       // the flash's radius at the muzzle, model cm (0: no flash)
    float plume;          // the flash's plume along the barrel, in radii
    bool flicker;         // small automatic flashes: size and rotation jitter every frame
    float recoil_deg;     // the drawn gun's kick up about the grip, degrees (peak)
    float recoil_cm;      // and back along the barrel, cm (peak)
    float recoil_tau;     // seconds to the kick's peak (the spring's time constant)
    float recoil_cap;     // overlapping kicks (automatic fire) are summed and capped at this many
    Haptic shot;          // the shot's haptic pattern
    float hit_ms;         // the hit tick's delay after the shot (after the shot's pattern)
};
// The weapon's feedback (key = kDefs key); an unknown key gets the pistol's.
const FeedbackDef& feedback_def(const char* key);
int feedback_count();
const FeedbackDef& feedback_at(int i);

// --- recoil ---

// One kick's shape: the impulse response of a critically damped spring, t/tau e^(1 - t/tau): 0
// at the shot, 1 at tau, back under 4% by 6.5 tau. 0 before the shot and after 10 tau.
float recoil_kick(float t, float tau);
// The kick of several recent shots (seconds since each): summed, then softly capped at `cap`
// (unchanged up to one kick or 70% of the cap, whichever is more), so automatic fire stays near a
// fixed kick instead of wandering.
float recoil_amount(const float* since, int n, float tau, float cap);
// The drawn gun's kick in the gun frame (gunpose.h: metres, barrel +z, up +y, muzzle at the
// origin): a turn by `deg` that lifts the muzzle about the grip point `grip_gun`, then `back_m`
// back along -z. Applied between gun_in_hand and model_to_gun for drawing only.
Mat4 recoil_in_gun(const float grip_gun[3], float deg, float back_m);

// --- muzzle flash ---

constexpr int kFlashFrames = 2;  // frames drawn per shot (a later frame never shows it)
struct FlashState {
    unsigned shot = 0;   // the weapon's shot count last seen
    int frame = 1000;    // frames drawn since that shot
    uint32_t seed = 0;
};
struct FlashFrame {
    bool on = false;
    int frame = 0;        // 0 or 1
    float scale = 0;      // times the weapon's flash_cm
    float rot_deg = 0;    // the star's turn about the view ray
    float intensity = 0;  // 0..1: brightness of the flash and the gun's lighting boost
};
// Once per frame the gun is drawn: a new shot count starts a flash (a new random rotation and size;
// a shot during a flash starts it again), which shows in this and the next frame only.
FlashFrame flash_step(FlashState* s, unsigned shot_count, const FeedbackDef& d);
// The flash of a given frame after a shot with a given seed (dev freezes and flash_step).
FlashFrame flash_frame(int frame, uint32_t seed, const FeedbackDef& d, uint32_t jitter);
uint32_t flash_hash(uint32_t x);

// The flash images, generated (no game files): A8R8G8B8 pixels for additive drawing (colour =
// light added; alpha unused). The star: a white-hot core, a warm glow and 5 uneven spikes, seen
// down the barrel or from the side. The plume: the flame along the barrel, the muzzle at u = 0.
std::vector<uint32_t> flash_star_image(int size);
std::vector<uint32_t> flash_plume_image(int w, int h);

// --- haptics ---

// A pattern step: a pulse of `us` (legacy pulse length, at most 3999) on the first frame at or
// after `at_ms` and on every later frame before at_ms + dur_ms.
struct HapticStep {
    float at_ms, dur_ms;
    unsigned short us;
};
const HapticStep* haptic_steps(Haptic h, int* n);

// Plays patterns: several can overlap (the strongest due step wins a frame), at most one pulse
// per frame, scaled by `haptics` (pulse lengths times the scale, capped at 3999 us; 0 = off). A
// pattern's times count from the first tick after it was queued (plus its delay).
class HapticPlayer {
public:
    void play(Haptic h, double now, float delay_ms = 0);
    // This frame's pulse (0: none). `now` in seconds (the same clock as play).
    unsigned tick(double now, float scale);
    void clear() { active_.clear(); }
    size_t active() const { return active_.size(); }

private:
    struct Entry {
        double start;  // queued at; from its first tick: when its steps' times count from
        double delay;  // seconds
        bool started;
        const HapticStep* steps;
        int n;
        uint32_t fired;  // bit per step: its first pulse was given
    };
    std::vector<Entry> active_;
};
constexpr unsigned kMaxPulseUs = 3999;

}  // namespace hv::vr
