# House of the Dead: Overkill VR

A VR mod for the **House of the Dead: Overkill** mode inside *The Typing of the Dead: Overkill*
(Steam app 246580, build 569304). The game is shown in stereo with head tracking through SteamVR;
the controller is the gun (the game's own weapon model in your hand, shots go along its barrel);
the HUD floats on a panel in front of you; menus, the loadout screen, cutscenes, the pause menu and
the death screen appear on a large flat virtual screen. Design: `docs/superpowers/specs/2026-11-17-hotd-overkill-vr-design.md`.

Status: **milestone 4** (comfort and release): the game's camera shake, hit jolts, head-bob and
WiiView edge-pan are off in VR, its own first-person reload animation is hidden, the view
recentres whenever gameplay resumes, and there is an installer for the Steam copy. Milestones 1-4
have been checked in sim mode (no headset) only; the playtest checklist below covers all of them
for the first headset session.

## How it works (short)

- `XINPUT1_3.dll` is a stand-in XInput DLL the game loads from its own folder. It forwards XInput
  to Windows and hooks Direct3D 9. Nothing in the game's files is changed. It only hooks the known
  build of `HOTD_NG.exe` (others: plain XInput pass-through), and with `mode=off` (or Shift held
  while the game starts) it hooks nothing at all.
- Each 3D scene draw is issued twice, once per eye, into double-wide render targets. The game's
  60 Hz frame limiter is lifted during gameplay; SteamVR's pose wait paces the game at the
  headset's rate.
- The gun (right controller by default) aims the game's own shots: every shot path of player 1
  (the bullet ray cast, the projectile weapon, the pickup/target checks) is fed the controller's
  position and direction instead of the camera's ray through the mouse cursor, so shots go where
  the gun points, from where the gun is. The game's cursor is moved to where the gun's ray meets
  the scene, so the game's own aim logic agrees. The controller's buttons become the game's
  mouse/keyboard input; the real mouse is ignored while the controllers are in use.
- The gun in your hand is the game's own first-person mesh (`GunArms_<Weapon>`, shipped in the
  game's hud packs and loaded with every level). The mod finds the resident mesh's GPU buffers and
  textures through the current weapon and draws them in both eyes, depth-tested against the scene.
  Shots fly along the drawn barrel, from its muzzle. Nothing is extracted from the game's files.
- Comfort: the game's camera shake and hit jolts (angle offsets of up to ~30 degrees), its
  head-bob, and its WiiView edge-pan (the view turns when the cursor reaches the screen edge -
  which the gun's cursor does whenever it points off-screen) are switched off in VR; the game's
  own first-person arms-and-gun reload animation (a second gun in front of your face) is not
  drawn. Each has a setting to bring it back.
- In VR modes the game's Direct3D 9 device is created as Direct3D 9Ex so the eye images, HUD and
  flat screen can be shared with a Direct3D 11 device that submits them to SteamVR (eyes) and
  shows them as SteamVR overlays (HUD panel, flat screen).

## Install (your Steam copy)

You need the zip from `tools\package.ps1`, unzipped (or, from the repo after `build.cmd`, run
`tools\install_game.ps1` the same way: it takes the DLLs from `out\`).

1. Close the game. Find its folder: Steam > Library > right-click *The Typing of the Dead:
   Overkill* > Manage > Browse local files (the folder with `HOTD_NG.exe`).
2. In PowerShell, from the unzipped folder:

   ```
   powershell -ExecutionPolicy Bypass -File .\install_game.ps1 -Game "<the game folder>"
   ```

   It checks that `HOTD_NG.exe` is the supported build (and installs nothing if not), refuses to
   overwrite an `XINPUT1_3.dll` / `openvr_api.dll` that another mod put there, copies the two DLLs,
   and writes `hovr\hovr.ini` with `mode=openvr` (an existing `hovr.ini` is kept). It does not touch
   your save.
3. Play: start SteamVR (controllers on), then the game from Steam as usual. Hold **Shift** while
   the game starts to play flat for that run; `mode=off` in `hovr\hovr.ini` turns VR off for good.
   The log is `hovr\hovr.log` (send it with any report).

Uninstall: `install_game.ps1 -Game "<the game folder>" -Uninstall` removes exactly the files it
installed (checked against the hashes in `hovr\installed.txt`; `XINPUT1_3.dll` also by the mod's
marker) and keeps `hovr\` (settings and log); add `-Purge` to remove `hovr\` too. Deleting
`XINPUT1_3.dll`, `openvr_api.dll` and `hovr\` by hand does the same.

**Your save**: the release install plays with the game's own save in
`%APPDATA%\sega\typing of the dead overkill` (Steam Auto-Cloud keeps syncing it). Nothing in the
mod writes to it; the game does, as it always does when you play.

## Build, package, development

- `build.cmd` (MSVC Build Tools x86, CMake, `C:\ninja.exe`) -> `out\XINPUT1_3.dll` and
  `out\openvr_api.dll`. Unit tests: `ctest --test-dir out` inside a vcvars32 shell.
- `tools\package.ps1 [-NoBuild] [-Version V]` -> `out\dist\hotd-overkill-vr-<version>.zip` (the
  DLLs, `install_game.ps1`, `hovr.default.ini`, this README, `LICENSES.txt`; no game files).
- **Development vs release** (`src/vr/harness.h`): a game folder whose `hovr\` holds `probe.ini`
  (or `hovr.ini` `[vr] dev=1`) runs the development harness: every user folder the game asks for
  is redirected into `hovr\AppData` etc. (the real save is never touched; the mod exits if the
  redirect fails), the window is minimized with faked activation and muted, player 1's pad is
  scripted, `hovr\cmd.txt` is polled for commands, assets/shader compiles are logged, and the log
  is `hovr\probe.log`. Without it (a release install) none of that happens: the game's own save
  folder, a normal window, sound on, the real pad, no command file, log `hovr\hovr.log`. The
  development log's second line (`harness (dry run of a release install)`) says what a release
  install would do with the same settings.
- The test copy (`C:\Users\Virtual\hotd-vr-game`) has `hovr\probe.ini`: `tools\run.ps1 -Mode
  off|sim|openvr` copies the DLL there and starts it minimized and muted with the save redirected
  (`-Stop` ends that process only). For a headset session in the test copy set `minimized=0` and
  `mute=0` in `hovr\probe.ini` (or start with `-Normal`); the save redirect stays on there.

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
| `aim` | `1` | the controller aims the shots (`0`: the mouse aims, as in milestone 1) |
| `hand` | `right` | the controller that is the gun (`left` for left-handed play) |
| `hand_pitch` | `0` | degrees the aim is tilted up from the controller's pointing axis. The pointing axis is the controller model's "tip" as SteamVR reports it (the direction SteamVR's own laser pointers use); tune this if shots land above/below where the barrel points |
| `hand_offset` | `0,0,0` | where the shot starts, in the controller's frame, metres (x right, y up, z back) |
| `haptics` | `1.0` | shot pulse strength (`0` = off); shotguns and the hand cannon pulse longer |
| `marker` | `0` | `1`: a small red dot in the eyes where the shot will land (yellow: the ray hit nothing within range, dot at 10 m). No laser |
| `reload_gesture` | `0` | `1`: pulling the trigger while the gun points more than `reload_gesture_deg` (60) degrees away from where you look reloads, like a light gun pointed off-screen |
| `btn_reload`, `btn_switch`, `btn_grenade`, `btn_pause`, `btn_recentre` | `gun.a`, `gun.b`, `gun.grip`, `off.b,off.a`, `gun.stick` | controller buttons for each action: `gun.` (the gun hand) or `off.` (the other hand) + `a` (A/X), `b` (B/Y: SteamVR's application-menu button), `grip`, `stick` (stick click), `trigger`; a comma means either. Recentre fires after holding for a second |
| `game_fire`, `game_reload`, `game_switch`, `game_grenade`, `game_pause` | `lmb`, `mmb`, `TAB`, `rmb`, `ESCAPE` | what each action sends to the game: `lmb`, `rmb`, `mmb`, `wheelup`, `wheeldown`, or a key name (`RETURN`, `SPACE`, `TAB`, `A`-`Z`, `0`-`9`, `F1`..., `0xNN`). The defaults are the game's own PC controls (Game Settings > Controls: left click fire, middle click reload, right click grenade, wheel or TAB swap weapon, Esc pause) |
| `gun_model` | `1` | draw the current weapon's model in the gun hand (`0`: no model, aim as in milestone 2) |
| `gun_grip` | `0,-0.03,0.05` | where the model's grip sits in the controller's frame, metres (x right, y up, z back; relative to the pointing pose SteamVR reports) |
| `gun_scale` | `1.0` | model scale (1 = the game's own size) |
| `aim_barrel` | `1` | shots start at the model's muzzle and fly along its barrel (`0`: from the controller) |
| `clip_anim` | `1` | the magazine drops out and back in during a reload |
| `clip_drop` | `0.2` | how far the magazine drops, metres |
| `gun_light` | `1.0` | brightness of the gun's lighting |
| `gun_face_sign` | `1` | `-1` if the gun looks lit from inside (flips which side of its triangles faces out) |
| `game_arms` | `0` | `1`: the game's own first-person arms and weapon during reloads (Varla and Washington both have them; in VR a second gun in front of your face). `0` hides them |
| `camera_shake` | `0` | `1`: the game's camera shake and hit jolts (turns of up to ~30 degrees when a zombie hits you) |
| `head_bob` | `0` | `1`: the game's head-bob while the rail moves (none was seen in the levels checked) |
| `recentre_resume` | `1` | recentre whenever gameplay resumes after a flat screen (pause, death screen and continue, cutscenes, the next chapter), as at a level start |
| `keep_active` | `0` | `1`: the game keeps running when its window loses the focus (try it if the game pauses or stops reacting while you are in the headset) |
| `dev` | `0` | `1`: the development harness (as with `hovr\probe.ini`): save redirected into `hovr\`, command file, minimized and muted window. Not for playing |
| `show_<weapon>` | see below | `0` hides that weapon's model (aim and everything else still work) |
| `clip_<weapon>` | see below | magazine: `0` hidden, `1` drops on reload, `2` always drawn, no animation |
| `hold_<weapon>` | `0,0` | yaw, pitch (degrees) the model is turned in the hand about its grip; the aim turns with the barrel |
| `grip_<weapon>`, `muzzle_<weapon>`, `barrel_<weapon>` | measured | model-space grip point, muzzle point (cm) and barrel direction; defaults measured from the meshes |
| `sim_hand` | `0.15,-0.25,-0.35` | sim only: gun hand position relative to the head, metres |
| `sim_hand_yaw`, `sim_hand_pitch` | `0` | sim only: gun hand direction relative to the head, degrees |

`<weapon>` is one of `magnum`, `gold_magnum`, `handcannon`, `pump`, `autoshotgun`, `smg`,
`rifle`, `minigun`, `crossbow`. Defaults: all models shown; magazines drop for the magnum, auto
shotgun, SMG and rifle, are hidden for the hand cannon, pump and crossbow (their "clip" meshes need
the game's arm skeleton to be placed) and drawn fixed for the minigun (its barrel cluster). The gold
magnum has no magazine.

### Controls (defaults)

| controller | game |
|---|---|
| gun trigger | fire (hold for automatic weapons); in menus: click where the gun points |
| gun A (A/X) | reload |
| gun B (B/Y) | switch weapon |
| gun grip | grenade |
| other hand's B or A (Y/X) | pause menu (Escape) |
| gun stick click, held 1 s | recentre the view |
| point at the flat screen | moves the menu cursor |

## Commands (development)

Development harness only (see above). Write lines to `hovr\cmd.txt` (`tools\cmd.ps1 'cmd' ...`; `tools\step.ps1 NAME 'cmd' ...` adds a
screenshot). The full list is at the top of `src/mod/control.cpp`. VR ones:

- `vr recentre` — recentre the view (also happens automatically when a level starts and when
  gameplay resumes after a flat screen).
- `vr state` — log the game state (level / cutscene / paused) and whether the frame is stereo.
- `vr handoff NAME` — save what the Direct3D 11 side holds: `NAME_eyes.png` + `NAME_hud.png`
  (gameplay) or `NAME_screen.png` (flat), alpha in `_a.png`.
- `vr crosshair 0|1`, `vr yaw D`, `vr pitch D`, `vr ipd M`, `vr unit M`, `vr cone D`,
  `vr mode off|sim` (stereo on/off live), `vr scene NAME`, `vr targets`, `vr check`.
- Sim gun: `vr hand YAW PITCH`, `vr handpos X Y Z` (relative to the head), `vr handat X Y` (point
  the hand at what the camera sees through cursor X, Y), `vr handscreen X Y` (point it at back-buffer
  pixel X, Y of the flat screen), `vr handseek` (point it at the zombie nearest the view's centre),
  `vr hand off`; sim buttons `vr btn trigger|a|b|grip|stick|off.b [MS]`,
  `vr hold NAME`, `vr release NAME|all`. `vr gun` logs the weapon, ammo and the aim; `vr marker`,
  `vr aim`, `vr gesture` switch those settings live; `vr god 1` keeps player 1 alive for tests;
  `vr actions 1` logs the game's input actions as they change.
- Gun model: `vr gunmodel [0|1]` (log, switch off/on), `vr gunview aim|back|right|left|front|top|bottom`
  (sim head/hand presets for looking at the gun), `vr barrelline 1` (a red line from the muzzle to
  the aim point), `vr clipout F` (hold the magazine F of the way out; no F = from the reload),
  `vr gunset WEAPON show|clip|grip|barrel|muzzle|clipdir|hold V...` (live tuning).
  `tools/gun_mesh.py PACK.pc --png DIR` measures the meshes in an installed hud pack.
- Comfort: `vr arms|shake|bob 0|1` (switch `game_arms` / `camera_shake` / `head_bob` live),
  `vr comfort` (arms draws hidden, largest shake offsets, bob amplitudes, edge-pan state),
  `vr wiiview 1` (let the game's edge-pan through, to compare), `vr camlog N` (camera position and
  forward axis for N frames), `vr shakelog 1`, `vr actors 1` (every actor the entity draw sees),
  `vr waithit [MS]` (the command queue waits until player 1 is hit), `vr measure` (world-scale ray
  casts: floor, ceiling, walls, zombie heights). `tools\scripts\to_naked_terror_vr.txt` reaches
  Naked Terror (Varla) at a 1920x1200 back buffer.

## Headset playtest checklist (milestones 1-4)

One session covers everything. Have the desktop window in view or a second person at the keyboard;
take notes of the setting values that feel right.

**Before**: install into the Steam copy (above), or use the test copy with `tools\run.ps1 -Mode
openvr -Normal` (`minimized=0`, `mute=0` in its `probe.ini`). Start SteamVR with both controllers
on, then the game. If anything goes wrong, close the game and start it again holding **Shift**
(flat, no hooks), and keep `hovr\hovr.log` (`hovr\probe.log` in the test copy).

1. **Start-up**: the log shows `harness (this run): release; hooks for VR; save the game's own
   folder ...` (test copy: `development ... redirected`), then `vr: OpenVR up, recommended WxH per
   eye`, and `vr: gun controller model '...': tip at ...` once the controllers are seen (Steam
   Frame controllers are expected to report as `oculus_quest2`, tip ~40 degrees below). SteamVR
   shows the game as the running app.
2. **Menus on the flat screen** (~3 m ahead, sharp): point the right controller at it - the menu
   cursor follows exactly; the trigger clicks. Go House of the Dead > Story > a level > Start
   without the mouse. Screen size/distance comfortable? (`screen_width`, `screen_distance`,
   `screen_drop`)
3. **Cutscenes** on the flat screen, subtitles readable; Y/X (pause button) skips.
4. **Gameplay, stereo and scale** (Papa's Palace kitchen): the room life-sized (camera ~1.6 m above
   the floor, ceiling ~2.8 m, zombies about human height); head tracking smooth at the headset's
   rate; leaning gives correct parallax; both eyes line up. Turning the head up to ~35 degrees
   shows no objects popping out at the edges (`cone_deg`). Report SteamVR's frame timing and the
   log's `fps` lines (`eye_scale=0.8` if it can't keep up).
5. **HUD panel**: ~1.5 m ahead, world-locked, readable, not doubled; no game crosshair.
   (`hud_width`, `hud_distance`, `hud_drop`)
6. **The gun in hand**: the magnum where the controller is, life-sized, grip in the palm
   (`gun_grip`, metres, z back, y up; `gun_scale`). Look at it from all sides: anything missing or
   inside-out? Lighting plausible (`gun_light`, `gun_face_sign=-1`)?
7. **Aim (the important one)**: with `marker=1` the red dot sits on the barrel's line at any
   distance, and shots land on it - near and far zombies, gun held off to the side of the head.
   Without the marker: do shots land where the barrel points? If consistently above/below, tune
   `hand_pitch` (degrees, + raises the aim) or `hold_<weapon> = 0,PITCH`; left/right offsets are a
   bug - report them.
8. **Buttons and haptics**: trigger fires (hold for the SMG), A reloads, B switches weapon (the
   model changes with the HUD), grip throws a grenade, Y/X pauses; a buzz per shot (`haptics`).
   Holding the right stick click for a second recentres the view.
9. **Reload (milestone 4)**: with the magnum and the SMG, the magazine drops out of the grip and
   slides back (`clip_drop`, `clip_anim`), and **no second pair of arms/gun rises in front of
   your face**. To compare, set `game_arms=1` once: the game's own arms and gun then appear
   head-locked during the reload (Washington with the magnum and SMG in Papa's Palace, Varla in
   Naked Terror).
10. **Getting hit (milestone 4)**: let a zombie hit you. The view must not jolt or swing (blood on
    the screen and the HUD's health change are expected). To compare, `camera_shake=1` once: each
    hit then turns the view by up to ~30 degrees. Nausea either way? Report.
11. **Screen edges (milestone 4)**: point the gun far off to the side (past the edge of where you
    look) during a fight: the view must **not** start turning (the game's WiiView edge-pan is off;
    in sim it turned the camera ~25 degrees in 1.5 s when allowed).
12. **Recentre (milestone 4)**: turn your body ~45 degrees, pause (Y/X) and resume: gameplay comes
    back facing where you now look (log: `vr: recentre (gameplay resumed after a flat screen)`).
    Same after dying and choosing YES (continue), after a cutscene, at the next chapter, and at a
    level start (`vr: recentre (level start)`). If the snapping is unwelcome, `recentre_resume=0`.
13. **Moving between rooms**: the rail camera carries you smoothly; no bobbing (`head_bob=1`
    brings the game's back, none was seen in sim).
14. **Pause and quit**: the pause menu on the flat screen; Quit goes back to the menus; quitting
    the game leaves SteamVR running normally.
15. **Shift at start**: start the game holding Shift: it plays flat on the monitor, the log says
    `vr: Shift held at start: VR off for this run` and `hooks none`.
16. **Your save**: progress made in the headset session shows up in the next flat session (the
    release install uses the game's own save).

Report: the log, the setting values you changed (`hand_pitch`, `gun_grip`, panel sizes,
`eye_scale`), anything that fired when it shouldn't, any weapon that looks broken
(`show_<weapon>=0`), and whether the comfort defaults (no shake, no edge-pan, no game arms,
recentre on resume) feel right.

## Known issues and untested

- **Nothing has run in a headset yet**: the OpenVR path (submit, overlays, controller poses and
  buttons, haptics) is compiled and reviewed only; every check above was done in sim mode.
- Release mode was checked by code review, a unit test of the decision (`test_harness_plan`) and
  the development log's dry-run line - not by a release run in the test copy, because that would
  play with the real save.
- Window focus: in release the game's window is left alone. If the game pauses or ignores the
  controllers while the desktop window is not in front, set `keep_active=1`.
- With the game's reload animation hidden (`game_arms=0`) the only reload cue in your hand is the
  magazine drop; the hand cannon, pump and crossbow have no magazine animation (their "clip"
  meshes need the arm skeleton).
- The death screen's red backdrop is the game's own (flat screen); no zoom or blur was seen in
  stereo frames, so there is no `hit_zoom` setting.
- No head-bob was seen in this build (the camera kept a constant height over 15 m of rail);
  `head_bob=0` only holds the game's bob amplitudes at zero as a safety net.
- Looking more than `cone_deg` away from the rail camera's direction can show objects culled by the
  game.
- Only Steam build 569304 of `HOTD_NG.exe` is supported; any other build leaves the mod a plain
  XInput pass-through (the installer refuses it).
