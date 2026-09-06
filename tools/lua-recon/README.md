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
| `F12` | `CTRL+F12` | pickup watch on/off (2 s diff) | `out\pickupwatch_<ts>.txt`, `out\pickupdeep_<ts>.txt` |
| `F5` | `CTRL+F5` | Enhanced Input / key bindings dump | `out\dump_<ts>_input.txt` |

`F5` is the only F-key left over: `F6` belongs to the C++ mod, `F9` and `F11` are engine binds, `F10`
is the console and `F12` is Steam's screenshot key.

**F10 is deliberately unused**: `ConsoleEnablerMod` maps it to the game's console on this build.
Plain `F9` may also trigger the engine screenshot bind and plain `F11` the engine fullscreen toggle —
that is what the `CTRL` duplicates are for. **F6 is off-limits too**: that is the `WuchangMinimap`
C++ mod's navmesh-dump hotkey.

Every dump writes one confirmation line to `ue4ss\UE4SS.log`, e.g.

```
[Lua] [WuchangRecon] world dump written: ...\out\dump_20260902_101647_menu_world.txt (908 lines)
```

## What each dump contains

**F8 — world dump**
1. *player / controller / camera* — pawn full name, class chain to `AActor`, location, rotation
   (and the raw `K2_GetActorRotation` value plus the forward vector, since UE4SS's rotator handling is
   quirky), `RootComponent`, `Controller`, `PlayerCameraManager` + its camera location/rotation/FOV,
   `PCM.ViewTarget.Target`, `PCM.CameraCachePrivate.POV` (location/rotation/FOV),
   **`PlayerController:GetViewTarget()`** — the *only* correct view-target source, with an explicit
   note when it is not the player pawn (cutscene / scripted camera) — `AcknowledgedPawn`, `AHUD`, and
   any `ExtendedStatComponent_C`.
2. *world / levels / streaming* — world + `PersistentLevel` names, every `StreamingLevels` entry with
   its load/visible state and `GetWorldAssetPackageFName()`, all loaded `ULevel`s, every World
   Partition / data-layer / level-streaming object, and every reflected `UWorld` property.
3. *actor census* — `FindAllOf("Actor")`, a class histogram sorted by count, then keyword-matched
   actors split into **bucket A** (the actor's *class* name matches — the real gameplay actors, listed
   with location, that class's own non-`AActor` properties, and any `bIsOpened`/`bCollected`/… flags
   that genuinely exist on it) and **bucket B** (only the object name matches — mostly `HISM_*`
   geometry, so it is counted and sampled instead of listed).
4. *markers* — for every class in `MARKER_CLASSES` (pickups `BP_PickupActor_C` / `BP_PickUpPT_C` /
   `BP_AutoPickUp_C` / `ItemCollectionBox_C`, chests `BP_treasurebox_C` / `BP_ItemRedBox_C`, shrine
   `BP_RebornFire_C`, fog gate `BP_Wumen_C`, doors) every instance sorted **nearest-player first**,
   with its location, its 3D distance to the player and the **VALUE** of every scalar own property
   (bool/int/float/enum/name/string). This is the section that answers "is this chest already
   opened?" — a list of property *names* cannot. Caps: 200 instances per class, 40 props each.
5. *function signatures* — every `UFunction` on the pawn, player-controller and camera-manager class
   chains whose name matches `Teleport|Cheat|Fly|Ghost|Walk|Cell|Load|Level|Stream|Debug|Camera|`
   `ViewTarget|Time|God|Unlock`, printed with its full parameter list (`Type Name`, last parameter is
   normally the return value). This is where `DebugSetCellLoad`'s signature comes from.
6. *loaded classes* — enumerated with `FindObjects`, filtered by keyword.
7. *navmesh* — every `RecastNavMesh` / `NavigationData` / `NavigationSystemV1` / nav volume / nav link,
   with **`GetAddress()`** (so the future C++ dumper can find the non-reflected `RecastNavMeshImpl`
   pointer) and a full property dump, plus a `ProjectPointToNavigation` probe at the player at three
   query extents.
8. *`DebugCommand_C`* — instance, every `UFunction` **with parameter names and types**, all its
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
walkable area, and a sanity check on the navmesh-as-map idea before writing any C++. Render one with
`python ..\navmesh\render.py --probe out\navprobe_*.csv --out out` — the same world→pixel mapping the
real navmesh render uses, so the two images can be overlaid to check alignment.

