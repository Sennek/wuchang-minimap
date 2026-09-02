#!/usr/bin/env python3
"""
Render the WuchangMinimap navmesh dumps as top-down walkable-area images.

Input
-----
The C++ dumper writes one JSON per dump into

    <game>\\Project_Plague\\Binaries\\Win64\\ue4ss\\Mods\\WuchangMinimap\\navmesh\\<agent>\\tiles_<ts>.json

(schema "wuchang-navmesh-tiles/1": a header with the actor settings, the dtNavMesh
params and every offset the dumper discovered, then one entry per live tile with its
bounds, its vertex array and its polygons as vertex indices).

Only 4-6 streaming cells are ever resident, so a full map is the union of many dumps.
This script merges them: for every (tile x, tile y, layer) it keeps the newest version
it can find, then fills every polygon.

Coordinate convention
---------------------
Unreal is X = forward (north), Y = right (east), Z = up. For a north-up image:

    u (column) = (world_Y - min_y) * px_per_uu        east  -> right
    v (row)    = (max_x - world_X) * px_per_uu        north -> up

so image width comes from the world Y extent and height from the world X extent.
Everything written to bounds.json is in world units (uu, 1 uu = 1 cm).

Floors
------
Stacked geometry (a building over a courtyard) has to be split or it renders as mush.
Per 12 800-uu streaming cell - the grid the game itself streams on - the polygon
centroid Z values are sorted and split wherever the gap exceeds --floor-gap (600 uu by
default). The bands of a cell are then ranked by height, and a polygon's floor index is
the rank of its band inside its own cell. So floor 0 is "the lowest surface everywhere",
floor 1 is "the first thing stacked on top of it", and a single-level area contributes
to floor 0 only.

Usage
-----
    # render every agent found under a navmesh root
    python render.py --input "<...>/Mods/WuchangMinimap/navmesh" --out out

    # one agent, bigger, with polygon edges drawn
    python render.py --input "<...>/navmesh" --agent Small --px-per-uu 0.05 --debug

    # self-test with fabricated tiles - works before any real dump exists
    python render.py --synthetic --out out_synthetic

    # cross-check target: render the F11 probe CSVs through the same bounds logic
    python render.py --probe ../../../.workspace/wuchang-minimap/context/dumps/navprobe_*.csv --out out
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

CELL_UU = 12800.0  # Wuchang's streaming cell, 128 m (see context/wuchang-classes.md)
DEFAULT_FLOOR_GAP = 600.0  # uu; a gap larger than this starts a new floor
DEFAULT_PX_PER_UU = 0.02  # 1 px = 50 uu = 0.5 m
MAX_IMAGE_PX = 16000  # guard against a --px-per-uu typo eating all the RAM

COL_BG = (16, 18, 22)
COL_FILL = (86, 168, 118)
COL_FILL_ALT = (60, 120, 86)  # every second floor, so overlaps stay legible
COL_EDGE = (232, 244, 236)
COL_TILE_BORDER = (70, 76, 84)
COL_PROBE_MISS = (44, 46, 52)

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
# loading
# ---------------------------------------------------------------------------------


def load_dump_files(paths: Iterable[Path], agent_hint: str = "") -> Dump:
    """Merge several tile JSONs, newest wins per (x, y, layer)."""
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
            dump.tiles[tile.key] = tile
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


def polygons_of(dump: Dump) -> list[dict]:
    """Flatten tiles into world-space polygons with a centroid."""
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
            cz = sum(p[2] for p in pts) / len(pts)
            cx = sum(p[0] for p in pts) / len(pts)
            cy = sum(p[1] for p in pts) / len(pts)
            out.append(
                {
                    "pts": pts,
                    "cx": cx,
                    "cy": cy,
                    "cz": cz,
                    "area": poly.get("area", 0),
                    "flags": poly.get("flags", 0),
                    "tile": tile.key,
                }
            )
    return out


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
    start = vs[0]
    prev = vs[0]
    for v in vs[1:]:
        if v - prev > gap:
            bands.append((start, prev))
            start = v
        prev = v
    bands.append((start, prev))
    return bands


def assign_floors(polys: list[dict], gap: float) -> int:
    """
    Per 12 800-uu cell, cluster centroid Z into bands and rank them low -> high.
    A polygon's floor is the rank of its band inside its own cell. Returns the
    number of floors used, and writes poly["floor"].
    """
    cells: dict[tuple[int, int], list[float]] = {}
    for poly in polys:
        cell = (math.floor(poly["cx"] / CELL_UU), math.floor(poly["cy"] / CELL_UU))
        poly["cell"] = cell
        cells.setdefault(cell, []).append(poly["cz"])

    cell_bands = {cell: split_bands(zs, gap) for cell, zs in cells.items()}

    for poly in polys:
        bands = cell_bands[poly["cell"]]
        floor = 0
        for i, (lo, hi) in enumerate(bands):
            if lo - 1e-6 <= poly["cz"] <= hi + 1e-6:
                floor = i
                break
        poly["floor"] = floor

    return max((len(b) for b in cell_bands.values()), default=1)


def floor_z_ranges(polys: list[dict], floors: int) -> list[list[float]]:
    ranges: list[list[float]] = []
    for i in range(floors):
        zs = [p["cz"] for p in polys if p["floor"] == i]
        ranges.append([min(zs), max(zs)] if zs else [0.0, 0.0])
    return ranges


# ---------------------------------------------------------------------------------
# drawing
# ---------------------------------------------------------------------------------


def render_floor(polys: list[dict], bounds: Bounds, floor: int, debug: bool, tiles: Iterable[Tile]) -> Image.Image:
    img = Image.new("RGB", (bounds.width, bounds.height), COL_BG)
    draw = ImageDraw.Draw(img)

    if debug:
        for tile in tiles:
            u0, v0 = bounds.to_px(tile.bmin[0], tile.bmin[1])
            u1, v1 = bounds.to_px(tile.bmax[0], tile.bmax[1])
            draw.rectangle([min(u0, u1), min(v0, v1), max(u0, u1), max(v0, v1)], outline=COL_TILE_BORDER)

    fill = COL_FILL if floor % 2 == 0 else COL_FILL_ALT
    drawn = 0
    for poly in polys:
        if poly["floor"] != floor:
            continue
        pts = [bounds.to_px(p[0], p[1]) for p in poly["pts"]]
        draw.polygon(pts, fill=fill, outline=COL_EDGE if debug else None)
        drawn += 1
    return img


def render_probe(csv_path: Path, bounds: Bounds | None, px_per_uu: float, out_path: Path) -> dict:
    """
    Render one WuchangRecon F11 navprobe CSV. Same world -> pixel mapping as the
    navmesh render, so the two images can be overlaid to check alignment.
    """
    rows: list[dict] = []
    with open(csv_path, "r", encoding="utf-8", errors="replace") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    for row in csv.DictReader(lines):
        try:
            rows.append(
                {
                    "x": float(row["x"]),
                    "y": float(row["y"]),
                    "hit": row["hit"] == "1",
                    "pz": float(row["projz"]) if row.get("projz") else None,
                }
            )
        except (KeyError, ValueError):
            continue
    if not rows:
        raise SystemExit(f"{csv_path}: no usable rows")

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
            draw.rectangle(box, fill=COL_PROBE_MISS)
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
    """Smallest non-zero spacing between neighbouring probe X values."""
    xs = sorted({round(r["x"], 3) for r in rows})
    diffs = [b - a for a, b in zip(xs, xs[1:]) if b - a > 1e-6]
    return min(diffs) if diffs else 100.0


def _ramp(t: float) -> tuple[int, int, int]:
    """Purple -> teal -> yellow. Self-contained so matplotlib is not required."""
    t = max(0.0, min(1.0, t))
    stops = [(0.0, (68, 1, 84)), (0.35, (49, 104, 142)), (0.7, (53, 183, 121)), (1.0, (253, 231, 37))]
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(int(round(a + (b - a) * f)) for a, b in zip(c0, c1))  # type: ignore[return-value]
    return stops[-1][1]


# ---------------------------------------------------------------------------------
# synthetic self-test data
# ---------------------------------------------------------------------------------


def make_synthetic(out_dir: Path) -> Path:
    """
    Fabricate three adjacent 1280-uu tiles with a handful of polygons each, in the
    exact schema the C++ dumper emits. Tile (1,0) additionally carries a second,
    higher band so the floor splitting has something to do.
    """
    tile_size = 1280.0
    tiles = []
    for tx, ty, base_z, extra_floor in ((0, 0, 100.0, False), (1, 0, 100.0, True), (0, 1, 160.0, False)):
        ox, oy = tx * tile_size, ty * tile_size
        verts: list[list[float]] = []
        polys: list[dict] = []

        def quad(x0: float, y0: float, x1: float, y1: float, z: float, area: int = 1) -> None:
            i = len(verts)
            verts.extend([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]])
            polys.append({"v": [i, i + 1, i + 2, i + 3], "area": area, "type": 0, "flags": 1})

        # an L-shaped walkable floor with a square hole in the middle
        quad(ox + 40, oy + 40, ox + 1240, oy + 400, base_z)
        quad(ox + 40, oy + 400, ox + 400, oy + 1240, base_z)
        quad(ox + 700, oy + 500, ox + 1240, oy + 900, base_z + 20.0)
        # a triangle, to prove non-quad polygons render
        i = len(verts)
        verts.extend([[ox + 500, oy + 1000, base_z], [ox + 900, oy + 1000, base_z], [ox + 700, oy + 1240, base_z]])
        polys.append({"v": [i, i + 1, i + 2], "area": 1, "type": 0, "flags": 1})
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
            "TileSizeUU": 1280.0,
            "CellSize": 10.0,
            "CellHeight": 40.0,
            "TilePoolSize": 4096,
            "PolyRefTileBits": 23,
            "PolyRefNavPolyBits": 32,
            "PolyRefSaltBits": 9,
        },
        "dtnavmesh": {
            "orig": [0.0, 0.0, 0.0],
            "tile_width": 1280.0,
            "tile_height": 1280.0,
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
    print(f"[{agent}] merging {len(files)} dump file(s)")
    dump = load_dump_files(files, agent_hint=agent)
    if not dump.tiles:
        print(f"[{agent}] no tiles in any dump - nothing to render", file=sys.stderr)
        return None

    polys = polygons_of(dump)
    print(f"[{agent}] {len(dump.tiles)} unique tiles, {len(polys)} polygons")
    if not polys:
        print(f"[{agent}] tiles carry no usable polygons", file=sys.stderr)
        return None

    bounds = compute_bounds(polys, args.px_per_uu)
    floors = assign_floors(polys, args.floor_gap)
    zranges = floor_z_ranges(polys, floors)
    print(
        f"[{agent}] world X {bounds.min_x:.0f}..{bounds.max_x:.0f}  Y {bounds.min_y:.0f}..{bounds.max_y:.0f}  "
        f"-> {bounds.width}x{bounds.height} px @ {bounds.px_per_uu:g} px/uu, {floors} floor(s)"
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    images = []
    for i in range(floors):
        n = sum(1 for p in polys if p["floor"] == i)
        if n == 0:
            continue
        img = render_floor(polys, bounds, i, args.debug, dump.tiles.values())
        name = f"{agent}_floor{i}.png"
        img.save(out_dir / name)
        print(f"[{agent}]   floor {i}: {n} polys, Z {zranges[i][0]:.0f}..{zranges[i][1]:.0f} -> {name}")
        images.append({"floor": i, "png": name, "polys": n, "z_min": zranges[i][0], "z_max": zranges[i][1]})

    return {
        "agent": agent,
        "source_files": [Path(f).name for f in dump.files],
        "tile_count": len(dump.tiles),
        "poly_count": len(polys),
        "floor_gap_uu": args.floor_gap,
        "cell_uu": CELL_UU,
        "floors": zranges,
        "images": images,
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
    ap.add_argument("--px-per-uu", type=float, default=DEFAULT_PX_PER_UU, help=f"pixels per uu (default {DEFAULT_PX_PER_UU})")
    ap.add_argument("--floor-gap", type=float, default=DEFAULT_FLOOR_GAP, help="Z gap that starts a new floor, uu")
    ap.add_argument("--debug", action="store_true", help="draw polygon edges and tile borders")
    ap.add_argument("--probe", nargs="*", default=[], help="WuchangRecon navprobe CSV(s) to render as a cross-check")
    ap.add_argument("--synthetic", action="store_true", help="fabricate tiles and render them (self-test)")
    args = ap.parse_args(argv)

    out_dir: Path = args.out
    manifest: dict = {"schema": "wuchang-navmesh-render/1", "agents": [], "probes": []}

    if args.synthetic:
        synth_root = out_dir / "_synthetic_input"
        make_synthetic(synth_root)
        args.input = synth_root
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
            out_png = out_dir / f"probe_{path.stem}.png"
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
        (out_dir / "bounds.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"wrote {out_dir / 'bounds.json'}")
        return 0
    return _fail("nothing was rendered")


def _fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
