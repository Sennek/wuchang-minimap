#!/usr/bin/env python3
"""
Minimal UE4/UE5 .pak reader + extractor for Wuchang: Fallen Feathers.

Handles pak version 11 with an AES-256-ECB encrypted index and encrypted /
compressed file entries.  Only the pieces we need are implemented:

  * footer (FPakInfo) incl. the compression-method name table
  * primary index  -> mount point, EncodedPakEntries blob, non-encoded Files[]
  * full directory index -> path -> encoded-entry offset
  * encoded-entry decoder (FPakFile::DecodePakEntry)
  * per-file read: AES decrypt + Zlib / Gzip / Oodle block decompress

Grown out of the earlier `pakread.py` index dumper (which only listed paths).

CLI
---
    python pak.py list  <pak> [--grep SUBSTR ...]        # paths only
    python pak.py info  <pak> --grep SUBSTR              # full entry details
    python pak.py unpack <pak> --grep SUBSTR --out DIR   # extract matches

Multiple paks: pass the *base* pak and the reader also mounts the `_N_P`
patch paks next to it (higher N wins), matching the engine's mount order.
"""

from __future__ import annotations

import argparse
import ctypes
import glob
import os
import struct
import sys
import zlib

try:
    from Crypto.Cipher import AES
except ImportError:  # pragma: no cover
    sys.exit("needs pycryptodome:  pip install pycryptodome")

AES_KEY = bytes.fromhex("C46B1A3D21AC9F6DF57A45A24CC479B413E3F326F93AB46DCFE0E0120DCFFBC8")
PAK_MAGIC = 0x5A6F12E1
COMPRESSION_NAME_LEN = 32

# ---------------------------------------------------------------------------
# little binary reader
# ---------------------------------------------------------------------------


class R:
    __slots__ = ("b", "o")

    def __init__(self, b: bytes, o: int = 0):
        self.b = b
        self.o = o

    def raw(self, n: int) -> bytes:
        v = self.b[self.o : self.o + n]
        if len(v) != n:
            raise EOFError(f"want {n} got {len(v)} at {self.o}")
        self.o += n
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.b, self.o)[0]
        self.o += 4
        return v

    def i64(self) -> int:
        v = struct.unpack_from("<q", self.b, self.o)[0]
        self.o += 8
        return v

    def u64(self) -> int:
        v = struct.unpack_from("<Q", self.b, self.o)[0]
        self.o += 8
        return v

    def u8(self) -> int:
        v = self.b[self.o]
        self.o += 1
        return v

    def fstring(self) -> str:
        n = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        if n == 0:
            return ""
        if n < 0:
            return self.raw(-n * 2).decode("utf-16-le").rstrip("\x00")
        return self.raw(n).decode("utf-8", "replace").rstrip("\x00")


def _aes_decrypt(data: bytes) -> bytes:
    if len(data) % 16:
        raise ValueError(f"encrypted block not 16-byte aligned: {len(data)}")
    return AES.new(AES_KEY, AES.MODE_ECB).decrypt(data)


# ---------------------------------------------------------------------------
# Oodle (optional) -- UE5 links Oodle statically, so the game ships no DLL.
# Point WUCHANG_OODLE_DLL at an oo2core_*_win64.dll if Oodle-compressed
# entries turn up.
# ---------------------------------------------------------------------------

_oodle = None


def _oodle_decompress(src: bytes, dst_size: int) -> bytes:
    global _oodle
    if _oodle is None:
        path = os.environ.get("WUCHANG_OODLE_DLL", "")
        if not path or not os.path.exists(path):
            raise RuntimeError(
                "Oodle-compressed entry but no oo2core DLL; set WUCHANG_OODLE_DLL"
            )
        _oodle = ctypes.CDLL(path)
        _oodle.OodleLZ_Decompress.restype = ctypes.c_int64
        _oodle.OodleLZ_Decompress.argtypes = [
            ctypes.c_char_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int64,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
            ctypes.c_int64, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_int64, ctypes.c_int,
        ]
    out = ctypes.create_string_buffer(dst_size)
    n = _oodle.OodleLZ_Decompress(
        src, len(src), out, dst_size, 1, 0, 0, None, 0, None, None, None, 0, 3
    )
    if n != dst_size:
        raise RuntimeError(f"OodleLZ_Decompress returned {n}, wanted {dst_size}")
    return out.raw[:dst_size]


# ---------------------------------------------------------------------------
# entries
# ---------------------------------------------------------------------------


class Entry:
    __slots__ = (
        "path", "pak", "offset", "size", "usize", "cmethod", "encrypted",
        "blocks", "block_size", "struct_size",
    )

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def __repr__(self):
        return (
            f"<{self.path} off={self.offset} size={self.size} usize={self.usize} "
            f"cm={self.cmethod} enc={self.encrypted} blocks={len(self.blocks or [])}>"
        )


