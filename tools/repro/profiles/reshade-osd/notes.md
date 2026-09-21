# `reshade-osd` — does a third party writing this back buffer keep the hardware plane?

`box` with neither C++ mod started and ReShade drawing its on-screen display. It exists for one
question in `present-hook-cost`: candidate 3, "be where ReShade is", rests entirely on ReShade
holding `Hardware: Independent Flip` **while it actually writes the game's back buffer**. Every cell
that ever read ReShade in independent flip had no effect switched on, so it was reading a runtime
that draws nothing.

## Why the OSD and not an effect

This install has no shaders: `EffectSearchPaths` is empty and there is no `reshade-shaders` folder.
`ShowFPS`, `ShowClock` and `ShowFrameTime` go through the same runtime and the same write, minus the
shader pass, and they need nothing installed. `box-ReShade-osd.ini` in the payload store is
`box-ReShade.ini` with those three on.

## The control is `box -Mod absent`, taken back to back

The two differ in one field. Run them in the same session, never against a table from another day:

```powershell
& .\repro.ps1 run reshade-osd -Probe present -Until hold -Hold 90 -Cells 1
& .\repro.ps1 run box -Mod absent -Probe present -Until hold -Hold 90 -Cells 1
```

## What the cell needs, and what voids it

`PresentMode` is a fact about a window DWM is compositing, so the cell is void unless the game's
window is **foreground and unoccluded for the whole capture**. The first attempt, 2026-09-21, read
`Composed: Flip` 97.4 % with no mod in the process and meant nothing: another application was
full-screen on the primary display, three monitors were connected and Discord was running. Take it
on a box nobody is touching.

## The half it cannot reach

NVIDIA's claim is about DLSS-G being **active**. Frame generation lives in `GameConfig.sav`, which a
profile reads and never writes, so this cell answers the plane question with frame generation as the
menu leaves it. The other half needs the owner in the graphics menu once.
