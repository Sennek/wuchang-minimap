#!/usr/bin/env python3
r"""
Shared reader for `markers/items.json` (schema `wuchang-minimap-items/1`).

`build_items.py` writes the file; `extract_markers.py` reads it for two things:

  * the **membership set** -- an integer that is a real row of one of the item
    DataTables is what makes the inline `Items` array scan safe (see
    `extract_markers.item_ids`);
  * the **display name** of an item id.

Kept in its own module so both tools agree on the shape and neither imports the
other.
"""

from __future__ import annotations

import json
import os

SCHEMA = "wuchang-minimap-items/1"

DEFAULT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "markers", "items.json")


class ItemDB:
    """`items.json` in memory.  An empty instance is legal and answers "unknown"
    to everything, so the extractor still runs before the file is built."""

    __slots__ = ("items",)

    def __init__(self, items: dict | None = None):
        # int id -> {"name": str, "des": str, "table": str};  name/des optional
        self.items: dict[int, dict] = items or {}

    def __len__(self) -> int:
        return len(self.items)

    def __contains__(self, item_id: int) -> bool:
        return item_id in self.items

    def name(self, item_id: int) -> str | None:
        rec = self.items.get(item_id)
        n = rec.get("name") if rec else None
        return n or None

    def table(self, item_id: int) -> str | None:
        rec = self.items.get(item_id)
        return rec.get("table") if rec else None

    @classmethod
    def load(cls, path: str | None = None) -> "ItemDB":
        p = path or DEFAULT_PATH
        if not os.path.exists(p):
            return cls()
        with open(p, encoding="utf-8") as fh:
            doc = json.load(fh)
        if doc.get("schema") != SCHEMA:
            raise SystemExit(f"{p}: unexpected schema {doc.get('schema')!r}")
        return cls({int(k): v for k, v in doc.get("items", {}).items()})
