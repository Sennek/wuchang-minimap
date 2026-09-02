# WuchangRecon — UE4SS Lua recon mod

A throwaway-but-careful reconnaissance mod for **Wuchang: Fallen Feathers** (UE 5.1.1). It exists to
answer step 1 of the minimap project: which classes, levels, widgets and navmesh objects the game
actually has at runtime. It is **read-only** — it never calls a setter, never spawns or destroys
anything, and every section runs inside `pcall` so a failing reflection call cannot crash the game.

## Install

```powershell
.\deploy-recon.ps1                # copy into the game's ue4ss\Mods\WuchangRecon\
.\deploy-recon.ps1 -Clean         # ... and wipe previous dumps first
.\deploy-recon.ps1 -Pull          # copy the game's out\ dumps back into this folder
```

Installs `Scripts\main.lua` + an empty `enabled.txt` (UE4SS's "load me without touching `mods.txt`"
opt-in) and creates `out\`.

## Hotkeys

Each key is registered **twice** — plain and with `CTRL` — because the engine and the game grab some
of the plain F-keys. `CTRL+O` stays UE4SS's own GUI toggle.

| Key | Also | Action | Writes |
|---|---|---|---|
| `F8` | `CTRL+F8` | world dump | `out\dump_<ts>_world.txt` |
| `F9` | `CTRL+F9` | UI dump | `out\dump_<ts>_ui.txt` |
| `F7` | `CTRL+F7` | tracker on/off (1 Hz) | `out\track.csv` |
| `F11` | `CTRL+F11` | navmesh probe grid | `out\navprobe_<ts>.csv` |

**F10 is deliberately unused**: `ConsoleEnablerMod` maps it to the game's console on this build.
Plain `F9` may also trigger the engine screenshot bind and plain `F11` the engine fullscreen toggle —
that is what the `CTRL` duplicates are for.

Every dump writes one confirmation line to `ue4ss\UE4SS.log`, e.g.

```
[Lua] [WuchangRecon] world dump written: ...\out\dump_20260902_101647_menu_world.txt (908 lines)
```

## What each dump contains

**F8 — world dump**
1. *player / controller / camera* — pawn full name, class chain to `AActor`, location, rotation
   (and the raw `K2_GetActorRotation` value plus the forward vector, since UE4SS's rotator handling is
   quirky), `RootComponent`, `Controller`, `PlayerCameraManager` + its camera location/rotation/FOV/
   view target, `AHUD`, and any `ExtendedStatComponent_C`.
2. *world / levels / streaming* — world + `PersistentLevel` names, every `StreamingLevels` entry with
   its load/visible state and `GetWorldAssetPackageFName()`, all loaded `ULevel`s, every World
   Partition / data-layer / level-streaming object, and every reflected `UWorld` property.
3. *actor census* — `FindAllOf("Actor")`, a class histogram sorted by count, then keyword-matched
   actors split into **bucket A** (the actor's *class* name matches — the real gameplay actors, listed
   with location, that class's own non-`AActor` properties, and any `bIsOpened`/`bCollected`/… flags
   that genuinely exist on it) and **bucket B** (only the object name matches — mostly `HISM_*`
   geometry, so it is counted and sampled instead of listed).
4. *loaded classes* — enumerated with `FindObjects`, filtered by keyword.
5. *navmesh* — every `RecastNavMesh` / `NavigationData` / `NavigationSystemV1` / nav volume / nav link,
   with **`GetAddress()`** (so the future C++ dumper can find the non-reflected `RecastNavMeshImpl`
   pointer) and a full property dump, plus a `ProjectPointToNavigation` probe at the player at three
   query extents.
6. *`DebugCommand_C`* — instance, every `UFunction` **with parameter names and types**, all its
   properties; then the player class's and player controller class's function names.

**F9 — UI dump** — every live `UUserWidget`: class, `IsInViewport()`, `IsVisible()`,
`GetVisibility()` (named, not numeric), parent widget class, full name, outer; a class histogram; and
one class-chain line per distinct widget class. Press it once in plain gameplay and once per open
menu, then diff — `IsInViewport()` is the useful discriminator (1 of 688 at the login screen).

**F7 — tracker** — appends `n,epoch,t,level,x,y,z,yaw,viewTargetClass` once a second. `LoopAsync`
drives it; the actual state read happens inside `ExecuteInGameThread`.

**F11 — navmesh probe** — 41×41 points at 100 uu spacing (±2000 uu) around the player through
`ProjectPointToNavigation` with extent (100,100,500), written as
`x,y,z,hit,projx,projy,projz` with the hit count in a header comment. A first low-res picture of the
walkable area, and a sanity check on the navmesh-as-map idea before writing any C++.

## Automatic menu-time dump

On load the mod polls for a valid `UWorld` (first attempt at 20 s, then every 5 s, giving up at 180 s)
and then runs the UI dump and the world dump once, as `dump_<ts>_menu_ui.txt` /
`dump_<ts>_menu_world.txt`. That gives a baseline from the main menu without anyone touching a key.

## Tuning

Everything adjustable lives in the `CFG` table at the top of `Scripts/main.lua`: per-class instance
caps, property caps, the probe grid size/step/extent, the tracker period, the auto-dump timings.
`KEYWORDS`, `FLAG_PROPS`, `NAV_CLASSES`, `WP_CLASSES` and `CLASS_KEYWORDS` are plain lists next to it.

## Output directory resolution

`main.lua` finds its own `out\` folder by (1) parsing `package.path` for its own
`…\WuchangRecon\Scripts\?.lua` entry, (2) falling back to the hard-coded game path, (3) trying paths
relative to the process CWD, (4) falling back to the mod root. The resolved directory is logged on
load, so a wrong guess is obvious.

## Testing without the game

The script is validated against a mock UE4SS environment (Lua 5.4 via `lupa`) before every deploy:
one mock supplies a small fake world and asserts the dumps come out right, a second "hostile" mock
makes *every* reflection call throw and asserts that no error escapes `pcall`. Those harnesses live in
the session scratchpad, not in the repo — re-create them if this mod ever needs another round of work.
