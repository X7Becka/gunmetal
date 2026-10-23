#!/usr/bin/env python3
"""Synthetic minimal binary fixtures for the Gunmetal data formats (``M3.2.7``).

The shipped files are large (``level0.sbw`` is megabytes, ``PICS/SBM.DAT`` is
an archive of 1648 textures), so they are useless as *minimal* test vectors:
a test that wants "one sector, one object, four vertices, one polygon" pulls
in megabytes of unrelated data.  This tool writes a tiny, self-consistent set
of files for every format the Python oracle has a writer for, so both the
Python reader and the C++ engine can be checked against the same bytes.

Usage::

    python3 tools/qa/fixtures.py --write /tmp/opencode/fixtures
    python3 tools/qa/fixtures.py --check /tmp/opencode/fixtures
    python3 tools/qa/fixtures.py --check DIR --engine --binary PATH
    python3 tools/qa/fixtures.py --self-test
    python3 tools/qa/fixtures.py --check DIR --json

``--write`` writes (all little-endian, all sizes in bytes)::

    fixture.sbw          1 texture, 0 effects, 1 sector, 1 object,
                         4 vertices, 1 triangle, 1 texture name
    fixture.sbt          SCRIPT (11: count=0, n=1, m=1, code=END 0xFF),
                         SOUND_SAMPLE_TABLE (12: count=1, one name),
                         END_OF_FILE (255)
    fixture-unknown.sbt  negative control: a valid 12 record followed by an
                         unknown record type (0x42) -- must be rejected
    fixture.sbc          SBC0, one 2x2 lightmap, 4 texels
    fixture.sbm          one 8x8 8-bit texture, one mip level (palette is
                         external: the game's shared VGA palette is not part
                         of the blob)
    fixture.sob          one 3-vertex / 1-face model, texture "FIXTURE"
    fixture.soo          1-frame animation (mode 0, 100 ticks)
    fixture.sml          a minimal ``<d>..</d>`` markup document
    fixture.idx/.dat     a one-entry BIGFILE archive holding fixture.sbm

``--check`` re-reads every file through the *oracle* (``gm.*``) and requires:

* the size and every field to match what the fixture is supposed to contain;
* ``parse -> to_bytes`` to be byte-exact (the writers are structural, not
  "keep the input bytes", except where the format has no serialiser: ``.sml``
  is verified as text identity plus the ``.sme`` cipher inverse);
* unknown/auxiliary fields to survive the round-trip (``.sbw`` polygon
  ``f74``/``f76``/``f7a`` and the 112-byte raw tail, ``.sbt`` record ``raw``
  payloads and the unused ``SOUND_SAMPLE_TABLE`` name slots, ``.sbm`` trailing
  bytes);
* ``fixture-unknown.sbt`` to be rejected with an explicit error -- neither
  reader preserves an unknown *record* type (see ``docs/FIXTURES.md``).

``--engine`` runs the C++ engine on the same directory:

* ``gunmetal -g DIR --dump FILE`` for every fixture (numbers must equal the
  Python oracle's numbers);
* ``gunmetal -g DIR -l fixture --info`` for the level trio (world/entities/
  lightmaps counts);
* the engine on every corrupted probe from ``--self-test`` must fail with
  ``[error]`` and a non-zero exit code (no silent success).

Exit status: ``0`` everything verified, ``1`` a value mismatch, ``2`` a hard
error (missing file/binary, format rejected, corrupted input accepted).
Stdlib only.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import bigfile, sbc, sbm, sbt, sbw, sml, sob, soo  # noqa: E402

LEVEL = "fixture"
SBW_TEXTURE = "FIXTURE"          # one name in the .sbw texture table
SBW_TEX_FLAG = 1
SBW_TEX_VALUE = 3                # per-level texture weight (see gm/sbw.py)
SBW_ORIGIN = (128.0, 64.0, 0.0)  # object placement (world = origin + local)
SBW_RADIUS = 45.254834           # |bounds| = |(32, 32, 0)|
SBW_BOUNDS = (32.0, 32.0, 0.0)
POLY_F74 = 0x1234                # auxiliary fields kept verbatim by the
POLY_F76 = 0x5678                # writers; used as preservation probes
POLY_F7A = 0x7A5A
POLY_FLAGS = 0x00008000          # bit 0x8000 = "UV/frame computed"
SAMPLE_NAME = "FIXTURE"
SAMPLE_TAIL = b"\xaa" * 9        # unused name slot 299: opaque bytes
SBM_WIDTH = 8
SBM_HEIGHT = 8
SOO_FRAME = ("fixture", 100)
SML_TEXT = "<d><r><p>Fixture</p></r></d>\r\n"


# --------------------------------------------------------------------------
# builders -- each one writes the smallest structurally valid file
# --------------------------------------------------------------------------


def build_sbw() -> bytes:
    """One sector, one object, 4 vertices, one 3-index polygon."""
    header = struct.pack("<12d", *SBW_ORIGIN,
                         1.0, 0.0, 0.0,     # basis row 0
                         0.0, 1.0, 0.0,     # basis row 1
                         0.0, 0.0, 1.0)     # basis row 2
    assert len(header) == 96
    vertices = ((0.0, 0.0, 0.0), (64.0, 0.0, 0.0),
                (64.0, 64.0, 0.0), (0.0, 64.0, 0.0))
    # 0x60..0x91: radius, f75, f74, f7c, f78, AABB half-extents, 24 zero bytes.
    tail = struct.pack("<fBBII3f", SBW_RADIUS, 0, 0, 0, 0, *SBW_BOUNDS)
    tail += b"\0" * 24
    assert len(tail) == 50
    # Polygon tail: disk order == engine offsets 0x00..0x6f (112 bytes).
    poly_tail = struct.pack(
        "<6H2i18f3HII3H",
        0, 0, 0, 0, 0, 0,                 # scroll_u/v, u/v_scale, uv_u/v
        0, 0,                             # uv_u_fp, uv_v_fp (signed)
        32.0, 32.0, 0.0,                  # centre
        0.0, 0.0, 1.0,                    # normal (unit)
        1.0, 0.0, 0.0,                    # u_axis (unit)
        0.0, 1.0, 0.0,                    # v_axis (unit)
        0.0, 0.0, 0.0,                    # corner_a
        64.0, 64.0, 0.0,                  # corner_b
        POLY_F74, POLY_F76, 0,            # f74, f76, handle
        0,                                # reserved
        POLY_FLAGS,                       # flags
        0, 0, 0)                          # anim0, anim1, anim2
    assert len(poly_tail) == 112
    poly = struct.pack("<5H", 1, 0, 0, 0, POLY_F7A)  # texture/lightmap/material/
    poly += bytes((0, 3))                            # overlay/f7a; brightness/n_idx
    poly += struct.pack("<3H", 0, 1, 2)              # vertex indices
    poly += poly_tail
    assert len(poly) == 124 + 2 * 3

    out = bytearray()
    out += struct.pack("<I", 1) + SBW_TEXTURE.encode() + b"\0"
    out += struct.pack("<HI", SBW_TEX_FLAG, SBW_TEX_VALUE)
    out += struct.pack("<I", 0)                       # effect table: empty
    out += struct.pack("<I", 1)                       # sectors
    out += struct.pack("<IHB", 1, 0, 0)               # objects, kind, sub
    out += struct.pack("<I", 1) + header              # object id + transform
    out += struct.pack("<HH", len(vertices), 1)       # stage 2 geometry
    for v in vertices:
        out += struct.pack("<3f", *v)
    out += tail + poly
    return bytes(out)


def _sbt_header() -> bytes:
    return sbt.MAGIC + b"\0" * (80 - len(sbt.MAGIC)) + struct.pack("<I", 0)


def _sbt_sample_table() -> bytes:
    counts = [0] * sbt.SAMPLE_SLOTS
    counts[0] = 1
    names = bytearray(sbt.SAMPLE_SLOTS * 9)
    names[:9] = SAMPLE_NAME.encode() .ljust(9, b"\0")
    names[299 * 9:] = SAMPLE_TAIL
    return struct.pack("<300I", *counts) + bytes(names)


def _sbt_script() -> bytes:
    # SCRIPT (type 11): count=0 things, n=1 handle, 3 columns of 1 u32,
    # m=1 byte of code (END = 0xFF), flag=0.
    return (struct.pack("<I", 0) + struct.pack("<I", 1)
            + struct.pack("<3I", 1, 0, 0) + struct.pack("<I", 1)
            + b"\xff" + b"\x00")


def build_sbt() -> bytes:
    return (_sbt_header()
            + b"\x0b" + _sbt_script()
            + b"\x0c" + _sbt_sample_table()
            + b"\xff")


def build_sbt_unknown() -> bytes:
    """A well-formed sample table followed by an unknown record type (0x42)."""
    return _sbt_header() + b"\x0c" + _sbt_sample_table() + b"\x42" + b"\xff"


def build_sbc() -> bytes:
    pixels = bytes((0, 32, 64, 96))
    return (b"SBC0" + struct.pack("<I", 1) + struct.pack("<I", (1) | (1 << 16))
            + pixels)


def build_sbm() -> bytes:
    pixels = bytes(((x * 9 + y * 17) & 0xFF)
                   for y in range(SBM_HEIGHT) for x in range(SBM_WIDTH))
    return sbm.encode(SBM_WIDTH, SBM_HEIGHT, pixels, levels=1)


def build_sob() -> bytes:
    face = sob.SobFace(
        textures=["FIXTURE", "none", "none", "none",
                  "none", "none", "none", "none"],
        flags=96,
        indices=[0, 1, 2],
        origin=(0.0, 0.0, 0.0),
        u=(65536.0, 0.0, 0.0),
        v=(0.0, 65536.0, 0.0),
        normal=(0.0, 0.0, 65536.0),
        spare=(0.0, 0.0, 0.0),
    )
    model = sob.Sob(vertices=[(0.0, 0.0, 0.0), (64.0, 0.0, 0.0),
                              (0.0, 64.0, 0.0)],
                    faces=[face], name="fixture")
    return model.to_bytes()


def build_soo() -> bytes:
    return soo.Soo(mode=0, count=1, frames=[soo.Frame(*SOO_FRAME)],
                   combined=False, eol="\r\n", trailing=True).to_bytes()


def build_sml() -> bytes:
    return SML_TEXT.encode("latin1")


def build_bigfile() -> tuple[bytes, bytes]:
    """One-entry BIGFILE index + DAT holding fixture.sbm."""
    dat = build_sbm()
    idx = (bigfile.MAGIC + b"\x1a" + struct.pack("<I", 1)
           + b"FIXTURE\0" + struct.pack("<I", 0))
    return idx, dat


FIXTURES = (
    ("fixture.sbw", build_sbw),
    ("fixture.sbt", build_sbt),
    ("fixture-unknown.sbt", build_sbt_unknown),
    ("fixture.sbc", build_sbc),
    ("fixture.sbm", build_sbm),
    ("fixture.sob", build_sob),
    ("fixture.soo", build_soo),
    ("fixture.sml", build_sml),
)


# --------------------------------------------------------------------------
# structural corruption probes (one flipped byte each)
# --------------------------------------------------------------------------


def _geometry_offset(data: bytes) -> int:
    """File offset of the only object's stage-2 header (n_verts/n_polys)."""
    return len(data) - (4 + 4 * 12 + 50 + 124 + 2 * 3)


