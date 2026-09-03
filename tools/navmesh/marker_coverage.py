#!/usr/bin/env python3
"""
marker_coverage.py - which markers have no walkable navmesh under them?

The map background is the navmesh, and the runtime slices it at the player's feet Z.
So a place with markers but no navmesh polygon at their Z is exactly what the user
sees as a black hole. Markers are the only offline ground truth we have for "a player
can be here", which makes this the audit that catches a missing floor without
launching the game.

    python marker_coverage.py --input <dumps root>/ch1 --markers ../../markers/chapter1.json
    python marker_coverage.py --input dumps/ch1 --markers ../../markers/chapter1.json \\
                              --no-filter          # audit the RAW navmesh instead

For every marker it reports the Z of every polygon whose XY contains the marker, and
flags the ones with nothing within `--z-tol` (default 400 uu) of the marker's own Z.
`--filters` mirrors what `build_map.py` ships: the out-of-bounds sheet drop and the
island filter, so a flag here is a flag on the shipped asset.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render  # noqa: E402


def build_index(polys: list[dict], cell: float = 640.0) -> dict[tuple[int, int], list[int]]:
    idx: dict[tuple[int, int], list[int]] = {}
    for i, p in enumerate(polys):
        xs = [q[0] for q in p["pts"]]
        ys = [q[1] for q in p["pts"]]
        for gx in range(int(math.floor(min(xs) / cell)), int(math.floor(max(xs) / cell)) + 1):
            for gy in range(int(math.floor(min(ys) / cell)), int(math.floor(max(ys) / cell)) + 1):
                idx.setdefault((gx, gy), []).append(i)
    return idx


def surfaces_at(polys, idx, x: float, y: float, cell: float = 640.0) -> list[float]:
    out = []
    for i in idx.get((int(math.floor(x / cell)), int(math.floor(y / cell))), ()):
        p = polys[i]
        if render._point_in_poly(p["pts"], x, y):
            out.append(p["cz"])
    return sorted(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="tile-dump root with agent subdirs")
    ap.add_argument("--agent", default="Small")
    ap.add_argument("--z-tol", type=float, default=400.0, help="a surface this close counts as 'under' (uu)")
    ap.add_argument("--no-filter", action="store_true", help="audit the raw navmesh, before any filtering")
    ap.add_argument("--no-islands", action="store_true", help="apply the sheet drop but not the island filter")
    ap.add_argument("--cats", default="", help="only these marker categories (comma separated)")
    ap.add_argument("--md", help="also write the report as markdown to this path")
    render.add_plane_args(ap)
    render.add_island_args(ap, default_on=True)
    a = ap.parse_args(argv)

    root = Path(a.input)
    agents = render.discover_agents(root if root.is_absolute() else Path(__file__).resolve().parent / root)
    if a.agent not in agents:
        sys.exit(f"agent '{a.agent}' not found under {root} (have: {', '.join(sorted(agents)) or 'nothing'})")
    dump = render.load_dump_files(agents[a.agent], agent_hint=a.agent, dedupe="richest")
    polys = render.polygons_of(dump)
    raw = len(polys)
    if not a.no_filter:
        render.classify_flat_planes(polys, render.DEFAULT_FLAT_PLANE_AREA,
                                    sheet_min=a.flat_plane_sheet_min, isolation=a.flat_plane_isolation)
        polys = [p for p in polys if not p["plane"]]
    if not a.markers:
        sys.exit("--markers is required (markers/chapterN.json; globs ok)")
    marker_files = [Path(p) for pat in a.markers for p in sorted(glob.glob(pat))]
    seeds = render.load_marker_seeds(marker_files)
    if not a.no_filter and not a.no_islands:
        polys, st = render.filter_islands(
            polys, seeds, grid=a.island_grid, z_tol=a.island_z_tol, min_area=a.island_min_area,
            seed_radius=a.island_seed_radius, require_seed=a.island_require_seed,
            bridge_xy=a.island_bridge_xy, bridge_z=a.island_bridge_z,
            cluster_area=a.island_cluster_area)
        print(render.describe_islands(a.agent, st))
    print(f"{raw} polygons decoded, {len(polys)} after filtering")

    cats = set(c for c in a.cats.split(",") if c)
    idx = build_index(polys)
    rows, missing = [], []
    for path in marker_files:
        doc = json.loads(path.read_text(encoding="utf-8"))
        for m in doc.get("markers", []):
            if cats and m.get("cat") not in cats:
                continue
            zs = surfaces_at(polys, idx, m["x"], m["y"])
            near = [z for z in zs if abs(z - m["z"]) <= a.z_tol]
            row = {"cat": m.get("cat"), "cls": m.get("cls"), "name": m.get("name"), "id": m.get("id"),
                   "level": m.get("level"), "x": m["x"], "y": m["y"], "z": m["z"],
                   "surfaces": [round(z) for z in zs], "under": [round(z) for z in near],
                   "dz": None if not zs else round(min(abs(z - m["z"]) for z in zs))}
            rows.append(row)
            if not near:
                missing.append(row)

    by_cat = Counter(r["cat"] for r in rows)
    miss_cat = Counter(r["cat"] for r in missing)
    print(f"\n{len(rows)} markers, {len(missing)} with NO surface within {a.z_tol:g} uu of their Z")
    for c in sorted(by_cat, key=lambda c: -miss_cat[c]):
        print(f"  {c:<10} {miss_cat[c]:>4} / {by_cat[c]:<4} uncovered")
    print("\nuncovered markers (nearest surface dz, then the surface Zs at that XY):")
    for r in sorted(missing, key=lambda r: (r["cat"] or "", -(r["dz"] or 0))):
        print(f"  {r['cat']:<9} {str(r['cls'])[:30]:<30} {r['x']:>9.0f} {r['y']:>9.0f} {r['z']:>8.0f} "
              f" dz={r['dz']}  surfaces={r['surfaces'][:6]}  {r['level']}")

    if a.md:
        out = [f"# Marker coverage audit - `{a.input}`, agent `{a.agent}`", "",
               f"* polygons: {raw} decoded, {len(polys)} after filtering"
               f"{' (no filtering)' if a.no_filter else ''}",
               f"* markers: {len(rows)}; **{len(missing)} with no walkable surface within {a.z_tol:g} uu**", ""]
        out += ["| cat | uncovered | total |", "|---|---:|---:|"]
        out += [f"| `{c}` | {miss_cat[c]} | {by_cat[c]} |" for c in sorted(by_cat)]
        out += ["", "## Uncovered markers", "",
                "| cat | class | x | y | z | nearest dz | surfaces at that XY | level |",
                "|---|---|---:|---:|---:|---:|---|---|"]
        out += [f"| `{r['cat']}` | `{r['cls']}` | {r['x']:.0f} | {r['y']:.0f} | {r['z']:.0f} | "
                f"{r['dz']} | {r['surfaces'][:6]} | `{r['level']}` |" for r in missing]
        Path(a.md).write_text("\n".join(out) + "\n", encoding="utf-8")
        print(f"\nwrote {a.md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
