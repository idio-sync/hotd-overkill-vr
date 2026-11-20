# Gun accuracy test - design

Date: 2026-11-19. Status: approved (measure and report first; fixes after the user sees the
report). Implemented by subagents in the repo.

## Why

The first session in a real headset (Steam Frame) went well overall, but aiming felt "slightly
off compared to other VR games" and more shots missed than expected, though hitting things was not
hard. Nobody could tell why. Shooting also felt "light", which is the next piece of work (muzzle
flash, recoil, haptics: its own design, after this one); with little feedback on where a shot
went, an aim error is hard to see, so the aim is measured first.

This work answers, with numbers: **do bullets land where the drawn barrel points, for every
weapon, standing still and while the rail moves?** Then it fixes what the numbers show.

## What exists today

- The shot ray is built to be the barrel line: `hand_world_ray` (src/mod/gun.cpp) takes the gun
  hand pose (`vrsys_hand_in_game`), the barrel from `gunmodel_barrel` (per-weapon description in
  src/mod/gunmodel.cpp `kDefs`: barrel direction, muzzle point, grip, measured offline from the
  meshes with tools/gun_mesh.py) and the engine's view matrix read at fire time (`read_view`,
  0x9C7E48), and hk_fire overrides FireBullets' origin and direction (+0x7B4) with it. The game
  then applies its own spread (multi-pellet weapons) and ray casts.
- tests/test_stereo.cpp checks the math (points along the drawn barrel lie on the shot ray). That
  is the mod checked against itself: it cannot catch a wrong description or a timing error.
- tools/release_check.ps1 checks the hit marker (`marker=1`, the predicted aim point) against the
  fake runtime's controller ray, magnum only, rail and controller still. It never looks at where
  the game's bullets land.
- Only the magnum and SMG have been seen in game (the test save owns those two). The other seven
  weapons' descriptions come from their meshes only.
- Known: during fast rail turns the hit marker is drawn 2-16 degrees beside the barrel at 90-310
  deg/s (docs/DEVELOPMENT.md, known issues): the aim point comes from the game frame before the one
  drawn. Whether the bullets are off too, or only the marker, was never measured. Naked Terror's
  rail turns up to ~575 deg/s.

## Goals

1. A repeatable harness test in the test copy (fake runtime, minimized, muted) that fires real
   shots and reports, per weapon:
   - the **bias**: the angle and the perpendicular distance (cm, at the impact) between each
     bullet's actual impact and the barrel line **as drawn in the frame the player was looking at
     when the shot was fired**;
   - the **spread** of multi-pellet weapons, separately from their bias;
   - both with the rail **still** and while it **turns**, with the camera's angular speed per shot,
     so the error can be read against speed.
2. An independent check of each weapon's description against its live mesh: the bore axis and
   muzzle fitted from the vertex data the game loaded, compared with `kDefs`.
3. All nine weapons.
4. **A report checkpoint**: the measuring work ends with the report shown to the user, before any
   fix. Fixes (Design 7) are a second plan, made from the report once the user has seen it, with
   the test re-run to show each fix.

## Non-goals

- Muzzle flash, recoil, haptics (next design).
- Changing the game's own spread. It is reported; whether to offer a setting for it is a question
  for later.
- How the gun sits in the hand compared with other games (grip angle, `hand_pitch`): that is feel,
  not measurable without a headset. It stays a playtest item.
- Testing in a real headset (the user does that afterwards).

## Anti-goals

- A reference that comes from the mod's own shot math. The truth must be independent: where the
  game's own ray cast put each bullet, the barrel as it was actually drawn, and the mesh's
  vertices.
- "Fixing" numbers by moving the shot off the drawn barrel. The barrel and the shot stay one line.

## Constraints

- The test copy only (C:\Users\Virtual\hotd-vr-game), started with tools\run.ps1, minimized and
  muted; stop only its own PID; the fake runtime, never SteamVR; never the Steam folder or
  %APPDATA%\sega.
- Getting all nine weapons may need changing the test copy's own save (the redirected save inside
  the test copy): back it up first and restore it afterwards.
- release_check must still pass 11/11.
- The new logging is development-only and off by default; nothing changes for players until a fix.

## Approaches

