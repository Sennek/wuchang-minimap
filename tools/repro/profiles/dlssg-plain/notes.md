# `dlssg-plain` — reporter 1's box, as far as this one goes

The plainest configuration any report has failed in: UE4SS 1.79, the mod, and **nothing else** in
the DXGI or D3D12 slot. What it keeps is what the game ships — Streamline's `sl.interposer.dll` and
`nvngx_dlssg.dll`, which load on every launch from `Engine\Plugins\Runtime\Nvidia\` whether frame
generation is on or off. That is the "DLSS-G loaded" half of his machine, and it needs no payload
at all; the profile's whole payload list is *removals*.

## This is a configuration, not a repro

**His failure has never been reproduced on this box.** Fourteen instrumented in-game runs, two
machines and three game installs — `_done/bugreport1-device-removed/context/reported-crash-dossier.md`,
section 3, and the `repro-run*` / `verify-*` folders beside it. The cause was read off his logs
(`Present` returning `DEVICE_REMOVED` on a back buffer belonging to DLSS-G, `DXGI_ERROR_ACCESS_DENIED`)
and fixed by `9ce35ee`, which stopped the overlay drawing into the frame at all.

So this profile promises **HEALTHY** and asserts the install. A profile that asserted his crash
would be asserting somebody else's machine, and it would fail every single run for the one reason
that is not a finding.

If it ever comes back CRASH, that is the report reproducing for the first time — and the manifest
already carries everything needed to say so.

## Four things this box cannot be

Measured, not assumed, all from the same section of the dossier:

| his box | this box |
|---|---|
| `Present` reports **`no detour`** | the Steam overlay injects `GameOverlayRenderer64.dll` even with "Enable the Steam Overlay while in-game" unticked for app 2277560, so Present is always `ALREADY DETOURED` |
| a **2-back-buffer** swapchain | no reachable setting produces one. Tried: vsync, the game's own frame generation both ways, `r.Streamline.DLSSG.Enable`, `FramesToGenerate`, `r.FidelityFX.FI.Enabled` + `FI.OverrideSwapChainDX12`, screen mode, windowed 2560x1440, `r.D3D12.UseAllowTearing=0`. The executable's cvar table has no back-buffer-count cvar at all |
| 2560x1440, HDR on | three 1080p SDR panels |
| RTX 5060 Ti, driver 32.0.16.1062 — his own GPU and driver | this box's |

Streamline itself is **byte-identical** on both: `nvngx_dlssg.dll` and `sl.interposer.dll` match his
sizes and stamps exactly. So the difference is not the frame-generation stack's version.

## The in-game graphics selection is read, never written

His box ran DLSS with **frame generation off in the menu**. That setting does not live in any ini —
it is in `Saved\<user>\GameConfig\GameConfig.sav`: a 10-byte marker, a zlib block, the same marker
as footer, JSON inside. `game_config_expect` decodes it and asserts
`superresolution = dlss`, `framegeneration = 0`.

It is **never written**, and the reason is the rest of that blob: `playedtime`, `equipment_*`,
`playerlevel`, `playerlocation`, `newgameplus`, the achievement list. A profile that pinned this
file would roll the owner's progress back to whenever it was captured, on every apply. So the
graphics selection is a precondition the apply reads back — like `manual.hags` — and an apply
refuses, naming the key, when the game's menu disagrees.

## What is measured here and what is taken from the report

| | |
|---|---|
| **From the report** | the whole configuration: UE4SS 1.79, no PAK mods, no ReShade, DLSS-G loaded, frame generation off, HAGS on, `HookInitGameState = 0` |
| **From the report, and his alone** | the verdict. `DEVICE_REMOVED` then a 120 s RenderThread timeout, on mod 1.0.0 |
| **Measured on this box** | that Streamline is the same build here; that his hook position reproduces exactly and does **not** reproduce the failure; that his `no detour` and 2-buffer swapchain are unreachable |
| **Not managed by the script** | UE4SS's own settings. `HookInitGameState = 0` is a `manual` item: changing it on this box would quietly stop this profile describing his machine |

## The cell worth running that this profile does not carry

A run pinned to `mod.build: dist:1.0.0` — the build he had. Its result is information in **either**
direction: a CRASH would reproduce the report for the first time, a HEALTHY would confirm what
fourteen runs already say. Neither is a failure of the profile, which is why it is a separate run
with `verdict: any` rather than an `expect` here.
