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
                            [--window-px 512] [--tol 200] [--fade 800]

The rule, per pixel, over the surfaces stored there
---------------------------------------------------
    nearest surface with |Z - feetZ| <= tol      -> OPAQUE   (alpha 1.00)
    else nearest surface below feetZ within fade -> DIM      (alpha 0.25)
    else nearest surface above feetZ within fade -> FAINT    (alpha 0.15)
    else                                         -> transparent

Colour, all three classes
-------------------------
Not a flat fill: the pixel is shaded by how far its surface sits above or below the
player's feet, so slopes, ramps and staircases inside one storey read as a gentle
gradient instead of a uniform blob.

    d     = surfaceZ - feetZ                       (uu, signed)
    scale = tol   for the OPAQUE class
            fade  for the DIM and FAINT classes
    lum   = 1 + floor_gradient_strength * clamp(d / scale, -1, +1)
    rgb   = clamp(round(floor_base_color * lum), 0, 255)      per channel

so the surface exactly at the feet is `floor_base_color` itself, one full tolerance
above it is `(1 + strength)` times as bright and one full tolerance below is
`(1 - strength)` times. Normalising the dim/faint classes by `fade` instead of `tol`
keeps a visible gradient in them too - by definition they are all further than `tol`
away, so `tol` would clamp every one of them to the same extreme.