def corruption_probes() -> list[tuple[str, int, int, str, bool]]:
    """``(file, offset, xor_mask, what, engine_rejects)`` -- one structural byte.

    Payload bytes (sbm pixels, sbc texels, sob coordinates) are deliberately
    not probed: flipping them yields an equally valid file, and calling that
    "detected" would be a lie.  The probe targets a byte that a conforming
    reader must reject.

    ``engine_rejects`` is ``False`` only where the engine's ``--dump`` path is
    known to be lenient: an invalid ``.sml`` falls back to a tag scan and still
    exits 0 (``engine/src/main.cpp``), so only the oracle can catch it.  That
    is recorded as a limitation, not hidden (see ``docs/FIXTURES.md``).
    """
    sbw_data = build_sbw()
    off = _geometry_offset(sbw_data)
    assert struct.unpack_from("<HH", sbw_data, off) == (4, 1)
    sob_data = build_sob()
    assert sob_data.startswith(b" 3 ")
    probes = [
        ("fixture.sbw", off + 1, 0x01, "vertex count high byte 0 -> 1 (truncated)", True),
        ("fixture.sbt", 0, 0x01, "magic 'BINARY SAVE' -> 'CINARY SAVE'", True),
        ("fixture-unknown.sbt", 0, 0x01, "magic byte", True),
        ("fixture.sbc", 0, 0x0B, "magic 'SBC0' -> 'XBC0'", True),
        ("fixture.sbm", 4, 0x0E, "log2(width) 3 -> 13 (implausible)", True),
        ("fixture.sob", 1, 0x0A, "vertex count '3' -> '9' (truncated list)", True),
        ("fixture.soo", 0, 0x48, "mode '0' -> 'x' (header not integers)", True),
        ("fixture.sml", 0, 0x44, "first '<' of <d> -> 'x' (no tags)", False),
        ("fixture.idx", 20, 0xFF, "entry 0 offset 0 -> 255 (beyond DAT)", True),
    ]
    return probes


