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
  (Gwendalin) and the minigun along +z; the assault rifle is toed in (bore (0.969, -0.111,
  -0.221): about 12.9 degrees of yaw and 6.4 of pitch). Each weapon gets a description (barrel
  direction, muzzle point on the bore, grip point, up, magazine drop direction) and the shot ray
  is the barrel line, so the barrel points exactly where shots go (unit-tested in
  tests/test_stereo.cpp: points along the drawn barrel lie on the shot ray).
- **Bore fit corrections (2026-11-20)**: the gun accuracy test's mesh check (`vr gunmeshcheck`,
  src/vr/gunfit.h) found the first descriptions off the real bore: the magnum's muzzle point 1.03 cm
  below it (23.4, 8.4, -0.87 vs the bore's 23.436, 9.433, -0.870), the gold magnum's 1.19 cm
  below (0, 5.3, 16.3 vs -0.007, 6.485, 16.328), the pump's 1.19 cm above (61.7, 12.0, -0.47 vs
  61.669, 10.807, -0.481), the auto shotgun's 0.40 cm below (73.3, 12.0, -0.75 vs 73.283, 12.400,
  -0.749), the hand cannon's 0.46 cm above (33.5, 9.6, -0.5 vs 33.495, 9.140, -0.519), and the
  rifle's barrel direction 2.47 degrees off its bore ((0.975, -0.069, -0.212) vs (0.968961,
  -0.110631, -0.221077), muzzle 52.9, 2.7, -11.05 vs 52.671, 2.870, -11.168): its shots went ~2.4
  degrees above the visible bore. The descriptions now use the fit's values. Each bore was also
  checked by circle fits at both ends of the barrel (magnum x 5.8 and 23.4, gold magnum z -4.2 and
  16.3, pump x 26 and 61.7, auto shotgun x 55.3 and 73.3, hand cannon x 12.7 and 33.4): the same
  centre to 0.005 cm, so the axes are exactly along +x/+z (the fit's own axis for the hand
  cannon is not found: its only ring within 15 cm of the muzzle is the muzzle face). Grip points
  are unchanged, so these guns sit in the hand as before; the rifle turns 2.47 degrees about its grip
  (its nose 2.4 up, 0.6 sideways, roll 0.04), which lines its bore up with the controller's pointing axis. Its magazine still
  drops along model -y (its long axis is (0.05, -0.99, -0.12)). SMG unchanged (fit within
  0.007 cm); crossbow and minigun have no bore to fit. In the game (fake runtime, Naked Terror):
  `vr gunmeshcheck` now gives 0.000 cm across the axis for all six (angle 0.000-0.034 deg), the
  still shots stay at 0.000 deg against the drawn barrel (rifle and shotguns: the spread-free
  ray), and release_check's aim marker sits 5.4 cm above the pointing axis (4.4 before: the
  magnum's bore is 1.03 cm higher than its old muzzle point).
- **Magazines**: magnum and SMG magazines sit in the grip in model space; the auto shotgun's
  and rifle's are in front of the trigger; the crossbow's is a small plain box (hidden); the
  minigun's is its barrel cluster. Reload state: cWeapon+0x764 reloading byte, +0x76C progress 0..1
  (magnum: ~1.2 s).
- **Correction (2026-11-20, gun feedback recon)**: the hand cannon's `_Clip` is not a speed loader
  but the revolver's **cylinder** (with crane and ejector rod), and the pump's is not a shell but the
  **forend** (a 19.5 cm sleeve): the gun meshes have neither, so both guns were drawn without them.
  Offsets (model cm) put them in place: the cylinder by (+10.30, +2.34, -0.81), in the frame's
  window with the top chamber on the bore (its axis in `_Clip` space: along +x through y 5.259, z
  0.288); the forend by (+32.5, +7.0, 0) on the magazine tube. The cylinder has **7 chambers**
  (holes centred at 90 + k 360/7 degrees about its axis), not 6. Each gun mesh is many closed
  pieces (connected components once positions within 0.01 cm are welded), so slides, hammers,
  triggers, knobs and handles can move as whole pieces (src/vr/gunparts.cpp has the boxes). In model
  space +z is the shooter's left for the +x-barrel guns (checked in captures: the cylinder swung to
  +z appears on the left from behind). Measured re-fire intervals (fake runtime, rapid trigger
  pulls): pump ~575 ms, auto shotgun ~375 ms, crossbow ~1.02 s, rifle bursts 100 ms apart, minigun
  ~25-50 ms; holding the trigger fires the semi-automatics once.
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

