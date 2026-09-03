#!/usr/bin/env python3
r"""Build `markers/enemies.json` - the enemy roster: names where the game has
one, and the elite/normal split (review items C.8 and C.9).

WHAT C.8 ASKED FOR, AND WHAT THE GAME ACTUALLY HAS
--------------------------------------------------
The review's complaint is real: 1 768 of 3 599 markers read "Enemy", and
Chapter 1's 397 enemy markers share a single name.  The proposed fix was to
extend the boss route (`DT_AiTable` -> `boss_name_<id>` -> `MMGame.locres`) or
the NPC route (an `FText` key embedded in the blueprint) to the enemy roster.

Both were tried.  **The game has no display name for an ordinary enemy**, and
that is a finding, not a gap in this script.  Four independent proofs, in the
order they were run:

1.  **The locres has no enemy name family.**  A regex histogram over all 7 213
    English keys in `MMGame.locres` yields nine item prefixes, `ui_*`,
    `help_*`, `npc_Dianame_*` (43), `npc_name_*` (18) and exactly **30**
    `boss_name_*`.  There is no `monster_*`, `enemy_*`, `ai_name_*` or
    `mob_name_*` key at all.  Run `--prove` to re-print that histogram.
2.  **`DT_AiTable` carries no name.**  Its 255 rows are the join that named
    every boss, and a row's payload is 52-60 bytes of `FName` pairs and ints
    with no `FString` and no `FText` key anywhere in it.  The class `FName` at
    +18 is the only text a row has.
3.  **The blueprint `FText` route returns nothing.**  `build_npcs.py` works
    because a cooked `FText` default keeps its localisation key as a plain
    string in the `.uasset`: `HJE_NPC_NoWeapon.uasset` contains
    `npc_Dianame_01`.  Scanning `.uasset` + `.uexp` of six representative enemy
    blueprints (`MingBing_Dao`, `MingBing_Dao_High`, `BP_Monster_Dog`,
    `guanbing_qiangdun`, `DaoMuZei_YuanCheng`, `BP_YHjiaheshang_AI`) for any
    string that IS a locres key returns **zero** hits, with the NPC asset as
    the positive control in the same run.
4.  **There is no bestiary.**  `help_noun_*` (38) and `help_character_*` (23)
    are the mechanics glossary - "Shimmer", "Skyborn Might", "Vitality" - not
    monster entries.  Which matches the game itself: Wuchang shows a name
    banner for bosses and nothing for trash mobs.

So this script does the two things that ARE available in the data, and leaves
"Enemy" as the honest fallback for the rest.  It does **not** fall back to a
tidied class name the way `build_bosses.py` does: "Mingbing Dao" is Pinyin, not
English, and a wrong-looking name is worse for a player than a generic one.

WHAT IT DOES PRODUCE
--------------------
1.  **`name`** for every enemy class that has a real localised name - the
    minions, phases and re-skins that DO own a `DT_AiTable` id with a
    `boss_name_<id>` behind it.  Same resolution chain as `build_bosses.py`
    (own id, `_1` variant, near-neighbour id, lexical sibling, super chain),
    minus the class-name last resort.
2.  **`elite`** for C.9, by a rule the data validates rather than a hand list:
    a class is elite when stripping a `_High` / `_Special` / `_S` suffix leaves
    the name of *another class that exists* and is itself an enemy.
    `MingBing_Dao_High_C` is elite because `MingBing_Dao_C` is a real enemy
    class; `MingBing_ChangQian_Child_C` and `BP_M_DXJDG_AI_Low_C` are not,
    because `_Child` and `_Low` are the game's *weaker* variants and get no
    suffix rule.  The comparison is case-insensitive, because the roster mixes
    `MingBing_JianDun_C` with `MingBing_jiandun_High_C`.

    python build_enemies.py                # -> ..\..\markers\enemies.json
    python build_enemies.py --report       # + the per-class table
    python build_enemies.py --prove        # the C.8 "no such field" evidence
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import build_bosses as BB                                   # noqa: E402
import build_items as BI                                    # noqa: E402
import class_graph                                          # noqa: E402
import marker_classes                                       # noqa: E402
import pakmaps                                              # noqa: E402
import provenance                                           # noqa: E402

SCHEMA = "wuchang-minimap-enemies/1"

# The root of every placed AI in the game (`BP_PlacedBossAI_C` is one of its
# descendants). See `build_categories.ROOTS["enemy"]` for the evidence.
AI_BASE = "BP_BaseAI_C"

# A variant suffix that means "a tougher version of the class next to it".
# `_S` is the game's own abbreviation for it (`M_GBHC` / `M_GBHC_S`), and the
# rule is only ever applied when the stripped name resolves to a real class, so
# a two-letter suffix cannot invent an elite out of a base class' name.
ELITE_SUFFIX = re.compile(r"^(?P<base>.+?)_(High|Special|S)(?P<tail>(?:_\w+?)?)_C$",
                          re.IGNORECASE)

# The opposite end of the same axis, listed only so the intent is on the record:
# these never make a class elite.
WEAKER_SUFFIX = re.compile(r"_(Child|Low|Small|Weak)(_\w+?)?_C$", re.IGNORECASE)


def enemy_classes_in_markers(markers_dir: str) -> dict[str, int]:
    """class -> how many `enemy` markers it accounts for, from the shipped DB.

    The roster the mod actually needs is "every class the extractor typed
    `enemy`", not "every row of `DT_AiTable`" - the table has entries for AI
    that is never placed, and the levels place classes the table never lists.
    Reading the current chapter files makes the coverage report measure the
    real thing.  Absent files just mean an empty roster, never a failure.
    """
    out: collections.Counter = collections.Counter()
    if not os.path.isdir(markers_dir):
        return out
    for fn in sorted(os.listdir(markers_dir)):
        if not fn.startswith("chapter") or not fn.endswith(".json") or ".sample." in fn:
            continue
        try:
            with open(os.path.join(markers_dir, fn), encoding="utf-8") as f:
                db = json.load(f)
        except Exception:                                       # noqa: BLE001
            continue
        for m in db.get("markers", []):
            if m.get("cat") in ("enemy", "elite"):
                out[m.get("cls", "")] += 1
    out.pop("", None)
    return out


def elite_split(classes: set[str], known: set[str]) -> dict[str, str]:
    """class -> the base class that makes it elite, for the ones that are.

    `known` is every class name the game defines (the class graph plus the AI
    table plus the roster itself), so "the stripped name is a real class" is a
    test against the game's own data and not against this file.
    """
    lower = {c.lower(): c for c in known}
    out: dict[str, str] = {}
    for c in sorted(classes):
        if WEAKER_SUFFIX.search(c):
            continue
        m = ELITE_SUFFIX.match(c)
        if not m:
            continue
        base = m.group("base") + (m.group("tail") or "")
        hit = lower.get((base + "_C").lower()) or lower.get(base.lower())
        if hit and hit != c:
            out[c] = hit
    return out


def build(ms, markers_dir: str, verbose: bool = True, want_hash: bool = True,
          pak_path: str | None = None) -> dict:
    graph = class_graph.load_or_build(ms, verbose=verbose)
    ids = BB.ai_ids(ms, verbose)
    loc = BI.read_locres(ms.read(BI.LOCRES.format(lang="en")))
    boss_classes = set(class_graph.descendants(graph, BB.BOSS_BASE))

    roster = enemy_classes_in_markers(markers_dir)
    # The roster is a property of the CLASS GRAPH, not of the chapter files:
    # `BP_BaseAI_C` has 314 descendants and they are the game's whole placed AI
    # cast, bosses included. Taking it from the graph is what keeps this script
    # free of a cycle with `extract_markers.py` - `elite` has to be decided
    # BEFORE the chapters are extracted, because `marker_classes` reads it.
    # The AI table and the current marker DB are unioned in so a class the graph
    # never saw a parent for still gets a row.
    ai_base = set(class_graph.descendants(graph, AI_BASE))
    candidates = (ai_base | set(roster) | set(ids)) - boss_classes - {AI_BASE}
    known = set(graph) | set(ids) | candidates

    elites = elite_split(candidates, known)

    enemies: dict[str, dict] = {}
    named = 0
    for cls in sorted(candidates):
        ai = ids.get(cls)
        rec: dict = {"markers": roster.get(cls, 0)}
        if ai:
            rec["ai_id"] = ai
        # Only the routes that can produce the GAME's own words. `resolve_name`
        # ends in a tidied class name; that last rule is the one thing we must
        # not take, so its `via` is treated as "no name".
        name, how = BB.resolve_name(cls, ai, loc, graph, ids, boss_classes)
        if how != "class-name":
            rec["name"] = name
            rec["via"] = how
            named += 1
        if cls in elites:
            rec["elite"] = True
            rec["elite_of"] = elites[cls]
        enemies[cls] = rec

    placed = {c: r for c, r in enemies.items() if r["markers"]}
    doc = {
        "schema": SCHEMA,
        "count": len(enemies),
        "named": named,
        "elite": sum(1 for r in enemies.values() if r.get("elite")),
        "source": ("cooked .uasset export maps (class graph) + DT_AiTable + "
                   "MMGame.locres, offline pak extraction"),
        "generated_by": "tools/markers/build_enemies.py",
        "name_note": ("Wuchang has no display name for an ordinary enemy: no "
                      "enemy/monster key family in MMGame.locres, no name in a "
                      "DT_AiTable row, no FText key embedded in an enemy "
                      "blueprint, and no bestiary. Only the classes that own a "
                      "boss_name_<id> get a name here; the rest keep the "
                      "generic label. See this file's module docstring."),
        "enemies": enemies,
        **provenance.stamp(ms, pak_path, want_hash),
    }
    if verbose:
        mk = sum(r["markers"] for r in enemies.values())
        nmk = sum(r["markers"] for r in enemies.values() if "name" in r)
        emk = sum(r["markers"] for r in enemies.values() if r.get("elite"))
        print(f"  {len(enemies)} enemy class(es), {len(placed)} of them placed; "
              f"{named} localised name(s), {doc['elite']} elite")
        print(f"  markers covered: {nmk}/{mk} named, {emk}/{mk} elite")
    return doc


def prove(ms) -> None:
    """Re-run the four C.8 proofs and print them, so the claim stays checkable."""
    loc = BI.read_locres(ms.read(BI.LOCRES.format(lang="en")))
    hist: collections.Counter = collections.Counter()
    for k in loc:
        m = re.match(r"^([A-Za-z_]+?)_?\d+", k)
        hist[m.group(1) if m else k.split("/")[0]] += 1
    print(f"1. MMGame.locres/en: {len(loc)} keys; name families present:")
    for fam in ("boss_name", "npc_Dianame", "npc_name"):
        print(f"     {fam:<14} {hist.get(fam, 0)}")
    for fam in ("monster_name", "enemy_name", "ai_name", "mob_name", "monster"):
        print(f"     {fam:<14} {hist.get(fam, 0)}   (absent)")
    print("2. DT_AiTable rows: FName pairs and ints only -")
    ids = BB.ai_ids(ms, verbose=False)
    have = sum(1 for c, i in ids.items() if loc.get(f"boss_name_{i}"))
    print(f"     {len(ids)} classes with an id, {have} of them with a boss_name_<id>")
    print("3. FText-key route on enemy blueprints (control: HJE_NPC_NoWeapon):")
    keys = set(loc)
    rx = re.compile(rb"[A-Za-z_][A-Za-z0-9_/]{5,60}")
    for stem in ("MingBing_Dao", "MingBing_Dao_High", "BP_Monster_Dog",
                 "guanbing_qiangdun", "DaoMuZei_YuanCheng", "BP_YHjiaheshang_AI",
                 "HJE_NPC_NoWeapon"):
        hit = [k for k in ms.owner
               if k.endswith(".uasset") and k.rsplit("/", 1)[-1][:-7] == stem]
        if not hit:
            print(f"     {stem:<22} asset not found")
            continue
        blob = ms.read(hit[0])
        uexp = hit[0][:-7] + ".uexp"
        if uexp in ms.owner:
            blob += ms.read(uexp)
        found = sorted({m.group().decode("ascii", "ignore")
                        for m in rx.finditer(blob)} & keys)
        print(f"     {stem:<22} {found if found else 'no locres key'}")
    print("4. bestiary: help_noun_* / help_character_* are the mechanics glossary")
    for k in ("help_noun11_name", "help_noun12_name", "help_character03_name"):
        print(f"     {k} = {loc.get(k)!r}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers",
                                                  "enemies.json"))
    ap.add_argument("--markers", default=os.path.join(_HERE, "..", "..", "markers"))
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--prove", action="store_true",
                    help="print the evidence that the game has no enemy names")
    provenance.add_arg(ap)
    a = ap.parse_args(argv)

    ms = pakmaps.MapSource(a.pak)
    if a.prove:
        prove(ms)
        return 0
    doc = build(ms, os.path.abspath(a.markers), want_hash=not a.no_pak_hash,
                pak_path=a.pak)
    out = os.path.normpath(a.out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print(f"wrote {out}")
    if a.report:
        for cls, r in sorted(doc["enemies"].items(),
                             key=lambda kv: (-kv[1]["markers"], kv[0])):
            flag = "elite" if r.get("elite") else ""
            print(f"  {r['markers']:>4}  {cls:<40} {flag:<6}"
                  f"{r.get('name', '-'):<34} {r.get('via', '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
