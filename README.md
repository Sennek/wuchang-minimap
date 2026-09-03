# WuchangMinimap

An interactive minimap, full map, compass and collection tracker for **Wuchang: Fallen
Feathers** (Unreal Engine 5.1.1, Windows x64, DX12). The minimap is height-sliced, so only
the storey you are standing on is drawn solid; `M` opens a full pannable, zoomable map of
the chapter with the same slice on a floor stepper. Markers cover shrines, chests,
pickups, bosses, elites, NPCs, readable notes, doors, ladders, lifts, fog gates and hidden
things, and they carry the game's own English names — "Mercury Workshop", "He Youzai",
"Sovereign - Zhang Xianzhong" — not internal ids. A collection tracker remembers what you
have opened, taken, lit, met and beaten (one file per save game, with a statistics page
per category and per chapter), a compass strip puts bearing pips with distances in metres
over the top of the screen, and an x-ray key draws nearby markers through walls with their
names. The map background is not hand-drawn: it is built offline from the game's own
navigation mesh, so what you see is walkable ground. UI language is English only.
Keyboard, mouse and gamepad are all supported, and every hotkey can be rebound in-game.

The mod reads the game's memory and its packaged data. It **never modifies game files or
save data**, so uninstalling it cannot affect a playthrough.

---

## Requirements

* **Wuchang: Fallen Feathers** on Windows x64.
* **UE4SS built for this game** — *UE4SS for Wuchang: Fallen Feathers*, Nexus Mods mod
  **384** for Wuchang. Install it into
  `<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\`.
  UE4SS is installed once that folder holds `dwmapi.dll` and a `ue4ss\` subfolder.

### Mandatory: `HookInitGameState = 0`

Open `Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini`, find the `[Hooks]` section
and set:

```ini
[Hooks]
HookInitGameState = 0
```

**Without this the game crashes about a third of a second into loading**, with or without
this mod: UE4SS's hook on `AGameModeBase::InitGameState` breaks Wuchang's custom game mode
and a blueprint then reads a null game state. Every other hook can stay on. This is a
UE4SS-plus-Wuchang problem, not a bug in this mod.

## Install

1. Close the game.
2. The download contains a single `ue4ss\` folder. Copy it into
   `<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\` and
   **merge** when Windows asks. Nothing of UE4SS's own is overwritten — everything in the
   package lives under `ue4ss\Mods\WuchangMinimap\`.
3. Check `[Hooks] HookInitGameState = 0`.
4. Launch the game and load a save. The overlay is deliberately hidden on the main menu.

There is no `mods.txt` to edit: the empty `enabled.txt` in the mod folder is what tells
UE4SS to load it. Delete that file to stop UE4SS loading the mod at all.

## Hotkeys

Shipped defaults. **All of them are rebindable** in the `F2` panel's **Bindings** tab —
click a row, press the new key, `Esc` cancels.

| Key | Action |
|---|---|
| `F2` | Settings panel (takes the mouse while it is open; `Esc` closes it) |
| `M` | Full map |
| `LALT` | X-ray highlight — **a toggle**: press to turn on, press again to turn off |
| `LB`+`RB` | The same on a controller (the pad chord is editable too) |
| `N` | Cycle the minimap zoom (13 / 26 / 52 uu per pixel) |
| `R` | Recentre the full map on the player |
| `F5` | Reload the config, the map assets and the marker database |

`highlight_mode = hold` (or **Mode: Toggle / Hold** in the panel) restores the old
hold-the-key behaviour. A toggled-on highlight always turns itself off on a level change.

While the full map is open:

| Input | Action |
|---|---|
| Drag, `WASD`, arrows | Pan |
| Wheel, `+` / `-` | Zoom |
| `Ctrl`+wheel, `Q` / `E`, `PgUp` / `PgDn` | Step the height slice down / up a floor |
| `Home` | Fit the whole chapter |
| `R` | Recentre on the player, clear the floor offset |
| Left-click a marker, or `F` | Toggle "found" by hand |
| Right-click, or `Space` | Set the waypoint (it also shows on the minimap and compass) |
| `C` | Copy the map to the clipboard. Nothing is written to disk. |
| **Stats** / **Shrines** buttons | Collection statistics · the chapter's named shrines by distance |
| `?` | Show the controls in-game |
| `M` or `Esc` | Close |
| Gamepad | Left stick pan · triggers or right stick zoom · `LB`/`RB` floor · `A` waypoint · `X` found · `Y` recentre · `Back` help · `B` close |

`F6` and `F9`–`F12` are refused: they belong to RenoDX/DLSS, the engine, the game console
and Steam. Everything else is fair game — `F1`–`F5`, `F7`, `F8`, letters, digits, `Tab`,
`Space`, `Enter`, the arrows, `Insert`/`Delete`/`Home`/`End`/`PgUp`/`PgDn`, the numpad,
`MOUSE3`–`MOUSE5`, the modifier keys, and `none` to leave an action unbound.

## Configuration

Everything is one plain-text file,
`ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`. It is `key = value` lines with
`;` or `#` comments, and it documents every key inline. Edit it and press `F5` in-game —
no restart.

The `F2` panel edits the same settings and has three tabs: **Player** (everything you
would plausibly change, in ten collapsible sections, plus three one-click presets —
Minimal HUD, Loot hunting, Exploration), **Advanced** (the tuning dials) and **Bindings**
(every hotkey and the gamepad chord). **Save** writes the file, **Revert** re-reads it and
throws away what you were fiddling with.

