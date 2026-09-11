# WuchangMinimap — developer documentation

A UE4SS C++ mod for **Wuchang: Fallen Feathers** (Unreal Engine 5.1.1, Windows x64, DX12): a
minimap, a full chapter map, a compass strip, an x-ray highlight and a collection tracker, drawn
by the mod's own Dear ImGui context on a MinHook'd DX12 `Present`. The map background is generated
offline from the game's own Recast navmesh.

**This file is the toolchain, the two offline pipelines and the map of the source.** Everything
else is documented where it lives and is not repeated here: how a module works is the prose block
at the top of its own file, what a config key does is the comment beside it in the config file,
and how to use the mod is the [user README](../README.md). Cutting a release is `docs/RELEASE.md`.

Paths (`src\...`, `tools\...`, `maps\...`) are relative to the **repository root**, the parent of
this `docs\` folder.

---

## Build

Steps 1-6 once per machine; after that, `.\build.ps1`. This project cannot `include("RE-UE4SS")`
the way the UE4SS docs describe — see [The UE4SS pin](#the-ue4ss-pin) — so the toolchain is
assembled by hand.

Machine paths come from parameters or environment variables, never from a hardcoded default:
`WUCHANG_UE4SS_ROOT`, `WUCHANG_XMAKE`, `WUCHANG_MSVC_TOOLSET`, `WUCHANG_GAME_ROOT`,
`WUCHANG_UE4SS_DLL`, `WUCHANG_VS_PATH`, `WUCHANG_PAK`.

### 1. Visual Studio with the C++ toolchain

<https://visualstudio.microsoft.com/downloads/> — any edition, with the **"Desktop development
with C++"** workload, which also brings the Windows SDK (`d3d12.h` / `dxgi.h`).

**MSVC toolset 14.40.33807** is the tested one; `build.ps1` prefers it and otherwise takes the
newest on the box with a warning (`tools\vs_detect.ps1` does the discovery, via `vswhere`). Pin it
with `-Toolset 14.40.33807` or `$env:WUCHANG_MSVC_TOOLSET`. If it is not offered, it is under
*Individual components* as "MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.40-17.10)".

> VS 2026 Insiders ships no `vcvars64.bat`, so `gen_ue4ss_importlib.ps1` enters the developer
> environment through `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell` instead. It works
> the same on a normal install.

### 2. xmake 3.1.1

<https://github.com/xmake-io/xmake/releases/tag/v3.1.1> — the portable `xmake-v3.1.1.win64.zip` is
enough. **3.1.1 is the only version this project builds with**; `xmake.lua` pins it with
`set_xmakever("3.1.1")`. `build.ps1` looks for `xmake.exe` at `F:\Tools\xmake\xmake.exe`, then on
`PATH`; point it at yours with `-Xmake` or `$env:WUCHANG_XMAKE`.

### 3. Install UE4SS into the game

**UE4SS for Wuchang: Fallen Feathers**, Nexus mod **384**, file version **1.79** — build
**`v3.0.1-934-gcac01ee2`**, which `ue4ss\UE4SS.log` calls `UE4SS - v3.0.1 Beta #0 - Git SHA
#cac01ee2`. It is not interchangeable: the mod links against this DLL's export table, so **any
other build fails to load, silently**. Unzip it into `<Game>\Project_Plague\Binaries\Win64\`.

Then check that `HookInitGameState = 0` in `ue4ss\UE4SS-settings.ini` — 1.79 ships it set — or the
game crashes a third of a second into loading, with or without this mod.

Step 5 needs the installed `UE4SS.dll` whether or not you intend to run the game.

### 4. Clone RE-UE4SS at the matching commit

The build needs UE4SS's **headers** at the commit the installed DLL was built from. The
`-gcac01ee2` suffix of the build string *is* that commit:

```powershell
git clone --no-recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS F:\Tools\RE-UE4SS
cd F:\Tools\RE-UE4SS
git checkout --recurse-submodules=no cac01ee29ca2e2fa723ae3a4e14f0d52b70b226d
```

**No submodules are needed** — every directory `xmake.lua` asks for (`UE4SS/include`,
`UE4SS/generated_include`, `deps/first/*/include`) is tracked directly in the repository, and
`deps/first/Unreal` cannot be cloned by anyone outside the Epic Games GitHub organisation anyway.
`build.ps1` checks for `UE4SS/include/Mod/CppUserModBase.hpp` under the root and reports a wrong
path; point it at your clone with `-Ue4ssRoot` or `$env:WUCHANG_UE4SS_ROOT`.

### 5. Generate the import library (once per UE4SS build)

No UE4SS release asset carries an import library, so one is synthesised from the installed DLL's
export table — `vswhere`, then `dumpbin /exports`, then `lib.exe` over the generated `.def`:

```powershell
.\tools\gen_ue4ss_importlib.ps1 -Ue4ssDll '<Game>\Project_Plague\Binaries\Win64\ue4ss\UE4SS.dll'
```

`-Ue4ssDll` has **no default**: it must be the DLL from the game folder you will run
(`WUCHANG_UE4SS_DLL` works instead). Both outputs — `sdk\UE4SS.def` and `sdk\lib\UE4SS.lib` — are
committed, so this step is only needed when moving UE4SS builds.

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

`-Mode Game__Debug__Win64` for the debug configuration, `-Rebuild` for a full rebuild, `-NoTests`
to skip the offline tests. Output is `build\windows\x64\<Mode>\main.dll` plus its `.pdb`, and `0
failure(s)` from the tests; `main.dll` exports `start_mod` / `uninstall_mod`.

`src/` and `tests/` build with `set_warnings("all", "error")` — `/W3 /WX`, so **a new warning
fails the build**; do not drop `/WX`. `third_party/` is compiled by its own targets at the default
level. The mod DLL also gets `/guard:cf`, `/DYNAMICBASE`, `/HIGHENTROPYVA` and
`/PDBALTPATH:%_PDB%`; see `hardened_link()` in `xmake.lua`.

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

## The offline tests

`tests/markers_test.cpp` links only the PURE sources, so it needs neither UE4SS nor Direct3D and
runs with the game closed. `build.ps1` builds and runs it (`-NoTests` skips):

```powershell
xmake build markers_test
xmake run   markers_test markers      # argv[1] is the marker dir
```

**New testable logic belongs in a PURE module** — that is what keeps it linkable here. Beyond
covering every one of them, the suite is the **drift guard** for three things that would otherwise
diverge silently:

* **the config tiers** — `keys(config_wuchang_minimap.txt) == Player ∪ Advanced`,
  `keys(config_wuchang_minimap_dev.txt) == Dev`, the `key == "..."` literals scraped out of
  `mmstate.cpp` equal every tier plus Legacy, the tiers pairwise disjoint, the shipped file's
  banner order matching the tier tags key for key, and a byte-identical round trip of the in-place
  rewrite over both real files;
* **the shipped marker data** — every `markers/*.json` on each build: each parses with
  `skipped == 0` and `unknown_cat == 0`, no marker id repeats within or across files, every
  `(chapter, category)` clears a floor from a table in the test *including the explicit zeros* (so
  "chapter 5 has no ladder" is a recorded decision rather than a blind spot), the categories the
  game itself pins to a number match it exactly (3 mystery gates, 7 benediction doors - the Sage
  and Discerning Eye achievements), every item id a pickup references is a row of `items.json`, every shrine marker has a row in `shrines.json`, and
  no single name accounts for more than half of a `(chapter, category)`'s named entries;
* **the shipped map assets** — `maps.json` parses at the current schema and the sparse height-plane
  store decodes from the shipped PNGs, with `z_requantise_worst_uu` under 20 uu.

So a change to the config keys, the marker data or the asset format that skips this suite fails
the build.

## Install and deploy

Two install paths, not interchangeable:

| | `deploy.ps1` | `tools\package.ps1` |
|---|---|---|
| For | this machine, while developing | a release a player downloads |
| Writes | straight into the Steam folder | `dist\` only |
| Ships | `main.dll` **and `main.pdb`** | `main.dll` only |
| Configs | never overwrites an edited one (`-ForceConfig` to force) | the pristine shipped defaults |
| Extras | leaves the mod's runtime output alone | asserts none of it is in the package |
| Checks | none | full smoke check + zip round-trip |

`.\deploy.ps1` copies the DLL into
`...\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\dlls\` and creates an empty
`enabled.txt` beside it, which is UE4SS's "load this without touching `mods.txt`" opt-in.
`deploy\ue4ss\Mods\WuchangMinimap\` mirrors the same layout, and the shipped config files live
there. `-GameRoot` points it elsewhere, `-Force` installs before UE4SS is present, `-NoMaps` skips
the map assets, `-Pull` copies in-game navmesh dumps back.

## Release packaging

The procedure — preconditions, stamp, tag, smoke test, upload — is **`docs/RELEASE.md`**. What
`tools\package.ps1` assembles is a tree mirroring exactly what a player copies into
`...\Project_Plague\Binaries\Win64\`:

```
WuchangMinimap-<version>\
  CHANGELOG.md                         from tools\CHANGELOG.template.md
  README.md  LICENSE  THIRD_PARTY_NOTICES.md    copied from the repo root
  BUILD_INFO.txt                       version, commit, branch, mode, DLL size, UE4SS build
  ue4ss\Mods\WuchangMinimap\
    dlls\main.dll                      no .pdb
    maps\maps.json, maps\chapter1..5\*.png
    markers\chapter{1..5,dlc}.json     chapter1.sample.json is excluded
    markers\shrines.json, markers\items.json
    config_wuchang_minimap.txt
    enabled.txt                        empty; UE4SS's "load me" opt-in
```

`main.pdb` goes into a separate `-symbols.zip`, staged in a temp folder so it never touches the
package tree. Three gates run before either zip is written and any failure stops the run: a
**smoke check** (every shipped data file parses at its current schema, everything `maps.json`
names exists, and an **allow-list** over the package tree makes any file the script has not been
taught about a leak by definition), the **consistency check** `tools\check_release.ps1` (one
version and one UE4SS build string across every file that states either, no unfilled placeholders,
every relative link resolving inside the package), and a **zip round-trip** against the tree on
disk.

`.\make_rar.cmd` runs all of it and adds a tested `.rar`. `-NoBuild` packages the existing
`build\` output; `-OutDir` writes somewhere other than `dist\`.

---

## Layout

Grouped by role. The authoritative description of any module is the prose block at the top of its
own file.

| | |
|---|---|
| **entry / lifecycle** | `dllmain.cpp` (`RC::CppUserModBase` subclass, `start_mod` / `uninstall_mod`), `modswitch.*` (the `mod_enabled` master switch; starts and stops every subsystem), `version.hpp`, `breadcrumb.*` |
| **overlay** | one `overlay` namespace across `overlay.cpp` (shared state, UI scale, font, HUD placement, the loop-thread half), `overlay_d3d12.cpp` (device objects, the swapchain hooks, the render entry point, the visibility choke point), `overlay_dcomp.cpp` (the mod's own queue, composition swapchain and DirectComposition visual), `overlay_input.cpp`, `overlay_slice.cpp`, `overlay_hud.cpp`, `overlay_extras.cpp`, `overlay_fullmap.cpp`, `overlay_panel.cpp`, all sharing `overlay_internal.hpp` (`overlay::ovl` holds the state) |
| **game-thread readers** | `gamestate.*` (pawn, view target, menu detection), `markers.*` (the `GUObjectArray` sweep and the found tracker), `highlight.*` (the camera pose), `shrines.*`, `saveslot.*`, `gamebinds.*`, `recon.*`, `navmesh_dump.*` |
| **cross-thread state** | `mmstate.*` (the snapshot seqlock, the config file, the log queue), `spinlock.hpp`, `atomicfile.hpp`, `perf.hpp` |
| **map data** | `mapmanifest.hpp` (PURE `maps.json` parser), `mapdata.*` (chapter residency + the sparse 128-px-block height store), `slicerule.hpp` (PURE; the rule both maps slice and shade by), `pngdecode.hpp` (WIC, shared with `markers_test`) |
| **marker model** | `markers_db.*` (PURE: the chapter JSON, category masks, item quality, the found-file round trip), `shrines_db.hpp`, `marker_dedupe.hpp`, `scriptmap.hpp` (PURE `FScriptMap` decode), `scan_sched.hpp` (PURE slice / wrap / rate arithmetic) |
| **PURE UI logic** | `mapview.*` (the full map's viewport transform and its exact inverse, the zoom ladder, the waypoint file), `compass.*`, `projection.hpp`, `glyphs.hpp`, `label_layout.hpp`, `textmatch.hpp`, `exchange.hpp`, `gamebinds_map.hpp`, `typing_gate.hpp`, `chapterid.hpp` |
| **config** | `config_keys.hpp` (the one key → tier table), `config_rewrite.hpp` (PURE in-place rewrite: values only), `json.hpp` |
| **engine access** | `ue_min.hpp` (hand-written `RC::Unreal` ABI declarations), `uereflect.hpp` (cached property offsets, `UFunction` calls), `mem.*` (`VirtualQuery` + SEH-guarded raw reads), `gamepad.*` (XInput, dynamically loaded, LOOP thread only) |
| **sdk** | `sdk/UE4SS.def` + `sdk/lib/UE4SS.lib` (both generated, both committed), `sdk/shim/GUI/GUI.hpp` (hand-written stand-in) |
| **third_party** | `imgui/` + the dx12 and win32 backends, `minhook/`, `fmt/` (header-only). Unmodified; provenance in `third_party/VENDORING.md` |
| **build / release** | `build.ps1`, `deploy.ps1`, `tools/vs_detect.ps1`, `tools/gen_ue4ss_importlib.ps1`, `tools/package.ps1`, `tools/make_rar.ps1`, `tools/check_release.ps1`, `tools/CHANGELOG.template.md` |
| **map pipeline** | `tools/navmesh/`: `offline/` (paks → tile JSON), `render.py`, `build_map.py`, `mapfmt.py` (the ON-DISK format), `repack_maps.py`, `slice_preview.py`, `marker_coverage.py` |
| **marker pipeline** | `tools/markers/`, driven by `tools/regen_all.py`; `class_graph.json` is a cached artifact |
| **recon** | `tools/lua-recon/` — the WuchangRecon Lua mod and its mock harness. Its `out/` dumps are committed evidence that cannot be re-taken |
| **data** | `maps/` and `markers/`, both **generated**, both deployed into the mod folder. `markers/chapter1.sample.json` is the one hand-written file there and documents the schema |
| **tests** | `tests/markers_test.cpp` — the whole offline suite, one file, `CHECK*` macros |

### Vendored versions

| Library | Version | Constraint |
|---|---|---|
| Dear ImGui | **v1.92.9b** | free — the mod runs its own ImGui context on its own Present hook |
| MinHook | **v1.3.3** | statically linked, so `MH_ALL_HOOKS` can never touch UE4SS's own hooks |
| fmt | **11.2.0** | must equal the version UE4SS pins: `DynamicOutput/Output.hpp` includes `<fmt/core.h>` |

Upstream tags, the files copied and how to re-verify a tree: `third_party/VENDORING.md`.

---

## The UE4SS pin

The official flow — `includes("RE-UE4SS")` against a `UE4SS` target built from source — is not
possible here: `RE-UE4SS/deps/first/Unreal` points at the private `Re-UE4SS/UEPseudo`, so UE4SS
cannot be compiled outside the Epic Games GitHub organisation, and the `zDEV-UE4SS_*.zip` release
asset carries no headers and no import library. So **headers** come from a plain clone of RE-UE4SS
at `cac01ee2` and **linking** goes through `sdk/lib/UE4SS.lib`, synthesised from that same build's
`UE4SS.dll`. One commit for both, so the ABI matches by construction.

Both `sdk/UE4SS.def` and `sdk/lib/UE4SS.lib` are **committed**, because regenerating them needs
the exact `UE4SS.dll` from an installed copy of the game — a file that is not in this repo and
does not exist on a CI runner. Committing the library is what makes `git clone` + `.\build.ps1`
work.

**Moving to a different UE4SS build** means all of: re-checkout RE-UE4SS at the new commit (step
4), re-run `gen_ue4ss_importlib.ps1` against the new `UE4SS.dll` (step 5), rebuild, and re-check
every declaration in `src/ue_min.hpp` against the new `sdk/UE4SS.def`. Headers and import library
from different builds is an ABI mismatch, not a warning. `UE4SS.def` regenerates byte-identically
from the same DLL, so a diff on it means the DLL changed; `UE4SS.lib` does not (`lib.exe` embeds a
timestamp), so **diff the `.def`, never the `.lib`**.

Three consequences, all load-bearing:

* **`sdk/shim` must stay first on the include path.** `<Mod/CppUserModBase.hpp>` includes
  `<GUI/GUITab.hpp>` → `<GUI/GUI.hpp>` → `<GUI/LiveView.hpp>` → UEPseudo. `GUITab.hpp` uses nothing
  from `GUI.hpp`, so `sdk/shim/GUI/GUI.hpp` is a near-empty stand-in that breaks the chain.
* **The CRT must be `/MD`.** The mod API passes `std::string_view`, `std::vector` and
  `std::unique_ptr` across the DLL boundary, and `UE4SS.dll` imports `MSVCP140.dll`. A `/MT` mod
  gets its own heap and its own `std::` internals, and crashes. `xmake.lua` pins
  `set_runtimes("MD")` for every target.
* **`UE4SS_ENABLE_IMGUI()` is unusable** — sharing UE4SS's own ImGui context needs
  `UE4SSProgram.hpp`, which needs UEPseudo, and would pin the vendored ImGui to UE4SS's version.
  `Output::send<LogLevel>` is a header template, so the formatting runs inside `main.dll`; that is
  why fmt is vendored header-only at UE4SS's own version.

**`src/ue_min.hpp`** is how the mod uses the full `RC::Unreal` reflection API without the headers
for it: an MSVC mangled name depends only on namespace, class name, function name, parameter
types, cv/ref qualifiers and the **access specifier** — never on class layout — so the file
re-declares the members the mod calls inside deliberately empty classes, each annotated with the
exact symbol from `sdk/UE4SS.def` it must match. Two rules for extending it: mirror the real
single, non-virtual, offset-0 inheritance chain so `this` needs no adjustment, and keep
`FField::GetNext` **private** behind a friend accessor, because it is private in UEPseudo and `A…`
vs `Q…` is part of the symbol.

---

## Threads

Three, with a strict split; `src/mmstate.hpp` states the invariants.

| thread | what runs there | what it must not touch |
|---|---|---|
| **UE4SS loop** (`CppUserModBase::on_update`) | hotkeys, all file and JSON I/O, PNG decode, the log drain, XInput | — |
| **game** (a `RegisterProcessEventPreCallback` pump) | every `UObject` traversal, reflection and raw read | D3D12; C++ iostreams and the C++ locale, which fault when touched from this game's game thread — it queues log text and parks results |
| **render** (the hooked `Present`) | everything ImGui and everything D3D12; the only thread that may release a D3D12 object | any `UObject` |

The game thread publishes an `mm::Snapshot` through a seqlock and the render thread reads it.
Because only the render thread may release a D3D12 object, `modswitch`'s stop is a three-step
state machine.

**There is no `std::mutex` anywhere** — `std::mutex::try_lock` faults against the MSVCP140 loaded
in this process. Locking is `spinlock.hpp`'s `std::atomic_flag` spinlock plus non-blocking
`std::atomic` exchanges.

Errors are logged and survived, never thrown across a boundary: every raw engine read goes through
`mem::read` (`VirtualQuery` + an SEH-guarded `memcpy`), and the Present hook wraps the frame in
`catch (...)` so nothing unwinds into DXGI. **Nothing derives a struct offset without validating
it against something independently measured, and logging the decision.** Logging is `mm::log` /
`mm::logf` (wide, always emitted) plus `MM_LOGV` / `MM_LOGT` gated on the `log_level` key. ---

## Runtime map

What each subsystem is, which file owns it, and the one thing about it that is not obvious from
reading that file's code. The file's own header comment is the full account.

**The overlay's hooks** — `overlay_d3d12.cpp`. `Present`, `Present1` and `ResizeBuffers`,
MinHook'd at addresses read off a throwaway device + queue + swapchain, because the game's
swapchain is not reachable from a UE4SS mod. *Discovery runs on every launch and must:* creating
those objects drives a ReShade proxy, its addons, Streamline's interposer and the Steam overlay
through their own creation interposers before MinHook writes a byte, and a launch that hooks
addresses cached in a file instead **intermittently black-screens from the first frame**, with the
mod presenting normally and nothing in any log.

**The surface it draws on** — `overlay_dcomp.cpp`. The game's swapchain is *followed* for geometry
and the frame tick; the queue and the surface are the mod's own (a DIRECT queue, a
`CreateSwapChainForComposition` swapchain, a DirectComposition visual). *The game's back buffers
are never written and nothing of the game's is submitted on*, which is what makes the overlay
survive frame generation and capture layers, and why nothing has to be probed before the first
frame. The three hooks still call the original unconditionally for **every** swapchain, so nothing
else in the process loses a frame to us.

**Device loss** — `overlay_d3d12.cpp`. `GetDeviceRemovedReason()` is asked on whichever thread
presented, because the removal that matters most is the one after which no Present ever arrives to
ask in. *Nothing is ever rebuilt on a dead device*: ImGui's font upload waits on a fence with no
timeout and would wedge the render thread inside the game's own recovery. A recoverable failure
asks for a re-adoption instead, capped per stretch.

**Visibility** — `set_hide_reason()` in `overlay_d3d12.cpp` is the single choke point, and
`hud_gate()` in `overlay_internal.hpp` is the one evaluation the minimap, the compass and the
highlight all ask. *Nothing latches anywhere in the path* — every condition is re-evaluated from
the live snapshot every frame. The F2 panel prints the current reason and every transition goes to
the log.

**Menu detection** — `gamestate.cpp` + `scan_sched.hpp`. `IsInViewport()` on a root `UserWidget`
whose `Visibility` is `Visible` is the whole decision; three finders supply the roots (a
watchlist, a UI-event path on the `ProcessEvent` context itself, and a sliced `GUObjectArray` walk
as the fallback) and none of them latches. *The UI-event path does raw reads only* — it runs
outside the re-entrancy guard, so it may never issue a `ProcessEvent`, and it decides nothing.
Pointer-keyed caches are dropped with the pawn, because a recycled address would answer from the
wrong entry; confirmed menu-root **class names** survive, so the same class is recognised at once
after a level load.

**The marker sweep** — `markers.cpp` + `scan_sched.hpp`. `FindAllOf` walks the whole object array,
so one call per class is one full walk per class; the sweep inverts that into one walk per round
in slices of `markers_scan_chunk` slots per pump, with the `UClass* -> marker spec` table memoised
per class so the super-chain name walk happens once per class per level. *Positions are read raw*
(`RootComponent` → `RelativeLocation`), never through `K2_GetActorLocation`: a `ProcessEvent` per
actor inside the engine's own call stack is not affordable. The slice is called from every
`ProcessEvent` pre-callback, not from the 10 Hz position pump, and throttles on
`QueryPerformanceCounter` because `GetTickCount64`'s ~15.6 ms granularity is coarser than a
one-frame slice period.

**The camera pose** — `highlight.cpp`. The `FMinimalViewInfo` offset inside `CameraCachePrivate`
is **discovered, not assumed**: the three camera getters are called once and the struct's first
bytes are scanned for the offset whose six doubles and following float match what they said. *A
pinned offset that produces eight insane reads in a row is dropped and re-discovered.* Reads cost
nothing at all while the highlight is disarmed and the compass is off.

**Height slicing** — `slicerule.hpp`, pure C++, so the offline tests run the same code both maps
do. Per pixel: a surface within `floor_z_tolerance` of your feet wins outright and opaque; failing
that the lowest surface up to `shade_above_band_uu` overhead; failing that the highest surface
below, however deep. *The full map always slices as if the band were infinite* —
`shade_above_band_uu` is the minimap's key only — and the priority order is what keeps that safe,
since a floor underfoot still wins and no ceiling is ever drawn over the player. Colour is
**absolute height** on one ramp for all three classes, its ends percentiles of the Z the cut
actually drew; the full map equalises that ramp against its own cut (`shade_map_equalize`), the
minimap keeps it linear.

**The full map** — `overlay_fullmap.cpp` + `mapview.*`. *It adds no copy of the asset*: it cuts
its own small decimated RGBA texture out of the same height planes the minimap slices, covering
the viewport plus a 30 % margin, and re-cuts only when the view leaves that region, the zoom or
floor slice changes, or the player crosses a storey. The live sweep still owns the truth —
un-marking a chest the game reports as `Used` is undone on the next round, because the tracker
follows the save, not the mod. Waypoints live in one fixed-capacity POD behind a spinlock, because
every draw site copies the whole set inside Present and a `std::vector` there would allocate.

**The x-ray highlight** — `overlay_hud.cpp` + `projection.hpp` + `highlight.*`. "Through walls" is
free: the overlay is composited on the finished frame, so there is no occlusion test, no
CustomDepth and no material. *The toggle is the one piece of latched input state in the mod*, so
it is cleared from live state and never remembered — `hl::drop_caches()` turns it off on every
level transition and every dropped pawn. Item-quality colours are the game's own pickup-beam
grouping, not a rarity ladder the game does not have; `markers_db.hpp` documents the three tiers
and why tier 0 keeps its category colour.

**The player's own key bindings** — `gamebinds.*` + `gamebinds_map.hpp` (PURE). The Keys tab
warns when a mod hotkey lands on a key the game already wants, read out of the running game rather
than guessed. *The game is stock UE 5.1 Enhanced Input with no user-settings object*, so a remap
made in the options menu is visible in exactly one place, and `gamebinds.hpp`'s header comment is
the map of the chain to it.

**The compass strip** — `compass.*`, pure. The heading is the **camera's** yaw when a pose is
fresh and the pawn's yaw otherwise, so it works with `highlight_enabled = 0` and during the camera
reader's warm-up.

**The save-slot ladder** — `saveslot.*`. Four rungs — the game's own KV accessor, its
settings-saver path string, the newest `.sav` on disk, then a shared file — each logged with the
route that answered. *Rungs 2 and 3 must answer with the same key for the same save* (the slot
name alone, never the Steam account id the filesystem path carries), or one playthrough splits
across two files mid-session as one rung's answer replaces the other's. A slot with no file of its
own is seeded once from the shared file.

**The collection tracker** — `markers.cpp` + `markers_db.*`. Found state is merged with the live
sweep on a **stable id**: the game's own shrine id for shrines, `<level short name>/<actor object
name>` for everything else. *Absence from the object array is not evidence of a collect* — an
unloaded level looks identical — so only state flags auto-mark, plus one guarded absence rule
whose safety rail is that a marker whose owning level cannot be matched to a loaded level is
**never** marked. The predicate is pure and its truth table is in the test suite.

| category | "found" means |
|---|---|
| shrine | the save's global `UnlockedFirepoints` list, not a per-actor flag. Never auto-marked: the tracker follows collectables, not rest points |
| chest, door, mystery gate, benediction door | the actor's `Used` flag, the one `SavedStatuKey=statu_use` names, so the save restores it. The special doors' own `DoorOpen` drives the dissolve animation and is false again after a reload |
| pickup, hidden | `dying`, or the actor parked at `(0,0,0)` |
| fog gate | `Active` |
| npc, note | MET — seen loaded near the player. A used-up NPC is made invisible, not moved, so the live twin is tested for visibility |
| boss | DEFEATED — zero health, or the arena's `bossdoor_*` point unlocked in the save |
| enemy | never written. A static entry is a **spawn point**; the live pawn overwrites its position under the same id |
| ladder, lift | never — navigation aids |

**The navmesh dumper** — `navmesh_dump.*`, **off by default** behind the `navmesh_dump` Dev key
(read once at start-up, so arming it takes a restart; there is deliberately no hotkey for arming a
memory scan that writes files). The map background comes from the paks, so this covers only cells
the paks do not carry and navmesh carved at runtime. *UE modifies `dtMeshTile` and
`dtMeshHeader`*, so the tile stride and every field offset are measured against something
independently known — the reflected `TileSizeUU` and `AgentRadius`, the owner back-pointer, the
gcd of the `DNAV` header spacing, every vertex inside `bmin..bmax` — with both the float and the
double `dtReal` layouts tried and the winner logged as one `PIN LINE`.

### Settings

`config_wuchang_minimap.txt`, plain `key = value`, `;` or `#` starts a comment. **Every key is
documented inline in the file itself**; that file and `config_wuchang_minimap_dev.txt` are the
reference for what a key does and what its default is. `src/config_keys.hpp` is the one table that
says which **tier** each key is in:

| tier | where it lives | what it is |
|---|---|---|
| **Player** | `config_wuchang_minimap.txt` under `; ---- PLAYER SETTINGS ----`; the F2 panel's player tabs | something a person tuning the HUD would plausibly change |
| **Advanced** | the same file under `; ---- ADVANCED ----`; F2 → *Debug* → *Tuning* | correct as shipped; changed to answer a symptom |
| **Dev** | `config_wuchang_minimap_dev.txt`; F2 → *Debug* | a dial that exists because a developer needed one |
| **Removed** | nowhere | a single warning naming it, then ignored |
| **Legacy** | nowhere | renamed; `cfgkeys::renamed_to` maps it to its current name |

**`config_wuchang_minimap_dev.txt` is not part of a release.** It is read only if it exists, in
the same folder, **after** the player config — so a key set in both wins there — and `package.ps1`
throws if it finds one in the staged package. `deploy.ps1` copies it. The 1 Hz timestamp watch and
F5 look at **both** files, so editing either reloads both.

Three keys take effect only on restart or on a `mod_enabled` off/on cycle, because each is read
once during start-up: `overlay_hooks`, `srv_heap_size` and `navmesh_dump`.

**Two off switches, and they are the first thing to ask a bug reporter for.** `mod_enabled = 0`
makes the whole DLL inert — the hooks are not installed (and are cleanly disabled if they already
were, the trampolines kept so turning it back on can never double-hook), the `ProcessEvent`
callback returns on its first statement (UE4SS exports no *Unregister*, so that early return is
the mechanism), no scan runs, the height maps are freed. All that keeps running is one
`GetFileAttributesEx` of the config per second, so setting the key back to `1` restarts the mod
within a second — but **F5 does not work while the mod is off**, because nothing samples the
keyboard. `overlay_hooks = 0` is narrower: nothing of the mod goes near DirectX, while the
game-thread reader, the tracker, the found file and the log carry on. That splits the render half
off from everything else in one line and a restart.

There is no Save button: every place that publishes a UI-edited config raises a flag the loop
thread consumes on a 750 ms debounce, and `mm::save_config_file()` rewrites **only the values** of
both files through `cfgrw::rewrite`, so comments, ordering and keys this build does not know
survive it.

Accepted hotkey names are F1-F5, F7, F8, any single letter or digit, TAB, SPACE, ENTER, BACKSPACE,
the arrows, INSERT/DELETE/HOME/END/PAGEUP/PAGEDOWN, NUM0-NUM9 and the numpad operators,
MOUSE3-MOUSE5, the L/R modifier keys and `none`, with one optional `ctrl+` / `shift+` / `alt+`
prefix. **F6** (RenoDX), **F9**/**F11** (engine binds), **F10** (game console) and **F12** (Steam)
are rejected in code, not merely discouraged in a comment.

