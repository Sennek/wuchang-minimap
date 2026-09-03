#pragma once

//
// gamestate - reads the player pawn, the camera's view target and the widget stack on
// the GAME THREAD and publishes an mm::Snapshot for the render thread.
//
// It runs inside UE4SS's ProcessEvent pre-callback, which is the only place in a UE4SS
// C++ mod where UObject traversal is safe (CppUserModBase::on_update runs on UE4SS's
// own event-loop thread - see lessons.md and the comment on
// RegisterProcessEventPreCallback in ue_min.hpp).
//

#include <cstdint>

namespace gamestate
{
    // Call once from on_unreal_init: registers the game-thread pump.
    void on_unreal_init();

    // Call from on_update (loop thread): nothing but bookkeeping / logging.
    void on_update();

    // ANY THREAD, for the loop thread's stall watchdog (overlay.cpp). `pump_calls()` is
    // the number of 10 Hz position pumps the GAME thread has run - a counter that stops
    // moving is the only in-process evidence that the game thread has stopped - and
    // `pump_stage()` is a pointer to a literal naming what that pump was last doing, so
    // a freeze names the step it froze on instead of only the fact of it.
    std::uint64_t pump_calls();
    const char* pump_stage();
} // namespace gamestate
