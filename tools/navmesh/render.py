#!/usr/bin/env python3
"""
Render Wuchang navmesh tile dumps as top-down walkable-area images.

Input
-----
Either source of `wuchang-navmesh-tiles/1` JSON works:

  * offline (the map source) -- `tools/navmesh/offline/navchunk.py` writes one file per
    cell package:  `dumps_offline/<agent>/tiles_<stamp>_<cell>.json`
  * runtime -- the C++ dumper writes one file per dump:
    `ue4ss/Mods/WuchangMinimap/navmesh/<agent>/tiles_<ts>.json`

Coordinate convention
---------------------
Unreal is X = forward (north), Y = right (east), Z = up. For a north-up image:

    u (column) = (world_Y - min_y) * px_per_uu        east  -> right
    v (row)    = (max_x - world_X) * px_per_uu        north -> up

so image width comes from the world Y extent and height from the world X extent.
Everything in `bounds.json` is in world units (uu, 1 uu = 1 cm).

Merging the cell packages
-------------------------
The streaming cell pitch is **10 240 uu** (`CELL_UU`), i.e. 8 navmesh tile columns of
1 280 uu; `floor(coord / 10240)` is the cell index. Each cell package additionally
ships a **one-column overlap ring** (measured: on the side facing the world origin),
so ~14 % of `(tile x, tile y, layer)` keys appear in two packages -- and the two copies
are *not* the same data: each package re-tiles its own ring, and 1 153 of Chapter 1's
1 378 shared keys differ in polygon count. `--dedupe richest` (the default) therefore
keeps whichever copy carries more polygons; `newest` keeps the last file read (the old
behaviour, which silently dropped geometry); `all` draws every copy.

Default output: ONE composite image
-----------------------------------
`<agent>_composite.png` draws **every** polygon, low Z first, shaded by height (lighter
= higher) with a thin darker edge on any polygon big enough to show one. Stacked
storeys therefore stay readable without any clustering guesswork, and nothing can be
lost to a mis-assigned floor. This is the map background.

Optional floor split (`--floors`)
---------------------------------
Two modes, both **global** -- neither buckets by streaming cell, which is what produced
the cell-shaped black squares in the first render (a 12-polygon Z outlier in one cell
monopolised that cell's "floor 0" and pushed its real ground into floor 1, so the cell
went black on floor 0 while its neighbours were fine):

  * `--floor-mode grid` (default): cluster on a fine XY grid (`--floor-grid`, 640 uu)
    by splitting sorted centroid Z wherever the gap exceeds `--floor-gap` (500 uu),
    then rank the resulting bands locally (0 = lowest surface in that bucket) and
    median-smooth those ranks across 4-neighbour buckets whose Z ranges are compatible
    for a few passes, so a floor stays continuous across grid, tile and package borders
    while a lone Z outlier can no longer take a whole bucket's floor 0 with it. (A
    union-find over the same relation was tried and is useless here: ramps connect
    every storey, so the whole chapter collapses into one sheet.)
  * `--floor-mode bands`: pick global Z bands from the histogram of all centroid Z
    (split at the widest empty Z gaps, `--floor-gap`). Cruder, but immune to ramps
    merging two storeys into one sheet.

Flat planes: out of bounds vs a real floor
------------------------------------------
Chapter 1 contains 347 polygons that are a single flat quad filling an entire 1 280-uu
tile (1 638 400 uu2). They carry the same `area = 63` (RC_WALKABLE_AREA) and `flags = 1`
as everything else, so they cannot be filtered by area type - but they are NOT all
scenery either. Dropping them all (what this script used to do) deleted the drained-lake
Commander Honglan boss arena and the Tang-palace shrine terrace along with the sky
plane, and the user saw those as black tile-aligned squares on the minimap.

So the quads are clustered by Z into *sheets* and a sheet is dropped only when it looks
out of bounds (see `classify_flat_planes`):

  * `--flat-plane-sheet-min` (48) coplanar quads or more - a perfectly flat surface
    spanning 60+ tiles is 500 m of dead level ground and no hand-built area is that, or
  * no *non-flat* navmesh within `--flat-plane-isolation` (5 000 uu) of the sheet's Z
    under its own tile footprint - the Z = 36 351 and Z = 38 871 sheets every chapter
    ships are 24 000-36 000 uu away from any real geometry.

Measured, chapters 1-5: every kept sheet has non-flat navmesh within 7 uu of its Z and
holds at most 25 quads; every dropped one is either 66..400 quads or 24 650+ uu away.
`--flat-planes drop|keep|only` still switches the filter off entirely, and
`--flat-plane-area` sets what counts as a candidate.
`--exclude-area N` is available for the few polygons that do carry area 1/2/3.

Unreachable islands (`--drop-islands`)
--------------------------------------
Recast leaves thousands of sub-square-metre scraps of navmesh under scenery, and they
read as speckle on the minimap. `connected_components` recovers polygon adjacency
geometrically (Detour's cross-tile links are in the part of the tile record the offline
decoder does not parse) and `filter_islands` drops the components no player can stand
on: a component is kept when a marker from `markers/chapter<N>.json` is within
`--island-seed-radius` of it, else when its area clears `--island-min-area`.
`--island-require-seed` is the strict "reachable from a shrine/marker only" mode; it
drops ~56 % of Chapter 1's walkable area (off-mesh links - ladders, lifts, drops -
are not in the data, so most of the map is not graph-connected to a shrine) and it eats
the Honglan arena, which is reachable only by dropping into it. Do not ship it.

Usage
-----
    # the map background: one composite image of everything
    python render.py --input dumps_offline --agent Small --out out_offline/v2 --px-per-uu 0.02

    # optional floor split
    python render.py --input dumps_offline --agent Small --out out_offline/v2 --floors

    # detail crop with the probe grid overlaid
    python render.py --input dumps_offline --agent Small --out out_offline/v2 \
        --crop 17950 4000 19050 5100 --px-per-uu 0.2 --suffix _spot1 \
        --probe-overlay ../../.workspace/wuchang-minimap/context/dumps/navprobe_20260902_102701.csv

    # self-test with fabricated tiles - works before any real dump exists
    python render.py --synthetic --out out_synthetic
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

# ---------------------------------------------------------------------------------
# constants
# ---------------------------------------------------------------------------------

CELL_UU = 10240.0  # Wuchang's streaming cell pitch: floor(coord / 10240) is the cell
TILE_UU = 1280.0  # navmesh tile pitch (TileSizeUU); 8 tiles per streaming cell
DEFAULT_FLOOR_GAP = 500.0  # uu; a Z gap larger than this starts a new floor
DEFAULT_FLOOR_GRID = 640.0  # uu; XY grid the floor clustering runs on
DEFAULT_PX_PER_UU = 0.02  # 1 px = 50 uu = 0.5 m
DEFAULT_FLAT_PLANE_AREA = 1.0e6  # uu2; a flat poly bigger than this is a "plane"
# A big flat quad is only OUT OF BOUNDS when it is part of a whole *sheet* of them at
# one Z, or when no ordinary navmesh exists anywhere near that Z under its footprint.
# See context/navmesh-arenas-and-islands.md: dropping every big flat quad deleted real
# floors (the Honglan boss arena, the Tang-palace shrine terrace) along with the sky sheet.
# 48 is measured, not guessed: across chapters 1-5 the coplanar-quad sheets come in two
# populations with nothing between them - real man-made floors and water bodies at 1..25
# quads (the Honglan arena floor is 4, a courtyard 16, a palace lake 21) and whole-region
# sheets at 66..400 (Chapter 1's sky plane 232, Chapter 4's pair 400 and 377). A perfectly
# flat surface spanning 60+ navmesh tiles is 500 m of dead level ground, which no hand-built
# area is; a 4-tile one is a drained lake.
DEFAULT_PLANE_SHEET_MIN = 48  # coplanar big flat quads that make it an out-of-bounds sheet
DEFAULT_PLANE_ISOLATION = 5000.0  # uu; nearest ordinary navmesh further than this -> drop
DEFAULT_PLANE_Z_TOL = 20.0  # uu; planes within this Z of each other are one sheet
# island filter (see §"unreachable islands")
DEFAULT_ISLAND_GRID = 64.0  # uu; XY grid the connectivity union-find runs on
DEFAULT_ISLAND_Z_TOL = 150.0  # uu; two polys in one grid cell join if their Z ranges are this close
DEFAULT_ISLAND_MIN_AREA = 40000.0  # uu2 (4 m2); a seedless component smaller than this is noise
DEFAULT_ISLAND_SEED_RADIUS = 300.0  # uu (3 m); a marker this close to a component keeps it
DEFAULT_ISLAND_SEED_Z = 600.0  # uu; ... and within this much Z of it
# marker categories that are evidence "a player can get here" (ladder / lift / door
# reach places the navmesh graph cannot, because off-mesh links are not in our data)
ISLAND_SEED_CATEGORIES = (
    "shrine", "chest", "pickup", "npc", "merchant", "boss", "ladder", "lift", "door", "fog_gate",
)
MAX_IMAGE_PX = 16000  # guard against a --px-per-uu typo eating all the RAM

COL_BG = (16, 18, 22)
COL_TILE_BORDER = (70, 76, 84)
COL_PROBE_MISS = (150, 60, 60)
COL_PROBE_HIT = (255, 214, 92)
# height ramp, low -> high (dark blue-green -> pale green-white)
Z_RAMP = [
    (0.00, (24, 52, 60)),
    (0.25, (34, 96, 92)),
    (0.50, (74, 150, 116)),
    (0.75, (140, 196, 142)),
    (1.00, (226, 242, 214)),
]


# ---------------------------------------------------------------------------------
# data model
# ---------------------------------------------------------------------------------


@dataclass
class Tile:
    x: int
    y: int
    layer: int
    bmin: list[float]
    bmax: list[float]
    verts: list[list[float]]
    polys: list[dict]
    source: str = ""

    @property
    def key(self) -> tuple[int, int, int]:
        return (self.x, self.y, self.layer)


@dataclass
class Dump:
    agent: str
    header: dict
    tiles: dict[tuple[int, int, int], Tile] = field(default_factory=dict)
    files: list[str] = field(default_factory=list)
    dupes: int = 0
    dupes_differing: int = 0
    replaced: int = 0


@dataclass
class Bounds:
    min_x: float
    min_y: float
    max_x: float
    max_y: float
    px_per_uu: float

    @property
    def width(self) -> int:
        return max(1, int(math.ceil((self.max_y - self.min_y) * self.px_per_uu)) + 1)

    @property
    def height(self) -> int:
        return max(1, int(math.ceil((self.max_x - self.min_x) * self.px_per_uu)) + 1)

    def to_px(self, wx: float, wy: float) -> tuple[float, float]:
        """World (X, Y) -> image (u, v). See the module docstring."""
        u = (wy - self.min_y) * self.px_per_uu
        v = (self.max_x - wx) * self.px_per_uu
        return (u, v)

    def as_dict(self) -> dict:
        return {
            "min_x": self.min_x,
            "min_y": self.min_y,
            "max_x": self.max_x,
            "max_y": self.max_y,
            "px_per_uu": self.px_per_uu,
            "image_width": self.width,
            "image_height": self.height,
            "mapping": "u = (world_Y - min_y) * px_per_uu ; v = (max_x - world_X) * px_per_uu",
        }


# ---------------------------------------------------------------------------------
# loading + merging
# ---------------------------------------------------------------------------------


def load_dump_files(paths: Iterable[Path], agent_hint: str = "", dedupe: str = "richest") -> Dump:
    """
    Merge several tile JSONs into one tile set.

    `dedupe`:
      richest -- on a key collision keep the copy with more polygons (default; the
                 overlap-ring copies genuinely differ, see the module docstring)
      newest  -- keep the last file read (legacy behaviour)
      all     -- keep every copy (keys become (x, y, layer, n))
    """
    ordered = sorted(paths, key=lambda p: (_generated_of(p), p.name))
    dump = Dump(agent=agent_hint, header={})
    for path in ordered:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, json.JSONDecodeError) as exc:
            print(f"  ! skipping {path.name}: {exc}", file=sys.stderr)
            continue
        if doc.get("schema") != "wuchang-navmesh-tiles/1":
            print(f"  ! skipping {path.name}: unknown schema {doc.get('schema')!r}", file=sys.stderr)
            continue

        dump.agent = doc.get("agent", dump.agent) or agent_hint
        dump.header = doc  # the newest file's header wins
        dump.files.append(str(path))
        for raw in doc.get("tiles", []):
            tile = Tile(
                x=int(raw["x"]),
                y=int(raw["y"]),
                layer=int(raw["layer"]),
                bmin=[float(v) for v in raw["bmin"]],
                bmax=[float(v) for v in raw["bmax"]],
                verts=[[float(c) for c in v] for v in raw.get("verts", [])],
                polys=list(raw.get("polys", [])),
                source=path.name,
            )
            key: tuple = tile.key
            old = dump.tiles.get(key)
            if old is None:
                dump.tiles[key] = tile
                continue
            dump.dupes += 1
            if len(old.polys) != len(tile.polys) or old.verts != tile.verts:
                dump.dupes_differing += 1
            if dedupe == "all":
                n = 1
                while (key + (n,)) in dump.tiles:
                    n += 1
                dump.tiles[key + (n,)] = tile
            elif dedupe == "newest" or (dedupe == "richest" and len(tile.polys) > len(old.polys)):
                dump.tiles[key] = tile
                dump.replaced += 1
    return dump


def _generated_of(path: Path) -> str:
    """Cheap sort key: the timestamp inside the filename, else the mtime."""
    stem = path.stem
    for prefix in ("tiles_", "probe_"):
        if stem.startswith(prefix):
            return stem[len(prefix) :]
    try:
        return str(path.stat().st_mtime)
    except OSError:
        return stem


def discover_agents(root: Path) -> dict[str, list[Path]]:
    """`root` may be the navmesh root (agent subdirectories) or one agent directory."""
    direct = sorted(root.glob("tiles_*.json"))
    if direct:
        return {root.name: direct}
    agents: dict[str, list[Path]] = {}
    for child in sorted(p for p in root.iterdir() if p.is_dir()):
        files = sorted(child.glob("tiles_*.json"))
        if files:
            agents[child.name] = files
    return agents


# ---------------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------------


def xy_area(pts: list[list[float]]) -> float:
    """Shoelace area of the polygon projected on XY, uu^2."""
    a = 0.0
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i][0], pts[i][1]
        x1, y1 = pts[(i + 1) % n][0], pts[(i + 1) % n][1]
        a += x0 * y1 - x1 * y0
    return abs(a) * 0.5


def polygons_of(dump: Dump) -> list[dict]:
    """Flatten tiles into world-space polygons with a centroid, area and Z spread."""
    out: list[dict] = []
    for tile in dump.tiles.values():
        nverts = len(tile.verts)
        for poly in tile.polys:
            idx = poly.get("v", [])
            if len(idx) < 3:
                continue
            pts = []
            ok = True
            for i in idx:
                if not isinstance(i, int) or i < 0 or i >= nverts:
                    ok = False
                    break
                pts.append(tile.verts[i])
            if not ok:
                continue
            zs = [p[2] for p in pts]
            out.append(
                {
                    "pts": pts,
                    "cx": sum(p[0] for p in pts) / len(pts),
                    "cy": sum(p[1] for p in pts) / len(pts),
                    "cz": sum(zs) / len(zs),
                    "zspread": max(zs) - min(zs),
                    "xyarea": xy_area(pts),
                    "area": poly.get("area", 0),
                    "flags": poly.get("flags", 0),
                    "tile": tile.key,
                }
            )
    return out


def classify_flat_planes(
    polys: list[dict],
    min_area: float,
    sheet_min: int = DEFAULT_PLANE_SHEET_MIN,
    isolation: float = DEFAULT_PLANE_ISOLATION,
    z_tol: float = DEFAULT_PLANE_Z_TOL,
) -> dict:
    """Mark poly['plane'] for the near-horizontal quads that are OUT OF BOUNDS.

    A "plane candidate" is a near-horizontal polygon of at least `min_area` uu^2 -
    Wuchang's cooked navmesh is full of them and they are *ordinary walkable polygons*
    with no flag to tell them apart (`flags` is 1 and `area` is 63 on all of them).
    Candidates are clustered by Z (`z_tol`) into sheets, and a sheet is only dropped
    when it looks like the game's out-of-bounds / kill plane rather than a floor:

      * `>= sheet_min` quads share the sheet's Z (the Chapter-1 sky sheet is 232), or
      * no *ordinary* (non-candidate) navmesh exists within `isolation` uu of the
        sheet's Z anywhere under its 1280-uu tile footprint.

    Everything else is a real floor and is kept. `min_area` alone used to decide this,
    which deleted the drained-lake boss arena (4 quads at Z 71 under the Honglan boss)
    and the Tang-palace shrine terrace (4 quads at Z 1671) - both of them rendered
    in-game as a black tile-aligned square.
    """
    by_z: dict[float, int] = {}
    cand: list[dict] = []
    for p in polys:
        p["plane_candidate"] = p["zspread"] < 1.0 and p["xyarea"] >= min_area
        p["plane"] = False
        if p["plane_candidate"]:
            cand.append(p)
            z = round(p["cz"], 1)
            by_z[z] = by_z.get(z, 0) + 1

    # Ordinary navmesh, bucketed by tile key, so the isolation test is local. "Ordinary"
    # means *not flat*: the out-of-bounds sheets are surrounded by small flat quads at
    # their own Z, so keying the test on "not a big flat quad" makes it always answer 0.
    ordinary: dict[tuple, list[float]] = {}
    for p in polys:
        if p["zspread"] < 1.0:
            continue
        for key in _tile_keys(p):
            ordinary.setdefault(key, []).append(p["cz"])

    sheets: list[dict] = []
    for members in _cluster_by(cand, lambda p: p["cz"], z_tol):
        zmid = sum(p["cz"] for p in members) / len(members)
        near = min(
            (abs(cz - zmid) for p in members for key in _tile_keys(p) for cz in ordinary.get(key, ())),
            default=float("inf"),
        )
        drop = len(members) >= sheet_min or near > isolation
        for p in members:
            p["plane"] = drop
        sheets.append(
            {
                "z": round(zmid, 1),
                "polys": len(members),
                "nearest_ordinary_dz": None if near == float("inf") else round(near, 1),
                "dropped": drop,
                "reason": ("sheet" if len(members) >= sheet_min else "isolated") if drop else "",
            }
        )

    sheets.sort(key=lambda s: -s["polys"])
    top = sorted(by_z.items(), key=lambda kv: -kv[1])[:8]
    return {
        "count": sum(1 for p in polys if p["plane"]),
        "candidates": len(cand),
        "min_area_uu2": min_area,
        "sheet_min": sheet_min,
        "isolation_uu": isolation,
        "sheets": sheets,
        "dropped_sheets": [s for s in sheets if s["dropped"]],
        "top_z": [{"z": z, "polys": c} for z, c in top],
    }


def _tile_keys(p: dict) -> list[tuple[int, int]]:
    xs = [q[0] for q in p["pts"]]
    ys = [q[1] for q in p["pts"]]
    return [
        (tx, ty)
        for tx in range(int(math.floor(min(xs) / TILE_UU)), int(math.floor(max(xs) / TILE_UU)) + 1)
        for ty in range(int(math.floor(min(ys) / TILE_UU)), int(math.floor(max(ys) / TILE_UU)) + 1)
    ]


def _cluster_by(items, key, tol: float) -> list[list]:
    """Group items into runs whose `key` never jumps by more than `tol`.

    Clusters the *items themselves*, never rounded copies of their keys - rounding the
    key and then re-selecting members by range silently loses the members that round
    the wrong way (it cost 14 of Chapter 1's 33 flat-plane sheets on the first try).
    """
    out: list[list] = []
    last = None
    for it in sorted(items, key=key):
        v = key(it)
        if out and last is not None and v - last <= tol:
            out[-1].append(it)
        else:
            out.append([it])
        last = v
    return out


# ---------------------------------------------------------------------------------
# connected components ("islands")
# ---------------------------------------------------------------------------------


class _DSU:
    __slots__ = ("p",)

    def __init__(self, n: int):
        self.p = list(range(n))

    def find(self, a: int) -> int:
        p = self.p
        while p[a] != a:
            p[a] = p[p[a]]
            a = p[a]
        return a

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[rb] = ra


def connected_components(
    polys: list[dict], grid: float = DEFAULT_ISLAND_GRID, z_tol: float = DEFAULT_ISLAND_Z_TOL
) -> list[dict]:
    """Union-find over the polygons; writes poly['comp'] and returns the components.

    Detour's own cross-tile links live in the part of the serialized tile record the
    offline decoder does not parse (`navmesh-offline.md` §10), so adjacency is
    recovered geometrically instead: every polygon is rasterised onto an XY grid of
    `grid` uu - cell centres inside it plus a walk along its edges, so two polygons
    that merely share an edge still land in a common cell - and two polygons sharing a
    cell are joined when their Z ranges are within `z_tol`. That keeps two storeys of
    the same building apart while letting a ramp connect them.
    """
    n = len(polys)
    dsu = _DSU(n)
    buckets: dict[tuple[int, int], list[int]] = {}
    for i, p in enumerate(polys):
        pts = p["pts"]
        zs = [q[2] for q in pts]
        p["_zlo"], p["_zhi"] = min(zs), max(zs)
        cells = set()
        # 1. cell centres inside the polygon
        xs = [q[0] for q in pts]
        ys = [q[1] for q in pts]
        gx0, gx1 = int(math.floor(min(xs) / grid)), int(math.floor(max(xs) / grid))
        gy0, gy1 = int(math.floor(min(ys) / grid)), int(math.floor(max(ys) / grid))
        if (gx1 - gx0 + 1) * (gy1 - gy0 + 1) <= 4096:
            for gx in range(gx0, gx1 + 1):
                cx = (gx + 0.5) * grid
                for gy in range(gy0, gy1 + 1):
                    if _point_in_poly(pts, cx, (gy + 0.5) * grid):
                        cells.add((gx, gy))
        # 2. walk the boundary so edge-sharing neighbours meet in a cell
        m = len(pts)
        for k in range(m):
            x0, y0 = pts[k][0], pts[k][1]
            x1, y1 = pts[(k + 1) % m][0], pts[(k + 1) % m][1]
            steps = max(1, int(math.hypot(x1 - x0, y1 - y0) / (grid * 0.5)) + 1)
            for s in range(steps + 1):
                t = s / steps
                cells.add((int(math.floor((x0 + (x1 - x0) * t) / grid)), int(math.floor((y0 + (y1 - y0) * t) / grid))))
        for c in cells:
            buckets.setdefault(c, []).append(i)

    for members in buckets.values():
        if len(members) < 2:
            continue
        members.sort(key=lambda i: polys[i]["_zlo"])
        for a, b in zip(members, members[1:]):
            pa, pb = polys[a], polys[b]
            # Z ranges overlap, or the gap between them is within tolerance. Sorting by
            # the low edge makes this a chain: a ramp's tall Z range bridges the storeys
            # it actually joins, and a 200-uu-clear gallery above a floor stays separate.
            if pb["_zlo"] - pa["_zhi"] <= z_tol:
                dsu.union(a, b)

    comps: dict[int, dict] = {}
    for i, p in enumerate(polys):
        r = dsu.find(i)
        c = comps.get(r)
        if c is None:
            c = comps[r] = {"root": r, "polys": 0, "area": 0.0, "members": [],
                            "min_x": 1e18, "min_y": 1e18, "max_x": -1e18, "max_y": -1e18,
                            "min_z": 1e18, "max_z": -1e18}
        c["polys"] += 1
        c["area"] += p["xyarea"]
        c["members"].append(i)
        for q in p["pts"]:
            c["min_x"] = min(c["min_x"], q[0]); c["max_x"] = max(c["max_x"], q[0])
            c["min_y"] = min(c["min_y"], q[1]); c["max_y"] = max(c["max_y"], q[1])
            c["min_z"] = min(c["min_z"], q[2]); c["max_z"] = max(c["max_z"], q[2])
    out = sorted(comps.values(), key=lambda c: -c["area"])
    for n_, c in enumerate(out):
        for i in c["members"]:
            polys[i]["comp"] = n_
        c["id"] = n_
    return out


def _point_in_poly(pts: list[list[float]], x: float, y: float) -> bool:
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i][0], pts[i][1]
        x2, y2 = pts[(i + 1) % n][0], pts[(i + 1) % n][1]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) / (y2 - y1) * (x2 - x1):
            inside = not inside
    return inside


def load_marker_seeds(paths: Iterable[Path], categories: Iterable[str] = ISLAND_SEED_CATEGORIES) -> list[dict]:
    """Marker positions that prove a component is reachable (`markers/chapter*.json`)."""
    cats = set(categories)
    seeds: list[dict] = []
    for path in paths:
        try:
            doc = json.loads(Path(path).read_text(encoding="utf-8"))
        except Exception as exc:  # pragma: no cover
            print(f"  ! cannot read markers {path}: {exc}", file=sys.stderr)
            continue
        for m in doc.get("markers", []):
            if m.get("cat") in cats and all(k in m for k in ("x", "y", "z")):
                seeds.append({"x": m["x"], "y": m["y"], "z": m["z"], "cat": m["cat"], "id": m.get("id", "")})
    return seeds


def filter_islands(
    polys: list[dict],
    seeds: list[dict],
    grid: float = DEFAULT_ISLAND_GRID,
    z_tol: float = DEFAULT_ISLAND_Z_TOL,
    min_area: float = DEFAULT_ISLAND_MIN_AREA,
    seed_radius: float = DEFAULT_ISLAND_SEED_RADIUS,
    seed_z: float = DEFAULT_ISLAND_SEED_Z,
    require_seed: bool = False,
) -> tuple[list[dict], dict]:
    """Drop the navmesh components no player can stand on.

    A component is KEPT when a seed marker (cat shrine / chest / pickup / npc /
    merchant / boss / ladder / lift / door) is within `seed_radius` uu horizontally
    and `seed_z` uu vertically of one of its polygons - that is direct evidence the
    place is reachable, including the small platforms only a ladder or a lift gets to.
    Without a seed it survives on size alone (`min_area`), unless `require_seed`.
    """
    comps = connected_components(polys, grid=grid, z_tol=z_tol)
    if not comps:
        return polys, {"components": 0}

    # seed lookup: grid-hash the seeds, then test them against each polygon's bbox
    shash: dict[tuple[int, int], list[dict]] = {}
    cell = max(seed_radius, 1.0)
    for s in seeds:
        shash.setdefault((int(s["x"] // cell), int(s["y"] // cell)), []).append(s)
    for c in comps:
        c["seeds"] = []
    for p in polys:
        c = comps[p["comp"]]
        xs = [q[0] for q in p["pts"]]
        ys = [q[1] for q in p["pts"]]
        x0, x1 = min(xs) - seed_radius, max(xs) + seed_radius
        y0, y1 = min(ys) - seed_radius, max(ys) + seed_radius
        for gx in range(int(x0 // cell), int(x1 // cell) + 1):
            for gy in range(int(y0 // cell), int(y1 // cell) + 1):
                for s in shash.get((gx, gy), ()):
                    if x0 <= s["x"] <= x1 and y0 <= s["y"] <= y1 and p["_zlo"] - seed_z <= s["z"] <= p["_zhi"] + seed_z:
                        if len(c["seeds"]) < 8 and s["cat"] not in [q["cat"] for q in c["seeds"]]:
                            c["seeds"].append(s)
                        c["seeded"] = True

    kept_comps, dropped = [], []
    for i, c in enumerate(comps):
        c["seeded"] = bool(c.get("seeded"))
        if i == 0:
            c["keep"], c["why"] = True, "largest"
        elif c["seeded"]:
            c["keep"], c["why"] = True, "seed:" + ",".join(sorted({s["cat"] for s in c["seeds"]}))
        elif require_seed:
            c["keep"], c["why"] = False, "no seed"
        elif c["area"] >= min_area:
            c["keep"], c["why"] = True, "area"
        else:
            c["keep"], c["why"] = False, f"area {c['area']:.0f} < {min_area:.0f}"
        (kept_comps if c["keep"] else dropped).append(c)

    keep_ids = {c["id"] for c in kept_comps}
    out = [p for p in polys if p["comp"] in keep_ids]
    stats = {
        "components": len(comps),
        "kept": len(kept_comps),
        "dropped": len(dropped),
        "polys_before": len(polys),
        "polys_after": len(out),
        "polys_dropped": len(polys) - len(out),
        "area_before": sum(c["area"] for c in comps),
        "area_dropped": sum(c["area"] for c in dropped),
        "seeds": len(seeds),
        "seeded_components": sum(1 for c in comps if c["seeded"]),
        "grid_uu": grid,
        "z_tol_uu": z_tol,
        "min_area_uu2": min_area,
        "seed_radius_uu": seed_radius,
        "require_seed": require_seed,
        "biggest_dropped": [
            {"id": c["id"], "polys": c["polys"], "area": round(c["area"]),
             "x": round((c["min_x"] + c["max_x"]) / 2), "y": round((c["min_y"] + c["max_y"]) / 2),
             "z": round((c["min_z"] + c["max_z"]) / 2)}
            for c in sorted(dropped, key=lambda c: -c["area"])[:12]
        ],
    }
    return out, stats


def describe_islands(tag: str, st: dict) -> str:
    """One-line summary of a filter_islands() run, for both tools' logs."""
    if not st.get("components"):
        return f"[{tag}] island filter: no components"
    return (
        f"[{tag}] islands: {st['components']} components, kept {st['kept']}, dropped {st['dropped']}"
        f" ({st['seeded_components']} seeded by {st['seeds']} markers); polygons"
        f" {st['polys_before']} -> {st['polys_after']}"
        f" (-{st['polys_dropped']}, -{100.0 * st['polys_dropped'] / max(1, st['polys_before']):.1f}%),"
        f" walkable area -{100.0 * st['area_dropped'] / max(1.0, st['area_before']):.2f}%"
        f"  [grid {st['grid_uu']:g} uu, z-tol {st['z_tol_uu']:g} uu,"
        f" min area {st['min_area_uu2']:.0f} uu2, seed r {st['seed_radius_uu']:.0f} uu"
        + (", require-seed" if st["require_seed"] else "") + "]"
    )


def add_island_args(ap: argparse.ArgumentParser, default_on: bool = False) -> None:
    """The island-filter and marker-seed flags, shared by render.py and build_map.py."""
    if default_on:
        ap.add_argument("--keep-islands", dest="drop_islands", action="store_false", default=True,
                        help="do NOT drop unreachable/tiny navmesh components")
    else:
        ap.add_argument("--drop-islands", action="store_true",
                        help="drop unreachable/tiny navmesh components (connected-component pass)")
    ap.add_argument("--markers", action="append", default=[],
                    help="markers JSON whose positions seed 'reachable' (repeatable, globs ok)")
    ap.add_argument("--island-grid", type=float, default=DEFAULT_ISLAND_GRID,
                    help=f"XY grid for the connectivity pass, uu (default {DEFAULT_ISLAND_GRID:g})")
    ap.add_argument("--island-z-tol", type=float, default=DEFAULT_ISLAND_Z_TOL,
                    help=f"Z tolerance that joins two polygons, uu (default {DEFAULT_ISLAND_Z_TOL:g})")
    ap.add_argument("--island-min-area", type=float, default=DEFAULT_ISLAND_MIN_AREA,
                    help=f"seedless component smaller than this is dropped, uu2 (default {DEFAULT_ISLAND_MIN_AREA:.0f})")
    ap.add_argument("--island-seed-radius", type=float, default=DEFAULT_ISLAND_SEED_RADIUS,
                    help=f"a marker this close keeps a component, uu (default {DEFAULT_ISLAND_SEED_RADIUS:g})")
    ap.add_argument("--island-require-seed", action="store_true",
                    help="keep ONLY components with a marker seed (strict reachability; drops ~56%% of Chapter 1)")


def add_plane_args(ap: argparse.ArgumentParser) -> None:
    ap.add_argument("--flat-plane-sheet-min", type=int, default=DEFAULT_PLANE_SHEET_MIN,
                    help=f"coplanar big flat quads that make an out-of-bounds sheet (default {DEFAULT_PLANE_SHEET_MIN})")
    ap.add_argument("--flat-plane-isolation", type=float, default=DEFAULT_PLANE_ISOLATION,
                    help=f"drop a flat sheet with no ordinary navmesh within this Z, uu (default {DEFAULT_PLANE_ISOLATION:g})")


def compute_bounds(polys: list[dict], px_per_uu: float, margin_uu: float = 128.0) -> Bounds:
    xs = [p[0] for poly in polys for p in poly["pts"]]
    ys = [p[1] for poly in polys for p in poly["pts"]]
    if not xs:
        return Bounds(0.0, 0.0, 1.0, 1.0, px_per_uu)
    b = Bounds(min(xs) - margin_uu, min(ys) - margin_uu, max(xs) + margin_uu, max(ys) + margin_uu, px_per_uu)
    while (b.width > MAX_IMAGE_PX or b.height > MAX_IMAGE_PX) and b.px_per_uu > 1e-6:
        b.px_per_uu /= 2.0
        print(f"  ! image would be {b.width}x{b.height} px, halving scale to {b.px_per_uu:g} px/uu", file=sys.stderr)
    return b


def split_bands(values: list[float], gap: float) -> list[tuple[float, float]]:
    """Sorted values -> [(zmin, zmax)] bands, split wherever the gap is too big."""
    if not values:
        return []
    vs = sorted(values)
    bands: list[tuple[float, float]] = []
    start = prev = vs[0]
    for v in vs[1:]:
        if v - prev > gap:
            bands.append((start, prev))
            start = v
        prev = v
    bands.append((start, prev))
    return bands


def assign_floors_grid(polys: list[dict], gap: float, grid: float, smooth_passes: int = 3) -> int:
    """
    Global, cell-border-agnostic floor split.

    1. bucket polygons on a `grid`-uu XY grid and split the centroid Z pooled over each
       bucket's 3x3 neighbourhood into bands at gaps > `gap`;
    2. start each band at its local rank (0 = lowest surface in that bucket);
    3. smooth those ranks over 4-neighbour buckets whose Z ranges are compatible, a
       few median passes, so a floor is continuous across grid / tile / streaming-cell
       borders and a lone Z outlier cannot renumber a whole bucket.

    Writes poly["floor"]; returns the floor count.
    """
    buckets: dict[tuple[int, int], list[float]] = {}
    for p in polys:
        g = (math.floor(p["cx"] / grid), math.floor(p["cy"] / grid))
        p["gkey"] = g
        buckets.setdefault(g, []).append(p["cz"])

    # Bands are computed from the pooled Z of the bucket's 3x3 neighbourhood, not from
    # the bucket alone: neighbouring buckets then share almost all of their input, which
    # is already most of the continuity, and an isolated upper platform can still see
    # the ground beside it (from its own bucket alone it would rank as floor 0).
    bands: dict[tuple[int, int], list[tuple[float, float]]] = {}
    for g in buckets:
        gx, gy = g
        pooled: list[float] = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                pooled.extend(buckets.get((gx + dx, gy + dy), ()))
        bands[g] = split_bands(pooled, gap)

    # 2. every band starts at its local rank (0 = lowest surface in that bucket), then
    #    the ranks are smoothed across 4-neighbour buckets whose Z ranges are
    #    compatible (they overlap once each is grown by gap/2). Smoothing is what makes
    #    a floor continuous across grid, tile and streaming-cell borders: a lone Z
    #    outlier that would otherwise push a whole bucket's ground up to rank 1 is
    #    outvoted by its neighbours. Ranks stay local, so a ramp connecting two storeys
    #    does not renumber the whole map (which is what a union-find over the same
    #    compatibility relation does - it merges the entire world into one sheet).
    half = gap * 0.5
    nbrs: dict[tuple, list[tuple]] = {}
    for g, bs in bands.items():
        gx, gy = g
        for i, (lo, hi) in enumerate(bs):
            acc: list[tuple] = []
            for ng in ((gx + 1, gy), (gx - 1, gy), (gx, gy + 1), (gx, gy - 1)):
                nbs = bands.get(ng)
                if not nbs:
                    continue
                for j, (nlo, nhi) in enumerate(nbs):
                    if lo - half <= nhi + half and nlo - half <= hi + half:
                        acc.append((ng, j))
            nbrs[(g, i)] = acc

    level = {(g, i): i for g, bs in bands.items() for i in range(len(bs))}
    for _ in range(smooth_passes):
        nxt = {}
        for node, acc in nbrs.items():
            vals = [level[node]] + [level[n] for n in acc]
            nxt[node] = int(round(_median(vals)))
        level = nxt

    for p in polys:
        bs = bands[p["gkey"]]
        idx = 0
        for i, (lo, hi) in enumerate(bs):
            if lo - 1e-6 <= p["cz"] <= hi + 1e-6:
                idx = i
                break
        p["floor"] = level[(p["gkey"], idx)]
    return max((p["floor"] for p in polys), default=0) + 1


def assign_floors_bands(polys: list[dict], gap: float) -> int:
    """Global Z bands straight from the whole-area centroid-Z histogram."""
    bands = split_bands([p["cz"] for p in polys], gap)
    for p in polys:
        p["floor"] = 0
        for i, (lo, hi) in enumerate(bands):
            if lo - 1e-6 <= p["cz"] <= hi + 1e-6:
                p["floor"] = i
                break
    return len(bands)


def _median(v: list[int]) -> float:  # kept for reference / tests
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def floor_z_ranges(polys: list[dict], floors: int) -> list[list[float]]:
    ranges: list[list[float]] = []
    for i in range(floors):
        zs = [p["cz"] for p in polys if p["floor"] == i]
        ranges.append([min(zs), max(zs)] if zs else [0.0, 0.0])
    return ranges


# ---------------------------------------------------------------------------------
# drawing
# ---------------------------------------------------------------------------------


def _ramp(t: float, stops=Z_RAMP) -> tuple[int, int, int]:
    t = max(0.0, min(1.0, t))
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(int(round(a + (b - a) * f)) for a, b in zip(c0, c1))  # type: ignore[return-value]
    return stops[-1][1]


def _percentiles(vals: list[float], lo: float, hi: float) -> tuple[float, float]:
    s = sorted(vals)
    if not s:
        return (0.0, 1.0)
    a = s[max(0, min(len(s) - 1, int(lo * (len(s) - 1))))]
    b = s[max(0, min(len(s) - 1, int(hi * (len(s) - 1))))]
    return (a, b if b > a else a + 1.0)


def draw_polys(
    img: Image.Image,
    polys: list[dict],
    bounds: Bounds,
    zlo: float,
    zhi: float,
    edges: bool = True,
    flat_fill: tuple[int, int, int] | None = None,
) -> int:
    """
    Paint polygons low Z first, shaded by height. A thin darker edge is drawn on any
    polygon whose pixel footprint is big enough to show one, so overlapping storeys
    stay separable.
    """
    dr = ImageDraw.Draw(img)
    drawn = 0
    for p in sorted(polys, key=lambda q: q["cz"]):
        pts = [bounds.to_px(q[0], q[1]) for q in p["pts"]]
        if flat_fill is not None:
            col = flat_fill
        else:
            col = _ramp((p["cz"] - zlo) / (zhi - zlo))
        outline = None
        if edges:
            us = [u for u, _ in pts]
            vs = [v for _, v in pts]
            if max(us) - min(us) >= 3.0 or max(vs) - min(vs) >= 3.0:
                outline = (max(0, col[0] - 55), max(0, col[1] - 60), max(0, col[2] - 55))
        dr.polygon(pts, fill=col, outline=outline)
        drawn += 1
    return drawn


def render_image(
    polys: list[dict],
    bounds: Bounds,
    edges: bool,
    tiles: Iterable[Tile] | None = None,
    zrange: tuple[float, float] | None = None,
) -> Image.Image:
    img = Image.new("RGB", (bounds.width, bounds.height), COL_BG)
    if tiles is not None:
        dr = ImageDraw.Draw(img)
        for tile in tiles:
            u0, v0 = bounds.to_px(tile.bmin[0], tile.bmin[1])
            u1, v1 = bounds.to_px(tile.bmax[0], tile.bmax[1])
            dr.rectangle([min(u0, u1), min(v0, v1), max(u0, u1), max(v0, v1)], outline=COL_TILE_BORDER)
    zlo, zhi = zrange if zrange else _percentiles([p["cz"] for p in polys], 0.02, 0.98)
    draw_polys(img, polys, bounds, zlo, zhi, edges=edges)
    return img


def overlay_probe(img: Image.Image, bounds: Bounds, csv_path: Path) -> dict:
    """Draw one F11 navprobe CSV's snapped hits (and misses) over an existing image."""
    rows = _read_probe(csv_path)
    dr = ImageDraw.Draw(img)
    hits = miss = 0
    for r in rows:
        if r["hit"] and r["px"] is not None:
            hits += 1
            u, v = bounds.to_px(r["px"], r["py"])
            dr.ellipse([u - 1.5, v - 1.5, u + 1.5, v + 1.5], fill=COL_PROBE_HIT)
        elif not r["hit"]:
            miss += 1
            u, v = bounds.to_px(r["x"], r["y"])
            dr.point([(u, v)], fill=COL_PROBE_MISS)
    return {"csv": str(csv_path), "samples": len(rows), "hits": hits, "misses": miss}


def _read_probe(csv_path: Path) -> list[dict]:
    with open(csv_path, "r", encoding="utf-8", errors="replace") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    rows: list[dict] = []
    for row in csv.DictReader(lines):
        try:
            rows.append(
                {
                    "x": float(row["x"]),
                    "y": float(row["y"]),
                    "hit": row["hit"] == "1",
                    "px": float(row["projx"]) if row.get("projx") else None,
                    "py": float(row["projy"]) if row.get("projy") else None,
                    "pz": float(row["projz"]) if row.get("projz") else None,
                }
            )
        except (KeyError, ValueError):
            continue
    if not rows:
        raise SystemExit(f"{csv_path}: no usable rows")
    return rows


def render_probe(csv_path: Path, bounds: Bounds | None, px_per_uu: float, out_path: Path) -> dict:
    """Standalone probe-grid render, same world -> pixel mapping as the navmesh render."""
    rows = _read_probe(csv_path)
    step = _probe_step(rows)
    if bounds is None:
        xs = [r["x"] for r in rows]
        ys = [r["y"] for r in rows]
        bounds = Bounds(min(xs) - step, min(ys) - step, max(xs) + step, max(ys) + step, px_per_uu)
        while (bounds.width > MAX_IMAGE_PX or bounds.height > MAX_IMAGE_PX) and bounds.px_per_uu > 1e-6:
            bounds.px_per_uu /= 2.0

    zs = [r["pz"] for r in rows if r["hit"] and r["pz"] is not None]
    zlo, zhi = (min(zs), max(zs)) if zs else (0.0, 1.0)

    img = Image.new("RGB", (bounds.width, bounds.height), COL_BG)
    draw = ImageDraw.Draw(img)
    half = step / 2.0
    hits = 0
    for r in rows:
        u0, v0 = bounds.to_px(r["x"] - half, r["y"] - half)
        u1, v1 = bounds.to_px(r["x"] + half, r["y"] + half)
        box = [min(u0, u1), min(v0, v1), max(u0, u1), max(v0, v1)]
        if r["hit"]:
            hits += 1
            t = 0.0 if zhi <= zlo else (r["pz"] - zlo) / (zhi - zlo)
            draw.rectangle(box, fill=_ramp(t))
        else:
            draw.rectangle(box, fill=(44, 46, 52))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path)
    return {
        "csv": str(csv_path),
        "png": str(out_path),
        "samples": len(rows),
        "hits": hits,
        "hit_rate": round(hits / len(rows), 4),
        "step_uu": step,
        "proj_z_min": zlo,
        "proj_z_max": zhi,
        "bounds": bounds.as_dict(),
    }


