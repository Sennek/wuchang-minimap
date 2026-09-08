# WuchangMinimap - changelog

## 1.1.0

**Fixed**

- **The minimap works with NVIDIA frame generation, and no longer takes the game down with it.**
  On a PC where DLSS frame generation is available, the game hands its frames to it, and the
  buffers behind them stop being the game's. The mod used to draw into them anyway: DirectX
  answered `ACCESS_DENIED`, the graphics device was lost, and the game froze and died about two
  minutes later. It now draws into a surface of its own that Windows composes over the game, so
  nothing of the game's is written and there is nothing left to be refused. It needs no setting
  and no restart of anything.
- The mod's own settings had nothing to do with it, and neither did the game's: Frame Generation
  reading **Off** in the graphics menu did not help, because the runtime loads whenever the PC can
  run it and keeps the buffers either way. Nobody who reported this had frame generation switched
  on.
- The overlay no longer submits anything on a command queue belonging to the game or to anything
  else in the process. It creates its own.

**Changed**

- ReShade and RenoDX no longer tint the minimap. The overlay is composed after the game's frame
  instead of being drawn into it, so colour grading, sharpening and LUTs apply to the game and
  leave the map alone.
- The mod installs three DirectX hooks instead of four.

**Notes**

- The one thing to watch on an older PC: where the display cannot compose an extra plane
  (multi-plane overlay support), the overlay can cost the game about one frame of latency. Where
  it can - which is most current hardware - it costs nothing measurable, and the log line
  `hardware composition (MPO)` says which case yours is.
- Thanks to the two players who reported the start-up crash and kept sending logs and test runs
  until it was found. One of them located the mechanism precisely - the queue the frames are
  presented on - and patched his own copy to prove it; that build should be replaced with this one.

## 1.0.2

**Added**

- `overlay_hooks = 0` (Advanced): the mod loads with no DirectX hooks and no overlay at all.
  The tracker, the found file and the log keep working.
- The log records the GPU and driver version, the display's colour space and peak brightness,
  the present mode, every graphics-related module in the game's process, and the size and link
  stamp of the game executable and of `UE4SS.dll`.
- The log names where the game's own logs and crash dumps are written.

**Fixed**

- A lost D3D12 device no longer hangs the game: the mod logs the reason DirectX gives, releases
  its objects once, and stays off for the rest of the session instead of rebuilding on the dead
  device.
- The overlay picks the command queue the game presents from by how much work each queue
  submits, instead of the first one it sees - with frame generation or another overlay in play,
  the wrong queue meant a minimap that tore, lagged a frame or never appeared.
- The overlay waits for the GPU to complete one empty test frame before it builds anything else.
- `wuchang_minimap_last_stage.txt` no longer names a start-up stage that was already passed, and
  says `mod off` while the mod is off.
- The freeze watchdogs fire on a start-up freeze and on a stopped frame counter; a long level
  load is no longer reported as a freeze.
- `game-state reader: the ProcessEvent pump is not firing` no longer prints on every machine a
  second after the game starts.
- Log lines from the render and game threads carry the time they were written.

**Notes**

- This release does not fix the start-up device loss reported on one machine - it is not
  reproducible on any machine here. It fails fast instead of freezing, and logs what is needed
  to find the cause.
- `overlay_enabled` is not a setting and has not been one since 1.0.0. If you were told to set
  it, that test did nothing; the line can be deleted.
- Not caused by this mod: clicking or dragging in UE4SS's console window puts it in selection
  mode, which freezes UE4SS mid-start-up and hangs the game before this mod loads. Press `Esc`
  in that window, or set `ConsoleEnabled = 0` in `ue4ss\UE4SS-settings.ini`. The tell is that
  `wuchang_minimap.log` was never created.

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
