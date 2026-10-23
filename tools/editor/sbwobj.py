#!/usr/bin/env python3
"""Editor for Gunmetal ``.sbw`` world *objects* (``M11.2``, slice 3).

``tools/editor/sbwedit.py`` (slice 1) edits the six documented **polygon**
fields.  This tool edits the **object**: its placement (``origin``), its
bounding sphere/box (``radius``/``bounds``) and the four auxiliary words in the
object tail (``f74``/``f75``/``f78``/``f7c``).  It never touches vertices or
polygons, so the record sizes do not change and no byte shifts.

An object lives in two places of the stream (``gm/sbw.py:1-128``,
``docs/FORMATS.md`` §10, ``docs/WORLD_FIELDS.md`` §2.3/§3):

===========  =============================================================
stage 1      ``u32 id`` + 96-byte header ``= 12 x f64``
             (``origin[3]`` then ``basis[3][3]`` row-major; the loader copies
             the 96 bytes verbatim at ``0x42ae86``..``0x42aee9``)
stage 2      ``u16 n_verts; u16 n_polys; f32 verts[n_verts][3]`` then a
             50-byte tail: ``f32 radius`` (0x60), ``u8 f75`` (0x75),
             ``u8 f74`` (0x74), ``u32 f7c`` (0x7c), ``u32 f78`` (0x78),
             ``f32 bounds[3]`` (0x84), 24 zero bytes (0x90)
===========  =============================================================

The engine places a vertex as ``world = origin + basis * local``
(``Object.world_vertices``; transform ``0x437af8`` reads the header with
``FLD double`` and the local vertex with ``FLD float``).  The basis is the
identity in **28 192/28 192** shipped objects, so a shipped object is a pure
translation and ``origin`` can be edited independently of the geometry: the
local vertices stay exactly as they are and the world frame moves by the
delta.  A non-identity basis is refused (:data:`BASIS_NOTE`) -- there is no
shipped example and the polygon frame cache (``normal``/``u_axis``/``v_axis``/
``centre``/``corners``) and the sector AABB are authored in the object-local
frame, so a rotation would need a transform-aware rebuild that this slice
cannot prove against the engine.

Editable fields (disk offsets are relative to the object):

==================  ================  ============  ==========================
field               disk              engine off    meaning
==================  ================  ============  ==========================
``origin``          ``header+0x04``   ``+0x00``     3 x f64, world translation
``basis``           ``header+0x1C``   ``+0x18``     9 x f64 rows; only
                                                   ``basis=identity``
``radius``          ``tail+0x00``     ``+0x60``     f32 bounding-sphere radius
``f75``             ``tail+0x04``     ``+0x75``     u8, 0/1 in shipped data
``f74``             ``tail+0x05``     ``+0x74``     u8, always 0 (brightness
                                                   bias, docs/WORLD_FIELDS §2.3)
``f7c``             ``tail+0x06``     ``+0x7c``     u32, always 0
``f78``             ``tail+0x0A``     ``+0x78``     u32, always 0
``bounds``          ``tail+0x0E``     ``+0x84``     3 x f32 AABB half-extents
==================  ================  ============  ==========================

Usage::

    python3 tools/editor/sbwobj.py --list level0
    python3 tools/editor/sbwobj.py --list level0 --limit 4 --json
    python3 tools/editor/sbwobj.py --get level0 --object 0 --field origin
    python3 tools/editor/sbwobj.py --set level0 --object 0 \\
        --field origin=1496,7568,48 --out /tmp/opencode/sbwobj/level0.sbw
    python3 tools/editor/sbwobj.py --check /tmp/opencode/sbwobj/level0.sbw \\
        --against level0 --object 0 --field origin=1496,7568,48
    python3 tools/editor/sbwobj.py --roundtrip
    python3 tools/editor/sbwobj.py --self-test
    python3 tools/editor/sbwobj.py --audit
    python3 tools/editor/sbwobj.py --fields

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
from dataclasses import dataclass, field as dc_field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbw as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sbwobj"

#: Same trust-boundary caps as the oracle (``gm/sbw.py:359-363``).
MAX_TABLE = 65_536
MAX_OBJECTS = 65_536

OBJECT_HEADER = 96                                # 12 x f64
OBJECT_TAIL = 50                                  # radius .. reserved[24]

IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))

#: Why an arbitrary basis is refused (repeated in ``--fields``).
BASIS_NOTE = (
    "only basis=identity is accepted: the basis is the identity in "
    "28192/28192 shipped objects (docs/FORMATS.md §10, docs/WORLD_FIELDS.md "
    "§3), while an arbitrary rotation/scale would invalidate the per-polygon "
    "frame cache (normal/u_axis/v_axis/centre/corners are authored in the "
    "object-local frame and 0x43529f only recomputes bounds/corners), the "
    "object radius/bounds and the sector AABB (0x434fed); no shipped example "
    "or engine test proves that chain, so the edit is refused instead of "
    "written blind")


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The file does not walk as a ``.sbw`` (own reader)."""


# ---------------------------------------------------------------------------
# documented object fields
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ObjField:
    """One documented object field: disk/engine offset, type and provenance."""

    name: str
    disk: str               # human readable disk offset
    engine: str             # in-memory offset in the runtime object record
    fmt: str                # struct format character (little-endian)
    count: int              # elements packed with that format
    doc: str                # documentation
    va: str                 # engine evidence
    meaning: str

    @property
    def width(self) -> int:
        return struct.calcsize(self.fmt) * self.count


FIELDS = {
    "origin": ObjField(
        "origin", "header+0x04", "+0x00", "<d", 3,
        "docs/FORMATS.md §10 (Трансформ объекта); gm/sbw.py:389-391",
        "loader copies 24 raw bytes at 0x42ae86; transform 0x437af8 (FLD double)",
        "world translation, 3 x f64: world = origin + basis * local",
    ),
    "basis": ObjField(
        "basis", "header+0x1C", "+0x18", "<d", 9,
        "docs/FORMATS.md §10; gm/sbw.py:389-391; docs/WORLD_FIELDS.md §2.3",
        "loader 0x42aea7/0x42aec8/0x42aee9 (3 x 24 B); transform 0x437af8",
        "3x3 row-major f64 basis; identity in every shipped object",
    ),
    "radius": ObjField(
        "radius", "tail+0x00", "+0x60", "<f", 1,
        "docs/FORMATS.md §10; gm/sbw.py:80,449-451; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c; cull 0x437d8c/0x437d05; sector sphere 0x4360f1",
        "f32 bounding-sphere radius (0x60), |bounds| in shipped data",
    ),
    "bounds": ObjField(
        "bounds", "tail+0x0E", "+0x84", "<f", 3,
        "docs/FORMATS.md §10; gm/sbw.py:83,451; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c; recompute 0x43529f; sector AABB 0x434fed",
        "3 x f32 AABB half-extents (0x84), non-negative integers in data",
    ),
    "f75": ObjField(
        "f75", "tail+0x04", "+0x75", "<B", 1,
        "docs/FORMATS.md §10; gm/sbw.py:81,450; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c; value 1 in 7 shipped objects (no reader found)",
        "u8 auxiliary; shipped {0, 1}, meaning not established",
    ),
    "f74": ObjField(
        "f74", "tail+0x05", "+0x74", "<B", 1,
        "docs/FORMATS.md §10; gm/sbw.py:81,450; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c; zeroed by 0x42ae55; read by 0x43c51f/0x43d8e3 as i8",
        "u8 auxiliary (signed brightness bias at runtime); 0 in all shipped",
    ),
    "f7c": ObjField(
        "f7c", "tail+0x06", "+0x7c", "<I", 1,
        "docs/FORMATS.md §10; gm/sbw.py:82,450; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c (no reader found)",
        "u32 auxiliary; 0 in all shipped objects",
    ),
    "f78": ObjField(
        "f78", "tail+0x0A", "+0x78", "<I", 1,
        "docs/FORMATS.md §10; gm/sbw.py:82,450; docs/WORLD_FIELDS.md §2.3",
        "loader 0x425f4c (no reader found)",
        "u32 auxiliary; 0 in all shipped objects",
    ),
}

