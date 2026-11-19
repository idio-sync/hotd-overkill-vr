#include "exe_anchors.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "exe_anchors_table.h"

namespace hv::vr {

namespace {

constexpr uint32_t kImageBase = 0x400000;

struct Section {
    std::string name;
    uint32_t va, vsize, raw, rawsize;  // va: image VA (base 0x400000)
};

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

struct Pe {
    uint32_t image_base = 0, size_of_headers = 0;
    std::vector<Section> sections;
};

// Section headers of a PE file in memory; false (with a reason) if it isn't one.
bool parse_pe(const uint8_t* d, size_t n, ExeCheck* out, Pe* pe) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') {
        out->problems.push_back("not an exe (no MZ header)");
        return false;
    }
    const uint32_t h = rd32(d + 0x3C);
    if (size_t(h) + 24 > n || memcmp(d + h, "PE\0\0", 4) != 0) {
        out->problems.push_back("not a PE file");
        return false;
    }
    out->timestamp = rd32(d + h + 8);
    out->characteristics = rd16(d + h + 22);
    const uint16_t nsec = rd16(d + h + 6), opt_size = rd16(d + h + 20);
    const size_t opt = size_t(h) + 24;
    if (opt + 64 > n || opt + opt_size + 40ull * nsec > n) {
        out->problems.push_back("truncated PE headers");
        return false;
    }
    pe->image_base = rd32(d + opt + 28);
    pe->size_of_headers = rd32(d + opt + 60);
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = d + opt + opt_size + 40 * i;
        char name[9] = {};
        memcpy(name, s, 8);
        pe->sections.push_back({name, kImageBase + rd32(s + 12), rd32(s + 8), rd32(s + 20), rd32(s + 16)});
    }
    return true;
}

// File offset of n bytes at image VA va: they must lie in one section's raw data, inside its
// mapped size (tools/anchors.py Pe.file_offset is the same rule).
bool file_offset(const Pe& pe, size_t file_size, uint32_t va, size_t n, size_t* off) {
    for (const Section& s : pe.sections) {
        const uint64_t limit = s.vsize ? std::min(s.rawsize, s.vsize) : s.rawsize;
        if (s.va <= va && uint64_t(va) + n <= uint64_t(s.va) + limit) {
            *off = size_t(s.raw) + (va - s.va);
            return *off + n <= file_size;
        }
    }
    return false;
}

struct Region {
    std::string label;
    size_t offset, size;
};

// Headers, each section's raw data, and what follows the last section (the Authenticode signature).
std::vector<Region> regions(const Pe& pe, size_t n) {
    std::vector<Region> r;
    r.push_back({"PE headers", 0, std::min<size_t>(pe.size_of_headers, n)});
    size_t end = pe.size_of_headers;
    for (const Section& s : pe.sections) {
        char label[64];
        snprintf(label, sizeof label, "%s (0x%X)", s.name.c_str(), s.va);
        r.push_back({label, s.raw, s.rawsize});
        end = std::max(end, size_t(s.raw) + s.rawsize);
    }
    r.push_back({"signature/overlay", end, n > end ? n - end : 0});
    return r;
}

}  // namespace

const char* anchor_kind_name(AnchorKind k) {
    switch (k) {
        case AnchorKind::detour: return "detour";
        case AnchorKind::call: return "call";
        case AnchorKind::retaddr: return "return address";
        case AnchorKind::global: return "global";
        case AnchorKind::vtable: return "vtable";
    }
    return "?";
}

uint64_t fnv1a64(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex_bytes(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, i ? " %02x" : "%02x", p[i]);
        s += b;
    }
    return s;
}

ExeCheck check_exe(const uint8_t* data, size_t size) {
    ExeCheck c;
    c.fnv64 = fnv1a64(data, size);
    c.known = size == kKnownExeSize && c.fnv64 == kKnownExeFnv64;
    Pe pe;
    c.pe = parse_pe(data, size, &c, &pe);
    if (!c.pe) return c;
    if (pe.image_base != kImageBase) {
        char s[96];
        snprintf(s, sizeof s, "PE image base: expected %08X, found %08X", kImageBase, pe.image_base);
        c.problems.push_back(s);
    }
    for (const Anchor& a : kAnchors) {
        size_t off = 0;
        AnchorFailure f{&a, file_offset(pe, size, a.va, a.len, &off), {}};
        if (f.in_file) memcpy(f.found, data + off, a.len);
        if (!f.in_file || memcmp(f.found, a.bytes, a.len) != 0) c.failures.push_back(f);
    }
    if (c.known) return c;
    const std::vector<Region> r = regions(pe, size);
    bool same_layout = r.size() == std::size(kKnownRegions);
    for (size_t i = 0; same_layout && i < r.size(); ++i)
        same_layout = r[i].offset == kKnownRegions[i].offset && r[i].size == kKnownRegions[i].size;
    if (!same_layout) {
        c.changed.push_back("section layout");
    } else {
        for (size_t i = 0; i < r.size(); ++i)
            if (r[i].offset + r[i].size > size || fnv1a64(data + r[i].offset, r[i].size) != kKnownRegions[i].fnv64)
                c.changed.push_back(r[i].label);
    }
    return c;
}

size_t anchor_count() { return std::size(kAnchors); }
const Anchor* anchors() { return kAnchors; }
size_t known_region_count() { return std::size(kKnownRegions); }
const KnownRegion* known_regions() { return kKnownRegions; }
uint64_t known_exe_size() { return kKnownExeSize; }
uint32_t known_exe_timestamp() { return kKnownExeTimestamp; }

}  // namespace hv::vr
