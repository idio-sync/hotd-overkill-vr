// The mod's settings: hovr\hovr.ini, section [vr] (the probe's harness keeps hovr\probe.ini
// [probe]). Read once at load; `vr ...` commands in the command file change some of them live.
//
//   [vr]
//   mode      = off | sim | openvr   off: the game untouched (default). sim: no headset; the desktop
//                                    window shows both eyes with the HUD panel (gameplay) or the
//                                    flat screen (menus, cutscenes, pause, death); head pose from
//                                    sim_yaw/sim_pitch. openvr: SteamVR (holding Shift while the
//                                    game starts, or OpenVR failing to start, means off).
//   ipd_m     = 0.064                sim eye separation, metres (openvr: the headset's)
//   unit_m    = 0.01                 metres per game unit (the game works in centimetres)
//   sim_yaw   = 0                    simulated head yaw, degrees (+ = right)
//   sim_pitch = 0                    simulated head pitch, degrees (+ = up)
//   cone_deg  = 35                   head-turn range around the rail camera that culling covers
//   eye_res   = auto                 per-eye resolution of the stereo targets: WxH, or auto
//                                    (sim 1280x720; openvr: SteamVR's recommended size x eye_scale)
//   eye_scale = 1.0                  openvr, eye_res=auto: multiplier on the recommended size
//   uncap     = 1                    clear the game's 60 Hz frame limiter in gameplay (sim/openvr)
//   crosshair = 0                    1: keep the game's crosshair on the HUD panel in gameplay
//   hud_distance = 1.5, hud_width = 1.3, hud_drop = 0       HUD panel: metres ahead, wide, below eye height
//   screen_distance = 3.0, screen_width = 3.2, screen_drop = 0   flat screen for menus/cutscenes
//   mirror    = 1                    desktop window shows the VR view (0: the game's own frame)
//   handoff   = 1                    sim: also run the D3D9Ex -> D3D11 hand-off (openvr: always)
//   handoff_sync = 1                 wait for the D3D9 frame on the GPU before D3D11 reads it
//
//   Gun and input (vrinput.cpp, gun.cpp):
//   aim       = 1                    the controller aims the game's shots (0: the mouse aims)
//   hand      = right | left         the controller that is the gun
//   hand_pitch = 0                   degrees the aim is tilted up from the controller's pointing axis
//                                    (its render model's tip when SteamVR reports one)
//   hand_offset = 0,0,0              aim origin in the controller's frame, metres (x right, y up, z back)
//   haptics   = 1.0                  shot pulse strength (0: off; shotguns and the hand cannon pulse harder)
//   marker    = 0                    1: a small dot in the eyes where the shot will land
//   reload_gesture = 0               1: pulling the trigger while pointing more than reload_gesture_deg
//   reload_gesture_deg = 60          (degrees) away from where you look reloads, like a light gun off-screen
//   btn_reload = gun.a,gun.grip      controller buttons for the game's actions: gun. (the gun hand) or
//   btn_switch = gun.b               off. (the other hand) + a | b | grip | stick | trigger; a = A/X,
//   btn_pause  = off.b,off.a         b = B/Y (SteamVR's application-menu button); comma = either
//   btn_recentre = gun.stick         held for a second: recentre the view
//   game_fire = lmb, game_reload = rmb, game_switch = wheeldown, game_pause = ESCAPE
//                                    what each action sends to the game (lmb, rmb, mmb, wheelup,
//                                    wheeldown, or a key name: RETURN, SPACE, A-Z, 0-9, F1..., 0xNN)
//   sim_hand  = 0.15,-0.25,-0.35     sim hand position relative to the head, metres (x right, y up, z back)
//   sim_hand_yaw = 0, sim_hand_pitch = 0   sim hand direction relative to the head, degrees
#include "common.h"

#include <cwchar>

