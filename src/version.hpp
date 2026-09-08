#pragma once
//
// The single source of truth for the mod's version.
//
// `tools\package.ps1 -Version x.y.z` rewrites the literal below by pattern, so the
// definition must stay on one line in exactly this shape.
//
#define WUCHANG_MINIMAP_VERSION "1.1.0"

// The UE4SS mod API is wide (RC_IS_ANSI=0), so the literal needs a wide twin. Two-step
// so the argument expands first.
#define WUCHANG_MINIMAP_WIDEN2(x) L##x
#define WUCHANG_MINIMAP_WIDEN(x) WUCHANG_MINIMAP_WIDEN2(x)
#define WUCHANG_MINIMAP_VERSION_W WUCHANG_MINIMAP_WIDEN(WUCHANG_MINIMAP_VERSION)