---

## Traps

- **The map schema string lives in six files** — `src/mapdata.hpp`, `src/mapmanifest.hpp`,
  `tools/navmesh/build_map.py`, `tools/navmesh/mapfmt.py`, `tools/navmesh/slice_preview.py` and
  `tools/package.ps1`. Grep it and change them together.
- **A height code is 12 bits of Z plus bit 12 = reachable.** Always mask with `mapdata::z_code()`
  and ask `HeightMaps::reachable()` for the flag. The layout, the coverage index and which schemas
  are accepted are specified in `src/mapmanifest.hpp`'s header comment.
- **Keep `slice_preview.py`'s `slice_window()` and `overlay_slice.cpp`'s in step.** They are the
  same rule, offline and online.
- **Regenerate, never hand-edit:** `maps/`, `markers/*.json` (except `chapter1.sample.json`),
  `sdk/UE4SS.def`, `sdk/lib/UE4SS.lib`, `tools/markers/class_graph.json`.
- **`xmake clean --all` drops the cached VS environment**, so it runs *before* `xmake f`, never
  after — otherwise the next compile starts with an empty `INCLUDE`. `build.ps1 -Rebuild` gets the
  order right.
- **`src/version.hpp` is the single source of the version.** `package.ps1 -Version x.y.z` rewrites
  it and `xmake.lua`'s `set_version` together, and nothing else may touch either.
