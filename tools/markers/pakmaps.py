#!/usr/bin/env python3
"""Mount Wuchang's pak set and hand back cooked `.umap` packages by logical path.

The three paks do **not** share a mount point -- the base and `_0_P` mount at
`../../../` (so their keys start `Project_Plague/Content/...`) while `_1_P`
mounts at `../../../Project_Plague/` (keys start `Content/...`).  Those are the
same logical files, so every path is normalised to `Content/...` and the
highest-numbered pak wins, exactly as the engine mounts them.
"""

from __future__ import annotations

import io
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "navmesh", "offline"))

import pak as pakmod        # noqa: E402
import uasset               # noqa: E402

# Machine-specific: the game's base pak on the original dev box. Point WUCHANG_PAK at
# the .pak (or WUCHANG_GAME_ROOT at the install folder) to run this anywhere else.
_FALLBACK_GAME_ROOT = r"E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers"
DEFAULT_PAK = os.environ.get("WUCHANG_PAK") or os.path.join(
    os.environ.get("WUCHANG_GAME_ROOT", _FALLBACK_GAME_ROOT),
    "Project_Plague", "Content", "Paks", "Project_Plague-Windows.pak")

_PREFIX = "Project_Plague/"


def normalise(p: str) -> str:
    return p[len(_PREFIX):] if p.startswith(_PREFIX) else p


class MapSource:
    def __init__(self, base_pak: str = DEFAULT_PAK, verbose: bool = False):
        self.ps = pakmod.PakSet(base_pak, verbose)
        rank = {p.path: i for i, p in enumerate(self.ps.paks)}
        self.owner: dict[str, object] = {}
        self.real: dict[str, str] = {}
        for raw, ow in self.ps.owner.items():
            k = normalise(raw)
            prev = self.owner.get(k)
            if prev is None or rank[ow.path] >= rank[prev.path]:
                self.owner[k] = ow
                self.real[k] = raw

    def paths(self):
        return self.owner.keys()

    def read(self, key: str) -> bytes:
        return self.owner[key].read(self.real[key])

    def umaps(self, prefix: str = "Content/Maps/"):
        return sorted(k for k in self.owner
                      if k.startswith(prefix) and k.endswith(".umap"))

    def package(self, key: str) -> "uasset.Package":
        """Parse a cooked package straight out of the paks (no files on disk).

        `.umap` (a level) and `.uasset` (a blueprint, so a caller can read a
        class' `Default__<class>` object) are the same shape: a header entry
        plus the `.uexp` beside it that holds every export's payload, which is
        why the extension is split off rather than assumed.
        """
        head = self.read(key)
        uexp_key = os.path.splitext(key)[0] + ".uexp"
        uexp = self.read(uexp_key) if uexp_key in self.owner else b""
        return _package_from_bytes(key, head, uexp)


def _package_from_bytes(name: str, head: bytes, uexp: bytes) -> "uasset.Package":
    p = uasset.Package.__new__(uasset.Package)
    p.path = name
    p.head = head
    p.uexp = uexp
    p._parse()
    return p
