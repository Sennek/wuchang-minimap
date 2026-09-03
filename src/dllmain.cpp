//
// WuchangMinimap - a UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, DX12).
//
// This translation unit only contains the UE4SS mod skeleton: it hands both entry
// points straight to modswitch, which owns the `mod_enabled` master switch and starts
// or stops every other subsystem (modswitch.hpp). The rendering side (Dear ImGui + a
// DX12 Present hook installed with MinHook) lives in overlay.cpp.
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

#include "breadcrumb.hpp"
#include "modswitch.hpp"
#include "overlay.hpp"
#include "version.hpp"

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
        ModVersion = WUCHANG_MINIMAP_VERSION_W;
        ModDescription = STR("In-game minimap overlay for Wuchang: Fallen Feathers");
        ModAuthors = STR("commcp");

        Output::send<LogLevel::Verbose>(STR("WuchangMinimap v") WUCHANG_MINIMAP_VERSION_W
                                         STR(" loaded\n"));
    }

    ~WuchangMinimap() override
    {
        // The one stage that means "this session ended on purpose". Everything else in
        // wuchang_minimap_last_stage.txt is read by the NEXT launch as a crash.
        crumb::stage(crumb::kCleanExit);
    }

    // The 'Unreal' namespace is usable from here on.
    auto on_unreal_init() -> void override
    {
        // Proves Dear ImGui and MinHook are linked in and callable. No hooks are
        // installed and no ImGui context is created yet.
        const auto report = overlay::selftest();
        Output::send<LogLevel::Verbose>(STR("WuchangMinimap: {}\n"), report);

        // EVERYTHING else - the overlay and its DX12 hooks, the markers, the
        // game-thread reader and the opt-in navmesh dumper - is started (or not) by
        // modswitch, which owns the `mod_enabled` master switch. See modswitch.hpp.
        modswitch::on_unreal_init();
    }

    auto on_update() -> void override
    {
        // NOTE: UE4SS calls this on its own EVENT-LOOP thread, not on the game thread
        // (proven 2026-09-02: our poll kept logging while the game thread was blocked in
        // WaitForSingleObject during a GPU crash dump). So nothing called from here may
        // traverse UObjects or read engine allocations.
        //
        // With mod_enabled = 0 this is one GetTickCount64 and, once a second, one
        // GetFileAttributesEx of the config file - that is the whole cost of a disabled
        // mod, and it is what lets the switch be flipped back on without a restart.
        modswitch::on_update();
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
