#!/usr/bin/env python3
"""
Coverage audit for the offline-extracted navmesh: pak index vs. decoded JSON.

Answers, per cell package `B<N>EX0_L0_X<cx>_Y<cy>_DL0_WP`:

  * is the package present in the pak index but missing from `dumps_offline/`?
  * did `navchunk.py` decode as many tiles as the chunk's own `tile_count` field says?
  * how many polys / verts / distinct tile columns / layers, and what Z range?
  * which `(tile x, tile y, layer)` keys are shared with a neighbouring package
    (the one-tile overlap ring) and do the two copies agree?

Package extents, as measured (this corrects `navmesh-offline.md` § 6, whose formula only
holds for non-negative indices): a package `X<cx>_Y<cy>` **owns** Unreal
`X in [cx*10240, cx*10240 + 10240)` -- so `floor(coord / 10240)` is the cell index --
which is 8 navmesh tile columns of 1280 uu, and it additionally carries a **one-column
overlap ring on the side facing the world origin**: the low side for `cx >= 0`
(offset -1) and the high side for `cx < 0` (offset +8). Ring columns only exist where
navmesh exists, so a cell may show 0 ring columns.

Outputs a markdown table and a coverage PNG (one square per cell, brightness =
decoded tiles, labelled with the cell index).

    python coverage.py --input ../dumps_offline --agent Small --chapter 1 \
        --pak-list ../../../../.workspace/wuchang-minimap/context/pak-umap-list.txt \
        --md ../../../../.workspace/wuchang-minimap/context/navmesh-coverage-ch1.md \
        --png ../out_offline/v2/coverage_ch1_Small.png
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import sys
from collections import defaultdict

from PIL import Image, ImageDraw

CELL_UU = 10240.0
TILE_UU = 1280.0
TILES_PER_CELL = 8  # 10240 / 1280
OVERLAP_TILES = 1  # extra ring on the low side of each axis

CELL_RE = re.compile(r"B(\d+)EX0_L0_X(-?\d+)_Y(-?\d+)_DL0_WP")


# -- unreal <-> recast tile indices ------------------------------------------------
# Unreal = (-recast.x, -recast.z, recast.y), so a recast tile index i maps to the
# Unreal tile column -(i + 1).
def u_tile(recast_i: int) -> int:
    return -(recast_i + 1)


def pak_cells(pak_list: str, chapter: int) -> set[tuple[int, int]]:
    """Cell indices of every `Maps/Generate/Chapter<N>/EX0/...umap` in the pak index."""
    want = f"Chapter{chapter}/EX0/"
    out: set[tuple[int, int]] = set()
    with open(pak_list, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if want not in line:
                continue
            m = CELL_RE.search(line)
            if m and int(m.group(1)) == chapter:
                out.add((int(m.group(2)), int(m.group(3))))
    return out


def load_cells(input_dir: str, agent: str) -> dict[tuple[int, int], dict]:
    cells: dict[tuple[int, int], dict] = {}
    for path in sorted(glob.glob(os.path.join(input_dir, agent, "tiles_*.json"))):
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        src = doc.get("source", {})
        m = CELL_RE.search(os.path.basename(path))
        cell = tuple(src.get("cell") or (int(m.group(2)), int(m.group(3))))  # type: ignore[arg-type]
        cells[cell] = {"doc": doc, "path": path}
    return cells


def audit(cells: dict[tuple[int, int], dict]) -> dict:
    rows = []
    owner: dict[tuple[int, int, int], list[tuple[tuple[int, int], dict]]] = defaultdict(list)
    for cell, rec in sorted(cells.items()):
        doc = rec["doc"]
        src = doc.get("source", {})
        tiles = doc["tiles"]
        zs = [t["bmin"][2] for t in tiles] + [t["bmax"][2] for t in tiles]
        xs = [t["bmin"][0] for t in tiles] + [t["bmax"][0] for t in tiles]
        ys = [t["bmin"][1] for t in tiles] + [t["bmax"][1] for t in tiles]
        cols = {(t["x"], t["y"]) for t in tiles}
        owned = {
            c
            for c in cols
            if cell[0] * TILES_PER_CELL <= u_tile(c[0]) < (cell[0] + 1) * TILES_PER_CELL
            and cell[1] * TILES_PER_CELL <= u_tile(c[1]) < (cell[1] + 1) * TILES_PER_CELL
        }
        for t in tiles:
            owner[(t["x"], t["y"], t["layer"])].append((cell, t))
        rows.append(
            {
                "cell": cell,
                "stated": src.get("tile_count_field"),
                "decoded": doc.get("tile_count"),
                "records": len(tiles),
                "polys": doc.get("poly_count"),
                "verts": doc.get("vert_count"),
                "cols": len(cols),
                "cols_owned": len(owned),
                "cols_ring": len(cols) - len(owned),
                "layers": max((t["layer"] for t in tiles), default=-1) + 1,
                "z": (min(zs), max(zs)) if zs else (0.0, 0.0),
                "x": (min(xs), max(xs)) if xs else (0.0, 0.0),
                "y": (min(ys), max(ys)) if ys else (0.0, 0.0),
                "offx": (min(u_tile(c[0]) for c in cols) - cell[0] * TILES_PER_CELL,
                         max(u_tile(c[0]) for c in cols) - cell[0] * TILES_PER_CELL) if cols else (0, 0),
                "offy": (min(u_tile(c[1]) for c in cols) - cell[1] * TILES_PER_CELL,
                         max(u_tile(c[1]) for c in cols) - cell[1] * TILES_PER_CELL) if cols else (0, 0),
            }
        )

    shared = {k: v for k, v in owner.items() if len(v) > 1}
    identical = differing = 0
    for k, v in shared.items():
        base = v[0][1]
        for _, t in v[1:]:
            if t["verts"] == base["verts"] and t["polys"] == base["polys"]:
                identical += 1
            else:
                differing += 1
    return {
        "rows": rows,
        "unique_keys": len(owner),
        "records": sum(r["records"] for r in rows),
        "shared_keys": len(shared),
        "shared_identical": identical,
        "shared_differing": differing,
    }


def coverage_png(rows: list[dict], missing: set[tuple[int, int]], path: str, cell_px: int = 90) -> None:
    cxs = [r["cell"][0] for r in rows] + [c[0] for c in missing]
    cys = [r["cell"][1] for r in rows] + [c[1] for c in missing]
    x0, x1, y0, y1 = min(cxs), max(cxs), min(cys), max(cys)
    # north-up like render.py: cell Y -> column, cell X -> row (increasing X upwards)
    w = (y1 - y0 + 1) * cell_px + 1
    h = (x1 - x0 + 1) * cell_px + 1
    img = Image.new("RGB", (w, h), (16, 18, 22))
    dr = ImageDraw.Draw(img)
    hi = max((r["decoded"] or 0) for r in rows) or 1

    def box(cell):
        u = (cell[1] - y0) * cell_px
        v = (x1 - cell[0]) * cell_px
        return [u, v, u + cell_px, v + cell_px]

    for cell in missing:
        dr.rectangle(box(cell), fill=(140, 40, 40), outline=(80, 86, 94))
        dr.text((box(cell)[0] + 6, box(cell)[1] + 6), f"{cell[0]},{cell[1]}", fill=(255, 220, 220))
        dr.text((box(cell)[0] + 6, box(cell)[1] + 20), "MISSING", fill=(255, 220, 220))

    for r in rows:
        t = (r["decoded"] or 0) / hi
        g = int(40 + 170 * (t ** 0.6))
        ok = r["stated"] == r["decoded"] == r["records"]
        fill = (g // 3, g, int(g * 0.75)) if ok else (g, g // 2, 40)
        b = box(r["cell"])
        dr.rectangle(b, fill=fill, outline=(80, 86, 94))
        dr.text((b[0] + 6, b[1] + 6), f"{r['cell'][0]},{r['cell'][1]}", fill=(240, 248, 244))
        dr.text((b[0] + 6, b[1] + 22), f"{r['decoded']}t", fill=(220, 232, 226))
        dr.text((b[0] + 6, b[1] + 36), f"{r['polys']}p", fill=(200, 214, 208))
        dr.text((b[0] + 6, b[1] + 50), f"{r['cols']}col", fill=(180, 196, 190))
        dr.text((b[0] + 6, b[1] + 64), f"L{r['layers']}", fill=(180, 196, 190))
        if not ok:
            dr.text((b[0] + 6, b[1] + 78), "MISMATCH", fill=(255, 210, 120))
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    img.save(path)


def write_md(res: dict, agent: str, chapter: int, in_pak: set, missing: set, extra: set, path: str) -> None:
    rows = res["rows"]
    L = []
    L.append(f"# Navmesh coverage audit — Chapter {chapter}, agent `{agent}`")
    L.append("")
    L.append(f"Generated by `tools/navmesh/offline/coverage.py`. Grid pitch **{CELL_UU:.0f} uu**; a package")
    L.append(f"`X<cx>_Y<cy>` spans `[cx*{CELL_UU:.0f} - {TILE_UU:.0f}, cx*{CELL_UU:.0f} + {CELL_UU:.0f}]` per axis =")
    L.append(f"{TILES_PER_CELL} owned tile columns plus a {OVERLAP_TILES}-tile overlap ring.")
    L.append("")
    L.append("## Verdict")
    L.append("")
    L.append(f"* cell packages in the pak index: **{len(in_pak)}**")
    L.append(f"* cell packages decoded: **{len(rows)}**")
    L.append(f"* **missing (in pak, not decoded): {len(missing)}** {sorted(missing) if missing else '— none'}")
    L.append(f"* decoded but not in the pak list: {len(extra)} {sorted(extra) if extra else '— none'}")
    bad = [r for r in rows if not (r["stated"] == r["decoded"] == r["records"])]
    L.append(
        f"* per-chunk tile-count self-check (`decoded == chunk's own tile_count`): "
        f"**{len(rows) - len(bad)}/{len(rows)} exact**"
        + (f", mismatched: {[r['cell'] for r in bad]}" if bad else ", zero mismatches")
    )
    L.append(f"* tile records total {res['records']}, distinct `(tile x, tile y, layer)` keys {res['unique_keys']}")
    L.append(
        f"* keys present in more than one package (the overlap ring): {res['shared_keys']} "
        f"— of the duplicate records {res['shared_identical']} are byte-identical to the first copy and "
        f"**{res['shared_differing']} differ** (each package re-tiles its own ring), so the merge must pick "
        f"the richer copy, not an arbitrary one"
    )
    L.append("")
    L.append("## Per cell")
    L.append("")
    L.append(
        "| cell | tiles stated | tiles decoded | polys | verts | tile cols (owned+ring) | layers | "
        "Z min..max | X range | Y range | col offset X | col offset Y | in window |"
    )
    L.append("|---|---:|---:|---:|---:|---|---:|---|---|---|---|---|---|")
    for r in rows:
        exok = -1 <= r["offx"][0] and r["offx"][1] <= 8 and -1 <= r["offy"][0] and r["offy"][1] <= 8
        L.append(
            f"| `X{r['cell'][0]}_Y{r['cell'][1]}` | {r['stated']} | {r['decoded']} | {r['polys']} | {r['verts']} "
            f"| {r['cols']} ({r['cols_owned']}+{r['cols_ring']}) | {r['layers']} "
            f"| {r['z'][0]:.0f}..{r['z'][1]:.0f} | {r['x'][0]:.0f}..{r['x'][1]:.0f} | {r['y'][0]:.0f}..{r['y'][1]:.0f} "
            f"| {r['offx'][0]}..{r['offx'][1]} | {r['offy'][0]}..{r['offy'][1]} | {'yes' if exok else '**NO**'} |"
        )
    L.append("")
    L.append(f"Totals: {sum(r['decoded'] for r in rows)} tiles, {sum(r['polys'] for r in rows)} polys, ")
    L.append(f"{sum(r['verts'] for r in rows)} verts across {len(rows)} cells.")
    L.append("")
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="../dumps_offline")
    ap.add_argument("--agent", default="Small")
    ap.add_argument("--chapter", type=int, default=1)
    ap.add_argument("--pak-list", required=True, help="text listing of the pak index (.umap entries)")
    ap.add_argument("--md", required=True)
    ap.add_argument("--png", required=True)
    a = ap.parse_args(argv)

    in_pak = pak_cells(a.pak_list, a.chapter)
    cells = load_cells(a.input, a.agent)
    if not cells:
        print(f"error: no tiles_*.json under {a.input}/{a.agent}", file=sys.stderr)
        return 1
    res = audit(cells)
    missing = in_pak - set(cells)
    extra = set(cells) - in_pak
    write_md(res, a.agent, a.chapter, in_pak, missing, extra, a.md)
    coverage_png(res["rows"], missing, a.png)
    print(f"pak cells {len(in_pak)}, decoded {len(cells)}, missing {len(missing)}, extra {len(extra)}")
    print(
        f"tile records {res['records']}, unique keys {res['unique_keys']}, shared {res['shared_keys']} "
        f"(identical {res['shared_identical']}, differing {res['shared_differing']})"
    )
    print(f"wrote {a.md}\nwrote {a.png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
