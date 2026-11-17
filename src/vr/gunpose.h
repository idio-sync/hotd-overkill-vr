#pragma once
// Placing the game's first-person gun mesh in the hand (milestone 3), free of D3D/OpenVR so it can
// be unit-tested (tests/test_stereo.cpp).
//
// Spaces (all left-handed like the game: x right, y up, z forward):
//   model  - the mesh's own coordinates, game units (centimetres). Each weapon's mesh is posed
//            differently (most with the barrel along +x, a few along +z, the assault rifle toed in
//            by ~13 degrees), so each one has a description: its barrel direction, a point on the
//            barrel axis at the muzzle, the point the hand holds, and an approximate up direction.
//   gun    - metres; the barrel is the +z axis, the muzzle at the origin, up is +y. The barrel's
//            line in this frame is exactly (0, 0, t): whatever the mesh's own pose, the barrel
//            points along the gun frame's forward axis.
//   hand   - the gun hand's pose in the game camera's space (vrsys_hand_in_game: metres, +z the
//            aiming direction). The gun frame sits in the hand at a fixed offset/rotation.
//
// The shot ray is the barrel line (origin at the muzzle, direction along the barrel), so the
// barrel always points exactly where the shots go: with no hold rotation the barrel is parallel
// to the controller's pointing axis (the milestone 2 aim, moved onto the barrel).
#include "aim.h"
#include "stereo.h"

namespace hv::vr {

struct GunModelDesc {
    float barrel[3];  // barrel direction (model space; need not be unit length)
    float up[3];      // approximate up direction (model space; made orthogonal to the barrel)
    float muzzle[3];  // a point on the barrel axis at the muzzle (model units)
    float grip[3];    // the point the hand holds (model units)
    float clip_dir[3];  // the direction the magazine drops out (model space)
};

// Model space (model units) -> gun frame (metres): rotation taking `barrel` to +z and `up` to +y
// (as near as orthogonality allows), the muzzle to the origin, scaled by metres_per_unit.
Mat4 model_to_gun(const GunModelDesc& d, float metres_per_unit);

// The gun frame's pose in the hand frame: the model's grip point lands on `grip_in_hand` (metres,
// hand frame, left-handed), the gun turned by hold_yaw/hold_pitch (degrees; yaw > 0 turns the
// barrel right, pitch > 0 up) about that grip point.
Mat4 gun_in_hand(const GunModelDesc& d, float metres_per_unit, const float grip_in_hand[3], float hold_yaw_deg, float hold_pitch_deg);

// The barrel ray (origin = muzzle, direction = barrel) of a gun frame pose `gun` (in any space).
Ray barrel_ray(const Mat4& gun);

// World matrix of the mesh for drawing: world <- game camera space (the inverse of the engine's
// view matrix `view`, game units) <- metres to game units <- hand <- gun <- model. False if the
// view can't be inverted.
bool gun_world_matrix(const Mat4& view, float unit_m, const Mat4& hand_lh, const Mat4& gun_in_hand_m, const Mat4& model_to_gun_m,
                      Mat4* out);

// Magazine reload animation: how far out (0 = seated, 1 = fully out) the magazine is at reload
// progress p (0..1, cWeapon+0x76C) while reloading. It drops out over the first 20%, stays out,
// and is back in by 85% (so it is seated before the reload ends).
float clip_out_fraction(bool reloading, float progress);

}  // namespace hv::vr