namespace hv {

VrConfig g_vr;

static std::wstring ini_path() { return path_in(L"hovr.ini"); }

static std::wstring ini_str(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[128];
    GetPrivateProfileStringW(L"vr", key, def, buf, 128, ini_path().c_str());
    return buf;
}

static void ini_vec3(const wchar_t* key, float v[3]) {
    std::wstring s = ini_str(key, L"");
    float t[3];
    if (!s.empty() && swscanf(s.c_str(), L"%f,%f,%f", &t[0], &t[1], &t[2]) == 3)
        for (int i = 0; i < 3; ++i) v[i] = t[i];
}

static std::string ini_ascii(const wchar_t* key, const std::string& def) {
    std::wstring w(def.begin(), def.end());
    w = ini_str(key, w.c_str());
    std::string out;
    for (wchar_t c : w)
        if (c != L' ') out += char(c);
    return out;
}

static float ini_float(const wchar_t* key, float def) {
    std::wstring s = ini_str(key, L"");
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : float(v);
}

const char* mode_name(VrMode m) {
    switch (m) {
        case VrMode::sim: return "sim";
        case VrMode::openvr: return "openvr";
        default: return "off";
    }
}

void load_vr_config() {
    VrConfig c;
    std::wstring mode = ini_str(L"mode", L"off");
    if (_wcsicmp(mode.c_str(), L"sim") == 0) {
        c.mode = VrMode::sim;
    } else if (_wcsicmp(mode.c_str(), L"openvr") == 0) {
        c.mode = VrMode::openvr;  // OpenVR starts at Direct3DCreate9 (vrsys.cpp); failure -> off
    } else if (_wcsicmp(mode.c_str(), L"off") != 0) {
        logf("vr: unknown mode '%ls'; running as mode=off", mode.c_str());
    }
    c.ipd_m = ini_float(L"ipd_m", c.ipd_m);
    c.unit_m = ini_float(L"unit_m", c.unit_m);
    if (c.unit_m <= 0) c.unit_m = 0.01f;
    c.sim_yaw_deg = ini_float(L"sim_yaw", c.sim_yaw_deg);
    c.sim_pitch_deg = ini_float(L"sim_pitch", c.sim_pitch_deg);
    c.cone_deg = ini_float(L"cone_deg", c.cone_deg);
    std::wstring res = ini_str(L"eye_res", L"auto");
    int w = 0, h = 0;
    if (_wcsicmp(res.c_str(), L"auto") == 0) {
        c.eye_res_auto = true;
    } else if (swscanf(res.c_str(), L"%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) {
        c.eye_w = w;
        c.eye_h = h;
        c.eye_res_auto = false;
    } else {
        logf("vr: bad eye_res '%ls'; using %dx%d", res.c_str(), c.eye_w, c.eye_h);
    }
    c.eye_scale = ini_float(L"eye_scale", c.eye_scale);
    if (c.eye_scale < 0.25f || c.eye_scale > 3) c.eye_scale = 1;
    c.uncap = ini_float(L"uncap", 1) != 0;
    c.crosshair = ini_float(L"crosshair", 0) != 0;
    c.hud_distance = ini_float(L"hud_distance", c.hud_distance);
    c.hud_width = ini_float(L"hud_width", c.hud_width);
    c.hud_drop = ini_float(L"hud_drop", c.hud_drop);
    c.screen_distance = ini_float(L"screen_distance", c.screen_distance);
    c.screen_width = ini_float(L"screen_width", c.screen_width);
    c.screen_drop = ini_float(L"screen_drop", c.screen_drop);
    c.mirror = ini_float(L"mirror", 1) != 0;
    c.handoff = ini_float(L"handoff", 1) != 0;
    c.handoff_sync = ini_float(L"handoff_sync", 1) != 0;
    c.aim = ini_float(L"aim", 1) != 0;
    c.left_hand = _wcsicmp(ini_str(L"hand", L"right").c_str(), L"left") == 0;
    c.hand_pitch = ini_float(L"hand_pitch", c.hand_pitch);
    ini_vec3(L"hand_offset", c.hand_offset);
    c.haptics = ini_float(L"haptics", c.haptics);
    c.marker = ini_float(L"marker", 0) != 0;
    c.reload_gesture = ini_float(L"reload_gesture", 0) != 0;
    c.reload_gesture_deg = ini_float(L"reload_gesture_deg", c.reload_gesture_deg);
    ini_vec3(L"sim_hand", c.sim_hand);
    c.sim_hand_yaw = ini_float(L"sim_hand_yaw", 0);
    c.sim_hand_pitch = ini_float(L"sim_hand_pitch", 0);
    c.btn_reload = ini_ascii(L"btn_reload", c.btn_reload);
    c.btn_switch = ini_ascii(L"btn_switch", c.btn_switch);
    c.btn_pause = ini_ascii(L"btn_pause", c.btn_pause);
    c.btn_recentre = ini_ascii(L"btn_recentre", c.btn_recentre);
    c.game_fire = ini_ascii(L"game_fire", c.game_fire);
    c.game_reload = ini_ascii(L"game_reload", c.game_reload);
    c.game_switch = ini_ascii(L"game_switch", c.game_switch);
    c.game_pause = ini_ascii(L"game_pause", c.game_pause);
    c.stereo = c.mode != VrMode::off;
    g_vr = c;
    logf("vr: mode=%s ipd_m=%.4f unit_m=%.4f sim_yaw=%.1f sim_pitch=%.1f cone_deg=%.1f eye_res=%s%dx%d uncap=%d crosshair=%d",
         mode_name(c.mode), c.ipd_m, c.unit_m, c.sim_yaw_deg, c.sim_pitch_deg, c.cone_deg, c.eye_res_auto ? "auto " : "", c.eye_w,
         c.eye_h, int(c.uncap), int(c.crosshair));
    logf("vr: hud %.2f m wide at %.2f m (drop %.2f), screen %.2f m wide at %.2f m, mirror=%d handoff=%d handoff_sync=%d",
         c.hud_width, c.hud_distance, c.hud_drop, c.screen_width, c.screen_distance, int(c.mirror), int(c.handoff), int(c.handoff_sync));
    logf("vr: aim=%d hand=%s hand_pitch=%.1f hand_offset=%.3f,%.3f,%.3f haptics=%.2f marker=%d reload_gesture=%d (%.0f deg)", int(c.aim),
         c.left_hand ? "left" : "right", c.hand_pitch, c.hand_offset[0], c.hand_offset[1], c.hand_offset[2], c.haptics, int(c.marker),
         int(c.reload_gesture), c.reload_gesture_deg);
    logf("vr: buttons reload=%s switch=%s pause=%s recentre=%s; game fire=%s reload=%s switch=%s pause=%s", c.btn_reload.c_str(),
         c.btn_switch.c_str(), c.btn_pause.c_str(), c.btn_recentre.c_str(), c.game_fire.c_str(), c.game_reload.c_str(),
         c.game_switch.c_str(), c.game_pause.c_str());
}

}  // namespace hv
