#pragma once

//
// shrines - which shrines ("fire points") the save has lit, read at runtime.
//
// WHY IT IS NOT A PER-ACTOR FLAG
// -----------------------------
// The recon dumps had all three resident `BP_RebornFire_C` reading `Open = false`,
// `DefaultEnable = true`, and `common.md` carried an open backlog item asking for the
// activation flag. `context/saveslot-and-teleport-research.md` §2.3 closed it: there is
// no per-actor bool. The state is a GLOBAL LIST OF ID STRINGS owned by the game mode's
// `RebornManagerComponent_C` -
//
//     UnlockedFirepoints        TArray<FString>   "can I travel there"
//     FirepointsEverUnlocked    TArray<FString>
//     DeactivatedFirepoints     TArray<FString>
//     rebornFirePointID         FString           the shrine last rested at
//
// - persisted in the save under `lockqueue`. They are ordinary reflected properties, so
// they are read RAW: no `ProcessEvent`, no guessed signature, nothing that can re-enter
// the engine. That makes this the cheapest and safest of the three routes the research
// lists, and it is why this module exists at all rather than being folded into markers.
//
// Note the two lists are not interchangeable. `UnlockedFirepoints` also holds boss-door
// and task pseudo-points (`bossdoor_*`, `Task1`, ...) which are not shrines, so a count
// taken from it is not "shrines lit" - the stats page counts only the ids that join to a
// shrine marker in the static DB.
//
// THREADS
//   game thread   game_thread_pump(), drop_caches()  - raw reads only
//   any thread    state(), is_unlocked()             - spinlocked copy / lookup
//

#include <cstdint>

namespace shr
{
    // How many ids are kept. The game has 57 shrines plus ~14 pseudo-points; 160 leaves
    // room for the DLC and for a list that grows in a patch without truncating silently
    // (and `truncated` says when it did).
    inline constexpr int kMaxIds = 160;
    inline constexpr int kIdLen = 40;

    struct State
    {
        bool valid = false;      // false = the component has not been read this session
        bool truncated = false;  // the game's list was longer than kMaxIds
        int unlocked = 0;        // entries in UnlockedFirepoints (shrines AND pseudo-points)
        int ever = 0;            // entries in FirepointsEverUnlocked
        int deactivated = 0;     // entries in DeactivatedFirepoints
        char current[kIdLen]{};  // rebornFirePointID - the shrine last rested at
        char route[64]{};        // which property names answered, or why none did
        int id_count = 0;
        char ids[kMaxIds][kIdLen]{}; // the UnlockedFirepoints entries, in game order
    };

    // Any thread. ~6.5 KB, so the render thread caches it per sweep round rather than
    // asking per frame - same rule as markers::stats().
    State state();

    // Any thread. True when `id` is in UnlockedFirepoints. Case-insensitive: the save's
    // ids and the marker DB's ids come from two different extraction paths and the game
    // itself is inconsistent about capitalisation (`Task1` vs `digong01`).
    bool is_unlocked(const char* id);

    // GAME THREAD ONLY, from markers::game_thread_pump. Polls at 1 Hz - the list only
    // changes when the player lights a shrine, and a raw read of four properties is
    // cheap, but FindAllOf to locate the component is not, so that part stops once it
    // has answered.
    void game_thread_pump(std::uint64_t now);

    // GAME THREAD ONLY, from markers::drop_caches.
    void drop_caches();
} // namespace shr
