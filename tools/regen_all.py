#!/usr/bin/env python3
r"""Regenerate every offline data artifact, in dependency order (review C.17).

    python tools\regen_all.py                    # everything, all six chapters
    python tools\regen_all.py --verify           # + score against the recon dumps
    python tools\regen_all.py --no-pak-hash      # skip the 63 GB of sha256
    python tools\regen_all.py --only extract     # one step (repeatable)
    python tools\regen_all.py --list             # the step graph, then exit
    python tools\regen_all.py --dry-run          # print the commands only

WHY A DRIVER
------------
There were seven scripts with an order that existed only in one paragraph of
`docs/DEVELOPMENT.md`, and one of the dependencies is a genuine two-pass cycle
(`build_bossdoors.py` reads the boss markers that `extract_markers.py` writes,
and `extract_markers.py` bakes the resulting `bossdoor` field back into them).
Regenerating by hand therefore had a wrong answer that looked like a right one:
run the steps alphabetically and every boss loses its save-backed defeat
signal, with nothing to say so.

FAIL FAST
---------
Every prerequisite is checked before the first step runs - the paks exist and
are readable, `pycryptodome` is importable (the pak index is AES-encrypted),
the `markers/` directory is writable - because the alternative is discovering
it 25 minutes into a full extraction.  A step that fails stops the run: the
later steps read what the earlier ones wrote, so continuing would only produce
a half-updated `markers/` that no diff can be trusted against.

THE STEP GRAPH
--------------
    items       build_items.py          -> markers/items.json
    graph       class_graph.py          -> tools/markers/class_graph.json
    categories  build_categories.py     -> markers/categories.json     [graph]
    enemies     build_enemies.py        -> markers/enemies.json        [graph]
    bosses      build_bosses.py         -> markers/bosses.json         [graph]
    npcs        build_npcs.py           -> markers/npcs.json
    extract     extract_markers.py      -> markers/chapter{1..5,dlc}.json
                                           [items categories enemies bosses npcs]
    bossdoors   build_bossdoors.py      -> markers/bossdoors.json      [extract]
    rebake      extract_markers.py      -> markers/chapter*.json       [bossdoors]
    shrines     extract_shrines.py      -> markers/shrines.json        [extract]
    recount     build_enemies.py        -> markers/enemies.json        [extract]
    verify      verify_markers.py       (reports only, --verify)

`rebake` is the second half of the bossdoors cycle and is skipped when
`bossdoors.json` came out byte-identical, which is the normal case once the
data has settled.  `recount` only refreshes the per-class marker counts inside
`enemies.json`; the names and the elite split come from the class graph and do
not depend on the chapters.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, ".."))
_MK = os.path.join(_HERE, "markers")
_OUT = os.path.join(_ROOT, "markers")

CHAPTERS = ("1", "2", "3", "4", "5", "DLC")

# name, script, argv, outputs (relative to the repo root)
STEPS: list[tuple[str, str, list[str], list[str]]] = [
    ("items", "build_items.py", [], ["markers/items.json"]),
    ("graph", "class_graph.py", ["--out", os.path.join(_MK, "class_graph.json"),
                                 "--load", os.path.join(_MK, "class_graph.json")],
     ["tools/markers/class_graph.json"]),
    ("categories", "build_categories.py", [], ["markers/categories.json"]),
    ("enemies", "build_enemies.py", [], ["markers/enemies.json"]),
    ("bosses", "build_bosses.py", ["--report"], ["markers/bosses.json"]),
    ("npcs", "build_npcs.py", [], ["markers/npcs.json"]),
    ("extract", "extract_markers.py", ["--all-chapters"],
     [f"markers/chapter{c.lower()}.json" for c in CHAPTERS]),
    ("bossdoors", "build_bossdoors.py", [], ["markers/bossdoors.json"]),
    ("rebake", "extract_markers.py", ["--all-chapters"],
     [f"markers/chapter{c.lower()}.json" for c in CHAPTERS]),
    ("shrines", "extract_shrines.py", [], ["markers/shrines.json"]),
    ("recount", "build_enemies.py", [], ["markers/enemies.json"]),
    ("verify", "verify_markers.py", [], []),
]

# Steps that take `--pak` / `--no-pak-hash`.
WANTS_PAK = {"items", "graph", "categories", "enemies", "bosses", "npcs",
             "extract", "bossdoors", "rebake", "shrines"}
WANTS_HASH_FLAG = {"categories", "enemies", "bosses", "npcs", "extract",
                   "rebake", "shrines"}
OPT_IN = {"verify"}


def digest(path: str) -> str | None:
    if not os.path.exists(path):
        return None
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:12]


def preflight(pak: str) -> list[str]:
    """Everything that must be true before the first step. Returns problems."""
    bad = []
    if sys.version_info < (3, 10):
        bad.append(f"python 3.10+ required for the `X | None` annotations "
                   f"(running {sys.version.split()[0]})")
    try:
        import Crypto.Cipher.AES                             # noqa: F401
    except ImportError:
        bad.append("pycryptodome is not importable, and the pak index is "
                   "AES-256 encrypted: pip install pycryptodome")
    if not os.path.exists(pak):
        bad.append(f"base pak not found: {pak}\n"
                   f"      point WUCHANG_PAK at the .pak or WUCHANG_GAME_ROOT at "
                   f"the install folder, or pass --pak")
    else:
        try:
            with open(pak, "rb") as f:
                f.read(4)
        except OSError as exc:
            bad.append(f"base pak is not readable: {exc}")
        for n in (0, 1):
            side = pak[:-len(".pak")] + f"_{n}_P.pak"
            if not os.path.exists(side):
                bad.append(f"patch pak missing: {os.path.basename(side)} - the "
                           f"DLC levels and the patched DT_FirePoint live in "
                           f"these, so the data would be incomplete")
    if not os.path.isdir(_OUT):
        bad.append(f"output directory missing: {_OUT}")
    elif not os.access(_OUT, os.W_OK):
        bad.append(f"output directory not writable: {_OUT}")
    for _n, script, _a, _o in STEPS:
        p = os.path.join(_MK, script)
        if not os.path.exists(p):
            bad.append(f"missing script: {p}")
    return bad


def chapter_table() -> tuple[list[str], dict]:
    """The per-chapter / per-category counts, for the summary and for a diff."""
    cats: set[str] = set()
    rows: dict[str, dict[str, int]] = {}
    for c in CHAPTERS:
        p = os.path.join(_OUT, f"chapter{c.lower()}.json")
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8") as f:
            doc = json.load(f)
        per: dict[str, int] = {}
        for m in doc.get("markers", []):
            per[m.get("cat", "?")] = per.get(m.get("cat", "?"), 0) + 1
        rows[c] = per
        cats.update(per)
    return sorted(cats), rows


def print_table(cats, rows) -> None:
    if not rows:
        return
    w = max([9] + [len(c) for c in cats])
    print("\n  markers per chapter and category")
    print("    " + "chapter".ljust(9) + "".join(c.rjust(w + 1) for c in cats)
          + "total".rjust(w + 1))
    for c in CHAPTERS:
        if c not in rows:
            continue
        per = rows[c]
        print("    " + c.ljust(9)
              + "".join(str(per.get(k, 0)).rjust(w + 1) for k in cats)
              + str(sum(per.values())).rjust(w + 1))
    print("    " + "all".ljust(9)
          + "".join(str(sum(r.get(k, 0) for r in rows.values())).rjust(w + 1)
                    for k in cats)
          + str(sum(sum(r.values()) for r in rows.values())).rjust(w + 1))


def main(argv=None) -> int:
    sys.path.insert(0, _MK)
    import pakmaps                                           # noqa: E402

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--only", action="append", default=[],
                    help="run just this step (repeatable)")
    ap.add_argument("--skip", action="append", default=[])
    ap.add_argument("--verify", action="store_true",
                    help="also score the result against the committed recon dumps")
    ap.add_argument("--no-pak-hash", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)

    if a.list:
        print(__doc__.split("THE STEP GRAPH")[1])
        return 0

    plan = [s for s in STEPS
            if (not a.only or s[0] in a.only)
            and s[0] not in a.skip
            and (s[0] not in OPT_IN or a.verify or s[0] in a.only)]
    if not plan:
        print("nothing to do")
        return 1

    bad = preflight(a.pak)
    if bad:
        print("regen_all: refusing to start -")
        for b in bad:
            print("    " + b)
        return 2

    before_cats, before_rows = chapter_table()
    results = []
    doors_before = digest(os.path.join(_OUT, "bossdoors.json"))
    t_all = time.time()

    for name, script, extra, outs in plan:
        argv2 = [sys.executable, os.path.join(_MK, script), *extra]
        if name in WANTS_PAK:
            argv2 += ["--pak", a.pak]
        if a.no_pak_hash and name in WANTS_HASH_FLAG:
            argv2 += ["--no-pak-hash"]
        if name == "rebake" and doors_before == digest(
                os.path.join(_OUT, "bossdoors.json")) and "rebake" not in a.only:
            results.append((name, "skipped", 0.0,
                            "bossdoors.json unchanged, no rebake needed"))
            continue
        print(f"\n=== {name}: {script} {' '.join(extra)}")
        if a.dry_run:
            print("    " + " ".join(argv2))
            results.append((name, "dry-run", 0.0, ""))
            continue
        t0 = time.time()
        rc = subprocess.run(argv2, cwd=_MK).returncode
        dt = time.time() - t0
        if rc != 0:
            results.append((name, f"FAILED rc={rc}", dt, ""))
            print(f"\n=== {name} failed (rc={rc}); stopping, because every later "
                  f"step reads what this one writes")
            break
        sizes = []
        for o in outs:
            p = os.path.join(_ROOT, o)
            sizes.append(f"{os.path.basename(o)} "
                         f"{os.path.getsize(p) // 1024} kB" if os.path.exists(p)
                         else f"{os.path.basename(o)} MISSING")
        results.append((name, "ok", dt, ", ".join(sizes[:3])
                        + (" ..." if len(sizes) > 3 else "")))

    print(f"\n{'=' * 72}\nregen_all summary  ({time.time() - t_all:.0f}s total)")
    print(f"  {'step':<12}{'status':<14}{'time':>8}  outputs")
    for name, status, dt, note in results:
        print(f"  {name:<12}{status:<14}{dt:>7.1f}s  {note}")

    after_cats, after_rows = chapter_table()
    print_table(after_cats, after_rows)
    if before_rows and before_rows != after_rows:
        print("\n  change vs the files that were there before this run")
        cats = sorted(set(before_cats) | set(after_cats))
        for c in CHAPTERS:
            b, f = before_rows.get(c, {}), after_rows.get(c, {})
            deltas = [f"{k} {f.get(k, 0) - b.get(k, 0):+d}" for k in cats
                      if f.get(k, 0) != b.get(k, 0)]
            if deltas:
                print(f"    {c:<9}{', '.join(deltas)}")
    elif before_rows:
        print("\n  no change in any per-chapter category count")

    return 0 if all(s == "ok" or s.startswith(("skipped", "dry-run"))
                    for _n, s, _d, _x in results) else 1


if __name__ == "__main__":
    sys.exit(main())
