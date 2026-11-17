# House of the Dead: Overkill VR — design

2026-11-17. Status: draft for approval (builder check done; the user answered its two questions).

## Intent

A native VR mod for the **House of the Dead: Overkill** mode inside *The Typing of the Dead:
Overkill* (Steam app 246580, the only build: 569304 / RC14). The experience to aim for is the
Time Crisis VR mods (DR-89): the original on-rails game, seen in stereo with head tracking, with a
tracked gun in your hand that shoots where it points. The difference from those mods is that we have
no decompilation: this is a hook mod on the shipped 32-bit Direct3D 9 executable, built the same way
as the user's Killer7 VR mod (github idio-sync/k7-vr).

The player is the user, playing on PC VR through SteamVR (Steam Frame headset and controllers).

## Goals (v1)

1. **Stereo, head-tracked gameplay.** Both eyes rendered from the same game frame, the head pose
   added on top of the game's rail camera, at the headset's refresh rate.
2. **The controller is the gun.** Shots go exactly where the gun points (the game's own ray cast,
   fed the controller's position and direction). Trigger fires; reload and weapon switching are on
   controller buttons.
3. **The game's own weapon model in hand.** The first-person gun mesh the game ships for the current
   weapon (`GunArms_<Weapon>`), following the controller and switching when the weapon changes;
   its magazine (`GunArms_<Weapon>_Clip`) drops out and comes back on reload. No arms or hands.
4. **The HUD stays readable**: drawn on a panel in front of the player, not glued to the eyes.
5. **Menus and cutscenes are usable in the headset**: shown on a virtual screen; the controller
   points at menu items like a light gun.
6. **Never harms the user's install or save.** The mod lives in the game folder as files the user
   installs; the Steam folder is never written by tooling; the save is untouched unless the user plays.

## Non-goals (v1)

- Hands or arms (the game ships first-person arms with per-weapon reload animations; a later version
  can try them once the gun has been seen in the headset).
- The typing modes (story/minigames). They may happen to work in stereo; nothing is done for them.
- Two players (local co-op), online, roomscale movement, physical cover/ducking (HotD has no cover).
- Free look-around beyond a cone around the rail direction (an A/B test after v1, see Rendering).
- Repairing weapon meshes that look wrong from other angles.
- Supporting any other build of the game, or any OS other than Windows.
- OpenXR. v1 uses OpenVR (the user's runtime is SteamVR, and k7-vr's OpenVR code is reusable).

## Anti-goals (would count as failure even if it works)

- Aiming that is "close": if the shot doesn't land where the gun barrel points, it has failed
  (lesson from Killer7: verify the barrel against the hit, not mod-internal proxies).
- A laser pointer on by default (the user removed it in Killer7). A small hit marker or none.
- HUD or 2D elements glued to the eyes.
- Writing to the real save or the Steam folder during development.
- Making the game run at its original 60 Hz in the headset (it must run at headset rate).

## What the probe established (docs/findings-probe.md, docs/findings-static.md)

- The stand-in `XINPUT1_3.dll` loads; SecuROM PA (totdo.dll) tolerated it and an inline code detour.
- Every 3D vertex shader computes `oPos = pos · cWVP` with **cWVP in c0..c3**; other view-dependent
  constants are `eyePos` and `cViewMatrix`. Camera: 100° × 68° FOV, 16:9, one view-projection per
  frame; units probably centimetres (unconfirmed).
- Frame structure: shadow maps → scene at a fixed 1280x720 → bloom/blur chain at quarter/eighth size →
  one composite draw into a 1291x807 target → **HUD** (2D draws plus the 3D HUD rounds) on that target →
  copy to the back buffer.
- The 60 Hz cap is the flag `[[0x9F09E0]+0x48]`; clearing it gives ~256 fps in gameplay. Game time
  uses a variable timestep and stays real-time when uncapped.
- `cWeapon::FireBullets` (0x50D110) takes the shot origin (= camera position) and direction
  (`cWeapon+0x7B4`); overriding the direction steered real hits.
- The first-person weapon meshes and magazines are in `<level>_hud_<weapon>.pc`, loaded with the
  level for the equipped weapons, and never drawn.
- Input: mouse position via `GetCursorPos`, buttons and keyboard via DirectInput 8; menus are
  cursor-driven.

## Approaches considered

**Stereo rendering**
1. *Chosen:* **every 3D scene draw issued twice into a side-by-side (double-wide) target**, each with
   its eye's corrected matrix and its half of the viewport. Same-frame stereo, works on plain D3D9,
   no shader patching for geometry. Screen-sized targets (the 1280x720 family and its blur chain) are
   created double-wide so full-screen post-processing passes keep working.
2. The engine's dormant stereoscopy path (`[0x9F09E0]+0x3F8`, eye matrix at `+0x470`) — might give
   engine-side culling per eye, but unproven and more reverse engineering. Kept as a fallback.
3. Alternate eyes on alternate frames — rejected: the eyes would show different moments of the game.

**Getting the shot to the controller**
1. *Chosen:* **override origin and direction inside the game's shot path** (FireBullets and the other
   cursor-driven checks), so the game's own ray cast decides hits.
