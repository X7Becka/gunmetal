"""Gunmetal ".soe" thing-type definitions (text source of .sbt THING_TYPES).

The engine loads ``thing\\<name>.soe`` as text (loader ``gundx.exe`` VMA
``0x47cb6c``, ``fopen(.., "rt")``) and compiles it into the ``THING_TYPES``
binary record of a saved ``.sbt`` (``char name[16]`` + 49 ``double`` + 528
bytes of integers).  Only one file ships (``SOBS/railrock.soe``), so the grammar
below was recovered from the loader itself, not from samples.

The file is a plain whitespace-separated token stream read with ``fscanf``.
The first token is a **version** (0..13); later blocks are gated on it.  The
loader reads the fields in this exact order (``%ld`` integer, ``%lf`` double,
``%s`` string):

    version
    %ld %ld %s %ld %ld                       id / class-name block
    [v>=9]  %ld
    %s %lf*6 %s                              collision type, 6 doubles, shape
    [shape == ellipsoid] %lf %lf %lf         radii
    [shape == sphere]    %lf                 radius
    %lf %lf %lf %lf                          0x50/0x58/0x60/0x68
    [v>=7]  %s %ld   |   %s %s %ld           0x1d4
    [v>=8]  %ld*4    |   %ld                 0x1d8..0x1e4 (max shield values)
    %s %s                                    texture info
    %ld                                      expire value (0x1e8)
    [v>=3]  %s                               expire event
    %s %lf %lf %lf %ld %s %s %ld %ld %ld     "0x2f2" block (10 fields)
    [v>=4]  %lf                              falling fear (0xc8)
    [v>=5]  %s                               create event
    [v>=6]  %s                               morale failure event
    [v>=6]  %s                               time out event
    [v>=6]  %s                               damaged event
    [v>=7]  %ld %ld                          0x1cc / 0x1d0
    [v>=10] %lf %lf                          0xa8 / 0xb0
    [v>=11] %lf %lf                          0xb8 / 0xc0
    [v>=12] %lf %lf                          0x18 / 0x70
    [v>=13] %s %lf                           leash location name / 0xd0
    [v>=2]  %ld                              path-point count, then count *%s
    %ld                                      weapon count, then count *(%s %ld*6)
    [v>=1]  %ld                              children count, then count *
                                             (%s %lf*6 %ld)

Field labels use the engine offsets (``fXX`` = byte 0xXX of the 0x2d8-byte
in-memory thing-type record) where the role is unknown; the role is named where
the loader's own error strings reveal it (collision, shields, events, weapons,
children, path points, ...).  ``parse`` verifies that the whole token stream is
consumed (``ThingType.extra == 0`` for a well-formed file).
"""

from __future__ import annotations

from dataclasses import dataclass, field

__all__ = ["Field", "ThingType", "SoeError", "parse", "parse_file", "parse_text"]

MAX_VERSION = 13


class SoeError(ValueError):
    """Raised when a .soe stream ends earlier than the grammar requires."""


@dataclass
class Field:
    label: str
    kind: str                       # "int" | "flt" | "str"
    value: int | float | str
    offset: int | None = None       # engine byte offset, when known


@dataclass
class ThingType:
    name: str
    version: int
    fields: list[Field] = field(default_factory=list)
    ctx: dict[str, object] = field(default_factory=dict)   # name -> last value
    extra: int = 0                  # leftover tokens (0 = fully consumed)

    def get(self, label: str, default=None):
        return self.ctx.get(label, default)


class _Reader:
    __slots__ = ("tok", "i", "n")

    def __init__(self, text: str):
        self.tok = text.split()
        self.i = 0
        self.n = len(self.tok)

    def eof(self) -> bool:
        return self.i >= self.n

    def s(self) -> str:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe")
        v = self.tok[self.i]
        self.i += 1
        return v

    def d(self) -> int:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe (expected an integer)")
        v = self.tok[self.i]
        self.i += 1
        try:
            return int(v, 10)
        except ValueError:
            raise SoeError(f"expected an integer, got {v!r}") from None

    def f(self) -> float:
        if self.i >= self.n:
            raise SoeError("unexpected end of .soe (expected a number)")
        v = self.tok[self.i]
        self.i += 1
        try:
            return float(v)
        except ValueError:
            raise SoeError(f"expected a number, got {v!r}") from None


