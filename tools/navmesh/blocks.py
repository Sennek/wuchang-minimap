#!/usr/bin/env python3
"""The game's invisible walls as geometry: `markers/blocks.json` -> a mask over map pixels.

PURE - numpy and the map `Bounds`, nothing else. Both consumers read this module: the map build,
where a wall is a cell the reachability flood may not enter, and `oob_pick.py`, which draws the
same answer so the rule is judged on its own picture.

A blocker is an oriented box: `RelativeLocation`, `RelativeRotation` and `RelativeScale3D` of a
100 uu cube mesh, so its half-extent is `50 * scale` along each of its own axes. Half of them are
tilted - a wall that follows a slope carries pitch or roll - so the Z a box occupies is not
constant over its footprint and the mask is built by clipping a **vertical ray** through the box
per pixel, never by its bounding Z.

`Cylinder`, `Plane` and the handful of Nanite meshes the Block levels also place (under 1 % of the
boxes) are treated as that same cube: they are read as a slight over-block, not as their own shape.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np

CUBE_HALF_UU = 50.0
# A wall stops a player standing on a surface when it occupies the space that player's body does:
# from just above their feet to the top of their head. A UE character capsule here is ~180 uu.
STAND_LO_UU = 20.0
STAND_HI_UU = 180.0


class Walls:
    """One chapter's blockers, as parallel arrays."""

    __slots__ = ("p", "m", "half", "cube", "plate", "levels", "meshes", "level_of", "mesh_of")

    def __len__(self) -> int:
        return len(self.p)


def load(path: str | Path, chapter: str) -> Walls | None:
    """`blocks.json` -> the chapter's boxes, with the rotation baked into a matrix per box."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    entry = doc.get("chapters", {}).get(chapter)
    if not entry or not entry.get("lv"):
        return None
    n = len(entry["lv"])
    w = Walls()
    w.p = np.asarray(entry["p"], dtype=np.float64).reshape(n, 3)
    rot = np.radians(np.asarray(entry["r"], dtype=np.float64).reshape(n, 3))
    scl = np.asarray(entry["s"], dtype=np.float64).reshape(n, 3)
    w.half = np.abs(scl) * doc.get("cube_half_uu", CUBE_HALF_UU)
    w.levels = entry["levels"]
    w.meshes = entry["meshes"]
    w.level_of = np.asarray(entry["lv"], dtype=np.int32)
    w.mesh_of = np.asarray(entry["m"], dtype=np.int32)
    cubes = set(doc.get("cube_meshes", ()))
    w.cube = np.asarray([m in cubes for m in w.meshes], dtype=bool)[w.mesh_of]
    w.m = rotation(rot[:, 0], rot[:, 1], rot[:, 2])
    ext = np.ptp(corners(w), axis=1)
    # A box thin in Z against both of its horizontal sides is a floor or a ceiling, not a wall.
    # They are 4-15 % of a chapter and the biggest things in it - one plate covers 10 000 uu of
    # ground - so a picture that does not tell them apart is a picture of the plates.
    w.plate = ext[:, 2] < 0.25 * np.minimum(ext[:, 0], ext[:, 1])
    return w


def rotation(pitch, yaw, roll) -> np.ndarray:
    """`FRotationMatrix` per box, shaped `(n, 3, 3)`: the rows are the box's own axes in world
    space, so a local offset is `lx*m[0] + ly*m[1] + lz*m[2]`."""
    sp, cp = np.sin(pitch), np.cos(pitch)
    sy, cy = np.sin(yaw), np.cos(yaw)
    sr, cr = np.sin(roll), np.cos(roll)
    m = np.empty((len(pitch), 3, 3), dtype=np.float64)
    m[:, 0, 0], m[:, 0, 1], m[:, 0, 2] = cp * cy, cp * sy, sp
    m[:, 1, 0], m[:, 1, 1], m[:, 1, 2] = sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp
    m[:, 2, 0], m[:, 2, 1], m[:, 2, 2] = -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp
    return m


def corners(w: Walls) -> np.ndarray:
    """The eight world corners of every box, `(n, 8, 3)`."""
    signs = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)],
                     dtype=np.float64)
    local = signs[None, :, :] * w.half[:, None, :]            # (n, 8, 3)
    return w.p[:, None, :] + np.einsum("nkj,nji->nki", local, w.m)


def pixel_boxes(w: Walls, bounds) -> np.ndarray:
    """Each box's pixel bounding box `(n, 4)` as `u0, v0, u1, v1`, half-open and clipped."""
    c = corners(w)
    u = (c[:, :, 1] - bounds.min_y) * bounds.px_per_uu
    v = (bounds.max_x - c[:, :, 0]) * bounds.px_per_uu
    out = np.stack([np.floor(u.min(1)), np.floor(v.min(1)),
                    np.ceil(u.max(1)) + 1, np.ceil(v.max(1)) + 1], axis=1)
    np.clip(out[:, 0], 0, bounds.width, out=out[:, 0])
    np.clip(out[:, 1], 0, bounds.height, out=out[:, 1])
    np.clip(out[:, 2], 0, bounds.width, out=out[:, 2])
    np.clip(out[:, 3], 0, bounds.height, out=out[:, 3])
    return out.astype(np.int32)


