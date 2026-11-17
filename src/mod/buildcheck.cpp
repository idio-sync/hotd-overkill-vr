// The mod patches addresses of one build of HOTD_NG.exe (Steam build 569304, RC14). Before any
// hook goes in, the exe on disk is compared with that build (size + FNV-1a 64 of the file); on a
// mismatch the DLL stays a plain XInput pass-through.
#include "common.h"

namespace hv {

constexpr uint64_t kExeSize = 6345784;
constexpr uint64_t kExeFnv64 = 0x6fdfd5e751e4b7e8ull;  // also SHA-256 f9db505a...0275bb03

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
    bool ok = false;
    if (uint64_t(size.QuadPart) != kExeSize) {
        logf("build check: %ls is %lld bytes, expected %llu", path, size.QuadPart, kExeSize);
    } else {
        std::vector<uint8_t> data(size_t(size.QuadPart));
        DWORD got = 0;
        if (ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size()) {
            uint64_t h = fnv64(data.data(), data.size());
            ok = h == kExeFnv64;
            logf("build check: %ls fnv64 %016llx %s", path, h, ok ? "(known build)" : "(UNKNOWN build)");
        } else {
            logf("build check: reading %ls failed", path);
        }
    }
    CloseHandle(f);
    return ok;
}

}  // namespace hv
