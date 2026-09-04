# WuchangMinimap - changelog

## 1.0.1

- **Shrines never disappear from the maps** - `Hide found` no longer removes a lit shrine
  from the minimap or the full map: a shrine is a landmark, not loot. A lit one is drawn
  solid, an unlit one hollow.
- **Right-click a marker on the full map to waypoint it**, again to take the waypoint off - on
  the map itself and on a search-result row. Right-clicking bare ground still drops one there.
- Full-map marker tooltip says `12 m above` / `8 m below` / `same level` instead of `-20 m up`.
- **No more black screen on some launches** - the DX12 hook addresses are found fresh every
  launch instead of being reused from `wuchang_minimap_hookaddr.txt`, which is now deleted on
  sight. Reusing them hooked the frame before ReShade, Streamline and the Steam overlay had
  been through their own start-up, and the game came up black.
- **The settings panel is rebuilt** - Overview, Categories, Map & tracker, Keys and Tuning.
  Every category filter is now one grid with a column per surface, the presets and the three
  HUD surfaces share one first screen, and `Reset to defaults` sits beside Save.
- **The x-ray key is now `TAB`** - Alt is a key the game itself uses.
- **The marker filters remember themselves** - a legend click survives a restart, with no Save.
- **The shrine list's Travel button is gone** - the game only travels at a shrine.
- **The nearest-unfound key ships unbound** - `G` is a key the game itself uses. Bind it on
  the `F2` panel's Keys tab.
- **The map's search results are a dropdown under the box** - they open where you are
  looking, never take the caret, and clicking one leaves you typing where you were.
- **Typing in a search box no longer fires the hotkeys** - the map's marker search and the
  panel's boxes keep every letter, and a binding capture waits for the caret to leave.

## 1.0.0

What the mod does:

- **A minimap and a full chapter map** on `M` to pan, zoom and place waypoints on.
- **The map is the game's own navigation data**, sliced by height: ground you can stand on.
- **Markers with the game's own names** for all five chapters and the DLC.
- **A name search on the full map**, with a nearest-first list you can waypoint straight from.
- **Up to 16 waypoints at once**; a key you bind sets one on the nearest marker you have not found.
- **Export / import** of the found list and waypoints as one JSON file - a merge, never a wipe.
- **An x-ray key** (`TAB`, or `LB`+`RB`) drawing markers through walls, tinted by item quality.
- **A collection tracker per save slot**: per-chapter progress, shrines, bosses from the save.
- **Everything configurable in-game** in the `F2` panel: every hotkey rebindable, no restart.
- **`wuchang_minimap.log`** rotated per launch, plus a watchdog file if the game hangs.

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
