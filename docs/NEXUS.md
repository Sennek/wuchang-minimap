# Nexus Mods page — WuchangMinimap

Copy-paste source for the Nexus mod page. The long description is BBCode; paste it via *Import description* or the BBCode editor.
Inline names use `[font=Courier New]` on purpose: Nexus renders `[code]` as a block, so
inline `[code]` breaks every sentence into separate boxes. `[code]` is kept only for the
two real blocks (the build string and the ini snippet).

**Category**: User Interface. **Tags**: UI, HUD, Map, Quality of Life, UE4SS.
**Requirements** (set these in the Nexus *Requirements* tab, not just in the text):
*UE4SS for Wuchang: Fallen Feathers* — Wuchang mod **384**, file **1.79** (26 Feb 2026),
which is UE4SS build **`v3.0.1-934-gcac01ee2`**.

> The file version belongs in the Requirements note as well as the description: any other
> UE4SS build fails to load with no overlay and no in-game message. The git build string
> must be identical to the one in `BUILD_INFO.txt`, `README.md` and
> `THIRD_PARTY_NOTICES.md`; `tools/check_release.ps1` fails the release if they drift.

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
[list]
[*][b]Minimap[/b] and [b]compass strip[/b] in-world, [b]full chapter map[/b] on M
[*][b]Markers[/b] for chests, pickups, shrines, bosses, NPCs and notes, with the game's own names, all five chapters and the DLC
[*][b]Collection tracker[/b] per save slot, up to 16 waypoints, name search on the full map
[*][b]X-ray[/b] on TAB: loot through walls, tinted by item quality
[*]Everything configured from the [b]F2[/b] panel, saved by itself
[*]Keyboard, mouse and gamepad; English UI
[/list]

[size=5]Requirements[/size]
[b]UE4SS for Wuchang: Fallen Feathers[/b] (Wuchang mod 384), installed into [font=Courier New]Project_Plague\Binaries\Win64\[/font].

[b]It has to be this exact UE4SS build:[/b]
[code]UE4SS for WFF 1.79       mod 384, file version 1.79 (26 Feb 2026)
v3.0.1-934-gcac01ee2     the same build, as a git description[/code]
[font=Courier New]ue4ss\UE4SS.log[/font] opens with [font=Courier New]UE4SS - v3.0.1 Beta #0 - Git SHA #cac01ee2[/font] when it is the right one.
This mod links straight to that DLL's exports, so [b]another UE4SS build will not work[/b], and the failure is silent: the game plays normally, no overlay appears, F2 does nothing, and [font=Courier New]ue4ss\UE4SS.log[/font] says [font=Courier New]Failed to load dll <...\Mods\WuchangMinimap\dlls\main.dll> for mod WuchangMinimap, error: The specified procedure could not be found.[/font] instead of a [font=Courier New]WuchangMinimap vX.Y.Z loaded[/font] line naming the version you installed. [font=Courier New]BUILD_INFO.txt[/font] in the download repeats the version.

