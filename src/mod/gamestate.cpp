// Gameplay vs menus/cutscenes/pause/death, from the game's own state (memory diffs of the main
// menu, loadout, loading, intro cutscene, gameplay, pause menu and death screen, 2026-11-17):
//
//   [0x99B770]        cCutsceneControl (allocated with the level by cGameModeRunLevel 0x4B4B50;
//                     null in the front end). +8 byte = cutscene mode on (set by the
//                     ActivateCutsceneMode script node, 0x514510; read by IsCutSceneRunning
//                     0x57B420 and ~30 callers of 0x469CD0).
//   [0x99B71C]        cGameModeManager. +0x310 byte = game paused (0x58A520 returns it): 1 in the
//                     pause menu and on the death screen ("Continue?"), 0 in play and cutscenes.
//
// Gameplay (stereo eyes + HUD panel) = in a level, not in a cutscene, not paused - and the scene
// drawn (stereo.cpp adds that from the frame structure: loading screens draw no scene).
#include "common.h"

namespace hv {

constexpr uintptr_t kCutsceneControl = 0x99B770, kGameModeManager = 0x99B71C;

static bool read_byte(uintptr_t addr, uint8_t* out) { return safe_copy(out, addr, 1); }

static uintptr_t read_ptr(uintptr_t image_va) { return *reinterpret_cast<const volatile uintptr_t*>(va(image_va)); }

GameState game_state() {
    GameState s;
    const uintptr_t cut = read_ptr(kCutsceneControl), gmm = read_ptr(kGameModeManager);
    s.level = cut != 0;
    uint8_t b = 0;
    if (s.level && read_byte(cut + 8, &b)) s.cutscene = b != 0;
    if (gmm && read_byte(gmm + 0x310, &b)) s.paused = b != 0;
    s.gameplay = s.level && !s.cutscene && !s.paused;
    return s;
}

}  // namespace hv
