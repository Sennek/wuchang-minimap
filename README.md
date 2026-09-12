# WuchangMinimap

A minimap, a full chapter map and a compass for **Wuchang: Fallen Feathers**, built from the
game's own navmesh.

## What it is

- **Minimap** and **compass strip** in-world, **full chapter map** on `M`
- **Markers** for chests, pickups, shrines, bosses, NPCs and notes, with the game's own names,
  all five chapters and the DLC
- **Collection tracker** per save slot, up to 16 waypoints, name search on the full map
- **X-ray** on `TAB`: loot through walls, tinted by item quality
- Everything configured from the **F2** panel, saved by itself
- Keyboard, mouse and gamepad; English UI

## Requirements

**UE4SS for Wuchang: Fallen Feathers** (Wuchang mod 384), installed into
`Project_Plague\Binaries\Win64\`.

**It has to be this exact UE4SS build:**

```
UE4SS for WFF 1.79       mod 384, file version 1.79 (26 Feb 2026)
v3.0.1-934-gcac01ee2     the same build, as a git description
```

`ue4ss\UE4SS.log` opens with `UE4SS - v3.0.1 Beta #0 - Git SHA #cac01ee2` when it is the right
one.

This mod links straight to that DLL's exports, so **another UE4SS build will not work**, and the
failure is silent: the game plays normally, no overlay appears, `F2` does nothing, and
`ue4ss\UE4SS.log` says `Failed to load dll <...\Mods\WuchangMinimap\dlls\main.dll> for mod
WuchangMinimap, error: The specified procedure could not be found.` instead of a
`WuchangMinimap vX.Y.Z loaded` line naming the version you installed. `BUILD_INFO.txt` in the
download repeats the version.

### Then: HookInitGameState = 0

Open `Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini` and check (1.79 already ships it
set):

```ini
[Hooks]
HookInitGameState = 0
```

**Without this the game crashes about a third of a second into loading**, with or without this
mod. It is a UE4SS-plus-Wuchang problem, not a bug here.

## Install

1. Close the game.
2. Copy the `ue4ss\` folder from the download into
   `<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\` and
   **merge** when Windows asks. Everything lives under `ue4ss\Mods\WuchangMinimap\`.
3. Check `[Hooks] HookInitGameState = 0`.
4. Launch the game and load a save — the overlay is hidden on the main menu — then press `F2`.

To uninstall, delete `ue4ss\Mods\WuchangMinimap\`. That is the whole mod; it modifies no game or
save data.

## Hotkeys

| Key | Action |
|---|---|
| `F2` | Settings panel (Overview / Categories / Map & tracker / Keys tabs). A change applies as you make it and is written to the config by itself. `Back`+`RS` on a controller |
| `M` | Full map: drag or `WASD` to pan, wheel to zoom, `Q`/`E` for the floor, `Home` to fit, right-click a marker to waypoint it (or the ground to drop one), `F1` or `H` for the rest |
| `TAB` | X-ray highlight through walls, with names and distances. A toggle. `LB`+`RB` on a controller |
| `N` | Cycle the minimap zoom |
| `R` | Recentre the map |
| `F5` | Reload the config and data |

Everything is rebindable in `F2` → **Keys**.

## Known conflicts

None of these stops the mod working.

- **RenoDX** — `F6` is refused as a mod hotkey, because it is RenoDX's own toggle.
- **Another UE4SS C++ mod that also hooks `Present`** — the one combination that can lose an
  overlay: whichever installs second usually wins, and the loser is invisible. Test them one at a
  time before reporting a blank screen.
- **The Steam overlay** — the first run of a new install creates and destroys a throwaway
  swapchain, which Steam's overlay follows, so its FPS counter can end up pointing at nothing.
  Shift+Tab still works.
- **UE4SS's own console window** — not a mod conflict, but it looks exactly like one. Clicking or
  dragging in that black console window puts it in selection mode, which blocks whoever writes to
  it: UE4SS stops mid-startup and the game hangs before this mod has run a single line. Press
  `Esc` in the console to release it, or set `ConsoleEnabled = 0` in `ue4ss\UE4SS-settings.ini`.
  The tell is that `wuchang_minimap.log` was never written and `UE4SS.log` stops in the middle.

## Reporting a bug

Attach `%LOCALAPPDATA%\WuchangMinimap\wuchang_minimap.log` — the mod's own log, rotated per launch.
Its first lines carry every version, file size, the GPU and driver, the display's colour space
and the graphics modules loaded in the game - everything a report needs about the machine. Add
`wuchang_minimap_last_stage.txt` if the game crashed, `wuchang_minimap_watchdog.txt` if it froze,
and your `config_wuchang_minimap.txt` — that one is in the mod folder,
`ue4ss\Mods\WuchangMinimap\`.

**If there is no `wuchang_minimap.log` at all**, this mod never ran, so it cannot be the cause.
Check `ue4ss\UE4SS.log`: `Failed to load dll ... [0x7f] The specified procedure could not be found`
means the mod does not match your UE4SS build, and a log that simply stops mid-startup usually
means the console window above.

If the game hangs or crashes at start with the mod enabled, set `overlay_hooks = 0` in
`config_wuchang_minimap.txt` and restart: the mod then never touches DirectX and draws nothing,
while the tracker and the log keep running. Send me that log too — with and without, the pair
says which half is at fault.

If the minimap simply is not on screen, send me `wuchang_minimap.log` from that folder —
it names the exact reason it stayed hidden. If I ask you to reproduce something, add the line
`log_level = verbose` to `config_wuchang_minimap.txt` first, press `F5`, and reproduce it.

## Permissions and credits

**MIT licensed** — fork it, reuse it, translate it; the licence has to travel with it. Credit
appreciated, not required. Third-party components: Dear ImGui (MIT), MinHook (BSD-2-Clause), fmt
(MIT), RE-UE4SS (MIT); full notices ship in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
The licence itself is [LICENSE](LICENSE).

The map backgrounds, the marker database and every name shown are extracted from the game's own
data. Not affiliated with the developers or publisher of Wuchang: Fallen Feathers.

## For developers

- What changed in this version: [CHANGELOG.md](CHANGELOG.md).
- Building from source, layout and architecture:
  [docs/DEVELOPMENT.md](https://github.com/Sennek/wuchang-minimap/blob/master/docs/DEVELOPMENT.md)
  in the repository, not in the download.
