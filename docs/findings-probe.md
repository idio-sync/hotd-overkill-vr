# Probe findings (2026-11-17)

Throwaway spike: a stand-in `XINPUT1_3.dll` (src/probe) in a test copy of the game
(`C:\Users\Virtual\hotd-vr-game`: hard-linked assets, copied executables). Static analysis is in
findings-static.md. Everything below was observed in the running game unless marked otherwise.

## Running it
- `build.cmd` → out\XINPUT1_3.dll; `tools\run.ps1 [-Stop] [-Normal]`; `tools\cmd.ps1 'cmd' ...`;
  `tools\step.ps1 NAME 'cmd' ...` (runs commands, then a screenshot + half-size copy);
  `tools\cmd.ps1 -File tools\scripts\to_gameplay.txt` right after run.ps1 reaches the first kitchen
  fight (~2 min; boot time varies, check with a screenshot). Commands: header of src/mod/control.cpp
  (the probe became the mod in src/mod; same DLL, same harness).
- Settings: `hovr\probe.ini` [probe] minimized, mute, windowed, fake_pad, cap_fps, vsync, gun_hook
  (development harness). The mod's own settings: `hovr\hovr.ini` [vr] (keys in src/mod/config.cpp);
  `tools\run.ps1 -Mode off|sim` sets its mode line.
- Launch HOTD_NG.exe directly (no Steam launch needed; steam_appid.txt is enough). The probe
  redirects `SHGetFolderPathA` (CSIDL_APPDATA etc.) into `hovr\AppData`, seeded once from the real save;
  the real `%APPDATA%\sega\typing of the dead overkill` (Steam Auto-Cloud) is never touched.
  The probe refuses to run if that redirect fails.

## Copy protection
- SecuROM PA (totdo.dll) loads the stand-in DLL without complaint, and an inline 5-byte detour on
  `cWeapon::FireBullets` (installed at the first Present) ran for minutes of gameplay with no reaction.

## Input
- Mouse position: `GetCursorPos` (faked by the probe). Mouse buttons and keyboard: DirectInput 8,
  immediate mode (`GetDeviceState`, DIMOUSESTATE 16 bytes / 256-byte key array), cooperative level
  NONEXCLUSIVE|FOREGROUND. Posted window messages alone do nothing.
- The XInput pad (faked as connected) did not move the menu cursor or selection.
- Menus are cursor-driven (the gun reticle is the menu pointer).

## Rendering (frame logs)
- Device: windowed is forced; the game asked for 1536x807 fullscreen, 4x MSAA back buffer.
- Per frame: shadow maps (1024² R32F + D24S8) → scene at a fixed **1280x720** ARGB8 → bloom/blur
  chain (640x180 RGBA16F, 320x180, 160x90, 640x360) → one composite draw into a **1291x807** target →
  **HUD: ~190 textured 2D DrawPrimitiveUP draws on that same target** (vs 1a10d46e76d587ab, ps
  73edbf0012ec0901) → one draw of the 1291x807 target into the back buffer (viewport x=122, pillarbox).
- All shaders are precompiled (D3DXCompileShader is never called). Every 3D vertex shader computes
  `oPos = dp4(v0, c0..c3)` with **cWVP** in c0..c3 (column-vector rows). Other view-dependent constants:
  cViewMatrix (shadow passes), eyePos (lighting), cShadowMapMatrix, cBoneMatrix (world-space bones).
- Camera recovered from cWVP·cObjectMatrix⁻¹ (tools/frame_camera.py): **hfov 100°, vfov 68°, 16:9**,
  centred projection, one VP per frame. eyePos ≈ (24, 236, 2800) at the kitchen start; units probably cm.

## Timing
- Gameplay and cutscenes: ~64 fps cap (frame time ≈15.6 ms, Sleep granularity); menus uncapped.
- `poke [va:0x9f09e0]+0x48 0` (the `disable-fps-limit` flag) → gameplay ran at **~256 fps** (827 draws).
- Game time (va 0x9F0B50) advanced 5.001 s over 5 s of wall time at dt ≈ 3.5 ms: variable timestep,
  real-time speed when uncapped. So the game can render at headset rate (90/120 Hz).