def _entry_struct_size(cmethod: int, nblocks: int) -> int:
    """FPakEntry::GetSerializedSize for pak version >= 8."""
    n = 8 + 8 + 8 + 4 + 20 + 1 + 4  # off,size,usize,cmindex,hash,flags,blocksize
    if cmethod != 0:
        n += 4 + nblocks * 16
    return n


def decode_entry(data: bytes, at: int) -> Entry:
    """FPakFile::DecodePakEntry - the packed form stored in EncodedPakEntries."""
    r = R(data, at)
    bits = r.u32()
    cmethod = (bits >> 23) & 0x3F
    encrypted = (bits & (1 << 22)) != 0
    nblocks = (bits >> 6) & 0xFFFF

    offset = r.u32() if (bits & (1 << 31)) else r.i64()
    usize = r.u32() if (bits & (1 << 30)) else r.i64()
    if cmethod != 0:
        size = r.u32() if (bits & (1 << 29)) else r.i64()
    else:
        size = usize

    block_size = 0
    if nblocks > 0:
        block_size = bits & 0x3F
        if block_size == 0x3F:
            block_size = r.u32()
        else:
            block_size <<= 11

    ssize = _entry_struct_size(cmethod, nblocks)
    blocks: list[tuple[int, int]] = []
    if nblocks == 1 and not encrypted:
        start = offset + ssize
        blocks.append((start, start + size))
    elif nblocks > 0:
        cur = offset + ssize
        for _ in range(nblocks):
            bs = r.u32()
            blocks.append((cur, cur + bs))
            cur += (bs + 15) & ~15 if encrypted else bs
    return Entry(
        offset=offset, size=size, usize=usize, cmethod=cmethod,
        encrypted=encrypted, blocks=blocks, block_size=block_size,
        struct_size=ssize,
    )


def read_full_entry(r: R) -> Entry:
    """FPakEntry::Serialize for pak version >= 8 (index or data-section copy)."""
    offset = r.i64()
    size = r.i64()
    usize = r.i64()
    cmethod = r.i32()
    r.raw(20)  # SHA1
    blocks: list[tuple[int, int]] = []
    if cmethod != 0:
        n = r.i32()
        for _ in range(n):
            blocks.append((r.i64(), r.i64()))
    flags = r.u8()
    block_size = r.u32()
    return Entry(
        offset=offset, size=size, usize=usize, cmethod=cmethod,
        encrypted=bool(flags & 0x01), blocks=blocks, block_size=block_size,
        struct_size=_entry_struct_size(cmethod, len(blocks)),
    )


# ---------------------------------------------------------------------------
# pak file
# ---------------------------------------------------------------------------


