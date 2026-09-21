"""
oob_pick.py - click a spot on a shipped chapter map and see the whole navmesh piece that draws it.

The map ships as a picture, so a slab the player can never stand on is indistinguishable from a
floor once it is baked. This tool puts the picture back on top of the geometry it came from: it
reproduces `build_map.py`'s chapter build up to the island filter, serves the chapter on localhost
as the mod's own full map draws it - the height planes cut at a feet Z, shaded by absolute Z, with
the ground the reachability flood never reached left out - and resolves a clicked pixel to the
navmesh polygon under it, then to that polygon's connected component and bridged cluster. The whole
piece is highlighted and described - area, world bounds, Z range, which streamed level and navmesh
tiles feed it, which pipeline stage let it through, and the height code plus reachable bit the
runtime actually reads at that pixel.

Verdicts go to `oob_picks.json` next to this file: a world point and the chapter it belongs to, not
a component id, because component ids are assigned in load order and do not survive a regeneration.

Every chapter in `maps.json` is reachable from the page's chapter box. One is loaded at startup and
the rest the first time the page asks for them - 11-24 s of geometry, then ~7 s for the first cut.
A chapter already loaded stays loaded, so switching back is instant.

The tool only reads the map and the dumps. The one thing it writes is `oob_picks.json`.

This file is the server: the HTTP surface and the command line. The rest is beside it -
`oob_chapter.py` the geometry and the pictures, `oob_measure.py` what a piece covers,
`oob_verdicts.py` the verdict file, `oob_page.html` the page.

Run:
    python oob_pick.py --chapter chapter1
"""

from __future__ import annotations

import argparse
import http.server
import json
import socketserver
import sys
import threading
import time
import urllib.parse
import webbrowser
from pathlib import Path

import render

from oob_chapter import (MARKER_GROUPS, SURFACE_CAP_M2, Chapter, ChapterUnavailable,
                         Library)
from oob_measure import describe, matches_z
from oob_rules import score
from oob_verdicts import GROUPS, load_doc, save_doc

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_MAPS = REPO / "maps"
DEFAULT_PICKS = HERE / "oob_picks.json"
PAGE_HTML = HERE / "oob_page.html"

# A hand verdict over this share of a chapter's walkable ground is asked again before it lands.
BIG_PIECE_PCT = 5.0