- **`.gitattributes`:** text is LF in the repo and CRLF on checkout; `*.png`, `*.lib`, `*.pak` and
  friends are `-text`. No Git LFS — the map PNGs are irreplaceable without another extraction run.
- **Conventions:** one short lowercase namespace per module; `snake_case` functions and variables,
  `kPascalCase` constants, `enum class`, Allman braces at 4 spaces with braces even on
  single-statement `if`s, lines under ~100 columns, no `.clang-format`. Comments are a prose block
  at the top of each file — what the module is, which thread it runs on, what it must not do — plus
  short notes above non-obvious declarations, present tense, no change history.
- **No art assets.** Category glyphs are `ImDrawList` primitives, and every category is
  distinguished by shape as well as colour (asserted in the tests for every palette).

---

## The map asset pipeline

`maps/` is one palette-PNG composite plus eight 16-bit height planes per chapter, and one
`maps.json`. `mapfmt.py` **owns the on-disk format** — schema string, palette encoder, height
quantisation — so a fresh build and a re-encode cannot disagree; `build_map.py` imports
`render.py`, so the loader, the richest-copy dedupe and the flat-plane filter are shared with the
raw renderer. Plane *k* holds, at every pixel, the Z of the *k*-th walkable surface from the
bottom.

```powershell
# paks -> tile JSON
python tools\navmesh\offline\pak.py unpack "<...>\Project_Plague-Windows.pak" `
       --grep "Maps/Generate/Chapter1/EX0/" --out <scratch>
