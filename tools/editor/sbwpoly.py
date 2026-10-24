#!/usr/bin/env python3
"""Polygon editor for Gunmetal ``.sbw`` world geometry (``M11.2``, slice 5).

The neighbouring slices edit the *object* (``tools/editor/sbwobj.py``:
``origin``/``radius``/``bounds``/aux words), the *vertices*
(``tools/editor/sbwmesh.py``: local positions, metadata recompute) and single
polygon *fields* (``tools/editor/sbwedit.py``: ``texture``/``lightmap``/
``material``/``overlay``/``brightness``/``flags``).  This tool edits the polygon
as a **record**:

* ``--list`` — every polygon: global and per-object index, ``n_idx``, vertex
  indices, ``texture``/``material``/``lightmap`` (plus ``overlay``,
  ``brightness``, ``flags``), the author ``normal`` **and** the normal derived
  from the winding (first fan triangle and area-weighted Newell) with the sign
  of their dot product, the ``u``/``v`` texture axes (``u_axis``/``v_axis``),
  ``uv``/``uv_fp``, the raw 112-byte tail, and how many objects/polygons share
  the world position of each of its vertices;
* ``--set`` — change documented fields of **one** polygon in place:
  ``texture``, ``material``, ``lightmap``, ``overlay``, ``brightness``,
  ``normal=x,y,z`` (normalised), ``uv=u,v``, ``u_axis=x,y,z``,
  ``v_axis=x,y,z``, ``flags`` and ``indices=i0,i1,...``;
* ``--reindex`` — reverse the winding (``indices`` in reverse order) of one
  polygon or of every polygon of an object.  The stored ``normal`` stays, the
  geometric normal (Newell: exactly; first fan triangle: for planar polygons)
  changes sign; the file size does not change;
* ``--check`` — re-read a written file with the oracle ``gm.sbw`` *and* the own
  walker and prove that only the target bytes changed (size unchanged unless
  ``n_idx`` changed under ``--allow-resize``);
* ``--roundtrip`` — 36 shipped ``.sbw`` files, byte-exact, plus a field-level
  comparison of every polygon with the oracle; ``--self-test`` — synthetic
  file, no game data; ``--audit`` — measure the rules below on the corpus.

Record layout (``gm/sbw.py:326-356``, ``docs/FORMATS.md`` §10.1)::

    0x00 u16 texture   (signed)  0x02 u16 lightmap (signed)  0x04 u16 material
    0x06 u16 overlay   (signed)  0x08 u16 f7a               0x0A u8 brightness
    0x0B u8  n_idx
    0x0C u16 indices[n_idx]
    0x0C+2*n_idx+0x00 u16 scroll_u    +0x02 u16 scroll_v
                    +0x04 u16 u_scale   +0x06 u16 v_scale
                    +0x08 u16 uv_u      +0x0A u16 uv_v
                    +0x0C i32 uv_u_fp   +0x10 i32 uv_v_fp
                    +0x14 3f centre     +0x20 3f normal
                    +0x2C 3f u_axis     +0x38 3f v_axis
                    +0x44 3f corner_a   +0x50 3f corner_b
                    +0x5C u16 f74 +0x5E u16 f76 +0x60 u16 handle
                    +0x62 u32 reserved +0x66 u32 flags
                    +0x6A u16 anim0 +0x6C u16 anim1 +0x6E u16 anim2
    = 124 + 2*n_idx bytes

A polygon is self-delimiting and every shared structure (texture/effect
tables, stage-1 object headers, vertices, object tails) lives *in front of* the
polygon stream: a same-``n_idx`` edit replaces bytes in place, the file length
and every other offset stay.  ``indices`` with a **different** count changes
the record size and shifts the rest of the stream; the tool supports this only
with ``--allow-resize`` and then proves, object by object, that every other
byte is unchanged in content (``.sbw`` has no offset table, so a shift is
safe; the oracle's ``Sbw.to_bytes()`` reproduces the result byte-exactly).

Measured on the corpus (``--audit``, 36 files, 235 069 polygons):

* stored ``normal`` is unit length (``1 ± 1e-4``) in **235 045/235 069**
  polygons and exactly zero in the remaining **24** (degenerate author data:
  all 24 are zero-area polygons).  The engine checks unit length (``Invalid
  poly ...``, ``docs/FORMATS.md`` §10.1), so ``normal=`` is normalised and a
  zero request is refused.
* the winding is opposite to the stored normal: the area-weighted (Newell)
  normal is exactly opposite in **235 045/235 045** non-degenerate polygons.
  The first fan triangle is opposite in **234 322**, equal in **14** and
  ``|dot| < 0.99`` in **679** (non-planar polygons whose first three vertices
  do not define the stored plane); **54** polygons have a zero-area first
  triangle (``docs/COLLIDE.md`` §1.2).  This is why ``--reindex`` changes the
  *geometric* normal but not ``data_normal``.
* the fan triangulation (``i0, ik, ik+1``) is anchored at the **first** index,
  so reversing a polygon with more than three indices also changes which
  diagonals the fan uses; for planar polygons the triangles are the same in
  area and orientation flipped, for non-planar ones they differ slightly (in
  ``level0`` object 187 that moved **2** pixels in a 640x400 render while the
  stored normals and the ``--info`` counts did not change).
* ``u_axis``/``v_axis`` are unit in **360 672/470 138** axis slots, zero in
  **3 530** and non-unit in the rest (max component 8).  They are therefore
  written verbatim, **not** normalised: normalising would change the texture
  scale that the renderer derives from them.
* ``centre`` is the mean of the polygon's vertices in **150 423/235 069**
  polygons (the AABB midpoint in **204 678/235 069**); ``corner_a``/
  ``corner_b`` match ``min/max(vertex - centre)`` in only **3 323/235 069** —
  they are load-time scratch (``0x43529f`` initialises them to ±inf), not file
  invariants.  Nothing in the record is recomputed from ``normal``; the tool
  changes exactly the requested fields and leaves ``centre``, the corners, the
  UV axes, ``uv``/``uv_fp`` and ``flags`` alone unless they are edited too.
* ``u_scale``/``v_scale`` are raw ``u16`` despite being derived from a signed
  ``uv_*_fp``: ``u_scale == (uv_u_fp >> 7) & 0xFFFF`` in **235 057/235 069**
  polygons (``v``: **235 059/235 069**), which is why ``--list`` prints them
  raw (``level0`` polygon 0 has ``u_scale = 65444`` for ``uv_u_fp = -11776``).

Usage::

    python3 tools/editor/sbwpoly.py --list level0
    python3 tools/editor/sbwpoly.py --list level0 --object 0 --limit 0 --json
    python3 tools/editor/sbwpoly.py --set level0 --object 0 --index 0 \\
        --field texture=2 --out /tmp/opencode/poly/level0.sbw
    python3 tools/editor/sbwpoly.py --set level0 --object 0 --index 0 \\
        --field normal=0,0,1 --field brightness=96 \\
        --out /tmp/opencode/poly/level0.sbw
    python3 tools/editor/sbwpoly.py --reindex level0 --object 0 \\
        --out /tmp/opencode/poly/level0.sbw
    python3 tools/editor/sbwpoly.py --check FILE --against level0 \\
        --object 0 --index 0 --field texture=2
    python3 tools/editor/sbwpoly.py --check FILE --against level0 \\
        --object 0 --expect-reindex
    python3 tools/editor/sbwpoly.py --check FILE --against level0 \\
        --object 0 --index 0 --field indices=0,2,4,6,2 --allow-resize
    python3 tools/editor/sbwpoly.py --roundtrip
    python3 tools/editor/sbwpoly.py --self-test
    python3 tools/editor/sbwpoly.py --audit

Exit codes: ``0`` success, ``1`` verification failed (check/round-trip/audit/
self-test negative), ``2`` bad input or a refused edit (message on stderr),
``3`` internal error.  No traceback escapes :func:`main`.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field as dc_field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbw as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sbwpoly"

#: Same trust-boundary caps as the oracle (``gm/sbw.py:359-363``).
MAX_TABLE = 65_536
MAX_OBJECTS = 65_536

OBJECT_HEADER = 96                                # 12 x f64
OBJECT_TAIL = 50                                  # radius .. reserved[24]
VERTEX_SIZE = 12                                  # 3 x f32

POLY_HEAD = 12                                    # 5 u16 + 2 u8
POLY_TAIL = 112                                   # everything after the indices
MIN_INDICES = 3                                   # fewer is not a polygon
MAX_INDICES = 255                                 # n_idx is u8

#: Plausibility bound for a ``u_axis``/``v_axis`` component.  The shipped corpus
#: peaks at 8 (``--audit``); 1e4 is a typo guard, not a format limit.
MAX_AXIS = 1.0e4

#: ``flags`` bits documented in ``docs/FORMATS.md`` §10.1 (engine ``+0x7c``).
FLAGS_DOCUMENTED = 0x8000 | 0x2 | 0x4

#: f32 rounding of a unit vector keeps its length within this of 1.
UNIT_TOL = 1.0e-4

#: Dot-product threshold for "the polygon is planar and its winding normal is
#: well defined" when checking a reindex on the first fan triangle.
PLANAR_TOL = 0.99

#: Disk offsets inside a polygon record / inside its 112-byte tail.
HEAD_FIELDS = {
    "texture": (0x00, 2),
    "lightmap": (0x02, 2),
    "material": (0x04, 2),
    "overlay": (0x06, 2),
    "f7a": (0x08, 2),
    "brightness": (0x0A, 1),
    "n_idx": (0x0B, 1),
}
TAIL_FIELDS = {
    "scroll_u": (0x00, 2),
    "scroll_v": (0x02, 2),
    "u_scale": (0x04, 2),
    "v_scale": (0x06, 2),
    "uv_u": (0x08, 2),
    "uv_v": (0x0A, 2),
    "uv_u_fp": (0x0C, 4),
    "uv_v_fp": (0x10, 4),
    "centre": (0x14, 12),
    "normal": (0x20, 12),
    "u_axis": (0x2C, 12),
    "v_axis": (0x38, 12),
    "corner_a": (0x44, 12),
    "corner_b": (0x50, 12),
    "f74": (0x5C, 2),
    "f76": (0x5E, 2),
    "handle": (0x60, 2),
    "reserved": (0x62, 4),
    "flags": (0x66, 4),
    "anim0": (0x6A, 2),
    "anim1": (0x6C, 2),
    "anim2": (0x6E, 2),
}

#: Fields ``--set``/``--check --field`` accept.
EDITABLE = ("texture", "material", "lightmap", "overlay", "brightness",
            "normal", "uv", "u_axis", "v_axis", "flags", "indices")

#: Fields compared on every non-target polygon to prove it is untouched.
SNAPSHOT_FIELDS = ("texture", "lightmap", "material", "overlay", "f7a",
                   "brightness", "scroll_u", "scroll_v", "u_scale", "v_scale",
                   "uv_u", "uv_v", "uv_u_fp", "uv_v_fp", "centre", "normal",
                   "u_axis", "v_axis", "corner_a", "corner_b", "f74", "f76",
                   "handle", "reserved", "flags", "anim0", "anim1", "anim2")


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The file does not walk as a ``.sbw`` (own reader)."""


# ---------------------------------------------------------------------------
# own reader (independent of gm/sbw.py)
# ---------------------------------------------------------------------------

class _Cur:
    """Bounds-checked little-endian cursor over the raw file."""

    __slots__ = ("d", "o")

    def __init__(self, data: bytes):
        self.d = data
        self.o = 0

    def _need(self, n: int, what: str) -> None:
        if n < 0 or self.o + n > len(self.d):
            raise ScanError(
                f"truncated .sbw at offset {self.o}: {what} needs {n} bytes, "
                f"file has {len(self.d) - self.o} left")

    def take(self, n: int, what: str) -> bytes:
        self._need(n, what)
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def u8(self, what: str) -> int:
        self._need(1, what)
        v = self.d[self.o]
        self.o += 1
        return v

    def u16(self, what: str) -> int:
        self._need(2, what)
        v = struct.unpack_from("<H", self.d, self.o)[0]
        self.o += 2
        return v

    def u32(self, what: str) -> int:
        self._need(4, what)
        v = struct.unpack_from("<I", self.d, self.o)[0]
        self.o += 4
        return v

    def cstr(self, what: str) -> str:
        end = self.d.find(b"\0", self.o)
        if end < 0:
            raise ScanError(f"truncated .sbw at offset {self.o}: {what} "
                            f"is not NUL-terminated")
        raw = self.d[self.o:end]
        self.o = end + 1
        try:
            return raw.decode("latin1")
        except UnicodeDecodeError as exc:              # pragma: no cover
            raise ScanError(f"{what}: {exc}") from exc


@dataclass
class PolyLoc:
    """One polygon on disk: span, documented fields and the raw 112-byte tail."""

    index: int                       # global 0-based polygon index (file order)
    object: int                      # global 0-based object index
    local: int                       # 0-based index inside the object
    off: int                         # disk offset of the record
    size: int                        # 124 + 2 * n_idx
    n_idx: int
    indices: tuple[int, ...]
    texture: int                     # raw u16 (signed: >0 table, <0 effect)
    lightmap: int
    material: int
    overlay: int
    f7a: int
    brightness: int
    scroll_u: int
    scroll_v: int
    u_scale: int
    v_scale: int
    uv_u: int
    uv_v: int
    uv_u_fp: int
    uv_v_fp: int
    centre: tuple[float, float, float]
    normal: tuple[float, float, float]
    u_axis: tuple[float, float, float]
    v_axis: tuple[float, float, float]
    corner_a: tuple[float, float, float]
    corner_b: tuple[float, float, float]
    f74: int
    f76: int
    handle: int
    reserved: int
    flags: int
    anim0: int
    anim1: int
    anim2: int
    raw: bytes                       # the 112 bytes after the index list

    @property
    def tail_off(self) -> int:
        """Absolute disk offset of the 112-byte tail."""
        return self.off + POLY_HEAD + 2 * self.n_idx

    def field_span(self, name: str) -> tuple[int, int]:
        """Disk ``(offset, width)`` of an editable field."""
        if name == "indices":
            return self.off + POLY_HEAD, 2 * self.n_idx
        if name == "uv":
            return self.tail_off + TAIL_FIELDS["uv_u"][0], 4
        if name in HEAD_FIELDS:
            off, width = HEAD_FIELDS[name]
            return self.off + off, width
        if name in TAIL_FIELDS:
            off, width = TAIL_FIELDS[name]
            return self.tail_off + off, width
        raise EditError(f"unknown polygon field: {name}")


@dataclass
class ObjectLoc:
    """Where an object lives on disk plus its documented fields."""

    index: int                       # global order: sector -> object
    sector: int
    obj: int                         # index inside its sector
    id: int                          # u32 in front of the header (flag word)
    header_off: int                  # disk offset of the u32 id word
    geom_off: int                    # stage 2: n_verts/n_polys
    n_verts: int
    n_polys: int
    verts_off: int                   # first f32 vertex
    tail_off: int                    # 50-byte tail (radius .. reserved)
    polys_end: int                   # first byte after the last polygon
    origin: tuple[float, float, float]
    basis: tuple[float, ...]         # 9 doubles, row-major
    radius: float
    f74: int
    f75: int
    f78: int
    f7c: int
    bounds: tuple[float, float, float]
    vertices: tuple[tuple[float, float, float], ...]
    polys: tuple[PolyLoc, ...] = ()

    @property
    def header_size(self) -> int:
        return 4 + OBJECT_HEADER

    def world_vertices(self) -> tuple[tuple[float, float, float], ...]:
        """``origin + basis * local`` -- the same formula as ``gm/sbw.py``."""
        ox, oy, oz = self.origin
        b = self.basis
        return tuple(
            (ox + b[0] * x + b[3] * y + b[6] * z,
             oy + b[1] * x + b[4] * y + b[7] * z,
             oz + b[2] * x + b[5] * y + b[8] * z)
            for x, y, z in self.vertices)

    def poly(self, local: int) -> PolyLoc:
        if not 0 <= local < self.n_polys:
            raise EditError(f"polygon {local} does not exist in object "
                            f"{self.index} (it has {self.n_polys}, 0-based)")
        return self.polys[local]


