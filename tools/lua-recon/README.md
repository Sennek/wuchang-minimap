# WuchangRecon — UE4SS Lua recon mod

A reconnaissance mod for **Wuchang: Fallen Feathers** (UE 5.1.1): it reports which classes, levels,
widgets, bindings and navmesh objects the game has at runtime. It is **read-only** — never calls a
setter, never spawns or destroys anything — and every section runs inside `pcall`, so a failing
reflection call cannot crash the game.

Its `WuchangRecon/out/` dumps are committed to the repository and are the evidence
`tools/markers/verify_markers.py` scores extracted marker coordinates against. They cannot be
re-taken without the game.

## Install

```powershell
.\deploy-recon.ps1                # copy into the game's ue4ss\Mods\WuchangRecon\
.\deploy-recon.ps1 -Clean         # ... and wipe previous dumps first
.\deploy-recon.ps1 -Pull          # copy the game's out\ dumps back into this folder
```

Installs `Scripts\main.lua` + an empty `enabled.txt` (UE4SS's "load me without touching
`mods.txt`" opt-in) and creates `out\`.

## Hotkeys

Each key is registered **twice** — plain and with `CTRL` — because the engine and the game grab some
of the plain F-keys. `CTRL+O` stays UE4SS's own GUI toggle.

| Key | Also | Action | Writes |
|---|---|---|---|
| `F8` | `CTRL+F8` | world dump | `out\dump_<ts>_world.txt`, `out\deepdump_<ts>.txt` |
| `F9` | `CTRL+F9` | UI dump | `out\dump_<ts>_ui.txt` |
| `F7` | `CTRL+F7` | tracker on/off (1 Hz) | `out\track.csv` |
| `F11` | `CTRL+F11` | navmesh probe grid | `out\navprobe_<ts>.csv` |
| `F12` | `CTRL+F12` | pickup watch on/off (2 s diff) | `out\pickupwatch_<ts>.txt`, `out\pickupdeep_<ts>.txt` |
| `F5` | `CTRL+F5` | Enhanced Input / key bindings dump | `out\dump_<ts>_input.txt` |

`F5` is the only F-key left over. **`F6` is off-limits** — it is the `WuchangMinimap` C++ mod's
navmesh-dump hotkey — and **`F10` is deliberately unused**, because `ConsoleEnablerMod` maps it to
the game's console on this build. Plain `F9` may also trigger the engine screenshot bind and plain
`F11` the engine fullscreen toggle; that is what the `CTRL` duplicates are for.

Every dump writes one confirmation line to `ue4ss\UE4SS.log`:

```
[Lua] [WuchangRecon] world dump written: ...\out\dump_20260902_101647_menu_world.txt (908 lines)
```

## What each dump contains

**F8 — world dump**, eight sections: the player / controller / camera chain (including
`PlayerController:GetViewTarget()`, the only correct view-target source, and
`PCM.CameraCachePrivate.POV`); the world, its levels and every streaming / data-layer object; an
actor census as a class histogram plus keyword-matched actors, split into those whose *class* name
matches (listed with location and own properties) and those where only the object name does
(counted and sampled); every instance of every `MARKER_CLASSES` class, nearest-player first, with
the **value** of every scalar own property — this is the section that answers "is this chest already
opened?", which a list of property *names* cannot; the full parameter lists of pawn / controller /
camera `UFunction`s matching `FN_PATTERNS`; loaded classes by keyword; every navmesh object with its
`GetAddress()` and a `ProjectPointToNavigation` probe; and `DebugCommand_C`.

**F9 — UI dump** — every live `UUserWidget`: class, `IsInViewport()`, `IsVisible()`,
`GetVisibility()` (named, not numeric), parent, full name, outer; a class histogram; one class-chain
line per distinct widget class. Press it once in plain gameplay and once per open menu, then diff —
`IsInViewport()` is the useful discriminator.

**F7 — tracker** — appends `n,epoch,t,level,x,y,z,yaw,viewTargetClass` once a second. `LoopAsync`
drives it; the state read happens inside `ExecuteInGameThread`.

**F11 — navmesh probe** — a 41×41 grid at 100 uu spacing around the player through
`ProjectPointToNavigation`, written as `x,y,z,hit,projx,projy,projz`. Render one with
`python ..\navmesh\render.py --probe out\navprobe_*.csv --out out`, which uses the same world→pixel
mapping as the real navmesh render, so the two images can be overlaid to check alignment.

**F12 — pickup watch** — a toggle. It snapshots every `MARKER_CLASSES` actor on the first press and
re-snapshots every 2 s, logging `DESTROYED` / `APPEARED` / `MOVED` / `CHANGED <prop>: old -> new`.
Procedure: stand next to a pickup, press `F12`, collect it, press `F12` again.

A pickup-family actor also gets a **deep dump** — every reflected property, TArrays expanded element
by element, structs and object refs walked — which is how an enemy drop's item identity is captured
before the player collects it. It runs one watch tick after the actor is first seen, because the
actor is still being constructed on the tick it appears. **Reading a half-built actor's memory can
fault inside the engine, and a native access violation is not a Lua error** — `pcall` never sees it
and the game dies on the spot. So the dump writes the whole property *list* before it reads any
value, brackets every read with `-> read <name>` / `ok <name>`, reads scalars before object refs
before arrays and structs, and flushes every line to disk on its own: if the game dies mid-dump, the
last line of the file names the property that killed it. `CFG.DEEP_VALUES = false` turns the value
phase off and leaves the list; `CFG.WATCH_DEEP_DELAY_TICKS` is the delay.

**F5 — input dump** — everything needed to read the player's *actual* key bindings at runtime.
Press it **in gameplay**, not at the title screen: the player controller and its `PlayerInput` only
exist once a save is loaded. Press it again after remapping a key in the game's own options screen —
the diff between the two dumps is what proves where a remap is stored. It reports the
`PlayerController -> ULocalPlayer -> PlayerInput -> InputComponent` chain, the applied mapping
contexts with their priorities and every entry of their `Mappings` arrays, the flattened
`EnhancedActionMappings` the engine actually consumes, the legacy `ActionMappings` / `AxisMappings`,
a census of every Enhanced Input class, a sweep of every loaded `UClass` that might hold remaps
under its own name, and the reflected property layouts of `FEnhancedActionKeyMapping`, `FKey`,
`FPlayerMappableKeyOptions` and `FPlayerKeyMapping`.

**Every field is probed by name and reported present or absent rather than assumed**: Enhanced Input
renamed several of them between engine versions, and a nested struct reads back opaque whether it is
missing or merely unformattable. A path printed `<not loaded>` does not exist under that name on
this build.

### Getting a key-bindings dump

```powershell
.\deploy-recon.ps1                 # install the updated main.lua
# start the game (or, with it running, use UE4SS' own GUI - CTRL+O - and
# "Restart All Mods", which reloads Lua mods in place)
# load a save, so a PlayerController and its PlayerInput exist
#   -> press F5 (or CTRL+F5)
# remap something in the game's options screen, then press F5 again
.\deploy-recon.ps1 -Pull           # copy the dumps back into out\
```

## Automatic menu-time dump

On load the mod polls for a valid `UWorld` (first attempt at 20 s, then every 5 s, giving up at
180 s) and then runs the UI dump and the world dump once, as `dump_<ts>_menu_ui.txt` /
`dump_<ts>_menu_world.txt`, so there is a main-menu baseline without anyone touching a key.

## Tuning

Everything adjustable is in the `CFG` table at the top of `Scripts/main.lua`: per-class instance
caps, property caps, the probe grid size / step / extent, the tracker period, the pickup-watch
period and actor cap, the auto-dump timings. `KEYWORDS`, `MARKER_CLASSES`, `SCALAR_PROP_TYPES`,
`FN_PATTERNS`, `FLAG_PROPS`, `NAV_CLASSES`, `WP_CLASSES` and `CLASS_KEYWORDS` are plain lists next
to it.

`KEYWORDS` **must** keep the game's own Pinyin vocabulary (`wumen`, `rebornfire`, `digong`, `dici`,
`zhuanjing`, `qicaishi`, `plume`). Without it whole marker categories vanish from the dumps with no
warning at all.

