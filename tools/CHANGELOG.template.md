# WuchangMinimap - changelog

## 1.0.0

What the mod does:

- A **minimap** with a compass strip, and a **full chapter map** on `M` to pan, zoom and
  place waypoints on. The background is built from the game's own navigation data, so
  what is drawn is ground you can stand on. The picture is sliced by height, so only the
  storey you are on is opaque.
- **Markers with the game's own names** - chests, pickups, shrines, bosses, NPCs, notes,
  doors, ladders, lifts and fog gates - for all five chapters and the DLC.
- **A name search on the full map**: type part of a name and only those markers are
  drawn, with a nearest-first list you can waypoint straight from.
- **Up to 16 waypoints at once**, drawn on the map, the minimap and the compass; the
  distance readout follows the nearest one. `G` sets one on the nearest marker you have
  not found yet.
- **Export / import**: the F2 panel writes your found list and waypoints to one JSON
  file next to the DLL, and reads one back in - a merge, so nothing you have is
  overwritten; waypoints you already have, and any past the 16-waypoint cap, are
  reported rather than added twice.
- **An x-ray key** (`LALT`, or `LB`+`RB`) that draws nearby markers through walls with
  names and distances, tinted by the game's own item-quality beam colours.
- **A collection tracker per save slot** that ticks things off as you take them, with
  per-chapter and per-category progress, a shrine list, and bosses read from the save.
- **Everything configurable in-game**: the `F2` panel's Player, Advanced and Bindings
  tabs, every hotkey rebindable, no file editing and no restart.
- **`wuchang_minimap.log`** next to the config, rotated per launch, opening with every
  version number a bug report needs. `wuchang_minimap_watchdog.txt` is written if the
  game stops responding, naming the stalled thread and what it was doing.

### Known issues

- **The DLC has no map.** The game ships no navigation data for it. Markers work there,
  but its 7 shrines keep their internal ids: the DLC's fire points have no row in the
  game's fire-point table.
- **A boss read from your save may be one you fought rather than one you beat.** For a
  boss killed before the mod was installed the world holds nothing to read, so the mod
  uses the arena's respawn point, which the save remembers unlocking. It is recomputed
  every launch and never written to the collection file, so `boss_defeat_from_save = 0`
  undoes it.
- **A few dozen detached patches of walkable ground per chapter are drawn although you
  cannot walk to them.** The navmesh carries no notion of "reachable", and a strict
  reachability filter costs 56 % of the walkable area, so the filter keeps any detached
  patch of at least 40 m2 or one that carries a marker. Chapter 1's palace lake is the
  most visible case.
- **The overlay draws underneath ReShade's effects**, so a heavy preset tints it.
- **Enemy markers are off by default.** The live sweep refreshes them about once a
  second, so they lag behind anything that moves.
- **Fast travel is off by default** (`fast_travel_enabled`) and is experimental.
