"""
oob_chapter.py - one chapter rebuilt from the dumps: its geometry, its pictures, its pieces.

`Chapter` stages a chapter exactly as `build_map.build_chapter` stages it, up to the island filter,
and then answers the three questions the picker asks: the cut the mod's own full map would draw
(with any piece struck out of the height stack), which navmesh polygon is under a clicked pixel, and
what the piece around it measures. `Library` keeps every chapter the page has asked for - a chapter
costs 11-24 s to build, so none is ever dropped.

One cut at a time, under `_cut_lock`: a page reload fires its requests together, and two threads
patching the standing cut in place would each see half the other's work.
"""

from __future__ import annotations

import glob
import io
import json
import math
import threading
import time
from pathlib import Path

import numpy as np
from PIL import Image

import blocks
import build_map
import mapfmt
import render
import slice_preview as sp
from oob_measure import (PICK_GRID_UU, STACK_BUDGET_PX, drawn_surfaces, inner, mask_of, matches_z,
                         overlap, overlay_png, raster_masks)
from oob_rules import RuleSet
from oob_verdicts import GROUPS, box_of, group_of, load_doc

HERE = Path(__file__).resolve().parent

# What a marker says about the ground under it. The picture paints the GROUP, not the category, so
# one glance answers "is anything here and what kind"; the category and the name are in the report.
MARKER_GROUPS = {
    "site":  {"rgb": (56, 189, 248), "label": "shrine, boss, npc, door, ladder",
              "cats": ["shrine", "boss", "elite", "npc", "fog_gate", "benediction_door",
                       "mystery_gate", "door", "lift", "ladder", "hidden", "bamboozling",
                       "cuckoo"]},
    "loot":  {"rgb": (250, 204, 21), "label": "chest, item, pickup",
              "cats": ["chest", "key", "item", "weapon", "armour", "amulet", "jade", "spell",
                       "ammo", "material", "consumable", "harvest"]},
    "enemy": {"rgb": (248, 113, 113), "label": "enemy", "cats": ["enemy"]},
    "note":  {"rgb": (148, 163, 184), "label": "note, other - hangs on a wall",
              "cats": ["note", "other"]},
}

# Finished pictures kept per chapter, keyed by the feet Z, the hidden pieces and the seam switch.
# Only the most recent cut keeps its slice arrays; the rest are a PNG each, so ticking a layer back
# off costs nothing the second time.
CUT_CACHE = 8

BLOCKS_JSON = HERE.parents[1] / "markers" / "blocks.json"
WALL_RGB = (217, 70, 239)
WALL_SHELL_LO = -300.0
WALL_SHELL_HI = 500.0

# ... and the budget the rule layer alone is measured on. It is one piece the size of the chapter,
# so the slot count is what a whole storey of the cut lives in: chapter 4 at two slots reads 2 035
# surfaces short of the truth and at four reads it exactly. 120 M values is 480 MB of float32, held
# for the chapter on screen and dropped from every other one.
RULE_STACK_PX = 120_000_000

