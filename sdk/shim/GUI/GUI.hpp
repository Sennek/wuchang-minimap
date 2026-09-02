#pragma once
//
// ---------------------------------------------------------------------------
//  SHIM - not part of UE4SS.
// ---------------------------------------------------------------------------
//  The real <GUI/GUI.hpp> from the RE-UE4SS checkout pulls in <GUI/LiveView.hpp>,
//  which includes <Unreal/UFunctionStructs.hpp> and friends. Those headers live in
//  the `Re-UE4SS/UEPseudo` submodule, which is a private repository (it requires
//  Epic Games GitHub organisation membership) and therefore cannot be checked out.
//
//  We only ever reach <GUI/GUI.hpp> transitively, from <GUI/GUITab.hpp>, which is
//  itself only reached from <Mod/CppUserModBase.hpp>. GUITab.hpp does not use a
//  single declaration out of GUI.hpp, so an (almost) empty stand-in is enough to
//  break the dependency chain while leaving `RC::CppUserModBase` byte-for-byte
//  identical to the one inside UE4SS.dll.
//
//  This header is put on the include path *before* <ue4ss-root>/UE4SS/include so
//  that it wins the lookup for <GUI/GUI.hpp> only. Every other UE4SS header still
//  comes from the real checkout.
//
//  If UE4SS ever becomes buildable from source here, delete sdk/shim from the
//  include list in xmake.lua and nothing else has to change.
//
#include <cstdint>

namespace RC::GUI
{
    class GUITab;
} // namespace RC::GUI