Considered in conversation:

1. **Measure, fix, then add feedback** (chosen). The feedback is not built on an aim that may be
   off, and the numbers say whether the turn lag explains the misses.
2. Feedback first, test after: something new to try in the headset sooner, but it can make an
   off aim feel nicer while the cause stays unknown.
3. Both at once in separate branches: faster in total, but both touch the gun drawing code.

For the bullets' truth:

- **The game's own ray cast results, hooked** (chosen): exact, every pellet, hits and misses.
- Finding the hit sparks in both eye images and triangulating them, as fakevr_check.py does for
  the marker: independent of hooks, but noisy, needs a hit on a surface, and slow. Kept as an
  optional cross-check for one weapon if the hook's numbers look surprising.

## Design

1. **Impact capture.** A dev switch (`vr shotlog 1`) logs every ray cast FireBullets makes for
   player 1 (filtered by return address, like hk_fire): origin, direction, hit point, entity and
   whether it is shootable, the weapon, the game frame number, and the pellet index. Bullets that
   carry on through a hit (FireBullets' recursive calls) are logged as such and kept out of the
   bias.
2. **The barrel as drawn.** Each frame gunmodel draws the gun, it records the muzzle point and
   barrel direction in world space from the world matrix it actually drew with, with the frame
   number and the camera's angular speed. Each shot is measured against the frame on screen when
   it was fired (the last frame submitted before hk_fire ran).
3. **Mesh check.** From the live vertex buffer of each weapon (the one gunmodel already holds),
   fit the bore axis near the muzzle and the muzzle end itself, and compare them with `kDefs`:
   angle and offset.
4. **Scenarios**, scripted with the fake runtime: rail still (god mode, a point where the camera
   holds), and rail turning (Naked Terror's turns, firing throughout). Two controller poses (as
   release_check's neutral and edge), near and far targets.
5. **Weapons**: all nine, equipped in turn through the loadout (the test save gets them, see
   Constraints).
6. **Report**: a script (tools/gun_accuracy.ps1 driving the game, plus a Python analysis of the
   log) writes a table per weapon: still bias (deg, cm), turning bias against angular speed,
   spread, mesh check. Proposed thresholds: still bias at most 0.25 degrees; mesh check within 0.5
   degrees and 0.5 cm. Turning: reported; the target after a fix is at most 1 degree up to 300
   deg/s.
7. **Fixes** (second plan, after the report checkpoint), depending on the numbers: turn lag - the shot uses the view of the frame the player
   saw (kept per submitted frame) instead of the view read at fire time; description errors -
   corrected `kDefs`; spread - reported to the user to decide.
8. **Release check**: a quick subset (magnum, still) joins release_check; the full run is a
   documented manual tool in docs/DEVELOPMENT.md.

## Decided after review (my calls, open to correction)

- **Projectile weapons** (weapon type 5, the 0x4E1330 path, e.g. the crossbow) never reach
  FireBullets' ray casts. For them the truth is the projectile's launch: the origin and direction
  the game actually gives it (logged inside that call, after its own camera-space offset), against
  the barrel line drawn. Where it lands is logged too if a collision hook is cheap to find; any
  drop or drift in flight is reported separately, not counted as bias.
- **A moving hand is in scope.** Besides the still poses, one scenario sweeps the controller
  across the view at a typical aiming speed (fake runtime `ctl ... MS`) while firing, rail still,
  so the error from reading the hand pose at fire time instead of the pose drawn shows up on its
  own. "The frame on screen" means both the view and the hand pose that frame was drawn with.
- **The marker and the cursor follow any timing fix.** If the shot moves to the frame the player
  saw, the per-frame aim point (`g_aim`, the hit marker, the SetCursor overrides) moves with it,
  so the marker, the crosshair and the shots keep agreeing; release_check's marker check is re-run.
- **The mesh check compares against the built-in `kDefs`.** The test copy's `hovr.ini` has no
  weapon overrides during the run (checked at the start of the run); an override found is logged,
  not failed.

## Left to the builder

- Which ray cast call inside FireBullets to hook, and its return address.
- How to fit the bore axis from the vertices.
- Where the rail holds still long enough, and how to equip each weapon in the test save.
- The report's exact format.
