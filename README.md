# House of the Dead: Overkill VR

A VR mod for the **House of the Dead: Overkill** mode inside *The Typing of the Dead: Overkill*
(Steam app 246580, build 569304). The game is shown in stereo with head tracking through SteamVR;
its HUD floats on a panel in front of you; menus, the loadout screen, cutscenes, the pause menu and
the death screen appear on a large flat virtual screen. Design: `docs/superpowers/specs/2026-11-17-hotd-overkill-vr-design.md`.

Status: **milestone 1** (stereo + head tracking, HUD panel, flat screen). The gun still aims with
the mouse; controller aiming, buttons and the weapon model are milestones 2 and 3.

## How it works (short)

- `XINPUT1_3.dll` is a stand-in XInput DLL the game loads from its own folder. It forwards XInput
  to Windows and hooks Direct3D 9. Nothing in the game's files is changed; delete the files to
  uninstall. It only hooks the known build of `HOTD_NG.exe` (others: plain XInput pass-through).
- Each 3D scene draw is issued twice, once per eye, into double-wide render targets. The game's
  60 Hz frame limiter is lifted during gameplay; SteamVR's pose wait paces the game at the
  headset's rate.
- In VR modes the game's Direct3D 9 device is created as Direct3D 9Ex so the eye images, HUD and
  flat screen can be shared with a Direct3D 11 device that submits them to SteamVR (eyes) and
  shows them as SteamVR overlays (HUD panel, flat screen).

## Build

`build.cmd` (MSVC Build Tools x86, CMake, `C:\ninja.exe`) → `out\XINPUT1_3.dll` and
`out\openvr_api.dll`. Unit tests: `ctest --test-dir out` inside a vcvars32 shell.

## Install

Copy into the game folder (next to `HOTD_NG.exe`):

- `out\XINPUT1_3.dll`
- `out\openvr_api.dll` (32-bit OpenVR 2.15.6; only loaded in `mode=openvr`)
- create `hovr\hovr.ini` (the mod makes the `hovr` folder on first start) with the settings below.

Development uses a test copy of the game (`C:\Users\Virtual\hotd-vr-game`): `tools\run.ps1
-Mode off|sim|openvr` copies the DLL there and starts it minimized and muted with the save
redirected to `hovr\AppData` (dev harness settings in `hovr\probe.ini`). For a headset session in
the test copy, set `minimized=0` and `mute=0` in `hovr\probe.ini` (or start with `-Normal`) so the
window and sound behave normally. The save redirect stays on in the test copy.

Uninstall: delete `XINPUT1_3.dll`, `openvr_api.dll` and the `hovr` folder.

## Settings: `hovr\hovr.ini`, section `[vr]`

