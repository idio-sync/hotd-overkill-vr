#include "common.h"

#include "../vr/harness.h"

namespace {

void log_plan(const char* what, const hv::vr::HarnessPlan& p) {
    hv::logf("harness (%s): %s; hooks %s; save %s; window %s; sound %s; pad %s; command file %s; asset/shader logging %s", what,
             p.dev ? "development" : "release", p.hooks ? (p.vr ? "for VR" : p.game_hooks ? "for the development harness, VR off" : "the harness's own only (exe refused: nothing in the game's code)") : "none (plain XInput pass-through)",
             p.redirect_save ? "redirected to hovr\\AppData (the real save is not touched)" : "the game's own folder (Steam Auto-Cloud syncs it)",
             p.minimized ? "minimized, activation faked (probe.ini minimized)" : "the game's own", p.mute ? "muted (probe.ini mute)" : "on",
             p.fake_pad ? "scripted, always connected (probe.ini fake_pad)" : "the real one", p.command_file ? "hovr\\cmd.txt polled" : "none",
             p.dev_logging ? "on" : "off");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        hv::init_paths();  // also decides development vs release (hovr\probe.ini, [vr] dev=1)
        hv::log_open();
        hv::logf("hovr loaded into %ls, exe base %p", hv::g_game_dir.c_str(), (void*)hv::exe_base());
        // The game hooks patch addresses of one build of the game: on any other build, or a copy
        // changed where the mod hooks or reads it, a release install stays a plain XInput
        // pass-through (development keeps only the harness's own import hooks: harness.h).
        const bool exe_ok = hv::exe_is_supported();
        if (!exe_ok) {
            hv::logf("HOTD_NG.exe is not the supported build (Steam 569304) or a compatible patched copy of it; "
                     "no hooks installed%s", hv::dev_harness() ? " in the game (development: the harness's own only)" : "");
            if (!hv::dev_harness()) return TRUE;
        }
        // A release install has no hovr.ini until this first start writes the defaults (mode=openvr);
        // an existing one is never replaced. Development (probe.ini) keeps its own files as they are.
        if (!hv::dev_harness()) hv::write_default_settings();
        hv::load_vr_config();
        // Shift held while the game starts: VR off for this run (release: nothing hooked at all).
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool vr_mode = hv::g_vr.mode != hv::VrMode::off;
        const hv::vr::HarnessPlan plan = hv::vr::plan_harness({hv::dev_harness(), false, vr_mode, shift, exe_ok});
        if (!exe_ok) {
            hv::g_vr.mode = hv::VrMode::off;
            hv::g_vr.stereo = false;
        }
        if (shift && vr_mode) {
            hv::logf("vr: Shift held at start: VR off for this run");
            hv::g_vr.mode = hv::VrMode::off;
            hv::g_vr.stereo = false;
        }
        log_plan("this run", plan);
        // Development: also log what a release install would do with the same settings (dry run).
        if (plan.dev) log_plan("dry run of a release install", hv::vr::plan_harness({false, false, vr_mode, shift, exe_ok}));
        if (!plan.hooks) return TRUE;
        // The exe's imports are bound before DLL initializers run, so its IAT can be patched here.
        if (plan.redirect_save) hv::install_folder_hook();  // refuses to run if it fails
        hv::install_window_hooks();
        if (plan.dev_logging) {
            hv::install_compile_hooks();
            hv::install_file_hooks();
        }
        hv::install_d3d_hooks(plan.game_hooks);
        if (plan.game_hooks) hv::install_dinput_hooks();
        hv::start_audio_mute();  // development only (probe.ini mute)
    }
    return TRUE;
}
