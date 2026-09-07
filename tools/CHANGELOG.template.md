# WuchangMinimap - changelog

## 1.0.2

A player reported the game hanging at start-up with the mod enabled, ending in a crash
after about two minutes. On their machine the mod's very first frame kills the D3D12
device. That does not happen here, on any of the machines or game installs it has been
tried on, so this release does not claim to fix the cause - it changes what the mod does
when it happens, and makes the mod say enough for the cause to be found.

**If the game hangs or crashes at start-up**

- **`overlay_hooks = 0`** in `config_wuchang_minimap.txt` (Advanced section) now runs the
  mod with nothing of it anywhere near DirectX: no hooks, no overlay, nothing drawn. The
  collection tracker, the found file and the log keep working. It takes effect on a
  restart. Before this, the only off switch was `mod_enabled = 0`, which stops everything.
- **The mod no longer hangs the game when its device dies.** It logs the reason DirectX
  gives, releases its objects once and stays off for the rest of the session instead of
  building everything again on a dead device - which is what turned a fast failure into a
  two-minute freeze.
- **`overlay_enabled` is not a setting** and has not been one since 1.0.0. If somebody
  told you to set it, that test did nothing; the line can be deleted.

**Getting the overlay onto the right frame**

- The mod now works out **which command queue the game actually presents from** by how
  much work each one submits between frames, instead of taking the first one it sees. On a
  PC with DLSS frame generation, an FSR3 frame-generation mod or another overlay, several
  queues are in play and the wrong one means a minimap that tears, lags a frame or never
  appears.
- Before it draws anything, the mod submits **one empty test frame** and waits for the GPU
  to finish it. Everything else it needs is built only after that frame comes back clean.
- If it cannot tell two queues apart it says so in the log, waits, and after ten seconds
  takes the better of the two rather than leaving you with no minimap and no explanation.

**The log is what a bug report is made of, so it stopped lying**

- The first lines now carry the **size of the game's executable** and of `UE4SS.dll`, not
  a version number that two different game patches share.
- `wuchang_minimap_last_stage.txt` can no longer name a start-up stage the mod has already
  passed, and it says `mod off` while the mod is off instead of naming a chapter change.
- The freeze watchdogs now fire on a start-up freeze and on a frame counter that has
  stopped moving, which are the two cases they used to miss. A long level load is not
  reported as a freeze.
- `game-state reader: the ProcessEvent pump is not firing` no longer prints once on every
  machine a second after the game starts. It only appears when the pump has really stopped.
- Lines written from the render and game threads carry the time they were written, not the
  time they reached the file.

**Known, not caused by this mod**

- Clicking or dragging inside UE4SS's black console window puts it in selection mode, which
  freezes UE4SS in the middle of start-up and hangs the game before this mod runs at all.
  Press `Esc` in that window, or set `ConsoleEnabled = 0` in `ue4ss\UE4SS-settings.ini`. The
  tell is that `wuchang_minimap.log` was never created.

## 1.0.0

What you get in this build.

**The map**

- **A minimap, a compass strip and a full chapter map** on `M`: drag or `WASD` to pan,
  wheel to zoom, `Q`/`E` for the floor, `Home` to fit, `C` to copy the map to the clipboard.
  A controller drives all of it.
- **The map is the game's own navigation data** - ground you can stand on, not a drawing.
  Surfaces the game walks but a player cannot reach (wall tops, roof ridges, the outside
  faces of arena walls) are left out, so a boss arena is the arena and its entrance.
- **Height is drawn as colour.** One shading ramp runs from the chapter's low ground to its
  high ground, so a slope reads as a slope and a gallery overhead reads as overhead. The
  floor you are standing on always wins over anything above it, and the full map shades the
  whole chapter at once instead of following your feet.
- **The chapter changes when the ground under your feet does**, so standing at a border -
  the passage into Hillswatch, say - shows the chapter you are actually in.
- **The overlay finds its DX12 hook addresses fresh every launch**, never from a cache.

**Markers and the tracker**

- **The game's own names** on chests, pickups, shrines, bosses, NPCs and notes, for all five
  chapters and the DLC, plus a name search on the full map with a nearest-first list.
- **Loot dropped in front of you is named too** - a drop reads the item it is holding, so it
  arrives as itself instead of a nameless pickup.
- **A marker off your own floor carries an up or down arrow** on the minimap, the full map
  and the compass alike; `compass_pip_height_uu` is how far off counts.
- **Shrines stay on the map** even with `Hide found` on - lit solid, unlit hollow.
- **Up to 16 waypoints**, kept per save slot. Right-click a marker (on the map or on a
  search row) to waypoint it and again to take it off; right-click bare ground to drop one.
  A key you bind marks the nearest thing you have not collected - unbound as shipped,
  because the game itself uses `G`.
- **A collection tracker per save slot** with export / import as one JSON file, and a button
  to clear a save's list for NG+.
- **An x-ray key on `TAB`** (`LB`+`RB` on a controller) drawing loot through walls, tinted
  by item quality.

**Settings**

- **The `F2` panel is the whole configuration**: Overview, Categories, Map & tracker, Keys.
  Every change applies on the spot and is written to `config_wuchang_minimap.txt` by itself
  - there is no Save button and nothing to remember.
- **The Keys tab reads your real in-game bindings**, so it names the action a key would take
  away from the game and follows a remap you made in the game's own options. `Back`+`RS`
  opens the panel on a controller, `Back`+`Y` the map.
- Typing in a search or name box never fires a hotkey.

**Known issues**

- **The DLC has no map.** The game ships no navigation data for it. Markers work there,
  but its 7 shrines keep their internal ids: the DLC's fire points have no row in the
  game's fire-point table.
- **A boss read from your save may be one you fought rather than one you beat.** For a
  boss killed before the mod was installed the world holds nothing to read, so the mod
  uses the arena's respawn point, which the save remembers unlocking. It is recomputed
  every launch and never written to the collection file, so `boss_defeat_from_save = 0`
  undoes it.
- **The overlay draws underneath ReShade's effects**, so a heavy preset tints it.
- **Enemy markers are off by default.** The live sweep refreshes them about once a
  second, so they lag behind anything that moves.
