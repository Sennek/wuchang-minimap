//
// WuchangMinimap - a UE4SS C++ mod for Wuchang: Fallen Feathers (UE 5.1.1, DX12).
//
// This translation unit only contains the UE4SS mod skeleton. The rendering side
// (Dear ImGui + a DX12 Present hook installed with MinHook) lives in overlay.cpp
// and is *not* wired up yet - see overlay::selftest().
//

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

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
    }

    auto on_update() -> void override
    {
        // Per-frame UE4SS tick. Nothing to do until the overlay is wired up.
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
