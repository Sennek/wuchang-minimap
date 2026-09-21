# `optiscaler` — firehoooo's second box

Not `box` plus something: this is a different injector in a different slot. OptiScaler 0.9.4 loads
as **`Binaries\Win64\d3d12.dll`**, and everything this install carries in the DXGI slot — ReShade's
proxy, the RenoDX addon, DLSSNR — is declared **absent**, because he had none of it. RTSS is
running, as it was in his first report; the mod is pinned to **1.2.0**.

## The report

firehoooo, 2026-09-13, one day after his `E_ABORT` report and in a **different configuration** —
which is why he is two profiles rather than one folder with his name on it. 3 crashes out of 3 at
11–17 seconds, with frame generation on **and** off. His build was identified by PE stamp rather
than by his word: `main.dll` size 4,882,432, stamp `0x6AA56E80` = `dist\WuchangMinimap-1.2.0`.

Evidence, which must not be re-derived:
`_done/discovery-swapchain-optiscaler/context/{truth-table,repro-recipe}.md` and
`context/reporter4-2026-09-13/`.

## Why the crash is expected here

The mod used to create a throwaway swapchain to read the three hook addresses off its vtable.
OptiScaler proxies D3D12 and adopts that swapchain; the crash follows from the adoption, exactly as
ReShade's adoption is what kills the 1.1.1 build. `71963f6` stopped the mod creating anything at all
— `hookfind.cpp` crosses `GameViewportClient` → `FViewport` → `FD3D12Viewport` → `IDXGISwapChain` on
the game thread and reads the addresses off the swapchain the **engine** owns. That fix shipped in
1.3.0, so this profile pins 1.2.0 to keep the bug, and `optiscaler-fixed` is the same box with the
mod as installed.

## What is measured here and what is taken from the report

| | |
|---|---|
| **Measured on this box** | the verdict: 3/3 crash at 11–17 s, both frame-generation states, 2026-09-13/14 |
| **Measured on this box** | the mod build, by PE stamp out of his own log |
| **From the report** | that he ran OptiScaler 0.9.4 and no ReShade |
| **Differs from the report** | RTSS is **7.3.5** here, 7.3.7 in his. The 1.1.1 crash reproduced across that same gap, so it is recorded rather than treated as a blocker |
| **Cannot be set by a script** | RTSS runs elevated and is closed from its own tray icon. The profile checks that it is running and never starts it — an apply refuses until the owner has |

## The payload, and why it is a tree

OptiScaler is ten files, not one: the proxy, plus **nine libraries** in
`Binaries\Win64\OptiScaler\` — 174 MB. The folder is declared as a `tree`, so `stock`, `box` and
every profile after them can state *"no OptiScaler here"* rather than *"none of the nine files
somebody remembered to list"*. The game ships its **own** `amd_fidelityfx_dx12.dll` directly in
`Win64` and it is not touched; the subfolder is what keeps the two apart, and `OptiDllPath` is what
points OptiScaler at it.

`fakenvapi`, `D3D12_Optiscaler\D3D12Core.dll`, the licences and the setup scripts ship in the same
archive and are **not** installed, because the recipe the crash was measured with did not install
them.

The archive itself is in `<store>\sources\`: `Optiscaler_0.9.4-final.20260718._MM.7z`, the official
`github.com/optiscaler/OptiScaler` release `v0.9.4`, 55 016 448 bytes, sha256
`575cb4df866116093df75af607e37fd70e10f5163e0f23fd5c804142e80ef0ad`. The version is part of the
configuration: a newer OptiScaler under this name would be a different box.

`OptiScaler.ini` is the vendor's own file with seven keys set on the lines it already carried for
them — `OptiDllPath=.\OptiScaler\`, `Dx12Upscaler=xess`, `[FrameGen] Enabled=true FGInput=dlssg
FGOutput=fsrfg`, `LogToFile=true`, `LogLevel=2`. It is marked `volatile`: OptiScaler rewrites it
while it runs, so it is snapshotted and restored like any other file but never counted as drift.

## Reading a cell

`Binaries\Win64\OptiScaler.log` is OptiScaler's own, beside the mod's
`%LOCALAPPDATA%\WuchangMinimap\wuchang_minimap.log` and the game's `Saved\Crashes\<newest>`. The
`CrashType` in that folder is read **before** anything is concluded: this box produces `GPUCrash`
dumps of its own from the DLSS stack, and one of those is not this bug.
