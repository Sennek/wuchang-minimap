"""
mapfmt - the ON-DISK format of the shipped map assets, in one place.

Both producers import this: `build_map.py` (which rasterises a chapter from the
navmesh dumps) and `repack_maps.py` (which re-encodes an already-shipped chapter
without needing the dumps). The C++ side that reads it is `src/mapmanifest.hpp` +
`src/mapdata.cpp`; the manifest's `schema` string is the contract between them and
lives here as `SCHEMA`.

WHY THE FORMAT CHANGED (schema /3 -> /4, 2026-09-04)
----------------------------------------------------
`maps/` was 43.1 MB of PNG for five chapters and the height planes cost ~350 MB of
RAM per resident chapter. Two encoding changes take that to 27.4 MB with no visible
loss, and the tile store in `src/mapdata.cpp` takes the RAM to ~90 MB:

  * THE COMPOSITE IS A 256-COLOUR PALETTE PNG. It is a flat-filled render of a
    5-stop grey ramp with a darkened outline per polygon, so its whole gamut is two
    1-D curves through RGB - 643..651 distinct RGBA values per chapter, and alpha is
    binary (0 for the background, 235 for the fill). Palette index 0 is the
    transparent background and 1..255 are the quantised fill colours, with a tRNS
    ARRAY carrying alpha per index so 235 survives exactly. Median cut over the
    unique colours weighted by pixel count lands within 3/255 per channel worst case
    and 0.19/255 on average: -44 % of the bytes (11.03 -> 6.17 MB) for an error that
    is a quarter of a JPEG quantisation step on an asset that is off by default
    (`fallback_use_composite = 0`).

  * THE HEIGHT PLANES ARE 12-BIT. The code is still one 16-bit grayscale sample per
    pixel - the WIC decode path and the `0 = no surface` sentinel are unchanged - but
    only 1..4095 are used, which is what the deflate stream is paid for: -35 %
    (32.07 -> 21.23 MB). The number that matters is the resulting Z step, and it has
    to stay far below the smallest Z distance the runtime cares about:

        chapter    Z span      16-bit step   12-bit step   worst error
        1          50 520 uu   0.771 uu      12.34 uu      +/- 6.17 uu
        2          50 076 uu   0.764 uu      12.23 uu      +/- 6.12 uu
        3          50 920 uu   0.777 uu      12.44 uu      +/- 6.22 uu
        4          29 752 uu   0.454 uu       7.26 uu      +/- 3.63 uu
        5          16 252 uu   0.248 uu       3.98 uu      +/- 1.99 uu

    The slicer's floor tolerance is `floor_z_tolerance` = 200 uu and its fade is
    800 uu, and the pipeline's own storey separator (`--floor-band-gap`) is 250 uu.
    The worst 12-bit error is 3.1 % of the tolerance, 0.8 % of the fade and 2.5 % of
    a storey gap - i.e. a pixel can never change which of the three classes it lands
    in unless it was already within 6 uu of the boundary, where the class was a coin
    flip anyway. 10-bit (49 uu, 25 % of the tolerance) would start to matter; 12 is
    the last power-of-two step that is unarguably free.

A NEW DLL MUST REFUSE AN OLD MANIFEST AND VICE VERSA, because the two differ only in
how a number is scaled: a /3 plane read with a /4 decoder puts every surface 16x too
high, which looks like a map that is simply empty rather than like a version error.
So `SCHEMA` is compared for exact equality on both sides and a mismatch is fatal -
not, as before, a warning followed by reading it anyway.
"""

from __future__ import annotations

import io
import json
from pathlib import Path

try:
    import numpy as np
except ImportError:  # pragma: no cover - same guard as build_map.py
    raise SystemExit("This module needs numpy:  pip install numpy")

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    raise SystemExit("This module needs Pillow:  pip install pillow")

Image.MAX_IMAGE_PIXELS = None

# The manifest contract with src/mapmanifest.hpp. Bump BOTH or neither.
SCHEMA = "wuchang-minimap-maps/4"

# Height codes. 0 means "no surface here"; 1..Z_CODE_MAX are heights, so the number
# of distinct heights is Z_CODE_MAX - 1 and the step is span / (Z_CODE_MAX - 1).
Z_BITS = 12
Z_CODE_MAX = (1 << Z_BITS) - 1  # 4095
# What schema /3 shipped, needed to requantise an already-built plane.
Z_CODE_MAX_LEGACY = 65535

