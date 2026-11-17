# HOTD_NG.exe static findings (2026-11-17)

Static analysis of `HOTD_NG.exe` (Steam build 569304, RC14; x86, MSVC 2010, D3D9, BlitzTech).
VAs assume image base 0x400000 (the exe is ASLR'd: RVA = VA − 0x400000). "Guessed" marks
anything not confirmed by disassembly or by the probe.

## Shot resolution — world-space ray cast (confirmed; hook verified in the probe, see findings-probe.md)
No screen-space pick, no render readback.
- **Cursor**: `cPlayer+0x1BC` (x), `+0x1C0` (y) floats, render-target pixels from screen centre, +y up.
  `SetCursorX` 0x5295F0 / `SetCursorY` 0x51FBD0 clamp to ±half of the size words at 0x9C3670/72
  (full size 0x9C366C/6E). Input-driven writer 0x534AB0 (four control modes by `player+0x200`);
  mouse pixels via 0x67E1D0 (channels 8/9).
- **Unproject**: 0x655620 `ScreenToWorld(x, y, out*)` — viewport 0x9C7F58/5C, near plane 0x9C7F18,
  inverse projection, inverse view → world point on the near plane.
- **Per-frame aim**: `cPlayer` update 0x43A100: origin `player+0x1E0` = camera position
  (`[engineCam+0xDC]+4`) + offset `player+0x1D4`; direction `player+0x1F0` =
  normalize(unproject(cursor) − origin). Weapon re-aims each frame in 0x487D80 / 0x562BD0 →
  `cWeapon+0x7B4` (vec4 direction); owner player at `cWeapon+0x794` (ctor 0x50A0A0).
- **Fire**: 0x52B090 → 0x50D110 `cWeapon::FireBullets(origin*, mode, travelled)` (thiscall, `ret 0xC`,
  9-byte prologue `55 8B EC 81 EC 74 03 00 00`). Scales `+0x7B4` to the weapon range, spread for
  multi-pellet weapons (0x42C780), world ray cast 0x5514E0(0, origin, dirSeg, 0, result) via
  `[cGameControl+0x68]` / 0x737CA0. Weapon type 5 goes through 0x4E1330 instead.
- Also cursor-driven: sphere-cast pickup/target checks 0x42B900 (r 80), 0x479250 (r 160).
- **Hook**: entry of 0x50D110 (overwrite `[ecx+0x7B4..0x7C0]` and `*origin`); also 0x4E1330 and
  the two sphere checks. 0x49E6C0 is an existing "fire in this direction" helper.

## Camera
- `cCameraManager` singleton pointer at **[0x99B8F4]** (ctor 0x4D8C60, size 0x144). Active `cCamera`:
  0x525C30 returns the list entry whose `+0x28` == `[[0x99B708]+0x64]` (0x99B708 = `cGameControl`).
- Engine camera node (vtable 0x80F748, ctor 0x6F4CE0): +0x0C position, +0x1C quaternion,
  +0x3C/+0x40 effective FOV X/Y in degrees (default 70; Y = 0 → from aspect), +0x68/+0x6C base FOV,
  +0x8C/+0x90 near/far (50/50000 → units probably cm, guessed), +0x94 flags, +0xDC transform
  (position +4, quaternion +0x14).
- `cCamera`: FOV copy +0x1B4/+0x1B0, aspect 4:3 at +0x1B8; update 0x4DCA50.
- Per-frame build: 0x6DBF50 (vtable slot 8) → 0x654B00 view = inverse(camWorld) → 0x9C7E48, camera
  position → 0x9C7E88. 0x66A8E0 FOV (radians; globals 0x9C7F20/24, sin/cos/tan 0x9C7F28–44) →
  0x663420 → 0x655460 LH perspective → 0x9C7DC0. 0x64DBD0 viewProj → 0x9C7D40 (transposed
  0x9C7D80). 0x6543E0 per-draw WVP.
- **Built-in stereoscopy path**: `stereoscopy*` config keys; when `[0x9F09E0]+0x3F8` is set, 0x6DBF50
  multiplies the camera by the eye matrix at `[0x9F09E0]+0x470` and adds a projection offset
  (0x9C822C/30). Natural per-eye hook (layout guessed).
- **WiiView edge-pan**: enable byte `[[0x99B8F4]+0x134]`, checked by 0x55F5C0; pan rate 0x4B6DB0,
  summed into manager +0x2C/+0x30 by 0x40D270. Zero it by forcing 0x55F5C0 → 0 or clearing +0x134.
  Where the sum is applied to the camera: not found.

## Frame timing (confirmed)
- Variable dt: 0x6E7200 writes dt = 1/max(fps, minfps) to 0x9F0B60; game time 0x9F0B50; dt×60 factor
  0x9F0A44; slow-motion scale from app +0x54/+0x58.
- Frame cap in the main loop 0x6FDEA0: `Sleep(0)` busy-wait until 1000/rate − 1 ms; rates {60, 30, 20,
  …} at app +0x24 (app = [0x9F09E0]). On unless option `disable-fps-limit` (0x4B4FAD: app+0x48 =
  !option).
- `VSyncThread` 0x655DF0 sleeps 16 ms and bumps an unread counter; not a cap.

## Culling (confirmed)
0x6DEDE0 builds frustum planes from the half-FOV sin/cos globals → a wider VR view needs a wider
FOV before culling.

## Copy protection
- totdo.dll = UPX-packed SecuROM PA. ~20 KB of .text zeroed and replaced by 532 `jmp [0xA71000..0xA71844]`
  stubs filled by totdo at runtime (e.g. `cGameModeRunLevel` vtable slot 5 0x4E63F0, init call 0x703473).
  None of the hook targets above are in or near them.
- The exe imports no VirtualProtect/VirtualQuery/module enumeration; a CRC would have to live in totdo
  (which imports VirtualProtect). Inline hooks: test empirically (see findings-probe.md).

## Tools
`tools/hotd.py` (image/disasm/xref/RTTI library), `strings_xref.py`, `hdis.py <va|rRVA> [count] [--func]`,
`fulldis.py` (→ out/text.asm), `funcgrep.py`, `securom_scan.py`, `totdo_peek.py`.