| key | default | meaning |
|---|---|---|
| `mode` | `off` | `off` = the game untouched; `sim` = no headset, the desktop window shows what the headset would; `openvr` = SteamVR. Holding **Shift** while the game starts, or SteamVR failing to start, means `off` for that run. |
| `eye_res` | `auto` | per-eye render size: `auto` (openvr: SteamVR's recommended size × `eye_scale`; sim: 1280x720) or `WxH` |
| `eye_scale` | `1.0` | multiplier on the recommended size (lower it if the frame rate can't keep up) |
| `unit_m` | `0.01` | metres per game unit (world scale; the game works in centimetres) |
| `ipd_m` | `0.064` | sim only; openvr uses the headset's |
| `cone_deg` | `35` | how far (degrees) you can turn your head from the rail camera's direction before objects may be culled |
| `uncap` | `1` | lift the game's 60 Hz limiter in gameplay |
| `crosshair` | `0` | `1` keeps the game's crosshair on the HUD panel |
| `hud_distance`, `hud_width`, `hud_drop` | `1.5`, `1.3`, `0` | HUD panel: metres in front, metres wide, metres below eye height |
| `screen_distance`, `screen_width`, `screen_drop` | `3.0`, `3.2`, `0` | flat screen for menus/cutscenes |
| `mirror` | `1` | desktop window shows the VR view (eyes side by side + HUD, or the flat screen) |
| `handoff` | `1` | sim: also run the Direct3D 9Ex → 11 hand-off (to test it without a headset) |
| `handoff_sync` | `1` | wait for the game's GPU work before Direct3D 11 reads it (0 = flush only) |
| `sim_yaw`, `sim_pitch` | `0` | sim head pose, degrees |

## Commands (development)

Write lines to `hovr\cmd.txt` (`tools\cmd.ps1 'cmd' ...`; `tools\step.ps1 NAME 'cmd' ...` adds a
screenshot). The full list is at the top of `src/mod/control.cpp`. VR ones:

- `vr recentre` — recentre the view (also happens automatically when a level starts).
- `vr state` — log the game state (level / cutscene / paused) and whether the frame is stereo.
- `vr handoff NAME` — save what the Direct3D 11 side holds: `NAME_eyes.png` + `NAME_hud.png`
  (gameplay) or `NAME_screen.png` (flat), alpha in `_a.png`.
- `vr crosshair 0|1`, `vr yaw D`, `vr pitch D`, `vr ipd M`, `vr unit M`, `vr cone D`,
  `vr mode off|sim` (stereo on/off live), `vr scene NAME`, `vr targets`, `vr check`.

## Headset playtest checklist (milestone 1)

Before starting: SteamVR running, headset on, `hovr\hovr.ini` has `mode=openvr`; in the test
copy `hovr\probe.ini` has `minimized=0` and `mute=0`. Start the game (`tools\run.ps1 -Mode openvr
-Normal`, or `HOTD_NG.exe` directly). If anything goes wrong, hold Shift while starting to play
flat, and send `hovr\probe.log`.

1. **Start-up**: the log (`hovr\probe.log`) shows `vr: OpenVR up, recommended WxH per eye` and the
   eye tangents; SteamVR shows the game as the running app (not the "loading" grid).
2. **Main menu**: a large flat screen about 3 m ahead shows the menu, sharp and readable; the
   eyes are black around it. The mouse still drives the menu (look at the desktop window if
   needed). Note whether the screen size/distance is comfortable.
3. **Loadout screen** (Story → Papa's Palace of Pain → Continue/Start): flat and sharp.
4. **Intro cutscene**: shown on the flat screen (no stereo, no head tracking), subtitles readable.
   Skipping with Escape works.
5. **Gameplay** (first kitchen fight): the world is in stereo at a sensible scale (the room
   should feel life-sized; zombies about human height), head tracking is smooth with no lag or
   judder at the headset's refresh rate, the rail camera's motion carries you along.
   - Turn your head left/right up to ~35°: no objects should pop out at the edges.
   - Look down/up: the view follows; lean in: parallax is correct (near objects move more).
   - The HUD (health, score, ammo rounds, grenades) floats on a panel ~1.5 m ahead, world-locked
     (it stays put when you turn your head), readable, and not doubled.
   - The game's crosshair is **not** shown.
   - Both eyes line up (no vertical offset, no eye strain); nothing is mirrored or swapped.
   - Report the SteamVR frame timing (frames dropped/reprojected) and the `fps` lines in the log.
6. **Pause** (Escape): the pause menu appears on the flat screen; resuming returns to stereo.
7. **Death screen** (let the zombies win): "Continue?" on the flat screen; clicking Yes returns
   to stereo gameplay.
8. **Recentre**: the view recentres when a level starts. (A controller button for it comes with
   milestone 2; until then, `vr recentre` in the command file.)
9. **Quit** from the pause menu: back to the menu on the flat screen; quitting the game leaves
   SteamVR running normally.

Things to note for the next milestone: whether the HUD panel size/distance/height is right
(`hud_width`, `hud_distance`, `hud_drop`), whether the flat screen is too big or too close, the
world scale (`unit_m`), and whether head turns beyond the cone show missing objects.
