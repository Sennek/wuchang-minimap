# `reshade-renodx` — firehoooo's E_ABORT box

`box` plus two things: **RTSS running**, and the mod pinned to **1.1.1**. Everything else — the
ReShade proxy, the RenoDX addon, DLSSNR, HAGS, the game's settings — is already what this install
carries, which is why this profile is a two-line diff against `box` rather than a configuration of
its own.

## The report

firehoooo, 2026-09-12, mod 1.1.1: `E_ABORT` from `sl.interposer.dll` on the **game's own** `Present`,
about 19 seconds after the title screen, 3 crashes out of 3. The same build with ReShade's proxy
moved aside is 3 healthy out of 3. Evidence, which must not be re-derived:
`_done/bugreport1-device-removed/context/repro-truth-table.md`.

## Why the crash is expected here

The stack, unwound 2026-09-19, is `overlay::ovl::render` → ReShade's proxy `dxgi.dll`, which has
adopted the mod's own swapchain as a second runtime → six frames of RTSS → a jump into RTSS's own
heap region. Streamline is only the caller below us; its `__except` writes a minidump and returns
`E_ABORT`, which is the symptom the whole thing was named after. ReShade adopts at the factory, so
there was nothing the mod could create that it would not take.

`4b97357` removed the second swapchain — the overlay reaches the screen through a DirectComposition
surface over D3D11On12 and presents nothing — and that shipped in 1.3.0. So this profile is a
**regression cell**: pinned to 1.1.1 it must crash, and the day it stops crashing, either the
configuration is no longer being reproduced or something else has changed. `expect.verdict` is
`CRASH` for exactly that reason.

## Reproduced on this box, 2026-09-20

`run reshade-renodx -Cells 3 -Hold 90`, RTSS 7.3.5 running: **3 crashes out of 3**, at ~19, ~19 and
~21 seconds, each decided on a window titled `The UE-Project_Plague Game  has crashed and will
close`. That is firehoooo's timing to the second, on a different RTSS build than his — so the bug is
not specific to 7.3.7. Run `20260920-113919Z-reshade-renodx-crash`.

The crash leaves no line of its own: the process dies mid-sentence under the modal box, and the mod
log's last entry is the ordinary no-pawn diagnosis at the title screen. The signature is the window
title, which the runner records as the cell's `why`.

## What is measured and what is not

- **Measured:** the payloads and their hashes (they are this box's own files), HAGS, the game's
  settings, the crash signature and its timing, and that 1.1.1 is the build that carries it.
- **Measured since:** the log. 1.1.1 writes `wuchang_minimap.log` beside the DLL rather than to
  `%LOCALAPPDATA%`, and the runner marks and reads both locations — all three cells read 101 lines
  out of the mod folder. `expect.log_lines` quotes three of them: `WuchangMinimap v1.1.1` (the build
  naming itself, not an md5), `hooks: by dummy-swapchain discovery` (the mechanism ReShade adopts,
  gone in 1.3.0) and `streamline: NVIDIA frame generation (DLSS-G) is LOADED`.
- **Not reproduced at all:** firehoooo's GPU, driver and panel. This profile reproduces the
  *software* configuration.

## RTSS is the one thing a script cannot do

RTSS runs elevated and is closed from its own tray icon, so `repro.ps1` checks that
`RTSS` is in the process list and refuses the cell if it is not. Start it by hand before the run;
`-Force` proceeds past the check, and a run taken that way is not this configuration.

RTSS injects `RTSSHooks64.dll`, which is why `expect.modules` names it: "RTSS was running on the
desktop" and "RTSS was in the game process" are different facts, and only the second one matters.
