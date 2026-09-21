#!/usr/bin/env python3
r"""
Offline item database for the Wuchang minimap mod -> `markers/items.json`.

Every pickup in the game carries a numeric item ID inline in its cooked `.umap`
export (see `extract_markers.item_ids`).  This tool turns those IDs into English
display names, entirely from the paks:

    row of an item DataTable  ->  its `Name` FText key  ->  MMGame.locres

  1. **Row ids.**  The six item DataTables are cooked `.uasset`/`.uexp` pairs.
     A `DataTable`'s row names are `FName`s, so they all sit in the package's
     own name map -- and every row of these tables is named by its numeric id.
     So "the numeric entries of the name map" *is* the row-name table, and no
     property decoding (hence no `.usmap`) is needed.  The counts come out at
     242 / 1656 / 26 / 210 / 54 / 421, exactly the row counts recorded in
     `context/item-names-research.md`.

  2. **Names.**  `Content/Localization/MMGame/en/MMGame.locres` is a locres v3
     file (`Optimized_CRC32`): a header, a namespace/key table whose values are
     indices, and a string table at `StringTableOffset`.  Wuchang puts every
     game string in the **empty namespace**, keyed `<prefix>_<ID>_name` with a
     matching `_des` (long description) and `_sum` (short one).  The prefix is
     the item's *kind*, not its table: `item`, `weapon`, `armor`, `ring`, `gem`,
     `spell`, `styleskill`, `weaponskill`, `CuiYu`.

     The ID in the key is not the row id: 21 rows are keyed by another id, and
     two of those swap names with each other.  So the key is read out of the
     row's own `Name` FText, which serialises it inline -- 929 of the 2 609 rows
     carry one.  The rest (the equipment `+1`..`+N` upgrade rows, and every row
     of the two tables whose struct has no `Name` column at all, `DT_SpellData`
     and `DT_SpecialItem`) fall back to synthesising `<prefix>_<ID>` from the
     row id, with the prefix picked by the row's own `ItemType`
     (`TYPE_KEY_PREFIX`) because prefixes collide on 46 ids.

  3. **Upgrade levels.**  A `+N` item's id is the base row's id plus N, and most of
     them are rows in their own right.  A gem's never is, and ten weapon levels are
     not either, so those are synthesised from the name the locres holds for them
     (`level_items`) -- otherwise a `+1` gem picked up in game has no name at all.

English only, on purpose: the mod's UI is English (task `CLAUDE.md § Goal`).
`--lang` is there for a future translation pass, nothing more.

    python build_items.py                    # -> ..\..\markers\items.json
    python build_items.py --stats            # per-table coverage
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.join(_HERE, "..", "navmesh", "offline"))

import pakmaps                                              # noqa: E402
import provenance                          # noqa: E402
import uasset                                               # noqa: E402
import itemdb                                               # noqa: E402
import pickup_buckets                                       # noqa: E402
from itemdb import SCHEMA                                   # noqa: E402
from uprops import parse_header                             # noqa: E402

# The item DataTables, in the priority order used to attribute an id that is a
# row of more than one of them (`DT_SpecialItem` mirrors rows of the others).
TABLES = [
    ("DT_Item_ToolTable", "Content/Game/DataTables/Items/DT_Item_ToolTable"),
    ("DT_Item_EquipmentTable", "Content/Game/DataTables/Items/DT_Item_EquipmentTable"),
    ("DT_Item_StyleSkillTable", "Content/Game/DataTables/Items/DT_Item_StyleSkillTable"),
    ("DT_GemNew", "Content/Game/DataTables/DT_GemNew"),
    ("DT_SpellData", "Content/Game/DataTables/DT_SpellData"),
    ("DT_SpecialItem", "Content/Game/DataTables/DT_SpecialItem"),
]

# Mirrors of the same tables; used only if a `Content/Game/` copy is missing.
MIRROR = "Content/DynamicCombatSystem/DataTables/"

# Both tables below synthesise a key for the 12 named rows that carry no `Name`
# FText of their own (`DT_SpellData` and `DT_SpecialItem` ids); every other name
# comes from the key the row itself holds, and neither table is consulted.
#
# Localisation key prefixes, highest priority first.  Measured key counts in
# `en`: weapon 374, armor 216, item 201, gem 107, spell 39, ring 36,
# weaponskill 34, styleskill 30, CuiYu 26.  Reached only when the row's
# `ItemType` did not decode -- `TYPE_KEY_PREFIX` answers first.
KEY_PREFIXES = ("item", "weapon", "armor", "ring", "gem", "spell",
                "styleskill", "weaponskill", "CuiYu")

# `E_ItemType` display name -> the key prefix that names that kind of item.
#
# 46 ids carry a `_name` under two prefixes, in two families.  A melee weapon
# also has `weaponskill_<id>_name`, its weapon skill (33 ids); and the thirteen
# spells of `DT_Item_ToolTable` in 24001..24081 share their id with a `+1` gem
# or a tool -- `gem_24041_name` is "Wei - Blood Force +1" where
# `spell_24041_name` is "Crimson Edge Incantation".  A fixed prefix order gets
# the second family wrong every time, so the item's own type picks the prefix.
#
# Total over `E_ItemType`: a new type stops the run rather than silently taking
# whatever the fallback order hands back.  `weaponskill` and `CuiYu` are never
# an item's own kind and appear only in `KEY_PREFIXES`.
TYPE_KEY_PREFIX = {
    "None": "item",
    "Tool": "item",
    "Arrows": "item",
    "Material": "item",
    "EnchantingMaterial": "item",
    "SpecialItem": "item",
    "MeleeWeapon": "weapon",
    "RangeWeapon": "weapon",
    "Shield": "weapon",
    "Head": "armor",
    "Top": "armor",
    "Legs": "armor",
    "Hands": "armor",
    "Feet": "armor",
    "Ring": "ring",
    "Necklace": "ring",
    "Gem": "gem",
    "Spell": "spell",
    "StyleSkill": "styleskill",
}

LOCRES = "Content/Localization/MMGame/{lang}/MMGame.locres"
LOCRES_MAGIC = bytes.fromhex("0e147475674a03fc4a15909dc3377f1b")


# ---------------------------------------------------------------------------
# locres
# ---------------------------------------------------------------------------

def _fstring(b: bytes, o: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<i", b, o)
    o += 4
    if n == 0:
        return "", o
    if n < 0:                                   # UTF-16, length in characters
        return b[o:o - 2 * n].decode("utf-16-le").rstrip("\0"), o - 2 * n
    return b[o:o + n].decode("utf-8", "replace").rstrip("\0"), o + n


def read_locres(blob: bytes) -> dict[str, str]:
    """`key -> localised string` for the empty namespace (which is where every
    Wuchang game string lives).  Namespaced keys are prefixed `<ns>/`."""
    if blob[:16] != LOCRES_MAGIC:
        raise SystemExit("not a locres file (bad magic)")
    o = 16
    version = blob[o]
    o += 1
    if version != 3:
        raise SystemExit(f"locres version {version} not supported (expected 3)")
    (string_table_offset,) = struct.unpack_from("<q", blob, o)
    o += 8
    (entry_count,) = struct.unpack_from("<I", blob, o)
    o += 4
    (ns_count,) = struct.unpack_from("<I", blob, o)
    o += 4

    index: dict[str, int] = {}
    for _ in range(ns_count):
        o += 4                                  # namespace hash
        ns, o = _fstring(blob, o)
        (key_count,) = struct.unpack_from("<I", blob, o)
        o += 4
        for _ in range(key_count):
            o += 4                              # key hash
            key, o = _fstring(blob, o)
            o += 4                              # source-string hash
            (idx,) = struct.unpack_from("<i", blob, o)
            o += 4
            index[key if not ns else f"{ns}/{key}"] = idx
    if len(index) != entry_count:
        print(f"  ! locres: {len(index)} keys parsed, header says {entry_count}",
              file=sys.stderr)

    p = string_table_offset
    (count,) = struct.unpack_from("<i", blob, p)
    p += 4
    strings: list[str] = []
    for _ in range(count):
        s, p = _fstring(blob, p)
        p += 4                                  # reference count (v3)
        strings.append(s)
    if p != len(blob):
        print(f"  ! locres: string table ended at {p} of {len(blob)}", file=sys.stderr)
    return {k: strings[i] for k, i in index.items() if 0 <= i < len(strings)}


# ---------------------------------------------------------------------------
# data tables
# ---------------------------------------------------------------------------

def package(ms: "pakmaps.MapSource", stem: str) -> "uasset.Package":
    """One cooked package by logical stem, falling back to the `MIRROR` copy of
    a DataTable that `Content/Game/` does not carry."""
    if (stem + ".uasset") not in ms.owner and "/DataTables/" in stem:
        stem = MIRROR + stem.split("/DataTables/", 1)[1]
    p = uasset.Package.__new__(uasset.Package)
    p.path = stem
    p.head = ms.read(stem + ".uasset")
    p.uexp = ms.read(stem + ".uexp") if (stem + ".uexp") in ms.owner else b""
    p._parse()
    return p


NUMERIC = re.compile(r"^\d{4,6}$")


# ---------------------------------------------------------------------------
# item type / rarity
# ---------------------------------------------------------------------------
#
# Wuchang has no rarity field (see `itemdb.ITEM_TYPE_RARITY`).  The axis the
# game itself colours pickups by is `ItemType` (`E_ItemType`), and it *is*
# readable without a `.usmap`:
#
#   * a cooked `UserDefinedStruct` export carries its `FField` children in
#     schema order, each as `FName type` + `FName name`, so the row struct's
#     property ORDER falls out of a self-validating scan (the hit count must
#     equal the number of `<Var>_<n>_<32 hex>` names in the package name map);
#   * `ItemType` sits at schema index 0 of `ST_Item_ToolConf` and index 1 of
#     `ST_Item_EquipmentConf` (behind one `IntProperty`), so the unversioned row
#     payload only has to be walked across fixed-size values to reach it;
#   * a row starts with its `FName` row name, which is numeric and therefore in
#     the package name map, so "the name map IS the row-name table" finds each
#     row's OFFSET as well as its id (`row_spans`).
#
# The three tables that carry no `ItemType` column get their type from the table
# itself (`ST_GemNew` is gems, `DT_SpellData` spells, ...).

# Bytes a serialized value of a given property type occupies.  Only the types
# that can precede `ItemType` need to be here; anything else stops the walk.
FIXED_VALUE_SIZE = {
    "BoolProperty": 1, "ByteProperty": 1, "IntProperty": 4, "FloatProperty": 4,
    "DoubleProperty": 8, "ObjectProperty": 4, "ClassProperty": 4,
    "NameProperty": 8,
}

# `<Var>_<creation index>_<32 hex>` -- how the struct editor names a member.
STRUCT_VAR = re.compile(r"^(.+)_\d+_[0-9A-F]{32}$")

# Row structs that declare no `ItemType`; the table fixes the kind instead.
TABLE_ITEM_TYPE = {
    "DT_Item_StyleSkillTable": "StyleSkill",
    "DT_GemNew": "Gem",
    "DT_SpellData": "Spell",
    "DT_SpecialItem": "SpecialItem",
}

STRUCT_DIR = "Content/DynamicCombatSystem/Structs/"
ITEM_TYPE_ENUM = "Content/DynamicCombatSystem/Enumerations/E_ItemType"


def struct_fields(ms, stem):
    """`[(member name, property type)]` of a cooked `UserDefinedStruct`, in
    schema order.  Found by scanning the export for `FName type` + `FName name`
    pairs; accepted only when the scan saw every member of the name map."""
    pkg = package(ms, stem)
    d = pkg.data(pkg.exports[0])
    types = {i: n for i, n in enumerate(pkg.names) if n.endswith("Property")}
    members = {i: STRUCT_VAR.match(n).group(1)
               for i, n in enumerate(pkg.names) if STRUCT_VAR.match(n)}
    out, seen, o = [], set(), 0
    while o <= len(d) - 16:
        a, an, b, bn = struct.unpack_from("<iiii", d, o)
        if an == 0 and bn == 0 and a in types and b in members:
            out.append((members[b], types[a]))
            seen.add(b)
            o += 16
            continue
        o += 1
    if seen != set(members):
        missing = sorted(members[i] for i in set(members) - seen)
        raise SystemExit(f"{stem}: field scan missed {missing}")
    return out


def enum_values(ms, stem=ITEM_TYPE_ENUM):
    """`value -> display name` of a cooked `UserDefinedEnum`.

    The export is `<unversioned props> <DisplayNameMap> ... <Names array>`: the
    map is `FName enumerator -> FText`, and the trailing `Names` array pairs the
    same enumerators with their (compacted, 0..n-1) values -- which is what a
    `ByteProperty` actually stores."""
    pkg = package(ms, stem)
    d = pkg.data(pkg.exports[0])

    def fstring(o):
        (n,) = struct.unpack_from("<i", d, o)
        o += 4
        if n == 0:
            return "", o
        if n < 0:
            return d[o:o - 2 * n].decode("utf-16-le").rstrip("\0"), o - 2 * n
        return d[o:o + n].decode("utf-8", "replace").rstrip("\0"), o + n

    o = 6                                       # 2-byte header + 4-byte value
    (count,) = struct.unpack_from("<i", d, o)
    o += 4
    display = {}
    for _ in range(count):
        idx, num = struct.unpack_from("<ii", d, o)
        o += 8 + 4                              # FName + FText flags
        o += 1                                  # FText history type (0 = Base)
        _ns, o = fstring(o)
        _key, o = fstring(o)
        src, o = fstring(o)
        display[pkg.names[idx] if num == 0 else ""] = src

    names = {}
    for o in range(0, len(d) - 4):
        (n,) = struct.unpack_from("<i", d, o)
        if n != count + 1:                      # the array also holds `_MAX`
            continue
        oo, ent, ok = o + 4, [], True
        for _ in range(n):
            i, num, val = struct.unpack_from("<iiq", d, oo)
            if num != 0 or not (0 <= i < len(pkg.names)) or not (0 <= val <= 64):
                ok = False
                break
            ent.append((pkg.names[i], val))
            oo += 16
        if ok:
            for nm, val in ent:
                short = nm.split("::")[-1]
                if short in display:
                    names[val] = display[short]
            break
    if len(names) != count:
        raise SystemExit(f"{stem}: decoded {len(names)} of {count} enum values")
    return names


def _item_type_at(d, values, end, fields, want, enum):
    """Read schema index `want` (a `ByteProperty`) out of one unversioned row."""
    off = end
    for idx, is_zero in values:
        if idx == want:
            return enum.get(0, "None") if is_zero else enum.get(d[off], "None")
        if is_zero:
            continue
        size = FIXED_VALUE_SIZE.get(fields[idx][1]) if idx < len(fields) else None
        if size is None:
            return None                         # unwalkable prefix
        off += size
    return enum.get(0, "None")                  # not serialized == the default


# The `Name` FText of a row serialises its localisation key inline, and it is
# the first `_name` key inside the row -- `Descript` (`_des`), `DescriptSummery`
# (`_sum`) and `PickupPrompt` follow it in schema order.
NAME_KEY = re.compile(rb"([A-Za-z]+)_(\d+)_name")


def row_spans(pkg, d):
    """`[(offset, row id, values, header end)]` of one cooked item DataTable, in
    payload order.  A row starts with its own numeric row `FName` -- a name-map
    entry with number 0 -- followed by an unversioned property header, and that
    pair is what the scan accepts."""
    numeric = {i: int(n) for i, n in enumerate(pkg.names) if NUMERIC.match(n)}
    out = []
    o = 0
    while o <= len(d) - 8:
        i, num = struct.unpack_from("<ii", d, o)
        if num != 0 or i not in numeric:
            o += 1
            continue
        try:
            values, end = parse_header(d, o + 8)
        except Exception:
            o += 1
            continue
        out.append((o, numeric[i], values, end))
        o += 8
    return out


def read_table(ms, name, stem, enum):
    """Everything one item DataTable says about its rows:
    `(row ids, row id -> ItemType display name, row id -> localisation key)`.

    The row id SET comes from the package name map -- every row of these tables
    is named by its numeric id, so "the numeric entries of the name map" *is*
    the row-name table, and no property decoding (hence no `.usmap`) is needed.
    Walking the payload is what the other two answers need, and it is trusted
    only where it reproduces the table's own declared row count.
    """
    pkg = package(ms, stem)
    classes = {e.class_name for e in pkg.exports}
    if "DataTable" not in classes:
        print(f"  ! {name}: no DataTable export ({sorted(classes)})", file=sys.stderr)
    rows = {int(n) for n in pkg.names if NUMERIC.match(n)}

    d = pkg.data(pkg.exports[0])
    (row_count,) = struct.unpack_from("<i", d, 10)
    spans = row_spans(pkg, d)

    keys = {}
    for k, (o, rid, _v, _e) in enumerate(spans):
        end = spans[k + 1][0] if k + 1 < len(spans) else len(d)
        m = NAME_KEY.search(d, o, end)
        if m and rid not in keys:
            keys[rid] = f"{m.group(1).decode()}_{m.group(2).decode()}"

    if name in TABLE_ITEM_TYPE:
        return rows, {}, keys               # the caller fills the types wholesale

    row_struct = next(n.rsplit("/", 1)[-1] for n in pkg.names
                      if n.startswith("/Game/") and "/Structs/" in n)
    fields = struct_fields(ms, STRUCT_DIR + row_struct)
    want = next(i for i, (m, _t) in enumerate(fields) if m == "ItemType")

    types = {}
    for o, rid, values, end in spans:
        got = _item_type_at(d, values, end, fields, want, enum)
        prev = types.get(rid)
        if prev is None or prev == "None":
            types[rid] = got
        elif got not in (None, "None") and got != prev:
            raise SystemExit(f"{name}: row {rid} decodes as {prev} and {got}")
    if len(types) != row_count:
        raise SystemExit(f"{name}: found {len(types)} rows, table says {row_count}")
    return rows, {k: (v or "None") for k, v in types.items()}, keys


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

def localise(loc, rid: int, type_name: str | None, key: str | None):
    """`(key, name, description)` of one item, or `None` when nothing names it.

    `key` is what the row's own `Name` FText said; a row that carries none is
    keyed by its row id under the prefix its kind implies, with `KEY_PREFIXES`
    behind that for the ids whose `ItemType` did not decode."""
    if key:
        nm = (loc.get(f"{key}_name") or "").strip()
        if nm:
            return key, nm, loc.get(f"{key}_des") or loc.get(f"{key}_sum")
    first = TYPE_KEY_PREFIX.get(type_name or "None")
    order = (first,) + tuple(p for p in KEY_PREFIXES if p != first)
    for pre in order:
        nm = (loc.get(f"{pre}_{rid}_name") or "").strip()
        if nm:
            return f"{pre}_{rid}", nm, (loc.get(f"{pre}_{rid}_des")
                                        or loc.get(f"{pre}_{rid}_sum"))
    return None


# Where the locres keeps the name of upgrade level `lv` of base row `base`, and how much
# that keying proves on its own.
#
# `base + lv` is the next id along, which any neighbouring item could own, so the name it
# hands back must be the base's plus ` +<lv>` exactly -- that is what tells
# "Cloudfrost's Edge +1" (10001, under row 10000) from the Sun Pendant sitting one past
# the Phoenix Pendant.  The thousand-up keying gems use is nobody's neighbour, so the
# offset itself identifies the level and the name only has to end in ` +<lv>`: it is how
# row 23013 "Bu - Skyborn Ward" reaches its own `gem_24013_name`, "Ren - Skyborn
# Ward +1".
LEVEL_KEYINGS = ((lambda base, lv: base + lv, True),
                 (lambda base, lv: base + 1000 * lv, False))

# "Cloudfrost's Edge +9" is the longest ladder the locres carries.
LEVEL_MAX = 10


def level_items(loc, items):
    """`{item id: (base id, level, name, description)}` for the upgrade levels that are
    no row of their own.

    The ITEM ID of a level is always the base row's id plus the level -- measured in
    game, where a live pickup reading 23070 is "Wei - Vitality Power +1" over row 23069.
    Most weapon levels are rows at that same id and need nothing; a gem's never is, and
    a handful of weapons (`Steel Fang +1..+10`) are named without one either.

    Every prefix is tried, rather than the one the base's kind implies: `Steel Fang` is a
    `DT_SpecialItem` row and its levels are named under `weapon_`.  What keeps a
    neighbour's id from being read as somebody's upgrade is the acceptance test each
    keying carries (`LEVEL_KEYINGS`)."""
    out: dict[int, tuple[int, int, str, str | None]] = {}
    for base, rec in sorted(items.items()):
        base_name = rec.get("name")
        if not base_name:
            continue
        for lv in range(1, LEVEL_MAX + 1):
            rid = base + lv
            if rid in items or rid in out:
                continue
            for prefix in KEY_PREFIXES:
                for key_id, exact in LEVEL_KEYINGS:
                    key = f"{prefix}_{key_id(base, lv)}"
                    nm = (loc.get(f"{key}_name") or "").strip()
                    if nm == f"{base_name} +{lv}" or (not exact and nm.endswith(f" +{lv}")):
                        out[rid] = (base, lv, nm,
                                    loc.get(f"{key}_des") or loc.get(f"{key}_sum"))
                        break
                if rid in out:
                    break
    return out


def build(ms, lang: str = "en", verbose: bool = True):
    loc = read_locres(ms.read(LOCRES.format(lang=lang)))
    enum = enum_values(ms)
    unmapped = sorted(set(enum.values()) - set(TYPE_KEY_PREFIX))
    if unmapped:
        raise SystemExit(f"E_ItemType has no key prefix for {unmapped}")

    owner: dict[int, str] = {}
    per_table: dict[str, set[int]] = {}
    types: dict[int, str] = {}
    keys: dict[int, str] = {}
    for name, stem in TABLES:
        rows, decoded, row_keys = read_table(ms, name, stem, enum)
        per_table[name] = rows
        fixed = TABLE_ITEM_TYPE.get(name)
        for rid in rows:
            owner.setdefault(rid, name)         # first table in priority order wins
            if rid not in types:
                t = fixed or decoded.get(rid)
                if t and t != "None":
                    types[rid] = t
            if rid not in keys and row_keys.get(rid):
                keys[rid] = row_keys[rid]

    items: dict[int, dict] = {}
    named_by_prefix = collections.Counter()
    rarities = collections.Counter()
    synthesised = foreign_key = 0
    for rid, table in sorted(owner.items()):
        rec = {"table": table}
        type_name = types.get(rid)
        if type_name:
            rec["type"] = type_name
        rec["rarity"] = itemdb.rarity_of_type(type_name)
        # The marker bucket this item puts a pickup in. Written per ITEM so both
        # sides of the pipeline read one answer: the extractor buckets a placed
        # pickup by `items[0]`, and the mod's live sweep buckets a runtime-spawned
        # one by the first item it grants, off this same field.
        rec["bucket"] = pickup_buckets.bucket_of_type(type_name)
        rarities[rec["rarity"]] += 1
        found = localise(loc, rid, type_name, keys.get(rid))
        if found:
            key, nm, des = found
            rec["name"] = nm
            if des:
                rec["des"] = des
            named_by_prefix[key.split("_", 1)[0]] += 1
            if key != keys.get(rid):
                synthesised += 1
            elif int(key.rsplit("_", 1)[-1]) != rid:
                foreign_key += 1
        items[rid] = rec

    # The upgrade levels, over the rows they hang under: same kind, same bucket, their
    # own name, and `level` saying what they are.
    levels = level_items(loc, items)
    for rid, (base, lv, nm, des) in sorted(levels.items()):
        rec = dict(items[base])
        rec["name"] = nm
        rec.pop("des", None)
        if des:
            rec["des"] = des
        rec["level"] = lv
        items[rid] = rec

    named = sum(1 for r in items.values() if r.get("name"))
    if verbose:
        print(f"  {len(TABLES)} tables, {len(items) - len(levels)} distinct row ids "
              f"+ {len(levels)} upgrade level(s), {named} named, "
              f"{len(items) - named} without a name")
        typed = sum(1 for r in items.values() if r.get("type"))
        print(f"    ItemType decoded for {typed} of {len(items)} ids; rarity "
              + ", ".join(f"{itemdb.RARITY_NAMES[k]}={rarities[k]}"
                          for k in sorted(rarities)))
        for name, _stem in TABLES:
            rows = per_table[name]
            n = sum(1 for r in rows if items[r].get("name"))
            print(f"    {name:26s} {len(rows):5d} rows  {n:5d} named")
        print(f"    named from the row's own key {named - synthesised} "
              f"({foreign_key} of them keyed by another id), synthesised from "
              f"the row id {synthesised}")
        print("    key prefixes used: "
              + ", ".join(f"{p}={c}" for p, c in named_by_prefix.most_common()))
    return items, per_table


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--lang", default="en")
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers",
                                                  "items.json"))
    ap.add_argument("--stats", action="store_true")
    provenance.add_arg(ap)
    a = ap.parse_args(argv)

    ms = pakmaps.MapSource(a.pak)
    items, per_table = build(ms, a.lang)

    doc = {
        "schema": SCHEMA,
        "lang": a.lang,
        "source": "cooked item DataTables + MMGame.locres, offline pak extraction",
        "generated_by": "tools/markers/build_items.py",
        "rarity_names": {str(k): v for k, v in sorted(itemdb.RARITY_NAMES.items())},
        "rarity_colors": {str(k): v for k, v in sorted(itemdb.RARITY_COLORS.items())},
        "rarity_source": ("E_ItemType of the item's DataTable row, grouped the way "
                          "the game's own pickup beam is (DT_Particle LightColor: "
                          "blue / pink / gold)"),
        "items": {str(k): v for k, v in sorted(items.items())},
        **provenance.stamp(ms, a.pak, not a.no_pak_hash),
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    print(f"  -> {a.out}  ({len(items)} ids, "
          f"{os.path.getsize(a.out) / 1024:.0f} KB)")

    if a.stats:
        for name, rows in per_table.items():
            missing = sorted(r for r in rows if not items[r].get("name"))
            print(f"    {name}: {len(missing)} unnamed, first 20 {missing[:20]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
