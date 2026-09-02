# WuchangMinimap

A UE4SS C++ mod for **Wuchang: Fallen Feathers** (Unreal Engine 5.1.1, Windows x64, DX12).

Current state: **navmesh dumper works, overlay is still a skeleton.** The mod loads under
UE4SS, logs `WuchangMinimap loaded`, has Dear ImGui (DX12 + Win32 backends) and MinHook
compiled and linked into `main.dll` — no hooks installed, nothing rendered yet — and
contains a live [`navmesh_dump`](#navmesh-dumper) module that locates the game's
Recast/Detour navmesh in memory and writes the streamed-in tiles out as JSON.

---

## Build

Prerequisites, all already in place on this machine:

| Thing | Where | Notes |
|---|---|---|
| MSVC toolset **14.40.33807** | `C:\Program Files\Microsoft Visual Studio\18\Insiders` | VS 2026 Insiders. There is no `vcvars64.bat`; use `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell`. |
| Windows SDK 10.0.26100.0 | `E:\Windows Kits\10` | supplies `d3d12.h` / `dxgi.h` |
| xmake **3.1.1** | `F:\Tools\xmake\xmake.exe` | portable, extracted from the release zip |
| RE-UE4SS checkout | `F:\Tools\RE-UE4SS` | commit `97b7e501c19d8b2b7c662feee73aaa0dc1f0a4d1`, headers only |
| UE4SS release binaries | `F:\Tools\ue4ss\rel\ue4ss\UE4SS.dll` | `v3.0.1-1111-g97b7e501` (`experimental-latest`) |

### One-time: generate the UE4SS import library

```powershell
.\tools\gen_ue4ss_importlib.ps1 -Ue4ssDll 'F:\Tools\ue4ss\rel\ue4ss\UE4SS.dll'
```

Writes `sdk\UE4SS.def` and `sdk\lib\UE4SS.lib`. Both are committed, so you only need to
re-run this when you move to a different UE4SS build. See
[Why an import library?](#why-an-import-library) below.

### Build

```powershell
.\build.ps1
```

or, spelled out:

```powershell
F:\Tools\xmake\xmake.exe f -m Game__Shipping__Win64 -p windows -a x64 `
    --vs_toolset=14.40.33807 --ue4ss_root=F:/Tools/RE-UE4SS -y
F:\Tools\xmake\xmake.exe -j 8
```

Debug configuration: `.\build.ps1 -Mode Game__Debug__Win64`.
Full rebuild: `.\build.ps1 -Rebuild`.

### Output

```
build\windows\x64\Game__Shipping__Win64\main.dll     (~1.7 MB)
build\windows\x64\Game__Shipping__Win64\main.pdb
```

`main.dll` exports `start_mod` / `uninstall_mod` and imports 16 symbols from `UE4SS.dll`.

A clean build takes about 4 seconds. Our own target is built with `set_warnings("all")`
and is warning-free; `third_party/` is left at the default warning level.

## Install

```powershell
.\deploy.ps1
```

Copies the DLL to

```
E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers\
    Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\dlls\main.dll
```

and creates an empty `enabled.txt` in `...\Mods\WuchangMinimap\`, which is UE4SS's
"load this without touching `mods.txt`" opt-in. A mirror of the same layout is staged in
`deploy\ue4ss\Mods\WuchangMinimap\` so the repo shows exactly what gets installed.

Point it elsewhere with `-GameRoot`. Pass `-Force` to install before UE4SS itself is
present.

---

## Layout

```
src/dllmain.cpp            RC::CppUserModBase subclass, start_mod/uninstall_mod
src/overlay.{hpp,cpp}      ImGui + MinHook link self-test (no hooks installed yet)
src/navmesh_dump.{hpp,cpp} dtNavMesh discovery + tile walker + JSON writer
src/mem.{hpp,cpp}          VirtualQuery + SEH guarded raw reads
src/ue_min.hpp             hand-written RC::Unreal ABI declarations, see below
sdk/shim/GUI/GUI.hpp       stand-in header, see below
sdk/UE4SS.def              UE4SS.dll export table, generated
sdk/lib/UE4SS.lib          import library, generated
third_party/imgui/         Dear ImGui v1.92.9b + backends/{dx12,win32} + misc/cpp
third_party/minhook/       MinHook v1.3.4
third_party/fmt/           fmt 11.2.0, headers only (FMT_HEADER_ONLY)
tools/gen_ue4ss_importlib.ps1
tools/navmesh/render.py    tile JSON -> top-down floor PNGs + bounds.json
tools/lua-recon/           WuchangRecon Lua recon mod + its offline mock harness
deploy/ue4ss/Mods/WuchangMinimap/
```

### Vendored versions

| Library | Version | Why that one |
|---|---|---|
| Dear ImGui | **v1.92.9b** (latest release) | Our own ImGui context, rendered through our own DX12 Present hook - independent of the v1.92.1 that UE4SS links internally. |
| MinHook | **v1.3.4** (latest release) | |
| fmt | **11.2.0** | Not a choice: `DynamicOutput/Output.hpp` includes `<fmt/core.h>`, and UE4SS itself pins `fmt 11.2.0`. |

---

## Why an import library?

The official flow is to `add_subdirectory(RE-UE4SS)` / `includes("RE-UE4SS")` and let the
mod link against a `UE4SS` target built from source. **That is not possible here.**
`RE-UE4SS/deps/first/Unreal` is a submodule pointing at `git@github.com:Re-UE4SS/UEPseudo.git`,
which is a private repository - it is derived from Unreal Engine source, so it needs Epic
Games GitHub organisation membership. Without it, UE4SS cannot be compiled at all, and the
docs say as much (steps 1-3 of *Creating a C++ mod* are "get Epic source access").

The `zDEV-UE4SS_*.zip` release asset does **not** help: it is the same payload as the normal
release plus `UE4SS.pdb`. It ships no headers and no import library.

So instead:

1. **Headers** come from a plain `git clone` of RE-UE4SS checked out at
   `97b7e501` - the exact commit the installed `UE4SS.dll` was built from
   (release `v3.0.1-1111-g97b7e501`; the `-g97b7e501` suffix *is* the commit). Only
   `deps/first/Unreal` and `deps/first/patternsleuth` fail to clone, and nothing on the C++
   mod API path needs them.
2. **Linking** goes through `sdk/lib/UE4SS.lib`, synthesised by
   `tools/gen_ue4ss_importlib.ps1`: it runs `dumpbin /exports` on that same `UE4SS.dll`
   (4239 exports), writes a `.def`, and feeds it to `lib /def: /machine:x64`.

Because headers and DLL come from one commit, the ABI matches by construction - this is
actually a *stronger* guarantee than building UE4SS yourself from a possibly-drifted
checkout. `RC::CppUserModBase` is used verbatim from the real header, so its vtable and
member layout are identical to the one inside `UE4SS.dll`.

### Two things this setup depends on

**`sdk/shim/GUI/GUI.hpp`.** `<Mod/CppUserModBase.hpp>` includes `<GUI/GUITab.hpp>`, which
includes `<GUI/GUI.hpp>`, which includes `<GUI/LiveView.hpp>`, which includes
`<Unreal/UFunctionStructs.hpp>` - i.e. UEPseudo, which we do not have. `GUITab.hpp` uses
nothing at all from `GUI.hpp`, so `sdk/shim` is put **first** on the include path and
supplies a near-empty `GUI/GUI.hpp` that breaks the chain. Every other UE4SS header still
resolves to the real checkout. If UE4SS ever becomes buildable here, drop `"sdk/shim"` from
`ue4ss_includedirs()` in `xmake.lua` and nothing else changes.

**The CRT must be `/MD`.** `UE4SS.dll` imports `MSVCP140.dll`, `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll`, and the mod API passes `std::string_view`, `std::vector` and
`std::unique_ptr` across the DLL boundary. A `/MT` mod would get its own heap and its own
`std::` internals and crash. `xmake.lua` pins `set_runtimes("MD")` for every target.

### Consequences to keep in mind

- The mod is tied to UE4SS `v3.0.1-1111-g97b7e501`. Upgrading UE4SS means: re-clone/checkout
  `F:\Tools\RE-UE4SS` at the new commit, re-run `tools\gen_ue4ss_importlib.ps1` against the
  new `UE4SS.dll`, rebuild. It also means re-checking `src/ue_min.hpp` against the new
  `sdk/UE4SS.def` — every declaration in there names the symbol it must match.
- `UE4SS_ENABLE_IMGUI()` (sharing UE4SS's own ImGui context, e.g. for `register_tab`) is
  **not** usable as things stand: it lives in `UE4SSProgram.hpp`, which needs UEPseudo, and
  it would additionally require our vendored ImGui to be exactly v1.92.1. The plan is our
  own ImGui context on our own DX12 Present hook, so this does not block the minimap.
- `Output::send<LogLevel>(...)` is a header template: the formatting runs inside `main.dll`
  and only `Output::DefaultTargets::get_default_devices_ref()` is imported. That is why fmt
  is vendored header-only, and why the fmt version has to match UE4SS's.

---

---

## Navmesh dumper

`src/navmesh_dump.cpp` finds the game's Recast/Detour navmesh in memory and writes the
tiles that are currently streamed in to

```
ue4ss\Mods\WuchangMinimap\navmesh\<agent>\tiles_<yyyymmdd_hhmmss>.json
```

one directory per `ARecastNavMesh` actor (`Small` / `Big` / `BitFat` / `Giant`, agent radii
34 / 60 / 90 / 120). `Small` is the one the map will use. A dump happens automatically 3 s
after the set of live tiles stops changing — i.e. once per area as you walk — and on demand
on **F6** (or `CTRL+F6`), which also writes a `probe_<ts>.json` diagnostics file when it
found nothing. Only 4-6 of the game's 12 800-uu streaming cells are ever resident, so a full
map is the union of many dumps; the renderer merges them.

### Nothing is hardcoded, everything is validated

The point of the design is that no struct offset is assumed. Each step derives a candidate
and then proves it against something the recon pass measured, and logs the decision:

| Step | How it is found | How it is validated |
|---|---|---|
| the actors | `UObjectGlobals::FindAllOf("RecastNavMesh")`, re-polled every 2 s | 0 results is normal (main menu) and reported once |
| `AgentRadius`, `TileSizeUU`, `PolyRef*Bits` | reflection: `FProperty::GetOffset_Internal()` by name | printed; `TileSizeUU` becomes the tile-size expectation below |
| `FPImplRecastNavMesh*` | scan the actor from the end of its reflected properties (`max(offset+size)` over the whole super-struct chain) to `GetStructureSize()`, 8 bytes at a time | the target's **second** pointer must be the actor itself — that is `FPImplRecastNavMesh::NavMeshOwner` |
| `dtNavMesh*` | first field of that struct | `dtNavMeshParams`: `tileWidth == tileHeight == TileSizeUU`, `maxTiles ∈ [1, 65536]`, finite origin — **float and double (`dtReal`) layouts both tried**, the winner logged |
| `dtMeshTile[]` and `sizeof(dtMeshTile)` | scan the pointer slots after `m_params` for an array containing pointers to `DNAV` headers; the **gcd of the hit spacing** gives the stride | stride in [96, 1024], 8-aligned, and every hit ≡ 8 mod stride (the `header` field is always at `+8`) |
| `dtMeshHeader.bmin/bmax` | search for six consecutive `dtReal` forming a box no bigger than one tile | scored: +4 each for `bmin.x/y == orig + index * tileWidth`, +3 each for the preceding `walkableHeight/Radius` matching `AgentHeight`/`AgentRadius` |
| `polyCount` / `vertCount` | search the int block before `bmin` | accepted **only** if every vertex lies inside `bmin..bmax` and every `dtPoly` has 3..6 vertices with in-range indices; `DT_VERTS_PER_POLYGON` 6/8/4 all tried |

UE modifies `dtMeshTile` and `dtMeshHeader` (off-mesh segments, clusters, the `layer` field),
which is why the stride and the field offsets are measured rather than taken from the Recast
headers. Once learned, the offsets are cached per agent and re-validated cheaply; they are
also printed as a single `PIN LINE` so they can be hardcoded later if that ever becomes
worthwhile.

Every raw read goes through `mem::read` — `VirtualQuery` first, then an SEH-guarded `memcpy`
(`src/mem.cpp`) — so a wrong guess yields a log line, never a crash.

### `src/ue_min.hpp` — reflection without UEPseudo

The C++ mod uses the full `RC::Unreal` reflection API even though the headers for it are
unavailable (see [Why an import library?](#why-an-import-library)). `UE4SS.dll` *exports* the
whole API, and an MSVC mangled name depends only on namespace, class name, function name,
parameter types, cv/ref qualifiers and the **access specifier** — never on class layout. So
`ue_min.hpp` re-declares the twelve members we call inside deliberately empty classes in
`namespace RC::Unreal`, each annotated with the exact symbol from `sdk/UE4SS.def` it must
match, and they link against the real code. Two things to keep in mind if it ever needs
extending: mirror the real single, non-virtual, offset-0 inheritance chain so `this` needs no
adjustment, and keep `FField::GetNext` **private** with a friend accessor, because it is
private in UEPseudo and `A…` vs `Q…` is part of the symbol.

## Rendering the dumps

```powershell
.\deploy.ps1 -Pull    # game -> tools\navmesh\dumps\<agent>\*.json

python tools\navmesh\render.py --input tools\navmesh\dumps --out tools\navmesh\out --debug
python tools\navmesh\render.py --synthetic --out out_synthetic --debug   # self-test
```

`tools/navmesh/render.py` (Pillow only) merges every dump — newest wins per
`(tile x, tile y, layer)` — fills the polygons, splits stacked geometry into floors and writes
`out\<agent>_floor<i>.png` plus `out\bounds.json`. North-up mapping:
`u = (world_Y - min_y) * px_per_uu`, `v = (max_x - world_X) * px_per_uu`. Floors come from
clustering polygon-centroid Z per 12 800-uu streaming cell and splitting at gaps over 600 uu;
a polygon's floor index is the rank of its band inside its own cell, so floor 0 is the lowest
surface everywhere. `--probe out\navprobe_*.csv` renders the Lua mod's F11
`ProjectPointToNavigation` grids through the identical mapping, as an overlay target for
checking alignment.

## Next steps

- [ ] Hook `IDXGISwapChain3::Present` with MinHook, create our own ImGui context, init
      `imgui_impl_dx12` + `imgui_impl_win32`, subclass the game's WndProc.
- [ ] Read player world transform + level bounds from UE, draw the minimap.
- [ ] Sweep all streaming cells so the navmesh dumps cover a whole region, not just the
      4-6 cells resident around the player.