2. Move the 2D cursor to where the controller ray meets the scene (Killer7's method) — rejected:
   depends on depth readback, breaks when the gun occludes the ray, and fires from the wrong place.

**The weapon model** (to be decided by the builder after investigation, see "Left to the builder")
1. Have the engine instance and draw the resident `GunArms_<Weapon>` model, then set its transform.
2. Find the resident mesh's GPU buffers, textures and shaders, and draw them ourselves.
3. Extract meshes offline from the `.pc` packs (zlib) — the fallback; nothing extracted may ship.

The user picked the game's own weapon models over a bundled pistol or no model, and gun only (no
hands) for v1.

## The design

### Process and files
- One DLL: a stand-in `XINPUT1_3.dll` in the game folder (as the probe), forwarding XInput to the
  system and hooking the game's imports (d3d9, DInput8, USER32, SHELL32, KERNEL32/MSVCR100) and,
  where needed, short inline detours on game functions (installed after SecuROM's start-up, each
  checking its expected bytes first and skipping itself with a log line if they differ).
- Refuses to run its hooks (and logs why) unless `HOTD_NG.exe` matches the known build's hash.
- Settings in an ini next to the DLL; a log file; `openvr_api.dll` (32-bit) shipped beside it and
  delay-loaded.
- Modes: `off` (pass-through), `sim` (no headset: side-by-side eyes on the desktop window, scripted
  head/controller poses — for development and automated tests), `openvr`.
- Development keeps the probe's harness: test copy of the game, save redirect, minimized and muted
  window, command file, screenshots, frame logs. Release builds default to the real save and a
  normal window.

### Rendering
- **Device**: create the device as D3D9Ex (`Direct3DCreate9Ex`) so eye textures can be shared with a
  D3D11 device that submits to SteamVR. Windowed always; the desktop window shows a mirror.
- **Eye targets**: the game's screen-sized targets are created at eye resolution × 2 wide (eye size
  from SteamVR's recommended size, scalable in the ini); shadow maps and other non-screen targets
  stay as they are.
