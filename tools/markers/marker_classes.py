#!/usr/bin/env python3
"""Actor class -> marker category, for schema `wuchang-minimap-markers/1`.

Categories: shrine, chest, pickup, boss, elite, enemy, npc, merchant, door,
ladder, lift, fog_gate, hidden, other.

`elite` is deliberately never produced offline: nothing in the cooked data
distinguishes an elite from a normal enemy -- that is a runtime judgement.
"""

from __future__ import annotations

import re

# Exact class -> category.
EXACT = {
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

    "DKDC_NPC_C": "merchant",

    # world mechanisms worth a pin but not a category of their own
    "BP_PickUpActor_Trap_C": "other",
    "BP_zhuanjingta_C": "other",
    "BP_QiCaiShi_2_C": "other",
    "BP_WoodenExternalPushRod_C": "other",
    "BP_KlesaCleaner_C": "other",
    "BP_StonePillar_C": "other",
}

# Ordered regex rules, applied when EXACT misses.
PATTERNS = [
    (re.compile(r"^BP_Door"), "door"),
    (re.compile(r"NPC", re.I), "npc"),
]

# --- enemies / bosses ------------------------------------------------------
# Enemies are not identifiable by class name (the roster is transliterated
# Chinese and open-ended), but they are identifiable by *where they live*:
# only `*_AI` sublevels carry spawned characters.  So inside an `_AI` package
# every actor that is not level plumbing is an enemy, and every actor in a
# `*_BOSS_AI` / `*_Boss_AI` package (or whose class names a boss) is a boss.
AI_LEVEL = re.compile(r"_AI$")
BOSS_LEVEL = re.compile(r"_BOSS_AI$|_Boss_AI$", re.I)
BOSS_CLASS = re.compile(r"boss", re.I)

# Level plumbing that shares the `_AI` packages with the actual spawners.
AI_NOISE = re.compile(
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
    r"DI_\w*)$"
)


def categorise(class_name: str, level_short: str) -> str | None:
    """Return the marker category, or None if this actor is not a marker."""
    if NEVER.match(class_name):
        return None
    cat = EXACT.get(class_name)
    if cat:
        return cat
    if BOSS_LEVEL.search(level_short) or BOSS_CLASS.search(class_name):
        if not AI_NOISE.match(class_name):
            return "boss"
        return None
    if AI_LEVEL.search(level_short):
        if AI_NOISE.match(class_name):
            return None
        return "enemy"
    for rx, c in PATTERNS:
        if rx.search(class_name):
            return c
    return None


# Human-facing label per category, used to build the `name` field.
LABEL = {
    "shrine": "Shrine", "chest": "Chest", "pickup": "Pickup",
    "boss": "Boss", "elite": "Elite", "enemy": "Enemy", "npc": "NPC",
    "merchant": "Merchant", "door": "Door", "ladder": "Ladder",
    "lift": "Lift", "fog_gate": "Fog gate", "hidden": "Hidden", "other": "Object",
}
