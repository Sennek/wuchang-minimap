#!/usr/bin/env python3
r"""
Offline marker database for the Wuchang minimap mod.

Reads the cooked `.umap` sublevels straight out of the game's paks (no game
running, no `.usmap`, no FModel) and emits `markers/chapter<N>.json` in schema
`wuchang-minimap-markers/1`.

    python extract_markers.py --chapter 1 --out ..\..\markers
    python extract_markers.py --all-chapters --out ..\..\markers
    python extract_markers.py --chapter 1 --census      # actor class histogram

How the positions come out without a usmap: see `uprops.py`'s module docstring
and `context/markers-offline.md` in the task workspace.  In one line: the
unversioned-property *header* is self-describing, `USceneComponent`'s block sits
at a per-class offset that the data itself reveals, and `RelativeLocation` is
five slots into that block.

Join key with the runtime: `obj` -- the cooked export name, which is exactly
what `FindAllOf` reports in-game (`...PersistentLevel.BP_RebornFire_C_0`).
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import re
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import pakmaps                                              # noqa: E402
import marker_classes                                       # noqa: E402
from uprops import (Schema, compose, find_strings, finite_vec, parse_header,   # noqa: E402
                    read_vec, SC_ATTACH_PARENT, SC_ATTACH_SOCKET,
                    SC_COMPONENT_VELOCITY, SC_REL_LOCATION, SC_REL_ROTATION,
                    SC_REL_SCALE3D)

SCHEMA = "wuchang-minimap-markers/1"
CELL_UU = 10240.0

# byte sizes of the USceneComponent-block values we know for certain
KNOWN_SIZES = {SC_ATTACH_PARENT: 4, SC_ATTACH_SOCKET: 8,
               SC_REL_LOCATION: 24, SC_REL_ROTATION: 24, SC_REL_SCALE3D: 24,
               SC_COMPONENT_VELOCITY: 24}

SHRINE_ID = re.compile(r"^[A-Za-z][A-Za-z0-9_]{2,23}$")
SHRINE_ID_SKIP = re.compile(r"^(BP_|Mark_|SM_|WB_|DI_)")


# ---------------------------------------------------------------------------
# package selection
# ---------------------------------------------------------------------------

def chapter_packages(ms: "pakmaps.MapSource", chapter: str) -> list[str]:
    """Every cooked sublevel that belongs to a chapter, minus the navmesh cells
    (`Maps/Generate/`, which hold `RecastNavMeshDataChunk` only)."""
    tag = f"Chapter{chapter}"
    out = []
    for k in ms.umaps():
        if "/Generate/" in k:
            continue
        base = os.path.basename(k)[:-len(".umap")]
        if base.startswith(tag) or f"/{tag}/" in k or f"/{tag}_" in k:
            out.append(k)
    return sorted(set(out))


def short_name(umap_key: str) -> str:
    return os.path.basename(umap_key)[:-len(".umap")]


# ---------------------------------------------------------------------------
# component decoding
# ---------------------------------------------------------------------------

class CompInfo:
    __slots__ = ("values", "hend", "payload", "shift", "pos", "run", "key")


def comp_info(pkg, ex, schema: Schema) -> CompInfo | None:
    shift = schema.shift.get(ex.class_name)
    if shift is None:
        return None
    data = pkg.data(ex)
    try:
        values, hend = parse_header(data)
    except Exception:
        return None
    ci = CompInfo()
    ci.values = values
    ci.hend = hend
    ci.payload = data[hend:]
    ci.shift = shift
    ci.pos = {i: k for k, (i, _z) in enumerate(values)}
    iloc = shift + SC_REL_LOCATION
    if iloc not in ci.pos or values[ci.pos[iloc]][1]:
        ci.run = []
        ci.key = None
        return ci
    run = [iloc]
    for nxt in (shift + SC_REL_ROTATION, shift + SC_REL_SCALE3D):
        p = ci.pos.get(nxt)
        if p == ci.pos[run[-1]] + 1 and not values[p][1]:
            run.append(nxt)
        else:
            break
    ci.run = run
    # a group of exports that must share one byte offset for RelativeLocation:
    # same class, same prefix of serialized values, same run length
    ci.key = (ex.class_name, tuple(values[:ci.pos[iloc]]), len(run))
    return ci


def walk_offset(ci: CompInfo, target: int) -> int | None:
    """Byte offset of `target` by summing the sizes of the preceding values --
    exact when every one of them is a value whose size we know."""
    p = ci.pos.get(target)
    if p is None:
        return None
    off = 0
    for i, z in ci.values[:p]:
        if z:
            continue
        sz = KNOWN_SIZES.get(i - ci.shift)
        if sz is None:
            return None
        off += sz
    return off


# Every cooked export ends with 8 bytes after its last property value (measured:
# 138/138 of the Chapter-1 components whose transform offset is known exactly by
# forward walking end exactly 8 bytes past `RelativeRotation`).  That makes the
# end of the payload a second, independent anchor -- and the decisive one for
# classes whose *preceding* properties are variable-size structs (`FBodyInstance`
# on every shape component, which is why forward walking alone cannot place a
# character's `CollisionCylinder`).
EXPORT_TRAILER = 8


def back_offset(ci: CompInfo):
    """Offset of the run counted back from the end of the payload -- valid only
    when nothing but implicit zeroes follows it."""
    if not ci.run:
        return None
    tail = 0
    for i, z in ci.values[ci.pos[ci.run[-1]] + 1:]:
        if z:
            continue
        sz = KNOWN_SIZES.get(i - ci.shift)
        if sz is None:
            return None
        tail += sz
    o = len(ci.payload) - EXPORT_TRAILER - tail - 24 * len(ci.run)
    return o if o >= 0 else None


def plausible_at(ci: CompInfo, o: int) -> bool:
    """Does the loc(/rot/scale) run decode to sane values at this byte offset?"""
    run = ci.run
    if o < 0 or o + 24 * len(run) > len(ci.payload):
        return False
    loc = read_vec(ci.payload, o)
    # a serialized RelativeLocation can never be (0,0,0): the unversioned header
    # would have flagged it in the zero mask instead of writing 24 bytes
    if not finite_vec(loc, 2e6) or loc == (0.0, 0.0, 0.0):
        return False
    for k, idx in enumerate(run[1:], start=1):
        v = read_vec(ci.payload, o + 24 * k)
        if idx == ci.shift + SC_REL_ROTATION:
            # degrees, and UE does NOT wrap them on save (a measured yaw of
            # 447.34 on a Chapter-1 NPC), so the bound is loose on purpose
            if not finite_vec(v, 1e4):
                return False
        elif not all(math.isfinite(c) and 1e-3 <= abs(c) <= 1e4 for c in v):
            return False
    return True


def scan_candidates(ci: CompInfo) -> list[int]:
    """Every byte offset at which the loc(/rot/scale) run decodes to plausible
    values.  Narrowed to one answer by intersecting across the whole group."""
    run = ci.run
    n_before = sum(1 for i, z in ci.values[:ci.pos[run[0]]] if not z)
    n_after = sum(1 for i, z in ci.values[ci.pos[run[-1]] + 1:] if not z)
    hi = len(ci.payload) - 24 * len(run) - n_after
    return [o for o in range(n_before, hi + 1) if plausible_at(ci, o)]


class Offsets:
    """Learned byte offset of `RelativeLocation` per (class, value-prefix).

    Exports that share a class *and* the exact list of values written before
    `RelativeLocation` must share its byte offset, because the property layout
    is identical.  So the true offset is a candidate in **every** export of the
    group while spurious ones (a run of bytes in a preceding property that
    happens to decode to sane doubles) are per-instance noise.  Voting across
    the group is what turns a fuzzy scan into a decision."""

    def __init__(self):
        self.votes: dict[tuple, collections.Counter] = {}
        self.seen: collections.Counter = collections.Counter()
        self.exact: dict[tuple, int] = {}

    def observe(self, ci: CompInfo) -> None:
        if not ci.run:
            return
        w = walk_offset(ci, ci.run[0])
        if w is not None and plausible_at(ci, w):
            self.exact[ci.key] = w
            return
        b = back_offset(ci)
        if b is not None and plausible_at(ci, b):
            return                       # resolved per export, no group needed
        self.seen[ci.key] += 1
        v = self.votes.setdefault(ci.key, collections.Counter())
        for o in scan_candidates(ci):
            v[o] += 1

    def offset(self, ci: CompInfo):
        if ci.key in self.exact:
            o = self.exact[ci.key]
            if plausible_at(ci, o):
                return o, "walk", 1
        b = back_offset(ci)
        if b is not None and plausible_at(ci, b):
            return b, "back", 1
        v = self.votes.get(ci.key)
        if not v:
            # this export was resolved by the walk for its group but does not
            # fit here: fall back to its own scan
            own = scan_candidates(ci)
            return (own[-1], "self", len(own)) if own else (None, "none", 0)
        top = max(v.values())
        best = sorted(o for o, n in v.items() if n == top and plausible_at(ci, o))
        if not best:
            return None, "none", 0
        # unanimous across the group is the normal case; on a tie the later
        # offset is the safer read (spurious hits sit inside earlier properties)
        return best[-1], ("group" if top == self.seen[ci.key] else "vote"), len(best)


def transform(ci: CompInfo, off: int):
    loc = read_vec(ci.payload, off)
    rot = (0.0, 0.0, 0.0)
    scl = (1.0, 1.0, 1.0)
    if len(ci.run) > 1:
        rot = read_vec(ci.payload, off + 24)
    if len(ci.run) > 2:
        scl = read_vec(ci.payload, off + 48)
    return loc, rot, scl


# ---------------------------------------------------------------------------
# per-package extraction
# ---------------------------------------------------------------------------

def fit_shifts(pkgs, schema: Schema, verbose=False) -> int:
    """Second-chance schema learning for component classes that never write the
    `UActorComponent` tail (e.g. `DCSFlatSoftCapsuleComponent`, the root of most
    of this game's characters -- without it a whole actor's position is lost).

    Fit `shift` by trying every serialized index `i` as `RelativeLocation` and
    keeping the one where, in **every** export of the class, the 8-byte-trailer
    anchor puts a valid `FVector` + `FRotator` pair exactly there."""
    seen = collections.defaultdict(list)
    for lvl in pkgs.values():
        for e in lvl.pkg.exports:
            if e.class_name in schema.shift:
                continue
            try:
                values, hend = parse_header(lvl.pkg.data(e))
            except Exception:
                continue
            seen[e.class_name].append((values, lvl.pkg.data(e)[hend:]))
    found = 0
    for cls, items in seen.items():
        best = None
        idxs = {i for values, _p in items for i, z in values if not z}
        for i in sorted(idxs):
            ok = bad = 0
            longest = 0
            for values, payload in items:
                pos = {v: k for k, (v, _z) in enumerate(values)}
                if i not in pos or values[pos[i]][1]:
                    continue
                run = 1
                while (pos.get(i + run) == pos[i] + run
                       and not values[pos[i + run]][1] and run < 3):
                    run += 1
                if any(not z for _v, z in values[pos[i] + run:]):
                    continue                      # no end anchor here
                o = len(payload) - EXPORT_TRAILER - 24 * run
                if o < 0:
                    continue
                loc = read_vec(payload, o) if o + 24 <= len(payload) else None
                good = (loc is not None and finite_vec(loc, 2e6)
                        and loc != (0.0, 0.0, 0.0))
                if good and run > 1:
                    good = finite_vec(read_vec(payload, o + 24), 1e4)
                if good:
                    ok += 1
                    longest = max(longest, run)
                else:
                    bad += 1
            # a lone FVector is too weak a signature; require that at least one
            # export shows the FVector+FRotator pair at the anchor
            if ok and not bad and longest >= 2:
                if best is None or (ok, longest) > (best[0], best[2]):
                    best = (ok, i, longest)
        if best:
            schema.shift[cls] = best[1] - SC_REL_LOCATION
            found += 1
            if verbose:
                print(f"    fitted shift {best[1] - SC_REL_LOCATION:4d} for "
                      f"{cls} ({best[0]} exports)")
    return found


class Level:
    def __init__(self, key, pkg):
        self.key = key
        self.short = short_name(key)
        self.pkg = pkg
        self.level_outers = {e.index + 1 for e in pkg.exports if e.class_name == "Level"}
        self.children = collections.defaultdict(list)
        for e in pkg.exports:
            self.children[e.outer].append(e)

    def actors(self):
        for e in self.pkg.exports:
            if e.outer in self.level_outers:
                yield e


def root_component(lvl: Level, actor, schema: Schema, cache: dict):
    """The actor's root: its scene-component child with no `AttachParent`.
    Falls back to an attached component (whose chain is then composed)."""
    roots, attached = [], []
    for c in lvl.children.get(actor.index + 1, []):
        ci = cache.get(c.index)
        if ci is None or not ci.run:
            continue
        if (ci.shift + SC_ATTACH_PARENT) in ci.pos:
            attached.append((c, ci))
        else:
            roots.append((c, ci))
    if len(roots) == 1:
        return roots[0], False
    if roots:
        # prefer the conventional root names, then the first in export order
        for want in ("DefaultSceneRoot", "Scene", "Root", "RootComponent",
                     "CollisionCylinder", "CapsuleComponent"):
            for c, ci in roots:
                if c.name == want:
                    return (c, ci), False
        return roots[0], False
    if attached:
        return attached[0], True
    return None, False


def world_location(lvl: Level, comp, ci, offs: Offsets, schema: Schema, cache: dict):
    """World location of a component, composing any attachment chain."""
    off, how, n = offs.offset(ci)
    if off is None:
        return None, how, n
    loc, rot, scl = transform(ci, off)
    depth = 0
    cur_comp, cur_ci = comp, ci
    while depth < 8:
        pidx = cur_ci.shift + SC_ATTACH_PARENT
        if pidx not in cur_ci.pos:
            break
        po = walk_offset(cur_ci, pidx)
        if po is None:
            break
        import struct
        (pkg_index,) = struct.unpack_from("<i", cur_ci.payload, po)
        if pkg_index <= 0:                       # import or null: nothing to compose
            break
        parent = lvl.pkg.exports[pkg_index - 1]
        pci = cache.get(parent.index)
        if pci is None or not pci.run:
            break
        poff, _h, _n = offs.offset(pci)
        if poff is None:
            break
        ploc, prot, pscl = transform(pci, poff)
        loc = compose(ploc, prot, pscl, loc)
        cur_comp, cur_ci = parent, pci
        depth += 1
    return loc, how, n


def shrine_id(pkg, actor) -> str | None:
    """The game-authored id (`digong01`) carried as a plain `FString` inside a
    `BP_RebornFire_C` export.  BP schemas are unknown without a usmap, but a
    string literal in the payload is unambiguous."""
    for _off, s in find_strings(pkg.data(actor), 4, 26):
        if SHRINE_ID.match(s) and not SHRINE_ID_SKIP.match(s):
            return s          # always the first string in the export
    return None


def cell_of(x: float, y: float, chapter: str) -> str:
    return (f"B{chapter}EX0_L0_X{int(math.floor(x / CELL_UU))}"
            f"_Y{int(math.floor(y / CELL_UU))}")


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

def extract(ms, chapter: str, verbose=True):
    keys = chapter_packages(ms, chapter)
    t0 = time.time()
    pkgs = {}
    for k in keys:
        try:
            pkgs[k] = Level(k, ms.package(k))
        except Exception as exc:                                # noqa: BLE001
            print(f"  ! {k}: {exc}", file=sys.stderr)
    # pass 1 - class schema shifts
    schema = Schema()
    for lvl in pkgs.values():
        for e in lvl.pkg.exports:
            try:
                v, _ = parse_header(lvl.pkg.data(e))
            except Exception:
                continue
            schema.observe(e.class_name, v)
    fitted = fit_shifts(pkgs, schema)
    # pass 2 - component info + RelativeLocation offsets
    offs = Offsets()
    infos: dict[str, dict] = {}
    for k, lvl in pkgs.items():
        cache = {}
        for e in lvl.pkg.exports:
            ci = comp_info(lvl.pkg, e, schema)
            if ci is not None:
                cache[e.index] = ci
                offs.observe(ci)
        infos[k] = cache
    # pass 3 - emit
    markers = []
    stats = collections.Counter()
    for k, lvl in pkgs.items():
        cache = infos[k]
        for actor in lvl.actors():
            cat = marker_classes.categorise(actor.class_name, lvl.short)
            if cat is None:
                continue
            stats["candidate"] += 1
            got = root_component(lvl, actor, schema, cache)
            if got[0] is None:
                stats["no-root"] += 1
                stats["no-root:" + actor.class_name] += 1
                continue
            (comp, ci), was_attached = got
            loc, how, n = world_location(lvl, comp, ci, offs, schema, cache)
            if loc is None or not finite_vec(loc, 2e6):
                stats["no-loc"] += 1
                continue
            stats["ok:" + how] += 1
            if n > 1:
                stats["ambiguous"] += 1
            sid = shrine_id(lvl.pkg, actor) if cat == "shrine" else None
            if cat == "shrine" and sid is None:
                stats["shrine-no-id"] += 1
            mid = sid if sid else f"{lvl.short}/{actor.name}"
            label = marker_classes.LABEL.get(cat, cat)
            name = f"{label} {sid}" if sid else label
            markers.append({
                "id": mid,
                "cat": cat,
                "cls": actor.class_name,
                "name": name,
                "obj": actor.name,
                "x": round(loc[0], 2),
                "y": round(loc[1], 2),
                "z": round(loc[2], 2),
                "cell": cell_of(loc[0], loc[1], chapter),
                "level": lvl.short,
            })
            stats["cat:" + cat] += 1
    markers.sort(key=lambda m: (m["cat"], m["level"], m["obj"]))
    # ids must be unique: two DLC shrines really do share `LiuHKK01`
    byid = collections.Counter(m["id"] for m in markers)
    for m in markers:
        if byid[m["id"]] > 1:
            stats["dup-id"] += 1
            m["id"] = f'{m["id"]}@{m["level"]}/{m["obj"]}'

    if verbose:
        print(f"  chapter {chapter}: {len(pkgs)} packages, {len(markers)} markers, "
              f"{time.time() - t0:.1f}s")
    return markers, stats, schema, pkgs


def census(ms, chapter):
    keys = chapter_packages(ms, chapter)
    cnt = collections.Counter()
    where = collections.defaultdict(set)
    for k in keys:
        pkg = ms.package(k)
        lvl = Level(k, pkg)
        for a in lvl.actors():
            cnt[a.class_name] += 1
            where[a.class_name].add(lvl.short)
    for c, n in cnt.most_common():
        cat = marker_classes.categorise(c, "")
        print(f"{n:6d}  {c:48s} {cat or '-':10s} {sorted(where[c])[:2]}")
    print(f"{len(cnt)} classes, {sum(cnt.values())} actors, {len(keys)} packages")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--chapter", default="1")
    ap.add_argument("--all-chapters", action="store_true")
    ap.add_argument("--out", default=os.path.join(_HERE, "..", "..", "markers"))
    ap.add_argument("--census", action="store_true")
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args(argv)

    ms = pakmaps.MapSource(a.pak)
    if a.census:
        census(ms, a.chapter)
        return 0

    chapters = ["1", "2", "3", "4", "5", "DLC"] if a.all_chapters else [a.chapter]
    os.makedirs(a.out, exist_ok=True)
    for ch in chapters:
        markers, stats, _schema, _pkgs = extract(ms, ch)
        if not markers:
            print(f"  chapter {ch}: nothing extracted, skipped")
            continue
        doc = {
            "schema": SCHEMA,
            "chapter": int(ch) if ch.isdigit() else ch,
            "source": "cooked .umap sublevels, offline pak extraction",
            "generated_by": "tools/markers/extract_markers.py",
            "markers": markers,
        }
        dst = os.path.join(a.out, f"chapter{ch.lower()}.json")
        with open(dst, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, ensure_ascii=False, indent=1)
        print(f"  -> {dst}  ({len(markers)} markers)")
        if a.stats:
            for k2, v in sorted(stats.items()):
                print(f"      {k2:32s} {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
