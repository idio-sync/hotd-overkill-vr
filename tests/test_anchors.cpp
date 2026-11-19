// Unit tests for the exe check (src/vr/exe_anchors): the generated anchor table is consistent,
// the check accepts a copy whose anchors match and names the anchor that doesn't (on a synthetic
// exe, so CI needs no game), and - only where the game exe exists (HOTD_EXE, else the test copy
// C:\Users\Virtual\hotd-vr-game\HOTD_NG.exe) - the table matches the real file and a 4 GB patch,
// a changed padding byte and a changed hook prologue get the expected verdicts.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "../src/vr/exe_anchors.h"

using namespace hv::vr;

static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static bool contains_le32(const Anchor& a, uint32_t v) {
    for (int i = 0; i + 4 <= a.len; ++i)
        if (memcmp(a.bytes + i, &v, 4) == 0) return true;
    return false;
}

static void test_table_consistent() {
    const Anchor* t = anchors();
    const size_t n = anchor_count();
    CHECK(n >= 30);
    std::set<std::string> names;
    std::vector<const Anchor*> sorted;
    for (size_t i = 0; i < n; ++i) {
        const Anchor& a = t[i];
        CHECK(a.name && a.name[0]);
        CHECK(names.insert(a.name).second);  // unique names: the log names a failing anchor
        CHECK(a.len >= 1 && a.len <= 16);
        // In the image (.text 0x401000 .. the end of .rdata), never in SecuROM's runtime-filled
        // jump table (0xA71000..0xA71844) or the writable .data sections.
        CHECK(a.va >= 0x401000 && a.va + a.len <= 0x966000);
        CHECK(a.va + a.len <= 0xA71000 || a.va >= 0xA71844);
        switch (a.kind) {
            case AnchorKind::detour:
                CHECK(a.len >= 5);  // a detour overwrites 5 bytes (jmp rel32)
                CHECK(a.target == a.va);
                CHECK(a.va < 0x7D1000);  // code
                break;
            case AnchorKind::call:
                CHECK(a.target == a.va);
                CHECK(a.va < 0x7D1000);
                break;
            case AnchorKind::retaddr:
                CHECK(a.len == 5 && a.bytes[0] == 0xE8 && a.va + 5 == a.target);  // call rel32
                break;
            case AnchorKind::global:
                CHECK(a.va < 0x7D1000);
                CHECK(a.target >= 0x968000);  // a global in .data
                CHECK(contains_le32(a, a.target));
                break;
            case AnchorKind::vtable:
                // a constructor's store of the vtable, or the vtable's own bytes in .rdata
                CHECK(a.va < 0x7D1000 ? contains_le32(a, a.target) : (a.target >= a.va - 4 && a.target < a.va + a.len));
                break;
        }
        sorted.push_back(&a);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Anchor* a, const Anchor* b) { return a->va < b->va; });
    for (size_t i = 1; i < sorted.size(); ++i) CHECK(sorted[i - 1]->va + sorted[i - 1]->len <= sorted[i]->va);  // no overlaps

    // A FireBullets detour must have its return-address anchor pointing at it.
    bool fire_call = false;
    for (size_t i = 0; i < n; ++i)
        if (t[i].kind == AnchorKind::retaddr) {
            int32_t rel;
            memcpy(&rel, t[i].bytes + 1, 4);
            const uint32_t callee = uint32_t(int64_t(t[i].va) + 5 + rel);
            for (size_t j = 0; j < n; ++j) fire_call |= t[j].kind == AnchorKind::detour && t[j].va == callee;
        }
    CHECK(fire_call);

    // Regions: contiguous headers + sections, sizes sane.
    const KnownRegion* r = known_regions();
    CHECK(known_region_count() >= 3);
    CHECK(std::string(r[0].label) == "PE headers" && r[0].offset == 0);
    for (size_t i = 0; i < known_region_count(); ++i) CHECK(uint64_t(r[i].offset) + r[i].size <= known_exe_size());
}

