# Changelog

All notable changes to House of the Dead: Overkill VR. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). Licence: MIT, see LICENSE.

## [Unreleased]

### Fixed

- **No gun in your hand with a 4 GB patched (Large Address Aware) `HOTD_NG.exe`** once the game's
  memory lies above 2 GB: the mod turned down every game address above 2 GB, so the gun model was
  never found (with nothing in the log about why). It now accepts the whole address range Windows
  gives the game.
- A weapon whose model wasn't found when it was first seen (while the level loads) is looked for
  again every second for two minutes, instead of never again until the level ends.

### Changed

- When no gun model is found, `hovr\hovr.log` now says why: what each place the mod looked at
  held and why it was passed over (an address out of range, no model there, another model), and
  what that most likely means. The mini-games (Stayin' Alive, Victim Support, Money Shot II) load
  no first-person weapon models at all, so they show no gun in your hand, only the aim; the log
  and the README's known issues now say so.

## [0.2.0] - 2026-11-20

Guns that feel like guns, and shots that go where the gun points. The guns' parts move, each shot
flashes and kicks, and the vibration differs per weapon. A test that compares where the game's
bullets land with the barrel drawn on screen found two aim errors, both fixed: shots while the
hand moved went slightly ahead of the barrel, and some guns shot from a point (or, for the
assault rifle, in a direction) a little off their visible barrel. All of it is checked with the
fake headset; it has not been tried in a real one yet.

### Added

- **The guns' own parts move**: on each shot the pistols' slides snap back (and stay back when
  the magazine is emptied, until the reload seats the next one), the magnum's hammer is pushed back
  with it, the hand cannon's hammer is cocked by your trigger finger and falls on the shot while its
  cylinder turns to the next chamber (and swings out to the left during reloads), the pump's forend
  racks back and forward after each shot and at the end of a reload, the auto shotgun's and
  assault rifle's charging handles kick back, the minigun's barrels spin up while the trigger is
  held and spin down after, and the crossbow's string snaps forward and is drawn again. Every
  trigger follows the controller's analog trigger. Drawing only: shots still go along the
  un-moved barrel. `gun_parts=0` in `hovr\hovr.ini` keeps the guns still.
- **Muzzle flash**: each shot flashes at the muzzle in both eyes for two frames and lights the gun
  up with it: big for the shotguns and the hand cannon, medium for the pistols and the rifle, small
  and flickering for the SMG and the minigun (a flash for every shot), none for the crossbow.
  `muzzle_flash=0` turns it off.
- **Recoil**: the gun kicks up about your grip and back on each shot and springs back in about
  0.1 s (the magnum about 4 degrees and 1.5 cm; the hand cannon and the pump the strongest; the SMG
  and minigun small, capped while firing so the gun doesn't wander). Drawing only: shots, the hit
  marker and the game's cursor still follow the un-kicked barrel (the gun accuracy test stays at
  0.000 deg). `recoil` scales it (`0` = off).

### Changed

- **Haptics per weapon**: instead of one buzz per shot, the pistols give a strong thump, the hand
  cannon a bigger one, the shotguns a double thump, the SMG and minigun a buzz while firing, the
  rifle a short thump per round; a light tick follows a shot that hits a zombie, a dry click comes
  when you pull the trigger on an empty magazine, and a click when a magazine (or the hand cannon's
  cylinder) seats on a reload and when the pump's forend closes. `haptics` still scales all of it
  (`0` = off).

### Fixed

- **The hand cannon had no cylinder and the pump no forend**: both are separate meshes in the
  game's files, which the mod took for a speed loader and a shell and left hidden; they are now
  drawn in place.
- **Shots while the hand moves go where the drawn gun pointed**: the shot, the crosshair and the
  hit marker now use the hand pose of the frame on screen instead of one about 15 ms newer, which
  put shots ahead of the barrel while sweeping onto a target (magnum, fake runtime: about 1 deg at
  60 deg/s and 2.5 deg at 180 deg/s before, 0.00 deg now; still aim unchanged at 0.000 deg).
- **Shots leave from the bore of the gun model**: the assault rifle shot about 2.4 deg above its
  visible barrel, and the magnum, gold magnum and pump shot from a point about 1 cm off their bore
  (hand cannon and auto shotgun 0.4-0.5 cm). Their descriptions now follow the bore fitted from the
  game's own meshes; the rifle model sits about 2.5 deg differently in the hand, so its barrel
  lines up with the controller.

## [0.1.4] - 2026-11-19

Fixes for anti-aliasing switched off or resolutions other than 1680x1050 in the game's options,
for missing textures on some graphics cards, and a crash report. They come from a user report
(NVIDIA RTX 4080 SUPER, Quest 3, 1920x1080 with anti-aliasing off on a 2560x1440 desktop) of the
game crashing a few seconds after it started; that crash itself could not be reproduced here, so
it may not be fixed: if it still happens, the new crash report says where.

### Fixed

- **Textures missing on some graphics cards** (seen on NVIDIA): the mod copies the game's texture
  uploads through a temporary surface, and that card's driver refused to make one for compressed
  (DXT) textures, so those uploads failed. Uploads now go through a system-memory texture, with
  two fallbacks, and an upload never fails: at worst its new pixels are dropped, and
  `hovr\hovr.log` says so.
- **Only part of the picture drawn with anti-aliasing off** in the game's options: the game then
  draws its final image with the desktop-sized depth buffer, smaller than the image the mod widens
  for two eyes, so menus filled only the top-left corner. A depth buffer of the right size now
  stands in.
- **The HUD and eye images with resolutions other than 1680x1050** in the game's options (such as
  1920x1080, or the desktop's own resolution): the mod now reads the resolution from the game's
  settings file to find the game's final image, instead of guessing it from its shape.

### Added

- **Crash report**: if the game crashes, `hovr\hovr.log` says where (module, offset and the
  return addresses on the stack) and `hovr\crash.dmp` holds a small minidump. Please attach both
  to a report. The log's 5-second status lines also show how much of the game's address space is
  used (it is a 32-bit game: 2 GB, or 4 GB with a Large Address Aware patch; about 1 GB of it at
  the menus without VR, 1.3 GB in gameplay with VR at 2752x2688 per eye on the test PC).

## [0.1.3] - 2026-11-18

Copies of the game exe changed by other patchers now work, as long as they leave the parts the
mod uses alone. Before, any change to `HOTD_NG.exe` switched the mod off.

### Changed

- **Patched copies of the supported `HOTD_NG.exe` are accepted.** Instead of comparing the whole
  file, the mod checks the 37 places in it that it depends on (the start of every function it
  hooks or calls, an instruction that uses each game variable and class it reads, and the return
  address it checks). If all of them match, VR stays on even when other parts were changed, for
  example by a 4 GB (Large Address Aware) patch; `hovr\hovr.log` then has a warning naming the
  parts of the exe that differ. If one of them differs, the mod stays off as before, and the log
  lists each place with the bytes it expected and the bytes it found. There is no setting to
  force VR on.
- Development: the table of those places is generated by `tools/anchors.py` (which also checks
  any exe: `--check`) and unit tested; `tools/install_game.ps1` accepts compatible patched copies
  too; with a refused exe the development harness keeps its save redirect, minimized window and
  mute but hooks nothing in the game.
- The build check in `hovr\hovr.log` now also logs the game exe's size, SHA-256, PE timestamp and
  Large Address Aware flag, and when the exe isn't the supported one it says whether it is a
  changed copy of the supported build (fix: Steam's "Verify integrity of game files") or a
  different build.
- README troubleshooting: what to do when the log says "not the supported build" or "patched copy
  ... compatible", and that Virtual Desktop / Quest users must start the game through SteamVR.

## [0.1.2] - 2026-11-18

Comfort settings for the on-rails camera. The first session in a real headset (a Steam Frame)
looked good; the one thing it found was that the rail camera's fast moves were too much, so this
release softens them. Still an early pre-release: please report problems with your
`hovr\hovr.log`.

### Added

- **Comfort while the rail moves you**, all on by default (the first headset session, on a Steam
  Frame, found the rail camera's fast moves too much):
  - a **vignette** that darkens the edges of each eye while the rail camera turns or moves fast
    (`comfort_vignette=0.6`; starts at 30 deg/s or 2 m/s, full at 120 deg/s or 6 m/s:
    `vignette_turn_dps`, `vignette_move_mps`; fades over `vignette_ease_ms=200`). Camera cuts
    don't trigger it; the HUD panel stays clear.
  - **smoothed turns**: the view follows the rail camera's orientation through a 150 ms filter
    (`comfort_smooth_ms`; position is not smoothed), lag capped at half of `cone_deg`, no
    smoothing across camera cuts. The gun aims through the same smoothed view, so shots still go
    where the barrel points.
  - a **level horizon**: the rail camera's roll is removed (`comfort_level=1`; `2` also removes
    its pitch).
  - Development commands: `vr comfort on|off|vignette X|smooth MS|level N|turn A B|move A B|ease MS`
    switch them live, `vr comfort stats [S]` logs the rail camera's speed percentiles and how often
    the vignette was on, for tuning.

### Changed

- The README is written for players, with screenshots; the full settings list moved to
  `docs/SETTINGS.md` and everything for developers (building, the test tools, releases, the
  headset playtest checklist) to `docs/DEVELOPMENT.md`. The comment at the top of a newly written
  `hovr\hovr.ini` points to the settings list.

## [0.1.1] - 2026-11-17

Installs by copying files: extract the zip into the game folder. Otherwise the same mod as 0.1.0;
still **not yet played in a real headset**.

### Changed

- **Drag-and-drop install**: the zip is laid out for the game folder (`XINPUT1_3.dll`,
  `openvr_api.dll`, `README.md`, `LICENSE`, `LICENSES.txt` at the top level). In Steam,
  right-click *The Typing of the Dead: Overkill* > Manage > Browse local files and extract the zip
  there, next to `HOTD_NG.exe`. Don't overwrite an `XINPUT1_3.dll` that another mod put there.
  Uninstall: delete `XINPUT1_3.dll`, `openvr_api.dll` and the `hovr` folder.
- **Default settings written on first run**: when `hovr\hovr.ini` is missing, the mod writes the
  default settings there at its first start (`mode=openvr`, with comments) and never replaces an
  existing file, so extracting a newer zip over an old install keeps your settings (the zip has no
  `hovr.ini`). The mod still checks the game build itself and does nothing on other builds.
- **The installer script is no longer shipped**: `install_game.ps1` and `hovr.default.ini` are not
  in the zip (the script stays in the repository as a development tool).

### Fixed

- Wrong folder name in the docs: the Steam folder is
  `...\steamapps\common\Typing of the Dead Overkill` (no "The").

## [0.1.0] - 2026-11-17

Withdrawn: superseded by 0.1.1, which installs by copying files.

The first release: milestones 1-4 (docs/DEVELOPMENT.md, "Design notes"). **Not yet played in a
real headset**: everything below was checked without one, in the mod's no-headset mode (`sim`)
and against a fake SteamVR runtime shaped like the Steam Frame. Please try it, and send `hovr\hovr.log` with any report.

Needs *The Typing of the Dead: Overkill* on Steam (Steam build 569304 of `HOTD_NG.exe`; the
installer checks it), SteamVR and a headset with tracked controllers.

### Added

- **Stereo VR with head tracking** (milestone 1) through SteamVR: each 3D draw is rendered once per
  eye at the headset's recommended size (`eye_scale` to lower it), the game's 60 Hz limiter is
  lifted during gameplay so the headset's rate paces it, and the culling is widened so objects do
  not pop when you look around (`cone_deg`, 35 degrees from the rail camera).
- **HUD panel and flat screen**: the HUD floats on a world-locked panel ~1.5 m ahead; menus, the
  loadout screen, cutscenes, the pause menu and the death screen appear on a large virtual screen
  ~3 m ahead. Panel sizes and distances are settings.
- **The controller is the gun** (milestone 2): every shot of player 1 goes along the controller's
  ray from the controller; the menu cursor follows where you point and the trigger clicks; buttons
  map to the game's own PC controls (trigger fire, A reload, B switch weapon, grip grenade, the
  other hand's B/A pause, stick click held 1 s recentres). Haptic pulse per shot; optional hit
  marker (`marker=1`) and reload gesture.
- **The game's own weapon in your hand** (milestone 3): the current weapon's first-person mesh is
  drawn at the controller, shots leave its muzzle along its barrel, and the magazine drops out and
  back in on reloads. Per-weapon tuning settings.
- **Comfort** (milestone 4): the game's camera shake and hit jolts, head-bob and WiiView edge-pan
  are off in VR, its own first-person reload animation (a second gun in front of your face) is
  hidden, and the view recentres whenever gameplay resumes after a flat screen. Each can be
  switched back on.
- **Installer** (`install_game.ps1`): checks the game build, refuses to overwrite another mod's
  `XINPUT1_3.dll`/`openvr_api.dll`, writes default settings; `-Uninstall` removes exactly what it
  installed (`-Purge` also the settings and log). Holding **Shift** while the game starts plays
  flat; `mode=off` in `hovr\hovr.ini` turns VR off for good. Your save is never touched by the mod.

### Fixed (found with the fake SteamVR runtime)

- The eye images were submitted at 2709x2940 per eye with non-square pixels (1.9x the pixels
  SteamVR asked for); they are now exactly the recommended size.
- The culling used the desktop field of view until the first stereo frame instead of the headset's.
- The first frame was submitted, and the menu screen placed at floor level, before SteamVR had
  given a head pose.
- SteamVR's quit request (SteamVR closing, or quitting from the dashboard) was ignored, so SteamVR
  would kill the game after a timeout; the game now closes cleanly.
- The desktop window no longer waits for the monitor's vsync in VR (it would cap the headset's
  frame rate).

### Untested / known issues

- Nothing has run in a real headset: lens comfort, the real controllers' names, button layout and
  pointing direction (assumed from another mod's notes on the Steam Frame), SteamVR's own handling
  of the eye images and panels, and frame timing on a real compositor. If shots land consistently
  above or below where the barrel points, tune `hand_pitch`; left/right offsets are a bug.
- Frame rate on the test machine (fake runtime, 2064x2016 per eye): 90 Hz held, 120 Hz about 100
  frames/s. Try 90 Hz or `eye_scale=0.8` if SteamVR reprojects.
- Small menu text on the flat screen is about half its desktop size (`screen_width`,
  `screen_distance`).
- The pause and death screens appear where the rail camera faces, not where you look.
- Only Steam build 569304 of the game is supported; any other build leaves the mod a plain XInput
  pass-through.

Install: README.md, "Install". Uninstall: `install_game.ps1 -Game "<game folder>"
-Uninstall`, or delete `XINPUT1_3.dll`, `openvr_api.dll` and `hovr\` from the game folder.

[Unreleased]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.1.4...v0.2.0
[0.1.4]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/idio-sync/hotd-overkill-vr/compare/c96bde3...v0.1.1
[0.1.0]: https://github.com/idio-sync/hotd-overkill-vr/tree/c96bde3
