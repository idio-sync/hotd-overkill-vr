#pragma once
// Shared declarations for the mod: a stand-in XINPUT1_3.dll loaded from the game folder.
// It grew out of the probe spike; the probe's development harness (save redirect, minimized and
// muted window, command file, screenshots, frame logs) is still part of it.
#include <windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../vr/aim.h"
#include "../vr/stereo.h"

struct ID3D11Texture2D;

namespace hv {

// Paths: the game folder and our hovr\ folder inside it.
extern std::wstring g_game_dir;
extern std::wstring g_dir;
void init_paths();
std::wstring path_in(const wchar_t* name);  // g_dir + L"\\" + name
// Development vs release (src/vr/harness.h): development = hovr\probe.ini exists or hovr.ini
// [vr] dev=1 (the test copy); decided once in init_paths.
bool dev_harness();
// A harness setting: hovr\probe.ini [probe] KEY in development (default dev_default), the fixed
// release value otherwise.
int harness_int(const wchar_t* key, int dev_default, int release_value);

void log_open();
void logf(const char* fmt, ...);

uintptr_t exe_base();
inline uintptr_t va(uintptr_t image_va) { return exe_base() + image_va - 0x400000; }  // the exe is ASLR'd
bool readable(uintptr_t addr);  // VirtualQuery: slow (page-table walk); not for per-draw use
bool safe_copy(void* dst, uintptr_t src, size_t n);  // exception-guarded read
// Inline detour at image VA `image_va`: checks that the first `n` bytes are `expect` (they must be
// position-independent, n >= 5), copies them to a trampoline and jumps to `hook`. Returns the
// trampoline (call it to run the original) or nullptr, with a log line, if the bytes differ.
void* install_detour(uintptr_t image_va, const uint8_t* expect, size_t n, void* hook, const char* name);
bool patch_import(HMODULE module, const char* dll, const char* func, void* hook, void** orig);
bool patch_vtbl(void* object, int index, void* hook, void** orig);
uint64_t fnv64(const void* data, size_t size);

// buildcheck.cpp: HOTD_NG.exe on disk is the supported build, or a patched copy of it whose
// anchors (every place the mod uses, src/vr/exe_anchors.h) all match; logs the verdict.
bool exe_is_supported();

// VR settings (config.cpp): hovr\hovr.ini [vr].
enum class VrMode { off, sim, openvr };
struct VrConfig {
    VrMode mode = VrMode::off;  // from the ini at load; decides whether targets are made double-wide
    bool stereo = false;        // live: draw both eyes (mode != off; `vr mode off` stops it)
    float ipd_m = 0.064f;
    float unit_m = 0.01f;       // metres per game unit
    float sim_yaw_deg = 0, sim_pitch_deg = 0;
    float cone_deg = 35;
    int eye_w = 1280, eye_h = 720;
    bool eye_res_auto = true;   // openvr: from SteamVR's recommended size
    float eye_scale = 1.0f;
    bool uncap = true;
    bool crosshair = false;     // keep the game's crosshair in stereo gameplay
    float hud_distance = 1.5f, hud_width = 1.3f, hud_drop = 0.0f;
    float screen_distance = 3.0f, screen_width = 3.2f, screen_drop = 0.0f;
    bool mirror = true;         // desktop window shows the VR view
    bool handoff = true;        // sim: run the D3D9Ex -> D3D11 hand-off too
    bool handoff_sync = true;   // wait for the D3D9 GPU work before D3D11 reads it
    // Gun and input (milestone 2).
    bool aim = true;            // the controller aims the game's shots (0: the mouse, as in milestone 1)
    bool left_hand = false;     // hand = left: the gun is the left controller
    float hand_pitch = 0;       // degrees the aim is tilted up from the controller's pointing axis
    float hand_offset[3] = {};  // aim origin in the controller's frame, metres (x right, y up, z back)
    float haptics = 1.0f;       // shot pulse strength scale (0: off)
    bool marker = false;        // a small dot in the eyes where the shot will land
    bool reload_gesture = false;  // point well away from the view and pull the trigger = reload
    float reload_gesture_deg = 60;
    float sim_hand[3] = {0.15f, -0.25f, -0.35f};  // sim hand position relative to the head, metres (RH)
    float sim_hand_yaw = 0, sim_hand_pitch = 0;    // sim hand direction relative to the head, degrees
    // Controller buttons ("gun.a", "off.b", ...; comma-separated alternatives) and the game input
    // each one sends ("lmb", "rmb", "mmb", "wheelup", "wheeldown", or a key name).
    // The game's own PC controls (its Controls screen): left click fire, middle click reload, right
    // click grenade, mouse wheel or TAB swap weapon, Escape pause.
    std::string btn_reload = "gun.a", btn_switch = "gun.b", btn_grenade = "gun.grip", btn_pause = "off.b,off.a",
                btn_recentre = "gun.stick";
    std::string game_fire = "lmb", game_reload = "mmb", game_switch = "TAB", game_grenade = "rmb", game_pause = "ESCAPE";
    // The weapon model (milestone 3, gunmodel.cpp); per-weapon keys are read by gunmodel_load_config.
    bool gun_model = true;       // draw the game's own gun mesh in the hand
    float gun_grip[3] = {0, -0.03f, 0.05f};  // where the mesh's grip point sits in the controller's frame, metres (x right, y up, z back)
    float gun_scale = 1.0f;      // mesh scale (1 = the game's size)
    bool aim_barrel = true;      // shots go along the barrel line, from the muzzle (0: from the controller, as in milestone 2)
    bool clip_anim = true;       // the magazine drops out and back in on reload
    float clip_drop = 0.2f;      // how far it drops, metres
    float gun_light = 1.0f;      // brightness of the gun's lighting
    float gun_face_sign = 1.0f;  // which side of the mesh's triangles faces out (lighting); -1 flips
    // Comfort (milestone 4, comfort.cpp).
    bool game_arms = false;      // the game's own first-person arms/reload animation (0: hidden in VR)
    bool camera_shake = false;   // the game's camera shake and hit smack
    bool head_bob = false;       // the game's head-bob while the rail moves
    bool recentre_resume = true; // recentre whenever gameplay resumes after a flat screen (pause, death, cutscene)
    bool keep_active = false;    // release: the game keeps running when its window loses focus
    // Rail camera comfort (comfort.cpp, src/vr/comfort.h): live with `vr comfort ...`.
    bool comfort_on = true;          // `vr comfort off|on`: all three below at once
    float comfort_vignette = 0.6f;   // vignette strength at full rail speed (0 = off)
    float vignette_turn_dps[2] = {30, 120};  // rail turn speed: vignette starts / full, deg/s
    float vignette_move_mps[2] = {2, 6};     // rail speed: vignette starts / full, m/s
    float vignette_ease_ms = 200;    // fade in/out time
    float comfort_smooth_ms = 150;   // rotation smoothing time constant (0 = off)
    int comfort_level = 1;           // horizon levelling: 0 off, 1 roll, 2 roll and pitch
};
extern VrConfig g_vr;
// Release install, first start: write the default hovr\hovr.ini if it is missing (config.cpp).
void write_default_settings();
void load_vr_config();
const char* mode_name(VrMode m);
bool vr_ini_floats(const char* key, float* out, int n);  // [vr] KEY = a,b,c...: true if n numbers were read

void install_folder_hook();   // SHGetFolderPathA(APPDATA) -> hovr\AppData
bool game_config_resolution(unsigned* w, unsigned* h);  // the game's options' resolution (read only)
void install_crash_handler();  // logs an unhandled exception and writes hovr\crash.dmp
std::string address_space_status();  // "address space 1234 of 2048 MB used, largest free block 300 MB"
void install_window_hooks();  // minimized window, faked activation and cursor
void install_compile_hooks();  // D3DXCompileShader / D3DXAssembleShader source dumps
// Direct3DCreate9 -> device vtable hooks; game_hooks false (development, exe refused): only the
// harness's forced windowed device, nothing hooked on the device.
void install_d3d_hooks(bool game_hooks);
void start_audio_mute();
void install_dinput_hooks();  // DirectInput8: scripted mouse buttons/motion and keys
void install_file_hooks();    // logs .pc asset opens

// Comfort (comfort.cpp): first-person arms, camera shake/smack, head-bob, WiiView edge-pan.
void comfort_frame();          // once per Present (installs its detours at the first one; VR modes only)
void comfort_log();            // `vr comfort`
void comfort_camlog(int frames);   // dev: log the camera position for the next N frames
void comfort_log_shake(bool on);   // dev: log every non-zero shake/smack offset
void comfort_log_actors(bool on);
void comfort_allow_wiiview(bool on);
// Rail camera comfort: smoothing, levelling, vignette (comfort.cpp; the math in src/vr/comfort.h).
void comfort_rail_frame(const vr::Mat4& view);  // stereo.cpp: once per stereo frame, the frame's game view
vr::Mat4 comfort_rail_delta();   // camera-space rotation applied to the eyes and the gun hand (vrsys.cpp)
float comfort_vignette_now();    // vignette strength to draw this frame (0..1)
// The rail camera's turn speed, deg/s: smoothed over ~100 ms (as the vignette sees it) and this
// stereo frame's own (0 on a camera cut).
void comfort_rail_turn(float* smoothed_dps, float* frame_dps);
void comfort_command(const std::string& sub, const std::vector<std::string>& args);  // `vr comfort SUB ...`
void comfort_log_settings(const char* why);
void comfort_log_now(const char* why);  // the rail camera's speeds, the vignette and the smoothing lag now  // dev: let the game's edge-pan answer through (to compare)  // dev: log each actor drawn through the entity draw (once per actor)

// The controller gun (gun.cpp): detours on the game's shot paths and cursor setters, installed
// from the first Present (after SecuROM's start-up patching).
void install_gun_hooks();
void gun_frame();  // once per Present: a new game frame for the aim
void gun_set_dir(bool on, float x, float y, float z);   // dev: `aimdir`
void gun_set_origin(bool on, float x, float y, float z);  // dev: `aimorigin`
void gun_capture_next();
// The last aim point (world, game units) and whether the ray hit the scene; false: no aim.
bool gun_aim_point(float out[3], bool* hit);
void gun_request_handat(float cursor_x, float cursor_y);  // sim: aim the hand at what the camera sees at a cursor
void gun_request_handseek();  // sim: aim the hand at the shootable thing nearest the view's centre
void gun_log_state();      // `vr gun`: weapon, ammo, aim
bool gun_aim_active();     // the controller drives player 1's aim now
void gun_log_actions(bool on);  // dev: log the game's input action values as they change
void gun_set_god(bool on);  // dev: keep player 1's health full (tests that need time in gameplay)
void gun_request_measure();
int gun_hits();             // player 1's health drops seen so far (dev `vr waithit`)  // dev `vr measure`: world-scale ray casts (next game frame)
unsigned gun_frame_no();    // the frame counter gun_frame() advances (once per Present)
// Gun accuracy test (dev `vr shotlog 0|1`, `vr shotlog tag NAME`): one `shotlog` line per player-1
// shot, per bullet ray cast and per projectile launch; tools/gun_accuracy.py reads them.
void gun_shotlog(bool on);
bool gun_shotlog_on();
void gun_shotlog_tag(const std::string& name);

// The weapon model (gunmodel.cpp).
void gunmodel_load_config();  // per-weapon [vr] keys: show_, clip_, grip_, barrel_, muzzle_, hold_<weapon>
// Draws the current weapon's mesh in both eyes; called by stereo.cpp with the scene target bound.
// eye_setup sets an eye's viewport and gives its world -> clip matrix.
void gunmodel_draw(IDirect3DDevice9* dev, bool (*eye_setup)(IDirect3DDevice9* dev, int eye, vr::Mat4* world_to_clip));
bool gunmodel_barrel(const vr::Mat4& hand_lh, vr::Ray* cam_ray);  // the barrel line in camera space (metres), if a model is shown
void gunmodel_reset();       // device reset: drop our D3D objects
void gunmodel_frame();       // once per Present: drop the models when the level ends
void gunmodel_log();         // `vr gunmodel`
void gunmodel_enable(bool on);
void gunmodel_force_clip(float out);  // dev: magazine out fraction (-1: from the reload state)
void gunmodel_barrel_line(bool on);   // dev: draw the barrel line to the aim point
void gunmodel_set(const std::string& key, const std::string& field, const float* v, int n);  // `vr gunset`
// The gun accuracy test: the barrel of a frame the gun was drawn in (shot log on), from the world
// matrix it was drawn with.
struct DrawnBarrel {
    unsigned frame = 0;          // gun_frame_no() when drawn
    float muzzle[3] = {}, dir[3] = {};  // world (game units), dir unit
    float hand[6] = {};          // the hand pose drawn with: position (m) and pointing axis, camera space (LH)
    float turn = 0, turn_frame = 0;  // the rail camera's turn speed, deg/s (smoothed, this frame)
    float hand_turn = 0;         // the hand's pointing axis in camera space, deg/s since the previous frame drawn
    float barrel_turn = 0;       // the drawn barrel's world direction, deg/s since the previous frame drawn
    char key[16] = {};           // the weapon (kDefs key)
};
bool gunmodel_last_drawn(DrawnBarrel* out);  // the last frame drawn (false: none since the shot log went on)
// The weapon's model name (GunArms_...) and kDefs key, if its model was resolved.
bool gunmodel_weapon_name(uintptr_t weapon, char name[64], char key[16]);
void gunmodel_meshcheck();  // dev `vr gunmeshcheck`: the current weapon's live mesh against its kDefs description

// Controller input (vrinput.cpp): buttons -> the game's DirectInput, menu pointing, haptics.
enum CtlButton : uint32_t { kBtnTrigger = 1, kBtnA = 2, kBtnB = 4, kBtnGrip = 8, kBtnStick = 16 };
struct CtlState {
    bool present = false;
    uint32_t buttons = 0;  // CtlButton bits (kBtnTrigger: the trigger past half travel)
    float trigger = 0;     // 0..1
};
void input_frame();             // once per Present, after the poses were read
bool input_owns_pointer();      // the controllers replace the real mouse (VR modes, a controller seen)
void input_shot(float strength);  // a player-1 shot: haptic pulse (1 = normal, 2 = strong)
void input_sim_button(const std::string& name, bool down);  // sim controller: "trigger", "a", "off.b", ...
void input_sim_pointer(bool on);  // sim: the scripted hand drives the menu cursor (`vr hand*` turns it on)
void input_aim_at_screen(float x, float y);  // sim: point the hand at back-buffer pixel x, y of the flat screen
void vrsys_sim_buttons(bool gun_hand, uint32_t buttons, float trigger);
void game_input(const std::string& token, bool down);  // "lmb", "rmb", "wheelup", key names
UINT key_from_name(const std::string& s);  // control.cpp

// Scripted DInput state (dinput.cpp).
void di_key(UINT vk, bool down);
void di_mouse_button(int button, bool down);
void di_mouse_move(LONG dx, LONG dy);
void di_mouse_wheel(LONG dz);

// Shaders (shaders.cpp): bytecode -> hash, CTAB constant names, disassembly dump.
struct ShaderConst {
    std::string name;
    int set;    // D3DXRS_BOOL 0, INT4 1, FLOAT4 2, SAMPLER 3
    int index;
    int count;
};
struct ShaderInfo {
    uint64_t hash = 0;
    bool vertex = false;
    std::vector<ShaderConst> consts;
    // Stereo: first registers of the 4-row matrices that produce clip space (cWVP, cProject,
    // cObjectViewProject, cProj), and of eyePos (-1: none).
    int clip_regs[4] = {};
    int n_clip = 0;
    int eye_reg = -1;
    int object_reg = -1;  // cObjectMatrix (for `vr check`)
};
void shader_created(void* shader, const DWORD* code, bool vertex);
const ShaderInfo* shader_info(void* shader);
void note_compiled(const void* code, size_t size, uint64_t source_hash);  // compile hook -> source link

// Window (window.cpp).
HWND game_window();
void set_fake_cursor(int x, int y);  // client coordinates; x < 0 = real cursor
bool get_fake_cursor(int* x, int* y);
void post_mouse_button(bool right, bool down);
void post_key(UINT vk, bool down);

// Command file hovr\cmd.txt (control.cpp), run once per Present on the render thread.
struct FrameRequests {
    bool shot = false;
    std::string shot_name;
    bool log_frame = false;
    bool log_frame_all = false;  // `frame all`: every uploaded constant, not just 12 per call
    bool scene_shot = false;     // `vr scene NAME`: the stereo scene target at full size
    std::string scene_name;
    bool check = false;          // `vr check`: compare the engine's P*V with cWVP*cObjectMatrix^-1
    bool dump_targets = false;   // `vr targets`: PNGs (colour + alpha) of every resized ARGB8 target
    bool handoff_shot = false;   // `vr handoff NAME`: PNGs of the D3D11-side eye/HUD/screen textures
    std::string handoff_name;
};
FrameRequests control_tick();
void write_status(const char* text);

// Scripted pad for player 1 (xinput.cpp).
bool fake_pad_enabled();
void pad_set(WORD buttons, SHORT lx, SHORT ly, SHORT rx, SHORT ry, BYTE lt, BYTE rt);
void pad_apply(XINPUT_STATE* state);

// Stereo (stereo.cpp), driven from the device hooks in d3d.cpp.
void stereo_set_backbuffer(UINT w, UINT h);  // the device's back buffer size (composite-target rule)
bool stereo_target_size(UINT w, UINT h, bool depth, UINT* out_w, UINT* out_h);  // resize a screen-sized target?
void stereo_track_texture(IDirect3DTexture9* tex, UINT w, UINT h);  // w, h = the size the game asked for
void stereo_track_surface(IDirect3DSurface9* s, UINT w, UINT h);
void stereo_set_rt(IDirect3DSurface9* s);  // render target 0 changed (the surface really set)
void stereo_note_depth(IDirect3DSurface9* s);  // the depth surface the game set
bool stereo_rt_resized();                  // render target 0 is a resized target
IDirect3DSurface9* stereo_scene_surface(); // the last scene target drawn in stereo
std::vector<IDirect3DSurface9*> stereo_targets();  // every live resized target
void stereo_game_viewport(const D3DVIEWPORT9& vp);  // the game's viewport (target-space of its own size)
const D3DVIEWPORT9& stereo_get_game_viewport();
bool stereo_map_viewport(D3DVIEWPORT9* vp);  // full (both halves): scale to the real target
bool stereo_map_rect(IDirect3DSurface9* s, RECT* r);  // scale a rect on a tracked surface
void stereo_game_scissor(const RECT& r);
bool stereo_map_scissor(RECT* r);
void stereo_scissor_enable(bool on);
void stereo_set_texture(DWORD stage, IDirect3DBaseTexture9* t);
using DrawFn = HRESULT (*)(void* ctx);
// Draws once or once per eye; up/stride: DrawPrimitiveUP vertices (crosshair check).
HRESULT stereo_draw(IDirect3DDevice9* dev, DrawFn draw, void* ctx, const void* up = nullptr, UINT stride = 0, UINT prims = 0);
// HUD redirection (gameplay frames): what the game draws on the composite target after the
// composite draw goes to our HUD texture instead.
// Before the game sets render target 0 to `next`: the end of the scene's opaque pass draws the gun.
void stereo_before_rt(IDirect3DDevice9* dev, IDirect3DSurface9* next);
IDirect3DSurface9* stereo_redirect_rt(IDirect3DSurface9* s);
IDirect3DSurface9* stereo_redirect_depth(IDirect3DSurface9* s);
HRESULT stereo_bind_depth(IDirect3DDevice9* dev, IDirect3DSurface9* s);  // binds s for real (tracked)
void stereo_rebind_depth(IDirect3DDevice9* dev);  // after a render target change: the depth the target needs
bool stereo_hud_rs(D3DRENDERSTATETYPE s, DWORD v);  // true: swallowed while the HUD is redirected
SIZE stereo_hud_size();
bool stereo_frame_active();    // this frame is drawn in stereo (gameplay)
bool stereo_eye_proj(int eye, vr::Proj* out);  // the eye projections of the current frame
void stereo_eye_half_angles(float game_hx, float game_hy, float* hx, float* hy);  // for culling
void stereo_present(IDirect3DDevice9* dev, const FrameRequests& req);  // at Present: hand-off, mirror, submit
void stereo_frame_end();       // Present: per-frame stats, next frame's mode
void stereo_reset();           // device Reset: drop our D3D9 resources
bool stereo_flat_rect(RECT* r);  // the back-buffer area shown on the flat screen (last flat frame)
bool stereo_world_to_eye(const float p[3], int eye, float* x, float* y);  // eye-texture pixel of a world point (stereo frames)
int stereo_draws_this_frame();  // perspective scene draws drawn per eye so far this frame
void stereo_request_check();   // `vr check` for the next frame
const char* stereo_status();   // for the status line
extern int g_dbg;              // `vr dbg BITS`: cost bisecting switches (stereo.cpp)
// Raw device calls past our hooks (d3d.cpp).
HRESULT raw_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count);
HRESULT raw_set_viewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp);
HRESULT raw_set_scissor(IDirect3DDevice9* dev, const RECT* r);
HRESULT raw_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
HRESULT raw_set_depth(IDirect3DDevice9* dev, IDirect3DSurface9* s);
HRESULT raw_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t);
HRESULT raw_set_vs(IDirect3DDevice9* dev, IDirect3DVertexShader9* s);
HRESULT raw_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* s);
HRESULT raw_set_rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v);
HRESULT raw_dpup(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride);
HRESULT raw_dip(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT si, UINT n);
HRESULT raw_clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st);
HRESULT raw_stretch(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                    D3DTEXTUREFILTERTYPE f);
HRESULT raw_create_texture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                           IDirect3DTexture9** tex, HANDLE* shared);
HRESULT raw_create_depth(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface9** s);
HRESULT raw_create_rt(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, IDirect3DSurface9** s);
const float* vs_shadow(UINT reg);  // the game's last value of a vertex shader constant register
IDirect3DVertexShader9* current_vs();
IDirect3DPixelShader9* current_ps();
void request_drawstack(uint64_t vs_hash, int n);  // dev `drawstack`: log call stacks of draws with that vertex shader
void frame_note(const char* text);  // a line in the frame log, if one is being written
bool frame_logging();

