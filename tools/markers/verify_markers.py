#!/usr/bin/env python3
r"""
Score `markers/chapter*.json` against the live WuchangRecon world dumps.

The dumps list, for every actor the game had loaded at the moment of the F8
press, `<Class> <full object path> | loc X Y Z`.  The full path carries the
owning sublevel and the cooked object name, which is exactly the join key the
offline extractor emits (`level` + `obj`).  So every matched pair is a direct
check of the pak-side position against the engine's own.

    python verify_markers.py --markers ..\..\markers\chapter1.json

Collected pickups are parked at the world origin by the level saver (see
`lessons.md`), so `loc 0 0 0` rows are reported separately and never counted as
mismatches.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import math
import os
import re
import sys

DUMPS = (r"E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers"
         r"\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangRecon\out"
         r"\dump_*_world.txt")

LINE = re.compile(
    r"^\s{2}(\S+)\s+(\S+)\s+\|\s+loc\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s*$")
PATH = re.compile(r"/([^/.]+)\.[^:]+:PersistentLevel\.(.+)$")


def load_dumps(pattern):
    live = {}
    files = 0
    for f in sorted(glob.glob(pattern)):
        if "_menu_" in os.path.basename(f):
            continue
        files += 1
        for ln in open(f, encoding="utf-8", errors="replace"):
            m = LINE.match(ln.rstrip("\n"))
            if not m:
                continue
            cls, path, x, y, z = m.groups()
            pm = PATH.search(path)
            if not pm:
                continue
            lvl, obj = pm.groups()
            live.setdefault((lvl, obj), (cls, float(x), float(y), float(z)))
    return live, files


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--markers", required=True)
    ap.add_argument("--dumps", default=DUMPS)
    ap.add_argument("--tol", type=float, default=5.0)
    ap.add_argument("--show", type=int, default=15)
    a = ap.parse_args(argv)

    live, nfiles = load_dumps(a.dumps)
    doc = json.load(open(a.markers, encoding="utf-8"))
    ms = doc["markers"]
    print(f"dumps: {nfiles} files, {len(live)} distinct live actors")
    print(f"markers: {len(ms)} in {os.path.basename(a.markers)}")

    matched = miss = parked = bad = 0
    errs = []
    per = collections.Counter()
    worst = []
    for m in ms:
        key = (m["level"], m["obj"])
        if key not in live:
            miss += 1
            continue
        cls, x, y, z = live[key]
        if x == 0.0 and y == 0.0 and z == 0.0:
            parked += 1
            continue
        d = math.dist((m["x"], m["y"], m["z"]), (x, y, z))
        matched += 1
        errs.append(d)
        per[m["cat"] + (" ok" if d <= a.tol else " BAD")] += 1
        if d > a.tol:
            bad += 1
            worst.append((d, m, (x, y, z)))
    errs.sort()
    print(f"\nin both: {matched}  (+{parked} collected/parked at origin, "
          f"{miss} not loaded in any dump)")
    if matched:
        print(f"agree within {a.tol} uu: {matched - bad}/{matched} = "
              f"{100.0 * (matched - bad) / matched:.1f} %")
        print(f"error: median {errs[len(errs)//2]:.3f} uu  "
              f"p90 {errs[int(len(errs)*0.9)]:.3f}  max {errs[-1]:.3f}")
    for k, v in sorted(per.items()):
        print(f"   {k:20s} {v}")
    worst.sort(reverse=True)
    for d, m, l in worst[:a.show]:
        print(f"   MISMATCH {d:12.1f}  {m['cls']:28s} {m['level']}/{m['obj']}"
              f"  ours=({m['x']:.1f},{m['y']:.1f},{m['z']:.1f}) "
              f"live=({l[0]:.1f},{l[1]:.1f},{l[2]:.1f})")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
