#!/usr/bin/env python3
"""Vertex editor for Gunmetal ``.sbw`` world geometry (``M11.2``, slice 4).

The two neighbouring slices edit the *object* (``tools/editor/sbwobj.py``:
``origin``/``radius``/``bounds``/aux words) and the *polygon* fields
(``tools/editor/sbwedit.py``).  This tool edits the **vertices**:

* ``--list`` — every object with its vertices: index, local ``x,y,z``,
  world ``x,y,z`` (through the object header) and how many polygons use the
  vertex;
* ``--set-vertex`` — move **one local** vertex; the world position follows
  ``origin + basis * local`` and nothing else in the file changes;
* ``--translate`` — move **all** local vertices of an object by a delta.  This
  differs from ``sbwobj.py --field origin``: there ``origin`` (the header)
  moves and the local coordinates stay; here the local coordinates move and the
  header stays;
* ``--check`` — re-read a written file with the oracle ``gm.sbw`` *and* the own
  walker and prove every other byte is identical;
* ``--roundtrip`` — 36 shipped ``.sbw`` files, byte-exact, plus a metadata
  check; ``--self-test`` — synthetic file, no game data; ``--audit`` — measure
  the recompute rule on the whole corpus.

A level is one sequential stream (``gm/sbw.py:1-128``, ``docs/FORMATS.md``
§10).  An object appears twice: stage 1 (``u32 flags`` + 96-byte header
``= 12 x f64``: ``origin[3]`` + ``basis[3][3]`` row-major) and stage 2
(geometry).  Stage 2 for one object is::

    u16 n_verts; u16 n_polys;
    f32 verts[n_verts][3];   // object-LOCAL positions, 12 bytes each
    f32 radius;              // 0x60
    u8  f75; u8 f74;         // 0x75 / 0x74
    u32 f7c; u32 f78;        // 0x7c / 0x78
    f32 bounds[3];           // 0x84, AABB half-extents around the object origin
    u8  reserved[24];        // 0x90
    n_polys * { poly }       // 124 + 2*n_idx bytes each

so a vertex is a 12-byte field inside a fixed-size record: it is patched in
place, ``n_verts``/``n_polys`` never change and no byte shifts
(:func:`patch_vertex`, :func:`patch_translate`).

``radius``/``bounds`` are recomputed from the **actual** vertices after every
edit (:func:`recompute_meta`), because the engine culls with them
(``0x437d8c``/``0x437d05``) and builds the sector AABB from ``origin ± bounds``
(``0x434fed``); ``docs/SBW_OBJECTS.md`` documents them as “metadata read by
cull”.  The rule below is not a guess — it reproduces the shipped values
**bit-exactly for 28 192/28 192 objects** (``--audit``, ``--roundtrip``)::

    bounds[k] = f32(max over vertices of |v[k]|)      // max(|x|), max(|y|), max(|z|)
    radius    = f32(max over vertices of |v|)         // max Euclidean distance

i.e. ``bounds`` is the AABB half-extent **around the object origin** (not
``(max-min)/2``), and ``radius`` is the true bounding-sphere radius around that
origin (``<= |bounds|``, equal when a vertex realises the AABB corner).
``docs/WORLD_FIELDS.md`` §2.3 states the ``bounds`` rule (``max(|x|), …``,
``FABS``); the ``radius`` rule is measured here on the whole corpus.

Usage::

    python3 tools/editor/sbwmesh.py --list level0
    python3 tools/editor/sbwmesh.py --list level0 --object 0 --limit 4
    python3 tools/editor/sbwmesh.py --list level0 --object 0 --limit 0 --json
    python3 tools/editor/sbwmesh.py --set-vertex level0 --object 0 --index 0 \\
        --pos 192,184,128 --out /tmp/opencode/sbwmesh/vtx/level0.sbw
    python3 tools/editor/sbwmesh.py --translate level0 --object 0 \\
        --delta 0,0,16 --out /tmp/opencode/sbwmesh/tr/level0.sbw
    python3 tools/editor/sbwmesh.py --check FILE --against level0 \\
        --object 0 --index 0 --pos 192,184,128
    python3 tools/editor/sbwmesh.py --check FILE --against level0 \\
        --object 0 --delta 0,0,16
    python3 tools/editor/sbwmesh.py --roundtrip
    python3 tools/editor/sbwmesh.py --self-test
    python3 tools/editor/sbwmesh.py --audit

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
DEFAULT_OUT_DIR = "/tmp/opencode/sbwmesh"

#: Same trust-boundary caps as the oracle (``gm/sbw.py:359-363``).
MAX_TABLE = 65_536
MAX_OBJECTS = 65_536

OBJECT_HEADER = 96                                # 12 x f64
OBJECT_TAIL = 50                                  # radius .. reserved[24]
VERTEX_SIZE = 12                                  # 3 x f32

#: Plausibility bound for a local vertex / delta.  The shipped corpus peaks at
#: ``max|local| = (16000, 23744, 10488)`` (``--audit``), so a value far above
#: that is a typo, not geometry.  ``0x434fed``/``0x437d05`` work in ``f64`` but
#: the file field is ``f32``; 1e7 keeps every value exactly representable
#: enough for the cull maths and leaves 400x headroom.
MAX_COORD = 1.0e7

#: Tolerance for the world-frame shift proof.  A local vertex is ``f32``, so
#: ``origin + basis * local`` inherits the ``f32`` quantisation of the local
#: value (ulp 0.00098 at ``23744``).  Shipped-style edits (integer coordinates,
#: identity basis) land exactly on 0.
WORLD_TOL = 1.0e-3

IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
#: The same matrix in the flat row-major form used by ``ObjectLoc.basis``.
IDENTITY_FLAT = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


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
    """One polygon on disk: byte span plus the fields this tool reports."""

    index: int                       # global 0-based polygon index
    local: int                       # index inside its object
    off: int
    size: int                        # 124 + 2 * n_idx
    n_idx: int
    indices: tuple[int, ...]
    texture: int
    material: int
    lightmap: int


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

    @property
    def tail_end(self) -> int:
        return self.tail_off + OBJECT_TAIL

    def vertex_span(self, index: int) -> tuple[int, int]:
        return self.verts_off + VERTEX_SIZE * index, VERTEX_SIZE

    def radius_span(self) -> tuple[int, int]:
        return self.tail_off, 4

    def bounds_span(self) -> tuple[int, int]:
        return self.tail_off + 14, 12

    def world_vertices(self) -> tuple[tuple[float, float, float], ...]:
        """``origin + basis * local`` -- the same formula as ``gm/sbw.py``."""
        ox, oy, oz = self.origin
        b = self.basis
        return tuple(
            (ox + b[0] * x + b[3] * y + b[6] * z,
             oy + b[1] * x + b[4] * y + b[7] * z,
             oz + b[2] * x + b[5] * y + b[8] * z)
            for x, y, z in self.vertices)

    def world_delta(self, local_delta) -> tuple[float, float, float]:
        """``basis * local_delta`` -- how a local move appears in the world."""
        b = self.basis
        dx, dy, dz = local_delta
        return (b[0] * dx + b[3] * dy + b[6] * dz,
                b[1] * dx + b[4] * dy + b[7] * dz,
                b[2] * dx + b[5] * dy + b[8] * dz)

    def aabb(self, world: bool = True):
        pts = self.world_vertices() if world else self.vertices
        if not pts:
            return None
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        return tuple(lo), tuple(hi)

    def usage(self) -> list[int]:
        """How many polygons of this object use vertex ``i``."""
        counts = [0] * self.n_verts
        for p in self.polys:
            for i in p.indices:
                if 0 <= i < self.n_verts:
                    counts[i] += 1
        return counts

    def bad_indices(self) -> int:
        """Vertex indices outside ``0..n_verts-1`` (0 in shipped data)."""
        return sum(1 for p in self.polys for i in p.indices
                   if not 0 <= i < self.n_verts)

    def recomputed(self):
        return recompute_meta(self.vertices, self.basis)


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
    """Read one polygon (``124 + 2*n_idx`` bytes) and remember its span.

    Disk order (``gm/sbw.py:326-356``): ``texture, lightmap, material,
    overlay, f7a`` (5 x u16), ``brightness`` (u8), ``n_idx`` (u8), then the
    index list and the 112-byte tail.
    """
    off = c.o
    head = c.take(12, f"obj[{obj_index}] polygon[{local}] head")
    texture, lightmap, material, _overlay, _f7a = struct.unpack_from(
        "<5H", head, 0)
    n_idx = head[11]
    indices = struct.unpack("<%dH" % n_idx, c.take(
        2 * n_idx, f"obj[{obj_index}] polygon[{local}] indices")) if n_idx else ()
    c.take(112, f"obj[{obj_index}] polygon[{local}] tail")
    return PolyLoc(index=index, local=local, off=off, size=124 + 2 * n_idx,
                   n_idx=n_idx, indices=tuple(indices), texture=texture,
                   material=material, lightmap=lightmap)


# ---------------------------------------------------------------------------
# metadata recompute (radius / bounds)
# ---------------------------------------------------------------------------

def _f32(v: float) -> float:
    """The value as stored in an ``f32`` field (bit-exact round-trip)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def recompute_meta(vertices, basis=IDENTITY_FLAT) -> tuple[float, tuple[float, float, float]]:
    """``(radius, bounds)`` from the actual vertices.

    The engine recompute ``0x43529f`` takes the vertex **minus the object
    origin** after applying the basis and then ``FABS`` (``docs/WORLD_FIELDS.md``
    §2.3), i.e. it works on ``basis * local``.  So::

        bounds[k] = max over vertices of |(basis * local)[k]|
        radius    = max over vertices of |basis * local|

    ``bounds`` is the AABB half-extent around the object *origin* (not
    ``(max-min)/2``) and ``radius`` is the distance to the farthest vertex,
    which is what cull ``0x437d8c``/``0x437d05`` and the sector sphere
    ``0x4360f1`` use.  With the shipped identity basis this is
    ``max |v[k]|`` / ``max |v|``.

    Both are rounded to ``f32`` (the file fields).  This rule reproduces the
    shipped values bit-exactly for **28 192/28 192** objects (``--audit``).
    """
    if not vertices:
        return 0.0, (0.0, 0.0, 0.0)
    b = basis
    mx0 = mx1 = mx2 = 0.0
    best = 0.0
    for x, y, z in vertices:
        wx = b[0] * x + b[3] * y + b[6] * z
        wy = b[1] * x + b[4] * y + b[7] * z
        wz = b[2] * x + b[5] * y + b[8] * z
        if abs(wx) > mx0:
            mx0 = abs(wx)
        if abs(wy) > mx1:
            mx1 = abs(wy)
        if abs(wz) > mx2:
            mx2 = abs(wz)
        d = math.sqrt(wx * wx + wy * wy + wz * wz)
        if d > best:
            best = d
    return _f32(best), (_f32(mx0), _f32(mx1), _f32(mx2))


