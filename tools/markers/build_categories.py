#!/usr/bin/env python3
r"""Derive the marker-category class table from the blueprint class graph
(review item C.10) -> `markers/categories.json`.

THE GRAPH, NOT A HAND-WRITTEN LEAF LIST
---------------------------------------
`class_graph.py` reads the `super` field out of every cooked `.uasset` export
map, so "which classes are doors?" is "what are the descendants of the game's
own door base?".  What this script adds is the choice of BASES - one per
category, taken from the game's own hierarchy - and the precedence that
resolves a class reachable from two of them.  Everything below a base is data,
which is what keeps a category from silently missing the classes nobody
thought to list.

WHAT THE GRAPH FOUND THAT THE HAND LIST DID NOT
-----------------------------------------------
    ladder   + BP_InteractionLadder_C                    (a separate root)
    lift     + BP_CylinderElevator_C, BP_CylinderElevator_02_C
    door     + 12 descendants of BP_InteractionObject_Door_C, where the hand
               list had BP_NewPuzzlesDoor_C and a `^BP_Door` regex
    pickup   + BP_PickupActor_New_C, BP_DropItem_C, BP_BombsBox_C,
               BP_SoulPackage_C, BP_AutoPickUp_Child_C, BP_PickUpPT_DSG_C,
               BP_PickUpPT_Red_C
    chest      BP_ItemBox_C is the base of both known chests, so a third
               chest blueprint would be picked up without an edit here

PRECEDENCE, AND WHY IT IS NOT COSMETIC
--------------------------------------
Three of the game's marker classes derive from `BP_NPC_C`, which has 78
descendants: `BP_RebornFire_C` (a shrine), `DKDC_NPC_C` / `ReadPointSP_NPC_C` /
`Letter01_NPC_C` (the readable notes), `ItemCollectionBox_C` (a pickup) and
`BP_KlesaCleaner_C` (a world mechanism).  `BP_PickUpActor_Trap_C` derives from
`BP_PickupActor_C` and must be `hidden`, not `pickup` - it is the trap that
looks like an item.  So the roots are applied in a fixed order, most specific
first, and the first category that claims a class keeps it.  `ORDER` below IS
that decision; a class' own explicit entry always wins over any root.

    python build_categories.py                      # -> ..\..\markers\categories.json
    python build_categories.py --report             # + every class, by category
    python build_categories.py --diff-hand          # what changed vs the old hand list
"""

from __future__ import annotations

import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import class_graph                                          # noqa: E402
import pakmaps                                              # noqa: E402
import provenance                                           # noqa: E402

SCHEMA = "wuchang-minimap-categories/1"