## Shots
- `FireBullets` origin = camera position (equals eyePos), direction = unit vector toward the cursor.
- Override test: control shots with the cursor in the top-right corner scored nothing; then one shot
  at a zombie captured its direction and two shots fired with the cursor in the corner but the
  direction overridden killed that zombie (score 0 → 100). The game's own ray cast follows the
  direction the mod supplies. (Origin override not exercised yet.)

## Weapon models (the game's own)
- The loadout screen's gun is a 2D picture (only 2D draws there). Gameplay draws no gun; the only 3D
  draws on the HUD target are the ammo rounds (vs 3a5de9a696514b3a, 1036 triangles each, one per round).
- Asset packs are zlib-compressed (`tools/pc_scan.py` inflates them and lists names). Per level:
  - `<level>_hud_<weapon>.pc` (9 weapons: pistol_magnum, pistol_gold_magnum, pistol_handcannon,
    shotgun_12gaugepump, shotgun_auto, mg_machinepistol, mg_assaultrifle, minigun, crossbow) each hold
    a first-person gun mesh `GunArms_<Weapon>` + magazine `GunArms_<Weapon>_Clip` (gold magnum:
    `GunArms_Gwendalin`, no clip) + the HUD round `HUD_Bullet_<Weapon>`; textures e.g. handgun_g_t/n/s.
  - `<level>_hud_arms{w,g,v,c}.pc` = Washington, Agent G, Varla, Candi: `GunArms_<Char>_Mesh` plus
    per-weapon reload animations (`_Magnum_Reload`, `_ShotGunPump_Reload`, `_Minigun_Overheat`,
    `_Gwen_Taunt`, ...).
- Level start opens hud_permasector, the equipped weapon's hud pack (only equipped weapons) and
  hud_armsw + hud_armsg (players 1/2) — the models are resident during gameplay but never drawn, in
  reload or otherwise (a cut first-person feature?). The exe has no "GunArms" strings (data-driven);
  the only related class is `cMeleeArms`. File opens go through MSVCR100 (hooked in files.cpp).

## Not checked
- HUD panel extraction, WiiView edge-pan (camera rides the rail during tests; static analysis
  found the enable byte), OpenVR submit from a D3D9 device, origin override, other weapons
  (type 5 path).
- Gameplay drains health quickly while idle; tests have to act right after reaching gameplay
  (continue = click (378,474) on the death screen).

## Stereo in sim mode (milestone 1 part 1, 2026-11-17)
- Same-frame stereo by per-draw duplication (src/mod/stereo.cpp): with `mode=sim` the screen-sized
  targets are double-wide (scene 2560x720 at eye_res 1280x720); each perspective draw on the scene
  target is drawn into each half with `C_eye * M` for its clip matrices (cWVP, cProject,
  cObjectViewProject, cProj) and eyePos moved per eye. ~310-340 scene draws per frame are
  duplicated in the kitchen; ~230 fps uncapped in sim mode over RDP.
- The engine's projection (0x9C7DC0) and view (0x9C7E48) globals at draw time equal
  cWVP * cObjectMatrix^-1 of the scene draws (`vr check`: max difference 1.2e-7 over 297 draws).
  Projection: sx 0.8391, sy 1.4917 (100 x 67.7 deg), near 5, far 18000.
- Units: camera at y = 236, zombies' roots (cObjectMatrix translation) at y = 73: eye height
  1.6 m (roots on the floor) to 2.4 m (floor at y = 0) at 1 unit = 1 cm. unit_m stays 0.01.
- Disparity at yaw 0, ipd 0.064 (tools/disparity.py): far walls -3..-6 px, floor -11..-18 px, a
  zombie close by -22..-26 px in a 1280-wide eye; no vertical offset.