# ---------------------------------------------------------------------------
# value parsing / validation
# ---------------------------------------------------------------------------

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


def validate_vec(value, what: str) -> None:
    if len(value) != 3:
        raise EditError(f"{what} needs three numbers")
    for v in value:
        if not math.isfinite(v):
            raise EditError(f"{what}: non-finite component {v!r}")
        if abs(v) > MAX_COORD:
            raise EditError(f"{what}: |{v!r}| is implausible for .sbw local "
                            f"geometry (shipped max |local| = 23744, "
                            f"accepted up to {MAX_COORD:g})")


def validate_delta(value) -> None:
    validate_vec(value, "delta")
    # A delta moves every vertex; it may be larger than a single coordinate
    # but not unbounded (the same f32/cull argument).
    for v in value:
        if abs(v) > MAX_COORD:
            raise EditError(f"delta: |{v!r}| exceeds {MAX_COORD:g}")


# ---------------------------------------------------------------------------
# patching (in place, fixed-size fields)
# ---------------------------------------------------------------------------

def _require_object(scan: Scan, index: int | None) -> ObjectLoc:
    if index is None:
        raise EditError("--object N is required")
    if not 0 <= index < scan.n_objects:
        raise EditError(f"object {index} does not exist "
                        f"(file has {scan.n_objects}, 0-based)")
    return scan.objects[index]


def _require_vertex(obj: ObjectLoc, index: int | None) -> int:
    if index is None:
        raise EditError(f"--index I is required (object {obj.index} has "
                        f"{obj.n_verts} vertices, 0-based)")
    if not 0 <= index < obj.n_verts:
        raise EditError(f"vertex {index} does not exist in object {obj.index} "
                        f"(it has {obj.n_verts} vertices, 0-based)")
    return index


def _write_meta(out: bytearray, obj: ObjectLoc, vertices) -> tuple[float, tuple]:
    radius, bounds = recompute_meta(vertices, obj.basis)
    roff, _ = obj.radius_span()
    boff, _ = obj.bounds_span()
    struct.pack_into("<f", out, roff, radius)
    struct.pack_into("<3f", out, boff, *bounds)
    return radius, bounds


def patch_vertex(data: bytes, obj: ObjectLoc, index: int,
                 pos) -> tuple[bytes, float, tuple]:
    """Move one local vertex and rewrite ``radius``/``bounds`` from vertices."""
    _require_vertex(obj, index)
    out = bytearray(data)
    off, _ = obj.vertex_span(index)
    struct.pack_into("<3f", out, off, *pos)
    verts = list(obj.vertices)
    verts[index] = tuple(_f32(c) for c in pos)
    radius, bounds = _write_meta(out, obj, verts)
    return bytes(out), radius, bounds


def patch_translate(data: bytes, obj: ObjectLoc,
                    delta) -> tuple[bytes, float, tuple]:
    """Move **every** local vertex of the object; the header (origin) stays."""
    if not obj.vertices:
        raise EditError(f"object {obj.index} has no vertices to translate")
    out = bytearray(data)
    verts = []
    for k, (x, y, z) in enumerate(obj.vertices):
        v = (_f32(x + delta[0]), _f32(y + delta[1]), _f32(z + delta[2]))
        verts.append(v)
        off, _ = obj.vertex_span(k)
        struct.pack_into("<3f", out, off, *v)
    radius, bounds = _write_meta(out, obj, verts)
    return bytes(out), radius, bounds


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


def _allowed_spans(obj: ObjectLoc, kind: str, index: int | None):
    """Byte spans that a legal edit may touch (vertices + radius + bounds)."""
    spans: list[tuple[str, int, int]] = []
    if kind == "vertex":
        off, width = obj.vertex_span(index)
        spans.append((f"vertex[{index}]", off, width))
    else:
        for k in range(obj.n_verts):
            off, width = obj.vertex_span(k)
            spans.append((f"vertex[{k}]", off, width))
    roff, rwidth = obj.radius_span()
    boff, bwidth = obj.bounds_span()
    spans.append(("radius", roff, rwidth))
    spans.append(("bounds", boff, bwidth))
    return spans


def _span_of(spans, off: int) -> str | None:
    for name, a, width in spans:
        if a <= off < a + width:
            return name
    return None


def _fmt_vec(v) -> str:
    def one(x: float) -> str:
        if math.isfinite(x) and x == int(x) and abs(x) < 1e15:
            return str(int(x))
        return f"{x:.6g}"
    return "(" + ", ".join(one(float(c)) for c in v) + ")"


def _json_vec(v):
    return [float(c) for c in v]


def _oracle_object(w, index: int):
    for sect in w.sectors:
        for obj in sect.objects:
            if index == 0:
                return obj
            index -= 1
    raise EditError(f"object {index} does not exist in the oracle view")


def _oracle_vert_count(w) -> int:
    """``Sbw`` has ``n_objects``/``n_polys`` but no vertex count property."""
    return sum(o.n_verts for s in w.sectors for o in s.objects)


