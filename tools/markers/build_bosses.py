#!/usr/bin/env python3
r"""Build `markers/bosses.json` - the game's boss roster, offline, with no `.usmap`.

BOSS IS A CLASS QUESTION
------------------------
Every boss in the game is a **placed actor** deriving from `BP_PlacedBossAI_C`
-- 25 instances across 25 `*_AI` sublevels, whose names are spelled `_BOSS_AI`
only in Chapter 1 (`Chapter3_ZhenWuG_ZhangXianZ_AI`,
`Chapter5_ZhenLiZM_YHXYZ_AI`, ...).  So the roster comes from the inheritance
graph (`class_graph.py`), never from a level-name or class-name heuristic: a
name rule misses chapters 2/3/5 entirely and types `BP_FireReed_C` (8 in
Chapter 1), `BP_BossPool_C` (one instance in the whole game, a spawner) and
`BossLightingEffectActor_C` (a light rig) as bosses.

THREE THINGS THIS SCRIPT WRITES
-------------------------------
1. `classes` -- every descendant of `BP_PlacedBossAI_C`, from the export maps'
   `super` field.  `marker_classes.BOSS_CLASSES` is this list.
2. `ai_id` per class -- from `DT_AiTable`, the debug plugin's AI table.  Its
   rows are keyed by the numeric AI id (row `FName`s are in the package's own
   name map, the trick from `build_items.py`) and each row carries the AI's
   class `FName` at a fixed +18 bytes from the row name.  No property decoding,
   so no mappings.
3. `name` per class -- `boss_name_<ai id>` in `MMGame.locres`, which gives the
   game's own English boss names ("Reborn Treant - Soulwood", "Commander -
   Honglan").  Cross-checked against the transliterations in the class names
   themselves: `BP_Dashuguai` (big tree monster) -> "Reborn Treant",
   `BP_DaYouYan` (youyan = centipede) -> "Great Centipede",
   `BP_XMWC` (xinmo = inner demon) -> "Demon of Obsession".

    python build_bosses.py                 # -> ..\..\markers\bosses.json
    python build_bosses.py --report        # + where every instance is placed
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

import build_items as BI
import class_graph
import pakmaps
import provenance                          # noqa: E402

SCHEMA = "wuchang-minimap-bosses/1"
BOSS_BASE = "BP_PlacedBossAI_C"

# The boss classes all live under one of these three roots, so the class-graph
# sweep does not have to read all 81 k `.uasset` entries. Verified by building
# the full graph once and bucketing the boss/NPC classes by asset prefix.
GRAPH_PREFIXES = (
    "Content/Game/AI/",
    "Content/Game/Blueprints/",
    "Content/DynamicCombatSystem/Blueprints/",
)

AI_TABLE = "Plugins/GameModules/DebugWidget/Content/ExDebug/AIControll/DT_AiTable.uasset"

# Distance from a `DT_AiTable` row's name to the row's AI class `FName`, in
# bytes. Measured, not assumed: with this offset 253 of the table's 255 numeric
# rows pair with a class whose name ends `_C`, and every pairing that involves a
# boss agrees with the transliteration in the class name.
CLASS_AT = 18

# One boss stage the AI table gives an id with no localised name of its own.
# `BP_Anim_ZXZ_StepA_C` and `BP_Anim_ZXZ_StepB_C` are the two phases of the same
# fight in the same package (`Chapter3_ZhenWuG_ZhangXianZ_AI`); StepB resolves to
# `boss_name_50600`, StepA's `50500` is not in the locres at all and its id is
# too far from 50600 for the neighbour rule below.
NAME_OVERRIDE = {
    "BP_Anim_ZXZ_StepA_C": "boss_name_50600_1",
}


def build_graph(src: pakmaps.MapSource, verbose: bool = True) -> dict[str, str]:
    keys = sorted(k for k in src.owner
                  if k.endswith(".uasset") and k.startswith(GRAPH_PREFIXES))
    graph: dict[str, str] = {}
    for k in keys:
        try:
            pkg = pakmaps._package_from_bytes(k, src.read(k), b"")
            rows = __import__("inspect_classes").export_supers(pkg)
        except Exception:                                       # noqa: BLE001
            continue
        for name, cls, sup in rows:
            if cls == "BlueprintGeneratedClass" and name not in graph:
                graph[name] = sup
    if verbose:
        print(f"  class graph: {len(graph)} blueprint class(es) "
              f"from {len(keys)} asset(s)")
    return graph


def ai_ids(src: pakmaps.MapSource, verbose: bool = True) -> dict[str, str]:
    """AI class name -> its numeric id in DT_AiTable."""
    pkg = pakmaps._package_from_bytes(
        AI_TABLE, src.read(AI_TABLE), src.read(AI_TABLE[:-len(".uasset")] + ".uexp"))
    ex = pkg.exports[0]
    d = pkg.data(ex)
    names = pkg.names

    def fname(off: int):
        if off < 0 or off + 8 > len(d):
            return None
        idx, num = struct.unpack_from("<ii", d, off)
        if 0 <= idx < len(names) and num == 0:
            return names[idx]
        return None

    out: dict[str, str] = {}
    rows = 0
    for o in range(len(d) - 8):
        row = fname(o)
        if not row or not row.isdigit():
            continue
        cls = fname(o + CLASS_AT)
        if cls and cls.endswith("_C"):
            rows += 1
            out.setdefault(cls, row)
    if verbose:
        print(f"  DT_AiTable: {rows} row(s) paired, {len(out)} distinct class(es)")
    return out


def code_of(cls: str) -> str:
    """The boss' all-caps code inside its class name (`BP_CZ_AI_3_C` -> `CZ`)."""
    for tok in cls.split("_"):
        if 2 <= len(tok) <= 6 and tok.isupper() and tok.isalnum() and tok not in (
                "BP", "AI", "C", "B", "LS", "NEW"):
            return tok
    return ""


