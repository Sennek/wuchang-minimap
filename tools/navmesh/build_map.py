#!/usr/bin/env python3
"""
build_map.py - turn the offline navmesh tile dumps into the minimap's shipped assets.

This is the *packaging* step that sits on top of `render.py`: same loader, same
dedupe, same flat-plane filter, same north-up mapping - but the output is a set of
textures plus a `maps.json` manifest the C++ mod reads at start-up.

    maps/
      maps.json                {"chapters": {"chapter1": {...}}}
      chapter1/small.png       RGBA, transparent background, Z-shaded composite of
                               every floor (the fallback / future full-map asset)
      chapter1/small_f0.png    8-bit GRAYSCALE coverage mask of surface ordinal 0
      chapter1/small_f1.png    ... one per ordinal, cropped to its own footprint

Why per-floor layers, and why the floor is a PER-PIXEL ordinal
-------------------------------------------------------------
The first in-world test showed the composite for what it is: standing in the dungeon
under the Hanguang temple, the minimap drew the temple roof, because every storey is
painted into one picture. So the shipped asset is now one layer per **surface
ordinal**: layer k holds, at every pixel, the k-th walkable surface counted from the
bottom (see `rasterize_ordinals`). Two surfaces above one another therefore always
land in different layers, and there are no grid-shaped seams, because the decision is
made per pixel.

Two cheaper schemes were measured first and both fail:
  * `render.py`'s global floor clustering (`assign_floors_grid`) leaves 62 % of
    Chapter 1's 640-uu cells with two or more surfaces in the same rank - up to seven
    heights of one cell share rank 1 at the Digong spiral - so the roof still covers
    the corridor;
  * union-find over "neighbouring cells' Z ranges overlap" merges the chapter into one
    surface (231 131 of 289 653 band-polygons), the same failure `lessons.md` already
    records for the polygon-level version. No connectivity rule can separate storeys:
    a staircase really does connect them.

Layers are **8-bit grayscale coverage masks**, not RGBA:
  * the runtime tints them (current floor bright, the floor below/above dimmed), so a
    per-pixel Z ramp inside one layer would only fight that tint;
  * an R8 texture is a quarter of the VRAM of RGBA8.
Each layer is cropped to its own footprint (plus a transparent margin, so the CLAMP
sampler in ImGui's DX12 backend smears nothing but transparency) and carries its own
bounds *and its own scale* in the manifest. Deep ordinals are sparse but scattered
map-wide, so cropping barely helps them (ordinal 11 lights 4 731 pixels inside a
4799x3764 box, and block-tiling measured no better) - what bounds the budget is
resolution, so `plan_layer_scales` coarsens the deepest layers until the whole set fits
`--max-layer-mb`.

Manifest (schema `wuchang-minimap-maps/2`), per chapter:

    image        composite PNG, relative to maps/            "chapter1/small.png"
    image_width  / image_height   pixels
    min_x min_y max_x max_y       world bounds in uu covered by the composite
    px_per_uu    scale
    mapping      the formula, spelled out, so the C++ side can be checked against it
    floor_count  number of layers actually written
    max_levels   ordinals baked (anything deeper folded into the last one)
    layers       [{floor, image, image_width, image_height, min_x, min_y, max_x,
                   max_y, px_per_uu, z_min, z_max, poly_count, png_bytes, lit_px}, ...]
                 `floor` IS the surface ordinal, and each layer has its own bounds and
                 px_per_uu.
    floor_grid_cell_uu   XY grid pitch of the floor Z grid (uu)
    floor_band_gap       Z gap that splits two surface bands inside a cell (uu)
    floor_grid   [[gx, gy, band_count, (zmin, zmax, floor_count, floor...) * n], ...]
                 For every occupied XY cell, the walkable **surface bands** at that
                 spot, low Z first, each tagged with the layer(s) its polygons were
                 rasterised into. This is what the runtime answers "which storey is the
                 player on at (x, y, z)?" with; `gx = floor(world_X / cell_uu)`,
                 `gy = floor(world_Y / cell_uu)`. See `build_floor_grid` for why the
                 table is banded per cell instead of being one Z range per floor.

World -> image mapping is `render.py`'s north-up convention, verbatim, and the same
for the composite and for every layer (each against its own bounds):

    u (column) = (world_Y - min_y) * px_per_uu        east  -> right
    v (row)    = (max_x - world_X) * px_per_uu        north -> up

so image width comes from the world **Y** extent and image height from the world
**X** extent. `min_x`/`max_x` therefore bound the *vertical* axis of the picture -
that is not a typo.

Sizing
------
`--px-per-uu` is a *request*: the scale is halved (repeatedly) until neither
dimension exceeds `--max-dim` (default 8192, the safe D3D12 texture limit on the
feature levels this game uses). `--layer-px-per-uu` defaults to the same value; the
script prints the total layer VRAM and warns above `--max-layer-mb` (150 MB).

Usage
-----
    # Chapter 1, primary (Small) agent - what ships
    python build_map.py --input dumps_offline --chapter chapter1 --out ../../maps \
                        --px-per-uu 0.06

The input tree is produced by `offline/navchunk.py` (see
`.workspace/wuchang-minimap/context/navmesh-offline.md` for the two commands).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import render  # noqa: E402  (same directory; the loader/geometry code is shared)

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

SCHEMA = "wuchang-minimap-maps/2"

# Light, desaturated cool grey ramp: low ground -> high ground. Deliberately low
# saturation so marker colours (and the player arrow) stay the only saturated
# things on the minimap. Used for the composite only - layers are coverage masks.
FILL_RAMP = [
    (0.00, (104, 114, 124)),
    (0.30, (140, 150, 158)),
    (0.60, (178, 186, 192)),
    (0.85, (208, 214, 218)),
    (1.00, (232, 236, 238)),
]

EDGE_DARKEN = 42  # per-channel subtraction for the thin polygon edge
FILL_ALPHA = 235  # walkable fill opacity; the background stays fully transparent

LAYER_FILL = 255  # coverage inside a walkable polygon
LAYER_EDGE = 176  # coverage on the polygon outline -> a subtle darker seam
LAYER_MARGIN_PX = 8  # transparent border so CLAMP sampling smears nothing


def clamp_scale(bounds: render.Bounds, max_dim: int) -> render.Bounds:
    while (bounds.width > max_dim or bounds.height > max_dim) and bounds.px_per_uu > 1e-7:
        new = bounds.px_per_uu / 2.0
        print(
            f"  ! {bounds.width}x{bounds.height} px exceeds --max-dim {max_dim}; "
            f"halving {bounds.px_per_uu:g} -> {new:g} px/uu"
        )
        bounds.px_per_uu = new
    return bounds


def draw_map(polys: list[dict], bounds: render.Bounds, edges: bool = True) -> Image.Image:
    """Transparent-background RGBA render, low Z first, Z-shaded, thin edges."""
    img = Image.new("RGBA", (bounds.width, bounds.height), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    zlo, zhi = render._percentiles([p["cz"] for p in polys], 0.02, 0.98)
    span = zhi - zlo if zhi > zlo else 1.0
    for p in sorted(polys, key=lambda q: q["cz"]):
        pts = [bounds.to_px(q[0], q[1]) for q in p["pts"]]
        col = render._ramp((p["cz"] - zlo) / span, FILL_RAMP)
        fill = (col[0], col[1], col[2], FILL_ALPHA)
        outline = None
        if edges:
            us = [u for u, _ in pts]
            vs = [v for _, v in pts]
            if max(us) - min(us) >= 3.0 or max(vs) - min(vs) >= 3.0:
                outline = (
                    max(0, col[0] - EDGE_DARKEN),
                    max(0, col[1] - EDGE_DARKEN),
                    max(0, col[2] - EDGE_DARKEN),
                    FILL_ALPHA,
                )
        dr.polygon(pts, fill=fill, outline=outline)
    return img


def rasterize_ordinals(
    polys: list[dict], bounds: render.Bounds, max_levels: int, edges: bool = True
) -> list["np.ndarray"]:
    """
    Rasterise the walkable polygons into ONE COVERAGE MASK PER SURFACE ORDINAL.

    Layer k holds, at every pixel, the k-th walkable surface counted from the bottom.
    Polygons are drawn low Z first and each pixel keeps a running count of how many
    surfaces have already been written there, so the layer a pixel lands in is decided
    per pixel - not per polygon, per grid cell or per global Z band.

    That is the whole point. Everything else we measured fails on the case that started
    this: standing in the dungeon under the Hanguang temple.
      * A global floor rank (render.py's `assign_floors_grid`) puts up to seven
        different heights of one 640-uu cell into the same rank - 62 % of Chapter 1's
        cells keep two or more surfaces in one layer, so the roof still covers the
        corridor.
      * Union-find over "the Z ranges of neighbouring cells overlap" merges the whole
        chapter into one surface (231 131 of 289 653 band-polygons), exactly as
        `lessons.md` already recorded for the polygon-level version. No connectivity
        rule can separate storeys anyway: a staircase genuinely connects them.
    Per-pixel ordinals have no such failure mode: two surfaces above one another are
    always in different layers, and there are no cell-shaped seams, because a pixel is
    the unit of the decision.

    `max_levels` folds everything deeper than the last layer into it (in Chapter 1 that
    is 0.3 % of polygons at ordinal >= 8), which bounds the texture budget.

    Each polygon also gets `poly["ordinal"]` - the ordinal most of its own pixels
    landed in - which is what `build_floor_grid` tags its surface bands with.
    """
    count = np.zeros((bounds.height, bounds.width), dtype=np.uint8)
    layers = [np.zeros((bounds.height, bounds.width), dtype=np.uint8) for _ in range(max_levels)]

    for p in sorted(polys, key=lambda q: q["cz"]):
        pts = [bounds.to_px(q[0], q[1]) for q in p["pts"]]
        us = [u for u, _ in pts]
        vs = [v for _, v in pts]
        x0 = max(0, int(math.floor(min(us))) - 1)
        x1 = min(bounds.width, int(math.ceil(max(us))) + 2)
        y0 = max(0, int(math.floor(min(vs))) - 1)
        y1 = min(bounds.height, int(math.ceil(max(vs))) + 2)
        p["ordinal"] = 0
        if x1 <= x0 or y1 <= y0:
            continue
        img = Image.new("L", (x1 - x0, y1 - y0), 0)
        ImageDraw.Draw(img).polygon(
            [(u - x0, v - y0) for u, v in pts], fill=LAYER_FILL, outline=LAYER_EDGE if edges else None
        )
        mask_img = np.asarray(img)
        mask = mask_img > 0
        if not mask.any():
            continue
        sub = count[y0:y1, x0:x1]
        ordv = np.minimum(sub, max_levels - 1)
        for o in np.unique(ordv[mask]):
            sel = mask & (ordv == o)
            target = layers[o][y0:y1, x0:x1]
            np.maximum(target, mask_img * sel, out=target)
        p["ordinal"] = int(np.bincount(ordv[mask], minlength=max_levels).argmax())
        sub[mask] = np.minimum(sub[mask].astype(np.int16) + 1, 255).astype(np.uint8)

    return layers


def _downsample(a: "np.ndarray", factor: int) -> "np.ndarray":
    """Max-pool by `factor` - coverage must survive, so max, not mean."""
    if factor <= 1:
        return a
    h = (a.shape[0] // factor) * factor
    w = (a.shape[1] // factor) * factor
    if h == 0 or w == 0:
        return a[:1, :1]
    return a[:h, :w].reshape(h // factor, factor, w // factor, factor).max(axis=(1, 3))


def plan_layer_scales(
    layers: list["np.ndarray"], budget_mb: float, max_factor: int = 4, min_side_px: int = 256
) -> list[int]:
    """
    Per-layer downsample factors (1, 2, 4, ...) that fit `budget_mb` of R8 texture.

    A layer's cost is its *cropped* size, and deep ordinals do not crop well: ordinal 11
    of Chapter 1 lights only 4 731 pixels, yet they are scattered through every building
    interior, so its bounding box is still nearly the whole map (and block-tiling it
    measured no better: 54 of 67 512-px blocks are non-empty). What does shrink it is
    resolution, and the layers that need it least are the deep ones - so the coarsening
    starts at the deepest layer and works up until the total fits.
    """
    factors = [1] * len(layers)
    crops = [_nonzero_crop(a) for a in layers]

    def cost(i: int) -> int:
        crop = crops[i]
        if crop is None:
            return 0
        x0, y0, x1, y1 = crop
        f = factors[i]
        return max(1, (x1 - x0) // f) * max(1, (y1 - y0) // f)

    budget = int(budget_mb * 1024 * 1024)
    guard = 0
    while sum(cost(i) for i in range(len(layers))) > budget and guard < 64:
        guard += 1
        # coarsen the deepest layer that is still worth coarsening
        victim = -1
        for i in range(len(layers) - 1, -1, -1):
            crop = crops[i]
            if crop is None:
                continue
            x0, y0, x1, y1 = crop
            if factors[i] >= max_factor:
                continue
            if min((x1 - x0) // factors[i], (y1 - y0) // factors[i]) >= min_side_px * 2:
                victim = i
                break
        if victim < 0:
            break
        factors[victim] *= 2
    return factors


def _nonzero_crop(a: "np.ndarray") -> tuple[int, int, int, int] | None:
    """(x0, y0, x1, y1) bounding box of the non-zero pixels, or None if empty."""
    cols = np.flatnonzero(a.any(axis=0))
    rows = np.flatnonzero(a.any(axis=1))
    if cols.size == 0 or rows.size == 0:
        return None
    return int(cols[0]), int(rows[0]), int(cols[-1]) + 1, int(rows[-1]) + 1


def emit_layer(
    a: "np.ndarray",
    base: render.Bounds,
    factor: int,
    path: Path,
) -> dict | None:
    """
    Write one ordinal layer as an 8-bit PNG, cropped to its own footprint, and return
    its manifest entry (bounds in world uu, so the runtime maps it exactly like the
    composite - `u = (Y - min_y) * px_per_uu`, `v = (max_x - X) * px_per_uu`).
    """
    small = _downsample(a, factor)
    crop = _nonzero_crop(small)
    if crop is None:
        return None
    ppu = base.px_per_uu / factor
    m = LAYER_MARGIN_PX
    x0 = max(0, crop[0] - m)
    y0 = max(0, crop[1] - m)
    x1 = min(small.shape[1], crop[2] + m)
    y1 = min(small.shape[0], crop[3] + m)
    sub = small[y0:y1, x0:x1]
    Image.fromarray(sub).save(path, optimize=True)
    width = int(sub.shape[1])
    height = int(sub.shape[0])
    min_y = base.min_y + x0 / ppu
    max_x = base.max_x - y0 / ppu
    return {
        "image": None,  # filled in by the caller
        "image_width": width,
        "image_height": height,
        "min_x": max_x - height / ppu,
        "min_y": min_y,
        "max_x": max_x,
        "max_y": min_y + width / ppu,
        "px_per_uu": ppu,
        "png_bytes": path.stat().st_size,
        "lit_px": int(np.count_nonzero(sub)),
    }


def build_floor_grid(
    polys: list[dict],
    cell_uu: float,
    band_gap: float,
    min_share: float = 0.12,
    max_floors_per_band: int = 3,
) -> list[list[float]]:
    """
    The runtime's "which storey am I on?" table: per XY cell, the **surface bands**.

    A first attempt stored one [zmin, zmax] per (cell, floor). That does not work: a
    floor index is a *global rank*, so inside a single 1280-uu cell one rank can cover
    a whole staircase (measured median span 1160 uu, p90 5002, max 41960) and several
    ranks then overlap - a containment test on those ranges is meaningless.

    So the cell is split by Z instead: sort the polygon centroid Zs in the cell and cut
    wherever the gap exceeds `band_gap` (250 uu). Each band is one walkable surface at
    that spot (measured median width 192 uu, p90 1090), and it carries the floor
    layer(s) that painted it - 35 % of bands mix two adjacent ranks at practically the
    same height, which is a smoothing artefact of the global clustering, and drawing
    both of them is exactly right.

    Row layout, flat and self-describing so the runtime's hand-written JSON parser
    only ever sees numbers:

        [gx, gy, band_count,
         zmin, zmax, layer_count, layer...,
         zmin, zmax, layer_count, layer..., ...]

    Bands are ordered low Z first; a band's layers are ordered by how much of the band
    they carry (dominant first), NOT numerically. `gx = floor(world_X / cell_uu)`,
    `gy = floor(world_Y / cell_uu)`.
    """
    cells: dict[tuple[int, int], list[tuple[float, int]]] = {}
    for p in polys:
        # A polygon can straddle a cell border; register it in every cell its
        # vertices touch, so the answer at the border is never "no floor here".
        keys = {(math.floor(q[0] / cell_uu), math.floor(q[1] / cell_uu)) for q in p["pts"]}
        for key in keys:
            cells.setdefault(key, []).append((p["cz"], p["ordinal"]))

    out: list[list[float]] = []
    for (gx, gy), items in sorted(cells.items()):
        items.sort()
        bands: list[list[tuple[float, int]]] = []
        current = [items[0]]
        for it in items[1:]:
            if it[0] - current[-1][0] > band_gap:
                bands.append(current)
                current = [it]
            else:
                current.append(it)
        bands.append(current)

        row: list[float] = [gx, gy, len(bands)]
        for band in bands:
            counts: dict[int, int] = {}
            for _, f in band:
                counts[f] = counts.get(f, 0) + 1
            ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
            keep = [ranked[0][0]]
            for f, c in ranked[1:]:
                if len(keep) >= max_floors_per_band:
                    break
                if c >= max(1.0, min_share * len(band)):
                    keep.append(f)
            # NOT sorted: the dominant ordinal stays first, and the runtime draws it
            # at full opacity and the rest dimmed. A band's polygons genuinely land on
            # more than one ordinal (the ordinal boundary is per pixel: a corridor is
            # surface #2 where two storeys exist below it and #1 where only one does),
            # which no grid pitch removes - measured 1.55 ordinals per band even at a
            # 160-uu pitch, vs 2.00 at 640 - so the fix is to rank them, not to split
            # the grid finer and pay a 7 MB manifest for it.
            row.extend([round(band[0][0], 1), round(band[-1][0], 1), len(keep)])
            row.extend(keep)
        out.append(row)
    return out


def build_chapter(args: argparse.Namespace) -> dict:
    root = args.input if args.input.is_absolute() else Path(__file__).resolve().parent / args.input
    agents = render.discover_agents(root)
    if args.agent not in agents:
        sys.exit(f"agent '{args.agent}' not found under {root} (have: {', '.join(sorted(agents)) or 'nothing'})")

    files = agents[args.agent]
    print(f"[{args.chapter}/{args.agent}] merging {len(files)} cell dump(s) from {root}")
    dump = render.load_dump_files(files, agent_hint=args.agent, dedupe="richest")
    if not dump.tiles:
        sys.exit("no tiles decoded - nothing to build")

    polys = render.polygons_of(dump)
    total = len(polys)
    planes = render.classify_flat_planes(polys, render.DEFAULT_FLAT_PLANE_AREA)
    polys = [p for p in polys if not p["plane"]]
    print(
        f"[{args.chapter}] {len(dump.tiles)} tiles, {total} polygons, "
        f"{planes['count']} flat planes dropped ({total - len(polys)} polys), {len(polys)} drawn"
    )
    if not polys:
        sys.exit("every polygon was filtered out")

    bounds = render.compute_bounds(polys, args.px_per_uu, margin_uu=args.margin)
    bounds = clamp_scale(bounds, args.max_dim)
    print(
        f"[{args.chapter}] world X {bounds.min_x:.0f}..{bounds.max_x:.0f}  "
        f"Y {bounds.min_y:.0f}..{bounds.max_y:.0f}  -> {bounds.width}x{bounds.height} px "
        f"@ {bounds.px_per_uu:g} px/uu (1 px = {1.0 / bounds.px_per_uu:.1f} uu)"
    )

    out_root = args.out if args.out.is_absolute() else Path.cwd() / args.out
    stem = args.agent.lower()

    # ---- the composite (fallback / full-map asset) -------------------------------
    img = draw_map(polys, bounds, edges=not args.no_edges)
    rel_png = f"{args.chapter}/{stem}.png"
    png_path = out_root / rel_png
    png_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(png_path, optimize=True)
    size_mb = png_path.stat().st_size / (1024 * 1024)
    print(
        f"[{args.chapter}] wrote {png_path}  ({size_mb:.2f} MB, "
        f"{bounds.width * bounds.height * 4 / (1024 * 1024):.0f} MB as RGBA8)"
    )
    if size_mb > args.max_mb:
        print(
            f"  ! {size_mb:.1f} MB is above the --max-mb budget of {args.max_mb} MB; "
            f"re-run with a smaller --px-per-uu",
            file=sys.stderr,
        )

    # ---- one grayscale layer per SURFACE ORDINAL ---------------------------------
    #
    # Rasterised at the composite's scale (or --layer-px-per-uu), then the deepest
    # layers are coarsened until the whole set fits --max-layer-mb of R8 texture.
    layer_ppu = args.layer_px_per_uu if args.layer_px_per_uu > 0.0 else bounds.px_per_uu
    lbounds = render.Bounds(bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y, layer_ppu)
    lbounds = clamp_scale(lbounds, args.max_dim)
    print(
        f"[{args.chapter}] rasterising up to {args.max_levels} surface ordinal(s) at "
        f"{lbounds.px_per_uu:g} px/uu ({lbounds.width}x{lbounds.height})..."
    )
    raster = rasterize_ordinals(polys, lbounds, args.max_levels, edges=not args.no_edges)
    per_ordinal = [0] * args.max_levels
    for p in polys:
        per_ordinal[p["ordinal"]] += 1
    print(f"[{args.chapter}] polygons per ordinal: " + ", ".join(f"f{i}={n}" for i, n in enumerate(per_ordinal)))

    factors = plan_layer_scales(raster, args.max_layer_mb)
    layers: list[dict] = []
    vram = 0
    png_total = 0
    for i, a in enumerate(raster):
        rel = f"{args.chapter}/{stem}_f{i}.png"
        entry = emit_layer(a, lbounds, factors[i], out_root / rel)
        if entry is None:
            print(f"  f{i}: no pixels - skipped")
            continue
        entry["image"] = rel
        entry["floor"] = i
        entry["poly_count"] = per_ordinal[i]
        zs = [p["cz"] for p in polys if p["ordinal"] == i]
        entry["z_min"] = min(zs) if zs else 0.0
        entry["z_max"] = max(zs) if zs else 0.0
        bytes_r8 = entry["image_width"] * entry["image_height"]
        vram += bytes_r8
        png_total += entry["png_bytes"]
        layers.append(entry)
        print(
            f"  f{i}: {per_ordinal[i]:6d} polys  Z {entry['z_min']:8.0f}..{entry['z_max']:8.0f}  "
            f"{entry['image_width']}x{entry['image_height']} px @ {entry['px_per_uu']:g} "
            f"(1/{factors[i]})  {entry['png_bytes'] / (1024 * 1024):5.2f} MB PNG  "
            f"{bytes_r8 / (1024 * 1024):5.1f} MB R8  {entry['lit_px']} lit px"
        )

    vram_mb = vram / (1024 * 1024)
    print(
        f"[{args.chapter}] {len(layers)} layer(s): {png_total / (1024 * 1024):.2f} MB of PNG, "
        f"{vram_mb:.1f} MB of R8 texture memory"
    )
    if vram_mb > args.max_layer_mb:
        print(
            f"  ! layers need {vram_mb:.0f} MB > --max-layer-mb {args.max_layer_mb:.0f}; "
            f"re-run with a smaller --layer-px-per-uu or --max-levels",
            file=sys.stderr,
        )

    # ---- the floor Z grid --------------------------------------------------------
    grid = build_floor_grid(polys, args.floor_grid_uu, args.floor_band_gap)
    bands = sum(int(r[2]) for r in grid)
    print(
        f"[{args.chapter}] floor Z grid: {len(grid)} cells of {args.floor_grid_uu:g} uu, "
        f"{bands} surface band(s) split at gaps > {args.floor_band_gap:g} uu"
    )

    return {
        "image": rel_png,
        "image_width": bounds.width,
        "image_height": bounds.height,
        "min_x": bounds.min_x,
        "min_y": bounds.min_y,
        "max_x": bounds.max_x,
        "max_y": bounds.max_y,
        "px_per_uu": bounds.px_per_uu,
        "mapping": "u = (world_Y - min_y) * px_per_uu ; v = (max_x - world_X) * px_per_uu",
        "agent": args.agent,
        "tile_count": len(dump.tiles),
        "poly_count": len(polys),
        "png_bytes": png_path.stat().st_size,
        "floor_count": len(layers),
        "max_levels": args.max_levels,
        "layers": layers,
        "layer_vram_bytes": vram,
        "floor_grid_cell_uu": args.floor_grid_uu,
        "floor_band_gap": args.floor_band_gap,
        "floor_grid_bands": bands,
        "floor_grid": grid,
    }


def dumps_manifest(manifest: dict) -> str:
    """
    `json.dumps(indent=1)`, except that every `floor_grid` row stays on one line.

    With plain indenting the grid's ~13 000 numbers would be one number per line and
    the manifest would be a 400 kB, 14 000-line file for no benefit; fully compact
    output would make the rest of the manifest unreadable in a diff. So the rows are
    serialised compactly, parked behind a token, and pasted back in afterwards.
    """
    rows: dict[str, str] = {}
    for key, ch in manifest.get("chapters", {}).items():
        grid = ch.get("floor_grid")
        if not isinstance(grid, list):
            continue
        token = f"@@floor_grid:{key}@@"
        body = ",\n  ".join(json.dumps(r, separators=(",", ":")) for r in grid)
        rows[token] = "[\n  " + body + "\n ]" if grid else "[]"
        ch["floor_grid"] = token
    text = json.dumps(manifest, indent=1)
    for token, replacement in rows.items():
        text = text.replace(f'"{token}"', replacement)
    return text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", type=Path, default=Path("dumps_offline"), help="tile-dump root with agent subdirs")
    ap.add_argument("--agent", default="Small", help="navmesh agent to ship (default Small, the player's)")
    ap.add_argument("--chapter", default="chapter1", help="chapter key in maps.json (default chapter1)")
    ap.add_argument("--out", type=Path, default=Path("maps"), help="output maps/ directory")
    ap.add_argument("--px-per-uu", type=float, default=0.06, help="requested composite scale (default 0.06)")
    ap.add_argument(
        "--layer-px-per-uu",
        type=float,
        default=0.0,
        help="scale for the per-floor layers (default: same as --px-per-uu)",
    )
    ap.add_argument("--max-dim", type=int, default=8192, help="hard texture-size cap in px (default 8192)")
    ap.add_argument("--max-mb", type=float, default=10.0, help="warn if the composite PNG exceeds this (MB)")
    ap.add_argument("--max-layer-mb", type=float, default=150.0, help="R8 texture budget for all layers (MB)")
    ap.add_argument("--max-levels", type=int, default=8, help="surface ordinals to bake; deeper ones fold into the last")
    ap.add_argument("--margin", type=float, default=256.0, help="world-space margin around the geometry, uu")
    ap.add_argument("--floor-grid-uu", type=float, default=640.0, help="XY pitch of the shipped floor Z grid")
    ap.add_argument("--floor-band-gap", type=float, default=250.0, help="Z gap that splits two surface bands in a cell")
    ap.add_argument("--no-edges", action="store_true", help="do not draw polygon edges")
    args = ap.parse_args(argv)

    entry = build_chapter(args)

    out_root = args.out if args.out.is_absolute() else Path.cwd() / args.out
    manifest_path = out_root / "maps.json"
    manifest = {"schema": SCHEMA, "chapters": {}}
    if manifest_path.exists():
        try:
            old = json.loads(manifest_path.read_text(encoding="utf-8"))
            if old.get("schema") == SCHEMA and isinstance(old.get("chapters"), dict):
                manifest["chapters"] = old["chapters"]
        except (OSError, ValueError) as exc:
            print(f"  ! ignoring unreadable {manifest_path}: {exc}", file=sys.stderr)
    manifest["chapters"][args.chapter] = entry
    manifest_path.write_text(dumps_manifest(manifest) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path} ({len(manifest['chapters'])} chapter(s), "
          f"{manifest_path.stat().st_size / 1024:.0f} kB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
