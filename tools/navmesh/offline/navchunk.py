#!/usr/bin/env python3
"""
Offline navmesh extraction: `RecastNavMeshDataChunk` exports -> tile JSON.

Reads a cooked World-Composition cell package (`B<N>EX0_L0_X<x>_Y<y>_DL0_WP.umap`
+ `.uexp`) that has already been unpacked from the paks (see `pak.py`), finds its
four `RecastNavMeshDataChunk` exports -- one per navmesh agent -- and decodes the
Detour tiles out of the raw blob, emitting the same JSON the runtime C++ dumper
writes (schema `wuchang-navmesh-tiles/1`) so `../render.py` can draw it.

Blob format, reverse-engineered on Wuchang 1.7 / UE 5.1.1 (offsets are relative
to the start of the export's serialized data; see `context/navmesh-offline.md`):

    +0    uint16  unversioned-property header (0x0300: 1 value, is-last)
    +2    FName   UNavigationDataChunk::NavigationDataName  ->  "RecastNavMesh-<agent>"
    +10   int32   0
    +14   int32   NavMeshVersion (1021 on this build)
    +18   int32   byte count from here to the end of the export
    +22   int32   0
    +26   int32   tile count
    +30   ...     per-tile records, back to back

Each tile record (a serialized `dtMeshHeader` + arrays, `dtReal` = double):

    H+0   uint16  0x006d  (constant marker / version)
    H+2   int32   tile x        (bmin[0] == x * TileSizeUU)
    H+6   int32   tile y        (bmin[2] == y * TileSizeUU)
    H+10  uint16  layer
    H+12  uint16  polyCount          H+14 uint16 vertCount
    H+16  uint16  maxLinkCount       H+18 uint16 detailMeshCount
    H+20  uint16  detailVertCount    H+22 uint16 detailTriCount
    H+24  uint16  bvNodeCount        H+26 uint16 offMeshConCount
    H+28  uint16  offMeshBase (== polyCount when there are no off-mesh links)
    H+30  double  bmin[3]            H+54 double  bmax[3]
    H+78  int32   (link/cluster count)
    H+82  uint16  polyCount (repeat) H+84 uint16 vertCount (repeat)
    H+86  double  verts[3 * vertCount]
    then          dtPoly[polyCount], stride 34:
                    +0 uint32 firstLink · +4 uint16 verts[6] · +16 uint16 neis[6]
                    +28 uint16 flags · +30 uint16 (unused) · +32 uint8 vertCount
                    +33 uint8 areaAndtype  (0x3f = RC_WALKABLE_AREA, type ground)

Coordinates are Recast space; Unreal = (-r.x, -r.z, r.y).

    python navchunk.py <cell.umap> [<cell.umap> ...] --out DIR
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uasset import Package  # noqa: E402

TILE_SIZE_UU = 1280.0
CELL_UU = 12800.0
HDR_MARKER = 0x006D
POLY_STRIDE = 34
DT_VERTS_PER_POLYGON = 6

AGENT_SETTINGS = {  # from context/wuchang-classes.md section 4
    "Small": {"AgentRadius": 34.0, "AgentHeight": 120.0},
    "Big": {"AgentRadius": 60.0, "AgentHeight": 188.0},
    "BitFat": {"AgentRadius": 90.0, "AgentHeight": 188.0},
    "Giant": {"AgentRadius": 120.0, "AgentHeight": 200.0},
}


def recast_to_unreal(x: float, y: float, z: float) -> tuple[float, float, float]:
    """Recast (x = -UnrealX, y = UnrealZ, z = -UnrealY) -> Unreal (X, Y, Z)."""
    return (-x, -z, y)


class Tile:
    __slots__ = ("x", "y", "layer", "bmin", "bmax", "verts", "polys", "counts",
                 "offset")


def parse_chunk(blob: bytes, verbose: bool = False) -> tuple[dict, list[Tile]]:
    """Decode one RecastNavMeshDataChunk export payload."""
    n = len(blob)
    head = {
        "prop_header": struct.unpack_from("<H", blob, 0)[0],
        "name_index": struct.unpack_from("<i", blob, 2)[0],
        "name_number": struct.unpack_from("<i", blob, 6)[0],
        "navmesh_version": struct.unpack_from("<i", blob, 14)[0],
        "payload_bytes": struct.unpack_from("<i", blob, 18)[0],
        "tile_count_field": struct.unpack_from("<i", blob, 26)[0],
    }
    tiles: list[Tile] = []
    seen: set[int] = set()
    H = 0
    while H < n - 96:
        if struct.unpack_from("<H", blob, H)[0] != HDR_MARKER:
            H += 1
            continue
        t = _try_tile(blob, H)
        if t is None:
            H += 1
            continue
        key = H
        if key not in seen:
            seen.add(key)
            tiles.append(t)
        # a valid record cannot overlap another header start
        H += 86 + 24 * len(t.verts) + POLY_STRIDE * len(t.polys)
    return head, tiles


def _try_tile(blob: bytes, H: int) -> Tile | None:
    n = len(blob)
    if H + 86 > n:
        return None
    x, y = struct.unpack_from("<ii", blob, H + 2)
    if not (-4096 < x < 4096 and -4096 < y < 4096):
        return None
    (layer, poly_count, vert_count, max_link, detail_mesh, detail_vert,
     detail_tri, bv_node, offmesh_con) = struct.unpack_from("<9H", blob, H + 10)
    offmesh_base = struct.unpack_from("<H", blob, H + 28)[0]
    if poly_count == 0 or vert_count == 0 or layer > 255:
        return None
    if detail_mesh != poly_count or offmesh_base != poly_count:
        return None
    if bv_node > 2 * poly_count + 1:
        return None
    try:
        bmin = struct.unpack_from("<3d", blob, H + 30)
        bmax = struct.unpack_from("<3d", blob, H + 54)
        pc2, vc2 = struct.unpack_from("<2H", blob, H + 82)
    except struct.error:
        return None
    if (pc2, vc2) != (poly_count, vert_count):
        return None
    if abs(bmin[0] - x * TILE_SIZE_UU) > 1e-6 or abs(bmin[2] - y * TILE_SIZE_UU) > 1e-6:
        return None
    if abs((bmax[0] - bmin[0]) - TILE_SIZE_UU) > 1e-6:
        return None
    if abs((bmax[2] - bmin[2]) - TILE_SIZE_UU) > 1e-6:
        return None
    if not (bmin[1] <= bmax[1]):
        return None

    vo = H + 86
    po = vo + 24 * vert_count
    if po + POLY_STRIDE * poly_count > n:
        return None

    raw = struct.unpack_from("<%dd" % (3 * vert_count), blob, vo)
    verts = []
    for i in range(vert_count):
        rx, ry, rz = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        if not (bmin[0] - 1.0 <= rx <= bmax[0] + 1.0):
            return None
        if not (bmin[2] - 1.0 <= rz <= bmax[2] + 1.0):
            return None
        if not (bmin[1] - 1e4 <= ry <= bmax[1] + 1e4):
            return None
        verts.append(list(recast_to_unreal(rx, ry, rz)))

    polys = []
    for i in range(poly_count):
        o = po + POLY_STRIDE * i
        idx = struct.unpack_from("<6H", blob, o + 4)
        flags = struct.unpack_from("<H", blob, o + 28)[0]
        nv = blob[o + 32]
        area_type = blob[o + 33]
        if not (3 <= nv <= DT_VERTS_PER_POLYGON):
            return None
        v = list(idx[:nv])
        if any(k >= vert_count for k in v):
            return None
        polys.append({"v": v, "area": area_type & 0x3F, "type": area_type >> 6,
                      "flags": flags})

    t = Tile()
    t.x, t.y, t.layer, t.offset = x, y, layer, H
    ux0, uy0, uz0 = recast_to_unreal(*bmin)
    ux1, uy1, uz1 = recast_to_unreal(*bmax)
    t.bmin = [min(ux0, ux1), min(uy0, uy1), min(uz0, uz1)]
    t.bmax = [max(ux0, ux1), max(uy0, uy1), max(uz0, uz1)]
    t.verts, t.polys = verts, polys
    t.counts = {"maxLinkCount": max_link, "detailMeshCount": detail_mesh,
                "detailVertCount": detail_vert, "detailTriCount": detail_tri,
                "bvNodeCount": bv_node, "offMeshConCount": offmesh_con}
    return t


def cell_of(path: str) -> tuple[str, int | None, int | None]:
    stem = os.path.splitext(os.path.basename(path))[0]
    cx = cy = None
    for part in stem.split("_"):
        if part.startswith("X") and part[1:].lstrip("-").isdigit():
            cx = int(part[1:])
        elif part.startswith("Y") and part[1:].lstrip("-").isdigit():
            cy = int(part[1:])
    return stem, cx, cy


def process(umap: str, out_root: str, stamp: str, verbose: bool = True) -> list[dict]:
    pkg = Package(umap)
    stem, cx, cy = cell_of(umap)
    reports = []
    for ex in pkg.exports:
        if ex.class_name != "RecastNavMeshDataChunk":
            continue
        blob = pkg.data(ex)
        head, tiles = parse_chunk(blob)
        actor = pkg.names[head["name_index"]] if 0 <= head["name_index"] < len(pkg.names) else "?"
        agent = actor.split("-")[-1] if "-" in actor else actor
        doc = {
            "schema": "wuchang-navmesh-tiles/1",
            "generated": stamp,
            "agent": agent,
            "primary_agent": agent == "Small",
            "actor": actor,
            "actor_full_name": f"OFFLINE {stem}:PersistentLevel.{ex.name}",
            "actor_address": "0x0",
            "source": {
                "kind": "offline-pak",
                "package": pkg.folder_name,
                "export": ex.name,
                "cell": [cx, cy],
                "uexp_offset": ex.uexp_offset,
                "uexp_size": ex.serial_size,
                "navmesh_version": head["navmesh_version"],
                "tile_count_field": head["tile_count_field"],
            },
            "settings": dict(
                AGENT_SETTINGS.get(agent, {}),
                TileSizeUU=TILE_SIZE_UU, CellSize=10.0, CellHeight=40.0,
                TilePoolSize=4096, PolyRefTileBits=23, PolyRefNavPolyBits=32,
                PolyRefSaltBits=9,
            ),
            "dtnavmesh": {
                "orig": [0.0, 0.0, 0.0],
                "tile_width": TILE_SIZE_UU,
                "tile_height": TILE_SIZE_UU,
                "max_tiles": 4096,
                "max_polys": 4194304,
            },
            "offsets": {"impl_ptr": "0x0", "params": "0x0", "tiles_ptr": "0x0",
                        "tile_stride": "0x22"},
            "tile_count": len(tiles),
            "poly_count": sum(len(t.polys) for t in tiles),
            "vert_count": sum(len(t.verts) for t in tiles),
            "tiles": [
                {"x": t.x, "y": t.y, "layer": t.layer, "bmin": t.bmin,
                 "bmax": t.bmax, "verts": t.verts, "polys": t.polys}
                for t in tiles
            ],
        }
        agent_dir = os.path.join(out_root, agent)
        os.makedirs(agent_dir, exist_ok=True)
        dst = os.path.join(agent_dir, f"tiles_{stamp}_{stem}.json")
        with open(dst, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, separators=(",", ":"))
        rep = {
            "cell": stem, "agent": agent, "tiles": len(tiles),
            "tile_count_field": head["tile_count_field"],
            "polys": doc["poly_count"], "verts": doc["vert_count"],
            "json": dst, "bytes": os.path.getsize(dst),
        }
        reports.append(rep)
        if verbose:
            print(f"  {stem:<26} {agent:<7} tiles {len(tiles):>4}/"
                  f"{head['tile_count_field']:<4} polys {doc['poly_count']:>7} "
                  f"verts {doc['vert_count']:>7}  -> {os.path.basename(dst)}")
    return reports


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("umaps", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stamp", default=time.strftime("%Y%m%d_%H%M%S"))
    a = ap.parse_args(argv)
    paths: list[str] = []
    for pat in a.umaps:
        paths.extend(sorted(glob.glob(pat)) or [pat])
    all_reports = []
    t0 = time.time()
    for p in paths:
        print(os.path.basename(p))
        all_reports.extend(process(p, a.out, a.stamp))
    dt = time.time() - t0
    print(f"\n{len(paths)} package(s), {len(all_reports)} chunk(s) in {dt:.2f}s")
    tot_t = sum(r["tiles"] for r in all_reports)
    tot_p = sum(r["polys"] for r in all_reports)
    print(f"total: {tot_t} tiles, {tot_p} polys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
