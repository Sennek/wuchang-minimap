"""
oob_pick.py - click a spot on a shipped chapter map and see the whole navmesh piece that draws it.

The map ships as a picture, so a slab the player can never stand on is indistinguishable from a
floor once it is baked. This tool puts the picture back on top of the geometry it came from: it
reproduces `build_map.py`'s chapter build up to the island filter, serves the chapter on localhost as
the mod's own full map draws it - the height planes cut at a feet Z, shaded by absolute Z, with the
ground the reachability flood never reached left out - and resolves a clicked pixel to the navmesh
polygon under it, then to that polygon's connected component and bridged cluster. The whole piece is highlighted and described - area, world
bounds, Z range, which streamed level and navmesh tiles feed it, which pipeline stage let it through,
and the height code plus reachable bit the runtime actually reads at that pixel.

Verdicts go to `oob_picks.json` next to this file: a world point and the chapter it belongs to, not a
component id, because component ids are assigned in load order and do not survive a regeneration.

Every chapter in `maps.json` is reachable from the page's chapter box. One is loaded at startup and
the rest the first time the page asks for them - 11-24 s of geometry, then ~10 s for the first cut.
A chapter already loaded stays loaded, so switching back is instant.

The tool only reads the map and the dumps. The one thing it writes is `oob_picks.json`.

Run:
    python oob_pick.py --chapter chapter1
"""

from __future__ import annotations

import argparse
import glob
import http.server
import io
import json
import math
import socketserver
import sys
import threading
import time
import urllib.parse
import webbrowser
from pathlib import Path

import numpy as np
from PIL import Image

import build_map
import mapfmt
import render
import slice_preview as sp

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_MAPS = REPO / "maps"
DEFAULT_PICKS = HERE / "oob_picks.json"

PICKS_SCHEMA = "wuchang-oob-picks/2"

# Which layer a piece belongs to, and the colour it is washed with when its layer is shown. A layer
# that is not shown is CUT from the picture - struck out of the height stack - which is the map the
# pipeline will produce once the rule ships. `legit` is the exception: a piece judged legitimate is
# ordinary ground and is never cut, so showing it only tints it.
GROUPS = {
    "batch":   {"rgb": (229, 72, 77),   "label": "this batch"},
    "earlier": {"rgb": (168, 85, 247),  "label": "earlier batches"},
    "doubt":   {"rgb": (34, 211, 238),  "label": "seeded, or barely drawn"},
    "rules":   {"rgb": (245, 158, 11),  "label": "one-way: no walk back home"},
    "legit":   {"rgb": (34, 197, 94),   "label": "judged legit"},
}

# What a marker says about the ground under it. The picture paints the GROUP, not the category, so
# one glance answers "is anything here and what kind"; the category and the name are in the report.
MARKER_GROUPS = {
    "site":  {"rgb": (56, 189, 248), "label": "shrine, boss, npc, door, ladder",
              "cats": ["shrine", "boss", "elite", "npc", "fog_gate", "benediction_door",
                       "mystery_gate", "door", "lift", "ladder", "hidden", "bamboozling"]},
    "loot":  {"rgb": (250, 204, 21), "label": "chest, item, pickup",
              "cats": ["chest", "key", "item", "weapon", "armour", "amulet", "jade", "spell",
                       "ammo", "material", "consumable", "harvest"]},
    "enemy": {"rgb": (248, 113, 113), "label": "enemy", "cats": ["enemy"]},
    "note":  {"rgb": (148, 163, 184), "label": "note, other - hangs on a wall",
              "cats": ["note", "other"]},
}

# A mark the pipeline disagrees with: its component carries a marker of the game's own - a shrine, an
# enemy, a pickup - or the mod draws almost none of it, so cutting it changes nothing.
DOUBT_DRAWN_PCT = 10.0

# Finished pictures kept per chapter, keyed by the feet Z, the hidden pieces and the seam switch.
# Only the most recent cut keeps its slice arrays; the rest are a PNG each, so ticking a layer back
# off costs nothing the second time.
CUT_CACHE = 8

# How many of the one-way components the page is shown. The rule takes thousands a chapter and the
# mod draws almost none of them; the largest are where its cost is.
ONE_WAY_SHOWN = 200

# A surface within this much Z of the polygon under the cursor is the same surface - the tolerance
# rasterize_heights merges height slots with.
SURFACE_MATCH_UU = 120.0

# Slots x pixels a piece's own height stack may take. A component is thousands of pixels and gets
# every slot; a whole cluster is 20 Mpx of bounding box, where eight slots would be 650 MB.
STACK_BUDGET_PX = 60_000_000

# XY pitch of the point-location grid. A navmesh tile is 1280 uu and a polygon is far smaller.
PICK_GRID_UU = 256.0

# The full map's own slice style, as `overlay_fullmap.cpp` builds it from the shipped config:
# style_from() plus the `above_band = 1e9` override, which is what makes the whole chapter one cut
# instead of a 600 uu band around the feet. Everything else is an mm::Config default.
FULLMAP_TOL_UU = 200.0        # floor_z_tolerance
FULLMAP_ABOVE_BAND = 1.0e9    # overlay_fullmap.cpp: show_adjacent_floors = 0 still means "my storey"
FULLMAP_EQUALIZE = True       # shade_map_equalize

# Rows per pass of the game view. The height planes are 343 MB raw; a band keeps the slice's
# working set small without changing a pixel of the result.
BAND_ROWS = 512


# ---------------------------------------------------------------------------------
# the chapter, rebuilt
# ---------------------------------------------------------------------------------


class ChapterUnavailable(RuntimeError):
    """A chapter cannot be built - unknown key, or no tile dumps for it on this box."""


def group_of(pick: dict, comp: dict | None, open_batch: int, drawn_pct: float) -> str:
    """Which layer a mark belongs to.

    Doubt comes first because it is the one that needs answering: a marker of the game's own
    standing on the component is the pipeline saying the player goes there, and a piece the mod
    barely draws is a mark that changes nothing. Either way the verdict is worth a second look,
    whichever batch it was made in.
    """
    if pick.get("verdict") == "ask":
        return "doubt"       # a piece put in front of the user; nothing is decided about it yet
    if pick.get("verdict") != "oob":
        return "legit"
    # A second verdict is a human answering the doubt, whichever way; the test has had its say.
    if not pick.get("revised") and (
            (comp is not None and comp.get("seeded")) or drawn_pct < DOUBT_DRAWN_PCT):
        return "doubt"
    return "batch" if pick.get("batch") == open_batch else "earlier"


