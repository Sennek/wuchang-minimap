#pragma once
//
// The single source of truth for the mod's version.
//
// Everything that shows a version reads THIS file:
//   * dllmain.cpp   - CppUserModBase::ModVersion and the "WuchangMinimap vX loaded"
//                     line in ue4ss\UE4SS.log (the first thing to ask a player for);
//   * overlay.cpp   - the F2 settings panel's title bar and header line;
//   * tools\package.ps1 - the release folder / zip name and the INSTALL_GUIDE stamp.
//
// package.ps1 -Version x.y.z REWRITES the literal below (and xmake.lua's set_version,
// which is metadata only), so keep the definition on one line in exactly this shape.
//
#define WUCHANG_MINIMAP_VERSION "0.9.2"

// The UE4SS mod API and DynamicOutput are wide (RC_IS_ANSI=0, STR(x) == L##x), so the
// version literal needs a wide twin. Two-step so the argument is expanded first.
#define WUCHANG_MINIMAP_WIDEN2(x) L##x
#define WUCHANG_MINIMAP_WIDEN(x) WUCHANG_MINIMAP_WIDEN2(x)
#define WUCHANG_MINIMAP_VERSION_W WUCHANG_MINIMAP_WIDEN(WUCHANG_MINIMAP_VERSION)