python tools\navmesh\offline\navchunk.py "<scratch>\...\Chapter1\EX0\*.umap" `
       --out tools\navmesh\dumps_offline --stamp 20260902_ch1

# tile JSON -> the shipped assets  (needs ~400 MB of tile JSON that is NOT in the repo)
cd tools\navmesh
python build_map.py --input dumps_offline --chapter chapter1 --out ..\..\maps

# format-only changes need no dumps at all
python tools\navmesh\repack_maps.py --dry-run     # measure, write nothing
python tools\navmesh\repack_maps.py               # re-encode maps\ in place

# reproduce "the floor looks wrong at X" without launching the game
python tools\navmesh\slice_preview.py --x 19537 --y 4587 --z 2505 --out temple.png

# raw runtime dumps -> floor PNGs, for checking the offline path against the game
.\deploy.ps1 -Pull
python tools\navmesh\render.py --input tools\navmesh\dumps --out tools\navmesh\out --debug
python tools\navmesh\render.py --synthetic --out out_synthetic --debug   # self-test
```

`repack_maps.py` re-palettises the composite from its own pixels and re-scales the planes from
their own codes, verifying every PNG by decoding the bytes back before they land, and stamps the
requantisation error into the manifest as `z_requantise_worst_uu` (the tests fail above 20 uu).
Use it when only the encoding changes; use `build_map.py` when the geometry, the filters or the
resolution change.

