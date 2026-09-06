#!/usr/bin/env python3
"""
Exercise WuchangRecon/Scripts/main.lua without launching the game.

Copies the mod into a throwaway directory laid out the way UE4SS lays it out
(<mod>/Scripts/main.lua plus <mod>/out), points package.path at it so the mod's own
resolve_out_dir() finds the temp out\\ folder, then runs mock/harness.lua twice:

  * friendly  - the fake UE4SS API answers everything; afterwards the dumps that were
                produced are checked for the sections we care about
  * hostile   - every reflection call raises; nothing may escape pcall, the mod must
                still register its keybinds and still write a file

    python run.py                 # both modes
    python run.py --keep          # leave the temp dir for inspection
    python run.py --mode hostile  # one mode only

Needs lupa (pip install lupa) - it embeds a real Lua interpreter.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

try:
    from lupa import LuaRuntime
except ImportError:  # pragma: no cover
    sys.exit("This harness needs lupa:  pip install lupa")

HERE = Path(__file__).resolve().parent
MOD_SRC = HERE.parent / "WuchangRecon"

# Section headers the friendly run must produce, and what each one proves.
EXPECTED_WORLD_SECTIONS = [
    "PLAYER / CONTROLLER / CAMERA",
    "WORLD / LEVELS / STREAMING",
    "ACTOR CENSUS",
    "MARKER CLASSES - VALUES AND DISTANCE TO PLAYER",
    "FUNCTION SIGNATURES",
    "NAVMESH",
]
EXPECTED_WORLD_STRINGS = [
    "PC:GetViewTarget()",          # the fixed view-target read
    "PCM.ViewTarget.Target",
    "POV location",
    "BP_Wumen_C",                  # the Pinyin keyword addition
    "Used=false",                  # property VALUES, not just names
    "dist ",                       # distance to player
    "DebugSetCellLoad(",           # a function signature with parameters
    "ClientCheatFly(",
]

# The input dump (F5). Enhanced Input hands most of these back as opaque userdata,
# so these strings are the proof that the nested structs were read, not skipped.
EXPECTED_INPUT_SECTIONS = [
    "INPUT: CONTROLLER -> LOCAL PLAYER -> PLAYER INPUT",
    "INPUT: APPLIED MAPPING CONTEXTS AND THEIR MAPPINGS",
    "INPUT: SUBSYSTEM / USER SETTINGS / OBJECT CENSUS",
    "INPUT: REMAP STORAGE CANDIDATES (class-name sweep)",
    "INPUT: REFLECTED STRUCT / CLASS LAYOUTS",
]
EXPECTED_INPUT_STRINGS = [
    "PC.PlayerInput       : EnhancedPlayerInput",
    "priority=0",                     # the TMap value came through
    "priority=10",
    "key=E",                          # FKey.KeyName out of a nested struct
    "key=SpaceBar",
    "name=Interact",                  # PlayerMappableOptions.Name
    "fields present: Action, Key",    # the field probe, not the formatter
    "action=IA_Interact",
    "key=G",                          # the remapped key, only in EnhancedActionMappings
    "EnhancedInputLocalPlayerSubsystem      : 1 instance(s)",
    "BP_KeyBindSettings_C",           # the game-side remap-storage candidate
    "/Script/EnhancedInput.EnhancedActionKeyMapping",
    "KeyName                                      NameProperty",
]


# The F12 deep dump of a freshly spawned BP_DropItem_C. It runs one watch tick after
# the actor is first seen, writes its property list before reading anything, and puts a
# flushed breadcrumb around every read - these strings are the proof of all three.
EXPECTED_DEEP_STRINGS = [
    "BP_DropItem_C",
    "(+1 tick(s))",                   # the deferred first-sight dump
    "PROPERTY LIST (names and types only, nothing read):",
    "VALUES - scalars:",
    "VALUES - object refs:",
    "VALUES - arrays / structs / other:",
    "-> read Items (ArrayProperty)",  # the breadcrumb written before the engine call
    "ok Items",
    "ItemID",                         # the item identity, out of a TArray of structs
    "OwnedItemID",                    # and off the component
    "element(s) via ForEach",
]


def run_mode(mode: str, keep: bool) -> bool:
    tmp = Path(tempfile.mkdtemp(prefix=f"wrmock_{mode}_"))
    try:
        mod = tmp / "WuchangRecon"
        (mod / "Scripts").mkdir(parents=True)
        (mod / "out").mkdir(parents=True)
        shutil.copy2(MOD_SRC / "Scripts" / "main.lua", mod / "Scripts" / "main.lua")

        lua = LuaRuntime(unpack_returned_tuples=True)
        # resolve_out_dir() derives the out dir from a package.path entry that looks
        # like "<mod>\Scripts\?.lua" and contains the mod name - give it exactly that.
        lua.execute(
            "package.path = [[{p}\\WuchangRecon\\Scripts\\?.lua;]] .. package.path".format(p=str(tmp).replace("\\", "\\\\"))
        )
        lua.globals().os.getenv  # noqa: B018  - touch it so lupa exposes os

        # lupa's os.getenv reads the real environment; inject through a Lua shim
        # instead so we do not have to mutate the process environment.
        lua.execute(
            "local real = os.getenv; os.getenv = function(k) "
            "if k == 'WR_MODE' then return [[{mode}]] end "
            "if k == 'WR_TMP' then return [[{tmp}]] end "
            "return real(k) end".format(mode=mode, tmp=str(tmp).replace("\\", "\\\\"))
        )

        out_lines: list[str] = []
        lua.globals().print = lambda *a: out_lines.append(" ".join(str(x) for x in a))

        harness = (HERE / "harness.lua").read_text(encoding="utf-8")
        try:
            lua.execute(harness)
        except Exception as exc:  # lupa raises LuaError for os.exit(1) too
            print("\n".join(out_lines))
            print(f"[{mode}] harness aborted: {exc}", file=sys.stderr)
            return False

        text = "\n".join(out_lines)
        print(text)
        if "HARNESS OK" not in text:
            print(f"[{mode}] harness did not reach the end", file=sys.stderr)
            return False

        dumps = sorted((mod / "out").glob("*"))
        print(f"[{mode}] {len(dumps)} output file(s): {[d.name for d in dumps]}")
        if not dumps:
            print(f"[{mode}] FAIL: the mod wrote no output at all", file=sys.stderr)
            return False

        if mode == "friendly":
            world = [d for d in dumps if "_world" in d.name]
            if not world:
                print("[friendly] FAIL: no world dump was written", file=sys.stderr)
                return False
            body = world[-1].read_text(encoding="utf-8", errors="replace")
            missing = [s for s in EXPECTED_WORLD_SECTIONS + EXPECTED_WORLD_STRINGS if s not in body]
            if missing:
                print(f"[friendly] FAIL: world dump is missing {missing}", file=sys.stderr)
                return False
            print(f"[friendly] world dump OK: {len(body.splitlines())} lines, all expected content present")

            watch = [d for d in dumps if d.name.startswith("pickupwatch_")]
            if not watch:
                print("[friendly] FAIL: no pickupwatch file", file=sys.stderr)
                return False
            wbody = watch[-1].read_text(encoding="utf-8", errors="replace")
            for needle in ("baseline:", "DESTROYED", "CHANGED"):
                if needle not in wbody:
                    print(f"[friendly] FAIL: pickup watch never logged {needle!r}\n{wbody}", file=sys.stderr)
                    return False
            print("[friendly] pickup watch OK: baseline + DESTROYED + CHANGED all reported")

            deep = [d for d in dumps if d.name.startswith("pickupdeep_")]
            if not deep:
                print("[friendly] FAIL: no pickupdeep file", file=sys.stderr)
                return False
            dbody = deep[-1].read_text(encoding="utf-8", errors="replace")
            dmissing = [s for s in EXPECTED_DEEP_STRINGS if s not in dbody]
            if dmissing:
                print(f"[friendly] FAIL: deep dump is missing {dmissing}\n{dbody}", file=sys.stderr)
                return False
            print(f"[friendly] deep dump OK: {len(dbody.splitlines())} lines, "
                  "list-before-values + read breadcrumbs + item id present")

            inp = [d for d in dumps if "_input" in d.name]
            if not inp:
                print("[friendly] FAIL: no input dump was written", file=sys.stderr)
                return False
            ibody = inp[-1].read_text(encoding="utf-8", errors="replace")
            imissing = [s for s in EXPECTED_INPUT_SECTIONS + EXPECTED_INPUT_STRINGS if s not in ibody]
            if imissing:
                print(f"[friendly] FAIL: input dump is missing {imissing}", file=sys.stderr)
                return False
            print(f"[friendly] input dump OK: {len(ibody.splitlines())} lines, "
                  "contexts + priorities + key names + layouts all present")

        print(f"[{mode}] PASS")
        return True
    finally:
        if keep:
            print(f"[{mode}] temp dir kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["friendly", "hostile", "both"], default="both")
    ap.add_argument("--keep", action="store_true", help="do not delete the temp directory")
    args = ap.parse_args()

    modes = ["friendly", "hostile"] if args.mode == "both" else [args.mode]
    ok = True
    for mode in modes:
        print(f"\n{'=' * 70}\n{mode}\n{'=' * 70}")
        ok = run_mode(mode, args.keep) and ok
    print("\nALL MODES PASS" if ok else "\nFAILURES ABOVE")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
