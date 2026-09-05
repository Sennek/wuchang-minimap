# WuchangMinimap

A minimap, full map, compass and collection tracker for **Wuchang: Fallen Feathers**,
with the map built from the game's own navmesh.

- A **minimap**, a **compass strip** and a **full chapter map** on `M` to pan and zoom.
- **Markers with the game's own names** for every chapter and the DLC, filtered by
  category — the filters are remembered between sessions on their own — and a **name
  search** on the full map.
- **Up to 16 waypoints** at once, on the map, the minimap and the compass; a key you bind
  drops one on the nearest thing you have not collected.
- **A collection tracker per save slot**, with **export / import** of your found list and
  waypoints as one JSON file.
- **An x-ray key** that draws nearby markers through walls.

## Requirements

**UE4SS for Wuchang: Fallen Feathers** (Nexus mod **384**), installed into
`Project_Plague\Binaries\Win64\`. It must be this exact build:

```
UE4SS v3.0.1-1111-g97b7e501   (the "experimental-latest" asset of Nexus mod 384)
```

The mod links to that DLL's exports, so **another UE4SS build will not work**, and the
failure is silent: the game plays normally, no overlay appears, `F2` does nothing, and
`ue4ss\UE4SS.log` says

```
Failed to load dll <...\Mods\WuchangMinimap\dlls\main.dll> for mod WuchangMinimap,
error: The specified procedure could not be found.
```

instead of `WuchangMinimap v1.0.0 loaded`. `BUILD_INFO.txt` in this download repeats the
version.

Then set this in `ue4ss\UE4SS-settings.ini`:

```ini
[Hooks]
HookInitGameState = 0
```

**Without it the game crashes a third of a second into loading**, with or without this
mod: UE4SS locates `AGameModeBase::InitGameState` by a fixed vtable slot index, and in Wuchang's
engine build that slot holds a different function, so the detour lands on the wrong virtual.
Every other hook can stay on. It is a UE4SS-plus-Wuchang problem, not a bug here.

## Install

1. Close the game.
2. Copy the `ue4ss\` folder from the download into
   `<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\`
   and **merge** when Windows asks. Everything lands under
   `ue4ss\Mods\WuchangMinimap\`, so nothing of UE4SS's own is overwritten. Updating an
   old install: keep your `config_wuchang_minimap.txt` when Windows offers to replace it.
3. Launch the game, load a save (nothing shows on the main menu), press `F2`.

There is no `mods.txt` to edit — the empty `enabled.txt` in the mod folder is the opt-in.

## Hotkeys

| Key | Action |
|---|---|
| `F2` | Settings panel: Overview, Categories, Map & tracker, Keys, Tuning |
| `M` | Full map. Drag or `WASD` to pan, wheel to zoom, `Q`/`E` for the floor, `Home` to fit, right-click a marker to waypoint it or the ground to drop one, `C` to copy the map to the clipboard, `F1` for the rest, `Esc` to close |
| `TAB` | X-ray highlight through walls — a toggle |
| `LB`+`RB` | The same, on a controller |
| `Back`+`RS` | Open / close the settings panel on a controller |
| `Back`+`Y` | Open / close the full map on a controller. The map takes a gamepad throughout: left stick pans, triggers zoom, `LB`/`RB` change floor, `A` drops or removes a waypoint, `B` closes it |
| `N` | Cycle the minimap zoom |
| `R` | Recentre the full map |
| unbound | Set a waypoint on the nearest marker you have not found — bind it in `F2` → **Keys** |
| `F5` | Reload the config, maps and markers |

Everything is rebindable in `F2` → **Keys**: click a row and press the new key, holding
`Ctrl`, `Shift` or `Alt` for a combination. `F6` and `F9`–`F12` are refused — they belong
to RenoDX/DLSS, the engine, the game console and Steam. A key bound to the mod is taken
away from the game while the mod is using it; put a modifier in front to leave the bare
key to the game.

The tab reads **your** game bindings out of the running game, so it names the action a
key would take away and follows a remap you made in the game's own options. Before a save
is loaded it says so and falls back to a built-in list of the usual binds.

## Known conflicts

None of these stops the mod working.

- **ReShade / RenoDX** (any `dxgi.dll` or `d3d12.dll` proxy next to the game exe). The
  overlay is drawn **before** ReShade's effects, so colour-grading, tone-mapping and
  sharpening are applied on top of the minimap and a strong LUT tints it. `F6` is
  refused as a mod hotkey because it is RenoDX's default toggle.
- **Another UE4SS C++ mod that hooks `Present`.** Two overlays on one swapchain is the
  one combination that can lose an overlay: whichever installs second usually wins.
  Test them one at a time before reporting a blank screen.
- **The Steam overlay.** Every launch creates and destroys a throwaway swapchain to find
  the addresses to hook, and Steam's overlay follows it, so its FPS counter can end up
  pointing at nothing. Shift+Tab still works.

## Files it writes, and uninstalling

```
<Game>\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\
    config_wuchang_minimap.txt      your settings (the F2 panel's Save writes this; the
                                    category filters write themselves)
    wuchang_minimap.log             the mod's log, rotated .1 .2 .3 per launch
    wuchang_minimap_found*.txt      the collection tracker, one file per save slot
                                    (NG+ keeps the save id, so start it over with the
                                    F2 panel's "Clear this save's found list")
    wuchang_minimap_waypoint*.txt   the full map's waypoints, one file per save slot
    wuchang_minimap_export_*.json   backups written by the F2 panel's Export button
    wuchang_minimap_last_stage.txt  crash breadcrumb: how far start-up got
    wuchang_minimap_watchdog.txt    stall report, rewritten each launch
    wuchang_minimap_firstrun.txt    records that the first-run key tip has been shown
    wuchang_minimap_panel.txt       which F2 panel sections are folded up
```

**To uninstall**, delete that whole `WuchangMinimap\` folder. The mod modifies no game
file and no save data. Copy the `wuchang_minimap_found*.txt` files out first to keep
your collection progress.

To turn it off without uninstalling, set `mod_enabled = 0` in the config, or delete
`enabled.txt` from the mod folder.

## Something went wrong

- **The game crashes on start-up** — almost always `HookInitGameState`; see
  [Requirements](#requirements).
- **Nothing on screen** — press `F2`. If the panel opens, the mod is running and only the
  minimap is suppressed: the **Overview** tab prints one orange `hidden because:` line
  naming the reason. Screenshot it.
- **Not even the F2 panel appears** — frame generation (DLSS-FG, FSR-FG) and other overlay
  proxies can present the frame themselves, so the game's `Present` never reaches the mod
  and it has nothing to draw into. Turn frame generation off to check.

**Bug reports**: attach `wuchang_minimap.log` from `ue4ss\Mods\WuchangMinimap\` — its
first six lines carry every version number a report needs. Add
`wuchang_minimap_last_stage.txt` if the game crashed, `wuchang_minimap_watchdog.txt` if it
froze, and your `config_wuchang_minimap.txt`. If you are asked to reproduce something,
first set `log_level = verbose` in the config (or **Tuning** → Log detail in the panel,
then Save), reproduce it, and send the log.

## More

- **This version**: [CHANGELOG.md](CHANGELOG.md), next to this file in the download.
- **Which build you have**: `BUILD_INFO.txt`, next to this file.
- **Every setting, documented inline**: `ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`.
- **Building from source**: `docs/DEVELOPMENT.md` in the repository, not in this download.

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This mod is not affiliated with the developers or publisher of Wuchang: Fallen Feathers.
It reads the game's memory and its packaged data; it never modifies game files or save
data.
