# WuchangMinimap

A UE4SS C++ mod for **Wuchang: Fallen Feathers** (Unreal Engine 5.1.1, Windows x64, DX12).

Current state: **skeleton only.** The mod loads under UE4SS, logs `WuchangMinimap loaded`,
and has Dear ImGui (with the DX12 + Win32 backends) and MinHook compiled and linked into
`main.dll`. No hooks are installed and nothing is rendered yet.

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
sdk/shim/GUI/GUI.hpp       stand-in header, see below
sdk/UE4SS.def              UE4SS.dll export table, generated
sdk/lib/UE4SS.lib          import library, generated
third_party/imgui/         Dear ImGui v1.92.9b + backends/{dx12,win32} + misc/cpp
third_party/minhook/       MinHook v1.3.4
third_party/fmt/           fmt 11.2.0, headers only (FMT_HEADER_ONLY)
tools/gen_ue4ss_importlib.ps1
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
  new `UE4SS.dll`, rebuild.
- `UE4SS_ENABLE_IMGUI()` (sharing UE4SS's own ImGui context, e.g. for `register_tab`) is
  **not** usable as things stand: it lives in `UE4SSProgram.hpp`, which needs UEPseudo, and
  it would additionally require our vendored ImGui to be exactly v1.92.1. The plan is our
  own ImGui context on our own DX12 Present hook, so this does not block the minimap.
- `Output::send<LogLevel>(...)` is a header template: the formatting runs inside `main.dll`
  and only `Output::DefaultTargets::get_default_devices_ref()` is imported. That is why fmt
  is vendored header-only, and why the fmt version has to match UE4SS's.

---

## Next steps

- [ ] Hook `IDXGISwapChain3::Present` with MinHook, create our own ImGui context, init
      `imgui_impl_dx12` + `imgui_impl_win32`, subclass the game's WndProc.
- [ ] Read player world transform + level bounds from UE, draw the minimap.