# Height-stack values a chapter keeps across measured pieces. One piece takes up to
# STACK_BUDGET_PX of them, so a session of clicks on whole clusters would hold gigabytes. A piece
# the page can cut is never dropped - the cut reads its stack - and the rest go oldest first.
SHAPE_CACHE_PX = 2 * STACK_BUDGET_PX

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
        self._shelf_of: dict[int, int] = {}
        # What a build run right now would cut this chapter with, and what the page starts from.
        self.defaults = RuleSet(small=build_map.SMALL_UNSEEDED_UU2)
        self.rules = self.defaults      # the thresholds the page is currently judging
        self._home: set[int] | None = None            # components a marker stands on
        self._escape: dict[float, "render.Routes"] = {}   # how kept ground joins up, by fall cap
        self._walld: dict[int, float] | None = None   # median distance to a wall, per component
        self._catch: dict[str, dict[int, str]] = {}   # what a rule set takes, by its key
        self._rule_layer: tuple[str, dict] | None = None   # that catch as one layer, measured
        self._drawn: int | None = None                # px the mod draws over every height plane
        self._kept_area: float | None = None          # m2 of ground the island filter keeps
        self._group_png: tuple[tuple, bytes] | None = None  # the shown layers, washed
        self._walls: object = False           # the chapter's invisible walls, loaded on first use
        self._shapes: dict[str, dict] = {}     # a piece's mask and measurement, by its key
        # One cut at a time. A page reload fires its requests together, and two threads patching the
        # standing cut in place would each see half the other's work. Re-entrant because the walls
        # are measured against the standing cut and take the same lock to keep it still.
        self._cut_lock = threading.RLock()

        self.input_root = input_root or build_map.chapter_input_root(self.args.input, key)
        self._load()

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
            unanchored=a.flat_plane_unanchored, shadow=a.flat_plane_shadow,
        )
        rest = []
        for p in polys:
            if p["plane"]:
                p["stage"] = "plane"
            else:
                rest.append(p)

        # stage 1b - ground inside an invisible wall, cut where the build cuts it: before the
        # components are found, so the picker's islands are the build's islands.
        walled = build_map.cut_inside_walls(rest, str(BLOCKS_JSON) if BLOCKS_JSON.exists() else "",
                                            self.key)
        if len(walled) != len(rest):
            kept = {id(p) for p in walled}
            for p in rest:
                if id(p) not in kept:
                    p["stage"] = "wall"
            rest = walled

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
            # The wall rule is a proposal here, not a cut: `cut_oob=False` keeps every piece on the
            # picture, and `wall_dist` is kept so the layer can draw what the build would take.
            wall_dist=None, wall_far=a.wall_far,
        )
        self.wall_dist = blocks.distance_to_walls(
            self.walls(), np.array([[p["cx"], p["cy"], p["cz"]] for p in rest]),
            where=~self.walls().plate) if self.walls() is not None and rest else None
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

    # ---- the cut rules, at whatever thresholds the page is holding ---------------

    def home(self) -> set[int]:
        """The components a marker stands on - where the escape costs are measured from."""
        if self._home is None:
            self._home = render.standing_components(self.rest, self.seeds)
        return self._home

    def escape(self, fall: float) -> "render.Routes":
        """How kept ground joins up at this fall cap. 6-8 s the first time each is asked for."""
        got = self._escape.get(fall)
        if got is None:
            t0 = time.time()
            got = self._escape[fall] = render.escape_routes(
                self.rest, self.keep_ids, self.home(), grid=self.args.island_grid, max_fall=fall,
                links=render.ladder_links(self.rest, self.keep_ids, self.seeds))
            print(f"[{self.key}] escape costs at fall {fall:g} uu: {len(got.cost)} components have "
                  f"a way home ({time.time() - t0:.1f}s)", flush=True)
        return got

    def walld(self) -> dict[int, float]:
        """The median distance to an invisible wall per component; empty without the walls."""
        if self._walld is None:
            self._walld = render.wall_medians(self.rest, self.keep_ids, self.wall_dist)
        return self._walld

    def catch(self, rs: RuleSet) -> dict[int, str]:
        """Which components these thresholds take, and which rule takes each.

        The marker rescue runs here as it runs in the build, so the preview cannot show ground cut
        away from under a marker that the pipeline would put straight back.
        """
        got = self._catch.get(rs.key)
        if got is None:
            why = render.out_of_bounds(
                self.comps, self.keep_ids, self.escape(rs.fall), self.walld(),
                escape_climb=rs.climb, wall_far=rs.wall_far, small_area=rs.small)
            left = self.keep_ids - set(why)
            render.marker_rescue(self.rest, self.comps, left, self.seeds,
                                 cover_z=render.DEFAULT_ISLAND_COVER_Z)
            for cid in left:
                why.pop(cid, None)      # put back: it holds the only surface under a marker
            got = self._catch[rs.key] = why
        return got

    def rule_of(self, cid: int | None) -> str | None:
        """Which rule takes this component at the thresholds the page is holding, if any."""
        return None if cid is None else self.catch(self.rules).get(cid)

    def rule_layer(self, rs: RuleSet) -> dict | None:
        """The whole catch as ONE layer of the picture, and what it costs the map.

        A layer, not a piece each: the catch is thousands of components and the page needs the sum
        of them - the ground that stops being drawn - not a list to click through. It is measured
        the way every piece is, so `drawn` here and `drawn` on a mark mean the same thing.
        """
        return self.layer_of(f"rules:{rs.key}", self.catch(rs))

    def layer_of(self, key: str, ids: "dict[int, str] | set[int]") -> dict | None:
        """Any set of components as one layer of the picture, measured. One is held at a time.

        `ids` may carry each component's reason - what `catch` returns - and then the layer counts
        them per rule.
        """
        if self._rule_layer is not None and self._rule_layer[0] == key:
            return self._rule_layer[1]
        sel = [q for cid in ids for q in self.comp_polys.get(cid, [])]
        if not sel:
            self._rule_layer = (key, None)
            return None
        t0 = time.time()
        x0, y0, m, z = mask_of(self, sel, budget=RULE_STACK_PX)
        lit, reach = raster_masks(self, x0, y0, m, z)
        why = list(ids.values()) if isinstance(ids, dict) else []
        lay = dict(key=key, group="rules", verdict="", comp=None, comps=len(ids),
                   area_m2=round(sum(q["xyarea"] for q in sel) / 10000.0, 1),
                   polys=len(sel), px=int(m.sum()), px_on_map=int(lit.sum()),
                   drawn=drawn_surfaces(self, x0, y0, m, z), x=x0, y=y0, mask=m, z=z, reach=reach,
                   by_rule={r: why.count(r) for r in sorted(set(why))})
        lay["drawn_pct"] = round(100.0 * lay["drawn"] / max(1, self.drawn_total()), 2)
        lay["slots"] = int(z.shape[0])
        print(f"[{self.key}] {key}: {len(ids)} components, {lay['area_m2']:.0f} m2, "
              f"{lay['drawn']} of {self.drawn_total()} drawn surfaces off the map "
              f"({lay['drawn_pct']:.2f}%, {lay['slots']} stack slots)  "
              f"({time.time() - t0:.1f}s)", flush=True)
        self._rule_layer = (key, lay)
        return lay

    def kept_area_m2(self) -> float:
        """The chapter's walkable ground before the cut rules - what a piece is a share OF."""
        if self._kept_area is None:
            self._kept_area = sum(c["area"] for c in self.comps
                                  if c["id"] in self.keep_ids) / 10000.0
        return self._kept_area

    def drawn_total(self) -> int:
        """Pixels the mod draws over every height plane - what a cut is a share OF."""
        if self._drawn is None:
            n = 0
            for i in range(len(self.plane_paths)):
                raw = self.plane(i)
                n += int((((raw & self.z_code_mask) != 0)
                          & ((raw & self.reach_bit) != 0)).sum())
            self._drawn = n
        return self._drawn

    def pieces(self, picks_path: Path) -> list[dict]:
        """Every piece the page can show or cut, located and tagged with its layer.

        A mark is re-located from the world point it stored, never from a component id - ids are
        assigned in load order and a regeneration renumbers them. A rule proposal has no stored
        point, so it carries the centre of its largest polygon for the page to jump to.

        Marks only. What the rules take is not a list of pieces but one layer over all of them -
        `rule_layer` - because the catch is thousands of components and the question about it is
        what the map loses, not which of them to click.
        """
        doc = load_doc(picks_path)
        sig = (len(doc["picks"]), doc["open_batch"])
        if self._pieces is not None and self._pieces[0] == sig:
            return self._pieces[1]
        out: list[dict] = []
        for i, p in enumerate(doc["picks"]):
            w = p.get("world") or []
            if p.get("chapter") != self.key or len(w) < 3:
                continue
            common = dict(i=i, verdict=p.get("verdict", "oob"), note=p.get("note", ""),
                          world=[float(w[0]), float(w[1]), float(w[2])], batch=p["batch"])
            box = box_of(p)
            if box is not None:
                # A region says where the ground is, not which component it belongs to, so there is
                # nothing to re-locate: the box IS the mark.
                key, sel = self.box_sel(box)
                if not sel:
                    continue
                pc = dict(self.shape_of(key, sel), mode="box", box=box, **common)
                comp = None
            else:
                poly = self.locate(float(w[0]), float(w[1]), float(w[2]))
                if poly is None:
                    continue
                comp = self.comps[poly["comp"]] if "comp" in poly else None
                pc = self._piece(poly, p.get("mode", "comp"), **common)
            pc["group"] = group_of(p, comp, doc["open_batch"], pc["drawn_pct"])
            out.append(pc)
        self._pieces = (sig, out)
        return out

    def _piece(self, poly: dict, mode: str, **rest) -> dict:
        """One piece: the ground's own measurement, plus the verdict standing on it.

        The shape belongs to the ground and the verdict to the person who gave it, so only the
        layer, the note and the pick index are built again on every read of the file.
        """
        return dict(self.shape(poly, mode), mode=mode, **rest)

    def forget_pieces(self) -> None:
        self._pieces = None

    def forget_rule_layer(self) -> None:
        """Drop the measured catch. Half a gigabyte of height stack, and only one is ever wanted."""
        self._rule_layer = None

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

    def walls(self):
        """The chapter's invisible walls, loaded once. `None` when `blocks.json` has none."""
        if self._walls is False:
            self._walls = blocks.load(BLOCKS_JSON, self.key) if BLOCKS_JSON.exists() else None
            if self._walls is not None:
                print(f"[{self.key}] {len(self._walls)} invisible walls "
                      f"({int(self._walls.cube.sum())} cubes)", flush=True)
        return self._walls

    def walls_png(self, feet_z: float, unreachable: str, layers: list[dict],
                  seams: bool) -> bytes:
        """The walls over the standing cut: what blocks a player on the ground the map draws.

        The test is the one the flood will run - a box filling any part of the body space above the
        surface - so the picture is the rule's own answer, not an illustration of it.
        """
        w = self.walls()
        if w is None:
            return b""
        with self._cut_lock:
            self.cut(feet_z, unreachable, layers, seams)  # the picture the walls are measured on
            st = self._cut
            sig = ("walls", st["feet_z"], st["unreachable"], tuple(st["keys"]))
            done = self._pngs.get(sig)
            if done is not None:
                return done
            t0 = time.time()
            # The storey's own Z where the map draws nothing, so the shell in the gaps is on the
            # picture too.
            zr = np.where(np.isfinite(st["z_pick"]), st["z_pick"], np.float32(feet_z))
            shell = blocks.band_mask(w, self.bounds, zr, WALL_SHELL_LO, WALL_SHELL_HI,
                                     where=~w.plate)
            body = blocks.standing_mask(w, self.bounds, st["z_pick"])
            rgba = np.zeros((self.height, self.width, 4), dtype=np.uint8)
            r, g, b = WALL_RGB
            rgba[shell] = (r, g, b, 55)
            rgba[shell & ~inner(shell)] = (r, g, b, 170)
            rgba[body] = (r, g, b, 140)
            rgba[body & ~inner(body)] = (r, g, b, 250)
            buf = io.BytesIO()
            Image.fromarray(rgba, "RGBA").save(buf, format="PNG")
            print(f"[{self.key}] walls over the cut: {int(body.sum())} px blocked, "
                  f"{int(shell.sum())} px of shell at this storey "
                  f"({time.time() - t0:.1f}s)", flush=True)
            return self._remember(sig, buf.getvalue())

    def groups_png(self, picks_path: Path, show: set[str]) -> bytes:
        """The shown layers washed over the map, each in its own colour.

        A shown layer is drawn by the slicer like any other ground - this only says which ground
        belongs to which layer, so a piece can be found and judged. It is a separate picture from
        the cut so that ticking a layer does not re-shade the map underneath it.
        """
        pieces = list(self.pieces(picks_path))
        if "rules" in show:
            lay = self.rule_layer(self.rules)
            if lay is not None:
                pieces.append(lay)
        key = (self._pieces[0], frozenset(show), self.rules.key)
        if self._group_png is not None and self._group_png[0] == key:
            return self._group_png[1]
        rgba = np.zeros((self.height, self.width, 4), dtype=np.uint8)
        for pc in pieces:
            if pc["group"] not in show:
                continue
            # The geometry is from before the cut tightened the chapter's bounds, so a piece can
            # sit wholly or partly off the picture. What is on it is what can be painted.
            met = overlap((0, 0, self.width, self.height), pc)
            if met is None:
                continue
            (ix0, iy0, ix1, iy1), (mx0, my0, mx1, my1) = met
            m, dr = pc["mask"][my0:my1, mx0:mx1], pc["reach"][my0:my1, mx0:mx1]
            sub = rgba[iy0:iy1, ix0:ix1]
            r, g, b = GROUPS[pc["group"]]["rgb"]
            # Ground the mod hides gets an OUTLINE and no fill. Measured the other way first: a
            # wash of alpha 18 over the page's near-black still reads as a solid piece of map, and
            # a piece the mod draws 1 % of then looks like a piece it draws. Only the drawn part is
            # filled, so the colour on screen is always ground that is on screen.
            sub[m & ~inner(m)] = (r, g, b, 90)
            sub[dr] = (r, g, b, 95)
            sub[dr & ~inner(dr)] = (r, g, b, 240)
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

    def selection(self, poly: dict, mode: str) -> tuple[str, list[dict]]:
        """The piece a pick highlights - its key and its polygons.

        The piece is the polygon, its component, its shelf or its cluster, and the key names that
        ground rather than the click that found it. That is what lets one measurement serve a rule
        proposal, a mark re-located onto the same ground, and every click on it.
        """
        if mode == "poly" or "comp" not in poly:
            return f"poly:{poly['idx']}", [poly]
        comp = self.comps[poly["comp"]]
        if mode == "shelf":
            self.shelves()
            g = self._shelf_of.get(poly["comp"])
            if g is not None:
                return (f"shelf:{g}",
                        [q for cid in self._shelves[g] for q in self.comp_polys.get(cid, [])])
        if mode == "cluster" and comp.get("cluster") is not None:
            out: list[dict] = []
            for cid in self.clusters[comp["cluster"]]["members"]:
                out.extend(self.comp_polys.get(cid, []))
            return f"cluster:{comp['cluster']}", out
        return f"comp:{poly['comp']}", self.comp_polys.get(poly["comp"], [poly])

    def box_sel(self, box: dict) -> tuple[str, list[dict]]:
        """The ground inside a region verdict, and the key that names that region.

        `render.polys_in_boxes` decides it, the same call the build cuts with, so the picture the
        region is judged on is the ground the build will take.
        """
        key = (f"box:{box['x0']:.0f},{box['y0']:.0f},{box['x1']:.0f},{box['y1']:.0f}"
               f",{box['z0']:.0f},{box['z1']:.0f}")
        return key, render.polys_in_boxes(self.rest, [box])

    def shape(self, poly: dict, mode: str) -> dict:
        """The pixels of a piece and what the height planes say about them, measured once.

        `px_on_map` is the pixels the planes hold at one of the piece's own heights and `drawn` the
        ones of those the mod actually puts on screen - the only number that says whether cutting
        the piece changes anything. A rule proposal has never been measured, and a mark's own stored
        figure is from whenever it was made, so both are taken here.

        Kept against the ground's own key: a second click on a piece, and a mark made on one the
        rules already proposed, re-use the measurement instead of rasterising it again.
        """
        key, sel = self.selection(poly, mode)
        return self.shape_of(key, sel, poly.get("comp"))

    def shape_of(self, key: str, sel: list[dict], comp: int | None = None) -> dict:
        """The same measurement over any ground that has a name - a piece, or a region."""
        got = self._shapes.pop(key, None)
        if got is not None:
            self._shapes[key] = got          # re-inserted: the cache drops the least recent first
        else:
            x0, y0, m, z = mask_of(self, sel)
            lit, reach = raster_masks(self, x0, y0, m, z)
            px, on_map, drawn = int(m.sum()), int(lit.sum()), int(reach.sum())
            got = self._shapes[key] = dict(
                key=key, sel=sel, comp=comp,
                comps=len({q.get("comp") for q in sel}),
                area_m2=round(sum(q["xyarea"] for q in sel) / 10000.0, 1),
                px=px, px_on_map=on_map, drawn=drawn,
                drawn_pct=round(100.0 * drawn / max(1, px), 1),
                reachable_pct=round(100.0 * drawn / max(1, on_map), 1),
                x=x0, y=y0, mask=m, z=z, reach=reach, overlay=None)
            self._trim_shapes()
        return got

    def overlay(self, poly: dict, mode: str) -> bytes:
        """The piece's highlight, drawn on the first pick that asks to see it."""
        return self.overlay_of(self.shape(poly, mode))

    def overlay_of(self, sh: dict) -> bytes:
        if sh["overlay"] is None:
            sh["overlay"] = overlay_png(sh["mask"])
        return sh["overlay"]

    def _trim_shapes(self) -> None:
        """Drop the least recent measurements until the kept height stacks fit the budget."""
        needed = {pc["key"] for pc in (self._pieces[1] if self._pieces is not None else [])}
        held = sum(g["z"].size for g in self._shapes.values())
        for key in list(self._shapes):
            if held <= SHAPE_CACHE_PX:
                break
            if key not in needed:
                held -= self._shapes.pop(key)["z"].size


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
            for other, ch in self._loaded.items():
                if other != key:
                    ch.forget_rule_layer()
            return self._loaded[key]

    def is_loaded(self, key: str) -> bool:
        return key in self._loaded

    def forget_pieces(self) -> None:
        for ch in self._loaded.values():
            ch.forget_pieces()
