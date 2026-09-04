#pragma once

//
// modswitch - the mod's master switch, and the only thing dllmain.cpp calls.
//
// `mod_enabled = 0` means the DLL is inert (`enabled` is the one that hides the
// overlay): no DX12 hook in Present, no work inside UE4SS's ProcessEvent callback, no
// GUObjectArray scan, no chapter height map in RAM (320-340 MB), no XInput polling, no
// keyboard sampling. The one thing that still runs: every kWatchPeriodMs the loop thread
// stat()s config_wuchang_minimap.txt and, only when the timestamp moved, reads
// `mod_enabled` from it - that is what turns the switch back on without a restart, since
// with the mod off nothing samples the keyboard for F5.
//
// D3D12 objects may only be released on the render thread, and the height planes only
// once no render thread can still be inside a slice, so a disable is a three-step state
// machine on the loop thread:
//
//   1. clear mm::g_mod_active. The game thread's pump and the Present hook test it on
//      their first statement, so nothing new starts;
//   2. the render thread tears down ImGui, the descriptor heaps, the slice buffers and
//      the map textures inside its own Present call, and says so;
//   3. the loop thread disables the MinHook hooks (the trampolines stay created, so a
//      re-enable is one MH_EnableHook, never a second hook on the same address) and
//      frees the map asset.
//
// If Present never fires again, step 2 times out after kStopTimeoutMs and step 3 runs
// anyway: the hooks come out and the ImGui/D3D objects stay allocated until the next
// enable reuses them.
//
// The ProcessEvent pre-callback cannot be undone - UE4SS exports a Register with no
// Unregister - so gamestate's pump early-outs on `mm::mod_active()` as its very first
// statement: one relaxed atomic load, no allocation, no config copy, no reflection.
//

namespace modswitch
{
    // Loop thread, once. Sets the loop thread, reads the config and starts every
    // subsystem, or starts nothing when the master switch is off.
    void on_unreal_init();

    // Loop thread, every tick. Runs the watcher, drives a pending stop, and forwards to
    // the subsystems' on_update while the mod is running.
    void on_update();

    // ANY THREAD. "Disable for this session": the same three-step stop as
    // `mod_enabled = 0` without writing the config, so the 1 Hz mtime watch turns the
    // mod back on when the file is next saved.
    void request_session_disable();
} // namespace modswitch