# Composite palette: index 0 is the transparent background, 1..255 the fill colours.
PALETTE_SIZE = 256
FILL_ALPHA = 235  # must match build_map.FILL_ALPHA

# The manifest key and the file naming for the height planes. BOTH changed with /4,
# and not for tidiness: 1.0.0 is released and its parser cannot be taught to refuse a
# /4 tree, so /4 has to be a tree it cannot find. A /3 build reading a /4 manifest
# sees no `height_maps`, guesses the `_z` names it used to write, finds nothing on
# disk and logs "NO height plane decoded ... build them with build_map.py" - which is
# a loud, accurate failure instead of a map drawn 16x too high. See the version
# discussion in src/mapmanifest.hpp.
HEIGHT_KEY = "height_planes"
HEIGHT_KEY_LEGACY = "height_maps"


def height_plane_name(chapter_key: str, stem: str, k: int) -> str:
    return f"{chapter_key}/{stem}_h{k}.png"


def height_plane_list(entry: dict) -> tuple[list[str], bool]:
    """(the plane list, whether it came from the /3 key) for a manifest entry."""
    planes = entry.get(HEIGHT_KEY)
    if isinstance(planes, list) and planes:
        return [str(x) for x in planes], False
    legacy = entry.get(HEIGHT_KEY_LEGACY)
    if isinstance(legacy, list) and legacy:
        return [str(x) for x in legacy], True
    return [], False


def z_step_uu(z_min: float, z_max: float, code_max: int = Z_CODE_MAX) -> float:
    """uu per height code - the number `HeightMaps::z_step()` recomputes at runtime."""
    span = float(z_max) - float(z_min)
    if span <= 0.0 or code_max < 2:
        return 0.0
    return span / float(code_max - 1)


def z_quantisation_text(code_max: int = Z_CODE_MAX) -> str:
    return (
        f"code = 1 + round((Z - z_min) / (z_max - z_min) * {code_max - 1}) ; 0 = no surface"
    )


# =================================================================================
# height planes
# =================================================================================


def quantise_z(z: "np.ndarray", z_min: float, z_max: float) -> "np.ndarray":
    """Float Z (NaN = no surface) -> uint16 codes in 0..Z_CODE_MAX."""
    span = z_max - z_min if z_max > z_min else 1.0
    code = np.zeros(z.shape, dtype=np.uint16)
    m = ~np.isnan(z)
    if m.any():
        v = 1.0 + np.rint((z[m].astype(np.float64) - z_min) / span * (Z_CODE_MAX - 1))
        code[m] = np.clip(v, 1.0, float(Z_CODE_MAX)).astype(np.uint16)
    return code


def requantise_codes(code: "np.ndarray", src_code_max: int, dst_code_max: int) -> "np.ndarray":
    """
    Re-scale already-quantised codes, preserving the 0 sentinel exactly.

    This is what lets the shipped 16-bit planes be repacked to 12-bit without the
    navmesh dumps: the extra error is one dst step, and the src error it inherits
    (0.25..0.8 uu) is two orders of magnitude smaller than that.
    """
    out = np.zeros(code.shape, dtype=np.uint16)
    nz = code != 0
    if nz.any():
        t = (code[nz].astype(np.float64) - 1.0) / float(src_code_max - 1)
        out[nz] = np.clip(1.0 + np.rint(t * (dst_code_max - 1)), 1.0, float(dst_code_max)).astype(
            np.uint16
        )
    return out


def encode_height_png(code: "np.ndarray") -> bytes:
    """One height plane -> 16-bit grayscale PNG bytes, verified bit-exact."""
    if code.dtype != np.uint16:
        raise ValueError(f"height codes must be uint16, got {code.dtype}")
    if int(code.max(initial=0)) > Z_CODE_MAX:
        raise ValueError(f"height code {int(code.max())} exceeds Z_CODE_MAX {Z_CODE_MAX}")
    buf = io.BytesIO()
    Image.frombytes(
        "I;16", (code.shape[1], code.shape[0]), code.astype("<u2").tobytes()
    ).save(buf, format="PNG", optimize=True)
    data = buf.getvalue()
    back = np.asarray(Image.open(io.BytesIO(data)))
    if back.shape != code.shape or not np.array_equal(
        back.astype(np.int64), code.astype(np.int64)
    ):
        raise RuntimeError("16-bit PNG did not round-trip")
    return data