# Category -> the game's own base class(es).  A category with several roots has
# them because the game really has several unrelated hierarchies for the thing
# (`BP_ElevatorBase_C` derives from `Actor`, `BP_ElevatorBox_C` from `DSCActor`).
ROOTS: dict[str, list[str]] = {
    "shrine":   ["BP_RebornFire_C"],
    # The trap that looks like an item. A `BP_PickupActor_C` descendant, so it
    # has to be claimed before `pickup`.
    "hidden":   ["BP_PickUpActor_Trap_C"],
    "chest":    ["BP_ItemBox_C"],
    "pickup":   ["BP_PickupActor_C", "BP_PickUpPT_C", "ItemCollectionBox_C"],
    # The game's readable notes. Not merchants: every placed instance carries a
    # read-point id (`NPC_DG_READ08`), the blueprint references no character
    # mesh, its only interaction string is `ui_263` = "Check", and it spawns the
    # `NS_Hint01` hint particle.
    "note":     ["DKDC_NPC_C", "ReadPointSP_NPC_C", "Letter01_NPC_C"],
    # The two special doors, one category each: "which riddle is unanswered" and
    # "which chisel door is unopened" are different questions. Both are siblings of
    # the door base rather than descendants, which is why `door` never claimed
    # `BP_NewGetGeemDoor_C` at all and the mod showed none of the seven.
    # `BP_PuzzlesDoor_C` is the superseded riddle door, 0 placements and a `BP_NPC_C`
    # descendant, so without it here the graph files it under `npc` while
    # `markers.cpp` calls it a gate.
    "mystery_gate":     ["BP_NewPuzzlesDoor_C", "BP_PuzzlesDoor_C"],
    "benediction_door": ["BP_NewGetGeemDoor_C"],
    "door":     ["BP_InteractionObject_Door_C"],
    # The three fog-gate blueprints are siblings, not a hierarchy.
    "fog_gate": ["BP_Wumen_C", "BP_Wumen_NetworkRang_C", "BP_Wumen_ClientOnly_C"],
    "ladder":   ["BP_LadderV2_C", "BP_InteractionLadder_C"],
    "lift":     ["BP_ElevatorBase_C", "BP_ElevatorBox_C"],
    "boss":     ["BP_PlacedBossAI_C"],
    # World mechanisms worth a pin but not a category of their own. Leaves, not
    # bases: each of these sits directly under `DSCActor` or
    # `BP_InteractionObject_C` alongside dozens of things that are not markers.
    # `BP_FireReed_C` is a lightable reed prop - it only ever looked like a boss
    # because 8 of them sit in `Chapter1_Wanrenk_BOSS_AI`.
    "other":    ["BP_FireReed_C", "BP_zhuanjingta_C", "BP_QiCaiShi_2_C",
                 "BP_WoodenExternalPushRod_C", "BP_KlesaCleaner_C",
                 "BP_StonePillar_C"],
    # 78 descendants, and a root that must come after the five of them that are
    # claimed above.
    "npc":      ["BP_NPC_C"],
    # `enemy` is a class question too, and this is the root that makes it one.
    # `BP_BaseAI_C` has 314 descendants and covers the whole placed roster -
    # `MingBing_Dao_C`, `guanbing_*`, `DaoMuZei_*`, `AI_Monster_baihu_C`, the
    # `M_*`/`E_*` chapter mobs - with **zero** overlap with `BP_NPC_C`'s 78.
    # The 32 boss classes are descendants of it as well, which is why `boss`
    # comes first in `ORDER`.
    #
    # The `_AI`-sublevel heuristic stays in `marker_classes` only as a counted
    # fallback, for the handful of classes whose `super` the graph never saw
    # (`BP_jiaheshang_AI_C` has no parent recorded). A name rule cannot do this
    # job: `Pangzi_NPC_C` is a `BP_PlacedAI_C` descendant - an ENEMY whose name
    # says NPC - while `PangZi_NPC_C`, differing only in one letter's case,
    # really is a `BP_NPC_C`. A case-insensitive lookup merges them; the graph
    # separates them.
    "enemy":    ["BP_BaseAI_C"],
}

# Most specific first. The first category to claim a class keeps it.
ORDER = ["shrine", "hidden", "chest", "pickup", "note", "mystery_gate",
         "benediction_door", "door", "fog_gate", "ladder", "lift", "boss",
         "other", "npc", "enemy"]

# Classes the graph would hand to a category but that are not markers. Each one
# needs a reason, and "it is not a marker" is not a reason - say what it is.
EXCLUDE = {
    # An invisible blocking volume that rides an elevator, not a lift to pin.
    "BP_ElevatorBlockBox_C": "elevator collision blocker, no mesh",
    # Abstract AI bases. Descendants of `BP_BaseAI_C` and therefore swept in as
    # `enemy`, but nothing is ever placed as one, so listing them as marker
    # classes would only make the runtime table longer.
    "BP_BaseAI_C": "abstract AI base, never placed",
    "BP_PlacedAI_C": "abstract AI base, never placed",
    "BP_PlacedBossAI_C": "abstract boss base, never placed",
}


