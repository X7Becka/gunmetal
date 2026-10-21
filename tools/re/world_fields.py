#!/usr/bin/env python3
"""World / sector / object / polygon field map of the Gunmetal runtime (M2.3.2).

Prints the engine-side layouts below and, with ``--verify``, checks them
against the shipped ``.sbw`` levels (read only).  ``--self-test`` runs
synthetic plus shipped-data checks and exits non-zero on any failure.

Maps (all addresses are ``gundx.exe`` 1.36 VA, ImageBase ``0x400000``):

* ``0x5670d8`` -- sector array pointer, stride ``0x18``, count ``0x5670dc``;
* ``0x567138`` -- per-sector AABB, stride ``0x30`` (6 doubles);
* ``0x5670e0`` -- material table pointer, stride ``0x40``, count ``0x5670e4``
  (30 = 5 light styles x 6 kinds);
* ``0x800b60`` -- ``SECTOR_STUFF``, 256 x ``0x58`` (gameplay per-sector record);
* object record ``0xa8``, polygon record ``0x80``;
* texture/effect/lightmap anchor tables.

Only the Python standard library is used.  ``gm/sbw.py`` is used as the
independent oracle (optional; the built-in parser works without it).

Usage:
    python3 tools/re/world_fields.py --map
    python3 tools/re/world_fields.py --verify
    python3 tools/re/world_fields.py --verify --levels level0 dm1
    python3 tools/re/world_fields.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import math
import os
import struct
import sys

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

# ---------------------------------------------------------------------------
# anchors
# ---------------------------------------------------------------------------

ADDRESSES = [
    ("0x5670d8", "u32*", "sector array base", "load 0x422f03, xrefs 39 functions"),
    ("0x5670dc", "u32", "sector count (= file count3)", "load 0x42ad73, 0x422f03"),
    ("0x567138", "AABB[256]", "per-sector AABB, stride 0x30", "write 0x434fed, read 0x4360f1"),
    ("0x5670e0", "u32*", "material table base", "init 0x420c03"),
    ("0x5670e4", "u32", "material count = 0x1e (30)", "0x420c03 allocation"),
    ("0x800b60", "u8*", "SECTOR_STUFF array, 256 x 0x58", "0x46b1b0, save block 8"),
    ("0x800bb0", "u32", "SECTOR_STUFF[0].flags (not a thing-type table)", "docs/THING_FIELDS.md"),
    ("0x76ed40", "u8*", "texture table, stride 0x10, count from file", "load 0x42ad85"),
    ("0x5310c0", "u32*", "per-texture refcount (seeded by file value)", "0x42aba9, 0x424eab"),
    ("0x5670cc", "u32", "effect/animation count (= file count2)", "load 0x42ad..."),
    ("0x5642c8", "u8*", "effect table, stride 0x5c, name at +0x4c", "load 0x42ad..."),
    ("0x5a45c8", "u32", "lightmap page count", "compact 0x429e3b"),
    ("0x5a45cc", "u8*", "lightmap page table, stride 8", "0x429e3b, 0x428dd0"),
]

# (offset, size, type, name, consumers/producers, status)
SECTOR_LAYOUT = [
    (0x00, 4, "u32", "n_objects", "load 0x42ad9c; alloc 0x422d55; lookup 0x434419/0x434463; add 0x4358c9; del 0x435995", "proven"),
    (0x04, 4, "u32*", "objects[n] -> object[0xa8]", "alloc 0x422d55 (n*4 + n*0xa8), read 0x434419", "proven"),
    (0x08, 2, "u16", "reserved_08", "zeroed 0x42adf3; no reader/writer found in the 39 xref functions", "hypothesis (reserved)"),
    (0x0a, 2, "u16", "n_sprites", "zeroed 0x42adfc; new 0x43659f (+1), delete 0x4363de (-1)", "proven"),
    (0x0c, 4, "u32", "reserved_0c", "zeroed by 0x422f03 memset; no consumer found", "hypothesis (reserved)"),
    (0x10, 4, "u32*", "sprites[n] -> sprite record", "alloc/grow 0x43659f, delete 0x4363de, read 0x428215", "proven"),
    (0x14, 2, "u16", "kind", "load 0x42adbd; getter 0x42d99f (v/6), setter 0x42d9d5 (v*6)", "proven"),
    (0x16, 1, "u8", "light", "zeroed 0x42ae05; SET_SECTOR_BRIGHTNESS 0x4578d6 (docs/FORMATS.md)", "proven"),
    (0x17, 1, "u8", "sub", "load 0x42adde; .sow writer 0x42906c", "proven"),
]

SECTOR_AABB_LAYOUT = [
    (0x00, 8, "f64", "min_x", "write 0x434fed, read 0x4360f1", "proven"),
    (0x08, 8, "f64", "min_y", "write 0x434fed, read 0x4360f1", "proven"),
    (0x10, 8, "f64", "min_z", "write 0x434fed, read 0x4360f1", "proven"),
    (0x18, 8, "f64", "max_x", "write 0x434fed, read 0x4360f1", "proven"),
    (0x20, 8, "f64", "max_y", "write 0x434fed, read 0x4360f1", "proven"),
    (0x28, 8, "f64", "max_z", "write 0x434fed, read 0x4360f1", "proven"),
]

OBJECT_LAYOUT = [
    (0x00, 24, "3 x f64", "origin", "load 0x42ae86 raw; apply 0x437af8; set 0x434887; read 0x43529f/0x434fed/0x437d05", "proven"),
    (0x18, 72, "9 x f64 (3 rows)", "basis (world = origin + B*local)", "load 0x42aea7/0x42aec8/0x42aee9 raw; apply 0x437af8", "proven"),
    (0x60, 4, "f32", "radius (|bounds|)", "load 0x425f4c; cull 0x437d8c/0x437d05; sector sphere 0x4360f1", "proven"),
    (0x64, 4, "f32*", "vertices[n_verts] (12 B each)", "alloc 0x422b88, fill 0x425f4c", "proven"),
    (0x68, 2, "u16", "n_verts", "load 0x425f5x", "proven"),
    (0x6a, 2, "u16", "n_polys", "load 0x425f5x; every world loop", "proven"),
    (0x6c, 4, "u32*", "polys[n_polys] (0x80 each)", "alloc 0x422b88, resolve 0x434463", "proven"),
    (0x70, 4, "u32", "flags (file field called 'id')", "load 0x42ae68; cull 0x437d75 (test 0x10010); free 0x422ab6 (0x40000/0x80000); 0x434887 (0x100000)", "proven"),
    (0x74, 1, "u8", "f74 (runtime aux)", "zeroed 0x42ae55; text loader zeroes 0x425df2", "proven"),
    (0x75, 1, "u8", "f75", "load 0x425f4c (1 in 7 shipped objects)", "proven"),
    (0x76, 2, "u16", "pad", "not read/written", "hypothesis"),
    (0x78, 4, "u32", "f78", "load 0x425f4c (always 0 in shipped data)", "proven (layout), unused"),
    (0x7c, 4, "u32", "f7c", "load 0x425f4c (always 0 in shipped data)", "proven (layout), unused"),
    (0x80, 4, "u32*", "owner model (0 = level object)", "read 0x435995/0x437c4e; 0x429e3b/0x42d045/0x42f183 branch on it", "proven"),
    (0x84, 12, "3 x f32", "bounds (half extents)", "load 0x425f4c; recomputed 0x43529f; sector AABB 0x434fed", "proven"),
    (0x90, 24, "3 x f64", "extent (extra AABB half-size)", "load 0x425f4c from file 'reserved'; read 0x434fed/0x437d05/0x440867", "proven (layout), always 0 in shipped"),
    (0xa8, 0, "-", "record end", "stride from 0x422d55 (n*0xa8)", "proven"),
]

POLY_LAYOUT = [
    (0x00, 2, "i16", "texture", "load 0x425f4c; renderer 0x437fe2; >0 tex table, <0 effects, 0 none", "proven"),
    (0x02, 2, "u16", "material", "load 0x425f4c; renderer 0x437fb3, trace 0x437d05 -> material table", "proven"),
    (0x04, 2, "i16", "lightmap", "load 0x425f4c; rebase/compact 0x429e3b (1-based, 0 none)", "proven"),
    (0x06, 2, "u16", "u_scale", "derived: uv_u_fp >> 7 (0x425db4)", "proven (derived)"),
    (0x08, 12, "3 x f32", "centre", "load; renderer/lightmap/trace (0x437d05, 0x437f90)", "proven"),
    (0x14, 12, "3 x f32", "normal (unit)", "load; trace plane test 0x437d05, sector hull 0x4360f1", "proven"),
    (0x20, 12, "3 x f32", "u_axis (unit)", "load; UV projection / auto-UV 0x43e02d", "proven"),
    (0x2c, 12, "3 x f32", "v_axis (unit)", "load; UV projection / auto-UV 0x43e02d", "proven"),
    (0x38, 12, "3 x f32", "corner_a (min AABB rel. object)", "recomputed 0x43529f, init +1e38", "proven"),
    (0x44, 12, "3 x f32", "corner_b (max AABB rel. object)", "recomputed 0x43529f, init -1e38", "proven"),
    (0x50, 2, "u16", "uv_u", "load; auto-UV 0x43e02d (+= 256*dot)", "proven"),
    (0x52, 2, "u16", "uv_v", "load; auto-UV 0x43e02d", "proven"),
    (0x54, 4, "i32", "uv_u_fp", "load; texel formula 0x43..., lerp 0x4342f0", "proven"),
    (0x58, 4, "i32", "uv_v_fp", "load; texel formula, lerp 0x4342f0", "proven"),
    (0x5c, 2, "i16", "scroll_u (weight)", "load; scroll helper 0x434c80", "proven"),
    (0x5e, 2, "u16", "anim0", "load; per-16-bit lerp 0x4342f0", "proven"),
    (0x60, 2, "i16", "scroll_v (weight)", "load; scroll helper 0x434c80", "proven"),
    (0x62, 2, "u16", "anim1", "load; lerp 0x4342f0", "proven"),
    (0x64, 2, "u16", "v_scale", "derived: uv_v_fp >> 7 (0x425ddc)", "proven (derived)"),
    (0x66, 2, "u16", "anim2", "load; lerp 0x4342f0", "proven"),
    (0x68, 4, "u32", "reserved", "always 0", "proven (data)"),
    (0x6c, 2, "u16*", "indices[n_idx]", "alloc 0x422a51, load 0x425f4c", "proven"),
    (0x70, 1, "u8", "brightness", "load; renderer face_brightness; forced 0x7f by 0x433b53", "proven"),
    (0x71, 1, "u8", "n_idx (3..12)", "load; every polygon loop", "proven"),
    (0x72, 2, "u16", "link_sector+1", "0x42f183 resolves; 0x42d045 renders; runtime markers 0x7ff8/0x7ff9", "proven"),
    (0x74, 2, "u16", "link_object", "0x42f183; project name 'f76'", "proven"),
    (0x76, 2, "u16", "link_poly+1", "0x42f183; project name 'handle'", "proven"),
    (0x78, 2, "i16", "overlay texture", "load; renderer second pass 0x437fd5", "proven"),
    (0x7a, 2, "u16", "f7a", "load (0..2 in shipped data)", "proven (layout), unused"),
    (0x7c, 4, "u32", "flags", "0x8000 UV done (0x43e02d), 0x2 transparent, 0x4 command xor (0x46adbc); 0x433a6a sets bit 0", "proven"),
]

MATERIAL_LAYOUT = [
    (0x00, 4, "u32", "mode (bit0 = second/blended pass)", "read 0x437fba, 0x439e75, 0x43d568, 0x437d05", "proven"),
    (0x04, 4, "u32*", "blend table A", "set 0x420c03 per style; consumer not traced", "hypothesis"),
    (0x08, 4, "u32*", "blend table B", "set 0x420c03 per kind; consumer not traced", "hypothesis"),
    (0x0c, 4, "u32*", "light-style table (0xa01040 + style*0x600)", "set 0x420c03; consumer not traced", "hypothesis"),
    (0x10, 4, "u32*", "factor table", "set 0x420c03", "hypothesis"),
    (0x14, 4, "u32*", "factor table", "set 0x420c03", "hypothesis"),
    (0x18, 8, "u32[2]", "zero", "allocation is memset? no - only some slots written", "unknown"),
    (0x20, 4, "f32", "r factor (1.0 / 0.5 / 1.0 / 1.0 / 0)", "set 0x420c03 per style", "proven (data)"),
    (0x24, 4, "f32", "g factor (1.0 / 1.0 / 0 / 0.5 / 0)", "set 0x420c03 per style", "proven (data)"),
    (0x28, 4, "f32", "b factor (1.0 / 1.0 / 0 / 0 / 1.0)", "set 0x420c03 per style", "proven (data)"),
    (0x2c, 4, "i32", "pass parameter 0/64/64/100/127/-128", "set 0x420c03 per kind", "proven (data)"),
    (0x30, 4, "u32", "style colour/tint (0x9c9c9c / 0x50a0a0 / 0xa00000 / 0xa05000 / 0xa0)", "set only for kind 0 of each style", "hypothesis"),
    (0x34, 12, "char[12]", "name (none/reflective/translucent/varadd/add/shade)", "0x420c03; docs only", "proven"),
]

SECTOR_STUFF_LAYOUT = [
    (0x00, 24, "3 x f64", "move (dx,dy,dz per tick)", "SET_SECTOR_MOVEMENT 0x457815; 0x4713a5", "proven"),
    (0x18, 24, "3 x f64", "extra", "0x457846..ac; no reader in build", "proven (layout)"),
    (0x30, 8, "f64", "sound_param", "SET_SECTOR_SOUND 0x4579ee, 0x46b740", "proven"),
    (0x38, 4, "u32", "event_a", "SET_SECTOR_EVENTS 0x4579ab; 0x445095", "proven"),
    (0x3c, 4, "u32", "event_b", "0x4579ab; 0x44503f", "proven"),
    (0x40, 2, "u16", "damage_lo", "SET_SECTOR_DAMAGE 0x457961; 0x4450f6", "proven"),
    (0x44, 2, "u16", "damage_hi", "0x457997", "proven"),
    (0x48, 4, "i32", "sound (SOUND_SAMPLE_TABLE index)", "0x4579ee, 0x46b6e3, 0x46c5fd", "proven"),
    (0x4c, 4, "i32", "sound_count", "0x4579ee, 0x46c649", "proven"),
    (0x50, 4, "u32", "flags (bit0..bit4)", "SET_SECTOR_FLAGS 0x457929; 0x444166/0x46bce5/0x46c652/0x444767/0x4727cb", "proven"),
    (0x54, 4, "u32", "unused", "no readers/writers, 0 in saves", "hypothesis"),
]


def _rows(rows):
    out = []
    for off, size, typ, name, who, status in rows:
        out.append("  +0x%02x %-4s %-26s %-34s %-8s %s"
                   % (off, ("%d" % size) if size else "-", typ, name, status, who))
    return "\n".join(out)


def print_maps():
    print("gundx.exe 1.36 -- world/sector/object/polygon runtime map (M2.3.2)\n")
    print("anchors:")
    for addr, typ, name, who in ADDRESSES:
        print("  %-9s %-10s %-46s %s" % (addr, typ, name, who))

    print("\nsector record (0x18), *[0x5670d8], count [0x5670dc]:")
    print(_rows(SECTOR_LAYOUT))
    print("  -> 0x00 count, 0x04 object3d list, 0x0a/0x10 sprite list, 0x14 kind, 0x16 light, 0x17 sub")

    print("\nsector AABB[256], base 0x567138, stride 0x30 (doubles):")
    print(_rows(SECTOR_AABB_LAYOUT))
    print("  built by 0x434fed from object 0: origin +- bounds, extended by |extent|")
    print("  sector 0 keeps the +-1e38 sentinel; 0x4360f1/0x437d05 scan from sector 1")
    print("  (NOT the box of all objects: 74/192 level0 objects, see --verify)")

    print("\nobject record (0xa8), sector.objects[i]:")
    print(_rows(OBJECT_LAYOUT))

    print("\npolygon record (0x80), object+0x6c + i*0x80:")
    print(_rows(POLY_LAYOUT))
    print("  note: disk writes link fields in the order 0x72/0x74/0x76, so gm/sbw.py and")
    print("        engine/res/sbw.h name them f74/f76/handle; engine semantics are")
    print("        sector+1 / object / poly+1.")

    print("\nmaterial table, *[0x5670e0], count [0x5670e4]=30, stride 0x40:")
    print(_rows(MATERIAL_LAYOUT))
    print("  index = style*6 + kind; style 0 = names above, styles 1..4 = '_ld_*_2..5'")
    print("  object+0x70 bit 0x10010 skips a whole object; material+0x00 bit0 forces a")
    print("  polygon into the pass (0x437fba).  Untextured polys are skipped by the")
    print("  world ray 0x437d05 unless material bit0 or an overlay is set.")

    print("\nSECTOR_STUFF (save block 8 / runtime 0x800b60), 256 x 0x58:")
    print(_rows(SECTOR_STUFF_LAYOUT))
    print("  +0x50 is the field docs/THING_FIELDS.md mistook for 0x800bb0 'thing types'.")


# ---------------------------------------------------------------------------
# minimal independent .sbw parser (stdlib only)
# ---------------------------------------------------------------------------

class ParseError(Exception):
    pass


def _need(data, off, size, what):
    if off < 0 or size < 0 or off + size > len(data):
        raise ParseError("truncated .sbw at %#x (%s)" % (off, what))


def _cstr(data, off):
    end = data.find(b"\0", off)
    if end < 0:
        raise ParseError("unterminated string at %#x" % off)
    return data[off:end].decode("latin1"), end + 1


def _points(header):
    """96-byte object header -> (origin, bx, by, bz); each word is (pad u32, f32)."""
    vals = [struct.unpack_from("<f", header, 8 * i + 4)[0] for i in range(12)]
    return (tuple(vals[0:3]), tuple(vals[3:6]), tuple(vals[6:9]), tuple(vals[9:12]))


def _world(points, v):
    o, bx, by, bz = points
    return (o[0] + bx[0] * v[0] + by[0] * v[1] + bz[0] * v[2],
            o[1] + bx[1] * v[0] + by[1] * v[1] + bz[1] * v[2],
            o[2] + bx[2] * v[0] + by[2] * v[1] + bz[2] * v[2])


def _parse_poly(data, off):
    """One 0x80 polygon: head, indices, then 112 bytes in disk order."""
    _need(data, off, 12, "poly head")
    texture, lightmap, material, overlay, f7a = struct.unpack_from("<5H", data, off)
    brightness = data[off + 10]
    n_idx = data[off + 11]
    off += 12
    _need(data, off, 2 * n_idx + 112, "poly body")
    indices = struct.unpack_from("<%dH" % n_idx, data, off) if n_idx else ()
    tail = off + 2 * n_idx
    (scroll_u, scroll_v, u_scale, v_scale, uv_u, uv_v) = struct.unpack_from("<6H", data, tail)
    uv_u_fp, uv_v_fp = struct.unpack_from("<ii", data, tail + 12)
    centre = struct.unpack_from("<3f", data, tail + 20)
    normal = struct.unpack_from("<3f", data, tail + 32)
    u_axis = struct.unpack_from("<3f", data, tail + 44)
    v_axis = struct.unpack_from("<3f", data, tail + 56)
    corner_a = struct.unpack_from("<3f", data, tail + 68)
    corner_b = struct.unpack_from("<3f", data, tail + 80)
    link_sector, link_object, link_poly = struct.unpack_from("<3H", data, tail + 92)
    reserved = struct.unpack_from("<I", data, tail + 98)[0]
    flags = struct.unpack_from("<I", data, tail + 102)[0]
    anim0, anim1, anim2 = struct.unpack_from("<3H", data, tail + 106)
    return {
        "texture": texture if texture < 0x8000 else texture - 0x10000,
        "material": material, "lightmap": lightmap,
        "overlay": overlay, "f7a": f7a, "brightness": brightness,
        "indices": indices,
        "scroll_u": scroll_u, "scroll_v": scroll_v,
        "u_scale": u_scale, "v_scale": v_scale,
        "uv_u": uv_u, "uv_v": uv_v, "uv_u_fp": uv_u_fp, "uv_v_fp": uv_v_fp,
        "centre": centre, "normal": normal, "u_axis": u_axis, "v_axis": v_axis,
        "corner_a": corner_a, "corner_b": corner_b,
        "link_sector_m1": link_sector, "link_object": link_object,
        "link_poly_m1": link_poly,
        "reserved": reserved, "flags": flags,
        "anim0": anim0, "anim1": anim1, "anim2": anim2,
        "raw": data[tail:tail + 112],
    }


def parse(data):
    """Parse a whole .sbw into tables + sectors/objects/geometry (engine offsets)."""
    off = 0
    _need(data, 0, 4, "texture count")
    n_tex = struct.unpack_from("<I", data, 0)[0]
    off = 4
    textures = []
    for _ in range(n_tex):
        name, off = _cstr(data, off)
        _need(data, off, 6, "texture entry")
        flag, value = struct.unpack_from("<HI", data, off)
        off += 6
        textures.append((name, flag, value))

    _need(data, off, 4, "effect count")
    n_eff = struct.unpack_from("<I", data, off)[0]
    off += 4
    effects = []
    for _ in range(n_eff):
        name, off = _cstr(data, off)
        _need(data, off, 12, "effect entry")
        kind, start, n = struct.unpack_from("<III", data, off)
        off += 12
        _need(data, off, 2 * n, "effect ids")
        ids = struct.unpack_from("<%dH" % n, data, off) if n else ()
        off += 2 * n
        effects.append((name, kind, start, ids))

    _need(data, off, 4, "sector count")
    n_sectors = struct.unpack_from("<I", data, off)[0]
    off += 4
    sectors = []
    for _ in range(n_sectors):
        _need(data, off, 7, "sector header")
        n_obj, kind, sub = struct.unpack_from("<IHB", data, off)
        off += 7
        objects = []
        for _ in range(n_obj):
            _need(data, off, 100, "object header")
            oid = struct.unpack_from("<I", data, off)[0]
            header = data[off + 4:off + 100]
            off += 100
            objects.append({"id": oid, "header": header, "points": _points(header)})
        sectors.append({"kind": kind, "sub": sub, "objects": objects})

    for sector in sectors:
        for obj in sector["objects"]:
            _need(data, off, 4, "geometry header")
            n_verts, n_polys = struct.unpack_from("<HH", data, off)
            off += 4
            _need(data, off, 12 * n_verts + 50, "vertices/tail")
            verts = [struct.unpack_from("<3f", data, off + 12 * i) for i in range(n_verts)]
            off += 12 * n_verts
            radius = struct.unpack_from("<f", data, off)[0]
            f75, f74, f7c, f78 = struct.unpack_from("<BBII", data, off + 4)
            bounds = struct.unpack_from("<3f", data, off + 14)
            off += 50                      # radius..bounds..reserved: 50 disk bytes
            polys = []
            for _ in range(n_polys):
                poly = _parse_poly(data, off)
                off += 124 + 2 * len(poly["indices"])
                polys.append(poly)
            obj.update(n_verts=n_verts, vertices=verts, radius=radius,
                       f75=f75, f74=f74, f7c=f7c, f78=f78, bounds=bounds, polys=polys)
    return {"textures": textures, "effects": effects, "sectors": sectors,
            "file_end": off, "size": len(data)}


def bad_indices(world):
    """Count polygons with a vertex index outside 0..n_verts-1."""
    bad = 0
    for sector in world["sectors"]:
        for obj in sector["objects"]:
            n = len(obj["vertices"])
            for poly in obj["polys"]:
                if any(i >= n for i in poly["indices"]):
                    bad += 1
    return bad


def triangles(world):
    """(triangles, degenerate) with the fan triangulation of engine/collide.cpp."""
    total = degenerate = 0
    for sector in world["sectors"]:
        for obj in sector["objects"]:
            pts = obj["points"]
            vw = [_world(pts, v) for v in obj["vertices"]]
            for poly in obj["polys"]:
                idx = poly["indices"]
                if len(idx) < 3:
                    continue
                for k in range(1, len(idx) - 1):
                    a, b, c = vw[idx[0]], vw[idx[k]], vw[idx[k + 1]]
                    u = [b[i] - a[i] for i in range(3)]
                    v = [c[i] - a[i] for i in range(3)]
                    cr = (u[1] * v[2] - u[2] * v[1],
                          u[2] * v[0] - u[0] * v[2],
                          u[0] * v[1] - u[1] * v[0])
                    total += 1
                    if cr == (0.0, 0.0, 0.0):
                        degenerate += 1
    return total, degenerate


def scan(path, deep=True):
    """Counters for one level; `deep` also validates object/poly invariants."""
    data = open(path, "rb").read()
    w = parse(data)
    st = {
        "file": os.path.basename(path),
        "sectors": len(w["sectors"]),
        "objects": sum(len(s["objects"]) for s in w["sectors"]),
        "polys": sum(len(o["polys"]) for s in w["sectors"] for o in s["objects"]),
        "textures": len(w["textures"]),
        "effects": len(w["effects"]),
        "end_ok": w["file_end"] == len(data),
    }
    if deep:
        kinds, subs, mats, texs, lms, pflags = set(), set(), set(), set(), set(), set()
        objflags, oid_ok, header_ok = set(), 0, 0
        neg_tex = 0
        bounds_bad = radius_exact = radius_total = 0
        len_bad = links = links_valid = 0
        sector_cov = []
        for sector in w["sectors"]:
            kinds.add(sector["kind"])
            subs.add(sector["sub"])
            o0 = sector["objects"][0]
            org = o0["points"][0]
            box = [(org[i] - abs(o0["bounds"][i]), org[i] + abs(o0["bounds"][i]))
                   for i in range(3)]
            cov = 0
            for obj in sector["objects"]:
                wv = [_world(obj["points"], v) for v in obj["vertices"]]
                if wv and all(min(v[i] for v in wv) >= box[i][0] - 1e-3
                              and max(v[i] for v in wv) <= box[i][1] + 1e-3
                              for i in range(3)):
                    cov += 1
            sector_cov.append((cov, len(sector["objects"])))
            for obj in sector["objects"]:
                header_ok += 1 if all(
                    struct.unpack_from("<I", obj["header"], 8 * i)[0] == 0
                    for i in range(12)) else 0
                objflags.add(obj["id"])
                verts = obj["vertices"]
                if verts:
                    for i in range(3):
                        lo = min(v[i] for v in verts)
                        hi = max(v[i] for v in verts)
                        if abs((hi - lo) / 2 - abs(obj["bounds"][i])) > 2.0:
                            bounds_bad += 1
                            break
                    radius_total += 1
                    r = math.sqrt(sum(b * b for b in obj["bounds"]))
                    if abs(r - obj["radius"]) <= 1e-6 * max(1.0, r):
                        radius_exact += 1
                for poly in obj["polys"]:
                    mats.add(poly["material"])
                    texs.add(poly["texture"])
                    if poly["texture"] < 0:
                        neg_tex += 1
                    lms.add(poly["lightmap"])
                    pflags.add(poly["flags"])
                    if poly["texture"] and poly["link_sector_m1"]:
                        links += 1
                        ts = poly["link_sector_m1"] - 1
                        to = poly["link_object"]
                        tp = poly["link_poly_m1"] - 1
                        if (0 <= ts < len(w["sectors"])
                                and 0 <= to < len(w["sectors"][ts]["objects"])
                                and 0 <= tp < len(w["sectors"][ts]["objects"][to]["polys"])):
                            links_valid += 1
        len_bad = bad_indices(w)
        tri_raw, tri_deg = triangles(w)
        st.update(kinds=sorted(kinds), subs=sorted(subs), materials=sorted(mats),
                  triangles=tri_raw, triangles_degenerate=tri_deg,
                  textures_used=sorted(texs), lightmaps=len(lms),
                  poly_flags=sorted(pflags), object_flags=sorted(objflags),
                  header_low_zero=header_ok, bounds_bad=bounds_bad,
                  radius_exact=radius_exact, radius_total=radius_total,
                  bad_indices=len_bad, textured_links=links,
                  textured_links_valid=links_valid, negative_textures=neg_tex,
                  sector_cov=sector_cov)
    return w, st


# ---------------------------------------------------------------------------
# --verify
# ---------------------------------------------------------------------------

CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print("  [%s] %s%s" % ("ok" if ok else "FAIL", name, (" -- " + detail) if detail else ""))
    return ok


def verify_levels(paths, oracle=True):
    print("verify: %d level(s)" % len(paths))
    totals = {"sectors": 0, "objects": 0, "polys": 0, "textures": 0, "effects": 0}
    all_kinds, all_mats, all_pflags, all_oflags = set(), set(), set(), set()
    all_subs = set()
    bad_end = bad_bounds = bad_header = bad_idx = neg_tex = 0
    bad_effect = 0
    tri_raw = tri_deg = 0
    radius_exact = radius_total = 0
    links = links_valid = 0
    cov_ok = cov_total = 0
    for path in paths:
        w, st = scan(path)
        for key in totals:
            totals[key] += st[key]
        all_kinds.update(st["kinds"])
        all_subs.update(st["subs"])
        all_mats.update(st["materials"])
        all_pflags.update(st["poly_flags"])
        all_oflags.update(st["object_flags"])
        bad_end += 0 if st["end_ok"] else 1
        bad_bounds += st["bounds_bad"]
        bad_header += st["objects"] - st["header_low_zero"]
        bad_idx += st["bad_indices"]
        neg_tex += st["negative_textures"]
        tri_raw += st["triangles"]
        tri_deg += st["triangles_degenerate"]
        lo = min(st["textures_used"]) if st["textures_used"] else 0
        if lo < -st["effects"]:
            bad_effect += 1
        radius_exact += st["radius_exact"]
        radius_total += st["radius_total"]
        links += st["textured_links"]
        links_valid += st["textured_links_valid"]
        cov_ok += sum(a for a, _ in st["sector_cov"])
        cov_total += sum(b for _, b in st["sector_cov"])
        print("  %-10s sectors %4d objects %5d polys %6d tex %3d eff %3d "
              "kind %s mats %s"
              % (st["file"], st["sectors"], st["objects"], st["polys"],
                 st["textures"], st["effects"], st["kinds"], st["materials"]))
        if oracle and os.path.basename(path) in ("level0.sbw", "dm1.sbw"):
            _oracle(path, w)

    print("\n  totals: sectors %d objects %d polys %d textures %d effects %d"
          % (totals["sectors"], totals["objects"], totals["polys"],
             totals["textures"], totals["effects"]))
    print("  triangles: %d raw, %d degenerate, %d kept"
          % (tri_raw, tri_deg, tri_raw - tri_deg))
    print("  sector kind set: %s, sub set: %s" % (sorted(all_kinds), sorted(all_subs)))
    print("  material values: %s" % sorted(all_mats))
    print("  poly flags: %s" % [hex(v) for v in sorted(all_pflags)])
    print("  object flags: %s" % [hex(v) for v in sorted(all_oflags)])
    print("  radius == |bounds| exactly: %d/%d" % (radius_exact, radius_total))
    print("  textured polys with a link: %d (valid %d)" % (links, links_valid))
    print("  objects inside their own sector AABB (object 0 sphere box): %d/%d"
          % (cov_ok, cov_total))

    check("all files end exactly at EOF", bad_end == 0, "%d bad" % bad_end)
    check("object header low dwords are 0 (f32 pairs)", bad_header == 0,
          "%d bad of %d" % (bad_header, totals["objects"]))
    check("bounds == half extents of vertices", bad_bounds == 0,
          "%d bad" % bad_bounds)
    check("negative texture refs are effect-table indices", bad_effect == 0,
          "%d negative refs, %d bad files" % (neg_tex, bad_effect))
    check("material indices < 30 (table size)", max(all_mats, default=0) < 30,
          "max %d" % max(all_mats, default=0))
    check("poly vertex indices in range", bad_idx == 0, "%d bad" % bad_idx)
    check("sector kinds are multiples of 6", all(k % 6 == 0 for k in all_kinds),
          str(sorted(all_kinds)))
    check("poly flags subset of 0x8000|0x2|0x4", all(v & ~0x8006 == 0 for v in all_pflags))
    check("textured links resolve", links_valid == links,
          "%d/%d" % (links_valid, links))
    check("sector AABB built from object 0 only (not all objects)",
          cov_ok < cov_total, "%d/%d objects covered" % (cov_ok, cov_total))
    check("level0 geometry: 3162 triangles after 2 degenerate",
          _level0_triangles(paths) if paths else True)
    return all(ok for _, ok, _ in CHECKS)


def _oracle(path, w):
    try:
        from gm import sbw as oracle
    except Exception as exc:  # pragma: no cover - optional oracle
        print("  (oracle gm/sbw.py unavailable: %s)" % exc)
        return
    ref = oracle.parse(open(path, "rb").read())
    same = (len(ref.sectors) == len(w["sectors"])
            and ref.n_objects == sum(len(s["objects"]) for s in w["sectors"])
            and ref.n_polys == sum(len(o["polys"]) for s in w["sectors"] for o in s["objects"])
            and len(ref.tex) == len(w["textures"])
            and len(ref.effects) == len(w["effects"]))
    check("oracle gm/sbw.py agrees on %s" % os.path.basename(path), same,
          "%d/%d/%d/%d/%d" % (len(w["sectors"]),
                              sum(len(s["objects"]) for s in w["sectors"]),
                              sum(len(o["polys"]) for s in w["sectors"] for o in s["objects"]),
                              len(w["textures"]), len(w["effects"])))


def _level0_triangles(paths):
    for path in paths:
        if os.path.basename(path).lower() == "level0.sbw":
            w = parse(open(path, "rb").read())
            total, degenerate = triangles(w)
            print("  level0: %d raw triangles, %d degenerate -> %d"
                  % (total, degenerate, total - degenerate))
            return (total, degenerate) == (3164, 2)
    return True


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def _synthetic_sbw():
    """Two sectors: one quad object in sector 0, one triangle object in sector 1."""
    out = bytearray()
    texs = [("wall", 0, 7)]
    out += struct.pack("<I", len(texs))
    for name, flag, value in texs:
        out += name.encode() + b"\0" + struct.pack("<HI", flag, value)
    out += struct.pack("<I", 0)                     # effects
    out += struct.pack("<I", 2)                     # sectors
    def header(origin, basis):
        return b"".join(struct.pack("<If", 0, value)
                        for value in list(origin) + list(basis[0]) + list(basis[1]) + list(basis[2]))
    hdr0 = header((1.5, -2.0, 0.25), ((1.875, 0.0, 0.0), (0.0, 1.875, 0.0), (0.0, 0.0, 1.875)))
    hdr1 = header((0.0, 0.0, 0.0), ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)))
    out += struct.pack("<IHB", 1, 12, 1) + struct.pack("<I", 0x10010) + hdr0
    out += struct.pack("<IHB", 1, 0, 0) + struct.pack("<I", 0x20) + hdr1

    def geom(verts, polys):
        buf = bytearray(struct.pack("<HH", len(verts), len(polys)))
        for v in verts:
            buf += struct.pack("<3f", *v)
        buf += struct.pack("<f", 1.0)   # radius
        buf += bytes((1, 0))            # f75, f74
        buf += struct.pack("<II", 0, 0)  # f7c, f78
        buf += struct.pack("<3f", 1.0, 1.0, 1.0)  # bounds
        buf += b"\0" * 24
        for head, idx, tail in polys:
            buf += head + struct.pack("<%dH" % len(idx), *idx) + tail
        return bytes(buf)

    quad_tail = bytearray(112)
    struct.pack_into("<6H", quad_tail, 0, 0, 0, 1, 1, 0, 0)   # scroll/scale/uv
    struct.pack_into("<ii", quad_tail, 12, 0, 0)
    struct.pack_into("<3f", quad_tail, 32, 0.0, 0.0, 1.0)     # normal
    struct.pack_into("<3f", quad_tail, 44, 1.0, 0.0, 0.0)     # u axis
    struct.pack_into("<3f", quad_tail, 56, 0.0, 1.0, 0.0)     # v axis
    struct.pack_into("<3H", quad_tail, 92, 2, 0, 1)           # link sector+1/obj/poly+1
    struct.pack_into("<I", quad_tail, 102, 0x8002)            # flags
    head = struct.pack("<5HBB", 1, 1, 2, 0, 0, 64, 4)
    out += geom([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
                [(head, (0, 1, 2, 3), bytes(quad_tail))])
    tri_tail = bytearray(112)
    struct.pack_into("<3f", tri_tail, 32, 0.0, 0.0, 1.0)
    out += geom([(0, 0, 0), (1, 0, 0), (0, 1, 0)],
                [(struct.pack("<5HBB", 0, 0, 0, 0, 0, 0, 3),
                  (0, 1, 2), bytes(tri_tail))])
    return bytes(out)


def self_test(game=DEFAULT_GAME):
    ok = True
    print("self-test: layout arithmetic")
    ok &= check("SECTOR_STUFF 256*0x58 == 0x5800", 256 * 0x58 == 0x5800)
    ok &= check("material 30*0x40 == 0x780", 30 * 0x40 == 0x780)
    ok &= check("material index = style*6+kind < 30", 4 * 6 + 5 == 29)
    ok &= check("poly record 0x80, object 0xa8, sector 0x18",
                0x80 == 128 and 0xa8 == 168 and 0x18 == 24)
    ok &= check("sector AABB 256*0x30 == 0x3000", 256 * 0x30 == 0x3000)

    print("\nself-test: synthetic .sbw")
    data = _synthetic_sbw()
    w = parse(data)
    ok &= check("synthetic round-trip to EOF", w["file_end"] == len(data))
    ok &= check("synthetic structure", len(w["sectors"]) == 2
                and sum(len(s["objects"]) for s in w["sectors"]) == 2
                and sum(len(o["polys"]) for s in w["sectors"] for o in s["objects"]) == 2)
    p = w["sectors"][0]["objects"][0]["polys"][0]
    ok &= check("poly head decoded", p["texture"] == 1 and p["material"] == 2
                and p["lightmap"] == 1 and p["brightness"] == 64
                and p["indices"] == (0, 1, 2, 3))
    ok &= check("poly tail decoded (normal/u/v/links/flags)",
                p["normal"] == (0.0, 0.0, 1.0) and p["u_axis"] == (1.0, 0.0, 0.0)
                and p["v_axis"] == (0.0, 1.0, 0.0)
                and (p["link_sector_m1"], p["link_object"], p["link_poly_m1"]) == (2, 0, 1)
                and p["flags"] == 0x8002)
    ok &= check("sector kind/sub decoded", w["sectors"][0]["kind"] == 12
                and w["sectors"][0]["sub"] == 1)
    ok &= check("object flags decoded (=file id field)", w["sectors"][0]["objects"][0]["id"] == 0x10010)
    total, deg = triangles(w)
    ok &= check("synthetic triangles (1 quad + 1 tri)", (total, deg) == (3, 0),
                "%d/%d" % (total, deg))
    ok &= check("synthetic indices are in range", bad_indices(w) == 0)
    bad = bytearray(data)
    # first polygon: 12-byte head, then indices; point index 0 at 0xFFFF
    pos = data.index(struct.pack("<5HBB", 1, 1, 2, 0, 0, 64, 4)) + 12
    bad[pos:pos + 2] = b"\xff\xff"
    ok &= check("corrupted index is detected", bad_indices(parse(bytes(bad))) == 1)

    print("\nself-test: shipped levels")
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    if not files:
        print("  [skip] no .sbw under %s" % game)
        return ok
    w0 = None
    for path in files:
        if os.path.basename(path).lower() == "level0.sbw":
            w0 = parse(open(path, "rb").read())
            break
    ok &= check("level0 shape 58/192/1449/381/37", w0 is not None
                and len(w0["sectors"]) == 58
                and sum(len(s["objects"]) for s in w0["sectors"]) == 192
                and sum(len(o["polys"]) for s in w0["sectors"] for o in s["objects"]) == 1449
                and len(w0["textures"]) == 381 and len(w0["effects"]) == 37)
    if w0 is not None:
        total, deg = triangles(w0)
        ok &= check("level0 3164 raw / 2 degenerate / 3162 kept",
                    (total, deg) == (3164, 2))
    ok &= check("all 36 levels, all invariants (scan)", verify_levels(files, oracle=False))
    return ok


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", action="store_true", help="print the field maps")
    ap.add_argument("--verify", nargs="*", metavar="SBW",
                    help="verify shipped .sbw files (default: all in --game)")
    ap.add_argument("--levels", nargs="*", help="level stems for --verify (level0 dm1)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--all", action="store_true", help="--map + --verify + --self-test")
    args = ap.parse_args(argv)

    if args.self_test or args.all:
        if not self_test(args.game):
            return 1
        if not args.all:
            return 0

    if args.map or args.all:
        print_maps()

    if args.verify is not None or args.all:
        files = list(args.verify or [])
        if args.levels:
            files = [os.path.join(args.game, stem if stem.endswith(".sbw") else stem + ".sbw")
                     for stem in args.levels]
        elif not files:
            files = sorted(glob.glob(os.path.join(args.game, "*.sbw")))
        if not files:
            print("verify: no files", file=sys.stderr)
            return 1
        for path in files:
            if not os.path.isfile(path):
                print("verify: missing %s" % path, file=sys.stderr)
                return 1
        if not verify_levels(files):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