def write_height_png(code: "np.ndarray", path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = encode_height_png(code)
    path.write_bytes(data)
    return len(data)


def read_height_png(path: Path) -> "np.ndarray":
    return np.asarray(Image.open(path)).astype(np.uint16)


# =================================================================================
# the composite, as a 256-colour palette PNG
# =================================================================================


def _median_cut(colors: "np.ndarray", counts: "np.ndarray", want: int) -> "np.ndarray":
    """
    Weighted median cut over a SMALL set of exact colours (~650 here, never a whole
    image), so this can afford to be plain numpy and to split on total weight rather
    than on box population.
    """
    boxes = [np.arange(len(colors))]
    while len(boxes) < want:
        best = -1.0
        bi = -1
        axis = 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            c = colors[b]
            extent = c.max(axis=0) - c.min(axis=0)
            a = int(np.argmax(extent))
            score = float(extent[a]) * float(counts[b].sum())
            if score > best:
                best = score
                bi = i
                axis = a
        if bi < 0:
            break  # every box is a single colour: fewer than `want` colours exist
        b = boxes.pop(bi)
        order = b[np.argsort(colors[b][:, axis], kind="stable")]
        w = counts[order].cumsum()
        k = int(np.searchsorted(w, w[-1] / 2.0)) + 1
        k = max(1, min(len(order) - 1, k))
        boxes.append(order[:k])
        boxes.append(order[k:])

    pal = []
    for b in boxes:
        w = counts[b].astype(np.float64)
        pal.append(np.rint((colors[b].astype(np.float64) * w[:, None]).sum(axis=0) / w.sum()))
    return np.array(pal, dtype=np.int32)


def encode_composite_png(rgba: "np.ndarray") -> tuple[bytes, dict]:
    """
    RGBA8 array -> palette PNG bytes plus a stats dict.

    Every pixel whose alpha is 0 becomes palette index 0 (the transparent
    background); the rest are quantised to at most 255 colours. Alpha is carried by a
    tRNS array, so a fill alpha of 235 stays 235 - a single-index tRNS could only
    express 0/255 and would make the whole map opaque.
    """
    if rgba.ndim != 3 or rgba.shape[2] != 4 or rgba.dtype != np.uint8:
        raise ValueError("expected an (h, w, 4) uint8 RGBA array")
    h, w, _ = rgba.shape
    alpha = rgba[:, :, 3]
    opaque = alpha != 0
    alphas = np.unique(alpha[opaque]) if opaque.any() else np.array([], dtype=np.uint8)
    if alphas.size > 1:
        raise ValueError(
            f"the composite has {alphas.size} non-zero alpha values {alphas.tolist()}; "
            "the palette encoder assumes binary alpha"
        )
    fill_alpha = int(alphas[0]) if alphas.size else FILL_ALPHA

    rgb = rgba[:, :, :3]
    packed = (
        (rgb[:, :, 0].astype(np.uint32) << 16)
        | (rgb[:, :, 1].astype(np.uint32) << 8)
        | rgb[:, :, 2].astype(np.uint32)
    )
    uniq, inverse, counts = np.unique(packed[opaque], return_inverse=True, return_counts=True)
    cols = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1).astype(np.int32)

    want = PALETTE_SIZE - 1
    pal = _median_cut(cols, counts, want) if len(cols) > want else cols.copy()
    d = ((cols[:, None, :] - pal[None, :, :]) ** 2).sum(axis=2)
    nearest = np.argmin(d, axis=1)
    per_colour = np.abs(cols - pal[nearest]).max(axis=1)

    idx = np.zeros((h, w), dtype=np.uint8)
    idx[opaque] = (nearest[inverse] + 1).astype(np.uint8)

    palette = np.zeros((PALETTE_SIZE, 3), dtype=np.uint8)
    palette[1 : 1 + len(pal)] = pal.astype(np.uint8)
    trns = bytes([0] + [fill_alpha] * (PALETTE_SIZE - 1))

    im = Image.fromarray(idx)
    im = im.convert("P") if im.mode != "P" else im
    im.putpalette(palette.reshape(-1).tolist())
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True, transparency=trns)
    data = buf.getvalue()

    # Prove it, rather than trusting Pillow's tRNS handling: decode the bytes we are
    # about to ship and compare against the source. A single-index tRNS (or a lost
    # one) shows up here as an alpha mismatch on every pixel.
    back = np.array(Image.open(io.BytesIO(data)).convert("RGBA"))
    if back.shape != rgba.shape:
        raise RuntimeError("palette PNG changed the image size")
    if not np.array_equal(back[:, :, 3], alpha):
        raise RuntimeError("palette PNG lost the alpha channel (tRNS array not written?)")
    rt_max = int(np.abs(back[:, :, :3].astype(np.int32) - rgb.astype(np.int32)).max())

    stats = {
        "unique_rgba_in": int(len(cols) + (1 if (~opaque).any() else 0)),
        "palette_colours": int(len(pal) + 1),
        "fill_alpha": fill_alpha,
        "max_channel_delta": int(per_colour.max(initial=0)),
        "mean_channel_delta": float((per_colour * counts).sum() / max(1, counts.sum())),
        "roundtrip_max_channel_delta": rt_max,
    }
    return data, stats