#: Documented *unavailable* object edits: the reason is printed by --fields.
UNAVAILABLE = {
    "id": (
        "the u32 in front of the 96-byte header (Object.id in gm/sbw.py) is a "
        "flag word (0x10 ray cull, 0x10000 trace, 0x20000 force render, "
        "0x40000/0x80000 arrays not owned, 0x100000 keep UV; "
        "docs/WORLD_MATERIALS.md §4.1, docs/WORLD_FIELDS.md §2.3). Nine "
        "values occur in the shipped data; this slice lists it but does not "
        "edit it -- no combination beyond those nine is proven"
    ),
    "vertices": (
        "local vertex positions: moving them requires recomputing every "
        "polygon's centre/normal/u_axis/v_axis/corners as well as the object "
        "radius/bounds and the sector AABB (0x43529f recomputes only "
        "bounds/corners).  Use --set origin to move the whole object"
    ),
    "n_verts/n_polys": (
        "changing a count resizes the record and shifts the rest of the "
        "sequential stream; the writer for that is a separate task"
    ),
    "reserved[24]": (
        "0x90: 24 zero bytes, read by the engine as 3 x f64 'extent' "
        "(0x434fed/0x437d05/0x440867).  Zero in all 28192 objects and never "
        "written by the world code (docs/WORLD_FIELDS.md §2.3), so there is "
        "no value to document -- kept verbatim"
    ),
    "sector.kind/sub": (
        "sector header (kind = sector type * 6, sub = extra flag), not an "
        "object field; render reads neither (docs/FORMATS.md §10)"
    ),
}


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
class ObjectLoc:
    """Where an object lives on disk plus the documented fields read from it."""

    index: int                       # global order: sector -> object
    sector: int
    obj: int                         # index inside its sector
    id: int                          # u32 in front of the header (flag word)
    header_off: int                  # disk offset of the u32 id word
    geom_off: int                    # stage 2: n_verts/n_polys
    n_verts: int
    n_polys: int
    verts_off: int
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
    poly_first: int
    poly_last: int

    @property
    def header_size(self) -> int:
        return 4 + OBJECT_HEADER

    @property
    def tail_end(self) -> int:
        return self.tail_off + OBJECT_TAIL

    def span(self, name: str) -> tuple[int, int]:
        """Disk ``(offset, width)`` of a documented field inside this object."""
        field = FIELDS.get(name)
        if field is None:
            raise EditError(f"unknown object field: {name} "
                            f"(known: {', '.join(FIELDS)})")
        if name == "origin":
            return self.header_off + 4, field.width
        if name == "basis":
            return self.header_off + 4 + 3 * 8, field.width
        if name == "radius":
            return self.tail_off, field.width
        if name == "f75":
            return self.tail_off + 4, field.width
        if name == "f74":
            return self.tail_off + 5, field.width
        if name == "f7c":
            return self.tail_off + 6, field.width
        if name == "f78":
            return self.tail_off + 10, field.width
        if name == "bounds":
            return self.tail_off + 14, field.width
        raise EditError(f"unknown object field: {name}")

    def world_vertices(self) -> tuple[tuple[float, float, float], ...]:
        """``origin + basis * local`` -- the same formula as ``gm/sbw.py``."""
        ox, oy, oz = self.origin
        b = self.basis
        return tuple(
            (ox + b[0] * x + b[3] * y + b[6] * z,
             oy + b[1] * x + b[4] * y + b[7] * z,
             oz + b[2] * x + b[5] * y + b[8] * z)
            for x, y, z in self.vertices)

    def aabb(self) -> tuple[tuple[float, ...], tuple[float, ...]] | None:
        world = self.world_vertices()
        if not world:
            return None
        lo = [min(p[i] for p in world) for i in range(3)]
        hi = [max(p[i] for p in world) for i in range(3)]
        return tuple(lo), tuple(hi)


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
                vertices=(), poly_first=0, poly_last=-1)
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
            raw_verts = c.take(12 * obj.n_verts, f"obj[{obj.index}] vertices")
            obj.vertices = tuple(_vec3(raw_verts, 12 * k)
                                 for k in range(obj.n_verts))
            obj.tail_off = c.o
            tail = c.take(OBJECT_TAIL, f"obj[{obj.index}] tail")
            obj.radius = struct.unpack_from("<f", tail, 0)[0]
            obj.f75 = tail[4]
            obj.f74 = tail[5]
            obj.f7c, obj.f78 = struct.unpack_from("<II", tail, 6)
            obj.bounds = struct.unpack_from("<3f", tail, 14)
            obj.poly_first = poly_index
            for _ in range(obj.n_polys):
                n_idx = _poly_n_idx(c, obj.index)
                c.take(2 * n_idx, f"obj[{obj.index}] poly indices")
                c.take(112, f"obj[{obj.index}] poly tail")
                poly_index += 1
            obj.poly_last = poly_index - 1
            obj.polys_end = c.o

    return Scan(len(data), n_tex, n_eff, sectors, objects, poly_index, c.o,
                table1_end, table2_end)


def _vec3(data: bytes, o: int) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", data, o)


def _poly_n_idx(c: _Cur, obj_index: int) -> int:
    """Read a polygon's five u16 head fields + ``brightness``/``n_idx``.

    The record starts with the five u16 fields (``texture``, ``lightmap``,
    ``material``, ``overlay``, ``f7a``); ``n_idx`` is byte 11 (``gm/sbw.py``
    ``_parse_poly``, ``docs/FORMATS.md`` §10.1).
    """
    head = c.take(12, f"obj[{obj_index}] polygon head")
    return head[11]


# ---------------------------------------------------------------------------
# field values: identity, formatting, helpers
# ---------------------------------------------------------------------------

def is_identity(basis) -> bool:
    return all(abs(basis[r * 3 + c] - IDENTITY[r][c]) == 0.0
               for r in range(3) for c in range(3))


def basis_rows(basis) -> list[list[float]]:
    return [[float(basis[r * 3 + c]) for c in range(3)] for r in range(3)]


