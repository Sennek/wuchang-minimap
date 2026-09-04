#pragma once

//
// gamestate - reads the player pawn, the camera's view target and the widget stack on
// the GAME THREAD and publishes an mm::Snapshot for the render thread.
//
// Runs inside UE4SS's ProcessEvent pre-callback: the only place in a UE4SS C++ mod
// where UObject traversal is safe (CppUserModBase::on_update runs on UE4SS's own
// event-loop thread).
//

#include <cstdint>

namespace gamestate
{
    // Call once from on_unreal_init: registers the game-thread pump.
    void on_unreal_init();

    // Call from on_update (loop thread): bookkeeping / logging only.
    void on_update();

    // ANY THREAD. `pump_calls()` counts 10 Hz game-thread position pumps; a stalled
    // counter is the loop thread's only evidence the game thread stopped.
    // `pump_stage()` is a literal naming what that pump was last doing.
    std::uint64_t pump_calls();
    const char* pump_stage();
} // namespace gamestate