`px_per_uu` is **per chapter**, each scaled to its own RAM budget (0.032 to 0.060). `--px-per-uu`
is a request: it is scaled continuously to fit `--max-ram-mb`, then halved until neither dimension
exceeds `--max-dim`. The planes live in ordinary RAM, read by a CPU loop and never sampled by the
GPU, so `mapdata::build_plane()` allocates only the non-empty **128-px blocks**; a row of the
picture therefore crosses several blocks and there is no row pointer — gather one with
`HeightMaps::gather_row()`, which returns `false` for a row with no surface at all.

**Reachability (bit 12).** The background is every walkable Recast polygon the game cooked, which
includes wall tops, roof ridges and the outside faces of arena walls. `build_map.py` floods a
directed 8-neighbour surface graph — an edge wherever the neighbour is no more than
`--reach-step-up` higher, so a walk, a small step up or a fall of any depth — from **every marker
in `markers/<chapter>.json`, every category**, and flags what it reaches. Nothing is deleted; the
runtime decides what to do with the rest (`map_unreachable`: `hide` | `dim` | `show`). *The seeds
are the whole lever*, so the risk is an area whose only access is a ladder, a lift or a jump and
which holds no marker at all — every unreached blob of 400 m² or more is listed in `maps.json`
under `reachability.big_unreached` with its centre and Z, so "why is there a hole here" starts
from a table. `--reach-seeds-extra <json>` adds seeds from a recorded player track; `--no-reach`
flags everything.