def verify_edit(orig: bytes, new: bytes, obj_index: int, kind: str, *,
                index: int | None = None, pos=None, delta=None) -> dict:
    """Prove a vertex edit: the target moved, every other byte is untouched.

    Checks (all mandatory):

    1. both blobs parse with the oracle to exactly EOF;
    2. texture/effect/sector/object/polygon/vertex counts and the object flag
       words are unchanged; the file size is unchanged;
    3. the target object's stage-1 header (``u32`` + 96 bytes) is identical;
    4. all bytes outside the target object are identical;
    5. every other object (header + vertices + tail + polygons) is identical;
    6. inside the target object only vertex spans plus ``radius``/``bounds``
       may differ -- so ``n_verts``/``n_polys``, the polygon records, the
       ``f74``/``f75``/``f78``/``f7c`` words and ``reserved`` are identical;
    7. every vertex carries exactly the expected new local value (``f32``):
       one moved for ``vertex``, all moved by ``delta`` for ``translate``;
    8. ``radius``/``bounds`` equal :func:`recompute_meta` of the new vertices;
    9. both readers see the new vertex values;
    10. the oracle's ``Sbw.to_bytes`` reproduces both blobs;
    11. the world frame moved exactly by ``basis * (new - old)`` for the moved
        vertex/vertices (tolerance ``WORLD_TOL``, 0 on shipped-style data), all
        other vertices stayed put, and no other object moved.
    """
    checks = []
    problems = []

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})
        if not ok:
            problems.append(text)

    if kind not in ("vertex", "translate"):
        raise EditError(f"unknown edit kind: {kind}")
    if kind == "vertex" and pos is None:
        raise EditError("vertex edit needs --pos")
    if kind == "translate" and delta is None:
        raise EditError("translate edit needs --delta")

    try:
        before = oracle.parse(orig)
        after = oracle.parse(new)
        check(before.data_end == len(orig), "oracle: source parses to EOF")
        check(after.data_end == len(new), "oracle: edited file parses to EOF")
        check((len(before.tex), len(before.effects), len(before.sectors),
               before.n_objects, before.n_polys, _oracle_vert_count(before)) ==
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
                "changes": []}

    try:
        s0 = walk(orig)
        s1 = walk(new)
    except ScanError as exc:
        check(False, f"own walker: {exc}")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": []}

    check(s0.data_end == len(orig) and s1.data_end == len(new),
          "own walker: both files to EOF")
    check((s0.n_tex, s0.n_eff, len(s0.sectors), s0.n_objects, s0.n_polys,
           s0.n_verts) ==
          (s1.n_tex, s1.n_eff, len(s1.sectors), s1.n_objects, s1.n_polys,
           s1.n_verts),
          "own walker: counts (objects/vertices/polygons) unchanged")
    check(s0.size == s1.size, "file size unchanged")
    if len(orig) != len(new):
        check(False, f"length differs: {len(orig)} vs {len(new)}")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": []}

    if not 0 <= obj_index < s0.n_objects:
        check(False, f"object {obj_index} does not exist "
                     f"(file has {s0.n_objects}, 0-based)")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": []}

    target0 = s0.objects[obj_index]
    target1 = s1.objects[obj_index]

    check(orig[target0.header_off:target0.header_off + target0.header_size] ==
          new[target1.header_off:target1.header_off + target1.header_size],
          "target object header (u32 flag word + 96-byte transform) unchanged")
    check(all(orig[a:b] == new[a:b]
              for a, b in _outside_spans(s0, target0)),
          "bytes outside the target object are identical")
    check(_other_objects_identical(s0, s1, orig, new, obj_index),
          f"{s0.n_objects - 1} other objects (header + vertices + tail + "
          f"polygons) are identical")

    if kind == "vertex" and not 0 <= (index or 0) < target0.n_verts:
        check(False, f"vertex {index} does not exist in object {obj_index} "
                     f"(it has {target0.n_verts})")
        return {"ok": False, "checks": checks, "problems": problems,
                "changes": []}

    spans = _allowed_spans(target0, kind, index)
    changed = diff_offsets(orig, new)
    legal = [d for d in changed if _span_of(spans, d) is not None]
    check(len(legal) == len(changed),
          f"only vertex/radius/bounds bytes differ ({len(changed)} byte(s) "
          f"changed, all inside {len(spans)} legal spans)")
    if changed:
        check(True, "the file actually changed")
    else:
        check(True, "no-op: the file already carries the requested values")

    # every vertex carries the expected local value
    pos_f32 = None
    delta_f32 = None
    if kind == "vertex":
        pos_f32 = tuple(_f32(c) for c in pos)
        ok_v = target1.vertices[index] == pos_f32
        same = sum(1 for k in range(target0.n_verts)
                   if k != index and target1.vertices[k] != target0.vertices[k])
        check(ok_v, f"own walker sees vertex[{index}] = {_fmt_vec(pos_f32)}")
        check(same == 0, f"{target0.n_verts - 1} other vertices of the target "
                         f"object are unchanged")
        local_deltas = [(0.0, 0.0, 0.0)] * target0.n_verts
        local_deltas[index] = tuple(pos_f32[i] - target0.vertices[index][i]
                                    for i in range(3))
        expect = {index}
    else:
        delta_f32 = tuple(_f32(c) for c in delta)
        # The stored value is f32(old + delta); report the expectation as the
        # f32 difference actually present in the file.
        local_deltas = []
        bad = 0
        for k in range(target0.n_verts):
            exp = tuple(_f32(target0.vertices[k][i] + delta_f32[i])
                        for i in range(3))
            if target1.vertices[k] != exp:
                bad += 1
            local_deltas.append(tuple(target1.vertices[k][i]
                                      - target0.vertices[k][i]
                                      for i in range(3)))
        check(bad == 0, f"all {target0.n_verts} vertices moved by "
                        f"{_fmt_vec(delta_f32)}")
        expect = set(range(target0.n_verts))

    # metadata follows the vertices
    radius_expect, bounds_expect = recompute_meta(target1.vertices,
                                                  target1.basis)
    check(target1.radius == radius_expect,
          f"radius equals max|vertex| = {radius_expect!r}")
    check(tuple(target1.bounds) == bounds_expect,
          f"bounds equal max|component| = {_fmt_vec(bounds_expect)}")
    poly_same = len(target0.polys) == len(target1.polys) and all(
        orig[p0.off:p0.off + p0.size] == new[p1.off:p1.off + p1.size]
        and p0.indices == p1.indices
        for p0, p1 in zip(target0.polys, target1.polys))
    check(poly_same, f"{target0.n_polys} polygon records of the target object "
                     f"are byte-identical")

    # both readers agree on the vertices
    try:
        o_after = _oracle_object(after, obj_index)
        o_before = _oracle_object(before, obj_index)
        check(tuple(tuple(v) for v in o_after.vertices) ==
              tuple(tuple(v) for v in target1.vertices),
              "oracle sees the new vertex values")
        if kind == "vertex":
            check(tuple(o_after.vertices[index]) == pos_f32,
                  f"oracle sees vertex[{index}] = {_fmt_vec(pos_f32)}")
        check(tuple(o_after.bounds) == bounds_expect and
              o_after.radius == radius_expect,
              "oracle sees the recomputed radius/bounds")
    except EditError as exc:
        o_before = o_after = None
        check(False, str(exc))

    check(before.to_bytes() == orig, "oracle round-trip of the source is exact")
    check(after.to_bytes() == new, "oracle round-trip of the edit is exact")

    # world frame: world = origin + basis * local
    others_moved = 0
    for a, b in zip(s0.objects, s1.objects):
        if a.index == obj_index:
            continue
        if a.world_vertices() != b.world_vertices():
            others_moved += 1
    check(others_moved == 0,
          "no other object moved in the world frame "
          "(world = origin + basis * local)")

    w0 = target0.world_vertices()
    w1 = target1.world_vertices()
    worst = 0.0
    worst_v = -1
    for k in range(target0.n_verts):
        exp = target0.world_delta(local_deltas[k])
        for i in range(3):
            dev = abs((w1[k][i] - w0[k][i]) - exp[i])
            if dev > worst:
                worst = dev
                worst_v = k
    span = (target1.world_delta(local_deltas[index]) if kind == "vertex"
            else target1.world_delta(delta_f32))
    n_moved = len(expect)
    check(worst <= WORLD_TOL,
          f"world vertices moved exactly by {_fmt_vec(span)} "
          f"({n_moved} {'vertex' if n_moved == 1 else 'vertices'}, "
          f"max deviation {worst:.3g}"
          f"{f' at vertex {worst_v}' if worst_v >= 0 else ''})")

    meta = {
        "radius_old": float(target0.radius),
        "radius_new": float(target1.radius),
        "bounds_old": _json_vec(target0.bounds),
        "bounds_new": _json_vec(target1.bounds),
        "radius_recomputed_old": float(
            recompute_meta(target0.vertices, target0.basis)[0]),
        "bounds_recomputed_old": _json_vec(
            recompute_meta(target0.vertices, target0.basis)[1]),
        "stored_old_matches_recompute":
            target0.radius == recompute_meta(target0.vertices,
                                             target0.basis)[0] and
            tuple(target0.bounds) == recompute_meta(target0.vertices,
                                                    target0.basis)[1],
    }
    world = {
        "kind": kind,
        "objects_moved": 1 if changed else 0,
        "vertices_moved": n_moved,
        "delta_world": _json_vec(span),
        "max_deviation": worst,
        "vertices": target1.n_verts,
    }
    if kind == "vertex":
        world["vertex"] = index
        world["delta_local"] = _json_vec(local_deltas[index])
        world["old_world"] = _json_vec(w0[index])
        world["new_world"] = _json_vec(w1[index])
    else:
        world["delta_local"] = _json_vec(delta_f32)

    rep = {
        "ok": all(c["ok"] for c in checks),
        "noop": not changed,
        "checks": checks,
        "problems": problems,
        "changes": changed,
        "object": obj_index,
        "kind": kind,
        "world": world,
        "meta": meta,
    }
    if kind == "vertex":
        rep["index"] = index
        rep["old"] = _json_vec(target0.vertices[index])
        rep["new"] = _json_vec(pos_f32)
        rep["old_world"] = _json_vec(w0[index])
        rep["new_world"] = _json_vec(w1[index])
    else:
        rep["delta"] = _json_vec(delta_f32)
        rep["old"] = _json_vec(target0.vertices[0]) if target0.vertices else None
        rep["new"] = _json_vec(target1.vertices[0]) if target1.vertices else None
    return rep


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def list_level(path: str, limit: int = 8, obj_index: int | None = None) -> dict:
    data = read_bytes(path)
    scan = walk(data)
    w = oracle.parse(data)
    if obj_index is None:
        objects = scan.objects
    else:
        objects = [_require_object(scan, obj_index)]
    shown = objects[:limit or len(objects)]
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
        "object_filter": obj_index,
        "object_list": [],
        "sector_list": [
            {"index": s.index, "kind": s.kind, "sub": s.sub,
             "objects": len(s.objects), "vertices": s.n_verts,
             "polygons": s.n_polys}
            for s in (scan.sectors[:limit or len(scan.sectors)])
        ],
    }
    for o in shown:
        usage = o.usage()
        aabb = o.aabb()
        radius_r, bounds_r = o.recomputed()
        vlimit = limit or o.n_verts
        verts = [{
            "index": k,
            "local": _json_vec(o.vertices[k]),
            "world": _json_vec(o.world_vertices()[k]),
            "polys": usage[k],
        } for k in range(min(vlimit, o.n_verts))]
        plimit = limit or o.n_polys if obj_index is not None else 0
        polys = [{
            "index": p.local,
            "indices": list(p.indices),
            "texture": p.texture,
            "material": p.material,
            "lightmap": p.lightmap,
            "offset": p.off,
            "size": p.size,
        } for p in o.polys[:plimit]]
        rep["object_list"].append({
            "index": o.index,
            "sector": o.sector,
            "obj": o.obj,
            "id": o.id,
            "n_verts": o.n_verts,
            "n_polys": o.n_polys,
            "origin": _json_vec(o.origin),
            "basis": [[float(o.basis[r * 3 + c]) for c in range(3)]
                      for r in range(3)],
            "basis_identity": all(o.basis[r * 3 + c] == IDENTITY[r][c]
                                  for r in range(3) for c in range(3)),
            "radius": float(o.radius),
            "bounds": _json_vec(o.bounds),
            "radius_recomputed": float(radius_r),
            "bounds_recomputed": _json_vec(bounds_r),
            "meta_matches_recompute": (
                o.radius == radius_r and tuple(o.bounds) == bounds_r),
            "aabb_min": _json_vec(aabb[0]) if aabb else None,
            "aabb_max": _json_vec(aabb[1]) if aabb else None,
            "header_off": o.header_off,
            "geom_off": o.geom_off,
            "verts_off": o.verts_off,
            "tail_off": o.tail_off,
            "shown_vertices": len(verts),
            "vertex_list": verts,
            "unused_vertices": sum(1 for c in usage if c == 0),
            "max_polys_per_vertex": max(usage) if usage else 0,
            "bad_indices": o.bad_indices(),
            "shown_polygons": len(polys),
            "polygon_list": polys,
        })
    return rep


