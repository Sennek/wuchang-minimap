#!/usr/bin/env python3
r"""The game's invisible walls -> `markers/blocks.json`, one oriented box per actor.

WHAT THIS GAME USES FOR A BOUNDARY
----------------------------------
Not volumes.  A sweep of all 835 cooked `.umap`s finds no `BlockingVolume`,
`KillZVolume`, `PainCausingVolume`, `TriggerVolume` or `NavArea*` anywhere, and
one `NavModifierVolume`.  `NavMeshBoundsVolume` says where navmesh is generated -
a superset of the play space, not a bound on it.

What the game ships instead is a level tree whose whole job is the invisible
wall: `Maps/Project_Landscape/Chapter1_Block.umap` and
`Maps/Chapter{2..5}/Block/*.umap` - 40 levels, ~31 000 `StaticMeshActor`.
99.5 % of them reference `/Engine/BasicShapes/Cube` or `/Game/Global/Mesh/
ENV_B_Cube`, both a 100 uu cube, so a blocker is an oriented box: the component's
`RelativeLocation`, `RelativeRotation` and `RelativeScale3D` with a half-extent
of `50 * scale`.

The other 0.5 % are not cubes, and reading them as one is not a rounding error.
32 of them are `*_Nanite_Block` Megascan rock and mountain meshes whose own
half-extents reach 3 015 x 1 651 x 2 149 uu; at the scales they are placed with,
the cube reading shrinks a mountain to a 30 uu pebble.  Six of chapter 1's 22 fence
one plateau the map was drawing, and the mask never saw them.  So every mesh a
blocker references that is not a cube is read for its own `FBoxSphereBounds` and
carried in `mesh_bounds`: origin and half-extent in the mesh's unscaled frame.

HOW THE TRANSFORM IS READ WITHOUT A `.usmap`
--------------------------------------------
`extract_markers.py`'s machinery, with one gap filled.  `Schema.observe` learns a
class' `shift` from the `UActorComponent` tail every component export ends with -
and `StaticMeshComponent` in these levels never writes that tail, so the class
has no shift and `extract_markers` reads no position for it.  `fit_shifts`' own
second chance does not close it either: it needs the run to be the last non-zero
value in the export, and 329 of chapter 1's exports write one more property after
`RelativeScale3D`.

So the shift is fitted here against a witness the data cannot fake: **the world
the boxes are supposed to bound**.  Every consecutive triple of serialized
indices present in nearly every export is tried as
`RelativeLocation`/`Rotation`/`Scale3D`, resolved to a byte offset by
`extract_markers.Offsets`' group vote, and scored on how many exports then decode
to a location inside the chapter's own map bounds from `maps/maps.json` with a
sane rotation and scale.  The right index wins by orders of magnitude, and the
margin is printed.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import struct
import sys
import time
from typing import Iterable

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import pakmaps                                                      # noqa: E402
import extract_markers as em                                        # noqa: E402
from uprops import parse_header, read_vec, finite_vec, SC_REL_LOCATION  # noqa: E402

_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
DEFAULT_OUT = os.path.join(_ROOT, "markers", "blocks.json")
MAPS_JSON = os.path.join(_ROOT, "maps", "maps.json")

# Both cube meshes are 100 uu on a side, centred on the component. Named by their
# object name, which is what a property points at.
CUBE_MESHES = ("Cube", "ENV_B_Cube")
CUBE_HALF_UU = 50.0
# `FBoxSphereBounds` sits at no fixed offset in a cooked `StaticMesh`, so `bounds_at` sweeps this
# many bytes of the export's head and validates every candidate. The floor rejects the denormal
# triples that would otherwise satisfy the sphere test.
MESH_BOUNDS_WINDOW = 4096
MESH_BOUNDS_FLOOR = 1.0

CHAPTERS = ("1", "2", "3", "4", "5")
# A fitted shift must decode this share of the class' exports inside the chapter
# bounds before it is believed.
FIT_MIN_SHARE = 0.80
# ... and beat the runner-up by this factor.
FIT_MIN_MARGIN = 4.0


def block_levels(ms: "pakmaps.MapSource") -> dict[str, list[str]]:
    """Chapter number -> its `*_Block` levels."""
    out: dict[str, list[str]] = collections.defaultdict(list)
    for k in ms.umaps():
        base = os.path.basename(k)[: -len(".umap")]
        if "/Generate/" in k or not (base.endswith("_Block") or "/Block/" in k):
            continue
        for c in CHAPTERS:
            if base.startswith(f"Chapter{c}") or f"/Chapter{c}/" in k:
                out[c].append(k)
                break
    return {c: sorted(v) for c, v in sorted(out.items())}


def chapter_bounds(chapter: str) -> tuple[float, float, float, float] | None:
    """The shipped map's world bbox, grown by half its size - the witness the
    shift is fitted against. A blocker may sit outside the navmesh but not in
    another world."""
    try:
        entry = json.load(open(MAPS_JSON, encoding="utf-8"))["chapters"][f"chapter{chapter}"]
    except (OSError, KeyError, ValueError):
        return None
    mx = (entry["max_x"] - entry["min_x"]) * 0.5
    my = (entry["max_y"] - entry["min_y"]) * 0.5
    return (entry["min_x"] - mx, entry["min_y"] - my,
            entry["max_x"] + mx, entry["max_y"] + my)


def candidate_shifts(exports: list[tuple], min_share: float = 0.9) -> list[int]:
    """Serialized indices that could be `RelativeLocation`: the start of a
    consecutive non-zero triple written by nearly every export."""
    n = len(exports)
    runs: collections.Counter = collections.Counter()
    for values, _payload in exports:
        pos = {i: k for k, (i, z) in enumerate(values) if not z}
        for i in pos:
            if pos.get(i + 1) == pos[i] + 1 and pos.get(i + 2) == pos[i] + 2:
                runs[i] += 1
    return [i for i, c in runs.items() if c >= n * min_share]


def plausible_box(loc, rot, scl, box) -> bool:
    x0, y0, x1, y1 = box
    return (finite_vec(loc, 2e6) and x0 <= loc[0] <= x1 and y0 <= loc[1] <= y1
            and finite_vec(rot, 1e4)
            and all(1e-4 <= abs(c) <= 1e4 for c in scl))


def fit_shift(pkgs, cls: str, box, verbose: bool) -> tuple[int | None, dict]:
    """The `shift` for `cls`, scored on how many exports decode inside `box`."""
    exports = []
    for lvl in pkgs:
        for e in lvl.pkg.exports:
            if e.class_name != cls:
                continue
            try:
                values, hend = parse_header(lvl.pkg.data(e))
            except Exception:                                       # noqa: BLE001
                continue
            exports.append((values, lvl.pkg.data(e)[hend:]))
    if not exports:
        return None, {"exports": 0}
    scores: dict[int, int] = {}
    for i in sorted(candidate_shifts(exports)):
        schema = em.Schema()
        schema.shift[cls] = i - SC_REL_LOCATION
        offs = em.Offsets()
        infos = []
        for lvl in pkgs:
            for e in lvl.pkg.exports:
                if e.class_name != cls:
                    continue
                ci = em.comp_info(lvl.pkg, e, schema)
                if ci is not None and ci.run:
                    offs.observe(ci)
                    infos.append(ci)
        ok = 0
        for ci in infos:
            off, _how, _n = offs.offset(ci)
            if off is None:
                continue
            if plausible_box(*em.transform(ci, off), box):
                ok += 1
        scores[i] = ok
        if verbose:
            print(f"    index {i:4d}: {ok:6d} / {len(exports)} decode inside the map bounds")
    if not scores:
        return None, {"exports": len(exports), "candidates": 0}
    best, top = max(scores.items(), key=lambda kv: kv[1])
    rest = sorted((v for k, v in scores.items() if k != best), reverse=True)
    runner = rest[0] if rest else 0
    rep = {"exports": len(exports), "index": best, "hits": top,
           "share": top / len(exports), "runner_up": runner,
           "candidates": len(scores)}
    if top < len(exports) * FIT_MIN_SHARE or top < max(1.0, runner * FIT_MIN_MARGIN):
        return None, rep
    return best - SC_REL_LOCATION, rep


def asset_imports(imports: list[str]) -> dict[int, str]:
    """Import slots that name a cooked asset object.

    A referenced asset takes two import entries - the package (`/Game/Global/Mesh/
    ENV_B_Cube`) and the object inside it (`ENV_B_Cube`) - and a property points
    at the object. So the object slots are the ones whose name is the basename of
    a path import."""
    paths = {os.path.basename(n) for n in imports if n.startswith("/")}
    return {i: n for i, n in enumerate(imports) if n in paths}


def mesh_offsets(infos: list, assets: dict[int, str]) -> dict:
    """Byte offset of the `StaticMesh` object reference per value-prefix group.

    The same group vote `Offsets` runs for the transform: exports that share a
    class and the exact list of values written before the run share every byte
    offset, so an offset where *every* export of the group reads a package index
    pointing at an asset object is a property, while a stray four bytes inside an
    earlier value is per-instance noise. The mesh is the first such property; the
    materials follow it."""
    groups: dict[tuple, list] = collections.defaultdict(list)
    for ci in infos:
        groups[ci.key].append(ci)
    import struct
    out = {}
    for key, members in groups.items():
        votes: collections.Counter = collections.Counter()
        sample = members[:64]
        for ci in sample:
            for o in range(0, len(ci.payload) - 4):
                (pi,) = struct.unpack_from("<i", ci.payload, o)
                if pi < 0 and (-pi - 1) in assets:
                    votes[o] += 1
        if votes and max(votes.values()) == len(sample):
            out[key] = min(o for o, v in votes.items() if v == len(sample))
    return out


def extract(ms, chapter: str, verbose: bool = True) -> tuple[list[dict], dict]:
    keys = block_levels(ms).get(chapter, [])
    box = chapter_bounds(chapter)
    if not keys:
        return [], {"levels": 0}
    if box is None:
        raise SystemExit(f"chapter {chapter} has no bounds in {MAPS_JSON} to fit against")
    t0 = time.time()
    pkgs = []
    for k in keys:
        try:
            pkgs.append(em.Level(k, ms.package(k)))
        except Exception as exc:                                    # noqa: BLE001
            print(f"  ! {k}: {exc}", file=sys.stderr)
    if verbose:
        print(f"  chapter {chapter}: {len(pkgs)} Block levels")

    # the schema every other class in these levels declares outright ...
    schema = em.Schema()
    for lvl in pkgs:
        for e in lvl.pkg.exports:
            try:
                v, _ = parse_header(lvl.pkg.data(e))
            except Exception:                                       # noqa: BLE001
                continue
            schema.observe(e.class_name, v)
    em.fit_shifts({lvl.key: lvl for lvl in pkgs}, schema)
    # ... and the one it does not, fitted against the world the boxes bound
    fits = {}
    for cls in ("StaticMeshComponent",):
        if cls in schema.shift:
            continue
        shift, rep = fit_shift(pkgs, cls, box, verbose)
        fits[cls] = rep
        if shift is None:
            raise SystemExit(
                f"chapter {chapter}: {cls} transform not identified - "
                f"{rep.get('hits')} of {rep.get('exports')} exports decode at index "
                f"{rep.get('index')}, runner-up {rep.get('runner_up')}")
        schema.shift[cls] = shift
        if verbose:
            print(f"    {cls}: RelativeLocation at index {rep['index']} "
                  f"({rep['hits']} of {rep['exports']} exports = {rep['share']:.1%} "
                  f"inside the map bounds; runner-up {rep['runner_up']})")

    boxes: list[dict] = []
    stats: collections.Counter = collections.Counter()
    stats["levels"] = len(pkgs)
    for lvl in pkgs:
        cache, comps = {}, []
        offs = em.Offsets()
        for e in lvl.pkg.exports:
            ci = em.comp_info(lvl.pkg, e, schema)
            if ci is None:
                continue
            cache[e.index] = ci
            offs.observe(ci)
            if ci.run:
                comps.append(ci)
        assets = asset_imports(lvl.pkg.imports)
        moff = mesh_offsets(comps, assets)
        import struct
        for actor in lvl.actors():
            stats["class:" + actor.class_name] += 1
            # `BP_DieInstantly_C` kill volumes share these levels and are measured
            # dead as a boundary: 0 of chapter 1's 30 marks fall in one, and a
            # 600 uu margin takes 62 of 916 markers with it.
            if actor.class_name != "StaticMeshActor":
                continue
            stats["actors"] += 1
            got = em.root_component(lvl, actor, schema, cache)
            if got[0] is None:
                stats["no-root"] += 1
                continue
            (comp, ci), _attached = got
            off, how, _n = offs.offset(ci)
            if off is None:
                stats["no-offset"] += 1
                continue
            loc, rot, scl = em.transform(ci, off)
            if not plausible_box(loc, rot, scl, box):
                stats["implausible"] += 1
                continue
            mesh = None
            o = moff.get(ci.key)
            if o is not None and o + 4 <= len(ci.payload):
                (pi,) = struct.unpack_from("<i", ci.payload, o)
                mesh = assets.get(-pi - 1) if pi < 0 else None
            stats["ok:" + how] += 1
            stats["mesh:" + (mesh or "unresolved")] += 1
            boxes.append((lvl.short, mesh or "", loc, rot, scl))
    stats["seconds"] = round(time.time() - t0, 1)
    return boxes, dict(stats)


def bounds_at(blob: bytes) -> list[float] | None:
    """`FBoxSphereBounds` inside a cooked `StaticMesh` export: Origin, BoxExtent, SphereRadius.

    Its byte offset is not fixed - the same nine doubles sit at 16 in `Cube`, 42 in one Megascan
    rock and 78 in another - and nothing here parses a `StaticMesh`'s serialized head to find it.
    So the window is swept and every candidate is CHECKED against the one relation the three fields
    must satisfy: the sphere contains the box and is contained by its circumsphere,
    `max(extent) <= radius <= |extent|`. A denormal triple passes that too, which is what
    `MESH_BOUNDS_FLOOR` is for - a mesh somebody blocks a path with is never a centimetre across.

    `Cube` and `ENV_B_Cube` both decode to a half-extent of exactly 50, which is the independent
    check that the sweep reads the right field.
    """
    for o in range(0, min(MESH_BOUNDS_WINDOW, len(blob) - 56)):
        v = struct.unpack_from("<7d", blob, o)
        if not all(map(math.isfinite, v)):
            continue
        ox, oy, oz, hx, hy, hz, radius = v
        if not all(MESH_BOUNDS_FLOOR <= h < 1e7 for h in (hx, hy, hz)):
            continue
        if max(abs(ox), abs(oy), abs(oz)) > 1e6:
            continue
        circum = math.sqrt(hx * hx + hy * hy + hz * hz)
        if not (max(hx, hy, hz) * (1.0 - 1e-6) <= radius <= circum * (1.0 + 1e-6)):
            continue
        return [round(c, 2) for c in (ox, oy, oz, hx, hy, hz)]
    return None


def mesh_bounds(ms, names: Iterable[str]) -> dict[str, list[float]]:
    """Origin and half-extent of each named mesh, in its own unscaled frame.

    A mesh whose bounds cannot be read keeps the cube fallback - `Plane` is genuinely flat and has
    no third extent to find, and a cube over it is the over-block the cube reading always was.
    """
    by_name: dict[str, list[str]] = collections.defaultdict(list)
    for k in ms.paths():
        if k.endswith(".uasset"):
            by_name[os.path.basename(k)[:-7]].append(k)
    out: dict[str, list[float]] = {}
    for name in sorted(set(names)):
        for key in by_name.get(name, ()):
            try:
                pkg = ms.package(key)
            except Exception:                                       # noqa: BLE001
                continue
            for ex in pkg.exports:
                if ex.class_name != "StaticMesh" or ex.name != name:
                    continue
                got = bounds_at(pkg.data(ex))
                if got is not None:
                    out[name] = got
                break
            if name in out:
                break
    return out


def columns(boxes: list[tuple]) -> dict:
    """The boxes as parallel arrays: what both consumers - the map build's
    rasteriser and the picker - actually read, and a third of the bytes of a
    record per box."""
    levels: list[str] = []
    meshes: list[str] = []
    idx = {}
    out = {"levels": levels, "meshes": meshes, "lv": [], "m": [],
           "p": [], "r": [], "s": []}
    for lv, mesh, loc, rot, scl in boxes:
        for name, table, key in ((lv, levels, "lv"), (mesh, meshes, "m")):
            i = idx.get((key, name))
            if i is None:
                i = idx[(key, name)] = len(table)
                table.append(name)
            out[key].append(i)
        out["p"] += [round(c, 1) for c in loc]
        out["r"] += [round(c, 2) for c in rot]
        out["s"] += [round(c, 4) for c in scl]
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chapter", action="append", default=[],
                    help="chapter number (repeatable); default: all five")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--report", action="store_true", help="per-chapter counters")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    ms = pakmaps.MapSource(a.pak)
    chapters = a.chapter or list(CHAPTERS)
    doc = {"schema": "wuchang-blocks/2",
           "note": "invisible walls, one oriented box per actor as parallel arrays: "
                   "p = world location (x,y,z), r = FRotator (pitch,yaw,roll) in "
                   "degrees, s = scale, lv/m index levels/meshes. A cube mesh is "
                   "100 uu, so the half-extent is 50*s; any other mesh has its own "
                   "origin and half-extent in mesh_bounds, in its unscaled frame.",
           "cube_half_uu": CUBE_HALF_UU,
           "cube_meshes": list(CUBE_MESHES),
           "mesh_bounds": {},
           "chapters": {}}
    shaped: set[str] = set()
    for c in chapters:
        boxes, stats = extract(ms, c, verbose=not a.quiet)
        doc["chapters"][f"chapter{c}"] = dict(columns(boxes), stats=stats)
        shaped |= {mesh for _lv, mesh, *_ in boxes if mesh not in CUBE_MESHES}
        if a.report or not a.quiet:
            got = stats.get("actors", 0)
            print(f"  chapter {c}: {len(boxes)} boxes of {got} actors "
                  f"({len(boxes) / got:.1%})" if got else f"  chapter {c}: nothing")
            if a.report:
                for k, v in sorted(stats.items()):
                    print(f"      {k:28s} {v}")
    doc["mesh_bounds"] = mesh_bounds(ms, shaped)
    if not a.quiet:
        print(f"  {len(doc['mesh_bounds'])} of {len(shaped)} non-cube meshes carry their own bounds")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"))
    total = sum(len(v["lv"]) for v in doc["chapters"].values())
    print(f"{a.out}: {total} boxes, {os.path.getsize(a.out) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
