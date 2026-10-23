#!/usr/bin/env python3
"""Editor for Gunmetal ``.sbw`` world geometry (``M11.2``, slice 1).

``.sbw`` is a single sequential stream (no offset table) read by the loader at
VMA ``0x42aa63``/``0x42ad65``/``0x425f4c`` (``gm/sbw.py`` module docstring,
``docs/FORMATS.md`` §10).  Because every polygon is self-delimiting
(``124 + 2*n_idx`` bytes on disk) and all shared state lives in front of the
polygons, a documented polygon field can be patched **in place**: the length of
the file does not change and no other byte moves.

This tool therefore

* walks the file with its **own** bounds-checked reader (:func:`walk`, no
  ``gm.sbw`` involvement) to locate every polygon on disk,
* patches exactly the bytes of one documented field (:func:`patch_bytes`),
* re-reads the result with the oracle ``gm.sbw`` *and* with the own walker and
  compares **every** other byte of the file (:func:`verify_edit`).

Editable fields (all documented in ``docs/FORMATS.md`` §10.1 / ``gm/sbw.py``):

===========  ========  ============  =========================================
field        disk      engine off    meaning
===========  ========  ============  =========================================
``texture``  ``+0x00``  ``+0x00``    signed: >0 1-based texture table, <0
                                     effect table, 0 untextured
``lightmap`` ``+0x02``  ``+0x04``    0 = none, 1..count of the sibling ``.sbc``
``material`` ``+0x04``  ``+0x02``    0..5 (none/reflective/translucent/varadd/
                                     add/shade)
``overlay``  ``+0x06``  ``+0x78``    overlay texture index, same encoding
``brightness`` ``+0x0A`` ``+0x70``   u8, additive light (SET_POLY_BRIGHTNESS)
``flags``    ``0x72+2*n_idx`` ``+0x7c``  bitfield (0x8000 frame/UV done, 0x2
                                     transparent, 0x4 command-toggled)
===========  ========  ============  =========================================

Everything else (``centre``/``normal``/``u_axis``/``v_axis``/corners/UV), the
object headers, vertices, texture/effect tables and the geometry tail are
preserved verbatim: the tool only ever writes the field's own bytes.

Not editable: the **sector light byte**.  It exists only in the runtime sector
record (in-memory ``+0x16``, signed, ``docs/FORMATS.md`` §10) -- the loader
writes 7 bytes per sector (``u32 objects``, ``u16 kind``, ``u8 sub``) and
initialises ``+0x16`` to 0; the value is set at run time by the script command
whose handler is ``0x4578f8`` (table ``0x4b6784``).  There is no such byte in
``.sbw``, so ``--sector ... --field light`` is refused with an explanation.

Usage::

    python3 tools/editor/sbwedit.py --list level0
    python3 tools/editor/sbwedit.py --list level0 --limit 4 --json
    python3 tools/editor/sbwedit.py --get level0 --poly 0 --field texture
    python3 tools/editor/sbwedit.py --set level0 --poly 0 --field texture=28 \\
        --out /tmp/opencode/sbwedit/level0.sbw
    python3 tools/editor/sbwedit.py --check /tmp/opencode/sbwedit/level0.sbw \\
        --against level0 --poly 0 --field texture=28
    python3 tools/editor/sbwedit.py --roundtrip
    python3 tools/editor/sbwedit.py --self-test
    python3 tools/editor/sbwedit.py --audit
    python3 tools/editor/sbwedit.py --fields

Exit codes: ``0`` success, ``1`` verification failed (check/round-trip/audit/
self-test negative), ``2`` bad input or a refused edit (message on stderr),
``3`` internal error.  No traceback escapes :func:`main`.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys
from dataclasses import dataclass, field as dc_field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbw as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sbwedit"

#: Same trust-boundary caps as the oracle (``gm/sbw.py:359-363``).
MAX_TABLE = 65_536
MAX_OBJECTS = 65_536

#: Documented ``flags`` bits (``docs/FORMATS.md`` §10.1, engine ``+0x7c``).
FLAGS_DOCUMENTED = 0x8000 | 0x2 | 0x4
FLAGS_FRAME_READY = 0x8000


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The file does not walk as a ``.sbw`` (own reader)."""


# ---------------------------------------------------------------------------
# documented polygon fields
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class PolyField:
    """One documented polygon field: disk/engine offset, type and provenance."""

    name: str
    disk: str               # human readable disk offset
    engine: str             # in-memory offset in the 0x80 poly record
    fmt: str                # struct format used for the raw u16/u8/u32
    width: int
    signed: bool            # interpreted as a signed value (texture/overlay)
    doc: str                # documentation
    va: str                 # engine evidence
    meaning: str


FIELDS = {
    "texture": PolyField(
        "texture", "+0x00", "+0x00", "<H", 2, True,
        "docs/FORMATS.md §10.1 (0x00); gm/sbw.py:75-77,196-202",
        "SET_POLY_TEXTURE (script id 0x0f)",
        "signed: 0 untextured, >0 1-based texture table, <0 effect table",
    ),
    "lightmap": PolyField(
        "lightmap", "+0x02", "+0x04", "<H", 2, False,
        "docs/FORMATS.md §10.1 (0x04); gm/sbw.py:89-91",
        "lightmap rebase at load; sampler 0x43ca5a, gm/render.py records[lm-1]",
        "0 = no lightmap, 1..count of the sibling .sbc",
    ),
    "material": PolyField(
        "material", "+0x04", "+0x02", "<H", 2, False,
        "docs/FORMATS.md §10.1 (0x02); gm/sbw.py:78-80",
        "material table ds:0x5670e0, stride 0x40 (name at +0x34)",
        "0..5 = none/reflective/translucent/varadd/add/shade",
    ),
    "overlay": PolyField(
        "overlay", "+0x06", "+0x78", "<H", 2, True,
        "docs/FORMATS.md §10.1 (0x78); gm/sbw.py:82,113-114",
        "same encoding as +0x00 (overlay texture index)",
        "signed overlay texture index (0 = none)",
    ),
    "brightness": PolyField(
        "brightness", "+0x0A", "+0x70", "<B", 1, False,
        "docs/FORMATS.md §10.1 (0x70); gm/sbw.py:109,178",
        "SET_POLY_BRIGHTNESS (script id 0x10)",
        "u8 additive light; shipped 0..255, viewer treats 0..127",
    ),
    "flags": PolyField(
        "flags", "0x72+2*n_idx", "+0x7c", "<I", 4, False,
        "docs/FORMATS.md §10.1 (0x7c); gm/sbw.py:116-119,185",
        "0x43e02d recomputes UV when 0x8000 clear; xor [poly+0x7c],4 at 0x46adbc",
        "bitfield: 0x8000 frame/UV ready, 0x2 transparent, 0x4 toggled",
    ),
}