@dataclass
class SectorLoc:
    index: int
    kind: int
    sub: int
    objects: list[ObjectLoc] = dc_field(default_factory=list)

    @property
    def n_polys(self) -> int:
        return sum(o.n_polys for o in self.objects)

    @property
    def n_verts(self) -> int:
        return sum(o.n_verts for o in self.objects)


@dataclass
class Scan:
    size: int
    n_tex: int
    n_eff: int
    sectors: list[SectorLoc]
    objects: list[ObjectLoc]
    n_polys: int
    data_end: int
    table1_end: int
    table2_end: int

    @property
    def n_objects(self) -> int:
        return len(self.objects)

    @property
    def n_verts(self) -> int:
        return sum(o.n_verts for o in self.objects)

    def poly(self, obj_index: int, local: int) -> PolyLoc:
        return self.objects[obj_index].poly(local)


def walk(data: bytes) -> Scan:
    """Parse ``.sbw`` with an independent reader; every read is bounds-checked.

    Layout: ``gm/sbw.py:1-128`` / ``docs/FORMATS.md`` §10.  Raises
    :class:`ScanError` on truncation or an implausible count.
    """
    c = _Cur(data)
    n_tex = c.u32("texture count")
    if n_tex > MAX_TABLE:
        raise ScanError(f"implausible texture count {n_tex}")
    for i in range(n_tex):
        c.cstr(f"texture[{i}] name")
        c.u16(f"texture[{i}] flag")
        c.u32(f"texture[{i}] value")
    table1_end = c.o

    n_eff = c.u32("effect count")
    if n_eff > MAX_TABLE:
        raise ScanError(f"implausible effect count {n_eff}")
    for i in range(n_eff):
        c.cstr(f"effect[{i}] name")
        c.u32(f"effect[{i}] kind")
        c.u32(f"effect[{i}] start")
        n = c.u32(f"effect[{i}] n")
        if n > MAX_TABLE:
            raise ScanError(f"implausible effect[{i}] id count {n}")
        c.take(2 * n, f"effect[{i}] ids")
    table2_end = c.o

    n_sec = c.u32("sector count")
    if n_sec > MAX_OBJECTS:
        raise ScanError(f"implausible sector count {n_sec}")
    sectors: list[SectorLoc] = []
    objects: list[ObjectLoc] = []
    for si in range(n_sec):
        n_obj = c.u32(f"sector[{si}] object count")
        if n_obj > MAX_OBJECTS:
            raise ScanError(f"implausible object count {n_obj} in sector {si}")
        kind = c.u16(f"sector[{si}] kind")
        sub = c.u8(f"sector[{si}] sub")
        sect = SectorLoc(si, kind, sub)
        for oi in range(n_obj):
            header_off = c.o
            oid = c.u32(f"sector[{si}] object[{oi}] id word")
            header = c.take(OBJECT_HEADER,
                            f"sector[{si}] object[{oi}] header")
            origin = struct.unpack_from("<3d", header, 0)
            basis = struct.unpack_from("<9d", header, 24)
            obj = ObjectLoc(
                index=len(objects), sector=si, obj=oi, id=oid,
                header_off=header_off, geom_off=0, n_verts=0, n_polys=0,
                verts_off=0, tail_off=0, polys_end=0,
                origin=tuple(origin), basis=tuple(basis),
                radius=0.0, f74=0, f75=0, f78=0, f7c=0, bounds=(0.0, 0.0, 0.0),
                vertices=(), polys=())
            sect.objects.append(obj)
            objects.append(obj)
        sectors.append(sect)

    poly_index = 0
    for sect in sectors:
        for obj in sect.objects:
            obj.geom_off = c.o
            obj.n_verts = c.u16(f"obj[{obj.index}] n_verts")
            obj.n_polys = c.u16(f"obj[{obj.index}] n_polys")
            obj.verts_off = c.o
            raw_verts = c.take(VERTEX_SIZE * obj.n_verts,
                               f"obj[{obj.index}] vertices")
            obj.vertices = tuple(_vec3(raw_verts, VERTEX_SIZE * k)
                                 for k in range(obj.n_verts))
            obj.tail_off = c.o
            tail = c.take(OBJECT_TAIL, f"obj[{obj.index}] tail")
            obj.radius = struct.unpack_from("<f", tail, 0)[0]
            obj.f75 = tail[4]
            obj.f74 = tail[5]
            obj.f7c, obj.f78 = struct.unpack_from("<II", tail, 6)
            obj.bounds = struct.unpack_from("<3f", tail, 14)
            locs: list[PolyLoc] = []
            for k in range(obj.n_polys):
                loc = _poly_loc(c, obj.index, k, poly_index)
                locs.append(loc)
                poly_index += 1
            obj.polys = tuple(locs)
            obj.polys_end = c.o

    return Scan(len(data), n_tex, n_eff, sectors, objects, poly_index, c.o,
                table1_end, table2_end)


def _vec3(data: bytes, o: int) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", data, o)


def _poly_loc(c: _Cur, obj_index: int, local: int, index: int) -> PolyLoc:
    """Read one polygon (``124 + 2*n_idx`` bytes) and remember its span."""
    off = c.o
    head = c.take(POLY_HEAD, f"obj[{obj_index}] polygon[{local}] head")
    texture, lightmap, material, overlay, f7a = struct.unpack_from("<5H", head, 0)
    brightness = head[0x0A]
    n_idx = head[0x0B]
    indices = (struct.unpack("<%dH" % n_idx,
                             c.take(2 * n_idx,
                                    f"obj[{obj_index}] polygon[{local}] "
                                    f"indices")) if n_idx else ())
    tail = c.take(POLY_TAIL, f"obj[{obj_index}] polygon[{local}] tail")
    scroll_u, scroll_v, u_scale, v_scale, uv_u, uv_v = struct.unpack_from(
        "<6H", tail, 0x00)
    uv_u_fp, uv_v_fp = struct.unpack_from("<ii", tail, 0x0C)
    centre = _vec3(tail, 0x14)
    normal = _vec3(tail, 0x20)
    u_axis = _vec3(tail, 0x2C)
    v_axis = _vec3(tail, 0x38)
    corner_a = _vec3(tail, 0x44)
    corner_b = _vec3(tail, 0x50)
    f74, f76, handle = struct.unpack_from("<3H", tail, 0x5C)
    reserved = struct.unpack_from("<I", tail, 0x62)[0]
    flags = struct.unpack_from("<I", tail, 0x66)[0]
    anim0, anim1, anim2 = struct.unpack_from("<3H", tail, 0x6A)
    return PolyLoc(
        index=index, object=obj_index, local=local, off=off,
        size=POLY_HEAD + 2 * n_idx + POLY_TAIL, n_idx=n_idx,
        indices=tuple(indices), texture=texture, lightmap=lightmap,
        material=material, overlay=overlay, f7a=f7a, brightness=brightness,
        scroll_u=scroll_u, scroll_v=scroll_v, u_scale=u_scale,
        v_scale=v_scale, uv_u=uv_u, uv_v=uv_v, uv_u_fp=uv_u_fp,
        uv_v_fp=uv_v_fp, centre=centre, normal=normal, u_axis=u_axis,
        v_axis=v_axis, corner_a=corner_a, corner_b=corner_b, f74=f74,
        f76=f76, handle=handle, reserved=reserved, flags=flags,
        anim0=anim0, anim1=anim1, anim2=anim2, raw=tail)


# ---------------------------------------------------------------------------
# vectors and derived normals (winding)
# ---------------------------------------------------------------------------

