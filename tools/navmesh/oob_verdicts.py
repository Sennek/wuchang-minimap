"""
oob_verdicts.py - the verdict file `oob_pick.py` writes, and the layer a verdict puts a piece in.

A verdict is stored against a world point and the chapter it belongs to, never a component id:
ids are assigned in load order and do not survive a regeneration. The layer is decided here too,
from the verdict and what the mod still draws of the piece - the picker colours the map by it.

PURE: the standard library only. Nothing here needs a chapter loaded.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

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
    "done":    {"rgb": (113, 122, 132), "label": "already gone from the map"},
}

# A mark the pipeline disagrees with: its component carries a marker of the game's own - a shrine, an
# enemy, a pickup - or the mod draws almost none of it, so cutting it changes nothing.
DOUBT_DRAWN_PCT = 10.0


# ---------------------------------------------------------------------------------
# which layer a piece belongs to
# ---------------------------------------------------------------------------------


def group_of(pick: dict, comp: dict | None, open_batch: int, drawn_pct: float) -> str:
    """Which layer a mark belongs to.

    `done` comes first: once the build cuts, a correct mark's ground is no longer in the height
    planes, so the mod draws none of it and there is nothing left on screen to look at. That is the
    mark having worked, not a reason to re-open it - and without this state every mark in a cut
    chapter falls into `doubt` on the "barely drawn" test and the layer becomes noise.

    Doubt is then only what still needs answering: a marker of the game's own stands on the
    component - the pipeline saying the player goes there - or the mod draws a sliver of the piece
    while the rest is hidden, so cutting it would change almost nothing.
    """
    if pick.get("verdict") == "ask":
        return "doubt"       # a piece put in front of the user; nothing is decided about it yet
    if pick.get("verdict") != "oob":
        return "legit"
    if drawn_pct <= 0.0:
        return "done"
    # A second verdict is a human answering the doubt, whichever way; the test has had its say.
    if not pick.get("revised") and (
            (comp is not None and comp.get("seeded")) or drawn_pct < DOUBT_DRAWN_PCT):
        return "doubt"
    return "batch" if pick.get("batch") == open_batch else "earlier"


# ---------------------------------------------------------------------------------
# the file
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
