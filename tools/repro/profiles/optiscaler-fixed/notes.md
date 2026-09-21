# `optiscaler-fixed` — the same box, with the bug fixed

Byte for byte the configuration of [`optiscaler`](../optiscaler/notes.md). Two fields differ:
`mod.build` is `keep` rather than `dist:1.2.0`, and `expect` wants a HEALTHY launch that logs
`hooks: from the engine's own swapchain`.

## Why a second profile rather than a flag

A profile is a configuration **and** what a launch into it must show. `optiscaler` says "this box
crashes the 1.2.0 build"; this one says "this box runs the current build". Both are true, both are
worth being able to fail, and a single profile with a switch would have to decide which of the two
it was at run time — the thing `repro.ps1` exists to stop.

It is also the reason the configuration is worth carrying at all now that `71963f6` has shipped.
`optiscaler` reproduces a bug that is fixed; without this profile the whole folder would be a museum
piece. With it, the day the fix regresses, one command says so.

## What it asserts, and why not just "it did not crash"

A crash that stops happening is not a fix — a timing window can close for any number of reasons, and
this box has its own `GPUCrash` history from the DLSS stack that has nothing to do with the mod. So
the check is the **mechanism**, in the mod's own words:

```
hooks: from the engine's own swapchain | Present 0 @ … | ResizeBuffers 0 @ … | Present1 0 @ …
```

That line is only printed when `hookfind.cpp` has crossed `GameViewportClient` → `FViewport` →
`FD3D12Viewport` → `IDXGISwapChain` on the game thread and read the three addresses off the
swapchain the **engine** owns. If a future build ever creates one of its own again, the line changes
and this profile fails before anything has to crash to prove it.

`WATCHDOG` must be absent, and the verdict HEALTHY.

## What it does NOT assert

The present mode. OptiScaler's own frame generation is on in this configuration, and the mode is a
property of what the player is doing — see `tools/repro/README.md`. A cell of this profile records
its mode and asserts nothing about it.
