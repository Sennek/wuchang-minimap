#pragma once

#include <String/StringType.hpp>

namespace overlay
{
    // Touches ImGui, its DX12/Win32 backends and MinHook so all three link into
    // main.dll. Returns a version report. Installs nothing.
    auto selftest() -> RC::StringType;

    // Loop thread. Loads the waypoint and map assets, installs the DX12 hooks
    // (Present / Present1 / ResizeBuffers / ExecuteCommandLists). Idempotent; called
    // by modswitch, and the config must already be loaded.
    void start();

    //==================================================================================
    // The master switch (see modswitch.hpp)
    //==================================================================================

    // Loop thread. Asks the render thread to release its D3D12 objects - only that
    // thread may. Requires `mm::g_mod_active` already false.
    void request_stop();

    // Loop thread. True once the render thread has finished (or was never up at all).
    bool stop_complete();

    // Loop thread. Disables the hooks, after stop_complete() or its timeout. Safe to
    // call when nothing is installed.
    void finish_stop();

    // Loop thread, every tick: hotkeys, config reload/save, log draining, and the
    // "did the Present hook ever fire?" watchdog. Never touches D3D12.
    void on_update();
} // namespace overlay
