#!/usr/bin/env python3
"""
repack_maps.py - re-encode an already-shipped maps/ tree into the current format.

WHY THIS EXISTS SEPARATELY FROM build_map.py
--------------------------------------------
`build_map.py` rasterises a chapter from `tools/navmesh/dumps_offline/`, which is
400 MB of extracted navmesh tiles and deliberately NOT in the repo (.gitignore says
so: reproducible from the paks in seconds, unlike the runtime probe dumps). Changing
the ENCODING of the five chapters that already ship does not need any of that: the
composite is re-palettised from its own pixels and the height planes are re-scaled
from their own codes, so this tool turns a schema-/3 tree into a /4 tree with nothing
but `maps/` on disk.

It is also the tool to reach for the next time only the format changes - a repack
takes about a minute and verifies its own output, where a rebuild depends on the
extraction step and on the island filter's thresholds staying put.

    python tools/navmesh/repack_maps.py --index-only         # only maps.json's coverage index
    python tools/navmesh/repack_maps.py --dry-run            # measure, write nothing
    python tools/navmesh/repack_maps.py --skip-heights       # composites only
    python tools/navmesh/repack_maps.py                      # in place, maps/
    python tools/navmesh/repack_maps.py --out /tmp/maps4     # leave maps/ alone

WHAT IT REWRITES
----------------
  * `<chapter>/small.png`     RGBA8 -> 256-colour palette PNG + tRNS array
  * `<chapter>/small_z*.png`  -> `<chapter>/small_h*.png`, 16-bit codes 1..65535 ->
                              1..4095 (still 16-bit grayscale PNGs; the /3 files are
                              deleted). THE FILES AND THE MANIFEST KEY ARE RENAMED ON
                              PURPOSE - see src/mapmanifest.hpp.
                              Bit 12, the reachable flag, rides through untouched: it
                              is not a number and is never re-scaled. A tree built
                              before the reachability pass carries no flag, so a repack
                              of one flags every surface - which is exactly what a
                              flagless asset means.
  * `maps/maps.json`          the coverage index (`--index-only` writes nothing else),
                              the format fields (`z_bits`, `z_code_max`, `z_step_uu`,
                              `z_quantisation`, `composite_format`), the new byte
                              counts, and the tile-store numbers the runtime pays
                              (`height_tiles_128`, `height_tile_ram_bytes`)

The `schema` string is bumped only when the HEIGHT codes are rewritten: a palette
composite is readable by any build (WIC converts an indexed frame to RGBA on the way
in), while a 12-bit plane read by a 16-bit decoder puts every surface 16x too high.

Every write is verified by decoding the bytes back before they land, so a Pillow that
dropped the tRNS array, or a plane that did not round-trip, is an error here rather
than a blank map in-game.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mapfmt  # noqa: E402  (path shim above)

try:
    import numpy as np
except ImportError:
    raise SystemExit("This script needs numpy:  pip install numpy")

TILE = 128


def _repack_composite(key: str, entry: dict, src: Path, dst: Path, dry_run: bool) -> tuple[int, int]:
    rel = entry.get("image", "")
    if not rel:
        raise SystemExit(f'{key}: no "image" in the manifest')
    src_png = src / rel
    before = src_png.stat().st_size
    rgba = mapfmt.read_rgba_png(src_png)
    data, stats = mapfmt.encode_composite_png(rgba)
    after = len(data)
    if not dry_run:
        out_png = dst / rel
        out_png.parent.mkdir(parents=True, exist_ok=True)
        out_png.write_bytes(data)
    print(
        f"  composite {rel}: {mapfmt.mb(before)} -> {mapfmt.mb(after)} "
        f"({100.0 * (after - before) / before:+.0f} %), "
        f"{stats['unique_rgba_in']} -> {stats['palette_colours']} colours, "
        f"worst channel delta {stats['max_channel_delta']}/255, "
        f"mean {stats['mean_channel_delta']:.3f}/255, alpha {stats['fill_alpha']} kept"
    )
    entry["png_bytes"] = after
    entry["composite_format"] = "png8-palette-trns"
    entry["composite_quantisation"] = {
        "unique_rgba_before": stats["unique_rgba_in"],
        "palette_colours": stats["palette_colours"],
        "max_channel_delta": stats["max_channel_delta"],
        "mean_channel_delta": round(stats["mean_channel_delta"], 4),
    }
    return before, after


def _repack_heights(key: str, entry: dict, src: Path, dst: Path, dry_run: bool) -> dict:
    z_min = float(entry.get("z_min", 0.0))
    z_max = float(entry.get("z_max", 0.0))
    if not z_max > z_min:
        raise SystemExit(f"{key}: unusable z_min/z_max ({z_min}..{z_max})")
    src_code_max = int(entry.get("z_code_max", mapfmt.Z_CODE_MAX_LEGACY))

    files, from_legacy = mapfmt.height_plane_list(entry)
    if not files:
        raise SystemExit(
            f'{key}: neither "{mapfmt.HEIGHT_KEY}" nor "{mapfmt.HEIGHT_KEY_LEGACY}" in the manifest'
        )
    # /4 renamed the plane files as well as the manifest key, so that a /3 build
    # reading a /4 tree finds nothing instead of decoding at the wrong scale. The
    # rename happens here, and the /3 files are deleted at the end - a maps/ folder
    # holding both would double the download and the first stale one would win the
    # next time somebody hand-edited the manifest.
    # `small_z0.png` and `small_h0.png` both belong to the stem `small`, so a repack of
    # a repack keeps the names it already wrote instead of stacking another suffix.
    stem = re.sub(r"_[hz]\d+$", "", Path(files[0]).stem)
    out_files = [mapfmt.height_plane_name(key, stem, k) for k in range(len(files))]

    src_step = mapfmt.z_step_uu(z_min, z_max, src_code_max)
    dst_step = mapfmt.z_step_uu(z_min, z_max)

    before = 0
    planes: list["np.ndarray"] = []
    worst_uu = 0.0
    for hrel in files:
        hp = src / hrel
        code = mapfmt.read_height_png(hp)
        before += hp.stat().st_size
        # A repack of a repack has to be a no-op, not a second rounding.
        out = code if src_code_max == mapfmt.Z_CODE_MAX else mapfmt.requantise_codes(
            code, src_code_max, mapfmt.Z_CODE_MAX
        )
        # The error this step actually introduced, in world units - measured over
        # every lit pixel rather than predicted from the step size.
        zc = mapfmt.z_codes(code)
        nz = zc != 0
        if nz.any():
            old_uu = (zc[nz].astype(np.float64) - 1.0) * src_step
            new_uu = (mapfmt.z_codes(out)[nz].astype(np.float64) - 1.0) * dst_step
            worst_uu = max(worst_uu, float(np.abs(new_uu - old_uu).max()))
        planes.append(out)

    # No flag anywhere means the source predates the reachability pass, and a flagless
    # asset means "every surface is reachable" - so say so explicitly rather than
    # shipping a /5 tree the runtime would draw as empty.
    flagged = any(bool(mapfmt.reach_mask(a).any()) for a in planes)
    if not flagged:
        planes = [mapfmt.apply_reach_bit(a, np.ones(a.shape, dtype=bool)) for a in planes]
        print("  reachability: the source carries no flag - flagging every surface")
        entry["reachability"] = {
            "enabled": False,
            "note": "repacked from an asset with no reachability pass: every surface is flagged",
        }

    sizes: list[int] = []
    for k, out in enumerate(planes):
        data = mapfmt.encode_height_png(out)
        if not dry_run:
            (dst / out_files[k]).parent.mkdir(parents=True, exist_ok=True)
            (dst / out_files[k]).write_bytes(data)
        sizes.append(len(data))
        print(
            f"    {files[k]} -> {out_files[k]}: "
            f"{mapfmt.mb((src / files[k]).stat().st_size)} -> {mapfmt.mb(sizes[-1])}"
        )

    if not dry_run and from_legacy:
        for hrel in files:
            old = dst / hrel
            if old.exists() and hrel not in out_files:
                old.unlink()

    after = sum(sizes)
    present, total = mapfmt.tile_occupancy(planes, TILE)
    ram_dense = sum(int(a.shape[0]) * int(a.shape[1]) for a in planes) * 2
    ram_tiled = present * TILE * TILE * 2

    print(
        f"  heights: {mapfmt.mb(before)} -> {mapfmt.mb(after)} "
        f"({100.0 * (after - before) / before:+.0f} %), "
        f"step {src_step:.3f} -> {dst_step:.2f} uu, worst measured shift {worst_uu:.2f} uu"
    )
    print(
        f"  RAM: dense {mapfmt.mb(ram_dense)} -> tiled {mapfmt.mb(ram_tiled)} "
        f"({present}/{total} tiles of {TILE} px = {100.0 * present / total:.1f} %)"
    )

    mapfmt.stamp_format(entry, z_min, z_max)
    _stamp_coverage(entry, planes, z_min, z_max)
    entry.pop(mapfmt.HEIGHT_KEY_LEGACY, None)
    entry[mapfmt.HEIGHT_KEY] = out_files
    entry["height_map_bytes"] = sizes
    entry["height_map_raw_bytes"] = ram_dense
    entry["height_tile_px"] = TILE
    entry["height_tiles_128"] = present
    entry["height_tiles_128_total"] = total
    entry["height_tile_ram_bytes"] = ram_tiled
    entry["z_requantise_worst_uu"] = round(worst_uu, 3)

    return {
        "heights_before": before,
        "heights_after": after,
        "ram_dense": ram_dense,
        "ram_tiled": ram_tiled,
        "tiles": present,
        "tiles_total": total,
        "worst_uu": worst_uu,
    }


def _stamp_coverage(entry: dict, planes: list["np.ndarray"], z_min: float, z_max: float) -> dict:
    """Recompute the coverage index from the planes in hand and print what it costs."""
    index = mapfmt.coverage_index(planes, z_min, z_max)
    entry[mapfmt.COVERAGE_KEY] = index
    print(
        f"  coverage: {index['tiles_x']}x{index['tiles_y']} tiles of {index['tile_px']} px, "
        f"{index['tiles_present']}/{index['tiles_total']} present, "
        f"{mapfmt.mb(index['bytes'])} raw -> {mapfmt.mb(len(index['data']))} base64, "
        f"worst tile Z span {index['z_tile_span_uu_max']:.0f} uu"
    )
    return index


def index_chapter(key: str, entry: dict, src: Path) -> dict:
    """
    --index-only: recompute the coverage index from the planes ON DISK and touch nothing
    else.

    This is how the index reaches an already-shipped tree. A full repack would re-encode
    every PNG, and the 26 MB of map PNGs are only reproducible from a 400 MB extraction
    run that is not in the repo - so the safe way to add a manifest field is to read the
    planes and write only maps.json.
    """
    print(f"\n{key}")
    z_min = float(entry.get("z_min", 0.0))
    z_max = float(entry.get("z_max", 0.0))
    if not z_max > z_min:
        raise SystemExit(f"{key}: unusable z_min/z_max ({z_min}..{z_max})")
    files, _ = mapfmt.height_plane_list(entry)
    if not files:
        raise SystemExit(f'{key}: no "{mapfmt.HEIGHT_KEY}" in the manifest')
    planes = [mapfmt.read_height_png(src / rel) for rel in files]
    index = _stamp_coverage(entry, planes, z_min, z_max)
    return {
        "key": key,
        "composite_before": int(entry.get("png_bytes", 0)),
        "composite_after": int(entry.get("png_bytes", 0)),
        "heights_before": sum(int(n) for n in entry.get("height_map_bytes", [])),
        "heights_after": sum(int(n) for n in entry.get("height_map_bytes", [])),
        "ram_dense": int(entry.get("height_map_raw_bytes", 0)),
        "ram_tiled": int(entry.get("height_tile_ram_bytes", 0)),
        "tiles": int(entry.get("height_tiles_128", 0)),
        "tiles_total": int(entry.get("height_tiles_128_total", 0)),
        "worst_uu": 0.0,
        "coverage_bytes": len(index["data"]),
    }


def repack_chapter(
    key: str,
    entry: dict,
    src: Path,
    dst: Path,
    dry_run: bool,
    do_composite: bool = True,
    do_heights: bool = True,
) -> dict:
    print(f"\n{key}")
    report: dict = {"key": key}

    if do_composite:
        cb, ca = _repack_composite(key, entry, src, dst, dry_run)
    else:
        cb = ca = int(entry.get("png_bytes", 0))
        print("  composite: left alone (--skip-composite)")
    report["composite_before"] = cb
    report["composite_after"] = ca

    if do_heights:
        report.update(_repack_heights(key, entry, src, dst, dry_run))
    else:
        hb = sum(int(n) for n in entry.get("height_map_bytes", []))
        report.update(
            heights_before=hb,
            heights_after=hb,
            ram_dense=int(entry.get("height_map_raw_bytes", 0)),
            ram_tiled=int(entry.get("height_tile_ram_bytes", 0)),
            tiles=int(entry.get("height_tiles_128", 0)),
            tiles_total=int(entry.get("height_tiles_128_total", 0)),
            worst_uu=0.0,
        )
        print("  heights: left alone (--skip-heights)")
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--maps", type=Path, default=Path("maps"), help="the maps/ tree to read (default maps)")
    ap.add_argument("--out", type=Path, default=None, help="where to write (default: in place)")
    ap.add_argument("--chapter", action="append", default=None, help="only this chapter key (repeatable)")
    ap.add_argument("--skip-composite", action="store_true", help="leave <chapter>/small.png alone")
    ap.add_argument("--skip-heights", action="store_true", help="leave the height planes (and the schema) alone")
    ap.add_argument(
        "--index-only",
        action="store_true",
        help="recompute only the coverage index and rewrite maps.json; no PNG is read for "
        "re-encoding and none is written",
    )
    ap.add_argument("--dry-run", action="store_true", help="measure and report, write nothing")
    args = ap.parse_args(argv)

    if args.index_only and (args.skip_composite or args.skip_heights or args.out):
        print("--index-only takes no --skip-* and no --out: it rewrites maps.json in place",
              file=sys.stderr)
        return 2
    if args.skip_composite and args.skip_heights:
        print("nothing to do: both --skip-composite and --skip-heights were given", file=sys.stderr)
        return 2

    src = args.maps
    dst = args.out or src
    manifest_path = src / "maps.json"
    if not manifest_path.is_file():
        print(f"{manifest_path} not found", file=sys.stderr)
        return 2

    manifest = mapfmt.load_manifest(manifest_path)
    chapters = manifest.get("chapters")
    if not isinstance(chapters, dict) or not chapters:
        print('maps.json has no "chapters" object', file=sys.stderr)
        return 2

    was = manifest.get("schema", "(none)")
    # The schema string is the version of everything a DECODER has to agree about: the
    # height codes and the coverage index. Rewriting only the composite leaves it alone
    # on purpose - an older DLL reads a palette PNG fine.
    will = was if (args.skip_heights and not args.index_only) else mapfmt.SCHEMA
    print(f"repack: {manifest_path} schema {was} -> {will}")
    if dst != src and not args.dry_run:
        dst.mkdir(parents=True, exist_ok=True)

    t0 = time.perf_counter()
    reports = []
    for key, entry in chapters.items():
        if args.chapter and key not in args.chapter:
            continue
        if args.index_only:
            reports.append(index_chapter(key, entry, src))
        else:
            reports.append(
                repack_chapter(
                    key,
                    entry,
                    src,
                    dst,
                    args.dry_run,
                    do_composite=not args.skip_composite,
                    do_heights=not args.skip_heights,
                )
            )
    if not reports:
        print("no chapter matched --chapter", file=sys.stderr)
        return 2

    manifest["schema"] = will
    if not args.dry_run:
        if dst != src:
            # A tree that is not the one we read has to be COMPLETE, or package.ps1
            # would happily zip a maps/ with four chapters in it.
            done = {r["key"] for r in reports}
            for key, entry in chapters.items():
                planes, _ = mapfmt.height_plane_list(entry)
                for rel in [entry.get("image", "")] + planes:
                    if not rel:
                        continue
                    out = dst / rel
                    if key in done and out.exists():
                        continue
                    out.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src / rel, out)
        (dst / "maps.json").write_text(mapfmt.dumps_manifest(manifest), encoding="utf-8")

    cb = sum(r["composite_before"] for r in reports)
    ca = sum(r["composite_after"] for r in reports)
    hb = sum(r["heights_before"] for r in reports)
    ha = sum(r["heights_after"] for r in reports)
    print(
        f"\nTOTAL {len(reports)} chapter(s) in {time.perf_counter() - t0:.0f} s"
        f"\n  composites {mapfmt.mb(cb)} -> {mapfmt.mb(ca)}"
        f"\n  heights    {mapfmt.mb(hb)} -> {mapfmt.mb(ha)}"
        f"\n  maps/      {mapfmt.mb(cb + hb)} -> {mapfmt.mb(ca + ha)}"
        f"\n  worst chapter RAM  dense {mapfmt.mb(max(r['ram_dense'] for r in reports))}"
        f" -> tiled {mapfmt.mb(max(r['ram_tiled'] for r in reports))}"
    )
    if args.dry_run:
        print("  (dry run: nothing was written)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