**F12 — pickup watch** — a toggle. On the first press it snapshots every `MARKER_CLASSES` actor
(class, full name, location, all scalar property values) and then re-snapshots every 2 s, logging
`DESTROYED` / `APPEARED` / `MOVED` / `CHANGED <prop>: old -> new` lines to `out\pickupwatch_<ts>.txt`
*and* to `UE4SS.log`. It exists to settle one question: when the player collects a pickup, is the
actor destroyed or does it survive with a flag flipped? Auto-marking "found" items depends on the
answer. Procedure: stand next to a pickup, press `F12`, collect it, press `F12` again.

A pickup-family actor (`BP_DropItem_C` and friends) additionally gets a **deep dump** into
`out\pickupdeep_<ts>.txt` — every reflected property, TArrays expanded element by element, structs
and object refs walked — which is how an enemy drop's item identity is captured before the player
collects it. It runs one watch tick (~2 s) after the actor is first seen, because the actor is still
being constructed on the tick it appears. Reading a half-built actor's memory can fault inside the
engine, and a native access violation is not a Lua error — `pcall` never sees it and the game dies
on the spot — so the dump writes the whole property **list** before it reads any value, brackets
every read with `-> read <name>` / `ok <name>`, reads scalars before object refs before
arrays/structs, and flushes every line to disk on its own. If the game dies mid-dump, the last line
of the file names the property that killed it. `CFG.DEEP_VALUES = false` turns the value phase off
entirely and leaves the list; `CFG.WATCH_DEEP_DELAY_TICKS` is the delay. The `F8` world dump does
the same for the pickups it lists, into `out\deepdump_<ts>.txt`.

**F5 — input dump** — everything needed to read the player's *actual* key bindings at runtime. Press
it **in gameplay**, not at the title screen: the player controller and its `PlayerInput` only exist
once a save is loaded. Press it again after remapping a key in the game's own options screen — the
diff between the two dumps is what proves where a remap is stored.

1. *chain* — `PlayerController` -> `Player` (`ULocalPlayer`) -> `PlayerInput`
   (`UEnhancedPlayerInput`) -> `InputComponent`, each with its class chain, plus every reflected
   property of the local player and of the player input.
2. *applied mapping contexts* — `PlayerInput.AppliedInputContexts`, the
   `TMap<UInputMappingContext*, int32>` the subsystem fills in, listed as context + **priority**,
   each followed by every entry of its `Mappings` array: action, key name, `bIsPlayerMappable`, the
   mappable name out of `PlayerMappableOptions` / `PlayerMappableKeySettings`, and the
   trigger/modifier counts. Then the flattened `PlayerInput.EnhancedActionMappings` — the post-remap
   list the engine actually consumes, and where a remapped key shows up when the asset context still
   holds the default — then the legacy `ActionMappings` / `AxisMappings`, then every loaded
   `UInputMappingContext` whether applied or not.
3. *object census* — instance counts and full names for every Enhanced Input class
   (`UEnhancedInputLocalPlayerSubsystem`, `UEnhancedInputUserSettings`,
   `UEnhancedPlayerMappableKeyProfile`, `UPlayerMappableInputConfig`, contexts, actions, modifiers,
   triggers, `ULocalPlayer`, …), with a full property dump of the first instance of each class that
   can hold bindings.
4. *remap storage candidates* — a sweep of every loaded `UClass` whose name contains
   `input`/`keybind`/`keymap`/`binding`/`remap`/`mappable`/`control`/`gamepad`, plus a property dump
   of an instance of the narrow matches. A game that keeps remaps in its own settings or save object
   instead of the engine's shows up here.
5. *reflected layouts* — the property list of `FEnhancedActionKeyMapping`, `FKey`,
   `FPlayerMappableKeyOptions`, `FPlayerKeyMapping` and the Enhanced Input `UClass`es, read off the
   `UScriptStruct` / `UClass` itself. These are the exact names the C++ reader has to walk by; a path
   printed `<not loaded>` does not exist under that name on this build.

Every field is probed by name and reported present or absent rather than assumed: Enhanced Input
renamed several of them between engine versions, and a nested struct reads back opaque whether it is
missing or merely unformattable.

