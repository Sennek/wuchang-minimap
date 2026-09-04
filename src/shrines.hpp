#pragma once

//
// shrines - which shrines ("fire points") the save has lit, read at runtime.
//
// There is no per-actor lit flag. The state is a global list of id strings owned by the
// game mode's `RebornManagerComponent_C`, persisted in the save under `lockqueue`:
//
//     UnlockedFirepoints        TArray<FString>   "can I travel there"
//     FirepointsEverUnlocked    TArray<FString>
//     DeactivatedFirepoints     TArray<FString>
//     rebornFirePointID         FString           the shrine last rested at
//
// They are ordinary reflected properties, read raw: no `ProcessEvent`, no guessed
// signature, nothing that can re-enter the engine.
//
// `UnlockedFirepoints` also holds boss-door and task pseudo-points (`bossdoor_*`,
// `Task1`, ...) which are not shrines, so its count is not "shrines lit" - the stats page
// counts only ids that join to a shrine marker in the static DB.
//
// THREADS
//   game thread   game_thread_pump(), drop_caches()  - raw reads only
//   any thread    state(), is_unlocked()             - spinlocked copy / lookup
//

#include <cstdint>
#include <vector>

#include "shrines_db.hpp"

namespace shr
{
    // 57 shrines plus ~14 pseudo-points, with room to grow; `truncated` says when the
    // game's list did not fit.
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

    // Any thread. ~6.5 KB, so the render thread caches it per sweep round.
    State state();

    // Any thread. True when `id` is in UnlockedFirepoints. Case-insensitive: the game is
    // inconsistent about capitalisation (`Task1` vs `digong01`).
    bool is_unlocked(const char* id);

    // GAME THREAD ONLY, from markers::game_thread_pump. Polls at 1 Hz; the FindAllOf that
    // locates the component stops once it has answered.
    void game_thread_pump(std::uint64_t now);

    // GAME THREAD ONLY, from markers::drop_caches.
    void drop_caches();

    //==================================================================================
    // The offline shrine table (markers/shrines.json)
    //==================================================================================

    // Loop thread, once. Reads markers/shrines.json and publishes it.
    void load_table();

    // ANY THREAD. The published table, or nullptr before load_table has run. Immutable
    // once published and leaked on reload - a render thread may be walking it.
    const std::vector<shdb::Shrine>* table();

    // ANY THREAD. What load_table() found, for the F2 readout.
    struct TableInfo
    {
        int rows = 0;
        int shrines = 0;
        int named = 0;
        char error[96]{}; // "" = loaded
    };

    TableInfo table_info();
} // namespace shr