// Game state (gamestate.cpp).
struct GameState {
    bool level = false;     // a level is loaded (cCutsceneControl exists)
    bool cutscene = false;  // cutscene mode
    bool paused = false;    // pause menu or death screen
    bool gameplay = false;  // level && !cutscene && !paused
};
GameState game_state();

// Direct3D 9Ex device with managed-pool emulation (d3d9ex.cpp).
bool ex_enabled();
bool ex_adapter_luid(LUID* out);
IDirect3D9* ex_create_d3d(UINT sdk);
HRESULT ex_create_device(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out);
D3DPOOL ex_pool(D3DPOOL pool, bool* converted);
void ex_track_texture(IDirect3DTexture9* t);
void ex_track_cube(IDirect3DCubeTexture9* t);
void ex_forget(void* res);
const char* ex_status();

// D3D9 -> D3D11 hand-off (handoff.cpp).
struct SharedTex {
    const char* name;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    HANDLE handle = nullptr;
    ID3D11Texture2D* t11 = nullptr;  // the same texture opened on the D3D11 device (null: no hand-off)
    UINT w = 0, h = 0;
};
bool handoff_init(IDirect3DDevice9* dev);
bool handoff_ready();
void handoff_release_d3d9();
bool shared_ensure(SharedTex& s, IDirect3DDevice9* dev, UINT w, UINT h, bool share = true);
void shared_release(SharedTex& s);
void handoff_sync(IDirect3DDevice9* dev);
const char* handoff_status();
ID3D11Texture2D* handoff_copy(ID3D11Texture2D*& own, ID3D11Texture2D* src);
ID3D11Texture2D* handoff_black();
void handoff_save(ID3D11Texture2D* tex, const std::string& name);

