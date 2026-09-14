"""
oob_measure.py - what a piece of navmesh covers, and what the shipped map says about those pixels.

Free functions over a `Chapter`: the coverage mask and per-pixel height stack of a polygon set,
which of those pixels the height planes hold, which of those the mod draws, the game's markers that
touch the piece, and the report the page shows. The chapter keeps what these measure; nothing here
holds state of its own.
"""

from __future__ import annotations

import io
import math
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np
from PIL import Image

import build_map
import render

if TYPE_CHECKING:
    from oob_chapter import Chapter

# A surface within this much Z of the polygon under the cursor is the same surface - the tolerance
# rasterize_heights merges height slots with.
SURFACE_MATCH_UU = 120.0

# A marker within this much Z of the polygon its XY falls inside is STANDING on it - the strict test,
# against `decide_islands`' seeding, which only asks for a box 300 uu wider and 600 uu taller.
STAND_Z_UU = 200.0

# Slots x pixels a piece's own height stack may take. A component is thousands of pixels and gets
# every slot; a whole cluster is 20 Mpx of bounding box, where eight slots would be 650 MB.
STACK_BUDGET_PX = 60_000_000

# XY pitch of the point-location grid. A navmesh tile is 1280 uu and a polygon is far smaller.
PICK_GRID_UU = 256.0


# ---------------------------------------------------------------------------------
# the piece on the map
# ---------------------------------------------------------------------------------


