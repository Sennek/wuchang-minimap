#!/usr/bin/env python3
"""
Unversioned-property decoding for Wuchang's cooked UE 5.1.1 packages -- enough
of it to read an actor's placement without a `.usmap`.

Why this works without mappings
-------------------------------
Wuchang cooks with `PKG_UnversionedProperties` (0x2000), so property *tags* are
gone and every serialized value is identified only by its index in the class's
schema.  A `.usmap` would hand us that schema.  We do not have one -- but we do
not need the whole schema, only three facts:

1.  `FUnversionedHeader` is **self-describing**: the fragment stream says which
    schema indices were written and which of them are zero.  No mappings needed.
    (`parse_header` below is a direct port of `FUnversionedHeader::Load`.)

2.  UE builds a struct's schema from `UStruct::PropertyLink`, which is
    **most-derived first, then the super chain**.  So for any
    `USceneComponent` subclass the engine's own `USceneComponent` block sits at
    a fixed *offset* `shift` = (number of properties declared by the subclasses
    ahead of it), and inside that block the order is fixed by UE 5.1's
    `SceneComponent.h`:

        shift+0  PhysicsVolume          shift+5  RelativeLocation  (FVector, 3 doubles)
        shift+1  AttachParent           shift+6  RelativeRotation  (FRotator, 3 doubles)
        shift+2  AttachSocketName       shift+7  RelativeScale3D   (FVector, 3 doubles)
        shift+3  AttachChildren
        shift+4  ClientAttachedChildren    shift+8  ComponentVelocity (FVector)

3.  `shift` is **learnable from the data itself**.  Every component export in
    these packages ends with the same three `UActorComponent` values -- schema
    indices `(B, zero) , (B+1) , (B+9)` -- where `B` is where the
    `UActorComponent` block starts.  For a plain `SceneComponent` export `B` is
    33, so `shift = B - 33` for any subclass, and
    `RelativeLocation index = B - 28`.

Everything is cross-checked: `RelativeScale3D` must be ~1, `AttachParent` must
resolve to a real export/import, and the byte accounting of
loc/rot/scale must fit inside the export.  See `context/markers-offline.md`.
"""

from __future__ import annotations

import math
import re
import struct

# --- USceneComponent block, UE 5.1 declaration order -----------------------
SC_PHYSICS_VOLUME = 0
SC_ATTACH_PARENT = 1
SC_ATTACH_SOCKET = 2
SC_ATTACH_CHILDREN = 3
SC_CLIENT_ATTACHED = 4
SC_REL_LOCATION = 5
SC_REL_ROTATION = 6
SC_REL_SCALE3D = 7
SC_COMPONENT_VELOCITY = 8

# `UActorComponent`'s block starts right after USceneComponent's 33 own props.
SCENECOMPONENT_OWN_PROPS = 33