- Not every draw on the scene target is perspective: screen overlays (blood on the screen, the death
  screen's backdrop) are orthographic and read no screen-sized texture; they go into each eye's half
  unchanged. Orthographic draws that read a resized target are drawn once across both halves.
- The DoF/SSAO pass's "texZBuffer" is a 16x16 dummy texture in both modes (no real depth input).
- Culling: the frustum builders 0x6DEDE0/0x6DF810 decide dynamic objects (zombies): `vr cone -45`
  (8 deg frustum) removed a zombie near the edge; cone 35 vs cone 0 at yaw 40 adds 14-23 draws.
  Static room geometry is not culled by them.
- Frame limiter: natively `[[0x9F09E0]+0x48]` = 1 (menus then run at ~31 fps); the game rewrites it
  at level start. The mod clears it only while the scene is drawn in stereo and restores it after.
- Device lost: in 2 of 6 early sim runs (frame limiter cleared from boot) the D3D9 device was lost
  during the intro (69 s / 94 s); TestCooperativeLevel stays lost and the game loops in 0x6786E0
  ("D3D device lost, waiting for ready state") forever while minimized. One loss coincided with DWM
  "registered the session port" events (RDP session churn), so not certainly the mod's doing.
- The game blurs/zooms the whole picture when the player is hit or dying; back-buffer shots taken
  then look smeared - check `vr scene` (the scene target itself) before suspecting stereo.

## HUD panel, flat screen, hand-off (milestone 1 part 2, 2026-11-17)
- **Game state** (memory diffs of menu, loadout, loading, intro cutscene, gameplay, pause, death):
  `[0x99B770]` = cCutsceneControl (made with the level by cGameModeRunLevel, null in the front
  end), +8 byte = cutscene mode (ActivateCutsceneMode node 0x514510; IsCutSceneRunning 0x57B420).
  `[0x99B71C]` = cGameModeManager, +0x310 byte = paused (getter 0x58A520): 1 in the pause menu
  *and* on the death screen. `[0x99B700]` = cGameSettings (+0x108 also 1 when paused).
  `[[0x99B708]+0x24]` = player 1 (cPlayer). Loading screens: level set, no cutscene, not paused,
  but no scene drawn - hence the frame-structure check too. At level start the cutscene flag rises
  ~0.3 s after the level appears: entering stereo waits for 0.5 s of steady gameplay state.
- **HUD**: after the composite draw the game draws ~8 3D ammo rounds (vs 3a5de9a696514b3a, DIP,
  perspective, depth-tested) and ~100-200 sprites (DPUP quads, vs 1a10d46e76d587ab whose cWVP
  places each quad; local coordinates). Then `SetRenderTarget(composite)` + z-clear, then the
  back-buffer blit (ps f84d4ddc609b00b2, viewport x=122 1291x807). Blend modes seen on HUD sprites:
  SRCALPHA/INVSRCALPHA (most) and DESTCOLOR/SRCCOLOR (a four-quad corner vignette). Forcing alpha
  to accumulate for every draw turned the vignette into opaque black corners; alpha is now only
  accumulated for "over" draws (multiplicative darkening is dropped from the panel).
- **Crosshair**: three to four 50x50 (local) sprites placed on the cursor; the cursor at
  `cPlayer+0x1BC/+0x1C0` is in the same space as the sprites' clip x*w/2, y*h/2 (composite
  pixels from the centre, +y up). Skipping sprites whose transformed quad centres on it (within
  2 px, smaller than 200 px) removes the reticle (1 draw per frame in the kitchen).