def ray_span(w: Walls, i: int, wx: np.ndarray, wy: np.ndarray):
    """The Z interval box `i` occupies over each world column, by slab clipping.

    A vertical ray through `(wx, wy)` is `P(z) = (wx, wy, z)`; in the box's own frame that is
    `A + z*B`, and each axis gives `|A_k + z*B_k| <= half_k`. The intersection of the three is the
    interval the box fills at that column - empty where `lo > hi`, which is every column outside
    the footprint.
    """
    m, half, p = w.m[i], w.half[i], w.p[i]
    lo = np.full(wx.shape, -np.inf)
    hi = np.full(wx.shape, np.inf)
    dx, dy = wx - p[0], wy - p[1]
    for k in range(3):
        a = dx * m[k, 0] + dy * m[k, 1] - p[2] * m[k, 2]
        b = m[k, 2]
        h = half[k]
        if abs(b) < 1e-12:                       # the ray never crosses this slab
            outside = np.abs(a) > h
            lo = np.where(outside, np.inf, lo)
            hi = np.where(outside, -np.inf, hi)
            continue
        t0, t1 = (-h - a) / b, (h - a) / b
        lo = np.maximum(lo, np.minimum(t0, t1))
        hi = np.minimum(hi, np.maximum(t0, t1))
    return lo, hi


def standing_mask(w: Walls, bounds, z: np.ndarray,
                  stand_lo: float = STAND_LO_UU, stand_hi: float = STAND_HI_UU) -> np.ndarray:
    """Where a wall stands in the way of a player on the surface `z` - `band_mask` over the body
    space, so a wall buried under the floor and a wall hanging out of reach both read as no wall."""
    return band_mask(w, bounds, z, stand_lo, stand_hi)


def band_mask(w: Walls, bounds, z: np.ndarray, lo: float, hi: float,
              where: np.ndarray | None = None) -> np.ndarray:
    """Where a box occupies `[z + lo, z + hi]` over the pixel.

    `z` is a reference height per pixel - the surface the map draws, or the storey's own Z where it
    draws nothing, so a wall standing in the gap between two ledges is still on the picture. NaN in
    `z` is a pixel with no reference at all and never counts. `where` is a boolean per box, for a
    caller that wants only some of them.
    """
    out = np.zeros(z.shape, dtype=bool)
    boxes = pixel_boxes(w, bounds)
    ok = np.isfinite(z)
    for i in range(len(w)):
        if where is not None and not where[i]:
            continue
        u0, v0, u1, v1 = boxes[i]
        if u1 <= u0 or v1 <= v0:
            continue
        tile = z[v0:v1, u0:u1]
        live = ok[v0:v1, u0:u1] & ~out[v0:v1, u0:u1]
        if not live.any():
            continue
        uu = (np.arange(u0, u1) + 0.5) / bounds.px_per_uu + bounds.min_y
        vv = bounds.max_x - (np.arange(v0, v1) + 0.5) / bounds.px_per_uu
        wy, wx = np.meshgrid(uu, vv)
        zlo, zhi = ray_span(w, i, wx, wy)
        out[v0:v1, u0:u1] |= live & (zlo <= tile + hi) & (zhi >= tile + lo)
    return out
