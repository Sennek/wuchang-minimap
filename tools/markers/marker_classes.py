#!/usr/bin/env python3
"""Actor class -> marker category, for schema `wuchang-minimap-markers/1`.

Categories: shrine, chest, pickup, boss, elite, enemy, npc, note, door,
ladder, lift, fog_gate, hidden, other.

WHERE THE TABLE COMES FROM (review item C.10)
---------------------------------------------
It is **generated**, not hand-written.  `markers/categories.json`
(`build_categories.py`) is the descendants of one base class per category, read
out of the cooked `.uasset` export maps' `super` field, plus the precedence
that resolves a class reachable from two bases.  A category is a property of
the CLASS, and the class hierarchy is in the paks - so `door` is "the
descendants of `BP_InteractionObject_Door_C`" rather than the four door
blueprints somebody happened to notice.  The hand list this replaced knew two
ladder classes and two lift classes, which is why Chapter 5 shipped 0 ladders
and Chapter 4 shipped 0 lifts.

`HAND_FALLBACK` below is the 1.0.0 list, used only when `categories.json` is
absent (a checkout with no paks mounted).  It is deliberately the old, narrow
answer: degrading to fewer markers is safe, silently degrading to a DIFFERENT
answer is not.

`elite` and `hidden` (review item C.9) are produced now:
  * `hidden` = `BP_PickUpActor_Trap_C` and its descendants - the trap that
    looks like an item.  It is a `BP_PickupActor_C` descendant, so the category
    order in `categories.json` claims it before `pickup` does.
  * `elite` = an enemy class whose name is another enemy class' name plus a
    `_High` / `_Special` / `_S` suffix (`MingBing_Dao_High_C` next to
    `MingBing_Dao_C`).  `build_enemies.py` decides that against the class graph
    and records it in `markers/enemies.json`; this module only applies it.
"""

from __future__ import annotations

import json
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
_MARKERS = os.path.join(_HERE, "..", "..", "markers")


# --- the 1.0.0 hand list, kept as the no-paks fallback ---------------------
HAND_FALLBACK = {
    "BP_RebornFire_C": "shrine",

    "BP_treasurebox_C": "chest",
    "BP_ItemRedBox_C": "chest",

    "BP_PickupActor_C": "pickup",
    "BP_PickUpPT_C": "pickup",
    "BP_AutoPickUp_C": "pickup",
    "ItemCollectionBox_C": "pickup",

    "BP_NewPuzzlesDoor_C": "door",

    "BP_Wumen_C": "fog_gate",
    "BP_Wumen_NetworkRang_C": "fog_gate",
    "BP_Wumen_ClientOnly_C": "fog_gate",

    "BP_LadderV2_C": "ladder",
    "BP_Ladder_Child_C": "ladder",

    "BP_WoodenElevator_C": "lift",
    "BP_ElevatorBox_C": "lift",

    # NOT a merchant, despite shipping as one up to 0.9.4: every placed
    # instance carries a read-point id (`NPC_DG_READ08`), the blueprint has no
    # character mesh, its only interaction string is `ui_263` = "Check" and it
    # spawns the `NS_Hint01` hint particle.  It is one of the game's readable
    # notes; see `build_npcs.READ_POINT`.  The game's actual merchant (Tao Qing,
    # `Zhangfangxiansheng_NPC_C` out of `AI/npc/NPC_GuDongShang/`) is a plain
    # `BP_NPC_C` descendant and stays `npc`.
    "DKDC_NPC_C": "note",
    "ReadPointSP_NPC_C": "note",
    "Letter01_NPC_C": "note",

    "BP_FireReed_C": "other",
    "BP_PickUpActor_Trap_C": "hidden",
    "BP_zhuanjingta_C": "other",
    "BP_QiCaiShi_2_C": "other",
    "BP_WoodenExternalPushRod_C": "other",
    "BP_KlesaCleaner_C": "other",
    "BP_StonePillar_C": "other",
}


def _load(name: str, key: str, default):
    p = os.path.join(_MARKERS, name)
    if not os.path.exists(p):
        return default
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f).get(key, default)
    except Exception:                                           # noqa: BLE001
        return default


CLASSES: dict[str, str] = _load("categories.json", "classes", {}) or dict(HAND_FALLBACK)
GENERATED = bool(_load("categories.json", "classes", None))

# class -> True for the tougher variant of another enemy class.
ELITE: set[str] = {c for c, e in (_load("enemies.json", "enemies", {}) or {}).items()
                   if isinstance(e, dict) and e.get("elite")}

# Ordered regex rules, applied when the class table misses. Kept as a safety
# net for classes whose `super` the graph never recorded (`BP_jiaheshang_AI_C`
# has no parent in it), and every hit is COUNTED by the extractor so the gap is
# visible instead of silently patched over by a name heuristic.
PATTERNS = [
    (re.compile(r"^BP_Door"), "door"),
    (re.compile(r"NPC", re.I), "npc"),
]

