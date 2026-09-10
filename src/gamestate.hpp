#pragma once

//
// gamestate - reads the player pawn, the camera's view target and the widget stack on
// the GAME THREAD and publishes an mm::Snapshot for the render thread.
//
// Runs inside UE4SS's ProcessEvent pre-callback, the only place in a UE4SS C++ mod where
// UObject traversal is safe (CppUserModBase::on_update runs on UE4SS's event-loop thread).
//

#include <cstdint>

namespace gamestate
{
    // Call once from on_unreal_init: registers the game-thread pump.
    void on_unreal_init();

    // Call from on_update (loop thread): bookkeeping / logging only.
    void on_update();

    // Loop thread. Re-seeds the "has the pump fired?" window. The master switch calls it
    // on every enable: `on_update` does not run while the mod is off, so without this the
    // first window after a re-enable is stale by the whole off period and the watchdog
    // reports a game thread that was never blocked.
    void reset_watchdog();

    // ANY THREAD. `pump_calls()` counts 10 Hz game-thread position pumps, the loop thread's only
    // evidence the game thread stopped; `pump_stage()` is a literal naming what that pump last did.
    std::uint64_t pump_calls();
    const char* pump_stage();
} // namespace gamestate