def shares_prefix(a: str, b: str, least: int) -> bool:
    """Do two class names share an underscore-delimited prefix this long?"""
    pa, pb = a.split("_"), b.split("_")
    shared, n = 0, 0
    for x, y in zip(pa, pb):
        if x != y:
            break
        shared += len(x) + 1
        n += 1
    return n >= 2 and shared - 1 >= least


def resolve_name(cls: str, ai_id: str | None, loc: dict[str, str],
                 graph: dict[str, str], ids: dict[str, str],
                 boss_classes: set[str]) -> tuple[str, str]:
    """(display name, how it was resolved)."""
    key = NAME_OVERRIDE.get(cls)
    if key and loc.get(key):
        return loc[key], "override:" + key
    # The class' own id, then the `_1` variant the locres uses for a boss'
    # second form ("Sovereign - Zhang Xianzhong" vs "...'s Obsession").
    for cand, how in ((ai_id, "id"), (f"{ai_id}_1", "id_1")):
        if cand and loc.get(f"boss_name_{cand}"):
            return loc[f"boss_name_{cand}"], how
    # A variant id: the game numbers a boss' special/second placement one or a
    # few above the base entry (50301 next to 50300, 51201 next to 51200,
    # 39911 next to 39910, 31704 next to 31703). Only ids sharing the first
    # three digits are considered, and the nearest wins.
    if ai_id and ai_id.isdigit():
        base = int(ai_id)
        best = None
        for k, v in loc.items():
            m = re.fullmatch(r"boss_name_(\d+)", k)
            if not m or not v:
                continue
            other = int(m.group(1))
            if m.group(1)[:3] != ai_id[:3] or abs(other - base) > 10:
                continue
            if best is None or abs(other - base) < abs(best[0] - base):
                best = (other, v)
        if best:
            return best[1], f"neighbour:{best[0]}"
    # A SIBLING: a variant blueprint of the same boss.  This has to come before
    # the super chain, because a blueprint duplicated from another boss inherits
    # from it -- `BP_CZ_AI_3_C`'s super really is `BP_BKL_AI_C`, and taking that
    # name would have called a Chongsheng variant "Monstrous Toddler - Bai Kru".
    # Two ways to be a sibling, both purely lexical: the same all-caps code
    # (`BP_CZ_AI_3_C` and `BP_CZ_AI_C` are both `CZ`), or a shared underscore
    # prefix of at least eight characters (`Boss_Luhongliu_*`).  The sibling with
    # the lowest AI id wins, so the base form names the variants.
    mine = code_of(cls)
    best = None
    for other, oid in sorted(ids.items(), key=lambda kv: kv[1]):
        if other == cls or other not in boss_classes or not loc.get(f"boss_name_{oid}"):
            continue
        if (mine and code_of(other) == mine) or shares_prefix(cls, other, 8):
            best = (other, oid)
            break
    if best:
        return loc[f"boss_name_{best[1]}"], "sibling:" + best[0]
    # The super chain: a `_S` / `_Special` subclass with no row of its own is
    # the same boss as its parent.
    for parent in class_graph.chain(graph, cls)[1:]:
        pid = ids.get(parent)
        if pid and loc.get(f"boss_name_{pid}"):
            return loc[f"boss_name_{pid}"], "super:" + parent
    # Last resort: the class name, tidied. Acceptable per the brief, and it is
    # never silent - `named` in the manifest header counts the real ones.
    pretty = re.sub(r"^(BP|AI|B|Boss)_|_(AI|BP)(_|$)|_C$", " ", cls).strip(" _")
    pretty = re.sub(r"[_\s]+", " ", pretty).strip()
    return pretty or cls, "class-name"


