#pragma once
// Which HOTD_NG.exe the mod can run on. The mod patches and reads fixed addresses of one build
// (Steam build 569304), so before any hook goes in, the exe on disk is checked:
//   - the whole file is that build (size + FNV-1a 64)                  -> known build
//   - the file differs, but every anchor below is byte-for-byte as in that build
//                                                                       -> a patched copy, compatible
//   - any anchor differs (or the exe isn't a PE with image base 0x400000) -> refused
// An anchor is a place in the exe the mod depends on: the prologue of each function it detours or
// calls, an instruction that references each fixed global or vtable it uses, the call whose return
// address it checks (the list: tools/anchors.py SPEC; the table, generated from the known exe:
// exe_anchors_table.h). Anchors are compared in the file, not in memory: the bytes are not
// relocated there, and SecuROM's runtime changes are not seen. No Windows code here (unit tested).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hv::vr {

enum class AnchorKind { detour, call, retaddr, global, vtable };
const char* anchor_kind_name(AnchorKind k);

struct Anchor {
    AnchorKind kind;
    const char* name;
    uint32_t va;      // image VA (base 0x400000) of the bytes
    uint32_t target;  // the address the mod uses: the function, global or vtable (== va for detour/call)
    uint8_t len;
    uint8_t bytes[16];
};

// A region of the known exe file (headers, a section's raw data, the signature at the end).
struct KnownRegion {
    const char* label;
    uint32_t offset, size;
    uint64_t fnv64;
};

struct AnchorFailure {
    const Anchor* anchor;
    bool in_file;        // false: the bytes are not inside the exe's sections at all
    uint8_t found[16];   // in_file: what the exe has there
};

struct ExeCheck {
    bool pe = false;          // a PE file whose headers could be read
    uint32_t timestamp = 0;   // PE TimeDateStamp
    uint16_t characteristics = 0;  // FILE_HEADER.Characteristics (0x20: large address aware)
    uint64_t fnv64 = 0;       // of the whole file
    bool known = false;       // the whole file is the known build
    std::vector<std::string> problems;     // structural: not a PE, other image base
    std::vector<AnchorFailure> failures;   // anchors that differ
    std::vector<std::string> changed;      // known-exe regions this file differs in (not for the known build)
    bool compatible() const { return known || (pe && problems.empty() && failures.empty()); }
};

uint64_t fnv1a64(const void* data, size_t size);
ExeCheck check_exe(const uint8_t* data, size_t size);
std::string hex_bytes(const uint8_t* p, size_t n);  // "55 8b ec"

// The table (exe_anchors_table.h).
size_t anchor_count();
const Anchor* anchors();
size_t known_region_count();
const KnownRegion* known_regions();
uint64_t known_exe_size();
uint32_t known_exe_timestamp();

}  // namespace hv::vr
