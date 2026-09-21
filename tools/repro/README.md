# repro — putting this box into someone else's configuration

A bug report names a machine. `repro.ps1` turns that machine into a **profile**, makes this box that
configuration, launches the game into it, decides each launch by
[`.claude/rules/in-game-verification.md`](../../.claude/rules/in-game-verification.md)'s criteria,
and puts the install back byte for byte.

```powershell
.\repro.ps1 status                          # what is applied, what the box carries
.\repro.ps1 run box -Cells 3 -Hold 90       # apply, three launches, restore, judge
.\repro.ps1 run box -Until exit             # apply, launch, hand the session over, restore
.\repro.ps1 run box -Probe present -Until exit -Cells 1   # the same, with both frame instruments
.\repro.ps1 verify                          # re-judge the newest run against its profile
.\repro.ps1 restore                         # take off whatever is applied
```

Every verb takes `-DryRun`, which validates and prints every write without performing one.

## The two halves

The **repo** holds the recipes and is only ever read: `profiles/<name>/profile.json` declares a
configuration, `profiles/<name>/notes.md` says which report it came from and which parts are guessed
rather than measured, `profiles/pinned.json` carries the UE4SS build this mod is pinned to.

The **store** — `F:\Tools\wuchang-repro\`, behind `WUCHANG_REPRO_STORE` — holds everything else,
because ReShade, RenoDX and OptiScaler are tens of megabytes of third-party binary and the repo has
no LFS:

```
payloads/     the injector binaries a profile references by name + sha256
vault/        the owner's play state, copied before anything else happens
snapshots/    the exact bytes each apply replaced, with hashes
runs/         one folder per run: manifest.json + cell-N.log (+ cell-N.csv under -Probe present)
tests/        probe_test.ps1 and the captured input it reads: the offline proof of the readers
current.json  what is applied right now
```

`tests\probe_test.ps1` defines this script's functions without running its dispatch and puts the
present probe's readers over a real two-swapchain capture and a real log, reproducing the numbers
the 2026-09-17 measurement was published with. Run it after any change to them.

A missing payload is reported by name and hash so it can be re-downloaded. A profile is a recipe,
not an archive.

## What a profile declares

| Field | Meaning |
|---|---|
| `payloads` | files in `Binaries\Win64`: a payload name + sha256 to place, or `null` to state it absent |
| `mod.build` | `keep`, or `dist:x.y.z` to **replace** the mod directory with that release — see below |
| `mod.state` | `on`, `hooks-off`, `off` (at start-up) or `absent` (`start_mod` is never called) |
| `mod.config` / `mod.config_dev` | keys to set in the installed configs |
| `recon` | `on` or `absent` — the Lua recon mod |
| `state` | `keep` or `fresh` for `%LOCALAPPDATA%\WuchangMinimap` |
| `game_settings` | keys in `GameUserSettings.ini` |
| `game_config_sav` | `{ user, source }` — the in-game graphics selector, as a captured payload |
| `processes` | what must be running and what must not, read back before anything moves |
| `expect` | what the launch must then show — see below |
| `manual` | what a script cannot set: driver vsync, monitor topology. `hags` is read back from the registry |

`expect` is what a run is judged against:

```json
"expect": {
  "modules":          ["Binaries\\Win64\\dxgi.dll", "WuchangMinimap\\dlls\\main.dll"],
  "modules_absent":   ["Binaries\\Win64\\d3d12.dll", "RTSSHooks64.dll"],
  "log_lines":        ["composition: the overlay draws into"],
  "log_lines_absent": ["WATCHDOG"],
  "verdict":          "HEALTHY"
}
```

Modules are matched on the **end of the loaded module's full path**, never on its name: Windows
loads its own `dxgi.dll` and `dwmapi.dll` from System32 into every process, so a name check for
either passes in a configuration carrying no injector at all. A misspelt field and an `expect` that
asserts nothing are both refused.

**The present mode is not among them.** It is recorded — per stream, with its counts — and never
asserted. A played cell showed the game's window in `Composed: Flip` for three minutes of menu and
then `Hardware: Independent Flip` on **100 % of 6049 presents** once gameplay began, with the
overlay's own composition surface presenting throughout. The mode is a property of what the player
is doing; a profile describes an install. A per-profile mode would have asserted whichever condition
a cell happened to sit in, and every held cell sits in the menu.

Reading the recorded mode, the label is exact — `Hardware: Independent Flip`,
`Hardware Composed: Independent Flip` and `Composed: Flip` are three different states, and reading
only the first of the two hardware labels scores a win as a loss.

## The probes

A probe is what a cell collects and how it is decided — one runner, and the difference is a
parameter. Both launch the game, resolve it by name, read the log rotation-aware and decide every
cell by `rules/in-game-verification.md` first.

| `-Probe` | What it adds | What it needs |
|---|---|---|
| `crash` (default) | nothing — the criteria, the module list, the mod's own lines | nothing |
| `present` | PresentMon on the game's own swapchain, and the mod's frame census | the right to open an ETW session, `-Hold 60`+ or `-Until exit` |

**PresentMon** is started once the game's pid is known and stops twice over — when the game exits,
and on its own clock if the game never does — so nothing here ever kills it to get the CSV flushed.
Its capture is **split by `SwapChainAddress` before a number is taken off it**: PresentMon records
every swapchain in the process, and a build that presents one of its own turns a pooled median into
a number describing no frame anyone saw. The game's stream is the busiest one carrying a real
address — the overlay presents no swapchain, so its composition surface is the null-address stream.
The mod's `hook discovery:` line is a **cross-check, not a gate**: it logs the swapchain object the
mod hooked, which on a box carrying `sl.interposer.dll` — every box here, it ships with the game —
is the wrapper in front of the runtime's swapchain, while ETW reports the runtime's own. The
manifest records both addresses and which case the cell is.

**The census** is the mod's own instrument, and it is the reason a measurement is one launch rather
than five: `dev_frame_cycle_ms` rotates the overlay through its five layers every couple of seconds
while the Present hook sorts the game's own present interval into a histogram per layer, so scene
drift and the mod's warm-up land on all of them equally. The probe writes that key, `log_level` and
`dev_frame_stop` into the dev config **inside the apply's own snapshot**, and only where
`mod.state` is `on` — a state in which nothing of ours runs has no census to switch on. While it
cycles, the overlay freezes and disappears every couple of seconds: that is the measurement. Pass
`-CycleMs 0` to play with the overlay as shipped and leave the layers unpriced.

**The ETW right is stated, not fought.** PresentMon's session is opened by an administrator or by a
member of Performance Log Users (`S-1-5-32-559`) — the group exists so a capture needs no elevation,
and this account is in it, so an ordinary shell captures. The right is read off the *token*, which a
membership granted since the last logon is not yet in. The probe says which grant it has before it
applies anything, rather than self-elevating in the middle of a measurement or returning a cell with
no numbers. `-Force` runs the cells with the census alone, and every cell then records *why* it
carries no capture.

## The rules it is built on

- **Nothing is destroyed.** An apply writes its undo record — the snapshot entries and
  `current.json` — *before* the first mutation and marks it complete only at the end, so an
  interrupted apply still names every file it had replaced. A restore that cannot prove itself exact
  is a failure, not a warning, and it puts the old `LastWriteTimeUtc` back as well as the bytes.
- **The owner's play state is never collateral.** Every verb vaults the save, `GameConfig.sav`, the
  game's ini files and `%LOCALAPPDATA%\WuchangMinimap` first, hashes every copy against what it
  wrote, and keeps the lot. A save is written back only by `repro.ps1 saves-restore <stamp>`, typed
  deliberately. There is one slot on this box and no cloud sync.
- **A write that changes no byte is still a change.** A config whose keys already hold the declared
  values is not rewritten and not recorded — the mod's 1 Hz watch hashes the configs' timestamps
  rather than reading them.
- **A pinned build replaces the mod directory, it does not copy over it.** `mod.build: dist:x.y.z`
  installs that release's files and removes the ones it does not carry, including
  `config_wuchang_minimap_dev.txt`, which no release ships and no reporter's box has. Every removal
  is snapshotted, so the way back leaves no trace. Runtime output is left alone: `navmesh\` in-game
  dumps, play-state `wuchang_minimap*.txt` beside the DLL, and logs. The dumps cannot be re-taken
  without another capture run, and the cheapest way not to lose irreplaceable evidence is not to
  delete it.
- **The cell is a launch, not an applied profile.** Once the overlay's composition target has
  existed in a process the window never returns to Hardware: Independent Flip, so a state reached by
  toggling is not the state reached by launching into it. `apply` and `run` refuse while the game is
  running, and `-Force` does not cover that.
- **The game is not the process you start.** Steam hands the launch on: the first process of that
  name is a stub that exits within seconds. The game is resolved *once it has settled* — the newest
  process of that name still alive — and a tracked process that exits while another is alive is a
  hand-off, not a crash.
- **`WM_CLOSE`, never `Stop-Process`.** Killing the game skips the OFF / ON / exit paths, which is
  where the bugs are. The one kill runs after a cell has already been decided CRASH, because a
  `LowLevelFatalError` modal cannot be closed any other way — and a killed game does not give back
  an exclusive audio endpoint, so several such cells can leave the audio engine clicking with no
  game running. That is a testing artifact; restart the audio device.
- **The mod's log is the player's log.** It is never cleared. A mark — length plus the hash of the
  first 4 KB — is taken before the launch and the new lines are read back against it, so a rotation
  is detected rather than lost. It is UTF-8.
- **It never touches the driver, the display, the registry or the repo working tree.**

## Verify, and why it is separate

`run` collects evidence; `verify` judges it, from the profile as it stands **now**. A manifest
written three weeks ago can therefore be re-judged against a profile that has since been corrected,
and `verify` says so when the profile has moved. A run that did not finish is refused by cell count
rather than judged short.

## Adding a profile

1. `profiles/<name>/profile.json`, and `notes.md` naming the report, the evidence and what is
   guessed.
2. Put any binaries it needs in `<store>\payloads\` and declare their sha256.
3. `.\repro.ps1 apply <name> -DryRun` — every unknown field, missing payload and precondition that
   reads back wrong is named before a byte moves.
4. `.\repro.ps1 run <name>` and read the verdict.
