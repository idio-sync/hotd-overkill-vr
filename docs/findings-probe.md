# Probe findings (2026-11-17)

Throwaway spike: a stand-in `XINPUT1_3.dll` (src/probe) in a test copy of the game
(`C:\Users\Virtual\hotd-vr-game`: hard-linked assets, copied executables). Static analysis is in
findings-static.md. Everything below was observed in the running game unless marked otherwise.

## Running it
- `build.cmd` → out\XINPUT1_3.dll; `tools\run.ps1 [-Stop] [-Normal]`; `tools\cmd.ps1 'cmd' ...`;
  `tools\step.ps1 NAME 'cmd' ...` (runs commands, then a screenshot + half-size copy);
  `tools\cmd.ps1 -File tools\scripts\to_gameplay.txt` right after run.ps1 reaches the first kitchen
  fight (~2 min; boot time varies, check with a screenshot). Commands: header of src/mod/control.cpp
  (the probe became the mod in src/mod; same DLL, same harness).
- Settings: `hovr\probe.ini` [probe] minimized, mute, windowed, fake_pad, cap_fps, vsync, gun_hook
  (development harness). The mod's own settings: `hovr\hovr.ini` [vr] (keys in src/mod/config.cpp);
  `tools\run.ps1 -Mode off|sim` sets its mode line.
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

## Weapon models (the game's own)
- The loadout screen's gun is a 2D picture (only 2D draws there). Gameplay draws no gun; the only 3D
  draws on the HUD target are the ammo rounds (vs 3a5de9a696514b3a, 1036 triangles each, one per round).
- Asset packs are zlib-compressed (`tools/pc_scan.py` inflates them and lists names). Per level:
  - `<level>_hud_<weapon>.pc` (9 weapons: pistol_magnum, pistol_gold_magnum, pistol_handcannon,
    shotgun_12gaugepump, shotgun_auto, mg_machinepistol, mg_assaultrifle, minigun, crossbow) each hold
    a first-person gun mesh `GunArms_<Weapon>` + magazine `GunArms_<Weapon>_Clip` (gold magnum:
    `GunArms_Gwendalin`, no clip) + the HUD round `HUD_Bullet_<Weapon>`; textures e.g. handgun_g_t/n/s.
  - `<level>_hud_arms{w,g,v,c}.pc` = Washington, Agent G, Varla, Candi: `GunArms_<Char>_Mesh` plus
    per-weapon reload animations (`_Magnum_Reload`, `_ShotGunPump_Reload`, `_Minigun_Overheat`,
    `_Gwen_Taunt`, ...).
- Level start opens hud_permasector, the equipped weapon's hud pack (only equipped weapons) and
  hud_armsw + hud_armsg (players 1/2) — the models are resident during gameplay but never drawn, in
  reload or otherwise (a cut first-person feature?). The exe has no "GunArms" strings (data-driven);
  the only related class is `cMeleeArms`. File opens go through MSVCR100 (hooked in files.cpp).

## Not checked
- HUD panel extraction, WiiView edge-pan (camera rides the rail during tests; static analysis
  found the enable byte), OpenVR submit from a D3D9 device, origin override, other weapons
  (type 5 path).
- Gameplay drains health quickly while idle; tests have to act right after reaching gameplay
  (continue = click (378,474) on the death screen).

## Stereo in sim mode (milestone 1 part 1, 2026-11-17)
- Same-frame stereo by per-draw duplication (src/mod/stereo.cpp): with `mode=sim` the screen-sized
  targets are double-wide (scene 2560x720 at eye_res 1280x720); each perspective draw on the scene
  target is drawn into each half with `C_eye * M` for its clip matrices (cWVP, cProject,
  cObjectViewProject, cProj) and eyePos moved per eye. ~310-340 scene draws per frame are
  duplicated in the kitchen; ~230 fps uncapped in sim mode over RDP.
- The engine's projection (0x9C7DC0) and view (0x9C7E48) globals at draw time equal
  cWVP * cObjectMatrix^-1 of the scene draws (`vr check`: max difference 1.2e-7 over 297 draws).
  Projection: sx 0.8391, sy 1.4917 (100 x 67.7 deg), near 5, far 18000.
- Units: camera at y = 236, zombies' roots (cObjectMatrix translation) at y = 73: eye height
  1.6 m (roots on the floor) to 2.4 m (floor at y = 0) at 1 unit = 1 cm. unit_m stays 0.01.
- Disparity at yaw 0, ipd 0.064 (tools/disparity.py): far walls -3..-6 px, floor -11..-18 px, a
  zombie close by -22..-26 px in a 1280-wide eye; no vertical offset.
- Not every draw on the scene target is perspective: screen overlays (blood on the screen, the death
  screen's backdrop) are orthographic and read no screen-sized texture; they go into each eye's half
  unchanged. Orthographic draws that read a resized target are drawn once across both halves.
- The DoF/SSAO pass's "texZBuffer" is a 16x16 dummy texture in both modes (no real depth input).
- Culling: the frustum builders 0x6DEDE0/0x6DF810 decide dynamic objects (zombies): `vr cone -45`
  (8 deg frustum) removed a zombie near the edge; cone 35 vs cone 0 at yaw 40 adds 14-23 draws.
  Static room geometry is not culled by them.
- Frame limiter: natively `[[0x9F09E0]+0x48]` = 1 (menus then run at ~31 fps); the game rewrites it
  at level start. The mod clears it only while the scene is drawn in stereo and restores it after.
- Device lost: in 2 of 6 early sim runs (frame limiter cleared from boot) the D3D9 device was lost
  during the intro (69 s / 94 s); TestCooperativeLevel stays lost and the game loops in 0x6786E0
  ("D3D device lost, waiting for ready state") forever while minimized. One loss coincided with DWM
  "registered the session port" events (RDP session churn), so not certainly the mod's doing.
- The game blurs/zooms the whole picture when the player is hit or dying; back-buffer shots taken
  then look smeared - check `vr scene` (the scene target itself) before suspecting stereo.