def mask_of(ch: Chapter, sel: list[dict],
            budget: int = STACK_BUDGET_PX) -> tuple[int, int, np.ndarray, np.ndarray]:
    """Coverage mask and per-pixel Z STACK of a polygon set, cropped to its bounding box.

    A piece folds over itself: chapter 3's component #40 spans 1 040 uu of Z, and 4 586 of its
    27 494 pixels carry two of its own surfaces more than 200 uu apart. One Z per pixel cuts one of
    them and leaves the other painted - the piece reads as marked, stays on the map, and refuses the
    next click on it. So the Z comes back as `(k, h, w)`, NaN where a slot holds nothing, and every
    caller asks `matches_z` whether a height is one of the piece's at that pixel.

    It is `build_map.rasterize_heights` that builds it - the same pass that laid the shipped height
    planes down, at the same `merge_tol` and the same `seam_px`. That is what the piece has to line
    up with, so there is nothing here to keep in step with it by hand.
    """
    us, vs = [], []
    for p in sel:
        for q in p["pts"]:
            u, v = ch.bounds.to_px(q[0], q[1])
            us.append(u)
            vs.append(v)
    x0 = max(0, int(math.floor(min(us))) - 1)
    x1 = min(ch.width, int(math.ceil(max(us))) + 2)
    y0 = max(0, int(math.floor(min(vs))) - 1)
    y1 = min(ch.height, int(math.ceil(max(vs))) + 2)
    w, h = max(1, x1 - x0), max(1, y1 - y0)

    # The crop's own bounds, on the chapter's pixel grid: `Bounds.width` is the span plus one pixel,
    # so the far edge sits on the last pixel's world position.
    ppu = ch.bounds.px_per_uu
    sub = render.Bounds(min_x=ch.bounds.max_x - (y0 + h - 1) / ppu,
                        min_y=ch.bounds.min_y + x0 / ppu,
                        max_x=ch.bounds.max_x - y0 / ppu,
                        max_y=ch.bounds.min_y + (x0 + w - 1) / ppu,
                        px_per_uu=ppu)
    # A whole cluster is 20 Mpx of bounding box, where eight slots would be 650 MB. The budget cuts
    # the slot count, never the pixels: a piece small enough to judge always gets all of them.
    # Slots that are cut are surfaces the piece folds over itself and loses - measured on chapter 4,
    # a whole chapter's worth of catch reads 2 035 of 94 133 surfaces short at two slots and nothing
    # short at four, so a caller measuring a cut hands over a budget that buys it four.
    slots = max(1, min(len(ch.plane_paths), int(budget // max(1, w * h))))
    z, _ = build_map.rasterize_heights(sel, sub, slots, merge_tol=SURFACE_MATCH_UU,
                                       progress=0)
    # `Bounds.width` rounds up, so the stack can come back one pixel wider than the crop asked for.
    # The origin is the same either way, so the extra row or column is simply trimmed - and it has
    # to be, or a piece at the image's edge would not line up with the height planes under it.
    z = z[:, :h, :w]
    return x0, y0, ~np.isnan(z).all(axis=0), z


def matches_z(zst: np.ndarray, z: "np.ndarray | float") -> np.ndarray:
    """Where a height is one of the piece's own - any slot of its stack. NaN never matches."""
    with np.errstate(invalid="ignore"):
        return (np.abs(zst - z) <= SURFACE_MATCH_UU).any(axis=0)


def raster_masks(ch: Chapter, x0: int, y0: int, m: np.ndarray,
                 z: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Which pixels of a piece the height planes hold, and which of those the mod draws.

    A plane can only answer for pixels the piece covers, and a pixel is answered by the first plane
    that holds one of its heights, so the work is carried as the LIST of pixels still unanswered:
    the bounding box of a whole cluster is five times the piece inside it, and the first plane
    usually takes almost all of it. Whole-box arithmetic over eight planes measured 5.7 s against
    0.3 s for the same answer.
    """
    h, w = m.shape
    lit = np.zeros((h, w), dtype=bool)
    reach = np.zeros((h, w), dtype=bool)
    # A piece can hang off the edge of the picture - the cut tightens a chapter's bounds, and this
    # tool holds the geometry from before it. Whatever falls outside reads as no surface.
    cy, cx = max(0, min(h, ch.height - y0)), max(0, min(w, ch.width - x0))
    if not (cy and cx):
        return lit, reach
    inside = m.copy()
    inside[cy:, :] = False
    inside[:, cx:] = False
    idx = np.flatnonzero(inside)          # unanswered pixels, as offsets into the crop
    rows, cols = np.divmod(idx, w)
    zst = z[:, rows, cols]
    flat_lit, flat_reach = lit.reshape(-1), reach.reshape(-1)
    for i in range(len(ch.plane_paths)):
        if idx.size == 0:
            break
        raw = ch.plane(i)[y0:y0 + cy, x0:x0 + cx][rows, cols]
        code = raw & ch.z_code_mask
        hit = (code != 0) & matches_z(zst, ch.z_of_code(code))
        if not hit.any():
            continue
        took = idx[hit]
        flat_lit[took] = True
        flat_reach[took[(raw[hit] & ch.reach_bit) != 0]] = True
        left = ~hit
        idx, rows, cols, zst = idx[left], rows[left], cols[left], zst[:, left]
    return lit, reach


def rules_on(ch: Chapter, cid: int) -> dict:
    """What the cut rules measure on this component, and which of them takes it.

    The numbers, not the verdict, are the reason this is on the report: a piece the user calls out
    of bounds that the rules leave standing says exactly where its threshold would have to go.
    """
    esc = ch.escape(ch.rules.fall).get(cid, float("inf"))
    wall = ch.walld().get(cid)
    return {"escape": None if esc == float("inf") else round(esc),
            "wall": None if wall is None else round(wall),
            "cut_by": ch.rule_of(cid)}


def drawn_surfaces(ch: Chapter, x0: int, y0: int, m: np.ndarray, z: np.ndarray) -> int:
    """How many drawn SURFACES a piece would take off the map, over every height plane.

    `raster_masks` answers per pixel - is any of this piece on screen here - which is what says
    whether cutting a piece changes anything. A cut is judged against the whole map instead, and the
    map is a stack: the same XY carries a surface per storey, and `Chapter.drawn_total` counts them
    all. So the numerator is counted the same way, one hit per plane, and no plane is dropped once a
    pixel has been answered.
    """
    h, w = m.shape
    cy, cx = max(0, min(h, ch.height - y0)), max(0, min(w, ch.width - x0))
    if not (cy and cx):
        return 0
    inside = m.copy()
    inside[cy:, :] = False
    inside[:, cx:] = False
    idx = np.flatnonzero(inside)
    rows, cols = np.divmod(idx, w)
    zst = z[:, rows, cols]
    n = 0
    for i in range(len(ch.plane_paths)):
        raw = ch.plane(i)[y0:y0 + cy, x0:x0 + cx][rows, cols]
        code = raw & ch.z_code_mask
        hit = (code != 0) & ((raw & ch.reach_bit) != 0) & matches_z(zst, ch.z_of_code(code))
        n += int(hit.sum())
    return n


def describe(ch: Chapter, poly: dict, mode: str) -> dict:
    sh = ch.shape(poly, mode)
    sel = sh["sel"]
    xs = [q[0] for p in sel for q in p["pts"]]
    ys = [q[1] for p in sel for q in p["pts"]]
    zs = [q[2] for p in sel for q in p["pts"]]
    stages: dict[str, int] = {}
    levels: dict[str, int] = {}
    tiles = set()
    for p in sel:
        stages[p["stage"]] = stages.get(p["stage"], 0) + 1
        tiles.add(p["tile"])
        name = Path(ch.tile_source.get(p["tile"], "?")).stem
        levels[name] = levels.get(name, 0) + 1

    comp = ch.comps[poly["comp"]] if "comp" in poly else None
    cl = (ch.clusters[comp["cluster"]]
          if comp is not None and comp.get("cluster") is not None else None)
    return {
        "mode": mode,
        "polys": len(sel),
        "comps": sh["comps"],
        "area_m2": sh["area_m2"],
        "bbox": [round(min(xs)), round(min(ys)), round(max(xs)), round(max(ys))],
        "z": [round(min(zs), 1), round(max(zs), 1)],
        "stages": stages,
        "tiles": len(tiles),
        "levels": sorted(levels.items(), key=lambda t: -t[1])[:8],
        "comp": None if comp is None else {
            "id": comp["id"], "polys": comp["polys"], "area_m2": round(comp["area"] / 10000.0, 1),
            "kept": comp["kept"], "seeded": bool(comp.get("seeded")),
        },
        "cluster": None if cl is None else {
            "id": cl["id"], "members": len(cl["members"]),
            "area_m2": round(cl["area"] / 10000.0, 1),
            "keep": cl.get("keep"), "why": cl.get("why"),
            "has_largest": cl.get("has_largest"),
        },
        "raster": {k: sh[k] for k in ("px", "px_on_map", "drawn", "reachable_pct")},
        "rules": None if comp is None else rules_on(ch, comp["id"]),
        "markers": markers_near(ch, sel),
        "overlay": {"x": sh["x"], "y": sh["y"],
                    "w": int(sh["mask"].shape[1]), "h": int(sh["mask"].shape[0])},
    }


def markers_near(ch: Chapter, sel: list[dict]) -> list[dict]:
    """The game's markers that touch this piece, and how.

    Two answers, never merged, because the difference is the whole question:

    * **on it** - the marker's XY falls inside one of the piece's polygons and its Z is within
      `STAND_Z_UU` of that polygon. The game put something here; the player comes here.
    * **near** - it only passes `decide_islands`' seed test against one of them: that polygon's
      bounding box grown by `seed_radius`, and its Z range widened by `DEFAULT_ISLAND_SEED_Z`. That
      is what makes a component `seeded` and immune to every marker-vetoed rule, and it is not the
      same statement at all - chapter 1's #20 is seeded by a scenery `Object` 282 uu to the side and
      553 uu above it, with nothing standing on the piece at all.

    The test runs per POLYGON, the way the seeding does. Against the piece's own bounding box it
    reported 40 markers for that component instead of one.
    """
    members = {p["idx"] for p in sel}
    by_idx = {p["idx"]: p for p in sel}
    r, zt = ch.args.island_seed_radius, render.DEFAULT_ISLAND_SEED_Z
    reach = int(math.ceil(r / PICK_GRID_UU)) + 1
    out = []
    for s in ch.seeds:
        gx, gy = int(math.floor(s["x"] / PICK_GRID_UU)), int(math.floor(s["y"] / PICK_GRID_UU))
        cand = {i for dx in range(-reach, reach + 1) for dy in range(-reach, reach + 1)
                for i in ch.grid.get((gx + dx, gy + dy), ()) if i in members}
        on, near = None, None
        for i in cand:
            p = by_idx[i]
            x0, y0, x1, y1 = p["bbox"]
            if (x0 <= s["x"] <= x1 and y0 <= s["y"] <= y1
                    and abs(p["cz"] - s["z"]) <= STAND_Z_UU
                    and render._point_in_poly(p["pts"], s["x"], s["y"])):
                on = p
                break
            if near is None and (x0 - r <= s["x"] <= x1 + r and y0 - r <= s["y"] <= y1 + r
                                 and p["_zlo"] - zt <= s["z"] <= p["_zhi"] + zt):
                near = p
        p = on or near
        if p is None:
            continue
        x0, y0, x1, y1 = p["bbox"]
        gap = math.hypot(max(x0 - s["x"], 0.0, s["x"] - x1), max(y0 - s["y"], 0.0, s["y"] - y1))
        out.append({"cat": s["cat"], "name": s.get("name", ""), "z": round(s["z"]),
                    "on": on is not None, "gap": round(gap), "dz": round(s["z"] - p["cz"])})
    out.sort(key=lambda t: (not t["on"], t["gap"], abs(t["dz"])))
    return out[:12]


def overlap(box: tuple[int, int, int, int],
            lay: dict) -> tuple[tuple[int, int, int, int], tuple[int, int, int, int]] | None:
    """Where a layer meets a pixel box: the box's own indices, then the layer's."""
    x0, y0, x1, y1 = box
    h, w = lay["mask"].shape
    ix0, iy0 = max(x0, lay["x"]), max(y0, lay["y"])
    ix1, iy1 = min(x1, lay["x"] + w), min(y1, lay["y"] + h)
    if ix1 <= ix0 or iy1 <= iy0:
        return None
    return ((ix0 - x0, iy0 - y0, ix1 - x0, iy1 - y0),
            (ix0 - lay["x"], iy0 - lay["y"], ix1 - lay["x"], iy1 - lay["y"]))


def inner(m: np.ndarray) -> np.ndarray:
    """The pixels of a mask with all four neighbours inside it - everything else is its edge."""
    return (np.pad(m[1:, :], ((0, 1), (0, 0))) & np.pad(m[:-1, :], ((1, 0), (0, 0)))
            & np.pad(m[:, 1:], ((0, 0), (0, 1))) & np.pad(m[:, :-1], ((0, 0), (1, 0))))


def overlay_png(m: np.ndarray) -> bytes:
    """The highlight: a translucent wash with a solid edge, as a bbox-sized RGBA PNG."""
    h, w = m.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[m] = (255, 64, 96, 110)
    rgba[m & ~inner(m)] = (255, 210, 40, 255)
    buf = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, format="PNG")
    return buf.getvalue()
