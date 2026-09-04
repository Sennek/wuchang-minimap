# Nexus Mods page — WuchangMinimap 1.0.0

Copy-paste source for the Nexus mod page. The long description is BBCode; paste it via *Import description* or the BBCode editor.
Inline names use `[font=Courier New]` on purpose: Nexus renders `[code]` as a block, so
inline `[code]` breaks every sentence into separate boxes. `[code]` is kept only for the
two real blocks (the build string and the ini snippet).

**Category**: User Interface. **Tags**: UI, HUD, Map, Quality of Life, UE4SS.
**Requirements** (set these in the Nexus *Requirements* tab, not just in the text):
*UE4SS for Wuchang: Fallen Feathers* — Wuchang mod **384**, the **`experimental-latest`**
asset, which is UE4SS build **`v3.0.1-1111-g97b7e501`**.

> The build string belongs in the Requirements note as well as the description: any other
> UE4SS build fails to load with no overlay and no in-game message. It must be identical
> to the one in `BUILD_INFO.txt`, `README.md`, `THIRD_PARTY_NOTICES.md` and
> `tools/INSTALL_GUIDE.html`; `tools/check_release.ps1` fails the release if they drift.

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
[b]UE4SS for Wuchang: Fallen Feathers[/b] (Wuchang mod 384), installed into [font=Courier New]Project_Plague\Binaries\Win64\[/font].

[b]It has to be this exact UE4SS build:[/b]
[code]UE4SS v3.0.1-1111-g97b7e501   (the "experimental-latest" asset of mod 384)[/code]
This mod links straight to that DLL's exports, so [b]another UE4SS build will not work[/b], and the failure is silent: the game plays normally, no overlay appears, F2 does nothing, and [font=Courier New]ue4ss\UE4SS.log[/font] says [font=Courier New]Failed to load dll <...\Mods\WuchangMinimap\dlls\main.dll> for mod WuchangMinimap, error: The specified procedure could not be found.[/font] instead of [font=Courier New]WuchangMinimap v1.0.0 loaded[/font]. [font=Courier New]BUILD_INFO.txt[/font] in the download repeats the version.

[size=4][color=#ff6600]Then: HookInitGameState = 0[/color][/size]
Open [font=Courier New]Project_Plague\Binaries\Win64\ue4ss\UE4SS-settings.ini[/font] and set:
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
[*][b]F2[/b] — settings panel (Player / Advanced / Bindings tabs)
[*][b]M[/b] — full map: drag or WASD to pan, wheel to zoom, Q/E for the floor, Home to fit, right-click for a waypoint, ? for the rest
[*][b]TAB[/b] — x-ray highlight through walls, with names and distances. A toggle. [b]LB+RB[/b] on a controller.
[*][b]N[/b] — cycle the minimap zoom · [b]R[/b] — recentre the map · [b]F5[/b] — reload the config and data
[/list]
Everything is rebindable in F2 → Bindings.

[size=5]Known conflicts[/size]
None of these stops the mod working.
[list]
[*][b]ReShade / RenoDX[/b] (any [font=Courier New]dxgi.dll[/font] or [font=Courier New]d3d12.dll[/font] next to the game exe) — the mod draws [i]before[/i] ReShade's effects, so grading and sharpening are applied on top of the minimap and a strong LUT tints it. Cosmetic only. F6 is refused as a mod hotkey because it is RenoDX's default toggle.
[*][b]Another UE4SS C++ mod that also hooks Present[/b] — the one combination that can lose an overlay: whichever installs second usually wins, and the loser is invisible. Test them one at a time before reporting a blank screen.
[*][b]The Steam overlay[/b] — the first run of a new install creates and destroys a throwaway swapchain, which Steam's overlay follows, so its FPS counter can end up pointing at nothing. The addresses are cached afterwards. Shift+Tab still works.
[/list]

[size=5]Reporting a bug[/size]
Attach [font=Courier New]ue4ss\Mods\WuchangMinimap\wuchang_minimap.log[/font] — the mod's own log, rotated per launch. Its first six lines carry every version number a report needs. Add [font=Courier New]wuchang_minimap_last_stage.txt[/font] if the game crashed, [font=Courier New]wuchang_minimap_watchdog.txt[/font] if it froze, and your [font=Courier New]config_wuchang_minimap.txt[/font].

If the minimap simply is not on screen, press F2 and screenshot the orange [font=Courier New]hidden because:[/font] line at the top of the Player tab — it names the exact reason. If I ask you to reproduce something, set [font=Courier New]log_level = verbose[/font] in the config first (or pick it under Advanced → Diagnostics → Log detail).

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
- [ ] **4 — The F2 panel, Player tab**, on the category-filter block so the coloured
  chips and glyphs are visible, a couple of sections expanded.
- [ ] **5 — The collection statistics page**, from a save with shrines lit, chests opened
  and at least one boss beaten.
- [ ] **6 — The shrine list on the full map**, with the game's own shrine names and
  distances.
- [ ] *(optional)* **7 — The Bindings tab**, mid-rebind.
