# Nexus Mods page — WuchangMinimap 1.0.0

Copy-paste source for the Nexus mod page. The long description is BBCode; paste it into
the description box with the BBCode editor, not the rich-text one.

**Category**: User Interface. **Tags**: UI, HUD, Map, Quality of Life, UE4SS.
**Requirements** (set these in the Nexus *Requirements* tab, not just in the text):
*UE4SS for Wuchang: Fallen Feathers* — Wuchang mod **384**.

---

## Short description

246 characters, including spaces (Nexus allows 250):

```
A minimap, a full chapter map and a compass, built from the game's own navmesh. Markers for chests, pickups, shrines, bosses, NPCs and notes with the game's own names, a collection tracker per save, and an x-ray key that shows loot through walls.
```

---

## Full description (BBCode)

```bbcode
[size=5]What it is[/size]
An interactive minimap, full map, compass and collection tracker for Wuchang: Fallen Feathers. The map background is not hand-drawn: it is built from the game's own navigation mesh, so what you see is ground you can actually stand on. UI language is English.

[size=5]Features[/size]
[list]
[*][b]A height-sliced minimap.[/b] Only the storey you are standing on is drawn solid; the ones above and below are dimmed, so a temple stops being a silhouette. Round or square, either corner, north-up or rotating with you.
[*][b]A full chapter map on M[/b] — pan, zoom, step the height slice a floor at a time, fit the whole chapter, drop a waypoint, tick a marker off by hand, copy the map to your clipboard. Mouse, keyboard and gamepad.
[*][b]Markers in fourteen categories[/b], each with its own shape and colour: shrines, chests, pickups, bosses, elites, enemies, NPCs, notes (the readable inscriptions), doors, ladders, lifts, fog gates, hidden things and other. They come from a database extracted from the game's own levels, merged with the live state of everything currently streamed in.
[*][b]Real names, not internal ids.[/b] Shrines read "Mercury Workshop" and "Pavilion of Knowledge", NPCs "He Youzai" and "Wu Gang", bosses "Sovereign - Zhang Xianzhong", and every pickup shows the item's own name.
[*][b]A collection tracker[/b] — chests opened, pickups taken, shrines lit, NPCs and notes met, bosses beaten — with a statistics page per category and per chapter. One file per save game, so a second character does not start with the first one's collection ticked off. It also works out what you collected [i]before[/i] installing the mod.
[*][b]A compass strip[/b] with bearing pips, the distance in metres under each, and an up or down arrow for anything well above or below you.
[*][b]An x-ray highlight[/b] on LALT (or LB+RB): nearby chests, pickups, shrines, bosses, NPCs and notes drawn through walls with their names and distances, and an arrow on the screen edge for anything behind you. A toggle by default; hold is one radio button away.
[*][b]A shrine list[/b] on the full map: every shrine of the chapter by distance, with its name and whether you have lit it. Click for a waypoint.
[*][b]Everything is configurable[/b] — one documented plain-text file, an in-game settings panel on F2 with Player / Advanced / Bindings tabs, three one-click presets, a colour-blind palette, a UI scale that gets 4K right, a log detail level for when something needs reporting, and F5 to reload without restarting the game.
[*][b]Every hotkey is rebindable[/b] in the panel by pressing the new key.
[/list]

[size=5]Requirements[/size]
[list]
[*]Wuchang: Fallen Feathers on Windows x64.
[*][b]UE4SS built for this game[/b] — [i]UE4SS for Wuchang: Fallen Feathers[/i] (Wuchang mod 384). Install it into [code]Project_Plague\Binaries\Win64\[/code] first.
[/list]

[size=4][color=#ff6600]Mandatory: HookInitGameState = 0[/color][/size]
Open [code]Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini[/code], find the [code][Hooks][/code] section and set:
[code]
[Hooks]
HookInitGameState = 0
[/code]
[b]Without this the game crashes about a third of a second into loading[/b], with or without this mod. UE4SS's hook on [code]AGameModeBase::InitGameState[/code] breaks Wuchang's custom game mode and a blueprint then reads a null game state. Every other hook can stay on. This is a UE4SS-plus-Wuchang problem, not a bug in this mod — if the game crashed the moment you installed UE4SS, this is why.

[size=5]Install[/size]
[list=1]
[*]Close the game.
[*]The download contains a single [code]ue4ss\[/code] folder. Copy it into [code]<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\[/code] and [b]merge[/b] when Windows asks. Nothing of UE4SS's own is overwritten — everything lives under [code]ue4ss\Mods\WuchangMinimap\[/code].
[*]Set [code][Hooks] HookInitGameState = 0[/code] as above.
[*]Launch the game and load a save. The overlay is deliberately hidden on the main menu.
[/list]
There is no [code]mods.txt[/code] to edit: the empty [code]enabled.txt[/code] in the mod folder is the opt-in. Delete it to stop UE4SS loading the mod.

[b]Uninstall:[/b] delete [code]ue4ss\Mods\WuchangMinimap\[/code]. That is the whole mod. It reads the game's memory and its packaged data; it touches no save file and modifies no game data, so removing it cannot affect a playthrough.

[size=5]Hotkeys[/size]
Shipped defaults. All of them are rebindable in the F2 panel's [b]Bindings[/b] tab.
[list]
[*][b]F2[/b] — settings panel
[*][b]M[/b] — full map
[*][b]LALT[/b] — x-ray highlight (a toggle; press again to turn it off). [b]LB+RB[/b] on a controller.
[*][b]N[/b] — cycle the minimap zoom
[*][b]R[/b] — recentre the full map
[*][b]F5[/b] — reload the config, the maps and the markers
[/list]
On the full map: drag or WASD to pan, wheel to zoom, Q/E to step the height slice, Home to fit the chapter, right-click for a waypoint, left-click a marker to tick it off, C to copy the map to the clipboard, ? for the full list, Esc to close. Gamepad throughout.

[size=5]Known issues[/size]
[list]
[*][b]The DLC has no map[/b] — the game ships no navigation data for it. Markers still work, but its 7 shrines keep their internal ids: the DLC's fire points have no row in the game's name table.
[*][b]A boss read from your save may be one you fought rather than one you beat.[/b] For a boss killed before you installed the mod there is nothing left in the world to read, so the mod uses its arena's respawn point, which your save remembers unlocking — and it is not certain the game unlocks that on the kill rather than on the first attempt. [code]boss_defeat_from_save = 0[/code] undoes it completely.
[*][b]Some patches of ground are drawn that you cannot walk to.[/b] The navmesh has no notion of "reachable" and a strict filter would delete real floors, so a few dozen detached patches per chapter survive. The palace lake area in Chapter 1 is the most visible case.
[*][b]ReShade / RenoDX draw over the overlay[/b], so a heavy preset tints it. Cosmetic.
[*][b]Enemy markers are off by default[/b] — the sweep refreshes about once a second, so they lag behind anything moving.
[/list]

[size=5]Reporting a bug[/size]
Please attach these, from [code]ue4ss\Mods\WuchangMinimap\[/code]:
[list]
[*][code]wuchang_minimap.log[/code] — the mod's own log, and [code]wuchang_minimap.log.1[/code] if the problem was in the previous session. This is the important one: UE4SS empties [i]its[/i] log every launch, so restarting the game to try something else destroys it; this file is rotated instead.
[*][code]wuchang_minimap_watchdog.txt[/code] — if the game froze. It names the thread that stopped.
[*][code]wuchang_minimap_last_stage.txt[/code] — if the game crashed. It names the stage the overlay was in, and survives when a log does not.
[*]Your [code]config_wuchang_minimap.txt[/code] — most "it does not work" reports turn out to be a setting, so send it whether or not you think you changed anything.
[*]Or, if you would rather paste than attach: the [b]first six lines of the log[/b] are a startup header carrying the mod version, the game executable's build, UE4SS's build and the Windows build.
[*]For a crash, [code]%LOCALAPPDATA%\Project_Plague\Saved\Crashes\CrashContext.runtime-xml[/code]. If its [code]<CrashType>[/code] says [code]GPUCrash[/code], it is a driver or ReShade/DLSS problem rather than this mod.
[/list]
If the minimap simply is not on screen, open F2 and screenshot the orange [code]hidden because:[/code] line at the top of the Player tab — it names the exact condition holding it back, and is worth more than any description. The log only repeats it at [code]verbose[/code].

[b]If I ask you to reproduce something:[/b] set [code]log_level = verbose[/code] in the config, or pick it under [i]Advanced → Diagnostics → Log detail[/i] in the F2 panel (it takes effect at once, no restart), reproduce the problem, then send [code]wuchang_minimap.log[/code]. The default [code]normal[/code] is quiet on purpose — a long session is a few hundred lines — and nothing is missing from it that is not one config value away.

[size=5]Permissions and credits[/size]
Source and full documentation: see the Docs tab / the repository link. The mod is [b]MIT licensed[/b] — do what you like with it, including forking or reusing the map pipeline, as long as the licence travels with it. Credit is appreciated, not required. If you want to publish a translation or a variant, go ahead; a link back is enough.

Third-party components, all under permissive licences: [b]Dear ImGui[/b] (MIT), [b]MinHook[/b] (BSD-2-Clause), [b]fmt[/b] (MIT) and [b]RE-UE4SS[/b] (MIT). The full notices ship in [code]THIRD_PARTY_NOTICES.md[/code].

The map backgrounds, the marker database and every name shown are extracted from the game's own data and are the property of the game's developers. This mod is not affiliated with the developers or the publisher of Wuchang: Fallen Feathers.
```