def cut_layers(ch: Chapter, picks_path: Path, show: set[str]) -> list[dict]:
    """What comes out of the picture: every piece being cut, plus what the rules take.

    Ground judged legitimate stays, and so does a piece that is only being ASKED about - an open
    question must not change the map behind the back of the person answering it. The rules are one
    layer over their whole catch, and like any layer, ticking them puts the ground back on screen.
    """
    out = [pc for pc in ch.pieces(picks_path)
           if pc["group"] not in show and pc["verdict"] not in ("ok", "ask")]
    if "rules" not in show:
        lay = ch.rule_layer(ch.rules)
        if lay is not None:
            out.append(lay)
    return out


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
        return cut_layers(ch, self.picks_path, show)

    def pieces_json(self, ch: Chapter) -> list[dict]:
        """This chapter's pieces for the page - no masks, and the pick index the file uses."""
        return [{k: pc[k] for k in
                 ("group", "comp", "comps", "area_m2", "drawn", "px", "drawn_pct",
                  "world", "i", "verdict", "note", "batch", "mode")}
                for pc in ch.pieces(self.picks_path)]

    def rules_json(self, ch: Chapter) -> dict:
        """The thresholds the chapter is holding, what they take, and what they cost.

        `drawn` is measured against the SHIPPED height planes, so it is what this map would lose on
        top of the cut it already carries. `caught` is measured over the picker's own world, where
        no rule has cut anything yet - so a mark whose ground the shipped build already removed
        still counts, and the score answers "do these thresholds reproduce the marks".
        """
        lay = ch.rule_layer(ch.rules)
        out = {"set": ch.rules.as_dict(), "defaults": ch.defaults.as_dict(),
               "drawn_total": ch.drawn_total(), "comps": 0, "area_m2": 0.0, "drawn": 0,
               "drawn_pct": 0.0, "by_rule": {}}
        if lay is not None:
            out.update({k: lay[k] for k in
                        ("comps", "area_m2", "drawn", "drawn_pct", "by_rule")})
        out.update(score(ch.pieces(self.picks_path), ch.catch(ch.rules)))
        return out

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
            # Read per request: the page is the one file here that can be changed without the
            # 11-24 s of geometry behind it, so an edit lands on a browser reload.
            self._send(200, "text/html; charset=utf-8", PAGE_HTML.read_bytes())
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
        elif url.path == "/walls.png":
            feet = float(q.get("z", [ch.feet_z0])[0])
            self._send(200, "image/png",
                       ch.walls_png(feet, "show" if q.get("all", ["0"])[0] == "1" else "hide",
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
                "rules": self.rules_json(ch),
                "pieces": self.pieces_json(ch),
                "picks_path": str(self.picks_path),
            })
        elif url.path == "/markers":
            self._json({"ok": True, "markers": ch.markers(), "groups": MARKER_GROUPS})
        elif url.path == "/pick":
            self._json(self.pick(ch, q))
        elif url.path == "/box":
            self._json(self.box(ch, q))
        else:
            self._send(404, "text/plain", b"no")

    def box(self, ch: Chapter, q: dict) -> dict:
        """What a region verdict would take, measured before it is made.

        The same measurement a piece gets, over the ground `render.polys_in_boxes` finds - which is
        the call the build cuts with, so what the page shows is what the map loses.
        """
        try:
            box = {k: float(q[k][0]) for k in ("x0", "y0", "x1", "y1", "z0", "z1")}
        except (KeyError, ValueError):
            return {"ok": False, "why": "a region needs x0, y0, x1, y1, z0, z1"}
        box = {"x0": min(box["x0"], box["x1"]), "x1": max(box["x0"], box["x1"]),
               "y0": min(box["y0"], box["y1"]), "y1": max(box["y0"], box["y1"]),
               "z0": min(box["z0"], box["z1"]), "z1": max(box["z0"], box["z1"])}
        key, sel = ch.box_sel(box)
        if not sel:
            return {"ok": False, "box": box,
                    "why": "no navmesh in that region at that height"}
        sh = ch.shape_of(key, sel)
        zs = [q[2] for p in sel for q in p["pts"]]
        rep = {"ok": True, "box": box, "polys": len(sel), "comps": sh["comps"],
               "area_m2": sh["area_m2"], "z": [round(min(zs), 1), round(max(zs), 1)],
               "raster": {k: sh[k] for k in ("px", "px_on_map", "drawn", "reachable_pct")},
               "comps_touched": sorted({p["comp"] for p in sel if "comp" in p})[:12]}
        key = f"{time.time():.6f}"
        self.overlays[key] = ch.overlay_of(sh)
        for stale in list(self.overlays)[:-4]:
            self.overlays.pop(stale, None)
        rep["overlay"] = {"key": key, "x": sh["x"], "y": sh["y"],
                          "w": int(sh["mask"].shape[1]), "h": int(sh["mask"].shape[0])}
        return rep

    def pick(self, ch: Chapter, q: dict) -> dict:
        u, v = float(q.get("u", [0])[0]), float(q.get("v", [0])[0])
        mode = q.get("mode", ["comp"])[0]
        # Server state, like the standing rules: one page, one reading of "the same surface".
        if q.get("step", [""])[0] != "":
            ch.surface_step = max(1.0, float(q["step"][0]))
        if q.get("band", [""])[0] != "":
            ch.surface_band = max(0.0, float(q["band"][0]))
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
        rep = describe(ch, poly, mode)
        rep["piece"] = None if done is None else {
            k: done[k] for k in ("group", "i", "verdict", "note", "batch", "mode")}
        # A pick the page has given up on can still be in flight, so the handoff keeps the last
        # few pictures rather than clearing: whichever answer the page took, its overlay is there.
        key = f"{time.time():.6f}"
        self.overlays[key] = ch.overlay(poly, mode)
        for stale in list(self.overlays)[:-4]:
            self.overlays.pop(stale, None)
        rep["overlay"]["key"] = key
        rep["ok"] = True
        rep["world"] = [round(wx, 1), round(wy, 1), round(poly["cz"], 1)]
        rep["pixel"] = [int(u), int(v)]
        rep["surfaces"] = surfaces
        if mode == "surface" and "comp" in poly:
            # A surface is judged as region verdicts, so the boxes that would carry it - and what
            # they would take beyond it - are part of what the click has to show.
            _key, sel = ch.selection(poly, mode)
            boxes = ch.box_cover(sel)
            took = render.polys_in_boxes(ch.rest, boxes)
            mine = sum(p["xyarea"] for p in sel)
            rep["cover"] = {"boxes": boxes, "n": len(boxes),
                            "area_m2": round(sum(p["xyarea"] for p in took) / 1e4, 1),
                            "over_m2": round(max(0.0, sum(p["xyarea"] for p in took) - mine) / 1e4,
                                             1),
                            "step": ch.surface_step, "band": ch.surface_band,
                            "capped": round(mine / 1e4, 1) >= SURFACE_CAP_M2}
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
        if url.path == "/rules":
            # Server state, like the standing cut: one page, one set of thresholds under judgement,
            # and every picture the page then asks for is cut with them.
            ch.rules = ch.rules.with_body(body)
            self._json({"ok": True, "rules": self.rules_json(ch)})
            return
        doc = load_doc(self.picks_path)
        picks = doc["picks"]
        # Removing a pick renumbers the ones after it, so every loaded chapter's located pieces are
        # stale, not just this chapter's.
        self.library.forget_pieces()
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        if url.path == "/mark" and body.get("boxes"):
            # A surface is ONE verdict carrying the cover of one walkable surface - one row in the
            # list, one mark in the score - so the boxes go in as they are. `mode: box` because
            # that is what it is to everything downstream; `via` only records what drew it.
            body["boxes"] = [[b["x0"], b["y0"], b["x1"], b["y1"], b["z0"], b["z1"]]
                             for b in body["boxes"]]
            body.update(mode="box", via="surface", chapter=ch.key, batch=doc["open_batch"], ts=now)
            body.setdefault("from", "hand")
            picks.append(body)
        elif url.path == "/mark":
            # A verdict cuts the whole piece, and the piece under a click can be the level itself:
            # chapter 4's main body is one component of 16 199 m2, and marking anywhere on it -
            # including the zone nobody can reach - would take the chapter with it. The rules can
            # never do this (they leave seeded ground and ground at cost zero alone); a hand does it
            # in one click, so the hand is asked twice.
            share = 100.0 * float(body.get("area_m2") or 0.0) / max(1.0, ch.kept_area_m2())
            if body.get("verdict") == "oob" and share > BIG_PIECE_PCT and not body.get("big_ok"):
                self._json({"ok": False, "confirm": True,
                            "why": f"this piece is {share:.0f} % of {ch.key}'s walkable ground "
                                   f"({body.get('area_m2')} m2). A verdict cuts all of it."})
                return
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
    # The first cut costs ~12 s, and the rules under it another ~20 s. Pay it here, so the page
    # never waits on it.
    opening.cut(opening.feet_z0, "hide", cut_layers(opening, args.picks, set()))

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