### Getting a key-bindings dump

```powershell
.\deploy-recon.ps1                 # install the updated main.lua
# start the game (or, with it already running, use UE4SS' own GUI - CTRL+O -
# and its "Restart All Mods" button, which reloads Lua mods in place)
# load a save, so a PlayerController and its PlayerInput exist
#   -> press F5 (or CTRL+F5)
# remap something in the game's options screen, then press F5 again
.\deploy-recon.ps1 -Pull           # copy the dumps back into out\
```

`ue4ss\UE4SS.log` gets one confirmation line per press:
`[Lua] [WuchangRecon] input dump written: ...\out\dump_<ts>_input.txt (<n> lines)`.

## Automatic menu-time dump

On load the mod polls for a valid `UWorld` (first attempt at 20 s, then every 5 s, giving up at 180 s)
and then runs the UI dump and the world dump once, as `dump_<ts>_menu_ui.txt` /
`dump_<ts>_menu_world.txt`. That gives a baseline from the main menu without anyone touching a key.

## Tuning

Everything adjustable lives in the `CFG` table at the top of `Scripts/main.lua`: per-class instance
caps, property caps, the probe grid size/step/extent, the tracker period, the pickup-watch period and
actor cap, the auto-dump timings. `KEYWORDS`, `MARKER_CLASSES`, `SCALAR_PROP_TYPES`, `FN_PATTERNS`,
`FLAG_PROPS`, `NAV_CLASSES`, `WP_CLASSES` and `CLASS_KEYWORDS` are plain lists next to it.

`KEYWORDS` **must** keep the game's own Pinyin vocabulary (`wumen`, `rebornfire`, `digong`, `dici`,
`zhuanjing`, `qicaishi`, `plume`) — without it whole marker categories vanish with no warning, which
is exactly what happened to `BP_Wumen_C` in the first recon pass.

## Output directory resolution

`main.lua` finds its own `out\` folder by (1) parsing `package.path` for its own
`…\WuchangRecon\Scripts\?.lua` entry, (2) falling back to the hard-coded game path, (3) trying paths
relative to the process CWD, (4) falling back to the mod root. The resolved directory is logged on
load, so a wrong guess is obvious.

## Testing without the game

The harness now lives in the repo — run it before every deploy:

```powershell
pip install lupa           # once; it embeds a real Lua interpreter
python mock\run.py         # both modes
python mock\run.py --mode friendly --keep    # leave the temp dir to inspect the dumps
```

`mock\harness.lua` fakes `FindAllOf` / `FindFirstOf` / `FindObjects` / `StaticFindObject` /
`RegisterKeyBind` / `LoopAsync` / `ExecuteInGameThread` / `Key` / `ModifierKey` / `UEHelpers` plus a
small object model with classes, super-struct chains, typed properties *with values* and `UFunction`s
with parameters. For the input dump it also fakes a whole Enhanced Input stack: `TArray` and `TMap`
values that answer `GetArrayNum` / `ForEach` and hand each element over inside a `:get()` handle,
nested struct values, two mapping contexts at different priorities, a player input whose flattened
`EnhancedActionMappings` has one action remapped off its asset key, a subsystem, a user-settings
object and a game-side `BP_KeyBindSettings_C`.
`mock\run.py` lays the mod out the way UE4SS does (so `resolve_out_dir()` finds a temp `out\`) and
runs it twice:

* **friendly** — everything answers; afterwards the world dump is asserted to contain every section
  and the specific strings that prove the fixes landed (`PC:GetViewTarget()`, `POV location`,
  `BP_Wumen_C`, `Used=false`, `dist `, `DebugSetCellLoad(`), and the pickup watch is asserted to have
  logged a baseline plus a `DESTROYED` and a `CHANGED` line after the harness fakes a collection.
  The input dump is asserted to carry both context priorities, key names read out of nested `FKey`
  structs, the mappable name, the remapped key that exists only in `EnhancedActionMappings`, and the
  reflected `FEnhancedActionKeyMapping` layout.
* **hostile** — *every* reflection call raises. Nothing may escape `pcall`, the keybinds must still
  register and the mod must still write its files.

The harness deliberately returns an `FRotator` with a lowercase `roll` and returns opaque userdata for
properties that do not exist, because those are the two UE4SS quirks that broke earlier versions.
