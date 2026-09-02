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
src/overlay.{hpp,cpp}      DX12 hooks + ImGui + the minimap and the F2 panel
src/gamestate.{hpp,cpp}    game-thread reader (pawn, view target, widgets)
src/mmstate.{hpp,cpp}      snapshot seqlock, config file, cross-thread log queue
src/mapdata.{hpp,cpp}      maps.json parser + WIC PNG decode
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
tools/navmesh/render.py    tile JSON -> top-down floor PNGs + bounds.json
tools/navmesh/build_map.py tile JSON -> composite + per-floor layers + maps/maps.json
maps/                      the shipped map assets (deployed into the mod folder)
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
> navmesh_dump_key = F3
> ```
>
> `F6`, `F10` and `F12` are **rejected** as hotkeys: F6 is the RenoDX DLSS 5 toggle (pressing
> it in-world GPU-crashed the game on 2026-09-02), F10 the game console, F12 the Steam
> screenshot key.

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
shared. It writes three things (schema `wuchang-minimap-maps/2`):

1. **`chapter1/small.png`** - the Z-shaded RGBA composite of every storey, transparent background.
   Chapter 1 at 0.06 px/uu is 4947 x 4333 px, 3.2 MB PNG, 82 MB as RGBA8. It is now only the *fallback*
   and is not even loaded unless `fallback_use_composite = 1`.
2. **`chapter1/small_f0.png` .. `_f7.png`** - one 8-bit grayscale coverage mask per **surface ordinal**:
   layer k holds, at every pixel, the k-th walkable surface counted from the bottom
   (`rasterize_ordinals()` draws the polygons low Z first and keeps a per-pixel count of how many
   surfaces are already there). A roof and the corridor beneath it are therefore never in the same layer,
   and there are no grid-shaped seams. Each layer is cropped to its own footprint with an 8 px
   transparent margin (ImGui's DX12 sampler is CLAMP) and carries **its own bounds and its own
   `px_per_uu`**; `plan_layer_scales()` coarsens the deepest ordinals until the whole set fits
   `--max-layer-mb`. Chapter 1: **8 layers, 4.2 MB of PNG, 109 MB of `R8_UNORM`** (f0-f4 at 0.06, f5 at
   0.03, f6/f7 at 0.015). Anything deeper than `--max-levels` (8) folds into the last layer - 0.7 % of
   the polygons.
3. **`maps.json`** - per chapter the composite's bounds/scale/mapping, the per-layer entries, and the
   **surface-band grid**: for each 640-uu XY cell, the walkable surfaces at that spot as
   `[gx, gy, band_count, (zmin, zmax, layer_count, layer...)...]`, low Z first, each band naming its
   layers dominant-first. That table is how the runtime answers "which storey is the player on?".
   Chapter 1: 7 894 cells, 23 838 bands, 586 kB.

Why per-pixel ordinals and not floor ranks: measured on Chapter 1, `render.py`'s global floor clustering
leaves 62 % of the 640-uu cells with two or more surfaces in the same rank, and union-find over
"neighbouring cells' Z ranges overlap" merges the chapter into one 231 k-polygon surface. Details in the
script's docstring and in `.workspace/wuchang-minimap/lessons.md`.

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

The minimap is drawn on the foreground draw list: player-centred crop, north-up by default or
rotate-with-player, round (a UV'd triangle fan - no mask needed) or square, configurable zoom, size,
anchor, offsets and opacity, with a yellow player arrow. It hides itself when a menu is open, when the
camera's view target is not the pawn, when the state snapshot is stale, or when the player is outside
every mapped chapter - and the F2 panel prints which of those it was.

`src/gamestate.cpp` reads the state on the **game thread** inside UE4SS's ProcessEvent pre-callback:
pawn location and yaw at 10 Hz via `K2_GetActorLocation` / `K2_GetActorRotation`, the pawn and
controller re-resolved at 2 Hz, and the menu test at 2 Hz (`UWidget::Visibility == Visible` prefiltered
from the reflected byte, then `IsInViewport()`). It publishes an `mm::Snapshot` through a seqlock; the
render thread never touches a UObject.

### Settings

`ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`, plain `key = value`: `enabled`,
`show_minimap`, `minimap_size`, `minimap_zoom`, `minimap_shape`, `minimap_anchor`,
`minimap_offset_x/y`, `rotate_with_player`, `opacity`, `hide_in_menus`, `require_pawn_view`,
`state_stale_ms`, `min_visible_after_state_ok_ms`, `menu_close_show_delay_ms`, `debug_readout`,
`debug_show_panel_on_start`, `panel_key`, `reload_key`, plus the floor block: `show_adjacent_floors`,
`adjacent_floor_opacity`, `floor_z_tolerance`, `floor_hysteresis`, `player_z_offset`,
`floor_fallback_hold_ms`, `fallback_use_composite`.
**F2** opens the panel, **F5** reloads the file and the maps. Only F1-F5, F7 and F8 are accepted as
hotkeys; F6 (RenoDX DLSS 5), F9/F11 (engine binds), F10 (game console) and F12 (Steam) are rejected in
code.

## Next steps

- [ ] Markers: shrines, chests, pickups, fog gates, enemies, with auto-mark.
- [ ] Full-screen pannable map, compass, waypoints, category filters.
- [x] Per-floor map selection from the player's Z (per-pixel surface-ordinal layers + the surface-band
      grid; tolerance 150 uu, hysteresis 100 uu, adjacent storeys dimmed).
- [ ] If the residual multi-layer draw reads as clutter in-world: the height-encoded texture + custom
      pixel shader (`|Z - playerZ| < window`, one ~86 MB RGBA texture, no floors at all).
- [ ] Build the other four chapters' maps and load/unload them by area.
- [ ] Sweep all streaming cells so the runtime navmesh dumps cover a whole region, not just the
      4-6 cells resident around the player.