def parse_text(text: str, name: str = "") -> ThingType:
    r = _Reader(text)
    tt = ThingType(name=name, version=0)

    def emit(label: str, kind: str, value, offset=None):
        tt.fields.append(Field(label, kind, value, offset))
        tt.ctx[label] = value
        return value

    def gi(label, off=None):
        return emit(label, "int", r.d(), off)

    def gf(label, off=None):
        return emit(label, "flt", r.f(), off)

    def gs(label, off=None):
        return emit(label, "str", r.s(), off)

    v = gi("version")
    tt.version = v

    # id / class-name block
    gi("f19c", 0x19C)
    gi("f198", 0x198)
    gs("class_name")
    gi("f1a8", 0x1A8)
    gi("f1ac", 0x1AC)

    if v >= 9:
        gi("f1b0", 0x1B0)

    # collision
    emit("collision_type", "str", r.s())
    gf("col_f10", 0x10)
    gf("col_f20", 0x20)
    gf("col_f28", 0x28)
    gf("col_f90", 0x90)
    gf("col_f98", 0x98)
    gf("col_fa0", 0xA0)
    shape = r.s().lower()
    emit("collision_shape", "str", shape)
    if shape == "ellipsoid":
        gf("radius_x"); gf("radius_y"); gf("radius_z")
    elif shape == "sphere":
        gf("radius")

    gf("f50", 0x50)
    gf("f58", 0x58)
    gf("f60", 0x60)
    gf("f68", 0x68)

    if v >= 7:
        gs("event_a")
        gi("f1d4", 0x1D4)
    else:
        gs("event_a")
        gs("event_b")
        gi("f1d4", 0x1D4)

    if v >= 8:
        for i in range(4):
            gi(f"shield{i}")
    else:
        gi("shield0")

    gs("tex_a")
    gs("tex_b")

    gi("expire_value", 0x1E8)
    if v >= 3:
        gs("expire_event")

    gs("str_c8")
    gf("f78", 0x78)
    gf("f80", 0x80)
    gf("f88", 0x88)
    gi("f1c8", 0x1C8)
    gs("str_f0")
    gs("str_1a4")
    gi("f200", 0x200)
    gi("f210", 0x210)
    gi("f214", 0x214)

    if v >= 4:
        gf("falling_fear", 0xC8)
    if v >= 5:
        gs("create_event")
    if v >= 6:
        gs("morale_failure_event")
        gs("time_out_event")
        gs("damaged_event")

    if v >= 7:
        gi("f1cc", 0x1CC)
        gi("f1d0", 0x1D0)
    if v >= 10:
        gf("f_a8", 0xA8); gf("f_b0", 0xB0)
    if v >= 11:
        gf("f_b8", 0xB8); gf("f_c0", 0xC0)
    if v >= 12:
        gf("f18", 0x18); gf("f70", 0x70)
    if v >= 13:
        gs("leash_location")
        gf("f_d0", 0xD0)

    if v >= 2:
        n = gi("path_point_count")
        for i in range(max(0, n)):
            gs(f"path_point[{i}]")

    n = gi("weapon_count")
    for i in range(max(0, n)):
        gs(f"weapon[{i}].name")
        for k in range(6):
            gi(f"weapon[{i}].w{k}")

    if v >= 1:
        n = gi("children_count")
        for i in range(max(0, n)):
            gs(f"child[{i}].name")
            for k in range(6):
                gf(f"child[{i}].c{k}")
            gi(f"child[{i}].x")

    tt.extra = r.n - r.i
    return tt


def parse_file(path: str) -> ThingType:
    import os
    stem = os.path.splitext(os.path.basename(path))[0]
    with open(path, "r", encoding="latin1") as f:
        return parse_text(f.read(), name=stem)


parse = parse_file
