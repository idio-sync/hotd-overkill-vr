# House of the Dead: Overkill VR

A VR mod for the **House of the Dead: Overkill** mode inside *The Typing of the Dead: Overkill*
(Steam app 246580, build 569304). The game is shown in stereo with head tracking through SteamVR;
its HUD floats on a panel in front of you; menus, the loadout screen, cutscenes, the pause menu and
the death screen appear on a large flat virtual screen. Design: `docs/superpowers/specs/2026-11-17-hotd-overkill-vr-design.md`.

Status: **milestone 2** (the controller is the gun: it aims every shot, its buttons fire, reload,
switch weapons and pause, it points at the flat screen in menus, and it pulses on each shot). The
gun model in your hand is milestone 3. Milestones 1 and 2 have been checked in sim mode only; the
headset checklists below are for the first headset session.

## How it works (short)

- `XINPUT1_3.dll` is a stand-in XInput DLL the game loads from its own folder. It forwards XInput
  to Windows and hooks Direct3D 9. Nothing in the game's files is changed; delete the files to
  uninstall. It only hooks the known build of `HOTD_NG.exe` (others: plain XInput pass-through).
- Each 3D scene draw is issued twice, once per eye, into double-wide render targets. The game's
  60 Hz frame limiter is lifted during gameplay; SteamVR's pose wait paces the game at the
  headset's rate.
