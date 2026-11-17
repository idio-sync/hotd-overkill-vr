# Probe findings (2026-11-17)

Throwaway spike: a stand-in `XINPUT1_3.dll` (src/probe) in a test copy of the game
(`C:\Users\Virtual\hotd-vr-game`: hard-linked assets, copied executables). Static analysis is in
findings-static.md. Everything below was observed in the running game unless marked otherwise.

## Running it
- `build.cmd` → out\XINPUT1_3.dll; `tools\run.ps1 [-Stop] [-Normal]`; `tools\cmd.ps1 'cmd' ...`;
  `tools\step.ps1 NAME 'cmd' ...` (runs commands, then a screenshot + half-size copy);
  `tools\cmd.ps1 -File tools\scripts\to_gameplay.txt` right after run.ps1 reaches the first kitchen
  fight (~2 min; boot time varies, check with a screenshot). Commands: header of src/probe/control.cpp.
- Settings: `hovr\probe.ini` [probe] minimized, mute, windowed, fake_pad, cap_fps, vsync, gun_hook.
- Launch HOTD_NG.exe directly (no Steam launch needed; steam_appid.txt is enough). The probe
  redirects `SHGetFolderPathA` (CSIDL_APPDATA etc.) into `hovr\AppData`, seeded once from the real save;
  the real `%APPDATA%\sega\typing of the dead overkill` (Steam Auto-Cloud) is never touched.
  The probe refuses to run if that redirect fails.

## Copy protection
- SecuROM PA (totdo.dll) loads the stand-in DLL without complaint, and an inline 5-byte detour on
  `cWeapon::FireBullets` (installed at the first Present) ran for minutes of gameplay with no reaction.

## Input
- Mouse position: `GetCursorPos` (faked by the probe). Mouse buttons and keyboard: DirectInput 8,
  immediate mode (`GetDeviceState`, DIMOUSESTATE 16 bytes / 256-byte key array), cooperative level
  NONEXCLUSIVE|FOREGROUND. Posted window messages alone do nothing.
- The XInput pad (faked as connected) did not move the menu cursor or selection.
- Menus are cursor-driven (the gun reticle is the menu pointer).

## Rendering (frame logs)
- Device: windowed is forced; the game asked for 1536x807 fullscreen, 4x MSAA back buffer.
- Per frame: shadow maps (1024² R32F + D24S8) → scene at a fixed **1280x720** ARGB8 → bloom/blur
  chain (640x180 RGBA16F, 320x180, 160x90, 640x360) → one composite draw into a **1291x807** target →
  **HUD: ~190 textured 2D DrawPrimitiveUP draws on that same target** (vs 1a10d46e76d587ab, ps
  73edbf0012ec0901) → one draw of the 1291x807 target into the back buffer (viewport x=122, pillarbox).
- All shaders are precompiled (D3DXCompileShader is never called). Every 3D vertex shader computes
  `oPos = dp4(v0, c0..c3)` with **cWVP** in c0..c3 (column-vector rows). Other view-dependent constants:
  cViewMatrix (shadow passes), eyePos (lighting), cShadowMapMatrix, cBoneMatrix (world-space bones).
- Camera recovered from cWVP·cObjectMatrix⁻¹ (tools/frame_camera.py): **hfov 100°, vfov 68°, 16:9**,
  centred projection, one VP per frame. eyePos ≈ (24, 236, 2800) at the kitchen start; units probably cm.

## Timing
- Gameplay and cutscenes: ~64 fps cap (frame time ≈15.6 ms, Sleep granularity); menus uncapped.
- `poke [va:0x9f09e0]+0x48 0` (the `disable-fps-limit` flag) → gameplay ran at **~256 fps** (827 draws).
- Game time (va 0x9F0B50) advanced 5.001 s over 5 s of wall time at dt ≈ 3.5 ms: variable timestep,
  real-time speed when uncapped. So the game can render at headset rate (90/120 Hz).

## Shots
- `FireBullets` origin = camera position (equals eyePos), direction = unit vector toward the cursor.
- Override test: control shots with the cursor in the top-right corner scored nothing; then one shot
  at a zombie captured its direction and two shots fired with the cursor in the corner but the
  direction overridden killed that zombie (score 0 → 100). The game's own ray cast follows the
  direction the mod supplies. (Origin override not exercised yet.)

## Not checked
- Stereo rendering itself (no per-eye render yet), HUD panel extraction, WiiView edge-pan (camera
  rides the rail during tests; static analysis found the enable byte), culling with a wider FOV,
  OpenVR submit from a D3D9 device, units (cm guess), origin override, other weapons (type 5 path).
- Gameplay drains health quickly while idle; tests have to act right after reaching gameplay
  (continue = click (378,474) on the death screen).
