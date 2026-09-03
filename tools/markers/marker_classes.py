#!/usr/bin/env python3
"""Actor class -> marker category, for schema `wuchang-minimap-markers/1`.

Categories: shrine, chest, pickup, boss, elite, enemy, npc, note, door,
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

    # NOT a merchant, despite shipping as one up to 0.9.4: every placed
    # instance carries a read-point id (`NPC_DG_READ08`), the blueprint has no
    # character mesh, its only interaction string is `ui_263` = "Check" and it
    # spawns the `NS_Hint01` hint particle.  It is one of the game's readable
    # notes; see `build_npcs.READ_POINT`.  The game's actual merchant (Tao Qing,
    # `Zhangfangxiansheng_NPC_C` out of `AI/npc/NPC_GuDongShang/`) is a plain
    # `BP_NPC_C` descendant and stays `npc`.
    "DKDC_NPC_C": "note",
    # Same object, blue hint particle / a letter prop; the `NPC` regex would
    # otherwise type them `npc`.
    "ReadPointSP_NPC_C": "note",
    "Letter01_NPC_C": "note",

    # world mechanisms worth a pin but not a category of their own
    # `BP_FireReed_C` is a lightable reed prop, not a character - it only ever
    # looked like a boss because 8 of them sit in `Chapter1_Wanrenk_BOSS_AI`.
    "BP_FireReed_C": "other",
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
# every actor that is not level plumbing is an enemy.
AI_LEVEL = re.compile(r"_AI$")

# BOSSES ARE A CLASS QUESTION (2026-09-03).  Every boss in the game is a placed
# actor deriving from `BP_PlacedBossAI_C`; this is the complete list of that
# class' descendants, read out of the cooked `.uasset` export maps' `super`
# field.  Regenerate with:
#
#     python class_graph.py --out class_graph.json --children BP_PlacedBossAI_C
#     python build_bosses.py --report        # -> ../../markers/bosses.json
#
# It REPLACES the old `_BOSS_AI` sublevel / "Boss" in the class name heuristic,
# which was wrong in both directions.  It missed chapters 2, 3 and 5 entirely
# (their boss sublevels are named `Chapter3_ZhenWuG_ZhangXianZ_AI`, not
# `_BOSS_AI`), which is the "no bosses in chapters 2/3" the user reported; and
# it typed three non-characters as bosses because of where they sit or what
# they are called -- `BP_FireReed_C` (8 in Chapter 1), `BP_BossPool_C` and
# `BossLightingEffectActor_C`.  `context/markers-offline.md` section 7 blamed
# `BP_BossPool_C` for the missing bosses; there is exactly one of it in the
# whole game, so it never spawned anybody's.
BOSS_CLASSES = {
    "AI_YHJS_BP_C", "BP_Anim_ZXZ_StepA_C", "BP_Anim_ZXZ_StepB_C",
    "BP_BKL_AI_C", "BP_BKL_AI_Special_C", "BP_CZ_AI_2_C", "BP_CZ_AI_3_C",
    "BP_CZ_AI_C", "BP_DaYouYan_AI_C", "BP_Dashuguai_AI_C", "BP_E_LWX_AI_C",
    "BP_Honglan_BossAI_C", "BP_MJJJ_AI_New_C", "BP_NRSL01_AI_C",
    "BP_NRSL_AI_C", "BP_XBFR_AI_C", "BP_XMWC_AI_New_C", "BP_XMWC_LS_AI_C",
    "BP_XYZ_AI_C", "BP_XYZ_AI_S_C", "BP_YHGN_AI_Special_C",
    "BP_YHGN_AI_Special_CJD_C", "BP_YHHL_BossAI_C", "BP_YuHuaXNAI_C",
    "BP_ZY_AI_C", "BP_toutuo_AI_C", "B_ANQ_C", "B_NW_C", "B_XBXN02_C",
    "Boss_Luhongliu_FirstStage_01_C", "Boss_Luhongliu_FirstStage_01_special_C",
    "Boss_Luhongliu_SecondStage_C",
}

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
    # Boss-arena plumbing. Both used to be typed `boss` by the old heuristic:
    # `BP_BossPool_C` is a `Derivative_Negative_C` spawner (one instance in the
    # whole game) and `BossLightingEffectActor_C` is a light rig. Neither is a
    # boss, and pinning either duplicates the real boss marker beside it.
    r"BP_BossPool_C|BossLightingEffectActor_C|"
    r"DI_\w*)$"
)


def categorise(class_name: str, level_short: str) -> str | None:
    """Return the marker category, or None if this actor is not a marker."""
    if NEVER.match(class_name):
        return None
    if class_name in BOSS_CLASSES:
        return "boss"
    cat = EXACT.get(class_name)
    if cat:
        return cat
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
    "note": "Note", "door": "Door", "ladder": "Ladder",
    "lift": "Lift", "fog_gate": "Fog gate", "hidden": "Hidden", "other": "Object",
}
