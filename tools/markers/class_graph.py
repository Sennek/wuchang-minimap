#!/usr/bin/env python3
"""Build the game's blueprint class graph (class -> super) from the paks.

Every cooked `.uasset` that defines a blueprint carries a
`BlueprintGeneratedClass` export whose `super` field names its parent -- and
`super` is in the export map, which needs no `.usmap`. Sweeping all 81 k
`.uasset` entries therefore yields the whole BP inheritance graph offline.

Why it matters: `src/markers.cpp`'s class table matches by NAME up the super
chain, so one base class entry covers every subclass. The game has ~50
`*_NPC_C` blueprints and they all derive from `BP_NPC_C`; without the graph
that is a guess.

    python class_graph.py --out class_graph.json
    python class_graph.py --out class_graph.json --children BP_NPC_C
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

import pakmaps
import inspect_classes

BGC = ("BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass",
       "AnimBlueprintGeneratedClass")


def build(src: pakmaps.MapSource, verbose: bool = True) -> dict:
    keys = sorted(k for k in src.owner if k.endswith(".uasset"))
    graph: dict[str, str] = {}
    where: dict[str, str] = {}
    t0 = time.time()
    for i, k in enumerate(keys):
        if verbose and i % 5000 == 0:
            print(f"  {i}/{len(keys)}  {time.time() - t0:.0f}s  {len(graph)} classes",
                  file=sys.stderr)
        try:
            head = src.read(k)
            pkg = pakmaps._package_from_bytes(k, head, b"")
            rows = inspect_classes.export_supers(pkg)
        except Exception:                                   # noqa: BLE001
            continue
        for name, cls, sup in rows:
            if cls in BGC and name not in graph:
                graph[name] = sup
                where[name] = k
    return {"class_super": graph, "asset": where}


# The whole-game sweep takes ~40 s and its answer only changes when the game
# does, so it is cached beside this file and committed: every other tool in the
# pipeline needs the graph, and none of them should pay for it or require the
# paks to be mounted just to read a class' parent.
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "class_graph.json")


def load_or_build(src=None, path: str = CACHE, verbose: bool = True) -> dict:
    """`{class: super}` from the cache, building and saving it if it is absent.

    Returns the bare `class_super` mapping, which is what most callers want;
    `load_assets()` is the other half of the same document.
    """
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        if verbose:
            print(f"  class graph: {len(doc['class_super'])} classes from "
                  f"{os.path.basename(path)}")
        return doc["class_super"]
    if src is None:
        raise SystemExit(f"{path} is missing and no pak source was given - run "
                         f"`python class_graph.py --out {path}`")
    doc = build(src, verbose)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
    if verbose:
        print(f"  class graph: built and cached {len(doc['class_super'])} classes")
    return doc["class_super"]


def load_assets(path: str = CACHE) -> dict[str, str]:
    """`{class: the .uasset key that defines it}` from the cached document.

    The sweep already recorded where every class lives, so a caller that needs
    a class' own package - to read its `Default__<class>` object, say - asks
    here instead of searching the paks for it again.  Empty when the cache is
    absent; the caller then has no CDO to read and falls back on its own.
    """
    if path not in _ASSETS:
        if not os.path.exists(path):
            return {}
        with open(path, encoding="utf-8") as f:
            _ASSETS[path] = json.load(f).get("asset", {})
    return _ASSETS[path]


_ASSETS: dict[str, dict[str, str]] = {}


def chain(graph: dict, name: str, cap: int = 32) -> list[str]:
    out = [name]
    seen = {name}
    for _ in range(cap):
        nxt = graph.get(out[-1])
        if not nxt or nxt in seen:
            break
        out.append(nxt)
        seen.add(nxt)
    return out


def descendants(graph: dict, root: str) -> list[str]:
    return sorted(c for c in graph if root in chain(graph, c)[1:])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default="")
    ap.add_argument("--load", default="")
    ap.add_argument("--children", default="", help="list descendants of this class")
    ap.add_argument("--chain", default="", help="print one class' super chain")
    a = ap.parse_args(argv)

    if a.load and os.path.exists(a.load):
        doc = json.load(open(a.load, encoding="utf-8"))
    else:
        doc = build(pakmaps.MapSource(a.pak))
        if a.out:
            with open(a.out, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=1, sort_keys=True)
            print(f"wrote {a.out}: {len(doc['class_super'])} classes")
    g = doc["class_super"]
    if a.chain:
        print(" -> ".join(chain(g, a.chain)))
    if a.children:
        kids = descendants(g, a.children)
        print(f"{len(kids)} descendant(s) of {a.children}")
        for c in kids:
            print("  " + c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