- **Hand-off options**: (b) a shared handle on the plain D3D9 device fails: `CreateTexture(...,
  &handle)` → D3DERR_INVALIDCALL (0x8876086c). (a) 9Ex works: Direct3DCreate9Ex + CreateDeviceEx
  (windowed), D3D11 device on the same adapter (LUID → DXGI adapter 0, R9700). The game creates
  1526 MANAGED textures, 27 MANAGED cube textures, 832 VBs and 830 IBs by the first fight (no
  volume textures); all became DEFAULT. Texture locks are staged through a SYSTEMMEM surface +
  UpdateSurface; ~1 lock per frame continues all the time (some per-frame texture update) and
  costs nothing visible. Rendering looked identical (menus, loadout, levels).
- **Cost** (sim, RDP, 1280x720 eyes, first kitchen fight, uncapped, stereo frames only, 5 s
  windows): plain D3D9 (handoff=0) 197-203 fps (~5.0 ms); 9Ex + hand-off (eye/HUD copies into
  shared textures, event-query wait 0.85 ms, D3D11 copies) 190-197 fps (~5.2 ms): about +0.2 ms
  a frame. Both include the desktop mirror. Flat gameplay with 9Ex (limiter poked off): 225 fps.
- **Pitfall**: `readable()` (VirtualQuery) costs ~0.2 ms in this process (it walks the page table
  of the huge heap regions to size them): calling it per HUD sprite took gameplay to 10 fps. Per
  frame or per draw reads use `safe_copy` (SEH) now.

