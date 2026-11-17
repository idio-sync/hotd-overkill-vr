// Controllers -> the game (design: "Gun and input", "Menus"). Once per frame, after the poses
// were read (vrsys_wait_poses):
//
// - Buttons become the game's own mouse/keyboard input through the DirectInput state the mod
//   already injects (dinput.cpp; the game reads mouse buttons and keys with GetDeviceState):
//   trigger = fire (held for automatic weapons), [vr] btn_reload / btn_switch / btn_grenade /
//   btn_pause = the game inputs game_reload / game_switch / game_grenade / game_pause (defaults: the
//   game's own PC controls - middle click reload, TAB swap weapon, right click grenade, Escape
//   pause); btn_recentre held for a second recentres.
//   Optional reload_gesture: pulling the trigger while pointing far away from where you look
//   reloads instead of firing, like a light gun pointed off-screen.
// - Flat frames (menus, loadout, pause, death screen, cutscenes): the gun's ray against the flat
//   screen (vrsys_panel_in_anchor(true)); where it hits becomes the game's mouse cursor (the fake
//   GetCursorPos, in back-buffer pixels of the area the flat screen shows) and the trigger clicks.
// - Gameplay: the aim itself goes through gun.cpp (shot paths and the cursor setters).
// - Haptics: a pulse per player-1 shot (gun.cpp calls input_shot), longer for strong weapons.
//
// In VR modes, once a controller has been seen, the real mouse no longer moves the cursor or
// clicks (input_owns_pointer; dinput.cpp and window.cpp ask).
#include "common.h"

#include <atomic>
#include <cmath>
#include <sstream>

#include "../vr/aim.h"

