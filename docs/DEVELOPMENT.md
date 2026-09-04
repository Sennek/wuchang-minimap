# WuchangMinimap — developer documentation

A UE4SS C++ mod for **Wuchang: Fallen Feathers** (Unreal Engine 5.1.1, Windows x64, DX12).
Toolchain, build, the navmesh and marker pipelines, and the runtime internals. For
installing and using the mod, see the [user README](../README.md).

The mod ships a minimap, a [full map](#the-full-map-m), [markers](#markers), a collection
tracker, a [compass](#the-compass-strip) and an [x-ray highlight](#the-x-ray-highlight-lalt);
the map background is built offline from the game's own navmesh. It also carries the opt-in
[`navmesh_dump`](#navmesh-dumper) module, which locates the game's Recast/Detour navmesh in
memory and writes the streamed-in tiles out as JSON.

Paths here (`src\...`, `tools\...`, `markers\...`, `maps\...`) are relative to the
**repository root**, the parent of this `docs\` folder.

The version is one `#define` in **`src/version.hpp`** — the DLL's `ModVersion`, the start-up
log line, the F2 panel header and `tools/package.ps1` all read it, and
`package.ps1 -Version x.y.z` is the only thing that changes it.

---

## Build

The order matters, and none of it is discoverable: this project cannot `include("RE-UE4SS")`
the way the UE4SS docs describe (see [Why an import library?](#why-an-import-library)), so
the toolchain is assembled by hand once. A clean Windows 10/11 x64 box is steps 1-6; after
that, `.\build.ps1`.

### 1. Visual Studio with the C++ toolchain

<https://visualstudio.microsoft.com/downloads/> — any edition. Install the **"Desktop
development with C++"** workload, which also brings the Windows SDK (`d3d12.h` / `dxgi.h`).

The mod is built and tested with **MSVC toolset 14.40.33807**. `build.ps1` prefers that
toolset and otherwise takes the newest on the box, warning that the choice is untested
(`tools\vs_detect.ps1` does the discovery, via `vswhere`). To pin it:

```powershell
.\build.ps1 -Toolset 14.40.33807
$env:WUCHANG_MSVC_TOOLSET = '14.40.33807'    # or set it once for the session
```

If 14.40.33807 is not offered, it is under *Individual components* in the VS Installer as
"MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.40-17.10)".

> VS 2026 Insiders ships **no `vcvars64.bat`**, so `gen_ue4ss_importlib.ps1` enters the
> developer environment through `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell`
> rather than a batch file. It works the same on a normal VS install.

### 2. xmake 3.1.1

<https://github.com/xmake-io/xmake/releases/tag/v3.1.1> — the portable
`xmake-v3.1.1.win64.zip` is enough. **3.1.1 is the only version this project builds with**,
and `xmake.lua` pins it with `set_xmakever("3.1.1")`.

`build.ps1` looks for `xmake.exe` at `F:\Tools\xmake\xmake.exe` and then on `PATH`. Point it
at yours with either:

```powershell
.\build.ps1 -Xmake xmake.exe            # anything on PATH
$env:WUCHANG_XMAKE = 'C:\tools\xmake\xmake.exe'
```

### 3. Install UE4SS into the game

**UE4SS for Wuchang: Fallen Feathers**, Nexus mod **384**, the **`experimental-latest`**
asset — build **`v3.0.1-1111-g97b7e501`**. It is not interchangeable: the mod links against
this DLL's export table, so any other build fails to load. Unzip it into

```
<Game>\Project_Plague\Binaries\Win64\
```

Then set `HookInitGameState = 0` in `ue4ss\UE4SS-settings.ini`, or the game crashes a third
of a second into loading with or without this mod.

Step 5 needs the installed `UE4SS.dll` whether or not you intend to run the game.

### 4. Clone RE-UE4SS at the matching commit

The build needs UE4SS's **headers** at the commit the installed DLL was built from. The
`-g97b7e501` suffix in the release name *is* that commit:

```powershell
git clone --no-recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS F:\Tools\RE-UE4SS
cd F:\Tools\RE-UE4SS
git checkout --recurse-submodules=no 97b7e501c19d8b2b7c662feee73aaa0dc1f0a4d1
```

**No submodules are needed.** RE-UE4SS has two — `deps/first/Unreal` and
`deps/first/patternsleuth` — and neither is on this project's include path; every directory
`xmake.lua` asks for (`UE4SS/include`, `UE4SS/generated_include`, `deps/first/*/include`) is
tracked directly in the repository (`git ls-files --error-unmatch deps/first/DynamicOutput/include`).

`deps/first/Unreal` **cannot be cloned** by anyone outside the Epic Games GitHub
organisation: it points at the private `Re-UE4SS/UEPseudo`, derived from Unreal Engine
source. That is why this project links against a synthesised import library
(see [Why an import library?](#why-an-import-library)).

To take the submodules anyway, rewrite the protocol rather than editing your global git
config, and expect `Unreal` to fail:

```powershell
git -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive
```

`build.ps1` checks for `UE4SS/include/Mod/CppUserModBase.hpp` under the root and reports a
wrong path. Point it at your clone with:

```powershell
.\build.ps1 -Ue4ssRoot D:\src\RE-UE4SS
$env:WUCHANG_UE4SS_ROOT = 'D:\src\RE-UE4SS'
```

### 5. Generate the import library (once per UE4SS build)

No UE4SS release asset carries an import library, so one is synthesised from the installed
DLL's export table:

```powershell
.\tools\gen_ue4ss_importlib.ps1 -Ue4ssDll '<Game>\Project_Plague\Binaries\Win64\ue4ss\UE4SS.dll'
```

It finds Visual Studio itself (`vswhere`; override with `-VsPath` or `WUCHANG_VS_PATH`), runs
`dumpbin /exports`, writes a `.def` and feeds it to `lib.exe`:

```
Wrote <repo>\sdk\UE4SS.def (4239 exports)
Wrote <repo>\sdk\lib\UE4SS.lib (2213 KB)
```

`-Ue4ssDll` has **no default**: it must be the DLL from the game folder you will run.
`WUCHANG_UE4SS_DLL` works instead of the parameter. Both outputs are committed, so this step
is only needed when moving to a different UE4SS build — see
[Why `sdk/lib/UE4SS.lib` is committed](#why-sdklibue4sslib-is-committed).

### 6. Build

```powershell
.\build.ps1
```

or, spelled out — this is what the wrapper runs:

```powershell
xmake f -m Game__Shipping__Win64 -p windows -a x64 `
        --vs_toolset=14.40.33807 --ue4ss_root=F:/Tools/RE-UE4SS -y
xmake -j 8
xmake build markers_test
xmake run markers_test <repo>\markers
```

| Want | Command |
|---|---|
| Debug configuration | `.\build.ps1 -Mode Game__Debug__Win64` |
| Full rebuild | `.\build.ps1 -Rebuild` |
| Skip the offline tests | `.\build.ps1 -NoTests` |

> `xmake clean --all` discards xmake's cached Visual Studio environment along with the
> intermediates, so it must run **before** `xmake f`, never after — otherwise the next
> compile starts with an empty `INCLUDE` and dies on `#include <memory>`.
> `.\build.ps1 -Rebuild` does it in that order.

### Expected output

```
build\windows\x64\Game__Shipping__Win64\main.dll     4.0 MB
build\windows\x64\Game__Shipping__Win64\main.pdb    24.3 MB
```

and, from the tests, `0 failure(s)`. A full rebuild takes about 20 seconds. `main.dll`
exports `start_mod` / `uninstall_mod` and imports 16 symbols from `UE4SS.dll`.

Our own targets are built with `set_warnings("all", "error")` — `/W3 /WX`, so a warning in
`src/` or `tests/` fails the build. `third_party/` is compiled by its own targets at the
default warning level. The mod DLL also gets `/guard:cf`, `/DYNAMICBASE`, `/HIGHENTROPYVA`
and `/PDBALTPATH:%_PDB%`; see `hardened_link()` in `xmake.lua` for why CFG is safe in a
process we hook, and for two xmake flag-plumbing traps.

### Troubleshooting a first build

| Symptom | Cause |
|---|---|
| `'<path>' does not look like an RE-UE4SS checkout` | step 4, or `-Ue4ssRoot` points at the wrong folder |
| `sdk\lib\UE4SS.lib is missing` | step 5 has not been run and the file is not in your clone |
| `No MSVC x64 toolset found` | step 1, or the C++ workload was not selected |
| `xmake not found at ...` | step 2, or pass `-Xmake` |
| `#include <memory>` cannot be found | `xmake clean --all` ran *after* `xmake f`; re-run `.\build.ps1 -Rebuild` |
| `static_assert ... requires compiling with /utf-8` | building without `xmake.lua`'s flags; fmt's `base.h` hard-requires `/utf-8` |
| Link errors on `RC::` symbols | the RE-UE4SS checkout and `sdk/lib/UE4SS.lib` are from different UE4SS builds; redo steps 4 and 5 together |

### The offline tests

`build.ps1` also builds and runs them (`-NoTests` skips). They link only the pure sources, so
they need neither UE4SS nor Direct3D and run with the game closed:

```
xmake build markers_test
xmake run   markers_test markers      # the repo's markers\ dir
```

They cover the `markers/<chapter>.json` loader (the shipped files, plus every way a file can
be wrong), the category-name <-> bitmask mapping shared by the config file and the F2
filters, the found-file round-trip, the config tier tables and the in-place config rewrite,
the `maps.json` parser and the sparse height-plane store decoded from the shipped PNGs, the
full map's viewport transform and its inverse, the zoom clamp and step, the waypoint file,
the x-ray projection against hand-computed screen coordinates, the compass arithmetic, the
scan scheduler and the label layout. Anything checkable without launching the game is checked
there.

## Install (development)

Two install paths, not interchangeable:

| | `deploy.ps1` | `tools\package.ps1` |
|---|---|---|
| For | this machine, while developing | a release a player downloads |
| Writes | straight into the Steam folder | `dist\` only |
| Ships | `main.dll` **and `main.pdb`** | `main.dll` only |
| Configs | never overwrites an edited one (`-ForceConfig` to force) | the pristine shipped defaults |
| Extras | leaves the mod's runtime output alone | asserts none of it is in the package |
| Checks | none | full smoke check + zip round-trip |

```powershell
.\deploy.ps1
```

copies the DLL to

```
E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers\
    Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\dlls\main.dll
```

and creates an empty `enabled.txt` in `...\Mods\WuchangMinimap\`, which is UE4SS's "load this
without touching `mods.txt`" opt-in. `deploy\ue4ss\Mods\WuchangMinimap\` mirrors the same
layout so the repo shows what gets installed. `-GameRoot` points it elsewhere; `-Force`
installs before UE4SS is present.

---

## Release packaging

The full procedure is **`docs/RELEASE.md`**:

```powershell
.\tools\package.ps1 -StampOnly -Version 1.0.1   # rewrite version.hpp + xmake.lua, stop
git commit -am "release 1.0.1"; git tag v1.0.1
.\tools\package.ps1                             # build, assemble, check, zip
```

`-StampOnly` exists because the script refuses a dirty tree (`BUILD_INFO.txt` names a commit
hash) while `-Version` dirties it, so stamping and packaging in one run records the commit
from *before* the stamp.

`package.ps1` runs `build.ps1`, then assembles `dist\WuchangMinimap-<version>\` — a tree that
mirrors exactly what a player copies into `...\Project_Plague\Binaries\Win64\` — and zips it:

```
WuchangMinimap-1.0.0\
  INSTALL_GUIDE.html                   from tools\INSTALL_GUIDE.html, @@VERSION@@/@@DATE@@ substituted
  CHANGELOG.md                         from tools\CHANGELOG.template.md
  README.md  LICENSE  THIRD_PARTY_NOTICES.md    copied from the repo root
  BUILD_INFO.txt                       version, commit, branch, mode, DLL size, UE4SS build
  ue4ss\Mods\WuchangMinimap\
    dlls\main.dll                      no .pdb
    maps\maps.json, maps\chapter1..5\*.png
    markers\chapter{1..5,dlc}.json     chapter1.sample.json is excluded
    markers\shrines.json               required; markers\items.json when present
    config_wuchang_minimap.txt
    config.ini
    enabled.txt                        empty; UE4SS's "load me" opt-in
```

**`dist\WuchangMinimap-<version>-symbols.zip`** comes out beside it with `main.pdb` and the
same `BUILD_INFO.txt`. It is the only way to read a crash dump from that build, since
`main.pdb` otherwise lives only in the gitignored `build\` folder. It is staged in a temp
folder so a `.pdb` never touches the package tree, and it is never the main upload.

Nothing else ships: no `main.pdb`, no `navmesh\` dumps, no player-state files
(`wuchang_minimap_found.txt`, `wuchang_minimap_waypoint.txt`) — the script fails if any turn
up in the tree.

**Smoke check**, before the zip is created:

* `maps.json` parses, is schema `wuchang-minimap-maps/4` and lists five chapters;
* every `image` and every `height_planes` entry it names exists and is non-empty — the list
  `mapdata.cpp` walks at start-up — and a PNG in `maps\` the manifest does *not* name is
  warned about as dead download weight;
* the five chapter marker manifests are present and are schema `wuchang-minimap-markers/1`;
* `shrines.json` is present, is schema `wuchang-minimap-shrines/1` and holds at least 40 real
  shrines;
* `main.dll`, both config files and `enabled.txt` are present;
* an **allow-list** on the package root and the mod folder, so any file the script has not
  been taught about is a leak by definition.

**Consistency check** (`tools\check_release.ps1`), over the assembled tree as part of the same
step and runnable on its own against the repo:

* one version across `src\version.hpp`, `xmake.lua`'s `set_version`, the top `## x.y.z`
  changelog heading and the package folder name;
* one UE4SS build string across `BUILD_INFO.txt`, `README.md`, `THIRD_PARTY_NOTICES.md`,
  `INSTALL_GUIDE.html` and `docs\NEXUS.md` — a *different* `v3.0.x-...-g...` string anywhere
  is a failure, not just a missing one;
* no unfilled placeholders: `@@...@@`, `<ALLCAPS>` template slots, `TODO`/`FIXME`;
* every relative link in a shipped document resolves to a file in the package.

**Zip round-trip**: entry count and every entry's uncompressed length are compared against the
tree on disk, and `maps.json` is decompressed and re-parsed, so a corrupt stream cannot pass
on metadata alone.

`-Version x.y.z` rewrites `src/version.hpp` and `xmake.lua`'s `set_version` together; without
it the script reads the header and warns if the two have drifted. `-NoBuild` packages the
existing `build\` output, for iterating on the packaging script. `-OutDir` writes somewhere
other than `dist\`; both `dist\` and `dist-test\` are gitignored.

---

## Layout

```
src/dllmain.cpp            RC::CppUserModBase subclass, start_mod/uninstall_mod
src/overlay.{hpp,cpp}      DX12 hooks + ImGui + the minimap, the full map and the F2 panel
src/gamestate.{hpp,cpp}    game-thread reader (pawn, view target, widgets)
src/mmstate.{hpp,cpp}      snapshot seqlock, config file, cross-thread log queue
src/mapdata.{hpp,cpp}      chapter residency + the sparse height-plane store
src/mapmanifest.hpp        maps.json parser (pure; tested offline)
src/pngdecode.hpp          the WIC PNG decode, shared with markers_test
src/markers_db.{hpp,cpp}   PURE marker model: markers/<chapter>.json, category masks,
                           the found-file round-trip. No Windows, no UE4SS - which is
                           what lets tests/markers_test.cpp link it
src/markers.{hpp,cpp}      the runtime half: the chunked game-thread object sweep, the merge
                           with the static DB, the found tracker's file I/O
src/scan_sched.hpp         PURE scan scheduler: which object-array slots this pump,
                           has the round wrapped, is it time yet
src/mapview.{hpp,cpp}      PURE full-map layer: the north-up viewport transform and its
                           exact inverse, the zoom clamp / step, zoom-to-fit, the
                           minimap's zoom-preset ladder, the waypoint file round-trip
src/glyphs.hpp             PURE shape-per-category and hue-per-category tables plus the
                           themes; markers_test asserts that no two categories share a
                           shape AND a colour, for every palette
src/label_layout.hpp       PURE greedy label placement for the x-ray highlight: an
                           occupied-rectangle list, push each box down until it clears,
                           refuse past a cap
src/gamepad.{hpp,cpp}      XInput, dynamically loaded, polled on the LOOP thread only
src/projection.hpp         PURE world -> camera -> NDC -> screen math for the x-ray
                           highlight (UE basis, horizontal FOV, behind-camera case)
src/compass.{hpp,cpp}      PURE compass arithmetic: yaw wrap, bearings, strip positions,
                           cardinal ticks
src/highlight.{hpp,cpp}    the x-ray highlight's game-thread half: finds the
                           PlayerCameraManager, calibrates the POV offset inside
                           CameraCachePrivate against the camera getters, then publishes
                           the pose through its own seqlock
src/version.hpp            the single WUCHANG_MINIMAP_VERSION define
src/config_keys.hpp        the one key -> tier table
src/config_rewrite.hpp     PURE in-place config rewrite: values only
src/json.hpp               the one JSON reader, shared by mapdata and markers
src/uereflect.hpp          cached property offsets and UFunction calls
src/navmesh_dump.{hpp,cpp} dtNavMesh discovery + tile walker + JSON writer
src/mem.{hpp,cpp}          VirtualQuery + SEH guarded raw reads
src/ue_min.hpp             hand-written RC::Unreal ABI declarations, see below
sdk/shim/GUI/GUI.hpp       stand-in header, see below
sdk/UE4SS.def              UE4SS.dll export table, generated
sdk/lib/UE4SS.lib          import library, generated
third_party/imgui/         Dear ImGui v1.92.9b + backends/{dx12,win32} + misc/cpp
third_party/minhook/       MinHook v1.3.3
third_party/fmt/           fmt 11.2.0, headers only (FMT_HEADER_ONLY)
tools/gen_ue4ss_importlib.ps1
tools/package.ps1          the RELEASE packager: build + assemble + smoke check + zip
tools/check_release.ps1    version, UE4SS build string, placeholder and link consistency
tools/INSTALL_GUIDE.html   the player-facing guide; @@VERSION@@ / @@DATE@@ are substituted
tools/CHANGELOG.template.md the changelog dropped at the package root
tools/navmesh/render.py    tile JSON -> top-down floor PNGs + bounds.json
tools/navmesh/build_map.py tile JSON -> composite + multi-surface height planes + maps/maps.json
tools/navmesh/mapfmt.py    the ON-DISK format: schema, palette PNG, 12-bit height codes
tools/navmesh/repack_maps.py re-encode a shipped maps/ tree (no dumps needed)
tools/navmesh/slice_preview.py the runtime's height-slicing rule, offline, for any (x, y, z)
maps/                      the shipped map assets (deployed into the mod folder)
markers/                   the static marker database (deployed into the mod folder);
                           chapter1.sample.json documents the schema by hand
tests/markers_test.cpp     offline tests - `xmake run markers_test markers`
tools/lua-recon/           WuchangRecon Lua recon mod + its offline mock harness
deploy/ue4ss/Mods/WuchangMinimap/
```

### Vendored versions

Upstream URLs, tags, the files copied and how to re-verify a tree against upstream:
**`third_party/VENDORING.md`**. None of the three is modified.

| Library | Version | Why that one |
|---|---|---|
| Dear ImGui | **v1.92.9b** | Our own ImGui context on our own DX12 Present hook, independent of the v1.92.1 UE4SS links internally. |
| MinHook | **v1.3.3** | Statically linked, so `MH_ALL_HOOKS` can never touch UE4SS's own hooks. |
| fmt | **11.2.0** | `DynamicOutput/Output.hpp` includes `<fmt/core.h>`, and UE4SS pins fmt 11.2.0. |

---

## Why an import library?

The official flow is to `add_subdirectory(RE-UE4SS)` / `includes("RE-UE4SS")` and link
against a `UE4SS` target built from source. **That is not possible here.**
`RE-UE4SS/deps/first/Unreal` points at `git@github.com:Re-UE4SS/UEPseudo.git`, a private
repository derived from Unreal Engine source that needs Epic Games GitHub organisation
membership. Without it UE4SS cannot be compiled at all. The `zDEV-UE4SS_*.zip` release asset
does not help: it is the normal release plus `UE4SS.pdb`, with no headers and no import
library.

So instead:

1. **Headers** come from a plain `git clone` of RE-UE4SS at `97b7e501`, the commit the
   installed `UE4SS.dll` was built from. Only `deps/first/Unreal` and
   `deps/first/patternsleuth` fail to clone, and nothing on the C++ mod API path needs them.
2. **Linking** goes through `sdk/lib/UE4SS.lib`, synthesised by
   `tools/gen_ue4ss_importlib.ps1`: `dumpbin /exports` on that same `UE4SS.dll` (4239
   exports), a `.def`, then `lib /def: /machine:x64`.

Headers and DLL come from one commit, so the ABI matches by construction, and
`RC::CppUserModBase` is used verbatim from the real header.

### Why `sdk/lib/UE4SS.lib` is committed

Both `sdk/UE4SS.def` (374 KB of text) and `sdk/lib/UE4SS.lib` (2.2 MB) are tracked, because
regenerating them needs **the exact `UE4SS.dll` from an installed copy of the game** — a file
that is not in this repo, is not downloadable without Nexus, and does not exist on a CI
runner. Committing the library is what makes `git clone` + `.\build.ps1` work. The `.def` is
also a greppable record of the ABI this build is tied to: `src/ue_min.hpp` names the symbols
it must match.

`sdk/lib/UE4SS.exp` is a `lib.exe` byproduct, is not needed to link, and is gitignored.

**To regenerate**, when moving to a different UE4SS build:

```powershell
.\tools\gen_ue4ss_importlib.ps1 -Ue4ssDll '<Game>\Project_Plague\Binaries\Win64\ue4ss\UE4SS.dll'
```

Do it **together with** re-checking out RE-UE4SS at the new build's commit (step 4): headers
and import library must come from the same UE4SS, or the result is link errors on `RC::`
symbols at best and an ABI mismatch at worst.

`UE4SS.def` regenerates **byte-identically** from the same DLL, so a diff on it means the DLL
changed. `UE4SS.lib` does not — `lib.exe` embeds a timestamp — so a `.lib` diff is not
evidence of anything; read the `.def`.

### Two things this setup depends on

**`sdk/shim/GUI/GUI.hpp`.** `<Mod/CppUserModBase.hpp>` includes `<GUI/GUITab.hpp>` ->
`<GUI/GUI.hpp>` -> `<GUI/LiveView.hpp>` -> `<Unreal/UFunctionStructs.hpp>`, i.e. UEPseudo.
`GUITab.hpp` uses nothing from `GUI.hpp`, so `sdk/shim` sits **first** on the include path
with a near-empty `GUI/GUI.hpp` that breaks the chain. Every other UE4SS header resolves to
the real checkout. If UE4SS ever becomes buildable here, drop `"sdk/shim"` from
`ue4ss_includedirs()` in `xmake.lua`.

**The CRT must be `/MD`.** `UE4SS.dll` imports `MSVCP140.dll`, `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll`, and the mod API passes `std::string_view`, `std::vector` and
`std::unique_ptr` across the DLL boundary. A `/MT` mod gets its own heap and its own `std::`
internals and crashes. `xmake.lua` pins `set_runtimes("MD")` for every target.

### Consequences to keep in mind

- The mod is tied to UE4SS `v3.0.1-1111-g97b7e501`. Upgrading means: re-checkout
  `F:\Tools\RE-UE4SS` at the new commit, re-run `tools\gen_ue4ss_importlib.ps1` against the
  new `UE4SS.dll`, rebuild, and re-check `src/ue_min.hpp` against the new `sdk/UE4SS.def` —
  every declaration there names the symbol it must match.
- `UE4SS_ENABLE_IMGUI()` (sharing UE4SS's own ImGui context, e.g. for `register_tab`) is
  unusable: it lives in `UE4SSProgram.hpp`, which needs UEPseudo, and it would require our
  vendored ImGui to be exactly v1.92.1.
- `Output::send<LogLevel>(...)` is a header template: the formatting runs inside `main.dll`
  and only `Output::DefaultTargets::get_default_devices_ref()` is imported. That is why fmt
  is vendored header-only and why its version has to match UE4SS's.

---

## Navmesh dumper

`src/navmesh_dump.cpp` finds the game's Recast/Detour navmesh in memory and writes the
currently streamed-in tiles to

```
ue4ss\Mods\WuchangMinimap\navmesh\<agent>\tiles_<yyyymmdd_hhmmss>.json
```

one directory per `ARecastNavMesh` actor (`Small` / `Big` / `BitFat` / `Giant`, agent radii
34 / 60 / 90 / 120).

> **The runtime dumper is off by default.** The map background is built offline from the paks
> (`tools/navmesh/offline`) — the whole game in ~2 minutes, with 0 % false positives against
> the game's own navigation probes — so the runtime path covers only cells the paks do not
> carry and navmesh carved at runtime. Turn it on for a session in
> `ue4ss\Mods\WuchangMinimap\config.ini`:
>
> ```ini
> [navmesh]
> navmesh_dump = 1
> ```
>
> With it on, **Dump the live navmesh tiles** on the F2 panel's Debug tab forces a dump.
> There is no hotkey: a memory scan that writes files must not be startable by a stray key
> press.

When enabled, a dump happens 3 s after the set of live tiles stops changing — once per area
as you walk — and on demand on **F3** (or `CTRL+F3`), which also writes a `probe_<ts>.json`
diagnostics file when it found nothing. `navmesh\last_stage.txt` records the stage the dumper
is in, rewritten and closed at every stage so it survives a crash that eats the log buffer.
Only 4-6 of the game's 10 240-uu streaming cells are ever resident, so a full map is the
union of many dumps and the renderer merges them.

### Threading rules (these bind the overlay too)

`CppUserModBase::on_update` runs on **UE4SS's event-loop thread**, not the game thread. So:

* every UObject traversal (`FindAllOf`, reflection) and every raw read of an engine
  allocation happens in a game-thread pump registered with
  `RC::Unreal::Hook::RegisterProcessEventPreCallback`;
* that pump does **raw memory work only** — it queues log text and parks results, because C++
  iostreams and the C++ locale fault when touched from this game's game thread;
* `on_update` drains the log queue and does all file and JSON writing;
* there is **no `std::mutex`** anywhere in the mod — `std::mutex::try_lock` faults against the
  MSVCP140 loaded in this process. Locking is a header-only `std::atomic_flag` spinlock plus a
  non-blocking `std::atomic<bool>` single-flight exchange.

### Nothing is hardcoded, everything is validated

No struct offset is assumed. Each step derives a candidate, proves it against something the
recon pass measured, and logs the decision:

| Step | How it is found | How it is validated |
|---|---|---|
| the actors | `UObjectGlobals::FindAllOf("RecastNavMesh")`, re-polled every 2 s | 0 results is normal (main menu) and reported once |
| `AgentRadius`, `TileSizeUU`, `PolyRef*Bits` | reflection: `FProperty::GetOffset_Internal()` by name | printed; `TileSizeUU` becomes the tile-size expectation below |
| `FPImplRecastNavMesh*` | scan the actor from the end of its reflected properties (`max(offset+size)` over the super-struct chain) to `GetStructureSize()`, 8 bytes at a time | the target's **second** pointer must be the actor itself — that is `FPImplRecastNavMesh::NavMeshOwner` |
| `dtNavMesh*` | first field of that struct | `dtNavMeshParams`: `tileWidth == tileHeight == TileSizeUU`, `maxTiles ∈ [1, 65536]`, finite origin — **float and double (`dtReal`) layouts both tried**, the winner logged |
| `dtMeshTile[]` and `sizeof(dtMeshTile)` | scan the pointer slots after `m_params` for an array containing pointers to `DNAV` headers; the **gcd of the hit spacing** gives the stride | stride in [96, 1024], 8-aligned, and every hit ≡ 8 mod stride (the `header` field is always at `+8`) |
| `dtMeshHeader.bmin/bmax` | search for six consecutive `dtReal` forming a box no bigger than one tile | scored: +4 each for `bmin.x/y == orig + index * tileWidth`, +3 each for the preceding `walkableHeight/Radius` matching `AgentHeight`/`AgentRadius` |
| `polyCount` / `vertCount` | search the int block before `bmin` | accepted **only** if every vertex lies inside `bmin..bmax` and every `dtPoly` has 3..6 vertices with in-range indices; `DT_VERTS_PER_POLYGON` 6/8/4 all tried |

UE modifies `dtMeshTile` and `dtMeshHeader` (off-mesh segments, clusters, the `layer` field),
so the stride and the field offsets are measured rather than taken from the Recast headers.
Once learned they are cached per agent, re-validated cheaply, and printed as a single
`PIN LINE`.

Every raw read goes through `mem::read` — `VirtualQuery`, then an SEH-guarded `memcpy`
(`src/mem.cpp`) — so a wrong guess yields a log line, never a crash.

### `src/ue_min.hpp` — reflection without UEPseudo

The mod uses the full `RC::Unreal` reflection API without the headers for it. `UE4SS.dll`
exports the whole API, and an MSVC mangled name depends only on namespace, class name,
function name, parameter types, cv/ref qualifiers and the **access specifier** — never on
class layout. So `ue_min.hpp` re-declares the members we call inside deliberately empty
classes in `namespace RC::Unreal`, each annotated with the exact symbol from `sdk/UE4SS.def`
it must match. Two rules for extending it: mirror the real single, non-virtual, offset-0
inheritance chain so `this` needs no adjustment, and keep `FField::GetNext` **private** with
a friend accessor, because it is private in UEPseudo and `A…` vs `Q…` is part of the symbol.

## Rendering the dumps

```powershell
.\deploy.ps1 -Pull    # game -> tools\navmesh\dumps\<agent>\*.json

python tools\navmesh\render.py --input tools\navmesh\dumps --out tools\navmesh\out --debug
python tools\navmesh\render.py --synthetic --out out_synthetic --debug   # self-test
```

`tools/navmesh/render.py` (Pillow only) merges every dump — the richer copy wins per
`(tile x, tile y, layer)` — fills the polygons, splits stacked geometry into floors and
writes `out\<agent>_floor<i>.png` plus `out\bounds.json`. North-up mapping:
`u = (world_Y - min_y) * px_per_uu`, `v = (max_x - world_X) * px_per_uu`. A polygon's floor
index is the rank of its Z band inside its own streaming cell, so floor 0 is the lowest
surface everywhere. `--probe out\navprobe_*.csv` renders the Lua mod's F11
`ProjectPointToNavigation` grids through the identical mapping, for checking alignment.

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

`build_map.py` imports `render.py`, so the loader, the richest-copy dedupe and the flat-plane
filter are shared, and `mapfmt.py`, which owns the on-disk format (schema string, palette
encoder, height quantisation) so a fresh build and a re-encode cannot disagree. It writes
three things (schema `wuchang-minimap-maps/4`):

1. **`chapter1/small.png`** — the Z-shaded composite of every storey, transparent background,
   as a **256-colour palette PNG** with a tRNS array. The render is flat-filled from a
   five-stop grey ramp with a darkened outline per polygon, so the picture only ever uses
   643..651 distinct RGBA values and its alpha is binary (0 background, 235 fill):
   quantising to 255 colours + transparent costs at most 3/255 on one channel (0.19/255
   mean) and saves 44 % of the bytes. Chapter 1 at 0.06 px/uu is 4947 x 4333 px, **1.5 MB
   PNG**, 82 MB as RGBA8 in VRAM. It is the *fallback* for a chapter with no height planes
   and is not loaded unless `fallback_use_composite = 1`.
2. **`chapter1/small_h0.png` .. `_h7.png`** — the **multi-surface height map**: eight 16-bit
   grayscale PNGs where plane k holds, at every pixel, the Z of the k-th walkable surface
   from the bottom. `code = 1 + round((Z - z_min) / (z_max - z_min) * 4094)`, and **code 0
   means "no surface"**. All eight share one size, one `px_per_uu` and one set of bounds.
   Chapter 1: **4947 x 4333, 5.2 MB of PNG, 86 MB of RAM** (`z_min` -11649, `z_max` 38871,
   12.34 uu per step). Properties that matter:
   * **Fill only, no outlines.** Coverage is "the sample point is inside the polygon, or
     within `--seam-px` (0.5) of its boundary", which closes the sub-pixel gaps. The ~1 px
     overlap that creates is absorbed by `--merge-tol` (120 uu): a polygon's pixels *join*
     the surface already at that pixel when the Z is that close, instead of opening a new
     slot, so no phantom storey appears along an edge.
   * **Z is interpolated per vertex** (barycentric over the polygon's fan triangles, clamped
     to the polygon's own vertex Z range), so a ramp stores a smoothly varying Z and the
     runtime's gradient comes out smooth rather than per-polygon flat.
   * Slots are sorted ascending, so `z0 <= z1 <= ... <= z7` per pixel. **Eight** slots: four
     hold 93 % of a chapter's lit pixels but only 50 % in the Digong-spiral /
     Hanguang-temple block (up to eleven surfaces at one pixel), where slicing at the
     temple's feet Z then costs two thirds of the floor. Eight is within 2 % of sixteen. The
     top slot is the **overflow** slot and keeps the *highest* Z. `--max-surfaces 4` halves
     the RAM.
3. **`maps.json`** — per chapter the bounds, scale, mapping, `z_min` / `z_max` / `z_bits` /
   `z_code_max` / `z_step_uu`, `max_surfaces`, the `height_planes` list (the array index IS
   the surface slot) and the measured tile-store cost (`height_tiles_128`,
   `height_tile_ram_bytes`). 11 kB.

### Formats and sizes (schema /4)

| | chapter 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| `px_per_uu` | 0.0600 | 0.0553 | 0.0483 | 0.0523 | 0.0320 |
| pixels | 4947x4333 | 4821x4613 | 3812x5835 | 4898x4541 | 3029x7342 |
| composite PNG | 1.51 MiB | 1.24 | 1.35 | 1.00 | 0.79 |
| height planes, PNG | 5.15 MiB | 4.08 | 4.14 | 3.09 | 3.79 |
| Z step | 12.34 uu | 12.23 | 12.44 | 7.26 | 3.98 |
| 128-px blocks lit | 2766/10608 | 2215/11248 | 2366/11040 | 1644/11232 | 1515/11136 |
| **RAM resident** | **86 MiB** | 69 | 74 | 51 | 47 |
| (dense would be) | 327 MiB | 339 | 339 | 339 | 339 |

`maps/` is **26.1 MiB** in total.

**Why 12 bits.** The slicer's decision is `|Z - feetZ| <= floor_z_tolerance` with a 200 uu
tolerance and an 800 uu fade, and the pipeline's own storey separator (`--floor-band-gap`) is
250 uu. 12-bit codes give a 3.98..12.44 uu step, at worst 3.1 % of the tolerance and 2.5 % of
a storey gap. 10-bit (49 uu, a quarter of the tolerance) would start to matter.
`repack_maps.py` measures the error it introduced over every lit pixel and stamps it into the
manifest as `z_requantise_worst_uu`; `markers_test` fails if it exceeds 20 uu.

**Why 128-px blocks.** The planes are read by a CPU loop, never sampled by the GPU, so they
live in ordinary RAM — and three quarters of a dense plane is code 0, because a chapter's
walkable area is a quarter of its bounding box and deeper surfaces are rarer still (plane 0
lights 62 % of the blocks, plane 7 lights 3 %). `mapdata::build_plane()` allocates only the
non-empty 128-px blocks plus an int32 index per slot. Cropping to a bounding box and 512-px
tiling both measure worse: the lit pixels are scattered through every building on the map
rather than clustered. Consequence for readers: a row of the picture crosses several blocks,
so there is no row pointer — gather a row with
`HeightMaps::gather_row(k, sy, col_x, n, dst)`, which returns `false` for a row with no
surface at all so the caller can skip it.

**The version is enforced in both directions.** `mapmanifest::parse()` refuses any schema
that is not exactly `kSchema`: a /3 plane read by a /4 decoder puts every surface sixteen
times too low and looks like an empty map rather than like an error. In the other direction,
/4 renamed the manifest key (`height_maps` -> `height_planes`) and the files
(`_z<k>.png` -> `_h<k>.png`), so an older parser finds no list, guesses the `_z` names, finds
nothing on disk and logs `NO height plane decoded ... build them with build_map.py`. Change
the schema string in `tools/navmesh/mapfmt.py`, `src/mapmanifest.hpp` and
`tools/package.ps1` together.

### Re-encoding what already ships: `repack_maps.py`

```powershell
python tools\navmesh\repack_maps.py --dry-run          # measure, write nothing
python tools\navmesh\repack_maps.py --skip-heights     # composites only
python tools\navmesh\repack_maps.py                    # in place, maps\
```

`build_map.py` needs `tools/navmesh/dumps_offline/`, 400 MB of extracted tile JSON that is
not in the repo. A pure **format** change does not: the composite is re-palettised from its
own pixels and the planes are re-scaled from their own codes, so `repack_maps.py` re-encodes
a `maps/` tree in about 45 seconds with nothing else on disk, verifying every PNG by decoding
the bytes back before they land. Use it when only the encoding changes; use `build_map.py`
when the geometry, the filters or the resolution change.

### Resolution is per chapter

Each chapter is scaled to its own RAM budget, so `px_per_uu` runs 0.032 (chapter 5) to 0.060
(chapter 1) — chapter 5 has half chapter 1's detail. A uniform scale does not fit:

| uniform `px_per_uu` | `maps/` | worst chapter RAM |
|---|---|---|
| as shipped (0.032..0.060) | 26.1 MiB | 86 MiB (ch 1) |
| 0.060 everywhere | **42.8 MiB** | **166 MiB** (ch 5 at 5674x13754) |
| 0.049 everywhere (the most that fits 30 MB) | 28.6 MiB | 114 MiB (ch 3) |

0.060 is over both the 30 MB download budget and the ~90-100 MB resident target, and the
largest uniform scale that fits 30 MB would *reduce* chapters 1, 2 and 4. Spending the whole
remaining budget on chapter 5 alone takes it from 0.032 to 0.0398 (+24 % linear, 73 MiB
resident) for 4 MB of download:
`python build_map.py --input dumps_offline --chapter chapter5 --px-per-uu 0.0398 --max-ram-mb 1200`
(the `--max-ram-mb` fitter assumes a dense store, so it has to be raised for the sparse one).

`--legacy-layers` still produces the per-pixel surface-ordinal layers (`small_f0..f7.png`,
8-bit coverage masks) plus the 640-uu surface-band grid. They separate storeys exactly, but
the runtime can only guess which ordinal its storey is from a band table that names up to
three of them; the height map answers the same question exactly, per pixel.

### `slice_preview.py` — the runtime's rule, offline

```powershell
python tools\navmesh\slice_preview.py --x 19537 --y 4587 --z 2505 --out temple.png
```

Renders what the overlay would draw at that world position, straight from the shipped height
maps, so a "the floor looks wrong at X" report can be reproduced without launching the game.
`slice_window()` + `shade()` are the reference implementation of the slicing rule; keep them
and `overlay.cpp`'s `slice_window()` in step.

`--px-per-uu` is a request: it is first scaled continuously to fit `--max-ram-mb`, then halved
until neither dimension exceeds `--max-dim` (8192). That order matters — clamping first
charges a chapter a full halving and then leaves it under budget.

`deploy.ps1` copies `maps\` into the mod folder every time (`-NoMaps` skips it).

## The overlay

`src/overlay.cpp` installs four MinHook hooks whose addresses come from a throwaway device +
queue + swapchain (the hudhook trick, since the game's swapchain and command queue are not
reachable from a UE4SS mod):

| slot | function |
|---|---|
| swapchain vtable 8 | `IDXGISwapChain::Present` |
| swapchain vtable 13 | `IDXGISwapChain::ResizeBuffers` |
| swapchain vtable 22 | `IDXGISwapChain1::Present1` |
| queue vtable 10 | `ID3D12CommandQueue::ExecuteCommandLists` (captures the real queue) |

**On this install all four land inside ReShade's `dxgi.dll`**, a 5.6 MB proxy next to the exe
that wraps the command queue too. The dummy objects are created through our own import table,
so we get the same wrappers the game holds; the module+offset of every hooked address is
logged. The overlay therefore draws before ReShade's effects. Two consequences:
`swapchain->GetDevice(IID_ID3D12Device)` **fails** on the wrapper, so the device is taken off
the captured queue; and the game presents a decoy **144x8 D3D11** swapchain every frame next
to the real **1920x1080 R10G10B10A2_UNORM** one, so the overlay picks one swapchain
(`GetBuffer(0, IID_ID3D12Resource)` is the test) and ignores Presents from any other. A
foreign swapchain is ignored for *drawing* only: `hk_Present`, `hk_Present1` and
`hk_ResizeBuffers` call the original unconditionally, for every swapchain, so nothing else in
the process loses a frame to us.

**The hook-address cache.** Discovery creates a throwaway device, queue and swapchain and
destroys them again; Steam's `GameOverlayRenderer64.dll` hooks the same creation entry points
and re-targets its overlay onto what it sees created, which is how a Steam FPS counter ends up
pointing at nothing. The addresses are a property of the DLL, not of the session, so the first
launch writes them to `wuchang_minimap_hookaddr.txt` next to the config as `module + RVA` and
**every launch after that hooks them directly and creates nothing**. The cache is keyed to the
module's `SizeOfImage`, `TimeDateStamp` and `CheckSum`, so a ReShade, driver or Windows update
invalidates it and discovery runs once more; if cached addresses produce no Present, the 8 s
watchdog deletes the file and the next launch rediscovers them.

The log also answers *who else is on this function*: before a byte is written, the first 8
bytes at each address are read and, if a `jmp` is already there, its target is resolved to
`module+offset` (`ALREADY DETOURED -> GameOverlayRenderer64.dll+0x...`). MinHook is a
trampoline on the function and never a vtable patch, so a detour installed before ours ends up
*downstream* of ours and one installed after ours *upstream*; either way the chain is intact.

Rendering owns its own SRV descriptor heap (ImGui 1.92's `ImGui_ImplDX12_InitInfo` allocates
through callbacks), one command allocator per back buffer fenced against reuse, and RTVs
recreated lazily after `ResizeBuffers`. The map texture is created and uploaded by us
(`CopyTextureRegion` + a barrier to `PIXEL_SHADER_RESOURCE`) and its GPU descriptor handle is
passed to ImGui as the `ImTextureID`.

The minimap is drawn on the foreground draw list: player-centred crop, north-up or
rotate-with-player, round (a UV'd triangle fan, no mask) or square, configurable zoom, size,
anchor, offsets and opacity, with a yellow player arrow. It hides itself when a menu is open,
when the camera's view target is not the pawn, when the state snapshot is stale, or when the
player is outside every mapped chapter — and the F2 panel prints which.

`src/gamestate.cpp` reads the state on the **game thread** inside UE4SS's ProcessEvent
pre-callback: pawn location and yaw at 10 Hz via `K2_GetActorLocation` /
`K2_GetActorRotation`, the pawn and controller re-resolved at 2 Hz, and the menu test
(`UWidget::Visibility == Visible` prefiltered from the reflected byte, then `IsInViewport()`)
on a sliced object-array walk with an adaptive period. It publishes an `mm::Snapshot` through
a seqlock; the render thread never touches a UObject.

### The F2 panel

Three tabs, the config's tiers made visible:

- **Player** — presets (*Minimal HUD* / *Loot hunting* / *Exploration*, each setting several
  Player keys at once and touching no hotkey, no UI scale and nothing on the Advanced tab),
  minimap, placement and scale, markers, the collection tracker, the full map, the x-ray
  highlight, the compass, and the key legend.
- **Advanced** — collapsing headers over the Advanced tier.
- **Debug** — present only while `debug_readout = 1`, a Dev key in a file a player does not
  have. It carries the Dev keys, the per-activity performance table, every read-only
  diagnostic (marker sweep, gamepad, map slice, x-ray camera, game state) and the
  `hidden because: <reason>` line.
- **Bindings** — every hotkey, rebound by clicking a row and pressing a key.

Category filters are coloured chips, each filled with the colour that category is drawn in, so
the filter row is also the legend. The Save / **Revert** / Reload row and the master switch
live outside the tabs, at the bottom, and never scroll away. Save writes the file named on the
button and rewrites *only the values*; Debug-tab settings go to the dev file.

### Settings

`ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`, plain `key = value`, `;` or `#` starts
a comment, every key documented inline in the file itself. `src/config_keys.hpp` is the one
table that says which **tier** each key is in:

| tier | where it lives | what it is |
|---|---|---|
| **Player** (58) | `config_wuchang_minimap.txt`, under `; ---- PLAYER SETTINGS ----`; F2 → *Player* | something a person tuning the HUD would plausibly change |
| **Advanced** (63) | the same file, under `; ---- ADVANCED ----`; F2 → *Advanced* | correct as shipped; changed to answer a symptom |
| **Dev** (24) | `config_wuchang_minimap_dev.txt`; F2 → *Debug* | a dial that exists because a developer needed one during bring-up |

Twelve further keys are **removed** — sanity caps that are constants now — and one is
**legacy** (`enabled` -> `overlay_enabled`). Either gets a single warning naming it and is
then ignored.

**`config_wuchang_minimap_dev.txt` is not part of a release.** It is read only if it exists,
in the same folder, **after** the player config — so a key set in both wins there — and
`tools\package.ps1` throws if it finds one in the staged package. `deploy.ps1` copies it. The
1 Hz timestamp watch and F5 look at **both** files, so editing either reloads both. The Debug
tab saves into the dev file, never into the player one; if the file does not exist, Save
leaves it that way.

Only `srv_heap_size` needs a restart: the descriptor heap is created once, when the overlay
first initialises.

**The master switch, `mod_enabled` (default 1).** `mod_enabled = 0` makes the DLL inert: the
DX12 hooks are not installed (and are cleanly disabled if they already were — the render
thread tears ImGui and every D3D12 object down inside one Present, then the MinHook
trampolines are disabled but kept, so turning it back on can never double-hook), the
ProcessEvent game-thread callback returns on its first statement (UE4SS exports no
*Unregister*, so that early return is the mechanism), no object-array scan runs, the chapter's
height maps are freed and XInput is never polled. What keeps running is one
`GetFileAttributesEx` of the config file per second on the loop thread, so setting the key back
to `1` restarts the mod within a second. **F5 does not work while the mod is off** — nothing
samples the keyboard — and the panel's checkbox can only turn it *off*. Every flip writes one
`master switch:` line into `UE4SS.log`. `overlay_enabled` is the *overlay*, not the mod: with
`overlay_enabled = 0` the reader, the marker sweep and the map asset still run.

`tests/markers_test.cpp` is the drift guard, in both directions:
`keys(config_wuchang_minimap.txt) == Player ∪ Advanced`,
`keys(config_wuchang_minimap_dev.txt) == Dev`,
`{key == "..." literals scraped out of mmstate.cpp} == Player ∪ Advanced ∪ Dev ∪ Legacy`, the
four tiers pairwise disjoint, no removed or renamed key in either file, the shipped file's
`; ---- PLAYER SETTINGS ----` / `; ---- ADVANCED ----` banner order matching the tier tags key
for key, and a byte-identical round trip of the in-place rewrite over both real files. So no
layout, no table and no parser branch can drift away from the others.

#### Absence as evidence of a collect

An item picked up **before the mod was installed** leaves nothing to read: the game parks a
collected pickup at `(0, 0, 0)` when its level loads and frees it at the next GC, so neither
`dying` nor the `(0,0,0)` test has an actor to speak for. Absence on its own is *not* evidence
here — an unloaded level and a collected pickup are indistinguishable from the object array —
so `markers_absence_marks` adds the facts that make it one:

1. the marker's owning level (its `level` field) is in the set `gamestate` reports as loaded.
   **A marker whose level cannot be matched is never marked** — that is the safety rail;
2. at least one **full** pass over the object array has completed since that level was first
   seen loaded, so "I did not see it" means "I looked at every object in the game while its
   level was streamed in";
3. that pass found no live twin with a usable position and no collected flag (a twin at the
   origin or flagged `dying` is itself collected, so it does not block the rule);
4. and the same held for `markers_absence_rounds` passes in a row.

The mark goes through the normal found tracker, so it persists and can be undone by clicking
the marker on the full map. The F2 panel shows `absence marks N   levels loaded M`; `M = 0`
means the rule can never fire. The predicate (`mdb::absence_round_confirms` /
`mdb::absence_marks`) is pure and its truth table is in `tests/markers_test.cpp`.

### The save-slot ladder (`src/saveslot.*`)

Which save is loaded decides which collection file is used. Four rungs, each **logged with the
route that answered**:

1. **uuid** — `GameSaveExecutor`'s own KV accessor. The `UFunction` is resolved by name over
   three candidate spellings and its **reflected parameter list is read and compared** to the
   predicted shape before anything happens; the signature is logged either way, and the call
   itself stays behind `saveslot_uuid_call`.
2. **slot path** — `Impl_GameSettingsSaver_C::TickCountSavPath`, a raw `FString` read with no
   `ProcessEvent`, parsed for its `GameSlots\<slot>` component.
3. **sav file** — the newest `*.sav` under `%LOCALAPPDATA%\Project_Plague\Saved\*\GameSlots\*`,
   giving `<accountid>_<slot>` from the path. It runs on the loop thread before the game thread
   pumps, so the first load already has a key.
4. **shared** — the global `wuchang_minimap_found.txt`.

On first sight of a slot with no file of its own the shared file is **copied** into it once,
and the copy is logged. A slot switch drops every cache, which re-arms the resolution, so the
tracker swaps files with no restart; a pending write goes to the *old* file first.

### Fast travel is guarded, not disabled (`src/shrines.*`)

The route is the game's own: `PlayerModelLibrary_C::PlayerChuanSongFirePoint` on the library
CDO, falling back to `BP_RebornFire_C::ChuanSong` on a resident shrine — **never
`K2_TeleportTo`**, which moves the pawn without the pre-travel save, the reborn info or the
`pmaps` level set, and lands the player in unstreamed geometry. The call is issued only after
`uer::func_params()` has read the real parameter list and it matches the prediction (one to
two 16-byte `FString` slots, the first at offset 0); a mismatch refuses and says so in the
panel, and a shrine the save has not unlocked refuses too. `ue_min.hpp` declares `UFunction`
as a `UStruct` subclass, which is what makes reading a signature possible.

**Dump the fast-travel / save-slot recon** on the Debug tab (`src/recon.cpp`) writes the game
mode's components, every property of `RebornManagerComponent_C` with the three firepoint
arrays, the reflected parameter lists of the ten functions both routes name, and the save-slot
fallback strings — calling nothing. When a name does not resolve it prints the names that did.

### `markers/shrines.json` (schema `wuchang-minimap-shrines/1`)

`tools/markers/extract_shrines.py` reads the game's `DT_FirePoint` DataTable with no `.usmap`:
88 contiguous rows, all named from `MMGame.locres`, all with a `BirthPosition`, 50 of them
joined to a shrine marker by id (the rest are the `bossdoor_*` / `Task*` pseudo-rows, flagged
`"shrine": false`). Three things make it safe: `BirthPosition` is schema slot 0, so it needs
no walk over variable-sized values; the row scan is validated by **contiguity** (all 38 787
payload bytes accounted for), which rejects the ~54 spurious matches on its own; and the
display name is found by its own evidence — `ShowName`'s locres key survives in the row as
ASCII and the locres either has it or it does not. `src/shrines_db.hpp` is the pure parser and
the shipped file's counts are asserted offline.

### The full map (`M`)

The same asset, the same slicing rule and the same markers as the minimap, at map scale: a
north-up window over the chapter with a dark backdrop, pannable and zoomable. While it is open
**the minimap is hidden** and the mod takes the mouse and the keyboard.

| input | mouse / keyboard | gamepad |
|---|---|---|
| pan | drag, `WASD`, arrows | left stick |
| zoom | wheel, `+` / `-` | triggers, right stick Y |
| floor slice up / down | `ctrl`+wheel, `E` / `Q`, PageUp / PageDown | RB / LB |
| recentre on the player | `R` (`map_recenter_key`), the Recentre button | Y |
| set the waypoint | right-click, `Space` / `Enter` | A (at the view centre) |
| toggle "found" by hand | left-click a marker, `F` (nearest to the centre) | X |
| close | `M`, `Esc`, the Close button | B |

Every control has a keyboard **and** a gamepad route: the mouse cursor is the one part that
depends on what the game does with the OS cursor while we hold the input.

**Memory: the map adds no copy of the asset.** The height planes (86 MB of RAM for Chapter 1,
in 128-px blocks) are read in place; the map cuts its own small dynamic RGBA texture
(768 x ~430 x 2 buffers, ~2.6 MB) out of the same planes the minimap slices. The cut is
*decimated* — one texture pixel covers `step` source pixels — and covers the visible viewport
plus a 30 % margin, so a small pan needs no new cut. Unlike the minimap, which re-cuts 12 times
a second because the player is always moving, the map re-cuts only when the view left the cut
region, the zoom changed, the floor slice moved, or the player moved far enough to be on
another storey — capped at `map_slice_hz`. An idle open map costs nothing per frame beyond the
draw, and the buffer being written is never one the GPU is still sampling (the same fence rule
as the minimap; a busy buffer skips the update instead of stalling Present).

**Height slicing at map scale** is the minimap's rule plus an offset:
`|Z - (feetZ + floor offset)| <= floor_z_tolerance` is opaque, the nearest surface below /
above within `floor_fade_uu` is dimmed, and the floor adjustment nudges the offset so you can
look at the storey above or the dungeon below without walking there. `map_show_all_floors`
widens the fade to infinity for a route-planning view.

**Markers** are the same published draw buffer, the same glyphs and the same category mask the
minimap uses — the legend column toggles the *same* `markers_categories` setting the F2 chips
and the config file drive. Hovering a marker shows its class, category, found state and
distance; a left-click toggles found by hand, which goes through the tracker mailbox to the
loop thread and into the found file. The live sweep still owns the truth: un-marking a chest
the game reports as `Used` is undone on the next sweep round, because the tracker follows the
save, not the mod.

**The waypoint** is a single position set with a right-click (or `Space`, or gamepad A). It is
drawn on the map and, edge-clamped with its distance in metres, on the minimap. It persists in
`wuchang_minimap_waypoint.txt` next to the config: three plain `key = value` lines,
hand-editable, written by the loop thread (the render thread only sets the value). It is
deliberately **not** part of `config_wuchang_minimap.txt`, which is only written by the panel's
Save button, because a waypoint set during play must survive without anybody pressing Save.

**Nothing latches.** The map closes itself the moment the state that allows it stops being true
— a menu opening, the pawn going away, a level transition, a stale snapshot — and the input
swallow condition *is* `g_map_open`, so closing hands the mouse and the keyboard back on the
very next message. The map key itself is sampled with `GetAsyncKeyState` on the loop thread,
precisely because the WndProc hook is swallowing every key while the map is up.

### Why the minimap is (not) on screen

`overlay.cpp`'s `set_hide_reason()` is the single choke point for visibility, and every show
condition is re-evaluated from the live snapshot on **every frame** — there is no latch
anywhere in the path. The F2 Player tab prints the current reason as
`hidden because: <reason>` (or `minimap: visible`) with how long that state has held, and
every transition goes to the log as `minimap HIDDEN: <reason> (previous state held N ms)`,
rate-limited to one line per 2 s. When the reason is a menu, the readout names the in-viewport
widget holding it open.

Accepted hotkey names are F1-F5, F7, F8, any single letter or digit, TAB, SPACE, ENTER,
BACKSPACE, the arrows, INSERT/DELETE/HOME/END/PAGEUP/PAGEDOWN, NUM0-NUM9 and the numpad
operators, MOUSE3-MOUSE5, the L/R modifier keys and `none`, with one optional `ctrl+` /
`shift+` / `alt+` prefix. F6 (RenoDX DLSS 5), F9/F11 (engine binds), F10 (game console) and
F12 (Steam) are rejected in code, not merely discouraged in a comment. The panel and the full
map print the live binding list, built from the config, so a rebound key is what you are told.

## The x-ray highlight (`LALT`)

Arm the key (or the gamepad chord, `LB+RB` by default) and every marker of the enabled
categories within `highlight_radius` of the player is drawn **at its position on screen** —
category glyph, name, distance in metres — fading with distance, over the scene. "Through
walls" is free: the overlay is composited on the finished frame, so there is no occlusion
test, no CustomDepth and no material. Anything off screen or behind the camera gets an arrow
on the screen edge (`highlight_edge_arrows`).

`highlight_show_found = 0` (the default) hides **collected loot** — chests, pickups and hidden
items, the three categories where finding a thing consumes it. Shrines, bosses, NPCs, notes,
doors and fog gates are landmarks and are highlighted whatever their found state.

**Item quality colours.** Wuchang has **no rarity ladder**: no `E_ItemQuality` / `Rarity` /
`Grade` enum anywhere in the paks, no quality word in `MMGame.locres`, and no such field on any
of the six item row structs. What it has is the colour of the beam a pickup gives off:
`BP_PickupActor_C` picks a `DT_Particle` row (`PickupEffect`, `PickupEffect4..6`,
`PickupEffect7..9`) whose `LightColor` is blue, pink or gold, and which row it picks follows the
item's `ItemType` (`E_ItemType`). `tools/markers/build_items.py` decodes that enum out of the
cooked item DataTables and `extract_markers.py` bakes the tier into every pickup marker as
`"rarity"`:

| tier | name | items | default colour (the game's own beam colour, sRGB) |
|---|---|---|---|
| 0 | Common | tools, consumables, arrows, enchanting materials | `ADAFDA` blue |
| 1 | Equipment | weapons, armour, accessories, gems, spells, skills | `DAADC5` pink |
| 2 | Key | quest items and red-mercury upgrade materials | `DAD6AD` gold |

While the highlight is armed, a marker with a tier above 0 is drawn — glyph, label and edge
arrow — in that tier's colour instead of its category colour
(`xray_rarity_colors_enabled = 1`). **Tier 0 keeps its category colour**: it is every chest,
every live actor the offline database does not know and every ordinary consumable, so tinting
it would recolour most of the screen to say nothing. `markers_rarity_tint = 1` extends the
tint to the minimap, the full map and the compass pips. Of chapter 1's 287 pickups, 18 are
Equipment and 21 are Key; over all six chapters 47 of 1 086 are Equipment and 85 are Key.

The derivation is checked against the game's own behaviour: every `BP_PickupActor_C` whose
live `PickupEffectName` was captured in the WuchangRecon world dumps agrees — 11/11
non-default beams and 7/7 default ones.

**Toggle or hold** (`highlight_mode`). `toggle` is the default. The toggle is the one piece of
latched input state in the mod, so it obeys the rule that goes with that: it is **cleared from
live state, never remembered** — `hl::drop_caches()` (which `markers::drop_caches()` calls on
every level transition and every dropped pawn) turns it off, and so does turning the feature
off or switching to hold mode. The highlight is gated by exactly the same evaluation as the
minimap (`hud_gate()` in `overlay.cpp`, asked by the minimap, the compass and the highlight;
the minimap keeps ownership of the `hidden because:` readout).

**The projection.** `src/projection.hpp` is dependency-free math with hand-computed tests in
`markers_test`: UE's `FRotationMatrix` basis written out row by row, the horizontal FOV with
the aspect applied to the vertical axis exactly as `FSceneView` does it
(`tan(vfov/2) = tan(hfov/2) / aspect` whenever the viewport is wider than tall), and a
behind-the-camera case that never produces a screen position — a naive divide by a negative
depth mirrors the point onto the opposite side of the screen — but does produce the direction
an edge arrow must point.

**The camera.** `src/highlight.cpp` reads it on the game thread from the local
`APlayerCameraManager` and publishes it through its own seqlock. `CameraCachePrivate` is an
`FCameraCacheEntry` whose `FMinimalViewInfo` starts with Location (3 doubles), Rotation (3
doubles) and FOV (float) — documented, but not verifiable on this build without launching it,
and a non-reflected engine struct must never be recognised by an assumed field order. So the
offset is **discovered**:

1. call `GetCameraLocation` / `GetCameraRotation` / `GetFOVAngle` once (one SEH-guarded
   `ProcessEvent` each);
2. scan the first bytes of `CameraCachePrivate` for the offset whose six doubles and following
   float match what the getters said, to 2 uu / 0.5 degrees / 0.5 degrees of FOV;
3. pin it. Every read after that is 56 bytes at a cached offset — cheap enough for
   `highlight_camera_hz` while the highlight is armed, and **nothing at all** while it is
   disarmed and the compass is off.

If the getters are unavailable the offset is accepted on sanity ranges alone, and a pinned
offset that produces eight insane reads in a row is dropped and re-discovered. The F2 panel's
*X-ray highlight* block names the route, the two offsets, the live pose and the read/reject
counts.

The camera reader is driven from **one clearly-marked hook** inside
`markers::game_thread_pump()`, which runs only while `gamestate` has a validated gameplay pawn
outside the transition cooldown; its caches are dropped from `markers::drop_caches()`.

## The compass strip

A heading strip across the top of the screen: N / NE / E ... with 15-degree ticks, a centre
reticle, and bearing pips for nearby markers of `compass_categories` plus the waypoint, which
clamps to the strip's edge with an arrow rather than being culled. `compass_span_deg` decides
how much of the world the strip covers; 360 makes it a full ring.

Each pip carries the **horizontal distance in metres** just outside the strip, nearest first,
each label reserving its own x range so two never overlap (`compass_pip_labels = 0` turns them
off). A marker more than `compass_pip_height_uu` off the player's own height also gets an up or
down arrow beside its glyph.

The heading is the **camera's** yaw when a pose is fresh and the pawn's yaw otherwise, so the
compass works with `highlight_enabled = 0` and during the camera reader's warm-up; the F2 panel
says which is in use. The arithmetic — wrap into `(-180, 180]`, bearings in the mod's `+X`
north / `+Y` east frame, strip positions, tick ranks — is pure and lives in `src/compass.cpp`,
which `markers_test` links.

## The marker data pipeline

Everything under `markers\` except `chapter1.sample.json` is **generated** from the game's paks
by `tools\markers\*.py` (Python 3.10+, `pycryptodome` for the AES-encrypted pak index).
One command regenerates all of it:

```powershell
python tools\regen_all.py                 # every step, all six chapters
python tools\regen_all.py --verify        # + score the result against the recon dumps
python tools\regen_all.py --no-pak-hash   # skip the 63 GB of sha256 while iterating
python tools\regen_all.py --list          # the step graph
python tools\regen_all.py --only extract  # one step (repeatable)
```

It finds the game through `WUCHANG_PAK` (the base `.pak`), `WUCHANG_GAME_ROOT` (the install
folder) or `--pak`; there is no hardcoded install path in the pipeline. Before the first step
it checks that the paks exist and are readable, that both `_N_P` patch paks are present (the
DLC levels and the patched `DT_FirePoint` live in them), that `pycryptodome` imports and that
`markers\` is writable. A failing step **stops** the run: every later step reads what an
earlier one wrote, so continuing would leave a half-updated `markers\` that no diff can be
trusted against.

### The step graph

| step | script | writes | needs |
|---|---|---|---|
| `items` | `build_items.py` | `markers/items.json` | — |
| `graph` | `class_graph.py` | `tools/markers/class_graph.json` | — |
| `categories` | `build_categories.py` | `markers/categories.json` | `graph` |
| `enemies` | `build_enemies.py` | `markers/enemies.json` | `graph` |
| `bosses` | `build_bosses.py` | `markers/bosses.json` | `graph` |
| `npcs` | `build_npcs.py` | `markers/npcs.json` | — |
| `extract` | `extract_markers.py` | `markers/chapter{1..5,dlc}.json` | the five above |
| `bossdoors` | `build_bossdoors.py` | `markers/bossdoors.json` | `extract` |
| `rebake` | `extract_markers.py` | `markers/chapter*.json` | `bossdoors` |
| `shrines` | `extract_shrines.py` | `markers/shrines.json` | `extract` |
| `recount` | `build_enemies.py` | `markers/enemies.json` | `extract` |
| `verify` | `verify_markers.py` | nothing (reports) | `--verify` |

Only `chapter*.json`, `shrines.json` and `items.json` are **shipped**; `categories.json`,
`enemies.json`, `bosses.json`, `npcs.json`, `bossdoors.json` and `class_graph.json` are
toolchain artifacts baked into the chapter files, so the runtime reads three files rather than
nine.

Two things about the order are the reason the driver exists:

* **`bossdoors` is a genuine cycle.** `build_bossdoors.py` joins the level scripts'
  `ST_LevelScriptBossData` to the *boss marker ids* `extract_markers.py` writes, and
  `extract_markers.py` bakes the resulting `bossdoor` id back onto those markers. So a full run
  is extract → bossdoors → extract. The driver skips the second extraction when
  `bossdoors.json` came out byte-identical, which is the normal case.
* **`categories` and `enemies` must precede `extract`.** `marker_classes.py` reads both at
  import time: `categories.json` is the class → category table and `enemies.json` carries the
  elite split. Running `extract` first falls back to a hand-written class list that ships
  **fewer** markers, and says so on stderr.

The run ends with a summary table — per step the status, wall time and outputs, then markers
per chapter and category, and the delta against whatever was in `markers\` before. A
regeneration should move names, items, categories and new entries, and **no coordinate of an
existing marker**.

### Category assignment is a class-graph question

`markers/categories.json` is the descendants of one base class per category, read out of every
cooked `.uasset` export map's `super` field (`class_graph.py`, ~40 s for 81 353 assets, cached
and committed as `tools/markers/class_graph.json`). `build_categories.py` holds the *choice of
bases* and the precedence; everything below a base is data:

```
shrine  BP_RebornFire_C          door      BP_InteractionObject_Door_C, BP_NewPuzzlesDoor_C
hidden  BP_PickUpActor_Trap_C    fog_gate  BP_Wumen_C + its two siblings
chest   BP_ItemBox_C             ladder    BP_LadderV2_C, BP_InteractionLadder_C
pickup  BP_PickupActor_C, BP_PickUpPT_C, ItemCollectionBox_C
note    DKDC_NPC_C, ReadPointSP_NPC_C, Letter01_NPC_C
boss    BP_PlacedBossAI_C        lift      BP_ElevatorBase_C, BP_ElevatorBox_C
npc     BP_NPC_C                 enemy     BP_BaseAI_C
```

The order is load-bearing. Five `BP_NPC_C` descendants have a category of their own
(`BP_RebornFire_C` is a shrine, the three read-points are notes, `ItemCollectionBox_C` is a
pickup), and `BP_PickUpActor_Trap_C` is a `BP_PickupActor_C` descendant that must be `hidden`.
`boss` precedes `enemy` because the 32 boss classes are `BP_BaseAI_C` descendants too.

`BP_BaseAI_C` (314 descendants, **zero** overlap with `BP_NPC_C`'s 78) is what makes `enemy` a
class question rather than a level-name one. The `*_AI` sublevel heuristic survives only as a
counted fallback for classes whose `super` the graph never recorded, and the extractor prints
the rule mix (`rule:table`, `rule:ai-level`, `rule:noise`, `rule:unmatched`) plus **every class
placed in a `_logic` level that matched no category**.

### What the data cannot tell you

Three answers are absent from the game:

* **Ordinary enemies have no name.** `MMGame.locres` has 30 `boss_name_*` keys and 61 `npc_*`
  keys and no `monster_*` / `enemy_*` / `ai_name_*` family at all; a `DT_AiTable` row carries
  no text but the class `FName`; the `FText`-in-the-blueprint route that names every NPC
  returns nothing for six representative enemy blueprints (with the NPC asset as the positive
  control in the same run); and `help_noun_*` is a mechanics glossary, not a bestiary.
  `python tools\markers\build_enemies.py --prove` re-runs all four. So 8 enemy classes get a
  real name — variants and phases of named characters, which own a `boss_name_<id>` — and the
  rest read "Enemy", deliberately not a tidied Pinyin class name.
* **The DLC has no fire-point rows.** `DT_FirePoint`'s 88 rows cover chapters 1-5 and its name
  map does not contain `BaiYS01`, `BaiYS02`, `borencl01`, `borencl02`, `LiuHKK01` or
  `pinmingk01`. There is no second fire-point table in the paks, no `ChapterDLC` folder under
  `Content/Scene/3D/Others/FirePoint/`, and no DLC area name among the `ui_*` keys. So the
  seven DLC shrines go into `shrines.json` from the marker DB with the marker's own
  `Shrine <fire-point id>` label and no `BirthPosition`, and the same absence is why the DLC
  boss is one of the two `bosses_without_a_door` in `bossdoors.json`.
* **The DLC pickups carry no item.** All 77 configured `BP_PickupActor_C` instances in the DLC
  hold `[{20001, 1}]`, and the whole `ChapterDLC_*_logic` set contains exactly **one** distinct
  valid item id where one Chapter-1 sublevel carries 19. 20001 is the *first row* of
  `DT_Item_ToolTable` and appears zero times across chapters 1-5, i.e. it is the default index
  of the blueprint's own editor tool (`BP_PickupActor_C` exposes `GetItemsByEditorTool` and
  `ReplaceItemIDByGamePlus`). `extract_markers.PLACEHOLDER_ITEM_IDS` suppresses it, so those
  markers read "Pickup".

### Provenance

Every generated file carries the same four fields, so a marker database is auditable against
the build it came from:

```json
"game_build": "5.1.1.0",
"exe":  {"name": "Project_Plague-Win64-Shipping.exe", "size": 141464648, "sha256": "..."},
"pak":  [{"name": "Project_Plague-Windows.pak", "size": 44740955247, "sha256": "..."}, ...],
"extractor_commit": "9a46681"
```

`game_build` is the exe's `FILEVERSION`, which a running mod can read cheaply and compare.
Know what it is worth on this title: Leenzee ship no game build number, so that string is the
*engine* version (`ProductVersion` is `++UE5+Release-5.1-CL-0`) and it does not move across
game patches. The identifiers that do move are the digests. They are cached in
`tools/markers/.provenance-cache.json` (gitignored, keyed on path + size + mtime) because
hashing the pak set is ~45 s; `--no-pak-hash` records sizes only and writes `"sha256": null`.
Every field is optional to a reader, which keeps the schema at `.../1`.

### Verifying against the game

`verify_markers.py` scores the extracted coordinates against the WuchangRecon **F8 world
dumps** — the same `(level short name, cooked object name)` join key `FindAllOf` reports
in-game — and defaults to the dumps committed to this repo
(`tools/lua-recon/WuchangRecon/out/dump_*_world.txt`, kept by an explicit `.gitignore`
exception), so it runs on a fresh clone with no game installed:

```powershell
python tools\markers\verify_markers.py                # every chapter, repo dumps
python tools\markers\verify_markers.py --require      # non-zero exit on a disagreement
```

The dumps cover only what was streamed in when F8 was pressed, so most chapters report "not
loaded in any dump" and that is not a failure. Of the 255 markers the committed dumps do cover,
252 agree with a **median error of 0.000 uu**; the three that do not are all
`BP_WoodenElevator_C`, a moving platform whose cooked transform is its authored start. That is
why the regen driver runs this as a report and not as a gate.

### Data invariants in the test suite

`tests/markers_test.cpp`'s `test_data_invariants` runs over **every** shipped `markers/*.json`
on each build (`build.ps1` passes the repo's `markers\` directory as `argv[1]`): each file
parses with `skipped == 0`, `unknown_cat == 0` and the expected chapter label; no marker id
repeats within a file or across files (the loader globs them into one id-keyed database, so a
collision is a marker that can never be marked found); every `(chapter, category)` clears a
floor from a table in the test — including the explicit zeros, so "chapter 5 has no ladder" is
a recorded decision rather than a blind spot; every item id a pickup references is a row of
`items.json`; every shrine marker has a row in `shrines.json`; and no single name accounts for
more than half of a `(chapter, category)`'s **named** entries.

---

## Markers

Markers come from two halves merged by a **stable id**, and the id is the whole design:

* the **static database**, `markers\<chapter>.json` (schema `wuchang-minimap-markers/1`), built
  offline from the cooked levels by `tools/markers`, so a marker exists for an area you have
  never visited;
* the **live sweep**, a chunked walk of `GUObjectArray` that classifies every object against a
  table of marker classes and supplies the position and the *state* of every actor currently
  streamed in.

The id is the game's own shrine id for shrines (`digong01` — `BP_RebornFire_C`'s CJK-named
"sitting-Buddha point ID", the only property that distinguishes sibling shrines) and
`<owning level short name>/<actor object name>` for everything else, because that object name
is what `FindAllOf` hands back at runtime.

| category | classes swept | "found" means |
|---|---|---|
| shrine | `BP_RebornFire_C` | not a per-actor flag: **`RebornManagerComponent_C::UnlockedFirepoints`**, a global `TArray<FString>` of shrine ids persisted under `lockqueue`, read raw at 1 Hz by `src/shrines.cpp`. That is what "shrines lit" and the shrine list's `Lit` column show. Shrine markers are never *auto-marked found*: the tracker follows collectables, not rest points. |
| chest | `BP_treasurebox_C`, `BP_ItemRedBox_C` | `Used == true` (persisted under `SavedStatuKey = statu_use`) |
| pickup | `BP_PickupActor_C` and subclasses (incl. `BP_DropItem_C`) | `dying == true` **or** the actor is parked at `(0,0,0)` |
| door | `BP_NewPuzzlesDoor_C` (`DoorOpen`), `BP_DoorZhong_C` (`Used`) | the door is open |
| fog gate | `BP_Wumen_C` | `Active == true` (inferred from `SavedStatuKey = status_active`) |
| ladder / lift | `BP_LadderV2_C`, `BP_WoodenElevator_C` | never — they are navigation aids |
| npc / note | `BP_NPC_C` descendants | MET: seen loaded within 3 000 uu of the player. A used-up NPC is made **invisible**, not moved, so a mobile marker's live twin is tested for visibility |
| boss | `BP_PlacedBossAI_C` descendants | DEFEATED: `pawn -> Controller -> Health -> CurrentValue <= 0`, or the arena's `bossdoor_*` point unlocked in the save (`boss_defeat_from_save`, derived on every publish, never persisted) |
| enemy | pawns possessed by `Impl_BaseAIController_C` | n/a — never written to the tracker. A static `enemy` entry is a **spawn point**; the live pawn overwrites its position under the same id, so the two are one marker. The same health read drops a corpse from the live view |

Absence from `FindAllOf` is **not** evidence of a collect: an unloaded level looks exactly the
same. Only the state flags auto-mark, plus the guarded absence rule above.

### Cost control

`UObjectGlobals::FindAllOf` walks the **whole** object array, so one `FindAllOf` per class is
one full walk per class — measured at **28.30 ms mean / 51.05 ms peak per game-thread pump**
for a single class.

The sweep inverts the loop. It walks `GUObjectArray` **once per round**, in slices of
`markers_scan_chunk` slots per pump (`src/scan_sched.hpp` holds the pure slice / wrap / rate
arithmetic and `markers_test` covers it). Per slot the cost is a bounds-checked
`FUObjectArray::IndexToObject`, an `FUObjectItem::IsValid(false)`, the object's `UClass*`, and
one lookup in a `UClass* -> marker spec` table memoised per class — so the super-chain name walk
that decides "is this a marker class, or a subclass of one" happens once per class per level,
not once per object per round. Everything expensive (`RootComponent` location, the state flag,
the `GetFullName` id) runs only for the handful of objects that matched. The menu-widget sweep
in `gamestate.cpp` uses the same slicer, with the raw reads on the slice and the
`IsInViewport()` confirmation on the validated 10 Hz pump.

The slice is called from **every** `ProcessEvent` pre-callback while the last validated state
stands, not from the 10 Hz position pump, and it throttles itself on
`QueryPerformanceCounter`, because `GetTickCount64`'s ~15.6 ms granularity is coarser than a
one-frame slice period.

Positions are read raw (`RootComponent` -> `RelativeLocation`), not through
`K2_GetActorLocation`: a `ProcessEvent` per actor for ~130 pickups plus ~95 enemies inside the
engine's own call stack is not affordable.

The F2 panel prints the two numbers that tune this: `scan pump <last> ms (avg, peak, max)` —
what one pump costs the game thread — and `round <ms> / <pumps> / <objects> of <total>
chunk N`. A `! FindAllOf fallback` suffix means `FUObjectArray::GetNumElements()` answered 0
and the per-class path took over.

Glyphs are drawn with `ImDrawList` primitives — no image atlas, so there is no art to keep in
sync with the category list — and each category gets a **shape as well as a colour**, so a
dimmed "found" marker keeps its shape after it has lost its colour contrast.

### The files the mod writes

`wuchang_minimap_found_<slot>.txt` (mod folder, next to the config) is the collection tracker:
one stable id per line, sorted, comments allowed, rewritten from the loop thread
`found_save_debounce_ms` after the last change, through a temporary file swapped into place so
the real file is either the old one or the new one. It keeps one `.bak`. There is one per save
game; `wuchang_minimap_found.txt` without a suffix is the shared file, used when no slot can be
identified and seeded into a new slot's file once. A deploy never touches either.

`wuchang_minimap.log` is the mod's **own** copy of everything it logs, rotated on every launch:
the live file plus `.1`, `.2`, `.3`. It exists because UE4SS truncates `UE4SS.log` on every
launch, so the evidence from an in-game session is gone as soon as the game is started again.
Writes are buffered and flushed on every crash-breadcrumb stage transition — so the log and
`wuchang_minimap_last_stage.txt` always agree about the last thing that happened — and every
three seconds from the loop thread. There is no config key: it is always on, and it is the file
to ask for in a bug report.

`wuchang_minimap_watchdog.txt` is written ONLY when the game stops responding. The loop thread
watches two counters — Presents on the render thread and ProcessEvent position pumps on the
game thread — and after six seconds of either not moving it appends one line naming which
thread stopped, how long ago, and the coarse stage each was last in (`build_ui`,
`imgui: ImplWin32_NewFrame (user32)`, `pawn validate`, ...). It is a second file with its own
writer: the line is assembled in a stack buffer and written with flat `CreateFileW` /
`WriteFile` / `FILE_FLAG_WRITE_THROUGH` and no allocation at all, because the failure it
describes can be a wedged process heap, in which case `std::format` would hang the last thread
still running. The mod log is flushed straight afterwards. A crash leaves a
`CrashContext.runtime-xml`; a hang leaves nothing, which is what this is for.

## Not implemented

- **X-ray highlight v2**: true silhouettes through `SetRenderCustomDepth` plus a post-process
  material shipped in a tiny pak. The game ships no outline material to reuse.
- **The slicing loop as a pixel shader.** It needs its own root signature, PSO, `D3DCompile`
  and `ImDrawList::AddCallback` juggling on a ReShade-wrapped swapchain, and it would save only
  a few ms per update and the ~1 MB upload; the CPU slicer already has the exactly-correct
  semantics.
- **A DLC map.** The paks carry no navmesh cells for it; it would need a runtime cell sweep or
  the ortho-capture fallback.
- **A whole-region runtime navmesh dump.** Only the 4-6 cells around the player are resident,
  so it needs a sweep over all streaming cells.