## The controller gun (milestone 2, 2026-11-17)
- **Controls** (the game's own Game Settings > Controls screen): left click fire, **middle click
  reload**, right click grenade, mouse wheel or **TAB** swap weapon, Esc pause; a gamepad layout
  exists too. (Right click is *not* reload.) Input goes through an input map (data, "Input Map")
  into per-player action values: dispatcher 0x426A20 (thiscall input, player*, index, flag;
  ret 0xC) reads `[[input+0x2C+index*4]+0x6C][action]+0x2C`: 3 fire (0x511650), 4 grenade
  (0x40DE20), 5 reload (0x551C20 -> weapon+0x78F; also a motion "shake" gesture, 0x47D190),
  0xB next weapon (0x485060 -> 0x4F9A50), 0xC previous weapon (0x4E9C10 -> 0x5564E0), 24/25 the
  mouse cursor in client pixels. `vr actions 1` logs them; injected middle click -> 5, TAB -> 0xB,
  right click -> 4 confirmed. The injected mouse wheel did not show up as an action.
- **Weapon state** (for milestone 3): player 1 `[[0x99B708]+0x24]`; current cWeapon at
  `player+0x224`; the equipped weapons are a list at `player+0x218` (switching walks it; both
  set +0x224 and call 0x562BD0, which re-aims the weapon at the cursor and refills the clip if the
  reload time has passed). cWeapon: +4 weapon data (+0x20 type: 5 = projectile, 4 = grenade-ish;
  +0x24 kind (magnum: 1; 0xB and 8 are special-cased in the fire logic); +0x1C4 clip size;
  +0x1D4 reload time; +0x1E0 pellets; +0x204 range), +0x760 rounds in the clip, +0x764 byte =
  **reloading** (set with +0x765 by StartReload 0x5861F0, which also zeroes +0x760 and stores the
  reload timing in +0x76C..+0x780), +0x78C/+0x78D trigger pressed/held, +0x78F reload request,
  +0x794 owner player, +0x7B4 aim direction (vec4). Player health: `player+0x20C` (int, 10 full).
- **Hits from the hand** (sim, `vr handseek` + `vr btn trigger`): the shot log names what the hand's
  ray meets (`ray hits a shootable entity ... 1.01 m from the hand`); zombies died from shots
  whose game direction (through the old cursor) pointed elsewhere (score 0 -> 100 -> 200), also
  with the hand 60 cm to the right of the head. With `vr aim 0` and the mouse in a corner the same
  shots went into the ceiling. Pause-menu stats after the test: 9 shots, 55.6% accuracy, 2 kills.
- **Cursor**: SetCursorX/Y (0x5295F0/0x51FBD0) are called every gameplay frame (input writer and
  player update), so overriding their argument keeps player 1's cursor on the aim point; the
  player's per-frame aim (+0x1F0) then points at the same world point. The cursor clamps at the
  screen edge when the gun points off-screen (shots are unaffected).
- **ScreenToWorld 0x655620** (cdecl x, y, out vec4) is affine in the cursor; the sphere-check
  override solves for the cursor whose near-plane point lies on the hand's ray: the game puts it
  0.003 units off. The sphere checks (0x42B900, 0x479250) did not run in the first kitchen fight.
- The ray cast 0x5514E0 (cdecl 0, origin vec4*, segment vec4*, 0, result*; result 0x38 bytes made by
  0x4D6830, hit point at +0; entity via 0x48C8D0, "shootable" = its vtable slot 0x154) is safe to
  call from the cursor setters (game thread) and costs nothing noticeable once per frame.
- Slow motion for tests: `[[0x9F09E0]+0x54]` = scale with `+0x58` = 1 scales dt, but the rail
  camera kept its pace; `vr god 1` was the useful one.
- Menu back arrows (the "<" buttons) ignore injected clicks (harness and controller alike); Escape
  (the controller's pause button) goes back.

## The weapon model (milestone 3, 2026-11-17)
- **The models are instanced, just never drawn.** At level start the game makes an entity with an
  actor instance for `GunArms_<Weapon>` and `GunArms_<Weapon>_Clip` of every equipped weapon (for
  both players). The weapon data block (`cWeapon+4`) lists its entities as (hash, 0, entity*)
  triples from +0x28 (magnum: gun entity at +0x6C, magazine at +0x78). The actor instance
  (engine class, vtable 0x80FB58, no RTTI) sits inside the entity's allocation (E+0x290 seen):
  I+0x08 actor hash, I+0x8C the ACTR resource, I+0xC8 render flags, I+0xDC its transform node
  (position, quaternion, scale), I+0xF0 the owning entity.
- **How the HUD rounds are drawn** (`drawstack 3a5de9a696514b3a`): `cPlayerHUD3DWeaponClip`
  (vtable 0x7FFEC4, slot 20 = 0x437720) sets a HUD camera (copies the view 0x9C7E48, builds a
  look-at), writes each round's node position/quaternion (+4/+0x14, flag |= 0x800), and draws one
  instanced batch through 0x5B6C20 -> 0x7697A0 -> 0x5207E0 -> the instance's vtable+0x64 (needs
  I+0xC8 & 0x40, not 0x800). So the engine can draw an actor on demand, but only with its own
  camera/state set-up around it.
- **Packs** (`tools/gun_mesh.py`): the inflated .pc is resident verbatim (pointer fields fixed up).
  Header = 5-dword entries from offset 0 (x, hash, size, name offset, type; type 0 VSHR, 4 PSHR,
  8 TEXR, 0xC ACTR, 0x10 MTRL, 0x14 VTXD), then the string table (16-byte aligned). ACTR: +8
  hash, +0xC size, +0x28 node, +0x6C name; node+0xB0 mesh; mesh +0 vertex count, +4 vertex buffer
  record {IDirect3DVertexBuffer9*, 0, count, stride}, +0xC primitive groups {count, material,
  texture list*} (the list pointer points at the first texture record, its count is the dword
  before; SMG's groups are not 4-byte aligned), +0x44 index count, +0x48 index buffer record
  {IDirect3DIndexBuffer9*, 0, count, 2, D3DFMT}, +0x60 vertex format hash. Texture record W:
  W+8 hash, W+0x20 == W, W+0x24 IDirect3DTexture9*; the list is diffuse, normal, specular.
  Vertex format vf_stock_staticsinglenormalmapped = position f3, normal f3, colour, uv f2,
  tangent f4 (52 bytes). Every GunArms mesh is one node and one primitive group; vertices are in
  the model's own space (no skinning needed for the bind pose).
- **Method chosen: option 2** (the engine's own GPU buffers and textures, our own draw call and
  shaders). Option 1 (making the engine draw its instance) would need the engine's camera/light
  state set up around an unknown draw entry point, and would draw with the game's single camera
  (we need both eyes, our matrices, and the right moment in the scene pass); option 3 (parsing
  the user's .pc at runtime) would duplicate what is already resident. Our draw: once per stereo
  frame, when the game leaves the scene target after its opaque pass (the scene target is drawn
  in two sessions: ~300 opaque draws, a one-draw detour to a 1280x180 target, then transparent
  effects), depth-tested against the scene depth, each eye with its own matrices and viewport.
  Shading: diffuse texture, fixed view-space key light + ambient, specular map; no normal map.
- **Mesh poses differ per weapon** (measured from the meshes, units cm, y up): magnum, hand
  cannon, pump, auto shotgun, SMG and crossbow have the barrel along +x; the gold magnum
  (Gwendalin) and the minigun along +z; the assault rifle is toed in (barrel ~(0.975, -0.069,
  -0.212): about 12.5 degrees of yaw and 4 of pitch). Each weapon gets a description (barrel
  direction, muzzle point on the bore, grip point, up, magazine drop direction) and the shot ray
  is the barrel line, so the barrel points exactly where shots go (unit-tested in
  tests/test_stereo.cpp: points along the drawn barrel lie on the shot ray).
- **Magazines**: magnum and SMG magazines sit in the grip in model space; the auto shotgun's
  and rifle's are in front of the trigger; the hand cannon's "clip" is a speed loader floating
  behind the gun, the pump's a shell under the receiver, the crossbow's a small wedge (all three
  need the arm skeleton to place: hidden), the minigun's is its barrel cluster (drawn fixed).
  Reload state: cWeapon+0x764 reloading byte, +0x76C progress 0..1 (magnum: ~1.2 s).
- **Muzzle effects**: none at the camera. Comparing a shot frame with a later one, a shot adds
  only world-space sprites at the hit (an orange spark and blood, drawn with the scene's
  perspective: already right in stereo) - there is no first-person muzzle flash in this PC build
  (it would have belonged to the never-drawn GunArms rig). The screen-space sprites seen in some
  frames are the "player hurt" blood on the screen. Nothing to move or hide.
- **Composite target at other resolutions**: with a 1920x1200 back buffer (console session) the
  game's composite target is 1680x1050 - 16:10 but not as tall as the back buffer - so milestone
  1's rule missed it and the HUD stayed in the left eye. kind_of() now also takes a 16:10 colour or
  depth target narrower than the back buffer. Checked: HUD panel and eyes handed off at 3360x1050.
- **Sim checks (2026-11-17)**: magnum and SMG (bought in the test save's gun shop) drawn in both
  eyes, textured, from aim/back/right/left/top/bottom/front views; the barrel line ends on the
  marker in both eyes for both; magazines drop and return on reload; switching weapons switches
  the model; quitting a level releases the models (`gun model: level ended, 2 weapon model(s)
  released`); mode=off draws nothing. Other weapons were measured from their meshes only.
- **The game does draw a first-person reload animation** (correction to "never drawn, in reload
  or otherwise"): in Naked Terror (player 1 = Varla, packs hud_armsv + hud_armsc loaded) an SMG
  rises at the lower right of the game camera during each SMG reload and is gone afterwards. It is
  drawn by the engine, not the mod: it stays with `vr gunmodel 0`; a reload frame adds two
  skinned draws (vs e4dfaaee7f77629f, cBoneMatrix; 76-byte vertices, ~2484 verts, two materials)
  through the actor draw path 0x7697A0 (caller 0x523FDD), not our 52-byte GunArms_SMG buffer.
  Most likely the arms rig's reload animation (GunArms_Varla_Mesh with its `_Reload` clips; not
  verified which pack). Not seen for the magnum in Papa's Palace. In VR it is a second gun at the
  head during reloads: a candidate to hide in milestone 4 / the hands version.
