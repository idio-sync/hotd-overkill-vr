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
bool readable(uintptr_t addr);
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
    bool uncap = true;
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
};
FrameRequests control_tick();
void write_status(const char* text);

// Scripted pad for player 1 (xinput.cpp).
bool fake_pad_enabled();
void pad_set(WORD buttons, SHORT lx, SHORT ly, SHORT rx, SHORT ry, BYTE lt, BYTE rt);
void pad_apply(XINPUT_STATE* state);

// Stereo (stereo.cpp), driven from the device hooks in d3d.cpp.
bool stereo_target_size(UINT w, UINT h, bool depth, UINT* out_w, UINT* out_h);  // resize a screen-sized target?
void stereo_track_texture(IDirect3DTexture9* tex, UINT w, UINT h);  // w, h = the size the game asked for
void stereo_track_surface(IDirect3DSurface9* s, UINT w, UINT h);
void stereo_set_rt(IDirect3DSurface9* s);  // render target 0 changed
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
HRESULT stereo_draw(IDirect3DDevice9* dev, DrawFn draw, void* ctx);  // draws once or once per eye
void stereo_frame_end();       // Present: per-frame stats and caches
int stereo_draws_this_frame();  // perspective scene draws drawn per eye so far this frame
void stereo_request_check();   // `vr check` for the next frame
const char* stereo_status();   // for the status line
// Raw device calls past our hooks (d3d.cpp).
HRESULT raw_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count);
HRESULT raw_set_viewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp);
HRESULT raw_set_scissor(IDirect3DDevice9* dev, const RECT* r);
const float* vs_shadow(UINT reg);  // the game's last value of a vertex shader constant register
IDirect3DVertexShader9* current_vs();
void frame_note(const char* text);  // a line in the frame log, if one is being written

// Engine patches (engine.cpp): frame limiter, culling widening.
void engine_frame(bool stereo_last_frame);  // once per Present

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra);

}  // namespace hv