# `*_AI` sublevels are where the game places spawned characters. The primary
# enemy rule is `categories.json`'s `BP_BaseAI_C` root; this is the fallback for
# a class the graph does not know.
AI_LEVEL = re.compile(r"_AI$")

# Engine and level plumbing: actors that exist in every package and are never a
# marker anywhere. Applied AFTER the class table, which is what makes it safe to
# be this broad - `\w*Box_C` would otherwise swallow `BP_ItemRedBox_C` (a chest)
# and `BP_ElevatorBox_C` (a lift), and both are claimed by the table first.
# It applies in every package, so `Model`, `LevelBounds`, `DCSWorldSettings`,
# `StaticMeshActor` and `LevelSequenceActor` stay out of the "matched no
# category" residue that a coverage report has to be read through.
LEVEL_NOISE = re.compile(
    r"^(StaticMeshActor|SkeletalMeshActor|Actor|Model|LevelBounds|"
    r"DecalActor|NiagaraActor|Emitter|InstancedFoliageActor|Landscape\w*|"
    r"PointLight|SpotLight|RectLight|DirectionalLight|SkyLight|SkyAtmosphere|"
    r"ExponentialHeightFog|VolumetricCloud|WindDirectionalSource|"
    r"SphereReflectionCapture|PostProcessVolume|DCSPostProcessVolume|"
    r"DCSWorldSettings|NavMeshBoundsVolume|RecastNavMesh\w*|"
    r"LevelSequenceActor|CameraActor|CineCameraActor|"
    r"AK\w*|Ak\w*|BP_PatrolPath_C|BP_ProxyActors_C|BP_OptimizedLayer\w*|"
    r"BlueprintStreamLevelLoader_C|LevelScript\w*|"
    r"\w*_Volume_C|\w*Volume_C|\w*Box_C|\w*BoxTrigger\w*|\w*_Land_C|"
    r"Chapter\w+_C|AnimCameraHelper_C|ModelComponent|LevelInstance|"
    r"\w*_Props_C|\w*_Props_\d+_C)$"
)

# Destructible clutter and streaming/optimisation volumes: never markers.
NEVER = re.compile(
    r"^(BP_DestructionObj\w*|BP_Breakenhbh_Obj_C|GeometryCollectionActor|"
    r"LightTriggerBox_C|BP_OptimizedLayerBox_C|BP_CVarOptimizedBox_C|"
    r"BP_TopSlowTickBox_C|PlayerCamerExternController_Box_C|"
    r"BP_TerrainTrap_C|BP_DiCiCombine_C|BP_DiCiTrigger_C|BP_ArrowShooter_C|"
    # Boss-arena plumbing, and neither is a boss: `BP_BossPool_C` is a
    # `Derivative_Negative_C` spawner (one instance in the whole game) and
    # `BossLightingEffectActor_C` is a light rig. Typing either as a boss
    # duplicates the real boss marker beside it.
    r"BP_BossPool_C|BossLightingEffectActor_C|"
    r"DI_\w*)$"
)


def categorise_ex(class_name: str, level_short: str) -> tuple[str | None, str]:
    """`(category, which rule decided it)`.

    The rule name is what makes the extractor's coverage report worth reading:
    `table` means the class graph answered; every other value is a fallback and
    a `rule:` counter in the extraction stats, so a growing fallback share is
    visible rather than something to discover by noticing a wrong count.
    """
    if NEVER.match(class_name):
        return None, "never"
    cat = CLASSES.get(class_name)
    if cat:
        if cat == "enemy" and class_name in ELITE:
            return "elite", "table+elite"
        return cat, "table"
    if LEVEL_NOISE.match(class_name):
        return None, "noise"
    if AI_LEVEL.search(level_short):
        if class_name in ELITE:
            return "elite", "ai-level+elite"
        return "enemy", "ai-level"
    for rx, c in PATTERNS:
        if rx.search(class_name):
            return c, "regex:" + rx.pattern
    return None, "unmatched"


def categorise(class_name: str, level_short: str) -> str | None:
    """Return the marker category, or None if this actor is not a marker."""
    return categorise_ex(class_name, level_short)[0]


# Human-facing label per category, used to build the `name` field.
LABEL = {
    "shrine": "Shrine", "chest": "Chest", "pickup": "Pickup",
    "boss": "Boss", "elite": "Elite", "enemy": "Enemy", "npc": "NPC",
    "note": "Note", "door": "Door", "ladder": "Ladder",
    "lift": "Lift", "fog_gate": "Fog gate", "hidden": "Trap", "other": "Object",
}
