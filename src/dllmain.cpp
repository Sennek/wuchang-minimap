//
// WuchangMinimap - a UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, DX12).
//
// The UE4SS mod skeleton only: both entry points go to modswitch, which owns `mod_enabled` and
// starts/stops every subsystem; rendering (Dear ImGui + MinHook'd DX12 Present) is overlay.cpp.
//
// The navmesh dumper is opt-in and off by default; the map background is built offline
// from the paks. `navmesh_dump = 1` in the dev config turns it on.
//

#include <Windows.h>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "breadcrumb.hpp"
#include "markers.hpp"
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
        // The one stage that means "ended on purpose"; the next launch reads any other
        // value as a crash.
        crumb::stage(crumb::kCleanExit);
    }

    // The 'Unreal' namespace is usable from here on.
    auto on_unreal_init() -> void override
    {
        // Proves Dear ImGui and MinHook are linked and callable. Installs no hooks and
        // creates no ImGui context.
        const auto report = overlay::selftest();
        Output::send<LogLevel::Verbose>(STR("WuchangMinimap: {}\n"), report);

        // Everything else - overlay and DX12 hooks, markers, the game-thread reader,
        // the opt-in navmesh dumper - starts under modswitch's `mod_enabled` switch.
        modswitch::on_unreal_init();
    }

    auto on_update() -> void override
    {
        // UE4SS calls this on its own EVENT-LOOP thread, not the game thread, so
        // nothing reached from here may traverse UObjects or read engine allocations.
        //
        // With mod_enabled = 0 the cost is one GetTickCount64 plus, once a second, one
        // GetFileAttributesEx of the config file, which is what lets the switch be
        // flipped back on without a restart.
        modswitch::on_update();
    }
};

#define WUCHANG_MINIMAP_API __declspec(dllexport)

// ALT+F4 and every other "the player closed the game" route: `~WuchangMinimap()` is not
// called and neither is the render thread's ImGui teardown. DLL_PROCESS_DETACH is the last
// thing this module hears about; the WndProc hook covers the window messages before it.
// Both call the same idempotent `crumb::mark_closing()`.
//
// DllMain runs under the loader lock, so this is flat Win32 calls only: no allocation,
// no engine access, no thread synchronisation.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        crumb::mark_closing();
        // The found tracker's debounced write, forced, or an ALT+F4 inside the debounce
        // window loses the last few seconds of marks. Bytes and path are staged by the
        // loop thread, so this is CreateFileW / WriteFile / MoveFileExW and nothing else.
        markers::flush_found_tracker_at_exit();
    }
    return TRUE;
}

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