def _probe_step(rows: list[dict]) -> float:
    xs = sorted({round(r["x"], 3) for r in rows})
    diffs = [b - a for a, b in zip(xs, xs[1:]) if b - a > 1e-6]
    return min(diffs) if diffs else 100.0


# ---------------------------------------------------------------------------------
# synthetic self-test data
# ---------------------------------------------------------------------------------


def make_synthetic(out_dir: Path) -> Path:
    """
    Fabricate three adjacent 1280-uu tiles with a handful of polygons each, in the
    exact schema the dumpers emit. Tile (1,0) additionally carries a second, higher
    band so the floor splitting has something to do.
    """
    tile_size = TILE_UU
    tiles = []
    for tx, ty, base_z, extra_floor in ((0, 0, 100.0, False), (1, 0, 100.0, True), (0, 1, 160.0, False)):
        ox, oy = tx * tile_size, ty * tile_size
        verts: list[list[float]] = []
        polys: list[dict] = []

        def quad(x0: float, y0: float, x1: float, y1: float, z: float, area: int = 63) -> None:
            i = len(verts)
            verts.extend([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]])
            polys.append({"v": [i, i + 1, i + 2, i + 3], "area": area, "type": 0, "flags": 1})

        quad(ox + 40, oy + 40, ox + 1240, oy + 400, base_z)
        quad(ox + 40, oy + 400, ox + 400, oy + 1240, base_z)
        quad(ox + 700, oy + 500, ox + 1240, oy + 900, base_z + 20.0)
        i = len(verts)
        verts.extend([[ox + 500, oy + 1000, base_z], [ox + 900, oy + 1000, base_z], [ox + 700, oy + 1240, base_z]])
        polys.append({"v": [i, i + 1, i + 2], "area": 63, "type": 0, "flags": 1})
        if extra_floor:
            quad(ox + 200, oy + 200, ox + 1000, oy + 1000, base_z + 1400.0, area=2)

        zs = [v[2] for v in verts]
        tiles.append(
            {
                "x": tx,
                "y": ty,
                "layer": 0,
                "bmin": [ox, oy, min(zs)],
                "bmax": [ox + tile_size, oy + tile_size, max(zs)],
                "verts": verts,
                "polys": polys,
            }
        )

    doc = {
        "schema": "wuchang-navmesh-tiles/1",
        "generated": "20260902_000000",
        "agent": "Small",
        "primary_agent": True,
        "actor": "RecastNavMesh-Small",
        "actor_full_name": "SYNTHETIC",
        "actor_address": "0x0",
        "settings": {
            "AgentRadius": 34.0,
            "AgentHeight": 120.0,
            "TileSizeUU": TILE_UU,
            "CellSize": 10.0,
            "CellHeight": 40.0,
            "TilePoolSize": 4096,
            "PolyRefTileBits": 23,
            "PolyRefNavPolyBits": 32,
            "PolyRefSaltBits": 9,
        },
        "dtnavmesh": {
            "orig": [0.0, 0.0, 0.0],
            "tile_width": TILE_UU,
            "tile_height": TILE_UU,
            "max_tiles": 4096,
            "max_polys": 4194304,
        },
        "offsets": {"impl_ptr": "0x0", "params": "0x0", "tiles_ptr": "0x0", "tile_stride": "0x0"},
        "tile_count": len(tiles),
        "poly_count": sum(len(t["polys"]) for t in tiles),
        "vert_count": sum(len(t["verts"]) for t in tiles),
        "tiles": tiles,
    }

    agent_dir = out_dir / "Small"
    agent_dir.mkdir(parents=True, exist_ok=True)
    path = agent_dir / "tiles_20260902_000000.json"
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    return out_dir


