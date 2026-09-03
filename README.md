# WuchangMinimap

A UE4SS C++ mod for **Wuchang: Fallen Feathers** (Unreal Engine 5.1.1, Windows x64, DX12).

Current state: **v0.9.0, feature-complete beta.** Minimap, [full map](#the-full-map-m),
[markers](#markers), collection tracker, [compass](#the-compass-strip) and
[x-ray highlight](#the-x-ray-highlight-hold-lalt) all ship; the map background is built
offline from the game's own navmesh. The mod loads under UE4SS and logs
`WuchangMinimap v0.9.0 loaded`. Also in here: the opt-in [`navmesh_dump`](#navmesh-dumper)
module that locates the game's Recast/Detour navmesh in memory and writes the streamed-in
tiles out as JSON.

The version is one `#define` in **`src/version.hpp`** — the DLL's `ModVersion`, the
start-up log line, the F2 panel header and `tools/package.ps1` all read it, and
`package.ps1 -Version x.y.z` is the only thing that should ever change it.

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
build\windows\x64\Game__Shipping__Win64\main.dll     (~2.9 MB)
build\windows\x64\Game__Shipping__Win64\main.pdb
```

`main.dll` exports `start_mod` / `uninstall_mod` and imports 16 symbols from `UE4SS.dll`.

A clean build takes about 4 seconds. Our own target is built with `set_warnings("all")`
and is warning-free; `third_party/` is left at the default warning level.

`build.ps1` also builds and runs the **offline tests** (`-NoTests` skips them). They link only
`src/markers_db.cpp` and `src/mapview.cpp`, so they need neither UE4SS nor Direct3D and run with the
game closed:

```
xmake build markers_test
xmake run   markers_test markers      # the repo's markers\ dir, for the sample manifest
```

They cover the `markers/<chapter>.json` loader (happy path against the shipped sample, plus every way
the file can be wrong), the category-name <-> bitmask mapping the config file and the F2 filter
checkboxes share, the `wuchang_minimap_found.txt` round-trip, and the full map's pure layer: the
world <-> screen transform round-tripped over the whole viewport at five zooms (plus north-is-up and
east-is-right asserted directly), the zoom clamp and per-notch step, and the waypoint file's exact
round-trip and its rejection of anything without a usable x and y. They also pin the x-ray highlight's
**world-to-screen projection** against hand-computed screen coordinates (screen centre, both FOV edges,
the vertical FOV that follows from the aspect, yaw and pitch, the behind-the-camera direction, and a
garbage camera producing nothing) and the **compass** arithmetic (wrap, bearings, strip positions, tick
ranks). Anything that can be checked without launching the game is checked there - a play session is the
expensive resource in this project.

## Install (development)

There are two install paths and they are not interchangeable:

| | `deploy.ps1` | `tools\package.ps1` |
|---|---|---|
| For | this machine, while developing | a release a player downloads |
| Writes | straight into the Steam folder | `dist\` only — never near the game |
| Ships | `main.dll` **and `main.pdb`** (crash dumps stay readable) | `main.dll` only |
| Configs | never overwrites one you have edited (`-ForceConfig` to force) | always the pristine shipped defaults |
| Extras | leaves the mod's runtime output (`navmesh\` dumps, your `wuchang_minimap_found.txt`) alone | asserts none of that is in the package |
| Checks | none | full smoke check + zip round-trip (below) |

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

## Release packaging

```powershell
.\tools\package.ps1                  # package whatever src\version.hpp says
.\tools\package.ps1 -Version 0.9.1   # stamp a new version first, then package
```

It runs `build.ps1` (so a release always compiles and passes the offline tests), then
assembles `dist\WuchangMinimap-<version>\` — a tree that mirrors **exactly** what a player
copies into `...\Project_Plague\Binaries\Win64\` — and zips it:

```
WuchangMinimap-0.9.0\
  INSTALL_GUIDE.html                   from tools\INSTALL_GUIDE.html, @@VERSION@@/@@DATE@@ substituted
  CHANGELOG.md                         from tools\CHANGELOG.template.md
  ue4ss\Mods\WuchangMinimap\
    dlls\main.dll                      no .pdb
    maps\maps.json, maps\chapter1..5\*.png
    markers\chapter{1..5,dlc}.json     chapter1.sample.json is excluded
    config_wuchang_minimap.txt
    config.ini
    enabled.txt                        empty; UE4SS's "load me" opt-in
```

Nothing else ships: no `main.pdb`, no `navmesh\` dumps, and none of the player-state files
(`wuchang_minimap_found.txt`, `wuchang_minimap_waypoint.txt`) — the script fails the build
if any of them turn up in the tree.

**Smoke check**, run before the zip is created, because the alternative is a player
discovering a missing PNG:

* `maps.json` parses, is schema `wuchang-minimap-maps/3`, and lists five chapters;
* every `image` and every `height_maps` entry it names exists and is non-empty — this is
  literally the list `mapdata.cpp` walks at start-up (45 PNG at 0.9.0), and a PNG in
  `maps\` that the manifest does *not* name is warned about as dead download weight;
* the five chapter marker manifests are present and are schema `wuchang-minimap-markers/1`;
* `main.dll`, both config files and `enabled.txt` are present.

**Zip round-trip**: entry count and every entry's uncompressed length are compared against
the tree on disk, and `maps.json` is actually decompressed and re-parsed, so a corrupt
stream cannot pass on metadata alone.

`-Version x.y.z` rewrites `src/version.hpp` (the source of truth) and `xmake.lua`'s
`set_version` together; without it the script reads the header and warns if the two have
drifted apart. `-NoBuild` packages the existing `build\` output — for iterating on the
packaging script itself, never for a release. `dist\` is gitignored.

---

## Layout

```
src/dllmain.cpp            RC::CppUserModBase subclass, start_mod/uninstall_mod
src/overlay.{hpp,cpp}      DX12 hooks + ImGui + the minimap, the full map and the F2 panel
src/gamestate.{hpp,cpp}    game-thread reader (pawn, view target, widgets)
src/mmstate.{hpp,cpp}      snapshot seqlock, config file, cross-thread log queue
src/mapdata.{hpp,cpp}      maps.json parser + WIC PNG decode
src/markers_db.{hpp,cpp}   PURE marker model: markers/<chapter>.json, category masks,
                           the found-file round-trip. No Windows, no UE4SS - which is
                           what lets tests/markers_test.cpp link it
src/markers.{hpp,cpp}      the runtime half: the chunked game-thread object sweep, the merge
                           with the static DB, the found tracker's file I/O
src/scan_sched.hpp         PURE scan scheduler: which object-array slots this pump,
                           has the round wrapped, is it time yet. No engine types,
                           so markers_test covers the live sweep's pacing
src/mapview.{hpp,cpp}      PURE full-map layer: the north-up viewport transform and its
                           exact inverse, the zoom clamp / step, zoom-to-fit, the
                           minimap's zoom-preset ladder, the waypoint file round-trip.
                           Same "no Windows, no UE4SS" rule as markers_db, so
                           markers_test links it too
src/glyphs.hpp             PURE shape-per-category and hue-per-category tables plus the
                           themes. The property that no two categories share a shape AND
                           a colour is a property of two tables, so markers_test asserts
                           it for every palette
src/label_layout.hpp       PURE greedy label placement for the x-ray highlight: an
                           occupied-rectangle list, push each box down until it clears,
                           refuse past a cap. Tested on the property that matters - no
                           two placed labels ever overlap
src/gamepad.{hpp,cpp}      XInput, dynamically loaded, polled on the LOOP thread only
src/projection.hpp         PURE world -> camera -> NDC -> screen math for the x-ray
                           highlight (UE basis, horizontal FOV, behind-camera case).
                           No engine types, so markers_test pins it against
                           hand-computed screen coordinates
src/compass.{hpp,cpp}      PURE compass arithmetic: yaw wrap, bearings, strip positions,
                           cardinal ticks. Same rule, same test binary
src/highlight.{hpp,cpp}    the x-ray highlight's game-thread half: finds the
                           PlayerCameraManager, calibrates the POV offset inside
                           CameraCachePrivate against the camera getters, then publishes
                           the pose through its own seqlock
src/version.hpp            the single WUCHANG_MINIMAP_VERSION define - the DLL's ModVersion,
                           the start-up log line, the F2 panel header and package.ps1
src/json.hpp               the one JSON reader, shared by mapdata and markers
src/uereflect.hpp          cached property offsets and UFunction calls
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
tools/package.ps1          the RELEASE packager: build + assemble + smoke check + zip
tools/INSTALL_GUIDE.html   the player-facing guide; @@VERSION@@ / @@DATE@@ are substituted
tools/CHANGELOG.template.md the changelog stub dropped at the package root
tools/navmesh/render.py    tile JSON -> top-down floor PNGs + bounds.json
tools/navmesh/build_map.py tile JSON -> composite + multi-surface height maps + maps/maps.json
tools/navmesh/slice_preview.py the runtime's height-slicing rule, offline, for any (x, y, z)
maps/                      the shipped map assets (deployed into the mod folder)
markers/                   the static marker database (deployed into the mod folder);
                           chapter1.sample.json documents the schema by hand
tests/markers_test.cpp     offline tests - `xmake run markers_test markers`
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
34 / 60 / 90 / 120).

> **The runtime dumper is OFF by default.** The map background is built **offline, out of the
> paks** (`tools/navmesh/offline`) — the whole game in ~2 minutes, with 0 % false positives
> against the game's own navigation probes — so the runtime path only matters for cells the
> paks do not carry and for navmesh carved at runtime. Turn it on for a session with
> `ue4ss\Mods\WuchangMinimap\config.ini`:
>
> ```ini
> [navmesh]
> navmesh_dump = 1
> ```
>
> With it on, **Dump the live navmesh tiles** on the F2 panel's Debug tab forces a dump. There
> is no hotkey: a memory scan that writes files must not be startable by a stray key press.

When enabled, a dump happens automatically 3 s after the set of live tiles stops changing —
i.e. once per area as you walk — and on demand on **F3** (or `CTRL+F3`), which also writes a
`probe_<ts>.json` diagnostics file when it found nothing. `navmesh\last_stage.txt` records the
stage the dumper is in, rewritten and closed at every stage so it survives a crash that eats
the log buffer. Only 4-6 of the game's 10 240-uu streaming cells are ever resident, so a full
map is the union of many dumps; the renderer merges them.

### Threading rules (these bind the overlay too)

`CppUserModBase::on_update` runs on **UE4SS's event-loop thread**, not the game thread. So:

* every UObject traversal (`FindAllOf`, reflection) and every raw read of an engine allocation
  happens in a game-thread pump registered with `RC::Unreal::Hook::RegisterProcessEventPreCallback`;
* that pump does **raw memory work only** — it queues log text and parks results, because C++
  iostreams and the C++ locale fault when touched from this game's game thread;
* `on_update` drains the log queue and does all file/JSON writing;
* there is **no `std::mutex`** anywhere in the mod — `std::mutex::try_lock` faults against the
  MSVCP140 loaded in this process. Locking is a header-only `std::atomic_flag` spinlock plus a
  non-blocking `std::atomic<bool>` single-flight exchange.

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

## The map assets

```powershell
# offline: paks -> tile JSON  (see .workspace/.../context/navmesh-offline.md)
python tools\navmesh\offline\pak.py unpack "<...>\Project_Plague-Windows.pak" `
       --grep "Maps/Generate/Chapter1/EX0/" --out <scratch>
python tools\navmesh\offline\navchunk.py "<scratch>\...\Chapter1\EX0\*.umap" `
       --out tools\navmesh\dumps_offline --stamp 20260902_ch1

# tile JSON -> the shipped assets
cd tools\navmesh
python build_map.py --input dumps_offline --chapter chapter1 --out ..\..\maps
```

`build_map.py` imports `render.py`, so the loader, the richest-copy dedupe and the flat-plane filter are
shared. It writes three things (schema `wuchang-minimap-maps/3`):

1. **`chapter1/small.png`** - the Z-shaded RGBA composite of every storey, transparent background.
   Chapter 1 at 0.06 px/uu is 4947 x 4333 px, 3.2 MB PNG, 82 MB as RGBA8. It is only the *fallback* for
   a chapter with no height maps, and is not loaded unless `fallback_use_composite = 1`.
2. **`chapter1/small_z0.png` .. `_z7.png`** - the **multi-surface height map**: eight 16-bit grayscale
   PNGs where plane k holds, at every pixel, the Z of the k-th walkable surface counted from the bottom.
   `code = 1 + round((Z - z_min) / (z_max - z_min) * 65534)`, and **code 0 means "no surface"**. All
   eight share one size, one `px_per_uu` and one set of bounds - no per-layer crops, no per-layer
   scales. Chapter 1: **4947 x 4333, 10.2 MB of PNG, 327 MB of RAM** (`z_min` -15089, `z_max` 38871,
   0.82 uu per step). Properties that matter:
   * **Fill only, no outlines.** Coverage is "the sample point is inside the polygon, or within
     `--seam-px` (0.5) of its boundary", which closes the sub-pixel gaps that made the old layers look
     like a triangle mesh. The ~1 px overlap that creates is absorbed by `--merge-tol` (120 uu): a
     polygon's pixels *join* the surface already at that pixel when the Z is that close, instead of
     opening a new slot - so no phantom storey appears along an edge.
   * **Z is interpolated per vertex** (barycentric over the polygon's fan triangles, clamped to the
     polygon's own vertex Z range), so a ramp or a staircase stores a smoothly varying Z and the
     runtime's gradient comes out smooth rather than per-polygon flat.
   * Slots are sorted ascending, so `z0 <= z1 <= ... <= z7` is guaranteed per pixel. **Eight** slots,
     not four: four hold 93 % of the chapter's lit pixels but only 50 % in the Digong-spiral /
     Hanguang-temple block (up to eleven surfaces at one pixel), and sliced at the temple's feet Z that
     cost two thirds of the floor (5 655 opaque px vs 17 807). Eight is within 2 % of sixteen. The top
     slot is the **overflow** slot and keeps the *highest* Z, so the top of a deep stack is never what
     gets dropped. `--max-surfaces 4` halves the RAM if needed.
3. **`maps.json`** - per chapter the bounds, scale, mapping, `z_min` / `z_max` / `z_step_uu`,
   `max_surfaces` and the `height_maps` list (the array index IS the surface slot). 1 kB.

Superseded, kept reachable behind `--legacy-layers`: the **per-pixel surface-ordinal** layers
(`small_f0..f7.png`, 8-bit coverage masks) plus the 640-uu **surface-band grid**. They separated storeys
exactly, but the *runtime* could only guess which ordinal its storey was from a band table that names up
to three of them - so a temple interior drew several layers blended and read as noise. The height map
answers the same question exactly, per pixel. `.workspace/wuchang-minimap/lessons.md` has the
measurements, including why floor ranks and surface connectivity both fail on this map.

### `slice_preview.py` - the runtime's rule, offline

```powershell
python tools\navmesh\slice_preview.py --x 19537 --y 4587 --z 2505 --out temple.png
```

Renders exactly what the overlay would draw at that world position, straight from the shipped height
maps - so a "the floor looks wrong at X" report can be reproduced and fixed without launching the game.
`slice_window()` + `shade()` are the reference implementation of the slicing rule; keep them and
`overlay.cpp`'s `slice_window()` in step.

`--px-per-uu` is a request - the scale halves until neither dimension exceeds `--max-dim` (8192).

`deploy.ps1` copies `maps\` into the mod folder every time (add `-NoMaps` to skip).

## The overlay

`src/overlay.cpp` installs four MinHook hooks whose addresses come from a throwaway
device + queue + swapchain (the hudhook trick, since the game's swapchain and its command queue
are not reachable from a UE4SS mod):

| slot | function |
|---|---|
| swapchain vtable 8 | `IDXGISwapChain::Present` |
| swapchain vtable 13 | `IDXGISwapChain::ResizeBuffers` |
| swapchain vtable 22 | `IDXGISwapChain1::Present1` |
| queue vtable 10 | `ID3D12CommandQueue::ExecuteCommandLists` (captures the real queue) |

**On this install all four land inside ReShade's `dxgi.dll`** - it is a 5.6 MB proxy next to the exe,
and it wraps the command queue too. The dummy objects are created through our own import table, so we
get the same wrappers the game holds; the module+offset of every hooked address is logged. Consequence:
the overlay draws before ReShade's effects. Two things that surprised us and are now handled:
`swapchain->GetDevice(IID_ID3D12Device)` **fails** on the wrapper (the device is taken off the captured
queue instead), and the game presents a decoy **144x8 D3D11** swapchain every frame next to the real
**1920x1080 R10G10B10A2_UNORM** one, so the overlay picks one swapchain -
`GetBuffer(0, IID_ID3D12Resource)` is the test - and ignores Presents from any other.

Rendering owns its own SRV descriptor heap (ImGui 1.92's `ImGui_ImplDX12_InitInfo` allocates through
callbacks), one command allocator per back buffer fenced against reuse, and RTVs recreated lazily after
`ResizeBuffers`. The map texture is created and uploaded by us (`CopyTextureRegion` + a barrier to
`PIXEL_SHADER_RESOURCE`), and its GPU descriptor handle is passed to ImGui as the `ImTextureID`.

The **full map** (`M`) is described in its own section below; the minimap is drawn on the foreground
draw list: player-centred crop, north-up by default or
rotate-with-player, round (a UV'd triangle fan - no mask needed) or square, configurable zoom, size,
anchor, offsets and opacity, with a yellow player arrow. It hides itself when a menu is open, when the
camera's view target is not the pawn, when the state snapshot is stale, or when the player is outside
every mapped chapter - and the F2 panel prints which of those it was.

`src/gamestate.cpp` reads the state on the **game thread** inside UE4SS's ProcessEvent pre-callback:
pawn location and yaw at 10 Hz via `K2_GetActorLocation` / `K2_GetActorRotation`, the pawn and
controller re-resolved at 2 Hz, and the menu test at 2 Hz (`UWidget::Visibility == Visible` prefiltered
from the reflected byte, then `IsInViewport()`). It publishes an `mm::Snapshot` through a seqlock; the
render thread never touches a UObject.

### The F2 panel

Three tabs since 0.9.2, and they are the config's tiers made visible:

- **Player** - presets (*Minimal HUD* / *Loot hunting* / *Exploration*, each setting several Player
  keys at once and deliberately touching no hotkey, no UI scale and nothing on the Advanced tab),
  minimap, placement and scale, markers, the collection tracker, the full map, the x-ray highlight,
  the compass, and the key legend.
- **Advanced** - six collapsing headers over the Advanced tier.
- **Debug** - present **only while `debug_readout = 1`**, which is a Dev key in a file a player does
  not have. It carries the 22 Dev keys, the per-activity performance table, every read-only
  diagnostic (marker sweep, gamepad, map slice, x-ray camera, game state) and the
  `hidden because: <reason>` line.

Category filters are coloured chips - each filled with the colour that category is drawn in, so the
filter row is also the legend. The Save / **Revert** / Reload row and the master switch live outside
the tabs, at the bottom, and never scroll away. Save writes the file named on the button and
rewrites *only the values*; Debug-tab settings go to the dev file instead.

### Settings

`ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`, plain `key = value`, `;` or `#` starts a
comment. Since 0.9.2 the keys are sorted into three **tiers**, and `src/config_keys.hpp` is the one
table that says which tier each one is in:

| tier | where it lives | what it is |
|---|---|---|
| **Player** (48) | `config_wuchang_minimap.txt`, under `; ---- PLAYER SETTINGS ----`; F2 → *Player* | something a person tuning the HUD would plausibly change |
| **Advanced** (53) | the same file, under `; ---- ADVANCED ----`; F2 → *Advanced* | correct as shipped; changed to answer a symptom |
| **Dev** (22) | `config_wuchang_minimap_dev.txt`; F2 → *Debug* | a dial that exists because a developer needed one during bring-up |

**`config_wuchang_minimap_dev.txt` is not part of a release.** It is read *only if it exists*, in the
same folder, **after** the player config — so a key set in both wins there — and `tools\package.ps1`
throws if it ever finds one in the staged package. `deploy.ps1` (the *dev* install) does copy it. The
1 Hz timestamp watch and F5 both look at **both** files, so editing either one reloads both. The F2
panel's Debug tab edits these keys and saves them back into the dev file, never into the player one;
if the file does not exist, Save leaves it that way.

Eleven former keys were **hard-coded** in 0.9.2 — `minimap_circle_segments`, `slice_min_px`,
`slice_max_px`, `map_slice_margin`, `reader_max_widgets`, `reader_max_menu_roots`,
`reader_max_levels`, `markers_live_max`, `markers_id_cache_max`, `markers_class_cache_max`,
`markers_fallback_max_per_class`. They were sanity caps, never preferences: a wrong value was a bug
report. An old file that still carries one gets a single warning naming it and is otherwise fine.
`enabled` was renamed to **`overlay_enabled`** in the same release; the old spelling still works and
also logs one warning.

**The master switch, `mod_enabled` (default 1).** `mod_enabled = 0` makes the DLL inert: the DX12
hooks are not installed (and are cleanly disabled again if they already were - the render thread
tears ImGui and every D3D12 object down inside one Present first, then the MinHook trampolines are
disabled but kept, so turning it back on can never double-hook), the ProcessEvent game-thread
callback returns on its first statement (UE4SS exports no *Unregister*, so that early return is the
mechanism), no object-array scan runs, the chapter's height maps are freed and XInput is never
polled. What keeps running is one `GetFileAttributesEx` of the config file per second on the loop
thread: change `mod_enabled` back to `1`, save, and the mod restarts within a second. **F5 does not
work while the mod is off** - nothing samples the keyboard - and the F2 panel's master-switch
checkbox can only turn it *off* (it writes the key and lets the watcher do the work). Every flip
writes one `master switch:` line into `UE4SS.log`. `overlay_enabled` is the *overlay*, not the mod:
with `overlay_enabled = 0` the reader, the marker sweep and the map asset all still run.

The other keys: `overlay_enabled`, `ui_scale`, `hud_preset`,
`show_minimap`, `minimap_size`, `minimap_zoom`, `minimap_shape`, `minimap_anchor`,
`minimap_offset_x/y`, `rotate_with_player`, `opacity`, `hide_in_menus`, `require_pawn_view`,
`state_stale_ms`, `min_visible_after_state_ok_ms`, `menu_close_show_delay_ms`, `panel_key`,
`reload_key`, `debug_readout` and `debug_show_panel_on_start` (both *dev*), the `highlight_*` block plus
`xray_rarity_colors_enabled` / `xray_rarity_colors` / `markers_rarity_tint` (see
[the x-ray highlight](#the-x-ray-highlight-hold-lalt)) and the `compass_*` block (9 keys - see
[the compass strip](#the-compass-strip)), plus the height-slicing block:

| key | default | meaning |
|---|---|---|
| `show_adjacent_floors` | 1 | also draw the surfaces below / above, dimmed |
| `adjacent_floor_opacity` | 0.25 | opacity of the surface below; the one above uses 60 % of it |
| `floor_z_tolerance` | 200 | uu: a surface within this of your feet is "my floor", drawn opaque |
| `floor_fade_uu` | 800 | uu: how far below / above is still drawn, dimmed |
| `floor_gradient_strength` | 0.18 | `lum = 1 + strength * clamp((surfaceZ - feetZ) / span, -1, +1)` |
| `floor_base_color` | `214 208 196` | the walkable fill, `R G B` |
| `slice_hz` | 12 | CPU re-slices per second (2..30) |
| `feet_z_smooth_ms` | 100 | EMA time constant on feet Z |
| `player_z_offset` | 90 | uu subtracted from the pawn's capsule centre to get feet Z |
| `fallback_use_composite` | 0 | also load `small.png` (+82 MB VRAM); only useful with no height maps |

Removed in the height-slicing rewrite: `floor_hysteresis` and `floor_fallback_hold_ms` - there is no
band grid to be off any more, and the only smoothing left is `feet_z_smooth_ms`.

The marker block:

| key | default | meaning |
|---|---|---|
| `markers_enabled` | 1 | draw markers at all |
| `markers_live` | 1 | run the game-thread class sweep (off = static positions, no state) |
| `markers_filter_chapter` | 1 | draw only the current chapter's static markers - the DB holds all six chapters and their world bounds overlap (chapter 4 covers nearly all of chapter 1); nothing is filtered until the chapter is recognised, and live actors are never filtered |
| `markers_rounds_per_sec` | 1 | ceiling on how often a full pass over the object array may **start** (1..10); a pass that finishes early idles |
| `markers_scan_chunk` | 8192 | object slots the sweep visits per game-thread pump (512..131072) - the frame-cost dial |
| `markers_scan_period_ms` | 8 | minimum milliseconds between pumps (1..500) |
| `markers_categories` | all but `enemy` | comma-separated category names, or `all` / `none`; the F2 checkboxes edit the same setting |
| `markers_hide_found` | 1 | 0 = draw a found marker dimmed and as an OUTLINE of its glyph, 1 = hide it. The F2 Player tab offers the inverse, "Show found markers". |
| `markers_found_alpha` | 0.30 | how dim, as a multiple of `opacity` |
| `markers_size` | 6.5 | glyph radius in minimap pixels |
| `markers_clamp_to_edge` | 0 | keep out-of-range markers on the rim, drawn smaller |
| `markers_max_draw` | 400 | hard cap per frame, nearest first (safety valve) |
| `markers_absence_marks` | 1 | mark a chest / pickup collected when its level is loaded and a full object-array round has not seen it (see below) |
| `markers_absence_rounds` | 2 | consecutive confirming rounds before that mark |
| `markers_absence_categories` | `chest,pickup` | which categories the absence rule may mark |
| `found_tracker` | 1 | write `wuchang_minimap_found.txt` |
| `found_save_debounce_ms` | 2000 | how long after the last change the file is written |

#### Absence as evidence of a collect

An item picked up **before the mod was installed** leaves nothing to read: the game parks a collected
pickup at `(0, 0, 0)` when its level loads and frees it at the next GC, so neither `dying` nor the
`(0,0,0)` test has an actor to speak for, and the marker stayed on the map for ever. Absence on its own
is famously *not* evidence here - an unloaded level and a collected pickup are indistinguishable from
the object array - so the rule adds the two facts that make it one:

1. the marker's owning level (its `level` field) is in the set `gamestate` currently reports as
   loaded. **A marker whose level cannot be matched is never marked** - that is the safety rail;
2. at least one **full** pass over the object array has completed since that level was first seen
   loaded, so "I did not see it" means "I looked at every object in the game while its level was
   streamed in";
3. that pass found no live twin with a usable position and no collected flag (a twin at the origin or
   flagged `dying` is itself collected, so it does not block the rule);
4. and the same held for `markers_absence_rounds` passes in a row.

The mark goes through the normal found tracker, so it persists and can be undone by clicking the
marker on the full map. The F2 panel shows `absence marks N   levels loaded M` - `M = 0` means the
rule can never fire, which is the failure worth seeing. The predicate itself
(`mdb::absence_round_confirms` / `mdb::absence_marks`) is pure and its truth table is in
`tests/markers_test.cpp`.

### Tuning - everything that used to be hard-coded

Added 2026-09-03. Every number below was a literal in `src/`; the defaults are exactly the previous
behaviour, and all of them are clamped on load. **Only `srv_heap_size` needs a restart** (the
descriptor heap is created once, when the overlay first initialises) - everything else is picked up by
F5 or by turning `mod_enabled` off and on. The rows marked *dev* live in
`config_wuchang_minimap_dev.txt`, not in the shipped file.

`tests/markers_test.cpp` is the drift guard, and it checks all of it in both directions:
`keys(config_wuchang_minimap.txt) == Player ∪ Advanced`,
`keys(config_wuchang_minimap_dev.txt) == Dev`,
`{key == "..." literals scraped out of mmstate.cpp} == Player ∪ Advanced ∪ Dev ∪ Legacy`, the four
tiers pairwise disjoint, no removed or renamed key in either file, and the shipped file's
`; ---- PLAYER SETTINGS ----` / `; ---- ADVANCED ----` banner order matching the tier tags key for
key. So no layout, no table and no parser branch can drift away from the others.

Eight keys were added in 0.9.4 - four Player, two Advanced, two Dev:

| key | tier | default | meaning |
|---|---|---|---|
| `found_profile` | Player | `auto` | Which collection file. `auto` runs the save-slot ladder in `src/saveslot.hpp` and writes `wuchang_minimap_found_<key>.txt`; `shared` pins the pre-0.9.4 global file; anything else is used verbatim as the key. The key is sanitised to `[A-Za-z0-9_-]` and capped at 48 chars before it reaches a filename - `sanitise_key` is tested offline against traversal and non-ASCII input, because this is the one setting that decides a path. |
| `first_run_toast` | Player | `1` | The once-per-install 10 s tip naming the bound keys. The sentinel is `wuchang_minimap_firstrun.txt` next to the config. |
| `shrine_list` | Player | `1` | The **Shrines** panel on the full map. |
| `screenshot_key` | Player | `C` | Copies the full map to the clipboard. A plain letter is safe as the default *because* it only fires while the map is open, and the map mode swallows the whole keyboard. |
| `crash_breadcrumb` | Advanced | `1` | Write `wuchang_minimap_last_stage.txt` at every overlay stage transition (`src/breadcrumb.hpp`). |
| `fast_travel_enabled` | Advanced | `0` | Adds a **Travel** action to the shrine list. Off until the in-game reflection self-check has been confirmed - see below. |
| `saveslot_uuid_call` | Dev | `0` | Actually call `GameSaveExecutor::Get Save Slot Value` instead of only reading and logging its reflected signature. |

### The save-slot ladder (`src/saveslot.*`, 0.9.4)

`wuchang_minimap_found.txt` was global, so a second character inherited the first one's collection.
The mod now answers "which save is this?" through four rungs, each **logged with the route that
answered** - a silent fallback is what makes a wrong tracker file impossible to diagnose:

1. **uuid** - `GameSaveExecutor`'s own KV accessor (research §1.2). The `UFunction` is resolved by
   name over three candidate spellings and its **reflected parameter list is read and compared** to
   the predicted shape before anything happens; the signature is logged either way, and the call
   itself stays behind `saveslot_uuid_call` until one in-game dump has confirmed it.
2. **slot path** - `Impl_GameSettingsSaver_C::TickCountSavPath`, a raw `FString` read with no
   `ProcessEvent` at all, parsed for its `GameSlots\<slot>` component.
3. **sav file** - the newest `*.sav` under `%LOCALAPPDATA%\Project_Plague\Saved\*\GameSlots\*`,
   giving `<accountid>_<slot>` straight from the path. Runs on the loop thread before the game
   thread ever pumps, so the first load already has a key.
4. **shared** - the old global file.

On first sight of a slot with no file of its own the shared file is **copied** into it once, and the
copy is logged. A slot switch (main menu -> another save) drops every cache, which re-arms the
resolution, so the tracker swaps files with no restart; a pending write goes to the *old* file first.

### Fast travel is guarded, not disabled (`src/shrines.*`, 0.9.4)

The route is the game's own (research §2.2):
`PlayerModelLibrary_C::PlayerChuanSongFirePoint` on the library CDO, falling back to
`BP_RebornFire_C::ChuanSong` on a resident shrine - **never `K2_TeleportTo`**, which would move the
pawn without the pre-travel save, the reborn info or the `pmaps` level set and land the player in
unstreamed geometry. `lessons.md` forbids calling a `UFunction` with a guessed signature, so the
call is only issued after `uer::func_params()` has read the real parameter list and it matches the
prediction (one to two 16-byte `FString` slots, the first at offset 0). A mismatch refuses and says
so in the panel; a shrine the save has not unlocked refuses too. `ue_min.hpp` declares `UFunction`
as a `UStruct` subclass, which is what makes reading a signature possible at all.

The one press that closes it is **Dump the fast-travel / save-slot recon** on the Debug tab (`src/recon.cpp`): the game mode's components,
every property of `RebornManagerComponent_C` with the three firepoint arrays, **the reflected
parameter lists of the ten functions both routes name**, and the save-slot fallback strings - into a
file, calling nothing. When a name does not resolve it prints the names that did.

### `markers/shrines.json` (schema `wuchang-minimap-shrines/1`)

`tools/markers/extract_shrines.py` reads the game's `DT_FirePoint` DataTable with no `.usmap`:
88 contiguous rows, all named from `MMGame.locres`, all with a `BirthPosition`, 50 of them joined to
a shrine marker by id (the rest are the `bossdoor_*` / `Task*` pseudo-rows, flagged
`"shrine": false`). Three things make it safe: `BirthPosition` is schema slot 0 so it needs no walk
over variable-sized values; the row scan is validated by **contiguity** (all 38 787 payload bytes
accounted for), which rejects the ~54 spurious matches on its own; and the display name is found by
its own evidence - `ShowName`'s locres key survives in the row as ASCII and the locres either has it
or it does not. `src/shrines_db.hpp` is the pure parser and the shipped file's counts are asserted
offline.

Six keys were added in 0.9.3 - three Player, three Advanced:

| key | tier | default | meaning |
|---|---|---|---|
| `theme` | Player | `neutral` | The CHROME: the minimap's frame and disc backdrop, the dark plate behind labels and the compass strip, and the walkable fill. `neutral` is 0.9.2's look; `ink` is a bronze frame on near-black with a warmer parchment fill. **Precedence:** a theme only supplies a colour the config file does not mention, so an explicit `minimap_frame_color` / `minimap_frame_alpha` / `minimap_backdrop_color` / `minimap_backdrop` / `floor_base_color` always wins - resolved after both files are parsed, so the line's position does not matter. Choosing a theme in the F2 panel, by contrast, writes those five keys outright. |
| `palette` | Player | `default` | The MARKER HUE SET. `colorblind` is an Okabe-Ito-derived set and also swaps in colour-blind item-quality tier colours (unless `xray_rarity_colors` is written out). Shapes never change with the palette: every category has its own, which is what keeps two categories apart when a hue is reused - `gly::palette_is_separable()` asserts that offline for every palette. |
| `zoom_key` | Player | `N` | Cycles `minimap_zoom_presets` and wraps. The mouse wheel over the minimap does the same, but **only while the F2 panel is open**: nothing in the mod swallows the wheel during play, so reading it in-world would zoom the minimap *and* work the game's own wheel binding. |
| `minimap_zoom_presets` | Advanced | `13, 26, 52` | The ladder `zoom_key` steps through, in uu per minimap pixel. One to eight numbers between 2 and 400, in any order (sorted, duplicates dropped); a bad entry is named in the log and does not consume a rung. |
| `compass_plate` | Advanced | `1` | `0` = ticks and letters only, each with a one-pixel shadow, so the strip sits more lightly on the game's own top-centre HUD. |
| `highlight_labels_max` | Advanced | `12` | Cap on x-ray **labels**, separate from `highlight_max_draw` (60) which caps glyphs - sixty names do not fit on a screen. Labels go to the nearest markers, never two for glyphs within `highlight_size * 2` of each other, and are laid out top to bottom by `src/label_layout.hpp` so no two boxes overlap; one that had to move draws a leader line to its glyph. |

Two keys were added in 0.9.2, both Player:

| key | default | meaning |
|---|---|---|
| `ui_scale` | `auto` | `auto` = `clamp(back-buffer height / 1080, 1, 4)`; a number pins it. Scales the ImGui font and style **and** every pixel key (`markers_size`, `map_marker_size`, `highlight_size`, `compass_height`, `compass_offset_y`, `minimap_offset_x/y`, `minimap_min_px`, `minimap_arrow_min_px`). `minimap_size` and `compass_width` are fractions and are left alone. |
| `hud_preset` | `custom` | `custom` obeys `minimap_anchor` / `minimap_offset_*` / `compass_anchor` as written (0.9.1's behaviour). `top-left` / `top-right` / `bottom-left` / `bottom-right` move the minimap into that corner **and** put the compass on the same vertical side. |

| key | default | meaning |
|---|---|---|
| `minimap_backdrop`, `minimap_backdrop_color` | 0.86, `6 9 13` | the dark disc the map is drawn on |
| `minimap_frame_color`, `minimap_frame_alpha` | `168 176 186`, 0.85 | the ring / border |
| `minimap_composite_alpha` | 0.85 | the no-height-map composite is drawn weaker than a slice |
| `minimap_min_px` | 72 | floor on the minimap's side length |
| `minimap_arrow_frac`, `minimap_arrow_min_px` | 0.055, 8 | the player arrow |
| `waypoint_size_scale` | 1.05 | waypoint glyph radius, as a multiple of `markers_size` |
| `reader_position_period_ms` | 100 | 10 Hz: the pawn's location and yaw - this also gates the marker scan |
| `reader_resolve_period_ms` | 500 | how often a missing pawn / controller is re-found |
| `reader_widget_sweep_period_ms` | 250 | the full menu-widget sweep (the cheap re-test runs every pump) |
| `reader_transition_cooldown_ms` | 2000 | no blueprint getter is called for this long after a pawn / world change |
| `reader_teleport_jump_uu` | 3000 | a position jump this big in one pump is a fast travel (sprinting is ~70) |
| `reader_chapter_period_ms` | 1000 | how often the streamed level set is walked to name the chapter |
| `reader_log_throttle_ms` | 5000 | rate limit on the reader's repeating log lines |
| `markers_live_grace_rounds` | 2 | rounds a live actor may go unseen before it leaves the live cache - **not** a collected test |
| `map_asset_retire_grace_ms` | 2000 | how long a retired chapter's height planes stay alive after the pointer is cleared |
| `hide_reason_log_ms` | 2000 | rate limit on the `hidden because:` log line |
| `srv_heap_size` | 64 | our SRV descriptor heap (**restart only**) |
| `highlight_camera_resolve_ms` | 500 | how often a missing camera manager is re-found |
| `highlight_compass_period_ms` | 50 | camera read rate when only the compass wants a heading |
| `highlight_getter_period_ms` | 33 | pace of the `ProcessEvent` fallback route |
| `highlight_pov_scan_bytes`, `highlight_pov_bad_reads` | 192, 8 | how far into `CameraCachePrivate` the POV block is looked for, and how many insane reads drop the pin |
| `compass_tick_step_deg` | 15 | minor-tick spacing (45 = labelled, 90 = a cardinal letter) |
| `compass_max_pips` | 32 | cap on marker pips, nearest first |

The full map block:

| key | default | meaning |
|---|---|---|
| `map_key` | `M` | open / close the full map |
| `map_recenter_key` | `R` | recentre it on the player and clear the floor offset |
| `map_zoom` | 30 | zoom the map opens at, in world uu per **screen** pixel (same unit as `minimap_zoom`) |
| `map_zoom_min` / `map_zoom_max` | 4 / 240 | zoom limits (min = most zoomed in) |
| `map_zoom_factor` | 1.15 | zoom multiplier per wheel notch |
| `map_pan_speed` | 900 | keyboard / stick pan, screen px per second (so it feels the same at every zoom) |
| `map_margin` | 0.045 | border around the map, as a fraction of the screen height |
| `map_backdrop` | 0.86 | opacity of the dark backdrop behind it |
| `map_marker_size` | 8.0 | glyph radius on the map, in screen px |
| `map_markers_max_draw` | 4000 | hard cap per frame |
| `map_floor_step` | 200 | uu the height slice moves per floor-adjust press |
| `map_show_all_floors` | 0 | 1 = draw every walkable surface, current storey still opaque |
| `map_slice_px` | 768 | width of the map's own dynamic slice texture |
| `map_slice_hz` | 6 | cap on re-cuts per second (it only cuts when the view changed at all) |
| `map_gamepad` | 1 | poll XInput while the map is open |
| `map_gamepad_deadzone` | 0.22 | fraction of full stick deflection |
| `map_waypoint_persist` | 1 | remember the waypoint in `wuchang_minimap_waypoint.txt` |

### The full map (`M`)

The same asset, the same slicing rule and the same markers as the minimap, at map scale: a north-up
window over the chapter with a dark backdrop, pannable and zoomable. While it is open **the minimap is
hidden** (two views of the same thing is clutter, and the slicer would be cutting two windows a frame)
and the mod takes the mouse and the keyboard.

| input | mouse / keyboard | gamepad |
|---|---|---|
| pan | drag, `WASD`, arrows | left stick |
| zoom | wheel, `+` / `-` | triggers, right stick Y |
| floor slice up / down | `ctrl`+wheel, `E` / `Q`, PageUp / PageDown | RB / LB |
| recentre on the player | `R` (`map_recenter_key`), the Recentre button | Y |
| set the waypoint | right-click, `Space` / `Enter` | A (at the view centre) |
| toggle "found" by hand | left-click a marker, `F` (nearest to the centre) | X |
| close | `M`, `Esc`, the Close button | B |

Every control has a keyboard **and** a gamepad route on purpose: the mouse cursor is the one part of
this that depends on what the game does with the OS cursor while we hold the input, so the map stays
fully usable if the cursor turns out to be locked.

**Memory: the map adds no copy of the asset.** The height planes (~327 MB of RAM for Chapter 1) are
read in place; the map cuts its own small dynamic RGBA texture (768 x ~430 x 2 buffers, ~2.6 MB) out of
the same planes the minimap slices. The cut is *decimated* - one texture pixel covers `step` source
pixels - and covers the visible viewport plus a 30 % margin, so a small pan needs no new cut at all.
Unlike the minimap (which re-cuts 12 times a second because the player is always moving) the map only
re-cuts when something changed: the view left the cut region, the zoom changed, the floor slice moved,
or the player moved far enough to be on another storey - capped at `map_slice_hz`. An idle open map
costs nothing per frame beyond the draw, and the buffer being written is never one the GPU is still
sampling (the same fence rule as the minimap; a busy buffer skips the update instead of stalling
Present).

**Height slicing at map scale** is the minimap's rule plus an offset: `|Z - (feetZ + floor offset)| <=
floor_z_tolerance` is opaque, the nearest surface below / above within `floor_fade_uu` is dimmed, and
the floor adjustment nudges the offset so you can look at the storey above or the dungeon below without
walking there. `map_show_all_floors` widens the fade to infinity for a route-planning view.

**Markers** are the same published draw buffer, the same glyphs and the same category mask the minimap
uses - the row of coloured buttons along the top of the map toggles the *same* `markers_categories`
setting the F2 checkboxes and the config file drive. Hovering a marker shows its class, category, stable
id, found state and distance; a left-click toggles found by hand, which goes through the existing
tracker mailbox to the loop thread and into `wuchang_minimap_found.txt`. Note the live sweep still owns
the truth - un-marking a chest the game reports as `Used` is undone on the next sweep round, which is
correct: the tracker follows the save, not the mod.

**The waypoint** is a single position set with a right-click (or `Space`, or gamepad A). It is drawn on
the map and, edge-clamped with its distance in metres, on the minimap - so it is a compass to it while
you walk. It persists in `wuchang_minimap_waypoint.txt` next to the config: three plain `key = value`
lines, hand-editable, written by the loop thread (the render thread only sets the value). It is
deliberately **not** part of `config_wuchang_minimap.txt`, because that file is only written by
the panel's Save button and a waypoint set during play must survive without anybody pressing Save.

**Nothing latches.** The map closes itself the moment the state that allows it stops being true - a
menu opening, the pawn going away, a level transition, a stale snapshot - and the input swallow
condition *is* `g_map_open`, so closing hands the mouse and the keyboard back to the game on the very
next message. The map key itself is sampled with `GetAsyncKeyState` on the loop thread precisely
because the WndProc hook is swallowing every key while the map is up.

### Why the minimap is (not) on screen

`overlay.cpp`'s `set_hide_reason()` is the single choke point for visibility, and every show condition
is re-evaluated from the live snapshot on **every frame** - there is no latch anywhere in the path. The
F2 debug block prints the current reason as `hidden because: <reason>` (or `minimap: visible`) together
with how long that state has held, and every transition is written to `UE4SS.log` as
`minimap HIDDEN: <reason> (previous state held N ms)`, rate-limited to one line per 2 s. When the reason
is a menu, the block also names the in-viewport widget holding it open.
**F2** opens the panel, **M** opens the full map (**R** recentres it), **F5** reloads the config, the
maps and the markers, and **holding LALT** turns on the x-ray highlight. The F2 panel's **Bindings**
tab rebinds every one of them: click the key, press the new one (Esc cancels), with a per-row reset and
a warning when two actions land on the same key. Accepted hotkey names are F1-F5, F7, F8, any single
letter or digit, TAB, SPACE, ENTER, BACKSPACE, the arrows, INSERT/DELETE/HOME/END/PAGEUP/PAGEDOWN,
NUM0-NUM9 and the numpad operators, MOUSE3-MOUSE5, the L/R modifier keys
(`LALT`/`ALT`/`LSHIFT`/`LCTRL`/...) and `none`; F6 (RenoDX DLSS 5), F9/F11 (engine binds), F10 (game
console) and F12 (Steam) are rejected in code, not merely discouraged in a comment. The panel and the
full map both print the live binding list, built from the config - so a rebound key is what you are
told.

## The x-ray highlight (hold `LALT`)

Hold the key (or the gamepad chord, `LB+RB` by default) and every marker of the enabled categories that
is still uncollected and within `highlight_radius` of the player is drawn **at its position on screen** -
category glyph, name, distance in metres - fading with distance, over the scene. "Through walls" is free
here: the overlay is composited on the finished frame, so there is no occlusion test, no CustomDepth and
no material - nothing that can disagree with the game's render state. Anything off screen or behind the
camera gets an arrow on the screen edge pointing the way to turn (`highlight_edge_arrows`).

**Item quality colours.** Wuchang has **no rarity ladder** - there is no `E_ItemQuality` / `Rarity` /
`Grade` enum anywhere in the paks, no quality word in `MMGame.locres`, and none of the six item row
structs declares such a field. What it *does* have is the colour of the beam a pickup gives off:
`BP_PickupActor_C` picks a `DT_Particle` row (`PickupEffect`, `PickupEffect4..6`, `PickupEffect7..9`)
whose `LightColor` is blue, pink or gold, and which row it picks follows the item's `ItemType`
(`E_ItemType`). `tools/markers/build_items.py` decodes that enum out of the cooked item DataTables and
`extract_markers.py` bakes the resulting tier into every pickup marker as `"rarity"`:

| tier | name | items | default colour (the game's own beam colour, sRGB) |
|---|---|---|---|
| 0 | Common | tools, consumables, arrows, enchanting materials | `ADAFDA` blue |
| 1 | Equipment | weapons, armour, accessories, gems, spells, skills | `DAADC5` pink |
| 2 | Key | quest items and red-mercury upgrade materials | `DAD6AD` gold |

While the key is held, a marker with a tier above 0 is drawn - glyph, label and edge arrow - in that
tier's colour instead of its category colour (`xray_rarity_colors_enabled = 1`,
`xray_rarity_colors = ADAFDA, DAADC5, DAD6AD`). **Tier 0 deliberately keeps its category colour**: it is
every chest, every live actor the offline database does not know and every ordinary consumable, so a
palette that repainted it would recolour most of the screen to say nothing. `markers_rarity_tint = 1`
extends the same tint to the minimap, the full map and the compass pips; it is off by default because
those views are read as a category map. Of chapter 1's 287 pickups, 18 are Equipment and 21 are Key; over all six chapters 47 of 1 086 are
Equipment and 85 are Key.

The derivation was checked against the game's own behaviour, not just asserted: every
`BP_PickupActor_C` whose live `PickupEffectName` was captured in the WuchangRecon world dumps agrees -
11/11 non-default beams (pink for the greataxe, the pendant, the blades, two gems and two armour sets;
gold for `Faint Red Feather`, `Lost Remains`, `Broken Token`) and 7/7 default ones.

It is a **hold, not a toggle**, so there is no visibility state to unstick, and it is gated by exactly
the same evaluation as the minimap (`hud_gate()` in `overlay.cpp` - one function, asked by the minimap,
the compass and the highlight; the minimap keeps ownership of the `hidden because:` readout).

**The projection.** `src/projection.hpp` is dependency-free math with hand-computed tests in
`markers_test`: UE's `FRotationMatrix` basis written out row by row, the horizontal FOV with the aspect
applied to the vertical axis exactly as `FSceneView` does it (`tan(vfov/2) = tan(hfov/2) / aspect`
whenever the viewport is wider than tall), and a behind-the-camera case that never produces a screen
position - a naive divide by a negative depth mirrors the point onto the opposite side of the screen,
which is the classic "the chest behind me is labelled on the wall ahead" bug - but does produce the
direction an edge arrow must point.

**The camera.** `src/highlight.cpp` reads it on the game thread from the local `APlayerCameraManager`
and publishes it through its own seqlock. `CameraCachePrivate` is an `FCameraCacheEntry` whose
`FMinimalViewInfo` starts with Location (3 doubles), Rotation (3 doubles) and FOV (float) - documented,
but *not verified on this build*, and `lessons.md` is unambiguous about recognising a non-reflected
engine struct by an assumed field order. So the offset is **discovered**:

1. call `GetCameraLocation` / `GetCameraRotation` / `GetFOVAngle` once (one SEH-guarded `ProcessEvent`
   each);
2. scan the first bytes of `CameraCachePrivate` for the offset whose six doubles and following float
   match what the getters just said, to 2 uu / 0.5 degrees / 0.5 degrees of FOV;
3. pin it. Every read after that is 56 bytes at a cached offset - cheap enough for `highlight_camera_hz`
   (60 by default) while the key is held, and **nothing at all** while it is up and the compass is off.

If the getters are unavailable the offset is accepted on sanity ranges alone, and if the pinned offset
ever produces eight insane reads in a row it is dropped and re-discovered. The F2 panel's *X-ray
highlight* block names the route, the two offsets, the live pose and the read/reject counts - that block
is the screenshot to take if the labels are ever in the wrong place.

The camera reader is driven from **one clearly-marked hook** inside `markers::game_thread_pump()`, which
already runs only while `gamestate` has a validated gameplay pawn outside the transition cooldown; its
caches are dropped from `markers::drop_caches()`, which is what "the world that owned that object is
gone" already means.

## The compass strip

A heading strip across the top of the screen: N / NE / E ... with 15-degree ticks, a centre reticle, and
bearing pips for nearby markers of `compass_categories` plus the waypoint (which clamps to the strip's
edge with an arrow rather than being culled - being told which way to walk while it is off the strip is
the point). `compass_span_deg` decides how much of the world the strip covers; 360 turns it into a full
ring.

The heading is the **camera's** yaw when a pose is fresh and the pawn's yaw otherwise, so the compass
works with `highlight_enabled = 0` and during the camera reader's warm-up; the F2 panel says which one
is in use. The arithmetic - wrap into `(-180, 180]`, bearings in the mod's `+X` north / `+Y` east frame,
strip positions, tick ranks - is pure and lives in `src/compass.cpp`, which `markers_test` links.

## Markers

Markers come from two halves that are merged by a **stable id**, and the id is the whole design:

* the **static database**, `markers\<chapter>.json` (schema `wuchang-minimap-markers/1`), built offline
  from the cooked levels by `tools/markers`, so a marker exists for an area you have never visited;
* the **live sweep**, a chunked walk of `GUObjectArray` that classifies every object against a table of
  marker classes, and supplies the position and - more importantly - the *state* of every actor
  currently streamed in.

The id is the game's own shrine id for shrines (`digong01` - `BP_RebornFire_C`'s CJK-named
"sitting-Buddha point ID", the only property that distinguishes sibling shrines) and
`<owning level short name>/<actor object name>` for everything else, because that object name is what
`FindAllOf` hands back at runtime.

State, all from the in-world recon (see the task workspace's `wuchang-classes.md`):

| category | classes swept | "found" means |
|---|---|---|
| shrine | `BP_RebornFire_C` | not a per-actor flag at all: **`RebornManagerComponent_C::UnlockedFirepoints`**, a global `TArray<FString>` of shrine ids persisted under `lockqueue`, read raw at 1 Hz by `src/shrines.cpp`. That is what the statistics page's "shrines lit" and the shrine list's `Lit` column show. Shrine markers are still never *auto-marked found* - the tracker follows collectables, not rest points. |
| chest | `BP_treasurebox_C`, `BP_ItemRedBox_C` | `Used == true` (persisted under `SavedStatuKey = statu_use`) |
| pickup | `BP_PickupActor_C` and subclasses (incl. `BP_DropItem_C`) | `dying == true` **or** the actor is parked at `(0,0,0)` |
| door | `BP_NewPuzzlesDoor_C` (`DoorOpen`), `BP_DoorZhong_C` (`Used`) | the door is open |
| fog gate | `BP_Wumen_C` | `Active == true` (inferred from `SavedStatuKey = status_active`; not yet observed passed) |
| ladder / lift | `BP_LadderV2_C`, `BP_WoodenElevator_C` | never - they are navigation aids, not collectables |
| enemy | pawns possessed by `Impl_BaseAIController_C` | n/a - never written to the tracker. A static `enemy` entry is a **spawn point**; the live pawn overwrites its position under the same id, so the two are one marker |

Absence from `FindAllOf` is deliberately **not** evidence of a collect: an unloaded level looks exactly
the same. Only the state flags auto-mark.

### Cost control

`UObjectGlobals::FindAllOf` walks the **whole** object array, so one `FindAllOf` per class means one
full walk per class. The first version of this sweep did exactly that, one class per game-thread pump,
and the in-game measurement was **28.30 ms mean / 51.05 ms peak per pump** - two to three dropped frames
ten times a second.

The sweep now inverts the loop. It walks `GUObjectArray` **once per round**, in slices of
`markers_scan_chunk` slots per pump (`src/scan_sched.hpp` holds the pure slice / wrap / rate arithmetic
and `tests/markers_test.cpp` covers it). Per slot the cost is a bounds-checked
`FUObjectArray::IndexToObject`, an `FUObjectItem::IsValid(false)`, the object's `UClass*`, and one
lookup in a `UClass* -> marker spec` table memoised per class - so the super-chain name walk that
decides "is this a marker class, or a subclass of one" happens once per class per level, not once per
object per round. Everything expensive (`RootComponent` location, the state flag, the `GetFullName`
id) runs only for the handful of objects that matched.

The slice is called from **every** `ProcessEvent` pre-callback while the last validated state stands,
not from the 10 Hz position pump - that caller-side throttle was the reason the old design could not be
made cheap - and it throttles itself on `QueryPerformanceCounter`, because `GetTickCount64`'s ~15.6 ms
granularity is coarser than a one-frame slice period.

Positions are still read raw (`RootComponent` -> `RelativeLocation`), not through
`K2_GetActorLocation`, because a `ProcessEvent` per actor for ~130 pickups plus ~95 enemies inside the
engine's own call stack is not affordable.

The F2 panel prints the two numbers that tune this: `scan pump <last> ms (avg, peak, max)` - what one
pump costs the game thread - and `round <ms> / <pumps> / <objects> of <total>  chunk N` - how long a
full pass took and how much of the array it covered. A `! FindAllOf fallback` suffix means
`FUObjectArray::GetNumElements()` answered 0 and the old per-class path took over.

Glyphs are drawn with `ImDrawList` primitives - no image atlas, so there is no art to keep in sync with
the category list, and each category gets a **shape as well as a colour** (a dimmed "found" marker keeps
its shape long after it has lost its colour contrast). The F2 panel carries the category filter
checkboxes and the per-chapter found/total counts; the same filter is the config file's
`markers_categories` list.

`wuchang_minimap_found_<slot>.txt` (mod folder, next to the config) is the collection tracker: one
stable id per line, sorted, comments allowed, rewritten from the loop thread
`found_save_debounce_ms` after the last change. There is one per save game (see the save-slot ladder
above); `wuchang_minimap_found.txt` without a suffix is the pre-0.9.4 shared file, still used when no
slot can be identified and seeded into a new slot's file once. A deploy never touches either.

## Next steps

- [x] Markers: shrines, chests, pickups, doors, fog gates, ladders, lifts and enemies, drawn as
      ImDrawList glyphs, with the live state sweep, the static `markers/<chapter>.json` database and the
      auto-marking collection tracker. **Not yet verified in-game.**
- [ ] `tools/markers`: the offline extraction that fills `markers/<chapter>.json` from the cooked
      `.umap` cells (the runtime already loads it; only the hand-written sample exists so far).
- [x] Full-screen pannable map (**M**): the same height-sliced asset at map scale with a floor
      adjustment, mouse + keyboard + XInput pan/zoom, the shared marker glyphs and category filter,
      hover tooltips, manual found toggling, and a persistent waypoint that is also drawn edge-clamped
      on the minimap. **Not yet verified in-game.**
- [x] Per-floor map selection from the player's Z. Round 2's surface-ordinal layers read as clutter
      in-world, so round 3 replaced them with a **multi-surface height map sliced on the CPU** into a
      small double-buffered dynamic texture (`|Z - feetZ| <= 200 uu` opaque with a height gradient,
      +/-800 uu dimmed, nothing else drawn).
- [x] X-ray highlight v1 (hold **LALT** / pad **LB+RB**): world-to-screen projection of every nearby
      uncollected marker, with names, distances, distance fade and edge arrows, over the scene. The
      camera pose is read on the game thread from a **self-calibrated** offset inside
      `CameraCachePrivate`. **Not yet verified in-game.**
- [x] Compass strip with cardinal headings, ticks and bearing pips for the waypoint and nearby shrines /
      bosses, hidden by the same gate as the minimap. **Not yet verified in-game.**
- [x] Key hints in the F2 panel and along the bottom of the full map, built from the config so they stay
      truthful after a rebind.
- [ ] X-ray highlight v2 (optional): true silhouettes through `SetRenderCustomDepth` plus a
      post-process material shipped in a tiny pak. Only if v1 is not enough - the game ships no outline
      material we can reuse.
- [ ] Optimisation, not a fix: move the same slicing loop into a **pixel shader**. It needs its own root
      signature, PSO, `D3DCompile` and `ImDrawList::AddCallback` juggling on a ReShade-wrapped
      swapchain, and it would only save the few ms per update and the ~1 MB upload - the CPU slicer
      already has the exactly-correct semantics.
- [x] Per-save-slot collection tracker, a collection statistics page, the full map on the clipboard,
      a first-run tip, an overlay crash breadcrumb and a "disable for this session" switch (0.9.4).
      **Not yet verified in-game** - see `context/extras-test-instructions.md`.
- [x] Shrine list on the full map with names from the game's own `DT_FirePoint` table, and guarded
      fast travel behind `fast_travel_enabled` plus a reflection self-check. **The route is not yet
      confirmed in-game**; one press of the Debug tab's recon-dump button produces everything needed to confirm it.
- [ ] Build the other four chapters' maps and load/unload them by area.
- [ ] Sweep all streaming cells so the runtime navmesh dumps cover a whole region, not just the
      4-6 cells resident around the player.
