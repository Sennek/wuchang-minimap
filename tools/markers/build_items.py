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
import uasset                                               # noqa: E402
from itemdb import SCHEMA                                   # noqa: E402

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
# driver
# ---------------------------------------------------------------------------

def build(ms, lang: str = "en", verbose: bool = True):
    loc = read_locres(ms.read(LOCRES.format(lang=lang)))

    owner: dict[int, str] = {}
    per_table: dict[str, set[int]] = {}
    for name, stem in TABLES:
        rows = table_rows(ms, name, stem)
        per_table[name] = rows
        for rid in rows:
            owner.setdefault(rid, name)         # first table in priority order wins

    items: dict[int, dict] = {}
    named_by_prefix = collections.Counter()
    for rid, table in sorted(owner.items()):
        rec = {"table": table}
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
    a = ap.parse_args(argv)

    ms = pakmaps.MapSource(a.pak)
    items, per_table = build(ms, a.lang)

    doc = {
        "schema": SCHEMA,
        "lang": a.lang,
        "source": "cooked item DataTables + MMGame.locres, offline pak extraction",
        "generated_by": "tools/markers/build_items.py",
        "items": {str(k): v for k, v in sorted(items.items())},
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