def _f32(v: float) -> float:
    """The value as it is stored in an ``f32`` field (round-trip exact)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _fmt_num(v: float) -> str:
    """Compact but unambiguous enough for a listing (use --get for exact)."""
    if math.isfinite(v) and v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return f"{v:.6g}"


def _fmt_vec(v) -> str:
    return "(" + ", ".join(_fmt_num(float(x)) for x in v) + ")"


def field_value(obj: ObjectLoc, name: str):
    """Documented field value, JSON-friendly."""
    if name == "origin":
        return [float(v) for v in obj.origin]
    if name == "basis":
        return {"rows": basis_rows(obj.basis), "identity": is_identity(obj.basis)}
    if name == "radius":
        return float(obj.radius)
    if name == "bounds":
        return [float(v) for v in obj.bounds]
    if name in ("f74", "f75", "f78", "f7c"):
        return int(getattr(obj, name))
    raise EditError(f"unknown object field: {name}")


def _matches(obj: ObjectLoc, name: str, expected) -> bool:
    """Does the parsed object carry exactly the requested value?"""
    if name == "origin":
        return tuple(obj.origin) == tuple(expected)
    if name == "basis":
        return is_identity(obj.basis)
    if name == "radius":
        return obj.radius == _f32(expected)
    if name == "bounds":
        return tuple(obj.bounds) == tuple(_f32(v) for v in expected)
    return int(getattr(obj, name)) == int(expected)


def _oracle_object(w, index: int):
    for sect in w.sectors:
        for obj in sect.objects:
            if index == 0:
                return obj
            index -= 1
    raise EditError(f"object {index} does not exist in the oracle view")


def _oracle_matches(obj, name: str, expected) -> bool:
    if name == "origin":
        return tuple(obj.points[0]) == tuple(expected)
    if name == "basis":
        flat = tuple(c for row in obj.points[1:] for c in row)
        return is_identity(flat)
    if name == "radius":
        return obj.radius == _f32(expected)
    if name == "bounds":
        return tuple(obj.bounds) == tuple(_f32(v) for v in expected)
    return int(getattr(obj, name)) == int(expected)


# ---------------------------------------------------------------------------
# value parsing / validation / patching
# ---------------------------------------------------------------------------

def parse_int(text: str) -> int:
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
    parts = [p for p in text.split(",")]
    if len(parts) != n:
        raise EditError(f"{what} needs {n} comma-separated numbers "
                        f"(got {len(parts)}: {text!r})")
    return tuple(parse_float(p) for p in parts)


def parse_assignment(assignment: str) -> tuple[str, object]:
    """``NAME=VALUE`` -> ``(name, parsed value)``; every failure is text."""
    if "=" not in assignment:
        raise EditError("--field needs NAME=VALUE")
    name, _, text = assignment.partition("=")
    name = name.strip()
    if name not in FIELDS:
        raise EditError(f"unknown object field: {name} "
                        f"(known: {', '.join(FIELDS)})")
    return name, parse_value(name, text)


def parse_value(name: str, text: str):
    if name == "origin":
        return parse_vec(text, 3, "origin=x,y,z")
    if name == "bounds":
        return parse_vec(text, 3, "bounds=x,y,z")
    if name == "basis":
        word = text.strip().lower()
        if word != "identity":
            raise EditError(f"basis={text.strip()!r}: {BASIS_NOTE}")
        return "identity"
    if name == "radius":
        return parse_float(text)
    return parse_int(text)


def validate(name: str, value) -> None:
    """Check a parsed value against the documented/shipped range."""
    if name == "origin":
        if len(value) != 3:
            raise EditError("origin needs three f64 values")
        for v in value:
            if not math.isfinite(v):
                raise EditError(f"origin: non-finite component {v!r}")
            if abs(v) > 1e12:
                raise EditError(f"origin: |{v!r}| is implausible for the "
                                f"world frame (shipped max |origin| = 131144)")
        return
    if name == "basis":
        if value != "identity":
            raise EditError(BASIS_NOTE)
        return
    if name == "radius":
        if not isinstance(value, float) or not math.isfinite(value):
            raise EditError(f"radius={value!r}: needs a finite number")
        if value < 0.0:
            raise EditError(f"radius={value}: a bounding-sphere radius cannot "
                            f"be negative")
        if abs(value) > 3.0e38:
            raise EditError(f"radius={value}: does not fit an f32 field")
        return
    if name == "bounds":
        if len(value) != 3:
            raise EditError("bounds needs three f32 values")
        for v in value:
            if not math.isfinite(v):
                raise EditError(f"bounds: non-finite component {v!r}")
            if v < 0.0:
                raise EditError(f"bounds={_fmt_vec(value)}: AABB half-extents "
                                f"cannot be negative (0x43529f uses FABS)")
            if abs(v) > 3.0e38:
                raise EditError(f"bounds={v!r}: does not fit an f32 field")
        return
    if name in ("f74", "f75", "f78", "f7c"):
        if not 0 <= value <= 0xFF:
            raise EditError(f"{name}={value}: byte-sized auxiliary field, "
                            f"accepted range 0..255 (shipped: f74/f78/f7c = 0, "
                            f"f75 in {{0, 1}})")
        return
    raise EditError(f"unknown object field: {name}")


def patch_bytes(data: bytes, obj: ObjectLoc, name: str, value) -> bytes:
    """Return ``data`` with exactly the field's bytes replaced."""
    field = FIELDS.get(name)
    if field is None:
        raise EditError(f"unknown object field: {name}")
    off, width = obj.span(name)
    if off < 0 or off + width > len(data):
        raise EditError(f"{name}: field span {off}..{off + width} is outside "
                        f"the file ({len(data)} bytes)")
    out = bytearray(data)
    if name == "basis":
        values = [c for row in IDENTITY for c in row]
    elif name in ("origin", "bounds"):
        values = [float(v) for v in value]
    elif name == "radius":
        values = [float(value)]
    else:
        values = [int(value)]
    struct.pack_into("<%d%s" % (field.count, field.fmt[1]), out, off, *values)
    return bytes(out)


def diff_offsets(a: bytes, b: bytes) -> list[int]:
    if len(a) != len(b):
        raise EditError(f"length differs: {len(a)} vs {len(b)}")
    return [i for i, (x, y) in enumerate(zip(a, b)) if x != y]


# ---------------------------------------------------------------------------
# paths, level resolution
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


# ---------------------------------------------------------------------------
# verification (oracle + own walker)
# ---------------------------------------------------------------------------

def _outside_spans(scan: Scan, target: ObjectLoc) -> list[tuple[int, int]]:
    """Every byte of the file that is *not* inside the target object."""
    excluded = sorted([(target.header_off, target.header_off + target.header_size),
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


def _world_shift(orig: ObjectLoc, new: ObjectLoc):
    """``(max_deviation, delta)`` of ``new.world - orig.world`` against delta."""
    delta = tuple(new.origin[i] - orig.origin[i] for i in range(3))
    w0 = orig.world_vertices()
    w1 = new.world_vertices()
    worst = 0.0
    for p0, p1 in zip(w0, w1):
        for i in range(3):
            worst = max(worst, abs((p1[i] - p0[i]) - delta[i]))
    return worst, delta


def verify_edit(orig: bytes, new: bytes, obj_index: int, name: str,
                expected) -> dict:
    """Prove an edit: oracle value changed, every other byte untouched.

    All checks are mandatory:

    1. both blobs parse with the oracle to exactly EOF;
    2. texture/effect/sector/object/polygon counts and the object id words are
       unchanged;
    3. the bytes outside the target object (headers, vertices, tails, polygons
       of the other objects, tables, sector headers) are identical;
    4. every other object's 100-byte header and stage-2 record is identical;
    5. only the target field's bytes differ and the file actually changed;
    6. the local vertices of the target object are unchanged;
    7. both readers see the new value;
    8. the oracle's ``Sbw.to_bytes`` reproduces both blobs;
    9. ``world_vertices`` of the target moved exactly by the ``origin`` delta
       (for ``origin``) or stayed exactly the same (every other field), and no
       other object's world vertices moved.
    """
    checks = []
    problems = []
    if name not in FIELDS:
        raise EditError(f"unknown object field: {name} "
                        f"(known: {', '.join(FIELDS)})")

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})
        if not ok:
            problems.append(text)

    try:
        before = oracle.parse(orig)
        after = oracle.parse(new)
        check(before.data_end == len(orig), "oracle: source parses to EOF")
        check(after.data_end == len(new), "oracle: edited file parses to EOF")
        check((len(before.tex), len(before.effects), len(before.sectors),
               before.n_objects, before.n_polys) ==
              (len(after.tex), len(after.effects), len(after.sectors),
               after.n_objects, after.n_polys),
              "oracle: texture/effect/sector/object/polygon counts unchanged")
        ids_before = [o.id for s in before.sectors for o in s.objects]
        ids_after = [o.id for s in after.sectors for o in s.objects]
        check(ids_before == ids_after, "oracle: object id words unchanged")
    except ValueError as exc:
        check(False, f"oracle: parse failed: {exc}")
        return {"ok": False, "checks": checks, "problems": problems}

    try:
        s0 = walk(orig)
        s1 = walk(new)
    except ScanError as exc:
        check(False, f"own walker: {exc}")
        return {"ok": False, "checks": checks, "problems": problems}

    check(s0.data_end == len(orig) and s1.data_end == len(new),
          "own walker: both files to EOF")
    check((s0.n_tex, s0.n_eff, len(s0.sectors), s0.n_objects, s0.n_polys,
           s0.n_verts) ==
          (s1.n_tex, s1.n_eff, len(s1.sectors), s1.n_objects, s1.n_polys,
           s1.n_verts),
          "own walker: counts (objects/vertices/polygons) unchanged")
    check(s0.size == s1.size, "file size unchanged")

    if not 0 <= obj_index < s0.n_objects:
        check(False, f"object {obj_index} does not exist "
                     f"(file has {s0.n_objects}, 0-based)")
        return {"ok": False, "checks": checks, "problems": problems}

    check(all(orig[a:b] == new[a:b] for a, b in _outside_spans(s0, s0.objects[obj_index])),
          "bytes outside the target object are identical")
    check(_other_objects_identical(s0, s1, orig, new, obj_index),
          f"{s0.n_objects - 1} other objects (header + vertices + tail + "
          f"polygons) are identical")

    target0 = s0.objects[obj_index]
    target1 = s1.objects[obj_index]
    check(orig[target0.verts_off:target0.verts_off + 12 * target0.n_verts] ==
          new[target1.verts_off:target1.verts_off + 12 * target1.n_verts],
          "local vertices of the target object are unchanged")

    off, width = target0.span(name)
    changed = diff_offsets(orig, new)
    if changed:
        inside = all(off <= d < off + width for d in changed)
        check(inside, f"only {name} bytes differ (span {off}..{off + width}, "
                      f"{len(changed)} byte(s) changed)")
        check(True, "the file actually changed")
    else:
        # Asking for the value the file already carries is a legitimate no-op
        # (e.g. ``basis=identity`` on shipped data): nothing is written, and
        # that is exactly what the checks below prove.
        check(True, f"no-op: the file already carries {name}="
                    f"{_show(name, expected)}, so no byte changed")

    check(_matches(target1, name, expected),
          f"own walker sees {name}={_show(name, expected)}")
    try:
        check(_oracle_matches(_oracle_object(after, obj_index), name, expected),
              f"oracle sees {name}={_show(name, expected)}")
    except EditError as exc:
        check(False, str(exc))

    check(before.to_bytes() == orig, "oracle round-trip of the source is exact")
    check(after.to_bytes() == new, "oracle round-trip of the edit is exact")

    # world frame: origin translates the object, nothing else moves
    world = {"field": name, "objects_moved": 0}
    others_moved = 0
    for a, b in zip(s0.objects, s1.objects):
        if a.index == obj_index:
            continue
        if a.world_vertices() != b.world_vertices():
            others_moved += 1
    check(others_moved == 0,
          "no other object moved in the world frame "
          "(world = origin + basis * local)")

    if name == "origin":
        worst, delta = _world_shift(target0, target1)
        world.update({"delta": list(delta), "max_deviation": worst,
                      "vertices": len(target1.vertices)})
        check(worst <= 1e-6,
              f"world vertices of the target object shifted exactly by "
              f"{_fmt_vec(delta)} ({len(target1.vertices)} vertices, "
              f"max deviation {worst:.3g})")
        world["objects_moved"] = 1 if target0.vertices else 0
    else:
        same = target0.world_vertices() == target1.world_vertices()
        check(same, "world vertices of the target object are unchanged")
        world.update({"delta": [0.0, 0.0, 0.0], "max_deviation": 0.0,
                      "vertices": len(target1.vertices), "objects_moved": 0})

    # an f32 consistency note for radius/bounds: the engine culls with radius
    if name in ("radius", "bounds"):
        norm = math.sqrt(sum(float(c) * float(c) for c in target1.bounds))
        world["radius_vs_bounds"] = abs(target1.radius - norm)

    return {
        "ok": all(c["ok"] for c in checks),
        "noop": not changed,
        "checks": checks,
        "problems": problems,
        "changes": changed,
        "object": obj_index,
        "field": name,
        "value": _json_value(name, expected),
        "world": world,
    }


