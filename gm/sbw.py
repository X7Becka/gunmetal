"""Gunmetal ".sbw" level files (world geometry).

Fully reversed and verified byte-exact (exact EOF) on all 36 shipped ``.sbw``
files.  A file is a single sequential stream read by the loader at VMA
``0x42aa63`` (tables 1/2), ``0x422f03``/``0x422d55`` (allocators), the stage-1
loop at ``0x42ad65`` and the stage-2 loop ``0x425f4c`` -> ``0x422b88`` /
``0x422a51``:

    u32 count1;                                     // texture-name table
    count1 * {
        char name[];            // NUL-terminated ASCII
        u16 flag;               // 0/1/256 in shipped data
        u32 value;              // per-level texture weight: the loader seeds the
                                // engine refcount array 0x5310c0[i] with it
                                // (0x42aba9), 0x424eab/0x424e50 then inc/dec it.
                                // NOT an index into PICS/SBM.IDX -- the same name
                                // carries different values in different levels
    };

    u32 count2;                                     // effect/animation table
    count2 * {
        char name[];            // NUL-terminated ASCII (animation name)
        u32 kind;               // animation class
        u32 start;              // first sample/frame id
        u32 n;                  // number of ids
        u16 ids[n];             // sample / frame ids, played in order
    };

    u32 count3;                                     // sectors
    count3 * {                                      // stage 1 (transforms)
        u32 objects;            // objects in this sector
        u16 kind;               // sector type * 6 (SET_SECTOR_TYPE; getter
                                // 0x42d9a0 divides by 6); render never reads it
        u8  sub;                // sector sub-flag
        objects * {
            u32 id;             // object id
            u8 header[96];      // 4 points x 3 components; each component is
                                // stored as (u32 pad=0, f32 value): origin +
                                // basis, always exactly diag(1.875)
        }
    }

    count3 * objects * {                            // stage 2 (geometry)
        u16 n_verts;
        u16 n_polys;
        f32 verts[n_verts][3];  // object-local positions
        f32 radius;             // 0x60: |bounds| (object bounding-sphere radius)
        u8  f75;                // 0x75: 0 (1 in 7 shipped objects)
        u8  f74;                // 0x74: 0
        u32 f7c;                // 0x7c: 0
        u32 f78;                // 0x78: 0
        f32 bounds[3];          // 0x84: AABB half-extents (integers in data)
        u8  reserved[24];       // 0x90: 24 zero bytes (6 floats)
        n_polys * { /* see Poly */ }
    }

Polygon records
---------------
Each polygon is a ``0x80``-byte in-memory record (``resolve_poly`` at
``0x434463`` returns ``object+0x6c + i*0x80``); the leading/other fields are
read from disk in a different order, so ``parse`` assigns them by engine
offset.  The six ``f32`` triples at ``0x08``/``0x14``/``0x20``/``0x2c``/
``0x38``/``0x44`` are the polygon's cached render frame; the engine validates
that ``0x14`` (normal), ``0x20`` (texture U axis) and ``0x2c`` (texture V
axis) are unit length (``Invalid poly [U|V] vector`` messages), and the two
corners at ``0x38``/``0x44`` plus the centre at ``0x08`` come from the polygon
geometry (they coincide with the vertices for axis-aligned faces).

Engine offsets -> meaning (evidence in parentheses):

    0x00 u16 texture   texture index (signed): >0 = 1-based entry of the
                       texture-name table, <0 = effect table (stride 0x5c),
                       0 = untextured (SET_POLY_TEXTURE, id 0x0f)
    0x02 u16 material  material index 0..5 = none/reflective/translucent/
                       varadd/add/shade; indexes the table at ds:0x5670e0
                       (stride 0x40, name at +0x34)
    0x04 u16 lightmap  lightmap index (signed): 0 = none, >0 = lightmap slot;
                       rebased by the level's lightmap base at load time
    0x06 u16 u_scale   derived: uv_u_fp >> 7 (text loader 0x425db4; holds for
                       235057/235069 shipped polys)
    0x08 3f  centre    object-local polygon centre
    0x14 3f  normal    face normal (unit)
    0x20 3f  u_axis    texture U axis (unit)
    0x2c 3f  v_axis    texture V axis (unit)
    0x38 3f  corner_a  reference corner (lightmap-plane projected)
    0x44 3f  corner_b  reference corner
    0x50 u16 uv_u      texture coordinate; the text loader's auto UV adds
                       dot(u_axis, v-centre)*256 per unit (0x4a2258 = 256.0)
    0x52 u16 uv_v      same for V
    0x54 u32 uv_u_fp   signed int offset: texel = ftol((dot(u_axis,
                       v-centre)*64 + 0x54)/2) >> 10 (consts 0x4a2298 = 64.0,
                       0x4a2280 = 2.0) -- the lightmap sampling formula
    0x58 u32 uv_v_fp   same for V
    0x5c u16 scroll_u  U scroll weight: helper 0x434c80 reads it as s16 and
                       builds t*(s16[0x5c]*u_axis + s16[0x60]*v_axis)/256.0
                       (SET_POLY_SCROLL relates)
    0x5e u16 anim0     animated s16; 0x4342f0 lerps the 16-bit halves between
                       two key polys
    0x60 u16 scroll_v  see 0x5c
    0x62 u16 anim1     see 0x5e
    0x64 u16 v_scale   derived: uv_v_fp >> 7 (text loader 0x425ddc)
    0x66 u16 anim2     see 0x5e
    0x68 u32 reserved  always 0
    0x6c u16 idx[]     vertex indices (n_idx of them, stored after 0x71)
    0x70 u8  bright    brightness byte (SET_POLY_BRIGHTNESS, id 0x10)
    0x71 u8  n_idx     number of vertex indices
    0x72 u16 handle    slot id; text loader maps input 1->0x7ff8, 2->0x7ff9
    0x74 u16 f74       runtime auxiliary: the text loader zeroes it (0x425df2)
    0x76 u16 f76       same (0x425de6)
    0x78 u16 overlay   overlay texture index (same encoding as 0x00)
    0x7a u16 f7a       auxiliary
    0x7c u32 flags     bit 0x8000 = "UV/frame computed" (set on every shipped
                       poly; when clear 0x43e02d recomputes the UVs), bit 0x2
                       = transparent ("sprites" texture), bit 0x4 toggled by
                       a command (xor [poly+0x7c],4 at 0x46adbc/0x46aefa)

The five animated scalars 0x5c/0x5e/0x60/0x62/0x66 are interpolated *per 16-bit
half* by ``0x4342f0`` (between two key polys).  Empirically (but never read as
such by the engine, which only ever loads these slots as ``s16``) the shipped
pairs ``(0x5c,0x5e)`` and ``(0x60,0x62)`` are bit-exact finite non-zero ``f32``
values clustered at powers of two (72 % within 5 % of ``2**k``, the most
common being exactly ``2**k*(1+1/128)``; the link to ``u_scale`` is only
statistical, median ``u_scale*f`` = 0.77) -- apparently an export artefact.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

Vec3 = tuple[float, float, float]


@dataclass
class TexRef:
    name: str
    flag: int
    value: int


@dataclass
class EffectRef:
    name: str
    kind: int
    start: int
    ids: list[int] = field(default_factory=list)


@dataclass
class Poly:
    """One polygon (engine record ``0x80`` bytes; ``124 + 2*n_idx`` on disk)."""

    texture: int                    # 0x00: texture index (>0 table, <0 effect, 0 none)
    material: int                   # 0x02: material index 0..5
    lightmap: int                   # 0x04: lightmap index (0 = none)
    u_scale: int                    # 0x06
    centre: Vec3                    # 0x08
    normal: Vec3                    # 0x14: unit face normal
    u_axis: Vec3                    # 0x20: unit texture U axis
    v_axis: Vec3                    # 0x2c: unit texture V axis
    corner_a: Vec3                  # 0x38
    corner_b: Vec3                  # 0x44
    uv_u: int                       # 0x50
    uv_v: int                       # 0x52
    uv_u_fp: int                    # 0x54
    uv_v_fp: int                    # 0x58
    scroll_u: int                   # 0x5c
    anim0: int                      # 0x5e
    scroll_v: int                   # 0x60
    anim1: int                      # 0x62
    v_scale: int                    # 0x64
    anim2: int                      # 0x66
    reserved: int                   # 0x68: always 0
    brightness: int                 # 0x70
    indices: tuple[int, ...]        # 0x6c, n_idx at 0x71
    handle: int                     # 0x72
    f74: int                        # 0x74
    f76: int                        # 0x76
    overlay: int                    # 0x78: overlay texture index
    f7a: int                        # 0x7a
    flags: int                      # 0x7c
    raw: bytes = b""                # the 112 bytes after the index list

    @property
    def n_idx(self) -> int:
        return len(self.indices)

    @property
    def size(self) -> int:
        return 124 + 2 * len(self.indices)

    def to_bytes(self) -> bytes:
        """Re-serialise the polygon in disk order (byte-exact round-trip)."""
        head = struct.pack("<5H", self.texture, self.lightmap, self.material,
                           self.overlay, self.f7a)
        head += bytes((self.brightness & 0xFF, len(self.indices) & 0xFF))
        idx = struct.pack("<%dH" % len(self.indices), *self.indices) if self.indices else b""
        return head + idx + self.raw


@dataclass
class Object:
    id: int
    header: bytes
    points: tuple[Vec3, ...] = ()   # origin + 3 basis vectors (diag 1.875)
    n_verts: int = 0
    vertices: tuple[Vec3, ...] = ()
    radius: float = 0.0             # 0x60: |bounds| (bounding-sphere radius)
    f74: int = 0                    # 0x74: 0
    f75: int = 0                    # 0x75: 0 (1 in 7 shipped objects)
    f78: int = 0                    # 0x78: 0
    f7c: int = 0                    # 0x7c: 0
    bounds: tuple[float, float, float] = (0.0, 0.0, 0.0)  # 0x84: AABB half-extents
    reserved: bytes = b""           # 0x90: 24 zero bytes
    raw_tail: bytes = b""           # the 50 bytes 0x60..0x91 (verbatim)
    polys: tuple[Poly, ...] = ()


@dataclass
class Sector:
    kind: int
    sub: int
    objects: list[Object] = field(default_factory=list)


@dataclass
class Sbw:
    tex: list[TexRef] = field(default_factory=list)
    effects: list[EffectRef] = field(default_factory=list)
    sectors: list[Sector] = field(default_factory=list)
    table1_end: int = 0
    table2_end: int = 0
    data_end: int = 0               # == file size after a successful parse

    @property
    def n_objects(self) -> int:
        return sum(len(s.objects) for s in self.sectors)

    @property
    def n_polys(self) -> int:
        return sum(len(o.polys) for o in (ob for s in self.sectors for ob in s.objects))

    def to_bytes(self) -> bytes:
        """Serialise the whole file (byte-exact round-trip of the shipped data)."""
        out = bytearray()
        out += struct.pack("<I", len(self.tex))
        for t in self.tex:
            out += t.name.encode("latin1") + b"\0"
            out += struct.pack("<HI", t.flag, t.value)
        out += struct.pack("<I", len(self.effects))
        for e in self.effects:
            out += e.name.encode("latin1") + b"\0"
            out += struct.pack("<III", e.kind, e.start, len(e.ids))
            out += struct.pack("<%dH" % len(e.ids), *e.ids) if e.ids else b""
        out += struct.pack("<I", len(self.sectors))
        for s in self.sectors:
            out += struct.pack("<IHB", len(s.objects), s.kind, s.sub)
            for o in s.objects:
                out += struct.pack("<I", o.id) + o.header[:96]
        for s in self.sectors:
            for o in s.objects:
                out += struct.pack("<HH", len(o.vertices), len(o.polys))
                for v in o.vertices:
                    out += struct.pack("<3f", *v)
                out += o.raw_tail[:50]
                for p in o.polys:
                    out += p.to_bytes()
        return bytes(out)


def _cstr(data: bytes, o: int) -> tuple[str, int]:
    e = data.index(b"\0", o)
    return data[o:e].decode("latin1"), e + 1


def _vec3(data: bytes, o: int) -> Vec3:
    return struct.unpack_from("<3f", data, o)


def _points(header: bytes) -> tuple[Vec3, ...]:
    """Decode the 4 points of an object header.

    Each of the 12 components is written as an 8-byte ``(u32 pad, f32 value)``
    pair; the pad is always 0 in shipped data.  The result is (origin, x, y, z).
    """
    vals = [struct.unpack_from("<f", header, 8 * i + 4)[0] for i in range(12)]
    return (tuple(vals[0:3]), tuple(vals[3:6]), tuple(vals[6:9]), tuple(vals[9:12]))


def _parse_poly(data: bytes, o: int) -> Poly:
    """Parse one polygon at *o*; returns it (``raw`` holds the 112-byte tail)."""
    # 0x00, 0x04, 0x02, 0x78, 0x7a are read first (disk order), then the two
    # bytes at 0x70/0x71 and the index list, then the rest of the tail.
    texture, lightmap, material, overlay, f7a = struct.unpack_from("<5H", data, o)
    brightness = data[o + 10]
    n_idx = data[o + 11]
    o += 12
    indices = struct.unpack_from("<%dH" % n_idx, data, o) if n_idx else ()
    o += 2 * n_idx
    scroll_u, scroll_v, u_scale, v_scale, uv_u, uv_v = struct.unpack_from("<6H", data, o)
    uv_u_fp, uv_v_fp = struct.unpack_from("<II", data, o + 12)
    centre, normal, u_axis, v_axis, corner_a, corner_b = (
        _vec3(data, o + 20 + 12 * t) for t in range(6))
    f74, f76, handle = struct.unpack_from("<3H", data, o + 92)
    reserved = struct.unpack_from("<I", data, o + 98)[0]
    flags = struct.unpack_from("<I", data, o + 102)[0]
    anim0, anim1, anim2 = struct.unpack_from("<3H", data, o + 106)
    raw = data[o:o + 112]
    return Poly(
        texture=texture, material=material, lightmap=lightmap, u_scale=u_scale,
        centre=centre, normal=normal, u_axis=u_axis, v_axis=v_axis,
        corner_a=corner_a, corner_b=corner_b, uv_u=uv_u, uv_v=uv_v,
        uv_u_fp=uv_u_fp, uv_v_fp=uv_v_fp, scroll_u=scroll_u, anim0=anim0,
        scroll_v=scroll_v, anim1=anim1, v_scale=v_scale, anim2=anim2,
        reserved=reserved, brightness=brightness, indices=tuple(indices),
        handle=handle, f74=f74, f76=f76, overlay=overlay, f7a=f7a,
        flags=flags, raw=raw)


def parse(data: bytes) -> Sbw:
    if len(data) < 4:
        raise ValueError("too short for .sbw")
    o = 0
    count1 = struct.unpack_from("<I", data, o)[0]
    o += 4
    tex: list[TexRef] = []
    for _ in range(count1):
        name, o = _cstr(data, o)
        flag, value = struct.unpack_from("<HI", data, o)
        o += 6
        tex.append(TexRef(name, flag, value))
    t1_end = o

    effects: list[EffectRef] = []
    count2 = struct.unpack_from("<I", data, o)[0]
    o += 4
    for _ in range(count2):
        name, o = _cstr(data, o)
        kind, start, n = struct.unpack_from("<III", data, o)
        o += 12
        ids = list(struct.unpack_from("<%dH" % n, data, o)) if n else []
        o += 2 * n
        effects.append(EffectRef(name, kind, start, ids))
    t2_end = o

    # stage 1: sector/object transform headers
    count3 = struct.unpack_from("<I", data, o)[0]
    o += 4
    sectors: list[Sector] = []
    for _ in range(count3):
        objs = struct.unpack_from("<I", data, o)[0]
        kind, sub = struct.unpack_from("<HB", data, o + 4)
        o += 7
        sect = Sector(kind, sub)
        for _j in range(objs):
            oid = struct.unpack_from("<I", data, o)[0]
            header = data[o + 4 : o + 4 + 96]
            o += 4 + 96
            sect.objects.append(Object(oid, header, _points(header)))
        sectors.append(sect)

    # stage 2: per-object geometry, same sector/object order
    for sect in sectors:
        for obj in sect.objects:
            nv, np_ = struct.unpack_from("<HH", data, o)
            o += 4
            verts = tuple(_vec3(data, o + 12 * k) for k in range(nv))
            o += 12 * nv
            obj.n_verts = nv
            obj.vertices = verts
            raw_tail = data[o : o + 50]
            obj.raw_tail = raw_tail
            obj.radius = struct.unpack_from("<f", data, o)[0]
            obj.f75 = data[o + 4]
            obj.f74 = data[o + 5]
            obj.f7c, obj.f78 = struct.unpack_from("<II", data, o + 6)
            obj.bounds = struct.unpack_from("<3f", data, o + 14)
            obj.reserved = data[o + 26 : o + 50]
            o += 50
            polys: list[Poly] = []
            for _k in range(np_):
                p = _parse_poly(data, o)
                o += p.size
                polys.append(p)
            obj.polys = tuple(polys)

    return Sbw(tex, effects, sectors, t1_end, t2_end, o)


def parse_file(path: str) -> Sbw:
    with open(path, "rb") as f:
        return parse(f.read())


def to_obj(w: Sbw, path: str, transform: bool = False, tex: bool = False) -> int:
    """Write the level geometry as a Wavefront OBJ.

    Every object becomes a group; its polygons reference the object-local
    vertices.  With ``transform=True`` each vertex is placed by the object's
    header (origin + 1.875-scaled basis); note the shipped headers keep every
    object near the world origin, so the untransformed (default) local meshes
    are the more faithful dump.  Returns the number of polygons written.
    """
    n = 0
    with open(path, "w") as f:
        f.write("# Gunmetal .sbw level geometry\n")
        f.write(f"# {len(w.tex)} textures, {len(w.effects)} effects, "
                f"{len(w.sectors)} sectors, {w.n_objects} objects\n")
        base = 0
        for si, s in enumerate(w.sectors):
            for oj, o in enumerate(s.objects):
                f.write(f"# sector {si} object {oj} id={o.id} "
                        f"origin={_fmt(o.points[0])}\n")
                f.write(f"g sector{si}_obj{oj}\n")
                for v in o.vertices:
                    if transform:
                        p = _apply(o.points, v)
                        f.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
                    else:
                        f.write(f"v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}\n")
                for p in o.polys:
                    if len(p.indices) < 3:
                        continue
                    tag = ""
                    if tex and p.texture != 0xFFFF:
                        tag = f" # tex={p.texture} lightmap={p.lightmap} mat={p.material}"
                    f.write("f " + " ".join(str(i + 1 + base) for i in p.indices) + tag + "\n")
                    n += 1
                base += len(o.vertices)
    return n


def _fmt(v: Vec3) -> str:
    return "(" + ",".join(f"{c:.3f}" for c in v) + ")"


def _apply(points: tuple[Vec3, ...], v: Vec3) -> Vec3:
    o, bx, by, bz = points
    return (
        o[0] + bx[0] * v[0] + by[0] * v[1] + bz[0] * v[2],
        o[1] + bx[1] * v[0] + by[1] * v[1] + bz[1] * v[2],
        o[2] + bx[2] * v[0] + by[2] * v[1] + bz[2] * v[2],
    )