- The gun (right controller by default) aims the game's own shots: every shot path of player 1
  (the bullet ray cast, the projectile weapon, the pickup/target checks) is fed the controller's
  position and direction instead of the camera's ray through the mouse cursor, so shots go where
  the gun points, from where the gun is. The game's cursor is moved to where the gun's ray meets
  the scene, so the game's own aim logic agrees. The controller's buttons become the game's
  mouse/keyboard input; the real mouse is ignored while the controllers are in use.
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
| `aim` | `1` | the controller aims the shots (`0`: the mouse aims, as in milestone 1) |
| `hand` | `right` | the controller that is the gun (`left` for left-handed play) |
| `hand_pitch` | `0` | degrees the aim is tilted up from the controller's pointing axis. The pointing axis is the controller model's "tip" as SteamVR reports it (the direction SteamVR's own laser pointers use); tune this if shots land above/below where the barrel points |
| `hand_offset` | `0,0,0` | where the shot starts, in the controller's frame, metres (x right, y up, z back) |
| `haptics` | `1.0` | shot pulse strength (`0` = off); shotguns and the hand cannon pulse longer |
| `marker` | `0` | `1`: a small red dot in the eyes where the shot will land (yellow: the ray hit nothing within range, dot at 10 m). No laser |
| `reload_gesture` | `0` | `1`: pulling the trigger while the gun points more than `reload_gesture_deg` (60) degrees away from where you look reloads, like a light gun pointed off-screen |
| `btn_reload`, `btn_switch`, `btn_pause`, `btn_recentre` | `gun.a,gun.grip`, `gun.b`, `off.b,off.a`, `gun.stick` | controller buttons for each action: `gun.` (the gun hand) or `off.` (the other hand) + `a` (A/X), `b` (B/Y: SteamVR's application-menu button), `grip`, `stick` (stick click), `trigger`; a comma means either. Recentre fires after holding for a second |
| `game_fire`, `game_reload`, `game_switch`, `game_pause` | `lmb`, `rmb`, `wheeldown`, `ESCAPE` | what each action sends to the game: `lmb`, `rmb`, `mmb`, `wheelup`, `wheeldown`, or a key name (`RETURN`, `SPACE`, `A`-`Z`, `0`-`9`, `F1`..., `0xNN`) |
| `sim_hand` | `0.15,-0.25,-0.35` | sim only: gun hand position relative to the head, metres |
| `sim_hand_yaw`, `sim_hand_pitch` | `0` | sim only: gun hand direction relative to the head, degrees |

### Controls (defaults)

| controller | game |
|---|---|
| gun trigger | fire (hold for automatic weapons); in menus: click where the gun points |
| gun A (A/X), gun grip | reload |
| gun B (B/Y) | switch weapon |
| other hand's B or A (Y/X) | pause menu (Escape) |
| gun stick click, held 1 s | recentre the view |
| point at the flat screen | moves the menu cursor |

## Commands (development)

Write lines to `hovr\cmd.txt` (`tools\cmd.ps1 'cmd' ...`; `tools\step.ps1 NAME 'cmd' ...` adds a
screenshot). The full list is at the top of `src/mod/control.cpp`. VR ones:

- `vr recentre` — recentre the view (also happens automatically when a level starts).
- `vr state` — log the game state (level / cutscene / paused) and whether the frame is stereo.
- `vr handoff NAME` — save what the Direct3D 11 side holds: `NAME_eyes.png` + `NAME_hud.png`
  (gameplay) or `NAME_screen.png` (flat), alpha in `_a.png`.
- `vr crosshair 0|1`, `vr yaw D`, `vr pitch D`, `vr ipd M`, `vr unit M`, `vr cone D`,
  `vr mode off|sim` (stereo on/off live), `vr scene NAME`, `vr targets`, `vr check`.
- Sim gun: `vr hand YAW PITCH`, `vr handpos X Y Z` (relative to the head), `vr handat X Y` (point
  the hand at what the camera sees through cursor X, Y), `vr handscreen X Y` (point it at back-buffer
  pixel X, Y of the flat screen), `vr hand off`; sim buttons `vr btn trigger|a|b|grip|stick|off.b [MS]`,
  `vr hold NAME`, `vr release NAME|all`. `vr gun` logs the weapon, ammo and the aim; `vr marker`,
  `vr aim`, `vr gesture` switch those settings live; `vr god 1` keeps player 1 alive for tests;
  `vr actions 1` logs the game's input actions as they change.

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
8. **Recentre**: the view recentres when a level starts, and when the gun's stick click is held
   for a second (milestone 2).
9. **Quit** from the pause menu: back to the menu on the flat screen; quitting the game leaves
   SteamVR running normally.

## Headset playtest checklist (milestone 2: the gun)

Same start-up as above. In SteamVR the controllers must be on before the game starts (or at
least before the first menu). The log (`hovr\probe.log`) names the gun controller and its model:
`vr: gun controller model '...': tip at ... pointing N degrees below the raw axis` (Steam Frame
controllers are expected to report as `oculus_quest2` with the tip about 40 degrees below).

1. **Menus by pointing**: on the main menu, point the right controller at the flat screen: the
   game's menu cursor follows the controller exactly where it points; pulling the trigger clicks.
   Go Main menu -> House of the Dead -> Story -> a level -> Start without touching the mouse.
   The real mouse should no longer move the cursor.
2. **Aim accuracy (the important one)**: in the first fight, set `marker=1` for this check (or
   `vr marker 1`). Hold the controller like a gun and point the barrel at a zombie's head. Does the
   red dot sit where the barrel points? Shoot: do hits land on the dot? Check near and far
   zombies, and with the controller held off to the side of your head (shots come from the
   hand, not your eyes, so a zombie half hidden behind a door frame may be hit from one hand
   position and not another - that is intended). If shots land consistently above or below the
   barrel's line, change `hand_pitch` (degrees, + raises the aim) in `hovr\hovr.ini` and restart;
   note the value that feels right. If they land consistently left/right, report it (that would be
   a bug, not a setting).
3. **Without the marker**: `marker=0` (the default). Can you aim by the gun alone? (Milestone 3
   adds the gun model; until then there is nothing drawn at your hand.)
4. **Buttons**: trigger fires (hold it with an automatic weapon if you have one); A or grip reloads
   (the HUD's rounds refill); B switches weapon (only with two weapons equipped in the loadout);
   Y/X opens the pause menu, and in the pause menu pointing + trigger works. Holding the right stick
   click for a second recentres the view.
5. **Haptics**: a short buzz on every shot, longer for shotguns and the hand cannon. Too strong or
   weak? (`haptics=0.5` / `2`; `0` turns it off.)
6. **Death screen**: point at YES and pull the trigger.
7. Optional: `reload_gesture=1`, then point the gun well away from where you look (e.g. at the
   floor beside you) and pull the trigger: it reloads instead of firing.

Report: the tip line from the log, the `hand_pitch` that felt right, whether the marker sat on the
barrel's line, and anything that fired when it shouldn't (or didn't when it should).

Things to note for the next milestone: whether the HUD panel size/distance/height is right
(`hud_width`, `hud_distance`, `hud_drop`), whether the flat screen is too big or too close, the
world scale (`unit_m`), and whether head turns beyond the cone show missing objects.
