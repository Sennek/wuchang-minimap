#!/usr/bin/env python3
"""
Just enough cooked-UE5-package parsing to locate an export's bytes.

No .usmap is available for Wuchang, so property tags cannot be decoded -- but
the package *summary*, *name map*, *import map* and *export map* are plain
versioned structs that need no mappings.  That is all we need: the export map
gives (name, class, serial offset, serial size) so the raw `RecastNavMeshDataChunk`
blob can be sliced out of the `.uexp` and parsed by hand.

Layout verified against
`Project_Plague/Content/Maps/Generate/Chapter1/EX0/B1EX0_L0_X1_Y0_DL0_WP.umap`
(cooked, unversioned, UE 5.1.1): FObjectImport = 32 bytes, FObjectExport = 96
bytes, no per-export PackageGuid, `bGeneratePublicHash` present.

    python uasset.py <file.umap> [--grep SUBSTR]
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

PKG_TAG = 0x9E2A83C1


class R:
    __slots__ = ("b", "o")

    def __init__(self, b: bytes, o: int = 0):
        self.b, self.o = b, o

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.o)[0]; self.o += 4; return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4; return v

    def i64(self):
        v = struct.unpack_from("<q", self.b, self.o)[0]; self.o += 8; return v

    def raw(self, n):
        v = self.b[self.o : self.o + n]; self.o += n; return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            return self.raw(-n * 2).decode("utf-16-le").rstrip("\x00")
        return self.raw(n).decode("utf-8", "replace").rstrip("\x00")


class Export:
    __slots__ = ("index", "name", "class_name", "outer", "serial_offset",
                 "serial_size", "uexp_offset")

    def __repr__(self):
        return (f"<{self.index} {self.name} : {self.class_name} "
                f"uexp[{self.uexp_offset}:{self.uexp_offset + self.serial_size}]>")


class Package:
    def __init__(self, umap_path: str):
        self.path = umap_path
        self.head = open(umap_path, "rb").read()
        stem = os.path.splitext(umap_path)[0]
        self.uexp = b""
        for ext in (".uexp",):
            if os.path.exists(stem + ext):
                self.uexp = open(stem + ext, "rb").read()
        self._parse()

    def _parse(self):
        r = R(self.head)
        if r.u32() != PKG_TAG:
            raise SystemExit(f"{self.path}: not a UE package")
        legacy = r.i32()
        if legacy != -8:
            print(f"  ! unexpected legacy version {legacy}", file=sys.stderr)
        r.i32()                      # LegacyUE3Version
        self.ver_ue4 = r.i32()
        self.ver_ue5 = r.i32()
        r.i32()                      # licensee
        for _ in range(r.i32()):     # custom versions
            r.raw(16); r.i32()
        self.total_header_size = r.i32()
        self.folder_name = r.fstring()
        self.package_flags = r.u32()
        name_count, name_off = r.i32(), r.i32()
        r.i32(); r.i32()             # soft object paths count / offset
        r.i32(); r.i32()             # gatherable text count / offset
        export_count, export_off = r.i32(), r.i32()
        import_count, import_off = r.i32(), r.i32()

        # names
        n = R(self.head, name_off)
        self.names: list[str] = []
        for _ in range(name_count):
            s = n.fstring()
            n.raw(4)                 # non-case-preserving + case-preserving hash
            self.names.append(s)

        # imports (32 bytes each): classPkg, className, outer, objectName, bOptional
        self.imports: list[str] = []
        i = R(self.head, import_off)
        for _ in range(import_count):
            i.raw(8); i.raw(8); i.i32()
            nm = self._fname(i)
            i.i32()
            self.imports.append(nm)

        # exports (96 bytes each)
        self.exports: list[Export] = []
        e = R(self.head, export_off)
        for k in range(export_count):
            ex = Export()
            ex.index = k
            cls = e.i32(); e.i32(); e.i32()      # class, super, template
            ex.outer = e.i32()
            ex.name = self._fname(e)
            e.u32()                              # object flags
            ex.serial_size = e.i64()
            ex.serial_offset = e.i64()
            e.raw(96 - 44)                       # the rest of the struct
            ex.class_name = self._resolve(cls)
            ex.uexp_offset = ex.serial_offset - self.total_header_size
            self.exports.append(ex)

    def _fname(self, r: R) -> str:
        idx, num = r.i32(), r.i32()
        s = self.names[idx] if 0 <= idx < len(self.names) else f"<{idx}>"
        return s if num == 0 else f"{s}_{num - 1}"

    def _resolve(self, package_index: int) -> str:
        if package_index < 0:
            k = -package_index - 1
            return self.imports[k] if k < len(self.imports) else f"<imp{k}>"
        if package_index > 0:
            k = package_index - 1
            return self.exports[k].name if k < len(self.exports) else f"<exp{k}>"
        return "None"

    def data(self, ex: Export) -> bytes:
        return self.uexp[ex.uexp_offset : ex.uexp_offset + ex.serial_size]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("umap")
    ap.add_argument("--grep", default="")
    a = ap.parse_args(argv)
    p = Package(a.umap)
    print(f"{os.path.basename(a.umap)}  header={p.total_header_size} "
          f"uexp={len(p.uexp)}  names={len(p.names)} imports={len(p.imports)} "
          f"exports={len(p.exports)}")
    for ex in p.exports:
        if a.grep and a.grep.lower() not in (ex.name + ex.class_name).lower():
            continue
        print(f"  [{ex.index:3d}] {ex.name:<40} {ex.class_name:<28} "
              f"uexp {ex.uexp_offset:>10} + {ex.serial_size:>10}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
