#pragma once

#include <String/StringType.hpp>

namespace overlay
{
    // Touches Dear ImGui, its DX12/Win32 backends and MinHook so that all three are
    // actually compiled into and linked against main.dll. Returns a human-readable
    // version report. Installs nothing.
    auto selftest() -> RC::StringType;

    // UE4SS event-loop thread: load the waypoint and the map assets, then install (or,
    // after a disable, re-enable) the DX12 hooks - Present / Present1 / ResizeBuffers /
    // ExecuteCommandLists. Idempotent: the MinHook trampolines are created exactly once
    // per process, so calling this again after stop() can never double-hook an address.
    // Called by modswitch, never by dllmain directly. The config must already be loaded.
    void start();

    //==================================================================================
    // The master switch (see modswitch.hpp)
    //==================================================================================

    // Loop thread. Asks the render thread to tear its own objects down - it is the only
    // thread that may release a D3D12 resource. `mm::g_mod_active` must already be
    // false, which is what makes the next Present take the teardown path.
    void request_stop();

    // Loop thread. True once the render thread has finished (or was never up at all).
    bool stop_complete();

    // Loop thread. Disables the hooks - the last step of a shutdown, run after
    // stop_complete() or after the timeout. Safe to call when nothing is installed.
    void finish_stop();

    // UE4SS event-loop thread, every tick: hotkeys, config reload/save, log draining,
    // and the "did the Present hook ever fire?" watchdog. Never touches D3D12.
    void on_update();
} // namespace overlay
