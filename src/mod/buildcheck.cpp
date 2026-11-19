// The mod patches addresses of one build of HOTD_NG.exe (Steam build 569304, RC14). Before any
// hook goes in, the exe on disk is compared with that build (size + FNV-1a 64 of the file); on a
// mismatch the DLL stays a plain XInput pass-through.
//
// The log line always carries the exe's SHA-256, PE timestamp and Large Address Aware flag, so a
// user's report tells a different build (other timestamp) from a patched copy of ours (same
// timestamp, other bytes) without asking for more files.
#include "common.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace hv {

constexpr uint64_t kExeSize = 6345784;
constexpr uint64_t kExeFnv64 = 0x6fdfd5e751e4b7e8ull;  // also SHA-256 f9db505a...0275bb03
constexpr uint32_t kExeTimestamp = 0x54DE0845;         // PE TimeDateStamp: 2015-02-13 09:20:53 UTC

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

// PE TimeDateStamp and FILE_HEADER.Characteristics of an image in memory; false if not a PE.
static bool pe_header(const std::vector<uint8_t>& d, uint32_t* timestamp, uint16_t* characteristics) {
    if (d.size() < 0x40) return false;
    uint32_t pe = *reinterpret_cast<const uint32_t*>(&d[0x3C]);
    if (size_t(pe) + 24 > d.size() || memcmp(&d[pe], "PE\0\0", 4) != 0) return false;
    *timestamp = *reinterpret_cast<const uint32_t*>(&d[pe + 8]);
    *characteristics = *reinterpret_cast<const uint16_t*>(&d[pe + 22]);
    return true;
}

bool exe_is_known_build() {
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

    uint64_t h = fnv64(data.data(), data.size());
    bool ok = uint64_t(size.QuadPart) == kExeSize && h == kExeFnv64;
    uint32_t stamp = 0;
    uint16_t chars = 0;
    bool pe = pe_header(data, &stamp, &chars);
    logf("build check: %ls %lld bytes, fnv64 %016llx %s", path, size.QuadPart, h, ok ? "(known build)" : "(UNKNOWN build)");
    logf("build check: sha256 %s, PE timestamp %08x, large-address-aware %s", sha256_hex(data).c_str(), pe ? stamp : 0,
         !pe ? "?" : (chars & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "on" : "off");
    if (!ok) {
        if (pe && stamp == kExeTimestamp)
            logf("build check: same build as the supported one (timestamp matches) but the file was changed, "
                 "probably by another mod or patcher; Steam > Properties > Installed Files > Verify integrity "
                 "restores it");
        else
            logf("build check: a different build (supported: Steam build 569304, %llu bytes, PE timestamp %08x)",
                 kExeSize, kExeTimestamp);
    }
    return ok;
}

}  // namespace hv
