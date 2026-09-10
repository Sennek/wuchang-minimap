# WuchangMinimap - changelog

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