def _show(name: str, expected) -> str:
    if name == "basis":
        return "identity"
    if name in ("origin", "bounds"):
        return _fmt_vec(expected)
    return str(expected)


def _json_value(name: str, expected):
    if name == "basis":
        return "identity"
    if name in ("origin", "bounds"):
        return [float(v) for v in expected]
    if name == "radius":
        return float(expected)
    return int(expected)


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def list_level(path: str, limit: int = 8, game: str = DEFAULT_GAME) -> dict:
    data = read_bytes(path)
    scan = walk(data)
    w = oracle.parse(data)
    shown = scan.objects[:limit or len(scan.objects)]
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
        "shown": len(shown),
        "object_list": [],
        "sector_list": [
            {"index": s.index, "kind": s.kind, "sub": s.sub,
             "objects": len(s.objects), "vertices": s.n_verts,
             "polygons": s.n_polys}
            for s in (scan.sectors[:limit or len(scan.sectors)])
        ],
    }
    for o in shown:
        aabb = o.aabb()
        rep["object_list"].append({
            "index": o.index,
            "sector": o.sector,
            "obj": o.obj,
            "id": o.id,
            "n_verts": o.n_verts,
            "n_polys": o.n_polys,
            "origin": [float(v) for v in o.origin],
            "basis": basis_rows(o.basis),
            "basis_identity": is_identity(o.basis),
            "radius": float(o.radius),
            "bounds": [float(v) for v in o.bounds],
            "f74": o.f74, "f75": o.f75, "f78": o.f78, "f7c": o.f7c,
            "aabb_min": [float(v) for v in aabb[0]] if aabb else None,
            "aabb_max": [float(v) for v in aabb[1]] if aabb else None,
            "header_off": o.header_off,
            "geom_off": o.geom_off,
            "tail_off": o.tail_off,
        })
    return rep


def print_list(rep: dict, limit: int = 8) -> None:
    tail = f" (showing {rep['shown']})" if limit else ""
    print(f"level: {rep['path']}")
    print(f"header: {rep['bytes']} bytes, oracle parse ends at "
          f"{rep['oracle_parse_end']} "
          f"({'EOF ok' if rep['eof_ok'] else 'MISMATCH'})")
    print(f"tables: textures={rep['textures']} effects={rep['effects']} "
          f"(end {rep['table1_end']}, {rep['table2_end']})")
    print(f"world: sectors={rep['sectors']} objects={rep['objects']} "
          f"vertices={rep['vertices']} polygons={rep['polygons']}")
    print(f"sectors{tail}:")
    for s in rep["sector_list"]:
        print(f"  sector[{s['index']:3d}] kind={s['kind']:2d} sub={s['sub']} "
              f"objects={s['objects']:3d} verts={s['vertices']:5d} "
              f"polys={s['polygons']:5d}")
    print(f"objects{tail}:")
    for o in rep["object_list"]:
        print(f"  obj[{o['index']:5d}] sector={o['sector']:3d} "
              f"id=0x{o['id']:08x} verts={o['n_verts']:4d} "
              f"polys={o['n_polys']:4d} radius={_fmt_num(o['radius'])}")
        print(f"    origin={_fmt_vec(o['origin'])} "
              f"bounds={_fmt_vec(o['bounds'])} "
              f"f74={o['f74']} f75={o['f75']} f7c={o['f7c']} f78={o['f78']}")
        if o["basis_identity"]:
            print("    basis=identity")
        else:
            for row in o["basis"]:
                print(f"    basis={_fmt_vec(row)}")
        if o["aabb_min"]:
            print(f"    aabb=({_fmt_vec(o['aabb_min'])} .. "
                  f"{_fmt_vec(o['aabb_max'])})")
        else:
            print("    aabb=(no vertices)")


# ---------------------------------------------------------------------------
# --get / --set
# ---------------------------------------------------------------------------

def _require_object(scan: Scan, index: int | None) -> ObjectLoc:
    if index is None:
        raise EditError("--object N is required")
    if not 0 <= index < scan.n_objects:
        raise EditError(f"object {index} does not exist "
                        f"(file has {scan.n_objects}, 0-based)")
    return scan.objects[index]


def get_field(path: str, obj_index: int, name: str) -> dict:
    if name not in FIELDS:
        raise EditError(f"unknown object field: {name} "
                        f"(known: {', '.join(FIELDS)})")
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    off, width = obj.span(name)
    fld = FIELDS[name]
    rep = {
        "path": os.path.abspath(path),
        "object": obj_index,
        "sector": obj.sector,
        "obj": obj.obj,
        "id": obj.id,
        "n_verts": obj.n_verts,
        "n_polys": obj.n_polys,
        "field": name,
        "value": field_value(obj, name),
        "disk_offset": off,
        "width": width,
        "disk": fld.disk,
        "engine": fld.engine,
        "doc": fld.doc,
        "writer": fld.va,
        "aabb": None,
    }
    aabb = obj.aabb()
    if aabb:
        rep["aabb"] = {"min": [float(v) for v in aabb[0]],
                       "max": [float(v) for v in aabb[1]]}
    if name == "radius":
        norm = math.sqrt(sum(float(c) * float(c) for c in obj.bounds))
        rep["bounds_norm"] = norm
    return rep