namespace hv {

static std::atomic<bool> g_owns;
static bool g_sim_pointer;  // sim: off until a `vr hand*` command (the harness's `cursor` drives menus otherwise)
void input_sim_pointer(bool on) { g_sim_pointer = on; }
static std::atomic<int> g_pulse_frames;
static std::atomic<unsigned> g_pulse_us;

bool input_owns_pointer() { return g_owns.load(); }

void input_shot(float strength) {
    if (g_vr.haptics <= 0) return;
    const bool strong = strength > 1.5f;
    // Legacy pulses are felt by their length (max 3999 us per call): strong ones repeat a few frames.
    const unsigned us = unsigned(std::fmin(3999.f, (strong ? 3999.f : 2000.f) * std::fmin(1.f, g_vr.haptics)));
    const int frames = int(std::lround((strong ? 3 : 1) * std::fmax(1.f, g_vr.haptics)));
    g_pulse_us = us;
    g_pulse_frames = frames;
}

// --- game inputs ---

void game_input(const std::string& t, bool down) {
    if (t.empty() || t == "none") return;
    if (t == "lmb") return post_mouse_button(false, down);
    if (t == "rmb") return post_mouse_button(true, down);
    if (t == "mmb") return di_mouse_button(2, down);
    if (t == "wheelup" || t == "wheeldown") {
        if (down) di_mouse_wheel(t == "wheelup" ? 120 : -120);
        return;
    }
    if (UINT vk = key_from_name(t)) post_key(vk, down);
}

// --- controller buttons ---

// "gun.a,off.b" -> pressed on either.
static bool binding_down(const std::string& spec, const CtlState& gun, const CtlState& off) {
    std::stringstream ss(spec);
    std::string part;
    while (std::getline(ss, part, ',')) {
        const size_t dot = part.find('.');
        if (dot == std::string::npos) continue;
        const std::string hand = part.substr(0, dot), b = part.substr(dot + 1);
        const CtlState& c = hand == "off" ? off : gun;
        const uint32_t bit = b == "a" ? kBtnA : b == "b" ? kBtnB : b == "grip" ? kBtnGrip : b == "stick" ? kBtnStick : b == "trigger" ? kBtnTrigger : 0;
        if (bit && (c.buttons & bit)) return true;
    }
    return false;
}

// Sim controller buttons (`vr btn`, `vr hold`, `vr release`): names trigger, a, b, grip, stick,
// with "off." for the other hand.
static uint32_t g_sim_buttons[2];
void input_sim_button(const std::string& name, bool down) {
    const bool off = name.rfind("off.", 0) == 0;
    const std::string b = off ? name.substr(4) : name.rfind("gun.", 0) == 0 ? name.substr(4) : name;
    const uint32_t bit = b == "trigger" ? kBtnTrigger : b == "a" ? kBtnA : b == "b" ? kBtnB : b == "grip" ? kBtnGrip : b == "stick" ? kBtnStick : 0;
    if (!bit && name != "all") {
        logf("vr btn: unknown button %s", name.c_str());
        return;
    }
    uint32_t& s = g_sim_buttons[off ? 1 : 0];
    if (name == "all") g_sim_buttons[0] = g_sim_buttons[1] = 0;
    else s = down ? (s | bit) : (s & ~bit);
    for (int h = 0; h < 2; ++h) vrsys_sim_buttons(h == 0, g_sim_buttons[h] & ~kBtnTrigger, (g_sim_buttons[h] & kBtnTrigger) ? 1.f : 0.f);
}

struct Action {
    const char* name;
    const std::string* buttons;  // [vr] btn_*
    const std::string* game;     // [vr] game_*
    bool down = false;
};

// --- menu pointing ---

static bool point_at_screen(int* px, int* py) {
    RECT r;
    vr::Mat4 hand;
    if (!stereo_flat_rect(&r) || !vrsys_hand_in_anchor(&hand)) return false;
    const float w = float(r.right - r.left), h = float(r.bottom - r.top);
    if (w <= 0 || h <= 0) return false;
    float u, v;
    if (!vr::ray_panel_uv(vrsys_panel_in_anchor(true), g_vr.screen_width, g_vr.screen_width * h / w, vr::pose_ray_rh(hand), &u, &v))
        return false;
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    *px = r.left + int(std::lround(u * w - 0.5f));
    *py = r.top + int(std::lround(v * h - 0.5f));
    return true;
}

// Sim `vr handscreen X Y`: point the hand at back-buffer pixel (X, Y) of the flat screen.
void input_aim_at_screen(float x, float y) {
    RECT r;
    if (!stereo_flat_rect(&r)) {
        logf("vr handscreen: no flat frame shown yet");
        return;
    }
    const float w = float(r.right - r.left), h = float(r.bottom - r.top);
    float p[3];
    vr::panel_point(vrsys_panel_in_anchor(true), g_vr.screen_width, g_vr.screen_width * h / w, (x + 0.5f - r.left) / w,
                    (y + 0.5f - r.top) / h, p);
    vrsys_sim_aim_at(p);
}

// Degrees between the gun's direction and where the head looks (reload gesture).
static float aim_off_view_deg() {
    vr::Mat4 hand, head;
    if (!vrsys_hand_in_anchor(&hand) || !vrsys_head_in_anchor(&head)) return 0;
    const vr::Ray a = vr::pose_ray_rh(hand), b = vr::pose_ray_rh(head);
    const float c = a.d[0] * b.d[0] + a.d[1] * b.d[1] + a.d[2] * b.d[2];
    return std::acos(std::fmax(-1.f, std::fmin(1.f, c))) * 180 / 3.14159265f;
}

void input_frame() {
    if (g_vr.mode == VrMode::off) return;
    CtlState gun, off;
    const bool any = vrsys_controller(true, &gun) | vrsys_controller(false, &off);
    if (any && !g_owns) logf("input: controllers seen; the real mouse no longer moves the cursor or clicks");
    if (any) g_owns = true;

    // Haptics.
    if (g_pulse_frames.load() > 0) {
        --g_pulse_frames;
        vrsys_haptic(g_pulse_us.load());
    }

    const bool gameplay = stereo_frame_active();
    // Menus: the gun points at the flat screen.
    static bool pointing;
    if (!gameplay && (vrsys_openvr() || g_sim_pointer)) {
        int x, y;
        const bool hit = point_at_screen(&x, &y);
        int fx, fy;
        if (hit && (!get_fake_cursor(&fx, &fy) || fx != x || fy != y)) set_fake_cursor(x, y);
        if (hit != pointing) logf("input: pointing %s the flat screen%s", hit ? "at" : "away from", hit ? "" : " (cursor stays)");
        pointing = hit;
    } else {
        pointing = false;
    }

    // Trigger -> fire (or the reload gesture).
    static bool fire_down, gesture_down;
    const bool trig = (gun.buttons & kBtnTrigger) != 0;
    if (trig && !fire_down && !gesture_down) {
        if (gameplay && g_vr.reload_gesture && aim_off_view_deg() > g_vr.reload_gesture_deg) {
            gesture_down = true;
            logf("input: reload gesture (aim %.0f deg off the view)", aim_off_view_deg());
            game_input(g_vr.game_reload, true);
        } else {
            fire_down = true;
            game_input(g_vr.game_fire, true);
        }
    } else if (!trig) {
        if (fire_down) game_input(g_vr.game_fire, false);
        if (gesture_down) game_input(g_vr.game_reload, false);
        fire_down = gesture_down = false;
    }

    // Other actions.
    static Action actions[] = {{"reload", &g_vr.btn_reload, &g_vr.game_reload},
                               {"switch weapon", &g_vr.btn_switch, &g_vr.game_switch},
                               {"grenade", &g_vr.btn_grenade, &g_vr.game_grenade},
                               {"pause", &g_vr.btn_pause, &g_vr.game_pause}};
    for (Action& a : actions) {
        const bool d = binding_down(*a.buttons, gun, off);
        if (d != a.down) {
            logf("input: %s %s (%s)", a.name, d ? "pressed" : "released", a.game->c_str());
            game_input(*a.game, d);
            a.down = d;
        }
    }

    // Recentre: held for a second.
    static ULONGLONG held_since;
    if (binding_down(g_vr.btn_recentre, gun, off)) {
        const ULONGLONG now = GetTickCount64();
        if (!held_since) held_since = now;
        if (held_since != ~0ull && now - held_since >= 1000) {
            logf("input: recentre (button held)");
            vrsys_recentre();
            held_since = ~0ull;  // once per hold
        }
    } else {
        held_since = 0;
    }
}

}  // namespace hv