class Pak:
    def __init__(self, path: str, verbose: bool = False):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        self._read_footer()
        self._read_index(verbose)

    # -- footer -----------------------------------------------------------
    def _read_footer(self):
        tail_len = min(8192, self.size)
        self.f.seek(self.size - tail_len)
        tail = self.f.read(tail_len)
        pos = tail.rfind(struct.pack("<I", PAK_MAGIC))
        if pos < 0:
            raise SystemExit(f"{self.path}: pak magic not found")
        r = R(tail, pos + 4)
        self.version = r.i32()
        self.index_off = r.i64()
        self.index_size = r.i64()
        r.raw(20)
        self.encrypted_index = bool(tail[pos - 1])
        self.guid = tail[pos - 17 : pos - 1]
        # compression method table: the tail of FPakInfo, 5 x 32 bytes for v>8
        nmeth = 4 if self.version == 8 else 5
        cm = tail[r.o : r.o + nmeth * COMPRESSION_NAME_LEN]
        self.compression_methods = ["None"]
        for i in range(nmeth):
            name = cm[i * 32 : (i + 1) * 32].split(b"\x00")[0].decode("ascii", "replace")
            if name:
                self.compression_methods.append(name)

    def _block(self, off: int, size: int) -> bytes:
        self.f.seek(off)
        n = (size + 15) & ~15 if self.encrypted_index else size
        data = self.f.read(n)
        if self.encrypted_index:
            data = _aes_decrypt(data)
        return data[:size]

    # -- index ------------------------------------------------------------
    def _read_index(self, verbose: bool):
        idx = self._block(self.index_off, self.index_size)
        r = R(idx)
        self.mount = r.fstring()
        self.num_entries = r.i32()
        if not (self.mount.startswith("../") or self.mount.startswith("/")):
            raise SystemExit(f"{self.path}: bad mount {self.mount!r} -> wrong AES key")
        self.path_hash_seed = r.u64()
        if r.i32():
            r.i64(); r.i64(); r.raw(20)
        has_fdi = r.i32()
        if not has_fdi:
            raise SystemExit(f"{self.path}: no full directory index")
        fdi_off, fdi_size = r.i64(), r.i64()
        r.raw(20)
        enc_len = r.i32()
        self.encoded = r.raw(enc_len)
        nfiles = r.i32()
        self.files: list[Entry] = [read_full_entry(r) for _ in range(nfiles)]

        fdi = self._block(fdi_off, fdi_size)
        d = R(fdi)
        ndirs = d.i32()
        self.index: dict[str, int] = {}
        for _ in range(ndirs):
            dirname = d.fstring()
            nf = d.i32()
            for _ in range(nf):
                fn = d.fstring()
                self.index[(dirname + fn).lstrip("/")] = d.i32()
        if verbose:
            print(
                f"{os.path.basename(self.path)}: v{self.version} "
                f"entries={self.num_entries} indexed={len(self.index)} "
                f"encoded={enc_len}B files[]={nfiles} "
                f"methods={self.compression_methods}"
            )

    # -- lookup / read ----------------------------------------------------
    def paths(self):
        return self.index.keys()

    def entry(self, path: str) -> Entry:
        off = self.index[path]
        e = decode_entry(self.encoded, off) if off >= 0 else self.files[-(off + 1)]
        e.path = path
        e.pak = self.path
        return e

    def read(self, path: str) -> bytes:
        e = self.entry(path)
        if e.cmethod == 0:
            self.f.seek(e.offset + e.struct_size)
            n = (e.size + 15) & ~15 if e.encrypted else e.size
            data = self.f.read(n)
            if e.encrypted:
                data = _aes_decrypt(data)
            return data[: e.usize]

        name = self.compression_methods[e.cmethod]
        out = bytearray()
        for i, (cs, ce) in enumerate(e.blocks):
            self.f.seek(cs)
            clen = ce - cs
            n = (clen + 15) & ~15 if e.encrypted else clen
            blob = self.f.read(n)
            if e.encrypted:
                blob = _aes_decrypt(blob)
            blob = blob[:clen]
            want = min(e.block_size, e.usize - len(out))
            if name.lower() in ("zlib", "gzip"):
                out += zlib.decompress(blob)
            elif name.lower().startswith("oodle"):
                out += _oodle_decompress(blob, want)
            else:
                raise RuntimeError(f"unsupported compression {name!r}")
        return bytes(out[: e.usize])


class PakSet:
    """Base pak + its `_N_P` patch paks; later paks shadow earlier ones."""

    def __init__(self, base: str, verbose: bool = False):
        d = os.path.dirname(base)
        stem = os.path.basename(base)[: -len(".pak")]
        paths = [base]
        patches = sorted(
            glob.glob(os.path.join(d, stem + "_*_P.pak")),
            key=lambda p: int(os.path.basename(p)[len(stem) + 1 :].split("_")[0]),
        )
        paths += patches
        self.paks = [Pak(p, verbose) for p in paths]
        self.owner: dict[str, Pak] = {}
        for pak in self.paks:  # later wins
            for p in pak.index:
                self.owner[p] = pak

    def paths(self):
        return self.owner.keys()

    def entry(self, path: str) -> Entry:
        return self.owner[path].entry(path)

    def read(self, path: str) -> bytes:
        return self.owner[path].read(path)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _matches(paths, greps):
    if not greps:
        return sorted(paths)
    low = [g.lower() for g in greps]
    return sorted(p for p in paths if any(g in p.lower() for g in low))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["list", "info", "unpack"])
    ap.add_argument("pak")
    ap.add_argument("--grep", action="append", default=[])
    ap.add_argument("--out", default=".")
    ap.add_argument("--single", action="store_true", help="ignore _N_P patch paks")
    a = ap.parse_args(argv)

    src = Pak(a.pak, verbose=True) if a.single else PakSet(a.pak, verbose=True)
    hits = _matches(src.paths(), a.grep)
    print(f"matches: {len(hits)}")

    if a.cmd == "list":
        for p in hits:
            print(p)
        return 0

    if a.cmd == "info":
        for p in hits:
            e = src.entry(p)
            holder = src.owner[p] if isinstance(src, PakSet) else src
            cm = (
                holder.compression_methods[e.cmethod]
                if e.cmethod < len(holder.compression_methods)
                else f"#{e.cmethod}"
            )
            print(
                f"{p}\n    pak={os.path.basename(e.pak or a.pak)} off={e.offset} "
                f"size={e.size} usize={e.usize} method={cm} enc={e.encrypted} "
                f"blocks={len(e.blocks)} blocksize={e.block_size}"
            )
        return 0

    os.makedirs(a.out, exist_ok=True)
    for p in hits:
        data = src.read(p)
        dst = os.path.join(a.out, p.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(data)
        print(f"  {len(data):>12,}  {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