def set_field(path: str, obj_index: int, assignment: str, out_path: str,
              game: str = DEFAULT_GAME) -> dict:
    name, value = parse_assignment(assignment)
    validate(name, value)
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    new = patch_bytes(data, obj, name, value)
    if not out_path:
        raise EditError("--set needs --out FILE (the game install is "
                        "read-only)")
    out_abs = os.path.abspath(out_path)
    game_abs = os.path.abspath(game)
    if out_abs == game_abs or out_abs.startswith(game_abs + os.sep):
        raise EditError(f"refusing to write into the game installation: "
                        f"{out_path} is inside {game}; choose an output "
                        f"outside it (e.g. /tmp/opencode/...)")
    write_bytes(out_path, new)
    rep = verify_edit(data, new, obj_index, name, value)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "old": field_value(obj, name),
        "new": _json_value(name, value),
        "bytes": len(new),
    })
    if rep["noop"]:
        rep["note"] = (f"{name} already had the requested value: the output "
                       f"is a byte-identical copy (nothing to change)")
    if name == "origin" and not rep["noop"]:
        rep["note"] = (f"object translated as a whole: {obj.n_verts} local "
                       f"vertices untouched, world frame shifted by "
                       f"{_fmt_vec(rep['world']['delta'])}")
    if (name == "radius" and not rep["noop"]
            and rep.get("world", {}).get("radius_vs_bounds") is not None):
        d = rep["world"]["radius_vs_bounds"]
        if d > 2.0:
            rep["note"] = (f"radius differs from |bounds| by {d:.3g}; the "
                           f"shipped data tolerates up to ~4129 (bounds are "
                           f"rounded integers), cull uses radius")
    return rep


def print_edit(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"obj[{rep['object']}] {rep['field']}: {_show_value(rep['old'])} -> "
          f"{_show_value(rep['new'])} ({rep['bytes']} bytes, unchanged size)")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("note"):
        print(f"  note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


def _show_value(v) -> str:
    if isinstance(v, dict) and "rows" in v:
        if v.get("identity"):
            return "identity"
        return "; ".join(_fmt_vec(r) for r in v["rows"])
    if isinstance(v, list):
        return _fmt_vec(v)
    if isinstance(v, float):
        return repr(v)
    return str(v)


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def check_file(path: str, against: str | None = None,
               obj_index: int | None = None, assignment: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    """Re-read *path* with the oracle and the own walker.

    With ``against`` (a level name/path) the two files are compared: when
    ``obj_index``/``assignment`` are given every other object and byte must be
    identical and the target field must carry the new value; otherwise the
    whole file must be identical.
    """
    data = read_bytes(path)
    rep: dict = {"path": os.path.abspath(path), "bytes": len(data), "ok": False,
                 "checks": [], "problems": []}

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
               w.n_polys, sum(o.n_verts for s in w.sectors for o in s.objects)),
              "own walker and oracle agree on counts")
        check(w.to_bytes() == data, "oracle: read -> write is byte-exact")
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
    if assignment:
        if obj_index is None:
            raise EditError("--check with --field needs --object N")
        name, value = parse_assignment(assignment)
        rep.update({"object": obj_index, "field": name,
                    "value": _json_value(name, value)})
        edit = verify_edit(orig, data, obj_index, name, value)
        rep["checks"].extend(edit["checks"])
        rep["problems"].extend(edit["problems"])
        rep["changes"] = edit.get("changes", [])
        rep["world"] = edit.get("world")
        rep["ok"] = all(c["ok"] for c in rep["checks"])
        return rep

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
    if rep.get("field"):
        print(f"target: obj[{rep['object']}] {rep['field']}="
              f"{_show_value(rep['value'])}")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("changes") and not rep["ok"]:
        print(f"  differing bytes: {rep['changes'][:16]}"
              f"{' ...' if len(rep['changes']) > 16 else ''}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --roundtrip / --audit
# ---------------------------------------------------------------------------

def roundtrip_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "identical": 0,
           "mismatched": [], "objects": 0, "vertices": 0, "polygons": 0,
           "bytes": 0, "failures": [], "ok": False}
    for path in files:
        name = os.path.basename(path)
        try:
            data = read_bytes(path)
            w = oracle.parse(data)
            built = w.to_bytes()
            scan = walk(data)
        except (EditError, ValueError, ScanError) as exc:
            rep["failures"].append(f"{name}: {exc}")
            if verbose:
                print(f"  FAIL {name}: {exc}")
            continue
        rep["bytes"] += len(data)
        rep["objects"] += scan.n_objects
        rep["vertices"] += scan.n_verts
        rep["polygons"] += scan.n_polys
        problems = []
        if built != data:
            problems.append("oracle to_bytes differs")
        if w.data_end != len(data):
            problems.append(f"parse ends at {w.data_end}/{len(data)}")
        if scan.data_end != len(data):
            problems.append("own walker does not end at EOF")
        if (scan.n_objects, scan.n_polys) != (w.n_objects, w.n_polys):
            problems.append(f"counts {scan.n_objects}/{scan.n_polys} != "
                            f"{w.n_objects}/{w.n_polys}")
        # every editable object field must agree between both readers
        flat = [o for s in w.sectors for o in s.objects]
        for loc, obj in zip(scan.objects, flat):
            if tuple(obj.points[0]) != tuple(loc.origin):
                problems.append(f"origin mismatch at object {loc.index}")
                break
            if not is_identity([c for row in obj.points[1:] for c in row]):
                problems.append(f"non-identity basis at object {loc.index}")
                break
            if (obj.radius, tuple(obj.bounds), obj.f74, obj.f75, obj.f78,
                    obj.f7c) != (loc.radius, tuple(loc.bounds), loc.f74,
                                 loc.f75, loc.f78, loc.f7c):
                problems.append(f"tail mismatch at object {loc.index}")
                break
        if problems:
            rep["mismatched"].append({"file": name, "problems": problems})
            if verbose:
                print(f"  FAIL {name}: {'; '.join(problems)}")
        else:
            rep["identical"] += 1
            if verbose:
                print(f"  ok   {name:14s} {len(data):8d} bytes "
                      f"{scan.n_objects:5d} objects {scan.n_polys:6d} polys")
    rep["ok"] = (rep["identical"] == rep["files"] and rep["files"] > 0
                 and not rep["failures"])
    return rep


def print_roundtrip(rep: dict) -> None:
    print(f"round-trip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['objects']} objects, {rep['vertices']} vertices, "
          f"{rep['polygons']} polygons, {rep['bytes']} bytes ({rep['game']})")
    for m in rep["mismatched"]:
        print(f"  MISMATCH {m['file']}: {'; '.join(m['problems'])}")
    for f in rep["failures"]:
        print(f"  FAILURE {f}")


