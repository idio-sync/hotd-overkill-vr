// Command file hovr\cmd.txt: tools\cmd.ps1 writes it, control_tick() (once per Present, on
// the render thread) appends its lines to a queue and runs them in order.
//
//   wait MS                 pause the queue
//   pad BTNS [MS]           hold buttons (A, B, X, Y, START, BACK, UP, DOWN, LEFT, RIGHT, LB, RB, LS, RS;
//                           joined with +) for MS (default 120), release, then wait 200 ms
//   hold BTNS / release     hold until release
//   stick LX LY [MS]        left stick (-1..1) for MS (default 300), then centred
//   rstick RX RY [MS]       right stick
//   trig LT RT [MS]         triggers 0..255
//   cursor X Y | cursor off fake cursor in window client pixels
//   move DX DY              relative DInput mouse motion
//   wheel DZ                DInput mouse wheel (120 = one notch up, -120 down)
//   click [left|right|middle] [MS]  mouse button press
//   key NAME [MS]           key press (RETURN, ESCAPE, SPACE, UP..., F1..., A-Z, 0-9, or 0xNN)
//   type TEXT               one key press per character
//   shot [NAME]             back buffer PNG (hovr\NAME.png, default shot_N)
//   frame [all]             log every call of the next frame to hovr\frame_N.txt (all: every
//                           uploaded constant register, not just the first 12 of each upload)
//   peek ADDR [N]           log N dwords (hex and float); ADDR = 0x..., va:0x..., exe+0x..., [ADDR]+0xOFF
//   poke ADDR VALUE         write a dword (0x...) or float (1.5)
//   aimdir X Y Z | off      replace the shot direction in FireBullets (world space)
//   aimdir capture          the next shot's direction becomes the override for the shots after it
//   aimorigin X Y Z | off   replace the shot origin
//   dump ADDR SIZE NAME     raw bytes to hovr\NAME.bin (for diffing game state)
//   findvt VA               log heap objects whose vtable is VA (va:0x... for an image VA)
//   log TEXT                write TEXT to the log
//   vr yaw D | vr pitch D   simulated head yaw (+ right) / pitch (+ up), degrees
//   vr ipd M | vr unit M    eye separation in metres / metres per game unit
//   vr cone D               head-turn range that culling covers, degrees
//   vr mode off|sim         stop/start drawing both eyes (target sizes are fixed at start-up by
//                           hovr.ini [vr] mode; started with mode=off, `vr mode sim` does nothing)
//   vr scene [NAME]         PNG of the stereo scene target at full size (hovr\NAME.png)
//   vr targets              PNGs of every resized colour target (hovr\rtN_WxH.png, alpha in _a.png)
//   vr check                compare the engine's projection*view with the draws' matrices
//   vr handoff [NAME]       PNGs of what the D3D11 side holds this frame: NAME_eyes + NAME_hud
//                           (gameplay) or NAME_screen (flat), with _a.png alpha images
//   vr recentre             recentre the head (yaw-only anchor at the current head pose)
//   vr state                log the game state (level, cutscene, paused) and the frame mode
//   vr crosshair 0|1        hide/show the game's crosshair in stereo gameplay
//   vr hand YAW PITCH       sim gun hand direction relative to the head, degrees (+ right / + up)
//   vr handpos X Y Z        sim gun hand position relative to the head, metres (x right, y up, z back)
//   vr handat X Y           sim: point the hand at what the game camera sees through cursor X, Y
//                           (the game's cursor space: composite pixels from the centre, +y up)
//   vr handscreen X Y       sim: point the hand at back-buffer pixel X, Y of the flat screen
//   vr handseek             sim: point the hand at the shootable entity (zombie) nearest the view's
//                           centre, found by ray casting a grid of the game camera's view
//   vr hand off             sim: the hand stops driving the menu cursor (`vr hand*` turns it on)
//   vr btn NAME [MS]        sim controller button press (trigger, a, b, grip, stick; off.NAME for
//                           the other hand), default 120 ms, then wait 200 ms
//   vr hold NAME | vr release NAME|all   hold / release sim controller buttons
//   vr marker 0|1           hit marker in the eyes
//   vr aim 0|1              controller aim on/off (0: the mouse aims)
//   vr gesture 0|1          reload gesture on/off
//   vr gun                  log player 1's weapon, ammo and the controller aim
//   vr god 0|1              dev: keep player 1's health full (player+0x20C)
//   vr arms|shake|bob 0|1   switch game_arms / camera_shake / head_bob live (and log)
//   vr comfort              log the comfort state (arms draws hidden, largest shake offsets, head-bob, edge-pan,
//                           the rail camera comfort settings)
//   vr comfort on|off       rail camera comfort (vignette, smoothing, levelling) all on / all off
//   vr comfort vignette X   vignette strength at full speed, 0..1 (0 = off)
//   vr comfort turn A B     rail turn speed (deg/s) where the vignette starts / is full
//   vr comfort move A B     rail speed (m/s) where the vignette starts / is full
//   vr comfort ease MS      vignette fade in/out time
//   vr comfort smooth MS    rotation smoothing time constant (0 = off)
//   vr comfort level N      horizon levelling: 0 as the game, 1 roll removed, 2 roll and pitch removed
//   vr comfort stats [S]    the rail camera's turn/move speed percentiles over the last S seconds (30),
//                           how often the vignette was on, the smoothing lag, the camera's roll
//   vr comfort wait [X] [MS]  dev: the queue waits until the vignette reaches X (0.1); if MS (60000) pass
//                           first, the lines up to the next `vr comfort end` are skipped
//   vr camlog [N]           dev: log the camera position and forward axis for the next N frames (120)
//   vr shakelog 0|1         dev: log every non-zero camera shake/smack offset
//   vr actors 0|1           dev: log each actor drawn through the entity draw 0x7697A0 (once per actor)
//   vr wiiview 0|1          dev: 1 lets the game's WiiView edge-pan through (to compare; VR keeps it off)
//   vr waithit [MS]         dev: the queue waits until player 1 is hit (or MS, default 30000, pass)
//   vr measure              dev: world-scale check - ray casts from the camera (floor, ceiling, walls) and
//                           the height of the zombie nearest the view's centre
//   vr actions 0|1          dev: log the game's input action values (fire 3, reload 5, weapon 0xB/0xC)
//   vr gunmodel [0|1]       log the gun model state (and switch drawing it off/on)
//   vr gunset W FIELD V...  per-weapon tuning live: W = magnum, gold_magnum, handcannon, pump,
//                           autoshotgun, smg, rifle, minigun, crossbow; FIELD = show 0|1, clip 0|1|2,
//                           grip X Y Z, barrel DX DY DZ, muzzle X Y Z, clipdir DX DY DZ (model cm),
//                           hold YAW PITCH (degrees)
//   vr gunview NAME         sim: look at the gun in the hand: aim, back, right, left, front, top, bottom
//   vr barrelline 0|1       dev: a red line along the barrel to the aim point
//   vr clipout F            dev: hold the magazine F (0..1) of the way out; no F: from the reload state
//   findstr TEXT            dev: heap addresses holding TEXT (slow: ~1 min)
//   drawstack HASH [N]      dev: call stacks and bound streams of the next N draws with vertex shader HASH
//   vr                      log the current VR settings
//   fakevr CMD...           a command for the fake SteamVR runtime (src/fakevr; tests only), e.g.
//                           `fakevr head 0 1.6 0 30 0 0`, `fakevr capture NAME`
#include "common.h"

