#pragma once

//
// modswitch - the mod's master switch, and the only thing dllmain.cpp calls.
//
// WHAT `mod_enabled = 0` HAS TO MEAN
// ----------------------------------
// Not "hide the overlay" (that is `enabled`), but "this DLL is inert": no DX12 hook in
// the game's Present path, no work inside UE4SS's ProcessEvent callback, no
// GUObjectArray scan, no chapter height map in RAM (320-340 MB), no XInput polling and
// no keyboard sampling. The player must be able to leave the mod installed and prove
// it is not the cause of whatever they are chasing.
//
// WHAT STILL RUNS
// ---------------
// Exactly one thing: every kWatchPeriodMs the loop thread stat()s
// config_wuchang_minimap.txt and, only if the timestamp moved, reads `mod_enabled` out
// of it. That is what lets the switch be turned back on without restarting the game -
// F5 cannot, because with the mod off nothing samples the keyboard.
//
// THE ORDER OF A SHUTDOWN MATTERS
// -------------------------------
// D3D12 objects may only be released on the render thread, and the height planes may
// only be freed once no render thread can still be inside a slice. So a disable is a
// three-step state machine on the loop thread:
//
//   1. clear mm::g_mod_active. The game thread's pump and the Present hook both test
//      it on their first statement, so from here on nothing new is started;
//   2. the render thread notices, tears down ImGui, the descriptor heaps, the slice
//      buffers and the map textures inside its own Present call, and says so;
//   3. the loop thread then disables the MinHook hooks (the trampolines stay CREATED,
//      so a re-enable is one MH_EnableHook and never a second hook on the same
//      address) and frees the map asset.
//
// If Present never fires again (the game is minimised, or it never fired at all) step 2
// times out after kStopTimeoutMs and step 3 runs anyway: the hooks come out, and the
// ImGui/D3D objects stay allocated until the next enable reuses them. That is the only
// case in which a disable leaves anything behind, and it is bounded and reported.
//
// The ProcessEvent pre-callback is the one thing that genuinely cannot be undone -
// UE4SS exports a Register with no matching Unregister. So gamestate's pump early-outs
// on `mm::mod_active()` as its very first statement: one relaxed atomic load, no
// allocation, no config copy, no reflection.
//

namespace modswitch
{
    // Loop thread, once (from on_unreal_init). Sets the loop thread, reads the config
    // and starts every subsystem - or logs that the master switch is off and starts
    // nothing at all.
    void on_unreal_init();

    // Loop thread, every tick. Runs the watcher, drives a pending stop, and forwards to
    // the subsystems' on_update while the mod is running.
    void on_update();
} // namespace modswitch
