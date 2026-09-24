#!/usr/bin/env python3
r"""The game's own strings, in every culture it ships: `MMGame.locres`.

The only game-string target is `Content/Localization/MMGame/<culture>/MMGame.locres`,
one per culture folder (`de en es fr it ja ko pt ru zh zh-Hant`; Simplified Chinese is
`zh`, the game's native culture).  Every builder that names something reads it through
here, so the reader, the culture list and the `--lang` option have one home.

A NAME IS JOINED BY KEY
-----------------------
A builder decides WHICH key names a thing in English (`SOURCE`), and every other
culture's name is that key's string there - never a match on the English value:
"Secret Letter" is two items whose Chinese names differ.  `name` stays English and
`names` holds only the cultures whose string differs from it; a culture missing the key
(zh-Hant lacks 128 of the pipeline's keys) is simply absent, and the runtime's fallback
chain answers for it.

    python build_items.py --lang zh,en       # a subset; `en` is always read
"""

from __future__ import annotations

import struct
import sys

LOCRES_DIR = "Content/Localization/MMGame/"
LOCRES = LOCRES_DIR + "{lang}/MMGame.locres"
LOCRES_MAGIC = bytes.fromhex("0e147475674a03fc4a15909dc3377f1b")

# The culture `name` is written in, and the one every key is chosen in.
SOURCE = "en"


def _fstring(b: bytes, o: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<i", b, o)
    o += 4
    if n == 0:
        return "", o
    if n < 0:                                   # UTF-16, length in characters
        return b[o:o - 2 * n].decode("utf-16-le").rstrip("\0"), o - 2 * n
    return b[o:o + n].decode("utf-8", "replace").rstrip("\0"), o + n


def read_locres(blob: bytes) -> dict[str, str]:
    """`key -> localised string` for the empty namespace (which is where every
    Wuchang game string lives).  Namespaced keys are prefixed `<ns>/`.

    A locres v3 file (`Optimized_CRC32`): a header, a namespace/key table whose
    values are indices, and a string table at `StringTableOffset`."""
    if blob[:16] != LOCRES_MAGIC:
        raise SystemExit("not a locres file (bad magic)")
    o = 16
    version = blob[o]
    o += 1
    if version != 3:
        raise SystemExit(f"locres version {version} not supported (expected 3)")
    (string_table_offset,) = struct.unpack_from("<q", blob, o)
    o += 8
    (entry_count,) = struct.unpack_from("<I", blob, o)
    o += 4
    (ns_count,) = struct.unpack_from("<I", blob, o)
    o += 4

    index: dict[str, int] = {}
    for _ in range(ns_count):
        o += 4                                  # namespace hash
        ns, o = _fstring(blob, o)
        (key_count,) = struct.unpack_from("<I", blob, o)
        o += 4
        for _ in range(key_count):
            o += 4                              # key hash
            key, o = _fstring(blob, o)
            o += 4                              # source-string hash
            (idx,) = struct.unpack_from("<i", blob, o)
            o += 4
            index[key if not ns else f"{ns}/{key}"] = idx
    if len(index) != entry_count:
        print(f"  ! locres: {len(index)} keys parsed, header says {entry_count}",
              file=sys.stderr)

    p = string_table_offset
    (count,) = struct.unpack_from("<i", blob, p)
    p += 4
    strings: list[str] = []
    for _ in range(count):
        s, p = _fstring(blob, p)
        p += 4                                  # reference count (v3)
        strings.append(s)
    if p != len(blob):
        print(f"  ! locres: string table ended at {p} of {len(blob)}", file=sys.stderr)
    return {k: strings[i] for k, i in index.items() if 0 <= i < len(strings)}


def cultures(ms) -> list[str]:
    """Every culture folder the paks carry an `MMGame.locres` for, sorted."""
    out = set()
    for k in ms.paths():
        if k.startswith(LOCRES_DIR) and k.endswith("/MMGame.locres"):
            out.add(k[len(LOCRES_DIR):].split("/", 1)[0])
    if SOURCE not in out:
        raise SystemExit(f"the paks carry no {LOCRES.format(lang=SOURCE)}")
    return sorted(out)


def _strip(s: str) -> str:
    return s.strip()


class Strings:
    """`MMGame.locres` of several cultures, answering by key.

    `get()` is the English string - what a builder chooses keys by and writes as
    `name`; `names()` is every other culture's string for that same key."""

    def __init__(self, locs: dict[str, dict[str, str]]):
        if SOURCE not in locs:
            raise SystemExit(f"locres: the culture list must include {SOURCE!r}")
        self.locs = locs
        self.en = locs[SOURCE]

    @classmethod
    def load(cls, ms, langs: "list[str] | str | None" = None) -> "Strings":
        """`langs` is a list, a comma-separated `--lang` value, or None for every
        culture the paks carry.  `SOURCE` is always read."""
        if isinstance(langs, str):
            langs = [x.strip() for x in langs.split(",") if x.strip()]
        have = cultures(ms)
        want = sorted(set(langs or have) | {SOURCE})
        missing = [c for c in want if c not in have]
        if missing:
            raise SystemExit(f"locres: no MMGame.locres for {missing} (the paks carry {have})")
        return cls({c: read_locres(ms.read(LOCRES.format(lang=c))) for c in want})

    @property
    def cultures(self) -> list[str]:
        return sorted(self.locs)

    def get(self, key: str) -> str | None:
        v = self.en.get(key)
        return v or None

    def names(self, key: str, clean=_strip) -> dict[str, str]:
        """`{culture: string}` for `key` in every culture but `SOURCE`, keeping only
        a non-empty string that differs from the English one.  `clean` is the same
        tidy the caller applies to the English string, so both compare alike."""
        en = clean(self.en.get(key) or "")
        out: dict[str, str] = {}
        for c, loc in sorted(self.locs.items()):
            if c == SOURCE:
                continue
            v = clean(loc.get(key) or "")
            if v and v != en:
                out[c] = v
        return out


def add_arg(ap) -> None:
    ap.add_argument("--lang", default=None,
                    help="comma-separated cultures to read names in (locres folder names); "
                         "default every culture the paks carry. `en` is always read")


def with_names(rec: dict, name: str, names: dict[str, str]) -> dict:
    """`rec` with `name` and, when any culture differs, `names` - the one shape every
    builder writes a named thing in."""
    rec["name"] = name
    if names:
        rec["names"] = dict(sorted(names.items()))
    return rec
