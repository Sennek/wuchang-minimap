#!/usr/bin/env python3
"""
build_map.py - turn the offline navmesh tile dumps into the minimap's shipped assets.

This is the *packaging* step that sits on top of `render.py`: same loader, same
dedupe, same flat-plane filter, same north-up mapping - but the output is a set of
textures plus a `maps.json` manifest the C++ mod reads at start-up.

    maps/
      maps.json                {"chapters": {"chapter1": {...}}}
      maps.json                schema `wuchang-minimap-maps/6` (see mapfmt.py)
      chapter1/small.png       256-COLOUR PALETTE PNG, transparent background,
                               Z-shaded composite of every floor (the fallback /
                               full-map asset; `fallback_use_composite = 0`)
      chapter1/small_h0.png    16-bit GRAYSCALE HEIGHT MAP of walkable surface 0,
                               12-BIT codes 1..4095 (0 = no surface),
                               bit 12 = the surface is REACHABLE
      chapter1/small_h1.png    ... surface 1 (the next one up), and so on to h7

The two ENCODINGS - the palette composite and the 12-bit height codes - and the
schema string live in `mapfmt.py`, which `repack_maps.py` shares. Use `repack_maps.py`
to change the encoding of chapters that already ship: it needs nothing but `maps/`,
where this script needs the 400 MB `dumps_offline/` extraction.

The shipped asset is a MULTI-SURFACE HEIGHT MAP
-----------------------------------------------
Per pixel we store the **Z of up to `--max-surfaces` walkable surfaces** (default 8),
ordered lowest first.
The runtime then slices: a surface within +/- `tol` of the player's feet is the floor
they are standing on, the nearest one below/above is drawn dimmed, everything else is
transparent. That is one texture set for the whole chapter, one scale, and no notion
of a "floor index" anywhere in the pipeline or the manifest.

This supersedes the schema-2 "per-pixel surface ordinal layer" scheme (eight R8
coverage masks, each with its own crop and scale, plus a 640-uu grid of surface bands
in the manifest to tell the runtime which layer to light up). Ordinals were already
the right *unit of decision* - a pixel - but they still forced the runtime to guess an
integer storey from a grid table, and 62 % of the interesting cells carried more than
one surface per rank. Storing the height itself removes the guess: the comparison the
runtime needs is `|Z - feetZ|`, and it has Z.

Why EIGHT slots, measured, not guessed. Two surfaces above one another must survive as
two numbers, or the dungeon under the Hanguang temple is either erased by the roof or
erases it. Chapter-wide, four slots hold 93 % of lit pixels - but the Digong-spiral /
Hanguang-temple block the user actually reported is the worst case on the map: in a
4 300-uu window there, the per-pixel surface count runs up to **11** and four slots hold
only **50 %** of the lit pixels. Sliced at the temple's feet Z that costs two thirds of
the floor (5 655 opaque px at N=4 vs 17 807 at N=8), and the preview reads as a
fragmented mess instead of a building. N=8 holds 96.9 % of that window's pixels and is
within 2 % of N=16, so eight is where the curve flattens. Anything deeper folds into the
last slot keeping the **highest** Z, so the top of a deep stack is never what is lost.
`--max-surfaces 4` halves the VRAM if the budget ever demands it - the numbers above are
what it costs.

Rasterisation rules (both matter to how the map *looks*)
-------------------------------------------------------
* **Fill only, no polygon edges, no seams.** The height maps are one continuous
  coverage per surface: adjacent polygons and adjacent 1280-uu navmesh tiles merge
  into one sheet. A pixel is covered when its sample point is inside the polygon
  *or* within `--seam-px` (default 0.5 px) of its boundary, which closes the sub-pixel
  rounding gaps that otherwise read as a hairline grid. The resulting ~1-px overlap
  between neighbours is absorbed by the surface-merge tolerance below, so it does not
  invent an extra surface along every edge.
* **Z is interpolated per vertex** (barycentric over the polygon's fan triangles,
  extrapolated from the nearest triangle inside the seam ring). A ramp or a staircase
  therefore stores a smoothly varying Z per pixel and the runtime's height gradient is
  smooth instead of banded per polygon.
* **Surfaces are merged, not counted.** A new polygon's pixels join an existing surface
  at that pixel when their Z is within `--merge-tol` (default 120 uu, well under the
  clearance any real storey has); only otherwise do they open a new slot. That is what
  makes the seam-closing overlap harmless, and it also collapses the genuinely
  coincident polygons Recast emits at tile borders.
* Slots are finally **sorted ascending by Z** per pixel, so `z0 <= z1 <= z2 <= z3` is
  guaranteed regardless of the order the polygons happened to be drawn in.

Reachability
------------
A pixel's surface carries **bit 12** when a player can get to it. The pass runs on the
rasterised height planes: the surfaces are the nodes of a directed 8-neighbour graph with
an edge from surface *s* to a neighbouring surface *t* when `Z_t <= Z_s + --reach-step-up`
(a walk, a small step up, or a fall of any depth), and it is flooded from every marker in
`markers/<chapter>.json` that has a surface within 400 uu of its own Z. What the flood does
not reach is wall tops, roof ridges, cliff ledges and the outside faces of arena walls -
geometry Recast walks and the player cannot. It is 34..63 % of the walkable area and 98 % of
the visually separate blobs on the map.

NOTHING IS DELETED. The unreached surfaces stay in the asset with the bit clear and the
runtime decides (`map_unreachable = hide | dim | show`), so the judgement is the player's
and the rollback is a config line. `--no-reach` flags every surface instead.

Z quantisation
--------------
    code = 1 + round((Z - z_min) / (z_max - z_min) * 4094)   clamped to 1..4095
    code 0  =  NO SURFACE at this pixel/slot
    bit 12  =  the surface is REACHABLE (see above); bits 13..15 are zero

`z_min` / `z_max` are the chapter's own walkable Z range (over the polygon vertices
that survive the flat-plane filter) and ship in the manifest, together with the
resulting step in uu.

Manifest (schema `wuchang-minimap-maps/6`), per chapter:

    image        composite PNG, relative to maps/            "chapter1/small.png"
    image_width  / image_height   pixels (== width / height)
    width height                  pixels of every shipped texture
    min_x min_y max_x max_y       world bounds in uu covered by ALL textures
    px_per_uu    scale, one for the composite and every height map
    mapping      the formula, spelled out, so the C++ side can be checked against it
    z_min z_max  walkable Z range the height codes are quantised over
    z_step_uu    (z_max - z_min) / 4094
    max_surfaces number of height planes (8)
    height_planes ["chapter1/small_h0.png", ... ] - index IS the surface slot,
                 lowest Z first; 16-bit grayscale, code 0 = no surface
    reachability seeds, reached area, blob counts and every unreached blob >= 400 m2
    height_map_bytes  PNG file sizes, same order
    surface_hist number of pixels with exactly 0/1/2/3/4 surfaces

World -> image mapping is `render.py`'s north-up convention, verbatim, and identical
for the composite and every height map:

    u (column) = (world_Y - min_y) * px_per_uu        east  -> right
    v (row)    = (max_x - world_X) * px_per_uu        north -> up

so image width comes from the world **Y** extent and image height from the world
**X** extent. `min_x`/`max_x` therefore bound the *vertical* axis of the picture -
that is not a typo. The sample point of pixel (u, v) is the integer (u, v) itself.

Sizing
------
`--px-per-uu` is a *request*: the scale is halved (repeatedly) until neither
dimension exceeds `--max-dim` (default 8192, the safe D3D12 texture limit on the
feature levels this game uses).

Usage
-----
    # Chapter 1, primary (Small) agent - what ships
    python build_map.py --input dumps_offline --chapter chapter1 --out ../../maps \
                        --px-per-uu 0.06

    # the superseded schema-2 ordinal layers + floor grid, for reference only
    python build_map.py ... --legacy-layers

The input tree is produced by `offline/navchunk.py` (see
`.workspace/wuchang-minimap/context/navmesh-offline.md` for the two commands).
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import mapfmt  # noqa: E402  (the on-disk format: schema, palette PNG, 12-bit heights)
import render  # noqa: E402  (same directory; the loader/geometry code is shared)

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

# The schema string and the two encodings live in mapfmt.py, which repack_maps.py
# shares, so a freshly built chapter and a re-encoded one are the same format by
# construction rather than by two matching constants.
SCHEMA = mapfmt.SCHEMA

# Light, desaturated cool grey ramp: low ground -> high ground. Deliberately low
# saturation so marker colours (and the player arrow) stay the only saturated
# things on the minimap. Used for the composite only - the height maps carry Z.
FILL_RAMP = [
    (0.00, (104, 114, 124)),
    (0.30, (140, 150, 158)),
    (0.60, (178, 186, 192)),
    (0.85, (208, 214, 218)),
    (1.00, (232, 236, 238)),
]

EDGE_DARKEN = 42  # per-channel subtraction for the thin polygon edge
FILL_ALPHA = 235  # walkable fill opacity; the background stays fully transparent

LAYER_FILL = 255  # coverage inside a walkable polygon      (legacy ordinal layers)
LAYER_EDGE = 176  # coverage on the polygon outline         (legacy ordinal layers)
LAYER_MARGIN_PX = 8  # transparent border so CLAMP sampling smears nothing

Z_CODE_MAX = mapfmt.Z_CODE_MAX  # 1..4095 are heights; 0 means "no surface" (schema /4)

# Reachability (see the module docstring). The step-up is the only geometric knob and it
# is deliberately insensitive: 60 -> 300 uu moves a chapter's reached area by 0.8 pp,
# because what the flood can and cannot enter is decided by the seeds, not by the climb.
REACH_STEP_UP_UU = 60.0
# A marker seeds the surface within this much of its own Z - marker_coverage.py's own
# criterion, so "the pass strands no marker" is measured the way the audit measures it.
REACH_SEED_Z_TOL_UU = 400.0
# Unreached blobs at least this big are listed in the manifest, so "why is there a hole
# here" starts from a table instead of from a screenshot.
REACH_BIG_UNREACHED_M2 = 400.0
REACH_OFFSETS = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))

# The tile size src/mapdata.cpp's sparse height store uses. Only reported here (the
# PNGs stay whole images); it is what turns "26 % of the pixels are lit" into a
# resident-RAM number the manifest can be tested against.
HEIGHT_TILE_PX = 128


def clamp_scale(bounds: render.Bounds, max_dim: int) -> render.Bounds:
    while (bounds.width > max_dim or bounds.height > max_dim) and bounds.px_per_uu > 1e-7:
        new = bounds.px_per_uu / 2.0
        print(
            f"  ! {bounds.width}x{bounds.height} px exceeds --max-dim {max_dim}; "
            f"halving {bounds.px_per_uu:g} -> {new:g} px/uu"
        )
        bounds.px_per_uu = new
    return bounds


def fit_ram_budget(bounds: render.Bounds, max_surfaces: int, budget_mb: float) -> render.Bounds:
    """
    Scale `px_per_uu` down until the height planes fit `budget_mb` of RAM.

    THE BUDGET IS THE REASON THIS EXISTS. The runtime keeps the whole height-plane set
    of ONE chapter in ordinary RAM (`max_surfaces` planes x width x height x 2 B) and
    slices a window out of it on the CPU. Chapter 1 at 0.06 px/uu is 4947x4333 -> 327 MB,
    which is what the first shipped build cost. The other chapters are not that size:
    at the same scale Chapter 5 is 6099x13755 = 84 Mpx = **1 280 MB**, Chapter 3 528 MB,
    Chapter 4 451 MB, Chapter 2 408 MB. Loading any of those unbounded is not an option,
    and neither is the alternative bound - fewer surface slots - because the slot count
    is what keeps a multi-storey interior readable (see the module docstring).

    So resolution is the lever, and it is applied CONTINUOUSLY rather than by halving:
    halving quarters the pixel count and would drop Chapter 5 to 0.015 px/uu (67 uu per
    pixel) when 0.031 fits. `px_per_uu *= sqrt(budget_px / current_px)` lands on the
    budget in one step.

    A chapter's scale therefore differs per chapter - which the manifest and the runtime
    already support, because `px_per_uu` was always a per-chapter field.
    """
    if budget_mb <= 0.0 or max_surfaces <= 0:
        return bounds
    budget_px = budget_mb * 1024.0 * 1024.0 / (2.0 * max_surfaces)
    have = float(bounds.width) * float(bounds.height)
    if have <= budget_px:
        return bounds
    factor = math.sqrt(budget_px / have)
    new = bounds.px_per_uu * factor
    print(
        f"  ! {bounds.width}x{bounds.height} px x {max_surfaces} planes x 2 B = "
        f"{have * 2 * max_surfaces / (1024 * 1024):.0f} MB exceeds the --max-ram-mb budget of "
        f"{budget_mb:g} MB; scaling {bounds.px_per_uu:g} -> {new:g} px/uu"
    )
    bounds.px_per_uu = new
    # The ceil() in Bounds.width/height can put us a pixel over; nudge until it fits.
    guard = 0
    while float(bounds.width) * float(bounds.height) > budget_px and guard < 32:
        bounds.px_per_uu *= 0.999
        guard += 1
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


# =================================================================================
# the shipped asset: a multi-surface height map
# =================================================================================


def _poly_coverage(
    pts: list[list[float]], bounds: render.Bounds, seam_px: float
) -> tuple[int, int, "np.ndarray", "np.ndarray"] | None:
    """
    Rasterise ONE convex navmesh polygon: coverage mask + per-pixel interpolated Z.

    Returns `(x0, y0, mask, z)` where `mask` and `z` cover the pixel box
    `[y0:y0+h, x0:x0+w]`, or None when the polygon lands outside the image.

    Coverage is fill-only (no outline) and deliberately generous: a pixel counts as
    covered when its sample point is inside the polygon **or** within `seam_px` of its
    boundary. That closes the sub-pixel gaps between neighbouring polygons and between
    navmesh tiles - which otherwise read as a hairline grid over the floor - at the
    price of a ~1-px overlap between neighbours, which the caller's Z-merge tolerance
    absorbs.

    Z is barycentric over the polygon's fan triangles (Recast polygons are convex, so a
    fan is a valid triangulation). Each pixel takes the value of the triangle whose
    minimum barycentric coordinate is largest: that is the containing triangle for an
    interior pixel, and the nearest triangle - linearly extrapolated - for a pixel in
    the seam ring. Continuous across every shared edge, so ramps and stairs come out as
    a smooth gradient rather than one flat step per polygon.
    """
    n = len(pts)
    us = np.empty(n)
    vs = np.empty(n)
    zs = np.empty(n)
    for i, q in enumerate(pts):
        u, v = bounds.to_px(q[0], q[1])
        us[i] = u
        vs[i] = v
        zs[i] = q[2]

    pad = seam_px + 1.0
    x0 = max(0, int(math.floor(us.min() - pad)))
    x1 = min(bounds.width, int(math.ceil(us.max() + pad)) + 1)
    y0 = max(0, int(math.floor(vs.min() - pad)))
    y1 = min(bounds.height, int(math.ceil(vs.max() + pad)) + 1)
    if x1 <= x0 or y1 <= y0:
        return None

    U = np.arange(x0, x1, dtype=np.float64)[None, :]
    V = np.arange(y0, y1, dtype=np.float64)[:, None]
    h = y1 - y0
    w = x1 - x0

    inside = np.zeros((h, w), dtype=bool)
    near = np.zeros((h, w), dtype=bool)
    r2 = seam_px * seam_px
    for i in range(n):
        ax, ay = us[i], vs[i]
        bx, by = us[(i + 1) % n], vs[(i + 1) % n]
        # even-odd crossing test, one edge at a time
        if by != ay:
            cross = (ay > V) != (by > V)  # (h, 1)
            xint = ax + (V - ay) * (bx - ax) / (by - ay)  # (h, 1)
            inside ^= cross & (U < xint)
        # distance from the sample point to this edge, for the seam ring
        dx = bx - ax
        dy = by - ay
        ll = dx * dx + dy * dy
        if ll <= 1e-12:
            d2 = (U - ax) ** 2 + (V - ay) ** 2
        else:
            t = np.clip(((U - ax) * dx + (V - ay) * dy) / ll, 0.0, 1.0)
            d2 = (U - (ax + t * dx)) ** 2 + (V - (ay + t * dy)) ** 2
        near |= d2 <= r2

    mask = inside | near
    if not mask.any():
        return None

    z = np.zeros((h, w), dtype=np.float32)
    best = np.full((h, w), -np.inf, dtype=np.float64)
    tris = 0
    for i in range(1, n - 1):
        au, av, az = us[0], vs[0], zs[0]
        bu, bv, bz = us[i], vs[i], zs[i]
        cu, cv, cz = us[i + 1], vs[i + 1], zs[i + 1]
        den = (bv - cv) * (au - cu) + (cu - bu) * (av - cv)
        if abs(den) < 1e-12:
            continue
        l1 = ((bv - cv) * (U - cu) + (cu - bu) * (V - cv)) / den
        l2 = ((cv - av) * (U - cu) + (au - cu) * (V - cv)) / den
        l3 = 1.0 - l1 - l2
        m = np.minimum(np.minimum(l1, l2), l3)
        take = m > best
        if take.any():
            best = np.where(take, m, best)
            zi = l1 * az + l2 * bz + l3 * cz
            z = np.where(take, zi.astype(np.float32), z)
        tris += 1
    if tris == 0:  # degenerate polygon - flat-fill with the mean vertex Z
        z[:] = np.float32(zs.mean())
    else:
        # A pixel of this polygon can never be outside the polygon's own vertex Z
        # range, and the seam-ring extrapolation *can* leave it: a sliver triangle is
        # only microns wide in barycentric terms, so half a pixel past its edge
        # extrapolates to absurd heights (measured: 2.8e13 uu before this clamp).
        np.clip(z, np.float32(zs.min()), np.float32(zs.max()), out=z)

    return x0, y0, mask, z


def rasterize_heights(
    polys: list[dict],
    bounds: render.Bounds,
    max_surfaces: int,
    merge_tol: float = 120.0,
    seam_px: float = 0.5,
    progress: int = 25000,
) -> tuple["np.ndarray", list[int]]:
    """
    Rasterise the walkable polygons into `max_surfaces` PER-PIXEL HEIGHT SLOTS.

    Returns `(zbuf, hist)` where `zbuf` is `(max_surfaces, height, width)` float32 with
    `NaN` for "no surface", sorted ascending by Z along axis 0, and `hist[k]` is the
    number of pixels carrying exactly `k` surfaces.

    Polygons are processed low-Z first, and a polygon's pixels either **join** the
    surface already at that pixel (when the Z difference is within `merge_tol`) or open
    the next free slot. Merging, rather than blind counting, is what lets the coverage
    overlap by a pixel along every shared edge without inventing a phantom storey
    there - and it also collapses the coincident polygons Recast emits at tile borders.

    Anything deeper than the last slot overwrites it with the **higher** Z, so the top
    of a deep stack is never the thing that gets dropped.
    """
    hs, ws = bounds.height, bounds.width
    zbuf = np.full((max_surfaces, hs, ws), np.nan, dtype=np.float32)
    count = np.zeros((hs, ws), dtype=np.uint8)

    order = sorted(polys, key=lambda q: q["cz"])
    t0 = time.time()
    for n_done, p in enumerate(order, 1):
        cov = _poly_coverage(p["pts"], bounds, seam_px)
        if cov is None:
            continue
        x0, y0, mask, zv = cov
        h, w = mask.shape
        zsub = zbuf[:, y0 : y0 + h, x0 : x0 + w]

        # join an existing surface at this pixel?
        close = np.abs(zsub - zv[None, :, :]) <= merge_tol  # NaN compares False
        rem = mask & ~close.any(axis=0)
        if rem.any():
            csub = count[y0 : y0 + h, x0 : x0 + w]
            slots = np.minimum(csub, max_surfaces - 1)
            for k in np.unique(slots[rem]):
                sel = rem & (slots == k)
                tgt = zbuf[k, y0 : y0 + h, x0 : x0 + w]
                free = sel & np.isnan(tgt)
                tgt[free] = zv[free]
                over = sel & ~free
                if over.any():  # only reachable in the last slot
                    tgt[over] = np.maximum(tgt[over], zv[over])
            csub[rem] = np.minimum(csub[rem].astype(np.int16) + 1, max_surfaces).astype(np.uint8)

        if progress and n_done % progress == 0:
            print(f"    {n_done}/{len(order)} polygons ({time.time() - t0:.0f}s)", flush=True)

    # guarantee z0 <= z1 <= z2 <= z3 per pixel; np.sort parks NaN at the end
    zbuf.sort(axis=0)
    hist = [0] * (max_surfaces + 1)
    filled = np.count_nonzero(~np.isnan(zbuf), axis=0)
    for k in range(max_surfaces + 1):
        hist[k] = int(np.count_nonzero(filled == k))
    return zbuf, hist


def load_extra_seeds(path: Path) -> list[dict]:
    """
    Extra seed positions from a JSON file - a recorded player track, or hand-picked spots.

    Accepts `[[x, y, z], ...]`, `[{"x":, "y":, "z":}, ...]` or either of those under a
    `"points"` / `"markers"` key, which is every shape the track writer and the marker
    files can produce.
    """
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    if isinstance(doc, dict):
        for key in ("points", "markers", "seeds", "track"):
            if isinstance(doc.get(key), list):
                doc = doc[key]
                break
        else:
            raise SystemExit(f"{path}: no list of points (tried points/markers/seeds/track)")
    out: list[dict] = []
    for it in doc:
        if isinstance(it, dict) and all(k in it for k in ("x", "y", "z")):
            out.append({"x": float(it["x"]), "y": float(it["y"]), "z": float(it["z"]), "cat": "track"})
        elif isinstance(it, (list, tuple)) and len(it) >= 3:
            out.append({"x": float(it[0]), "y": float(it[1]), "z": float(it[2]), "cat": "track"})
        else:
            raise SystemExit(f"{path}: {it!r} is not a point")
    return out


def flood_reachable(
    zbuf: "np.ndarray",
    bounds: render.Bounds,
    seeds: list[dict],
    step_up: float = REACH_STEP_UP_UU,
    seed_z_tol: float = REACH_SEED_Z_TOL_UU,
    big_m2: float = REACH_BIG_UNREACHED_M2,
) -> tuple["np.ndarray", dict]:
    """
    Which of the rasterised surfaces a player can get to. Returns `(reach, stats)`.

    `reach` is a boolean array shaped like `zbuf`; it is true only where `zbuf` holds a
    surface. The graph is the pixel stack itself: node = one surface cell `(k, v, u)`,
    directed edge to every surface at an 8-neighbour pixel whose Z is at most
    `step_up` above it - so a walk and a fall are edges and a climb is not. Every marker
    with a surface within `seed_z_tol` of its own Z is a seed, and the flood is one BFS
    from a virtual super-source over all of them.

    The stats dict is what lands in `maps.json` under `reachability`.
    """
    try:
        from scipy.sparse import csr_matrix
        from scipy.sparse.csgraph import breadth_first_order
        from scipy import ndimage
    except ImportError:  # pragma: no cover
        raise SystemExit("the reachability pass needs scipy:  pip install scipy  (or --no-reach)")

    k_n, h, w = zbuf.shape
    ok = ~np.isnan(zbuf)
    n = int(ok.sum())
    if n == 0:
        return np.zeros_like(ok), {"seeds": 0, "surface_cells": 0}
    cid = np.full(ok.shape, -1, dtype=np.int32)
    cid[ok] = np.arange(n, dtype=np.int32)
    # +inf where there is no surface, so every comparison against it is False.
    z = np.where(ok, zbuf, np.inf).astype(np.float32)

    rows: list["np.ndarray"] = []
    cols: list["np.ndarray"] = []
    for dv, du in REACH_OFFSETS:
        sv = slice(max(0, -dv), h - max(0, dv))
        dvs = slice(max(0, dv), h - max(0, -dv))
        su = slice(max(0, -du), w - max(0, du))
        dus = slice(max(0, du), w - max(0, -du))
        for ka in range(k_n):
            src_ok = ok[ka][sv, su]
            if not src_ok.any():
                continue
            za = z[ka][sv, su]
            ia = cid[ka][sv, su]
            for kb in range(k_n):
                dst_ok = ok[kb][dvs, dus]
                if not dst_ok.any():
                    continue
                m = src_ok & dst_ok & (z[kb][dvs, dus] <= za + step_up)
                if m.any():
                    rows.append(ia[m])
                    cols.append(cid[kb][dvs, dus][m])

    r = np.concatenate(rows).astype(np.int32) if rows else np.zeros(0, np.int32)
    c = np.concatenate(cols).astype(np.int32) if cols else np.zeros(0, np.int32)
    del rows, cols

    seed_cells: list[int] = []
    stranded = 0
    covered = 0
    for sd in seeds:
        fu, fv = bounds.to_px(sd["x"], sd["y"])
        u = int(round(fu))
        v = int(round(fv))
        if not (0 <= u < w and 0 <= v < h):
            continue
        col = zbuf[:, v, u]
        if not np.isfinite(col).any():
            continue
        k = int(np.nanargmin(np.abs(col - sd["z"])))
        if abs(col[k] - sd["z"]) > seed_z_tol:
            continue
        covered += 1
        seed_cells.append(int(cid[k, v, u]))
    if not seed_cells:
        raise SystemExit("no marker seeded a surface - the flood would delete the whole map")

    src = np.full(len(seed_cells), n, np.int32)
    r = np.concatenate([r, src])
    c = np.concatenate([c, np.asarray(seed_cells, np.int32)])
    graph = csr_matrix((np.ones(len(r), np.uint8), (r, c)), shape=(n + 1, n + 1))
    del r, c
    order = breadth_first_order(graph, n, directed=True, return_predecessors=False)
    del graph
    hit = np.zeros(n + 1, dtype=bool)
    hit[order] = True
    reach = np.zeros(ok.shape, dtype=bool)
    reach[ok] = hit[cid[ok]]
    del cid, hit, order

    # A seeded surface that the BFS did not mark is impossible; a seeded MARKER whose
    # surface is unreached is not - it is the number the audit cares about.
    for sd in seeds:
        fu, fv = bounds.to_px(sd["x"], sd["y"])
        u, v = int(round(fu)), int(round(fv))
        if not (0 <= u < w and 0 <= v < h):
            continue
        col = zbuf[:, v, u]
        near = np.isfinite(col) & (np.abs(col - sd["z"]) <= seed_z_tol)
        if near.any() and not reach[near, v, u].any():
            stranded += 1

    occ = ok.any(axis=0)
    occ_r = reach.any(axis=0)
    px_m2 = (1.0 / bounds.px_per_uu) ** 2 / 10000.0  # 1 uu = 1 cm
    struct = np.ones((3, 3), dtype=bool)  # 8-connected
    _, blobs_before = ndimage.label(occ, structure=struct)
    _, blobs_after = ndimage.label(occ_r, structure=struct)

    lost = occ & ~occ_r
    big: list[dict] = []
    if lost.any():
        lab, count = ndimage.label(lost, structure=struct)
        areas = np.bincount(lab.ravel(), minlength=count + 1)
        areas[0] = 0
        ids = np.nonzero(areas * px_m2 >= big_m2)[0]
        if ids.size:
            # +inf, not NaN, for "nothing unreached in this column": a whole-column NaN
            # is a warning and a NaN out of np.nanmin, and every such column is masked
            # away by `lost` anyway.
            zlow = np.where(reach | np.isnan(zbuf), np.inf, zbuf).min(axis=0)
            zlow = np.where(lost, zlow, np.nan)
            centres = ndimage.center_of_mass(lost, lab, ids)
            zs = ndimage.median(zlow, lab, ids)
            for (cv_, cu_), zc, i in zip(centres, np.atleast_1d(zs), ids):
                big.append({
                    "x": round(bounds.max_x - float(cv_) / bounds.px_per_uu, 1),
                    "y": round(bounds.min_y + float(cu_) / bounds.px_per_uu, 1),
                    "z": round(float(zc), 1),
                    "area_m2": round(float(areas[i]) * px_m2, 1),
                })
            big.sort(key=lambda b: -b["area_m2"])

    stats = {
        "step_up_uu": float(step_up),
        "seed_z_tol_uu": float(seed_z_tol),
        "seeds": len(seed_cells),
        "seeds_total_markers": len(seeds),
        "markers_covered": covered,
        "markers_stranded": stranded,
        "surface_cells": n,
        "reached_cells": int(reach.sum()),
        "lit_px": int(occ.sum()),
        "reached_px": int(occ_r.sum()),
        "area_m2": round(float(occ.sum()) * px_m2, 1),
        "reached_area_m2": round(float(occ_r.sum()) * px_m2, 1),
        "reached_area_pct": round(100.0 * float(occ_r.sum()) / max(1, int(occ.sum())), 2),
        "blobs_before": int(blobs_before),
        "blobs_after": int(blobs_after),
        "big_unreached_min_m2": float(big_m2),
        "big_unreached": big,
    }
    return reach, stats


def quantize_heights(z: "np.ndarray", z_min: float, z_max: float) -> "np.ndarray":
    """One slot of `zbuf` -> uint16 codes. 0 = no surface, 1..Z_CODE_MAX = height."""
    return mapfmt.quantise_z(z, z_min, z_max)


def write_height_png(code: "np.ndarray", path: Path) -> int:
    """Write a 16-bit grayscale PNG and verify it reads back bit-exact."""
    return mapfmt.write_height_png(code, path)


# =================================================================================
# superseded: schema-2 per-pixel ordinal layers + the floor Z grid
#
# Kept reachable behind --legacy-layers for reference. Both are SUPERSEDED BY HEIGHT
# SLICING (see the module docstring): the ordinal layers stored *which* surface a pixel
# was, and the floor grid existed only to tell the runtime which ordinal to light up.
# Storing the surface's Z instead answers both questions with one comparison, so
# nothing below is used by the shipped manifest any more.
# =================================================================================


def rasterize_ordinals(
    polys: list[dict], bounds: render.Bounds, max_levels: int, edges: bool = True
) -> list["np.ndarray"]:
    """
    SUPERSEDED BY HEIGHT SLICING (`rasterize_heights`). Kept for reference.

    Rasterise the walkable polygons into ONE COVERAGE MASK PER SURFACE ORDINAL: layer k
    holds, at every pixel, the k-th walkable surface counted from the bottom. Polygons
    are drawn low Z first and each pixel keeps a running count of how many surfaces have
    already been written there, so the layer a pixel lands in is decided per pixel - not
    per polygon, per grid cell or per global Z band.

    That per-pixel decision was the right idea and the height maps keep it; what this
    version lacks is the height itself, so the runtime had to look the storey up in
    `build_floor_grid`'s table instead of just comparing Z.

    Two cheaper schemes were measured before it and both fail (kept here so nobody
    re-measures them): `render.py`'s global floor clustering leaves 62 % of Chapter 1's
    640-uu cells with two or more surfaces in the same rank, and union-find over
    "neighbouring cells' Z ranges overlap" merges the chapter into one 231 131-polygon
    surface - no connectivity rule can separate storeys, because a staircase really does
    connect them.

    `max_levels` folds everything deeper than the last layer into it. Each polygon also
    gets `poly["ordinal"]` - the ordinal most of its own pixels landed in - which is what
    `build_floor_grid` tags its surface bands with.
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
    SUPERSEDED. Per-layer downsample factors (1, 2, 4, ...) that fit `budget_mb` of R8.

    A layer's cost is its *cropped* size, and deep ordinals do not crop well: ordinal 11
    of Chapter 1 lights only 4 731 pixels, yet they are scattered through every building
    interior, so its bounding box is still nearly the whole map. What does shrink it is
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


