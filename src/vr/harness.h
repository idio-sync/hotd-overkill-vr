#pragma once
// Release vs development (design: "Process and files"; milestone 4). Pure decision logic, unit
// tested (tests/test_stereo.cpp); src/mod/dllmain.cpp applies it.
//
// Development = the folder holds hovr\probe.ini (the test copy does), or hovr.ini [vr] dev=1:
// the probe's harness as before - every user folder redirected into hovr\ (the real save,
// %APPDATA%\sega\typing of the dead overkill, synced by Steam Auto-Cloud, is never touched),
// probe.ini's minimized/mute/fake_pad/windowed/vsync/cap_fps keys (defaults: minimized, muted, fake
// pad), the command file hovr\cmd.txt and status.txt, asset-open and shader-compile logging, log
// hovr\probe.log. Hooks are installed whatever the mode, so the redirect is always on.
//
// Release = no probe.ini and no dev=1: the game's own save folder (Steam Auto-Cloud keeps syncing
// it), a normal window, sound on, the real XInput pad, no command file or status file, no asset
// logging, log hovr\hovr.log. With mode=off - or Shift held while the game starts - nothing is
// hooked at all: the DLL is a plain XInput pass-through. VR modes hook only what VR needs (Direct3D,
// DirectInput for the controller buttons, the cursor for menu pointing) and keep the window as the
// game makes it (forced windowed: the D3D9Ex hand-off needs it), unless [vr] keep_active=1 asks
// for the game to keep running when its window loses focus.

namespace hv::vr {

struct HarnessIn {
    bool probe_ini = false;  // hovr\probe.ini exists
    bool dev_key = false;    // hovr.ini [vr] dev=1
    bool vr_mode = false;    // [vr] mode is sim or openvr
    bool shift = false;      // Shift held while the DLL loaded
};

struct HarnessPlan {
    bool dev = false;            // development harness
    bool vr = false;             // VR runs (mode != off and no Shift)
    bool hooks = false;          // install any hooks at all (false: plain XInput pass-through)
    bool redirect_save = false;  // SHGetFolderPathA user folders -> hovr\ (refuse to run if it fails)
    bool command_file = false;   // poll hovr\cmd.txt, write hovr\status.txt
    bool dev_logging = false;    // asset-open and shader-compile logging
    bool probe_ini_keys = false; // probe.ini [probe] keys apply (else the release values below)
    // Release values of the probe.ini keys (dev: probe.ini, with the dev defaults in brackets).
    bool minimized = false;      // [1] minimized window with faked activation
    bool mute = false;           // [1] mute this process's audio sessions
    bool fake_pad = false;       // [1] player 1's pad always connected (scripted)
    bool windowed = true;        // [1] force a windowed device (VR: always)
};

inline HarnessPlan plan_harness(const HarnessIn& in) {
    HarnessPlan p;
    p.dev = in.probe_ini || in.dev_key;
    p.vr = in.vr_mode && !in.shift;
    p.hooks = p.dev || p.vr;
    p.redirect_save = p.dev;
    p.command_file = p.dev;
    p.dev_logging = p.dev;
    p.probe_ini_keys = p.dev;
    p.minimized = p.dev;  // dev defaults; probe.ini may change them
    p.mute = p.dev;
    p.fake_pad = p.dev;
    p.windowed = true;
    return p;
}

}  // namespace hv::vr
