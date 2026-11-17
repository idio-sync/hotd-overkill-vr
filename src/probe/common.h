#pragma once
// Shared declarations for the probe: a stand-in XINPUT1_3.dll loaded from the game folder.
// Throwaway spike code: it logs what the game does so the real mod can be designed.
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
bool patch_import(HMODULE module, const char* dll, const char* func, void* hook, void** orig);
bool patch_vtbl(void* object, int index, void* hook, void** orig);
uint64_t fnv64(const void* data, size_t size);

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
};
FrameRequests control_tick();
void write_status(const char* text);

// Scripted pad for player 1 (xinput.cpp).
bool fake_pad_enabled();
void pad_set(WORD buttons, SHORT lx, SHORT ly, SHORT rx, SHORT ry, BYTE lt, BYTE rt);
void pad_apply(XINPUT_STATE* state);

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra);

}  // namespace hv
