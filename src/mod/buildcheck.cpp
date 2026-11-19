// The mod patches addresses of one build of HOTD_NG.exe (Steam build 569304, RC14). Before any
// hook goes in, the exe on disk is checked (src/vr/exe_anchors.h): the known build, or a copy
// changed by another patcher that left every place the mod uses (the anchors) alone, gets the
// mod; anything else leaves the DLL a plain XInput pass-through, and the log names each anchor
// that differs (expected and found bytes).
//
// The log always carries the exe's size, SHA-256, PE timestamp and Large Address Aware flag, so a
// user's report tells a different build (other timestamp) from a patched copy of ours (same
// timestamp, other bytes) without asking for more files.
#include "common.h"

#include <bcrypt.h>

#include "../vr/exe_anchors.h"

#pragma comment(lib, "bcrypt.lib")

namespace hv {

static std::string sha256_hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32];
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            char hex[3];
            for (uint8_t b : digest) {
                snprintf(hex, sizeof hex, "%02x", b);
                out += hex;
            }
        }
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out.empty() ? "unavailable" : out;
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s.empty() ? "nothing" : s;
}

bool exe_is_supported() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        logf("build check: cannot open %ls (error %lu)", path, GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::vector<uint8_t> data;
    bool read_ok = false;
    if (size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
        data.resize(size_t(size.QuadPart));
        DWORD got = 0;
        read_ok = ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
    }
    CloseHandle(f);
    if (!read_ok) {
        logf("build check: reading %ls (%lld bytes) failed", path, size.QuadPart);
        return false;
    }

    const vr::ExeCheck c = vr::check_exe(data.data(), data.size());
    const size_t n = vr::anchor_count();
    const bool ok = c.compatible();
    logf("build check: %ls %lld bytes, fnv64 %016llx %s", path, size.QuadPart, c.fnv64,
         c.known ? "(known build)" : ok ? "(patched copy of the supported build - compatible)" : "(UNKNOWN build)");
    logf("build check: sha256 %s, PE timestamp %08x, large-address-aware %s", sha256_hex(data).c_str(), c.timestamp,
         !c.pe ? "?" : (c.characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "on" : "off");
    if (c.known) return true;
    if (ok) {
        logf("build check: WARNING: HOTD_NG.exe was changed (by another mod or patcher?), but all %zu places the mod "
             "uses are as in the supported build, so VR stays on. Changed: %s. If something misbehaves, try the "
             "unchanged exe (Steam > Properties > Installed Files > Verify integrity) before reporting it.",
             n, join(c.changed).c_str());
        return true;
    }
    for (const std::string& p : c.problems) logf("build check: %s", p.c_str());
    for (const vr::AnchorFailure& fl : c.failures) {
        const vr::Anchor& a = *fl.anchor;
        logf("build check: anchor \"%s\" (%s) at 0x%X: expected %s, found %s", a.name, vr::anchor_kind_name(a.kind), a.va,
             vr::hex_bytes(a.bytes, a.len).c_str(),
             fl.in_file ? vr::hex_bytes(fl.found, a.len).c_str() : "nothing (outside the file's sections)");
    }
    if (c.pe) logf("build check: differs from the supported build in: %s", join(c.changed).c_str());
    if (c.pe && c.timestamp == vr::known_exe_timestamp())
        logf("build check: %zu of %zu places the mod uses differ: the supported build, changed by another mod or "
             "patcher where this mod hooks or reads it; Steam > Properties > Installed Files > Verify integrity "
             "restores it",
             c.failures.size(), n);
    else
        logf("build check: a different build (supported: Steam build 569304, %llu bytes, PE timestamp %08x)",
             static_cast<unsigned long long>(vr::known_exe_size()), vr::known_exe_timestamp());
    return false;
}

}  // namespace hv
