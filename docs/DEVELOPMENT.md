# Development

Everything for building, testing and releasing House of the Dead: Overkill VR. Players: see the
[README](../README.md). Settings reference: [SETTINGS.md](SETTINGS.md). Reverse-engineering notes:
[findings-probe.md](findings-probe.md) (observed in the running game) and
[findings-static.md](findings-static.md) (static analysis of `HOTD_NG.exe`).

Status: milestone 4 (comfort and release) is done. Milestones 1-4 have been checked in sim mode and
against the fake SteamVR runtime only, never in a headset; the [playtest checklist](#headset-playtest-checklist-milestones-1-4)
below covers all of them for the first headset session.

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

## Design notes

The aim: the original on-rails game seen in stereo with head tracking, with a tracked gun that
shoots exactly where it points (the Time Crisis VR mods are the model). There is no decompilation:
it is a hook mod on the shipped 32-bit Direct3D 9 executable, built like the Killer7 VR mod
(github idio-sync/k7-vr) and reusing its OpenVR plumbing, head math, sim mode and harness.

Milestones (code comments refer to them):

1. **Stereo + head tracking** in sim and OpenVR: D3D9Ex, double-wide targets, per-draw stereo,
   culling, uncapped frame rate, HUD panel, flat screen for menus/cutscenes.
2. **Controller gun**: aim override on every shot path, buttons, haptics, menu pointing.
3. **The weapon model**: find and draw `GunArms_<Weapon>`, weapon switching, magazine reload.
4. **Comfort and release**: shake/head-bob/edge-pan off, recentre, world-scale check, install,
   README with a playtest checklist.

Decisions and why:

- **Stereo**: every 3D scene draw issued twice into a side-by-side target, each with its eye's
  matrix and half of the viewport (same-frame stereo on plain D3D9, no shader patching). Rejected:
  the engine's dormant stereoscopy path (`[0x9F09E0]+0x3F8`; unproven, kept as a fallback) and
  alternate-frame eyes (the eyes would show different moments).
- **Shots**: the origin and direction are overridden inside the game's own shot paths, so the
  game's ray casts decide hits. Rejected: moving the 2D cursor to where the controller's ray meets
  the scene (Killer7's method), which needs depth readback and fires from the wrong place.
- **Weapon model**: the resident mesh's GPU buffers and textures, drawn by the mod. Rejected:
  having the engine draw its own instance (needs its camera/light state per eye) and extracting
  meshes offline (game assets may not be redistributed). Details in findings-probe.md.
- **Look-around**: a cone around the rail camera's direction (`cone_deg`, ±35°): the game camera's
  field of view is widened before culling and the per-eye projection undoes it.
- **Aim quality is the bar**: "close" counts as failure; the barrel is checked against the hit, not
  mod-internal proxies (a lesson from Killer7). No laser by default.
- Never write to the real save or the Steam folder during development.

Not in v1, possible later: hands and arms (the game has per-weapon reload animations on an arm
skeleton), free look-around without the cone (an A/B test where the game camera turns with the
head before culling), the typing modes, co-op, OpenXR, other game builds.

## Building

- `build.cmd` (MSVC x86 found through vswhere, CMake, Ninja: `C:\ninja.exe` if present, else
  `ninja` on PATH) -> `out\XINPUT1_3.dll` and `out\openvr_api.dll` (Valve's), plus the test
  runtime `out\fakevr\openvr_api.dll` (below; never packaged). Unit tests: `build.cmd test` (or
  `ctest --test-dir out`).
- `tools\package.ps1 [-NoBuild] [-Version V]` -> `out\dist\hotd-overkill-vr-<version>.zip` (to
  extract into the game folder: the two DLLs, the README, `LICENSE`, `LICENSES.txt`, all at the
  top level; no `hovr.ini`, no installer script, no game files), plus `-pdb.zip` (the mod's PDB)
  and `.sha256`. `tools\check_package.ps1 <zip>` checks the zip's file list.
- The default settings are `tools\hovr.default.ini`, the only copy: CMake builds it into the DLL
  (`out\generated\hovr_default_ini.h`), which writes it at a release install's first start
  (`src/vr/settings_file.h`, unit tested).

## The exe check and its anchor table

The mod patches and reads fixed addresses of Steam build 569304, so at every start, before any
hook, `src/mod/buildcheck.cpp` reads `HOTD_NG.exe` from disk and `src/vr/exe_anchors.cpp` judges it:

- the whole file is that build (size + FNV-1a 64): **known build**;
- the file differs but every **anchor** is byte-for-byte as in that build: **a patched copy,
  compatible**. VR is on, and a warning line names the regions that differ (`PE headers`, a
  section such as `.text (0x401000)`, `signature/overlay`), from per-region hashes of the known exe;
- any anchor differs, or the exe isn't a PE with image base 0x400000: **refused**. A release
  install stays a plain XInput pass-through (development keeps only the harness's own import
  hooks, so a test run never touches the real save: `src/vr/harness.h`), and the log has one line
  per failing anchor: name, kind, VA, expected bytes, found bytes.

The log always has the exe's size, FNV-1a 64, SHA-256, PE timestamp and Large Address Aware flag.
There is no setting to force VR on.

An anchor is a place in the exe the mod depends on, compared in the file. The file's bytes are
not relocated, so absolute addresses in instructions are the image-base 0x400000 ones; nothing
SecuROM fills or rewrites at runtime (such as its jump table at 0xA71000..0xA71844) is used.

- `detour`: the prologue of each function the mod detours (the same bytes the detour itself
  checks at install time, which stays as a second check);
- `call`: the first instructions of each game function the mod calls;
- `retaddr`: the calls whose return addresses the mod compares (the weapon's fire calling
  FireBullets, 0x52B164; FireBullets calling the world ray cast, 0x50D27E, for the shot log);
- `global`: for each fixed global the mod reads or writes (0x9F09E0, 0x99B708, 0x99B8F4,
  0x99B770, 0x99B71C, 0x9C7DC0, 0x9C7E48, 0x9C7E88, 0x9C7F20..0x9C7F34; 0x9B01A8 is inside the
  shake detour's prologue), an instruction in the game's code that references it (in a function
  the mod hooks where possible, and with the field offset the mod uses), so a moved global fails;
- `vtable`: the constructor's store of each vtable the mod compares objects with, and the
  vtable's own first bytes.

The list is `SPEC` in `tools/anchors.py`; the table, `src/vr/exe_anchors_table.h`, is generated
from the known exe and is not edited by hand. When the mod starts using another address: add it
to `SPEC` (`tools\anchors.py --find VA` lists the instructions that reference a global), run
`.venv\Scripts\python.exe tools\anchors.py --write` (it refuses any exe but the known one; the
default is the test copy's, or `--exe PATH`), rebuild and `build.cmd test`. The `anchors` test
checks the table's consistency (unique names, lengths, no overlaps, each global referenced, nothing
in SecuROM's table) and the check itself on a synthetic exe; where the real exe exists
(`HOTD_EXE`, else the test copy) also the table against it, a 4 GB patch, a changed padding byte
and every anchor changed in turn. CI has no game, so that part is skipped there.

`tools\anchors.py --check EXE` (standard library only) gives the same verdict for any exe, e.g.
one a user sends; `tools\install_game.ps1` uses it when the exe isn't the exact known file.

## Installing: details

The release zip is laid out for the game folder: `XINPUT1_3.dll` (the mod), `openvr_api.dll`
(Valve's OpenVR API), `README.md`, `LICENSE` and `LICENSES.txt`, all at the top level.

- **First start**: the mod checks that `HOTD_NG.exe` is the supported build or a compatible
  patched copy of it (otherwise it does nothing at all, a plain XInput pass-through), creates `hovr\` and, when `hovr\hovr.ini` is
  missing, writes the default settings there (`mode=openvr`, with comments; the same text as
  `tools\hovr.default.ini`). It never replaces an existing `hovr.ini`. If the folder can't be
  written, the mod uses the same defaults for that run.
- **Play**: `mode=off` in `hovr\hovr.ini` turns VR off for good; Shift at start for one run. The
  log is `hovr\hovr.log`.
- **Your save**: a release install plays with the game's own save in
  `%APPDATA%\sega\typing of the dead overkill` (Steam Auto-Cloud keeps syncing it). Nothing in the
  mod writes to it; the game does, as it always does when you play.

From the repo after `build.cmd`, `tools\install_game.ps1 -Game "<game folder>" [-Uninstall
[-Purge]]` (a development tool, not in the release zip) copies the DLLs from `out\` the same way,
checks the build and refuses to overwrite another mod's DLLs first, and keeps a hash record so
`-Uninstall` removes only what it installed.

## Development vs release, the test copy

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
  (`-Stop` ends that process only; `-NoStart` only installs). `-OpenVR fake|real` installs the
  fake runtime or Valve's `openvr_api.dll` there (kept until changed); `-Mode openvr` refuses to
  start with Valve's DLL unless `-OpenVR real` is in the same call, so a test can never start
  SteamVR. For a headset session in the test copy: `tools\run.ps1 -Mode openvr -OpenVR real
  -Normal` (and `mute=0` in `hovr\probe.ini`); the save redirect stays on there.
- Sim mode (`mode=sim`): no headset; the desktop window shows both eyes side by side with the HUD
  panel, and the head and gun hand are driven by commands (`vr yaw`, `vr hand`, ...). Scripts in
  `tools\scripts\` reach gameplay from boot: `to_gameplay.txt` (mouse) and `to_gameplay_vr.txt`
  (sim gun hand) to Papa's Palace, `to_loadout.txt` to the loadout screen,
  `to_naked_terror_vr.txt` to Naked Terror (Varla) at a 1920x1200 back buffer. Run one with
  `tools\cmd.ps1 -File tools\scripts\<name>` right after `run.ps1`; boot time varies.

## Commands (development)

Development harness only. Write lines to `hovr\cmd.txt` (`tools\cmd.ps1 'cmd' ...`;
`tools\step.ps1 NAME 'cmd' ...` adds a screenshot). The full list is at the top of
`src/mod/control.cpp`. VR ones:

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
  `vr aim`, `vr gesture` switch those settings live; `vr god 1` keeps player 1 alive for tests
  (idle gameplay gets the player killed quickly); `vr actions 1` logs the game's input actions as
  they change.
- Gun model: `vr gunmodel [0|1]` (log, switch off/on), `vr gunview aim|back|right|left|front|top|bottom`
  (sim head/hand presets for looking at the gun), `vr barrelline 1` (a red line from the muzzle to
  the aim point), `vr clipout F` (hold the magazine F of the way out; no F = from the reload),
  `vr gunset WEAPON show|clip|grip|barrel|muzzle|clipdir|hold V...` (live tuning).
  `tools/gun_mesh.py PACK.pc --png DIR` measures the meshes in an installed hud pack.
- Moving parts (gun feedback stage 1;
  `src/vr/gunparts.h` has the per-weapon boxes, pivots, travels and timings, `gunmodel.cpp` the
  read-back, the part stream and the animation state): the log has one `gun parts: <weapon> gun|clip`
  line per mesh resolved (pieces found per rule; a rule that doesn't match leaves that mesh still) and
  `gun anim: shot N of <weapon>` for each weapon's first 40 shots (with the time since the one
  before: the game's re-fire interval). `vr gunparts 0|1` switches the motion live (setting
  `gun_parts`). `vr gunanim WEAPON T` holds that weapon's parts as they are T ms after a shot (T < 0:
  just before it, the hand cannon's hammer cocked; `any` for the current weapon), with optional
  fields `lock 0|1` (slide lock), `out F` (a reload's magazine / cylinder 0..1 of the way out),
  `reloadend MS` (time since a reload ended: the pump's rack), `trigger F`, `spin DEG` (minigun);
  `vr gunanim off` goes live again. `vr gunbackdrop 1` clears each eye to grey behind the gun, for
  captures of its parts (the HUD panel, screen blood and some sprites still draw over it). Recipe
  (fake runtime, gameplay, `vr god 1`): clear the zombies nearby first (their hits splash the
  screen), then `vr gunbackdrop 1` and per phase `vr gunanim ...` + `tools\fakevr_shot.ps1 NAME -Plain
  'head 0 1.65 0 0 0 0' 'ctl R 0 1.55 -0.30 90 0 0'` (the gun's right side; long guns 0.1-0.3 m
  further left), from behind `ctl R 0 1.60 -0.17 0 0 0`, from above `head 0 1.65 0 0 -89 0` with `ctl
  R -0.12 1.28 -0.02 90 0 0`, the minigun's muzzles `ctl R 0 1.72 -0.85 180 0 0`. The live analog
  trigger: `fakevr axis R 1 0.4 0`.
- Muzzle flash, recoil, haptic patterns (gun feedback stage 2; `src/vr/gunfeedback.h` has the
  per-weapon flash sizes, recoil and haptic patterns, the flash's two-frame state, its generated
  images and the pattern player; `gunmodel.cpp` draws the flash and kicks the drawn gun,
  `vrinput.cpp` plays the patterns, `gun.cpp` queues the shot pattern and the hit tick): `vr
  muzzleflash 0|1`, `vr recoil X`, `vr haptics X` switch the settings live. `vr gunanim WEAPON T`
  also holds the recoil at T ms after the shot; its `flash F` field shows the flash as in frame F
  after the shot (0 and 1; 2 = gone), e.g. `vr gunanim magnum 8 flash 1`. `vr gunbackdrop 1 R G B`
  clears to that colour (a dark one, `25 27 32`, shows the flash). `vr fxlog 1` logs `fx:` lines:
  each haptic pattern queued (`fx: haptic pistol, in 0 ms`), each pulse sent (`fx: pulse 3999 us
  (16.0 ms after the last pattern was queued; gun frame N)`) and each flash frame drawn (`fx: flash
  of smg shot 12, frame 0`); the fake runtime records the pulses it receives (`fakevr haptics`).
  Recoil and flash use the drawn (kicked) gun; the shot ray, `shotlog`'s drawn barrel, the hit
  marker and the cursor use the un-kicked one, so `tools\gun_accuracy.ps1` must stay at 0.000 deg.
- Gun accuracy test (measuring only):
  `vr shotlog 1` (off by default) logs `shotlog shot` per player-1 shot (the spread-free ray the mod
  set, the barrel line at fire time, the barrel as drawn in the last frame on screen with its frame
  number, hand pose and the rail camera's turn speed), `shotlog pellet` per ray cast FireBullets
  makes (the game's origin, direction after its spread, hit point, entity; `depth` 1+ = bullets
  carrying on through a hit) and `shotlog proj` per projectile launch (the origin the game wrote
  back after its offset, the direction; landing not logged); `vr shotlog tag NAME` marks a
  scenario. `vr gunmeshcheck` fits the current weapon's bore and muzzle from its live vertex buffer
  (`src/vr/gunfit.h`) and logs `shotlog mesh` against the built-in description (hovr.ini overrides
  as `mesh_override`). `.venv\Scripts\python -I tools\gun_accuracy.py hovr\probe.log` (the test
  copy's development log; `hovr.log` in a release install) writes the per-weapon, per-tag report:
  bias against the drawn and the fire-time barrel, the game's spread, bias by turn speed, the mesh
  check, the thresholds; `--selftest` checks it on a synthetic log; `--summary` puts one table
  first, per weapon and scenario: still (`still*` tags, rail and hand under 30 deg/s), turn (rail
  at 30+ deg/s, hand under 30), sweep60/sweep180 (`sweep*` tags, rail under 30). Each shot line also
  carries `hand_turn` (the hand's pointing axis in camera space) and `barrel_turn` (the drawn
  barrel in the world), deg/s between the last two frames drawn. The ray cast hook (0x5514E0,
  FireBullets' call at 0x50D279) is installed in the development harness only.
  The full run, `tools\gun_accuracy.ps1` (fake runtime, Naked Terror; phases so the loadout can be
  checked between them): `-Phase boot` starts the test copy, makes **every weapon available and
  bought in memory** (player 1's profile `[[0x99B748]+0x228]`: `+0x5C` available mask, `+0x60`
  purchased mask, bit = weapon id: 1 magnum, 2 hand cannon, 4 pump, 5 auto shotgun, 6 SMG, 8
  rifle, 9 and 10 crossbow and Gwendolyn, 11 minigun, as the game's own "Unlock Everything" script
  function sets them, 0x424450; with the existing `peek`/`poke`, nothing written to the save by the
  script) and goes to Naked Terror's loadout, captured to `hovr\gunacc_loadout_screen.png`;
  `-Phase loadout -Clicks 'R2,R2,L1'` clicks the loadout's arrows (slot 1 or 2, right or left; the
  list skips the other slot's weapon; slot 2 also has EMPTY) and captures again; `-Phase play
  -Weapons 'magnum,pump'` (the two slots in order) starts the level with `vr god 1` and the shot log,
  and per weapon fires `still_neutral`, `still_edge` (release_check's two poses), `still_seek` (held
  on a zombie), `sweep60`, `sweep180` (the controller swept ±25 degrees while firing), `vr
  gunmeshcheck`, then `turn` (firing on through the level, re-aimed at zombies every 6 s), B to the
  next weapon; `-Phase stop`. One boot per pair: five runs for the nine weapons. The game may write
  the test copy's save (`hovr\AppData\sega\typing of the dead overkill\fnhotd`) at a level's end:
  back it up first.
- Comfort: `vr arms|shake|bob 0|1` (switch `game_arms` / `camera_shake` / `head_bob` live),
  `vr comfort` (arms draws hidden, largest shake offsets, bob amplitudes, edge-pan state),
  `vr wiiview 1` (let the game's edge-pan through, to compare), `vr camlog N` (camera position and
  forward axis for N frames), `vr shakelog 1`, `vr actors 1` (every actor the entity draw sees),
  `vr waithit [MS]` (the command queue waits until player 1 is hit), `vr measure` (world-scale ray
  casts: floor, ceiling, walls, zombie heights).
- Rail comfort (vignette, smoothing, levelling; `src/vr/comfort.h`): `vr comfort on|off` (all
  three), `vr comfort vignette X`, `vr comfort turn A B` (deg/s), `vr comfort move A B` (m/s),
  `vr comfort ease MS`, `vr comfort smooth MS`, `vr comfort level 0|1|2`, each logged with the
  resulting settings; `vr comfort stats [S]` logs the rail camera's turn and move speed
  percentiles (smoothed over 100 ms, as the vignette sees them) over the last S seconds (30), the
  camera cuts, how often the vignette was on and the smoothing lag. A/B in the headset: play a
  stretch with `vr comfort off`, then `vr comfort on`, and compare; `stats` after a fast section
  shows where to put `turn`/`move`. `vr comfort wait [X] [MS]` holds the command queue until the
  vignette reaches X; on a timeout it skips to the next `vr comfort end` (captures during fast turns:
  `vr comfort wait 0.5 1200`, `fakevr capture NAME`, `vr comfort end`).
- Crashes: `crash now` writes to address 0x10, to check the crash handler (`src/mod/crash.cpp`:
  the faulting module and offset, registers, return addresses and address-space use in the log,
  `hovr\crash.dmp`). Every 5-second frame line also ends with the address space used.

Texture uploads (`src/mod/d3d9ex.cpp`): locks of the game's textures go through a one-level
system-memory texture, else a system-memory offscreen surface, else a plain buffer whose pixels are
dropped. The environment variable `HOVR_STAGING=texture|offscreen|buffer` forces one of them for a
run (`$env:HOVR_STAGING='buffer'; tools\run.ps1`), and `HOVR_STAGING=fail` makes every
block-compressed lock fail, as 0.1.3 did on a user's NVIDIA card (with this, the game here
still ran: it checks the result).

The game's options (`hovr\AppData\sega\typing of the dead overkill\config` in the test copy, 12
bytes): byte 1 = anti-aliasing (1 on: a 4x MSAA device; 0 off: no MSAA, flip swap effect, lockable
back buffer, and the composite target shares the back buffer's depth surface), bytes 2-5 = the
resolution (16-bit width, height). The composite target is that resolution fitted into the back
buffer.

## Fake SteamVR runtime (tests without a headset)

`out\fakevr\openvr_api.dll` (`src/fakevr/`) stands in for Valve's DLL in the test copy
(`tools\run.ps1 -Mode openvr -OpenVR fake`). It answers the OpenVR entry points itself and never
loads `vrclient.dll` or starts SteamVR, so the mod's real OpenVR code path runs: VR_Init, the eye
matrices, WaitGetPoses, Submit, the overlays, controller poses/buttons/render-model tip, haptics.
Its interfaces derive from the same `openvr.h` classes (stubs generated by
`tools\fakevr_gen_stubs.py`), so every vtable slot matches; methods it does not model log their
first calls to `hovr\fakevr.log`.

- **Headset** (shaped like the Steam Frame; the real numbers may differ): per-eye half-angles 52
  deg outer, 45 inner, 46 up, 50 down (asymmetric both ways, ~97 x 96 deg per eye), IPD 64 mm,
  recommended 2064x2016 per eye, 120 Hz; WaitGetPoses returns at the next 120 Hz vsync. Head at
  1.65 m, standing universe.
- **Controllers**: left = device 1, right = 2, type `oculus_touch`, render model
  `oculus_quest2_controller_<hand>` whose `tip` component is 1.5 cm below and 5 cm ahead of the
  raw pose, pointing 40 deg below its axis (as Killer7 saw the Steam Frame's controllers report);
  axes undescribed by default (the Steam Frame driver's way). Scripts give the pointing (tip) pose;
  the raw pose is derived, so a mod that ignored the tip would aim 40 deg low. Legacy buttons:
  trigger (+ axis 1), grip (+ axis 2), a (A/X), b (B/Y, application menu), stick (click), system.
  Haptic pulses are recorded.
- **Checks** returned as SteamVR's error codes: Submit (texture, D3D11 format, shader-resource
  binding, bounds, one submit per eye per frame), overlays (key in use, handle, width, texture).
  `hovr\fakevr.log` has one line per second: waits/s, frame time, missed vsyncs, submits per eye,
  overlay texture updates, errors returned, overlay visibility, haptics.
- **Scripting**: `tools\fakevr.ps1 'LINE' ...` (to `hovr\fakevr_cmd.txt`) or `fakevr LINE` in the
  mod's command file (in order with its commands). `head X Y Z YAW PITCH ROLL [MS]` (yaw + right,
  pitch + up, roll + right ear down; MS = move there over that time), `look`, `headpos`,
  `ctl L|R X Y Z YAW PITCH ROLL [MS]`, `ctlrel L|R X Y Z YAW PITCH [MS]` (relative to the head),
  `aim L|R X Y Z`, `aimov L|R KEY PX PY [REFW REFH]` (point at an overlay's texture pixel, e.g. a
  menu item on `hovr.screen`; with REFW REFH the pixel is given on a texture of that size and
  scaled to the real one, so a script written at 1920x1200 works at any back buffer), `btn L|R NAME [MS]`, `hold`/`release`, `axis`, `connect L|R 0|1`, `axes 0|1`,
  `tip 0|1`, `ipd`, `cant`, `hz`, `fov`, `wait MS`, `state`, `haptics`. Full list at the top of
  `src/fakevr/fakevr.cpp`. In openvr mode with the fake runtime, `vr handat`, `vr handscreen` and
  `vr handseek` point the scripted controller. `tools\scripts\to_naked_terror_fakevr.txt` plays
  from boot to Naked Terror through the flat-screen overlay with the controller.
- **Headset views**: `tools\fakevr_shot.ps1 NAME [lines...]` writes to `hovr\fakevr\`:
  `NAME_view0/1.png` (each eye as the wearer would see it: the submitted eye image plus every
  visible overlay drawn where SteamVR places it, from that eye's pose through its projection; no
  lens distortion or reprojection), `NAME_truth0/1.png` (the same with ground truth from the
  fake's own math: controller pointing rays with dots at 0.25-10 m, a cross at each tip, the level
  horizon, overlay outlines), `NAME_submit0/1.png`, `NAME_ov_<key>.png`, `NAME.txt` (poses,
  overlays, the rays' pixel positions) and `NAME_pair.png` (both eyes side by side; the truth
  images, or the plain views with `-Plain`).
  `tools\fakevr_check.py <dir>\NAME` finds the hit marker (`vr marker 1`) in both eyes,
  triangulates it and reports how far it is from the controller's pointing ray.
- **Approximations**: no lens distortion, reprojection, dashboard, focus loss or events; the
  real controllers' axis layout, tip transform and names are assumed from Killer7's notes; overlays
  are drawn double-sided and always on top (as SteamVR does) without its overlay filtering.

### The README's screenshots

`media/*.jpg` are `NAME_view0/1.png` captures (the plain views: no truth overlays, hit marker and
crosshair off) from the fake runtime in the test copy, cropped and saved as JPEG (quality 85, at
most 1600 px wide). Recipe: `tools\run.ps1 -Mode openvr -OpenVR fake`; the mod's command file gets
the menu clicks of `to_naked_terror_fakevr.txt` with `fakevr capture NAME` between them (the
loadout screen), then `vr god 1`, `vr marker 0`, `vr crosshair 0` in gameplay; then
`tools\fakevr_shot.ps1 NAME -Plain 'look ...' 'ctl R ...'` for each picture: gameplay with the
controller at about `0.18 1.38 -0.40` pointing slightly left, the gun from the side with
`ctl R 0 1.50 -0.30 90 0 0` (B switches to the SMG first), the reload with `hold R trigger`,
`release`, `btn R a 150` and a capture 350 ms later. Afterwards `tools\run.ps1 -Stop` and
`tools\run.ps1 -Mode sim -OpenVR real -NoStart`.

## Releases (maintainers)

- **CI** (`.github/workflows/build.yml`): every push and pull request to main builds the 32-bit
  RelWithDebInfo DLLs with MSVC x86 and Ninja, runs the unit tests, packages
  `hotd-overkill-vr-ci-<sha>.zip` and its `-pdb.zip`, checks the zip's file list
  (`tools\check_package.ps1`: exactly the two DLLs, README, `LICENSE`, `LICENSES.txt` at the top
  level; Valve's `openvr_api.dll` by hash, never the fake runtime; the default settings built into
  the mod's DLL) and uploads both zips as artifacts. The file list is also a notice on the run's
  summary page.
- **Before tagging**, on this machine (CI has no game): `powershell -ExecutionPolicy Bypass -File
  tools\release_check.ps1`. It builds, runs the unit tests and the package check, then plays the
  test copy (`C:\Users\Virtual\hotd-vr-game`) with the fake SteamVR runtime: start-up, menus by
  pointing to gameplay, a stereo capture, the hit marker against the controller ray
  (`tools\fakevr_check.py`), buttons, recentre and quit. It writes the pass/fail summary to
  `out\release_check.txt`, stops the game it started (by PID) and puts the test copy back to
  `mode=sim` with Valve's `openvr_api.dll` (checked by hash). `-SkipBuild` reuses `out\`.
- **Tag**: move the `## [Unreleased]` notes in `CHANGELOG.md` into a `## [X.Y.Z] - YYYY-MM-DD`
  section, commit, push, wait for CI to pass, then `git tag -a vX.Y.Z -m "vX.Y.Z"` and
  `git push origin vX.Y.Z`. The release workflow (`.github/workflows/release.yml`) builds and
  tests the tag again, packages `hotd-overkill-vr-X.Y.Z.zip`, `-pdb.zip` and `.sha256`, and
  creates the GitHub Release with that CHANGELOG section as its notes (it fails if the section is
  missing or undated). Versions 0.x and versions with a `-` suffix (`v0.2.0-rc1`) are marked
  pre-release. Tags are never moved or reused: a broken release gets a new patch version.

## Headset playtest checklist (milestones 1-4)

One session covers everything. Have the desktop window in view or a second person at the keyboard;
take notes of the setting values that feel right.

**Before**: install into the Steam copy (extract the release zip), or use the test copy with
`tools\run.ps1 -Mode openvr -OpenVR real -Normal` (`minimized=0`, `mute=0` in its `probe.ini`).
Start SteamVR with both controllers on, then the game. If anything goes wrong, close the game and
start it again holding **Shift** (flat, no hooks), and keep `hovr\hovr.log` (`hovr\probe.log` in
the test copy).

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
   model changes with the HUD), grip throws a grenade, Y/X pauses; a thump per shot (a double one for shotguns, a buzz
   for the SMG/minigun), a tick on a hit, a dry click on an empty pull, a click on reloads (`haptics`).
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

## Known issues and untested (details)

- **Nothing has run in a headset yet**. The OpenVR path (submit, overlays, controller poses and
  buttons, haptics, events) has run against the fake SteamVR runtime (above) with a Steam
  Frame-shaped headset: menus by pointing, stereo and head tracking (yaw, pitch, roll, lean), the
  asymmetric projections, the hit marker on the controller's ray at any distance and image
  position, shots from a hand held off to the side, buttons, haptics, recentre (button and
  resume), controller disconnects. What only the headset can tell: lens/comfort, the real
  controllers' names, axes and tip transform (assumed from Killer7), SteamVR's own Submit and
  overlay behaviour, frame timing on the real compositor.
- Frame rate (fake runtime, this VM's R9700, 2064x2016 per eye, first Naked Terror room): 90 Hz
  held, 120 Hz not quite (~100 frames/s). If SteamVR reprojects, try 90 Hz or `eye_scale=0.8`.
- The flat screen (3.2 m wide at 3 m) maps the 1920-pixel menu onto about 1000 headset pixels:
  small menu text is about half its desktop size. A larger `screen_width` or a smaller
  `screen_distance` helps if it is hard to read.
- The pause/death screens appear where the rail camera faces (the anchor), not where you look;
  gameplay recentres to where you look when it resumes.
- Release mode was checked by code review, a unit test of the decision (`test_harness_plan`) and
  the development log's dry-run line - not by a release run in the test copy, because that would
  play with the real save. The same goes for the first start writing `hovr\hovr.ini`: unit tested
  (`test_default_settings_written_only_when_missing`) and reviewed, not run in the game.
- Window focus: in release the game's window is left alone. If the game pauses or ignores the
  controllers while the desktop window is not in front, set `keep_active=1`.
- With the game's reload animation hidden (`game_arms=0`) the only reload cue in your hand is the
  magazine drop (with `gun_parts`: the hand cannon's cylinder swinging out, the pump's forend
  racking at the end); the crossbow has no reload animation (its "clip" is a plain box).
- The death screen's red backdrop is the game's own (flat screen); no zoom or blur was seen in
  stereo frames, so there is no `hit_zoom` setting.
- No head-bob was seen in this build (the camera kept a constant height over 15 m of rail);
  `head_bob=0` only holds the game's bob amplitudes at zero as a safety net.
- Looking more than `cone_deg` away from the rail camera's direction can show objects culled by the
  game. With `comfort_smooth_ms` the view can lag the rail camera by up to `cone_deg/2`, which
  leaves the other half for the head on that side.
- During fast rail turns the hit marker (`marker=1`) is drawn up to a few degrees beside the barrel
  line (fake runtime, Naked Terror: 2-16 degrees at 90-310 deg/s, with or without the comfort
  smoothing): the aim point comes from the game frame before the one drawn. With the camera still
  it sits on the barrel line, also with a 15-degree smoothing lag held (`smooth 5000`, 2026-11-18).
  Naked Terror's rail turns at up to ~575 deg/s (100 ms average), ~1000 deg/s in a single frame;
  no camera cuts were seen there.
- Only Steam build 569304 of `HOTD_NG.exe` is supported, and copies of it changed elsewhere than
  the anchors; anything else leaves the mod a plain XInput pass-through (the mod checks this itself
  at every start: "The exe check and its anchor table"). Patched copies were tested in sim with a
  4 GB patch and a changed int3 padding byte (2026-11-18); other patchers' real changes are
  untested.
