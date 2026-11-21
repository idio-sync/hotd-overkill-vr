#pragma once
// The weapon model's pointer checks and the weapon data scan's verdicts (src/mod/gunmodel.cpp),
// kept free of Windows so they can be unit tested.
//
// Pointers read from game memory are checked against the process's real user address range: the
// top is GetSystemInfo's lpMaximumApplicationAddress, 0x7FFEFFFF for a plain 32-bit exe and
// 0xFFFEFFFF for a large-address-aware one (the "4 GB patch") on 64-bit Windows, where the game's
// heap, its packs and the driver's objects can sit above 2 GB once the low 2 GB fills up.
#include <cstdint>
#include <string>
#include <vector>

namespace hv::vr {

constexpr uint32_t kMinUserAddress = 0x10000;     // below: never a valid object
constexpr uint32_t kMaxUserAddress2G = 0x7FFEFFFF;  // a 32-bit process without large-address-awareness

enum class PtrVerdict : uint8_t { ok, null, low, high, misaligned };

// `p` as a pointer to an object in a process whose highest user address is `max_app`: non-null, at
// least kMinUserAddress, at most max_app, and 4-byte aligned when `aligned`.
PtrVerdict check_ptr(uint32_t p, uint32_t max_app, bool aligned);
inline bool ptr_ok(uint32_t p, uint32_t max_app, bool aligned) { return check_ptr(p, max_app, aligned) == PtrVerdict::ok; }
const char* ptr_verdict_name(PtrVerdict v);

// An actor name: a weapon's first-person mesh ("GunArms_<Weapon>"), its magazine
// ("GunArms_<Weapon>_Clip"), or something else. Case-insensitive, like the game's names.
enum class ActorKind : uint8_t { other, gun, clip };
ActorKind gunarms_kind(const char* name);

// One dword of the weapon data block (cWeapon+4, offsets 0x28..0xC0) and what the scan made of it.
enum class SlotVerdict : uint8_t {
    null,         // zero
    rejected,     // not a plausible pointer (ptr says why): the triples' hashes, or an object out of range
    self,         // the weapon object itself
    no_instance,  // a readable address, but no actor instance (vtable 0x80FB58) in it
    no_name,      // an actor instance whose ACTR name cannot be read
    other_actor,  // an actor that is not a GunArms_* mesh (name)
    mesh_failed,  // a GunArms_* actor whose mesh did not resolve (name)
    duplicate,    // a second gun or magazine mesh (name), skipped
    gun,          // the gun mesh (name)
    clip,         // the magazine mesh (name)
};
const char* slot_verdict_name(SlotVerdict v);

struct ScanSlot {
    uint32_t off = 0, value = 0;
    SlotVerdict verdict = SlotVerdict::null;
    PtrVerdict ptr = PtrVerdict::ok;  // rejected: why
    std::string name;                  // the actor's name, where there is one
};

// The scan as log lines, for when no model was found: the null slots counted, every other slot as
// "+OFF VALUE why" (rejected pointers: "high"/"low"/"misaligned"; actors: their name), `per_line`
// slots per line, at most `max_lines` lines in all (the first is the summary; what does not fit
// is counted on the last line).
std::vector<std::string> describe_scan(const std::vector<ScanSlot>& slots, uint32_t max_app, int per_line, int max_lines);

// What a failed scan most likely means, in one sentence for the log ("" if a gun mesh was found).
// No GunArms_* actor at all: the game did not load the weapon's first-person model, as in the
// mini-games, which load no weapon hud pack (seen 2026-11-20: Stayin' Alive, Victim Support).
std::string scan_hint(const std::vector<ScanSlot>& slots);

}  // namespace hv::vr
