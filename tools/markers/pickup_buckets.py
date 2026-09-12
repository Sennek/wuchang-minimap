#!/usr/bin/env python3
r"""The pickup buckets: `E_ItemType` -> marker category, and the registry of the
eleven categories that replace the single `pickup`.

Design reference: `.workspace/pickup-subcategories/context/buckets.md`.

TWO RULES, TWO TABLES
---------------------
A pickup that carries an `Items` array takes the bucket of `items[0]`'s
`E_ItemType` - the same first item that already decides the marker's display
name and its `rarity` (`extract_markers.item_name()`, `itemdb.ItemDB.rarity`),
so the bucket agrees with the label the player reads.

A pickup with no `Items` takes the bucket of its actor CLASS, and that table is
not here: it is `build_categories.ROOTS`, applied to the game's own class graph,
so `BP_PickUpPT_Red_C` is a harvest node because it descends from
`BP_PickUpPT_C` rather than because somebody listed it.  `CLASS_BUCKETS` below
only names which of the eleven are reachable that way; `build_categories`
asserts its roots against it.

THE MAP IS TOTAL, AND UNKNOWN IS AN ERROR
-----------------------------------------
Every `E_ItemType` the enum declares has a bucket.  A type with none raises out
of `bucket_of_type()`: a new item type is a real change in the game's data and
the pipeline stops, rather than quietly filing the item under a default and
shipping a marker with the wrong glyph.

THE TIER IS NOT THE BUCKET
--------------------------
`itemdb.ITEM_TYPE_RARITY` is a separate table and stays one.  Over the shipped
data the tier is a strict coarsening of the bucket, but the two disagree in one
cell: `EnchantingMaterial` is a `material` (tier Key) while its rarity is
Common, because the game's pickup beam groups it with the consumables.  No
placed pickup grants one today, so no shipped marker straddles.
"""

from __future__ import annotations

# wire name -> (label, tier)  --  the tier is `itemdb`'s rarity tier, repeated
# here only as documentation of which colour family the bucket draws in.
# Order is the order the buckets read in a report and in the F2 filter: the
# Common family, then Equipment, then Key.
BUCKETS: dict[str, tuple[str, int]] = {
    "consumable": ("Consumable", 0),
    "item":       ("Item", 0),
    "harvest":    ("Harvest", 0),
    "ammo":       ("Cannon ammo", 0),
    "armour":     ("Armour", 1),
    "amulet":     ("Amulet", 1),
    "weapon":     ("Weapon", 1),
    "jade":       ("Jade", 1),
    "spell":      ("Spell", 1),
    "material":   ("Material", 2),
    "key":        ("Key item", 2),
}

ORDER = list(BUCKETS)
LABELS = {k: v[0] for k, v in BUCKETS.items()}

# The buckets an item-less pickup can land in, i.e. the ones `build_categories`
# gives a class root.  Everything else is reachable only through an item type.
CLASS_BUCKETS = ("item", "harvest", "ammo")

# `E_ItemType` display name -> bucket.  The enum is decoded from
# `Content/DynamicCombatSystem/Enumerations/E_ItemType` by `build_items.py`;
# `Arrows`, `Feet`, `Necklace` and `None` carry no rows in `items.json` and are
# mapped so the table stays total over the enum.  `Shield` (33 rows) and
# `RangeWeapon` (155) are cut content - not one row has a localised name - so
# they fold into `weapon` instead of earning buckets of their own.
ITEM_TYPE_BUCKET: dict[str, str] = {
    "None": "item",
    "Tool": "consumable",
    "Arrows": "consumable",
    "Material": "material",
    "EnchantingMaterial": "material",
    "SpecialItem": "key",
    "MeleeWeapon": "weapon",
    "RangeWeapon": "weapon",
    "Shield": "weapon",
    "Head": "armour",
    "Top": "armour",
    "Hands": "armour",
    "Legs": "armour",
    "Feet": "armour",
    "Ring": "amulet",
    "Necklace": "amulet",
    "Gem": "jade",
    "Spell": "spell",
    "StyleSkill": "spell",
}

assert set(ITEM_TYPE_BUCKET.values()) <= set(BUCKETS)
assert set(CLASS_BUCKETS) <= set(BUCKETS)


def bucket_of_type(type_name: str | None) -> str:
    """Bucket of an `E_ItemType` display name. Unknown is fatal, by design."""
    b = ITEM_TYPE_BUCKET.get(type_name or "None")
    if b is None:
        raise SystemExit(
            f"pickup_buckets: no bucket for E_ItemType {type_name!r}. Add it to "
            f"ITEM_TYPE_BUCKET (and to context/buckets.md) - a new item type "
            f"must not fall back to a default.")
    return b