[size=4][color=#ff6600]Then: HookInitGameState = 0[/color][/size]
Open [font=Courier New]Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini[/font] and check (1.79 already ships it set):
[code]
[Hooks]
HookInitGameState = 0
[/code]
[b]Without this the game crashes about a third of a second into loading[/b], with or without this mod. It is a UE4SS-plus-Wuchang problem, not a bug here.

[size=5]Install[/size]
[list=1]
[*]Close the game.
[*]Copy the [font=Courier New]ue4ss\[/font] folder from the download into [font=Courier New]<Steam>\steamapps\common\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64\[/font] and [b]merge[/b] when Windows asks. Everything lives under [font=Courier New]ue4ss\Mods\WuchangMinimap\[/font].
[*]Check [font=Courier New][Hooks] HookInitGameState = 0[/font].
[*]Launch the game and load a save — the overlay is hidden on the main menu — then press F2.
[/list]
To uninstall, delete [font=Courier New]ue4ss\Mods\WuchangMinimap\[/font]. That is the whole mod; it modifies no game or save data.

[size=5]Hotkeys[/size]
[list]
[*][b]F2[/b] — settings panel (Overview / Categories / Map & tracker / Keys tabs). A change applies as you make it and is written to the config by itself. [b]Back+RS[/b] on a controller.
[*][b]M[/b] — full map: drag or WASD to pan, wheel to zoom, Q/E for the floor, Home to fit, right-click a marker to waypoint it (or the ground to drop one), F1 or H for the rest
[*][b]TAB[/b] — x-ray highlight through walls, with names and distances. A toggle. [b]LB+RB[/b] on a controller.
[*][b]N[/b] — cycle the minimap zoom · [b]R[/b] — recentre the map · [b]F5[/b] — reload the config and data
[/list]
Everything is rebindable in F2 → Keys.

[size=5]Known conflicts[/size]
None of these stops the mod working.
[list]
[*][b]ReShade / RenoDX[/b] (any [font=Courier New]dxgi.dll[/font] or [font=Courier New]d3d12.dll[/font] next to the game exe) — the mod draws [i]before[/i] ReShade's effects, so grading and sharpening are applied on top of the minimap and a strong LUT tints it. Cosmetic only. F6 is refused as a mod hotkey because it is RenoDX's default toggle.
[*][b]Another UE4SS C++ mod that also hooks Present[/b] — the one combination that can lose an overlay: whichever installs second usually wins, and the loser is invisible. Test them one at a time before reporting a blank screen.
[*][b]The Steam overlay[/b] — the first run of a new install creates and destroys a throwaway swapchain, which Steam's overlay follows, so its FPS counter can end up pointing at nothing. Shift+Tab still works.
[/list]

[size=5]Reporting a bug[/size]
Attach [font=Courier New]ue4ss\Mods\WuchangMinimap\wuchang_minimap.log[/font] — the mod's own log, rotated per launch. Its first six lines carry every version number a report needs. Add [font=Courier New]wuchang_minimap_last_stage.txt[/font] if the game crashed, [font=Courier New]wuchang_minimap_watchdog.txt[/font] if it froze, and your [font=Courier New]config_wuchang_minimap.txt[/font].

If the minimap simply is not on screen, send me [font=Courier New]wuchang_minimap.log[/font] from the mod's own folder — it names the exact reason it stayed hidden. If I ask you to reproduce something, add the line [font=Courier New]log_level = verbose[/font] to [font=Courier New]config_wuchang_minimap.txt[/font] first, press [b]F5[/b], and reproduce it.

[size=5]Permissions and credits[/size]
[b]MIT licensed[/b] — fork it, reuse it, translate it; the licence has to travel with it. Credit appreciated, not required. Third-party components: Dear ImGui (MIT), MinHook (BSD-2-Clause), fmt (MIT), RE-UE4SS (MIT); full notices ship in [font=Courier New]THIRD_PARTY_NOTICES.md[/font].

The map backgrounds, the marker database and every name shown are extracted from the game's own data. Not affiliated with the developers or publisher of Wuchang: Fallen Feathers.
```

---

## Screenshots to take

Image 1 is the mod page thumbnail and has to read at a small size. Native resolution, HUD
not cropped.

- [ ] **1 — The minimap in the world (thumbnail).** A lit area with real structure,
  minimap top-left at its default size, several marker categories at once (shrine, a
  chest or two, a pickup, an NPC), the compass strip in frame.
- [ ] **2 — The full map on `M`.** A chapter fitted to the screen (`Home`), the legend
  column visible, a waypoint placed, the floor offset showing. Chapter 1 or 3.
- [ ] **3 — The x-ray highlight.** In front of a wall with loot behind it, highlight on,
  several labelled markers through the wall, an edge arrow if one is showing.
- [ ] **4 — The F2 panel, Categories tab**, so the grid's coloured glyphs, the per-surface
  columns and the live found / known counts are visible.
- [ ] **5 — The collection statistics page**, from a save with shrines lit, chests opened
  and at least one boss beaten.
- [ ] **6 — The shrine list on the full map**, with the game's own shrine names and
  distances.
- [ ] *(optional)* **7 — The Keys tab**, mid-rebind.
