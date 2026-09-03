# WuchangMinimap - changelog

All notable changes to this mod. Versions follow `MAJOR.MINOR.PATCH`.

## Unreleased

A pass over the first round of in-game feedback: the x-ray sees more, the panel says
less, the numbers fit on the screen, and every hotkey can be rebound in the panel.

### Added
- **A Bindings tab in the F2 panel.** Every hotkey - settings panel, full map, recentre,
  minimap zoom, reload, map-to-clipboard and the x-ray hold key - is rebound by clicking
  it and pressing the new key (Esc cancels), with a reset per row, a **Reset every
  binding** button and a warning when two actions end up on the same key. The gamepad
  chord is edited there too. Changes take effect at once and are written by **Save**.
- **More keys are bindable**: ENTER, BACKSPACE, the arrows, INSERT / DELETE / HOME / END /
  PAGEUP / PAGEDOWN, NUM0..NUM9 and the numpad operators, MOUSE3 / MOUSE4 / MOUSE5, and
  `none` to leave an action unbound. F6, F9, F10 and F12 stay refused.
- **The compass tells you how far and whether it is up or down.** Every bearing pip now
  carries the distance in metres just outside the strip, nearest first, and a marker more
  than `compass_pip_height_uu` (300 uu = 3 m) off your own height gets an up or down arrow
  beside its glyph. New keys `compass_pip_labels` (Player) and `compass_pip_height_uu`
  (Advanced).
- **Three clearly separated category filters** in the Player tab, one per feature: **Map &
  minimap**, **Compass** and **X-ray highlight**, all three the same widget with `all` /
  `none` buttons, and every chip now carrying its category's glyph as well as its colour.

- **NPCs have names.** Every NPC marker used to read "NPC" and every merchant marker
  "Merchant"; they now carry the game's own English name - "He Youzai", "Huang Jian'e",
  "Wu Gang", "Qiao Ying", "Villager" - on the map, in the tooltip and in the x-ray label.
  All 248 of them, and no marker moved. The ones typed as merchants turn out to be the
  game's readable notes rather than shops, so they read "Reading point".
- **Bosses in every chapter, with their real names.** Chapters 2, 3 and 5 had no boss
  markers at all and chapter 4 had two; they now have 5, 6, 2 and 5, and every boss
  marker is labelled with the game's own English name ("Reborn Treant - Soulwood",
  "Sovereign - Zhang Xianzhong") instead of "Boss".
- **Shrines you have lit now count as found.** The statistics table said 0/12 shrines
  while your save had eighteen lit, and lit shrines still drew as un-found on the map.
- **NPCs and merchants count as met** once you have been within 30 m of them, so the
  NPC and Merchant columns of the statistics table fill in as you explore.
- **Bosses count as defeated.** A boss you have killed is marked found and stays marked
  after you leave the arena.

### Changed
- **The x-ray highlight ships with shrines, bosses, NPCs and merchants on** as well as
  chests and pickups, so holding the key near a shrine or a merchant shows something. If
  your config already spells `highlight_categories` out, that line still wins - change it
  in the panel.
- **"Hide collected loot" now only hides loot.** `highlight_show_found = 0` used to drop
  every found marker from the x-ray, which meant a lit shrine or an NPC you had already
  met disappeared. It now suppresses chests, pickups and hidden items only - the three
  categories where finding a thing consumes it; shrines, bosses, NPCs, merchants, doors
  and fog gates are landmarks and are always drawn. The **Loot hunting** and
  **Exploration** presets pick x-ray sets to match.
- **The F2 panel opens in the middle of the screen** and takes the whole mouse while it is
  open, raw mouse input included, so moving the mouse over the panel no longer turns the
  game camera. You can still drag the window anywhere; the keyboard still reaches the game
  except while a text field or a key capture wants it.
- **The collection statistics show only what you collect** - Shrines, Chests, Pickups,
  Bosses, NPCs, Merchants - in both tables. The per-chapter matrix no longer scrolls
  sideways at 1080p, every one of the six columns is always present (empty cells read
  `-`), and the Enemies / Doors / Ladders / Lifts / Fog gates / Hidden / Other / all
  columns are gone.
- **The option help text says what the option does and stops there.** The "this does not
  touch X" sentences, the pixel-count explanations and the implementation notes are out of
  the panel, and no user-visible text mentions which version something changed in.