## The marker data pipeline

Everything under `markers\` except `chapter1.sample.json` is generated from the game's paks by
`tools\markers\*.py` (Python 3.10+, `pycryptodome` for the AES-encrypted pak index). One command
regenerates all of it:

```powershell
python tools\regen_all.py                 # every step, all six chapters
python tools\regen_all.py --verify        # + score the result against the recon dumps
python tools\regen_all.py --no-pak-hash   # skip the sha256 of the pak set while iterating
python tools\regen_all.py --list          # the step graph
python tools\regen_all.py --only extract  # one step (repeatable); --skip is the inverse
```

It finds the game through `WUCHANG_PAK`, `WUCHANG_GAME_ROOT` or `--pak`, and before the first step
checks that the paks are readable, that **both `_N_P` patch paks are present** (the DLC levels and
the patched `DT_FirePoint` live in them), that `pycryptodome` imports and that `markers\` is
writable. A failing step **stops** the run: every later step reads what an earlier one wrote, so
continuing would leave a half-updated `markers\` that no diff can be trusted against.

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

Only `chapter*.json`, `shrines.json` and `items.json` are **shipped**; the rest are toolchain
artifacts baked into the chapter files, so the runtime reads three files rather than nine. Two
things about the order are the reason the driver exists:

* **`bossdoors` is a genuine cycle.** It joins the level scripts' `ST_LevelScriptBossData` to the
  boss marker ids `extract_markers.py` writes, and `extract_markers.py` bakes the resulting
  `bossdoor` id back onto those markers — so a full run is extract → bossdoors → extract. The driver
  skips the second extraction when `bossdoors.json` came out byte-identical, the normal case.
* **`categories` and `enemies` must precede `extract`**, because `marker_classes.py` reads both at
  import time. Running `extract` first falls back to a hand-written class list that ships **fewer**
  markers, and says so on stderr.

The run ends with a summary table and the delta against whatever was in `markers\` before. **A
regeneration should move names, items, categories and new entries, and no coordinate of an
existing marker.**

**Category assignment is a class-graph question.** `markers/categories.json` is the descendants of
one base class per category, read out of every cooked `.uasset` export map's `super` field
(`class_graph.py`, cached and committed). `build_categories.py` holds the choice of bases and the
precedence; everything below a base is data. **The precedence is load-bearing**: five `BP_NPC_C`
descendants have a category of their own, `BP_PickUpActor_Trap_C` is a `BP_PickupActor_C`
descendant that must be `hidden`, and `boss` precedes `enemy` because the boss classes are
`BP_BaseAI_C` descendants too. `BP_BaseAI_C` having zero overlap with `BP_NPC_C` is what makes
`enemy` a class question rather than a level-name one; the `*_AI` sublevel heuristic survives as a
counted fallback, and the extractor prints the rule mix plus **every class in a `_logic` level
that matched no category**.

**Three answers are simply absent from the game data**, so do not go looking for them again:
ordinary enemies have no name anywhere (`build_enemies.py --prove` re-runs all four checks that
establish this, so they read "Enemy" rather than a tidied Pinyin class name); the DLC has no
fire-point rows, so its shrines carry the marker's own label and no `BirthPosition`; and every
configured DLC pickup holds the same placeholder item id, the default index of the blueprint's own
editor tool, which `extract_markers.PLACEHOLDER_ITEM_IDS` suppresses.

**Provenance.** Every generated file carries `game_build`, `exe`, `pak` and `extractor_commit`.
Know what `game_build` is worth on this title: Leenzee ship no game build number, so it is the
*engine* version and does not move across game patches — **the identifiers that do move are the
digests**, cached in `tools/markers/.provenance-cache.json` because hashing the pak set is slow
(`--no-pak-hash` records sizes only). Every field is optional to a reader, which keeps the schema
at `.../1`.

**Verification.** `verify_markers.py` scores the extracted coordinates against the WuchangRecon F8
world dumps on the same `(level short name, cooked object name)` join key the engine reports
in-game, defaulting to the dumps committed to this repo, so it runs on a fresh clone with no game
installed. The dumps cover only what was streamed in when F8 was pressed, so most chapters report
"not loaded in any dump" and that is not a failure; the markers they do cover agree at a median
error of 0 uu, the exceptions all being a moving platform whose cooked transform is its authored
start. That is why the regen driver runs this as a report and not as a gate.

---

## The files the mod writes

All next to the mod's own DLL, all gitignored, and the packager fails if any reach the package
tree.

| file | what it is |
|---|---|
| `wuchang_minimap_found[_<slot>].txt` | the collection tracker: one stable id per line, sorted, comments allowed, debounced, written through a temp file swapped into place so the real file is either the old one or the new one. Keeps one `.bak`. One per save game; the suffixless one is shared, used when no slot can be identified and seeded into a new slot's file once |
| `wuchang_minimap.log` | the mod's **own** copy of everything it logs, rotated per launch (`.1`, `.2`, `.3`), because UE4SS truncates `UE4SS.log` on every launch. Always on, no config key. **This is the file to ask for in a bug report** |
| `wuchang_minimap_watchdog.txt` | written **only** when the game stops responding: the loop thread watches the Present and pump counters and after six seconds of either not moving appends one line naming which thread stopped and the stage each was last in |
| `wuchang_minimap_last_stage.txt` | the crash breadcrumb. The log is flushed at every stage transition, so the two always agree about the last thing that happened |
| `wuchang_minimap_waypoint[_<slot>].txt` | play state, not settings: one hand-editable `waypoint = <x> <y> <z>` line each |
| `wuchang_minimap_panel.txt` | the F2 fold bits. **Positional**, so the enum in `overlay_panel.cpp` is the file format — a token bump is how a changed enum invalidates an old file |
| `wuchang_minimap_export_<date>_<time>.json` | on demand; imported back as a **merge**, never a replacement |

The watchdog line is assembled in a stack buffer and written with flat `CreateFileW` / `WriteFile`
/ `FILE_FLAG_WRITE_THROUGH` and **no allocation at all**, because the failure it describes can be
a wedged process heap, in which case `std::format` would hang the last thread still running. A
crash leaves a `CrashContext.runtime-xml`; a hang leaves nothing, which is what that file is for.

## Not implemented

- **X-ray highlight v2**: true silhouettes through `SetRenderCustomDepth` plus a post-process
  material shipped in a tiny pak. The game ships no outline material to reuse.
- **The slicing loop as a pixel shader.** Its own root signature, PSO, `D3DCompile` and
  `ImDrawList::AddCallback` juggling on a wrapped swapchain, to save a few ms per update and the
  upload; the CPU slicer already has the exactly-correct semantics.
- **A DLC map.** The paks carry no navmesh cells for it; it would need a runtime cell sweep or an
  ortho-capture fallback.
- **A whole-region runtime navmesh dump.** Only the cells around the player are resident, so it
  needs a sweep over all streaming cells.
