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
A minimap, a full chapter map, a compass and a collection tracker for Wuchang: Fallen Feathers. The map background is not hand-drawn: it is built from the game's own navigation mesh, so what you see is ground you can stand on. Markers for chests, pickups, shrines, bosses, NPCs and notes carry the game's own English names. Keyboard, mouse and gamepad; English UI.

[size=5]Requirements[/size]
[b]UE4SS for Wuchang: Fallen Feathers[/b] (Wuchang mod 384), installed into [code]Project_Plague\Binaries\Win64\[/code].

[size=4][color=#ff6600]Then: HookInitGameState = 0[/color][/size]
Open [code]Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini[/code] and set:
[code]
[Hooks]
HookInitGameState = 0
[/code]
[b]Without this the game crashes about a third of a second into loading[/b], with or without this mod. It is a UE4SS-plus-Wuchang problem, not a bug here — if the game started crashing the moment you installed UE4SS, this is why.

[size=5]Install[/size]
[list=1]
[*]Close the game.
[*]Copy the [code]ue4ss\[/code] folder from the download into [code]<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\[/code] and [b]merge[/b] when Windows asks. Everything lives under [code]ue4ss\Mods\WuchangMinimap\[/code].
[*]Check [code][Hooks] HookInitGameState = 0[/code].
[*]Launch the game and load a save — the overlay is hidden on the main menu — then press F2.
[/list]
To uninstall, delete [code]ue4ss\Mods\WuchangMinimap\[/code]. That is the whole mod; it modifies no game or save data.

[size=5]Hotkeys[/size]
[list]
[*][b]F2[/b] — settings panel (Player / Advanced / Bindings tabs)
[*][b]M[/b] — full map: drag or WASD to pan, wheel to zoom, Q/E for the floor, Home to fit, right-click for a waypoint, ? for the rest
[*][b]LALT[/b] — x-ray highlight through walls, with names and distances. A toggle. [b]LB+RB[/b] on a controller.
[*][b]N[/b] — cycle the minimap zoom · [b]R[/b] — recentre the map · [b]F5[/b] — reload the config and data
[/list]
Everything is rebindable in F2 → Bindings.

[size=5]Reporting a bug[/size]
Attach [code]ue4ss\Mods\WuchangMinimap\wuchang_minimap.log[/code] — the mod's own log, rotated per launch, so it survives restarting the game. Its first six lines carry every version number a report needs. Add [code]wuchang_minimap_last_stage.txt[/code] if the game crashed, [code]wuchang_minimap_watchdog.txt[/code] if it froze, and your [code]config_wuchang_minimap.txt[/code].

If the minimap simply is not on screen, press F2 and screenshot the orange [code]hidden because:[/code] line at the top of the Player tab — it names the exact reason. If I ask you to reproduce something, set [code]log_level = verbose[/code] in the config first (or pick it under Advanced → Diagnostics → Log detail).

[size=5]Permissions and credits[/size]
[b]MIT licensed[/b] — fork it, reuse it, translate it; the licence just has to travel with it. Credit appreciated, not required. Third-party components: Dear ImGui (MIT), MinHook (BSD-2-Clause), fmt (MIT), RE-UE4SS (MIT); full notices ship in [code]THIRD_PARTY_NOTICES.md[/code].

The map backgrounds, the marker database and every name shown are extracted from the game's own data. Not affiliated with the developers or publisher of Wuchang: Fallen Feathers.
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
