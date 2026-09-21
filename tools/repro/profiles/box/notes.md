# `box` — the owner's dev box as it stands

Not an idealised machine. Everything below was measured on 2026-09-19, not remembered.

## What loads into a launch

ReShade 6.8.0.2155 sits in front of DXGI as `dxgi.dll` and pulls in `renodx-dlss.addon64` through
`LoadFromDllMain` in `ReShade.ini`. NVIDIA's DLSSNR runtime (`nvngx_dlssnr.dll`, 165 MB) arrived in
the same install batch. `renodx-dlss5.addon64x` is on disk but **inert** — ReShade scans `*.addon`
and `*.addon64`, and that extension is neither.

UE4SS is loaded by its own injection proxy, `dwmapi.dll` — a UE4SS file, not a Windows one. It then
loads two C++ mods, **WuchangMinimap** and **WuchangRecon** (both by an `enabled.txt`, neither
through `mods.txt`), and six Lua mods enabled in `mods.txt`: `CheatManagerEnablerMod`,
`ConsoleCommandsMod`, `ConsoleEnablerMod`, `BPML_GenericFunctions`, `BPModLoaderMod`, `Keybinds`.

**Every measurement this project has ever taken had all of that in the process.** That is why this
profile declares them as present instead of omitting them.

## The mod it carries

`mod.build` is `keep`: the installed `main.dll` is a **local dev build** (md5
`92bc8a4719395d9d8f3485043559072b`, 2026-09-18), newer than 1.3.0 and matching no release. It is the
build the 2026-09-19 composition measurements were taken with. Pin a release with
`mod.build: "dist:1.3.0"` when a cell needs a shipped one.

The installed mod folder carries no `BUILD_INFO.txt` — that file sits at a dist folder's root and is
not part of what `deploy.ps1` installs, so a build is identified here by md5 and nothing else.

## Two files that must not be hash-checked after a launch

`ReShade.ini` is rewritten by ReShade while the game runs, so it is declared `volatile`: it is
placed and restored like any other payload, and a mismatch after a run is information, not a
failure. `ReShade.log` is truncated and rewritten every launch and is not a payload at all.

## Hardware this profile does not reproduce and cannot

- NVIDIA GeForce **RTX 5060 Ti**, driver `32.0.16.1062` (2026-06-11)
- three monitors: 1920×1080 @ **75 Hz** primary, 1920×1080 @ 100 Hz, 1080×1920 @ 75 Hz rotated
- HAGS **on** (`HwSchMode = 2`), read from the registry, not asked for
- driver-level vsync / g-sync: the owner's word, not readable from a script

A reporter's panel, GPU and driver are none of these. A run's report says which of their hardware
facts were not reproduced rather than implying they were.

## Neighbours

- `stock` is this profile with every third-party payload removed and neither C++ mod loaded.
- `reshade-renodx` is this profile plus RTSS running and the mod pinned to 1.1.1 — firehoooo's
  E_ABORT box. RTSS is installed here (`C:\Program Files (x86)\RivaTuner Statistics Server`) and not
  running; it runs elevated and is closed from its tray icon, so it is a `manual` item a script
  checks and never starts.
