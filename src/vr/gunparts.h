#pragma once
// Moving parts of the game's first-person gun meshes (gun feedback stage 1, docs/DEVELOPMENT.md
// "Moving parts"), free of D3D so it can be unit-tested
// (tests/test_stereo.cpp).
//
// Each GunArms mesh is many closed pieces: connected components once positions within 0.01 cm are
// welded. They touch without sharing vertices, so a moving part is a set of whole pieces, picked by
// its bounding box in model space (never by piece order or vertex index: another level's pack could
// order them differently). Each vertex gets a part index and a weight; the mod's gun shader moves it
// by its part's matrix (model space, cm) before the model's world matrix, blended by the weight: 1
// for rigid parts, while the crossbow string bends (its weight runs from 0 at the cam to 1 at the
// nock, so each half stays a straight line from its cam to the moving nock). Part 0 is the rest.
//
// A weapon's `_Clip` mesh has its own parts (the hand cannon's cylinder, the minigun's barrel
// cluster), and the pose of the whole clip mesh on the gun (its rest offset, the cylinder swinging
// out, the pump's forend racking) is folded into its part matrices.
//
// Nothing here touches the aim: the shot ray is the un-moved barrel line (gunpose.h).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "stereo.h"

namespace hv::vr {

constexpr int kMaxParts = 6;  // per mesh, part 0 included (the gun shader's constant array)

// Gun mesh parts.
enum GunPart { kPartRest = 0, kPartSlide = 1, kPartHammer = 2, kPartTrigger = 3 };
// Clip mesh parts: 0 the clip's base (the hand cannon's crane, the pump's forend, the minigun's
// sight block), 1 what turns in it (the cylinder, the barrel cluster).
enum ClipPart { kClipBase = 0, kClipTurn = 1 };

// --- pieces ---

struct MeshPiece {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    int tris = 0, verts = 0;
};

// The piece index of each vertex (-1: used by no triangle), welding positions within `weld` model
// units. `pos`: n points `stride_floats` apart (x, y, z first); triangles with an index >= n are
// skipped.
std::vector<int> mesh_pieces(const float* pos, size_t n, size_t stride_floats, const uint32_t* idx, size_t nidx, float weld,
                             std::vector<MeshPiece>* pieces);

// Pieces to a part: every piece not taken by an earlier rule whose bounding box lies inside
// [lo, hi] (0.02 tolerance) with min_tris..max_tris triangles. The rule must take
// min_count..max_count pieces, or the mesh is not the one expected and nothing moves.
// ramp_axis >= 0: the weight of each vertex goes from 0 where that coordinate is ramp[0] to 1 where
// it is ramp[1] (clamped) - a bend instead of a rigid move.
struct PartRule {
    const char* name;
    int part;  // 0: stays (a check that the mesh is the expected one)
    float lo[3], hi[3];
    int min_tris, max_tris;
    int min_count, max_count;
    int ramp_axis = -1;
    float ramp[2] = {0, 0};
};

struct PartsResult {
    bool ok = false;
    std::vector<float> stream;  // 2 floats per vertex: part index, weight (0, 1 for every vertex unless ok)
    int pieces = 0;
    int vertices_moving = 0;    // vertices given a part other than 0
    std::string log;            // per rule: pieces taken (and, failing, which rule and why)
};
PartsResult assign_parts(const float* pos, size_t n, size_t stride_floats, const uint32_t* idx, size_t nidx, const PartRule* rules,
                         int nrules, float weld = 0.01f);
// The stream that leaves every vertex at rest (part 0, weight 1).
std::vector<float> rest_stream(size_t n);

// --- per weapon ---

struct WeaponParts {
    const char* key;                 // kDefs key (gunmodel.cpp)
    float barrel[3], up[3];          // model space, as kDefs: the motions are built from them
    const PartRule* gun = nullptr;   // the gun mesh's rules
    int n_gun = 0;
    const PartRule* clip = nullptr;  // the clip mesh's rules (clip_part weapons)
    int n_clip = 0;
    bool clip_part = false;          // the `_Clip` mesh belongs on the gun (cylinder, forend, barrels)
    float clip_rest[3] = {};         // its offset onto the gun (model units)
    // Part 1 (slide, knob, handle; the crossbow's string): `slide_travel` cm along `slide_dir` at a
    // kick's peak (the string: along +barrel when released).
    float slide_travel = 0;
    float slide_dir[3] = {};
    bool slide_is_string = false;    // part 1 follows GunMotion::string instead of kick
    bool slide_lock = false;         // empty after a shot: the slide stays back until the reload
    // Part 2 (hammer): turns `hammer_deg` top-back about `hammer_pivot` (axis barrel x up).
    float hammer_pivot[3] = {};
    float hammer_deg = 0;
    // Part 3 (trigger): turns `trigger_deg` bottom-back about `trigger_pivot` (axis up x barrel).
    float trigger_pivot[3] = {};
    float trigger_deg = 0;
    // Clip part 1 (cylinder, barrel cluster) turns about the barrel direction through `turn_point`
    // (clip space); `turn_sign` makes positive angles the intended direction.
    float turn_point[3] = {};
    float turn_sign = 1;
    int chambers = 0;                // hand cannon: chambers in the cylinder (a shot turns 360/chambers)
    // Hand cannon: the cylinder and crane swing out by `swing_deg` about the barrel direction
    // through `swing_point` (clip space); positive = `swing_sign`.
    float swing_point[3] = {};
    float swing_deg = 0;
    float swing_sign = 1;
    // Pump: the forend racks `rack_travel` cm along -barrel.
    float rack_travel = 0;
    // Timings (seconds).
    float kick_back = 0.025f, kick_home = 0.07f;
    float rack_start = 0.1f, rack_back = 0.12f, rack_hold = 0.03f, rack_fwd = 0.12f;
    float string_snap = 0.02f, string_draw = 0.6f;
};
const WeaponParts* weapon_parts(const char* key);  // null: no moving parts for this key
int weapon_parts_count();
const WeaponParts& weapon_parts_at(int i);

// What the parts do this frame (gunmodel.cpp works it out from the shots, ammo, reload, trigger).
struct GunMotion {
    float kick = 0;      // 0..1: slide / knob / handle back (the magnum's hammer follows it)
    float hammer = 0;    // 0..1: hammer back
    float trigger = 0;   // 0..1: the controller's trigger
    float cyl_deg = 0;   // hand cannon: the cylinder's turn (one chamber, 360/7 degrees, per shot)
    float swing = 0;     // 0..1: the cylinder swung out (reload)
    float rack = 0;      // 0..1: the pump's forend back
    float spin_deg = 0;  // minigun: the barrel cluster's angle
    float string = 0;    // crossbow: 1 = released (forward), 0 = drawn
};
struct GunPose {
    Mat4 gun[kMaxParts];   // gun mesh: part -> model space
    Mat4 clip[kMaxParts];  // clip mesh (clip_part weapons): part -> the gun's model space
};
// Null `w`: everything at rest (identity).
void pose_weapon(const WeaponParts* w, const GunMotion& m, GunPose* out);

// A rotation by `deg` about the line through `point` along `axis` (need not be unit length), by
// Rodrigues' formula: v -> v cos + (k x v) sin + k (k.v)(1 - cos), k = the unit axis. Positive
// angles turn a point at `v` from the axis towards `k x v` (componentwise cross product: the same
// numbers in either handedness).
Mat4 rotation_about(const float axis[3], const float point[3], float deg);

// --- curves (t = seconds since the event; anything before it or long after it is 0) ---

// A kick: 0 -> 1 in `back` (fast, easing out), 1 -> 0 in `home` (eased).
float kick_curve(float t, float back, float home);
// After a shot: hand cannon's cylinder turn 0..1 (once the hammer has fallen: 15 ms, then 80 ms).
float cylinder_turn(float t);
constexpr float kHammerFall = 0.015f;
// The pump's forend: waits `start`, back in `back`, stays `hold`, forward in `fwd`.
float rack_curve(float t, float start, float back, float hold, float fwd);
// The crossbow's string: forward (1) in `snap`, a moment there, then drawn back to 0 over `draw`.
float string_curve(float t, float snap, float draw);
// Minigun: one frame of the barrels' spin. `speed` (deg/s) eases towards kSpinSpeed while
// `spinning`, else down to 0; the angle advances by speed * dt but never more than kSpinMaxStep per
// frame, so 6 barrels (60-degree symmetry) can't look still or turning backwards at any frame rate.
constexpr float kSpinSpeed = 2400, kSpinUp = 0.5f, kSpinDown = 1.5f, kSpinMaxStep = 24;
void spin_step(bool spinning, float dt, float* speed, float* angle_deg);

}  // namespace hv::vr
