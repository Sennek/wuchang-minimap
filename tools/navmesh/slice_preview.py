#!/usr/bin/env python3
"""
slice_preview.py - render the RUNTIME's height-slicing rule offline, from maps/.

The shipped map asset (see `build_map.py`, schema `wuchang-minimap-maps/6`) is a stack
of 16-bit grayscale height planes - up to eight of them: at every pixel, the Z of up
to eight walkable surfaces, lowest first, with code 0 meaning "no surface". A code is
12 bits of Z plus bit 12, the reachable flag. The number of planes and the code range
are read from the manifest through `mapfmt`, never assumed. This tool answers the only question
that matters about that asset - *does the slice at the player's feet read as one
coherent floor plan?* - without launching the game, and it is the reference
implementation of the rule the C++ overlay must reproduce pixel for pixel.

    usage: slice_preview.py --x 19537 --y 4587 --z 2505 [--out out.png]
                            [--window-px 512] [--tol 200] [--above-band 600]
                            [--unreachable hide|dim|show] [--equalize] [--fullmap]

`--fullmap` is the FULL MAP's own settings in one flag: `--above-band inf --equalize`,
plus a window big enough to hold the whole chapter. The runtime hard-codes both for the
full map - `shade_above_band_uu` and the linear ramp are the minimap's.

The rule, per pixel, over the surfaces stored there
---------------------------------------------------
    any surface with |Z - feetZ| <= tol
        -> FLOOR, the one NEAREST the feet, alpha 1.00. Everything above it at that
           pixel is a ceiling and is ignored.
    else any surface in (feetZ + tol, feetZ + above_band]
        -> ABOVE, the LOWEST such one (a ledge, or the upper terrain a ramp leads to),
           at shade_above_alpha. Surfaces below it are ignored - the ledge hides them.
    else any surface below feetZ - tol
        -> BELOW, the HIGHEST one, at shade_below_alpha. No lower bound.
    else nothing is drawn.

so a ceiling never covers the player, and a ledge overhead is ground, not a hint. A
floor underfoot wins outright, so a deck over a solid floor of the player's shows only
through the holes in that floor - which is why `above_band` is one storey.

`map_unreachable` (`--unreachable`) decides what a surface bit 12 says the reachability
flood never reached is worth: `hide` drops it, `dim` draws it one rung further down the
opacity ladder, `show` draws it like any other. Within one class a reachable surface
beats an unreachable one - the runtime's rank is `class * 2 + reachable`.

Colour, all three classes
-------------------------
Hypsometric: the pixel's colour is its surface's ABSOLUTE world Z on a two-colour ramp,
so slopes, staircases and the storey below all read by their height. ONE ramp serves
every class, at full opacity, so a height is the same colour whichever storey it belongs
to and the cut reads as a floor plan; in game the player marker at the centre says which
storey is theirs.

    t   = clamp((surfaceZ - z_lo) / (z_hi - z_lo), 0, 1) ** shade_gamma
    rgb = shade_lo_color + (shade_hi_color - shade_lo_color) * t

A flat slab one storey up is a single Z and therefore a single flat tone, so the ramp
draws no edge where two storeys abut; a pixel whose left or up neighbour is more than
`SEAM_STEP_UU` away in Z is darkened by `SEAM_DARKEN` (`srule::seam_factor`), which is
the only boundary cue the cut has.

`z_lo`/`z_hi` are PERCENTILES of the Z of the pixels this cut actually draws -
`p(shade_range_pct_lo) .. p(100 - shade_range_pct_lo)` out of a 128-bin histogram over
the asset's own Z range - widened to at least `shade_min_range_uu` about their centre.
One deep pit or one high gallery must not push every playable storey into two colour
levels. The minimap eases that span over `shade_range_smooth_ms`, which a still picture
cannot show.

With `--equalize` (the full map's `shade_map_equalize`, on by default in game) `t` is
instead the CDF of the drawn Z out of that same histogram, interpolated inside the bin
it lands in:

    t   = cdf(surfaceZ) ** shade_gamma

so every tenth of the ramp holds a tenth of the drawn pixels and a stretch of Z nothing
was drawn at costs no contrast. Over a ten-kilometre chapter that is the difference
between a readable picture and one tone. The reported ramp is then the drawn min..max -
p0..p100 - because that is what the picture actually spans.

`shade_lo_color`, `shade_hi_color`, `shade_gamma`, `shade_below_alpha`,
`shade_above_alpha`, `shade_above_band_uu`, `shade_range_pct_lo` and
`shade_min_range_uu` are the names these knobs carry in the runtime's config, on
purpose.

The result is composited over a dark disc-less flat background (24,26,30) purely so the
PNG is readable, and the query point is marked with a small high-contrast cross. Neither
is part of the rule.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

import mapfmt

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

BACKGROUND = (24, 26, 30)
DEFAULT_LO_COLOR = (88, 84, 78)
DEFAULT_HI_COLOR = (244, 240, 232)
DEFAULT_GAMMA = 0.80
DEFAULT_BELOW_ALPHA = 1.0
DEFAULT_ABOVE_ALPHA = 1.0
DEFAULT_ABOVE_BAND = 600.0
DEFAULT_RANGE_PCT_LO = 3.0
DEFAULT_MIN_RANGE = 400.0
ALPHA_OPAQUE = 1.00
# srule::kSeamStepUu / srule::kSeamDarken.
SEAM_STEP_UU = 300.0
SEAM_DARKEN = 0.45

# srule's class numbers: the numbers ARE the priority.
CLS_NONE = 0
CLS_BELOW = 1
CLS_ABOVE = 2
CLS_FLOOR = 3
CLS_NAME = {CLS_NONE: "empty", CLS_BELOW: "below", CLS_ABOVE: "above", CLS_FLOOR: "floor"}

# srule::ZHistogram::kBins.
ZHIST_BINS = 128

# The schemas whose height codes carry bit 12 (src/mapmanifest.hpp).
REACH_SCHEMAS = ("wuchang-minimap-maps/6", "wuchang-minimap-maps/5")


def load_chapter(maps_dir: Path, chapter: str) -> tuple[dict, str]:
    path = maps_dir / "maps.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    schema = manifest.get("schema", "?")
    chapters = manifest.get("chapters", {})
    if chapter not in chapters:
        sys.exit(f"chapter '{chapter}' not in {path} (have: {', '.join(sorted(chapters)) or 'nothing'})")
    ch = chapters[chapter]
    if not mapfmt.height_plane_list(ch)[0]:
        sys.exit(
            f"{path} carries no '{mapfmt.HEIGHT_KEY}' or '{mapfmt.HEIGHT_KEY_LEGACY}' "
            f"(schema {schema}); rebuild with build_map.py"
        )
    return ch, schema


def read_window(
    maps_dir: Path, ch: dict, cu: int, cv: int, half: int, has_reach: bool
) -> tuple["np.ndarray", "np.ndarray", "np.ndarray", int, int]:
    """
    Load the `2*half` px window centred on (cu, cv) out of every height map.

    Returns `(z, valid, reach, x0, y0)` with `z` shaped `(slots, h, w)` in world uu,
    `valid` the same shape (code != 0) and `reach` bit 12 - all True on an asset that
    carries no reachability, exactly as `HeightMaps::reachable()` answers. Windows are
    clipped to the image, so the returned box may be smaller than requested; `x0`/`y0`
    are its top-left pixel.
    """
    w_img = int(ch["width"])
    h_img = int(ch["height"])
    x0 = max(0, cu - half)
    x1 = min(w_img, cu + half)
    y0 = max(0, cv - half)
    y1 = min(h_img, cv + half)
    if x1 <= x0 or y1 <= y0:
        sys.exit("the query point is outside the chapter's image bounds")

    z_min = float(ch["z_min"])
    z_max = float(ch["z_max"])
    span = z_max - z_min
    code_max = int(ch.get("z_code_max", mapfmt.Z_CODE_MAX_LEGACY))
    zs = []
    vs = []
    rs = []
    for rel in mapfmt.height_plane_list(ch)[0]:
        img = Image.open(maps_dir / rel)
        if img.size != (w_img, h_img):
            sys.exit(f"{rel} is {img.size[0]}x{img.size[1]}, manifest says {w_img}x{h_img}")
        raw = np.asarray(img.crop((x0, y0, x1, y1))).astype(np.uint16)
        code = mapfmt.z_codes(raw).astype(np.uint32)
        valid = code > 0
        reach = mapfmt.reach_mask(raw) if has_reach else np.ones(valid.shape, dtype=bool)
        z = np.full(code.shape, np.nan, dtype=np.float64)
        z[valid] = z_min + (code[valid].astype(np.float64) - 1.0) / float(code_max - 1) * span
        zs.append(z)
        vs.append(valid)
        rs.append(reach & valid)
    return np.stack(zs), np.stack(vs), np.stack(rs), x0, y0


def alpha_for(
    cls: "np.ndarray", reach: "np.ndarray", below_alpha: float, above_alpha: float
) -> "np.ndarray":
    """srule::alpha_for: an unreachable surface takes the next rung down the ladder."""
    alpha = np.zeros(cls.shape, dtype=np.float64)
    alpha[(cls == CLS_FLOOR) & reach] = ALPHA_OPAQUE
    alpha[(cls == CLS_FLOOR) & ~reach] = below_alpha
    alpha[(cls == CLS_BELOW) & reach] = below_alpha
    alpha[(cls == CLS_BELOW) & ~reach] = above_alpha
    alpha[(cls == CLS_ABOVE) & reach] = above_alpha
    alpha[(cls == CLS_ABOVE) & ~reach] = above_alpha * 0.6
    return alpha


def slice_window(
    z: "np.ndarray",
    reach: "np.ndarray",
    feet_z: float,
    tol: float,
    above_band: float,
    below_alpha: float = DEFAULT_BELOW_ALPHA,
    above_alpha: float = DEFAULT_ABOVE_ALPHA,
    unreachable: str = "hide",
) -> tuple["np.ndarray", "np.ndarray", "np.ndarray", "np.ndarray"]:
    """
    Apply the slicing rule. Returns `(cls, z_pick, alpha, reach_pick)`.

    `cls`   : srule's class numbers - 0 empty, 1 below, 2 above, 3 floor.
    `z_pick`: the chosen surface's world Z (NaN where empty).
    """
    d = z - feet_z
    with np.errstate(invalid="ignore"):
        valid = ~np.isnan(d)
    if unreachable == "show":
        reach = np.ones(reach.shape, dtype=bool)
    elif unreachable == "hide":
        valid = valid & reach

    with np.errstate(invalid="ignore"):
        floor = valid & (d >= -tol) & (d <= tol)
        above = valid & (d > tol) & (d <= above_band)
        below = valid & (d < -tol)

    cls_s = np.zeros(d.shape, dtype=np.uint8)
    cls_s[below] = CLS_BELOW
    cls_s[above] = CLS_ABOVE
    cls_s[floor] = CLS_FLOOR

    # The runtime's rank, and inside one rank the height tie-break that class wants:
    # nearest the feet on the floor, the lowest of a stack overhead, the highest below.
    # Both fold into one score to maximise, the rank spaced far enough apart that no
    # tie-break can cross it.
    rank = cls_s.astype(np.float64) * 2.0 + (reach & valid).astype(np.float64)
    dz = np.nan_to_num(d, nan=0.0)
    tie = np.where(floor, -np.abs(dz), np.where(above, -dz, dz))
    score = np.where(cls_s > 0, rank * 1.0e9 + np.clip(tie, -1.0e8, 1.0e8), -np.inf)

    k = np.argmax(score, axis=0)
    found = np.isfinite(np.take_along_axis(score, k[None], axis=0)[0])
    out_cls = np.where(found, np.take_along_axis(cls_s, k[None], axis=0)[0], CLS_NONE).astype(np.uint8)
    out_reach = np.take_along_axis(reach & valid, k[None], axis=0)[0] & found
    out_z = np.where(found, np.take_along_axis(np.nan_to_num(z, nan=0.0), k[None], axis=0)[0], np.nan)
    return out_cls, out_z, alpha_for(out_cls, out_reach, below_alpha, above_alpha), out_reach


def _percentile(counts: "np.ndarray", lo: float, hi: float, pct: float) -> float:
    """srule::ZHistogram::percentile, bin-interpolated."""
    total = float(counts.sum())
    if total <= 0.0:
        return lo
    target = total * min(max(pct, 0.0), 100.0) * 0.01
    width = (hi - lo) / float(len(counts))
    seen = 0.0
    for i, raw in enumerate(counts):
        c = float(raw)
        if c <= 0.0:
            continue
        if seen + c >= target:
            return lo + (float(i) + (target - seen) / c) * width
        seen += c
    return hi


def zhist(
    cls: "np.ndarray", z_pick: "np.ndarray", z_min: float, z_max: float
) -> tuple["np.ndarray", float, float]:
    """srule::ZHistogram over the DRAWN pixels. Returns `(counts, lo, hi)`."""
    hi_edge = z_max if z_max > z_min + 1.0e-3 else z_min + 1.0
    drawn = z_pick[cls != CLS_NONE]
    counts, _ = np.histogram(drawn, bins=ZHIST_BINS, range=(z_min, hi_edge))
    # The runtime clamps a Z outside the asset's range into the end bins rather than
    # dropping it, so the counts must total the drawn pixels.
    counts[0] += int(np.count_nonzero(drawn < z_min))
    counts[-1] += int(np.count_nonzero(drawn > hi_edge))
    return counts, z_min, hi_edge


def ramp_range(
    cls: "np.ndarray",
    z_pick: "np.ndarray",
    z_min: float,
    z_max: float,
    pct_lo: float = DEFAULT_RANGE_PCT_LO,
    min_range: float = DEFAULT_MIN_RANGE,
) -> tuple[float, float]:
    """
    srule::ZHistogram + hist_range + widen_range: the ramp's two ends for one cut.

    Percentiles of the Z of the DRAWN pixels, out of a `ZHIST_BINS`-bin histogram over
    the asset's own Z range, then widened to at least `min_range` about their centre.
    """
    if not bool(np.any(cls != CLS_NONE)):
        return 0.0, 1.0
    counts, lo_edge, hi_edge = zhist(cls, z_pick, z_min, z_max)
    p = min(max(pct_lo, 0.0), 49.0)
    lo = _percentile(counts, lo_edge, hi_edge, p)
    hi = _percentile(counts, lo_edge, hi_edge, 100.0 - p)
    if hi - lo < min_range:
        mid = 0.5 * (lo + hi)
        lo, hi = mid - 0.5 * min_range, mid + 0.5 * min_range
    return lo, hi


def _cdf(counts: "np.ndarray", lo: float, hi: float, z: "np.ndarray") -> "np.ndarray":
    """srule::ZHistogram::cdf, vectorised and bin-for-bin identical."""
    total = float(counts.sum())
    if total <= 0.0:
        return np.full(z.shape, 0.5)
    cum = np.concatenate(([0.0], np.cumsum(counts.astype(np.float64))))
    width = (hi - lo) / float(len(counts))
    pos = (z - lo) / width
    i = np.clip(pos.astype(np.int64), 0, len(counts) - 1)
    frac = np.clip(pos - i, 0.0, 1.0)
    below = cum[i] + frac * counts[i].astype(np.float64)
    return np.clip(below / total, 0.0, 1.0)


def seam_factor(z_pick: "np.ndarray", cls: "np.ndarray") -> "np.ndarray":
    """srule::seam_factor over the whole cut: SEAM_DARKEN where a DRAWN left or up
    neighbour's chosen surface is more than SEAM_STEP_UU away in Z, else 1."""
    drawn = cls != CLS_NONE
    z = np.nan_to_num(z_pick, nan=0.0)
    step = np.zeros(z.shape, dtype=bool)
    step[:, 1:] |= drawn[:, :-1] & (np.abs(z[:, 1:] - z[:, :-1]) > SEAM_STEP_UU)
    step[1:, :] |= drawn[:-1, :] & (np.abs(z[1:, :] - z[:-1, :]) > SEAM_STEP_UU)
    return np.where(step & drawn, SEAM_DARKEN, 1.0)