## No gun model: a player's report (v0.2.0, 2026-11-20)
- The report: no gun in the hand, only the red dot; `weapon 2E7B3EC0 (data 12EBFBA0) has no
  resident GunArms model`. Story levels put cWeapon and its data in one pool around 0x13xxxxxx
  here (Naked Terror: weapon 13F9FB80, data 13285780; Ballistic Trauma 1404B410 / 134E4AB0; Scream
  Train 13AF2E00 / 1307D5D0; the same addresses with the low 2 GB reserved at the loadout: the
  level's objects come from pools made at boot). The **mini-games** put the weapon around
  0x2E9xxxxx and its data around 0x12E/0x12F: Stayin' Alive 2E92ECC0 / 12E8F080 (then 2E933E30 /
  12E8F000), Victim Support 2E933E30 / 12F8BE20 - the report's pattern.
- **The mini-games load no weapon hud pack and no arms pack** (Stayin' Alive opens only
  `w_survivalmode_*`, `w_global*`, `w_civilians*`; the game folder has hud weapon packs only for
  the story levels l1..l9), so their weapon data has the hashes of the triples but no GunArms
  entities (`vr gunscan`: only a `Weapon_Grenade` actor). No model can be found; only a model the
  mod loads itself from a story level's `<level>_hud_<weapon>.pc` could be drawn there.
- **Large-address-aware exe**: with `probe.ini high_addresses=64` (the low 2 GB reserved at load)
  everything of the game's lands above 2 GB (player 87F2F0D0, weapon 88C8FB80, data 87F75780,
  entities 0x87Axxxxx); v0.2.0's `< 0x7FFF0000` checks then turned down the weapon pointer itself
  (nothing logged). Checked against GetSystemInfo's maximum address (0xFFFEFFFF here) the magnum is
  drawn as before.
- **Player 1 in Ballistic Trauma is Agent G** (black suit sleeves in the game's reload animation,
  `game_arms=1`); the level loads hud_armsg before hud_armsw, as Papa's Palace and Scream Train do
  (Naked Terror: armsv before armsc, Varla). The gun model was found in all of them: G as player 1
  makes no difference. Candi is player 2 only (Naked Terror). The levels after Naked Terror were
  reached by setting the profile's progress bytes `[[0x99B748]+0x228]+0x2D..0x2F` to 9 in memory
  (the "Unlock Everything" script function 0x424450 writes them), never in the save.

