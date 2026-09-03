#!/usr/bin/env python3
"""Inspect a cooked BP `.uasset`: its BlueprintGeneratedClass super chain.

The export map's `super` field is discarded by `uasset.Package` (the navmesh
work never needed it), so this tool re-reads the export map itself. That is
enough to answer "what is the common base class of the game's ~50 `*_NPC_C`
blueprints?" entirely offline -- the runtime class table in `src/markers.cpp`
matches by name up the super chain, so ONE base class replaces fifty entries.

    python inspect_classes.py --find NPC
    python inspect_classes.py --super Content/.../BP_NPC_cunmin.uasset
"""

from __future__ import annotations

import argparse
import struct
import sys

import pakmaps


def export_supers(pkg) -> list[tuple[str, str, str]]:
    """[(export name, class name, super name)] for every export."""
    r = pakmaps.uasset.R(pkg.head)
    r.u32(); r.i32(); r.i32(); r.i32(); r.i32(); r.i32()
    for _ in range(r.i32()):
        r.raw(16); r.i32()
    r.i32()                       # total header size
    r.fstring()                   # folder name
    r.u32()                       # package flags
    r.i32(); r.i32()              # names
    r.i32(); r.i32()              # soft object paths
    r.i32(); r.i32()              # gatherable text
    export_count, export_off = r.i32(), r.i32()
    out = []
    e = pakmaps.uasset.R(pkg.head, export_off)
    for _ in range(export_count):
        cls = e.i32(); sup = e.i32(); e.i32()
        e.i32()                   # outer
        name = pkg._fname(e)
        e.u32(); e.i64(); e.i64()
        e.raw(96 - 44)
        out.append((name, pkg._resolve(cls), pkg._resolve(sup)))
    return out


def read_pkg(src: pakmaps.MapSource, key: str):
    head = src.read(key)
    uexp_key = key.rsplit(".", 1)[0] + ".uexp"
    uexp = src.read(uexp_key) if uexp_key in src.owner else b""
    return pakmaps._package_from_bytes(key, head, uexp)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--find", default="", help="substring of the asset path")
    ap.add_argument("--suffix", default=".uasset")
    ap.add_argument("--super", dest="one", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--names", default="", help="also dump name-map entries matching this")
    a = ap.parse_args(argv)

    src = pakmaps.MapSource(a.pak)
    if a.one:
        keys = [a.one]
    else:
        keys = sorted(k for k in src.owner
                      if k.endswith(a.suffix) and a.find.lower() in k.lower())
        if a.limit:
            keys = keys[: a.limit]
    print(f"{len(keys)} package(s)")
    for k in keys:
        try:
            pkg = read_pkg(src, k)
            rows = export_supers(pkg)
        except Exception as exc:                      # noqa: BLE001
            print(f"  {k}: FAILED {exc}")
            continue
        for name, cls, sup in rows:
            if cls in ("BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass",
                       "AnimBlueprintGeneratedClass", "Class"):
                print(f"  {k}\n      {name} : {cls}  super={sup}")
        if a.names:
            hits = [n for n in pkg.names if a.names.lower() in n.lower()]
            if hits:
                print(f"      names~{a.names}: {hits[:40]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
