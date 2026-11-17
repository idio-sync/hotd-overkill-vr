// Spike: inline detour on cWeapon::FireBullets (VA 0x50D110, thiscall, ret 0xC; from the static
// analysis). Logs every shot's origin and direction (cWeapon+0x7B4, vec4) and, with `aimdir`,
// replaces the direction - the question being whether a VR controller can aim the game's own
// ray cast, and whether SecuROM tolerates a code patch.
#include "common.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace hv {

constexpr uintptr_t kFireRva = 0x50D110 - 0x400000;
static const uint8_t kPrologue[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x74, 0x03, 0x00, 0x00};

using Fire_t = void(__fastcall*)(void* self, void* edx, float* origin, int mode, float travelled);
static Fire_t g_tramp;
static std::mutex g_mx;
static bool g_override;
static bool g_capture_next;

void gun_capture_next() {
    std::lock_guard lock(g_mx);
    g_capture_next = true;
    g_override = false;
}
static float g_aim_dir[3];
static bool g_origin_override;
static float g_aim_origin[3];

void gun_set_dir(bool on, float x, float y, float z) {
    std::lock_guard lock(g_mx);
    g_override = on;
    float n = std::sqrt(x * x + y * y + z * z);
    if (n > 0) {
        g_aim_dir[0] = x / n;
        g_aim_dir[1] = y / n;
        g_aim_dir[2] = z / n;
    }
}

void gun_set_origin(bool on, float x, float y, float z) {
    std::lock_guard lock(g_mx);
    g_origin_override = on;
    g_aim_origin[0] = x;
    g_aim_origin[1] = y;
    g_aim_origin[2] = z;
}

static void __fastcall hk_fire(void* self, void* edx, float* origin, int mode, float travelled) {
    auto dir = reinterpret_cast<float*>(static_cast<uint8_t*>(self) + 0x7B4);
    logf("FireBullets weapon=%p mode=%d travelled=%g origin %.1f %.1f %.1f dir %.4f %.4f %.4f %.4f", self, mode & 0xFF,
         travelled, origin[0], origin[1], origin[2], dir[0], dir[1], dir[2], dir[3]);
    {
        std::lock_guard lock(g_mx);
        if (g_capture_next) {
            // `aimdir capture`: this shot goes where the cursor is; later shots reuse its direction.
            g_capture_next = false;
            float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            for (int i = 0; i < 3; ++i) g_aim_dir[i] = n > 0 ? dir[i] / n : 0;
            g_override = true;
            logf("  -> direction captured for later shots");
        } else if (g_override) {
            // Keep the length the game uses (it scales the vector to the weapon range itself).
            float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len <= 0) len = 1;
            for (int i = 0; i < 3; ++i) dir[i] = g_aim_dir[i] * len;
            logf("  -> dir overridden %.4f %.4f %.4f", dir[0], dir[1], dir[2]);
        }
        if (g_origin_override) {
            for (int i = 0; i < 3; ++i) origin[i] = g_aim_origin[i];
            logf("  -> origin overridden %.1f %.1f %.1f", origin[0], origin[1], origin[2]);
        }
    }
    g_tramp(self, edx, origin, mode, travelled);
}

void install_gun_hook() {
    static bool done;
    if (done) return;
    done = true;
    auto target = reinterpret_cast<uint8_t*>(exe_base() + kFireRva);
    if (memcmp(target, kPrologue, sizeof kPrologue) != 0) {
        logf("FireBullets prologue mismatch at %p (%02x %02x %02x ...); hook not installed", target, target[0], target[1], target[2]);
        return;
    }
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, kPrologue, sizeof kPrologue);
    tramp[9] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + 10) = int32_t((target + 9) - (tramp + 14));
    g_tramp = reinterpret_cast<Fire_t>(tramp);
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = int32_t(reinterpret_cast<uint8_t*>(&hk_fire) - (target + 5));
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    logf("FireBullets hook installed at %p (trampoline %p)", target, tramp);
}

}  // namespace hv