# --------------------------------------------------------------------------
# --write
# --------------------------------------------------------------------------


def write_fixtures(dirpath: str) -> dict:
    os.makedirs(dirpath, exist_ok=True)
    written = {}
    for name, builder in FIXTURES:
        data = builder()
        path = os.path.join(dirpath, name)
        with open(path, "wb") as fh:
            fh.write(data)
        written[name] = {"size": len(data),
                         "sha256": hashlib.sha256(data).hexdigest()}
    idx, dat = build_bigfile()
    for name, data in (("fixture.idx", idx), ("fixture.dat", dat)):
        path = os.path.join(dirpath, name)
        with open(path, "wb") as fh:
            fh.write(data)
        written[name] = {"size": len(data),
                         "sha256": hashlib.sha256(data).hexdigest()}
    return written


# --------------------------------------------------------------------------
# --check (oracle side)
# --------------------------------------------------------------------------


class CheckFailed(Exception):
    """A fixture does not contain what it claims to contain."""


def require(cond: bool, message: str) -> None:
    if not cond:
        raise CheckFailed(message)


def _equal(actual, expected, what: str) -> None:
    require(actual == expected, f"{what}: expected {expected!r}, got {actual!r}")


def check_sbw(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    w = sbw.parse(data)
    _equal(len(w.tex), 1, "texture count")
    _equal((w.tex[0].name, w.tex[0].flag, w.tex[0].value),
           (SBW_TEXTURE, SBW_TEX_FLAG, SBW_TEX_VALUE), "texture[0]")
    _equal(len(w.effects), 0, "effect count")
    _equal(len(w.sectors), 1, "sector count")
    sector = w.sectors[0]
    _equal((sector.kind, sector.sub), (0, 0), "sector kind/sub")
    _equal(len(sector.objects), 1, "objects in sector 0")
    obj = sector.objects[0]
    _equal(obj.id, 1, "object id")
    _equal(obj.points[0], SBW_ORIGIN, "object origin (12 x f64 header)")
    _equal(obj.points[1:], ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                            (0.0, 0.0, 1.0)), "object basis")
    _equal(len(obj.vertices), 4, "vertex count")
    _equal(len(obj.polys), 1, "polygon count")
    poly = obj.polys[0]
    _equal(poly.texture, 1, "poly texture")
    _equal((poly.material, poly.lightmap, poly.overlay), (0, 0, 0),
           "poly material/lightmap/overlay")
    _equal(poly.indices, (0, 1, 2), "poly indices")
    _equal(poly.brightness, 0, "poly brightness")
    _equal(poly.normal, (0.0, 0.0, 1.0), "poly normal")
    _equal(poly.u_axis, (1.0, 0.0, 0.0), "poly u_axis")
    _equal(poly.v_axis, (0.0, 1.0, 0.0), "poly v_axis")
    _equal(poly.centre, (32.0, 32.0, 0.0), "poly centre")
    # Unknown/auxiliary fields: the writer keeps the 112-byte raw tail and the
    # object's 50-byte tail verbatim, so these must come back unchanged.
    _equal((poly.f74, poly.f76, poly.f7a), (POLY_F74, POLY_F76, POLY_F7A),
           "poly auxiliary fields (f74/f76/f7a)")
    _equal(poly.flags, POLY_FLAGS, "poly flags")
    _equal(poly.reserved, 0, "poly reserved")
    _equal(struct.unpack_from("<IH", poly.raw, 102)[0], POLY_FLAGS,
           "raw tail keeps flags at +0x66")
    _equal(obj.reserved, b"\0" * 24, "object 24-byte reserved block")
    require(len(poly.raw) == 112, f"poly raw tail is {len(poly.raw)} bytes, want 112")
    require(w.data_end == len(data), f"data end {w.data_end} != size {len(data)}")
    require(w.to_bytes() == data, "parse -> to_bytes is not byte-exact")
    return {"size": len(data), "textures": len(w.tex), "effects": len(w.effects),
            "sectors": len(w.sectors), "objects": w.n_objects,
            "vertices": sum(len(o.vertices) for s in w.sectors for o in s.objects),
            "polygons": w.n_polys, "tables_end": w.table2_end,
            "data_end": w.data_end}