def print_list(rep: dict, limit: int) -> None:
    tail = f" (showing {rep['shown']})" if rep["object_filter"] is None else ""
    print(f"level: {rep['path']}")
    print(f"header: {rep['bytes']} bytes, oracle parse ends at "
          f"{rep['oracle_parse_end']} "
          f"({'EOF ok' if rep['eof_ok'] else 'MISMATCH'})")
    print(f"tables: textures={rep['textures']} effects={rep['effects']} "
          f"(end {rep['table1_end']}, {rep['table2_end']})")
    print(f"world: sectors={rep['sectors']} objects={rep['objects']} "
          f"vertices={rep['vertices']} polygons={rep['polygons']}")
    if rep["object_filter"] is None:
        print(f"sectors{tail}:")
        for s in rep["sector_list"]:
            print(f"  sector[{s['index']:3d}] kind={s['kind']:2d} "
                  f"sub={s['sub']} objects={s['objects']:3d} "
                  f"verts={s['vertices']:5d} polys={s['polygons']:5d}")
    print(f"objects{tail}:")
    for o in rep["object_list"]:
        print(f"  obj[{o['index']:5d}] sector={o['sector']:3d} "
              f"index={o['obj']:3d} flag=0x{o['id']:08x} "
              f"verts={o['n_verts']:4d} polys={o['n_polys']:4d} "
              f"radius={o['radius']:.6g}")
        print(f"    origin={_fmt_vec(o['origin'])} "
              f"bounds={_fmt_vec(o['bounds'])} "
              f"basis={'identity' if o['basis_identity'] else 'non-identity'}")
        meta = "match" if o["meta_matches_recompute"] else "MISMATCH"
        print(f"    metadata: radius recomputed "
              f"{o['radius_recomputed']:.6g}, bounds recomputed "
              f"{_fmt_vec(o['bounds_recomputed'])} ({meta})")
        if o["aabb_min"]:
            print(f"    aabb=({_fmt_vec(o['aabb_min'])} .. "
                  f"{_fmt_vec(o['aabb_max'])})")
        print(f"    vertices (showing {o['shown_vertices']} of "
              f"{o['n_verts']}, unused={o['unused_vertices']}, "
              f"max polys/vertex={o['max_polys_per_vertex']}, "
              f"bad indices={o['bad_indices']}):")
        for v in o["vertex_list"]:
            print(f"      v[{v['index']:3d}] local={_fmt_vec(v['local'])} "
                  f"world={_fmt_vec(v['world'])} polys={v['polys']}")
        if o["shown_vertices"] < o["n_verts"]:
            print(f"      ... {o['n_verts'] - o['shown_vertices']} more "
                  f"(--limit 0 shows all)")
        if rep["object_filter"] is not None and o["polygon_list"]:
            print(f"    polygons (showing {o['shown_polygons']} of "
                  f"{o['n_polys']}):")
            for p in o["polygon_list"]:
                idx = ", ".join(str(i) for i in p["indices"])
                print(f"      p[{p['index']:3d}] indices=({idx}) "
                      f"texture={p['texture']} material={p['material']} "
                      f"lightmap={p['lightmap']} disk={p['offset']}"
                      f"+{p['size']}")


