"""
oob_rules.py - the thresholds the cut rules stand on, as one thing the picker can turn.

`render.out_of_bounds` decides the cut from the per-component measurements and three thresholds.
The measurements cost seconds and a threshold costs nothing, so the picker holds the measurements
per chapter and re-decides the whole cut on every turn of a knob - that is what a `RuleSet` names.
The build reads the same thresholds off its own command line; this file is only how the page and
the server pass one around.

`score` is the other half of the loop: a threshold is worth what it catches of the ground the user
marked out of bounds, against what it takes of the ground they called legitimate.

PURE: `render`'s defaults and the standard library. Nothing here loads a chapter.
"""

from __future__ import annotations

from dataclasses import dataclass, replace

import render


@dataclass(frozen=True)
class RuleSet:
    """The three cut thresholds plus the fall cap the escape costs are measured with.

    Areas are uu2 the way `render` counts them, not m2 - the page converts, because the number a
    person judges is 50 m2 and the number the build takes is 500000.
    """

    climb: float = render.DEFAULT_ESCAPE_CLIMB
    wall_far: float = render.DEFAULT_WALL_FAR
    small: float = render.DEFAULT_SMALL_UNSEEDED
    fall: float = render.DEFAULT_ESCAPE_FALL

    @property
    def key(self) -> str:
        """Names the cut these thresholds make - what a cached catch or picture is kept under."""
        return f"{self.climb:g}|{self.wall_far:g}|{self.small:g}|{self.fall:g}"

    def as_dict(self) -> dict:
        return {"climb": self.climb, "wall_far": self.wall_far, "small": self.small,
                "fall": self.fall}

    def with_body(self, body: dict) -> "RuleSet":
        """This set with whatever the page sent changed on it. A missing field keeps its value."""
        got = {k: float(body[k]) for k in ("climb", "wall_far", "small", "fall") if k in body}
        return replace(self, **got)


def score(pieces: list[dict], catch: dict[int, str]) -> dict:
    """What these thresholds do to the ground the user has already judged.

    Both numbers are over the picker's own world, where no rule has cut anything yet, so a mark
    whose ground the shipped build already removed still counts: the question is whether the rules
    REPRODUCE the marks, not whether today's map still shows them.

    A mark is caught by the component under its point - the one the verdict was made on. A shelf or
    a cluster mark covers more, and the rest of it is judged the same way it was marked: by eye, on
    the picture.
    """
    out = {"oob_caught": 0, "oob_total": 0, "legit_cut": 0, "legit_total": 0}
    for pc in pieces:
        if pc["comp"] is None or pc["verdict"] not in ("oob", "ok"):
            continue
        hit = pc["comp"] in catch
        if pc["verdict"] == "oob":
            out["oob_total"] += 1
            out["oob_caught"] += hit
        else:
            out["legit_total"] += 1
            out["legit_cut"] += hit
    return out
