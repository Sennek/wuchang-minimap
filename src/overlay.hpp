#pragma once

#include <String/StringType.hpp>

namespace overlay
{
    // Touches Dear ImGui, its DX12/Win32 backends and MinHook so that all three are
    // actually compiled into and linked against main.dll. Returns a human-readable
    // version report. Installs nothing.
    auto selftest() -> RC::StringType;

    // UE4SS event-loop thread, once: load the config and the map assets, then install
    // the DX12 hooks (Present / Present1 / ResizeBuffers / ExecuteCommandLists).
    void on_unreal_init();

    // UE4SS event-loop thread, every tick: hotkeys, config reload/save, log draining,
    // and the "did the Present hook ever fire?" watchdog. Never touches D3D12.
    void on_update();
} // namespace overlay