def build(ms, verbose: bool = True, want_hash: bool = True,
          pak_path: str | None = None) -> dict:
    graph = class_graph.load_or_build(ms, verbose=verbose)
    missing: dict[str, list[str]] = {}
    classes: dict[str, str] = {}
    per: dict[str, list[str]] = {}
    for cat in ORDER:
        roots = ROOTS[cat]
        got: set[str] = set()
        for r in roots:
            if r not in graph:
                missing.setdefault(cat, []).append(r)
            got.add(r)
            got.update(class_graph.descendants(graph, r))
        keep = []
        for c in sorted(got):
            if c in EXCLUDE or c in classes:
                continue
            classes[c] = cat
            keep.append(c)
        per[cat] = keep
    if verbose:
        print(f"  {len(classes)} class(es) in {len(per)} categor(ies) from "
              f"{len(graph)} blueprint classes")
        for cat in ORDER:
            print(f"    {cat:<9} {len(per[cat]):>4}  roots: {', '.join(ROOTS[cat])}")
        for cat, ms_ in sorted(missing.items()):
            print(f"    ! {cat}: root(s) not in the class graph: {', '.join(ms_)}")
    return {
        "schema": SCHEMA,
        "count": len(classes),
        "order": ORDER,
        "roots": ROOTS,
        "excluded": EXCLUDE,
        "by_category": per,
        "classes": classes,
        "missing_roots": missing,
        "source": "cooked .uasset export maps (super field), offline pak extraction",
        "generated_by": "tools/markers/build_categories.py",
        **provenance.stamp(ms, pak_path, want_hash),
    }


# The table this replaces, kept only so `--diff-hand` can show what moved. It is
# the `marker_classes.EXACT` of 1.0.0.
HAND = {
    "BP_RebornFire_C": "shrine", "BP_treasurebox_C": "chest",
    "BP_ItemRedBox_C": "chest", "BP_PickupActor_C": "pickup",
    "BP_PickUpPT_C": "pickup", "BP_AutoPickUp_C": "pickup",
    "ItemCollectionBox_C": "pickup", "BP_NewPuzzlesDoor_C": "door",
    "BP_Wumen_C": "fog_gate", "BP_Wumen_NetworkRang_C": "fog_gate",
    "BP_Wumen_ClientOnly_C": "fog_gate", "BP_LadderV2_C": "ladder",
    "BP_Ladder_Child_C": "ladder", "BP_WoodenElevator_C": "lift",
    "BP_ElevatorBox_C": "lift", "DKDC_NPC_C": "note",
    "ReadPointSP_NPC_C": "note", "Letter01_NPC_C": "note",
    "BP_FireReed_C": "other", "BP_PickUpActor_Trap_C": "other",
    "BP_zhuanjingta_C": "other", "BP_QiCaiShi_2_C": "other",
    "BP_WoodenExternalPushRod_C": "other", "BP_KlesaCleaner_C": "other",
    "BP_StonePillar_C": "other",
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers",
                                                  "categories.json"))
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--diff-hand", action="store_true")
    provenance.add_arg(ap)
    a = ap.parse_args(argv)

    doc = build(pakmaps.MapSource(a.pak), want_hash=not a.no_pak_hash,
                pak_path=a.pak)
    out = os.path.normpath(a.out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print(f"wrote {out}")
    if a.report:
        for cat in doc["order"]:
            print(f"  {cat}:")
            for c in doc["by_category"][cat]:
                print(f"      {c}")
    if a.diff_hand:
        new = doc["classes"]
        print("  added:")
        for c in sorted(set(new) - set(HAND)):
            print(f"      {new[c]:<9} {c}")
        print("  retyped:")
        for c in sorted(set(new) & set(HAND)):
            if new[c] != HAND[c]:
                print(f"      {c}: {HAND[c]} -> {new[c]}")
        print("  dropped:")
        for c in sorted(set(HAND) - set(new)):
            print(f"      {HAND[c]:<9} {c}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