def shade(
    z_pick: "np.ndarray",
    cls: "np.ndarray",
    z_lo: float,
    z_hi: float,
    lo_color: tuple[int, int, int],
    hi_color: tuple[int, int, int],
    gamma: float,
    hist: tuple["np.ndarray", float, float] | None = None,
) -> "np.ndarray":
    """The colour formula from the docstring. Returns float RGB, shape (h, w, 3).

    One ramp serves every class; `cls` says only which pixels are drawn, for the seam.
    `hist` non-None equalises - `t` is the cut's own CDF (srule::SliceStyle::equalize)
    instead of a linear span.
    """
    z = np.nan_to_num(z_pick, nan=z_lo)
    if hist is not None:
        t = _cdf(hist[0], hist[1], hist[2], z)
    else:
        span = z_hi - z_lo
        with np.errstate(invalid="ignore"):
            t = np.clip((z - z_lo) / max(span, 1e-3), 0.0, 1.0)
    if gamma != 1.0 and gamma > 0.0:
        t = t ** gamma
    lo = np.array(lo_color, dtype=np.float64)
    hi = np.array(hi_color, dtype=np.float64)
    rgb = lo[None, None, :] + (hi - lo)[None, None, :] * t[..., None]
    rgb *= seam_factor(z_pick, cls)[..., None]
    return np.clip(rgb, 0.0, 255.0)