// A minimal PE whose one section maps image VA 0x401000..0x970000 to file offset 0x400, with every
// anchor's bytes in place (everything else zero).
static std::vector<uint8_t> synthetic_exe() {
    const uint32_t sec_va = 0x1000, sec_size = 0x56F000, raw = 0x400;
    std::vector<uint8_t> d(raw + sec_size);
    auto w16 = [&](size_t o, uint16_t v) { memcpy(&d[o], &v, 2); };
    auto w32 = [&](size_t o, uint32_t v) { memcpy(&d[o], &v, 4); };
    d[0] = 'M', d[1] = 'Z';
    w32(0x3C, 0x80);
    memcpy(&d[0x80], "PE\0\0", 4);
    w16(0x84, 0x14C);   // i386
    w16(0x86, 1);       // one section
    w32(0x88, 0x12345678);
    w16(0x94, 0xE0);    // optional header size
    w16(0x96, 0x102);
    const size_t opt = 0x98;
    w16(opt, 0x10B);
    w32(opt + 28, 0x400000);  // image base
    w32(opt + 60, raw);       // size of headers
    const size_t s = opt + 0xE0;
    memcpy(&d[s], ".all", 4);
    w32(s + 8, sec_size);
    w32(s + 12, sec_va);
    w32(s + 16, sec_size);
    w32(s + 20, raw);
    for (size_t i = 0; i < anchor_count(); ++i) {
        const Anchor& a = anchors()[i];
        memcpy(&d[raw + (a.va - 0x400000 - sec_va)], a.bytes, a.len);
    }
    return d;
}

static size_t synthetic_offset(uint32_t va) { return 0x400 + (va - 0x401000); }

static const AnchorFailure* failure_named(const ExeCheck& c, const char* name) {
    for (const AnchorFailure& f : c.failures)
        if (strcmp(f.anchor->name, name) == 0) return &f;
    return nullptr;
}

static void test_synthetic_exe() {
    std::vector<uint8_t> d = synthetic_exe();
    ExeCheck c = check_exe(d.data(), d.size());
    CHECK(c.pe && !c.known);
    CHECK(c.problems.empty() && c.failures.empty());
    CHECK(c.compatible());
    CHECK(!c.changed.empty() && c.changed[0] == "section layout");

    // One byte changed inside a detour's prologue: refused, named, with the bytes found.
    const Anchor* fire = nullptr;
    for (size_t i = 0; i < anchor_count(); ++i)
        if (strcmp(anchors()[i].name, "FireBullets") == 0) fire = &anchors()[i];
    CHECK(fire != nullptr);
    if (fire) {
        std::vector<uint8_t> e = d;
        e[synthetic_offset(fire->va) + 3] ^= 0xFF;
        c = check_exe(e.data(), e.size());
        CHECK(!c.compatible());
        CHECK(c.failures.size() == 1);
        const AnchorFailure* f = failure_named(c, "FireBullets");
        CHECK(f && f->in_file && f->found[3] == (fire->bytes[3] ^ 0xFF));
        CHECK(hex_bytes(fire->bytes, 3) == "55 8b ec");
    }

    // An instruction that references a global moved to another address: refused.
    for (size_t i = 0; i < anchor_count(); ++i) {
        const Anchor& a = anchors()[i];
        if (a.kind != AnchorKind::global) continue;
        std::vector<uint8_t> e = d;
        for (int k = 0; k + 4 <= a.len; ++k)
            if (memcmp(a.bytes + k, &a.target, 4) == 0) {
                const uint32_t moved = a.target + 0x10;
                memcpy(&e[synthetic_offset(a.va) + k], &moved, 4);
                break;
            }
        c = check_exe(e.data(), e.size());
        CHECK(!c.compatible() && failure_named(c, a.name));
        break;
    }

    // Bytes outside the anchors changed: still compatible.
    {
        std::vector<uint8_t> e = d;
        e[synthetic_offset(0x401000) + 0x10] = 0x90;
        c = check_exe(e.data(), e.size());
        CHECK(c.compatible() && c.failures.empty());
    }

    // Truncated: anchors past the end are reported as not in the file.
    {
        std::vector<uint8_t> e(d.begin(), d.begin() + synthetic_offset(0x500000));
        c = check_exe(e.data(), e.size());
        CHECK(!c.compatible());
        bool missing = false;
        for (const AnchorFailure& f : c.failures) missing |= !f.in_file;
        CHECK(missing);
    }

    // Another image base: refused (the mod's addresses assume 0x400000).
    {
        std::vector<uint8_t> e = d;
        const uint32_t base = 0x10000000;
        memcpy(&e[0x98 + 28], &base, 4);
        c = check_exe(e.data(), e.size());
        CHECK(!c.compatible() && c.problems.size() == 1);
    }

    // Not a PE at all.
    {
        const uint8_t junk[64] = {'Z', 'M'};
        c = check_exe(junk, sizeof junk);
        CHECK(!c.pe && !c.compatible() && !c.problems.empty());
    }
}

