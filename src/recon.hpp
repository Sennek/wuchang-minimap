#pragma once

//
// recon - one key press that dumps everything about the fast-travel and save-slot
// routes to a file. Four sections:
//
//   1. the game mode's component list;
//   2. every property of `RebornManagerComponent_C`, plus the contents of
//      `UnlockedFirepoints` / `FirepointsEverUnlocked` / `DeactivatedFirepoints` /
//      `rebornFirePointID` and the three save-key strings;
//   3. the reflected parameter lists of the functions the two routes name - unavailable
//      offline, since a cooked `UFunction` export serialises its parameter `FProperty`s
//      inline and there is no `.usmap`;
//   4. `Impl_GameSettingsSaver_C::TickCountSavPath` and `GameSettingsSaver_C::UserName`.
//
// When a name does not resolve the dump prints the names that did.
//
// Reflection lookups and raw property reads only: no `ProcessEvent`, no travel, no
// writes to the game. The output file is the only side effect.
//
// THREADS: loop thread on_update() writes wuchang_minimap_recon_<timestamp>.txt; game thread
// game_thread_pump() gathers and queues the text as lines; any thread request(), status().
//

#include <cstdint>

namespace recon
{
    // ANY THREAD (the F2 Debug tab button).
    void request();

    struct Status
    {
        bool pending = false;  // asked for, not gathered yet
        int lines = 0;         // lines in the last dump
        char file[128]{};      // the last file written, or ""
        char error[96]{};      // "" = fine
    };

    Status status();

    // GAME THREAD ONLY. Returns at once unless a dump has been asked for. `world` is
    // the pawn's UWorld*.
    void game_thread_pump(const void* world);

    // Loop thread, every tick. Writes the file once the game thread has filled it.
    void on_update();
} // namespace recon
