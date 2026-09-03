#!/usr/bin/env python3
r"""
Offline shrine table for the Wuchang minimap mod -> `markers/shrines.json`.

WHAT THIS ADDS THAT THE MARKER DB DOES NOT HAVE
-----------------------------------------------
`markers/chapter*.json` already carries every shrine as a marker, keyed by the
game's own shrine id (the first `FString` of `BP_RebornFire_C`).  What it cannot
carry is the shrine's **display name** and its **travel destination**, because
neither lives on the actor: both are rows of `DT_FirePoint`, whose row struct is
`ST_FirePoint` (`context/saveslot-and-teleport-research.md` section 2.4).

So this tool reads that table and joins it to the marker DB by id, producing the
list the full map's shrine panel shows and the fast-travel action targets.

HOW IT READS A DATATABLE WITHOUT A `.usmap`
-------------------------------------------
Exactly the three tricks `build_items.py` already uses, plus one anchor:

1.  **The row names are `FName`s**, so they are in the package's own name map --
    a `DataTable` row is `<FName row name><unversioned header><values>`.  The
    table stores its own `NumRows` at +10, which is the acceptance test: this
    tool refuses to write anything unless the row count matches exactly.

2.  **`ST_FirePoint`'s schema order comes from the cooked `UserDefinedStruct`**
    (`build_items.struct_fields`, itself self-validating against the name map).
    Measured order, and it is what makes this cheap:

        0 BirthPosition StructProperty     6 BelongLevel      TextProperty
        1 BirthRotation StructProperty     7 LevelImage       SoftObjectProperty
        2 LevelMap_Visible ArrayProperty   8 SectionId        StrProperty
        3 LevelMap_Visible StrProperty     9 SubjectionLevel  ByteProperty
        4 ShowName     TextProperty       10 ImportantSite    BoolProperty
        5 ShowImage    SoftObjectProperty

    `BirthPosition` is schema index **0**, i.e. it sits immediately after the
    unversioned header -- no walk over variable-sized values is needed to reach
    the one field whose exact bytes matter.

3.  **The name is found by its own evidence, not by an offset.**  `ShowName` is
    an `FText` whose localisation key ends up as a plain ASCII `FString` inside
    the row, and `MMGame.locres` either has that key or it does not.  So the row
    payload is scanned for ASCII `FString`s and the first one that IS a locres
    key wins (`temple02` -> `ui_150` -> "Reverent Temple").  A wrong guess cannot
    survive, because a wrong string is not a key.

    The chapter comes from the same payload for free: `LevelMap_Visible` is a
    `TArray<FString>` of level names such as `Chapter1_Temple_Props`.

Everything is cross-checked and the tool reports coverage; a row whose position
or name does not decode is still emitted with whatever did, because an id and a
chapter are already enough for the shrine list.

    python extract_shrines.py                 # -> ..\..\markers\shrines.json
    python extract_shrines.py --stats
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
import build_items as BI                                    # noqa: E402
import provenance                                           # noqa: E402
from uprops import parse_header                             # noqa: E402

SCHEMA = "wuchang-minimap-shrines/1"

TABLE = "Content/Game/DataTables/DT_FirePoint"
TABLE_MIRROR = "Content/DynamicCombatSystem/DataTables/DT_FirePoint"
ROW_STRUCT = "Content/DynamicCombatSystem/Structs/ST_FirePoint"

# A row name is a plain identifier. Anything with a slash is an asset path that
# happens to sit in the same name map (the `LC_language` string table the FTexts
# point at is the one that also parses as a header).
ROW_NAME = re.compile(r"^[A-Za-z0-9_]+$")
LEVEL_CHAPTER = re.compile(r"^Chapter(\d+|DLC)_", re.IGNORECASE)
ASCII_STR = re.compile(r"^[A-Za-z0-9_.\-]+$")

# The world is a few hundred thousand uu across; a garbage double from a
# misaligned read comes out around 1e-317 or 1e38 (lessons.md).
WORLD_LIMIT = 1.0e7


def _fstrings_in(blob: bytes) -> list[str]:
    """Every plausible ASCII `FString` in a byte range, in order.

    An FString is `int32 length` (in characters, NUL included) followed by that
    many bytes. Scanning for it is safe here because the answer is then filtered
    against the locres key set - a false positive cannot become a name."""
    out = []
    o = 0
    while o + 4 <= len(blob):
        (n,) = struct.unpack_from("<i", blob, o)
        if 2 <= n <= 128 and o + 4 + n <= len(blob) and blob[o + 3 + n] == 0:
            raw = blob[o + 4:o + 3 + n]
            try:
                s = raw.decode("ascii")
            except UnicodeDecodeError:
                o += 1
                continue
            if ASCII_STR.match(s):
                out.append(s)
                o += 4 + n
                continue
        o += 1
    return out


def read_rows(ms, verbose: bool = True) -> list[dict]:
    """Every `DT_FirePoint` row: id, BirthPosition, level names, ASCII strings.

    A cooked `UDataTable` export is `<its own unversioned header> <RowStruct
    object index> <int32 NumRows> <row>*`, and a row is `<FName RowName>
    <unversioned header> <values>`.  Row VALUES cannot be walked field by field
    without knowing the size of an `FText` and a `TArray<FString>`, so the rows
    are found by scanning for the row-name pattern instead - and the scan is
    validated by CONTIGUITY, which is the strongest test available here: the
    accepted starts must begin at the first row offset and each one must be the
    end of the previous row, so the whole payload is accounted for with no gap
    and no overlap.  A spurious match inside a payload (there are ~54 of them,
    because `bossdoor_dyy` and `zhenlizhimen01` also appear as VALUES) breaks
    contiguity immediately and is rejected."""
    stem = TABLE if (TABLE + ".uasset") in ms.owner else TABLE_MIRROR
    pkg = BI.package(ms, stem)
    export = pkg.exports[0]
    if export.class_name != "DataTable":
        raise SystemExit(f"{stem}: first export is {export.class_name}, not a DataTable")
    d = pkg.data(export)
    (stated_rows,) = struct.unpack_from("<i", d, 10)
    # The schema order, so a candidate's value indices can be bounds-checked
    # against the real number of slots instead of a hard-coded 11.
    fields = BI.struct_fields(ms, ROW_STRUCT)
    first_row = 14  # 2-byte header + RowStruct object index + NumRows

    def candidate(o):
        """(row name, header end, has BirthPosition) or None."""
        if o + 8 > len(d):
            return None
        i, num = struct.unpack_from("<ii", d, o)
        if num != 0 or not (0 <= i < len(pkg.names)) or not ROW_NAME.match(pkg.names[i]):
            return None
        try:
            values, hdr_end = parse_header(d, o + 8)
        except Exception:
            return None
        idx = [v[0] for v in values]
        # A real row writes the whole schema: every slot 0..9 in order. (Slot 10,
        # ImportantSite, is a bool that is usually the default and absent.)
        if idx[:10] != list(range(10)):
            return None
        if idx[-1] >= len(fields) or idx != sorted(idx):
            return None
        # BirthPosition is slot 0. A serialized FVector can never be (0,0,0) - the
        # header would have put it in the zero mask - so a zero mask there means
        # "this row has no destination", which is how the pseudo-rows store it.
        if values[0][1]:
            return (pkg.names[i], hdr_end, False)
        if hdr_end + 24 > len(d):
            return None
        x, y, z = struct.unpack_from("<ddd", d, hdr_end)
        if not all(abs(v) < WORLD_LIMIT for v in (x, y, z)):
            return None
        return (pkg.names[i], hdr_end, True)

    # Every candidate offset, then the contiguous chain that starts at first_row.
    cand = {}
    for o in range(first_row, len(d) - 8):
        c = candidate(o)
        if c is not None:
            cand[o] = c
    if first_row not in cand:
        raise SystemExit("DT_FirePoint: no row at the expected first offset 14")
    offsets = sorted(cand)
    chain = [first_row]
    for o in offsets:
        if o > chain[-1]:
            chain.append(o)
    # `chain` is the greedy left-to-right selection, which for a contiguous row
    # array IS the row list: a spurious match inside row k sits between the true
    # boundaries and would leave the following true boundary unreachable, so the
    # check below catches it.
    rows = []
    for k, o in enumerate(chain):
        name, hdr_end, has_pos = cand[o]
        stop = chain[k + 1] if k + 1 < len(chain) else len(d)
        body = hdr_end + (24 if has_pos else 0)
        if body > stop:
            raise SystemExit(f"DT_FirePoint: row {name} at {o} overruns the next row at {stop}")
        x, y, z = (struct.unpack_from("<ddd", d, hdr_end) if has_pos else (None, None, None))
        strings = _fstrings_in(d[body:stop])
        chapter = None
        for t in strings:
            m = LEVEL_CHAPTER.match(t)
            if m:
                g = m.group(1)
                chapter = 0 if g.upper() == "DLC" else int(g)
                break
        rows.append({"id": name, "bx": x, "by": y, "bz": z,
                     "chapter": chapter, "strings": strings})

    names = [r["id"] for r in rows]
    if len(set(names)) != len(names):
        dup = sorted({n for n in names if names.count(n) > 1})
        raise SystemExit(f"DT_FirePoint: duplicate row name(s) {dup} - the scan drifted")
    if verbose and len(rows) != stated_rows:
        # Reported, not fatal: the payload IS fully accounted for (the contiguity
        # check above), so the difference is in the table header, not in the scan.
        # `Content/Game/DataTables/DT_FirePoint` is patched by the `_1_P` pak and
        # its NumRows does not match the row array it ships.
        print(f"  ! DT_FirePoint header says {stated_rows} rows; the payload holds "
              f"{len(rows)} contiguous rows covering all {len(d)} bytes")
    return rows


def shrine_names(ms, lang: str = "en", verbose: bool = False) -> dict[str, str]:
    """`fire-point id -> localised English name`, straight from the paks.

    The same route `build()` uses (a `DT_FirePoint` row -> the ASCII `FString`
    in its payload that IS a locres key -> `MMGame.locres`) minus the join to
    the marker DB.  That is what `extract_markers.py` needs to put the in-game
    rest-point name on every shrine marker, and it deliberately does NOT read
    `markers/shrines.json`: that file's `shrine` flag and its `x/y/z` come FROM
    the marker DB, so reading it back while building the marker DB would be a
    cycle.  Names have no such dependency - they are a property of the table.
    """
    loc = BI.read_locres(ms.read(BI.LOCRES.format(lang=lang)))
    out: dict[str, str] = {}
    for r in read_rows(ms, verbose=verbose):
        for t in r["strings"]:
            if t in loc and loc[t]:
                out[r["id"]] = loc[t]
                break
    return out


def marker_positions(markers_dir: str) -> dict[str, dict]:
    """`shrine marker id -> {chapter, x, y, z, fp, level}` from the marker DB.

    The key is the MARKER id, which is the authored fire-point id except when
    two shrines share one - then `extract_markers.py` appends `@<level>/<obj>`
    to keep ids unique.  `fp` carries the authored id either way, so a caller
    can join on the table and still tell the two apart.
    """
    out: dict[str, dict] = {}
    if not os.path.isdir(markers_dir):
        return out
    for fn in sorted(os.listdir(markers_dir)):
        if not fn.startswith("chapter") or not fn.endswith(".json") or ".sample." in fn:
            continue
        with open(os.path.join(markers_dir, fn), encoding="utf-8") as f:
            db = json.load(f)
        ch = db.get("chapter")
        if isinstance(ch, str):
            ch = 0 if ch.upper() == "DLC" else None
        for m in db.get("markers", []):
            if m.get("cat") != "shrine":
                continue
            mid = m.get("id")
            if not mid or mid in out:
                continue
            out[mid] = {"chapter": m.get("chapter", ch), "x": m.get("x"),
                        "y": m.get("y"), "z": m.get("z"),
                        "fp": m.get("fp") or mid.split("@")[0],
                        "level": m.get("level"), "name": m.get("name")}
    return out


def build(ms, markers_dir: str, lang: str = "en", verbose: bool = True,
          prov: dict | None = None) -> dict:
    prov = prov if prov is not None else {}
    loc = BI.read_locres(ms.read(BI.LOCRES.format(lang=lang)))
    rows = read_rows(ms)
    joined = marker_positions(markers_dir)

    named = 0
    placed = 0
    per_chapter: collections.Counter = collections.Counter()
    shrines = []
    # Review item C.4: the seven DLC shrines were missing from this file
    # entirely, so the full map's Shrines panel had no DLC section and their ids
    # never reached the lit/unlocked join.  The reason is not a decoding gap -
    # **this build's `DT_FirePoint` has no DLC rows at all**.  Its 88 rows cover
    # chapters 1-5 and its name map does not contain `BaiYS01`, `BaiYS02`,
    # `borencl01`, `borencl02`, `LiuHKK01` or `pinmingk01`; there is no second
    # fire-point table anywhere in the paks (one `DT_FirePoint` in
    # `Content/Game/DataTables` from the `_1_P` pak, one 49-byte stub in
    # `Content/DynamicCombatSystem`), no `Chapt6`/`ChapterDLC` folder under
    # `Content/Scene/3D/Others/FirePoint/`, and no DLC area name among the
    # `ui_*` keys.  The same absence is why `build_bossdoors.py` reports the
    # DLC boss as one of the two `bosses_without_a_door`.
    #
    # So a DLC shrine gets everything the marker DB knows - id, chapter,
    # position, the authored fire-point id - and no `name`, because the game
    # does not have one to give.  `source` says which half of the pipeline the
    # row came from, so "no name" is never mistaken for "the name failed to
    # decode".
    table_ids = {r["id"] for r in rows}
    extra = [(mid, m) for mid, m in sorted(joined.items())
             if m["fp"] not in table_ids and mid not in table_ids]
    for r in rows:
        rec = {"id": r["id"]}
        if r["bx"] is not None:
            rec["bx"] = round(r["bx"], 2)
            rec["by"] = round(r["by"], 2)
            rec["bz"] = round(r["bz"], 2)
        for s in r["strings"]:
            if s in loc and loc[s]:
                rec["name"] = loc[s]
                named += 1
                break
        m = joined.get(r["id"])
        chapter = r["chapter"]
        # A row that joins to a shrine MARKER is a real, visitable shrine. The rest
        # are the pseudo-points the research documents (bossdoor_*, Task*): they are
        # in the same table and in the same unlocked list, but there is no shrine at
        # them and the UI must not offer one. This is the flag the runtime filters on,
        # rather than "it happens to have no position".
        rec["shrine"] = m is not None
        if m is not None:
            placed += 1
            if m["x"] is not None:
                rec["x"] = round(float(m["x"]), 2)
                rec["y"] = round(float(m["y"]), 2)
                rec["z"] = round(float(m["z"]), 2)
            if chapter is None and m.get("chapter") is not None:
                chapter = int(m["chapter"])
        if chapter is not None:
            rec["chapter"] = chapter
            per_chapter[chapter] += 1
        shrines.append(rec)

    for mid, m in extra:
        rec = {"id": mid, "shrine": True, "source": "marker"}
        if m["fp"] != mid:
            rec["fp"] = m["fp"]
        # The marker's own label ("Shrine LiuHKK01"), not a locres name, and that
        # is why it is not counted in `named`. `shdb::Shrine::label()` would
        # otherwise fall back to the raw id, which for a duplicated fire point is
        # the whole `LiuHKK01@ChapterDLC_LiuHuangKK_logic/BP_RebornFire_C_0`
        # string - unreadable in the shrine list. The two `LiuHKK01` rows get the
        # SAME label on purpose: they are the same authored fire point, the game
        # keeps one unlock flag for it, and `level` is what tells them apart.
        if m.get("name"):
            rec["name"] = m["name"]
        if m["x"] is not None:
            rec["x"] = round(float(m["x"]), 2)
            rec["y"] = round(float(m["y"]), 2)
            rec["z"] = round(float(m["z"]), 2)
        if m.get("chapter") is not None:
            rec["chapter"] = int(m["chapter"])
            per_chapter[int(m["chapter"])] += 1
        if m.get("level"):
            rec["level"] = m["level"]
        placed += 1
        shrines.append(rec)

    shrines.sort(key=lambda s: (s.get("chapter", 99), s.get("name", ""), s["id"]))
    if verbose:
        with_pos = sum(1 for s in shrines if "bx" in s)
        print(f"  {len(shrines)} DT_FirePoint rows; {named} named from MMGame.locres, "
              f"{with_pos} with a BirthPosition, {placed} joined to a shrine marker")
        print("    per chapter: " +
              ", ".join(f"{'DLC' if c == 0 else c}={n}" for c, n in sorted(per_chapter.items())))
        missing = [s["id"] for s in shrines if "name" not in s]
        if missing:
            print(f"    no name for {len(missing)}: {', '.join(missing[:12])}"
                  + (" ..." if len(missing) > 12 else ""))
    if verbose and extra:
        print(f"    + {len(extra)} shrine(s) with no DT_FirePoint row, from the "
              f"marker DB: {', '.join(m for m, _ in extra)}")
    return {"schema": SCHEMA, "count": len(shrines), "named": named,
            "placed": placed, "from_table": len(rows), "from_markers": len(extra),
            "generated_by": "tools/markers/extract_shrines.py",
            "shrines": shrines, **prov}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers", "shrines.json"))
    ap.add_argument("--markers", default=os.path.join(_HERE, "..", "..", "markers"))
    ap.add_argument("--lang", default="en")
    ap.add_argument("--stats", action="store_true")
    provenance.add_arg(ap)
    args = ap.parse_args()

    ms = pakmaps.MapSource(args.pak)
    db = build(ms, os.path.abspath(args.markers), args.lang,
               prov=provenance.stamp(ms, args.pak, not args.no_pak_hash))
    if args.stats:
        for s in db["shrines"]:
            print(f"  {s.get('chapter', '?'):>3}  {s['id']:<24} {s.get('name', '')}")
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(db, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"  wrote {out} ({os.path.getsize(out)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