class Chapter:
    """One chapter's geometry, staged exactly as `build_map.build_chapter` stages it."""

    def __init__(self, key: str, maps_dir: Path, input_root: Path | None) -> None:
        self.key = key
        self.maps_dir = maps_dir
        manifest = json.loads((maps_dir / "maps.json").read_text(encoding="utf-8"))
        if key not in manifest.get("chapters", {}):
            raise ChapterUnavailable(f"{key} is not in {maps_dir / 'maps.json'} (have: "
                                     f"{', '.join(sorted(manifest.get('chapters', {})))})")
        self.entry = manifest["chapters"][key]
        self.args = build_map.build_parser().parse_args([])
        self.args.chapter = key
        self.args.agent = self.entry.get("agent", "Small")

        # The shipped bounds, not recomputed ones: the picture on screen must be the shipped picture.
        self.bounds = render.Bounds(
            min_x=self.entry["min_x"], min_y=self.entry["min_y"],
            max_x=self.entry["max_x"], max_y=self.entry["max_y"],
            px_per_uu=self.entry["px_per_uu"],
        )
        self.width = int(self.entry["image_width"])
        self.height = int(self.entry["image_height"])
        self.z_min = float(self.entry["z_min"])
        self.z_max = float(self.entry["z_max"])
        self.z_code_mask = int(self.entry.get("z_code_mask", 4095))
        self.reach_bit = int(self.entry.get("reach_bit", 4096))
        self.plane_paths = list(self.entry.get("height_planes", []))
        self._planes: list[np.ndarray | None] = [None] * len(self.plane_paths)
        self._cut: dict | None = None  # the standing cut: picture, ramp, pieces taken out
        self._pngs: dict[tuple, bytes] = {}   # finished pictures, keyed by what went into them
        self._pieces: tuple[tuple, list[dict]] | None = None  # the marks and proposals, located
        self._shelves: list[list[int]] | None = None   # components grouped into flat shelves
        self._oneway: list[dict] | None = None        # the biggest one-way components
        self._shelf_of: dict[int, int] = {}
        self._group_png: tuple[tuple, bytes] | None = None  # the shown layers, washed
        self._shapes: dict[str, dict] = {}     # a piece's mask and measurement, by its key
        # One cut at a time. A page reload fires its requests together, and two threads patching the
        # standing cut in place would each see half the other's work.
        self._cut_lock = threading.Lock()

        self.input_root = input_root or self._guess_input_root()
        self._load()

    def _guess_input_root(self) -> Path:
        root = self.args.input if self.args.input.is_absolute() else HERE / self.args.input
        sub = root / f"ch{build_map.chapter_number(self.key)}"
        return sub if sub.is_dir() else root

    def _load(self) -> None:
        t0 = time.time()
        agents = render.discover_agents(self.input_root)
        if self.args.agent not in agents:
            raise ChapterUnavailable(
                f"{self.key}: agent '{self.args.agent}' not found under {self.input_root} "
                f"(have: {', '.join(sorted(agents)) or 'nothing'})")
        dump = render.load_dump_files(agents[self.args.agent], agent_hint=self.args.agent,
                                      dedupe="richest")
        self.tile_source = {t.key: t.source for t in dump.tiles.values()}
        polys = render.polygons_of(dump)
        for i, p in enumerate(polys):
            p["idx"] = i
            p["stage"] = "drawn"
        self.polys = polys

        # stage 1 - the out-of-bounds flat sheets
        a = self.args
        render.classify_flat_planes(
            polys, render.DEFAULT_FLAT_PLANE_AREA,
            sheet_min=a.flat_plane_sheet_min, isolation=a.flat_plane_isolation,
        )
        rest = []
        for p in polys:
            if p["plane"]:
                p["stage"] = "plane"
            else:
                rest.append(p)

        # stage 2 - components, clusters, the island filter's verdict
        seed_files = [Path(q) for pat in (a.markers or build_map.default_marker_globs(self.key))
                      for q in sorted(glob.glob(str(pat)))]
        self.seeds = render.load_marker_seeds(seed_files)
        # `cut_oob=False` on purpose: the build cuts the out-of-bounds ground, and this tool is
        # where that cut is judged. It has to show the map WITHOUT it and take the pieces out as
        # layers, or the proposals would already be missing from the picture they are proposed on.
        self.comps, self.clusters, keep_ids, self.islands = render.decide_islands(
            rest, self.seeds,
            grid=a.island_grid, z_tol=a.island_z_tol, min_area=a.island_min_area,
            seed_radius=a.island_seed_radius, require_seed=a.island_require_seed,
            bridge_xy=a.island_bridge_xy, bridge_z=a.island_bridge_z,
            cluster_area=a.island_cluster_area, cut_oob=False,
        )
        self.rest = rest              # what the island filter judged: the rule reads these
        self.keep_ids = keep_ids
        self.comp_polys: dict[int, list[dict]] = {}
        for p in rest:
            self.comp_polys.setdefault(p["comp"], []).append(p)
            if p["comp"] not in keep_ids:
                p["stage"] = "island"
        for c in self.comps:
            c["kept"] = c["id"] in keep_ids

        self._index(polys)
        # The storey the page opens on: the area-weighted median Z of the drawn ground, which puts
        # the feet on whatever the chapter is mostly made of.
        drawn = sorted(((q["cz"], q["xyarea"]) for q in rest if q["comp"] in keep_ids),
                       key=lambda t: t[0])
        half = sum(a for _, a in drawn) / 2.0
        run = 0.0
        self.feet_z0 = 0.0
        for cz, a in drawn:
            run += a
            if run >= half:
                self.feet_z0 = float(cz)
                break
        print(f"[{self.key}] {len(polys)} polygons, {len(self.comps)} components, "
              f"{len(self.clusters)} clusters, {self.islands['polys_after']} drawn  "
              f"({time.time() - t0:.1f}s)", flush=True)
        print(render.describe_islands(self.key, self.islands), flush=True)

    def _index(self, polys: list[dict]) -> None:
        """Bucket every polygon by its XY bounding box, for point location."""
        grid: dict[tuple[int, int], list[int]] = {}
        for p in polys:
            xs = [q[0] for q in p["pts"]]
            ys = [q[1] for q in p["pts"]]
            p["bbox"] = (min(xs), min(ys), max(xs), max(ys))
            for gx in range(int(math.floor(min(xs) / PICK_GRID_UU)),
                            int(math.floor(max(xs) / PICK_GRID_UU)) + 1):
                for gy in range(int(math.floor(min(ys) / PICK_GRID_UU)),
                                int(math.floor(max(ys) / PICK_GRID_UU)) + 1):
                    grid.setdefault((gx, gy), []).append(p["idx"])
        self.grid = grid

    # ---- coordinates -------------------------------------------------------------

    def to_world(self, u: float, v: float) -> tuple[float, float]:
        b = self.bounds
        return (b.max_x - v / b.px_per_uu, b.min_y + u / b.px_per_uu)

    def markers(self) -> list[dict]:
        """Every marker of the chapter at its map pixel, against the surface the cut drew there.

        `dz` is the marker's height over the surface the picture actually shows under it, which is
        what separates a marker standing on this storey from one three floors away. It is null where
        the picture draws nothing there - including ground a mark has cut away, so a piece that loses
        its markers when it is cut says so.
        """
        st = self._cut
        zp = st["z_pick"] if st else None
        out = []
        for s in self.seeds:
            u, v = self.bounds.to_px(s["x"], s["y"])
            iu, iv = int(u), int(v)
            dz = None
            if zp is not None and 0 <= iv < self.height and 0 <= iu < self.width:
                z = float(zp[iv, iu])
                if z == z:      # not NaN - something is drawn under the marker
                    dz = round(s["z"] - z)
            out.append({"u": round(u, 1), "v": round(v, 1), "z": round(s["z"]),
                        "cat": s["cat"], "name": s.get("name", ""), "dz": dz})
        return out

    def plane(self, i: int) -> np.ndarray:
        if self._planes[i] is None:
            self._planes[i] = np.array(Image.open(self.maps_dir / self.plane_paths[i]))
        return self._planes[i]

    # ---- the marks ---------------------------------------------------------------

    def shelves(self) -> list[list[int]]:
        """Kept components grouped into shelves - one height, one place, nothing underneath.

        A selection mode only. `flat_shelves` was a cut rule until the fall cap reached everything it
        reached; what is left is a useful unit to judge - one verdict for a neighbourhood.
        """
        if self._shelves is None:
            self._shelves = render.flat_shelves(self.rest, self.comps, self.keep_ids)
            self._shelf_of = {cid: i for i, g in enumerate(self._shelves) for cid in g}
        return self._shelves

    def rule_one_way(self) -> list[dict]:
        """The biggest components you cannot walk back off.

        The rule takes thousands of components a chapter and the mod draws a few hundred of them.
        Measuring every one costs a minute of startup for pieces with no pixel on screen, so the page
        is handed the largest `ONE_WAY_SHOWN` by area. That is a SAMPLE, and since the fall cap it is
        no longer most of the cost: in chapter 3 the sample draws 184 520 px of the 637 427 the rule
        takes. It is the big end of it - judge the sample, and the small pieces follow it.
        """
        if self._oneway is None:
            got = render.one_way_ground(self.rest, self.comps, self.keep_ids)
            self._oneway = sorted(got, key=lambda c: -c["area"])[:ONE_WAY_SHOWN]
        return self._oneway

    def pieces(self, picks_path: Path) -> list[dict]:
        """Every piece the page can show or cut, located and tagged with its layer.

        A mark is re-located from the world point it stored, never from a component id - ids are
        assigned in load order and a regeneration renumbers them. A rule proposal has no stored
        point, so it carries the centre of its largest polygon for the page to jump to.

        A component the user has already judged is never also proposed by the rules: the verdict is
        the answer, whichever way it went.
        """
        doc = load_doc(picks_path)
        sig = (len(doc["picks"]), doc["open_batch"])
        if self._pieces is not None and self._pieces[0] == sig:
            return self._pieces[1]
        out: list[dict] = []
        judged: set[int] = set()
        for i, p in enumerate(doc["picks"]):
            w = p.get("world") or []
            if p.get("chapter") != self.key or len(w) < 3:
                continue
            poly = self.locate(float(w[0]), float(w[1]), float(w[2]))
            if poly is None:
                continue
            comp = self.comps[poly["comp"]] if "comp" in poly else None
            sel = self.selection(poly, p.get("mode", "comp"))
            # The verdict speaks for every component the mark covers, not just the one under the
            # point: a shelf judged whole must not come back as a proposal minus one component.
            judged.update(q["comp"] for q in sel if "comp" in q)
            pc = self._piece(
                key=f"{w[0]},{w[1]},{w[2]}", sel=sel, comp=comp,
                i=i, verdict=p.get("verdict", "oob"), note=p.get("note", ""),
                world=[float(w[0]), float(w[1]), float(w[2])], batch=p["batch"])
            pc["group"] = group_of(p, comp, doc["open_batch"], pc["drawn_pct"])
            out.append(pc)
        taken = set(judged)
        for comp in self.rule_one_way():
            if comp["id"] in taken:
                continue
            taken.add(comp["id"])
            pc = self._proposal(f"comp:{comp['id']}", self.comp_polys.get(comp["id"], []),
                                comp, "comp")
            if pc is not None:
                out.append(pc)
        self._pieces = (sig, out)
        return out

    def _proposal(self, key: str, sel: list[dict], comp: dict, mode: str) -> dict | None:
        """A rule's catch as a piece, pointing at the centre of its largest polygon."""
        if not sel:
            return None
        big = max(sel, key=lambda q: q["xyarea"])
        return self._piece(
            key=key, sel=sel, comp=comp, group="rules", mode=mode,
            i=None, verdict="", note="", batch=None,
            world=[round(sum(q[0] for q in big["pts"]) / len(big["pts"]), 1),
                   round(sum(q[1] for q in big["pts"]) / len(big["pts"]), 1),
                   round(big["cz"], 1)])

    def _piece(self, key: str, sel: list[dict], comp: dict | None, **rest) -> dict:
        """One piece, measured now rather than remembered.

        `drawn` is the pixels of it the mod puts on screen - the only number that says whether
        cutting the piece changes anything. A rule proposal has never been measured at all, and a
        mark's own stored figure is from whenever it was made, so both are taken here.

        The shape and the measurement belong to the ground, not to the verdict on it, so they are
        kept against the piece's key: re-reading the file after a mark re-uses them, and only the
        verdict, the layer and the note are built again.
        """
        got = self._shapes.get(key)
        if got is None:
            x0, y0, m, z = mask_of(self, sel)
            lit, reach = raster_masks(self, x0, y0, m, z)
            got = self._shapes[key] = dict(
                key=key, comp=None if comp is None else comp["id"],
                area_m2=round(sum(q["xyarea"] for q in sel) / 10000.0, 1),
                drawn=int(reach.sum()), px=int(m.sum()),
                drawn_pct=round(100.0 * int(reach.sum()) / max(1, int(m.sum())), 1),
                x=x0, y=y0, mask=m, z=z, reach=reach)
        return dict(got, **rest)

    def forget_pieces(self) -> None:
        self._pieces = None

    # ---- the picture -------------------------------------------------------------

    def cut(self, feet_z: float, unreachable: str, layers: list[dict],
            seams: bool = True) -> bytes:
        """The chapter cut the way the mod's full map cuts it, with `layers` taken out of it.

        This is the only picture the picker draws. The composite `small.png` is not it: the mod never
        loads it (`fallback_use_composite = 0`), it is a flat per-polygon fill with drawn edges, and
        it carries ground the reachability flood never reached.

        Every decision here belongs to `slice_preview`, which mirrors `overlay_slice.cpp` and is kept
        in step with it: `slice_window` picks the surface and its class, `ramp_range` / `zhist` set
        the ramp's ends over the drawn pixels, `shade` colours by absolute Z with the seam darkening.
        The only things this adds are doing it over the whole image in bands, keeping the result's
        alpha instead of compositing it onto a background, and removing the marked pieces.

        A `layer` is a marked piece - `{key, x, y, mask, z}` off `mask_of`. Its surface is struck out
        of the height stack BEFORE the slice runs, so the pixel falls through to whatever lies under
        it: what the map will show once the piece is cut from the pipeline. Painting the piece out
        afterwards would hide that ground instead of revealing it.

        `seams` is `srule::seam_factor`: the mod darkens a pixel whose left or up neighbour sits
        more than 300 uu away in Z, which outlines every storey against the one below it. On by
        default, because the mod draws it.

        A new mark re-cuts only the box it covers (0.4 s against 10 s) on the ramp the picture
        already stands on. The equalised ramp is a CDF of the drawn Z, so removing a piece does shift
        it - measured at no more than 2/255 per channel, and the shapes are identical. The Z field's
        button takes the whole cut again.

        `unreachable` is the runtime key of the same name: `hide` is the shipped default, `show`
        draws the ground the flood never reached like any other.
        """
        with self._cut_lock:
            sig = (feet_z, unreachable, seams, frozenset(lay["key"] for lay in layers))
            done = self._pngs.get(sig)
            if done is not None:
                return done
            st = self._cut
            if st is not None and st["feet_z"] == feet_z and st["unreachable"] == unreachable:
                fresh = [lay for lay in layers if lay["key"] not in st["keys"]]
                if len(st["keys"]) + len(fresh) == len(layers):  # nothing put back: patch in place
                    for lay in fresh:
                        self._patch(st, lay, layers)
                        st["keys"].append(lay["key"])
                    if st["seams"] != seams:
                        self._reshade(st, seams)
                    return self._remember(sig, st["png"])
            self._full_cut(feet_z, unreachable, layers, seams)
            return self._remember(sig, self._cut["png"])

    def _remember(self, sig: tuple, png: bytes) -> bytes:
        """Keep the finished picture. Its inputs name it, so nothing here can go stale."""
        self._pngs[sig] = png
        for old in list(self._pngs)[:-CUT_CACHE]:
            del self._pngs[old]
        return png

    def groups_png(self, picks_path: Path, show: set[str]) -> bytes:
        """The shown layers washed over the map, each in its own colour.

        A shown layer is drawn by the slicer like any other ground - this only says which ground
        belongs to which layer, so a piece can be found and judged. It is a separate picture from
        the cut so that ticking a layer does not re-shade the map underneath it.
        """
        pieces = self.pieces(picks_path)
        key = (self._pieces[0], frozenset(show))
        if self._group_png is not None and self._group_png[0] == key:
            return self._group_png[1]
        rgba = np.zeros((self.height, self.width, 4), dtype=np.uint8)
        for pc in pieces:
            if pc["group"] not in show:
                continue
            m, dr = pc["mask"], pc["reach"]
            sub = rgba[pc["y"]:pc["y"] + m.shape[0], pc["x"]:pc["x"] + m.shape[1]]
            r, g, b = GROUPS[pc["group"]]["rgb"]
            # Ground the mod hides gets an OUTLINE and no fill. Measured the other way first: a
            # wash of alpha 18 over the page's near-black still reads as a solid piece of map, and
            # a piece the mod draws 1 % of then looks like a piece it draws. Only the drawn part is
            # filled, so the colour on screen is always ground that is on screen.
            sub[m & ~_inner(m)] = (r, g, b, 90)
            sub[dr] = (r, g, b, 95)
            sub[dr & ~_inner(dr)] = (r, g, b, 240)
        buf = io.BytesIO()
        Image.fromarray(rgba, "RGBA").save(buf, format="PNG")
        self._group_png = (key, buf.getvalue())
        return self._group_png[1]

    # ---- the cut, in pieces ------------------------------------------------------

    def _stack(self, box: tuple[int, int, int, int],
               layers: list[dict]) -> tuple[np.ndarray, np.ndarray] | None:
        """The height stack over a pixel box, with every layer's own surface struck out."""
        x0, y0, x1, y1 = box
        zs, rs = [], []
        # Which pieces meet this box is asked once, not once per height plane: a chapter under review
        # carries a hundred of them and the cut walks the whole picture in bands.
        met = [(lay, ov) for lay in layers for ov in (overlap(box, lay),) if ov is not None]
        for i in range(len(self.plane_paths)):
            raw = self.plane(i)[y0:y1, x0:x1]
            code = mapfmt.z_codes(raw).astype(np.float32)
            have = code > 0
            if not have.any():
                continue  # an empty slot can never be chosen; the deep ones are nearly all empty
            z = np.where(have, self.z_of_code(code), np.float32("nan")).astype(np.float32)
            for lay, ov in met:
                (ax0, ay0, ax1, ay1), (bx0, by0, bx1, by1) = ov
                sub = z[ay0:ay1, ax0:ax1]
                sub[lay["mask"][by0:by1, bx0:bx1]
                    & matches_z(lay["z"][:, by0:by1, bx0:bx1], sub)] = np.nan
            zs.append(z)
            rs.append(mapfmt.reach_mask(raw) & have & ~np.isnan(z))
        if not zs:
            return None
        return np.stack(zs), np.stack(rs)

    def _slice(self, box: tuple[int, int, int, int], feet_z: float, unreachable: str,
               layers: list[dict]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """`slice_window` over a pixel box, in row bands. Returns (cls, z_pick, alpha)."""
        x0, y0, x1, y1 = box
        h, w = y1 - y0, x1 - x0
        cls = np.zeros((h, w), dtype=np.uint8)
        z_pick = np.full((h, w), np.nan, dtype=np.float32)
        alpha = np.zeros((h, w), dtype=np.float32)
        for by in range(0, h, BAND_ROWS):
            by1 = min(h, by + BAND_ROWS)
            got = self._stack((x0, y0 + by, x1, y0 + by1), layers)
            if got is None:
                continue
            c, zp, al, _ = sp.slice_window(got[0], got[1], feet_z, FULLMAP_TOL_UU,
                                           FULLMAP_ABOVE_BAND, unreachable=unreachable)
            cls[by:by1], z_pick[by:by1], alpha[by:by1] = c, zp, al
        return cls, z_pick, alpha

    def _paint(self, st: dict, box: tuple[int, int, int, int], cls: np.ndarray,
               z_pick: np.ndarray, alpha: np.ndarray, ctx_rows: int = 0) -> None:
        """Shade a slice on the cut's ramp and write it in.

        The arrays cover `box` grown upward by `ctx_rows`, which `seam_factor` needs to see and the
        picture does not want. `sp.shade` reads `cls` for one thing only - which pixels the seam may
        darken - so a `cls` of zeros is how the seam is turned off without forking the shader.
        """
        x0, y0, x1, y1 = box
        rgb = sp.shade(z_pick, cls if st["seams"] else np.zeros_like(cls), st["z_lo"], st["z_hi"],
                       sp.DEFAULT_LO_COLOR, sp.DEFAULT_HI_COLOR, sp.DEFAULT_GAMMA,
                       st["hist"])[ctx_rows:]
        cls, alpha = cls[ctx_rows:], alpha[ctx_rows:]
        tile = st["rgba"][y0:y1, x0:x1]
        tile[..., :3] = rgb.astype(np.uint8)
        tile[..., 3] = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
        tile[cls == sp.CLS_NONE, 3] = 0

    def _full_cut(self, feet_z: float, unreachable: str, layers: list[dict],
                  seams: bool) -> None:
        t0 = time.time()
        w, h = self.width, self.height
        cls, z_pick, alpha = self._slice((0, 0, w, h), feet_z, unreachable, layers)
        drawn = cls != sp.CLS_NONE
        z_lo, z_hi = sp.ramp_range(cls, z_pick, self.z_min, self.z_max,
                                   0.0 if FULLMAP_EQUALIZE else sp.DEFAULT_RANGE_PCT_LO,
                                   0.0 if FULLMAP_EQUALIZE else sp.DEFAULT_MIN_RANGE)
        st = {"feet_z": feet_z, "unreachable": unreachable, "seams": seams,
              "keys": [lay["key"] for lay in layers], "z_lo": z_lo, "z_hi": z_hi,
              "hist": (sp.zhist(cls, z_pick, self.z_min, self.z_max)
                       if FULLMAP_EQUALIZE and bool(drawn.any()) else None),
              "rgba": np.zeros((h, w, 4), dtype=np.uint8),
              "cls": cls, "z_pick": z_pick, "alpha": alpha}
        self._cut = st
        self._shade_all(st)
        print(f"[{self.key}] cut at feet Z {feet_z:.0f}, unreachable={unreachable}, "
              f"{len(layers)} piece(s) removed: {int(drawn.sum())} px drawn, "
              f"ramp {z_lo:.0f}..{z_hi:.0f} uu ({time.time() - t0:.1f}s)", flush=True)

    def _shade_all(self, st: dict) -> None:
        """Re-colour the whole picture from the slice the cut is holding, in bands."""
        h, w = self.height, self.width
        for y0 in range(0, h, BAND_ROWS):
            y1 = min(h, y0 + BAND_ROWS)
            top = max(0, y0 - 1)
            self._paint(st, (0, y0, w, y1), st["cls"][top:y1], st["z_pick"][top:y1],
                        st["alpha"][top:y1], y0 - top)
        self._encode(st)

    def _reshade(self, st: dict, seams: bool) -> None:
        t0 = time.time()
        st["seams"] = seams
        self._shade_all(st)
        print(f"[{self.key}] re-shaded, seams={'on' if seams else 'off'} "
              f"({time.time() - t0:.1f}s)", flush=True)

    def _patch(self, st: dict, lay: dict, layers: list[dict]) -> None:
        """Re-cut just the box a newly marked piece covers, on the standing ramp."""
        t0 = time.time()
        mh, mw = lay["mask"].shape
        box = (max(0, lay["x"] - 1), max(0, lay["y"] - 1),
               min(self.width, lay["x"] + mw + 1), min(self.height, lay["y"] + mh + 1))
        top = max(0, box[1] - 1)
        cls, z_pick, alpha = self._slice((box[0], top, box[2], box[3]), st["feet_z"],
                                         st["unreachable"], layers)
        ctx = box[1] - top
        st["cls"][box[1]:box[3], box[0]:box[2]] = cls[ctx:]
        st["z_pick"][box[1]:box[3], box[0]:box[2]] = z_pick[ctx:]
        st["alpha"][box[1]:box[3], box[0]:box[2]] = alpha[ctx:]
        self._paint(st, box, cls, z_pick, alpha, ctx)
        self._encode(st)
        print(f"[{self.key}] re-cut {box[2] - box[0]}x{box[3] - box[1]} px around a new mark "
              f"({time.time() - t0:.1f}s)", flush=True)

    def _encode(self, st: dict) -> None:
        buf = io.BytesIO()
        Image.fromarray(st["rgba"], "RGBA").save(buf, format="PNG")
        st["png"] = buf.getvalue()

    def z_of_code(self, code: "int | np.ndarray") -> "float | np.ndarray":
        return self.z_min + (code - 1) / 4094.0 * (self.z_max - self.z_min)

    def surfaces_at(self, u: int, v: int) -> list[dict]:
        """Every height slot the runtime would find at this pixel, low Z first."""
        out = []
        if not (0 <= u < self.width and 0 <= v < self.height):
            return out
        for i in range(len(self.plane_paths)):
            raw = int(self.plane(i)[v, u])
            code = raw & self.z_code_mask
            if code == 0:
                continue
            out.append({"plane": i, "code": code, "z": round(float(self.z_of_code(code)), 1),
                        "reachable": bool(raw & self.reach_bit)})
        return out

    # ---- picking -----------------------------------------------------------------

    def locate(self, wx: float, wy: float, wz: float | None) -> dict | None:
        """The polygon under a world point, preferring the one nearest `wz`."""
        cand = self.grid.get((int(math.floor(wx / PICK_GRID_UU)),
                              int(math.floor(wy / PICK_GRID_UU))), [])
        hits = []
        for idx in cand:
            p = self.polys[idx]
            x0, y0, x1, y1 = p["bbox"]
            if x0 <= wx <= x1 and y0 <= wy <= y1 and render._point_in_poly(p["pts"], wx, wy):
                hits.append(p)
        if not hits:  # a click a hair outside a sliver still means that sliver
            reach = 1.0 / self.bounds.px_per_uu
            near = [(math.hypot(self.polys[i]["cx"] - wx, self.polys[i]["cy"] - wy), i)
                    for i in cand]
            near = [t for t in near if t[0] <= reach]
            if not near:
                return None
            hits = [self.polys[min(near)[1]]]
        if wz is None:
            return max(hits, key=lambda p: p["cz"])
        return min(hits, key=lambda p: abs(p["cz"] - wz))

    def selection(self, poly: dict, mode: str) -> list[dict]:
        """The piece a pick highlights: the polygon, its component, its shelf or its cluster."""
        if mode == "poly" or "comp" not in poly:
            return [poly]
        comp = self.comps[poly["comp"]]
        if mode == "shelf":
            self.shelves()
            g = self._shelf_of.get(poly["comp"])
            if g is not None:
                return [q for cid in self._shelves[g] for q in self.comp_polys.get(cid, [])]
        if mode == "cluster" and comp.get("cluster") is not None:
            out: list[dict] = []
            for cid in self.clusters[comp["cluster"]]["members"]:
                out.extend(self.comp_polys.get(cid, []))
            return out
        return self.comp_polys.get(poly["comp"], [poly])


# ---------------------------------------------------------------------------------
# the chapters on offer
# ---------------------------------------------------------------------------------


class Library:
    """Every chapter `maps.json` ships, loaded on first use and kept.

    A chapter costs 11-24 s of geometry, so the page names the one it wants on every request and
    the first mention of a new one builds it. Keeping them all is deliberate: all five loaded, three
    of them cut, measure 3.7 GB - nothing against a dev box - while dropping one would make every
    switch back cost the full build again. The lock makes concurrent requests for the same chapter
    wait for one build instead of starting several.
    """

    def __init__(self, maps_dir: Path, input_root: Path | None) -> None:
        self.maps_dir = maps_dir
        self.input_root = input_root
        manifest = json.loads((maps_dir / "maps.json").read_text(encoding="utf-8"))
        self.keys = sorted(manifest.get("chapters", {}))
        self._loaded: dict[str, Chapter] = {}
        self._lock = threading.Lock()

    def get(self, key: str) -> Chapter:
        with self._lock:
            if key not in self._loaded:
                self._loaded[key] = Chapter(key, self.maps_dir, self.input_root)
            return self._loaded[key]

    def is_loaded(self, key: str) -> bool:
        return key in self._loaded

    def forget_pieces(self) -> None:
        for ch in self._loaded.values():
            ch.forget_pieces()


# ---------------------------------------------------------------------------------
# the report
# ---------------------------------------------------------------------------------


def mask_of(ch: Chapter, sel: list[dict]) -> tuple[int, int, np.ndarray, np.ndarray]:
    """Coverage mask and per-pixel Z STACK of a polygon set, cropped to its bounding box.

    A piece folds over itself: chapter 3's component #40 spans 1 040 uu of Z, and 4 586 of its
    27 494 pixels carry two of its own surfaces more than 200 uu apart. One Z per pixel cuts one of
    them and leaves the other painted - the piece reads as marked, stays on the map, and refuses the
    next click on it. So the Z comes back as `(k, h, w)`, NaN where a slot holds nothing, and every
    caller asks `matches_z` whether a height is one of the piece's at that pixel.

    It is `build_map.rasterize_heights` that builds it - the same pass that laid the shipped height
    planes down, at the same `merge_tol` and the same `seam_px`. That is what the piece has to line
    up with, so there is nothing here to keep in step with it by hand.
    """
    us, vs = [], []
    for p in sel:
        for q in p["pts"]:
            u, v = ch.bounds.to_px(q[0], q[1])
            us.append(u)
            vs.append(v)
    x0 = max(0, int(math.floor(min(us))) - 1)
    x1 = min(ch.width, int(math.ceil(max(us))) + 2)
    y0 = max(0, int(math.floor(min(vs))) - 1)
    y1 = min(ch.height, int(math.ceil(max(vs))) + 2)
    w, h = max(1, x1 - x0), max(1, y1 - y0)

    # The crop's own bounds, on the chapter's pixel grid: `Bounds.width` is the span plus one pixel,
    # so the far edge sits on the last pixel's world position.
    ppu = ch.bounds.px_per_uu
    sub = render.Bounds(min_x=ch.bounds.max_x - (y0 + h - 1) / ppu,
                        min_y=ch.bounds.min_y + x0 / ppu,
                        max_x=ch.bounds.max_x - y0 / ppu,
                        max_y=ch.bounds.min_y + (x0 + w - 1) / ppu,
                        px_per_uu=ppu)
    # A whole cluster is 20 Mpx of bounding box, where eight slots would be 650 MB. The budget cuts
    # the slot count, never the pixels: a piece small enough to judge always gets all of them.
    slots = max(1, min(len(ch.plane_paths), int(STACK_BUDGET_PX // max(1, w * h))))
    z, _ = build_map.rasterize_heights(sel, sub, slots, merge_tol=SURFACE_MATCH_UU,
                                       progress=0)
    return x0, y0, ~np.isnan(z).all(axis=0), z


def matches_z(zst: np.ndarray, z: "np.ndarray | float") -> np.ndarray:
    """Where a height is one of the piece's own - any slot of its stack. NaN never matches."""
    with np.errstate(invalid="ignore"):
        return (np.abs(zst - z) <= SURFACE_MATCH_UU).any(axis=0)


def raster_masks(ch: Chapter, x0: int, y0: int, m: np.ndarray,
                 z: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Which pixels of a piece the height planes hold, and which of those the mod draws."""
    h, w = m.shape
    lit = np.zeros((h, w), dtype=bool)
    reach = np.zeros((h, w), dtype=bool)
    for i in range(len(ch.plane_paths)):
        raw = ch.plane(i)[y0:y0 + h, x0:x0 + w].astype(np.int32)
        code = raw & ch.z_code_mask
        hit = m & (code != 0) & matches_z(z, ch.z_of_code(code)) & ~lit
        reach |= hit & ((raw & ch.reach_bit) != 0)
        lit |= hit
    return lit, reach


def raster_stats(ch: Chapter, x0: int, y0: int, m: np.ndarray, z: np.ndarray) -> dict:
    """How the shipped height planes describe the pixels this piece covers."""
    lit, reach = raster_masks(ch, x0, y0, m, z)
    return {"px": int(m.sum()), "px_on_map": int(lit.sum()), "px_reachable": int(reach.sum()),
            "reachable_pct": round(100.0 * int(reach.sum()) / max(1, int(lit.sum())), 1)}


def describe(ch: Chapter, poly: dict, sel: list[dict], mode: str) -> dict:
    x0, y0, m, z = mask_of(ch, sel)
    xs = [q[0] for p in sel for q in p["pts"]]
    ys = [q[1] for p in sel for q in p["pts"]]
    zs = [q[2] for p in sel for q in p["pts"]]
    stages: dict[str, int] = {}
    levels: dict[str, int] = {}
    tiles = set()
    for p in sel:
        stages[p["stage"]] = stages.get(p["stage"], 0) + 1
        tiles.add(p["tile"])
        name = Path(ch.tile_source.get(p["tile"], "?")).stem
        levels[name] = levels.get(name, 0) + 1

    comp = ch.comps[poly["comp"]] if "comp" in poly else None
    cl = (ch.clusters[comp["cluster"]]
          if comp is not None and comp.get("cluster") is not None else None)
    return {
        "mode": mode,
        "polys": len(sel),
        "comps": len({p.get("comp") for p in sel}),
        "area_m2": round(sum(p["xyarea"] for p in sel) / 10000.0, 1),
        "bbox": [round(min(xs)), round(min(ys)), round(max(xs)), round(max(ys))],
        "z": [round(min(zs), 1), round(max(zs), 1)],
        "stages": stages,
        "tiles": len(tiles),
        "levels": sorted(levels.items(), key=lambda t: -t[1])[:8],
        "comp": None if comp is None else {
            "id": comp["id"], "polys": comp["polys"], "area_m2": round(comp["area"] / 10000.0, 1),
            "kept": comp["kept"], "seeded": bool(comp.get("seeded")),
        },
        "cluster": None if cl is None else {
            "id": cl["id"], "members": len(cl["members"]),
            "area_m2": round(cl["area"] / 10000.0, 1),
            "keep": cl.get("keep"), "why": cl.get("why"),
            "has_largest": cl.get("has_largest"),
        },
        "raster": raster_stats(ch, x0, y0, m, z),
        "markers": markers_near(ch, (min(xs), min(ys), max(xs), max(ys)), (min(zs), max(zs))),
        "overlay": {"x": x0, "y": y0, "w": int(m.shape[1]), "h": int(m.shape[0])},
        "mask": m,
    }


def markers_near(ch: Chapter, bbox: tuple[float, float, float, float],
                 zr: tuple[float, float]) -> list[dict]:
    """The markers `decide_islands` would call this piece's seeds, named.

    Same window the seeding uses - the bounding box grown by `seed_radius`, `seed_z` of the Z range -
    so the report says WHICH marker made a piece `seeded` instead of only that one did. `dz` is the
    marker's height over the piece: a wall note reads as a large one.
    """
    r, zt = ch.args.island_seed_radius, render.DEFAULT_ISLAND_SEED_Z
    out = []
    for s in ch.seeds:
        if not (bbox[0] - r <= s["x"] <= bbox[2] + r and bbox[1] - r <= s["y"] <= bbox[3] + r):
            continue
        if not (zr[0] - zt <= s["z"] <= zr[1] + zt):
            continue
        dz = 0.0
        if s["z"] > zr[1]:
            dz = s["z"] - zr[1]
        elif s["z"] < zr[0]:
            dz = s["z"] - zr[0]
        out.append({"cat": s["cat"], "name": s.get("name", ""), "z": round(s["z"]),
                    "dz": round(dz)})
    out.sort(key=lambda t: abs(t["dz"]))
    return out[:12]


def overlap(box: tuple[int, int, int, int],
            lay: dict) -> tuple[tuple[int, int, int, int], tuple[int, int, int, int]] | None:
    """Where a layer meets a pixel box: the box's own indices, then the layer's."""
    x0, y0, x1, y1 = box
    h, w = lay["mask"].shape
    ix0, iy0 = max(x0, lay["x"]), max(y0, lay["y"])
    ix1, iy1 = min(x1, lay["x"] + w), min(y1, lay["y"] + h)
    if ix1 <= ix0 or iy1 <= iy0:
        return None
    return ((ix0 - x0, iy0 - y0, ix1 - x0, iy1 - y0),
            (ix0 - lay["x"], iy0 - lay["y"], ix1 - lay["x"], iy1 - lay["y"]))


def _inner(m: np.ndarray) -> np.ndarray:
    """The pixels of a mask with all four neighbours inside it - everything else is its edge."""
    return (np.pad(m[1:, :], ((0, 1), (0, 0))) & np.pad(m[:-1, :], ((1, 0), (0, 0)))
            & np.pad(m[:, 1:], ((0, 0), (0, 1))) & np.pad(m[:, :-1], ((0, 0), (1, 0))))


def overlay_png(m: np.ndarray) -> bytes:
    """The highlight: a translucent wash with a solid edge, as a bbox-sized RGBA PNG."""
    h, w = m.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[m] = (255, 64, 96, 110)
    rgba[m & ~_inner(m)] = (255, 210, 40, 255)
    buf = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, format="PNG")
    return buf.getvalue()


# ---------------------------------------------------------------------------------
# picks file
# ---------------------------------------------------------------------------------


def load_doc(path: Path) -> dict:
    """The verdict file: every mark ever made, and the number of the batch still open.

    A mark carries the batch it was made in, so the working list can be emptied without moving
    anything out of the file - closing a batch is one number, and the evidence stays in one place.
    Marks written before batches existed are batch 0, which is to say already closed.
    """
    doc: dict = {}
    if path.exists():
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            doc = raw if isinstance(raw, dict) else {"picks": list(raw)}
        except (OSError, ValueError) as exc:
            print(f"  ! ignoring unreadable {path}: {exc}", file=sys.stderr)
    picks = doc.get("picks", [])
    for p in picks:
        p.setdefault("batch", 0)
        p.setdefault("from", "hand")
    doc["picks"] = picks
    doc["open_batch"] = int(doc.get("open_batch", max((p["batch"] for p in picks), default=-1) + 1))
    return doc


def save_doc(path: Path, doc: dict) -> None:
    out = {"schema": PICKS_SCHEMA, "open_batch": doc["open_batch"], "picks": doc["picks"]}
    path.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------------
# server
# ---------------------------------------------------------------------------------


class Handler(http.server.BaseHTTPRequestHandler):
    library: Library
    opening: str          # the chapter the page opens on, and the default of every request
    picks_path: Path
    overlays: dict[str, bytes] = {}

    def chapter_of(self, q: dict) -> Chapter:
        """The chapter this request is about. Building it takes 11-24 s; the page waits."""
        return self.library.get(q.get("ch", [""])[0] or self.opening)

    @staticmethod
    def shown(q: dict) -> set[str]:
        """The layers the page is showing. Everything else is cut out of the picture."""
        return {g for g in q.get("show", [""])[0].split(",") if g in GROUPS}

    def cut_pieces(self, ch: Chapter, show: set[str]) -> list[dict]:
        """What comes out of the picture: every hidden piece that is actually being cut.

        Ground judged legitimate stays, and so does a piece that is only being ASKED about - an open
        question must not change the map behind the back of the person answering it.
        """
        return [pc for pc in ch.pieces(self.picks_path)
                if pc["group"] not in show and pc["verdict"] not in ("ok", "ask")]

    def pieces_json(self, ch: Chapter) -> list[dict]:
        """This chapter's pieces for the page - no masks, and the pick index the file uses."""
        return [{k: pc[k] for k in
                 ("key", "group", "comp", "area_m2", "drawn", "px", "drawn_pct",
                  "world", "i", "verdict", "note", "batch")}
                | ({"mode": pc["mode"]} if "mode" in pc else {})
                | ({"comps": pc["comps"]} if "comps" in pc else {})
                for pc in ch.pieces(self.picks_path)]

    def log_message(self, fmt, *a):
        pass

    def _send(self, code: int, ctype: str, body: bytes) -> None:
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except (ConnectionError, BrokenPipeError):
            pass   # the page gave up waiting, or was reloaded; the answer is simply not wanted

    def _json(self, obj: dict) -> None:
        self._send(200, "application/json", json.dumps(obj).encode("utf-8"))

    def do_GET(self) -> None:
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        if url.path == "/":
            self._send(200, "text/html; charset=utf-8", PAGE.encode("utf-8"))
            return
        if url.path == "/overlay.png":
            self._send(200, "image/png", self.overlays.get(q.get("k", [""])[0], b""))
            return
        try:
            ch = self.chapter_of(q)
        except ChapterUnavailable as exc:
            self._json({"ok": False, "why": str(exc)})
            return
        if url.path == "/map.png":
            feet = float(q.get("z", [ch.feet_z0])[0])
            self._send(200, "image/png",
                       ch.cut(feet, "show" if q.get("all", ["0"])[0] == "1" else "hide",
                              self.cut_pieces(ch, self.shown(q)),
                              q.get("seams", ["1"])[0] == "1"))
        elif url.path == "/groups.png":
            self._send(200, "image/png", ch.groups_png(self.picks_path, self.shown(q)))
        elif url.path == "/info":
            self._json({
                "ok": True, "chapter": ch.key, "agent": ch.args.agent,
                "chapters": [{"key": k, "loaded": self.library.is_loaded(k)}
                             for k in self.library.keys],
                "width": ch.width, "height": ch.height, "bounds": ch.bounds.as_dict(),
                "polys": len(ch.polys), "comps": len(ch.comps), "clusters": len(ch.clusters),
                "feet_z": ch.feet_z0,
                "groups": {g: dict(GROUPS[g], css=f"rgb{GROUPS[g]['rgb']}") for g in GROUPS},
                "open_batch": load_doc(self.picks_path)["open_batch"],
                "pieces": self.pieces_json(ch),
                "picks_path": str(self.picks_path),
            })
        elif url.path == "/markers":
            self._json({"ok": True, "markers": ch.markers(), "groups": MARKER_GROUPS})
        elif url.path == "/pick":
            self._json(self.pick(ch, q))
        else:
            self._send(404, "text/plain", b"no")

    def pick(self, ch: Chapter, q: dict) -> dict:
        u, v = float(q.get("u", [0])[0]), float(q.get("v", [0])[0])
        mode = q.get("mode", ["comp"])[0]
        show = self.shown(q)
        wx, wy = ch.to_world(u, v)
        surfaces = ch.surfaces_at(int(u), int(v))
        for surf in surfaces:  # a cut piece is off the picture; its surface is off the stack too
            surf["cut"] = any(
                lay["mask"][int(v) - lay["y"], int(u) - lay["x"]]
                and matches_z(lay["z"][:, int(v) - lay["y"], int(u) - lay["x"]], surf["z"])
                for lay in self.cut_pieces(ch, show)
                if lay["y"] <= int(v) < lay["y"] + lay["mask"].shape[0]
                and lay["x"] <= int(u) < lay["x"] + lay["mask"].shape[1])
        if q.get("z", [""])[0] != "":
            wz = float(q["z"][0])
        else:  # the topmost surface still on the picture
            live = [s for s in surfaces if s["reachable"] and not s["cut"]]
            wz = (live or surfaces)[-1]["z"] if surfaces else None
        if q.get("all", ["0"])[0] != "1":
            hidden = self.hidden_reason(surfaces, wz)
            if hidden:
                return {"ok": False, "world": [round(wx, 1), round(wy, 1)],
                        "pixel": [int(u), int(v)], "surfaces": surfaces, "why": hidden}
        poly = ch.locate(wx, wy, wz)
        if poly is None:
            return {"ok": False, "world": [round(wx, 1), round(wy, 1)], "surfaces": surfaces,
                    "pixel": [int(u), int(v)],
                    "why": "no navmesh polygon under that point"}
        # A piece already carrying a verdict, or already proposed by the rules, is judged again
        # rather than marked twice - but only while its layer is on the picture. Cut out of it,
        # there is nothing on screen to have meant to click.
        done = next((pc for pc in ch.pieces(self.picks_path)
                     if pc["comp"] is not None and pc["comp"] == poly.get("comp")), None)
        if done is not None and done["group"] not in show and q.get("all", ["0"])[0] != "1":
            return {"ok": False, "world": [round(wx, 1), round(wy, 1)],
                    "pixel": [int(u), int(v)], "surfaces": surfaces,
                    "why": (f"cut as '{GROUPS[done['group']]['label']}'"
                            + (f" - pick {done['i']}" if done["i"] is not None else "")
                            + f". Tick '{done['group']}' to judge it again.")}
        rep = describe(ch, poly, ch.selection(poly, mode), mode)
        rep["piece"] = None if done is None else {
            k: done[k] for k in ("key", "group", "i", "verdict", "note", "batch")}
        key = f"{time.time():.6f}"
        self.overlays.clear()
        self.overlays[key] = overlay_png(rep.pop("mask"))
        rep["overlay"]["key"] = key
        rep["ok"] = True
        rep["world"] = [round(wx, 1), round(wy, 1), round(poly["cz"], 1)]
        rep["pixel"] = [int(u), int(v)]
        rep["surfaces"] = surfaces
        rep["poly"] = {"idx": poly["idx"], "tile": list(poly["tile"]),
                       "level": Path(ch.tile_source.get(poly["tile"], "?")).stem,
                       "stage": poly["stage"], "area_m2": round(poly["xyarea"] / 10000.0, 2),
                       "zspread": round(poly["zspread"], 1), "flags": poly["flags"]}
        return rep

    @staticmethod
    def hidden_reason(surfaces: list[dict], wz: float | None) -> str:
        """Why the mod would not draw this pixel - empty when it would.

        `map_unreachable` defaults to hide, so a surface the reachability flood never reached is
        never on screen and marking it is wasted work.
        """
        if not surfaces:
            return "no surface at this pixel"
        if not any(s["reachable"] for s in surfaces):
            return (f"the mod hides this pixel - none of its {len(surfaces)} surface(s) is "
                    f"reachable. Tick 'show what the mod hides' to pick it anyway.")
        if wz is not None:
            near = min(surfaces, key=lambda s: abs(s["z"] - wz))
            if not near["reachable"]:
                return (f"the mod hides this surface (h{near['plane']}, Z {near['z']}) - not "
                        f"reachable. Pick a reachable one from the list, or tick 'show what the "
                        f"mod hides'.")
        return ""

    def do_POST(self) -> None:
        url = urllib.parse.urlparse(self.path)
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        ch = self.chapter_of(urllib.parse.parse_qs(url.query))
        doc = load_doc(self.picks_path)
        picks = doc["picks"]
        # Removing a pick renumbers the ones after it, so every loaded chapter's located pieces are
        # stale, not just this chapter's.
        self.library.forget_pieces()
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        if url.path == "/mark":
            i = body.pop("i", None)
            if i is not None and 0 <= int(i) < len(picks):
                # A second verdict on the same piece corrects the first; it is not new evidence.
                # Merged, not replaced: the verdict changes, the evidence the mark was made
                # from stays whatever it was.
                was = picks[int(i)]
                picks[int(i)] = {**was, **body, "chapter": ch.key, "batch": was["batch"],
                                 "ts": was["ts"], "from": was.get("from", "hand"), "revised": now}
            else:
                body.update(chapter=ch.key, batch=doc["open_batch"], ts=now)
                body.setdefault("from", "hand")
                picks.append(body)
        elif url.path == "/unmark":
            i = int(body.get("i", -1))
            if 0 <= i < len(picks):
                picks.pop(i)
        elif url.path == "/close-batch":
            doc["open_batch"] += 1
        else:
            self._send(404, "text/plain", b"no")
            return
        save_doc(self.picks_path, doc)
        self._json({"ok": True, "open_batch": doc["open_batch"], "pieces": self.pieces_json(ch)})


PAGE = r"""<!doctype html>
<meta charset="utf-8"><title>OOB picker</title>
<style>
 html,body{margin:0;height:100%;background:#15171a;color:#dfe3e8;
   font:13px/1.5 ui-monospace,Consolas,monospace}
 #wrap{display:flex;height:100%}
 #view{flex:1;overflow:hidden;position:relative;cursor:crosshair;background:#0d0f11}
 #scene{position:absolute;transform-origin:0 0}
 #scene img{position:absolute;image-rendering:pixelated;display:block}
 /* an author `display` beats the browser's own [hidden] rule, so a layer switched off stayed on */
 #scene img[hidden]{display:none}
 #base{left:0;top:0}
 /* The markers never take a click - the map under them is what is being picked. Each dot is
    counter-scaled by the zoom, so it stays the same size on screen at any magnification. */
 #marks{position:absolute;left:0;top:0;pointer-events:none}
 #marks[hidden]{display:none}
 #marks b{position:absolute;width:9px;height:9px;border-radius:50%;
   transform:translate(-50%,-50%) scale(calc(1/var(--z,1)));box-shadow:0 0 0 1px #0b0d0fcc}
 /* a marker on another storey: an outline, never a filled dot - a fill reads as evidence */
 #marks b.far{width:7px;height:7px;background:transparent!important;opacity:.45;
   box-shadow:none;border:1px solid currentColor}
 #side{width:430px;overflow:auto;padding:10px 12px;background:#1b1e22;
   border-left:1px solid #2c3137}
 h2{font-size:12px;margin:14px 0 5px;color:#8ab4f8;text-transform:uppercase;letter-spacing:.08em}
 table{border-collapse:collapse;width:100%}
 td{padding:1px 4px;vertical-align:top}
 td:first-child{color:#8b939c;white-space:nowrap;width:32%}
 .row{display:flex;gap:6px;align-items:center;margin:6px 0;flex-wrap:wrap}
 button{background:#2b3138;color:#dfe3e8;border:1px solid #3a424b;border-radius:4px;
   padding:4px 9px;cursor:pointer;font:inherit}
 button:hover{background:#353d46}
 button.oob{background:#5a2230;border-color:#83323f}
 button.ok{background:#20452c;border-color:#2f6640}
 input,select{background:#23272c;color:#dfe3e8;border:1px solid #3a424b;border-radius:4px;
   padding:3px 6px;font:inherit}
 input[type=text]{flex:1;min-width:80px}
 .pick{border-top:1px solid #2c3137;padding:3px 0;display:flex;gap:6px;align-items:baseline}
 .pick b{cursor:pointer;color:#8ab4f8}
 .oobtag{color:#ff7b91}.oktag{color:#7bd694}
 .surf{cursor:pointer;padding:1px 4px;border-radius:3px}
 .surf:hover{background:#2b3138}
 .surf.sel{background:#33404f}
 .hint{color:#6f7780}
 .lay{display:flex;gap:5px;align-items:center;padding:1px 0}
 .lay i{width:10px;height:10px;border-radius:2px;display:inline-block}
 .lay span{margin-left:auto;color:#6f7780}
 h2 button{float:right;font-size:11px;padding:1px 6px;text-transform:none;letter-spacing:0}
</style>
<div id=wrap>
  <div id=view><div id=scene><img id=base><img id=grp hidden><img id=ov hidden>
    <div id=marks hidden></div></div></div>
  <div id=side>
    <div class=row>
      <select id=chapter></select>
      <select id=mode>
        <option value=comp selected>component</option>
        <option value=poly>polygon</option>
        <option value=shelf>shelf</option>
        <option value=cluster>cluster</option>
      </select>
      <input type=text id=goto placeholder="world X Y [Z]" size=12>
      <button id=gob>go</button>
    </div>
    <div class=row>
      feet Z <input type=text id=feetz size=7>
      <button id=reshade title="re-cut the chapter at this feet Z">&#8635;</button>
      <label><input type=checkbox id=allview> show what the mod hides</label>
      <label><input type=checkbox id=seams checked> storey seams</label>
      <label><input type=checkbox id=mks> markers</label>
    </div>
    <div class=hint id=mkleg></div>
    <h2>layers <span class=hint>unticked is cut out of the map</span></h2>
    <div id=layers></div>
    <div class=hint id=busy hidden></div>
    <div class=hint id=head></div>
    <div id=report></div>
    <h2>verdict</h2>
    <div class=row>
      <input type=text id=note placeholder="note">
      <button class=oob id=mkoob>out of bounds</button>
      <button class=ok id=mkok>legit</button>
    </div>
    <h2>pieces <button id=closeb title="start a new batch: these marks move to 'earlier'"
      >close the batch</button></h2>
    <div class=row>
      <input type=text id=find placeholder="find: #component, coords, note" size=12>
      <span class=hint id=findhint></span>
    </div>
    <div id=picks></div>
  </div>
</div>
<script>
let info=null, last=null, picks=[], zoom=1, ox=0, oy=0, selZ="", chapter="", cutZ=0;
let visible=[];   // the rows the list is showing - what the search's Enter goes to
const show=new Set();   // the layers on the picture; everything else is cut out of it
const view=document.getElementById('view'), scene=document.getElementById('scene');
const base=document.getElementById('base'), ov=document.getElementById('ov');
const grp=document.getElementById('grp');

function apply(){
  scene.style.transform=`translate(${ox}px,${oy}px) scale(${zoom})`;
  scene.style.setProperty('--z',zoom);   // the marker dots counter-scale off this
}
function esc(s){ return String(s).replace(/[<&]/g,c=>c=='<'?'&lt;':'&amp;'); }
function tr(k,v){ return `<tr><td>${k}</td><td>${v}</td></tr>`; }

view.addEventListener('wheel',e=>{
  e.preventDefault();
  const r=view.getBoundingClientRect(), mx=e.clientX-r.left, my=e.clientY-r.top;
  const nz=Math.min(16,Math.max(0.02,zoom*(e.deltaY<0?1.2:1/1.2)));
  ox=mx-(mx-ox)*(nz/zoom); oy=my-(my-oy)*(nz/zoom); zoom=nz; apply();
},{passive:false});

let drag=null, moved=0;
view.addEventListener('mousedown',e=>{ drag=[e.clientX,e.clientY,ox,oy]; moved=0; });
window.addEventListener('mousemove',e=>{
  if(!drag) return;
  moved=Math.max(moved,Math.abs(e.clientX-drag[0])+Math.abs(e.clientY-drag[1]));
  ox=drag[2]+e.clientX-drag[0]; oy=drag[3]+e.clientY-drag[1]; apply();
});
window.addEventListener('mouseup',e=>{
  if(drag&&moved<4){
    const r=view.getBoundingClientRect();
    pick((e.clientX-r.left-ox)/zoom,(e.clientY-r.top-oy)/zoom);
  }
  drag=null;
});

function centreOn(u,v){
  const r=view.getBoundingClientRect();
  ox=r.width/2-u*zoom; oy=r.height/2-v*zoom; apply();
}
function toPx(x,y){
  const b=info.bounds;
  return [(y-b.min_y)*b.px_per_uu,(b.max_x-x)*b.px_per_uu];
}
// Go to a world point and pick what is under it - what the go box, a row of the list and the
// search all do, at the zoom a piece is judged at.
function jump(w){
  const [u,v]=toPx(w[0],w[1]);
  zoom=Math.max(zoom,2); centreOn(u,v); pick(u,v,w.length>2?w[2]:"");
}

function show_all(){ return document.getElementById('allview').checked; }
function busy(msg){
  const b=document.getElementById('busy');
  b.hidden=!msg; if(msg) b.textContent=msg;
}

function showq(){ return '&show='+[...show].join(','); }

function reload_base(force){
  // The standing cut's feet Z, not the box's: the box is what the NEXT cut will use, and a mark
  // must patch the picture it is standing on rather than re-cut the chapter at a new storey.
  const z=cutZ;
  const src='/map.png?ch='+encodeURIComponent(chapter)+'&z='+encodeURIComponent(z)
           +showq()+(show_all()?'&all=1':'')
           +(document.getElementById('seams').checked?'':'&seams=0')
           +(force?'&v='+Date.now():'');
  const gsrc='/groups.png?ch='+encodeURIComponent(chapter)+showq()
            +(force?'&v='+Date.now():'');
  if(show.size){ grp.src=gsrc; grp.width=info.width; grp.height=info.height; grp.hidden=false; }
  else grp.hidden=true;
  if(!force && base.getAttribute('src')===src) return;
  busy('cutting the chapter…');
  // A new cut moves the surface under every marker, so what stands on this storey is re-asked.
  base.onload=()=>{ busy(''); load_marks(); };
  base.src=src;
}

// ---- the game's own markers over the picture --------------------------------------------------
// A dot is FILLED only where the marker stands on the surface the cut drew under it; anywhere else
// - another storey, or ground a mark has cut away - it is an outline. A filled dot is evidence.
let marks=null;
const MARK_NEAR_UU=300;

async function load_marks(){
  if(!document.getElementById('mks').checked){ draw_marks(); return; }
  marks=await fetch('/markers?ch='+encodeURIComponent(chapter)).then(r=>r.json());
  draw_marks();
}
function draw_marks(){
  const box=document.getElementById('marks'), leg=document.getElementById('mkleg');
  if(!marks||!document.getElementById('mks').checked){
    box.hidden=true; leg.innerHTML=''; return;
  }
  const grp={};
  for(const [g,d] of Object.entries(marks.groups)) for(const c of d.cats) grp[c]=g;
  const css=g=>`rgb(${marks.groups[g].rgb.join(',')})`;
  const near={}, all={};
  box.innerHTML=marks.markers.map(m=>{
    const g=grp[m.cat]||'note', c=css(g);
    const on=m.dz!=null&&Math.abs(m.dz)<=MARK_NEAR_UU;
    all[g]=(all[g]||0)+1; if(on) near[g]=(near[g]||0)+1;
    return `<b class="${on?'':'far'}" style="left:${m.u}px;top:${m.v}px;`
          +`background:${c};color:${c}"></b>`;
  }).join('');
  box.hidden=false;
  leg.innerHTML=Object.entries(marks.groups).map(([g,d])=>
    `<span style="color:${css(g)}">&#9679;</span> ${near[g]||0} of ${all[g]||0} on this storey `
   +`&mdash; ${d.label}`).join('<br>');
}
document.getElementById('mks').addEventListener('change',load_marks);

async function pick(u,v,z){
  if(!info||u<0||v<0||u>=info.width||v>=info.height) return;
  if(z!==undefined) selZ=z;
  const m=document.getElementById('mode').value;
  const all=show_all()?'1':'0';
  last=await fetch(`/pick?ch=${encodeURIComponent(chapter)}&u=${u}&v=${v}&mode=${m}`
                  +`&z=${selZ}&all=${all}`+showq()).then(r=>r.json());
  report(last);
}

function surfaces(r){
  if(!r.surfaces||!r.surfaces.length)
    return '<h2>height planes</h2><p class=hint>no surface at this pixel</p>';
  let h='<h2>height planes at this pixel</h2><div>';
  for(const s of r.surfaces)
    h+=`<div class="surf${String(s.z)===String(selZ)?' sel':''}" data-z="${s.z}">`
      +`h${s.plane} &middot; Z ${s.z} &middot; code ${s.code} &middot; `
      +(s.reachable?'reachable':'<span class=hint>not reachable</span>')
      +(s.cut?' &middot; <span class=oobtag>cut</span>':'')+'</div>';
  return h+'</div>';
}

function report(r){
  const el=document.getElementById('report');
  if(!r.ok){ el.innerHTML=`<h2>piece</h2><p class=hint>${esc(r.why)}</p>`+surfaces(r);
             ov.hidden=true; return; }
  const st=Object.entries(r.stages).map(([k,n])=>`${k} ${n}`).join(', ');
  const lv=r.levels.map(([k,n])=>`${esc(k)} <span class=hint>${n}</span>`).join('<br>');
  let h='<h2>piece</h2>';
  if(r.piece&&r.piece.mode&&r.piece.mode!=document.getElementById('mode').value){
    // The rule found this as a shelf; judging it as one component would answer a different question.
    document.getElementById('mode').value=r.piece.mode;
    if(r.pixel) pick(r.pixel[0],r.pixel[1]);
    return;
  }
  if(r.piece){
    const p=r.piece, g=info.groups[p.group];
    h+=`<p><span style="color:${g.css}">&#9646;</span> `
      +(p.verdict?`already <b>${p.verdict}</b>`:'proposed by the rules')
      +(p.i!=null?` &middot; pick ${p.i}`:'')+(p.batch!=null?` &middot; batch ${p.batch}`:'')
      +`<br><span class=hint>a verdict now replaces it</span></p>`;
  }
  h+='<table>';
  h+=tr('world',`X ${r.world[0]} Y ${r.world[1]} Z ${r.world[2]}`);
  h+=tr('size',`${r.area_m2} m&sup2; &middot; ${r.polys} polys &middot; ${r.comps} comp`);
  h+=tr('Z range',`${r.z[0]} .. ${r.z[1]} uu`);
  h+=tr('bbox X',`${r.bbox[0]} .. ${r.bbox[2]}`);
  h+=tr('bbox Y',`${r.bbox[1]} .. ${r.bbox[3]}`);
  h+=tr('stage',st);
  h+=tr('on map',`${r.raster.px_on_map}/${r.raster.px} px &middot; `
                +`reachable ${r.raster.reachable_pct}%`);
  h+=tr('the mod draws', r.raster.px_reachable
    ? `${r.raster.px_reachable} px`
    : '<b class=oobtag>nothing</b> &mdash; every pixel is hidden');
  h+='</table><h2>pipeline</h2><table>';
  if(r.comp) h+=tr('component',`#${r.comp.id} &middot; ${r.comp.polys} polys &middot; `
    +`${r.comp.area_m2} m&sup2; &middot; ${r.comp.kept?'kept':'dropped'}`
    +`${r.comp.seeded?' &middot; seeded':''}`);
  // What made it `seeded`, by name: the same window the seeding uses, so a wall note reads as one.
  h+=tr('markers', r.markers&&r.markers.length
    ? r.markers.map(m=>`${esc(m.name||m.cat)} <span class=hint>${esc(m.cat)} &middot; Z ${m.z}`
        +`${m.dz?` &middot; ${m.dz>0?'+':''}${m.dz} uu off the piece`:' &middot; on it'}`
        +`</span>`).join('<br>')
    : '<span class=hint>none in the seed window</span>');
  if(r.cluster) h+=tr('cluster',`#${r.cluster.id} &middot; ${r.cluster.members} comps &middot; `
    +`${r.cluster.area_m2} m&sup2;<br>${esc(r.cluster.why||'')}`
    +`${r.cluster.has_largest?'<br>holds the largest component':''}`);
  h+=tr('polygon',`#${r.poly.idx} &middot; ${r.poly.area_m2} m&sup2; &middot; `
    +`flags ${r.poly.flags} &middot; zspread ${r.poly.zspread}`);
  h+=tr('tile',`${r.poly.tile.join(', ')} &middot; ${r.tiles} in the piece`);
  h+=tr('level',lv||esc(r.poly.level));
  el.innerHTML=h+'</table>'+surfaces(r);
  document.getElementById('feetz').value=r.world[2];   // what the re-cut button would use
  const o=r.overlay;
  ov.src=`/overlay.png?k=${o.key}`; ov.style.left=o.x+'px'; ov.style.top=o.y+'px';
  ov.width=o.w; ov.height=o.h; ov.hidden=false;
}

document.getElementById('report').addEventListener('click',e=>{
  const d=e.target.closest('.surf');
  if(d&&last&&last.pixel) pick(last.pixel[0],last.pixel[1],d.dataset.z);
});
document.getElementById('mode').addEventListener('change',()=>{
  if(last&&last.pixel) pick(last.pixel[0],last.pixel[1]);
});
document.getElementById('allview').addEventListener('change',()=>{
  reload_base();
  if(last&&last.pixel) pick(last.pixel[0],last.pixel[1]);
});
document.getElementById('reshade').addEventListener('click',()=>{
  cutZ=document.getElementById('feetz').value||info.feet_z; reload_base(true);
});
document.getElementById('seams').addEventListener('change',()=>reload_base());
document.getElementById('feetz').addEventListener('keydown',e=>{
  if(e.key=='Enter'){ cutZ=document.getElementById('feetz').value||info.feet_z; reload_base(true); }
});
document.getElementById('gob').addEventListener('click',()=>{
  const m=document.getElementById('goto').value.trim().split(/[ ,]+/).map(Number);
  if(m.length<2||m.some(isNaN)) return;
  jump(m);
});

// The search narrows the list in place. A piece carries two numbers - the pick it is in the file
// and the component it is on the map - and both are printed on the row, so a whole number (with or
// without the `#` the row prints) matches either. Anything else is matched against the row's text.
function find_q(){ return document.getElementById('find').value.trim().toLowerCase(); }
function matches(p){
  const q=find_q();
  if(!q) return true;
  const n=Number(q.replace(/^#/,''));
  if(Number.isInteger(n)&&(p.comp===n||p.i===n)) return true;
  return ['#'+(p.comp!=null?p.comp:''),p.world.join(' '),p.verdict||'rules',p.note||'',p.group]
         .join(' ').toLowerCase().includes(q);
}
document.getElementById('find').addEventListener('input',()=>render_pieces(picks));
document.getElementById('find').addEventListener('keydown',e=>{
  if(e.key=='Enter'&&visible.length==1) jump(visible[0].world);   // one hit answers itself
});

async function mark(verdict){
  if(!last||!last.ok) return;
  const b={verdict, note:document.getElementById('note').value,
           world:last.world, pixel:last.pixel, mode:last.mode, surfaces:last.surfaces,
           polys:last.polys, area_m2:last.area_m2, z:last.z, bbox:last.bbox,
           stages:last.stages, levels:last.levels, tiles:last.tiles,
           comp:last.comp, cluster:last.cluster, poly:last.poly, raster:last.raster};
  if(last.piece){
    if(last.piece.i!=null) b.i=last.piece.i;               // a second verdict corrects the first
    if(last.piece.group=='rules') b.from='rule';           // the rules found it, the user judged it
  }
  const r=await fetch('/mark?ch='+encodeURIComponent(chapter),
                      {method:'POST',body:JSON.stringify(b)}).then(r=>r.json());
  document.getElementById('note').value=''; took(r);
  ov.hidden=true; last=null; reload_base(true);
}
document.getElementById('mkoob').onclick=()=>mark('oob');
document.getElementById('mkok').onclick=()=>mark('ok');

document.getElementById('closeb').onclick=async()=>{
  const r=await fetch('/close-batch?ch='+encodeURIComponent(chapter),
                      {method:'POST',body:'{}'}).then(r=>r.json());
  took(r); reload_base(true);
};

function took(r){ if(r.open_batch!=null) info.open_batch=r.open_batch; render_pieces(r.pieces); }

function render_pieces(ps){
  picks=ps;   // every piece of this chapter; `p.i` is its index in the file, what /unmark takes
  document.getElementById('layers').innerHTML=Object.entries(info.groups).map(([g,d])=>
    `<label class=lay><input type=checkbox data-g="${g}"${show.has(g)?' checked':''}>`
   +`<i style="background:${d.css}"></i>${g} <span class=hint>${d.label}</span>`
   +`<span>${ps.filter(p=>p.group==g).length}</span></label>`).join('');
  // The list is the working set: this batch, plus whatever layer is on the picture to be judged.
  const working=ps.filter(p=>p.group=='batch'||show.has(p.group));
  const rows=visible=working.filter(matches)
              .sort((a,b)=>b.drawn-a.drawn);   // the queue is worth working top down
  const q=find_q();
  // A search that finds nothing here but something in an unticked layer says so - the alternative
  // is an empty list and no reason for it.
  const other=q?ps.filter(p=>!working.includes(p)&&matches(p)):[];
  document.getElementById('findhint').innerHTML=!q?''
    :`${rows.length} of ${working.length}`+(other.length
      ?` &middot; ${other.length} more in ${[...new Set(other.map(p=>p.group))].join(', ')}`
       +` <span class=oobtag>not ticked</span>`:'');
  document.getElementById('picks').innerHTML=rows.map(p=>
    `<div class=pick><span style="color:${info.groups[p.group].css}">&#9646;</span>`
   +(p.i!=null?`<span class=hint>pick ${p.i}</span>`:'')
   +`<b data-k="${esc(p.key)}">${p.world[0]}, ${p.world[1]}, ${p.world[2]}</b>`
   +`<span class="${p.verdict=='oob'?'oobtag':p.verdict=='ok'?'oktag':'hint'}">`
   +`${p.verdict||'rules'}</span>`
   +`<span class=hint>${p.comps?p.comps+' comps ':p.comp!=null?'#'+p.comp+' ':''}`
   +`${p.area_m2} m&sup2; &middot; `
   +`${p.drawn?p.drawn+' px drawn':'<b class=oobtag>not drawn</b>'} ${esc(p.note||'')}</span>`
   +(p.i!=null?`<button data-del="${p.i}">&times;</button>`:'')+'</div>').join('')
   || (q?'<p class=hint>nothing here matches</p>'
        :'<p class=hint>nothing in this batch - tick a layer to work through it</p>');
}

document.getElementById('layers').addEventListener('change',e=>{
  const g=e.target.dataset.g;
  if(!g) return;
  e.target.checked?show.add(g):show.delete(g);
  render_pieces(picks); reload_base();
});

document.getElementById('picks').addEventListener('click',async e=>{
  if(e.target.dataset.del!==undefined){
    const r=await fetch('/unmark?ch='+encodeURIComponent(chapter),{method:'POST',
      body:JSON.stringify({i:+e.target.dataset.del})}).then(r=>r.json());
    took(r); reload_base(true); return;
  }
  const b=e.target.closest('b');
  if(!b) return;
  jump(picks.find(q=>q.key==b.dataset.k).world);
});

async function open_chapter(key){
  ov.hidden=true; last=null; selZ=""; marks=null; draw_marks();
  document.getElementById('report').innerHTML='';
  busy(`loading ${key||'the chapter'}… (one the tool has not seen yet takes 10-25 s)`);
  const got=await fetch('/info'+(key?'?ch='+encodeURIComponent(key):''))
                  .then(r=>r.json());
  if(!got.ok){ busy(got.why); return; }
  info=got; chapter=info.chapter;
  base.width=info.width; base.height=info.height;
  document.getElementById('feetz').value=info.feet_z; cutZ=info.feet_z;
  document.getElementById('chapter').innerHTML=info.chapters.map(c=>
    `<option value="${c.key}"${c.key==info.chapter?' selected':''}>`
   +`${c.key}${c.loaded?'':' (load)'}</option>`).join('');
  document.getElementById('head').innerHTML=
    `${info.chapter} / ${info.agent} &middot; ${info.width}&times;${info.height} px<br>`
   +`${info.polys} polys &middot; ${info.comps} components &middot; ${info.clusters} clusters<br>`
   +`batch ${info.open_batch} &middot; picks &rarr; ${esc(info.picks_path)}`;
  document.title=`OOB picker - ${info.chapter}`;
  render_pieces(info.pieces);
  const r=view.getBoundingClientRect();
  zoom=Math.min(r.width/info.width,r.height/info.height); ox=0; oy=0; apply();
  reload_base();
}

document.getElementById('chapter').addEventListener('change',e=>open_chapter(e.target.value));

open_chapter(new URLSearchParams(location.search).get('ch')||'');
</script>
"""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chapter", default="chapter1",
                    help="chapter the page opens on; the rest load when the page asks for them")
    ap.add_argument("--maps", type=Path, default=DEFAULT_MAPS, help="shipped maps/ directory")
    ap.add_argument("--input", type=Path, default=None,
                    help="tile-dump root with agent subdirs (default: guessed from the chapter)")
    ap.add_argument("--picks", type=Path, default=DEFAULT_PICKS, help="verdict file to append to")
    ap.add_argument("--port", type=int, default=8731)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args(argv)

    Handler.library = Library(args.maps, args.input)
    Handler.opening = args.chapter
    Handler.picks_path = args.picks
    try:
        opening = Handler.library.get(args.chapter)
    except ChapterUnavailable as exc:
        sys.exit(str(exc))
    # The first cut costs ~12 s. Pay it here, so the page never waits on it.
    opening.cut(opening.feet_z0, "hide",
                [pc for pc in opening.pieces(args.picks) if pc["group"] != "legit"])

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    url = f"http://127.0.0.1:{args.port}/"
    with Server(("127.0.0.1", args.port), Handler) as srv:
        print(f"picker on {url}   (Ctrl-C to stop)", flush=True)
        if not args.no_browser:
            threading.Timer(0.4, lambda: webbrowser.open(url)).start()
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
