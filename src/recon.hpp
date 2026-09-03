#pragma once

//
// recon - ONE key press that answers every open question about the fast-travel and
// save-slot routes, and writes the answer to a file the user can send back.
//
// WHY IT EXISTS
// -------------
// `context/saveslot-and-teleport-research.md` section 3 lists exactly four things that
// cannot be recovered from the cooked assets and need one in-game reflection pass:
//
//   1. the game mode's component list (does `RebornManagerComponent` / `GameSaveExe`
//      really hang off `DCSGameMode`?);
//   2. every property of `RebornManagerComponent_C`, with the contents of
//      `UnlockedFirepoints` / `FirepointsEverUnlocked` / `DeactivatedFirepoints` /
//      `rebornFirePointID` and the three save-key strings;
//   3. the reflected PARAMETER LISTS of the ten functions the two routes name - the one
//      thing an offline pass provably cannot get, because a cooked `UFunction` export
//      serialises its parameter `FProperty`s inline and there is no `.usmap`;
//   4. `Impl_GameSettingsSaver_C::TickCountSavPath` and `GameSettingsSaver_C::UserName`.
//
// In this project an in-game session is the scarce resource (lessons.md), and the way a
// session gets wasted is a diagnostic that only says "not found". So this dump is
// deliberately exhaustive and it records raw evidence: when a name does not resolve it
// prints the names that DID, so the next build can be fixed without another launch.
//
// IT CALLS NOTHING AND CHANGES NOTHING. Reflection lookups and raw property reads only:
// no `ProcessEvent`, no travel, no writes to the game. The output file is the only side
// effect.
//
// THREADS
//   loop thread   on_update()  - writes wuchang_minimap_recon_<timestamp>.txt
//   game thread   game_thread_pump()  - gathers, and queues the text as lines
//   any thread    request(), status()
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

    // GAME THREAD ONLY, from markers::game_thread_pump. Returns immediately (one atomic
    // load) unless a dump has been asked for. `world` is the pawn's UWorld*.
    void game_thread_pump(const void* world);

    // Loop thread, every tick. Writes the file once the game thread has filled it.
    void on_update();
} // namespace recon
