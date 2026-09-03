#!/usr/bin/env python3
r"""
Offline item database for the Wuchang minimap mod -> `markers/items.json`.

Every pickup in the game carries a numeric item ID inline in its cooked `.umap`
export (see `extract_markers.item_ids`).  This tool turns those IDs into English
display names, entirely from the paks:

    row FName of an item DataTable  ->  `<prefix>_<ID>_name` in MMGame.locres

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
     `spell`, `styleskill`, `weaponskill`, `CuiYu`.  They never collide on a
     referenced id, so a fixed priority order resolves every one.

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

# Localisation key prefixes, highest priority first.  Measured key counts in
# `en`: weapon 374, armor 216, item 201, gem 107, spell 39, ring 36,
# weaponskill 34, styleskill 30, CuiYu 26.
KEY_PREFIXES = ("item", "weapon", "armor", "ring", "gem", "spell",
                "styleskill", "weaponskill", "CuiYu")

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
    p = uasset.Package.__new__(uasset.Package)
    p.path = stem
    p.head = ms.read(stem + ".uasset")
    p.uexp = ms.read(stem + ".uexp") if (stem + ".uexp") in ms.owner else b""
    p._parse()
    return p


NUMERIC = re.compile(r"^\d{4,6}$")


def table_rows(ms, name: str, stem: str) -> set[int]:
    """The numeric row FNames of one item DataTable."""
    if (stem + ".uasset") not in ms.owner:
        stem = MIRROR + stem.split("/DataTables/", 1)[1]
    pkg = package(ms, stem)
    classes = {e.class_name for e in pkg.exports}
    if "DataTable" not in classes:
        print(f"  ! {name}: no DataTable export ({sorted(classes)})", file=sys.stderr)
    return {int(n) for n in pkg.names if NUMERIC.match(n)}


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
#     the package name map -- the same "the name map IS the row-name table"
#     trick `table_rows()` uses, now used to find each row's OFFSET.
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


def table_item_types(ms, name, stem, enum):
    """`row id -> ItemType display name` for one item DataTable."""
    if name in TABLE_ITEM_TYPE:
        return {}                               # the caller fills these wholesale
    if (stem + ".uasset") not in ms.owner:
        stem = MIRROR + stem.split("/DataTables/", 1)[1]
    pkg = package(ms, stem)
    d = pkg.data(pkg.exports[0])
    (row_count,) = struct.unpack_from("<i", d, 10)

    row_struct = next(n.rsplit("/", 1)[-1] for n in pkg.names
                      if n.startswith("/Game/") and "/Structs/" in n)
    fields = struct_fields(ms, STRUCT_DIR + row_struct)
    want = next(i for i, (m, _t) in enumerate(fields) if m == "ItemType")

    numeric = {i for i, n in enumerate(pkg.names) if NUMERIC.match(n)}
    out = {}
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
        rid = int(pkg.names[i])
        got = _item_type_at(d, values, end, fields, want, enum)
        prev = out.get(rid)
        if prev is None or prev == "None":
            out[rid] = got
        elif got not in (None, "None") and got != prev:
            raise SystemExit(f"{name}: row {rid} decodes as {prev} and {got}")
        o += 8
    if len(out) != row_count:
        raise SystemExit(f"{name}: found {len(out)} rows, table says {row_count}")
    return {k: (v or "None") for k, v in out.items()}


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

def build(ms, lang: str = "en", verbose: bool = True):
    loc = read_locres(ms.read(LOCRES.format(lang=lang)))
    enum = enum_values(ms)

    owner: dict[int, str] = {}
    per_table: dict[str, set[int]] = {}
    types: dict[int, str] = {}
    for name, stem in TABLES:
        rows = table_rows(ms, name, stem)
        per_table[name] = rows
        fixed = TABLE_ITEM_TYPE.get(name)
        decoded = table_item_types(ms, name, stem, enum)
        for rid in rows:
            owner.setdefault(rid, name)         # first table in priority order wins
            if rid not in types:
                t = fixed or decoded.get(rid)
                if t and t != "None":
                    types[rid] = t

    items: dict[int, dict] = {}
    named_by_prefix = collections.Counter()
    rarities = collections.Counter()
    for rid, table in sorted(owner.items()):
        rec = {"table": table}
        type_name = types.get(rid)
        if type_name:
            rec["type"] = type_name
        rec["rarity"] = itemdb.rarity_of_type(type_name)
        rarities[rec["rarity"]] += 1
        for pre in KEY_PREFIXES:
            nm = loc.get(f"{pre}_{rid}_name")
            if nm:
                rec["name"] = nm
                des = loc.get(f"{pre}_{rid}_des") or loc.get(f"{pre}_{rid}_sum")
                if des:
                    rec["des"] = des
                named_by_prefix[pre] += 1
                break
        items[rid] = rec

    named = sum(1 for r in items.values() if r.get("name"))
    if verbose:
        print(f"  {len(TABLES)} tables, {len(items)} distinct row ids, "
              f"{named} named, {len(items) - named} without a name")
        typed = sum(1 for r in items.values() if r.get("type"))
        print(f"    ItemType decoded for {typed} of {len(items)} ids; rarity "
              + ", ".join(f"{itemdb.RARITY_NAMES[k]}={rarities[k]}"
                          for k in sorted(rarities)))
        for name, _stem in TABLES:
            rows = per_table[name]
            n = sum(1 for r in rows if items[r].get("name"))
            print(f"    {name:26s} {len(rows):5d} rows  {n:5d} named")
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