## Output directory resolution

`main.lua` finds its own `out\` folder by (1) parsing `package.path` for its own
`…\WuchangRecon\Scripts\?.lua` entry, (2) falling back to the hard-coded game path, (3) trying paths
relative to the process CWD, (4) falling back to the mod root. The resolved directory is logged on
load, so a wrong guess is obvious.

## Testing without the game

Run the harness before every deploy:

```powershell
pip install lupa           # once; it embeds a real Lua interpreter
python mock\run.py         # both modes
python mock\run.py --mode friendly --keep    # leave the temp dir to inspect the dumps
```

`mock\harness.lua` fakes `FindAllOf` / `FindFirstOf` / `FindObjects` / `StaticFindObject` /
`RegisterKeyBind` / `LoopAsync` / `ExecuteInGameThread` / `Key` / `ModifierKey` / `UEHelpers` plus a
small object model with classes, super-struct chains, typed properties *with values* and
`UFunction`s with parameters. For the input dump it also fakes a whole Enhanced Input stack:
`TArray` and `TMap` values that answer `GetArrayNum` / `ForEach` and hand each element over inside a
`:get()` handle, nested struct values, two mapping contexts at different priorities, a player input
whose flattened `EnhancedActionMappings` has one action remapped off its asset key, a subsystem, a
user-settings object and a game-side `BP_KeyBindSettings_C`.

`mock\run.py` lays the mod out the way UE4SS does, so `resolve_out_dir()` finds a temp `out\`, and
runs it twice:

* **friendly** — everything answers, and the dumps are asserted to contain every section and the
  specific strings that only a correct walk produces (`PC:GetViewTarget()`, `POV location`,
  `BP_Wumen_C`, `Used=false`, `dist `, `DebugSetCellLoad(`), the pickup watch to have logged a
  baseline plus a `DESTROYED` and a `CHANGED` line after the harness fakes a collection, and the
  input dump to carry both context priorities, key names read out of nested `FKey` structs, the
  mappable name, the remapped key that exists only in `EnhancedActionMappings`, and the reflected
  `FEnhancedActionKeyMapping` layout.
* **hostile** — *every* reflection call raises. Nothing may escape `pcall`, the keybinds must still
  register and the mod must still write its files.

The harness deliberately returns an `FRotator` with a lowercase `roll` and returns opaque userdata
for properties that do not exist, because those are the two UE4SS quirks a naive walk gets wrong.
