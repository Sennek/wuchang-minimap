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
the 2026-09-17 measurement was published with. `tests\tree_test.ps1` proves the tree payload, the
payload no-op and `verdict: any` by driving a real apply / restore against a **sandbox** — a fake
game root, store and profiles dir, built from invented files — so the round trip is proven before it
is trusted with the install. The no-op is proven by mtime: the destination is given a timestamp
nothing else on the box has, and `Copy-Item` would carry the source's over it. It needs `WUCHANG_REPRO_PROFILES_DIR` beside the two state overrides, because `profiles\`
is committed and a test must not write one into the repo. Run both after any change to the readers,
the payload loop or the snapshot.

A missing payload is reported by name and hash so it can be re-downloaded. A profile is a recipe,
not an archive.

## What a profile declares

| Field | Meaning |
|---|---|
| `payloads` | files and trees in `Binaries\Win64`: a payload name + sha256 to place, or `null` to state it absent. `"tree": true` makes `dest` a **directory** — see below |
| `mod.build` | `keep`, or `dist:x.y.z` to **replace** the mod directory with that release — see below. An unreleased build is not declarable: deploy it and let `keep` take it, and the run manifest's `mod.main_dll_md5` is what names the cell afterwards |
| `mod.state` | `on`, `hooks-off`, `off` (at start-up) or `absent` (`start_mod` is never called) |
| `mod.config` / `mod.config_dev` | keys to set in the installed configs |
| `recon` | `on` or `absent` — the Lua recon mod |
| `state` | `keep` or `fresh` for `%LOCALAPPDATA%\WuchangMinimap` |
| `game_settings` | keys in `GameUserSettings.ini` |
| `game_config_expect` | `{ user, gameset }` — the in-game graphics selector, **read** out of `GameConfig.sav` and asserted. Never written: that blob carries the owner's playtime, equipment and achievements beside his graphics settings, so a profile that pinned it would roll his progress back on every apply. An apply names the key and refuses when the game's menu disagrees |
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

`verdict` is `HEALTHY` (the default when it is absent), `CRASH` for a profile that reproduces a bug,
or **`any`** — record the verdict and assert nothing about it. `any` is not a weaker `CRASH`: it is
for a cell whose every answer is information, such as a build old enough to carry a bug that has
never been reproduced on this box. Reporting FAIL for a result that is not a failure of the box is
how a verifier stops being read. A profile using it must still assert something else; an `expect`
that asserts nothing is refused.

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

## A payload that is a directory