// The headset (vrsys.cpp): OpenVR or simulated.
void vrsys_start();         // at Direct3DCreate9: OpenVR init (mode=openvr), eye resolution
bool vrsys_openvr();
void vrsys_recentre(const char* why);  // logged; openvr: the anchor moves to the head at the next pose
void vrsys_wait_poses();    // after Present: the next frame's head pose
vr::Mat4 vrsys_eye_in_anchor(int eye);  // right-handed, metres
bool vrsys_eye(int eye, vr::Mat4* pose_lh, float tan[4]);  // false: sim (no headset tangents)
vr::Mat4 vrsys_panel_in_anchor(bool screen);
bool vrsys_hand_in_game(vr::Mat4* pose_lh);    // the gun hand in the game camera's space (LH, metres)
bool vrsys_hand_in_anchor(vr::Mat4* pose_rh);  // the gun hand relative to the anchor (RH, metres)
bool vrsys_head_in_anchor(vr::Mat4* pose_rh);
bool vrsys_controller(bool gun_hand, CtlState* out);  // openvr: the controller; sim: scripted buttons
void vrsys_haptic(unsigned microseconds);      // pulse on the gun hand
void vrsys_sim_aim_at(const float p_anchor_rh[3]);  // sim: point the hand at a point (anchor space)
void vrsys_submit(ID3D11Texture2D* eyes, ID3D11Texture2D* hud, ID3D11Texture2D* screen);

// Desktop mirror (mirror.cpp).
void mirror_stereo(IDirect3DDevice9* dev, IDirect3DTexture9* eyes, IDirect3DTexture9* hud);
void mirror_flat(IDirect3DDevice9* dev, IDirect3DTexture9* screen);
void mirror_release();
void draw_dots(IDirect3DDevice9* dev, IDirect3DSurface9* target, const float (*xy)[2], int n, float radius, D3DCOLOR colour);
// Black triangles (a list, x, y in target pixels, per-vertex opacity) blended over `target`
// inside `clip`; colour only, the target's alpha is kept (the vignette).
void draw_shade(IDirect3DDevice9* dev, IDirect3DSurface9* target, const float (*xya)[3], int n, const RECT& clip);

// Engine patches (engine.cpp): frame limiter, culling widening.
void engine_frame(bool stereo_next_frame);  // once per Present

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra);

}  // namespace hv
