//
// WuchangMinimap - a UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, DX12).
//
// This translation unit only contains the UE4SS mod skeleton. The rendering side
// (Dear ImGui + a DX12 Present hook installed with MinHook) lives in overlay.cpp
// and is *not* wired up yet - see overlay::selftest().
//
// The navmesh dumper (navmesh_dump.cpp) is present but OPT-IN and off by default:
// the map background is built offline from the paks (tools/navmesh/offline), so the
// runtime dumper only matters for cells the paks do not carry. Turn it on with
//     ue4ss/Mods/WuchangMinimap/config.ini  ->  [navmesh] navmesh_dump = 1
// and it then writes navmesh/<agent>/tiles_<timestamp>.json on F3 and automatically.
// See navmesh_dump.hpp.
//

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "gamestate.hpp"
#include "mapdata.hpp"
#include "markers.hpp"
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

        // The overlay: config + map assets are loaded on this thread, then the DX12
        // hooks go in. It never touches a UObject.
        overlay::on_unreal_init();

        // Markers: the static markers/<chapter>.json database and the found tracker
        // are read here on the loop thread. The live half runs inside gamestate's
        // ProcessEvent pump, so this must come before it.
        markers::on_unreal_init();

        // The game-state reader: registers the ProcessEvent game-thread pump.
        gamestate::on_unreal_init();

        // The navmesh dumper needs the Unreal reflection API, so it can only start
        // here. It stays quiet until a RecastNavMesh actor actually shows up.
        navmesh::on_unreal_init();
    }

    auto on_update() -> void override
    {
        // NOTE: UE4SS calls this on its own EVENT-LOOP thread, not on the game thread
        // (proven 2026-09-02: our poll kept logging while the game thread was blocked in
        // WaitForSingleObject during a GPU crash dump). So nothing called from here may
        // traverse UObjects or read engine allocations - navmesh::on_update() only
        // samples the hotkey and hands the work to a game-thread pump.
        navmesh::on_update();
        overlay::on_update();
        // The chapter's map asset is loaded and unloaded here, on the loop thread:
        // gamestate names the chapter from the game thread with one atomic store, and
        // mapdata does the (multi-second, allocating) PNG work off it. See mapdata.hpp.
        mapdata::on_update();
        gamestate::on_update();
        markers::on_update();
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