# ---------------------------------------------------------------------------
# --set-vertex / --translate
# ---------------------------------------------------------------------------

def set_vertex(path: str, obj_index: int, index: int, pos, out_path: str,
               game: str = DEFAULT_GAME) -> dict:
    validate_vec(pos, "pos")
    if not out_path:
        raise EditError("--set-vertex needs --out FILE (the game install is "
                        "read-only)")
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    _require_vertex(obj, index)
    _refuse_game_write(out_path, game)
    new, radius, bounds = patch_vertex(data, obj, index, pos)
    write_bytes(out_path, new)
    rep = verify_edit(data, new, obj_index, "vertex", index=index, pos=pos)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "bytes": len(new),
        "policy": "local vertex moved; origin/basis header untouched",
    })
    if rep["noop"]:
        rep["note"] = ("vertex already had the requested position and the "
                       "metadata already matched: the output is a "
                       "byte-identical copy")
    else:
        m = rep["meta"]
        rep["note"] = (
            f"vertex[{index}] local {_fmt_vec(rep['old'])} -> "
            f"{_fmt_vec(rep['new'])}; radius {m['radius_old']:.6g} -> "
            f"{m['radius_new']:.6g}; bounds "
            f"{_fmt_vec(m['bounds_old'])} -> {_fmt_vec(m['bounds_new'])}")
    return rep


def translate(path: str, obj_index: int, delta, out_path: str,
              game: str = DEFAULT_GAME) -> dict:
    validate_delta(delta)
    if not out_path:
        raise EditError("--translate needs --out FILE (the game install is "
                        "read-only)")
    data = read_bytes(path)
    scan = walk(data)
    obj = _require_object(scan, obj_index)
    _refuse_game_write(out_path, game)
    new, radius, bounds = patch_translate(data, obj, delta)
    write_bytes(out_path, new)
    rep = verify_edit(data, new, obj_index, "translate", delta=delta)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "bytes": len(new),
        "policy": "all local vertices moved; origin/basis header untouched",
    })
    if rep["noop"]:
        rep["note"] = ("delta is zero and the metadata already matched: the "
                       "output is a byte-identical copy")
    else:
        m = rep["meta"]
        rep["note"] = (
            f"{obj.n_verts} vertices moved by {_fmt_vec(delta)}; radius "
            f"{m['radius_old']:.6g} -> {m['radius_new']:.6g}; bounds "
            f"{_fmt_vec(m['bounds_old'])} -> {_fmt_vec(m['bounds_new'])}")
    return rep


