# Gun feedback - design

Date: 2026-11-20. Status: approved in conversation (the user asked for this write-up, a review, and
then implementation without a separate implementation plan).

## Why

In the first real-headset session (Steam Frame) shooting felt "light" and not immersive. The aim
itself was measured and fixed separately (docs/superpowers/specs/2026-11-19-gun-accuracy-design.md:
shots now use the hand pose of the frame on screen, and the barrel descriptions sit on the real
bores). This work makes each shot feel like something happened: the gun's own parts move, it
flashes, kicks and thumps. The user mostly plays with the pistol (magnum), so the pistol is the
quality bar.

## What exists today

- The mod draws the game's own first-person gun mesh in the hand with its own shaders
  (src/mod/gunmodel.cpp): one static mesh per weapon (one node, one primitive group, 52-byte
  vertices, no bones) plus a `_Clip` mesh, drawn into each eye after the scene's opaque pass with
  the scene's depth, so the game's bloom treats it like the scene. The `_Clip` is dropped and
  re-seated on reloads (kClipDrop), drawn fixed (kClipFixed, minigun) or hidden (kClipNone).
- Live vertex buffers can be read back (the gun accuracy work's `vr gunmeshcheck` does it with a
  READONLY lock); the index buffer the same way.
- Shot events: hk_fire (FireBullets, player 1) and the type-5 projectile path in src/mod/gun.cpp;
  shot_feedback -> input_shot (one legacy haptic pulse, longer for "strong" weapons).
- Weapon state: cWeapon+0x760 ammo, +0x764 reloading flag, +0x76C reload progress
  (vr::clip_out_fraction drives the magazine animation), weapon data type/kind/pellets
  (gun_log_state, shot_feedback).
- The gun hand controller's analog trigger: vrsys.cpp (c.state.trigger, 0..1).
- Haptics: legacy TriggerHapticPulse only (max 3999 us per call; strength is felt by length); the
  mod reads buttons through the legacy controller state, and an action manifest would switch the
  app off legacy input.
- The PC game has no first-person muzzle flash (docs/findings-probe.md); shots only add world
  sparks and blood at the hit.

## What the meshes allow (recon, 2026-11-20)