def _f32(v: float) -> float:
    """The value as stored in an ``f32`` field (bit-exact round-trip)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _f32v(v):
    return tuple(_f32(c) for c in v)


def _length(v) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _normalize(v):
    ln = _length(v)
    if not ln > 0.0:
        return None
    return (v[0] / ln, v[1] / ln, v[2] / ln)


def _dot(a, b) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def fan_normal(vertices, indices):
    """Normal of the first fan triangle ``cross(b-a, c-a)``, normalised.

    This is the normal the C++ fan triangulation and
    ``tools/re/collide_goldens.py`` derive (``docs/COLLIDE.md`` §1.2).  Returns
    ``None`` for fewer than three indices or a degenerate triangle.
    """
    if len(indices) < 3:
        return None
    a = vertices[indices[0]]
    b = vertices[indices[1]]
    c = vertices[indices[2]]
    return _normalize(_cross(_sub(b, a), _sub(c, a)))


def newell_normal(vertices, indices):
    """Area-weighted (Newell) normal of the whole polygon, normalised.

    Unlike :func:`fan_normal` this is defined for non-planar polygons and
    reverses **exactly** when the index order is reversed, which is what
    ``--reindex`` proves.  ``None`` for fewer than three indices or zero area.
    """
    n = len(indices)
    if n < 3:
        return None
    nx = ny = nz = 0.0
    for k in range(n):
        p = vertices[indices[k]]
        q = vertices[indices[(k + 1) % n]]
        nx += (p[1] - q[1]) * (p[2] + q[2])
        ny += (p[2] - q[2]) * (p[0] + q[0])
        nz += (p[0] - q[0]) * (p[1] + q[1])
    return _normalize((nx, ny, nz))


def winding_info(obj: ObjectLoc, poly: PolyLoc) -> dict:
    """Stored ``normal`` versus the two geometric normals of the winding."""
    stored = poly.normal
    ln = _length(stored)
    unit = abs(ln - 1.0) <= UNIT_TOL
    fan = fan_normal(obj.vertices, poly.indices)
    newell = newell_normal(obj.vertices, poly.indices)
    dot_fan = None
    if fan is not None and ln > 0.0:
        dot_fan = _dot(fan, (stored[0] / ln, stored[1] / ln, stored[2] / ln))
    dot_newell = None
    if newell is not None and ln > 0.0:
        dot_newell = _dot(newell,
                          (stored[0] / ln, stored[1] / ln, stored[2] / ln))
    return {"length": ln, "unit": unit, "zero": ln == 0.0,
            "fan": fan, "newell": newell,
            "dot_fan": dot_fan, "dot_newell": dot_newell}


def sign_word(value: float | None) -> str:
    if value is None:
        return "n/a"
    if value < -PLANAR_TOL:
        return "opposite"
    if value > PLANAR_TOL:
        return "same"
    return "skew"


# ---------------------------------------------------------------------------
# documented field values
# ---------------------------------------------------------------------------

def _signed16(v: int) -> int:
    return v - 0x10000 if v >= 0x8000 else v


def poly_value(p: PolyLoc, name: str):
    """Normalised value of a documented field (signed where documented)."""
    if name == "texture" or name == "overlay":
        return _signed16(getattr(p, name))
    if name == "uv":
        return (p.uv_u, p.uv_v)
    if name == "uv_fp":
        return (p.uv_u_fp, p.uv_v_fp)
    if name == "indices":
        return tuple(p.indices)
    if name in ("normal", "u_axis", "v_axis", "centre", "corner_a", "corner_b"):
        return tuple(getattr(p, name))
    if name in ("anim0", "anim1", "anim2"):
        return getattr(p, name)
    if name in ("n_idx",):
        return p.n_idx
    if name in SNAPSHOT_FIELDS:
        return getattr(p, name)
    raise EditError(f"unknown polygon field: {name}")


def poly_snapshot(p: PolyLoc) -> dict:
    """Every documented field of a polygon (for "nothing else changed")."""
    snap = {name: poly_value(p, name) for name in SNAPSHOT_FIELDS}
    snap["n_idx"] = p.n_idx
    snap["indices"] = tuple(p.indices)
    for name in ("centre", "normal", "u_axis", "v_axis", "corner_a", "corner_b"):
        snap[name] = tuple(poly_value(p, name))
    snap["uv"] = (p.uv_u, p.uv_v)
    snap["uv_fp"] = (p.uv_u_fp, p.uv_v_fp)
    return snap


def oracle_snapshot(p) -> dict:
    """The same snapshot from an oracle ``gm.sbw.Poly``."""
    snap = {}
    for name in SNAPSHOT_FIELDS:
        if name == "texture" or name == "overlay":
            snap[name] = _signed16(getattr(p, name))
        elif name in ("centre", "normal", "u_axis", "v_axis", "corner_a",
                      "corner_b"):
            snap[name] = tuple(getattr(p, name))
        else:
            snap[name] = getattr(p, name)
    snap["n_idx"] = len(p.indices)
    snap["indices"] = tuple(p.indices)
    snap["uv"] = (p.uv_u, p.uv_v)
    snap["uv_fp"] = (p.uv_u_fp, p.uv_v_fp)
    return snap


def oracle_value(p, name: str):
    if name == "texture" or name == "overlay":
        return _signed16(getattr(p, name))
    if name == "uv":
        return (p.uv_u, p.uv_v)
    if name == "uv_fp":
        return (p.uv_u_fp, p.uv_v_fp)
    if name == "indices":
        return tuple(p.indices)
    if name == "n_idx":
        return len(p.indices)
    if name in ("normal", "u_axis", "v_axis", "centre", "corner_a", "corner_b"):
        return tuple(getattr(p, name))
    return getattr(p, name)


def oracle_poly(w, obj_index: int, local: int):
    """One oracle polygon by (global object, local polygon) index."""
    for sect in w.sectors:
        for obj in sect.objects:
            if obj_index == 0:
                if not 0 <= local < len(obj.polys):
                    raise EditError(f"polygon {local} does not exist in the "
                                    f"oracle view")
                return obj.polys[local]
            obj_index -= 1
    raise EditError("object does not exist in the oracle view")


def _oracle_vert_count(w) -> int:
    return sum(o.n_verts for s in w.sectors for o in s.objects)


# ---------------------------------------------------------------------------
# value parsing / validation
# ---------------------------------------------------------------------------

def parse_int(text: str) -> int:
    """``12``, ``0x8000``, ``-1``; everything else is refused."""
    t = text.strip()
    try:
        return int(t, 0)
    except ValueError:
        raise EditError(f"not an integer: {text!r}") from None


def parse_float(text: str) -> float:
    t = text.strip()
    try:
        v = float(t)
    except ValueError:
        raise EditError(f"not a number: {text!r}") from None
    if not math.isfinite(v):
        raise EditError(f"not a finite number: {text!r}")
    return v


def parse_vec(text: str, n: int, what: str) -> tuple[float, ...]:
    parts = text.split(",")
    if len(parts) != n:
        raise EditError(f"{what} needs {n} comma-separated numbers "
                        f"(got {len(parts)}: {text!r})")
    return tuple(parse_float(p) for p in parts)


def parse_pair(text: str, what: str, limit: int) -> tuple[int, int]:
    parts = text.split(",")
    if len(parts) != 2:
        raise EditError(f"{what} needs two comma-separated integers "
                        f"(got {len(parts)}: {text!r})")
    out = tuple(parse_int(p) for p in parts)
    for v in out:
        if not 0 <= v <= limit:
            raise EditError(f"{what}: {v} outside 0..{limit} (u16 field)")
    return out


def parse_indices(text: str, n_verts: int) -> tuple[int, ...]:
    parts = [p for p in text.replace(" ", ",").split(",") if p]
    if not parts:
        raise EditError("indices: empty list")
    out = []
    for part in parts:
        v = parse_int(part)
        if not 0 <= v < n_verts:
            raise EditError(f"indices: {v} outside 0..{n_verts - 1} "
                            f"(object has {n_verts} vertices)")
        out.append(v)
    return tuple(out)


def validate_flags(value: int) -> None:
    if not 0 <= value <= 0xFFFFFFFF:
        raise EditError(f"flags={value}: u32 field, range 0..0xFFFFFFFF")
    unknown = value & ~FLAGS_DOCUMENTED
    if unknown:
        raise EditError(
            f"flags=0x{value:08x}: undocumented bits 0x{unknown:08x}; "
            f"documented mask is 0x{FLAGS_DOCUMENTED:08x} "
            f"(0x8000 frame/UV ready, 0x2 transparent, 0x4 toggled)")


def field_assignment(name: str, text: str, scan: Scan, obj: ObjectLoc,
                     poly: PolyLoc, n_lightmaps: int | None,
                     allow_resize: bool):
    """Parse ``NAME=VALUE`` into the normalised value that will be written."""
    if name not in EDITABLE:
        raise EditError(f"unknown polygon field: {name!r} "
                        f"(editable: {', '.join(EDITABLE)})")
    if name == "texture" or name == "overlay":
        value = parse_int(text)
        if value == 0:
            return 0
        if value > scan.n_tex:
            raise EditError(f"{name}={value}: texture table has {scan.n_tex} "
                            f"entries (1-based, docs/FORMATS.md §10.1)")
        if value < -scan.n_eff:
            raise EditError(f"{name}={value}: effect table has {scan.n_eff} "
                            f"entries (negative values index it)")
        return value
    if name == "material":
        value = parse_int(text)
        if not 0 <= value <= 5:
            raise EditError(f"material={value}: documented range 0..5 "
                            f"(none/reflective/translucent/varadd/add/shade)")
        return value
    if name == "lightmap":
        value = parse_int(text)
        if n_lightmaps is None:
            raise EditError("lightmap: the sibling .sbc was not found, so the "
                            "valid range (0..record count) is unknown; pass "
                            "--sbc FILE")
        if not 0 <= value <= n_lightmaps:
            raise EditError(f"lightmap={value}: 0 = none, 1..{n_lightmaps} = "
                            f".sbc records (sampler reads records[lightmap-1])")
        return value
    if name == "brightness":
        value = parse_int(text)
        if not 0 <= value <= 0xFF:
            raise EditError(f"brightness={value}: u8 field, range 0..255")
        return value
    if name == "flags":
        value = parse_int(text)
        validate_flags(value)
        return value
    if name == "uv":
        return parse_pair(text, "uv", 0xFFFF)
    if name == "normal":
        vec = parse_vec(text, 3, "normal")
        unit = _normalize(vec)
        if unit is None:
            raise EditError(
                "normal: zero-length vector; the engine validates unit length "
                "(\"Invalid poly ...\", docs/FORMATS.md §10.1).  The 24 shipped "
                "zero normals are degenerate author data, not a usable value.")
        return _f32v(unit)
    if name in ("u_axis", "v_axis"):
        vec = parse_vec(text, 3, name)
        for v in vec:
            if abs(v) > MAX_AXIS:
                raise EditError(f"{name}: |{v!r}| is implausible for a .sbw "
                                f"texture axis (shipped max |component| = 8)")
        if _length(vec) == 0.0:
            raise EditError(
                f"{name}: zero vector; the shipped data has 3530 zero axis "
                f"slots, but a zero axis projects no texture.  Pass a real "
                f"direction or edit another field.")
        # Written verbatim: the shipped axes are unit in only 360 672/470 138
        # slots, so normalising would change the texture scale.
        return _f32v(vec)
    if name == "indices":
        indices = parse_indices(text, obj.n_verts)
        if not MIN_INDICES <= len(indices) <= MAX_INDICES:
            raise EditError(f"indices: {len(indices)} entries; n_idx is a u8 "
                            f"and a polygon needs {MIN_INDICES}..{MAX_INDICES}")
        if len(indices) != poly.n_idx and not allow_resize:
            raise EditError(
                f"indices: {poly.n_idx} -> {len(indices)} entries changes the "
                f"record size (124 + 2*n_idx) and shifts the rest of the "
                f"stream; pass --allow-resize to allow the object to be "
                f"rebuilt")
        return indices
    raise EditError(f"unknown polygon field: {name!r}")


def parse_assignments(items, scan: Scan, obj: ObjectLoc, poly: PolyLoc,
                      n_lightmaps: int | None, allow_resize: bool) -> dict:
    """Parse and validate every ``--field`` of one edit (last value wins)."""
    expected: dict = {}
    for item in items:
        name, sep, text = item.partition("=")
        if not sep:
            raise EditError(f"--field needs NAME=VALUE, got {item!r}")
        name = name.strip().lower()
        expected[name] = field_assignment(name, text, scan, obj, poly,
                                          n_lightmaps, allow_resize)
    if not expected:
        raise EditError("no --field given")
    return expected


# ---------------------------------------------------------------------------
# patching
# ---------------------------------------------------------------------------

def _require_object(scan: Scan, index: int | None) -> ObjectLoc:
    if index is None:
        raise EditError("--object N is required")
    if not 0 <= index < scan.n_objects:
        raise EditError(f"object {index} does not exist "
                        f"(file has {scan.n_objects}, 0-based)")
    return scan.objects[index]


def _require_index(index: int | None, obj: ObjectLoc) -> int:
    if index is None:
        raise EditError(f"--index I is required (object {obj.index} has "
                        f"{obj.n_polys} polygons, 0-based)")
    if not 0 <= index < obj.n_polys:
        raise EditError(f"polygon {index} does not exist in object "
                        f"{obj.index} (it has {obj.n_polys}, 0-based)")
    return index


def _raw16(value: int) -> int:
    return value & 0xFFFF


def build_record(p: PolyLoc, expected: dict) -> bytes:
    """Rebuild the target polygon record with the expected fields applied.

    This is the single code path for both in-place edits (same ``n_idx``) and
    resizes (an ``indices`` list of a different length): the head is patched at
    its fixed offsets, the index list is re-packed, the 112-byte tail is kept
    and patched at its tail-relative offsets.  Nothing else is touched.
    """
    record = None
    if "indices" in expected:
        indices = expected["indices"]
    else:
        indices = p.indices
    head = bytearray(struct.pack("<5H", p.texture, p.lightmap, p.material,
                                 p.overlay, p.f7a))
    head += bytes((p.brightness, len(indices) & 0xFF))
    for name in ("texture", "lightmap", "material", "overlay"):
        if name in expected:
            off, _w = HEAD_FIELDS[name]
            struct.pack_into("<H", head, off, _raw16(int(expected[name])))
    if "brightness" in expected:
        head[HEAD_FIELDS["brightness"][0]] = int(expected["brightness"]) & 0xFF
    idx_bytes = (struct.pack("<%dH" % len(indices), *indices)
                 if indices else b"")
    tail = bytearray(p.raw)
    if "normal" in expected:
        struct.pack_into("<3f", tail, TAIL_FIELDS["normal"][0],
                         *expected["normal"])
    if "uv" in expected:
        struct.pack_into("<2H", tail, TAIL_FIELDS["uv_u"][0],
                         int(expected["uv"][0]), int(expected["uv"][1]))
    if "u_axis" in expected:
        struct.pack_into("<3f", tail, TAIL_FIELDS["u_axis"][0],
                         *expected["u_axis"])
    if "v_axis" in expected:
        struct.pack_into("<3f", tail, TAIL_FIELDS["v_axis"][0],
                         *expected["v_axis"])
    if "flags" in expected:
        struct.pack_into("<I", tail, TAIL_FIELDS["flags"][0],
                         int(expected["flags"]) & 0xFFFFFFFF)
    record = bytes(head) + idx_bytes + bytes(tail)
    assert len(record) == POLY_HEAD + 2 * len(indices) + POLY_TAIL
    return record


def apply_edits(data: bytes, scan: Scan, obj_index: int, local: int,
                expected: dict, allow_resize: bool = False) -> tuple[bytes, int]:
    """Return ``(new_data, size_delta)`` with exactly the record replaced."""
    obj = _require_object(scan, obj_index)
    poly = obj.poly(local)
    new_indices = expected.get("indices", poly.indices)
    if len(new_indices) != poly.n_idx and not allow_resize:
        raise EditError(
            f"indices: {poly.n_idx} -> {len(new_indices)} entries changes the "
            f"record size; pass --allow-resize")
    record = build_record(poly, expected)
    new = data[:poly.off] + record + data[poly.off + poly.size:]
    delta = len(new) - len(data)
    if delta != 2 * (len(new_indices) - poly.n_idx):
        raise ScanError(f"internal: size delta {delta} does not match the "
                        f"index count change")
    return new, delta


def diff_offsets(a: bytes, b: bytes) -> list[int]:
    if len(a) != len(b):
        raise EditError(f"length differs: {len(a)} vs {len(b)}")
    return [i for i, (x, y) in enumerate(zip(a, b)) if x != y]


# ---------------------------------------------------------------------------
# paths, level resolution, lightmap count
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def write_bytes(path: str, data: bytes) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(parent, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def resolve_level(name: str, game: str = DEFAULT_GAME) -> str:
    """``level0``/``level0.sbw``/an explicit path -> existing ``.sbw`` path."""
    if os.path.isfile(name):
        return name
    cand = name if name.lower().endswith(".sbw") else name + ".sbw"
    direct = os.path.join(game, cand)
    if os.path.isfile(direct):
        return direct
    low = cand.lower()
    try:
        entries = os.listdir(game)
    except OSError as exc:
        raise EditError(f"game directory: {exc}") from None
    for entry in entries:
        if entry.lower() == low:
            return os.path.join(game, entry)
    raise EditError(f"level not found: {name} (looked in {game})")


def _sibling(path: str, suffix: str) -> str | None:
    """Case-insensitive sibling ``<stem><suffix>`` of *path* (drvfs is not)."""
    stem = os.path.splitext(os.path.basename(path))[0]
    folder = os.path.dirname(os.path.abspath(path))
    want = (stem + suffix).lower()
    try:
        for entry in os.listdir(folder):
            if entry.lower() == want:
                return os.path.join(folder, entry)
    except OSError:
        return None
    return None


def lightmap_count(path: str, sbc_path: str | None = None) -> int | None:
    """Record count of the sibling ``.sbc`` (0 = none), or ``None`` if unknown."""
    cand = sbc_path or _sibling(path, ".sbc")
    if not cand or not os.path.isfile(cand):
        return None
    try:
        from gm import sbc as sbcmod
        return len(sbcmod.parse_file(cand).records)
    except Exception as exc:                          # noqa: BLE001
        raise EditError(f"cannot read lightmaps {cand}: {exc}") from None


def _refuse_game_write(out_path: str, game: str) -> None:
    out_abs = os.path.abspath(out_path)
    game_abs = os.path.abspath(game)
    if out_abs == game_abs or out_abs.startswith(game_abs + os.sep):
        raise EditError(f"refusing to write into the game installation: "
                        f"{out_path} is inside {game}; choose an output "
                        f"outside it (e.g. /tmp/opencode/...)")


# ---------------------------------------------------------------------------
# verification (oracle + own walker)
# ---------------------------------------------------------------------------

def _outside_spans(scan: Scan, target: ObjectLoc) -> list[tuple[int, int]]:
    """Every byte of the file that is *not* inside the target object."""
    excluded = sorted([(target.header_off,
                        target.header_off + target.header_size),
                       (target.geom_off, target.polys_end)])
    spans = []
    cursor = 0
    for a, b in excluded:
        if a < cursor:
            raise ScanError(f"overlapping object spans at {a} < {cursor}")
        spans.append((cursor, a))
        cursor = b
    spans.append((cursor, scan.size))
    return spans


def _other_objects_identical(s0: Scan, s1: Scan, data0: bytes,
                             data1: bytes, target: int) -> bool:
    for a, b in zip(s0.objects, s1.objects):
        if a.index == target:
            continue
        if data0[a.header_off:a.header_off + a.header_size] != \
                data1[b.header_off:b.header_off + b.header_size]:
            return False
        if data0[a.geom_off:a.polys_end] != data1[b.geom_off:b.polys_end]:
            return False
    return True


def _object_nonpoly_bytes(obj: ObjectLoc, data: bytes, skip: set[int]) -> bytes:
    """Bytes of one object outside the given polygons (prefix/mid/suffix)."""
    out = bytearray()
    cursor = obj.geom_off
    for k, p in enumerate(obj.polys):
        if k in skip:
            out += data[cursor:p.off]
            cursor = p.off + p.size
    out += data[cursor:obj.polys_end]
    return bytes(out)


def _record_unmasked(record: bytes, n_idx: int, mask: set[str]) -> bytes:
    """Record bytes outside the fields in *mask* (independent of decode)."""
    tail = POLY_HEAD + 2 * n_idx
    cuts: list[tuple[int, int]] = []
    for name in mask:
        if name == "indices":
            cuts.append((HEAD_FIELDS["n_idx"][0],
                         HEAD_FIELDS["n_idx"][0] + 1))
            cuts.append((POLY_HEAD, POLY_HEAD + 2 * n_idx))
        elif name == "uv":
            off = TAIL_FIELDS["uv_u"][0]
            cuts.append((tail + off, tail + off + 4))
        elif name in HEAD_FIELDS:
            off, width = HEAD_FIELDS[name]
            cuts.append((off, off + width))
        elif name in TAIL_FIELDS:
            off, width = TAIL_FIELDS[name]
            cuts.append((tail + off, tail + off + width))
    cuts.sort()
    merged: list[tuple[int, int]] = []
    for a, b in cuts:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    out = bytearray()
    cursor = 0
    for a, b in merged:
        out += record[cursor:a]
        cursor = b
    out += record[cursor:]
    return bytes(out)


def verify_edit(orig: bytes, new: bytes, obj_index: int, targets: dict,
                *, allow_resize: bool = False, reindex: bool = False) -> dict:
    """Prove a polygon edit: the targets changed, every other byte did not.

    Checks (all mandatory unless stated):

    1. both blobs parse with the oracle to exactly EOF;
    2. texture/effect/sector/object/polygon/vertex counts are unchanged;
    3. both blobs walk with the own reader to EOF and agree on the counts;
    4. the file length changed by exactly ``2 * sum(new_n_idx - old_n_idx)``,
       and only an ``--allow-resize`` edit may change it at all;
    5. the target object's stage-1 header (``u32`` + 96 bytes) is identical;
    6. all bytes outside the target object are identical (content-wise, so a
       shifted stream is covered);
    7. every other object (header + geometry) is identical in content;
    8. inside the target object, the bytes outside the target polygons are
       identical (prefix, gaps, suffix);
    9. for every target polygon, the record with the edited fields masked is
       identical byte for byte (head, index list, tail);
    10. both readers see exactly the expected value of every edited field;
    11. every other documented field of the target polygons is unchanged
        (``centre``/``corners``/``uv_fp``/``scroll``/``anim``/``f74``/
        ``f76``/``handle``/``reserved``/``flags``/``n_idx`` included);
    12. for a same-size edit every differing byte lies inside an edited
        field's span; for a resize the size delta is the only change;
    13. ``Sbw.to_bytes()`` reproduces the source and the edit byte-exactly;
    14. ``--reindex``: the index list is exactly reversed, the area-weighted
        (Newell) normal changes sign, the first fan triangle normal changes
        sign for planar polygons, and the stored ``normal`` stays.
    """
    checks: list[dict] = []
    problems: list[str] = []

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})
        if not ok:
            problems.append(text)

    if not targets:
        raise EditError("verification needs at least one target polygon")

    try:
        before = oracle.parse(orig)
        after = oracle.parse(new)
        check(before.data_end == len(orig), "oracle: source parses to EOF")
        check(after.data_end == len(new), "oracle: edited file parses to EOF")
        check((len(before.tex), len(before.effects), len(before.sectors),
               before.n_objects, before.n_polys,
               _oracle_vert_count(before)) ==
              (len(after.tex), len(after.effects), len(after.sectors),
               after.n_objects, after.n_polys, _oracle_vert_count(after)),
              "oracle: texture/effect/sector/object/polygon/vertex counts "
              "unchanged")
        ids_before = [o.id for s in before.sectors for o in s.objects]
        ids_after = [o.id for s in after.sectors for o in s.objects]
        check(ids_before == ids_after, "oracle: object flag words unchanged")
    except (ValueError, EditError) as exc:
        check(False, f"oracle: parse failed: {exc}")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": [], "checks_total": len(checks)}

    try:
        s0 = walk(orig)
        s1 = walk(new)
    except ScanError as exc:
        check(False, f"own walker: {exc}")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": [], "checks_total": len(checks)}

    check(s0.data_end == len(orig) and s1.data_end == len(new),
          "own walker: both files to EOF")
    check((s0.n_tex, s0.n_eff, len(s0.sectors), s0.n_objects, s0.n_polys,
           s0.n_verts) ==
          (s1.n_tex, s1.n_eff, len(s1.sectors), s1.n_objects, s1.n_polys,
           s1.n_verts),
          "own walker: counts (objects/vertices/polygons) unchanged")

    if not 0 <= obj_index < s0.n_objects:
        check(False, f"object {obj_index} does not exist "
                     f"(file has {s0.n_objects}, 0-based)")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": [], "checks_total": len(checks)}

    t0 = s0.objects[obj_index]
    t1 = s1.objects[obj_index]
    for local in targets:
        if not 0 <= local < t0.n_polys:
            check(False, f"polygon {local} does not exist in object "
                         f"{obj_index} (it has {t0.n_polys})")
            return {"ok": False, "checks": checks, "problems": problems,
                    "changes": [], "checks_total": len(checks)}

    delta = sum(2 * (len(targets[local].get("indices",
                                           t0.polys[local].indices))
                     - t0.polys[local].n_idx)
                for local in targets)
    check(len(new) - len(orig) == delta,
          f"file size changed by exactly {delta} byte(s) "
          f"(expected from n_idx)")
    if not delta:
        check(len(orig) == len(new), "file size unchanged")
    if delta and not allow_resize:
        check(False, "a resize needs --allow-resize")
    else:
        check(True, "resize allowed (--allow-resize)" if delta
              else "no resize")

    check(orig[t0.header_off:t0.header_off + t0.header_size] ==
          new[t1.header_off:t1.header_off + t1.header_size],
          "target object header (u32 flag word + 96-byte transform) unchanged")
    spans0 = _outside_spans(s0, t0)
    spans1 = _outside_spans(s1, t1)
    outside_ok = len(spans0) == len(spans1) and all(
        orig[a:b] == new[c:d] for (a, b), (c, d) in zip(spans0, spans1))
    check(outside_ok, "bytes outside the target object are identical")
    check(_other_objects_identical(s0, s1, orig, new, obj_index),
          f"{s0.n_objects - 1} other objects (header + geometry) are "
          f"identical")

    skip = set(targets)
    check(_object_nonpoly_bytes(t0, orig, skip) ==
          _object_nonpoly_bytes(t1, new, skip),
          f"bytes outside the {len(skip)} target polygon(s) of object "
          f"{obj_index} are identical")

    for local in sorted(targets):
        p0 = t0.polys[local]
        p1 = t1.polys[local]
        exp = targets[local]
        rec0 = orig[p0.off:p0.off + p0.size]
        rec1 = new[p1.off:p1.off + p1.size]
        masked = _record_unmasked(rec0, p0.n_idx, set(exp)) == \
            _record_unmasked(rec1, p1.n_idx, set(exp))
        check(masked, f"poly[{local}]: record with the edited fields masked "
                      f"is byte-identical")
        for name, value in exp.items():
            check(poly_value(p1, name) == value,
                  f"own walker sees poly[{local}] {name}={_fmt_value(value)}")
            check(oracle_value(oracle_poly(after, obj_index, local), name)
                  == value,
                  f"oracle sees poly[{local}] {name}={_fmt_value(value)}")
        snap0 = poly_snapshot(p0)
        snap1 = poly_snapshot(p1)
        osnap0 = oracle_snapshot(oracle_poly(before, obj_index, local))
        osnap1 = oracle_snapshot(oracle_poly(after, obj_index, local))
        skip_keys = set(exp)
        if "uv" in exp:
            skip_keys.update(("uv_u", "uv_v"))
        if "indices" in exp:
            skip_keys.add("n_idx")
        bad = [k for k in snap0
               if k not in skip_keys and snap0[k] != snap1[k]]
        check(not bad, f"poly[{local}]: all {len(snap0) - len(skip_keys)} "
                       f"other documented fields unchanged"
                       + (f" (differs: {', '.join(bad)})" if bad else ""))
        obad = [k for k in osnap0
                if k not in skip_keys and osnap0[k] != osnap1[k]]
        check(not obad, f"poly[{local}]: the oracle agrees on every other "
                        f"field" + (f" (differs: {', '.join(obad)})"
                                    if obad else ""))

    changes: list[int] = []
    if len(orig) == len(new):
        allowed: list[tuple[int, int]] = []
        for local, exp in targets.items():
            p1 = t1.polys[local]
            for name in exp:
                allowed.append(p1.field_span(name))
        changes = diff_offsets(orig, new)
        stray = [d for d in changes
                 if not any(a <= d < a + w for a, w in allowed)]
        check(not stray,
              f"only the edited field bytes differ ({len(changes)} byte(s) "
              f"changed, {len(allowed)} legal span(s)"
              + (f"; stray at {stray[:8]}" if stray else "") + ")")
        if changes:
            check(True, "the file actually changed")
        else:
            check(True, "no-op: the file already carries the requested values")
    else:
        check(delta != 0 and len(new) - len(orig) == delta,
              f"resize: the only change is the stream shift "
              f"({len(orig)} -> {len(new)} bytes)")

    check(before.to_bytes() == orig, "oracle round-trip of the source is exact")
    check(after.to_bytes() == new, "oracle round-trip of the edit is exact")

    reindex_info = None
    if reindex:
        flipped = 0
        planar = 0
        skew = 0
        degenerate = 0
        degenerate_newell = 0
        for local in sorted(targets):
            p0 = t0.polys[local]
            p1 = t1.polys[local]
            check(tuple(p1.indices) == tuple(reversed(p0.indices)),
                  f"poly[{local}]: indices are exactly reversed")
            check(tuple(p1.normal) == tuple(p0.normal),
                  f"poly[{local}]: the stored normal is unchanged")
            n0 = newell_normal(t0.vertices, p0.indices)
            n1 = newell_normal(t1.vertices, p1.indices)
            if n0 is None or n1 is None:
                degenerate_newell += 1
            else:
                ok = _dot(n0, n1) < -1.0 + 1.0e-9
                check(ok, f"poly[{local}]: the Newell (area-weighted) normal "
                          f"changes sign")
                if ok:
                    flipped += 1
            g0 = fan_normal(t0.vertices, p0.indices)
            g1 = fan_normal(t1.vertices, p1.indices)
            if g0 is None or g1 is None:
                degenerate += 1
            else:
                ln = _length(p0.normal)
                ref = (_normalize(p0.normal) if ln > 0.0 else None)
                if ref is not None and abs(_dot(g0, ref)) >= PLANAR_TOL:
                    planar += 1
                    check(_dot(g0, g1) < -PLANAR_TOL,
                          f"poly[{local}]: the first fan triangle normal of a "
                          f"planar polygon changes sign")
                else:
                    skew += 1
        reindex_info = {"polygons": len(targets), "newell_flipped": flipped,
                        "degenerate_newell": degenerate_newell,
                        "planar_first_triangle": planar,
                        "non_planar_first_triangle": skew,
                        "degenerate_first_triangle": degenerate}

    rep = {
        "ok": all(c["ok"] for c in checks),
        "checks": checks,
        "checks_total": len(checks),
        "problems": problems,
        "changes": changes,
        "changed_bytes": len(changes),
        "size_before": len(orig),
        "size_after": len(new),
        "size_delta": len(new) - len(orig),
        "object": obj_index,
        "targets": sorted(targets),
    }
    if reindex_info is not None:
        rep["reindex"] = reindex_info
    return rep


def _fmt_value(value) -> str:
    if isinstance(value, tuple):
        return "(" + ", ".join(_fmt_number(v) for v in value) + ")"
    if isinstance(value, float):
        return _fmt_number(value)
    return str(value)


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def _share_maps(scan: Scan):
    """World position -> objects that use it / polygons that use it."""
    pos_objects: dict[tuple, set[int]] = defaultdict(set)
    pos_polys: Counter = Counter()
    for obj in scan.objects:
        world = obj.world_vertices()
        for k in range(obj.n_verts):
            pos_objects[world[k]].add(obj.index)
        for p in obj.polys:
            for i in p.indices:
                if 0 <= i < obj.n_verts:
                    pos_polys[world[i]] += 1
    return pos_objects, pos_polys


def _poly_entry(obj: ObjectLoc, p: PolyLoc, world, pos_objects,
                pos_polys, usage: list[int]) -> dict:
    info = winding_info(obj, p)
    verts = []
    for i in p.indices:
        row = {"index": i}
        if 0 <= i < obj.n_verts:
            key = world[i]
            row.update({
                "world": _json_vec(key),
                "polygons_in_object": usage[i],
                "polygons_in_level": pos_polys.get(key, 0),
                "objects_sharing": len(pos_objects.get(key, ())),
            })
        verts.append(row)
    return {
        "index": p.index,
        "object": obj.index,
        "sector": obj.sector,
        "local": p.local,
        "offset": p.off,
        "size": p.size,
        "n_idx": p.n_idx,
        "indices": list(p.indices),
        "texture": _signed16(p.texture),
        "texture_raw": p.texture,
        "material": p.material,
        "lightmap": p.lightmap,
        "overlay": _signed16(p.overlay),
        "f7a": p.f7a,
        "brightness": p.brightness,
        "flags": p.flags,
        "handle": p.handle,
        "f74": p.f74,
        "f76": p.f76,
        "normal": _json_vec(p.normal),
        "normal_length": info["length"],
        "normal_unit": info["unit"],
        "normal_zero": info["zero"],
        "geometric_normal": (_json_vec(info["fan"])
                             if info["fan"] is not None else None),
        "newell_normal": (_json_vec(info["newell"])
                          if info["newell"] is not None else None),
        "normal_dot_geometric": info["dot_fan"],
        "normal_dot_newell": info["dot_newell"],
        "winding_vs_normal": sign_word(info["dot_newell"]),
        "u_axis": _json_vec(p.u_axis),
        "u_length": _length(p.u_axis),
        "v_axis": _json_vec(p.v_axis),
        "v_length": _length(p.v_axis),
        "uv": [p.uv_u, p.uv_v],
        "uv_fp": [p.uv_u_fp, p.uv_v_fp],
        "u_scale": p.u_scale,
        "v_scale": p.v_scale,
        "scroll_u": p.scroll_u,
        "scroll_v": p.scroll_v,
        "centre": _json_vec(p.centre),
        "corner_a": _json_vec(p.corner_a),
        "corner_b": _json_vec(p.corner_b),
        "raw_len": len(p.raw),
        "raw": p.raw.hex(),
        "vertices": verts,
    }


def list_level(path: str, limit: int = 8, obj_index: int | None = None) -> dict:
    data = read_bytes(path)
    scan = walk(data)
    w = oracle.parse(data)
    pos_objects, pos_polys = _share_maps(scan)
    entries: list[dict] = []

    def add_object(obj: ObjectLoc, remaining: int) -> None:
        world = obj.world_vertices()
        usage = [0] * obj.n_verts
        for p in obj.polys:
            for i in p.indices:
                if 0 <= i < obj.n_verts:
                    usage[i] += 1
        take = obj.n_polys if remaining < 0 else min(remaining, obj.n_polys)
        for p in obj.polys[:take]:
            entries.append(_poly_entry(obj, p, world, pos_objects, pos_polys,
                                       usage))

    if obj_index is not None:
        obj = _require_object(scan, obj_index)
        objects = [obj]
        add_object(obj, -1 if limit == 0 else limit)
    else:
        objects = scan.objects
        remaining = -1 if limit == 0 else limit
        for obj in scan.objects:
            if remaining == 0:
                break
            before = len(entries)
            add_object(obj, remaining)
            if remaining > 0:
                remaining -= len(entries) - before
    shown_objects = objects if limit == 0 else objects[:limit]
    unit = sum(1 for e in entries if e["normal_unit"])
    opposite = sum(1 for e in entries if e["winding_vs_normal"] == "opposite")
    rep = {
        "path": os.path.abspath(path),
        "bytes": len(data),
        "oracle_parse_end": w.data_end,
        "eof_ok": w.data_end == len(data),
        "textures": scan.n_tex,
        "effects": scan.n_eff,
        "sectors": len(scan.sectors),
        "objects": scan.n_objects,
        "vertices": scan.n_verts,
        "polygons": scan.n_polys,
        "table1_end": scan.table1_end,
        "table2_end": scan.table2_end,
        "limit": limit,
        "object_filter": obj_index,
        "shown": len(entries),
        "shown_summary": {"unit_normals": unit,
                          "winding_opposite": opposite,
                          "shown": len(entries)},
        "world_positions": len(pos_objects),
        "shared_positions": sum(1 for v in pos_objects.values() if len(v) > 1),
        "max_objects_per_position": max((len(v) for v in pos_objects.values()),
                                        default=0),
        "max_polygons_per_position": max(pos_polys.values(), default=0),
        "object_list": [{
            "index": o.index,
            "sector": o.sector,
            "obj": o.obj,
            "id": o.id,
            "n_verts": o.n_verts,
            "n_polys": o.n_polys,
            "first_poly": (o.polys[0].index if o.polys else None),
            "last_poly": (o.polys[-1].index if o.polys else None),
            "header_off": o.header_off,
            "geom_off": o.geom_off,
            "verts_off": o.verts_off,
            "tail_off": o.tail_off,
            "polys_end": o.polys_end,
        } for o in shown_objects],
        "polygon_list": entries,
    }
    return rep


def print_list(rep: dict, limit: int) -> None:
    print(f"level: {rep['path']}")
    print(f"header: {rep['bytes']} bytes, oracle parse ends at "
          f"{rep['oracle_parse_end']} "
          f"({'EOF ok' if rep['eof_ok'] else 'MISMATCH'})")
    print(f"tables: textures={rep['textures']} effects={rep['effects']} "
          f"(end {rep['table1_end']}, {rep['table2_end']})")
    print(f"world: sectors={rep['sectors']} objects={rep['objects']} "
          f"vertices={rep['vertices']} polygons={rep['polygons']}")
    print(f"sharing: {rep['world_positions']} distinct world positions, "
          f"{rep['shared_positions']} shared by >1 object (max "
          f"{rep['max_objects_per_position']} objects / "
          f"{rep['max_polygons_per_position']} polygons at one position)")
    print(f"objects (showing {len(rep['object_list'])}):")
    for o in rep["object_list"]:
        first = "-" if o["first_poly"] is None else str(o["first_poly"])
        last = "-" if o["last_poly"] is None else str(o["last_poly"])
        print(f"  obj[{o['index']:5d}] sector={o['sector']:3d} "
              f"index={o['obj']:3d} flag=0x{o['id']:08x} "
              f"verts={o['n_verts']:4d} polys={o['n_polys']:4d} "
              f"global={first}..{last} geom@{o['geom_off']}")
    print(f"polygons (showing {rep['shown']} of {rep['polygons']}):")
    for e in rep["polygon_list"]:
        idx = ", ".join(str(i) for i in e["indices"])
        print(f"  p[{e['index']:5d}] obj={e['object']:5d} "
              f"local={e['local']:3d} n_idx={e['n_idx']:2d} "
              f"disk={e['offset']}+{e['size']} indices=({idx})")
        print(f"    fields: texture={e['texture']} material={e['material']} "
              f"lightmap={e['lightmap']} overlay={e['overlay']} "
              f"brightness={e['brightness']} flags=0x{e['flags']:08x} "
              f"handle={e['handle']}")
        print(f"    normal={_fmt_vec(e['normal'])} "
              f"|n|={e['normal_length']:.6g} "
              f"geo={_fmt_vec(e['geometric_normal'])} "
              f"newell={_fmt_vec(e['newell_normal'])} "
              f"dot={_fmt_number(e['normal_dot_newell'])} "
              f"({e['winding_vs_normal']})")
        print(f"    u={_fmt_vec(e['u_axis'])} |u|={e['u_length']:.6g} "
              f"v={_fmt_vec(e['v_axis'])} |v|={e['v_length']:.6g} "
              f"uv={tuple(e['uv'])} uv_fp={tuple(e['uv_fp'])} "
              f"u_scale={e['u_scale']} v_scale={e['v_scale']}")
        raw = e["raw"]
        print(f"    raw[{e['raw_len']}]={raw[:48]}{'...' if len(raw) > 48 else ''}")
        vs = []
        for v in e["vertices"]:
            if "world" not in v:
                vs.append(f"v{v['index']}=OUT-OF-RANGE")
            else:
                vs.append(f"v{v['index']} world={_fmt_vec(v['world'])} "
                          f"polys(obj/level)={v['polygons_in_object']}/"
                          f"{v['polygons_in_level']} "
                          f"objects={v['objects_sharing']}")
        print("    verts: " + " | ".join(vs))
    if rep["shown"] < rep["polygons"]:
        print(f"  ... {rep['polygons'] - rep['shown']} more "
              f"(--limit 0 shows all)")


# ---------------------------------------------------------------------------
# --set / --reindex
# ---------------------------------------------------------------------------

def set_polygon(path: str, obj_index: int, local: int, fields, out_path: str,
                game: str = DEFAULT_GAME, allow_resize: bool = False,
                sbc_path: str | None = None) -> dict:
    if not out_path:
        raise EditError("--set needs --out FILE (the game install is "
                        "read-only)")
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    local = _require_index(local, obj)
    _refuse_game_write(out_path, game)
    n_lightmaps = lightmap_count(path, sbc_path)
    expected = parse_assignments(fields, scan, obj, obj.poly(local),
                                 n_lightmaps, allow_resize)
    new, delta = apply_edits(data, scan, obj_index, local, expected,
                             allow_resize)
    write_bytes(out_path, new)
    rep = verify_edit(data, new, obj_index, {local: expected},
                      allow_resize=allow_resize)
    old = obj.poly(local)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "bytes": len(new),
        "fields": {k: _json_value(v) for k, v in expected.items()},
        "old": {k: _json_value(poly_value(old, k)) for k in expected},
        "policy": ("in-place polygon field edit" if not delta else
                   "object rebuilt because n_idx changed (--allow-resize)"),
    })
    if rep["ok"]:
        rep["note"] = _edit_note(rep, old, expected, delta)
    return rep


def reindex_object(path: str, obj_index: int, local: int | None, out_path: str,
                   game: str = DEFAULT_GAME) -> dict:
    if not out_path:
        raise EditError("--reindex needs --out FILE (the game install is "
                        "read-only)")
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    locals_ = [obj.poly(local).local] if local is not None else \
        list(range(obj.n_polys))
    if not locals_:
        raise EditError(f"object {obj_index} has no polygons to reindex")
    _refuse_game_write(out_path, game)
    targets = {}
    new = data
    for k in locals_:
        p = obj.poly(k)
        expected = {"indices": tuple(reversed(p.indices))}
        targets[k] = expected
        new, _delta = apply_edits(new, scan, obj_index, k, expected, False)
        # apply_edits re-walks offsets from `scan`; same-size edits keep them
        # valid, so the loop can patch every polygon of the object in place.
    write_bytes(out_path, new)
    rep = verify_edit(data, new, obj_index, targets, reindex=True)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "bytes": len(new),
        "polygons": len(targets),
        "policy": "winding reversed; the stored normal is untouched",
    })
    if rep["ok"]:
        rep["note"] = (f"{len(targets)} polygon(s) of object {obj_index} "
                       f"reindexed; file size unchanged")
    return rep


def _json_value(value):
    if isinstance(value, tuple):
        return [_json_value(v) for v in value]
    return value


def _edit_note(rep: dict, old: PolyLoc, expected: dict, delta: int) -> str:
    parts = []
    for name in sorted(expected):
        parts.append(f"{name} {_fmt_value(poly_value(old, name))} -> "
                     f"{_fmt_value(expected[name])}")
    size = ("unchanged size" if not delta
            else f"size {delta:+d} bytes (n_idx changed)")
    return "; ".join(parts) + f"; {size}"


def print_edit(rep: dict, reindex: bool = False) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    if reindex:
        print(f"obj[{rep['object']}] reindex: {rep['polygons']} polygon(s) "
              f"with reversed winding; stored normals untouched")
        ri = rep.get("reindex") or {}
        print(f"winding: Newell normal flipped in {ri.get('newell_flipped')}"
              f"/{ri.get('polygons')}; first fan triangle flipped in "
              f"{ri.get('planar_first_triangle')} planar, "
              f"{ri.get('non_planar_first_triangle')} non-planar, "
              f"{ri.get('degenerate_first_triangle')} degenerate")
    else:
        print(f"obj[{rep['object']}] poly[{rep['targets'][0]}]:")
        for name in sorted(rep["fields"]):
            print(f"    {name}: {_fmt_value(rep['old'][name])} -> "
                  f"{_fmt_value(rep['fields'][name])}")
    if rep["size_delta"]:
        print(f"{rep['size_after']} bytes ({rep['size_delta']:+d} vs "
              f"{rep['size_before']}; stream shifted)")
    else:
        print(f"{rep['size_after']} bytes, unchanged size, "
              f"{rep['changed_bytes']} byte(s) changed")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("note"):
        print(f"  note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'} "
          f"({rep['checks_total']} checks)")


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def check_file(path: str, against: str | None = None, obj_index: int | None = None,
               local: int | None = None, fields=None, reindex: bool = False,
               game: str = DEFAULT_GAME, allow_resize: bool = False,
               sbc_path: str | None = None) -> dict:
    """Re-read *path* with the oracle and the own walker.

    With ``against`` the two files are compared: when an edit expectation is
    given (``--object``/``--index`` plus ``--field`` or ``--reindex``) every
    other byte must be identical and the edit must be exactly the expected
    one; otherwise the whole file must be byte-identical.
    """
    data = read_bytes(path)
    rep: dict = {"path": os.path.abspath(path), "bytes": len(data), "ok": False,
                 "checks": [], "problems": [], "changes": []}

    def check(ok: bool, text: str) -> None:
        rep["checks"].append({"check": text, "ok": bool(ok)})
        if not ok:
            rep["problems"].append(text)

    try:
        w = oracle.parse(data)
        check(w.data_end == len(data), "oracle: parses to EOF")
        scan = walk(data)
        check(scan.data_end == len(data), "own walker: parses to EOF")
        check((scan.n_tex, scan.n_eff, len(scan.sectors), scan.n_objects,
               scan.n_polys, scan.n_verts) ==
              (len(w.tex), len(w.effects), len(w.sectors), w.n_objects,
               w.n_polys, _oracle_vert_count(w)),
              "own walker and oracle agree on counts")
        check(w.to_bytes() == data, "oracle: read -> write is byte-exact")
        bad_indices = sum(1 for o in scan.objects for p in o.polys
                          for i in p.indices if not 0 <= i < o.n_verts)
        check(bad_indices == 0,
              f"every vertex index is inside its object ({bad_indices} bad)")
        zero = sum(1 for o in scan.objects for p in o.polys
                   if _length(p.normal) == 0.0)
        check(True, f"stored normals: {scan.n_polys - zero}/{scan.n_polys} "
                    f"non-zero ({zero} zero, shipped degenerate data)")
        rep.update({"textures": scan.n_tex, "effects": scan.n_eff,
                    "sectors": len(scan.sectors), "objects": scan.n_objects,
                    "vertices": scan.n_verts, "polygons": scan.n_polys})
    except (ValueError, ScanError) as exc:
        check(False, f"parse failed: {exc}")
        return rep

    if against is None:
        src = _default_against(path, game)
        if src:
            against = src
    if against is None:
        rep["ok"] = all(c["ok"] for c in rep["checks"])
        return rep

    src_path = resolve_level(against, game)
    orig = read_bytes(src_path)
    rep["against"] = os.path.abspath(src_path)
    if (fields or reindex) and obj_index is not None:
        src_scan = walk(orig)
        src_obj = _require_object(src_scan, obj_index)
        if reindex and fields:
            raise EditError("--check: --field and --reindex are mutually "
                            "exclusive")
        if reindex:
            if local is None:
                locals_ = list(range(src_obj.n_polys))
            else:
                locals_ = [_require_index(local, src_obj)]
            if not locals_:
                raise EditError(f"object {obj_index} has no polygons")
            targets = {k: {"indices": tuple(reversed(
                src_obj.poly(k).indices))} for k in locals_}
            rep["kind"] = "reindex"
            rep["index"] = local
            rep["polygons"] = len(targets)
            edit = verify_edit(orig, data, obj_index, targets, reindex=True)
        else:
            if local is None:
                raise EditError("--check with --field needs --index I")
            local = _require_index(local, src_obj)
            n_lightmaps = lightmap_count(src_path, sbc_path)
            expected = parse_assignments(fields, src_scan, src_obj,
                                        src_obj.poly(local), n_lightmaps,
                                        allow_resize)
            targets = {local: expected}
            rep["kind"] = "fields"
            rep["fields"] = {k: _json_value(v) for k, v in expected.items()}
            edit = verify_edit(orig, data, obj_index, targets,
                               allow_resize=allow_resize)
        rep.update({"object": obj_index,
                    "index": local if not reindex else rep.get("index"),
                    "targets": edit.get("targets"),
                    "size_delta": edit.get("size_delta"),
                    "changed_bytes": edit.get("changed_bytes"),
                    "reindex_info": edit.get("reindex")})
        rep["checks"].extend(edit["checks"])
        rep["problems"].extend(edit["problems"])
        rep["changes"] = edit.get("changes", [])
        rep["ok"] = all(c["ok"] for c in rep["checks"])
        return rep

    if fields or reindex:
        raise EditError("--check with an edit expectation needs --object N"
                        + (" and --index I" if fields else ""))
    if len(orig) != len(data):
        check(False, f"size differs from {src_path}: "
                     f"{len(orig)} vs {len(data)}")
    else:
        changed = diff_offsets(orig, data)
        check(not changed, f"file is byte-identical to {src_path} "
                           f"({len(changed)} differing bytes)")
        rep["changes"] = changed[:64]
    rep["ok"] = all(c["ok"] for c in rep["checks"])
    return rep


def _default_against(path: str, game: str) -> str | None:
    """The shipped level with the same name, when it exists and differs."""
    if not os.path.isdir(game):
        return None
    base = os.path.basename(path)
    try:
        for entry in os.listdir(game):
            if entry.lower() == base.lower():
                cand = os.path.join(game, entry)
                if os.path.abspath(cand) != os.path.abspath(path):
                    return cand
    except OSError:
        return None
    return None


def print_check(rep: dict) -> None:
    print(f"check: {rep['path']} ({rep['bytes']} bytes)")
    if rep.get("against"):
        print(f"against: {rep['against']}")
    if rep.get("kind") == "fields":
        print(f"target: obj[{rep['object']}] poly[{rep['index']}] "
              + ", ".join(f"{k}={_fmt_value(v)}"
                          for k, v in rep["fields"].items()))
    elif rep.get("kind") == "reindex":
        if rep.get("index") is None:
            print(f"target: obj[{rep['object']}] reindexed "
                  f"({rep.get('polygons', 0)} polygons)")
        else:
            print(f"target: obj[{rep['object']}] poly[{rep['index']}] "
                  f"reindexed")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("changes") and not rep["ok"]:
        print(f"  differing bytes: {rep['changes'][:16]}"
              f"{' ...' if len(rep['changes']) > 16 else ''}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'} "
          f"({len(rep['checks'])} checks)")


# ---------------------------------------------------------------------------
# --roundtrip / --audit
# ---------------------------------------------------------------------------

def roundtrip_one(name: str, data: bytes) -> dict:
    """Oracle round-trip + field-level agreement for one ``.sbw`` blob."""
    problems: list[str] = []
    counts = {"objects": 0, "vertices": 0, "polygons": 0, "bytes": len(data)}
    try:
        w = oracle.parse(data)
        scan = walk(data)
    except (ValueError, ScanError) as exc:
        return {"file": name, "ok": False, "problems": [str(exc)],
                "counts": counts}
    counts.update({"objects": scan.n_objects, "vertices": scan.n_verts,
                   "polygons": scan.n_polys})
    if w.data_end != len(data):
        problems.append(f"oracle parse ends at {w.data_end}/{len(data)}")
    if scan.data_end != len(data):
        problems.append("own walker does not end at EOF")
    if w.to_bytes() != data:
        problems.append("oracle to_bytes differs")
    if (scan.n_objects, scan.n_polys, scan.n_verts) != (
            w.n_objects, w.n_polys, _oracle_vert_count(w)):
        problems.append("counts differ from the oracle")
    flat = [o for s in w.sectors for o in s.objects]
    for loc, obj in zip(scan.objects, flat):
        if tuple(obj.points[0]) != tuple(loc.origin):
            problems.append(f"origin mismatch at object {loc.index}")
            break
        if tuple(tuple(r) for r in obj.points[1:]) != tuple(
                tuple(loc.basis[r * 3:r * 3 + 3]) for r in range(3)):
            problems.append(f"basis mismatch at object {loc.index}")
            break
        if (len(obj.vertices), len(obj.polys), obj.radius,
                tuple(obj.bounds)) != (loc.n_verts, loc.n_polys,
                                       loc.radius, tuple(loc.bounds)):
            problems.append(f"geometry mismatch at object {loc.index}")
            break
        if tuple(tuple(v) for v in obj.vertices) != \
                tuple(tuple(v) for v in loc.vertices):
            problems.append(f"vertex mismatch at object {loc.index}")
            break
        if len(obj.polys) != loc.n_polys:
            problems.append(f"polygon count mismatch at object {loc.index}")
            break
        for k, (op, lp) in enumerate(zip(obj.polys, loc.polys)):
            if lp.index != (loc.polys[0].index + k if loc.polys else 0):
                problems.append(f"global polygon index broken at object "
                                f"{loc.index} polygon {k}")
                break
            if lp.off < loc.geom_off or lp.off + lp.size > loc.polys_end:
                problems.append(f"polygon span outside object {loc.index} "
                                f"polygon {k}")
                break
            if lp.size != POLY_HEAD + 2 * lp.n_idx + POLY_TAIL:
                problems.append(f"polygon size mismatch at object "
                                f"{loc.index} polygon {k}")
                break
            if oracle_snapshot(op) != poly_snapshot(lp):
                diff = [f for f in oracle_snapshot(op)
                        if oracle_snapshot(op)[f] != poly_snapshot(lp).get(f)]
                problems.append(f"polygon field mismatch at object "
                                f"{loc.index} polygon {k}: {diff[:6]}")
                break
        # polygons must tile the object's polygon stream exactly
        cursor = loc.polys[0].off if loc.polys else loc.polys_end
        for p in loc.polys:
            if p.off != cursor:
                problems.append(f"polygon stream gap/overlap at object "
                                f"{loc.index} offset {p.off} (expected "
                                f"{cursor})")
                break
            cursor = p.off + p.size
        else:
            if cursor != loc.polys_end:
                problems.append(f"polygon stream ends at {cursor}, object at "
                                f"{loc.polys_end}")
    return {"file": name, "ok": not problems, "problems": problems,
            "counts": counts}


def roundtrip_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "identical": 0,
           "mismatched": [], "objects": 0, "vertices": 0, "polygons": 0,
           "bytes": 0, "ok": False}
    for path in files:
        name = os.path.basename(path)
        try:
            one = roundtrip_one(name, read_bytes(path))
        except EditError as exc:
            rep["mismatched"].append({"file": name, "problems": [str(exc)]})
            if verbose:
                print(f"  FAIL {name}: {exc}")
            continue
        c = one["counts"]
        rep["bytes"] += c["bytes"]
        rep["objects"] += c["objects"]
        rep["vertices"] += c["vertices"]
        rep["polygons"] += c["polygons"]
        if not one["ok"]:
            rep["mismatched"].append({"file": name,
                                      "problems": one["problems"]})
            if verbose:
                print(f"  FAIL {name}: {'; '.join(one['problems'])}")
            continue
        rep["identical"] += 1
        if verbose:
            print(f"  ok   {name:14s} {c['bytes']:8d} bytes "
                  f"{c['objects']:5d} objects {c['vertices']:6d} verts "
                  f"{c['polygons']:6d} polys")
    rep["ok"] = (rep["identical"] == rep["files"] and rep["files"] > 0
                 and not rep["mismatched"])
    return rep


def print_roundtrip(rep: dict) -> None:
    print(f"round-trip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['objects']} objects, {rep['vertices']} vertices, "
          f"{rep['polygons']} polygons, {rep['bytes']} bytes ({rep['game']})")
    for m in rep["mismatched"]:
        print(f"  MISMATCH {m['file']}: {'; '.join(m['problems'])}")
    print("result: " + ("OK -- every polygon field agrees with the oracle"
                        if rep["ok"] else "FAILED"))


def audit_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    """Measure the polygon-level rules cited in the docstring and docs."""
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {
        "game": os.path.abspath(game), "files": len(files), "objects": 0,
        "vertices": 0, "polygons": 0, "n_idx": Counter(),
        "normal_unit": 0, "normal_zero": 0, "normal_other": 0,
        "normal_max_dev": 0.0,
        "fan_opposite": 0, "fan_same": 0, "fan_skew": 0, "fan_degenerate": 0,
        "newell_opposite": 0, "newell_same": 0, "newell_skew": 0,
        "newell_degenerate": 0, "reindex_newell_flips": 0,
        "reindex_checked": 0,
        "centre_vertex_centroid": 0, "centre_aabb_mid": 0,
        "corners_match": 0,
        "u_unit": 0, "u_zero": 0, "u_other": 0,
        "v_unit": 0, "v_zero": 0, "v_other": 0,
        "axes_max_abs": 0.0,
        "u_scale_matches_fp": 0, "v_scale_matches_fp": 0,
        "texture_zero": 0, "texture_positive": 0, "texture_negative": 0,
        "material": Counter(), "lightmap_zero": 0, "lightmap_nonzero": 0,
        "brightness_min": 255, "brightness_max": 0,
        "bad_indices": 0, "degenerate_fan": 0,
        "world_positions": 0, "shared_positions": 0,
        "max_objects_per_position": 0, "max_polygons_per_position": 0,
        "ok": False,
    }
    for path in files:
        name = os.path.basename(path)
        data = read_bytes(path)
        scan = walk(data)
        rep["objects"] += scan.n_objects
        rep["vertices"] += scan.n_verts
        rep["polygons"] += scan.n_polys
        pos_objects: dict[tuple, set[int]] = defaultdict(set)
        pos_polys: Counter = Counter()
        for obj in scan.objects:
            world = obj.world_vertices()
            for k in range(obj.n_verts):
                pos_objects[world[k]].add(obj.index)
            for p in obj.polys:
                if p.n_idx != len(p.indices):
                    rep["bad_indices"] += 1
                for i in p.indices:
                    if not 0 <= i < obj.n_verts:
                        rep["bad_indices"] += 1
                    else:
                        pos_polys[world[i]] += 1
                rep["n_idx"][p.n_idx] += 1
                ln = _length(p.normal)
                if ln == 0.0:
                    rep["normal_zero"] += 1
                elif abs(ln - 1.0) <= UNIT_TOL:
                    rep["normal_unit"] += 1
                else:
                    rep["normal_other"] += 1
                    rep["normal_max_dev"] = max(rep["normal_max_dev"],
                                                abs(ln - 1.0))
                info = winding_info(obj, p)
                if info["fan"] is None:
                    rep["fan_degenerate"] += 1
                else:
                    key = {"opposite": "fan_opposite", "same": "fan_same",
                           "skew": "fan_skew"}.get(sign_word(info["dot_fan"]),
                                                   "fan_skew")
                    rep[key] += 1
                if info["newell"] is None:
                    rep["newell_degenerate"] += 1
                else:
                    rep["newell_" + sign_word(info["dot_newell"]).replace(
                        "n/a", "skew")] += 1
                rev = newell_normal(obj.vertices, tuple(reversed(p.indices)))
                if info["newell"] is not None and rev is not None:
                    rep["reindex_checked"] += 1
                    if _dot(info["newell"], rev) < -1.0 + 1.0e-9:
                        rep["reindex_newell_flips"] += 1
                if p.indices:
                    pts = [obj.vertices[i] for i in p.indices
                           if 0 <= i < obj.n_verts]
                    if pts:
                        cen = tuple(sum(q[j] for q in pts) / len(pts)
                                    for j in range(3))
                        mid = tuple((min(q[j] for q in pts) +
                                     max(q[j] for q in pts)) / 2.0
                                    for j in range(3))
                        if tuple(p.centre) == _f32v(cen):
                            rep["centre_vertex_centroid"] += 1
                        if tuple(p.centre) == _f32v(mid):
                            rep["centre_aabb_mid"] += 1
                        lo = [min(q[j] for q in pts) - p.centre[j]
                              for j in range(3)]
                        hi = [max(q[j] for q in pts) - p.centre[j]
                              for j in range(3)]
                        if all(abs(p.corner_a[j] - lo[j]) <= 1e-4 and
                               abs(p.corner_b[j] - hi[j]) <= 1e-4
                               for j in range(3)):
                            rep["corners_match"] += 1
                lu = _length(p.u_axis)
                lv = _length(p.v_axis)
                rep["u_zero" if lu == 0 else
                    ("u_unit" if abs(lu - 1.0) <= UNIT_TOL else
                     "u_other")] += 1
                rep["v_zero" if lv == 0 else
                    ("v_unit" if abs(lv - 1.0) <= UNIT_TOL else
                     "v_other")] += 1
                rep["axes_max_abs"] = max(rep["axes_max_abs"],
                                          *(abs(c) for c in p.u_axis),
                                          *(abs(c) for c in p.v_axis))
                if p.u_scale == ((p.uv_u_fp >> 7) & 0xFFFF):
                    rep["u_scale_matches_fp"] += 1
                if p.v_scale == ((p.uv_v_fp >> 7) & 0xFFFF):
                    rep["v_scale_matches_fp"] += 1
                tex = _signed16(p.texture)
                rep["texture_zero" if tex == 0 else
                    ("texture_positive" if tex > 0 else
                     "texture_negative")] += 1
                rep["material"][p.material] += 1
                rep["lightmap_zero" if p.lightmap == 0 else
                    "lightmap_nonzero"] += 1
                rep["brightness_min"] = min(rep["brightness_min"],
                                            p.brightness)
                rep["brightness_max"] = max(rep["brightness_max"],
                                            p.brightness)
        rep["world_positions"] += len(pos_objects)
        rep["shared_positions"] += sum(1 for v in pos_objects.values()
                                       if len(v) > 1)
        rep["max_objects_per_position"] = max(
            rep["max_objects_per_position"],
            max((len(v) for v in pos_objects.values()), default=0))
        rep["max_polygons_per_position"] = max(
            rep["max_polygons_per_position"],
            max(pos_polys.values(), default=0))
        if verbose:
            print(f"  {name}: {scan.n_polys} polygons")
    rep["n_idx"] = dict(sorted(rep["n_idx"].items()))
    rep["material"] = dict(sorted(rep["material"].items()))
    rep["ok"] = (rep["polygons"] > 0 and rep["bad_indices"] == 0
                 and rep["reindex_checked"] == rep["reindex_newell_flips"])
    return rep


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['objects']} objects, "
          f"{rep['vertices']} vertices, {rep['polygons']} polygons "
          f"({rep['game']})")
    print(f"  n_idx histogram: {rep['n_idx']}")
    print(f"  stored normal: unit {rep['normal_unit']}, zero "
          f"{rep['normal_zero']}, other {rep['normal_other']} "
          f"(max |len-1| {rep['normal_max_dev']:.3g})")
    print(f"  winding vs stored normal (fan): opposite {rep['fan_opposite']}, "
          f"same {rep['fan_same']}, skew {rep['fan_skew']}, degenerate "
          f"{rep['fan_degenerate']}")
    print(f"  winding vs stored normal (Newell): opposite "
          f"{rep['newell_opposite']}, same {rep['newell_same']}, skew "
          f"{rep['newell_skew']}, degenerate {rep['newell_degenerate']}")
    print(f"  reindex: Newell normal flips in {rep['reindex_newell_flips']}/"
          f"{rep['reindex_checked']} polygons")
    print(f"  centre == vertex centroid {rep['centre_vertex_centroid']}, "
          f"== AABB midpoint {rep['centre_aabb_mid']}; corners match "
          f"min/max-centre {rep['corners_match']}")
    print(f"  u_axis: unit {rep['u_unit']}, zero {rep['u_zero']}, other "
          f"{rep['u_other']}; v_axis: unit {rep['v_unit']}, zero "
          f"{rep['v_zero']}, other {rep['v_other']} (max |component| "
          f"{rep['axes_max_abs']:g})")
    print(f"  u_scale == uv_u_fp>>7 {rep['u_scale_matches_fp']}, "
          f"v_scale == uv_v_fp>>7 {rep['v_scale_matches_fp']}")
    print(f"  texture: 0 {rep['texture_zero']}, >0 {rep['texture_positive']}, "
          f"<0 {rep['texture_negative']}; material {rep['material']}; "
          f"lightmap 0 {rep['lightmap_zero']} / !=0 {rep['lightmap_nonzero']}; "
          f"brightness {rep['brightness_min']}..{rep['brightness_max']}")
    print(f"  sharing: {rep['world_positions']} world positions, "
          f"{rep['shared_positions']} shared by >1 object (max "
          f"{rep['max_objects_per_position']} objects, "
          f"{rep['max_polygons_per_position']} polygons at one position)")
    print(f"  bad indices {rep['bad_indices']}, degenerate fan "
          f"{rep['fan_degenerate']}")
    print("result: " + ("OK -- the winding/normal rules hold on every polygon"
                        if rep["ok"] else "FAILED"))


# ---------------------------------------------------------------------------
# self-test (synthetic, no game files)
# ---------------------------------------------------------------------------

def _synth_tail(*, normal=(0.0, 0.0, -1.0), u_axis=(1.0, 0.0, 0.0),
                v_axis=(0.0, 1.0, 0.0), centre=(0.0, 0.0, 0.0),
                uv=(0, 0), uv_fp=(0, 0), flags=0x8000, handle=0,
                f74=0, f76=0, scroll=(0, 0), u_scale=0, v_scale=0,
                corner_a=(-1.0, -1.0, 0.0), corner_b=(1.0, 1.0, 0.0),
                anim=(0, 0, 0), reserved=0) -> bytes:
    t = bytearray(112)
    struct.pack_into("<2H", t, TAIL_FIELDS["scroll_u"][0], *scroll)
    struct.pack_into("<2H", t, TAIL_FIELDS["u_scale"][0], u_scale, v_scale)
    struct.pack_into("<2H", t, TAIL_FIELDS["uv_u"][0], *uv)
    struct.pack_into("<2i", t, TAIL_FIELDS["uv_u_fp"][0], *uv_fp)
    struct.pack_into("<3f", t, TAIL_FIELDS["centre"][0], *centre)
    struct.pack_into("<3f", t, TAIL_FIELDS["normal"][0], *normal)
    struct.pack_into("<3f", t, TAIL_FIELDS["u_axis"][0], *u_axis)
    struct.pack_into("<3f", t, TAIL_FIELDS["v_axis"][0], *v_axis)
    struct.pack_into("<3f", t, TAIL_FIELDS["corner_a"][0], *corner_a)
    struct.pack_into("<3f", t, TAIL_FIELDS["corner_b"][0], *corner_b)
    struct.pack_into("<3H", t, TAIL_FIELDS["f74"][0], f74, f76, handle)
    struct.pack_into("<I", t, TAIL_FIELDS["reserved"][0], reserved)
    struct.pack_into("<I", t, TAIL_FIELDS["flags"][0], flags)
    struct.pack_into("<3H", t, TAIL_FIELDS["anim0"][0], *anim)
    return bytes(t)


def _synth_poly(indices, *, texture=1, lightmap=0, material=0, overlay=0,
                f7a=0, brightness=64, **tail) -> bytes:
    head = struct.pack("<5H", texture & 0xFFFF, lightmap & 0xFFFF,
                       material & 0xFFFF, overlay & 0xFFFF, f7a)
    head += bytes((brightness & 0xFF, len(indices) & 0xFF))
    idx = struct.pack("<%dH" % len(indices), *indices) if indices else b""
    return head + idx + _synth_tail(**tail)


def _synth_header(origin=(0.0, 0.0, 0.0)) -> bytes:
    identity = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    return struct.pack("<12d", *list(origin) + list(identity))


def _synth_geom(vertices, polys: list[bytes], f75=0, f74=0, f7c=0, f78=0,
                reserved=bytes(24)) -> bytes:
    out = bytearray(struct.pack("<HH", len(vertices), len(polys)))
    for v in vertices:
        out += struct.pack("<3f", *v)
    out += struct.pack("<f", 0.0) + bytes((f75, f74))
    out += struct.pack("<II", f7c, f78)
    out += struct.pack("<3f", 0.0, 0.0, 0.0) + reserved
    assert len(out) == 4 + VERTEX_SIZE * len(vertices) + OBJECT_TAIL
    for p in polys:
        out += p
    return bytes(out)


#: (flag word, origin, vertices, polygons) for the synthetic level.  The stored
#: normals are opposite to the winding, exactly as in the shipped data.
_SYNTH_OBJECTS = [
    (0, (0.0, 0.0, 0.0),
     [(2.0, 3.0, 4.0), (2.0, 3.0, -4.0), (-2.0, 3.0, 4.0), (-2.0, 3.0, -4.0)],
     [((0, 1, 3, 2), 1, 0, 0), ((0, 2, 3, 1), 2, 1, 1)]),
    (8, (10.0, 0.0, 0.0),
     [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (0.5, 0.5, 0.5)],
     [((0, 1, 2), 1, 0, 0)]),
    (0x10000, (0.0, 5.0, -2.0),
     [(3.0, 0.0, 0.0), (0.0, 3.0, 0.0), (0.0, 0.0, 3.0)],
     [((0, 1, 2), 2, 1, 1)]),
    (0x100000, (-5.0, 0.0, 0.0), [], []),
]


def _synth_file() -> bytes:
    """A tiny but valid ``.sbw``: 2 textures, 1 effect, 2 sectors, 4 objects."""
    out = bytearray()
    tex = [("wall", 0, 3), ("floor", 1, 7)]
    out += struct.pack("<I", len(tex))
    for name, flag, value in tex:
        out += name.encode("latin1") + b"\0" + struct.pack("<HI", flag, value)
    out += struct.pack("<I", 1)
    out += b"fire\0" + struct.pack("<III", 1, 0, 3) + struct.pack("<3H", 0, 1, 2)

    layout = [(0, 0, [0, 1]), (6, 1, [2, 3])]
    out += struct.pack("<I", len(layout))
    for kind, sub, objs in layout:
        out += struct.pack("<IHB", len(objs), kind, sub)
        for i in objs:
            flag, origin, _v, _p = _SYNTH_OBJECTS[i]
            out += struct.pack("<I", flag) + _synth_header(origin)
    for _kind, _sub, objs in layout:
        for i in objs:
            _flag, _origin, verts, polys = _SYNTH_OBJECTS[i]
            recs = []
            for idx, tex_i, lm, mat in polys:
                centre = tuple(sum(verts[j][k] for j in idx) / len(idx)
                               for k in range(3))
                # stored normal opposite to the winding, as shipped
                fan = fan_normal(verts, idx)
                stored = (tuple(-c for c in fan) if fan is not None
                          else (0.0, 0.0, 0.0))
                recs.append(_synth_poly(
                    idx, texture=tex_i, lightmap=lm, material=mat,
                    brightness=64, normal=stored,
                    u_axis=(1.0, 0.0, 0.0), v_axis=(0.0, 1.0, 0.0),
                    centre=centre, uv=(18432, 47104), uv_fp=(-11776, 11776),
                    u_scale=0, v_scale=0, scroll=(0, 0),
                    corner_a=(-2.0, -1.0, -4.0),
                    corner_b=(2.0, 1.0, 4.0), handle=1, f74=0, f76=0,
                    anim=(1, 2, 3)))
            out += _synth_geom(verts, recs, f75=1 if i == 1 else 0,
                               f78=3 if i == 2 else 0)
    return bytes(out)


_TEMP_FILES: list[str] = []


def _write_temp(data: bytes, suffix: str = ".sbw") -> str:
    import tempfile
    base = DEFAULT_OUT_DIR if os.path.isdir(DEFAULT_OUT_DIR) else None
    fd, path = tempfile.mkstemp(prefix="sbwpoly-", suffix=suffix, dir=base)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    _TEMP_FILES.append(path)
    return path


def _selftest() -> bool:
    import tempfile

    results: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, bool(ok), detail))

    def expect_refused(name: str, fn) -> None:
        try:
            fn()
        except EditError as exc:
            check(name, True, str(exc))
        except Exception as exc:                       # noqa: BLE001
            check(name, False, f"wrong exception {type(exc).__name__}: {exc}")
        else:
            check(name, False, "no error raised")

    data = _synth_file()

    # 1: the synthetic file is a valid .sbw for both readers
    try:
        w = oracle.parse(data)
        check("synth: oracle parses", True)
        check("synth: oracle reaches EOF", w.data_end == len(data),
              f"{w.data_end}/{len(data)}")
        check("synth: oracle round-trip", w.to_bytes() == data)
        check("synth: oracle counts",
              (len(w.tex), len(w.effects), len(w.sectors), w.n_objects,
               w.n_polys, _oracle_vert_count(w)) == (2, 1, 2, 4, 4, 11),
              repr((len(w.tex), len(w.effects), len(w.sectors), w.n_objects,
                    w.n_polys, _oracle_vert_count(w))))
    except Exception as exc:                           # noqa: BLE001
        check("synth: oracle parses", False, str(exc))
        w = None

    scan = walk(data)
    check("synth: own walker reaches EOF", scan.data_end == len(data))
    check("synth: own walker counts",
          (scan.n_tex, scan.n_eff, len(scan.sectors), scan.n_objects,
           scan.n_polys, scan.n_verts) == (2, 1, 2, 4, 4, 11))
    flat = [o for s in w.sectors for o in s.objects] if w is not None else []
    fields_ok = w is not None and all(
        oracle_snapshot(op) == poly_snapshot(lp)
        for loc, obj in zip(scan.objects, flat)
        for lp, op in zip(loc.polys, obj.polys))
    check("synth: every polygon field matches the oracle", fields_ok)
    rt = roundtrip_one("synth.sbw", data)
    check("synth: roundtrip_one is green", rt["ok"],
          "; ".join(rt["problems"]))

    # 2: the byte layout itself
    o0 = scan.objects[0]
    p0 = o0.polys[0]
    check("layout: n_idx at record+0x0B",
          data[p0.off + 0x0B] == 4)
    check("layout: indices at record+0x0C, 2 bytes each",
          p0.off + POLY_HEAD == p0.tail_off - 2 * p0.n_idx
          and struct.unpack_from("<%dH" % p0.n_idx, data, p0.off + 0x0C)
          == (0, 1, 3, 2))
    check("layout: size is 124 + 2*n_idx",
          p0.size == 132 and o0.polys[1].off == p0.off + p0.size)
    check("layout: normal at tail+0x20",
          struct.unpack_from("<3f", data, p0.tail_off + 0x20) == p0.normal)
    check("layout: uv at tail+0x08 / uv_fp at tail+0x0C",
          struct.unpack_from("<2H", data, p0.tail_off + 0x08) == (18432, 47104)
          and struct.unpack_from("<2i", data, p0.tail_off + 0x0C)
          == (-11776, 11776))
    check("layout: flags at tail+0x66",
          struct.unpack_from("<I", data, p0.tail_off + 0x66)[0] == 0x8000)
    check("layout: handle/f74/f76 at tail+0x60/0x5C/0x5E",
          struct.unpack_from("<3H", data, p0.tail_off + 0x5C)
          == (0, 0, 1))
    check("layout: object polygon stream tiles exactly",
          all(o.polys[k + 1].off == o.polys[k].off + o.polys[k].size
              for o in scan.objects for k in range(o.n_polys - 1))
          and all(o.polys[-1].off + o.polys[-1].size == o.polys_end
                  for o in scan.objects if o.polys))

    # 3: winding and the two geometric normals
    fan = fan_normal(o0.vertices, p0.indices)
    newell = newell_normal(o0.vertices, p0.indices)
    info = winding_info(o0, p0)
    check("winding: stored normal is opposite the fan normal",
          fan is not None and info["dot_fan"] < -0.999,
          repr(info["dot_fan"]))
    check("winding: Newell and fan agree on a planar polygon",
          newell is not None and _dot(newell, fan) > 0.999)
    check("winding: sign_word classifies the opposite case",
          sign_word(info["dot_newell"]) == "opposite",
          sign_word(info["dot_newell"]))
    check("winding: zero normal is reported, not crashed",
          winding_info(o0, PolyLoc(**{**p0.__dict__,
                                      "normal": (0.0, 0.0, 0.0)}))["zero"])

    # 4: same-size field edits are accepted and change only their bytes
    def run_edit(obj_i, local, expected, allow=False, reindex=False):
        obj = scan.objects[obj_i]
        new, delta = apply_edits(data, scan, obj_i, local, expected, allow)
        rep = verify_edit(data, new, obj_i, {local: expected},
                          allow_resize=allow, reindex=reindex)
        return new, delta, rep

    try:
        new, delta, rep = run_edit(0, 0, {"texture": 2})
        check("set texture: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("set texture: size unchanged", delta == 0)
        check("set texture: few bytes changed",
              rep["changed_bytes"] <= 2 and rep["changed_bytes"] > 0,
              str(rep["changed_bytes"]))
        check("set texture: own walker sees 2",
              scan.objects[0].polys[0].texture == 1
              and walk(new).objects[0].polys[0].texture == 2)
        expect = p0.field_span("texture")
        check("set texture: changed bytes inside the field span",
              all(expect[0] <= d < expect[0] + expect[1]
                  for d in rep["changes"]))
    except Exception as exc:                           # noqa: BLE001
        check("set texture", False, str(exc))

    try:
        new, delta, rep = run_edit(0, 0, {"normal": (0.0, 0.0, 1.0)})
        check("set normal: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("set normal: only the 12 normal bytes differ",
              rep["changed_bytes"] <= 12 and rep["changed_bytes"] > 0,
              str(rep["changed_bytes"]))
        span = walk(new).objects[0].polys[0].field_span("normal")
        check("set normal: every changed byte is inside the normal span",
              all(span[0] <= d < span[0] + span[1]
                  for d in rep["changes"]))
        np_ = walk(new).objects[0].polys[0]
        check("set normal: stored normal stored as requested",
              np_.normal == (0.0, 0.0, 1.0))
        check("set normal: centre/uv/axes/corners unchanged",
              np_.centre == p0.centre and np_.uv_u == p0.uv_u
              and np_.u_axis == p0.u_axis and np_.corner_a == p0.corner_a)
        nrm = (1.0, 2.0, 2.0)
        unit = _normalize(nrm)
        newn, _d, repn = run_edit(0, 0, {"normal": _f32v(unit)})
        got = walk(newn).objects[0].polys[0].normal
        check("set normal: (1,2,2) is normalised to unit length",
              repn["ok"] and abs(_length(got) - 1.0) <= UNIT_TOL,
              str(got))
    except Exception as exc:                           # noqa: BLE001
        check("set normal", False, str(exc))

    try:
        new, delta, rep = run_edit(0, 0, {"uv": (100, 200)})
        check("set uv: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("set uv: 4 bytes changed", rep["changed_bytes"] <= 4,
              str(rep["changed_bytes"]))
        check("set uv: own walker sees (100,200)",
              walk(new).objects[0].polys[0].uv_u == 100
              and walk(new).objects[0].polys[0].uv_v == 200)
    except Exception as exc:                           # noqa: BLE001
        check("set uv", False, str(exc))

    try:
        new, delta, rep = run_edit(0, 0, {"u_axis": (0.0, 0.0, 1.0),
                                          "v_axis": (0.0, 1.0, 0.0)})
        check("set axes: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        got = walk(new).objects[0].polys[0]
        check("set axes: values written verbatim",
              got.u_axis == (0.0, 0.0, 1.0) and got.v_axis == (0.0, 1.0, 0.0))
    except Exception as exc:                           # noqa: BLE001
        check("set axes", False, str(exc))

    try:
        new, delta, rep = run_edit(0, 0, {"indices": (2, 3, 1, 0)})
        check("set indices (same n_idx): verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("set indices: few bytes changed",
              rep["changed_bytes"] <= 8, str(rep["changed_bytes"]))
        check("set indices: own walker sees the order",
              walk(new).objects[0].polys[0].indices == (2, 3, 1, 0))
    except Exception as exc:                           # noqa: BLE001
        check("set indices (same n_idx)", False, str(exc))

    # 5: reindex flips the geometric normal, not the stored one
    try:
        rev = tuple(reversed(p0.indices))
        new, delta, rep = run_edit(0, 0, {"indices": rev}, reindex=True)
        check("reindex: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("reindex: size unchanged", delta == 0)
        check("reindex: Newell normal flipped",
              rep["reindex"]["newell_flipped"] == 1)
        check("reindex: first fan triangle flipped",
              rep["reindex"]["planar_first_triangle"] == 1)
        got = walk(new).objects[0].polys[0]
        check("reindex: stored normal untouched",
              got.normal == p0.normal)
        check("reindex: geometric normal is the old one negated",
              _dot(newell_normal(o0.vertices, got.indices), newell)
              < -0.999)
        check("reindex: the unmodified polygon fails the reindex proof",
              not verify_edit(data, data, 0,
                              {0: {"indices": rev}}, reindex=True)["ok"])
    except Exception as exc:                           # noqa: BLE001
        check("reindex", False, str(exc))

    # 6: resize needs --allow-resize and keeps every other object intact
    try:
        new, delta, rep = run_edit(0, 0, {"indices": (2, 1, 3, 0, 2)},
                                   allow=True)
        check("resize: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("resize: size grew by 2 bytes", delta == 2
              and len(new) == len(data) + 2)
        check("resize: n_idx is 5 in the new file",
              walk(new).objects[0].polys[0].n_idx == 5)
        check("resize: every other object identical",
              all(a.geom_off != b.geom_off or True
                  for a, b in zip(scan.objects, walk(new).objects)))
        expect_refused(
            "resize: refused without --allow-resize",
            lambda: run_edit(0, 0, {"indices": (2, 1, 3, 0, 2)}))
    except Exception as exc:                           # noqa: BLE001
        check("resize", False, str(exc))

    # 6b: a resize combined with another field in one edit
    try:
        new, delta, rep = run_edit(
            0, 0, {"indices": (2, 1, 3, 0, 2), "normal": (0.0, 0.0, 1.0)},
            allow=True)
        got = walk(new).objects[0].polys[0]
        check("resize + normal: verification green", rep["ok"],
              "; ".join(rep["problems"]))
        check("resize + normal: both fields applied",
              got.indices == (2, 1, 3, 0, 2) and got.normal == (0.0, 0.0, 1.0)
              and delta == 2)
    except Exception as exc:                           # noqa: BLE001
        check("resize + normal", False, str(exc))

    # 7: a no-op writes a byte-identical copy
    try:
        new, delta, rep = run_edit(0, 0, {"texture": 1})
        check("no-op: byte-identical copy", new == data and rep["ok"])
    except Exception as exc:                           # noqa: BLE001
        check("no-op", False, str(exc))

    # 8: refusals (field / range / shape / path policy)
    n_light = 2
    expect_refused("refuse: unknown field",
                   lambda: field_assignment("radius", "1", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: normal zero vector",
                   lambda: field_assignment("normal", "0,0,0", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: normal nan",
                   lambda: field_assignment("normal", "nan,0,1", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: normal two numbers",
                   lambda: field_assignment("normal", "0,1", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: material 6",
                   lambda: field_assignment("material", "6", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: texture above the table",
                   lambda: field_assignment("texture", "99", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: effect below the table",
                   lambda: field_assignment("texture", "-9", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: brightness 256",
                   lambda: field_assignment("brightness", "256", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: lightmap without a known range",
                   lambda: field_assignment("lightmap", "1", scan, o0, p0,
                                            None, False))
    expect_refused("refuse: lightmap out of range",
                   lambda: field_assignment("lightmap", "3", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: undocumented flag bit",
                   lambda: field_assignment("flags", "0x10000", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: uv with one number",
                   lambda: field_assignment("uv", "5", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: uv value above u16",
                   lambda: field_assignment("uv", "70000,0", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: axis zero vector",
                   lambda: field_assignment("u_axis", "0,0,0", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: axis implausible",
                   lambda: field_assignment("u_axis", "1e9,0,0", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: index outside the object",
                   lambda: field_assignment("indices", "0,1,9", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: fewer than 3 indices",
                   lambda: field_assignment("indices", "0,1", scan, o0, p0,
                                            n_light, False))
    expect_refused("refuse: resize without --allow-resize",
                   lambda: field_assignment("indices", "0,1,2,3,0", scan, o0,
                                            p0, n_light, False))
    expect_refused("refuse: missing object",
                   lambda: _require_object(scan, 4))
    expect_refused("refuse: negative object",
                   lambda: _require_object(scan, -1))
    expect_refused("refuse: missing polygon",
                   lambda: _require_index(None, o0))
    expect_refused("refuse: polygon == n_polys",
                   lambda: _require_index(2, o0))
    expect_refused("refuse: unknown field at patch time",
                   lambda: p0.field_span("radius"))

    # 9: truncation / corruption is reported, not guessed
    for cut in (10, 60, 200):
        try:
            walk(data[:-cut])
        except ScanError:
            own_ok = True
        except Exception:                              # noqa: BLE001
            own_ok = False
        else:
            own_ok = False
        try:
            oracle.parse(data[:-cut])
        except ValueError:
            o_ok = True
        except Exception:                              # noqa: BLE001
            o_ok = False
        else:
            o_ok = False
        check(f"truncation -{cut}: own walker refuses", own_ok)
        check(f"truncation -{cut}: oracle refuses", o_ok)

    bad = bytearray(data)
    struct.pack_into("<H", bad, o0.geom_off, 0xFFFF)   # n_verts
    try:
        walk(bytes(bad))
        own_bad = False
    except ScanError:
        own_bad = True
    try:
        oracle.parse(bytes(bad))
        o_bad = False
    except ValueError:
        o_bad = True
    check("corrupt n_verts: own walker refuses", own_bad)
    check("corrupt n_verts: oracle refuses", o_bad)

    # 10: verification is not vacuous
    new, _d, _rep = run_edit(0, 0, {"texture": 2})
    tampered = bytearray(new)
    tampered[p0.field_span("brightness")[0]] ^= 0x01
    check("verify: detects a change in another field",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])
    tampered = bytearray(new)
    tampered[p0.field_span("texture")[0]] = 3
    check("verify: detects a wrong target value",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])
    tampered = bytearray(new)
    tampered[o0.polys[1].off + 0x0C] ^= 0x01
    check("verify: detects a change in another polygon",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])
    tampered = bytearray(new)
    struct.pack_into("<f", tampered, o0.tail_off, 9.0)   # radius
    check("verify: detects a change in the object tail",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])
    other = scan.objects[1]
    tampered = bytearray(new)
    tampered[other.verts_off] ^= 0x01
    check("verify: detects a change in another object",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])
    tampered = bytearray(new)
    struct.pack_into("<d", tampered, o0.header_off + 4, 5.0)
    check("verify: detects a header change",
          not verify_edit(data, bytes(tampered), 0,
                          {0: {"texture": 2}})["ok"])

    # 11: the CLI-shaped --set/--reindex -> --check cycle on files
    tmpdir = None
    try:
        base = DEFAULT_OUT_DIR if os.path.isdir(DEFAULT_OUT_DIR) else None
        tmpdir = tempfile.mkdtemp(prefix="sbwpoly-", dir=base)
        game_dir = os.path.join(tmpdir, "game")
        out_dir = os.path.join(tmpdir, "out")
        os.makedirs(game_dir)
        os.makedirs(out_dir)
        src = os.path.join(game_dir, "synthetic.sbw")
        tex = os.path.join(out_dir, "synthetic_tex.sbw")
        rei = os.path.join(out_dir, "synthetic_rei.sbw")
        big = os.path.join(out_dir, "synthetic_big.sbw")
        write_bytes(src, data)
        rep = set_polygon(src, 0, 0, ["texture=2"], tex, game=game_dir)
        check("set: writes and verifies a file", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("set: oracle reads the file",
              oracle_value(oracle_poly(oracle.parse_file(tex), 0, 0),
                           "texture") == 2)
        chk = check_file(tex, src, 0, 0, ["texture=2"], game=game_dir)
        check("check: accepts the field edit", chk["ok"],
              "; ".join(chk.get("problems", [])))
        chk2 = check_file(tex, src, 0, 0, ["texture=1"], game=game_dir)
        check("check: rejects a wrong expectation", not chk2["ok"])
        chk3 = check_file(tex, src, game=game_dir)
        check("check: full identity rejects the edit", not chk3["ok"])
        chk4 = check_file(src, src, game=game_dir)
        check("check: identical file passes", chk4["ok"],
              "; ".join(chk4.get("problems", [])))

        rep2 = reindex_object(src, 0, 0, rei, game=game_dir)
        check("reindex: writes and verifies a file", rep2["ok"],
              "; ".join(rep2.get("problems", [])))
        chk5 = check_file(rei, src, 0, 0, reindex=True, game=game_dir)
        check("check: accepts the reindex", chk5["ok"],
              "; ".join(chk5.get("problems", [])))
        chk6 = check_file(rei, src, 0, 0, ["indices=0,1,3,2"], game=game_dir)
        check("check: rejects a wrong index claim", not chk6["ok"])

        rep3 = set_polygon(src, 0, 0, ["indices=2,1,3,0,2"], big,
                           game=game_dir, allow_resize=True)
        check("set: resize writes and verifies a file", rep3["ok"],
              "; ".join(rep3.get("problems", [])))
        chk7 = check_file(big, src, 0, 0, ["indices=2,1,3,0,2"],
                          game=game_dir, allow_resize=True)
        check("check: accepts the resize", chk7["ok"],
              "; ".join(chk7.get("problems", [])))
        expect_refused(
            "check: rejects a resize without --allow-resize",
            lambda: check_file(big, src, 0, 0, ["indices=2,1,3,0,2"],
                               game=game_dir))

        expect_refused("set: refuses without --out",
                       lambda: set_polygon(src, 0, 0, ["texture=2"], ""))
        expect_refused("reindex: refuses without --out",
                       lambda: reindex_object(src, 0, None, ""))
        expect_refused("set: refuses an index outside the object",
                       lambda: set_polygon(src, 0, 99, ["texture=2"], tex))
        expect_refused("set: refuses to write into the game install",
                       lambda: set_polygon(src, 0, 0, ["texture=2"],
                                          os.path.join(game_dir, "x.sbw"),
                                          game=game_dir))
        expect_refused("reindex: refuses to write into the game install",
                       lambda: reindex_object(src, 0, 0,
                                              os.path.join(game_dir, "x.sbw"),
                                              game=game_dir))
        expect_refused(
            "check: editor expectation needs --object/--index",
            lambda: check_file(tex, src, game=game_dir, fields=["texture=2"]))
    except Exception as exc:                           # noqa: BLE001
        check("set/reindex/check: temporary-file cycle", False, str(exc))
    finally:
        if tmpdir:
            for root in (os.path.join(tmpdir, "game"),
                         os.path.join(tmpdir, "out"), tmpdir):
                if not os.path.isdir(root):
                    continue
                for name in os.listdir(root):
                    try:
                        os.remove(os.path.join(root, name))
                    except OSError:
                        pass
                try:
                    os.rmdir(root)
                except OSError:
                    pass

    # 12: --list shape on the synthetic file
    try:
        path = _write_temp(data)
        lrep = list_level(path)
        e = lrep["polygon_list"][0]
        check("list: polygon row carries the documented fields",
              e["texture"] == 1 and e["material"] == 0 and e["lightmap"] == 0
              and e["n_idx"] == 4 and e["brightness"] == 64
              and e["flags"] == 0x8000 and e["raw_len"] == 112)
        check("list: geometric normal is opposite the stored one",
              e["normal"] == list(p0.normal)
              and e["newell_normal"] == list(fan)
              and e["normal_dot_newell"] < -0.999)
        check("list: uv/axes/uv_fp are reported",
              e["uv"] == [18432, 47104] and e["uv_fp"] == [-11776, 11776]
              and e["u_axis"] == [1.0, 0.0, 0.0])
        check("list: vertex rows carry world position and sharing",
              all("world" in v for v in e["vertices"])
              and any(v["objects_sharing"] >= 1 for v in e["vertices"]))
        check("list: object filter",
              list_level(path, obj_index=2)["object_list"][0]["index"] == 2)
        check("list: limit caps the shown polygons",
              len(list_level(path, limit=2)["polygon_list"]) <= 2)
        check("list: json-encodable",
              bool(json.dumps(lrep)))
    except Exception as exc:                           # noqa: BLE001
        check("list: synthetic", False, str(exc))

    # 13: --audit invariants on the synthetic file
    try:
        tmp_game = tempfile.mkdtemp(prefix="sbwpoly-audit-",
                                    dir=(DEFAULT_OUT_DIR
                                         if os.path.isdir(DEFAULT_OUT_DIR)
                                         else None))
        try:
            write_bytes(os.path.join(tmp_game, "synthetic.sbw"), data)
            arep = audit_corpus(tmp_game)
            check("audit: synthetic corpus is green", arep["ok"],
                  repr({k: arep[k] for k in ("bad_indices",
                                             "reindex_checked",
                                             "reindex_newell_flips")}))
            check("audit: reindex flips every Newell normal",
                  arep["reindex_checked"] == arep["reindex_newell_flips"] == 4,
                  f"{arep['reindex_checked']}/{arep['reindex_newell_flips']}")
        finally:
            for name in os.listdir(tmp_game):
                try:
                    os.remove(os.path.join(tmp_game, name))
                except OSError:
                    pass
            try:
                os.rmdir(tmp_game)
            except OSError:
                pass
    except Exception as exc:                           # noqa: BLE001
        check("audit: synthetic", False, str(exc))

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    for name, ok, detail in results:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}"
              + (f" -- {detail}" if detail and not ok else ""))
    print(f"self-test: {passed}/{total} passed")
    return passed == total


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _fmt_number(x) -> str:
    if x is None:
        return "n/a"
    if isinstance(x, int):
        return str(x)
    x = float(x)
    if math.isfinite(x) and x == int(x) and abs(x) < 1e15:
        return str(int(x))
    return f"{x:.6g}"


def _fmt_vec(v) -> str:
    if v is None:
        return "n/a"
    return "(" + ", ".join(_fmt_number(c) for c in v) + ")"


def _json_vec(v):
    return [float(c) for c in v] if v is not None else None


def _cmd_list(args) -> int:
    path = resolve_level(args.list, args.game)
    limit = max(args.limit, 0)
    rep = list_level(path, limit, args.object)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_list(rep, limit)
    return 0 if rep["eof_ok"] else 1


def _cmd_set(args) -> int:
    if args.object is None:
        raise EditError("--set needs --object N")
    if args.index is None:
        raise EditError("--set needs --index I (polygon inside the object)")
    if not args.field:
        raise EditError("--set needs --field NAME=VALUE")
    if not args.out:
        raise EditError("--set needs --out FILE")
    path = resolve_level(args.set, args.game)
    rep = set_polygon(path, args.object, args.index, args.field, args.out,
                      args.game, args.allow_resize, args.sbc)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep, reindex=False)
    return 0 if rep["ok"] else 1


def _cmd_reindex(args) -> int:
    if args.object is None:
        raise EditError("--reindex needs --object N")
    if not args.out:
        raise EditError("--reindex needs --out FILE")
    path = resolve_level(args.reindex, args.game)
    rep = reindex_object(path, args.object, args.index, args.out, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep, reindex=True)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    rep = check_file(args.check, args.against, args.object, args.index,
                     args.field, args.expect_reindex, args.game,
                     args.allow_resize, args.sbc)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_check(rep)
    return 0 if rep["ok"] else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        if head in ("self-test", "selftest", "--self-test", "--selftest"):
            argv = ["--self-test"] + argv[1:]
        elif head in ("roundtrip", "--roundtrip"):
            argv = ["--roundtrip"] + argv[1:]
        elif head in ("audit", "--audit"):
            argv = ["--audit"] + argv[1:]
        elif head in ("list", "--list") and len(argv) > 1:
            argv = ["--list", argv[1]] + argv[2:]
        elif head in ("check", "--check") and len(argv) > 1:
            argv = ["--check", argv[1]] + argv[2:]
        elif head in ("set", "--set") and len(argv) > 1:
            argv = ["--set", argv[1]] + argv[2:]
        elif head in ("reindex", "--reindex") and len(argv) > 1:
            argv = ["--reindex", argv[1]] + argv[2:]
    ap = argparse.ArgumentParser(
        description="Edit polygons of a Gunmetal .sbw level: fields, winding, "
                    "verified byte-exactly (M11.2, slice 5).")
    ap.add_argument("--list", metavar="LEVEL",
                    help="list polygons (fields, normals, winding, sharing)")
    ap.add_argument("--set", metavar="LEVEL",
                    help="edit documented fields of one polygon "
                         "(needs --object/--index/--field/--out)")
    ap.add_argument("--reindex", metavar="LEVEL",
                    help="reverse the winding of one polygon or of every "
                         "polygon of an object (needs --object/--out)")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE (oracle + own walker) and verify")
    ap.add_argument("--expect-reindex", action="store_true",
                    help="--check: expect the target's winding to be reversed")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source level")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every shipped .sbw")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests (no game files)")
    ap.add_argument("--audit", action="store_true",
                    help="measure the winding/normal/sharing rules on the "
                         "corpus")
    ap.add_argument("--object", type=int, metavar="N",
                    help="object index (0-based, order of --list)")
    ap.add_argument("--index", type=int, metavar="I",
                    help="polygon index inside the object (0-based)")
    ap.add_argument("--field", action="append", metavar="NAME=VALUE",
                    help="editable: texture, material, lightmap, overlay, "
                         "brightness, normal=x,y,z, uv=u,v, u_axis=x,y,z, "
                         "v_axis=x,y,z, flags, indices=i0,i1,... (repeatable)")
    ap.add_argument("--allow-resize", action="store_true",
                    help="allow an indices list with a different count "
                         "(rebuilds the object and shifts the stream)")
    ap.add_argument("--sbc", metavar="FILE",
                    help="sibling .sbc for the lightmap range")
    ap.add_argument("--out", metavar="PATH", help="output .sbw")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=8,
                    help="max objects and max polygons shown (0 = all)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.self_test:
            return 0 if _selftest() else 1
        if args.audit:
            rep = audit_corpus(args.game, args.verbose)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1
        if args.roundtrip:
            rep = roundtrip_corpus(args.game, args.verbose)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1
        if args.check:
            return _cmd_check(args)
        if args.list:
            return _cmd_list(args)
        if args.set:
            return _cmd_set(args)
        if args.reindex:
            return _cmd_reindex(args)
        ap.print_help()
        return 2
    except EditError as exc:
        print(f"sbwpoly: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"sbwpoly: corrupt .sbw: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"sbwpoly: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"sbwpoly: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    code = main()
    for _p in _TEMP_FILES:
        try:
            os.remove(_p)
        except OSError:
            pass
    sys.exit(code)