def placements(src: pakmaps.MapSource, classes: set[str]) -> dict[str, list[str]]:
    """class -> the `Content/Maps/...umap` packages that place an instance."""
    out: dict[str, list[str]] = {}
    for k in sorted(src.owner):
        if not (k.startswith("Content/Maps/") and k.endswith(".umap")):
            continue
        try:
            pkg = src.package(k)
        except Exception:                                       # noqa: BLE001
            continue
        for ex in pkg.exports:
            if ex.class_name in classes:
                out.setdefault(ex.class_name, []).append(os.path.basename(k)[:-5])
    return out


def build(src: pakmaps.MapSource, verbose: bool = True, report: bool = False,
          prov: dict | None = None) -> dict:
    graph = build_graph(src, verbose)
    if BOSS_BASE not in graph.values() and BOSS_BASE not in graph:
        raise SystemExit(f"{BOSS_BASE} not in the class graph - wrong prefixes?")
    classes = class_graph.descendants(graph, BOSS_BASE)
    if verbose:
        print(f"  {len(classes)} descendant(s) of {BOSS_BASE}")
    ids = ai_ids(src, verbose)
    loc = BI.read_locres(src.read(BI.LOCRES.format(lang="en")))

    bosses = {}
    for cls in classes:
        ai = ids.get(cls)
        name, how = resolve_name(cls, ai, loc, graph, ids, set(classes))
        bosses[cls] = {"name": name, "ai_id": ai, "via": how}
    named = sum(1 for b in bosses.values() if b["via"] != "class-name")

    doc = {
        "schema": SCHEMA,
        "base": BOSS_BASE,
        "count": len(bosses),
        "named": named,
        "source": ("cooked .uasset export maps (class graph) + DT_AiTable + "
                   "MMGame.locres, offline pak extraction"),
        "bosses": bosses,
        "generated_by": "tools/markers/build_bosses.py",
        **(prov or {}),
    }
    if report:
        doc["placed_in"] = placements(src, set(classes))
    if verbose:
        print(f"  {len(bosses)} boss class(es), {named} with a localised name")
    return doc


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "markers", "bosses.json"))
    ap.add_argument("--report", action="store_true",
                    help="also record where every boss class is placed")
    provenance.add_arg(ap)
    a = ap.parse_args(argv)
    ms = pakmaps.MapSource(a.pak)
    doc = build(ms, report=a.report,
                prov=provenance.stamp(ms, a.pak, not a.no_pak_hash))
    out = os.path.normpath(a.out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print(f"wrote {out}")
    for cls, b in sorted(doc["bosses"].items()):
        print(f"  {cls:<42} {str(b['ai_id']):<8} {b['name']}  [{b['via']}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