def parse_header(b: bytes, o: int = 0):
    """Port of `FUnversionedHeader::Load`.

    Returns `(values, end_offset)` where `values` is an ordered list of
    `(schema_index, is_zero)`; zero values occupy no bytes in the payload.
    """
    frags = []
    zero_num = 0
    while True:
        (packed,) = struct.unpack_from("<H", b, o)
        o += 2
        skip = packed & 0x7F
        has_zero = bool(packed & 0x80)
        vnum = packed >> 9
        is_last = bool(packed & 0x100)
        frags.append((skip, has_zero, vnum))
        if has_zero:
            zero_num += vnum
        if is_last:
            break
        if o > len(b):
            raise ValueError("runaway fragment stream")
    bits = []
    if zero_num:
        if zero_num <= 8:
            nb = 1
        elif zero_num <= 16:
            nb = 2
        else:
            nb = ((zero_num + 31) // 32) * 4
        raw = int.from_bytes(b[o:o + nb], "little")
        o += nb
        bits = [(raw >> i) & 1 for i in range(zero_num)]
    values = []
    idx = 0
    zi = 0
    for skip, has_zero, vnum in frags:
        idx += skip
        for _ in range(vnum):
            if has_zero:
                values.append((idx, bool(bits[zi])))
                zi += 1
            else:
                values.append((idx, False))
            idx += 1
    return values, o


def actor_component_base(values) -> int | None:
    """Recover `B` (where the `UActorComponent` block starts) from the trailing
    `(B, zero) (B+1) (B+9)` signature that every component export carries."""
    if len(values) < 3:
        return None
    (i0, z0), (i1, _), (i2, _) = values[-3], values[-2], values[-1]
    if z0 and i1 == i0 + 1 and i2 == i0 + 9:
        return i0
    return None


class Schema:
    """Per-class `shift` table, learned from the packages themselves."""

    def __init__(self):
        self.shift: dict[str, int] = {}
        self.conflicts: dict[str, set] = {}

    def observe(self, class_name: str, values) -> None:
        b = actor_component_base(values)
        if b is None:
            return
        shift = b - SCENECOMPONENT_OWN_PROPS
        if shift < 0:
            return
        prev = self.shift.get(class_name)
        if prev is None:
            self.shift[class_name] = shift
        elif prev != shift:
            self.conflicts.setdefault(class_name, {prev}).add(shift)
            # keep the smallest: a class whose UActorComponent tail was partly
            # defaulted would read high, never low.
            self.shift[class_name] = min(prev, shift)

    def index_of(self, class_name: str, sc_index: int):
        s = self.shift.get(class_name)
        return None if s is None else s + sc_index


# --- value readers ---------------------------------------------------------


def value_offsets(values, sizes: dict[int, int]):
    """Walk the serialized values, returning `{schema_index: byte_offset}` for as
    far as every preceding value's size is known.  `sizes` maps schema index to
    byte size; an unknown index stops the walk."""
    out = {}
    off = 0
    for idx, is_zero in values:
        if is_zero:
            out[idx] = None          # implicit zero, no bytes
            continue
        out[idx] = off
        n = sizes.get(idx)
        if n is None:
            break
        off += n
    return out


def read_vec(data: bytes, off: int):
    x, y, z = struct.unpack_from("<ddd", data, off)
    return (x, y, z)


def finite_vec(v, limit=1e7, floor=1e-100):
    """A real UE coordinate is either exactly zero or an ordinary magnitude.
    Rejecting near-denormals (`4.2e-317`) is what makes a byte-offset scan usable:
    random bytes reinterpreted as doubles are overwhelmingly denormal or huge."""
    for c in v:
        if not math.isfinite(c) or abs(c) >= limit:
            return False
        if c != 0.0 and abs(c) < floor:
            return False
    return True


_STR_OK = re.compile(rb"^[\x20-\x7e]+$")


def find_strings(data: bytes, min_len: int = 3, max_len: int = 96):
    """Every plausible cooked `FString` (int32 length + ASCII + NUL) in a blob.

    Used to lift the game-authored shrine id (`digong01`) out of a
    `BP_RebornFire_C` export -- BP class schemas are unknown without a usmap,
    but a string literal in the payload is unambiguous."""
    out = []
    n = len(data)
    for off in range(0, n - 4):
        (ln,) = struct.unpack_from("<i", data, off)
        if not (min_len <= ln <= max_len):
            continue
        end = off + 4 + ln
        if end > n:
            continue
        s = data[off + 4:end]
        if s[-1:] != b"\x00":
            continue
        body = s[:-1]
        if not body or not _STR_OK.match(body):
            continue
        out.append((off, body.decode("ascii")))
    return out


# --- transforms ------------------------------------------------------------


def rot_matrix(pitch: float, yaw: float, roll: float):
    """UE `FRotationMatrix` rows (degrees in, row-major axes X, Y, Z)."""
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    return (
        (cp * cy, cp * sy, sp),
        (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
        (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp),
    )


def compose(parent_loc, parent_rot, parent_scale, child_loc):
    """World location of a child given the parent's world transform."""
    m = rot_matrix(*parent_rot)
    v = (child_loc[0] * parent_scale[0],
         child_loc[1] * parent_scale[1],
         child_loc[2] * parent_scale[2])
    return (
        parent_loc[0] + v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0],
        parent_loc[1] + v[0] * m[0][1] + v[1] * m[1][1] + v[2] * m[2][1],
        parent_loc[2] + v[0] * m[0][2] + v[1] * m[1][2] + v[2] * m[2][2],
    )