#include <cstdio>
#include <deque>
#include <sstream>

namespace hv {

static std::deque<std::string> g_queue;
static ULONGLONG g_wait_until;
static int g_wait_hits = -1;  // `vr waithit`: the queue waits until player 1's hit count passes this
static float g_wait_vignette = -1;  // `vr comfort wait X`: the queue waits until the vignette reaches X
static WORD g_buttons;
static int g_shot_no;

void write_status(const char* text) {
    std::wstring tmp = path_in(L"status.tmp"), dst = path_in(L"status.txt");
    if (FILE* f = _wfopen(tmp.c_str(), L"w")) {
        fprintf(f, "%s\n", text);
        fclose(f);
        MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

static WORD parse_buttons(const std::string& s) {
    static const struct { const char* n; WORD b; } names[] = {
        {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
        {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK}, {"UP", XINPUT_GAMEPAD_DPAD_UP},
        {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}};
    WORD out = 0;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, '+')) {
        bool found = false;
        for (auto& n : names)
            if (_stricmp(n.n, part.c_str()) == 0) {
                out |= n.b;
                found = true;
            }
        if (!found) logf("unknown button %s", part.c_str());
    }
    return out;
}

UINT key_from_name(const std::string& s) {
    static const struct { const char* n; UINT vk; } names[] = {
        {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE}, {"SPACE", VK_SPACE},
        {"TAB", VK_TAB}, {"BACK", VK_BACK}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SHIFT", VK_SHIFT}, {"CONTROL", VK_CONTROL}, {"CTRL", VK_CONTROL}, {"ALT", VK_MENU}};
    for (auto& n : names)
        if (_stricmp(n.n, s.c_str()) == 0) return n.vk;
    if (s.size() >= 2 && (s[0] == 'F' || s[0] == 'f') && isdigit(uint8_t(s[1]))) return VK_F1 + atoi(s.c_str() + 1) - 1;
    if (s.rfind("0x", 0) == 0) return UINT(strtoul(s.c_str(), nullptr, 16));
    if (s.size() == 1) return UINT(toupper(uint8_t(s[0])));
    logf("unknown key %s", s.c_str());
    return 0;
}

// ADDR: 0x... (absolute), va:0x... (image VA at base 0x400000, rebased), exe+0x... (RVA),
// or [ADDR]+0xOFF (dereference, then add).
static uintptr_t parse_addr(const std::string& s) {
    if (!s.empty() && s[0] == '[') {
        size_t close = 0;  // the matching bracket (they nest: [[va:0x99B708]+0x24]+0x224)
        for (int depth = 0; close < s.size(); ++close) {
            if (s[close] == '[') ++depth;
            else if (s[close] == ']' && --depth == 0) break;
        }
        uintptr_t inner = parse_addr(s.substr(1, close - 1));
        if (!readable(inner)) return 0;
        uintptr_t p = *reinterpret_cast<const uintptr_t*>(inner);
        if (close + 1 < s.size() && s[close + 1] == '+') p += strtoul(s.c_str() + close + 2, nullptr, 16);
        return p;
    }
    if (s.rfind("exe+", 0) == 0) return exe_base() + strtoul(s.c_str() + 4, nullptr, 16);
    if (s.rfind("va:", 0) == 0) return exe_base() + strtoul(s.c_str() + 3, nullptr, 16) - 0x400000;
    return strtoul(s.c_str(), nullptr, 16);
}

static SHORT axis(float v) { return SHORT((std::max)(-1.f, (std::min)(1.f, v)) * 32767); }

static SHORT g_lx, g_ly, g_rx, g_ry;
static BYTE g_lt, g_rt;
static void apply_pad() { pad_set(g_buttons, g_lx, g_ly, g_rx, g_ry, g_lt, g_rt); }

static void push_front(std::initializer_list<std::string> lines) {
    for (auto it = std::rbegin(lines); it != std::rend(lines); ++it) g_queue.push_front(*it);
}

static void read_cmd_file() {
    std::wstring path = path_in(L"cmd.txt");
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        g_queue.push_back(s);
        ++n;
    }
    fclose(f);
    DeleteFileW(path.c_str());
    logf("cmd.txt: %d commands queued", n);
}

FrameRequests control_tick() {
    FrameRequests req;
    if (!dev_harness()) return req;  // release: no command file
    static ULONGLONG last_check;
    ULONGLONG now = GetTickCount64();
    if (now - last_check >= 100) {
        last_check = now;
        read_cmd_file();
    }
    if (g_wait_hits >= 0 && (gun_hits() > g_wait_hits || now >= g_wait_until)) {
        logf("vr waithit: %s", gun_hits() > g_wait_hits ? "player 1 was hit" : "timed out");
        g_wait_hits = -1;
        g_wait_until = now;
    }
    if (g_wait_vignette >= 0 && (comfort_vignette_now() >= g_wait_vignette || now >= g_wait_until)) {
        const bool reached = comfort_vignette_now() >= g_wait_vignette;
        comfort_log_now(reached ? "vr comfort wait: reached" : "vr comfort wait: timed out; skipping to `vr comfort end`");
        if (!reached) {  // drop the lines up to the next `vr comfort end`
            while (!g_queue.empty()) {
                const std::string l = g_queue.front();
                g_queue.pop_front();
                if (l.rfind("vr comfort end", 0) == 0) break;
            }
        }
        g_wait_vignette = -1;
        g_wait_until = now;
    }
    while (!g_queue.empty() && now >= g_wait_until && g_wait_hits < 0 && g_wait_vignette < 0) {
        std::string line = g_queue.front();
        g_queue.pop_front();
        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd != "wait" && cmd != "_release" && cmd != "_stick0" && cmd != "_rstick0" && cmd != "_trig0" && cmd != "_keyup" && cmd != "_clickup" &&
            cmd != "_vrbtnup")
            logf("cmd: %s", line.c_str());
        if (cmd == "wait") {
            int ms = 0;
            ss >> ms;
            g_wait_until = now + ms;
        } else if (cmd == "pad") {
            std::string b;
            int ms = 120;
            ss >> b >> ms;
            g_buttons |= parse_buttons(b);
            apply_pad();
            push_front({"wait " + std::to_string(ms), "_release " + b, "wait 200"});
        } else if (cmd == "hold") {
            std::string b;
            ss >> b;
            g_buttons |= parse_buttons(b);
            apply_pad();
        } else if (cmd == "release" || cmd == "_release") {
            std::string b;
            ss >> b;
            g_buttons = b.empty() ? 0 : WORD(g_buttons & ~parse_buttons(b));
            apply_pad();
        } else if (cmd == "stick" || cmd == "rstick") {
            float x = 0, y = 0;
            int ms = 300;
            ss >> x >> y >> ms;
            (cmd == "stick" ? g_lx : g_rx) = axis(x);
            (cmd == "stick" ? g_ly : g_ry) = axis(y);
            apply_pad();
            push_front({"wait " + std::to_string(ms), cmd == "stick" ? "_stick0" : "_rstick0"});
        } else if (cmd == "_stick0") {
            g_lx = g_ly = 0;
            apply_pad();
        } else if (cmd == "_rstick0") {
            g_rx = g_ry = 0;
            apply_pad();
        } else if (cmd == "trig") {
            int lt = 0, rt = 0, ms = 200;
            ss >> lt >> rt >> ms;
            g_lt = BYTE(lt);
            g_rt = BYTE(rt);
            apply_pad();
            push_front({"wait " + std::to_string(ms), "_trig0"});
        } else if (cmd == "_trig0") {
            g_lt = g_rt = 0;
            apply_pad();
        } else if (cmd == "cursor") {
            std::string a;
            int y = 0;
            ss >> a >> y;
            if (a == "off") set_fake_cursor(-1, -1);
            else set_fake_cursor(atoi(a.c_str()), y);
        } else if (cmd == "move") {
            LONG dx = 0, dy = 0;
            ss >> dx >> dy;
            di_mouse_move(dx, dy);
        } else if (cmd == "wheel") {
            LONG dz = 0;
            ss >> dz;
            di_mouse_wheel(dz);
        } else if (cmd == "click") {
            std::string which = "left";
            int ms = 80;
            ss >> which >> ms;
            if (which == "middle") di_mouse_button(2, true);
            else post_mouse_button(which == "right", true);
            push_front({"wait " + std::to_string(ms), "_clickup " + which});
        } else if (cmd == "_clickup") {
            std::string which;
            ss >> which;
            if (which == "middle") di_mouse_button(2, false);
            else post_mouse_button(which == "right", false);
        } else if (cmd == "_vrbtnup") {
            std::string name;
            ss >> name;
            input_sim_button(name, false);
        } else if (cmd == "key") {
            std::string k;
            int ms = 80;
            ss >> k >> ms;
            UINT vk = key_from_name(k);
            if (vk) {
                post_key(vk, true);
                push_front({"wait " + std::to_string(ms), "_keyup " + std::to_string(vk), "wait 60"});
            }
        } else if (cmd == "_keyup") {
            UINT vk = 0;
            ss >> vk;
            post_key(vk, false);
        } else if (cmd == "type") {
            std::string text = line.size() > 5 ? line.substr(5) : "";
            std::vector<std::string> keys;
            for (char c : text) keys.push_back(c == ' ' ? "key SPACE" : std::string("key ") + c);
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) g_queue.push_front(*it);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            req.shot = true;
            req.shot_name = name.empty() ? "shot_" + std::to_string(++g_shot_no) : name;
        } else if (cmd == "frame") {
            std::string a;
            ss >> a;
            req.log_frame = true;
            req.log_frame_all = a == "all";
        } else if (cmd == "vr") {
            std::string what, a;
            ss >> what >> a;
            float v = strtof(a.c_str(), nullptr);
            if (what == "yaw") g_vr.sim_yaw_deg = v;
            else if (what == "pitch") g_vr.sim_pitch_deg = v;
            else if (what == "ipd") g_vr.ipd_m = v;
            else if (what == "unit" && v > 0) g_vr.unit_m = v;
            else if (what == "cone") g_vr.cone_deg = v;
            else if (what == "mode") {
                if (g_vr.mode == VrMode::off) logf("vr mode: started with mode=off (single-size targets); unchanged");
                else g_vr.stereo = a != "off";
            } else if (what == "scene") {
                req.scene_shot = true;
                req.scene_name = a.empty() ? "scene_" + std::to_string(++g_shot_no) : a;
            } else if (what == "check") {
                req.check = true;
            } else if (what == "targets") {
                req.dump_targets = true;
            } else if (what == "handoff") {
                req.handoff_shot = true;
                req.handoff_name = a.empty() ? "handoff_" + std::to_string(++g_shot_no) : a;
            } else if (what == "recentre" || what == "recenter") {
                vrsys_recentre("command");
            } else if (what == "dbg") {

                g_dbg = atoi(a.c_str());
            } else if (what == "crosshair") {
                g_vr.crosshair = v != 0;
            } else if (what == "handseek") {
                input_sim_pointer(true);
                gun_request_handseek();
            } else if (what == "hand" || what == "handpos" || what == "handat" || what == "handscreen") {
                std::string b, c;
                ss >> b >> c;
                const float y = strtof(b.c_str(), nullptr), z = strtof(c.c_str(), nullptr);
                if (what == "hand" && a == "off") {
                    input_sim_pointer(false);
                } else {
                    input_sim_pointer(true);
                    if (what == "hand") g_vr.sim_hand_yaw = v, g_vr.sim_hand_pitch = y;
                    else if (what == "handpos") g_vr.sim_hand[0] = v, g_vr.sim_hand[1] = y, g_vr.sim_hand[2] = z;
                    else if (what == "handat") gun_request_handat(v, y);
                    else input_aim_at_screen(v, y);
                }
                logf("vr hand: at %.3f %.3f %.3f m, yaw %.2f pitch %.2f deg (relative to the head)", g_vr.sim_hand[0], g_vr.sim_hand[1],
                     g_vr.sim_hand[2], g_vr.sim_hand_yaw, g_vr.sim_hand_pitch);
            } else if (what == "btn") {
                int ms = 120;
                ss >> ms;
                input_sim_button(a, true);
                push_front({"wait " + std::to_string(ms), "_vrbtnup " + a, "wait 200"});
            } else if (what == "hold") {
                input_sim_button(a, true);
            } else if (what == "release") {
                input_sim_button(a.empty() ? "all" : a, false);
            } else if (what == "marker") {
                g_vr.marker = v != 0;
            } else if (what == "aim") {
                g_vr.aim = v != 0;
            } else if (what == "gesture") {
                g_vr.reload_gesture = v != 0;
            } else if (what == "actions") {
                gun_log_actions(v != 0);
            } else if (what == "god") {
                gun_set_god(v != 0);
            } else if (what == "comfort" && a == "wait") {
                std::string x, ms;
                ss >> x >> ms;
                g_wait_vignette = x.empty() ? 0.1f : strtof(x.c_str(), nullptr);
                g_wait_until = now + (ms.empty() ? 60000 : ULONGLONG(atoi(ms.c_str())));
            } else if (what == "comfort" && a == "end") {
                // the end of a `vr comfort wait` block
            } else if (what == "comfort" && !a.empty()) {
                std::vector<std::string> args;
                for (std::string t; ss >> t;) args.push_back(t);
                comfort_command(a, args);
            } else if (what == "arms" || what == "shake" || what == "bob" || what == "comfort") {
                if (!a.empty() && what != "comfort") {
                    bool& flag = what == "arms" ? g_vr.game_arms : what == "shake" ? g_vr.camera_shake : g_vr.head_bob;
                    flag = v != 0;
                }
                comfort_log();
            } else if (what == "camlog") {
                comfort_camlog(a.empty() ? 120 : atoi(a.c_str()));
            } else if (what == "shakelog") {
                comfort_log_shake(v != 0);
            } else if (what == "actors") {
                comfort_log_actors(v != 0);
            } else if (what == "measure") {
                gun_request_measure();
            } else if (what == "wiiview") {
                comfort_allow_wiiview(v != 0);
            } else if (what == "waithit") {
                g_wait_hits = gun_hits();
                g_wait_until = now + (a.empty() ? 30000 : ULONGLONG(atoi(a.c_str())));
            } else if (what == "gun") {
                gun_log_state();
            } else if (what == "gunmodel") {
                if (!a.empty()) gunmodel_enable(v != 0);
                gunmodel_log();
            } else if (what == "gunset") {
                std::string field;
                float vals[4];
                int n = 0;
                ss >> field;
                while (n < 4 && ss >> vals[n]) ++n;
                gunmodel_set(a, field, vals, n);
                gunmodel_log();
            } else if (what == "barrelline") {
                gunmodel_barrel_line(v != 0);
            } else if (what == "clipout") {
                gunmodel_force_clip(a.empty() ? -1.f : v);
            } else if (what == "gunview") {
                // Sim views of the gun in the hand: head and hand presets (hand relative to the head).
                struct View { const char* name; float head_yaw, head_pitch, hx, hy, hz, hand_yaw, hand_pitch; };
                static const View views[] = {
                    {"aim", 0, 0, 0.15f, -0.25f, -0.35f, 0, 0},     // the default sim hand, aiming ahead
                    {"back", 0, 0, 0.0f, -0.08f, -0.30f, 0, 0},     // just below the eyes, from behind
                    {"right", 0, 0, 0.0f, -0.03f, -0.50f, 90, 0},   // barrel to the right: its right side
                    {"left", 0, 0, 0.0f, -0.03f, -0.50f, -90, 0},   // barrel to the left: its left side
                    {"front", 0, 0, 0.0f, -0.03f, -0.55f, 180, 0},  // barrel at the eyes: the muzzle
                    {"top", 0, -89.5f, 0.0f, 0.0f, -0.45f, 0, 89.5f},    // looking down on it, barrel ahead
                    {"bottom", 0, 89.5f, 0.0f, 0.0f, -0.45f, 0, -89.5f},  // looking up at it
                };
                bool found = false;
                for (const View& w : views)
                    if (a == w.name) {
                        g_vr.sim_yaw_deg = w.head_yaw, g_vr.sim_pitch_deg = w.head_pitch;
                        g_vr.sim_hand[0] = w.hx, g_vr.sim_hand[1] = w.hy, g_vr.sim_hand[2] = w.hz;
                        g_vr.sim_hand_yaw = w.hand_yaw, g_vr.sim_hand_pitch = w.hand_pitch;
                        input_sim_pointer(true);
                        found = true;
                    }
                if (!found) logf("vr gunview: aim, back, right, left, front, top or bottom");
            } else if (what == "state") {
                GameState st = game_state();
                logf("vr state: level %d cutscene %d paused %d gameplay %d; this frame %s", int(st.level), int(st.cutscene),
                     int(st.paused), int(st.gameplay), stereo_frame_active() ? "stereo" : "flat");
            } else if (!what.empty()) {
                logf("unknown vr command: %s", line.c_str());
            }
            logf("vr: mode=%s stereo=%d yaw=%.1f pitch=%.1f ipd_m=%.4f unit_m=%.4f cone_deg=%.1f crosshair=%d aim=%d marker=%d gesture=%d",
                 mode_name(g_vr.mode), int(g_vr.stereo), g_vr.sim_yaw_deg, g_vr.sim_pitch_deg, g_vr.ipd_m, g_vr.unit_m, g_vr.cone_deg,
                 int(g_vr.crosshair), int(g_vr.aim), int(g_vr.marker), int(g_vr.reload_gesture));
        } else if (cmd == "peek") {
            std::string a;
            int n = 4;
            ss >> a >> n;
            uintptr_t addr = parse_addr(a);
            if (!readable(addr)) {
                logf("peek %p: not readable", reinterpret_cast<void*>(addr));
            } else {
                for (int i = 0; i < n; ++i) {
                    DWORD d = reinterpret_cast<const DWORD*>(addr)[i];
                    float fl;
                    memcpy(&fl, &d, 4);
                    logf("  %p: %08lx  %g", reinterpret_cast<void*>(addr + i * 4), d, fl);
                }
            }
        } else if (cmd == "poke") {
            // poke ADDR VALUE: VALUE is a dword (0x...) or a float (with a '.')
            std::string a, v;
            ss >> a >> v;
            uintptr_t addr = parse_addr(a);
            if (!readable(addr)) {
                logf("poke %p: not writable", reinterpret_cast<void*>(addr));
            } else {
                DWORD d;
                if (v.find('.') != std::string::npos) {
                    float fl = strtof(v.c_str(), nullptr);
                    memcpy(&d, &fl, 4);
                } else {
                    d = strtoul(v.c_str(), nullptr, 0);
                }
                DWORD old, before = *reinterpret_cast<DWORD*>(addr);
                VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_READWRITE, &old);
                *reinterpret_cast<DWORD*>(addr) = d;
                VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &old);
                logf("poke %p: %08lx -> %08lx", reinterpret_cast<void*>(addr), before, d);
            }
        } else if (cmd == "aimdir" || cmd == "aimorigin") {
            std::string a;
            ss >> a;
            float x = strtof(a.c_str(), nullptr), y = 0, z = 0;
            ss >> y >> z;
            bool on = a != "off";
            if (cmd == "aimdir" && a == "capture") gun_capture_next();
            else if (cmd == "aimdir") gun_set_dir(on, x, y, z);
            else gun_set_origin(on, x, y, z);
        } else if (cmd == "dump") {
            // dump ADDR SIZE NAME: raw bytes to hovr\NAME.bin (state diffing)
            std::string a, name;
            int n = 256;
            ss >> a >> n >> name;
            uintptr_t addr = parse_addr(a);
            if (!readable(addr) || !readable(addr + n - 1) || name.empty()) {
                logf("dump %s: not readable", a.c_str());
            } else {
                std::wstring w = path_in((std::wstring(name.begin(), name.end()) + L".bin").c_str());
                if (FILE* f = _wfopen(w.c_str(), L"wb")) {
                    fwrite(reinterpret_cast<const void*>(addr), 1, size_t(n), f);
                    fclose(f);
                }
                logf("dump %s (%p) %d bytes -> %s.bin", a.c_str(), reinterpret_cast<void*>(addr), n, name.c_str());
            }
        } else if (cmd == "findvt") {
            // findvt VA: heap objects whose first dword is the (rebased) vtable VA
            std::string a;
            ss >> a;
            const DWORD target = DWORD(parse_addr(a));
            int hits = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (uintptr_t p = 0x10000; p < 0x7FFF0000 && hits < 32; p += mbi.RegionSize) {
                if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
                    (mbi.Protect & PAGE_GUARD))
                    continue;
                auto d = static_cast<const DWORD*>(mbi.BaseAddress);
                for (size_t i = 0; i < mbi.RegionSize / 4 && hits < 32; ++i)
                    if (d[i] == target) {
                        logf("findvt %08lx: object at %p", target, &d[i]);
                        ++hits;
                    }
            }
            logf("findvt %08lx: %d hits", target, hits);
        } else if (cmd == "findstr") {
            // findstr TEXT: heap addresses holding TEXT followed by a NUL (case-sensitive)
            std::string text;
            ss >> text;
            int hits = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (uintptr_t p = 0x10000; !text.empty() && p < 0x7FFF0000 && hits < 32; p += mbi.RegionSize) {
                if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
                    (mbi.Protect & PAGE_GUARD))
                    continue;
                auto d = static_cast<const char*>(mbi.BaseAddress);
                for (size_t i = 0; i + text.size() + 1 <= mbi.RegionSize && hits < 32; ++i)
                    if (d[i] == text[0] && !memcmp(d + i, text.c_str(), text.size() + 1)) {
                        logf("findstr %s: at %p", text.c_str(), d + i);
                        ++hits;
                    }
            }
            logf("findstr %s: %d hits", text.c_str(), hits);
        } else if (cmd == "drawstack") {
            std::string h;
            int n = 1;
            ss >> h >> n;
            request_drawstack(strtoull(h.c_str(), nullptr, 16), n);
        } else if (cmd == "log") {
            logf("log: %s", line.size() > 4 ? line.c_str() + 4 : "");
        } else if (line == "crash now") {
            // Tests the crash handler (src/mod/crash.cpp): a write to address 0x10.
            logf("crash now: writing to 0x10");
            *reinterpret_cast<volatile int*>(uintptr_t(0x10)) = 1;
        } else if (cmd == "fakevr") {
            // The test runtime (src/fakevr): its scripting, in order with these commands.
            using FakeCmd = void(__cdecl*)(const char*);
            HMODULE m = GetModuleHandleW(L"openvr_api.dll");
            FakeCmd f = m ? reinterpret_cast<FakeCmd>(GetProcAddress(m, "fakevr_command")) : nullptr;
            if (f) f(line.size() > 7 ? line.c_str() + 7 : "");
            else logf("fakevr: openvr_api.dll is not loaded or is not the fake runtime");
        } else {
            logf("unknown command: %s", line.c_str());
        }
        if (req.shot || req.log_frame || req.scene_shot || req.check || req.dump_targets || req.handoff_shot) break;  // one capture per frame
    }
    return req;
}

}  // namespace hv