Each gun mesh is many closed pieces (connected components after welding positions within 0.01
cm) that touch without sharing vertices, so most moving parts can move as whole pieces. Model units
are cm in each mesh's own space. Rank = a component's place when sorted by triangle count (for
orientation only; the build must select pieces by geometry, not rank). Scripts and pictures:
the session scratchpad `meshparts\` (load.py, comps.py, render.py, place.py; contact_side.png).

Two current bugs found:

- **Hand cannon**: its `_Clip` is the revolver's 6-chamber **cylinder** (with crane and ejector
  rod), not a speed loader. The gun has an empty window for it (x 5.88-11.77, y 5.27-9.86), so the
  hand cannon is drawn today without a cylinder. Offset (+10.30, +2.34, -0.81) puts it in the
  window with the top chamber on the bore; in `_Clip` space it turns about +x through
  (y 5.259, z 0.288), 60 degrees per chamber.
- **Pump**: its `_Clip` is the **forend** (a 19.5 cm closed sleeve), not a shell; the gun mesh has
  no forend. Offset (x +32.5, y +7.0, z 0) puts it on the magazine tube (tube x 20.6-50.6, axis
  y 7.93, z -0.49); racking back to x +24 gives ~8.5 cm of travel (10.4 cm at most fits).

Per weapon:

| Weapon | Moving parts (separable) |
|---|---|
| Magnum (Desert Eagle style; fixed barrel on top, slide below it) | slide = ranks 2, 3, 4 (slide safeties), 19 (rear sight), moves along -x ~2.5-3.5 cm; barrel rank 1 stays; hammer rank 9 (x -2.2..0.63, y 6.87..10.06), in the slide's path: tilts back about (x -0.3, y 7.0); trigger rank 8 |
| Gold magnum (along +z, no magazine) | slide and barrel are one piece (rank 1): it moves with ranks 5 (front sight), 6 (side panels) along -z; no hammer; trigger rank 4 |
| Hand cannon (the only revolver) | cylinder = `_Clip` (above); hammer rank 2 (x 0.84-4.64, pivot near (3.8, 5.0)); trigger rank 4 |
| Pump | forend = `_Clip` (above); trigger rank 4 |
| Auto shotgun | charging knob rank 13 (16 triangles, x 20.6-21.0, 1.3 cm out of the -z side); trigger rank 7 |
| SMG (Uzi style) | no bolt or charging knob modelled; trigger rank 8 |
| Rifle (toed in) | charging handle ranks 6 + 12 (x 9.2-17.7, -z side), moves along -bore (-0.969, 0.111, 0.221); trigger rank 9 |
| Minigun | the whole `_Clip` (6 barrels, radius 4.0-4.2, clamps, caps) spins about +z through (0.04, 0.01), except one 22-triangle sight block (y 4.9-6.3, z 17.8-19.8) |
| Crossbow | string = two halves (ranks 6, 7); each nock end = 4 positions at x ~1.7, y 8.83, z -1.83 / 0.14, touching 10 triangles that should stretch; on release the nock moves forward ~15-18 cm; cams (ranks 10, 11) at (20.21, 8.82, -22.25 / 20.57) r 3.8; limbs ranks 3, 5; no bolt in the mesh; trigger rank 29; `_Clip` is a plain box (stays hidden) |

## Goals

Stage 1 - parts (its own test DLL for the user):

1. The hand cannon's cylinder and the pump's forend are drawn in place (bug fixes).
2. Shot animations, driven by player 1's real shots:
   - magnum: slide snaps back ~3 cm and returns, the hammer rocks back with it; on the shot that
     empties the magazine the slide stays back (slide lock) until the reload seats the new magazine.
   - gold magnum: slide (with its barrel) snaps back and returns; slide lock on empty likewise.
   - hand cannon: the hammer cocks and falls, the cylinder turns one chamber (60 degrees).
   - pump: after each shot the forend racks back ~8.5 cm and forward, finished before the game lets
     the next shot fire.
   - auto shotgun and rifle: the charging knob/handle kicks back and returns.
   - minigun: the barrels spin up while the trigger is held (fire button down) and spin down after;
     no stroboscopic "standing still / turning backwards" look at the headset's frame rate.
   - crossbow: the string snaps forward on the shot and is drawn back again over the time until the
     next shot is possible (the nock triangles stretch, as a string should).
3. Every weapon's trigger follows the controller's analog trigger.

Stage 2 - flash, recoil, haptics (its own test DLL):

4. **Muzzle flash**: a bright flash at the muzzle in both eyes for about 2 frames (~30 ms) per shot,
   with a random rotation each shot; size per weapon (big: shotguns, hand cannon; medium: pistols,
   rifle; small and flickering: SMG, minigun; none: crossbow); the gun model's lighting brightens
   briefly with it. The flash image is generated in code (no game files extracted). Drawn
   depth-tested in the scene pass, so bloom picks it up.
5. **Recoil (visual only)**: the drawn gun kicks up and back about the grip and springs back over
   ~100 ms; per weapon (magnum ~4 degrees and 1.5 cm; hand cannon and pump the strongest; SMG and
   minigun small and fast, overlapping kicks capped so the gun doesn't wander).
6. **Haptic patterns** (legacy pulses): pistols a single strong thump; shotguns a longer double
   thump; SMG and minigun a buzz at the fire rate; a short extra tick when the shot hits a
   shootable entity; a dry click on a trigger pull with an empty weapon; a small click when the
   magazine seats on a reload (and when the pump forend closes).
7. **Settings** (`[vr]`, documented in docs/SETTINGS.md and the first-run hovr.ini comments):
   `gun_parts=1` (moving parts; 0 = the static mesh as before, but the two bug fixes stay),
   `muzzle_flash=1`, `recoil=1.0` (strength scale, 0 = off); the existing `haptics` scales the
   patterns (0 = off).

## Non-goals

- Changing the game's spread or anything else about how weapons behave in the game.
- The arms/hands (the game's skinned arms rig stays hidden).
- SteamVR Input (action manifest) haptics.
- Shell casings, sounds, smoke trails.
- Animating parts the meshes don't have (SMG bolt, crossbow bolt).

## Anti-goals

- **The aim moves.** Recoil and part motion are drawing only: the shot ray (gunmodel_barrel,
  hand_world_ray), the hit marker and the cursor must not include recoil or any part transform.
  The gun accuracy test (still and sweep) must stay at 0.000 deg against the un-kicked barrel.
- Animations that look wrong at a glance: a slide moving the wrong way, a cylinder turning
  backwards, a minigun appearing to stand still, triangles torn at a part's edge (except the
  crossbow string's intended stretch).
- Selecting pieces by component rank or vertex index alone (another level's pack could order them
  differently): select by geometry (bounding boxes in model space from the table above), and if a
  weapon's pieces don't match, draw it static and log why.
- Anything that can crash the game when a weapon's mesh is unexpected: every read stays
  exception-guarded as today, and a failure only disables the parts for that weapon.

## Constraints

- Development and tests only in the test copy C:\Users\Virtual\hotd-vr-game through tools\run.ps1
  (minimized, muted), with the fake SteamVR runtime; never SteamVR; never the Steam folder or
  %APPDATA%\sega; stop only its own game process.
- Weapons beyond the test save's magnum and SMG: unlocked in memory only, at the main menu, as the
  accuracy run did (player 1's profile `[[0x99B748]+0x228]`, `+0x5C` available and `+0x60`
  purchased masks = 0xF76, via peek/poke; two weapons per boot from the loadout screen). The save is
  never written that way; back it up anyway and restore it.
- 32-bit address space: the extra buffers stay small (per-weapon part data only).
- release_check 11/11; build.cmd test passes; the gun accuracy still and sweep scenarios stay at
  0.000 deg for the pistols.
- Branch `gun-feedback` (from `gun-accuracy`, which holds the unreleased aim fixes); no push, no
  tag, no release: the user tests each stage's DLL first.

## Approaches

Considered in conversation:

- **Recoil**: visual only (chosen: keeps the measured aim exact) vs a recoil that moves the aim
  (more "real", but makes the guns less accurate than the game and undoes the accuracy work).
- **Muzzle flash image**: generated in code (chosen) vs reusing the game's spark textures (finding
  them at runtime is harder, and the PC game has no muzzle flash to match anyway).
- **Haptics**: legacy pulses with per-weapon patterns (chosen) vs SteamVR Input actions (strength
  and frequency control, but it changes how every button is read).
- **Moving parts**: move whole pieces in our own vertex shader (chosen; the recon showed the meshes
  are built from separable pieces) vs skipping parts the user asked for. The SMG has nothing to
  move but its trigger.

## Design

1. **Part data, built once per weapon model when it is resolved**: read the vertex and index
   buffers back (as gunmeshcheck does), weld positions, find connected components, and assign each
   piece to a part by the model-space boxes in a per-weapon table (next to kDefs): slide, hammer,
   trigger, charging handle, string halves, etc. The `_Clip` meshes of the hand cannon, pump and
   minigun become parts with a rest offset (cylinder, forend, barrel cluster). Each vertex gets a
   part index in a copy of the vertex buffer the mod owns (or a second vertex stream); part 0 = the
   static rest.
2. **Drawing**: the existing gun shader gets a small array of per-part matrices (model space, in
   cm, applied before model_to_gun) and picks one by the vertex's part index. Normals are rotated
   with them. The crossbow's string nock vertices are moved by their own part matrix while their
   neighbours stay, which stretches those triangles as intended.
3. **Animation state, per weapon**: driven by player 1's shot events (hk_fire/the projectile path:
   a "shot" with its time), ammo (+0x760), the reloading flag and progress (+0x764/+0x76C, as the
   magazine animation uses), the analog trigger, and the fire button held (minigun). Curves:
   slide/handle back fast (~25 ms) and home slower (~60-80 ms); hammer follows the slide (magnum)
   or cocks before the shot and falls on it (hand cannon); cylinder turns 60 degrees after the
   hammer falls (~80 ms, eased); pump forend racks back and forward within the weapon's re-fire
   interval (measure the game's interval; start ~100 ms after the shot); minigun angular speed
   eases up/down (pick a speed whose per-frame step at 90-120 Hz doesn't alias for 6 barrels, or
   add a slight blur-friendly random phase); crossbow string snaps in ~20 ms and draws back over
   the re-fire time. Slide lock: if ammo is 0 after a shot, the slide stays back until the reload's
   magazine seats.
4. **Muzzle flash**: one or two additive camera-facing quads at the muzzle (gun frame origin,
   slightly ahead), per eye, depth-tested, drawn right after the gun; a procedural flash texture
   (a star/flare made at init, a few variants or random rotation); 2 frames; per-weapon size.
   Brighten the gun's lighting constant for those frames.
5. **Recoil**: a per-weapon kick (angle about the grip point, back distance) with a critically
   damped spring, applied to the drawn gun's matrix only (after the shot ray is taken); automatic
   weapons add smaller kicks with a cap.
6. **Haptics**: replace input_shot's single pulse with a small per-weapon pattern player (a queue of
   pulse lengths per frame), plus events: hit tick (the hit is known before firing: hk_fire's cast
   along the shot ray hits a shootable entity), empty dry click (trigger pressed with ammo 0 and
   not reloading), magazine seat / forend close click. Patterns are scaled by `haptics`.
7. **Dev commands** for checking each animation still: freeze a weapon's animation at a given
   time after a shot (e.g. `vr gunanim WEAPON T`), so side-view captures can show each phase;
   `vr gunanim off` returns to live.

## Verification (per stage)

- build.cmd test (unit tests for the pure parts: component splitting/welding on a small synthetic
  mesh, part selection by box, the animation curves, the pattern player).
- In-game, fake runtime, test copy: side-view captures (fakevr_shot.ps1, like the README's gun
  shots) of each weapon at rest and at each animation phase (dev freeze), and live captures while
  firing; a contact sheet for the user. Stage 2: the flash in both eyes, the kick, and the haptic
  pulses recorded by the fake runtime (`fakevr haptics` / hovr\fakevr.log).
- The gun accuracy still and sweep scenarios for the magnum (and the SMG) stay at 0.000 deg.
- release_check 11/11.
- Test copy restored by hash at the end; a test DLL copied to the session scratchpad.

## Decided after review (my calls, open to correction)

- **Stages**: stage 1 ends with a test DLL for the user (in the session scratchpad); stage 2 then
  starts straight away without waiting for their verdict, and adapts if their stage 1 feedback
  changes the shared pieces. Still no push, tag or release.
- **Reloads**: the hand cannon's cylinder swings out to the left on its crane (about an axis
  parallel to the bore, below-left of the cylinder, found from the crane piece) at the start of
  the reload and back in as the reload completes (the same timing the magazine animation uses); if
  the crane pivot can't be found cleanly it stays in place. The pump's forend racks back and forward
  once at the end of a reload (with the "forend closes" click). Neither ever drops like a magazine.
  `clip_handcannon` / `clip_pump` keep their meaning only as on/off: 0 = hidden (as before), any
  other value = drawn in place as a part (default on).
- **Directions**: the cylinder turns counter-clockwise seen from behind the gun (the shooter's
  view; Smith & Wesson style). The minigun spins clockwise seen from behind. The sign in model
  space must be checked with a capture from behind (not reasoned out), since model_to_gun flips
  handedness.
- The meshes and pictures from the recon are in
  C:\Users\Virtual\AppData\Local\Temp\claude\200e7d2c1826a451\fa217c89-0c2a-434e-a7ab-87c89b64c1ca\scratchpad\meshparts\.

## Left to the builder

- The exact boxes for selecting each part, pivots for triggers/hammers and travel distances within
  the ranges above; curve shapes and timings within the numbers above.
- Copy of the vertex buffer vs a second stream; shader constant layout.
- The flash texture's look, within "bright, short, generated in code".
- How the minigun avoids aliasing.
