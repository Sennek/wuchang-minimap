#!/usr/bin/env python3
"""
Render one world-space window of the offline navmesh, clipped to a Z band.

`render.py` does the real map (per-cell Z clustering into floors).  This is the
apples-to-apples cross-check against an F11 probe grid: the probe snaps within
+-500 uu of its own plane and collapses everything else, so the only fair
comparison is "polygons whose Z is inside that same band, over that same
window", drawn at the probe's own scale.

    python zslice.py --input ../dumps_offline --agent Small \
        --center 18142 5609 -1550 --half 2050 --z-half 500 \
        --out ../out_offline/_spot1_zslice.png
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

from PIL import Image, ImageDraw

COL_BG = (16, 18, 22)
COL_FILL = (86, 168, 118)
COL_EDGE = (232, 244, 236)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--agent", default="Small")
    ap.add_argument("--center", nargs=3, type=float, required=True,
                    metavar=("X", "Y", "Z"))
    ap.add_argument("--half", type=float, default=2050.0, help="uu, XY half-extent")
    ap.add_argument("--z-half", type=float, default=500.0, help="uu, Z half-band")
    ap.add_argument("--px-per-uu", type=float, default=0.12)
    ap.add_argument("--edges", action="store_true")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    cx, cy, cz = a.center
    x0, x1 = cx - a.half, cx + a.half
    y0, y1 = cy - a.half, cy + a.half
    z0, z1 = cz - a.z_half, cz + a.z_half

    w = max(1, int((y1 - y0) * a.px_per_uu) + 1)
    h = max(1, int((x1 - x0) * a.px_per_uu) + 1)
    img = Image.new("RGB", (w, h), COL_BG)
    dr = ImageDraw.Draw(img)

    def to_px(X, Y):
        return ((Y - y0) * a.px_per_uu, (x1 - X) * a.px_per_uu)

    kept = total = 0
    for path in sorted(glob.glob(os.path.join(a.input, a.agent, "tiles_*.json"))):
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        for t in doc["tiles"]:
            tb0, tb1 = t["bmin"], t["bmax"]
            if tb1[0] < x0 or tb0[0] > x1 or tb1[1] < y0 or tb0[1] > y1:
                continue
            if tb1[2] < z0 or tb0[2] > z1:
                continue
            verts = t["verts"]
            for p in t["polys"]:
                pts = [verts[i] for i in p["v"]]
                total += 1
                zc = sum(q[2] for q in pts) / len(pts)
                if not (z0 <= zc <= z1):
                    continue
                kept += 1
                dr.polygon([to_px(q[0], q[1]) for q in pts], fill=COL_FILL,
                           outline=COL_EDGE if a.edges else None)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    img.save(a.out)
    print(f"[{a.agent}] window X {x0:.0f}..{x1:.0f} Y {y0:.0f}..{y1:.0f} "
          f"Z {z0:.0f}..{z1:.0f} -> {kept}/{total} polys, {w}x{h} px -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
