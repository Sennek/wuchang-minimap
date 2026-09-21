# `stock` — the floor a performance number is honest against

`box` with every third-party payload removed and neither C++ mod loaded. It exists because
"the mod turned off" is not a baseline: once the overlay's composition target has existed in a
process, the window never returns to `Hardware: Independent Flip`, so the only honest control is a
launch that never had it.

## What it removes

The ReShade proxy `dxgi.dll` — which takes ReShade and every addon with it — plus
`renodx-dlss.addon64`, the inert `renodx-dlss5.addon64x`, `nvngx_dlssnr.dll`, both ReShade `.ini`
files, and `d3d12.dll` (OptiScaler's slot, empty here and asserted empty). `mod.state` is `absent`
and `recon` is `absent`: both `enabled.txt` files are renamed aside, so neither C++ mod starts.

## `absent` means the mod never starts, not that its DLL is gone

Measured 2026-09-20 and confirmed in RE-UE4SS's `UE4SSProgram.cpp`: `CppMod`'s constructor
`LoadLibrary`s `dlls/main.dll` for **every** mod folder that has a `dlls` directory, and
`enabled.txt` gates only the later `start_mod()` call. So in a `stock` cell
`…\WuchangMinimap\dlls\main.dll` is in the process's module list, while nothing of ours runs: no
hooks, no threads, and the mod does not even rotate its log, which is how a cell proves it.

That is why this profile asserts the absence in the **log**, not in the module list. Keeping the
DLL out of the process altogether would mean moving the `dlls` directory aside so UE4SS constructs
no `CppMod` at all — a different state, not yet offered.

## What it keeps, deliberately

**UE4SS stays.** Its injection proxy `dwmapi.dll` and `UE4SS.dll` are untouched, and the six Lua
mods enabled in `mods.txt` still load. Nothing in this pipeline ever installs, moves or modifies
UE4SS — the DLL links against exactly one build of it, and a profile that swapped it would be
testing nothing. A future "no UE4SS at all" cell is `dwmapi.dll` moved aside; it is not this one.

Game settings are identical to `box`, so the difference between the two profiles is the payloads and
the mods and nothing else.

## What this is NOT

**It is not the cell the 2026-09-19 table calls "the mod not loaded".** That cell was `box` with
`mod: absent` — ReShade, RenoDX and DLSSNR were all still in the process, and it read
`Hardware: Independent Flip`, 8.846 ms p50. `stock` is a floor beneath that one and has not been
measured yet. Do not compare a `stock` number to that table without saying so.

The four mod states are an axis, not four profiles: `apply box -Mod absent` reproduces that cell
exactly, and `apply stock` is the further step down.

## One cell of that table no launch can reproduce

`hooks installed, mod_enabled = 0` was reached by writing the key while the game ran. Launching
into `-Mod off` is not the same state: the hooks are never installed at all, so the composition
target never exists and the window is never demoted. That cell is a property of a session, not of a
configuration, and the pipeline cannot and should not offer it.