# ---------------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------------


def render_agent(agent: str, files: list[Path], args: argparse.Namespace, out_dir: Path) -> dict | None:
    print(f"[{agent}] merging {len(files)} dump file(s), dedupe={args.dedupe}")
    dump = load_dump_files(files, agent_hint=agent, dedupe=args.dedupe)
    if not dump.tiles:
        print(f"[{agent}] no tiles in any dump - nothing to render", file=sys.stderr)
        return None

    polys = polygons_of(dump)
    total = len(polys)
    print(
        f"[{agent}] {len(dump.tiles)} tiles kept, {total} polygons; "
        f"{dump.dupes} duplicate tile records ({dump.dupes_differing} of them differing), "
        f"{dump.replaced} replaced by a richer copy"
    )

    planes = classify_flat_planes(
        polys, args.flat_plane_area,
        sheet_min=getattr(args, "flat_plane_sheet_min", DEFAULT_PLANE_SHEET_MIN),
        isolation=getattr(args, "flat_plane_isolation", DEFAULT_PLANE_ISOLATION),
    )
    if planes["candidates"]:
        dropped = ", ".join(f"Z={s['z']:.1f} x{s['polys']} ({s['reason']})" for s in planes["dropped_sheets"][:4])
        print(f"[{agent}] flat quads >= {args.flat_plane_area:g} uu2: {planes['candidates']} in "
              f"{len(planes['sheets'])} sheet(s); out of bounds: {planes['count']} "
              f"[{dropped or 'none'}] - the rest are real floors and are kept")

    dropped_area = dropped_plane = 0
    if args.exclude_area:
        excl = set(args.exclude_area)
        before = len(polys)
        polys = [p for p in polys if p["area"] not in excl]
        dropped_area = before - len(polys)
    if args.flat_planes == "drop":
        before = len(polys)
        polys = [p for p in polys if not p["plane"]]
        dropped_plane = before - len(polys)
    elif args.flat_planes == "only":
        polys = [p for p in polys if p["plane"]]
    if dropped_area or dropped_plane:
        print(f"[{agent}] dropped {dropped_area} polys by area type, {dropped_plane} flat planes")
    if not polys:
        print(f"[{agent}] every polygon was filtered out", file=sys.stderr)
        return None

    if getattr(args, "drop_islands", False):
        seeds = load_marker_seeds([Path(p) for pat in args.markers for p in sorted(glob.glob(pat))])
        polys, isl = filter_islands(
            polys, seeds,
            grid=args.island_grid, z_tol=args.island_z_tol, min_area=args.island_min_area,
            seed_radius=args.island_seed_radius, require_seed=args.island_require_seed,
        )
        print(describe_islands(agent, isl))

    if args.crop:
        x0, y0, x1, y1 = args.crop
        bounds = Bounds(min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1), args.px_per_uu)
        polys = [
            p
            for p in polys
            if max(q[0] for q in p["pts"]) >= bounds.min_x
            and min(q[0] for q in p["pts"]) <= bounds.max_x
            and max(q[1] for q in p["pts"]) >= bounds.min_y
            and min(q[1] for q in p["pts"]) <= bounds.max_y
        ]
        print(f"[{agent}] crop X {bounds.min_x:.0f}..{bounds.max_x:.0f} Y {bounds.min_y:.0f}..{bounds.max_y:.0f}"
              f" -> {len(polys)} polys")
        if not polys:
            print(f"[{agent}] crop is empty", file=sys.stderr)
            return None
    else:
        bounds = compute_bounds(polys, args.px_per_uu)

    zlo, zhi = _percentiles([p["cz"] for p in polys], 0.02, 0.98)
    print(
        f"[{agent}] world X {bounds.min_x:.0f}..{bounds.max_x:.0f}  Y {bounds.min_y:.0f}..{bounds.max_y:.0f}  "
        f"-> {bounds.width}x{bounds.height} px @ {bounds.px_per_uu:g} px/uu, Z shade {zlo:.0f}..{zhi:.0f}"
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    images = []
    tiles_for_debug = dump.tiles.values() if args.debug else None

    name = f"{agent}{args.suffix}_composite.png"
    img = render_image(polys, bounds, edges=not args.no_edges, tiles=tiles_for_debug, zrange=(zlo, zhi))
    probes = []
    for pattern in args.probe_overlay:
        for match in sorted(glob.glob(pattern)):
            probes.append(overlay_probe(img, bounds, Path(match)))
            print(f"[{agent}]   probe overlay {Path(match).name}: "
                  f"{probes[-1]['hits']} hits / {probes[-1]['misses']} misses")
    img.save(out_dir / name)
    print(f"[{agent}]   composite: {len(polys)} polys -> {name}")
    images.append({"kind": "composite", "png": name, "polys": len(polys)})

    zranges: list[list[float]] = []
    floors = 0
    if args.floors:
        if args.floor_mode == "bands":
            floors = assign_floors_bands(polys, args.floor_gap)
        else:
            floors = assign_floors_grid(polys, args.floor_gap, args.floor_grid, args.floor_smooth)
        zranges = floor_z_ranges(polys, floors)
        print(f"[{agent}] floor mode {args.floor_mode}: {floors} floor(s)")
        for i in range(floors):
            sub = [p for p in polys if p["floor"] == i]
            if not sub:
                continue
            fimg = render_image(sub, bounds, edges=not args.no_edges, tiles=tiles_for_debug, zrange=(zlo, zhi))
            fname = f"{agent}{args.suffix}_floor{i}.png"
            fimg.save(out_dir / fname)
            print(f"[{agent}]   floor {i}: {len(sub)} polys, Z {zranges[i][0]:.0f}..{zranges[i][1]:.0f} -> {fname}")
            images.append(
                {"kind": "floor", "floor": i, "png": fname, "polys": len(sub),
                 "z_min": zranges[i][0], "z_max": zranges[i][1]}
            )

    return {
        "agent": agent,
        "source_files": [Path(f).name for f in dump.files],
        "tile_count": len(dump.tiles),
        "tile_duplicates": dump.dupes,
        "tile_duplicates_differing": dump.dupes_differing,
        "dedupe": args.dedupe,
        "poly_count_total": total,
        "poly_count_drawn": len(polys),
        "flat_planes": planes,
        "excluded_areas": args.exclude_area,
        "flat_planes_mode": args.flat_planes,
        "cell_uu": CELL_UU,
        "tile_uu": TILE_UU,
        "z_shade": [zlo, zhi],
        "floor_mode": args.floor_mode if args.floors else None,
        "floor_gap_uu": args.floor_gap,
        "floor_grid_uu": args.floor_grid,
        "floor_smooth_passes": args.floor_smooth,
        "floors": zranges,
        "images": images,
        "probe_overlays": probes,
        "settings": dump.header.get("settings", {}),
        "dtnavmesh": dump.header.get("dtnavmesh", {}),
        "offsets": dump.header.get("offsets", {}),
        **bounds.as_dict(),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", type=Path, help="navmesh root (with agent subdirs) or one agent directory")
    ap.add_argument("--agent", action="append", default=[], help="restrict to this agent (repeatable)")
    ap.add_argument("--out", type=Path, default=Path("out"), help="output directory (default: out)")
    ap.add_argument("--suffix", default="", help="appended to every output file name (e.g. _spot1)")
    ap.add_argument("--px-per-uu", type=float, default=DEFAULT_PX_PER_UU, help=f"pixels per uu (default {DEFAULT_PX_PER_UU})")
    ap.add_argument("--crop", nargs=4, type=float, metavar=("X0", "Y0", "X1", "Y1"), help="world-space crop window")
    ap.add_argument("--dedupe", choices=("richest", "newest", "all"), default="richest",
                    help="overlap-ring tile collisions: keep the richer copy (default), the newest, or all")
    ap.add_argument("--floors", action="store_true", help="additionally write a per-floor image set")
    ap.add_argument("--floor-mode", choices=("grid", "bands"), default="grid", help="floor clustering strategy")
    ap.add_argument("--floor-gap", type=float, default=DEFAULT_FLOOR_GAP, help="Z gap that starts a new floor, uu")
    ap.add_argument("--floor-grid", type=float, default=DEFAULT_FLOOR_GRID, help="XY grid for --floor-mode grid, uu")
    ap.add_argument("--floor-smooth", type=int, default=3, help="median smoothing passes for --floor-mode grid")
    ap.add_argument("--exclude-area", action="append", type=int, default=[],
                    help="drop polygons with this dtPoly area value (repeatable)")
    ap.add_argument("--flat-planes", choices=("keep", "drop", "only"), default="drop",
                    help="what to do with big flat single-quad planes (water / landscape)")
    ap.add_argument("--flat-plane-area", type=float, default=DEFAULT_FLAT_PLANE_AREA,
                    help="XY area (uu2) above which a flat polygon counts as a plane candidate")
    add_plane_args(ap)
    add_island_args(ap, default_on=False)
    ap.add_argument("--no-edges", action="store_true", help="do not draw polygon edges")
    ap.add_argument("--debug", action="store_true", help="draw tile borders as well")
    ap.add_argument("--probe-overlay", nargs="*", default=[], help="navprobe CSV(s) to overlay on the render")
    ap.add_argument("--probe", nargs="*", default=[], help="navprobe CSV(s) to render as standalone images")
    ap.add_argument("--synthetic", action="store_true", help="fabricate tiles and render them (self-test)")
    args = ap.parse_args(argv)

    out_dir: Path = args.out
    manifest: dict = {"schema": "wuchang-navmesh-render/2", "agents": [], "probes": []}

    if args.synthetic:
        synth_root = out_dir / "_synthetic_input"
        make_synthetic(synth_root)
        args.input = synth_root
        args.floors = True
        print(f"synthetic input written to {synth_root}")

    if not args.input and not args.probe:
        ap.error("nothing to do: pass --input, --probe or --synthetic")

    primary_bounds: Bounds | None = None

    if args.input:
        root: Path = args.input
        if not root.exists():
            return _fail(f"input path does not exist: {root}")
        agents = discover_agents(root)
        if args.agent:
            wanted = {a.lower() for a in args.agent}
            agents = {k: v for k, v in agents.items() if k.lower() in wanted}
        if not agents:
            return _fail(f"no tiles_*.json found under {root}")
        for agent, files in agents.items():
            info = render_agent(agent, files, args, out_dir)
            if info:
                manifest["agents"].append(info)
                if primary_bounds is None or agent.lower() == "small":
                    primary_bounds = Bounds(
                        info["min_x"], info["min_y"], info["max_x"], info["max_y"], info["px_per_uu"]
                    )

    for pattern in args.probe:
        for match in sorted(glob.glob(pattern)):
            path = Path(match)
            out_png = out_dir / f"probe_{path.stem}{args.suffix}.png"
            try:
                info = render_probe(path, primary_bounds, args.px_per_uu, out_png)
            except SystemExit as exc:
                print(f"  ! {exc}", file=sys.stderr)
                continue
            print(f"[probe] {path.name}: {info['hits']}/{info['samples']} hits ({info['hit_rate']:.1%}) -> {out_png.name}")
            manifest["probes"].append(info)

    if primary_bounds is not None:
        manifest.update(primary_bounds.as_dict())
    elif manifest["probes"]:
        manifest.update(manifest["probes"][0]["bounds"])

    if manifest["agents"] or manifest["probes"]:
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / f"bounds{args.suffix}.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"wrote {out_dir / f'bounds{args.suffix}.json'}")
        return 0
    return _fail("nothing was rendered")


def _fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