`floor_gradient_strength` (default 0.18) and `floor_base_color` (default 214,208,196)
are the names these two knobs carry in the runtime's config, on purpose.

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
DEFAULT_BASE_COLOR = (214, 208, 196)
DEFAULT_GRADIENT_STRENGTH = 0.18
ALPHA_OPAQUE = 1.00
ALPHA_DIM = 0.25
ALPHA_FAINT = 0.15


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
    maps_dir: Path, ch: dict, cu: int, cv: int, half: int
) -> tuple["np.ndarray", "np.ndarray", int, int]:
    """
    Load the `2*half` px window centred on (cu, cv) out of every height map.

    Returns `(z, valid, x0, y0)` with `z` shaped `(slots, h, w)` in world uu and
    `valid` the same shape (code != 0). Windows are clipped to the image, so the
    returned box may be smaller than requested; `x0`/`y0` are its top-left pixel.
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
    for rel in mapfmt.height_plane_list(ch)[0]:
        img = Image.open(maps_dir / rel)
        if img.size != (w_img, h_img):
            sys.exit(f"{rel} is {img.size[0]}x{img.size[1]}, manifest says {w_img}x{h_img}")
        raw = np.asarray(img.crop((x0, y0, x1, y1))).astype(np.uint16)
        code = mapfmt.z_codes(raw).astype(np.uint32)
        valid = code > 0
        z = np.full(code.shape, np.nan, dtype=np.float64)
        z[valid] = z_min + (code[valid].astype(np.float64) - 1.0) / float(code_max - 1) * span
        zs.append(z)
        vs.append(valid)
    return np.stack(zs), np.stack(vs), x0, y0


def slice_window(
    z: "np.ndarray",
    feet_z: float,
    tol: float,
    fade: float,
) -> tuple["np.ndarray", "np.ndarray", "np.ndarray"]:
    """
    Apply the slicing rule. Returns `(cls, d, alpha)`.

    `cls`: 0 empty, 1 opaque, 2 dim (below), 3 faint (above).
    `d`  : the chosen surface's `Z - feetZ` (NaN where empty).
    """
    d = z - feet_z
    ad = np.abs(d)

    with np.errstate(invalid="ignore"):
        near = ad <= tol
        below = (d < 0) & (ad <= fade)
        above = (d > 0) & (ad <= fade)

    big = np.float64(1e30)

    def pick(sel: "np.ndarray") -> tuple["np.ndarray", "np.ndarray"]:
        """Per pixel, the slot with the smallest |d| among `sel`; (found, chosen d)."""
        cost = np.where(sel, np.nan_to_num(ad, nan=big), big)
        k = np.argmin(cost, axis=0)
        found = np.take_along_axis(sel, k[None], axis=0)[0]
        chosen = np.take_along_axis(d, k[None], axis=0)[0]
        return found, chosen

    f_near, d_near = pick(near)
    f_below, d_below = pick(below)
    f_above, d_above = pick(above)

    cls = np.zeros(d.shape[1:], dtype=np.uint8)
    out_d = np.full(d.shape[1:], np.nan)
    cls[f_above] = 3
    out_d[f_above] = d_above[f_above]
    cls[f_below] = 2
    out_d[f_below] = d_below[f_below]
    cls[f_near] = 1
    out_d[f_near] = d_near[f_near]

    alpha = np.zeros(cls.shape)
    alpha[cls == 1] = ALPHA_OPAQUE
    alpha[cls == 2] = ALPHA_DIM
    alpha[cls == 3] = ALPHA_FAINT
    return cls, out_d, alpha


def shade(
    cls: "np.ndarray",
    d: "np.ndarray",
    tol: float,
    fade: float,
    base_color: tuple[int, int, int],
    strength: float,
) -> "np.ndarray":
    """The colour formula from the docstring. Returns float RGB, shape (h, w, 3)."""
    scale = np.where(cls == 1, tol, fade)
    with np.errstate(invalid="ignore"):
        t = np.clip(np.nan_to_num(d, nan=0.0) / np.maximum(scale, 1e-6), -1.0, 1.0)
    lum = 1.0 + strength * t
    base = np.array(base_color, dtype=np.float64)
    return np.clip(lum[..., None] * base[None, None, :], 0.0, 255.0)


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
    ap.add_argument("--fade", type=float, default=800.0, help="below/above fade range, uu (default 800)")
    ap.add_argument(
        "--gradient-strength",
        type=float,
        default=DEFAULT_GRADIENT_STRENGTH,
        help=f"runtime key floor_gradient_strength (default {DEFAULT_GRADIENT_STRENGTH})",
    )
    ap.add_argument(
        "--base-color",
        default=",".join(str(c) for c in DEFAULT_BASE_COLOR),
        help=f"runtime key floor_base_color, 'R,G,B' (default {','.join(str(c) for c in DEFAULT_BASE_COLOR)})",
    )
    ap.add_argument("--scale", type=int, default=1, help="nearest-neighbour upscale of the PNG")
    args = ap.parse_args(argv)

    try:
        base_color = tuple(int(v) for v in args.base_color.split(","))
        if len(base_color) != 3:
            raise ValueError
    except ValueError:
        return _fail("--base-color wants three comma-separated 0..255 numbers, e.g. 214,208,196")

    ch, schema = load_chapter(args.maps, args.chapter)
    ppu = float(ch["px_per_uu"])
    cu = int(round((args.y - float(ch["min_y"])) * ppu))
    cv = int(round((float(ch["max_x"]) - args.x) * ppu))
    half = max(8, args.window_px // 2)

    print(f"maps.json schema {schema}, chapter {args.chapter}, {ch['max_surfaces']} surface slots")
    print(
        f"query world X {args.x:.0f} Y {args.y:.0f} feetZ {args.z:.0f} -> pixel (u {cu}, v {cv}) "
        f"of {ch['width']}x{ch['height']} @ {ppu:g} px/uu"
    )

    z, valid, x0, y0 = read_window(args.maps, ch, cu, cv, half)
    cls, d, alpha = slice_window(z, args.z, args.tol, args.fade)
    rgb = shade(cls, d, args.tol, args.fade, base_color, args.gradient_strength)

    bg = np.array(BACKGROUND, dtype=np.float64)
    out = bg[None, None, :] * (1.0 - alpha[..., None]) + rgb * alpha[..., None]
    draw_cross(out, cu - x0, cv - y0)

    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
    if args.scale > 1:
        img = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out, optimize=True)

    n = cls.size
    n_op = int(np.count_nonzero(cls == 1))
    n_dim = int(np.count_nonzero(cls == 2))
    n_fa = int(np.count_nonzero(cls == 3))
    n_em = int(np.count_nonzero(cls == 0))
    surf = int(np.count_nonzero(valid))
    here = [
        None if not valid[k, cv - y0, cu - x0] else round(float(z[k, cv - y0, cu - x0]), 1)
        for k in range(z.shape[0])
    ]
    print(f"window {cls.shape[1]}x{cls.shape[0]} px at image ({x0}, {y0}), {surf} stored surfaces in it")
    print(f"surfaces under the query pixel (lowest first): {here}")
    print(
        f"opaque {n_op} ({100.0 * n_op / n:.1f} %)  dim {n_dim} ({100.0 * n_dim / n:.1f} %)  "
        f"faint {n_fa} ({100.0 * n_fa / n:.1f} %)  empty {n_em} ({100.0 * n_em / n:.1f} %)"
    )
    if cls[cv - y0, cu - x0] != 1:
        print("  ! the query pixel itself is NOT opaque - feetZ does not match any stored surface there")
    print(
        f"tol {args.tol:g} uu, fade {args.fade:g} uu, floor_base_color {base_color[0]},{base_color[1]},"
        f"{base_color[2]}, floor_gradient_strength {args.gradient_strength:g}"
    )
    print(f"wrote {args.out} ({args.out.stat().st_size / 1024:.0f} kB)")
    return 0


def _fail(msg: str) -> int:
    print(msg, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