## Comfort and release (milestone 4, 2026-11-17, sim, 1920x1200 console session)
- **The reload animation's draws**: logging every entity the engine draws through 0x7697A0 (the
  entity draw: thiscall, one argument, ret 4; vtable slot 0xA4 of the drawable entity classes,
  called by the scene list walker 0x523FC0; E+0x48 = actor instance, I+0x8C = ACTR, [ACTR+0x6C] =
  name) during a reload shows three entities that are otherwise not drawn: a
  **cPlayerHUDWeaponDisplay** (vtable 0x800BCC, actor "Motion": the arms rig, the two skinned
  76-byte draws) and the weapon's own **GunArms_SMG / GunArms_SMG_Clip** (static-mesh entities,
  vtable 0x813768). Correction: the same happens for **Washington** with the magnum and the SMG in
  Papa's Palace (the earlier "not seen for the magnum" was wrong). Skipping every
  cPlayerHUDWeaponDisplay and every GunArms_* actor in that draw (`game_arms=0`) removes arms and
  gun in both cases; zombies and the level are unaffected (scene shots `m4_arms.png`,
  `m4_washington_off.png` in the test copy's hovr\). A first attempt keyed on cPlayer+0x25C hid a
  zombie instead: that pointer is the zombie currently attacking player 1, not the arms.
- **Camera shake / smack**: cCamera's update 0x4DCA50 sums angle offsets (radians, applied with
  0x6A7570 to the camera's orientation) from 0x4BF210 (shake: spring-damped, two slots of 10
  dwords at cCameraManager+0xA4 written by the CameraSetShakeSettings handler 0x560690) and
  0x529CA0 (smack: clamped by cCameraManager+0xF4.., handler 0x437410), plus the WiiView pan
  (manager +0x40/+0x44). Script handlers are in a table of {name hash, handler, ..., name} entries
  (e.g. 0x96D2A4 -> "CameraSetShakeSettings"). With a zombie hitting player 1 every ~2 s the shake
  output reached 0.51 rad (~29 degrees); allowing it live (`vr shake 1`) swung the camera's forward
  axis from (-0.08, 0.00, 1.00) to (-0.53, 0.16, 0.84) with ~5 degrees of roll. The smack output
  stayed 0 in everything seen. Zeroed by default (`camera_shake=0`).
- **WiiView edge-pan is live on PC**: cCameraManager+0x134 = 1 in gameplay and 0x55F5C0 returned 1
  on every call. With the sim gun pointed 70 degrees right (cursor clamped at the right edge),
  letting it through (`vr wiiview 1`) turned the camera from forward x -0.08 to 0.35 (~25 degrees)
  within 1.5 s; blocked (the VR default) the camera stayed put. Shots and the cursor are
  unaffected by the block.
- **Head-bob**: the moving bob (cCameraManager +0x8C up, +0x90 sideways, +0x94 wavelength, +0x98
  minimum speed; defaults 6.5, 7, 250, 10) - with the amplitudes restored the camera moved 1500
  units along Naked Terror's rail at a constant y (-135.00), so no bob was seen; the static bob
  values (+0x9C/+0xA0) have no reader. `head_bob=0` holds the amplitudes at 0 anyway.
- **Hit and death effects in stereo**: hits at health 9..1 and the fatal hit showed only the blood
  overlays (orthographic, per eye) in the scene target and in the mirror; no full-screen zoom or
  blur in stereo frames. The death screen (flat) has a red-tinted backdrop. Game FOV changes would
  be undone by the eye projections in stereo anyway. No `hit_zoom` setting.
- **World scale** (`vr measure`, the game's ray cast from the camera): Papa's Palace kitchen -
  floor 158 units below the camera, ceiling 124 above (room 2.82 m at 1 unit = 1 cm), a zombie's
  hit points up to 1.97 m above the floor (arm raised); Naked Terror basement - floor 165 below,
  ceiling 131 above (2.96 m), a zombie's top 1.72 m above the floor. So the camera is a ~1.6 m eye
  height above a floor at y ~78 (kitchen), rooms are ~2.8-3 m high and zombies human-sized:
  1 unit = 1 cm confirmed, `unit_m=0.01` kept. (The floor is not at y = 0, which settles milestone
  1's 1.6 vs 2.4 m question.)
- **Recentre**: logged at the level start (`vr: recentre (level start)`, also after quitting to
  the level select and starting Papa's Palace) and after each death + continue (`gameplay resumed
  after a flat screen`, twice). The pause menu has no restart; quitting drops the level flag, so
  the next start recentres as a level start.
- **Release vs development**: the development log's second harness line is a dry run of what a
  release install would do with the same settings ("release; hooks for VR; save the game's own
  folder; window the game's own; sound on; pad the real one; command file none"). No release run
  was made in the test copy (it would use the real save).