def write_composite_png(rgba: "np.ndarray", path: Path) -> tuple[int, dict]:
    path.parent.mkdir(parents=True, exist_ok=True)
    data, stats = encode_composite_png(rgba)
    path.write_bytes(data)
    return len(data), stats


def read_rgba_png(path: Path) -> "np.ndarray":
    return np.array(Image.open(path).convert("RGBA"))


# =================================================================================
# the manifest
# =================================================================================


def stamp_format(entry: dict, z_min: float, z_max: float) -> dict:
    """Write the format fields src/mapmanifest.hpp reads into one chapter entry."""
    entry["z_bits"] = Z_BITS
    entry["z_code_max"] = Z_CODE_MAX
    entry["z_code_no_surface"] = 0
    entry["z_step_uu"] = z_step_uu(z_min, z_max)
    entry["z_quantisation"] = z_quantisation_text()
    entry["composite_format"] = "png8-palette-trns"
    return entry


def load_manifest(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def dumps_manifest(manifest: dict) -> str:
    """
    `json.dumps(indent=1)` plus a trailing newline - byte-for-byte the layout
    build_map.py already wrote, so a repack shows up in `git diff` as the fields that
    changed and nothing else. (build_map.py keeps its own wrapper for the
    `--legacy-layers` `floor_grid` rows, which have to stay one row per line; the
    shipped manifest has none.)
    """
    return json.dumps(manifest, indent=1) + "\n"


def tile_occupancy(planes: list["np.ndarray"], tile: int = 128) -> tuple[int, int]:
    """
    (non-empty tiles, total tiles) over a chapter's planes at `tile` px.

    This is the number `src/mapdata.cpp`'s sparse store pays for, so the pipeline
    reports it next to the PNG sizes: RAM = non_empty * tile * tile * 2 B.
    """
    present = 0
    total = 0
    for a in planes:
        h, w = a.shape
        nty = (h + tile - 1) // tile
        ntx = (w + tile - 1) // tile
        pad = np.zeros((nty * tile, ntx * tile), dtype=bool)
        pad[:h, :w] = a != 0
        blocks = pad.reshape(nty, tile, ntx, tile).any(axis=(1, 3))
        present += int(blocks.sum())
        total += nty * ntx
    return present, total


def mb(n: float) -> str:
    return f"{n / (1024.0 * 1024.0):.2f} MB"


def ceil_div(a: int, b: int) -> int:
    return -(-a // b) if b else 0


__all__ = [
    "SCHEMA",
    "Z_BITS",
    "Z_CODE_MAX",
    "Z_CODE_MAX_LEGACY",
    "PALETTE_SIZE",
    "FILL_ALPHA",
    "z_step_uu",
    "z_quantisation_text",
    "quantise_z",
    "requantise_codes",
    "encode_height_png",
    "write_height_png",
    "read_height_png",
    "encode_composite_png",
    "write_composite_png",
    "read_rgba_png",
    "stamp_format",
    "HEIGHT_KEY",
    "HEIGHT_KEY_LEGACY",
    "height_plane_name",
    "height_plane_list",
    "load_manifest",
    "dumps_manifest",
    "tile_occupancy",
    "mb",
    "ceil_div",
]
