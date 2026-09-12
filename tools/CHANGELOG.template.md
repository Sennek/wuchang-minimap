# WuchangMinimap - changelog

## Unreleased

Added
- Loot is eleven marker categories instead of one: consumables, materials, key items, weapons, armour, amulets, jades, spells, harvest nodes, cannon ammo and plain items. Each has its own filter checkbox, count and glyph, so the map can answer "where are the amulets" instead of showing 1105 identical dots.
- The 17 cannon resupply crates are their own category and read "Shrapnel Bomb"; they stand beside the cannon emplacements.

Changed
- Loot is coloured by quality tier everywhere it is drawn — minimap, full map, compass, x-ray and the F2 legend — in the game's own three pickup-beam hues: blue for common, pink for equipment, gold for key items and materials. The kind of loot is a small mark inside the disc, drawn on the full map and in the legend; the minimap shows the disc alone, where colour is the question a glance asks.
- A marker you have already collected keeps its colour: it is drawn as a hollow ring at full strength over a faded fill, so what fades is the fill and not the identity. `markers_found_alpha` fades the fill.
- The eleven names replace `pickup` in `markers_categories`, `highlight_categories` and `markers_absence_categories`. A config file that still says `pickup` still means all eleven, and the F2 panel rewrites it into the current names when it next saves.
- The player's storage box is filed under Other, not as loot.
- Loot an enemy drops is filed by what it is: a dropped chest piece draws as armour, a dropped jade as a jade. The kind comes from the item itself, so a drop and a pickup of the same thing read the same.

Fixed
- The legend's chapter and its counts follow the chapter you are in. They used to stay on the chapter that was in force when you last picked something up, so the full map could say "all chapters" while the map itself showed one.

Removed
- `xray_rarity_colors_enabled`, `markers_rarity_tint` and `xray_rarity_colors`. The quality tint is no longer an alternative to the category colour, it is the loot colour on every surface; the three hues come from `palette` like every other marker colour. A config file that still carries one of these keys gets a warning naming it.

## 1.2.0

Added
- The 3 red riddle doors and the 7 yellow chisel doors are their own marker categories, Mystery gates and Benediction doors, each with its own glyph, colour, filter checkbox and count. The seven chisel doors were never on the map before.

Fixed
- The game no longer crashes at the title screen with RivaTuner Statistics Server or MSI Afterburner installed alongside ReShade: the overlay presents no swapchain of its own, it draws into a composition surface beside the game's frame.
- Changing the resolution no longer freezes the game for two seconds.
- Turning the mod off can no longer hang the game when the overlay takes too long to shut down.
- A Present that fails is logged whatever the reason, not only when the graphics device is lost.
- A riddle door you have answered now counts as opened. The mod read the flag that drives the door's opening animation, which is false again the moment the area reloads; it reads the one the save restores.
- Where two chapters meet, the map stays on one of them instead of swapping back and forth and going blank: the chapter is chosen by how much ground its map has around the player, not by one spot under his feet.
- Resting at a shrine no longer leaves the minimap and the full map hidden until the next area load.
- A note the player can neither see nor read is no longer counted as found when he walks past it.
- Loot that is not in the world yet - an item a quest or an NPC has still to hand over - is no longer drawn on the map and the x-ray over empty ground.

Changed
- The log, the found-items file, the waypoint and the rest of the mod's state live in %LOCALAPPDATA%\WuchangMinimap. They survive reinstalling the mod, and they work on the Game Pass build, where the mod folder is read-only. Existing files are moved there on the first launch.

## 1.1.1

Fixed
- A menu opened with a gamepad hides the minimap at once, instead of only once the mouse was moved.
- The minimap no longer comes back over a menu a second after that menu opens.
- The chapter banner is no longer mistaken for a menu.

## 1.1.0

Fixed
- The minimap works with NVIDIA frame generation instead of crashing the game.
- The overlay submits on a command queue it creates, never on the game's or another overlay's.

Changed
- ReShade and RenoDX no longer tint the minimap: it is composed after the game's frame, not into it.
- Three DirectX hooks instead of four.

## 1.0.2

Added
- `overlay_hooks = 0` (Advanced): the mod loads with no DirectX hooks and no overlay at all.
- The log records the GPU and driver, the display's colour space and peak brightness, the present mode and every graphics module in the process.
- The log names where the game's own logs and crash dumps are written.

Fixed
- A lost D3D12 device no longer hangs the game: the mod logs the reason, releases its objects once and stays off for the session.
- The overlay picks the command queue the game presents from by how much work each queue submits, instead of the first one it sees.
- The overlay waits for the GPU to complete one empty test frame before it builds anything else.
- `wuchang_minimap_last_stage.txt` no longer names a start-up stage that was already passed, and says `mod off` while the mod is off.
- The freeze watchdogs fire on a start-up freeze and on a stopped frame counter, and a long level load is no longer reported as a freeze.
- `the ProcessEvent pump is not firing` no longer prints a second after the game starts.
- Log lines from the render and game threads carry the time they were written.

## 1.0.0

Added
- A minimap, a compass strip and a full chapter map on `M`, with pan, zoom, floor stepping and copy to clipboard; a controller drives all of it.
- The map is the game's own navigation data, with the surfaces it walks but a player cannot reach left out.
- Height is drawn as colour on one ramp per chapter, and the floor you are standing on wins over anything above it.
- The chapter changes when the ground under your feet does.
- The game's own names on chests, pickups, shrines, bosses, NPCs and notes, for all five chapters and the DLC, with a nearest-first name search.
- Loot dropped in front of you is named by the item it is holding.
- A marker off your own floor carries an up or down arrow on the minimap, the full map and the compass.
- Shrines stay on the map with `Hide found` on: lit solid, unlit hollow.
- Up to 16 waypoints per save slot, dropped and removed by right-click, plus a bindable key for the nearest thing you have not collected.
- A collection tracker per save slot, with JSON export and import and a per-save clear for NG+.
- An x-ray key on `TAB` drawing loot through walls, tinted by item quality.
- The `F2` panel configures everything and writes the config by itself.
- The Keys tab reads your real in-game bindings, so it names what a key would take from the game and follows a remap made in the game's own options.
