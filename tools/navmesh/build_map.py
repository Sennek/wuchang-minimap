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
      chapter1/small_z0.png    16-bit GRAYSCALE HEIGHT MAP of walkable surface 0
      chapter1/small_z1.png    ... surface 1 (the next one up), and so on to z7

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
  *or* within `--seam-px` (default 0.5 px) of its boundary, so the sub-pixel rounding
  gaps that used to show up as a hairline grid are closed. The resulting ~1-px overlap
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

Z quantisation
--------------
    code = 1 + round((Z - z_min) / (z_max - z_min) * 65534)   clamped to 1..65535
    code 0  =  NO SURFACE at this pixel/slot

`z_min` / `z_max` are the chapter's own walkable Z range (over the polygon vertices
that survive the flat-plane filter) and ship in the manifest, together with the
resulting step in uu.

Manifest (schema `wuchang-minimap-maps/3`), per chapter:

    image        composite PNG, relative to maps/            "chapter1/small.png"
    image_width  / image_height   pixels (== width / height)
    width height                  pixels of every shipped texture
    min_x min_y max_x max_y       world bounds in uu covered by ALL textures
    px_per_uu    scale, one for the composite and every height map
    mapping      the formula, spelled out, so the C++ side can be checked against it
    z_min z_max  walkable Z range the height codes are quantised over
    z_step_uu    (z_max - z_min) / 65534
    max_surfaces number of height maps (8)
    height_maps  ["chapter1/small_z0.png", ... ] - index IS the surface slot,
                 lowest Z first; 16-bit grayscale, code 0 = no surface
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
import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import render  # noqa: E402  (same directory; the loader/geometry code is shared)

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

SCHEMA = "wuchang-minimap-maps/3"

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

Z_CODE_MAX = 65535  # 1..65535 are heights; 0 means "no surface"


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
    navmesh tiles - the hairline grid that made the old layers look like a mesh instead
    of a floor - at the price of a ~1-px overlap between neighbours, which the caller's
    Z-merge tolerance absorbs.

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


def quantize_heights(z: "np.ndarray", z_min: float, z_max: float) -> "np.ndarray":
    """One slot of `zbuf` -> uint16 codes. 0 = no surface, 1..65535 = height."""
    span = z_max - z_min if z_max > z_min else 1.0
    code = np.zeros(z.shape, dtype=np.uint16)
    m = ~np.isnan(z)
    if m.any():
        v = 1.0 + np.rint((z[m].astype(np.float64) - z_min) / span * (Z_CODE_MAX - 1))
        code[m] = np.clip(v, 1.0, float(Z_CODE_MAX)).astype(np.uint16)
    return code


def write_height_png(code: "np.ndarray", path: Path) -> int:
    """Write a 16-bit grayscale PNG and verify it reads back bit-exact."""
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.frombytes("I;16", (code.shape[1], code.shape[0]), code.astype("<u2").tobytes()).save(
        path, optimize=True
    )
    back = np.asarray(Image.open(path))
    if back.shape != code.shape or not np.array_equal(back.astype(np.int64), code.astype(np.int64)):
        raise RuntimeError(f"16-bit PNG did not round-trip: {path}")
    return path.stat().st_size


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

    # ---- the multi-surface height map --------------------------------------------
    zvals = np.array([q[2] for p in polys for q in p["pts"]], dtype=np.float64)
    z_min = float(zvals.min())
    z_max = float(zvals.max())
    z_step = (z_max - z_min) / (Z_CODE_MAX - 1)
    print(
        f"[{args.chapter}] walkable Z {z_min:.1f}..{z_max:.1f} uu -> 16-bit step "
        f"{z_step:.4f} uu (code 0 = no surface)"
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
    for k in range(used):
        rel = f"{args.chapter}/{stem}_z{k}.png"
        code = quantize_heights(zbuf[k], z_min, z_max)
        nb = write_height_png(code, out_root / rel)
        height_maps.append(rel)
        height_bytes.append(nb)
        nz = int(np.count_nonzero(code))
        zs = zbuf[k][~np.isnan(zbuf[k])]
        rng = f"Z {zs.min():8.0f}..{zs.max():8.0f}" if zs.size else "Z (empty)"
        print(
            f"  z{k}: {nz:9d} px  {rng}   {nb / (1024 * 1024):5.2f} MB PNG  (round-trip verified)"
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
        "z_step_uu": z_step,
        "z_code_no_surface": 0,
        "z_quantisation": "code = 1 + round((Z - z_min) / (z_max - z_min) * 65534) ; 0 = no surface",
        "chapter": chapter_number(args.chapter),
        "max_surfaces": used,
        "max_surfaces_requested": args.max_surfaces,
        "surface_merge_tol_uu": args.merge_tol,
        "height_maps": height_maps,
        "height_map_bytes": height_bytes,
        "height_map_raw_bytes": raw,
        "surface_hist": hist,
    }

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
