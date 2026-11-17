// The mod's settings: hovr\hovr.ini, section [vr] (the probe's harness keeps hovr\probe.ini
// [probe]). Read once at load; `vr ...` commands in the command file change some of them live.
//
//   [vr]
//   mode      = off | sim | openvr   off: the game untouched (default). sim: no headset, both eyes
//                                    side by side on the desktop window, head pose from sim_yaw/
//                                    sim_pitch. openvr: not implemented yet (treated as off).
//   ipd_m     = 0.064                eye separation, metres
//   unit_m    = 0.01                 metres per game unit (the game works in centimetres)
//   sim_yaw   = 0                    simulated head yaw, degrees (+ = right)
//   sim_pitch = 0                    simulated head pitch, degrees (+ = up)
//   cone_deg  = 35                   head-turn range around the rail camera that culling covers
//   eye_res   = 1280x720             sim: per-eye resolution of the stereo targets
//   uncap     = 1                    clear the game's 60 Hz frame limiter (sim/openvr only)
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
        logf("vr: mode=openvr is not implemented yet; running as mode=off");
        c.mode = VrMode::off;
    } else if (_wcsicmp(mode.c_str(), L"off") != 0) {
        logf("vr: unknown mode '%ls'; running as mode=off", mode.c_str());
    }
    c.ipd_m = ini_float(L"ipd_m", c.ipd_m);
    c.unit_m = ini_float(L"unit_m", c.unit_m);
    if (c.unit_m <= 0) c.unit_m = 0.01f;
    c.sim_yaw_deg = ini_float(L"sim_yaw", c.sim_yaw_deg);
    c.sim_pitch_deg = ini_float(L"sim_pitch", c.sim_pitch_deg);
    c.cone_deg = ini_float(L"cone_deg", c.cone_deg);
    std::wstring res = ini_str(L"eye_res", L"1280x720");
    int w = 0, h = 0;
    if (swscanf(res.c_str(), L"%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) {
        c.eye_w = w;
        c.eye_h = h;
    } else {
        logf("vr: bad eye_res '%ls'; using %dx%d", res.c_str(), c.eye_w, c.eye_h);
    }
    c.uncap = ini_float(L"uncap", 1) != 0;
    c.stereo = c.mode != VrMode::off;
    g_vr = c;
    logf("vr: mode=%s ipd_m=%.4f unit_m=%.4f sim_yaw=%.1f sim_pitch=%.1f cone_deg=%.1f eye_res=%dx%d uncap=%d",
         mode_name(c.mode), c.ipd_m, c.unit_m, c.sim_yaw_deg, c.sim_pitch_deg, c.cone_deg, c.eye_w, c.eye_h, int(c.uncap));
}

}  // namespace hv
