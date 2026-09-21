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

## The result, 2026-09-21

**3/3 HEALTHY**, `main.dll` md5 `f8941638439e15fbc6ace6bff0f692e5` = `dist\WuchangMinimap-1.0.0`,
3/3 PASS. The report still does not reproduce on this box — and this is the first time that
statement is backed by a run anyone can repeat with one command, against a configuration recorded in
a file, with the install restored byte for byte afterwards (77 files).

Fourteen manual runs said it before. These three say it reproducibly.
