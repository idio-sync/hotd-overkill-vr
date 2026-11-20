# Settings: `hovr\hovr.ini`, section `[vr]`

Every setting the mod reads, in `hovr\hovr.ini` in the game's folder. The mod writes that file at
its first start with the most useful settings and their comments; add any other key below to the
`[vr]` section to change it. Delete the file to get the defaults back. The short list for players
is in the [README](../README.md#settings).

| key | default | meaning |
|---|---|---|
| `mode` | `off` (the `hovr.ini` written at the first start: `openvr`) | `off` = the game untouched; `sim` = no headset, the desktop window shows what the headset would; `openvr` = SteamVR. Holding **Shift** while the game starts, or SteamVR failing to start, means `off` for that run. |
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
| `gun_parts` | `1` | the gun's own parts move: the slide snaps back on each shot (and stays back when the magazine is empty, until the reload), hammers, the hand cannon's cylinder turns a chamber and swings out on reloads, the pump's forend racks, charging handles kick, the minigun's barrels spin while the trigger is held, the crossbow's string snaps forward and is drawn again, and every trigger follows the controller's trigger. Drawing only: shots still go along the un-moved barrel. `0`: the models stay still (the hand cannon's cylinder and the pump's forend are still drawn in place) |
| `game_arms` | `0` | `1`: the game's own first-person arms and weapon during reloads (Varla and Washington both have them; in VR a second gun in front of your face). `0` hides them |
| `camera_shake` | `0` | `1`: the game's camera shake and hit jolts (turns of up to ~30 degrees when a zombie hits you) |
| `head_bob` | `0` | `1`: the game's head-bob while the rail moves (none was seen in the levels checked) |
| `recentre_resume` | `1` | recentre whenever gameplay resumes after a flat screen (pause, death screen and continue, cutscenes, the next chapter), as at a level start |
| `comfort_vignette` | `0.6` | darkens the edges of each eye while the rail camera turns or moves fast (a "tunnel" that hides the fast-moving periphery). The strength at full speed: `0` = off; at `0.6` the view is clear within 35 degrees of the eye's centre and 96% dark beyond 50 (the edges of the headset's view); `1` = black beyond 40 degrees, clear inside 25. Lower values are lighter and wider (`0.3`: at most 48% dark, from 42 degrees out). In between the speed thresholds it scales with the speed. Drawn under the HUD panel; camera cuts don't trigger it |
| `vignette_turn_dps` | `30,120` | rail camera turn speed (degrees per second, measured over ~100 ms) where the vignette starts / reaches full strength |
| `vignette_move_mps` | `2,6` | rail camera speed (metres per second) where the vignette starts / reaches full strength; the larger of the two speeds wins |
| `vignette_ease_ms` | `200` | how long the vignette takes to fade fully in or out |
| `comfort_smooth_ms` | `150` | the view follows the rail camera's turns through a smoothing filter with this time constant (milliseconds; `0` = off), softening sudden starts and stops of a turn. Position is not smoothed. The lag is capped at half of `cone_deg` (culling only covers the cone); camera cuts (a jump of more than 1 m or 45 degrees in one frame) snap. The gun aims through the same view, so shots still go where the barrel points |
| `comfort_level` | `1` | keeps the horizon level: `0` = the game's camera as it is, `1` = its roll removed, `2` = roll and up/down tilt removed (only its heading kept; at most `cone_deg/2` of tilt is removed) |
| `keep_active` | `0` | `1`: the game keeps running when its window loses the focus (try it if the game pauses or stops reacting while you are in the headset) |
| `dev` | `0` | `1`: the development harness (as with `hovr\probe.ini`): save redirected into `hovr\`, command file, minimized and muted window. Not for playing |
| `show_<weapon>` | see below | `0` hides that weapon's model (aim and everything else still work) |
| `clip_<weapon>` | see below | magazine: `0` hidden, `1` drops on reload, `2` always drawn, no animation. For the hand cannon (its cylinder), pump (its forend) and minigun (its barrel cluster) only on/off: `0` hidden, anything else drawn in place as a moving part |
| `hold_<weapon>` | `0,0` | yaw, pitch (degrees) the model is turned in the hand about its grip; the aim turns with the barrel |
| `grip_<weapon>`, `muzzle_<weapon>`, `barrel_<weapon>` | measured | model-space grip point, muzzle point (cm) and barrel direction; defaults measured from the meshes |
| `sim_hand` | `0.15,-0.25,-0.35` | sim only: gun hand position relative to the head, metres |
| `sim_hand_yaw`, `sim_hand_pitch` | `0` | sim only: gun hand direction relative to the head, degrees |

`<weapon>` is one of `magnum`, `gold_magnum`, `handcannon`, `pump`, `autoshotgun`, `smg`,
`rifle`, `minigun`, `crossbow`. Defaults: all models shown; magazines drop for the magnum, auto
shotgun, SMG and rifle; the hand cannon's cylinder, the pump's forend and the minigun's barrel
cluster (their "clip" meshes) are drawn in place as moving parts; the crossbow's (a plain box) is
hidden. The gold magnum has no magazine.

## Default controls

| controller | game |
|---|---|
| gun trigger | fire (hold for automatic weapons); in menus: click where the gun points |
| gun A (A/X) | reload |
| gun B (B/Y) | switch weapon |
| gun grip | grenade |
| other hand's B or A (Y/X) | pause menu (Escape) |
| gun stick click, held 1 s | recentre the view |
| point at the flat screen | moves the menu cursor |

The milestones mentioned above are the stages the mod was built in (see
[DEVELOPMENT.md](DEVELOPMENT.md#design-notes)): 1 stereo and head tracking, 2 the controller as the
gun, 3 the weapon model, 4 comfort.
