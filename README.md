# WuchangMinimap

A minimap, full map, compass and collection tracker for **Wuchang: Fallen Feathers**,
with the map built from the game's own navmesh.

## Requirements

**UE4SS for Wuchang: Fallen Feathers** (Nexus mod **384**), installed into
`Project_Plague\Binaries\Win64\`. Then open `ue4ss\UE4SS-settings.ini` and set:

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

## Bug reports

Attach `wuchang_minimap.log` from `ue4ss\Mods\WuchangMinimap\` — and if you are asked to
reproduce something, set `log_level = verbose` in the config first.

Details: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md), [CHANGELOG](tools/CHANGELOG.template.md).

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
