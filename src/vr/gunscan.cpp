#include "gunscan.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace hv::vr {

PtrVerdict check_ptr(uint32_t p, uint32_t max_app, bool aligned) {
    if (!p) return PtrVerdict::null;
    if (p < kMinUserAddress) return PtrVerdict::low;
    if (p > max_app) return PtrVerdict::high;
    if (aligned && (p & 3)) return PtrVerdict::misaligned;
    return PtrVerdict::ok;
}

const char* ptr_verdict_name(PtrVerdict v) {
    switch (v) {
        case PtrVerdict::ok: return "ok";
        case PtrVerdict::null: return "null";
        case PtrVerdict::low: return "low";
        case PtrVerdict::high: return "high";
        case PtrVerdict::misaligned: return "misaligned";
    }
    return "?";
}

static bool iprefix(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix)
        if (std::tolower(static_cast<unsigned char>(*s)) != std::tolower(static_cast<unsigned char>(*prefix))) return false;
    return true;
}

ActorKind gunarms_kind(const char* name) {
    if (!name || !iprefix(name, "GunArms_")) return ActorKind::other;
    const size_t n = std::strlen(name);
    return n > 13 && iprefix(name + n - 5, "_Clip") ? ActorKind::clip : ActorKind::gun;
}

const char* slot_verdict_name(SlotVerdict v) {
    switch (v) {
        case SlotVerdict::null: return "null";
        case SlotVerdict::rejected: return "rejected";
        case SlotVerdict::self: return "the weapon itself";
        case SlotVerdict::no_instance: return "no actor instance";
        case SlotVerdict::no_name: return "actor without a readable name";
        case SlotVerdict::other_actor: return "actor";
        case SlotVerdict::mesh_failed: return "mesh not resolved";
        case SlotVerdict::duplicate: return "second mesh";
        case SlotVerdict::gun: return "gun";
        case SlotVerdict::clip: return "magazine";
    }
    return "?";
}

static std::string slot_token(const ScanSlot& s) {
    char buf[128];
    switch (s.verdict) {
        case SlotVerdict::rejected:
            snprintf(buf, sizeof buf, "+%02X %08X %s", unsigned(s.off), unsigned(s.value), ptr_verdict_name(s.ptr));
            break;
        case SlotVerdict::self:
        case SlotVerdict::no_instance:
        case SlotVerdict::no_name:
            snprintf(buf, sizeof buf, "+%02X %08X %s", unsigned(s.off), unsigned(s.value), slot_verdict_name(s.verdict));
            break;
        default:
            snprintf(buf, sizeof buf, "+%02X %08X %s \"%.40s\"", unsigned(s.off), unsigned(s.value), slot_verdict_name(s.verdict), s.name.c_str());
            break;
    }
    return buf;
}

std::vector<std::string> describe_scan(const std::vector<ScanSlot>& slots, uint32_t max_app, int per_line, int max_lines) {
    if (per_line < 1) per_line = 1;
    if (max_lines < 1) max_lines = 1;
    int n[10] = {};
    int high = 0, low = 0, misaligned = 0;
    std::vector<const ScanSlot*> shown;
    for (const ScanSlot& s : slots) {
        ++n[int(s.verdict)];
        if (s.verdict == SlotVerdict::rejected) {
            if (s.ptr == PtrVerdict::high) ++high;
            else if (s.ptr == PtrVerdict::low) ++low;
            else if (s.ptr == PtrVerdict::misaligned) ++misaligned;
        }
        if (s.verdict != SlotVerdict::null) shown.push_back(&s);
    }
    std::vector<std::string> out;
    char head[384];
    snprintf(head, sizeof head,
             "%zu slots read, pointer limit %08X: %d null, %d rejected (%d above the limit, %d below 0x10000, %d misaligned), "
             "%d without an actor instance, %d other actors, %d GunArms meshes not resolved, %d gun, %d magazine",
             slots.size(), unsigned(max_app), n[int(SlotVerdict::null)], n[int(SlotVerdict::rejected)], high, low, misaligned,
             n[int(SlotVerdict::no_instance)], n[int(SlotVerdict::other_actor)] + n[int(SlotVerdict::no_name)],
             n[int(SlotVerdict::mesh_failed)], n[int(SlotVerdict::gun)], n[int(SlotVerdict::clip)]);
    out.push_back(head);
    size_t i = 0;
    while (i < shown.size() && int(out.size()) < max_lines) {
        std::string line;
        const bool last_line = int(out.size()) == max_lines - 1;
        for (int k = 0; k < per_line && i < shown.size(); ++k, ++i) line += (line.empty() ? "" : "; ") + slot_token(*shown[i]);
        if (last_line && i < shown.size()) {
            char more[48];
            snprintf(more, sizeof more, "; ... %zu more", shown.size() - i);
            line += more;
            i = shown.size();
        }
        out.push_back(line);
    }
    return out;
}

std::string scan_hint(const std::vector<ScanSlot>& slots) {
    bool gunarms = false;
    for (const ScanSlot& s : slots) {
        if (s.verdict == SlotVerdict::gun) return "";
        gunarms = gunarms || s.verdict == SlotVerdict::clip || s.verdict == SlotVerdict::mesh_failed || s.verdict == SlotVerdict::duplicate;
    }
    if (gunarms)
        return "a GunArms actor is there, but not the gun's own mesh (or its mesh did not resolve: the reason is on its slot above)";
    return "no GunArms_* actor among the weapon's entities: the game has not loaded a first-person model for this weapon. The "
           "mini-games (Stayin' Alive, Victim Support, Money Shot II) load none, so they show no gun model; the story levels do";
}

}  // namespace hv::vr
