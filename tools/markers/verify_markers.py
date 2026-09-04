#!/usr/bin/env python3
r"""
Score `markers/chapter*.json` against the live WuchangRecon world dumps.

The dumps list, for every actor the game had loaded at the moment of the F8
press, `<Class> <full object path> | loc X Y Z`.  The full path carries the
owning sublevel and the cooked object name, which is exactly the join key the
offline extractor emits (`level` + `obj`).  So every matched pair is a direct
check of the pak-side position against the engine's own.

    python verify_markers.py                       # every markers/chapter*.json
    python verify_markers.py --markers ..\..\markers\chapter1.json
    python verify_markers.py --dumps "D:\somewhere\dump_*_world.txt"

Collected pickups are parked at the world origin by the level saver (see
`lessons.md`), so `loc 0 0 0` rows are reported separately and never counted as
mismatches.

WHERE THE DUMPS COME FROM (review item C.19)
--------------------------------------------
The default is the set committed to this repo -
`tools/lua-recon/WuchangRecon/out/dump_*_world.txt`, kept by an explicit
`.gitignore` exception because they are irreplaceable in-game evidence - so the
tool runs on a fresh clone with no game installed and no environment set up.
`WUCHANG_RECON_DUMPS` or `--dumps` override the glob when you have a newer
session's dumps.

The dumps only ever cover the areas that were loaded when F8 was pressed, so a
chapter with no overlap is reported as "no live actors in common" and is not a
failure - `--require` turns a chapter with matches but disagreements into a
non-zero exit, which is what the regen driver uses.
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

_HERE = os.path.dirname(os.path.abspath(__file__))

# The reference dumps committed to this repo. No machine-specific path and no
# game install needed; `WUCHANG_RECON_DUMPS` or `--dumps` point at a newer set.
REPO_DUMPS = os.path.join(_HERE, "..", "lua-recon", "WuchangRecon", "out",
                          "dump_*_world.txt")
DUMPS = os.environ.get("WUCHANG_RECON_DUMPS") or REPO_DUMPS
MARKERS = os.path.join(_HERE, "..", "..", "markers")

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


def score(path: str, live: dict, tol: float, show: int) -> tuple[int, int]:
    """Print one chapter's agreement; return (matched, disagreements)."""
    doc = json.load(open(path, encoding="utf-8"))
    ms = doc["markers"]
    print(f"markers: {len(ms)} in {os.path.basename(path)}")

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
        per[m["cat"] + (" ok" if d <= tol else " BAD")] += 1
        if d > tol:
            bad += 1
            worst.append((d, m, (x, y, z)))
    errs.sort()
    print(f"  in both: {matched}  (+{parked} collected/parked at origin, "
          f"{miss} not loaded in any dump)")
    if matched:
        print(f"  agree within {tol} uu: {matched - bad}/{matched} = "
              f"{100.0 * (matched - bad) / matched:.1f} %")
        print(f"  error: median {errs[len(errs)//2]:.3f} uu  "
              f"p90 {errs[int(len(errs)*0.9)]:.3f}  max {errs[-1]:.3f}")
    for k, v in sorted(per.items()):
        print(f"     {k:20s} {v}")
    worst.sort(reverse=True)
    for d, m, l in worst[:show]:
        print(f"     MISMATCH {d:12.1f}  {m['cls']:28s} {m['level']}/{m['obj']}"
              f"  ours=({m['x']:.1f},{m['y']:.1f},{m['z']:.1f}) "
              f"live=({l[0]:.1f},{l[1]:.1f},{l[2]:.1f})")
    return matched, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--markers", default="",
                    help="a chapter json, or a directory of them "
                         "(default: the repo's markers/)")
    ap.add_argument("--dumps", default=DUMPS,
                    help="glob of WuchangRecon F8 world dumps "
                         "(default: the ones committed to this repo)")
    ap.add_argument("--tol", type=float, default=5.0)
    ap.add_argument("--show", type=int, default=15)
    ap.add_argument("--require", action="store_true",
                    help="exit non-zero when a chapter has matches that disagree; "
                         "without it the tool only reports")
    a = ap.parse_args(argv)

    live, nfiles = load_dumps(a.dumps)
    print(f"dumps: {nfiles} file(s) matching {a.dumps}, "
          f"{len(live)} distinct live actors")
    if not live:
        print("  no dumps found - nothing to score. This is not a failure: the "
              "dumps are recorded in-game evidence, not a build input.")
        return 0

    target = a.markers or MARKERS
    if os.path.isdir(target):
        paths = sorted(glob.glob(os.path.join(target, "chapter*.json")))
        paths = [p for p in paths if ".sample." not in os.path.basename(p)]
    else:
        paths = [target]
    if not paths:
        print(f"  no chapter json under {target}")
        return 1

    total_matched = total_bad = 0
    for p in paths:
        m, b = score(p, live, a.tol, a.show)
        total_matched += m
        total_bad += b
    print(f"\ntotal: {total_matched} marker(s) scored, {total_bad} disagree "
          f"beyond {a.tol} uu")
    if total_matched == 0:
        print("  no live actors in common with these dumps - not a failure")
        return 0
    return 1 if (a.require and total_bad) else 0


if __name__ == "__main__":
    sys.exit(main())