- **Per-draw stereo**: for each scene draw on those targets, draw left then right with
  `cWVP_eye = P_eye · E_eye · P_game⁻¹ · cWVP` (column-vector convention; `P_game` = the frame's game
  projection, `E_eye` = head pose and eye offset in the game camera's space, in game units) and the
  eye's half viewport; `eyePos` per eye. Full-screen passes (post-processing) are drawn once over
  the whole double-wide target.
- **Look-around range (user's decision)**: v1 supports head turns within a **limited cone around the
  rail camera's direction** (default about ±35° yaw, adjustable in the ini). Culling: before the frame
  is built, the game camera's FOV is widened enough to cover the headset's FOV plus that cone, so
  off-centre objects aren't culled; the per-eye projection undoes the widening for display. Past the
  cone, objects may be culled — accepted for v1 (a gentle fade to the edge is the builder's call).
- **Free look-around is a planned follow-up**: after v1 lands, an A/B test against free look, where
  the game camera itself is turned with the head before culling (so culling follows the gaze, with no
  cone), to see how unfinished off-rail scenery looks. v1 should not make that harder (keep the
  head-to-game-camera transform in one place).
- **HUD**: everything drawn on the 1291x807 target after the composite draw goes to a separate HUD
  texture instead, shown on a panel ~1.5 m in front at a comfortable size. The game's crosshair is
  hidden in gameplay (an optional small marker where the shot will land; off by default).
- **Menus, loadout, death screen and cutscenes**: shown flat on a large virtual screen (the composited
  frame), with no stereo and no head-locked camera. Gameplay vs flat is detected from the game state
  where possible, otherwise from the frame structure (scene target drawn or not).
- **Frame rate**: the 60 Hz cap flag is cleared; SteamVR's pose wait paces frames at the headset rate.

### Head and camera
- The game's rail camera (position and orientation, including its scripted turns) is the head's
  origin; the tracked head pose (relative to a recentre point) is added on top.
- Comfort defaults: the game's camera shake and head-bob off (`CameraSetShakeSettings`,
  `CameraSet*HeadBob` paths) with an ini switch to restore them; the WiiView edge-pan
  (`[[0x99B8F4]+0x134]`) always off.
- World scale: game units → metres from the measured unit (probably 1 unit = 1 cm), adjustable.
- Recentre: a controller button (held) and on level start.

### Gun and input
- **Aim**: the controller's pose (with the Steam Frame controller's grip/barrel offset; the render
  model reports as oculus_quest2 with the tip ~40° below the raw axis, per Killer7) gives a world
  origin and direction through the same head-to-game transform. Every shot path is overridden:
  `FireBullets` (0x50D110), the weapon-type-5 path (0x4E1330), and the cursor-driven sphere checks
  (0x42B900, 0x479250) that pick up items and hit targets.
- The game's 2D cursor is still driven (projection of the aim point into the game view) so anything
  else that reads it — and the game's own aim logic — stays consistent.
- **Buttons** (injected through the DInput mouse/keyboard the game already reads): trigger = fire
  (held for automatic weapons), a face button = reload, another = switch weapon, a menu button =
  pause. Exact mapping in the ini. A light-gun style "point away from the screen and fire" reload
  gesture is optional.
- **Haptics**: a short pulse per shot, stronger for shotguns/hand cannon.
- **Menus**: the controller points at the virtual screen; the hit point becomes the game cursor;
  trigger clicks.

### The weapon model
- The model for the current weapon (`GunArms_<Weapon>`) is drawn in both eyes at the controller's
  pose (scaled to the world scale, with a per-weapon grip offset in the ini), lit by the game where
  possible. It switches with the weapon. The magazine mesh follows the gun and, on reload, drops and
  returns (a short procedural animation keyed to the game's reload state).
- Shots and muzzle effects should come from the barrel; if the game's effects are drawn at the camera,
  they are moved or hidden.
- **If a weapon's model looks broken in hand (user's decision)** — first-person view models are often
  built only for the side the player sees, or posed by the arm skeleton — that weapon shows **no
  model** (the aim and everything else still work); a per-weapon ini switch decides, defaulting to the
  builder's judgement after the headset check. No time goes into repairing meshes in v1; broken
  ones are revisited together with the hands work.

## Constraints

- 32-bit, MSVC (VS 18 Build Tools, vcvars32), CMake + C:\ninja.exe, C++20 — as k7-vr.
- Direct3D 9 (no geometry shaders, no instancing tricks for stereo); OpenVR submit from D3D11 via
  shared textures.
- SecuROM PA patches ~20 KB of .text at runtime (stubs at 0xA71000..0xA71844); hooks must avoid them.
- The game must stay playable with the mod in `off` mode, and uninstall = delete the files.
- Game assets may not be redistributed (no extracted meshes or textures in the repo or releases).
- Tests run on the test copy only, minimized and muted; the user playtests in the headset.

## Milestones (for the plan)

1. **Stereo + head tracking** in sim and OpenVR: D3D9Ex, double-wide targets, per-draw stereo,
   culling, uncapped frame rate, HUD panel, flat screen for menus/cutscenes.
2. **Controller gun**: aim override on every shot path, buttons, haptics, menu pointing.
3. **The weapon model**: find and draw `GunArms_<Weapon>`, weapon switching, magazine reload.
4. **Comfort and release**: shake/head-bob/edge-pan off, recentre, world-scale check, install and
   uninstall scripts, README with a playtest checklist.

Each milestone ends with a sim-mode check by the builder and a headset playtest by the user.

## Left to the builder

- The method for drawing the weapon model (the three options above), after investigation.
- How weapon switching and reload are triggered in this build (mouse wheel/keys/buttons) and
  where the game's reload state can be read.
- The exact detection of gameplay vs menus/cutscenes.
- Draw classification details (which targets count as "screen-sized", which draws are full-screen
  passes vs scene geometry, handling of the 3D HUD rounds).
- Haptic strengths, panel distances and sizes, default button mapping.
- Code layout; reuse of k7-vr pieces (head math, OpenVR plumbing, sim mode, harness).
