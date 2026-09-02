# WuchangMinimap - changelog

All notable changes to this mod. Versions follow `MAJOR.MINOR.PATCH`.

## @@VERSION@@ - @@DATE@@

First public build. **Beta:** the Chapter 1 map, the markers, the minimap, the full map
and the collection tracker have been tested in-game; the Chapter 2-5 maps and the x-ray
highlight have not.

### Added
- Live minimap built from the game's own navigation mesh (walkable area only),
  height-sliced so only the storey you are standing on is drawn solid.
- Full pannable / zoomable map on `M`, with mouse, keyboard and gamepad control.
- Markers for shrines, chests, pickups, bosses, elites, NPCs, merchants, doors,
  ladders, lifts, fog gates and hidden things - a static database extracted from the
  game's own levels, merged with the live state of everything currently streamed in.
- Collection tracker with auto-marking (`wuchang_minimap_found.txt`) and a waypoint you
  can set anywhere on the full map.
- Compass strip with bearing pips for the waypoint and for nearby markers.
- Hold-key x-ray highlight of nearby uncollected loot (`LALT`, or `LB`+`RB`).
- `F2` settings panel; every setting is also a plain-text line in
  `config_wuchang_minimap.txt`, and `F5` reloads it without restarting the game.

### Known limitations
- The DLC has no map: the game ships no navigation data for it. Markers still work.
- The overlay draws underneath ReShade's effects, so a heavy ReShade preset tints it.
- Enemy markers are off by default - the sweep refreshes them about once a second, so
  they lag behind moving enemies.
