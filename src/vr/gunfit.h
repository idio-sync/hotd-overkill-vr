#pragma once
// The bore of a gun mesh fitted from its vertices (the gun accuracy test's mesh check, `vr
// gunmeshcheck`), free of D3D so it can be unit-tested (tests/test_stereo.cpp). It is an
// independent check of the per-weapon description in gunmodel.cpp kDefs (measured offline with
// tools/gun_mesh.py): the description's barrel direction only picks which end of the mesh to look
// at and the plane the rings are cut in.
//
// Method. A gun barrel near its muzzle is a turned part: its vertices lie on circles (the outer
// barrel, the bore, chamfers) around one axis, in rings across that axis. Everything else there
// (a front sight, a rib, a slide's flat sides) is not round.
//   1. The vertices (duplicates merged) are projected on the hint direction; the slab within
//      `slab` model units of the front-most vertex is kept.
//   2. Its vertices are grouped into rings by their position along the hint (a new ring starts
//      after a gap of more than 0.3 units, or 0.6 from the ring's first point).
//   3. In each ring, circles are found across the hint: RANSAC over point triples for the circle
//      with the most points on it (within 0.02), then every circle concentric with it (bore and
//      barrel; points grouped by their distance from the centre, 6+ points going most of the way
//      round: no gap over 75 degrees - a box section's 4 corners are on a circle too), then a
//      geometric fit with one radius per circle
//      (Gauss-Newton). It counts if its points fit within 0.05 units RMS (0.1 max) and its radii
//      are 0.2-6 units. Its points are taken out and the rest searched again (a part on another
//      axis in the same ring, e.g. a barrel nut 0.1 off the barrel's axis).
//   4. Only circle sets within 3 units of the described barrel line are kept (given a hint point:
//      a pump's magazine tube or a crossbow's limb pins are round too). The front ring is the
//      front-most of them (the innermost of several there: the bore). Lines through its centre and
//      each ring centre at least 1 unit behind it, within 5 degrees of the hint, are tried; the one
//      with the most ring centres within 0.03 of it wins (of equals, the one with the smallest
//      radii: the bore or barrel, not a shroud around it). The bore axis is the least-squares line
//      through those centres (weighted by point count) - method "rings".
//   5. With only the front ring (a muzzle face on a flat-sided slide), the axis goes through its
//      centre along the dominant direction of vertex pairs within 5 degrees of the hint: modelled
//      guns are full of edges exactly parallel to the barrel - method "edges".
//   6. The muzzle is the axis point level with the front-most vertex within the largest radius
//      (+0.3) of the axis.
// The fit is repeated with its own axis as the hint until it settles (rings close together along
// a tilted axis are then concentric across it).
#include <cstddef>

namespace hv::vr {

struct BoreFit {
    struct RingInfo {
        float t = 0;            // position along the hint
        float centre[3] = {};   // model units
        float rmin = 0, rmax = 0;
        int points = 0;
        bool used = false;      // on the chosen axis
    };
    bool axis_ok = false;       // an axis was found (method rings or edges)
    bool muzzle_ok = false;     // a muzzle point was found (along the hint if there is no axis)
    const char* method = "none";  // "rings", "edges", "none"
    int rings = 0;              // ring centres on the axis
    int rings_round = 0;        // circle sets found in all
    int slab_points = 0;        // distinct vertices in the slab
    int edge_support = 0;       // method edges: vertex pairs in the winning direction bin
    float dir[3] = {};          // the bore axis (unit; the hint's sense)
    float muzzle[3] = {};       // the axis point at the muzzle end (model units)
    float radius = 0;           // the largest circle radius of the rings used
    float rms = 0;              // the worst ring's RMS circle residual
    float span = 0;             // distance between the first and last ring centres along the axis
    const char* why = "";       // a note on a fallback or failure
    RingInfo info[12];          // the circle sets found (for the log)
    int n_info = 0;
};

// `pos`: n points, each `stride_floats` floats apart (x, y, z first). `hint`: the approximate
// barrel direction (need not be unit length). `hint_point` (optional): a point near the barrel's
// axis (the description's muzzle); only round parts within 3 units of the line through it along
// the hint are considered - which of the mesh's round parts is the bore, not where it is exactly.
BoreFit fit_bore(const float* pos, size_t n, size_t stride_floats, const float hint[3], const float* hint_point = nullptr,
                 float slab = 15.f);

// The description against the fit: the angle between the barrel directions (degrees; -1 without
// an axis), and the description's muzzle point relative to the fitted muzzle: across the axis
// (`perp`) and along it (`along`, + = ahead of the fitted muzzle).
void compare_bore(const BoreFit& f, const float desc_dir[3], const float desc_muzzle[3], float* angle_deg, float* perp, float* along);

}  // namespace hv::vr
