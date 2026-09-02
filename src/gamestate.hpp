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

namespace gamestate
{
    // Call once from on_unreal_init: registers the game-thread pump.
    void on_unreal_init();

    // Call from on_update (loop thread): nothing but bookkeeping / logging.
    void on_update();
} // namespace gamestate
