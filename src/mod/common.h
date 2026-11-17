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

#include "../vr/stereo.h"

struct ID3D11Texture2D;

namespace hv {

// Paths: the game folder and our hovr\ folder inside it.
extern std::wstring g_game_dir;
extern std::wstring g_dir;
void init_paths();
std::wstring path_in(const wchar_t* name);  // g_dir + L"\\" + name
int ini_int(const wchar_t* key, int def);   // hovr\probe.ini [probe] key

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

bool exe_is_known_build();  // buildcheck.cpp: HOTD_NG.exe on disk is the supported build

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
};
extern VrConfig g_vr;
void load_vr_config();
const char* mode_name(VrMode m);

void install_folder_hook();   // SHGetFolderPathA(APPDATA) -> hovr\AppData
void install_window_hooks();  // minimized window, faked activation and cursor
void install_compile_hooks();  // D3DXCompileShader / D3DXAssembleShader source dumps
void install_d3d_hooks();     // Direct3DCreate9 -> device vtable hooks
void start_audio_mute();
void install_dinput_hooks();  // DirectInput8: scripted mouse buttons/motion and keys
void install_file_hooks();    // logs .pc asset opens

// FireBullets detour (gunhook.cpp), installed from the first Present.
void install_gun_hook();
void gun_set_dir(bool on, float x, float y, float z);
void gun_set_origin(bool on, float x, float y, float z);
void gun_capture_next();

// Scripted DInput state (dinput.cpp).
void di_key(UINT vk, bool down);
void di_mouse_button(int button, bool down);
void di_mouse_move(LONG dx, LONG dy);

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
IDirect3DSurface9* stereo_redirect_rt(IDirect3DSurface9* s);
IDirect3DSurface9* stereo_redirect_depth(IDirect3DSurface9* s);
bool stereo_hud_rs(D3DRENDERSTATETYPE s, DWORD v);  // true: swallowed while the HUD is redirected
SIZE stereo_hud_size();
bool stereo_frame_active();    // this frame is drawn in stereo (gameplay)
bool stereo_eye_proj(int eye, vr::Proj* out);  // the eye projections of the current frame
void stereo_eye_half_angles(float game_hx, float game_hy, float* hx, float* hy);  // for culling
void stereo_present(IDirect3DDevice9* dev, const FrameRequests& req);  // at Present: hand-off, mirror, submit
void stereo_frame_end();       // Present: per-frame stats, next frame's mode
void stereo_reset();           // device Reset: drop our D3D9 resources
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
void vrsys_recentre();
void vrsys_wait_poses();    // after Present: the next frame's head pose
vr::Mat4 vrsys_eye_in_anchor(int eye);  // right-handed, metres
bool vrsys_eye(int eye, vr::Mat4* pose_lh, float tan[4]);  // false: sim (no headset tangents)
vr::Mat4 vrsys_panel_in_anchor(bool screen);
void vrsys_submit(ID3D11Texture2D* eyes, ID3D11Texture2D* hud, ID3D11Texture2D* screen);

// Desktop mirror (mirror.cpp).
void mirror_stereo(IDirect3DDevice9* dev, IDirect3DTexture9* eyes, IDirect3DTexture9* hud);
void mirror_flat(IDirect3DDevice9* dev, IDirect3DTexture9* screen);
void mirror_release();

// Engine patches (engine.cpp): frame limiter, culling widening.
void engine_frame(bool stereo_next_frame);  // once per Present

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra);

}  // namespace hv
