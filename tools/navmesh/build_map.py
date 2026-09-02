#!/usr/bin/env python3
"""
build_map.py - turn the offline navmesh tile dumps into the minimap's shipped assets.

This is the *packaging* step that sits on top of `render.py`: same loader, same
dedupe, same flat-plane filter, same north-up mapping - but the output is an RGBA
texture with a fully transparent background (so the overlay can composite it over
its own panel) plus a `maps.json` manifest the C++ mod reads at start-up.

    maps/
      maps.json                {"chapters": {"chapter1": {...}}}
      chapter1/small.png       RGBA, transparent background, Z-shaded walkable fill

Manifest (schema `wuchang-minimap-maps/1`), per chapter:

    image        path of the PNG relative to maps/            "chapter1/small.png"
    image_width  / image_height   pixels
    min_x min_y max_x max_y       world bounds in uu covered by the image
    px_per_uu    scale
    mapping      the formula, spelled out, so the C++ side can be checked against it
    floors       [[z_min, z_max], ...]  global Z bands (informational for now)

World -> image mapping is `render.py`'s north-up convention, verbatim:

    u (column) = (world_Y - min_y) * px_per_uu        east  -> right
    v (row)    = (max_x - world_X) * px_per_uu        north -> up

so image width comes from the world **Y** extent and image height from the world
**X** extent. `min_x`/`max_x` therefore bound the *vertical* axis of the picture -
that is not a typo.

Sizing
------
`--px-per-uu` is a *request*: the scale is halved (repeatedly) until neither
dimension exceeds `--max-dim` (default 8192, the safe D3D12 texture limit on the
feature levels this game uses). Chapter 1 spans ~71 700 x ~82 000 uu, so 0.08 px/uu
(1 px = 12.5 cm) gives ~6 600 x 5 800 px and fits comfortably.

Usage
-----
    # Chapter 1, primary (Small) agent - what ships
    python build_map.py --input dumps_offline --chapter chapter1 --out ../../maps

    # everything the dumps_offline tree has, at a coarser scale
    python build_map.py --input dumps_offline --chapter chapter1 --px-per-uu 0.05

The input tree is produced by `offline/navchunk.py` (see
`.workspace/wuchang-minimap/context/navmesh-offline.md` for the two commands).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render  # noqa: E402  (same directory; the loader/geometry code is shared)

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit("This script needs Pillow:  pip install pillow")

SCHEMA = "wuchang-minimap-maps/1"

# Light, desaturated cool grey ramp: low ground -> high ground. Deliberately low
# saturation so marker colours (and the player arrow) stay the only saturated
# things on the minimap.
FILL_RAMP = [
    (0.00, (104, 114, 124)),
    (0.30, (140, 150, 158)),
    (0.60, (178, 186, 192)),
    (0.85, (208, 214, 218)),
    (1.00, (232, 236, 238)),
]

EDGE_DARKEN = 42  # per-channel subtraction for the thin polygon edge
FILL_ALPHA = 235  # walkable fill opacity; the background stays fully transparent


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

    floors = render.assign_floors_bands(polys, render.DEFAULT_FLOOR_GAP)
    zranges = render.floor_z_ranges(polys, floors)
    print(f"[{args.chapter}] {floors} global Z band(s): " + ", ".join(f"{a:.0f}..{b:.0f}" for a, b in zranges))

    img = draw_map(polys, bounds, edges=not args.no_edges)

    out_root = args.out if args.out.is_absolute() else Path.cwd() / args.out
    rel_png = f"{args.chapter}/{args.agent.lower()}.png"
    png_path = out_root / rel_png
    png_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(png_path, optimize=True)
    size_mb = png_path.stat().st_size / (1024 * 1024)
    print(f"[{args.chapter}] wrote {png_path}  ({size_mb:.2f} MB)")
    if size_mb > args.max_mb:
        print(
            f"  ! {size_mb:.1f} MB is above the --max-mb budget of {args.max_mb} MB; "
            f"re-run with a smaller --px-per-uu",
            file=sys.stderr,
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
        "floors": [[a, b] for a, b in zranges],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", type=Path, default=Path("dumps_offline"), help="tile-dump root with agent subdirs")
    ap.add_argument("--agent", default="Small", help="navmesh agent to ship (default Small, the player's)")
    ap.add_argument("--chapter", default="chapter1", help="chapter key in maps.json (default chapter1)")
    ap.add_argument("--out", type=Path, default=Path("maps"), help="output maps/ directory")
    ap.add_argument("--px-per-uu", type=float, default=0.08, help="requested scale (default 0.08 = 1 px per 12.5 uu)")
    ap.add_argument("--max-dim", type=int, default=8192, help="hard texture-size cap in px (default 8192)")
    ap.add_argument("--max-mb", type=float, default=10.0, help="warn if the PNG exceeds this (default 10 MB)")
    ap.add_argument("--margin", type=float, default=256.0, help="world-space margin around the geometry, uu")
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
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path} ({len(manifest['chapters'])} chapter(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