---

## Screenshots to take

The first image is the mod page's thumbnail, so it has to read at a small size. Take
everything at the game's native resolution and do not crop the HUD away.

- [ ] **1 — The minimap in the world (thumbnail).** Daylight or a lit area, standing
  somewhere with real structure, minimap top-left at its default size, several marker
  categories visible at once (a shrine, a chest or two, a pickup, an NPC). The compass
  strip in the frame at the top. This is the one people judge the mod by.
- [ ] **2 — The full map on `M`.** A chapter fitted to the screen (`Home`) so the shape of
  the level reads, the legend column on the right visible, a waypoint placed, the floor
  offset showing. Chapter 1 or 3 — they have the most recognisable silhouettes.
- [ ] **3 — The x-ray highlight.** Standing in front of a wall with loot behind it,
  highlight on, several labelled markers through the wall with names and distances, an
  edge arrow if one is showing. This is the feature that needs a picture to be understood.
- [ ] **4 — The F2 panel, Player tab.** Open on the category-filter block so the coloured
  chips with their glyphs are visible, with a couple of sections expanded — it shows at a
  glance that the mod is configurable in-game rather than by file editing.
- [ ] **5 — The collection statistics page.** The per-chapter matrix with real numbers in
  it, from a save with a decent amount of progress. Empty columns look broken; make sure
  the save has shrines lit, chests opened and at least one boss beaten.
- [ ] **6 — The shrine list on the full map**, showing the game's own shrine names and
  distances next to the map. This is the concrete proof of "real names, not internal ids",
  which is otherwise a claim.
- [ ] *(optional)* **7 — The Bindings tab**, mid-rebind, for the "every hotkey is
  rebindable" claim.
