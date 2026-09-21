# `dlssg-plain-1.0.0` — his configuration on his build

[`dlssg-plain`](../dlssg-plain/notes.md) with one field changed: `mod.build` is `dist:1.0.0`, the
release reporter 1 actually ran. Read that file first — the configuration, the four things this box
cannot be, and why the graphics selection is read rather than written are all there.

## Why it asserts no verdict

1.0.0 is the build that drew into the game's own back buffer. `9ce35ee` stopped that, and his
`DEVICE_REMOVED` is supposed to be a property of the old behaviour — so this cell is the one place
the report could still reproduce on this box.

Every answer is information:

- **CRASH** would reproduce the report for the first time, on the seventeenth attempt.
- **HEALTHY** adds to the fourteen runs that already say it does not reproduce here.

Neither is a failure of the profile, so `expect.verdict` is **`any`**: the cell records its verdict
and asserts nothing about it. What it *does* assert is the install — Streamline and DLSS-G loaded,
every injector gone, UE4SS pinned — because a cell that ran the wrong configuration would tell us
nothing whichever way it fell.

`expect.log_lines` is empty for the same reason: 1.0.0 predates the DirectComposition path and never
printed `composition: the overlay draws into`. Asserting the current build's line against an old
release would fail for a reason that is not a finding.

## The result, 2026-09-21 — **the report reproduces**

**HUNG, every cell.** `main.dll` md5 `f8941638439e15fbc6ace6bff0f692e5` = `dist\WuchangMinimap-1.0.0`.
The mod's own log, identically in all of them:

```
the overlay is releasing its D3D12 objects and will adopt the swapchain again on a later Present:
Present returned DEVICE_REMOVED
game-state reader: no snapshot in the last 10 s (the ProcessEvent pump is not firing)      x9
```

`DEVICE_REMOVED` is reporter 1's exact failure, and the pump lines are his "hangs during startup":
the game thread never runs again, zero `state: pawn` lines, the game sitting on its splash screen
for the whole hold. **Seventeen attempts in, on the first one that was reproducible with a command,
it reproduced.**

### The first three cells said HEALTHY, and that was the verdict's fault

Run `20260921-090642Z` called this 3/3 HEALTHY. Every crash criterion is about the process dying —
no crash window, no `CrashReportClient`, no new `Saved\Crashes` entry, no exit, no `WATCHDOG`
(1.0.0 predates it) — and a hang satisfies all of them. Unreal's RenderThread timeout is 120 s and
the hold was 60, so it never reached the crash that would have come.

The owner watched the window, saw the frozen splash, and asked whether the hang had been detected.
It had not. `HUNG` is the third verdict now, and the hold ends at ~21 s instead of holding a dead
game for sixty — `rules/in-game-verification.md` carries the rule.

Measured while fixing it: `Process.Responding` answered **every** sample during the hang
(`worst_pause_seconds: 0`). Windows thought the window was fine. Only the mod's own game-state
reader saw it.

### What this settles

`9ce35ee` fixes reporter 1's failure, and that is now measured **here** rather than read off his
logs: same box, same configuration, same hour — 1.0.0 hangs with `DEVICE_REMOVED`, the current build
runs clean with the pump firing and 6900 frames in the census. The pair is the evidence.
