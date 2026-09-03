# WuchangMinimap - changelog

All notable changes to this mod. Versions follow `MAJOR.MINOR.PATCH`.

## @@VERSION@@ - @@DATE@@

A settings and legibility release. Nothing about the map, the markers or the tracker
changed; everything here is about the parts you look at and edit.

### Added
- **`ui_scale`** - one setting that fixes the mod on a high-DPI screen. `auto` (the
  default) works it out from your resolution; a number pins it. It scales the font, the
  settings panel and every pixel-sized setting (marker sizes, the compass, the minimap's
  offsets), so a config tuned at 1080p is right at 4K without being re-tuned.
- **`hud_preset`** - `top-left`, `top-right`, `bottom-left` or `bottom-right` moves the
  minimap **and** the compass together, instead of three settings that can disagree.
  `custom` (the default) keeps exactly the placement you already have. The compass can
  now sit at the bottom of the screen (`compass_anchor`).
- **The `F2` panel is now three tabs.** *Player* has everything you would plausibly
  change, including three one-click presets - **Minimal HUD**, **Loot hunting**,
  **Exploration** - that set several settings at once. *Advanced* has the rest.
  *Debug* only exists for developers and is not present in a normal install.
- **Category filters are coloured chips** instead of rows of identical checkboxes: each
  chip is filled with the colour that category is drawn in, so the filter is also the
  legend. The "mark items whose level is loaded but absent" categories can be edited in
  the panel for the first time.
- A **Revert** button next to Save, which re-reads the file and throws away whatever you
  were fiddling with.

### Changed
- **Save no longer rewrites your config file.** It replaces the values on the existing
  `key = value` lines and leaves every comment, every blank line, your ordering and any
  setting it does not recognise exactly where they were. Previously one click replaced
  the whole documented file with a bare list.
- `config_wuchang_minimap.txt` is laid out under two banners, `; ---- PLAYER SETTINGS ----`
  and `; ---- ADVANCED ----`, matching the panel's first two tabs.
- `enabled` is now called **`overlay_enabled`** (the three switches are `mod_enabled` =
  the whole mod, `overlay_enabled` = anything drawn, `show_minimap` = the disc). The old
  name still works and says so once in the log.
- The minimap shape is a checkbox rather than a drop-down.

### Removed
- Eleven settings that were internal sanity limits, never preferences, are fixed values
  now: `minimap_circle_segments`, `slice_min_px`, `slice_max_px`, `map_slice_margin`,
  `reader_max_widgets`, `reader_max_menu_roots`, `reader_max_levels`, `markers_live_max`,
  `markers_id_cache_max`, `markers_class_cache_max`, `markers_fallback_max_per_class`.
  An old config that still lists one is fine - it says so once in the log and carries on.
- About twenty developer-only settings moved out of the shipped config into an optional
  `config_wuchang_minimap_dev.txt` that is not part of this download.

## 0.9.1 - 2026-09-02

### Added
- `mod_enabled` master switch: turns the whole mod inert without uninstalling it, and
  back on again within a second of saving the file - no restart.
- Real item names on pickups and in the x-ray labels, and item-quality colours taken
  from the game's own pickup-beam palette.
- Items collected before the mod was installed are now marked as found once their level
  is loaded and no live actor for them ever appears.
- 38 settings that used to be baked into the code.

## 0.9.0 - 2026-09-02

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
