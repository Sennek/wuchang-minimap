#!/usr/bin/env python3
r"""
Shared reader for `markers/items.json` (schema `wuchang-minimap-items/1`).

`build_items.py` writes the file; `extract_markers.py` reads it for two things:

  * the **membership set** -- an integer that is a real row of one of the item
    DataTables is what makes the inline `Items` array scan safe (see
    `extract_markers.item_ids`);
  * the **display name** of an item id.

Kept in its own module so both tools agree on the shape and neither imports the
other.
"""

from __future__ import annotations

import json
import os

SCHEMA = "wuchang-minimap-items/2"

# --- item quality ("rarity") ------------------------------------------------
#
# Wuchang has NO rarity ladder: there is no `E_ItemQuality` / `Rarity` / `Grade`
# enum anywhere in the paks, no quality word in `MMGame.locres`, and not one of
# the six item row structs declares such a field.  What the game *does* have is
# the pickup beam: `BP_PickupActor_C` picks a `DT_Particle` row (`PickupEffect`,
# `PickupEffect4..6`, `PickupEffect7..9`) whose `LightColor` is blue, pink or
# gold, and the group is decided by the item's `ItemType` (`E_ItemType`).  So
# "rarity" here is the game's own three-colour pickup grouping, and the default
# palette below is the game's own `LightColor`s converted from linear to sRGB.
#
# Evidence (17 instances whose live `PickupEffectName` was captured in the
# WuchangRecon world dumps): every weapon / armour / accessory / gem is pink,
# every `Material` / `SpecialItem` is gold, everything else is blue.  See
# `context/item-names-research.md` section "Rarity".

RARITY_COMMON, RARITY_EQUIPMENT, RARITY_KEY = 0, 1, 2

RARITY_NAMES = {RARITY_COMMON: "Common",
                RARITY_EQUIPMENT: "Equipment",
                RARITY_KEY: "Key"}

# sRGB of the `DT_Particle` `LightColor` of `PickupEffect` / `PickupEffect4` /
# `PickupEffect7` -- linear (0.420, 0.428, 0.700) / (0.700, 0.420, 0.560) /
# (0.701, 0.672, 0.418).
RARITY_COLORS = {RARITY_COMMON: "ADAFDA",
                 RARITY_EQUIPMENT: "DAADC5",
                 RARITY_KEY: "DAD6AD"}

# `E_ItemType` display name -> rarity tier.  The enum itself is decoded from
# `Content/DynamicCombatSystem/Enumerations/E_ItemType` by `build_items.py`.
ITEM_TYPE_RARITY = {
    "None": RARITY_COMMON,
    "Tool": RARITY_COMMON,
    "Arrows": RARITY_COMMON,
    "EnchantingMaterial": RARITY_COMMON,
    "Spell": RARITY_EQUIPMENT,
    "Shield": RARITY_EQUIPMENT,
    "Head": RARITY_EQUIPMENT,
    "Top": RARITY_EQUIPMENT,
    "Legs": RARITY_EQUIPMENT,
    "Hands": RARITY_EQUIPMENT,
    "Feet": RARITY_EQUIPMENT,
    "Ring": RARITY_EQUIPMENT,
    "Necklace": RARITY_EQUIPMENT,
    "MeleeWeapon": RARITY_EQUIPMENT,
    "RangeWeapon": RARITY_EQUIPMENT,
    "Gem": RARITY_EQUIPMENT,
    "StyleSkill": RARITY_EQUIPMENT,
    "Material": RARITY_KEY,
    "SpecialItem": RARITY_KEY,
}


def rarity_of_type(type_name: str | None) -> int:
    """Tier of an `E_ItemType` display name; anything unknown is Common."""
    return ITEM_TYPE_RARITY.get(type_name or "", RARITY_COMMON)

DEFAULT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "markers", "items.json")


class ItemDB:
    """`items.json` in memory.  An empty instance is legal and answers "unknown"
    to everything, so the extractor still runs before the file is built."""

    __slots__ = ("items",)

    def __init__(self, items: dict | None = None):
        # int id -> {"name": str, "des": str, "table": str};  name/des optional
        self.items: dict[int, dict] = items or {}

    def __len__(self) -> int:
        return len(self.items)

    def __contains__(self, item_id: int) -> bool:
        return item_id in self.items

    def name(self, item_id: int) -> str | None:
        rec = self.items.get(item_id)
        n = rec.get("name") if rec else None
        return n or None

    def table(self, item_id: int) -> str | None:
        rec = self.items.get(item_id)
        return rec.get("table") if rec else None

    def type_name(self, item_id: int) -> str | None:
        rec = self.items.get(item_id)
        return rec.get("type") if rec else None

    def rarity(self, item_id: int) -> int:
        """Quality tier of an item id; unknown ids are Common (0)."""
        rec = self.items.get(item_id)
        if not rec:
            return RARITY_COMMON
        r = rec.get("rarity")
        return r if isinstance(r, int) else RARITY_COMMON

    @classmethod
    def load(cls, path: str | None = None) -> "ItemDB":
        p = path or DEFAULT_PATH
        if not os.path.exists(p):
            return cls()
        with open(p, encoding="utf-8") as fh:
            doc = json.load(fh)
        if doc.get("schema") != SCHEMA:
            raise SystemExit(f"{p}: unexpected schema {doc.get('schema')!r}")
        return cls({int(k): v for k, v in doc.get("items", {}).items()})