def check_sbt(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    level = sbt.parse(data)
    _equal(level.version, 0, "sbt version")
    _equal([r.type for r in level.records],
           [11, 12, sbt.END_OF_FILE], "record types")
    _equal(level.header, _sbt_header(), "84-byte header")
    script = level.records_of(11)[0].payload
    _equal((script.count, script.n, script.m, script.flag), (0, 1, 1, 0),
           "SCRIPT header")
    _equal(script.things, b"", "SCRIPT things")
    _equal(script.code, b"\xff", "SCRIPT code (END)")
    require(len(script.handles) == 12, "SCRIPT handles are not 3 x 1 u32")
    _equal(script.encode(), level.records_of(11)[0].raw, "SCRIPT structural encode")
    table = level.records_of(12)[0].payload
    _equal(table.counts[0], 1, "sample slot 0 refcount")
    _equal(sum(table.counts), 1, "sample refcount total")
    _equal(table.names[0], SAMPLE_NAME, "sample name 0")
    _equal(len(table.names), sbt.SAMPLE_SLOTS, "sample name slots")
    # The unused slot is opaque: only the raw 0xa8c region preserves it.
    _equal(table.name_slots[299 * 9:], SAMPLE_TAIL, "sample slot 299 raw bytes")
    _equal(level.records_of(12)[0].raw, sbt.encode_payload(12, table),
           "SOUND_SAMPLE_TABLE structural encode")
    require(level.to_bytes() == data, "parse -> to_bytes is not byte-exact")
    require(level.rebuild(verify=True) == data,
            "rebuild(verify=True) is not byte-exact")
    return {"size": len(data), "records": len(level.records),
            "version": level.version, "script_things": script.count,
            "script_handles": script.n, "script_code": script.m,
            "sample_count": sum(table.counts)}


def check_sbt_unknown(path: str) -> dict:
    """The negative control: an unknown record type must be rejected."""
    with open(path, "rb") as fh:
        data = fh.read()
    try:
        sbt.parse(data)
    except ValueError as exc:
        message = str(exc)
        require("record type" in message,
                f"rejected for the wrong reason: {message}")
        return {"size": len(data), "rejected": True, "error": message}
    raise CheckFailed("unknown record type 0x42 was accepted silently")


def check_sbc(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    blob = sbc.parse(data)
    _equal(blob.magic, b"SBC0", "magic")
    _equal(blob.count, 1, "record count")
    rec = blob.records[0]
    _equal((rec.width, rec.height, len(rec.pixels)), (2, 2, 4), "record 0")
    _equal(rec.pixels, bytes((0, 32, 64, 96)), "record 0 pixels")
    require(blob.to_bytes() == data, "parse -> to_bytes is not byte-exact")
    return {"size": len(data), "magic": "SBC0", "records": blob.count,
            "texels": sum(len(r.pixels) for r in blob.records),
            "first": f"{rec.width}x{rec.height}"}


def check_sbm(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    tex = sbm.parse(data)
    _equal((tex.width, tex.height, tex.levels, tex.flags),
           (SBM_WIDTH, SBM_HEIGHT, 1, 0), "dimensions/levels/flags")
    _equal(len(tex.pixels), SBM_WIDTH * SBM_HEIGHT, "level-0 pixel count")
    _equal(tex.pixels, build_sbm()[12:], "level-0 pixels")
    _equal(tex.extra, b"", "trailing bytes")
    require(tex.to_bytes() == data, "parse -> to_bytes is not byte-exact")
    return {"size": len(data), "dimensions": f"{tex.width}x{tex.height}",
            "levels": tex.levels, "flags": tex.flags,
            "mip0": f"{tex.width}x{tex.height}"}


def check_sob(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    model = sob.parse_data(data)
    _equal(len(model.vertices), 3, "vertex count")
    _equal(len(model.faces), 1, "face count")
    face = model.faces[0]
    _equal(face.texture, "FIXTURE", "face texture")
    _equal(face.indices, [0, 1, 2], "face indices")
    _equal(face.flags, 96, "face flags")
    _equal(model.to_bytes(), data, "canonical text round-trip")
    require(sob.parse_data(model.to_bytes()).to_bytes() == data,
            "second parse/write pass is not stable")
    return {"size": len(data), "vertices": len(model.vertices),
            "faces": len(model.faces), "texture": face.texture}


def check_soo(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    anim = soo.parse(data)
    _equal((anim.mode, anim.count, anim.loop), (0, 1, False),
           "mode/count/loop")
    _equal([(f.name, f.duration) for f in anim.frames], [SOO_FRAME], "frames")
    _equal(anim.dropped_frames, [], "dropped frames")
    require(anim.to_bytes() == data, "parse -> to_bytes is not byte-exact")
    return {"size": len(data), "mode": anim.mode, "declared": anim.count,
            "frames": len(anim.frames),
            "first": f"{anim.frames[0].name} {anim.frames[0].duration} ticks"}


def check_sml(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()
    text = data.decode("latin1")
    _equal(text, SML_TEXT, "text")
    tags = sml.tags(text)
    _equal([str(t) for t in tags],
           ["<d>", "<r>", "<p>", "</p>", "</r>", "</d>"], "tag stream")
    require(tags and not tags[0].close and tags[0].name.lower() == "d",
            "document does not start with <d>")
    require(tags[-1].close and tags[-1].name.lower() == "d",
            "document does not end with </d>")
    require(sml.tag_letters(text) <= sml.SHIPPED_TAGS,
            f"unknown tag letters {sorted(sml.tag_letters(text) - sml.SHIPPED_TAGS)}")
    _equal(sml.plain(text).strip(), "Fixture", "plain text")
    # No serialiser exists for markup (the bytes are the document), so the
    # round-trip is the text identity plus the .sme cipher inverse.
    require(sml.encode(sml.decode(data)) == data,
            "sml.encode(sml.decode(x)) is not the identity")
    return {"size": len(data), "chars": len(text),
            "lines": text.count("\n") + 1, "tags": len(tags)}


def check_bigfile(path: str) -> dict:
    idx_path = path
    dat_path = os.path.join(os.path.dirname(path), "fixture.dat")
    archive = bigfile.BigFile(idx_path, dat_path)
    try:
        _equal(archive.count, 1, "entry count")
        entry = archive.entries[0]
        _equal(entry.name, "FIXTURE", "entry 0 name")
        _equal((entry.offset, entry.size), (0, len(build_sbm())), "entry 0 range")
        _equal(archive.read(entry), build_sbm(), "entry 0 payload")
        with open(idx_path, "rb") as fh:
            idx = fh.read()
        # The writer side: rebuild the index from the parsed entries and
        # require the same bytes (parse -> to_bytes for the container).
        rebuilt = (bigfile.MAGIC + b"\x1a" + struct.pack("<I", archive.count)
                   + b"".join(e.name.encode().ljust(8, b"\0")
                              + struct.pack("<I", e.offset)
                              for e in archive.entries))
        _equal(rebuilt, idx, "index rebuild")
        return {"size": len(idx), "entries": archive.count,
                "data": os.path.getsize(dat_path),
                "first": f"{entry.name} ({entry.size} bytes)"}
    finally:
        archive.close()


CHECKERS = {
    "fixture.sbw": check_sbw,
    "fixture.sbt": check_sbt,
    "fixture-unknown.sbt": check_sbt_unknown,
    "fixture.sbc": check_sbc,
    "fixture.sbm": check_sbm,
    "fixture.sob": check_sob,
    "fixture.soo": check_soo,
    "fixture.sml": check_sml,
    "fixture.idx": check_bigfile,
}


# --------------------------------------------------------------------------
# --check (driver)
# --------------------------------------------------------------------------


def check_dir(dirpath: str, quiet: bool = False) -> dict:
    report = {"dir": os.path.abspath(dirpath), "files": {}, "errors": [],
              "ok": True, "hard": False}
    for name, checker in CHECKERS.items():
        path = os.path.join(dirpath, name)
        entry = {"ok": False}
        try:
            facts = checker(path)
        except FileNotFoundError:
            message = f"{name}: no such file"
            report["hard"] = True
        except (CheckFailed, ValueError) as exc:
            message = f"{name}: {exc}"
        except OSError as exc:
            message = f"{name}: {exc}"
            report["hard"] = True
        else:
            entry.update(facts)
            entry["ok"] = True
        if entry["ok"]:
            report["files"][name] = entry
            if not quiet:
                detail = " ".join(
                    f"{k}={v}" for k, v in entry.items()
                    if k not in ("ok", "size", "sha256"))
                print(f"[ok  ] {name:<20} {entry['size']:>5} bytes  {detail}")
        else:
            report["ok"] = False
            report["errors"].append(message)
            report["files"][name] = {"ok": False, "error": message}
            tag = "[error]" if report["hard"] else "[FAIL]"
            print(f"{tag} {message}")
    return report


# --------------------------------------------------------------------------
# --engine
# --------------------------------------------------------------------------


def find_binary(explicit: str | None) -> str | None:
    candidates = [explicit,
                  os.environ.get("GUNMETAL_ENGINE"),
                  os.path.join(ROOT, "build", "sandbox", "engine", "gunmetal"),
                  os.path.expanduser(
                      "~/gunmetal-re/build/sandbox/engine/gunmetal")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def run_engine(binary: str, args: list[str]) -> subprocess.CompletedProcess:
    try:
        return subprocess.run([binary, *args], capture_output=True, text=True,
                              timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return subprocess.CompletedProcess([binary, *args], -1, "",
                                           f"[error] engine: {exc}")


def _dump_fields(stdout: str) -> dict:
    """Split ``--dump`` output into ``key -> value``.

    Several lines carry more than one field (``chars: 30 lines: 2 tags: 6``),
    and values themselves contain spaces (``first: FIXTURE (76 bytes)``), so
    the split is anchored on the start of a line or a space before a
    lowercase key.
    """
    field = re.compile(r"(?:^|(?<= ))([a-z][a-z0-9 ]*?): ")
    fields = {}
    for line in stdout.splitlines():
        matches = list(field.finditer(line))
        for i, match in enumerate(matches):
            end = matches[i + 1].start() if i + 1 < len(matches) else len(line)
            fields[match.group(1).strip()] = line[match.end():end].strip()
    return fields


def _sbw_stage1_end(world: sbw.Sbw) -> int:
    """End of the sector/object header stage (the engine's ``header_end``).

    ``Sbw.table2_end`` only covers the texture/effect tables; the C++ reader
    also counts the sector + object transform blocks before stage 2.
    """
    end = world.table2_end + 4
    for sector in world.sectors:
        end += 7 + 100 * len(sector.objects)
    return end


def _engine_expectations(dirpath: str) -> dict:
    """Numbers the engine must print, taken from the Python oracle."""
    with open(os.path.join(dirpath, "fixture.sbw"), "rb") as fh:
        world = sbw.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sbt"), "rb") as fh:
        entities = sbt.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sbc"), "rb") as fh:
        lightmaps = sbc.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sbm"), "rb") as fh:
        texture = sbm.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sob"), "rb") as fh:
        model = sob.parse_data(fh.read())
    with open(os.path.join(dirpath, "fixture.soo"), "rb") as fh:
        anim = soo.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sml"), "rb") as fh:
        markup = fh.read().decode("latin1")
    archive = bigfile.BigFile(os.path.join(dirpath, "fixture.idx"),
                              os.path.join(dirpath, "fixture.dat"))
    try:
        entry = archive.entries[0]
        big = {"entries": str(archive.count), "data": str(archive.dat_size),
               "first": f"{entry.name} ({entry.size} bytes)"}
    finally:
        archive.close()
    polygons = sum(len(o.polys) for s in world.sectors for o in s.objects)
    vertices = sum(len(o.vertices) for s in world.sectors for o in s.objects)
    return {
        "fixture.sbw": {
            "kind": "SBW level",
            "textures": str(len(world.tex)),
            "effects": str(len(world.effects)),
            "sectors": str(len(world.sectors)),
            "objects": str(world.n_objects),
            "vertices": str(vertices),
            "polygons": str(polygons),
            "header": f"{_sbw_stage1_end(world)} bytes",
            "data end": f"{world.data_end} bytes",
        },
        "fixture.sbt": {
            "kind": "SBT level",
            "version": str(entities.version),
            "records": str(len(entities.records)),
            "poly handles": "0",
            "object handles": "0",
            "sector handles": "0",
            "sobs": "0",
            "script": "things=0 handles=1 code=1 flag=0",
        },
        "fixture.sbc": {
            "kind": "SBC lightmaps",
            "magic": "SBC0",
            "records": str(len(lightmaps.records)),
            "texels": str(sum(len(r.pixels) for r in lightmaps.records)),
            "first": f"{lightmaps.records[0].width}x{lightmaps.records[0].height}",
        },
        "fixture.sbm": {
            "kind": "SBM texture",
            "dimensions": f"{texture.width}x{texture.height}",
            "levels": str(texture.levels),
            "flags": str(texture.flags),
            "mip0": f"{texture.width}x{texture.height}",
        },
        "fixture.sob": {
            "kind": "SOB model",
            "vertices": str(len(model.vertices)),
            "faces": str(len(model.faces)),
            "first texture": model.faces[0].texture,
        },
        "fixture.soo": {
            "kind": "SOO model animation",
            "mode": str(anim.mode),
            "declared frames": str(anim.count),
            "frames": str(len(anim.frames)),
            "first": f"{anim.frames[0].name} {anim.frames[0].duration} ticks",
        },
        "fixture.sml": {
            "kind": "SML markup",
            "note": "plain",
            "chars": str(len(markup)),
            "lines": str(markup.count("\n") + 1),
            "tags": str(len(sml.tags(markup))),
            "images": "0",
            "fonts": "0",
            "music": "0",
            "text runs": "1",
            "document": "ok",
        },
        "fixture.idx": {
            "kind": "BIGFILE index",
            "entries": big["entries"],
            "data": f"{big['data']} bytes",
            "first": big["first"],
        },
    }


def _engine_info(dirpath: str) -> dict:
    with open(os.path.join(dirpath, "fixture.sbw"), "rb") as fh:
        world = sbw.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sbt"), "rb") as fh:
        entities = sbt.parse(fh.read())
    with open(os.path.join(dirpath, "fixture.sbc"), "rb") as fh:
        lightmaps = sbc.parse(fh.read())
    vertices = sum(len(o.vertices) for s in world.sectors for o in s.objects)
    script = entities.records_of(11)[0].payload
    histogram = {}
    for record in entities.records:
        histogram[record.type] = histogram.get(record.type, 0) + 1
    return {
        "world": (f"textures={len(world.tex)} effects={len(world.effects)} "
                  f"sectors={len(world.sectors)} objects={world.n_objects} "
                  f"vertices={vertices} polygons={world.n_polys} "
                  f"bytes={os.path.getsize(os.path.join(dirpath, 'fixture.sbw'))}"),
        "entities": (f"records={len(entities.records)} poly=0 object=0 sector=0 "
                     f"sobs=0 script={script.n}/{script.m}"),
        "record types": " ".join(
            f"{t}:{histogram[t]}" for t in sorted(histogram)),
        "lightmaps": (f"magic={lightmaps.magic.decode()} "
                      f"records={len(lightmaps.records)} "
                      f"texels={sum(len(r.pixels) for r in lightmaps.records)}"),
    }


def engine_check(dirpath: str, binary: str, quiet: bool = False) -> dict:
    report = {"binary": binary, "checks": {}, "errors": [], "ok": True}
    expectations = _engine_expectations(dirpath)
    for name, expected in expectations.items():
        path = os.path.join(dirpath, name)
        proc = run_engine(binary, ["-g", dirpath, "--dump", path])
        if proc.returncode != 0:
            report["ok"] = False
            message = (f"{name}: engine rejected (rc={proc.returncode}): "
                       f"{_first_error(proc)}")
            report["errors"].append(message)
            report["checks"][name] = {"ok": False, "error": message}
            print(f"[FAIL] engine {name}: {message}")
            continue
        fields = _dump_fields(proc.stdout)
        wrong = {key: (fields.get(key), value) for key, value in expected.items()
                 if fields.get(key) != value}
        if wrong:
            report["ok"] = False
            detail = "; ".join(f"{k}: engine {a!r} != oracle {e!r}"
                               for k, (a, e) in sorted(wrong.items()))
            report["errors"].append(f"{name}: {detail}")
            report["checks"][name] = {"ok": False, "expected": expected,
                                      "engine": fields, "diff": detail}
            print(f"[FAIL] engine {name}: {detail}")
        else:
            report["checks"][name] = {"ok": True, "expected": expected}
            if not quiet:
                print(f"[ok  ] engine {name:<20} " +
                      " ".join(f"{k}={v}" for k, v in expected.items()))

    # The level trio through the real loader (-l fixture --info).
    expected_info = _engine_info(dirpath)
    proc = run_engine(binary, ["-g", dirpath, "-l", LEVEL, "--info"])
    info_ok = proc.returncode == 0
    info_errors = []
    if info_ok:
        for key, want in expected_info.items():
            match = re.search(rf"^{re.escape(key)}: (.*)$", proc.stdout,
                              re.MULTILINE)
            got = match.group(1).strip() if match else "<missing>"
            if got != want:
                info_ok = False
                info_errors.append(f"{key}: engine {got!r} != oracle {want!r}")
    else:
        info_errors.append(
            f"rc={proc.returncode}: {_first_error(proc)}")
    if info_ok:
        if not quiet:
            print("[ok  ] engine level --info     " +
                  " | ".join(expected_info.values()))
    else:
        report["ok"] = False
        report["errors"].extend(f"level --info: {e}" for e in info_errors)
        for error in info_errors:
            print(f"[FAIL] engine level --info: {error}")
    report["checks"]["level --info"] = {"ok": info_ok,
                                        "expected": expected_info,
                                        "errors": info_errors}
    return report


def _first_error(proc: subprocess.CompletedProcess) -> str:
    for stream in (proc.stderr, proc.stdout):
        for line in (stream or "").splitlines():
            if "[error]" in line:
                return line.strip()
    tail = (proc.stderr or proc.stdout or "").strip().splitlines()
    return tail[-1] if tail else "no output"


def engine_corruption_check(dirpath: str, binary: str,
                            quiet: bool = False) -> dict:
    """The engine must reject every structurally corrupted fixture."""
    report = {"probes": {}, "errors": [], "limitations": [], "ok": True}
    corrupt_dir = os.path.join(dirpath, "corrupt")
    os.makedirs(corrupt_dir, exist_ok=True)
    # The BIGFILE index resolves its DAT next to itself: without the copy the
    # engine would fail for the wrong reason ("data file not found").
    dat_source = os.path.join(dirpath, "fixture.dat")
    if os.path.isfile(dat_source):
        shutil.copyfile(dat_source, os.path.join(corrupt_dir, "fixture.dat"))
    for name, offset, mask, what, engine_rejects in corruption_probes():
        source = os.path.join(dirpath, name)
        with open(source, "rb") as fh:
            data = bytearray(fh.read())
        data[offset] ^= mask
        path = os.path.join(corrupt_dir, name)
        with open(path, "wb") as fh:
            fh.write(data)
        args = ["-g", dirpath, "--dump", path]
        proc = run_engine(binary, args)
        rejected = proc.returncode != 0 and "[error]" in (proc.stderr or "")
        entry = {"what": what, "offset": offset, "rc": proc.returncode,
                 "ok": rejected}
        if rejected:
            if not quiet:
                print(f"[ok  ] engine corrupt {name:<20} rc={proc.returncode} "
                      f"{_first_error(proc)}")
        elif not engine_rejects:
            # Known engine leniency, measured rather than assumed: the dump of
            # an invalid .sml falls back to a tag scan and exits 0.
            message = (f"{name}: engine accepted corruption ({what}) with "
                       f"rc={proc.returncode}; the --dump path has no error "
                       f"branch for this format (oracle rejects it)")
            entry["limitation"] = True
            report["limitations"].append(message)
            if not quiet:
                print(f"[note] {message}")
        else:
            report["ok"] = False
            message = (f"{name}: engine accepted corruption ({what}) with "
                       f"rc={proc.returncode} -- silent skip")
            report["errors"].append(message)
            print(f"[FAIL] {message}")
        report["probes"][name] = entry
    return report


# --------------------------------------------------------------------------
# --self-test (no game, no engine)
# --------------------------------------------------------------------------


def _temp_dir() -> str:
    for candidate in ("/tmp/opencode", "/var/tmp"):
        if os.path.isdir(candidate) and os.access(candidate, os.W_OK):
            return tempfile.mkdtemp(prefix="gm-fixtures-", dir=candidate)
    return tempfile.mkdtemp(prefix="gm-fixtures-")


def self_test(quiet: bool = False) -> dict:
    report = {"checks": {}, "errors": [], "ok": True}

    def ok(name: str, condition: bool, detail: str = "") -> None:
        report["checks"][name] = {"ok": bool(condition), "detail": detail}
        if not condition:
            report["ok"] = False
            report["errors"].append(f"{name}: {detail or 'failed'}")
            if not quiet:
                print(f"[FAIL] {name}: {detail}")
        elif not quiet:
            print(f"[ok  ] {name:<34} {detail}")

    tmp = _temp_dir()
    try:
        written = write_fixtures(tmp)
        ok("write", len(written) == 10, f"{len(written)} files")
        # Determinism: a second write must produce identical bytes.
        second = write_fixtures(tmp)
        ok("write is deterministic",
           all(written[n]["sha256"] == second[n]["sha256"] for n in written),
           "same sha256 on a second write")

        checked = check_dir(tmp, quiet=True)
        ok("check generated set", checked["ok"],
           f"{len(checked['files'])} checked file(s), "
           f"{len(checked['errors'])} error(s)")

        # Bit-rot: every probe must be rejected by the oracle, never accepted.
        bad_dir = os.path.join(tmp, "corrupt")
        os.makedirs(bad_dir, exist_ok=True)
        shutil.copyfile(os.path.join(tmp, "fixture.dat"),
                        os.path.join(bad_dir, "fixture.dat"))
        for name, offset, mask, what, _engine_rejects in corruption_probes():
            data = bytearray(open(os.path.join(tmp, name), "rb").read())
            data[offset] ^= mask
            path = os.path.join(bad_dir, name)
            with open(path, "wb") as fh:
                fh.write(data)
            checker = CHECKERS[name]
            try:
                checker(path)
            except (CheckFailed, ValueError, OSError) as exc:
                ok(f"corrupt {name}", True, f"{what}: {type(exc).__name__}")
            else:
                ok(f"corrupt {name}", False,
                   f"{what}: parser accepted the corrupted file")

        # A payload flip is still a structurally valid file: only the fixture
        # checker (exact content comparison) notices it.  Kept explicit so the
        # "corruption detected" claim stays honest about what each layer does.
        payload = bytearray(open(os.path.join(tmp, "fixture.sbm"), "rb").read())
        payload[20] ^= 0xFF
        payload_path = os.path.join(bad_dir, "fixture-pixel.sbm")
        with open(payload_path, "wb") as fh:
            fh.write(payload)
        parsed_ok = sbm.parse(bytes(payload)).width == SBM_WIDTH
        try:
            check_sbm(payload_path)
        except CheckFailed:
            content_mismatch = True
        else:
            content_mismatch = False
        ok("payload flip: parse ok, content mismatch",
           parsed_ok and content_mismatch,
           "parser accepts any pixel; --check compares the expected bytes")

        # The negative control is part of the checked set already; state the
        # outcome explicitly so the report names the record type.
        try:
            sbt.parse(build_sbt_unknown())
        except ValueError as exc:
            ok("unknown record rejected", "record type" in str(exc), str(exc))
        else:
            ok("unknown record rejected", False, "type 0x42 was accepted")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return report


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Synthetic minimal fixtures for the Gunmetal formats.")
    parser.add_argument("--write", metavar="DIR",
                        help="write the fixture set into DIR")
    parser.add_argument("--check", metavar="DIR",
                        help="verify DIR with the Python oracle (gm.*)")
    parser.add_argument("--engine", action="store_true",
                        help="run the C++ engine on --check DIR / --write DIR")
    parser.add_argument("--binary", metavar="PATH",
                        help="gunmetal binary (default: build/sandbox or "
                             "$GUNMETAL_ENGINE)")
    parser.add_argument("--self-test", action="store_true",
                        help="generate into a temp dir and verify corruption "
                             "is detected (no game, no engine)")
    parser.add_argument("--json", action="store_true",
                        help="print one canonical JSON line")
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures and the summary")
    opts = parser.parse_args(argv)

    if not (opts.write or opts.check or opts.self_test):
        parser.print_help()
        return 2

    # --json promises one machine-readable line: run the steps with stdout
    # captured and print only the JSON document afterwards.
    if opts.json:
        real_stdout = sys.stdout
        sys.stdout = io.StringIO()
        try:
            result, rc = run_steps(opts, quiet=True)
        finally:
            sys.stdout = real_stdout
        result["rc"] = rc
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return rc

    result, rc = run_steps(opts, quiet=opts.quiet)
    print(f"status: {'ok' if result['ok'] else 'FAILED'} (rc={rc})")
    return rc


def run_steps(opts: argparse.Namespace, quiet: bool) -> tuple[dict, int]:
    """Execute the requested steps; returns ``(result, rc)``."""
    result = {"ok": True, "steps": {}}
    rc = 0

    def merge(name: str, report: dict, hard: bool = False) -> None:
        nonlocal rc
        result["steps"][name] = report
        if not report.get("ok", True):
            result["ok"] = False
            rc = max(rc, 2 if hard else 1)

    dirpath = opts.check or opts.write
    if opts.write:
        written = write_fixtures(opts.write)
        if not quiet:
            for name, meta in written.items():
                print(f"[ok  ] wrote {name:<20} {meta['size']:>5} bytes  "
                      f"sha256={meta['sha256'][:16]}")
        result["steps"]["write"] = {"ok": True, "files": written,
                                    "dir": os.path.abspath(opts.write)}

    if opts.check:
        report = check_dir(opts.check, quiet=quiet)
        merge("check", report, hard=report.get("hard", False))
        if not report["ok"] and not quiet:
            print(f"[error] check: {len(report['errors'])} failure(s) in "
                  f"{report['dir']}")

    if opts.engine:
        if not dirpath:
            print("[error] engine: --engine needs --write DIR or --check DIR")
            return 2
        binary = None
        if opts.binary and not (os.path.isfile(opts.binary)
                                and os.access(opts.binary, os.X_OK)):
            problem = f"no such executable: {opts.binary}"
        else:
            binary = find_binary(opts.binary)
            problem = None if binary else "binary not found"
        if problem:
            print(f"[error] engine: {problem} "
                  "(pass --binary or set GUNMETAL_ENGINE)")
            result["steps"]["engine"] = {"ok": False, "error": problem}
            result["ok"] = False
            rc = 2
        else:
            report = engine_check(dirpath, binary, quiet=quiet)
            merge("engine", report, hard=True)
            corrupt = engine_corruption_check(dirpath, binary, quiet=quiet)
            merge("engine-corrupt", corrupt, hard=True)

    if opts.self_test:
        report = self_test(quiet=quiet)
        merge("self-test", report)

    return result, rc


if __name__ == "__main__":
    raise SystemExit(main())