**Save never rewrites your file.** It replaces the values on the existing `key = value`
lines and leaves every comment, blank line, your ordering and any key it does not
recognise exactly where they were. Installing an update does not overwrite an edited
config either — the packaged config is only laid down where none exists yet.

`mod_enabled = 0` makes the mod completely inert (no graphics hook, no scanning, no map in
memory) without uninstalling it; setting it back to `1` and saving starts it again within
a second, with no game restart. `F2` → **Disable for this session** does the same for one
session without touching the file.

The second shipped file, `config.ini`, drives a developer-only runtime navmesh dumper and
is off by default. It has nothing to do with the maps you see.

## Your files

Written by the mod next to the config, in `ue4ss\Mods\WuchangMinimap\`. No install or
update overwrites them.

| File | What it is |
|---|---|
| `wuchang_minimap_found_<slot>.txt` | The collection tracker, **one file per save game**: one marker id per line. Plain text, hand-editable; delete it to reset your tracking. The F2 Player tab shows which file is in force. |
| `wuchang_minimap_found.txt` | The shared tracker, used when the save slot cannot be worked out or with `found_profile = shared`. A new slot's file is seeded from this one, so nothing is lost. |
| `wuchang_minimap_waypoint.txt` | Your waypoint, so it survives a restart. Three `key = value` lines. |
| `wuchang_minimap_firstrun.txt` | Records that the first-run tip has been shown. Delete it to see the tip again. |
| `wuchang_minimap.log` (+ `.1` … `.3`) | The mod's own log, rotated per launch. |
| `wuchang_minimap_watchdog.txt` | Written only if the game stops responding for six seconds. |
| `wuchang_minimap_last_stage.txt` | One line naming the stage the overlay was in. |
| `wuchang_minimap_hookaddr.txt` | The cached graphics hook addresses. Deleting it is always safe. |

## Known limitations

* **The DLC has no map.** The game ships no navigation data for it. Markers still work,
  but the DLC's 7 shrines keep their internal ids instead of names — the DLC's fire points
  have no row in the game's fire-point table.
* **A boss read from your save may not be one you beat.** For bosses killed before the mod
  was installed there is nothing left in the world to read, so the mod uses the boss
  arena's respawn point, which your save remembers unlocking. It is not certain the game
  unlocks that point on the kill rather than on the first attempt, so a boss you fought
  and lost to may read as defeated. It is recomputed every launch and never written to the
  tracker: `boss_defeat_from_save = 0` undoes it completely.
* **A few dozen detached patches of walkable ground per chapter are drawn although you
  cannot walk to them** — the navmesh has no notion of "reachable", and the filter keeps
  any patch that is large enough or carries a marker. The palace lake area in Chapter 1 is
  the most visible case.
* **The overlay draws underneath ReShade's effects**, so a heavy preset tints it. That is
  cosmetic.
* **Enemy markers are off by default.** The live sweep refreshes about once a second, so
  they lag behind anything that moves.

## Troubleshooting and bug reports

* **The game crashes on start-up** — almost always `HookInitGameState`. See
  [Requirements](#mandatory-hookinitgamestate--0). To confirm UE4SS is the cause at all,
  rename `dwmapi.dll`: if the game still crashes, it is neither UE4SS nor this mod.
* **Nothing shows on the main menu** — by design. The overlay is hidden on the menu, in
  cutscenes, in menus and for a moment after a load. Load a save first.
* **Nothing shows in the world** — press `F2`. If the panel opens, the mod is running and
  only the minimap is suppressed; the top of the Player tab prints one orange
  `hidden because:` line naming the exact condition. Screenshot that line. If `F2` does
  nothing either, check `enabled.txt` exists.
* **A hotkey does nothing** — check the Bindings tab, which prints what is actually in
  force, and that it is not one of the refused keys above.

**Reporting a bug**: attach these, from `ue4ss\Mods\WuchangMinimap\`.

* `wuchang_minimap.log` — the mod's own log, and `wuchang_minimap.log.1` if the problem
  was in the previous session. This is the important one: UE4SS empties its own log every
  launch, so restarting the game to try something else destroys it, and this file survives.
* `wuchang_minimap_watchdog.txt` — if the game froze. One line naming the stalled thread
  and what it was last doing.
* `wuchang_minimap_last_stage.txt` — if the game crashed. One line naming the stage the
  overlay was in; it survives when a log does not.
* For a crash, also `%LOCALAPPDATA%\Project_Plague\Saved\Crashes\` —
  `CrashContext.runtime-xml`, whose `<CrashType>` says whether it was a `GPUCrash` (a
  driver / ReShade / DLSS problem, not this mod).
* Your `config_wuchang_minimap.txt` if you have edited it, and the version, which is on
  the first line of the log.

## Uninstall

Close the game and delete
`Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\`. That is the whole mod. It
writes nothing anywhere else, touches no save file and modifies no game data. Deleting the
folder also removes your tracker and waypoint files — copy them out first if you want to
keep them.

To remove UE4SS as well, delete `dwmapi.dll` and the `ue4ss\` folder from
`Project_Plague\Binaries\Win64\`.

## Building from source

See **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** — toolchain, `build.ps1`, the offline
navmesh and marker pipelines that produce the map assets, and the runtime internals.
`tools/package.ps1 -Version x.y.z` builds a release zip.

## Credits and licence

MIT — see [LICENSE](LICENSE). Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):
[Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause),
[fmt](https://github.com/fmtlib/fmt) (MIT) and
[RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (MIT).

The map backgrounds, the marker database and every name shown are extracted from the
game's own data. This mod is not affiliated with the developers or the publisher of
Wuchang: Fallen Feathers.
