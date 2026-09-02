#pragma once

#include <String/StringType.hpp>

namespace overlay
{
    // Touches Dear ImGui, its DX12/Win32 backends and MinHook so that all three are
    // actually compiled into and linked against main.dll. Returns a human-readable
    // version report. Installs nothing.
    auto selftest() -> RC::StringType;
} // namespace overlay