def emit_layer(a: "np.ndarray", base: render.Bounds, factor: int, path: Path) -> dict | None:
    """SUPERSEDED. Write one ordinal layer as an 8-bit PNG cropped to its footprint."""
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
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(sub).save(path, optimize=True)
    width = int(sub.shape[1])
    height = int(sub.shape[0])
    min_y = base.min_y + x0 / ppu
    max_x = base.max_x - y0 / ppu
    return {
        "image": None,
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
    SUPERSEDED BY HEIGHT SLICING. The old runtime "which storey am I on?" table.

    Per XY cell, the walkable **surface bands**: sort the polygon centroid Zs in the
    cell and cut wherever the gap exceeds `band_gap`. Each band is one walkable surface
    at that spot and carries the ordinal layer(s) that painted it. With per-pixel
    heights the runtime reads the Z out of the texture instead, so this table (and the
    ~13 000-number `floor_grid` array it produced in the manifest) is no longer shipped.

    Row layout, flat and self-describing:

        [gx, gy, band_count,
         zmin, zmax, layer_count, layer..., ...]

    Bands low Z first; a band's layers ordered by how much of the band they carry.
    `gx = floor(world_X / cell_uu)`, `gy = floor(world_Y / cell_uu)`.
    """
    cells: dict[tuple[int, int], list[tuple[float, int]]] = {}
    for p in polys:
        keys = {(math.floor(q[0] / cell_uu), math.floor(q[1] / cell_uu)) for q in p["pts"]}
        for key in keys:
            cells.setdefault(key, []).append((p["cz"], p.get("ordinal", 0)))

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
            row.extend([round(band[0][0], 1), round(band[-1][0], 1), len(keep)])
            row.extend(keep)
        out.append(row)
    return out


def build_legacy_layers(args: argparse.Namespace, polys: list[dict], bounds: render.Bounds, out_root: Path) -> dict:
    """SUPERSEDED. The schema-2 ordinal layers + floor grid, behind --legacy-layers."""
    stem = args.agent.lower()
    layer_ppu = args.layer_px_per_uu if args.layer_px_per_uu > 0.0 else bounds.px_per_uu
    lbounds = render.Bounds(bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y, layer_ppu)
    lbounds = clamp_scale(lbounds, args.max_dim)
    print(
        f"[{args.chapter}] LEGACY: rasterising up to {args.max_levels} surface ordinal(s) at "
        f"{lbounds.px_per_uu:g} px/uu ({lbounds.width}x{lbounds.height})..."
    )
    raster = rasterize_ordinals(polys, lbounds, args.max_levels, edges=not args.no_edges)
    per_ordinal = [0] * args.max_levels
    for p in polys:
        per_ordinal[p["ordinal"]] += 1
    factors = plan_layer_scales(raster, args.max_layer_mb)
    layers: list[dict] = []
    vram = 0
    for i, a in enumerate(raster):
        rel = f"{args.chapter}/{stem}_f{i}.png"
        entry = emit_layer(a, lbounds, factors[i], out_root / rel)
        if entry is None:
            continue
        entry["image"] = rel
        entry["floor"] = i
        entry["poly_count"] = per_ordinal[i]
        zs = [p["cz"] for p in polys if p["ordinal"] == i]
        entry["z_min"] = min(zs) if zs else 0.0
        entry["z_max"] = max(zs) if zs else 0.0
        vram += entry["image_width"] * entry["image_height"]
        layers.append(entry)
        print(f"  f{i}: {per_ordinal[i]:6d} polys  {entry['image_width']}x{entry['image_height']} px")
    grid = build_floor_grid(polys, args.floor_grid_uu, args.floor_band_gap)
    bands = sum(int(r[2]) for r in grid)
    print(f"[{args.chapter}] LEGACY floor Z grid: {len(grid)} cells, {bands} band(s)")
    return {
        "floor_count": len(layers),
        "max_levels": args.max_levels,
        "layers": layers,
        "layer_vram_bytes": vram,
        "floor_grid_cell_uu": args.floor_grid_uu,
        "floor_band_gap": args.floor_band_gap,
        "floor_grid_bands": bands,
        "floor_grid": grid,
    }


# =================================================================================


def chapter_number(key: str) -> int:
    """`"chapter3"` -> 3. 0 for a key with no digits (e.g. `"chapterdlc"`).

    The runtime detects a chapter NUMBER from the streamed `B<N>EX0_...` cell packages,
    so the manifest states the number explicitly instead of making the key's spelling
    load-bearing.
    """
    digits = "".join(c for c in key if c.isdigit())
    return int(digits) if digits else 0


def default_marker_globs(chapter_key: str) -> list[Path]:
    """`markers/<chapter key>.json` next to the repo root - the island filter's seeds."""
    root = Path(__file__).resolve().parent.parent.parent / "markers"
    return [root / f"{chapter_key}.json"]


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
    planes = render.classify_flat_planes(
        polys, render.DEFAULT_FLAT_PLANE_AREA,
        sheet_min=args.flat_plane_sheet_min, isolation=args.flat_plane_isolation,
    )
    polys = [p for p in polys if not p["plane"]]
    dropped_sheets = ", ".join(f"Z={s['z']:.1f} x{s['polys']} ({s['reason']})" for s in planes["dropped_sheets"])
    print(
        f"[{args.chapter}] {len(dump.tiles)} tiles, {total} polygons; "
        f"{planes['candidates']} big flat quads in {len(planes['sheets'])} sheet(s), "
        f"{planes['count']} of them out of bounds and dropped [{dropped_sheets or 'none'}]; "
        f"{len(polys)} drawn"
    )
    if not polys:
        sys.exit("every polygon was filtered out")

    seed_files = [Path(p) for pat in (args.markers or default_marker_globs(args.chapter))
                  for p in sorted(glob.glob(str(pat)))]
    # Every marker in the chapter, every category: the island filter's seeds and the
    # reachability flood's are the same set.
    marker_seeds = render.load_marker_seeds(seed_files)

    islands = {}
    if args.drop_islands:
        seeds = marker_seeds
        if not seeds:
            print(f"  ! no marker seeds found ({', '.join(str(p) for p in seed_files) or 'no files'}); "
                  f"the island filter falls back to the area threshold alone", file=sys.stderr)
        polys, islands = render.filter_islands(
            polys, seeds,
            grid=args.island_grid, z_tol=args.island_z_tol, min_area=args.island_min_area,
            seed_radius=args.island_seed_radius, require_seed=args.island_require_seed,
            bridge_xy=args.island_bridge_xy, bridge_z=args.island_bridge_z,
            cluster_area=args.island_cluster_area,
        )
        print(render.describe_islands(args.chapter, islands))
        if not polys:
            sys.exit("the island filter removed every polygon")

    bounds = render.compute_bounds(polys, args.px_per_uu, margin_uu=args.margin)
    # RAM budget FIRST, max-dim second: the budget scales continuously while the
    # max-dim clamp can only halve, so clamping first would charge a chapter that is
    # merely a little too wide (Chapter 5: 6099x13755 at 0.06) a full halving and then
    # leave it well under the RAM budget it could have spent on resolution.
    bounds = fit_ram_budget(bounds, args.max_surfaces, args.max_ram_mb)
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
    # A 256-COLOUR PALETTE PNG, not RGBA8 (schema /4): the render is flat-filled from
    # a 5-stop ramp with a darkened outline, so ~650 distinct colours cover the whole
    # picture and 255 of them are within 3/255 of every one. See mapfmt.py.
    _, pal_stats = mapfmt.write_composite_png(np.array(img), png_path)
    size_mb = png_path.stat().st_size / (1024 * 1024)
    print(
        f"[{args.chapter}] wrote {png_path}  ({size_mb:.2f} MB palette PNG, "
        f"{pal_stats['unique_rgba_in']} -> {pal_stats['palette_colours']} colours, "
        f"worst channel delta {pal_stats['max_channel_delta']}/255, "
        f"{bounds.width * bounds.height * 4 / (1024 * 1024):.0f} MB as RGBA8 in VRAM)"
    )
    if size_mb > args.max_mb:
        print(
            f"  ! {size_mb:.1f} MB is above the --max-mb budget of {args.max_mb} MB; "
            f"re-run with a smaller --px-per-uu",
            file=sys.stderr,
        )

    # ---- the multi-surface height map --------------------------------------------
    zvals = np.array([q[2] for p in polys for q in p["pts"]], dtype=np.float64)
    z_min = float(zvals.min())
    z_max = float(zvals.max())
    z_step = (z_max - z_min) / (Z_CODE_MAX - 1)
    print(
        f"[{args.chapter}] walkable Z {z_min:.1f}..{z_max:.1f} uu -> {mapfmt.Z_BITS}-bit step "
        f"{z_step:.4f} uu (codes 1..{Z_CODE_MAX}, code 0 = no surface)"
    )
    print(
        f"[{args.chapter}] rasterising up to {args.max_surfaces} height slot(s), "
        f"merge-tol {args.merge_tol:g} uu, seam {args.seam_px:g} px, fill only (no edges)..."
    )
    t0 = time.time()
    zbuf, hist = rasterize_heights(
        polys, bounds, args.max_surfaces, merge_tol=args.merge_tol, seam_px=args.seam_px
    )
    print(f"[{args.chapter}] rasterised in {time.time() - t0:.0f}s")
    lit = bounds.width * bounds.height - hist[0]
    print(
        f"[{args.chapter}] surfaces per pixel: "
        + ", ".join(f"{k}={hist[k]}" for k in range(len(hist)))
        + f"  ({lit} lit px, {100.0 * lit / (bounds.width * bounds.height):.1f} % of the image)"
    )

    # ---- reachability: bit 12 of every surface the player can get to ------------
    if args.no_reach:
        reach = ~np.isnan(zbuf)
        reachability = {
            "enabled": False,
            "note": "--no-reach: every surface carries the reachable flag",
        }
        print(f"[{args.chapter}] reachability pass OFF (--no-reach): every surface is flagged")
    else:
        reach_seeds = list(marker_seeds)
        for extra in args.reach_seeds_extra or []:
            more = load_extra_seeds(Path(extra))
            reach_seeds += more
            print(f"[{args.chapter}] {len(more)} extra seed(s) from {extra}")
        if not reach_seeds:
            sys.exit("the reachability pass needs marker seeds; pass --markers or --no-reach")
        print(
            f"[{args.chapter}] flooding reachability from {len(reach_seeds)} marker(s), "
            f"step-up {args.reach_step_up:g} uu, 8-neighbour..."
        )
        t0 = time.time()
        reach, reachability = flood_reachable(
            zbuf, bounds, reach_seeds, step_up=args.reach_step_up
        )
        reachability["enabled"] = True
        print(
            f"[{args.chapter}] reached {reachability['reached_area_pct']:.1f} % of the lit area "
            f"({reachability['reached_area_m2']:.0f} of {reachability['area_m2']:.0f} m2) from "
            f"{reachability['seeds']} of {reachability['seeds_total_markers']} markers; "
            f"blobs {reachability['blobs_before']} -> {reachability['blobs_after']}; "
            f"{reachability['markers_stranded']} marker(s) stranded; "
            f"{len(reachability['big_unreached'])} unreached blob(s) >= "
            f"{reachability['big_unreached_min_m2']:.0f} m2 "
            f"({time.time() - t0:.0f}s)"
        )
        for b in reachability["big_unreached"][:10]:
            print(f"    unreached {b['area_m2']:9.0f} m2 at ({b['x']:.0f}, {b['y']:.0f}, {b['z']:.0f})")

    # Trailing planes that no pixel ever reached cost RAM at runtime for nothing (the
    # slicer walks `count` planes), so they are not shipped. `hist[k]` counts pixels
    # with EXACTLY k surfaces, so plane k is lit iff some pixel has more than k.
    used = args.max_surfaces
    while used > 1 and sum(hist[used:]) == 0:
        used -= 1
    if used < args.max_surfaces:
        print(
            f"[{args.chapter}] planes z{used}..z{args.max_surfaces - 1} are empty on this chapter "
            f"- shipping {used} plane(s)"
        )

    height_maps: list[str] = []
    height_bytes: list[int] = []
    height_codes: list["np.ndarray"] = []  # kept for the tile-occupancy count and the coverage index
    for k in range(used):
        rel = mapfmt.height_plane_name(args.chapter, stem, k)
        code = mapfmt.apply_reach_bit(quantize_heights(zbuf[k], z_min, z_max), reach[k])
        nb = write_height_png(code, out_root / rel)
        height_codes.append(code)
        height_maps.append(rel)
        height_bytes.append(nb)
        nz = int(np.count_nonzero(mapfmt.z_codes(code)))
        nr = int(np.count_nonzero(mapfmt.reach_mask(code)))
        zs = zbuf[k][~np.isnan(zbuf[k])]
        rng = f"Z {zs.min():8.0f}..{zs.max():8.0f}" if zs.size else "Z (empty)"
        print(
            f"  z{k}: {nz:9d} px ({nr:9d} reachable)  {rng}   "
            f"{nb / (1024 * 1024):5.2f} MB PNG  (round-trip verified)"
        )
    raw = bounds.width * bounds.height * 2 * used
    print(
        f"[{args.chapter}] {used} height map(s): "
        f"{sum(height_bytes) / (1024 * 1024):.2f} MB of PNG, "
        f"{bounds.width}x{bounds.height}x2Bx{used} = {raw} B "
        f"({raw / (1024 * 1024):.1f} MB) of R16_UNORM texture"
    )

    entry = {
        "image": rel_png,
        "image_width": bounds.width,
        "image_height": bounds.height,
        "width": bounds.width,
        "height": bounds.height,
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
        "z_min": z_min,
        "z_max": z_max,
        "chapter": chapter_number(args.chapter),
        "max_surfaces": used,
        "max_surfaces_requested": args.max_surfaces,
        "surface_merge_tol_uu": args.merge_tol,
        "flat_plane_sheets_dropped": planes["dropped_sheets"],
        "island_filter": {k: islands[k] for k in
                          ("components", "clusters", "clusters_kept", "clusters_detached_kept",
                           "kept", "rescued_marker_components", "dropped", "polys_dropped", "area_dropped",
                           "seeds", "seeded_components", "grid_uu", "z_tol_uu", "min_area_uu2",
                           "cluster_area_uu2", "cover_z_uu", "bridge_xy_uu", "bridge_z_uu",
                           "seed_radius_uu", "require_seed") if k in islands},
        "reachability": reachability,
        mapfmt.HEIGHT_KEY: height_maps,
        "height_map_bytes": height_bytes,
        "height_map_raw_bytes": raw,
        "surface_hist": hist,
    }
    # The format fields (z_bits / z_code_max / z_step_uu / z_quantisation /
    # composite_format) come from mapfmt so this tool and repack_maps.py cannot
    # disagree about what they just wrote.
    mapfmt.stamp_format(entry, z_min, z_max)
    # What the runtime's SPARSE tile store will actually cost, measured here rather
    # than guessed in-game: RAM = non-empty 128-px tiles x 128 x 128 x 2 B.
    tiles, tiles_total = mapfmt.tile_occupancy(height_codes, HEIGHT_TILE_PX)
    entry["height_tile_px"] = HEIGHT_TILE_PX
    entry["height_tiles_128"] = tiles
    entry["height_tiles_128_total"] = tiles_total
    entry["height_tile_ram_bytes"] = tiles * HEIGHT_TILE_PX * HEIGHT_TILE_PX * 2
    # The COVERAGE INDEX: min/max Z code per 32-px tile, which is how the runtime asks a
    # chapter it has not loaded whether it has ground under the player's feet
    # (src/chapterid.hpp's vote cannot tell at a chapter boundary). See mapfmt.
    coverage = mapfmt.coverage_index(height_codes, z_min, z_max)
    entry[mapfmt.COVERAGE_KEY] = coverage
    print(
        f"[{args.chapter}] coverage: {coverage['tiles_x']}x{coverage['tiles_y']} tiles of "
        f"{coverage['tile_px']} px, {coverage['tiles_present']}/{coverage['tiles_total']} present, "
        f"{len(coverage['data']) / 1024.0:.0f} KB base64, "
        f"worst tile Z span {coverage['z_tile_span_uu_max']:.0f} uu"
    )
    print(
        f"[{args.chapter}] tile store: {tiles}/{tiles_total} tiles of {HEIGHT_TILE_PX} px "
        f"({100.0 * tiles / max(1, tiles_total):.1f} %) = "
        f"{entry['height_tile_ram_bytes'] / (1024 * 1024):.0f} MB resident, "
        f"against {raw / (1024 * 1024):.0f} MB dense"
    )

    if args.legacy_layers:
        entry.update(build_legacy_layers(args, polys, bounds, out_root))
    return entry


def dumps_manifest(manifest: dict) -> str:
    """
    `json.dumps(indent=1)`, except that every `floor_grid` row stays on one line.

    Only relevant with `--legacy-layers`: with plain indenting the grid's ~13 000 numbers
    would be one number per line. The shipped schema-3 manifest has no `floor_grid`.
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
    ap.add_argument("--px-per-uu", type=float, default=0.06, help="requested scale (default 0.06)")
    ap.add_argument("--max-surfaces", type=int, default=8, help="height slots per pixel (default 8)")
    ap.add_argument(
        "--merge-tol",
        type=float,
        default=120.0,
        help="Z within which a polygon joins the surface already at a pixel (uu, default 120)",
    )
    ap.add_argument(
        "--seam-px",
        type=float,
        default=0.5,
        help="coverage is expanded by this many px past the polygon boundary, to close seams (default 0.5)",
    )
    ap.add_argument("--max-dim", type=int, default=8192, help="hard texture-size cap in px (default 8192)")
    ap.add_argument(
        "--max-ram-mb",
        type=float,
        default=340.0,
        help="RAM budget for the chapter's height planes; px_per_uu is scaled down to fit "
        "(default 340, which is what Chapter 1 already costs). 0 disables the budget.",
    )
    ap.add_argument("--max-mb", type=float, default=10.0, help="warn if the composite PNG exceeds this (MB)")
    ap.add_argument("--margin", type=float, default=256.0, help="world-space margin around the geometry, uu")
    ap.add_argument("--no-edges", action="store_true", help="do not draw polygon edges in the composite")
    ap.add_argument(
        "--reach-step-up",
        type=float,
        default=REACH_STEP_UP_UU,
        help="a neighbouring surface this much higher is still walkable (uu, default 60); "
        "a fall of any depth always is",
    )
    ap.add_argument(
        "--reach-seeds-extra",
        action="append",
        default=None,
        help="JSON file of extra xyz seed points (a recorded player track); repeatable",
    )
    ap.add_argument(
        "--no-reach",
        action="store_true",
        help="skip the reachability flood and flag every surface as reachable",
    )
    render.add_plane_args(ap)
    render.add_island_args(ap, default_on=True)
    # ---- superseded schema-2 path ------------------------------------------------
    ap.add_argument(
        "--legacy-layers",
        action="store_true",
        help="also emit the SUPERSEDED per-pixel ordinal layers + floor grid (schema-2 scheme)",
    )
    ap.add_argument("--layer-px-per-uu", type=float, default=0.0, help="legacy: scale for the ordinal layers")
    ap.add_argument("--max-layer-mb", type=float, default=150.0, help="legacy: R8 budget for all ordinal layers")
    ap.add_argument("--max-levels", type=int, default=8, help="legacy: surface ordinals to bake")
    ap.add_argument("--floor-grid-uu", type=float, default=640.0, help="legacy: XY pitch of the floor Z grid")
    ap.add_argument("--floor-band-gap", type=float, default=250.0, help="legacy: Z gap that splits two bands")
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