- **The manual dumps are buttons, not hotkeys.** The recon dump and the runtime navmesh
  dump are both on the F2 Debug tab now; `recon_dump_key` and `navmesh_dump_key` are gone
  (an old `recon_dump_key` line logs one warning and is ignored). The player-facing
  map-to-clipboard key stays a binding.

### Fixed
- **The stutter three times a second is gone.** The mod's check for "is a game menu open?"
  swept every object in the game in one go, which cost about 25 ms of the game's own frame
  time - a dropped frame roughly three times a second, all the time, whether or not
  anything was happening. That sweep now walks the object list a small slice at a time,
  spread across frames, so no single frame pays more than a fraction of a millisecond; and
  it slows itself down while you are simply playing instead of running flat out. The
  minimap still hides the instant you open a menu and comes back the instant you close it.
  The F2 Debug performance table calls the new rows **widget scan slice** and **widget
  round commit**.
- **The minimap no longer stays away for good after a fast travel taken from the shrine
  menu.** In that one case the mod could lose the player and never find them again: the
  minimap and the compass stayed hidden, the full map refused to open ("there is no
  gameplay pawn") and only restarting the game brought them back. The reader now looks for
  the player twice a second whatever else it has or has not found, and it says in the log
  when it has been looking for more than ten seconds.
- **Killing something now actually registers.** The dead-enemy and boss-defeated rules
  both read the character's health, and on this build they never found it, so corpses
  stayed on the minimap and no boss was ever marked as defeated. The mod now finds the
  health component itself instead of assuming what it is called, reads the two values
  under both names the game could be using, and if it still cannot, it writes one line
  to the log naming every number it did find on the component.
- **A dead enemy no longer reappears at its spawn point.** Dropping the corpse only
  removed the moving marker; the enemy's original spot is on the map as well, so the
  marker jumped back there instead of going away. Both are hidden now until the game
  clears the body away.
- **A boss you have beaten is no longer highlighted through walls.** It keeps its
  hollow "found" mark on the map, but there is nothing left in the arena to see.
- **"Shrines lit" in the log was always zero** after the first session, because it
  counted only shrines lit for the first time. It now reads `lit 8 of 12`.
- **NPCs and merchants are shown where they are, not where they started.** People move
  in this game - talk to a quest NPC and it relocates - and the mod was still drawing
  the original spot, so the x-ray could label an "NPC 2 m" through a wall at a place
  the NPC had left. A live NPC's own position always wins now; once the mod can see
  that the area is loaded and nobody is there, the old spot is not drawn at all; and
  the x-ray only ever highlights an NPC or a merchant it can actually see. Whether you
  have met them is unaffected. ("It can actually see" is now literal: an NPC whose
  actor the mod found but could not locate no longer counts as seen - that was the
  remaining way an "NPC 2 m" label could hang over an empty spot.)
- **Dead enemies no longer sit on the minimap.** A corpse keeps its position in the game
  for a while after it dies; enemies are now dropped from the map as soon as their health
  reads zero, and a live enemy marker that stops being seen is dropped after one sweep
  instead of two.
- Eight lightable reeds in a Chapter 1 boss arena, a boss spawner and a stage light were
  all drawn as bosses. They are not.

## @@VERSION@@ - @@DATE@@

An extras release: one collection file per save, a statistics page, the map on your
clipboard, a shrine list, and the diagnostics that make a bug report answerable.

### Added
- **One collection file per save game.** `wuchang_minimap_found.txt` was global, so a
  second character started with the first one's collection already ticked off. The mod
  now works out which save slot is loaded and uses
  `wuchang_minimap_found_<slot>.txt`; the first time it sees a new slot it copies your
  old shared file into it, so nothing is lost. Switching saves from the main menu swaps
  files with no restart. `found_profile = auto | shared | <name>` if you want to decide
  yourself, and the F2 Player tab always shows which file is in force and how it was
  chosen.
- **A collection statistics page** - found / total for every category, the same per
  chapter, the shrines your save has lit, and one overall percentage. On the F2 Player
  tab and behind a **Stats** button on the full map.
- **`C` copies the full map to the clipboard** while the map is open, ready to paste into
  a chat or an image editor. Nothing is written to disk.
- **A shrine list on the full map** (**Shrines** in the header): every shrine of the
  chapter with its in-game name, the distance to it and whether you have lit it. Click a
  row to put a waypoint on it, double-click to centre the map there.
- **Fast travel, off by default.** With `fast_travel_enabled = 1` each unlocked shrine
  gets a **Travel** button that calls the game's own fast-travel function - the same one
  the shrine menu uses, so the loading screen, the save and the level streaming all happen
  normally. It refuses to travel to a shrine your save has not unlocked, and it refuses
  outright unless a check of the game's own function signature passes, saying why in the
  panel. Treat it as experimental.
- **A first-run tip**: on the first launch after installing, a ten-second message naming
  the keys that are actually bound. `first_run_toast = 0` to skip it.
- **A crash breadcrumb.** `wuchang_minimap_last_stage.txt` holds one line saying which
  stage the overlay was in. If the game ever dies with the mod loaded, that file survives
  when the log does not - please include it in a bug report. `crash_breadcrumb = 0` to
  turn the file off.
- **"Disable for this session"** next to the master switch: stops everything now, exactly
  like `mod_enabled = 0`, but without touching your config file. Saving or editing the
  file turns it back on within a second, so ruling the mod out of a problem no longer
  leaves you with a config to repair.

### Changed
- The F2 Player tab's collection block is the new statistics page rather than a
  three-category table.
- The full map's `?` legend lists the new `C` and **Shrines** actions.

### Notes
- The shrine names and destinations come from a new `markers\shrines.json`, extracted from
  the game's own fire-point table: 88 rows, all of them named, 50 of them real shrines
  (the rest are the boss doors and quest points that share the table).

## 0.9.3 - 2026-09-03

A legibility release. Everything you look at got easier to read; nothing about the map,
the markers or the tracker changed.

### Added
- **Every marker category now has its own SHAPE.** Boss, elite and enemy used to be the
  same triangle at three sizes - a three-pixel difference - and "hidden" was a shrine
  with the fill switched off. Fourteen categories, fourteen shapes, each with a dark
  halo behind it so it survives on a bright scene.
- **`theme = ink | neutral`** presets the frame, the disc backdrop, the label plates and
  the walkable fill in one setting. `neutral` is exactly what 0.9.2 looked like; `ink` is
  bronze on near-black with a warmer parchment fill. Any colour you have written out in
  the config file still wins over the theme.
- **`palette = default | colorblind`** switches the marker hues to an Okabe-Ito-derived
  set (and the item-quality colours with them). The shapes never change, which is what
  keeps two categories apart when a hue is reused.
- **`zoom_key`** (default `N`) cycles the minimap zoom through **`minimap_zoom_presets`**
  (13 / 26 / 52). The mouse wheel over the minimap does the same, but only while the F2
  panel is open - during play the wheel belongs to the game.
- **The full map has a legend**, on the right, which is also the filter: the glyph, the
  category, and how many of them you have found in this chapter. Clicking a row toggles
  it. The row of fourteen coloured buttons is gone.
- **Fit** (button, or `Home`) zooms the full map to the whole chapter, and **`?`** (or
  the gamepad's Back button) shows the controls as a two-column legend - the gamepad
  half only when a pad is plugged in.
- **`compass_plate = 0`** turns off the filled plate behind the compass strip, leaving
  ticks and letters with a shadow, so it sits more lightly over the game's own HUD.
- **`highlight_labels_max`** (12) caps the x-ray NAMES separately from the glyphs (60).
- Small things that move: the HUD fades in over 150 ms (it still disappears instantly),
  the waypoint pulses, and a marker you have just collected gets a brief ring.

### Changed
- **Found markers are hidden by default** and are drawn as an outline, not a dim blob,
  when you switch them back on ("Show found markers" on the F2 Player tab).
- The minimap always shows **north**: an `N` and four ticks on the rim, which rotate with
  you in rotate mode. Before, the north dot only existed if you had turned rotation on.
- X-ray labels no longer stack on top of each other: the nearest markers get the names,
  never two for glyphs a few pixels apart, and a label that has to move down draws a line
  back to its glyph.
- The full map shows the floor offset in metres (`+2.0 m`), and a right-click on the
  waypoint you already placed clears it, with a one-second "waypoint cleared" message.
- The compass is a little narrower by default (0.34 of the screen), and bearing pips of
  the same category within three pixels of each other collapse into one.
- The marker id in the full map's tooltip is now only shown with `debug_readout`.

## 0.9.2 - 2026-09-03

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
