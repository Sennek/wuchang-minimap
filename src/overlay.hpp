#pragma once

#include <String/StringType.hpp>

namespace overlay
{
    // Touches ImGui, its DX12/Win32 backends and MinHook so all three link into
    // main.dll. Returns a version report. Installs nothing.
    auto selftest() -> RC::StringType;

    // Loop thread. Loads the waypoint and map assets, installs the DX12 hooks (Present, Present1,
    // ResizeBuffers). Idempotent; called by modswitch, and the config must already be loaded.
    void start();

    //==================================================================================
    // The master switch (see modswitch.hpp)
    //==================================================================================

    // Loop thread. Asks the render thread to release its D3D12 objects - only that
    // thread may. Requires `mm::g_mod_active` already false.
    void request_stop();

    // How far the render thread's own teardown has got. The loop thread's timeout
    // means two different things and has to tell them apart: nothing started means no
    // Present is arriving and the hooks can come out; started-and-unfinished means a
    // thread is inside the detour and nothing may be pulled out from under it.
    enum class StopPhase
    {
        NotStarted, // no Present has taken the teardown path (or the mod is running)
        InProgress, // a thread is inside release_device_objects right now
        Done,       // the render side is torn down
    };

    // Loop thread.
    StopPhase stop_phase();

    // Loop thread. Disables the hooks, after stop_phase() reports Done or the render
    // thread never started. Safe to call when nothing is installed. FALSE means the
    // render lock was still held, so a thread is inside the detour and no hook was
    // touched - the caller must go on waiting rather than finish the stop.
    bool finish_stop();

    // ANY THREAD. Stops and joins the overlay's surface thread and releases nothing, so
    // it is legal off the render thread. For the two paths the render thread's own
    // teardown never reaches: the master switch's timeout, and module unload - a thread
    // whose procedure lives in main.dll must not outlive the module.
    void stop_surface_thread();

    // Loop thread, every tick: hotkeys, config reload/save, log draining, and the
    // "did the Present hook ever fire?" watchdog. Never touches D3D12.
    void on_update();
} // namespace overlay
