#!/usr/bin/env python3
r"""Provenance stamp for every generated `markers/*.json` (review item C.12).

WHY
---
A marker file is a *derived* artifact: it is only correct for the game build it
was extracted from and the extractor that read it.  Before this, a chapter file
said `schema` / `source` / `generated_by` and nothing about either input, so a
game patch that moved an actor produced a silently wrong database and a
re-extraction produced an undiffable file.  Three facts make it auditable:

    "game_build":       "5.1.1.0"                    exe FILEVERSION
    "exe":              {name, size, sha256}         the shipping exe
    "pak":              [{name, size, sha256}, ...]  the paks actually mounted
    "extractor_commit": "9a46681"                    this repo's HEAD

`game_build` is the one the runtime reads: it is what a running mod can get
cheaply for itself (`GetFileVersionInfoW` on the game exe), so `gamestate` can
compare and warn, and a player on a newer patch is told why a marker may be in
the wrong place instead of silently trusting it.  Be aware of what it is worth
on THIS title: Leenzee ship no game build number at all - the exe's FILEVERSION
is the *engine* version (`5.1.1.0`) and its `ProductVersion` string is
`++UE5+Release-5.1-CL-0`, so `game_build` will not move across game patches.
The identifiers that do move are the digests, which is why they are here: the
exe's and the paks'.  The other two are for us: `pak` identifies the exact
bytes read (a Steam depot update changes the size and the digest even when the
version string does not move), `extractor_commit` identifies the code that read
them.  Every field is optional to a reader: a consumer that does not know them
ignores them, which is what keeps schema `.../1` valid.

The digests are cached in `.provenance-cache.json` next to this file (keyed by
path + size + mtime, gitignored) because the pak set is ~63 GB: about 45 s of
disk on a warm NVMe, once, rather than once per chapter.  `--no-pak-hash` (and
`WUCHANG_NO_PAK_HASH=1`) skips the digest and records size only, for a quick
iteration loop; the stamp then says `"sha256": null`, which is honest rather
than absent.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_CACHE = os.path.join(_HERE, ".provenance-cache.json")

EXE_REL = os.path.join("Project_Plague", "Binaries", "Win64",
                       "Project_Plague-Win64-Shipping.exe")


# ---------------------------------------------------------------------------
# the game build
# ---------------------------------------------------------------------------

def _fixed_file_info(path: str) -> str | None:
    """`FILEVERSION` of a PE, as `a.b.c.d`, via the Win32 version API.

    The resource is read rather than parsed by hand because the alternative
    (walking the PE to `VS_FIXEDFILEINFO`) is fifty lines that can be wrong in
    silence.  Returns None when the exe has no version resource, which is a
    fact worth recording as `null` rather than crashing an extraction over.
    """
    if not os.path.exists(path):
        return None
    try:
        ver = ctypes.WinDLL("version.dll")
    except OSError:
        return None
    ver.GetFileVersionInfoSizeW.argtypes = [wt.LPCWSTR, ctypes.POINTER(wt.DWORD)]
    ver.GetFileVersionInfoSizeW.restype = wt.DWORD
    ver.GetFileVersionInfoW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID]
    ver.GetFileVersionInfoW.restype = wt.BOOL
    ver.VerQueryValueW.argtypes = [wt.LPVOID, wt.LPCWSTR,
                                   ctypes.POINTER(wt.LPVOID),
                                   ctypes.POINTER(wt.UINT)]
    ver.VerQueryValueW.restype = wt.BOOL

    dummy = wt.DWORD(0)
    size = ver.GetFileVersionInfoSizeW(path, ctypes.byref(dummy))
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(path, 0, size, buf):
        return None
    block = wt.LPVOID()
    blen = wt.UINT()
    if not ver.VerQueryValueW(buf, "\\", ctypes.byref(block), ctypes.byref(blen)):
        return None
    if blen.value < 52:
        return None
    raw = ctypes.string_at(block, blen.value)
    ms = int.from_bytes(raw[8:12], "little")
    ls = int.from_bytes(raw[12:16], "little")
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def game_build(pak_path: str | None = None, game_root: str | None = None) -> str | None:
    """The shipping exe's FILEVERSION, found from the pak path or the game root.

    The pak lives at `<root>/Project_Plague/Content/Paks/*.pak`, so the root is
    three directories up - derived rather than configured, because the two are
    always the same install and a second env var is a second thing to get wrong.
    """
    root = game_root or os.environ.get("WUCHANG_GAME_ROOT")
    if not root and pak_path:
        root = os.path.abspath(os.path.join(os.path.dirname(pak_path),
                                            "..", "..", ".."))
    if not root:
        return None
    return _fixed_file_info(os.path.join(root, EXE_REL))


# ---------------------------------------------------------------------------
# the paks
# ---------------------------------------------------------------------------

def _load_cache() -> dict:
    try:
        with open(_CACHE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:                                           # noqa: BLE001
        return {}


def _save_cache(c: dict) -> None:
    try:
        with open(_CACHE, "w", encoding="utf-8") as f:
            json.dump(c, f, indent=1, sort_keys=True)
    except OSError:
        pass


def _sha256(path: str, size: int, mtime: int, cache: dict) -> str:
    key = f"{os.path.abspath(path)}|{size}|{mtime}"
    hit = cache.get(key)
    if hit:
        return hit
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
    cache[key] = h.hexdigest()
    _save_cache(cache)
    return cache[key]


def pak_stamp(paths: list[str], want_hash: bool = True) -> list[dict]:
    """`[{name, size, sha256}]` for every pak that was mounted, in mount order."""
    if os.environ.get("WUCHANG_NO_PAK_HASH") == "1":
        want_hash = False
    cache = _load_cache() if want_hash else {}
    out = []
    for p in paths:
        try:
            st = os.stat(p)
        except OSError:
            out.append({"name": os.path.basename(p), "size": None, "sha256": None})
            continue
        rec = {"name": os.path.basename(p), "size": st.st_size, "sha256": None}
        if want_hash:
            rec["sha256"] = _sha256(p, st.st_size, int(st.st_mtime), cache)
        out.append(rec)
    return out


# ---------------------------------------------------------------------------
# this repo
# ---------------------------------------------------------------------------

def extractor_commit() -> str | None:
    """Short HEAD of the repo this file lives in, `+dirty` when it is."""
    try:
        rev = subprocess.run(["git", "-C", _HERE, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=20)
        if rev.returncode != 0:
            return None
        sha = rev.stdout.strip()
        st = subprocess.run(["git", "-C", _HERE, "status", "--porcelain"],
                            capture_output=True, text=True, timeout=30)
        if st.returncode == 0 and st.stdout.strip():
            sha += "+dirty"
        return sha
    except Exception:                                           # noqa: BLE001
        return None


def stamp(ms=None, pak_path: str | None = None, want_hash: bool = True) -> dict:
    """The three provenance fields, ready to merge into a generated document.

    `ms` is a `pakmaps.MapSource`; its own mount list is used so the stamp
    records the paks that were actually read rather than the ones the CLI was
    pointed at.
    """
    paths = []
    if ms is not None:
        paths = [p.path for p in ms.ps.paks]
    elif pak_path:
        paths = [pak_path]
    first = pak_path or (paths[0] if paths else None)
    out = {
        "game_build": game_build(first),
        "pak": pak_stamp(paths, want_hash),
        "extractor_commit": extractor_commit(),
    }
    exe = exe_path(first)
    if exe:
        out["exe"] = pak_stamp([exe], want_hash)[0]
    return out


def exe_path(pak_path: str | None) -> str | None:
    root = os.environ.get("WUCHANG_GAME_ROOT")
    if not root and pak_path:
        root = os.path.abspath(os.path.join(os.path.dirname(pak_path), "..", "..", ".."))
    if not root:
        return None
    p = os.path.join(root, EXE_REL)
    return p if os.path.exists(p) else None


def add_arg(ap) -> None:
    """The one CLI flag every generator shares."""
    ap.add_argument("--no-pak-hash", action="store_true",
                    help="record pak size only, skip the sha256 (fast iteration)")


def main(argv=None) -> int:
    import argparse
    import pakmaps
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    add_arg(ap)
    a = ap.parse_args(argv)
    ms = pakmaps.MapSource(a.pak)
    print(json.dumps(stamp(ms, a.pak, not a.no_pak_hash), indent=1))
    return 0


if __name__ == "__main__":
    sys.path.insert(0, _HERE)
    sys.exit(main())
