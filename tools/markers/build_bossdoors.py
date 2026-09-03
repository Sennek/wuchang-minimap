#!/usr/bin/env python3
r"""bossdoor_<abbr> -> the boss actor it belongs to, from the game's own level data.

WHY THIS FILE EXISTS
--------------------
The save's `RebornManagerComponent_C::UnlockedFirepoints` list carries, beside
the 57 real shrine ids, 24 `bossdoor_<abbr>` pseudo-points (plus 15 `Task*`
ones).  They are the only *save-backed* per-boss state this project has found,
and the mod needs them because a boss the player killed BEFORE installing the
mod never spawns again - so the runtime health read (`Rule::BossPawn`) can
never fire for it and the marker draws as not-found forever.

WHERE THE MAPPING LIVES - AND IT IS AUTHORED, NOT GUESSED
---------------------------------------------------------
Every `Chapter*_<Area>_logic.umap` holds one level-script actor whose class
carries an array of `ST_LevelScriptBossData` (see the struct's own cooked
export: `Boss机关大门` mechanism door, `Boss激活触发盒` activation trigger box,
`Boss雾门` fog gate, `雾门Mark标记` fog-gate tag, `隐藏击败过的尸体` "hide the
corpse of a defeated boss", `单个Boss数据` -> `ST_BossLevelScript`, and
**`Boss门坐佛点`** - "boss-door sitting-Buddha point", i.e. the firepoint id).
The cooked payload of that one export is plain enough to read with no `.usmap`:

    ... 雾门Mark标记       FString   "wumen_honglan"
    ... BOSS               FSoftObjectPath
                             FName  /Game/Maps/Chapter1_logic/Chapter1_TangW_BOSS_AI
                             FName  Chapter1_TangW_BOSS_AI
                             FString "PersistentLevel.BP_Honglan_BossAI_C_1"
    ... Boss命名           FString   "BP_Honglan_BossAI"
    ... respawn transform  doubles
    ... Boss门坐佛点       FString   "bossdoor_hl"

`<AssetName FName>/<the part after "PersistentLevel.">` is **exactly** the
marker id `extract_markers.py` writes into `markers/chapter*.json`
(`<level short name>/<cooked object name>`), so the join needs no position
matching and no abbreviation guessing.  The abbreviation is used only as an
independent WITNESS in the report (`bossdoor_hl` -> `BP_Honglan_...`), never as
the rule - per lessons.md, a heuristic over class names must be scored against
a witness the data already carries, not trusted.

ACCEPTANCE, so a wrong offset cannot ship
-----------------------------------------
A candidate pair is kept only when the id it forms is a boss marker that the
shipped manifests already contain.  Anything else is reported and dropped.

WHAT THE ID MEANS - READ THIS BEFORE TRUSTING IT
------------------------------------------------
This tool answers *which boss a `bossdoor_*` id belongs to*.  It does NOT
answer *when the game unlocks it* - entering the arena, dying to the boss, or
killing it.  The offline evidence is in
`context/boss-defeat-from-save.md`; the runtime treats an unlocked id as
"defeated" behind a config key and says so in the log.

Usage
    python build_bossdoors.py [--out ../../markers/bossdoors.json] [--pak PATH]
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import pakmaps  # noqa: E402

SCHEMA = "wuchang-minimap-bossdoors/1"

# The sub-path a level actor's FSoftObjectPath carries.
_SUBPATH = b"PersistentLevel."


def _fstring_at(b: bytes, at: int) -> "tuple[str, int] | None":
    """Read an FString whose int32 length starts at `at`; ASCII only.

    Returns (value, offset just past the terminating null) or None.
    """
    if at + 4 > len(b):
        return None
    n = struct.unpack_from("<i", b, at)[0]
    if n <= 0 or n > 512 or at + 4 + n > len(b):
        return None
    raw = b[at + 4 : at + 4 + n]
    if raw[-1] != 0:
        return None
    try:
        return raw[:-1].decode("ascii"), at + 4 + n
    except UnicodeDecodeError:
        return None


def soft_paths(payload: bytes, names: "list[str]") -> "list[tuple[int, str, str]]":
    """Every FSoftObjectPath in the payload that points at a level actor.

    Yields (offset of the record, level short name, actor object name).  The
    record is (FName package, FName asset, FString subpath), and FName is an
    (int32 index, int32 number) pair - so the asset FName sits 8 bytes before
    the subpath's length field.
    """
    out = []
    for m in re.finditer(re.escape(_SUBPATH), payload):
        # the FString length field is 4 bytes before the text
        lp = m.start() - 4
        if lp < 16:
            continue
        got = _fstring_at(payload, lp)
        if got is None or not got[0].startswith("PersistentLevel."):
            continue
        sub = got[0][len("PersistentLevel.") :]
        if not sub or "." in sub:
            continue
        idx, num = struct.unpack_from("<ii", payload, lp - 8)
        if num != 0 or not (0 <= idx < len(names)):
            continue
        out.append((lp - 16, names[idx], sub))
    return out


def bossdoor_ids(payload: bytes) -> "list[tuple[int, str]]":
    out = []
    for m in re.finditer(rb"bossdoor_[a-z0-9_]{1,24}\x00", payload):
        lp = m.start() - 4
        if lp < 0:
            continue
        got = _fstring_at(payload, lp)
        if got is not None:
            out.append((lp, got[0]))
    return out


def scan(ms, boss_ids: "set[str]", verbose=True):
    """bossdoor id -> {marker id, level, obj, class} for every logic level."""
    found: dict[str, dict] = {}
    stats = collections.Counter()
    rejected: list[dict] = []
    keys = [k for k in ms.umaps() if k.endswith("_logic.umap")]
    for k in sorted(keys):
        try:
            pkg = ms.package(k)
        except Exception as exc:  # noqa: BLE001
            print(f"  ! {k}: {exc}", file=sys.stderr)
            continue
        for e in pkg.exports:
            if not e.class_name.startswith("LevelScript"):
                continue
            payload = pkg.uexp[e.uexp_offset : e.uexp_offset + e.serial_size]
            doors = bossdoor_ids(payload)
            if not doors:
                continue
            paths = soft_paths(payload, pkg.names)
            for at, did in doors:
                stats["door"] += 1
                # the boss this door belongs to is the nearest soft object path
                # BEFORE the id inside the same ST_LevelScriptBossData element
                before = [p for p in paths if p[0] < at]
                hit = None
                for off, lvl, obj in reversed(before):
                    mid = f"{lvl}/{obj}"
                    if mid in boss_ids:
                        hit = (lvl, obj, mid, at - off)
                        break
                if hit is None:
                    stats["unresolved"] += 1
                    rejected.append({"id": did, "logic": os.path.basename(k),
                                     "nearest": [f"{l}/{o}" for _o, l, o in before[-3:]]})
                    if verbose:
                        print(f"  ? {did:22s} no boss marker among "
                              f"{[f'{l}/{o}' for _o, l, o in before[-3:]]}")
                    continue
                lvl, obj, mid, gap = hit
                prev = found.get(did)
                if prev is not None and prev["marker"] != mid:
                    stats["conflict"] += 1
                    if verbose:
                        print(f"  ! {did}: {prev['marker']} vs {mid}")
                    continue
                found[did] = {"marker": mid, "level": lvl, "obj": obj,
                              "logic": os.path.basename(k)[:-len(".umap")],
                              "gap": gap}
                stats["mapped"] += 1
    return found, stats, rejected


def load_boss_markers() -> "dict[str, dict]":
    """Every boss marker in the shipped manifests, by id."""
    out = {}
    for p in sorted(glob.glob(os.path.join(_HERE, "..", "..", "markers", "chapter*.json"))):
        if ".sample." in os.path.basename(p):
            continue
        with open(p, encoding="utf-8") as f:
            doc = json.load(f)
        ch = doc.get("chapter", 0)
        for m in doc.get("markers", []):
            if m.get("cat") == "boss":
                out[m["id"]] = {"cls": m.get("cls", ""), "name": m.get("name", ""),
                                "chapter": ch, "x": m.get("x", 0.0),
                                "y": m.get("y", 0.0), "z": m.get("z", 0.0),
                                "level": m.get("level", "")}
    return out


def nearest_boss_by_position(bosses: "dict[str, dict]") -> "dict[str, str]":
    """The SECOND, independent witness: bossdoor id -> nearest boss marker.

    `markers/shrines.json` carries every `DT_FirePoint` row's `BirthPosition`,
    including the `bossdoor_*` pseudo-rows and their chapter, so "which boss
    marker is closest to this door's respawn point, within the door's own
    chapter" is an answer derived from completely different bytes than the
    level-script scan.  Chapters overlap in world space (Chapter 4 covers
    nearly all of Chapter 1), which is exactly why the chapter has to
    constrain it - see lessons.md on `contains(x, y)`.
    """
    p = os.path.join(_HERE, "..", "..", "markers", "shrines.json")
    if not os.path.exists(p):
        return {}
    with open(p, encoding="utf-8") as f:
        rows = json.load(f).get("shrines", [])
    out = {}
    for r in rows:
        rid = str(r.get("id", ""))
        if not rid.startswith("bossdoor_"):
            continue
        best, bestd = None, None
        for mid, b in bosses.items():
            if b["chapter"] != r.get("chapter"):
                continue
            d = ((b["x"] - r["bx"]) ** 2 + (b["y"] - r["by"]) ** 2
                 + (b["z"] - r["bz"]) ** 2)
            if bestd is None or d < bestd:
                best, bestd = mid, d
        if best is not None:
            out[rid] = best
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers",
                                                  "bossdoors.json"))
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    bosses = load_boss_markers()
    if not bosses:
        print("no boss markers in markers/chapter*.json - run extract_markers.py first",
              file=sys.stderr)
        return 2
    ms = pakmaps.MapSource(args.pak)
    found, stats, rejected = scan(ms, set(bosses), verbose=not args.quiet)
    nearest = nearest_boss_by_position(bosses)

    doors = {}
    for did, info in sorted(found.items()):
        b = bosses[info["marker"]]
        # Two independent witnesses, both REPORTED and neither used as the rule:
        # the abbreviation against the boss class' own code, and the nearest
        # boss marker to the door's own `BirthPosition` inside its chapter.
        abbr = did[len("bossdoor_") :].lower()
        cls = b["cls"].lower()
        doors[did] = {"marker": info["marker"], "level": info["level"],
                      "obj": info["obj"], "cls": b["cls"], "name": b["name"],
                      "chapter": b["chapter"], "logic": info["logic"],
                      "abbr_in_class": abbr in cls,
                      "nearest_agrees": nearest.get(did) == info["marker"],
                      "nearest_says": nearest.get(did, "")}

    by_marker: dict[str, list[str]] = collections.defaultdict(list)
    for did, d in doors.items():
        by_marker[d["marker"]].append(did)

    # ---- MULTI-STAGE BOSSES SHARE THEIR ARENA'S DOOR -----------------------
    #
    # Zhang Xianzhong, Zhu Youjian / The Reborn and Fang Ling are each TWO
    # placed boss actors in one `*_AI` sublevel, and the level script names only
    # the one it drives, so the sibling would draw as not-found forever beside
    # its twin. A boss actor in the same `*_AI` level as a mapped one is in the
    # same arena behind the same door by construction, so it inherits the id -
    # marked `via: "level"` so the runtime and the next reader can tell an
    # authored mapping from an inherited one.
    inherited = {}
    level_of_door: dict[str, str] = {}
    for did, d in doors.items():
        level_of_door.setdefault(d["level"], did)
    for mid, b in sorted(bosses.items()):
        if mid in by_marker:
            continue
        did = level_of_door.get(b["level"])
        if did is None:
            continue
        inherited[mid] = did
        by_marker[mid].append(did)
    stats["inherited"] = len(inherited)

    # The table `extract_markers.py` actually stamps onto a boss marker.
    markers = {}
    for did, d in doors.items():
        markers[d["marker"]] = {"bossdoor": did, "via": "authored"}
    for mid, did in inherited.items():
        markers[mid] = {"bossdoor": did, "via": "level"}

    doc = {
        "schema": SCHEMA,
        "source": ("cooked *_logic.umap level-script exports "
                   "(ST_LevelScriptBossData: Boss门坐佛点 + the boss' FSoftObjectPath), "
                   "joined to markers/chapter*.json boss ids"),
        "count": len(doors),
        "witness_abbr_in_class": sum(1 for d in doors.values() if d["abbr_in_class"]),
        "witness_nearest_agrees": sum(1 for d in doors.values() if d["nearest_agrees"]),
        "bosses_with_a_door": len(by_marker),
        "bosses_total": len(bosses),
        "inherited": len(inherited),
        "doors": doors,
        "markers": markers,
        "bosses_without_a_door": sorted(m for m in bosses if m not in by_marker),
    }
    out = os.path.abspath(args.out)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    if not args.quiet:
        print(f"\n{out}")
        print(f"  {len(doors)} boss doors mapped; witnesses: "
              f"{doc['witness_abbr_in_class']} abbr-in-class, "
              f"{doc['witness_nearest_agrees']}/{len(doors)} nearest-boss-in-chapter")
        for did, d in sorted(doors.items()):
            if not d["nearest_agrees"]:
                print(f"    ! {did}: authored {d['marker']}, "
                      f"nearest says {d['nearest_says'] or '(none)'}")
        print(f"  {len(by_marker)} of {len(bosses)} boss markers have a door "
              f"({len(inherited)} inherited from an arena sibling)")
        for m in doc["bosses_without_a_door"]:
            print(f"    no door: {m}  ({bosses[m]['name']})")
        print(f"  stats: {dict(stats)}")
        for r in rejected:
            print(f"    unresolved: {r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
