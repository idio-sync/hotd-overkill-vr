#include "common.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        hv::init_paths();
        hv::log_open();
        hv::logf("hovr probe loaded into %ls, exe base %p", hv::g_game_dir.c_str(), (void*)hv::exe_base());
        // The exe's imports are bound before DLL initializers run, so its IAT can be patched here.
        hv::install_folder_hook();
        hv::install_window_hooks();
        hv::install_compile_hooks();
        hv::install_d3d_hooks();
        hv::install_dinput_hooks();
        hv::install_file_hooks();
        hv::start_audio_mute();
    }
    return TRUE;
}