def print_edit(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    if rep["kind"] == "vertex":
        print(f"obj[{rep['object']}] vertex[{rep['index']}]: "
              f"{_fmt_vec(rep['old'])} -> {_fmt_vec(rep['new'])} "
              f"(local; world {_fmt_vec(rep['old_world'])} -> "
              f"{_fmt_vec(rep['new_world'])})")
    else:
        print(f"obj[{rep['object']}] translate by "
              f"{_fmt_vec(rep['delta'])} ({rep['world']['vertices_moved']} "
              f"vertices)")
    m = rep["meta"]
    print(f"metadata: radius {m['radius_old']:.9g} -> {m['radius_new']:.9g}, "
          f"bounds {_fmt_vec(m['bounds_old'])} -> "
          f"{_fmt_vec(m['bounds_new'])} (recomputed from the vertices)")
    print(f"{rep['bytes']} bytes, unchanged size")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("note"):
        print(f"  note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def check_file(path: str, against: str | None = None,
               obj_index: int | None = None, index: int | None = None,
               pos=None, delta=None,
               game: str = DEFAULT_GAME) -> dict:
    """Re-read *path* with the oracle and the own walker.

    With ``against`` the two files are compared: when an edit expectation is
    given (``--index``+``--pos`` or ``--delta``) every other byte must be
    identical and the edit must be exactly the expected one; otherwise the
    whole file must be byte-identical.
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
        meta_bad = [o.index for o in scan.objects
                    if o.radius != o.recomputed()[0] or
                    tuple(o.bounds) != o.recomputed()[1]]
        check(not meta_bad, f"radius/bounds equal the recompute rule in every "
                            f"object ({scan.n_objects} objects, "
                            f"{len(meta_bad)} mismatch)")
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
    if pos is not None or delta is not None:
        if obj_index is None:
            raise EditError("--check with an edit needs --object N")
        if pos is not None and delta is not None:
            raise EditError("--check: --pos and --delta are mutually exclusive")
        if pos is not None:
            validate_vec(pos, "pos")
            if index is None:
                raise EditError("--check with --pos needs --index I")
            kind = "vertex"
            rep.update({"object": obj_index, "index": index,
                        "value": {"local": _json_vec(pos)}})
            edit = verify_edit(orig, data, obj_index, kind, index=index,
                               pos=pos)
        else:
            validate_delta(delta)
            kind = "translate"
            rep.update({"object": obj_index, "delta": _json_vec(delta)})
            edit = verify_edit(orig, data, obj_index, kind, delta=delta)
        rep["kind"] = kind
        rep["checks"].extend(edit["checks"])
        rep["problems"].extend(edit["problems"])
        rep["changes"] = edit.get("changes", [])
        rep["world"] = edit.get("world")
        rep["meta"] = edit.get("meta")
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
    if rep.get("kind") == "vertex":
        print(f"target: obj[{rep['object']}] vertex[{rep['index']}]="
              f"{_fmt_vec(rep['value']['local'])}")
    elif rep.get("kind") == "translate":
        print(f"target: obj[{rep['object']}] translated by "
              f"{_fmt_vec(rep['delta'])}")
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
           "bytes": 0, "failures": [], "meta_exact": 0, "meta_mismatch": [],
           "ok": False}
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
        if (scan.n_objects, scan.n_polys, scan.n_verts) != (
                w.n_objects, w.n_polys,
                sum(o.n_verts for s in w.sectors for o in s.objects)):
            problems.append("counts differ from the oracle")
        # every object: own reader == oracle reader, and the metadata equals
        # the recompute rule (bit-exact f32)
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
            for k, poly in enumerate(obj.polys):
                if tuple(poly.indices) != loc.polys[k].indices:
                    problems.append(f"index mismatch at object {loc.index} "
                                    f"polygon {k}")
                    break
            r, b = recompute_meta(loc.vertices, loc.basis)
            if loc.radius == r and tuple(loc.bounds) == b:
                rep["meta_exact"] += 1
            else:
                if len(rep["meta_mismatch"]) < 16:
                    rep["meta_mismatch"].append(
                        f"{name} obj {loc.index}: radius {loc.radius} vs {r}, "
                        f"bounds {tuple(loc.bounds)} vs {b}")
        if problems:
            rep["mismatched"].append({"file": name, "problems": problems})
            if verbose:
                print(f"  FAIL {name}: {'; '.join(problems)}")
        else:
            rep["identical"] += 1
            if verbose:
                print(f"  ok   {name:14s} {len(data):8d} bytes "
                      f"{scan.n_objects:5d} objects {scan.n_verts:6d} verts "
                      f"{scan.n_polys:6d} polys")
    rep["ok"] = (rep["identical"] == rep["files"] and rep["files"] > 0
                 and not rep["failures"] and not rep["meta_mismatch"])
    return rep


def print_roundtrip(rep: dict) -> None:
    print(f"round-trip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['objects']} objects, {rep['vertices']} vertices, "
          f"{rep['polygons']} polygons, {rep['bytes']} bytes ({rep['game']})")
    print(f"metadata: radius/bounds equal the recompute rule in "
          f"{rep['meta_exact']}/{rep['objects']} objects")
    for m in rep["mismatched"]:
        print(f"  MISMATCH {m['file']}: {'; '.join(m['problems'])}")
    for f in rep["failures"]:
        print(f"  FAILURE {f}")
    for m in rep["meta_mismatch"]:
        print(f"  META {m}")


def audit_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    """Measure the recompute rule and vertex usage on the whole corpus.

    Evidence behind :func:`recompute_meta` and the vertex policies: that the
    shipped ``radius``/``bounds`` are exactly the recompute rule, the local
    coordinate range, and that no shipped vertex is unreferenced.
    """
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "objects": 0,
           "vertices": 0, "polygons": 0, "vertices_referenced": 0,
           "unused_vertices": 0, "max_polys_per_vertex": 0,
           "zero_vertex_objects": 0, "bad_indices": 0,
           "meta_exact": 0, "meta_mismatch": [], "max_coord": [0.0] * 3,
           "max_radius": 0.0, "non_identity_basis": 0, "ok": False}
    for path in files:
        name = os.path.basename(path)
        data = read_bytes(path)
        scan = walk(data)
        rep["objects"] += scan.n_objects
        rep["vertices"] += scan.n_verts
        rep["polygons"] += scan.n_polys
        for o in scan.objects:
            if not o.vertices:
                rep["zero_vertex_objects"] += 1
            for v in o.vertices:
                for i in range(3):
                    rep["max_coord"][i] = max(rep["max_coord"][i], abs(v[i]))
            usage = o.usage()
            used = sum(1 for c in usage if c)
            rep["vertices_referenced"] += used
            rep["unused_vertices"] += o.n_verts - used
            rep["max_polys_per_vertex"] = max(
                rep["max_polys_per_vertex"], max(usage) if usage else 0)
            rep["bad_indices"] += o.bad_indices()
            rep["max_radius"] = max(rep["max_radius"], o.radius)
            if any(o.basis[r * 3 + c] != IDENTITY[r][c]
                   for r in range(3) for c in range(3)):
                rep["non_identity_basis"] += 1
            r, b = recompute_meta(o.vertices, o.basis)
            if o.radius == r and tuple(o.bounds) == b:
                rep["meta_exact"] += 1
            else:
                if len(rep["meta_mismatch"]) < 32:
                    rep["meta_mismatch"].append(
                        f"{name} obj {o.index}: radius {o.radius} vs {r}, "
                        f"bounds {tuple(o.bounds)} vs {b}")
                if verbose:
                    print(f"  META {name} obj {o.index}: {o.radius} vs {r}")
    rep["ok"] = (rep["objects"] > 0 and not rep["meta_mismatch"]
                 and rep["bad_indices"] == 0)
    return rep


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['objects']} objects, "
          f"{rep['vertices']} vertices, {rep['polygons']} polygons "
          f"({rep['game']})")
    print(f"  local coords max |x|,|y|,|z| = "
          f"{rep['max_coord'][0]:g}, {rep['max_coord'][1]:g}, "
          f"{rep['max_coord'][2]:g}")
    print(f"  radius max = {rep['max_radius']:.6g}; "
          f"basis non-identity = {rep['non_identity_basis']}")
    print(f"  vertices referenced = {rep['vertices_referenced']}/"
          f"{rep['vertices']} (unused {rep['unused_vertices']}), "
          f"max polys/vertex = {rep['max_polys_per_vertex']}, "
          f"bad indices = {rep['bad_indices']}, "
          f"zero-vertex objects = {rep['zero_vertex_objects']}")
    print(f"  radius/bounds == recompute rule: {rep['meta_exact']}/"
          f"{rep['objects']}")
    for m in rep["meta_mismatch"]:
        print(f"  MISMATCH {m}")
    print("result: " + ("OK -- the recompute rule reproduces every shipped "
                        "object bit-exactly"
                        if rep["ok"] else "FAILED"))


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


def _synth_geom(vertices, polys: list[bytes], f75=0, f74=0, f7c=0, f78=0,
                reserved=bytes(24)) -> bytes:
    """Stage-2 record with ``radius``/``bounds`` from :func:`recompute_meta`."""
    radius, bounds = recompute_meta(vertices)
    out = bytearray(struct.pack("<HH", len(vertices), len(polys)))
    for v in vertices:
        out += struct.pack("<3f", *v)
    out += struct.pack("<f", radius) + bytes((f75, f74))
    out += struct.pack("<II", f7c, f78)
    out += struct.pack("<3f", *bounds)
    out += reserved
    assert len(out) == 4 + VERTEX_SIZE * len(vertices) + OBJECT_TAIL
    for p in polys:
        out += p
    return bytes(out)


_SYNTH_OBJECTS = [
    # (flag word, origin, vertices, polygons)
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

    layout = [
        (0, 0, [0, 1]),
        (6, 1, [2, 3]),
    ]
    out += struct.pack("<I", len(layout))
    for kind, sub, objs in layout:
        out += struct.pack("<IHB", len(objs), kind, sub)
        for i in objs:
            flag, origin, _v, _p = _SYNTH_OBJECTS[i]
            out += struct.pack("<I", flag) + _synth_header(origin)
    for _kind, _sub, objs in layout:
        for i in objs:
            _flag, _origin, verts, polys = _SYNTH_OBJECTS[i]
            recs = [_synth_poly(indices=idx, texture=tex_i, lightmap=lm,
                                material=mat,
                                flags=0x8000)
                    for idx, tex_i, lm, mat in polys]
            out += _synth_geom(verts, recs,
                               f75=1 if i == 1 else 0,
                               f78=3 if i == 2 else 0)
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
        tuple(obj.points[0]) == tuple(loc.origin)
        and tuple(tuple(r) for r in obj.points[1:]) ==
        tuple(tuple(loc.basis[r * 3:r * 3 + 3]) for r in range(3))
        and (obj.radius, tuple(obj.bounds), obj.f74, obj.f75, obj.f78, obj.f7c,
             len(obj.vertices), len(obj.polys)) ==
            (loc.radius, tuple(loc.bounds), loc.f74, loc.f75, loc.f78, loc.f7c,
             loc.n_verts, loc.n_polys)
        and tuple(tuple(v) for v in obj.vertices) ==
        tuple(tuple(v) for v in loc.vertices)
        for loc, obj in zip(scan.objects, flat))
    check("synth: every object field and vertex matches the oracle", fields_ok)

    # 2: the byte layout itself
    o0 = scan.objects[0]
    check("layout: header is u32 flag + 96 bytes",
          o0.header_size == 100 and o0.geom_off >= o0.header_off + 100)
    check("layout: n_verts/n_polys at geom_off",
          struct.unpack_from("<HH", data, o0.geom_off) == (4, 2))
    check("layout: vertices at geom_off+4, 12 bytes each",
          o0.verts_off == o0.geom_off + 4
          and struct.unpack_from("<3f", data, o0.verts_off) == (2.0, 3.0, 4.0)
          and o0.vertex_span(3) == (o0.verts_off + 36, 12))
    check("layout: tail starts after the vertices",
          o0.tail_off == o0.verts_off + 12 * o0.n_verts)
    r, b = recompute_meta(o0.vertices)
    check("layout: radius at tail+0x00",
          struct.unpack_from("<f", data, o0.tail_off)[0] == r)
    check("layout: f75/f74 at tail+0x04/+0x05, f7c/f78 at +0x06/+0x0A",
          data[o0.tail_off + 4] == 0 and data[o0.tail_off + 5] == 0
          and struct.unpack_from("<II", data, o0.tail_off + 6) == (0, 0))
    check("layout: bounds at tail+0x0E",
          struct.unpack_from("<3f", data, o0.tail_off + 14) == b)
    check("layout: reserved 24 zero bytes at tail+0x1A",
          data[o0.tail_off + 26:o0.tail_off + 50] == bytes(24))
    check("layout: record sizes", all(
        o.polys_end - o.geom_off == 4 + VERTEX_SIZE * o.n_verts + OBJECT_TAIL
        + sum(124 + 2 * p.n_idx for p in o.polys) for o in scan.objects))

    # 3: world = origin + basis * local and the recompute rule
    o1 = scan.objects[1]
    check("world: origin (10,0,0) translates the object",
          o1.world_vertices()[1] == (10.0, 1.0, 0.0),
          repr(o1.world_vertices()[1]))
    check("meta: recompute is exact on the synthetic data",
          all(o.radius == o.recomputed()[0]
              and tuple(o.bounds) == o.recomputed()[1] for o in scan.objects))
    rot = (0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    r_rot, b_rot = recompute_meta([(3.0, 4.0, 0.0)], rot)
    check("meta: a non-identity basis is applied like 0x43529f",
          r_rot == 5.0 and b_rot == (4.0, 3.0, 0.0),
          f"{r_rot}, {b_rot}")
    check("usage: object 0 vertices are used 2/2/2/2x",
          o0.usage() == [2, 2, 2, 2], repr(o0.usage()))
    check("usage: object 1 has one unreferenced vertex",
          o1.usage() == [1, 1, 1, 0], repr(o1.usage()))

    # 4: a vertex edit passes every check and moves only the world point
    try:
        new, r, b = patch_vertex(data, o0, 0, (2.0, 3.0, 8.0))
        rep = verify_edit(data, new, 0, "vertex", index=0,
                          pos=(2.0, 3.0, 8.0))
        check("edit: vertex 0 -> (2,3,8)", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("edit: radius recomputed", r == rep["meta"]["radius_new"])
        check("edit: world vertex moved by +4 on Z",
              rep["world"]["delta_world"] == [0.0, 0.0, 4.0],
              repr(rep["world"]["delta_world"]))
        check("edit: bounds Z grew to 8",
              rep["meta"]["bounds_new"] == [2.0, 3.0, 8.0],
              repr(rep["meta"]["bounds_new"]))
    except Exception as exc:                           # noqa: BLE001
        check("edit: vertex 0 -> (2,3,8)", False, str(exc))

    # 5: moving an interior vertex leaves radius/bounds alone
    try:
        new, r, b = patch_vertex(data, o0, 3, (-1.0, 0.0, 0.0))
        rep = verify_edit(data, new, 0, "vertex", index=3,
                          pos=(-1.0, 0.0, 0.0))
        check("edit: interior vertex keeps radius/bounds",
              rep["ok"] and rep["meta"]["radius_old"] ==
              rep["meta"]["radius_new"]
              and rep["meta"]["bounds_old"] == rep["meta"]["bounds_new"],
              "; ".join(rep.get("problems", [])))
    except Exception as exc:                           # noqa: BLE001
        check("edit: interior vertex keeps radius/bounds", False, str(exc))

    # 6: translate moves every vertex and recomputes the metadata
    try:
        new, r, b = patch_translate(data, o0, (0.0, 0.0, 16.0))
        rep = verify_edit(data, new, 0, "translate", delta=(0.0, 0.0, 16.0))
        check("edit: translate (0,0,16)", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("edit: translate moved all 4 vertices",
              rep["world"]["vertices_moved"] == 4)
        check("edit: translate keeps the header (origin) unchanged",
              struct.unpack_from("<3d", new, o0.header_off + 4) ==
              struct.unpack_from("<3d", data, o0.header_off + 4))
        check("edit: translate moved bounds Z from 4 to 20",
              rep["meta"]["bounds_new"] == [2.0, 3.0, 20.0],
              repr(rep["meta"]["bounds_new"]))
        check("edit: translate delta world is (0,0,16)",
              rep["world"]["delta_world"] == [0.0, 0.0, 16.0],
              repr(rep["world"]["delta_world"]))
    except Exception as exc:                           # noqa: BLE001
        check("edit: translate (0,0,16)", False, str(exc))

    # 7: a no-op is written as a byte-identical copy and reported
    try:
        new, _r, _b = patch_vertex(data, o0, 0, o0.vertices[0])
        rep = verify_edit(data, new, 0, "vertex", index=0, pos=o0.vertices[0])
        check("edit: same position is a verified no-op",
              rep["ok"] and rep["noop"] and new == data,
              "; ".join(rep.get("problems", [])))
        new2, _r, _b = patch_translate(data, o0, (0.0, 0.0, 0.0))
        check("edit: zero translate is a byte-identical no-op", new2 == data)
    except Exception as exc:                           # noqa: BLE001
        check("edit: no-op", False, str(exc))

    # 8: refusals (index / shape / range / path policy)
    expect_refused("refuse: vertex index == n_verts",
                   lambda: _require_vertex(o0, 4))
    expect_refused("refuse: negative vertex index",
                   lambda: _require_vertex(o0, -1))
    expect_refused("refuse: missing vertex index",
                   lambda: _require_vertex(o0, None))
    expect_refused("refuse: pos with two numbers",
                   lambda: validate_vec(parse_vec("1,2", 3, "pos"), "pos"))
    expect_refused("refuse: pos with four numbers",
                   lambda: validate_vec(parse_vec("1,2,3,4", 3, "pos"), "pos"))
    expect_refused("refuse: pos nan",
                   lambda: validate_vec(parse_vec("nan,0,0", 3, "pos"), "pos"))
    expect_refused("refuse: pos inf",
                   lambda: validate_vec(parse_vec("inf,0,0", 3, "pos"), "pos"))
    expect_refused("refuse: pos not a number",
                   lambda: validate_vec(parse_vec("1,2,x", 3, "pos"), "pos"))
    expect_refused("refuse: pos implausible",
                   lambda: validate_vec((1e8, 0.0, 0.0), "pos"))
    expect_refused("refuse: delta implausible",
                   lambda: validate_delta((0.0, 0.0, -1e8)))
    expect_refused("refuse: object out of range",
                   lambda: _require_object(scan, 4))
    expect_refused("refuse: negative object index",
                   lambda: _require_object(scan, -1))
    expect_refused("refuse: translate an object without vertices",
                   lambda: patch_translate(data, scan.objects[3],
                                           (1.0, 0.0, 0.0)))
    expect_refused("refuse: unknown edit kind",
                   lambda: verify_edit(data, data, 0, "scale"))
    expect_refused("refuse: vertex edit without pos",
                   lambda: verify_edit(data, data, 0, "vertex", index=0))

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

    # 10: verification is not vacuous
    new, _r, _b = patch_vertex(data, o0, 0, (2.0, 3.0, 8.0))
    tampered = bytearray(new)
    tampered[o0.vertex_span(1)[0]] ^= 0xFF               # another vertex
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a change in another vertex", not rep["ok"])

    tampered = bytearray(new)
    tampered[o0.vertex_span(0)[0] + 1] ^= 0x01           # wrong target value
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a wrong target value", not rep["ok"])

    tampered = bytearray(new)
    tampered[o0.polys[0].off + 12] ^= 0x01               # polygon index list
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a polygon change", not rep["ok"])

    tampered = bytearray(new)
    tampered[o0.tail_off] ^= 0x01                        # radius
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a stale radius", not rep["ok"])

    tampered = bytearray(new)
    tampered[o0.tail_off + 4] ^= 0x01                    # f75
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a tail word change", not rep["ok"])

    tampered = bytearray(new)
    other = scan.objects[1]
    tampered[other.verts_off] ^= 0xFF                    # another object
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a change in another object", not rep["ok"])

    tampered = bytearray(new)
    struct.pack_into("<d", tampered, o0.header_off + 4, 99.0)   # origin
    rep = verify_edit(data, bytes(tampered), 0, "vertex", index=0,
                      pos=(2.0, 3.0, 8.0))
    check("verify: detects a header change", not rep["ok"])

    # 11: the whole --set-vertex/--translate -> --check path on files
    import tempfile
    tmpdir = None
    try:
        base = DEFAULT_OUT_DIR if os.path.isdir(DEFAULT_OUT_DIR) else None
        tmpdir = tempfile.mkdtemp(prefix="sbwmesh-", dir=base)
        src = os.path.join(tmpdir, "synthetic.sbw")
        vtx = os.path.join(tmpdir, "synthetic_vertex.sbw")
        tr = os.path.join(tmpdir, "synthetic_translate.sbw")
        write_bytes(src, data)
        rep = set_vertex(src, 0, 0, (2.0, 3.0, 8.0), vtx)
        check("set-vertex: writes and verifies", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("set-vertex: oracle reads the file",
              oracle.parse_file(vtx).n_objects == 4)
        chk = check_file(vtx, src, 0, 0, (2.0, 3.0, 8.0), None, game=tmpdir)
        check("check: accepts the vertex edit", chk["ok"],
              "; ".join(chk.get("problems", [])))
        chk2 = check_file(vtx, src, 0, 0, (2.0, 3.0, 9.0), None, game=tmpdir)
        check("check: rejects a wrong vertex expectation", not chk2["ok"])
        chk3 = check_file(vtx, src, game=tmpdir)
        check("check: full identity rejects the edit", not chk3["ok"])

        rep2 = translate(src, 0, (0.0, 0.0, 16.0), tr)
        check("translate: writes and verifies", rep2["ok"],
              "; ".join(rep2.get("problems", [])))
        chk4 = check_file(tr, src, 0, None, None, (0.0, 0.0, 16.0),
                          game=tmpdir)
        check("check: accepts the translate", chk4["ok"],
              "; ".join(chk4.get("problems", [])))
        chk5 = check_file(tr, src, 0, None, None, (0.0, 0.0, 8.0),
                          game=tmpdir)
        check("check: rejects a wrong delta", not chk5["ok"])
        chk6 = check_file(tr, src, 0, 0, (2.0, 3.0, 8.0), None, game=tmpdir)
        check("check: rejects a vertex claim on a translate", not chk6["ok"])
        chk7 = check_file(src, src, game=tmpdir)
        check("check: identical file passes", chk7["ok"],
              "; ".join(chk7.get("problems", [])))

        expect_refused(
            "set-vertex: refuses without --out",
            lambda: set_vertex(src, 0, 0, (1.0, 2.0, 3.0), ""))
        expect_refused(
            "translate: refuses without --out",
            lambda: translate(src, 0, (1.0, 0.0, 0.0), ""))
        expect_refused(
            "set-vertex: refuses an index outside the object",
            lambda: set_vertex(src, 0, 99, (1.0, 2.0, 3.0), vtx))
        expect_refused(
            "set-vertex: refuses to write into the game install",
            lambda: set_vertex(src, 0, 0, (1.0, 2.0, 3.0), vtx, game=tmpdir))
        expect_refused(
            "translate: refuses to write into the game install",
            lambda: translate(src, 0, (1.0, 0.0, 0.0), tr, game=tmpdir))
    except Exception as exc:                           # noqa: BLE001
        check("set-vertex/translate: temporary-file cycle", False, str(exc))
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

    # 12: --list shape on the synthetic file
    try:
        lrep = list_level(_write_temp(data))
        o = lrep["object_list"][0]
        check("list: vertex rows carry local/world/usage",
              o["vertex_list"][0]["local"] == [2.0, 3.0, 4.0]
              and o["vertex_list"][0]["world"] == [2.0, 3.0, 4.0]
              and o["vertex_list"][0]["polys"] == 2,
              repr(o["vertex_list"][0]))
        check("list: metadata matches the recompute rule",
              o["meta_matches_recompute"] is True)
        check("list: --object filter", list_level(
            _write_temp(data), obj_index=2)["object_list"][0]["index"] == 2)
    except Exception as exc:                           # noqa: BLE001
        check("list: synthetic", False, str(exc))

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    for name, ok, detail in results:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}"
              + (f" -- {detail}" if detail and not ok else ""))
    print(f"self-test: {passed}/{total} passed")
    return passed == total


_TEMP_FILES: list[str] = []


def _write_temp(data: bytes) -> str:
    """A throw-away file for the self-test (outside the game install)."""
    import tempfile
    base = DEFAULT_OUT_DIR if os.path.isdir(DEFAULT_OUT_DIR) else None
    fd, path = tempfile.mkstemp(prefix="sbwmesh-", suffix=".sbw", dir=base)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    _TEMP_FILES.append(path)
    return path


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _cmd_list(args) -> int:
    path = resolve_level(args.list, args.game)
    limit = max(args.limit, 0)
    rep = list_level(path, limit, args.object)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_list(rep, limit)
    return 0 if rep["eof_ok"] else 1


def _cmd_set_vertex(args) -> int:
    if args.object is None:
        raise EditError("--set-vertex needs --object N")
    if args.index is None:
        raise EditError("--set-vertex needs --index I")
    if args.pos is None:
        raise EditError("--set-vertex needs --pos x,y,z")
    if not args.out:
        raise EditError("--set-vertex needs --out FILE")
    pos = parse_vec(args.pos, 3, "pos")
    path = resolve_level(args.set_vertex, args.game)
    rep = set_vertex(path, args.object, args.index, pos, args.out, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep)
    return 0 if rep["ok"] else 1


def _cmd_translate(args) -> int:
    if args.object is None:
        raise EditError("--translate needs --object N")
    if args.delta is None:
        raise EditError("--translate needs --delta dx,dy,dz")
    if not args.out:
        raise EditError("--translate needs --out FILE")
    delta = parse_vec(args.delta, 3, "delta")
    path = resolve_level(args.translate, args.game)
    rep = translate(path, args.object, delta, args.out, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    pos = parse_vec(args.pos, 3, "pos") if args.pos else None
    delta = parse_vec(args.delta, 3, "delta") if args.delta else None
    rep = check_file(args.check, args.against, args.object, args.index, pos,
                     delta, args.game)
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
        elif head in ("set-vertex", "--set-vertex") and len(argv) > 1:
            argv = ["--set-vertex", argv[1]] + argv[2:]
        elif head in ("translate", "--translate") and len(argv) > 1:
            argv = ["--translate", argv[1]] + argv[2:]
    ap = argparse.ArgumentParser(
        description="Edit .sbw world vertices (local positions) of a "
                    "Gunmetal level, recomputing radius/bounds (M11.2, "
                    "slice 4).")
    ap.add_argument("--list", metavar="LEVEL",
                    help="list objects and their vertices (local/world/"
                         "polygon usage)")
    ap.add_argument("--set-vertex", metavar="LEVEL",
                    help="move one local vertex (needs --object/--index/--pos/"
                         "--out)")
    ap.add_argument("--translate", metavar="LEVEL",
                    help="move every local vertex of one object "
                         "(needs --object/--delta/--out)")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE (oracle + own walker) and verify")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source level")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every shipped .sbw")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests (no game files)")
    ap.add_argument("--audit", action="store_true",
                    help="measure the radius/bounds recompute rule and vertex "
                         "usage on the corpus")
    ap.add_argument("--object", type=int, metavar="N",
                    help="object index (0-based, order of --list)")
    ap.add_argument("--index", type=int, metavar="I",
                    help="vertex index inside the object (0-based)")
    ap.add_argument("--pos", metavar="x,y,z", help="new LOCAL vertex position")
    ap.add_argument("--delta", metavar="dx,dy,dz",
                    help="local translation applied to every vertex")
    ap.add_argument("--out", metavar="PATH", help="output .sbw")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=8,
                    help="max objects and max vertices/polygons shown per "
                         "block (0 = all)")
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
        if args.set_vertex:
            return _cmd_set_vertex(args)
        if args.translate:
            return _cmd_translate(args)
        ap.print_help()
        return 2
    except EditError as exc:
        print(f"sbwmesh: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"sbwmesh: corrupt .sbw: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"sbwmesh: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"sbwmesh: internal error: {type(exc).__name__}: {exc}",
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