#: Documented *unavailable* field: proves why ``--sector`` is not offered.
UNAVAILABLE = {
    "sector.light": (
        "runtime-only: in-memory sector record +0x16 (signed byte), "
        "docs/FORMATS.md §10; the .sbw sector header is only "
        "u32 objects + u16 kind + u8 sub (7 bytes), and the loader writes 0 "
        "into +0x16 -- the script command (handler 0x4578f8) sets it at run "
        "time, so there is nothing to patch on disk"
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
class PolyLoc:
    """Where a polygon lives on disk plus the documented fields read from it."""

    index: int                       # global order: sector -> object -> poly
    sector: int
    obj: int
    offset: int                      # disk offset of the 124+2*n_idx record
    n_idx: int
    texture: int                     # raw u16 (signed interpretation: -1 = 0xFFFF)
    lightmap: int
    material: int
    overlay: int
    f7a: int
    brightness: int
    flags: int

    @property
    def size(self) -> int:
        return 124 + 2 * self.n_idx

    @property
    def end(self) -> int:
        return self.offset + self.size

    def span(self, name: str) -> tuple[int, int]:
        """Disk ``(offset, width)`` of a documented field inside this record."""
        if name == "texture":
            return self.offset + 0x00, 2
        if name == "lightmap":
            return self.offset + 0x02, 2
        if name == "material":
            return self.offset + 0x04, 2
        if name == "overlay":
            return self.offset + 0x06, 2
        if name == "brightness":
            return self.offset + 0x0A, 1
        if name == "flags":
            # tail starts after the index list (0x0C + 2*n_idx); the flags
            # word is at tail+0x66 (gm/sbw.py:345 `o + 102`).
            return self.offset + 0x72 + 2 * self.n_idx, 4
        raise EditError(f"unknown polygon field: {name}")


@dataclass
class ObjLoc:
    index: int
    id: int
    header_off: int
    geom_off: int
    n_verts: int
    n_polys: int
    poly_first: int
    poly_last: int
    origin: tuple[float, float, float]


@dataclass
class SectorLoc:
    index: int
    kind: int
    sub: int
    objects: list[ObjLoc] = dc_field(default_factory=list)

    @property
    def n_polys(self) -> int:
        return sum(o.n_polys for o in self.objects)


@dataclass
class Scan:
    size: int
    n_tex: int
    n_eff: int
    tex: list          # [(name, flag, value)]
    effects: list      # [(name, kind, start, ids)]
    sectors: list[SectorLoc]
    polys: list[PolyLoc]
    data_end: int
    table1_end: int
    table2_end: int

    @property
    def n_objects(self) -> int:
        return sum(len(s.objects) for s in self.sectors)


def walk(data: bytes) -> Scan:
    """Parse ``.sbw`` with an independent reader; every read is bounds-checked.

    Layout: ``gm/sbw.py:1-128`` / ``docs/FORMATS.md`` §10.  Raises
    :class:`ScanError` on truncation or an implausible count.
    """
    c = _Cur(data)
    n_tex = c.u32("texture count")
    if n_tex > MAX_TABLE:
        raise ScanError(f"implausible texture count {n_tex}")
    tex = []
    for i in range(n_tex):
        name = c.cstr(f"texture[{i}] name")
        flag = c.u16(f"texture[{i}] flag")
        value = c.u32(f"texture[{i}] value")
        tex.append((name, flag, value))
    table1_end = c.o

    n_eff = c.u32("effect count")
    if n_eff > MAX_TABLE:
        raise ScanError(f"implausible effect count {n_eff}")
    effects = []
    for i in range(n_eff):
        name = c.cstr(f"effect[{i}] name")
        kind = c.u32(f"effect[{i}] kind")
        start = c.u32(f"effect[{i}] start")
        n = c.u32(f"effect[{i}] n")
        if n > MAX_TABLE:
            raise ScanError(f"implausible effect[{i}] id count {n}")
        ids = list(struct.unpack_from("<%dH" % n, c.take(2 * n, f"effect[{i}] ids"),
                                     0)) if n else []
        effects.append((name, kind, start, ids))
    table2_end = c.o

    n_sec = c.u32("sector count")
    if n_sec > MAX_OBJECTS:
        raise ScanError(f"implausible sector count {n_sec}")
    sectors: list[SectorLoc] = []
    poly_index = 0
    for si in range(n_sec):
        n_obj = c.u32(f"sector[{si}] object count")
        if n_obj > MAX_OBJECTS:
            raise ScanError(f"implausible object count {n_obj} in sector {si}")
        kind = c.u16(f"sector[{si}] kind")
        sub = c.u8(f"sector[{si}] sub")
        sect = SectorLoc(si, kind, sub)
        for oi in range(n_obj):
            header_off = c.o
            oid = c.u32(f"sector[{si}] object[{oi}] flags word")
            header = c.take(96, f"sector[{si}] object[{oi}] header")
            origin = struct.unpack_from("<3d", header, 0)
            sect.objects.append(ObjLoc(oi, oid, header_off, 0, 0, 0, -1, -1,
                                       tuple(origin)))
        sectors.append(sect)

    polys: list[PolyLoc] = []
    for sect in sectors:
        for obj in sect.objects:
            obj.geom_off = c.o
            obj.n_verts = c.u16(f"sector[{sect.index}] object[{obj.index}] n_verts")
            obj.n_polys = c.u16(f"sector[{sect.index}] object[{obj.index}] n_polys")
            c.take(12 * obj.n_verts,
                   f"sector[{sect.index}] object[{obj.index}] vertices")
            c.take(50, f"sector[{sect.index}] object[{obj.index}] tail")
            obj.poly_first = poly_index
            for _ in range(obj.n_polys):
                off = c.o
                texture = c.u16("poly texture")
                lightmap = c.u16("poly lightmap")
                material = c.u16("poly material")
                overlay = c.u16("poly overlay")
                f7a = c.u16("poly f7a")
                brightness = c.u8("poly brightness")
                n_idx = c.u8("poly n_idx")
                c.take(2 * n_idx, "poly indices")
                tail = c.take(112, "poly tail")
                flags = struct.unpack_from("<I", tail, 0x66)[0]
                polys.append(PolyLoc(poly_index, sect.index, obj.index, off,
                                     n_idx, texture, lightmap, material,
                                     overlay, f7a, brightness, flags))
                poly_index += 1
            obj.poly_last = poly_index - 1
    return Scan(len(data), n_tex, n_eff, tex, effects, sectors, polys, c.o,
                table1_end, table2_end)


def poly_values(p: PolyLoc) -> dict:
    """Documented field values of a polygon (signed where documented)."""
    return {
        "texture": _signed16(p.texture),
        "material": p.material,
        "lightmap": p.lightmap,
        "brightness": p.brightness,
        "overlay": _signed16(p.overlay),
        "flags": p.flags,
    }


def _signed16(v: int) -> int:
    return v - 0x10000 if v >= 0x8000 else v


# ---------------------------------------------------------------------------
# field values: parse, validate, patch
# ---------------------------------------------------------------------------

def parse_int(text: str) -> int:
    """``12``, ``0x8000``, ``-1``; everything else is refused."""
    t = text.strip()
    try:
        return int(t, 0)
    except ValueError:
        raise EditError(f"not an integer: {text!r}") from None


def validate(name: str, value: int, n_tex: int, n_eff: int,
             n_lightmaps: int | None) -> None:
    """Check a value against the field's documented range.

    ``n_lightmaps`` is the record count of the sibling ``.sbc`` (``None`` when
    it is unknown -- then the lightmap range cannot be proven and the edit is
    refused instead of guessed).
    """
    if name == "texture" or name == "overlay":
        if value == 0:
            return
        if value > n_tex:
            raise EditError(
                f"{name}={value}: texture table has {n_tex} entries "
                f"(1-based, docs/FORMATS.md §10.1)")
        if value < -n_eff:
            raise EditError(
                f"{name}={value}: effect table has {n_eff} entries "
                f"(negative values index it, docs/FORMATS.md §10.1)")
        return
    if name == "material":
        if not 0 <= value <= 5:
            raise EditError(
                f"material={value}: documented range 0..5 "
                f"(none/reflective/translucent/varadd/add/shade, "
                f"table ds:0x5670e0)")
        return
    if name == "lightmap":
        if n_lightmaps is None:
            raise EditError(
                "lightmap: the sibling .sbc was not found, so the valid range "
                "(0..record count) is unknown; pass --sbc FILE")
        if not 0 <= value <= n_lightmaps:
            raise EditError(
                f"lightmap={value}: 0 = none, 1..{n_lightmaps} = .sbc records "
                f"(sampler reads records[lightmap-1])")
        return
    if name == "brightness":
        if not 0 <= value <= 0xFF:
            raise EditError(f"brightness={value}: u8 field, range 0..255")
        return
    if name == "flags":
        if not 0 <= value <= 0xFFFFFFFF:
            raise EditError(f"flags={value}: u32 field, range 0..0xFFFFFFFF")
        unknown = value & ~FLAGS_DOCUMENTED
        if unknown:
            raise EditError(
                f"flags=0x{value:08x}: undocumented bits 0x{unknown:08x}; "
                f"documented mask is 0x{FLAGS_DOCUMENTED:08x} "
                f"(0x8000 frame/UV ready, 0x2 transparent, 0x4 toggled)")
        return
    raise EditError(f"unknown polygon field: {name}")


def patch_bytes(data: bytes, poly: PolyLoc, name: str, value: int) -> bytes:
    """Return ``data`` with exactly the field's bytes replaced."""
    field = FIELDS.get(name)
    if field is None:
        raise EditError(f"unknown polygon field: {name}")
    off, width = poly.span(name)
    if off < 0 or off + width > len(data):
        raise EditError(f"{name}: field span {off}..{off + width} is outside "
                        f"the file ({len(data)} bytes)")
    out = bytearray(data)
    raw = value & ((1 << (8 * width)) - 1)
    struct.pack_into(field.fmt, out, off, raw)
    return bytes(out)


def diff_offsets(a: bytes, b: bytes) -> list[int]:
    """Offsets where two equal-length blobs differ."""
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


# ---------------------------------------------------------------------------
# verification (oracle + own walker)
# ---------------------------------------------------------------------------

def _protected_spans(scan: Scan) -> list[tuple[int, int]]:
    """Every byte of the file that is *not* inside a polygon record."""
    spans = []
    cursor = 0
    for p in scan.polys:
        if p.offset < cursor:
            raise ScanError(f"overlapping polygons at {p.offset} < {cursor}")
        spans.append((cursor, p.offset))
        cursor = p.end
    spans.append((cursor, scan.size))
    return spans


def verify_edit(orig: bytes, new: bytes, poly_index: int, name: str,
                expected: int) -> dict:
    """Prove an edit: oracle value changed, every other byte untouched.

    Steps (all must pass):

    1. both blobs parse with the oracle to exactly EOF;
    2. table/sector/object/polygon counts are unchanged;
    3. the bytes outside the target polygon are identical (stage-1 sector and
       object headers, vertices, object tails, other polygons);
    4. inside the file, only the target field's bytes differ;
    5. the oracle sees the new value in the target polygon.
    """
    checks = []
    problems = []
    if name not in FIELDS:
        raise EditError(f"unknown polygon field: {name} "
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
    check((s0.n_tex, s0.n_eff, len(s0.sectors), s0.n_objects, len(s0.polys)) ==
          (s1.n_tex, s1.n_eff, len(s1.sectors), s1.n_objects, len(s1.polys)),
          "own walker: counts unchanged")
    check(s0.size == s1.size, "file size unchanged")

    # every byte outside polygons is identical (headers, vertices, tails)
    outside_ok = all(orig[a:b] == new[a:b] for a, b in _protected_spans(s0))
    check(outside_ok, "bytes outside polygons are identical")

    # every polygon except the target is identical byte for byte
    others_ok = all(orig[p.offset:p.end] == new[p.offset:p.end]
                    for i, p in enumerate(s0.polys) if i != poly_index)
    check(others_ok, f"{len(s0.polys) - 1} other polygons are identical")

    if not 0 <= poly_index < len(s0.polys):
        check(False, f"polygon {poly_index} does not exist")
        return {"ok": False, "checks": checks, "problems": problems}

    target = s0.polys[poly_index]
    off, width = target.span(name)
    changed = diff_offsets(orig, new)
    inside = all(off <= d < off + width for d in changed)
    check(inside, f"only {name} bytes differ (span {off}..{off + width}, "
                  f"{len(changed)} byte(s) changed)")
    check(bool(changed), "the file actually changed")

    # oracle and own reader agree on the new value
    raw_expected = expected & ((1 << (8 * FIELDS[name].width)) - 1)
    check(_raw_value(s1.polys[poly_index], name) == raw_expected,
          f"own walker sees {name}={expected}")
    oraw = _raw_value_oracle(after, poly_index, name)
    check(oraw == raw_expected, f"oracle sees {name}={expected}")

    # the oracle's own serialiser is stable for both blobs
    check(before.to_bytes() == orig, "oracle round-trip of the source is exact")
    check(after.to_bytes() == new, "oracle round-trip of the edit is exact")

    return {
        "ok": all(c["ok"] for c in checks),
        "checks": checks,
        "problems": problems,
        "changes": changed,
        "polygon": poly_index,
        "field": name,
        "value": expected,
    }


def _raw_value(p: PolyLoc, name: str) -> int:
    if name == "texture":
        return p.texture
    if name == "lightmap":
        return p.lightmap
    if name == "material":
        return p.material
    if name == "overlay":
        return p.overlay
    if name == "brightness":
        return p.brightness
    if name == "flags":
        return p.flags
    raise EditError(f"unknown polygon field: {name}")


def _raw_value_oracle(w, poly_index: int, name: str) -> int:
    for sect in w.sectors:
        for obj in sect.objects:
            if poly_index >= len(obj.polys):
                poly_index -= len(obj.polys)
                continue
            return int(getattr(obj.polys[poly_index], name))
    raise EditError(f"polygon {poly_index} does not exist in the oracle view")


def oracle_vertex_count(w) -> int:
    return sum(o.n_verts for s in w.sectors for o in s.objects)


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def list_level(path: str, limit: int = 8, game: str = DEFAULT_GAME) -> dict:
    data = read_bytes(path)
    scan = walk(data)
    w = oracle.parse(data)
    eof = w.data_end == len(data)
    rep = {
        "path": os.path.abspath(path),
        "bytes": len(data),
        "oracle_parse_end": w.data_end,
        "eof_ok": eof,
        "textures": scan.n_tex,
        "effects": scan.n_eff,
        "sectors": len(scan.sectors),
        "objects": scan.n_objects,
        "vertices": oracle_vertex_count(w),
        "polygons": len(scan.polys),
        "table1_end": scan.table1_end,
        "table2_end": scan.table2_end,
        "texture_table": [
            {"index": i + 1, "name": n, "flag": f, "value": v}
            for i, (n, f, v) in enumerate(scan.tex[:limit or len(scan.tex)])
        ],
        "effect_table": [
            {"index": -(i + 1), "name": n, "kind": k, "start": s, "ids": ids}
            for i, (n, k, s, ids) in
            enumerate(scan.effects[:limit or len(scan.effects)])
        ],
        "sector_list": [
            {"index": s.index, "kind": s.kind, "sub": s.sub,
             "objects": len(s.objects), "polygons": s.n_polys,
             "objects_inline": [
                 {"index": o.index, "id": o.id, "n_verts": o.n_verts,
                  "n_polys": o.n_polys,
                  "origin": [round(c, 4) for c in o.origin]}
                 for o in s.objects[:limit or len(s.objects)]
             ]}
            for s in (scan.sectors[:limit or len(scan.sectors)])
        ],
        "poly_list": [
            {"index": p.index, "sector": p.sector, "obj": p.obj,
             "n_idx": p.n_idx,
             **poly_values(p)}
            for p in (scan.polys[:limit or len(scan.polys)])
        ],
    }
    return rep


def print_list(rep: dict, limit: int = 8) -> None:
    tail = f" (showing {limit})" if limit else ""
    print(f"level: {rep['path']}")
    print(f"header: {rep['bytes']} bytes, oracle parse ends at "
          f"{rep['oracle_parse_end']} ({'EOF ok' if rep['eof_ok'] else 'MISMATCH'})")
    print(f"tables: textures={rep['textures']} effects={rep['effects']}")
    print(f"world: sectors={rep['sectors']} objects={rep['objects']} "
          f"vertices={rep['vertices']} polygons={rep['polygons']}")
    if rep["texture_table"]:
        print(f"textures{tail}:")
        for t in rep["texture_table"]:
            print(f"  tex[{t['index']:3d}] {t['name']:16s} flag={t['flag']} "
                  f"value={t['value']}")
    if rep["effect_table"]:
        print(f"effects{tail}:")
        for e in rep["effect_table"]:
            print(f"  fx[{e['index']:4d}] {e['name']:16s} kind={e['kind']} "
                  f"start={e['start']} ids={e['ids']}")
    print(f"sectors{tail}:")
    for s in rep["sector_list"]:
        print(f"  sector[{s['index']:3d}] kind={s['kind']:2d} sub={s['sub']} "
              f"objects={s['objects']} polys={s['polygons']}")
        for o in s["objects_inline"]:
            print(f"    obj[{o['index']:2d}] id=0x{o['id']:x} "
                  f"verts={o['n_verts']} polys={o['n_polys']} "
                  f"origin={tuple(o['origin'])}")
    print(f"polygons{tail}:")
    for p in rep["poly_list"]:
        print(f"  poly[{p['index']:5d}] s={p['sector']:3d} o={p['obj']:2d} "
              f"n_idx={p['n_idx']:2d} texture={p['texture']:6d} "
              f"material={p['material']} lightmap={p['lightmap']:4d} "
              f"brightness={p['brightness']:3d} overlay={p['overlay']:4d} "
              f"flags=0x{p['flags']:04x}")


# ---------------------------------------------------------------------------
# --get / --set
# ---------------------------------------------------------------------------

def _require_poly(scan: Scan, index: int | None) -> PolyLoc:
    if index is None:
        raise EditError("--poly N is required")
    if not 0 <= index < len(scan.polys):
        raise EditError(f"polygon {index} does not exist "
                        f"(file has {len(scan.polys)}, 0-based)")
    return scan.polys[index]


def get_field(path: str, poly_index: int, name: str) -> dict:
    if name not in FIELDS:
        raise EditError(f"unknown polygon field: {name} "
                        f"(known: {', '.join(FIELDS)})")
    data = read_bytes(path)
    scan = walk(data)
    p = _require_poly(scan, poly_index)
    raw = _raw_value(p, name)
    off, width = p.span(name)
    fld = FIELDS[name]
    return {
        "path": os.path.abspath(path),
        "polygon": poly_index,
        "sector": p.sector,
        "obj": p.obj,
        "n_idx": p.n_idx,
        "field": name,
        "value": _signed16(raw) if fld.signed else raw,
        "raw": raw,
        "disk_offset": off,
        "width": width,
        "disk": fld.disk,
        "engine": fld.engine,
        "doc": fld.doc,
        "writer": fld.va,
    }


def set_field(path: str, poly_index: int, assignment: str, out_path: str,
              game: str = DEFAULT_GAME, sbc_path: str | None = None) -> dict:
    if "=" not in assignment:
        raise EditError("--set needs --field NAME=VALUE")
    name, _, text = assignment.partition("=")
    name = name.strip()
    if name not in FIELDS:
        raise EditError(f"unknown polygon field: {name} "
                        f"(known: {', '.join(FIELDS)})")
    value = parse_int(text)
    data = read_bytes(path)
    scan = walk(data)
    p = _require_poly(scan, poly_index)
    n_lm = lightmap_count(path, sbc_path)
    validate(name, value, scan.n_tex, scan.n_eff, n_lm)
    new = patch_bytes(data, p, name, value)
    if not out_path:
        raise EditError("--set needs --out FILE (the game install is read-only)")
    write_bytes(out_path, new)
    rep = verify_edit(data, new, poly_index, name, value)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(out_path),
        "old": _signed16(_raw_value(p, name)) if FIELDS[name].signed
        else _raw_value(p, name),
        "new": value,
        "bytes": len(new),
        "lightmaps": n_lm,
    })
    if name == "flags" and not (value & FLAGS_FRAME_READY):
        rep["note"] = ("0x8000 cleared: the engine will recompute UV from the "
                       "geometry at 0x43e02d")
    return rep


def print_edit(rep: dict) -> None:
    print(f"{rep['source']} -> {rep['out']}")
    print(f"poly[{rep['polygon']}] {rep['field']}: {rep['old']} -> {rep['new']} "
          f"({rep['bytes']} bytes, unchanged size)")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("note"):
        print(f"  note: {rep['note']}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def check_file(path: str, against: str | None = None,
               poly_index: int | None = None, assignment: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    """Re-read *path* with the oracle and the own walker.

    With ``against`` (a level name/path) the two files are compared: when
    ``poly_index``/``assignment`` are given every other polygon and sector must
    be byte-identical and the target field must carry the new value; otherwise
    the whole file must be identical.
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
               len(scan.polys)) ==
              (len(w.tex), len(w.effects), len(w.sectors), w.n_objects,
               w.n_polys),
              "own walker and oracle agree on counts")
        check(w.to_bytes() == data, "oracle: read -> write is byte-exact")
        rep.update({"textures": scan.n_tex, "effects": scan.n_eff,
                    "sectors": len(scan.sectors), "objects": scan.n_objects,
                    "polygons": len(scan.polys)})
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
        if poly_index is None:
            raise EditError("--check with --field needs --poly N")
        if "=" not in assignment:
            raise EditError("--check --field needs NAME=VALUE")
        name, _, text = assignment.partition("=")
        name = name.strip()
        value = parse_int(text)
        rep.update({"polygon": poly_index, "field": name, "value": value})
        edit = verify_edit(orig, data, poly_index, name, value)
        rep["checks"].extend(edit["checks"])
        rep["problems"].extend(edit["problems"])
        rep["changes"] = edit.get("changes", [])
        rep["ok"] = all(c["ok"] for c in rep["checks"])
        return rep

    # no target: the whole file must equal the source
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
        print(f"target: poly[{rep['polygon']}] {rep['field']}={rep['value']}")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("changes") and not rep["ok"]:
        print(f"  differing bytes: {rep['changes'][:16]}"
              f"{' ...' if len(rep['changes']) > 16 else ''}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --roundtrip
# ---------------------------------------------------------------------------

def roundtrip_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "identical": 0,
           "mismatched": [], "polygons": 0, "objects": 0, "bytes": 0,
           "failures": [], "ok": False}
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
        rep["polygons"] += len(scan.polys)
        rep["objects"] += scan.n_objects
        problems = []
        if built != data:
            problems.append("oracle to_bytes differs")
        if w.data_end != len(data):
            problems.append(f"parse ends at {w.data_end}/{len(data)}")
        if scan.data_end != len(data):
            problems.append("own walker does not end at EOF")
        if len(scan.polys) != w.n_polys:
            problems.append(f"polygon count {len(scan.polys)} != {w.n_polys}")
        if problems:
            rep["mismatched"].append({"file": name, "problems": problems})
            if verbose:
                print(f"  FAIL {name}: {'; '.join(problems)}")
        else:
            rep["identical"] += 1
            if verbose:
                print(f"  ok   {name:14s} {len(data):8d} bytes "
                      f"{len(scan.polys):6d} polys")
    rep["ok"] = (rep["identical"] == rep["files"] and rep["files"] > 0
                 and not rep["failures"])
    return rep


def print_roundtrip(rep: dict) -> None:
    print(f"round-trip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['objects']} objects, {rep['polygons']} polygons, "
          f"{rep['bytes']} bytes ({rep['game']})")
    for m in rep["mismatched"]:
        print(f"  MISMATCH {m['file']}: {'; '.join(m['problems'])}")
    for f in rep["failures"]:
        print(f"  FAILURE {f}")


def audit_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    """Check every documented field of every shipped polygon against its range.

    This is the evidence behind the range checks in :func:`validate`: the tool
    is only allowed to write values the shipped data itself uses (plus the
    documented neighbours).  ``lightmap`` is checked against the record count of
    the level's own ``.sbc``.
    """
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    rep = {"game": os.path.abspath(game), "files": len(files), "polygons": 0,
           "violations": [], "flags_seen": [], "ranges": {},
           "ok": False}
    ranges = {name: [None, None] for name in FIELDS}
    flags_seen = set()
    for path in files:
        data = read_bytes(path)
        scan = walk(data)
        n_lm = lightmap_count(path)
        rep["polygons"] += len(scan.polys)
        for p in scan.polys:
            flags_seen.add(p.flags)
            for name in FIELDS:
                value = _signed16(_raw_value(p, name)) if FIELDS[name].signed \
                    else _raw_value(p, name)
                bounds = ranges[name]
                bounds[0] = value if bounds[0] is None else min(bounds[0], value)
                bounds[1] = value if bounds[1] is None else max(bounds[1], value)
                try:
                    validate(name, value, scan.n_tex, scan.n_eff, n_lm)
                except EditError as exc:
                    if len(rep["violations"]) < 32:
                        rep["violations"].append(
                            f"{os.path.basename(path)} poly {p.index}: {exc}")
                    if verbose:
                        print(f"  VIOLATION {os.path.basename(path)} "
                              f"poly {p.index}: {exc}")
    rep["ranges"] = ranges
    rep["flags_seen"] = [f"0x{v:04x}" for v in sorted(flags_seen)]
    rep["ok"] = not rep["violations"]
    return rep


def print_audit(rep: dict) -> None:
    print(f"audit: {rep['files']} files, {rep['polygons']} polygons "
          f"({rep['game']})")
    for name, (lo, hi) in rep["ranges"].items():
        print(f"  {name:11s} shipped range [{lo}, {hi}]")
    print(f"  flags seen: {', '.join(rep['flags_seen'])}")
    for v in rep["violations"]:
        print(f"  VIOLATION {v}")
    print(f"result: {'OK -- every documented field is inside its range' if rep['ok'] else 'FAILED'}")


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
            "type": ("s" if f.signed else "u") + str(8 * f.width),
            "width": f.width,
            "meaning": f.meaning,
            "doc": f.doc,
            "evidence": f.va,
        })
    if as_json:
        print(json.dumps({"editable": rows, "unavailable": UNAVAILABLE},
                         indent=1, ensure_ascii=False))
        return
    print("editable polygon fields (disk offset is relative to the polygon):")
    for r in rows:
        print(f"  {r['field']:11s} disk {r['disk']:12s} engine {r['engine']}  "
              f"{r['meaning']}")
        print(f"              doc: {r['doc']}")
        print(f"              engine evidence: {r['evidence']}")
    print("not editable:")
    for k, v in UNAVAILABLE.items():
        print(f"  {k}: {v}")


# ---------------------------------------------------------------------------
# self-test (synthetic, no game files)
# ---------------------------------------------------------------------------

def _synth_poly(texture=1, lightmap=1, material=0, overlay=0, brightness=8,
                flags=0x8000, indices=(0, 1, 2), f7a=0) -> bytes:
    tail = bytearray(112)
    struct.pack_into("<6H", tail, 0, 0, 0, 0, 0, 0, 0)      # scroll/u/v/u_scale
    struct.pack_into("<ii", tail, 12, 0, 0)                 # uv_u_fp/uv_v_fp
    struct.pack_into("<I", tail, 0x66, flags)
    head = struct.pack("<5H", texture & 0xFFFF, lightmap & 0xFFFF,
                       material & 0xFFFF, overlay & 0xFFFF, f7a & 0xFFFF)
    return (head + bytes((brightness & 0xFF, len(indices) & 0xFF))
            + struct.pack("<%dH" % len(indices), *indices) + bytes(tail))


def _synth_header(origin=(0.0, 0.0, 0.0)) -> bytes:
    vals = list(origin) + [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    return struct.pack("<12d", *vals)


def _synth_object(n_verts: int, polys: list[bytes]) -> bytes:
    out = bytearray(struct.pack("<HH", n_verts, len(polys)))
    for k in range(n_verts):
        out += struct.pack("<3f", float(k), 0.5 * k, 1.0)
    out += struct.pack("<f", 2.0) + bytes((0, 0))
    out += struct.pack("<II", 0, 0)
    out += struct.pack("<3f", 1.0, 2.0, 3.0)
    out += bytes(24)
    assert len(out) == 4 + 12 * n_verts + 50
    for p in polys:
        out += p
    return bytes(out)


def _synth_file() -> bytes:
    """A tiny, valid ``.sbw``: 2 textures, 1 effect, 2 sectors, 3 objects."""
    out = bytearray()
    tex = [("wall", 0, 3), ("floor", 1, 7)]
    out += struct.pack("<I", len(tex))
    for name, flag, value in tex:
        out += name.encode("latin1") + b"\0" + struct.pack("<HI", flag, value)
    effects = [("fire", 1, 0, [0, 1, 2])]
    out += struct.pack("<I", len(effects))
    for name, kind, start, ids in effects:
        out += name.encode("latin1") + b"\0"
        out += struct.pack("<III", kind, start, len(ids))
        out += struct.pack("<%dH" % len(ids), *ids)
    sectors = [
        (0, 0, [
            (0, (0.0, 0.0, 0.0), 4, [_synth_poly(1, 2, 0, 0, 8, 0x8000, (0, 1, 2)),
                                     _synth_poly(2, 0, 1, -1, 64, 0x8002, (0, 1, 2, 3))]),
            (0, (10.0, 0.0, 0.0), 3, [_synth_poly(1, 1, 2, 0, 127, 0x8004, (0, 1, 2))]),
        ]),
        (6, 1, [
            (0x10000, (0.0, 5.0, 0.0), 3, [_synth_poly(2, 3, 5, 2, 255, 0x8006, (0, 1, 2))]),
        ]),
    ]
    out += struct.pack("<I", len(sectors))
    for kind, sub, objects in sectors:
        out += struct.pack("<IHB", len(objects), kind, sub)
        for oid, origin, _nv, _polys in objects:
            out += struct.pack("<I", oid) + _synth_header(origin)
    for _kind, _sub, objects in sectors:
        for _oid, _origin, nv, polys in objects:
            out += _synth_object(nv, polys)
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
    # 1-3: the synthetic file is a valid .sbw for both readers
    try:
        w = oracle.parse(data)
        check("synth: oracle parses", True)
        check("synth: oracle reaches EOF", w.data_end == len(data),
              f"{w.data_end}/{len(data)}")
        check("synth: oracle round-trip", w.to_bytes() == data)
        check("synth: oracle counts",
              (len(w.tex), len(w.effects), len(w.sectors), w.n_objects,
               w.n_polys) == (2, 1, 2, 3, 4))
    except Exception as exc:                           # noqa: BLE001
        check("synth: oracle parses", False, str(exc))
        w = None

    scan = walk(data)
    check("synth: own walker reaches EOF", scan.data_end == len(data))
    check("synth: own walker counts",
          (scan.n_tex, scan.n_eff, len(scan.sectors), scan.n_objects,
           len(scan.polys)) == (2, 1, 2, 3, 4))
    check("synth: own fields match oracle",
          w is not None and all(
              poly_values(p) == {
                  "texture": _signed16(op.texture), "material": op.material,
                  "lightmap": op.lightmap, "brightness": op.brightness,
                  "overlay": _signed16(op.overlay), "flags": op.flags}
              for p, op in zip(scan.polys,
                               [op for s in w.sectors for o in s.objects
                                for op in o.polys])))

    # 4: the byte layout itself (disk offsets) is what the docs say
    p0 = scan.polys[0]
    check("layout: texture at +0x00",
          struct.unpack_from("<H", data, p0.offset)[0] == 1)
    check("layout: lightmap at +0x02",
          struct.unpack_from("<H", data, p0.offset + 2)[0] == 2)
    check("layout: material at +0x04",
          struct.unpack_from("<H", data, p0.offset + 4)[0] == 0)
    check("layout: overlay at +0x06",
          struct.unpack_from("<H", data, p0.offset + 6)[0] == 0)
    check("layout: brightness at +0x0A", data[p0.offset + 10] == 8)
    check("layout: flags at 0x72+2*n_idx",
          struct.unpack_from("<I", data, p0.offset + 0x72 + 2 * p0.n_idx)[0]
          == 0x8000)
    check("layout: record size 124+2*n_idx",
          all(p.size == 124 + 2 * p.n_idx for p in scan.polys))

    # 5: every editable field round-trips through patch_bytes
    edits = [
        ("texture", 2, "material poly 0 to the second texture", 0),
        ("texture", -1, "effect-table index (negative)", 0),
        ("lightmap", 0, "drop the lightmap", 0),
        ("lightmap", 3, "last .sbc record (1-based)", 0),
        ("material", 5, "shade", 0),
        ("overlay", -1, "overlay effect", 0),
        ("overlay", 2, "overlay texture", 0),
        ("brightness", 127, "max documented light", 0),
        ("brightness", 255, "u8 maximum (used by shipped data)", 0),
        ("flags", 0x8002, "transparent", 0),
        ("flags", 0x8000, "frame-ready only (poly 2 has 0x8004)", 2),
        ("flags", 0x8006, "transparent + toggled", 0),
    ]
    for name, value, why, target in edits:
        try:
            new = patch_bytes(data, scan.polys[target], name, value)
            rep = verify_edit(data, new, target, name, value)
            check(f"edit: {name}={value} ({why})", rep["ok"],
                  "; ".join(rep.get("problems", [])))
        except Exception as exc:                       # noqa: BLE001
            check(f"edit: {name}={value} ({why})", False, str(exc))

    # 6: editing each polygon works, too (index 3 = single poly of sector 1)
    try:
        new = patch_bytes(data, scan.polys[3], "brightness", 0)
        rep = verify_edit(data, new, 3, "brightness", 0)
        check("edit: last polygon", rep["ok"], "; ".join(rep.get("problems", [])))
    except Exception as exc:                           # noqa: BLE001
        check("edit: last polygon", False, str(exc))

    # 7: refusals (documented range / unknown field / bad index)
    expect_refused("refuse: texture > table",
                   lambda: validate("texture", 3, 2, 1, 3))
    expect_refused("refuse: texture < -effects",
                   lambda: validate("texture", -2, 2, 1, 3))
    expect_refused("refuse: material 6",
                   lambda: validate("material", 6, 2, 1, 3))
    expect_refused("refuse: lightmap > count",
                   lambda: validate("lightmap", 4, 2, 1, 3))
    expect_refused("refuse: lightmap without .sbc",
                   lambda: validate("lightmap", 1, 2, 1, None))
    expect_refused("refuse: brightness 256",
                   lambda: validate("brightness", 256, 2, 1, 3))
    expect_refused("refuse: brightness -1",
                   lambda: validate("brightness", -1, 2, 1, 3))
    expect_refused("refuse: flags undocumented bit",
                   lambda: validate("flags", 0x8001, 2, 1, 3))
    expect_refused("refuse: unknown field",
                   lambda: validate("normal", 1, 2, 1, 3))
    expect_refused("refuse: non-integer", lambda: parse_int("abc"))
    expect_refused("refuse: empty", lambda: parse_int(""))
    expect_refused("refuse: polygon out of range",
                   lambda: _require_poly(scan, 4))
    expect_refused("refuse: negative polygon index",
                   lambda: _require_poly(scan, -1))

    # 8: truncation / corruption is reported, not guessed
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
    bad[scan.polys[1].offset + 11] = 200                 # n_idx runs past EOF
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
    check("corrupt n_idx: own walker refuses", own_bad)
    check("corrupt n_idx: oracle refuses", oracle_bad)

    # 9: verification is not vacuous -- a tampered byte outside the target
    #    field must be caught
    new = patch_bytes(data, scan.polys[0], "brightness", 127)
    tampered = bytearray(new)
    tampered[scan.polys[0].offset + 0x14] ^= 0xFF        # centre.x
    rep = verify_edit(data, bytes(tampered), 0, "brightness", 127)
    check("verify: detects a change outside the field", not rep["ok"])
    check("verify: reports the offending span",
          any("other polygons" in c["check"] or "only brightness" in c["check"]
              for c in rep["checks"] if not c["ok"]))

    # 10: the whole --set/--check path through temporary files
    import tempfile
    tmpdir = None
    try:
        tmpdir = tempfile.mkdtemp(prefix="sbwedit-", dir=DEFAULT_OUT_DIR
                                  if os.path.isdir(DEFAULT_OUT_DIR) else None)
        src = os.path.join(tmpdir, "synthetic.sbw")
        dst = os.path.join(tmpdir, "synthetic_edit.sbw")
        write_bytes(src, data)
        rep = set_field(src, 0, "texture=2", dst, game=tmpdir)
        check("set: writes and verifies", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("set: output is readable by the oracle",
              oracle.parse_file(dst).n_polys == 4)
        chk = check_file(dst, src, 0, "texture=2", game=tmpdir)
        check("check: accepts the edit", chk["ok"],
              "; ".join(chk.get("problems", [])))
        chk2 = check_file(dst, src, 0, "texture=1", game=tmpdir)
        check("check: rejects a wrong expectation", not chk2["ok"])
        chk3 = check_file(dst, src, game=tmpdir)
        check("check: full identity rejects the edit", not chk3["ok"])
        chk4 = check_file(src, src, game=tmpdir)
        check("check: identical file passes", chk4["ok"])
        expect_refused(
            "set: refuses without --out",
            lambda: set_field(src, 0, "texture=2", "", game=tmpdir))
        expect_refused(
            "set: refuses too large brightness",
            lambda: set_field(src, 0, "brightness=300", dst, game=tmpdir))
        expect_refused(
            "set: refuses an unknown field",
            lambda: set_field(src, 0, "normal=1", dst, game=tmpdir))
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
    path = resolve_level(args.get, args.game)
    rep = get_field(path, args.poly, args.field)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print(f"poly[{rep['polygon']}] (sector {rep['sector']}, object "
              f"{rep['obj']}, n_idx {rep['n_idx']})")
        print(f"  {rep['field']} = {rep['value']}"
              + (f"  (u16 {rep['raw']}, i.e. {rep['value']})"
                 if FIELDS[rep['field']].signed and rep['value'] < 0 else ""))
        print(f"  disk {rep['disk']} (offset {rep['disk_offset']}), "
              f"engine {rep['engine']}")
        print(f"  doc: {rep['doc']}")
        print(f"  engine evidence: {rep['writer']}")
    return 0


def _cmd_set(args) -> int:
    if args.field is None:
        raise EditError("--set needs --field NAME=VALUE")
    path = resolve_level(args.set, args.game)
    if not args.out:
        raise EditError("--set needs --out FILE")
    rep = set_field(path, args.poly, args.field, args.out, args.game, args.sbc)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    rep = check_file(args.check, args.against, args.poly, args.field, args.game)
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
        description="Edit .sbw world geometry fields of a Gunmetal level "
                    "(M11.2).")
    ap.add_argument("--list", metavar="LEVEL",
                    help="list header, tables, sectors, objects, polygons")
    ap.add_argument("--get", metavar="LEVEL", help="read one polygon field")
    ap.add_argument("--set", metavar="LEVEL", help="edit one polygon field")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE (oracle + own walker) and verify")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source level")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every shipped .sbw")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests (no game files)")
    ap.add_argument("--fields", action="store_true",
                    help="print the editable field map with provenance")
    ap.add_argument("--audit", action="store_true",
                    help="check every shipped polygon field against its range")
    ap.add_argument("--poly", type=int, metavar="N",
                    help="polygon index (0-based, order of --list)")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="field name; --set/--check need NAME=VALUE")
    ap.add_argument("--out", metavar="PATH", help="output .sbw")
    ap.add_argument("--sbc", metavar="FILE",
                    help="lightmap blob for the lightmap range (default: the "
                         "sibling <level>.sbc)")
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
        print(f"sbwedit: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"sbwedit: corrupt .sbw: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"sbwedit: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"sbwedit: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