Some injectors are not one file. OptiScaler puts nine libraries under
`Binaries\Win64\OptiScaler\` beside its proxy, and a profile that listed them one by one could
install them but could never state the **folder** absent — every other profile would have to name
all nine by hand, and a file nobody listed would survive into a cell claiming to carry no OptiScaler.
So `"tree": true` makes `dest` a directory:

```json
{ "dest": "OptiScaler", "tree": true, "source": "optiscaler-0.9.4",
  "sha256": { "libxess.dll": "…", "nested\\lib.dll": "…" } }
{ "dest": "OptiScaler", "tree": true, "source": null }
```

A tree's `source` is a **folder in the store** and its `sha256` is a map of relative path → hash,
one entry per file: a single hash over a directory would name a traversal order rather than the
files, and could not say which one drifted. A file in the store the map does not name, or a name the
store does not hold, is refused.

Installing a tree **replaces** the directory, exactly as a pinned `mod.build` replaces the mod
directory: a file standing there that the source does not carry is not part of this configuration,
so it is snapshotted and removed rather than left to load beside ours. Stating a tree `null`
snapshots every file under it — without the profile listing one — and takes the whole thing.

The snapshot format needs nothing new for this: a tree is a set of files, and "this file was not
here before" already means "delete it on the way back". What a tree does add is the **directories
the apply had to create**, which the files alone would leave standing empty — a residue the
fingerprint never sees, because it counts files. They are recorded in `snapshot.json` as `dirs` and
removed on restore, deepest first, and only when empty: a folder holding anything at all is holding
something the restore did not put there.

## Processes, and who put them there

A configuration is not only files. `reshade-renodx` and both OptiScaler profiles need **RTSS
running**, and the box has to go back to how it was afterwards — which is the same contract the
files already have, so processes follow it:

> The script touches only what it changed. It starts what a profile requires and is **not already
> up**, records that in the snapshot, and on restore stops exactly those. A process the owner
> already had running is neither started nor stopped, at either end.

`processes.running` therefore no longer refuses when something is down — the apply starts it.
What still refuses is being unable to: `profiles/pinned.json` says where each one lives, under
`processes.exe`, as a path or `{ "path": …, "args": [ … ] }`, and `processes.children` names the
helpers that come up with it (RTSS brings `RTSSHooksLoader64` and `EncoderServer`, and leaving those
behind is the same residue as leaving RTSS itself). `processes.absent` is unchanged and still
refuses: stopping something the owner is using is not this script's call.

**Stopping is the asymmetric half.** `RTSS.exe` is `requireAdministrator`, so an unelevated shell
starts it — it elevates itself — and then `Stop-Process` is *Access denied* across the integrity
boundary and there is no main window to close. Three ways down, cheapest first:

1. `processes.stop_task` in `pinned.json` names a Scheduled Task the owner may register **once**,
   with RunLevel Highest, that stops those processes by name. `Start-ScheduledTask` fires it
   without elevating *us* at all, so it can never put a prompt in the middle of a run. That is the
   only reason it is tried first; it is optional.
2. Otherwise, elevate exactly one fixed system binary with fixed arguments —
   `taskkill.exe /F /IM <name>.exe`. Not a shell: an elevated shell is a general capability asked
   for a specific job, and it reads that way to anything watching the machine. On a box whose UAC
   is set to consent this raises a prompt, which is the box saying so rather than a fault.
3. If both fail — a declined prompt, a locked-down policy — the restore says what it left running
   and writes `left_running.json` into the store. **Every `status` reprints it** until those
   processes are actually gone, and clears it itself when they are. Nobody has to remember that
   something was switched on an hour ago.

## The probes

A probe is what a cell collects and how it is decided — one runner, and the difference is a
parameter. Both launch the game, resolve it by name, read the log rotation-aware and decide every
cell by `rules/in-game-verification.md` first.

| `-Probe` | What it adds | What it needs |
|---|---|---|
| `crash` (default) | nothing — the criteria, the module list, the mod's own lines | nothing |
| `present` | PresentMon on the game's own swapchain, and the mod's frame census | the right to open an ETW session, `-Hold 60`+ or `-Until exit` |
| `gate` | the same capture, with the mod's GATE census instead: the shipped `overlay_update_hz` alternating between off and `-GateHz`, which is the ceiling's A/B inside one capture | the same |

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

**A held cell never leaves the title screen.** `-Until hold` launches and waits; the game stops at
"Press any button" and stays there — measured to 200 s, captured. So a held cell measures the title
screen, never the main menu, and anything that only happens in the menu is invisible to it. The
`present` probe brings the game's window to the front before capturing, which is a precondition it
can meet on its own; the button is not.

**The game's window must be in front, and the cell says whether it was.** A `PresentMode` is a fact
about a window DWM is compositing: an occluded one gets no independent flip, so a capture taken
behind another window measures the occlusion and nothing about the install. Every cell samples the
foreground window once a second beside its liveness sample; a present cell prints the share and,
below 95 %, marks the reading **VOID** — in the cell, in the manifest's `foreground` block and again
in `verify`. It does not touch the verdict: the launch was still healthy or it was not. The one
cell this caught read `Composed: Flip` 97.4 % with no mod in the process at all.

**The census** is the mod's own instrument, and it is the reason a measurement is one launch rather
than five: `dev_frame_cycle_ms` rotates the overlay through its five layers every couple of seconds
while the Present hook sorts the game's own present interval into a histogram per layer, so scene
drift and the mod's warm-up land on all of them equally. The probe writes that key, `log_level` and
`dev_frame_stop` into the dev config **inside the apply's own snapshot**, and only where
`mod.state` is `on` — a state in which nothing of ours runs has no census to switch on. While it
cycles, the overlay freezes and disappears every couple of seconds: that is the measurement. Pass
`-CycleMs 0` to play with the overlay as shipped and leave the layers unpriced.

**The gate census** is the other instrument on the same stream of presents, which is why the mod
runs one or the other and never both: `dev_gate_cycle_ms` rotates three arms — draw every present,
a wall-clock ceiling, one present in N — and the hook sorts the interval into a histogram per arm,
counting what each one skipped. It answers whether drawing less often ever pays, which two runs in
2026-09-18 answered *no* at a skipped fraction of about a quarter and nowhere else: the saving
scales with that fraction while the penalty measured as a step, so break-even sits near 42 %
skipped at the +0.950 ms a frame costs here under frame generation. The third arm is the point of
the shape — a wall clock against a game at an unrelated rate skips IRREGULARLY, one present in N
does not, and an irregular cadence is the only surviving explanation for a middle that costs more
than either end. While it cycles the overlay does not vanish; it visibly draws at three different
rates in turn.

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
- **A write that changes no byte is not made — but a payload is recorded anyway.** A config whose
  keys already hold the declared values is neither rewritten nor recorded: the mod's 1 Hz watch
  hashes the configs' timestamps rather than reading them, so touching one *is* the change. A
  payload already holding the declared bytes is not copied either — `apply box` would otherwise
  rewrite 166 MB into the game's own install and the restore another 166 MB back — but its
  snapshot entry is written all the same, because a third party can mutate the file afterwards
  (ReShade rewrites its own ini while the game runs) and that entry is the only thing that makes
  the way back possible. Copy conditionally, record always. Equal hashes are the same proof the
  copy would have returned, taken before the write instead of after it; the snapshot's own copies
  and the vault's are unconditional, since a backup that declines to write itself is not a backup.
- **A pinned build replaces the mod directory, it does not copy over it.** `mod.build: dist:x.y.z`
  installs that release's files and removes the ones it does not carry, including
  `config_wuchang_minimap_dev.txt`, which no release ships and no reporter's box has. Every removal
  is snapshotted, so the way back leaves no trace. Runtime output is left alone: `navmesh\` in-game
  dumps, play-state `wuchang_minimap*.txt` beside the DLL, and logs. The dumps cannot be re-taken
  without another capture run, and the cheapest way not to lose irreplaceable evidence is not to
  delete it. A build that has no release to name is measured the other way round: `deploy.ps1` puts
  it in, `mod.build: keep` leaves it there, and the manifest records its md5 — enough to identify a
  cell after the fact, not enough to re-create one, so a candidate worth guarding needs a release.
- **The cell is a launch, not an applied profile.** A state reached by toggling is not the state
  reached by launching into it: the hooks are installed at start-up, the start-up block that names
  the device, the swapchain and the composition targets prints once per process, and a window
  demoted to `Composed: Flip` by our target was still composed 25 s and 90 s after `mod_enabled = 0`
  released it. `apply` and `run` refuse while the game is running, and `-Force` does not cover that.
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
