# Changelog

All notable changes to House of the Dead: Overkill VR. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). Licence: MIT, see LICENSE.

## [Unreleased]

## [0.1.0] - 2026-11-17

The first release: milestones 1-4 of the design. **Not yet played in a real headset**: everything
below was checked without one, in the mod's no-headset mode (`sim`) and against a fake SteamVR
runtime shaped like the Steam Frame. Please try it, and send `hovr\hovr.log` with any report.

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

Install: README.md, "Install (players)". Uninstall: `install_game.ps1 -Game "<game folder>"
-Uninstall`, or delete `XINPUT1_3.dll`, `openvr_api.dll` and `hovr\` from the game folder.

[Unreleased]: https://github.com/idio-sync/hotd-overkill-vr/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/idio-sync/hotd-overkill-vr/releases/tag/v0.1.0
