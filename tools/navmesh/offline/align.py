#!/usr/bin/env python3
"""
Score the offline-extracted navmesh against the in-game F11 probe grids / F7 track.

The probe CSVs (`context/dumps/navprobe_*.csv`) are a 41x41 grid of
`ProjectPointToNavigation` results taken around a spot the player actually stood
on; `track.csv` is a recorded walk.  Both are independent ground truth for the
offline extraction.

Four numbers are reported per probe, weakest to strongest:

  probe-hit XY      fraction of `hit=1` rows whose *query* (x, y) lands in a
                    polygon at that row's Z.  Deliberately pessimistic:
                    `ProjectPointToNavigation` reports a hit whenever navmesh
                    exists anywhere inside its (100, 100, 500) extent box, so a
                    query point over a wall still says hit and snaps sideways.
  probe-miss XY     fraction of `hit=0` rows that *are* covered.  This is the
                    false-positive rate and must be ~0.
  snap strict       fraction of snapped nav points (projx, projy, projz) inside a
                    polygon whose centroid Z is within `--z-tol`.
  snap bbox         same, but polygon XY *bounding boxes* instead of exact
                    containment.  This is the headline number: it is immune to
                    the tie-breaking artefact where a snapped point sits exactly
                    on an edge shared by two polygons and the even-odd test picks
                    the one belonging to another floor.

    python align.py --input ../dumps_offline --agent Small --z-tol 150 \
        --probe <csv> [--probe <csv>] [--track <csv>] [--json-out out.json]
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import sys
from collections import defaultdict

GRID = 512.0  # uu bucket for the point-in-poly index


def load_polys(root: str, agent: str) -> list[dict]:
    polys: list[dict] = []
    for path in sorted(glob.glob(os.path.join(root, agent, "tiles_*.json"))):
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        for t in doc["tiles"]:
            verts = t["verts"]
            for p in t["polys"]:
                pts = [verts[i] for i in p["v"]]
                polys.append({"pts": pts, "z": sum(q[2] for q in pts) / len(pts)})
    return polys


def build_index(polys: list[dict]):
    idx = defaultdict(list)
    for k, p in enumerate(polys):
        xs = [q[0] for q in p["pts"]]
        ys = [q[1] for q in p["pts"]]
        p["bb"] = (min(xs), min(ys), max(xs), max(ys))
        for gx in range(int(math.floor(min(xs) / GRID)), int(math.floor(max(xs) / GRID)) + 1):
            for gy in range(int(math.floor(min(ys) / GRID)), int(math.floor(max(ys) / GRID)) + 1):
                idx[(gx, gy)].append(k)
    return idx


def _bucket(idx, x, y):
    return idx.get((int(math.floor(x / GRID)), int(math.floor(y / GRID))), ())


def inside(pts, x, y) -> bool:
    """Even-odd point-in-polygon in the world XY plane."""
    c = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, yi = pts[i][0], pts[i][1]
        xj, yj = pts[j][0], pts[j][1]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def hits_at(polys, idx, x, y) -> list[int]:
    """Indices of polygons strictly containing (x, y)."""
    out = []
    for k in _bucket(idx, x, y):
        bb = polys[k]["bb"]
        if bb[0] <= x <= bb[2] and bb[1] <= y <= bb[3] and inside(polys[k]["pts"], x, y):
            out.append(k)
    return out


def nearest_z(polys, idx, x, y, z, pad: float = 0.0):
    """Smallest |polygon centroid Z - z| over polygons whose XY bbox covers (x, y)."""
    best = None
    for k in _bucket(idx, x, y):
        bb = polys[k]["bb"]
        if bb[0] - pad <= x <= bb[2] + pad and bb[1] - pad <= y <= bb[3] + pad:
            d = abs(polys[k]["z"] - z)
            if best is None or d < best:
                best = d
    return best


def _rows(path: str) -> list[dict]:
    """Read a WuchangRecon CSV, skipping its leading `#` comment banner."""
    with open(path, "r", encoding="utf-8") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    return list(csv.DictReader(lines))


def _pct(a: int, b: int) -> float:
    return 100.0 * a / b if b else 0.0


def score_probe(polys, idx, path: str, z_tol: float = 150.0, pad: float = 0.0) -> dict:
    rows = _rows(path)
    hit_n = hit_in = miss_n = miss_in = 0
    snap_n = snap_strict = snap_bbox = 0
    dzs: list[float] = []
    for r in rows:
        x, y, z = float(r["x"]), float(r["y"]), float(r["z"])
        is_hit = r["hit"] == "1"
        ref = float(r["projz"]) if (is_hit and r.get("projz")) else z
        ks = hits_at(polys, idx, x, y)
        dz = min((abs(polys[k]["z"] - ref) for k in ks), default=None)
        covered = bool(ks) and (z_tol <= 0.0 or (dz is not None and dz <= z_tol))
        if is_hit:
            hit_n += 1
            hit_in += covered
        else:
            miss_n += 1
            miss_in += covered

        if is_hit and r.get("projx"):
            px, py, pz = float(r["projx"]), float(r["projy"]), float(r["projz"])
            snap_n += 1
            pks = hits_at(polys, idx, px, py)
            pdz = min((abs(polys[k]["z"] - pz) for k in pks), default=None)
            if pks and (z_tol <= 0.0 or (pdz is not None and pdz <= z_tol)):
                snap_strict += 1
            bdz = nearest_z(polys, idx, px, py, pz, pad)
            if bdz is not None:
                dzs.append(bdz)
                if z_tol <= 0.0 or bdz <= z_tol:
                    snap_bbox += 1
    dzs.sort()
    return {
        "probe": os.path.basename(path),
        "rows": len(rows),
        "z_tol": z_tol,
        "bbox_pad": pad,
        "probe_hits": hit_n,
        "probe_hits_covered": hit_in,
        "probe_hits_covered_pct": _pct(hit_in, hit_n),
        "probe_misses": miss_n,
        "probe_misses_covered": miss_in,
        "probe_misses_covered_pct": _pct(miss_in, miss_n),
        "snap_points": snap_n,
        "snap_strict": snap_strict,
        "snap_strict_pct": _pct(snap_strict, snap_n),
        "snap_bbox": snap_bbox,
        "snap_bbox_pct": _pct(snap_bbox, snap_n),
        "dz_median": round(dzs[len(dzs) // 2], 2) if dzs else None,
        "dz_p90": round(dzs[min(int(0.9 * len(dzs)), len(dzs) - 1)], 2) if dzs else None,
    }


def score_track(polys, idx, path: str, z_tol: float = 150.0) -> dict:
    """
    Track samples are pawn *origin* positions -- the navmesh surface sits
    34-75 uu below them (capsule half-height), so the Z test uses the same
    tolerance as the probes rather than demanding an exact match.
    """
    rows = _rows(path)
    n = ins = ins_z = 0
    for r in rows:
        try:
            x, y, z = float(r["x"]), float(r["y"]), float(r["z"])
        except (TypeError, ValueError, KeyError):
            continue
        n += 1
        ks = hits_at(polys, idx, x, y)
        if ks:
            ins += 1
            if min(abs(polys[k]["z"] - z) for k in ks) <= z_tol:
                ins_z += 1
    return {"track": os.path.basename(path), "points": n,
            "inside_xy": ins, "inside_xy_pct": _pct(ins, n),
            "inside_xyz": ins_z, "inside_xyz_pct": _pct(ins_z, n)}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--agent", action="append", default=[])
    ap.add_argument("--z-tol", type=float, default=150.0)
    ap.add_argument("--bbox-pad", type=float, default=0.0)
    ap.add_argument("--probe", action="append", default=[])
    ap.add_argument("--track", action="append", default=[])
    ap.add_argument("--json-out", default="")
    a = ap.parse_args(argv)

    agents = a.agent or ["Small", "Big", "BitFat", "Giant"]
    out = {"schema": "wuchang-navmesh-alignment/1", "input": a.input,
           "z_tol": a.z_tol, "agents": []}
    for agent in agents:
        polys = load_polys(a.input, agent)
        print(f"[{agent}] {len(polys)} polygons")
        if not polys:
            continue
        idx = build_index(polys)
        rec = {"agent": agent, "polygons": len(polys), "probes": [], "tracks": []}
        for pat in a.probe:
            for p in sorted(glob.glob(pat)) or [pat]:
                r = score_probe(polys, idx, p, a.z_tol, a.bbox_pad)
                rec["probes"].append(r)
                print(f"  {r['probe']}")
                print(f"      probe-hit XY  {r['probe_hits_covered']:>5}/{r['probe_hits']:<5}"
                      f" = {r['probe_hits_covered_pct']:5.1f}%   (pessimistic, see docstring)")
                print(f"      probe-miss XY {r['probe_misses_covered']:>5}/{r['probe_misses']:<5}"
                      f" = {r['probe_misses_covered_pct']:5.1f}%   (false positives)")
                print(f"      snap strict   {r['snap_strict']:>5}/{r['snap_points']:<5}"
                      f" = {r['snap_strict_pct']:5.1f}%")
                print(f"      snap bbox     {r['snap_bbox']:>5}/{r['snap_points']:<5}"
                      f" = {r['snap_bbox_pct']:5.1f}%   dz median {r['dz_median']} p90 {r['dz_p90']} uu")
        for pat in a.track:
            for p in sorted(glob.glob(pat)) or [pat]:
                r = score_track(polys, idx, p, a.z_tol)
                rec["tracks"].append(r)
                print(f"  {r['track']}: XY {r['inside_xy']}/{r['points']}"
                      f" = {r['inside_xy_pct']:.1f}%, XYZ {r['inside_xyz']}/{r['points']}"
                      f" = {r['inside_xyz_pct']:.1f}%")
        out["agents"].append(rec)
    if a.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(a.json_out)), exist_ok=True)
        with open(a.json_out, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=1)
        print(f"wrote {a.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
