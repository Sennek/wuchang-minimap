//
// WuchangMinimap - a UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, DX12).
//
// This translation unit only contains the UE4SS mod skeleton. The rendering side
// (Dear ImGui + a DX12 Present hook installed with MinHook) lives in overlay.cpp
// and is *not* wired up yet - see overlay::selftest().
//
// The navmesh dumper (navmesh_dump.cpp) IS live: it locates the game's Recast/Detour
// navmesh and writes the streamed-in tiles to
//     ue4ss/Mods/WuchangMinimap/navmesh/<agent>/tiles_<timestamp>.json
// automatically and on F6. See navmesh_dump.hpp.
//

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "navmesh_dump.hpp"
#include "overlay.hpp"

using namespace RC;

namespace
{
    constexpr auto ModNameLiteral = STR("WuchangMinimap");
}

class WuchangMinimap : public CppUserModBase
{
  public:
    WuchangMinimap() : CppUserModBase()
    {
        ModName = ModNameLiteral;
        ModVersion = STR("0.1.0");
        ModDescription = STR("In-game minimap overlay for Wuchang: Fallen Feathers");
        ModAuthors = STR("commcp");

        Output::send<LogLevel::Verbose>(STR("WuchangMinimap loaded\n"));
    }

    ~WuchangMinimap() override = default;

    // The 'Unreal' namespace is usable from here on.
    auto on_unreal_init() -> void override
    {
        // Proves Dear ImGui and MinHook are linked in and callable. No hooks are
        // installed and no ImGui context is created yet.
        const auto report = overlay::selftest();
        Output::send<LogLevel::Verbose>(STR("WuchangMinimap: {}\n"), report);

        // The navmesh dumper needs the Unreal reflection API, so it can only start
        // here. It stays quiet until a RecastNavMesh actor actually shows up.
        navmesh::on_unreal_init();
    }

    auto on_update() -> void override
    {
        // Per-frame UE4SS tick. The overlay is not wired up yet; the navmesh dumper
        // throttles itself internally (actor poll every 2 s, hotkey edge-detected).
        navmesh::on_update();
    }
};

#define WUCHANG_MINIMAP_API __declspec(dllexport)

extern "C"
{
    WUCHANG_MINIMAP_API RC::CppUserModBase* start_mod()
    {
        return new WuchangMinimap();
    }

    WUCHANG_MINIMAP_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