static void test_real_exe() {
    const char* env = std::getenv("HOTD_EXE");
    const std::string path = env && *env ? env : "C:\\Users\\Virtual\\hotd-vr-game\\HOTD_NG.exe";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("real exe test skipped: no game exe at %s (set HOTD_EXE)\n", path.c_str());
        return;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ExeCheck c = check_exe(d.data(), d.size());
    if (!c.known) {
        std::printf("real exe test skipped: %s is not the known build (fnv64 %016llx)\n", path.c_str(),
                    static_cast<unsigned long long>(c.fnv64));
        return;
    }
    CHECK(c.failures.empty() && c.problems.empty() && c.changed.empty());  // the table matches the file
    CHECK(c.timestamp == known_exe_timestamp());

    // 4 GB patch: the LAA flag and the PE checksum, nothing else -> compatible, headers changed.
    {
        std::vector<uint8_t> e = d;
        uint32_t pe;
        memcpy(&pe, &e[0x3C], 4);
        e[pe + 22] |= 0x20;
        e[pe + 24 + 64] ^= 0x5A;  // the checksum
        ExeCheck p = check_exe(e.data(), e.size());
        CHECK(!p.known && p.compatible());
        CHECK(p.changed.size() == 1 && p.changed[0] == "PE headers");
    }
    // An int3 padding byte between functions (never executed), away from every anchor.
    {
        std::vector<uint8_t> e = d;
        size_t pad = 0;
        for (size_t o = 0x2000; o + 8 < 0x3D0000 && !pad; ++o)
            if (memcmp(&e[o], "\xC3\xCC\xCC\xCC\xCC", 5) == 0) pad = o + 2;  // ret; int3 x4
        CHECK(pad != 0);
        e[pad] = 0x90;
        ExeCheck p = check_exe(e.data(), e.size());
        CHECK(!p.known && p.compatible());
        CHECK(p.changed.size() == 1 && p.changed[0] == ".text (0x401000)");
    }
    // Every anchor: one byte changed -> refused, that anchor named.
    for (size_t i = 0; i < anchor_count(); ++i) {
        const Anchor& a = anchors()[i];
        // its file offset: a .text/.rdata VA maps to raw = VA - 0x400000 - section VA + section raw
        size_t off = 0;
        if (a.va < 0x7D1000) off = a.va - 0x401000 + 0x1000;
        else off = a.va - 0x7D1000 + 0x3D0800;
        CHECK(memcmp(&d[off], a.bytes, a.len) == 0);
        std::vector<uint8_t> e = d;
        e[off + a.len - 1] ^= 0x01;
        ExeCheck p = check_exe(e.data(), e.size());
        CHECK(!p.compatible() && p.failures.size() == 1 && p.failures[0].anchor == &a);
    }
    std::printf("real exe test: %s is the known build; %zu anchors match, patched variants judged as expected\n",
                path.c_str(), anchor_count());
}

int main() {
    test_table_consistent();
    test_synthetic_exe();
    test_real_exe();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all exe anchor tests passed (%zu anchors)\n", anchor_count());
    return 0;
}