def audit_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    """Measure every editable object field on the whole shipped corpus.

    This is the evidence behind :func:`validate`: which values the shipped
    data itself uses, that the basis is the identity, and how far ``radius``
    drifts from ``|bounds|``.
    """
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "objects": 0,
           "violations": [], "basis_identity": 0, "non_identity": 0,
           "ranges": {}, "f_sets": {}, "ids": [], "radius_vs_bounds": {},
           "ok": False}
    origin = [[None, None] for _ in range(3)]
    ranges = {"origin": origin, "radius": [None, None],
              "bounds": [[None, None] for _ in range(3)]}
    f_sets = {k: set() for k in ("f74", "f75", "f78", "f7c")}
    ids = set()
    drift = []
    for path in files:
        data = read_bytes(path)
        scan = walk(data)
        rep["objects"] += scan.n_objects
        for o in scan.objects:
            ids.add(o.id)
            for i in range(3):
                b = ranges["origin"][i]
                b[0] = o.origin[i] if b[0] is None else min(b[0], o.origin[i])
                b[1] = o.origin[i] if b[1] is None else max(b[1], o.origin[i])
                b = ranges["bounds"][i]
                b[0] = o.bounds[i] if b[0] is None else min(b[0], o.bounds[i])
                b[1] = o.bounds[i] if b[1] is None else max(b[1], o.bounds[i])
            b = ranges["radius"]
            b[0] = o.radius if b[0] is None else min(b[0], o.radius)
            b[1] = o.radius if b[1] is None else max(b[1], o.radius)
            if is_identity(o.basis):
                rep["basis_identity"] += 1
            else:
                rep["non_identity"] += 1
                if len(rep["violations"]) < 32:
                    rep["violations"].append(
                        f"{os.path.basename(path)} obj {o.index}: "
                        f"non-identity basis")
            for k in f_sets:
                f_sets[k].add(int(getattr(o, k)))
            try:
                validate("origin", o.origin)
                validate("radius", float(o.radius))
                validate("bounds", tuple(o.bounds))
            except EditError as exc:
                if len(rep["violations"]) < 32:
                    rep["violations"].append(
                        f"{os.path.basename(path)} obj {o.index}: {exc}")
                if verbose:
                    print(f"  VIOLATION {os.path.basename(path)} "
                          f"obj {o.index}: {exc}")
            norm = math.sqrt(sum(float(c) * float(c) for c in o.bounds))
            drift.append((abs(o.radius - norm), o.index, os.path.basename(path)))
            if not 0.0 <= o.radius <= 1e7:
                if len(rep["violations"]) < 32:
                    rep["violations"].append(
                        f"{os.path.basename(path)} obj {o.index}: radius "
                        f"{o.radius} outside [0, 1e7]")
    drift.sort()
    rep["ranges"] = {
        "origin": [[round(v, 3) if v is not None else None for v in comp]
                   for comp in origin],
        "radius": ranges["radius"],
        "bounds": [[round(v, 3) if v is not None else None for v in comp]
                   for comp in ranges["bounds"]],
    }
    rep["f_sets"] = {k: sorted(v) for k, v in f_sets.items()}
    rep["ids"] = [f"0x{v:x}" for v in sorted(ids)]
    rep["radius_vs_bounds"] = {
        "max": drift[-1][0] if drift else 0.0,
        "max_at": f"{drift[-1][2]} obj {drift[-1][1]}" if drift else "",
        "exact": sum(1 for d in drift if d[0] < 1e-6),
        "within_2": sum(1 for d in drift if d[0] <= 2.0),
    }
    rep["ok"] = not rep["violations"] and rep["objects"] > 0
    return rep


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['objects']} objects "
          f"({rep['game']})")
    o = rep["ranges"]["origin"]
    print(f"  origin      x [{o[0][0]}, {o[0][1]}]  y [{o[1][0]}, {o[1][1]}]"
          f"  z [{o[2][0]}, {o[2][1]}]")
    print(f"  radius      [{rep['ranges']['radius'][0]}, "
          f"{rep['ranges']['radius'][1]}]")
    b = rep["ranges"]["bounds"]
    print(f"  bounds      x [{b[0][0]}, {b[0][1]}]  y [{b[1][0]}, {b[1][1]}]"
          f"  z [{b[2][0]}, {b[2][1]}]")
    print(f"  basis       identity {rep['basis_identity']}/{rep['objects']}"
          f", non-identity {rep['non_identity']}")
    print(f"  f74 {rep['f_sets']['f74']}  f75 {rep['f_sets']['f75']}  "
          f"f78 {rep['f_sets']['f78']}  f7c {rep['f_sets']['f7c']}")
    print(f"  id words    {', '.join(rep['ids'])}")
    d = rep["radius_vs_bounds"]
    print(f"  radius vs |bounds|: exact {d['exact']}/{rep['objects']}, "
          f"<=2 {d['within_2']}/{rep['objects']}, max {d['max']:.4g} "
          f"({d['max_at']})")
    for v in rep["violations"]:
        print(f"  VIOLATION {v}")
    print(f"result: {'OK -- every editable object field is inside its range' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --fields
# ---------------------------------------------------------------------------

def print_fields(as_json: bool = False) -> None:
    rows = []
    for name, f in FIELDS.items():
        rows.append({
            "field": name,
            "disk": f.disk,
            "engine": f.engine,
            "type": f"{f.count}x{f.fmt[1]}",
            "width": f.width,
            "meaning": f.meaning,
            "doc": f.doc,
            "evidence": f.va,
        })
    if as_json:
        print(json.dumps({"editable": rows, "unavailable": UNAVAILABLE},
                         indent=1, ensure_ascii=False))
        return
    print("editable object fields (disk offset is relative to the object; "
          "header is the 96-byte transform in stage 1, tail the 50-byte stage-2 tail):")
    for r in rows:
        print(f"  {r['field']:8s} disk {r['disk']:12s} engine {r['engine']}  "
              f"{r['type']:8s} {r['meaning']}")
        print(f"            doc: {r['doc']}")
        print(f"            engine evidence: {r['evidence']}")
    print(f"  basis policy: {BASIS_NOTE}")
    print("not editable:")
    for k, v in UNAVAILABLE.items():
        print(f"  {k}: {v}")


# ---------------------------------------------------------------------------
# self-test (synthetic, no game files)
# ---------------------------------------------------------------------------

def _synth_poly(texture=1, lightmap=0, material=0, overlay=0, brightness=8,
                flags=0x8000, indices=(0, 1, 2)) -> bytes:
    """A minimal polygon record: 12-byte head + indices + 112-byte tail."""
    tail = bytearray(112)
    struct.pack_into("<I", tail, 0x66, flags)
    head = struct.pack("<5H", texture & 0xFFFF, lightmap & 0xFFFF,
                       material & 0xFFFF, overlay & 0xFFFF, 0)
    return (head + bytes((brightness & 0xFF, len(indices) & 0xFF))
            + struct.pack("<%dH" % len(indices), *indices) + bytes(tail))


def _synth_header(origin=(0.0, 0.0, 0.0)) -> bytes:
    vals = list(origin) + [c for row in IDENTITY for c in row]
    return struct.pack("<12d", *vals)


def _synth_geom(n_verts: int, polys: list[bytes], radius=1.0, f75=0, f74=0,
                f7c=0, f78=0, bounds=(1.0, 1.0, 1.0)) -> bytes:
    out = bytearray(struct.pack("<HH", n_verts, len(polys)))
    for k in range(n_verts):
        out += struct.pack("<3f", float(k), 0.5 * k, 1.0 + k)
    out += struct.pack("<f", radius) + bytes((f75, f74))
    out += struct.pack("<II", f7c, f78)
    out += struct.pack("<3f", *bounds)
    out += bytes(24)
    assert len(out) == 4 + 12 * n_verts + 50
    for p in polys:
        out += p
    return bytes(out)


def _synth_file() -> bytes:
    """A tiny but valid ``.sbw``: 2 textures, 1 effect, 2 sectors, 4 objects."""
    out = bytearray()
    tex = [("wall", 0, 3), ("floor", 1, 7)]
    out += struct.pack("<I", len(tex))
    for name, flag, value in tex:
        out += name.encode("latin1") + b"\0" + struct.pack("<HI", flag, value)
    out += struct.pack("<I", 1)
    out += b"fire\0" + struct.pack("<III", 1, 0, 3) + struct.pack("<3H", 0, 1, 2)

    objects = [
        # (id word, origin, n_verts, n_polys, stage-2 tail kwargs)
        (0, (0.0, 0.0, 0.0), 4, 1, dict(radius=1.5, bounds=(1.0, 2.0, 3.0))),
        (8, (10.0, 0.0, 0.0), 4, 2, dict(radius=2.0, f75=1,
                                        bounds=(1.0, 1.0, 1.0))),
        (0x10000, (0.0, 5.0, -2.0), 3, 1, dict(radius=3.5,
                                               bounds=(2.0, 2.0, 2.0))),
        (0x100000, (-5.0, 0.0, 0.0), 5, 0, dict(radius=0.0,
                                                bounds=(0.0, 0.0, 0.0))),
    ]
    layout = [
        (0, 0, [(0, (0.0, 0.0, 0.0)), (8, (10.0, 0.0, 0.0))]),
        (6, 1, [(0x10000, (0.0, 5.0, -2.0)), (0x100000, (-5.0, 0.0, 0.0))]),
    ]
    out += struct.pack("<I", len(layout))
    for kind, sub, objs in layout:
        out += struct.pack("<IHB", len(objs), kind, sub)
        for oid, origin in objs:
            out += struct.pack("<I", oid) + _synth_header(origin)
    by_id = {0: 0, 8: 1, 0x10000: 2, 0x100000: 3}
    for _kind, _sub, objs in layout:
        for oid, _origin in objs:
            i = by_id[oid]
            _oid, _orig, nv, np_, kw = objects[i]
            polys = [_synth_poly(1 + k % 2) for k in range(np_)] if np_ else []
            out += _synth_geom(nv, polys, **kw)
    return bytes(out)


def _selftest() -> bool:
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
               w.n_polys) == (2, 1, 2, 4, 4))
    except Exception as exc:                           # noqa: BLE001
        check("synth: oracle parses", False, str(exc))
        w = None

    scan = walk(data)
    check("synth: own walker reaches EOF", scan.data_end == len(data))
    check("synth: own walker counts",
          (scan.n_tex, scan.n_eff, len(scan.sectors), scan.n_objects,
           scan.n_polys, scan.n_verts) == (2, 1, 2, 4, 4, 16))
    flat = [o for s in w.sectors for o in s.objects] if w is not None else []
    fields_ok = w is not None and all(
        tuple(obj.points[0]) == tuple(loc.origin)
        and tuple(tuple(r) for r in obj.points[1:]) ==
        tuple(tuple(loc.basis[r * 3:r * 3 + 3]) for r in range(3))
        and (obj.radius, tuple(obj.bounds), obj.f74, obj.f75, obj.f78, obj.f7c,
             obj.n_verts, len(obj.polys)) ==
            (loc.radius, tuple(loc.bounds), loc.f74, loc.f75, loc.f78, loc.f7c,
             loc.n_verts, loc.n_polys)
        for loc, obj in zip(scan.objects, flat))
    check("synth: every object field matches the oracle", fields_ok)

    # 2: the byte layout itself
    o0 = scan.objects[0]
    check("layout: header is u32 id + 96 bytes",
          o0.header_size == 100 and o0.geom_off >= o0.header_off + 100)
    check("layout: origin at header+0x04 (3 x f64)",
          struct.unpack_from("<3d", data, o0.header_off + 4) == (0.0, 0.0, 0.0))
    check("layout: basis at header+0x1C (9 x f64)",
          struct.unpack_from("<9d", data, o0.header_off + 28) ==
          tuple(c for row in IDENTITY for c in row))
    check("layout: n_verts/n_polys at geom_off",
          struct.unpack_from("<HH", data, o0.geom_off) == (4, 1))
    check("layout: vertices at geom_off+4",
          struct.unpack_from("<3f", data, o0.verts_off) == (0.0, 0.0, 1.0))
    check("layout: radius at tail+0x00",
          struct.unpack_from("<f", data, o0.tail_off)[0] == 1.5)
    check("layout: f75 at tail+0x04, f74 at tail+0x05",
          data[o0.tail_off + 4] == 0 and data[o0.tail_off + 5] == 0)
    check("layout: f7c at tail+0x06, f78 at tail+0x0A",
          struct.unpack_from("<II", data, o0.tail_off + 6) == (0, 0))
    check("layout: bounds at tail+0x0E",
          struct.unpack_from("<3f", data, o0.tail_off + 14) == (1.0, 2.0, 3.0))
    check("layout: reserved 24 zero bytes at tail+0x1A",
          data[o0.tail_off + 26:o0.tail_off + 50] == bytes(24))
    check("layout: record sizes", all(
        o.polys_end - o.geom_off == 4 + 12 * o.n_verts + 50
        + sum(124 + 2 * 3 for _ in range(o.n_polys)) for o in scan.objects))

    # 3: world = origin + basis * local
    world = o1 = scan.objects[1]
    check("world: origin (10,0,0) translates the object",
          o1.world_vertices()[0] == (10.0, 0.0, 1.0))
    aabb = o1.aabb()
    check("world: aabb of the world vertices",
          aabb is not None and aabb[0] == (10.0, 0.0, 1.0)
          and aabb[1] == (13.0, 1.5, 4.0), repr(aabb))

    # 4: every editable field round-trips through patch_bytes + verify_edit
    edits = [
        ("origin", (64.0, 0.0, 0.0), 0, "translate +64 on X"),
        ("origin", (0.0, 0.0, 64.0), 2, "translate +64 on Z"),
        ("origin", (-1.5, 2.25, -3.0), 3, "fractional translation"),
        ("radius", 289.5, 0, "new bounding-sphere radius"),
        ("radius", 0.0, 1, "degenerate radius"),
        ("bounds", (7.5, 8.0, 9.0), 0, "new AABB half-extents"),
        ("bounds", (0.0, 0.0, 0.0), 2, "zero box"),
        ("f75", 1, 0, "auxiliary flag 1"),
        ("f74", 7, 1, "auxiliary byte 7"),
        ("f78", 9, 2, "auxiliary word 9"),
        ("f7c", 11, 3, "auxiliary word 11"),
    ]
    for name, value, target, why in edits:
        try:
            validate(name, value)
            new = patch_bytes(data, scan.objects[target], name, value)
            rep = verify_edit(data, new, target, name, value)
            check(f"edit: {name}={value} ({why})", rep["ok"],
                  "; ".join(rep.get("problems", [])))
        except Exception as exc:                       # noqa: BLE001
            check(f"edit: {name}={value} ({why})", False, str(exc))

    # 5: basis=identity is accepted as a no-op on shipped-style data
    try:
        new = patch_bytes(data, scan.objects[0], "basis", "identity")
        check("basis: identity patch does not change the bytes", new == data)
        rep = verify_edit(data, new, 0, "basis", "identity")
        check("basis: identity is a verified no-op",
              rep["ok"] and rep["noop"],
              "; ".join(rep.get("problems", [])))
    except Exception as exc:                           # noqa: BLE001
        check("basis: identity patch", False, str(exc))

    # 6: refusals (range / shape / unknown / index)
    expect_refused("refuse: origin with two numbers",
                   lambda: validate("origin", parse_vec("1,2", 3, "origin")))
    expect_refused("refuse: origin with four numbers",
                   lambda: validate("origin", parse_vec("1,2,3,4", 3, "origin")))
    expect_refused("refuse: origin nan",
                   lambda: validate("origin", parse_value("origin", "nan,0,0")))
    expect_refused("refuse: origin inf",
                   lambda: validate("origin", parse_value("origin", "inf,0,0")))
    expect_refused("refuse: origin not a number",
                   lambda: parse_value("origin", "1,2,x"))
    expect_refused("refuse: origin implausible",
                   lambda: validate("origin", (1e13, 0.0, 0.0)))
    expect_refused("refuse: bounds negative",
                   lambda: validate("bounds", (-1.0, 0.0, 0.0)))
    expect_refused("refuse: bounds two numbers",
                   lambda: validate("bounds", parse_vec("1,2", 3, "bounds")))
    expect_refused("refuse: bounds nan",
                   lambda: validate("bounds", parse_value("bounds", "nan,1,1")))
    expect_refused("refuse: radius negative",
                   lambda: validate("radius", -1.0))
    expect_refused("refuse: radius nan",
                   lambda: validate("radius", parse_value("radius", "nan")))
    expect_refused("refuse: radius too large for f32",
                   lambda: validate("radius", 1e39))
    expect_refused("refuse: radius not a number",
                   lambda: parse_value("radius", "abc"))
    expect_refused("refuse: f75 256", lambda: validate("f75", 256))
    expect_refused("refuse: f74 -1", lambda: validate("f74", -1))
    expect_refused("refuse: f78 256", lambda: validate("f78", 256))
    expect_refused("refuse: f7c 300", lambda: validate("f7c", 300))
    expect_refused("refuse: f75 not an integer",
                   lambda: parse_value("f75", "1.5"))
    expect_refused("refuse: explicit rotation basis",
                   lambda: parse_value("basis", "1,0,0,0,1,0,0,0,1"))
    expect_refused("refuse: scaled basis",
                   lambda: parse_value("basis", "2,0,0,0,2,0,0,0,2"))
    expect_refused("refuse: unknown basis word",
                   lambda: parse_value("basis", "translate"))
    expect_refused("refuse: unknown field",
                   lambda: validate("normal", (1.0, 0.0, 0.0)))
    expect_refused("refuse: unknown field in --get",
                   lambda: get_field("/nonexistent", 0, "normal"))
    expect_refused("refuse: object out of range",
                   lambda: _require_object(scan, 4))
    expect_refused("refuse: negative object index",
                   lambda: _require_object(scan, -1))
    expect_refused("refuse: assignment without =",
                   lambda: parse_assignment("origin"))

    # 7: truncation / corruption is reported, not guessed
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
            oracle_ok = True
        except Exception:                              # noqa: BLE001
            oracle_ok = False
        else:
            oracle_ok = False
        check(f"truncation -{cut}: own walker refuses", own_ok)
        check(f"truncation -{cut}: oracle refuses", oracle_ok)

    bad = bytearray(data)
    bad[o0.geom_off:o0.geom_off + 2] = struct.pack("<H", 0xFFFF)   # n_verts
    try:
        walk(bytes(bad))
        own_bad = False
    except ScanError:
        own_bad = True
    try:
        oracle.parse(bytes(bad))
        oracle_bad = False
    except ValueError:
        oracle_bad = True
    check("corrupt n_verts: own walker refuses", own_bad)
    check("corrupt n_verts: oracle refuses", oracle_bad)

    # 8: verification is not vacuous
    new = patch_bytes(data, o0, "origin", (64.0, 0.0, 0.0))
    tampered = bytearray(new)
    tampered[o0.verts_off] ^= 0xFF                       # a local vertex
    rep = verify_edit(data, bytes(tampered), 0, "origin", (64.0, 0.0, 0.0))
    check("verify: detects a changed vertex", not rep["ok"])
    check("verify: reports the field span",
          any("only origin bytes differ" in c["check"] for c in rep["checks"]
              if not c["ok"]))

    tampered = bytearray(new)
    other = scan.objects[1]
    tampered[other.tail_off] ^= 0xFF                     # another object
    rep = verify_edit(data, bytes(tampered), 0, "origin", (64.0, 0.0, 0.0))
    check("verify: detects a change in another object", not rep["ok"])

    tampered = bytearray(new)
    tampered[o0.tail_off + 14] ^= 0x01                   # same object, tail
    rep = verify_edit(data, bytes(tampered), 0, "origin", (64.0, 0.0, 0.0))
    check("verify: detects bounds change on an origin edit", not rep["ok"])

    # 9: the whole --set/--check path through temporary files
    import tempfile
    tmpdir = None
    try:
        tmpdir = tempfile.mkdtemp(prefix="sbwobj-", dir=DEFAULT_OUT_DIR
                                  if os.path.isdir(DEFAULT_OUT_DIR) else None)
        src = os.path.join(tmpdir, "synthetic.sbw")
        dst = os.path.join(tmpdir, "synthetic_edit.sbw")
        write_bytes(src, data)
        rep = set_field(src, 1, "origin=74,0,0", dst)
        check("set: writes and verifies", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("set: output is readable by the oracle",
              oracle.parse_file(dst).n_objects == 4)
        chk = check_file(dst, src, 1, "origin=74,0,0", game=tmpdir)
        check("check: accepts the edit", chk["ok"],
              "; ".join(chk.get("problems", [])))
        chk2 = check_file(dst, src, 1, "origin=10,0,0", game=tmpdir)
        check("check: rejects a wrong expectation", not chk2["ok"])
        chk3 = check_file(dst, src, game=tmpdir)
        check("check: full identity rejects the edit", not chk3["ok"])
        chk4 = check_file(src, src, game=tmpdir)
        check("check: identical file passes", chk4["ok"])
        expect_refused(
            "set: refuses without --out",
            lambda: set_field(src, 0, "origin=1,2,3", ""))
        expect_refused(
            "set: refuses a negative radius",
            lambda: set_field(src, 0, "radius=-1", dst))
        expect_refused(
            "set: refuses a rotated basis",
            lambda: set_field(src, 0, "basis=1,0,0,0,1,0,0,0,1", dst))
        expect_refused(
            "set: refuses an unknown field",
            lambda: set_field(src, 0, "normal=1,2,3", dst))
        expect_refused(
            "set: refuses to write into the game install",
            lambda: set_field(src, 0, "origin=1,2,3", dst, game=tmpdir))
    except Exception as exc:                           # noqa: BLE001
        check("set: temporary-file cycle", False, str(exc))
    finally:
        if tmpdir:
            for name in os.listdir(tmpdir):
                try:
                    os.remove(os.path.join(tmpdir, name))
                except OSError:
                    pass
            try:
                os.rmdir(tmpdir)
            except OSError:
                pass

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

def _cmd_list(args) -> int:
    path = resolve_level(args.list, args.game)
    limit = max(args.limit, 0)
    rep = list_level(path, limit, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_list(rep, limit)
    return 0 if rep["eof_ok"] else 1


def _cmd_get(args) -> int:
    if args.field is None:
        raise EditError("--get needs --field NAME")
    if args.object is None:
        raise EditError("--get needs --object N")
    path = resolve_level(args.get, args.game)
    rep = get_field(path, args.object, args.field)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print(f"obj[{rep['object']}] (sector {rep['sector']}, index "
              f"{rep['obj']}, id word 0x{rep['id']:08x}, {rep['n_verts']} "
              f"verts, {rep['n_polys']} polys)")
        print(f"  {rep['field']} = {_show_value(rep['value'])}")
        print(f"  disk {rep['disk']} (offset {rep['disk_offset']}), "
              f"{rep['width']} bytes, engine {rep['engine']}")
        print(f"  doc: {rep['doc']}")
        print(f"  engine evidence: {rep['writer']}")
        if rep["field"] == "radius" and rep.get("bounds_norm") is not None:
            print(f"  |bounds| = {rep['bounds_norm']:.6g} "
                  f"(difference {abs(rep['value'] - rep['bounds_norm']):.3g})")
        if rep["field"] == "basis" and rep["value"]["identity"]:
            print("  note: basis is the identity, so world = origin + local")
        if rep["aabb"]:
            print(f"  aabb = ({_fmt_vec(rep['aabb']['min'])} .. "
                  f"{_fmt_vec(rep['aabb']['max'])})")
    return 0


def _cmd_set(args) -> int:
    if args.field is None:
        raise EditError("--set needs --field NAME=VALUE")
    path = resolve_level(args.set, args.game)
    if args.object is None:
        raise EditError("--set needs --object N")
    if not args.out:
        raise EditError("--set needs --out FILE")
    rep = set_field(path, args.object, args.field, args.out, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    rep = check_file(args.check, args.against, args.object, args.field, args.game)
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
        elif head in ("fields", "--fields"):
            argv = ["--fields"] + argv[1:]
        elif head in ("audit", "--audit"):
            argv = ["--audit"] + argv[1:]
        elif head in ("list", "--list") and len(argv) > 1:
            argv = ["--list", argv[1]] + argv[2:]
        elif head in ("check", "--check") and len(argv) > 1:
            argv = ["--check", argv[1]] + argv[2:]
        elif head in ("get", "--get") and len(argv) > 1:
            argv = ["--get", argv[1]] + argv[2:]
        elif head in ("set", "--set") and len(argv) > 1:
            argv = ["--set", argv[1]] + argv[2:]
    ap = argparse.ArgumentParser(
        description="Edit .sbw world objects (origin/basis/radius/bounds/"
                    "aux words) of a Gunmetal level (M11.2, slice 3).")
    ap.add_argument("--list", metavar="LEVEL",
                    help="list sectors and objects (origin/basis/radius/"
                         "bounds/AABB)")
    ap.add_argument("--get", metavar="LEVEL", help="read one object field")
    ap.add_argument("--set", metavar="LEVEL", help="edit one object field")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE (oracle + own walker) and verify")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source level")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every shipped .sbw")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests (no game files)")
    ap.add_argument("--fields", action="store_true",
                    help="print the editable object field map with provenance")
    ap.add_argument("--audit", action="store_true",
                    help="measure every editable object field on the corpus")
    ap.add_argument("--object", type=int, metavar="N",
                    help="object index (0-based, order of --list)")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="field name; --set/--check need NAME=VALUE")
    ap.add_argument("--out", metavar="PATH", help="output .sbw")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=8,
                    help="--list: max elements shown per block (0 = all)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.self_test:
            return 0 if _selftest() else 1
        if args.fields:
            print_fields(args.json)
            return 0
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
        if args.get:
            return _cmd_get(args)
        if args.set:
            return _cmd_set(args)
        ap.print_help()
        return 2
    except EditError as exc:
        print(f"sbwobj: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"sbwobj: corrupt .sbw: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"sbwobj: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"sbwobj: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
