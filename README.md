# WuchangMinimap

A minimap, full map, compass and collection tracker for **Wuchang: Fallen Feathers**,
with the map built from the game's own navmesh.

## Requirements

**UE4SS for Wuchang: Fallen Feathers** (Nexus mod **384**), installed into
`Project_Plague\Binaries\Win64\`.

This mod is compiled against one exact UE4SS build:

```
UE4SS v3.0.1-1111-g97b7e501   (the "experimental-latest" asset of Nexus mod 384)
```

It links directly to that DLL's exports, so **a different UE4SS build will not work** —
and it does not fail politely. What a mismatch looks like: the game launches and plays
normally, no overlay ever appears, `F2` does nothing, and `ue4ss\UE4SS.log` has

```
Failed to load dll <...\Mods\WuchangMinimap\dlls\main.dll> for mod WuchangMinimap,
error: The specified procedure could not be found.
```

instead of `WuchangMinimap v1.0.0 loaded`. If you see that, you are on another UE4SS
build; reinstall the one named above. `BUILD_INFO.txt` in this download repeats the
version, so a bug report can be checked against it.

Then open `ue4ss\UE4SS-settings.ini` and set:

```ini
[Hooks]
HookInitGameState = 0
```

**Without this the game crashes a third of a second into loading**, with or without this
mod. It is a UE4SS-plus-Wuchang problem, not a bug here.

## Install

1. Close the game.
2. Copy the `ue4ss\` folder from the download into
   `<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\`
   and **merge** when Windows asks.
3. Launch the game, load a save (nothing shows on the main menu), press `F2`.

## Hotkeys

| Key | Action |
|---|---|
| `F2` | Settings panel |
| `M` | Full map |
| `LALT` | X-ray highlight through walls — a toggle |
| `LB`+`RB` | The same, on a controller |
| `N` | Cycle the minimap zoom |
| `R` | Recentre the full map |
| `F5` | Reload the config, maps and markers |

Everything is rebindable in `F2` → **Bindings**.

## Known conflicts

None of these stops the mod working, but all three have surprised someone.

- **ReShade / RenoDX** (any `dxgi.dll` or `d3d12.dll` proxy next to the game exe). The
  mod hooks `Present` through whatever wrapper is already installed, so the overlay is
  drawn **before** ReShade's effects — colour-grading, tone-mapping and sharpening are
  applied on top of the minimap, and a strong LUT will tint it. Nothing breaks; the map
  just is not colour-accurate. Also: RenoDX's default toggle is `F6`, so `F6` is refused
  as a mod hotkey (see the config file's header).
- **Another UE4SS C++ mod that hooks `Present` too.** Two overlays on one swapchain is
  the one combination that can actually lose an overlay: whichever installs second
  usually wins, and the loser is invisible. If you run one, test them one at a time
  before reporting a blank screen here.
- **The Steam overlay.** Bringing the mod up creates and destroys a throwaway swapchain
  once, and Steam's overlay follows what it sees created — the reported symptom is the
  Steam FPS counter pointing at nothing. The addresses are cached in
  `wuchang_minimap_hookaddr.txt` after the first launch, so this happens on the **first**
  run of a new install only. Shift+Tab still works.

## Files it writes, and uninstalling

Everything the mod reads or writes lives in one folder:

```
<Game>\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\
    config_wuchang_minimap.txt      your settings (the F2 panel's Save writes this)
    wuchang_minimap.log             the mod's log, rotated .1 .2 .3 per launch
    wuchang_minimap_found*.txt      the collection tracker, one file per save slot
    wuchang_minimap_waypoint.txt    the full map's waypoint
    wuchang_minimap_hookaddr.txt    cached hook addresses (delete to re-discover)
    wuchang_minimap_last_stage.txt  crash breadcrumb: how far start-up got
    wuchang_minimap_watchdog.txt    written only if the game froze
```

**To uninstall**, delete that whole `WuchangMinimap\` folder. That is the entire mod: it
modifies no game file and no save data, and removing it leaves the game exactly as it
was. To keep your collection progress for later, copy the `wuchang_minimap_found*.txt`
files out first.

To turn it off without uninstalling, set `mod_enabled = 0` in the config, or delete
`enabled.txt` from the mod folder.

## Bug reports

Attach `wuchang_minimap.log` from `ue4ss\Mods\WuchangMinimap\` — and if you are asked to
reproduce something, set `log_level = verbose` in the config first.

Add `wuchang_minimap_last_stage.txt` if the game crashed and
`wuchang_minimap_watchdog.txt` if it froze.

## More

- **What changed in this version**: [CHANGELOG.md](CHANGELOG.md), next to this file in
  the download.
- **Which build you have**: `BUILD_INFO.txt`, next to this file.
- **Every setting, documented inline**: `ue4ss\Mods\WuchangMinimap\config_wuchang_minimap.txt`.
- **Building from source**: see `docs/DEVELOPMENT.md` in the repository — it is not part
  of this download.

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