def draw_cross(rgb: "np.ndarray", u: int, v: int, arm: int = 7) -> None:
    """A small high-contrast cross at (u, v), outlined so it reads on any fill."""
    h, w = rgb.shape[:2]
    hot = np.array([255.0, 70.0, 50.0])
    dark = np.array([10.0, 10.0, 12.0])

    def put(x: int, y: int, col: "np.ndarray") -> None:
        if 0 <= x < w and 0 <= y < h:
            rgb[y, x] = col

    for r in range(-arm - 1, arm + 2):
        for o in (-1, 1):
            put(u + r, v + o, dark)
            put(u + o, v + r, dark)
    for r in range(-arm, arm + 1):
        put(u + r, v, hot)
        put(u, v + r, hot)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--maps", type=Path, default=Path(__file__).resolve().parents[2] / "maps")
    ap.add_argument("--chapter", default="chapter1")
    ap.add_argument("--x", type=float, required=True, help="world X of the query point (uu)")
    ap.add_argument("--y", type=float, required=True, help="world Y of the query point (uu)")
    ap.add_argument("--z", type=float, required=True, help="world Z of the player's FEET (uu)")
    ap.add_argument("--out", type=Path, default=Path("slice.png"))
    ap.add_argument("--window-px", type=int, default=512, help="source pixels across (default 512)")
    ap.add_argument("--tol", type=float, default=200.0, help="same-floor Z tolerance, uu (default 200)")
    ap.add_argument(
        "--above-band",
        type=float,
        default=DEFAULT_ABOVE_BAND,
        help=f"runtime key shade_above_band_uu (default {DEFAULT_ABOVE_BAND:g})",
    )
    ap.add_argument(
        "--unreachable",
        choices=("hide", "dim", "show"),
        default="hide",
        help="runtime key map_unreachable (default hide)",
    )
    ap.add_argument(
        "--gamma",
        type=float,
        default=DEFAULT_GAMMA,
        help=f"runtime key shade_gamma (default {DEFAULT_GAMMA:g})",
    )
    ap.add_argument(
        "--lo-color",
        default=",".join(str(c) for c in DEFAULT_LO_COLOR),
        help=f"runtime key shade_lo_color, 'R,G,B' (default {','.join(str(c) for c in DEFAULT_LO_COLOR)})",
    )
    ap.add_argument(
        "--hi-color",
        default=",".join(str(c) for c in DEFAULT_HI_COLOR),
        help=f"runtime key shade_hi_color, 'R,G,B' (default {','.join(str(c) for c in DEFAULT_HI_COLOR)})",
    )
    ap.add_argument(
        "--below-alpha", type=float, default=DEFAULT_BELOW_ALPHA, help="runtime key shade_below_alpha"
    )
    ap.add_argument(
        "--above-alpha", type=float, default=DEFAULT_ABOVE_ALPHA, help="runtime key shade_above_alpha"
    )
    ap.add_argument(
        "--range-pct-lo",
        type=float,
        default=DEFAULT_RANGE_PCT_LO,
        help=f"runtime key shade_range_pct_lo (default {DEFAULT_RANGE_PCT_LO:g})",
    )
    ap.add_argument(
        "--min-range",
        type=float,
        default=DEFAULT_MIN_RANGE,
        help=f"runtime key shade_min_range_uu (default {DEFAULT_MIN_RANGE:g})",
    )
    ap.add_argument(
        "--equalize",
        action="store_true",
        help="runtime key shade_map_equalize: t is the cut's own CDF (the full map's mode)",
    )
    ap.add_argument(
        "--fullmap",
        action="store_true",
        help="the full map's settings: --above-band inf --equalize over the whole chapter",
    )
    ap.add_argument("--scale", type=int, default=1, help="nearest-neighbour upscale of the PNG")
    ap.add_argument(
        "--max-px", type=int, default=0, help="downscale the PNG to at most this on the long side"
    )
    args = ap.parse_args(argv)
    if args.fullmap:
        # What overlay_fullmap.cpp hard-codes: nothing overhead is out of range, and the
        # ramp is equalised over the cut. The window opens to the whole image, which
        # read_window clips to the chapter's bounds.
        args.above_band = float("inf")
        args.equalize = True
        args.window_px = max(args.window_px, 1 << 20)

    def rgb_arg(text: str, flag: str) -> tuple[int, int, int]:
        try:
            parts = tuple(int(v) for v in text.split(","))
        except ValueError:
            parts = ()
        if len(parts) != 3:
            sys.exit(f"{flag} wants three comma-separated 0..255 numbers, e.g. 96,92,84")
        return parts

    lo_color = rgb_arg(args.lo_color, "--lo-color")
    hi_color = rgb_arg(args.hi_color, "--hi-color")

    ch, schema = load_chapter(args.maps, args.chapter)
    has_reach = schema in REACH_SCHEMAS
    ppu = float(ch["px_per_uu"])
    cu = int(round((args.y - float(ch["min_y"])) * ppu))
    cv = int(round((float(ch["max_x"]) - args.x) * ppu))
    half = max(8, args.window_px // 2)

    print(f"maps.json schema {schema}, chapter {args.chapter}, {ch['max_surfaces']} surface slots")
    print(
        f"query world X {args.x:.0f} Y {args.y:.0f} feetZ {args.z:.0f} -> pixel (u {cu}, v {cv}) "
        f"of {ch['width']}x{ch['height']} @ {ppu:g} px/uu"
    )

    z, valid, reach, x0, y0 = read_window(args.maps, ch, cu, cv, half, has_reach)
    cls, z_pick, alpha, reach_pick = slice_window(
        z,
        reach,
        args.z,
        args.tol,
        args.above_band,
        args.below_alpha,
        args.above_alpha,
        args.unreachable,
    )
    # An equalised ramp is bounded by the drawn set itself, so its ends are p0..p100 and
    # min_range never widens them - exactly as slice_region reports it.
    z_lo, z_hi = ramp_range(
        cls,
        z_pick,
        float(ch["z_min"]),
        float(ch["z_max"]),
        0.0 if args.equalize else args.range_pct_lo,
        0.0 if args.equalize else args.min_range,
    )
    hist = None
    if args.equalize and bool(np.any(cls != CLS_NONE)):
        hist = zhist(cls, z_pick, float(ch["z_min"]), float(ch["z_max"]))
    rgb = shade(z_pick, cls, z_lo, z_hi, lo_color, hi_color, args.gamma, hist)

    bg = np.array(BACKGROUND, dtype=np.float64)
    out = bg[None, None, :] * (1.0 - alpha[..., None]) + rgb * alpha[..., None]
    draw_cross(out, cu - x0, cv - y0)

    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
    if args.scale > 1:
        img = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
    if args.max_px > 0 and max(img.width, img.height) > args.max_px:
        f = args.max_px / float(max(img.width, img.height))
        img = img.resize((max(1, int(img.width * f)), max(1, int(img.height * f))), Image.BOX)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out, optimize=True)

    n = cls.size
    n_floor = int(np.count_nonzero(cls == CLS_FLOOR))
    n_below = int(np.count_nonzero(cls == CLS_BELOW))
    n_above = int(np.count_nonzero(cls == CLS_ABOVE))
    n_em = int(np.count_nonzero(cls == CLS_NONE))
    n_unreach = int(np.count_nonzero((cls != CLS_NONE) & ~reach_pick))
    surf = int(np.count_nonzero(valid))
    here = [
        None
        if not valid[k, cv - y0, cu - x0]
        else (round(float(z[k, cv - y0, cu - x0]), 1), bool(reach[k, cv - y0, cu - x0]))
        for k in range(z.shape[0])
    ]
    drawn = z_pick[cls != CLS_NONE]
    print(f"window {cls.shape[1]}x{cls.shape[0]} px at image ({x0}, {y0}), {surf} stored surfaces in it")
    print(f"surfaces under the query pixel (lowest first, (Z, reachable)): {here}")
    print(
        f"floor {n_floor} ({100.0 * n_floor / n:.1f} %)  above {n_above} ({100.0 * n_above / n:.1f} %)  "
        f"below {n_below} ({100.0 * n_below / n:.1f} %)  empty {n_em} ({100.0 * n_em / n:.1f} %)  "
        f"unreachable {n_unreach} px"
    )
    if drawn.size:
        pcts = np.percentile(drawn, [0, 5, 25, 50, 75, 95, 100])
        print(
            "drawn Z: min {:.0f}  p5 {:.0f}  p25 {:.0f}  p50 {:.0f}  p75 {:.0f}  p95 {:.0f}  "
            "max {:.0f}".format(*pcts)
        )
    if cls[cv - y0, cu - x0] != CLS_FLOOR:
        print(
            f"  ! the query pixel itself is {CLS_NAME[int(cls[cv - y0, cu - x0])]}, not floor - feetZ "
            f"does not match any stored surface there"
        )
    if drawn.size:
        # How flat the picture is, in the one number the diagnosis used: the fraction of
        # drawn pixels inside the densest 0.1-wide band of t. An equalised ramp cannot
        # push it far above 0.1; a linear one over a chapter reaches 0.6.
        if hist is not None:
            t_all = _cdf(hist[0], hist[1], hist[2], drawn)
        else:
            t_all = np.clip((drawn - z_lo) / max(z_hi - z_lo, 1e-3), 0.0, 1.0)
        if args.gamma != 1.0 and args.gamma > 0.0:
            t_all = t_all ** args.gamma
        band = np.histogram(t_all, bins=10, range=(0.0, 1.0))[0]
        print(f"densest 0.1 band of t: {band.max() / float(t_all.size):.3f} of the drawn pixels")
    ramp_kind = "equalised" if args.equalize else f"p{args.range_pct_lo:g}"
    print(
        f"tol {args.tol:g} uu, shade_above_band_uu {args.above_band:g} uu, map_unreachable "
        f"{args.unreachable}, ramp {z_lo:.0f}..{z_hi:.0f} ({ramp_kind}, span "
        f"{z_hi - z_lo:.0f} uu), shade_lo_color {lo_color[0]},{lo_color[1]},{lo_color[2]}, "
        f"shade_hi_color {hi_color[0]},{hi_color[1]},{hi_color[2]}, shade_gamma {args.gamma:g}"
    )
    print(f"wrote {args.out} ({args.out.stat().st_size / 1024:.0f} kB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
