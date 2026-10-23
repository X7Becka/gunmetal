#!/usr/bin/env python3
"""Single-source schema check for the Gunmetal data formats (M3.2.1).

The layouts of the shipped formats currently live in three places:

* the C++ readers under ``engine/src/res/``,
* the Python oracle parsers under ``gm/``,
* the prose specifications in ``docs/FORMATS.md``.

They drift silently.  This script declares the **schema once** (the ``SCHEMA``
and ``CONST`` tables below) and then verifies all three sides against it:

1. ``C++ -> schema``: every ``struct`` in ``engine/src/res/{sbt,sbw,sbc}.h`` is
   parsed with a small regex reader; field names, field order-insensitive
   composition and type widths are compared with the declared binding.
2. ``Python -> schema``: the ``@dataclass`` fields of ``gm/sbt.py``,
   ``gm/sbw.py`` and ``gm/sbc.py`` are parsed the same way.
3. ``data -> schema``: an **independent** reader (stdlib ``struct`` only, no
   ``gm`` import) walks every shipped ``.sbw``/``.sbt``/``.sbc`` and checks the
   size invariants (``124 + 2*n`` polygon, record chain up to EOF, column
   tables, ``300 * u32 + 300 * 9`` sample table, ``2**w * 2**h`` lightmaps).

Every row reports ``подтверждено`` / ``расходится`` / ``не проверяемо``.

Usage::

    python3 tools/qa/schema_check.py --self-test
    python3 tools/qa/schema_check.py --check
    python3 tools/qa/schema_check.py --check --json
    python3 tools/qa/schema_check.py --check --strict
    python3 tools/qa/schema_check.py --check --game ~/.cache/gunmetal-re/game
    python3 tools/qa/schema_check.py --check --no-mirror

Exit codes: 0 — all checks ran, nothing differs; 1 — ``--strict`` and at least
one row differs; 2 — a header/module/game file could not be read or the schema
itself is malformed.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_MIRROR = os.environ.get("GUNMETAL_MIRROR") or os.path.join(
    os.path.expanduser("~"), ".cache", "gunmetal-re", "game")

# --------------------------------------------------------------------------
# the single source of truth
# --------------------------------------------------------------------------
# Fixed sizes/limits quoted by docs/FORMATS.md §4, §10, §12.  The independent
# readers below use these numbers (not gm/*.py), so a change to the constants
# here is immediately checked against all shipped data.
CONST = {
    "sbt_header": 84,           # char magic[80] + u32 version
    "sbt_eof": 255,             # END_OF_FILE, no payload
    "sbt_script_thing": 40,     # SCRIPT: count * 40-byte THING rows
    "sbt_script_max_things": 0xC8,
    "sbt_script_max_handles": 0x400,
    "sbt_script_max_code": 0x30000,
    "sbt_script_table": 9 * 0x400,
    "sbt_sample_count": 300,    # SOUND_SAMPLE_TABLE
    "sbt_sample_name": 9,
    "sbt_switch": 0x20,
    "sbt_door": 0xB8,
    "sbt_lift": 0xA8,
    "sbt_lift_stop": 0x0C,
    "sbt_thing": 0x528,
    "sbt_thing_type": 936,
    "sbt_named_item": 52,
    "sbt_sprite": 44,
    "sbt_sector_stuff": 0x5800,
    "sbt_player": 0x3510,
    "sbt_item": 0x7080,
    "sbw_object_header": 96,    # 12 f64: origin + 3 basis rows
    "sbw_stage2_tail": 50,      # radius + f75/f74 + f7c/f78 + bounds + reserved
    "sbw_poly_fixed": 124,      # 12-byte disk head + 112-byte tail
    "sbw_poly_tail": 112,
    "sbw_poly_max_idx": 26,     # largest n_idx in the shipped corpus (level22)
    "sbw_poly_doc_max": 12,     # docs/FORMATS.md §10.1 claims 3..12
    "sbc_header": 8,            # magic[4] + u32 count
    "sbc_max_log": 16,
}

# Per record type of a binary .sbt: how many bytes its payload occupies.
# ``None`` marks a payload whose size depends on fields inside the record and
# is therefore decoded by ``read_sbt_payload``.
SBT_FIXED = {
    1: None, 2: None, 3: None, 4: None, 5: None, 6: None, 7: None,
    8: CONST["sbt_sector_stuff"], 9: None, 11: None, 12: None, 13: "unsupported",
    15: None, 16: None, 17: None, 18: None, 19: CONST["sbt_item"], 20: None,
}

# Expected language bindings.  ``cpp`` is a list of (struct, [(field, type)]),
# ``py`` a list of (module, class, [(field, type)]).  Types are normalised by
# ``normalize_cpp_type`` / ``normalize_py_type``; Python records the family
# (int/bytes/str/...), C++ the exact width, so ``uint32_t`` is caught while a
# Python ``int`` is not needlessly tied to 32 bits.
SCHEMA = {
    "SbtRecord": {
        "layout": [
            ("type", "u8", 1, "one-byte record type; 255 = END_OF_FILE"),
            ("payload", "rest", "n", "everything up to the next record; size = 1 + n"),
        ],
        "cpp": [("SbtRecord", [
            ("type", "u8"), ("name", "string"), ("offset", "size"),
            ("size", "size"), ("payload", "vector"),
        ])],
        "py": [("gm/sbt.py", "Record", [
            ("type", "int"), ("name", "str"), ("offset", "int"),
            ("size", "int"), ("payload", "object"),
        ])],
        "allow_extra": {"Record": ["raw"]},
        "note": "Binary .sbt records are [u8 type][payload] (docs/FORMATS.md §12); "
                "there is no u32 type/size header on disk.",
    },
    "SbtHandle": {
        "layout": [
            ("tag", "u32", 4, "editor-assigned handle tag"),
            ("sector", "u32", 4, "column 2 (poly table: sector)"),
            ("object", "u32", 4, "column 3 (poly table: object, object table: object)"),
            ("poly", "u32", 4, "column 4 of POLY_HANDLE only"),
        ],
        "cpp": [("SbtHandle", [
            ("tag", "u32"), ("a", "u32"), ("b", "u32"), ("c", "u32"),
        ])],
        "py": [],
        "note": "Python keeps handle tables column-major (HandleTable/PolyHandles); "
                "4 x u32 = 16 bytes per row.",
    },
    "Script": {
        "layout": [
            ("count", "u32", 4, "THING rows"),
            ("things", "u8[count*40]", "count*40", "raw THING rows"),
            ("n", "u32", 4, "handle rows"),
            ("handles", "u32[n]*3", "n*12", "three u32 columns (stored separately)"),
            ("m", "u32", 4, "compiled bytecode length"),
            ("code", "u8[m]", "m", "bytecode"),
            ("flag", "u8", 1, "1 = 9*0x400 tables follow"),
            ("tables", "u8[9*0x400]", "0 or 9*0x400", "optional tables"),
        ],
        "cpp": [("SbtScript", [
            ("things", "u32"), ("handles", "u32"), ("code_size", "u32"),
            ("flag", "u8"),
        ])],
        "py": [("gm/sbt.py", "Script", [
            ("count", "int"), ("n", "int"), ("m", "int"), ("flag", "int"),
            ("things", "bytes"), ("handles", "bytes"), ("code", "bytes"),
            ("tables", "bytes"),
        ])],
    },
    "SoundSampleTable": {
        "layout": [
            ("counts", "u32[300]", 300 * 4, "reference counts per slot"),
            ("names", "char[9][300]", 300 * 9, "9-byte name slots, NUL after <=8 chars"),
        ],
        "cpp": [("SoundSampleTable", [
            ("counts", "array[300]<u32>"), ("names", "array[300]<string>"),
        ])],
        "py": [("gm/sbt.py", "SampleTable", [
            ("counts", "tuple"), ("names", "tuple"), ("name_slots", "bytes"),
        ])],
        "constants": {"SoundSampleTable": {"kSlots": 300, "kNameWidth": 9}},
    },
    "SbwPoly": {
        "layout": [
            ("texture", "u16", 2, "0x00 signed texture/effect index"),
            ("lightmap", "u16", 2, "0x04"),
            ("material", "u16", 2, "0x02"),
            ("overlay", "u16", 2, "0x78"),
            ("f7a", "u16", 2, "0x7a"),
            ("brightness", "u8", 1, "0x70"),
            ("n_idx", "u8", 1, "0x71, 3..12"),
            ("indices", "u16[n_idx]", "2*n_idx", "vertex indices"),
            ("tail", "u8[112]", 112, "remaining engine fields 0x08..0x7c"),
        ],
        "cpp": [("SbwPoly", [
            ("texture", "u16"), ("lightmap", "u16"), ("material", "u16"),
            ("overlay", "u16"), ("f7a", "u16"), ("brightness", "u8"),
            ("indices", "vector"), ("scroll_u", "u16"), ("scroll_v", "u16"),
            ("u_scale", "u16"), ("v_scale", "u16"), ("uv_u", "u16"),
            ("uv_v", "u16"), ("uv_u_fp", "u32"), ("uv_v_fp", "u32"),
            ("centre", "struct:Vec3f"), ("normal", "struct:Vec3f"),
            ("u_axis", "struct:Vec3f"), ("v_axis", "struct:Vec3f"),
            ("corner_a", "struct:Vec3f"), ("corner_b", "struct:Vec3f"),
            ("f74", "u16"), ("f76", "u16"), ("handle", "u16"),
            ("reserved", "u32"), ("flags", "u32"), ("anim0", "u16"),
            ("anim1", "u16"), ("anim2", "u16"), ("raw", "array[112]<u8>"),
        ])],
        "py": [("gm/sbw.py", "Poly", [
            ("texture", "int"), ("material", "int"), ("lightmap", "int"),
            ("u_scale", "int"), ("centre", "tuple"), ("normal", "tuple"),
            ("u_axis", "tuple"), ("v_axis", "tuple"), ("corner_a", "tuple"),
            ("corner_b", "tuple"), ("uv_u", "int"), ("uv_v", "int"),
            ("uv_u_fp", "int"), ("uv_v_fp", "int"), ("scroll_u", "int"),
            ("anim0", "int"), ("scroll_v", "int"), ("anim1", "int"),
            ("v_scale", "int"), ("anim2", "int"), ("reserved", "int"),
            ("brightness", "int"), ("indices", "tuple"), ("handle", "int"),
            ("f74", "int"), ("f76", "int"), ("overlay", "int"),
            ("f7a", "int"), ("flags", "int"), ("raw", "bytes"),
        ])],
    },
    "SbwObject": {
        "layout": [
            ("id", "u32", 4, "stage-1 flags/id word"),
            ("header", "f64[12]", 96, "origin[3] + basis[3][3] (identity in data)"),
            ("n_verts", "u16", 2, "stage-2 vertex count"),
            ("n_polys", "u16", 2, "stage-2 polygon count"),
            ("verts", "f32[3][n_verts]", "12*n_verts", "object-local positions"),
            ("radius", "f32", 4, "0x60 = |bounds|"),
            ("f75", "u8", 1, "0x75"),
            ("f74", "u8", 1, "0x74"),
            ("f7c", "u32", 4, "0x7c"),
            ("f78", "u32", 4, "0x78"),
            ("bounds", "f32[3]", 12, "0x84 AABB half-extents"),
            ("reserved", "u8[24]", 24, "0x90"),
            ("polys", "SbwPoly[n_polys]", "sum(124+2*n)", "polygon records"),
        ],
        "cpp": [
            ("SbwObjectRef", [("id", "u32"), ("header", "array[96]<u8>")]),
            ("SbwGeometry", [
                ("n_verts", "u16"), ("n_polys", "u16"), ("vertices", "vector"),
                ("radius", "f32"), ("f75", "u8"), ("f74", "u8"),
                ("f7c", "u32"), ("f78", "u32"), ("bounds", "struct:Vec3f"),
                ("reserved", "array[24]<u8>"), ("tail", "array[50]<u8>"),
                ("polys", "vector"),
            ]),
        ],
        "py": [("gm/sbw.py", "Object", [
            ("id", "int"), ("header", "bytes"), ("points", "tuple"),
            ("n_verts", "int"), ("vertices", "tuple"), ("radius", "float"),
            ("f74", "int"), ("f75", "int"), ("f78", "int"), ("f7c", "int"),
            ("bounds", "tuple"), ("reserved", "bytes"), ("raw_tail", "bytes"),
            ("polys", "tuple"),
        ])],
    },
    "Sbc": {
        "layout": [
            ("magic", "char[4]", 4, "SBC0 or SBC1"),
            ("count", "u32", 4, "number of lightmap records"),
            ("records", "record[count]", "sum(4 + 2**w * 2**h)", "desc then pixels"),
        ],
        "cpp": [
            ("Sbc", [("magic", "string"), ("records", "vector"),
                     ("file_end", "size")]),
            ("SbcRecord", [("width", "i32"), ("height", "i32"),
                           ("pixels", "vector")]),
        ],
        "py": [
            ("gm/sbc.py", "Sbc", [("magic", "bytes"), ("records", "list")]),
            ("gm/sbc.py", "Record", [("width", "int"), ("height", "int"),
                                     ("pixels", "bytes")]),
        ],
    },
}

CPP_FILES = ("engine/src/res/sbt.h", "engine/src/res/sbw.h", "engine/src/res/sbc.h")

# --------------------------------------------------------------------------
# small parsers
# --------------------------------------------------------------------------
_COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
_COMMENT_LINE = re.compile(r"//[^\n]*")
_CPP_STRUCT = re.compile(r"\bstruct\s+(\w+)\s*\{(.*?)\n\s*\};", re.S)
_CPP_DECL = re.compile(
    r"^\s*(?P<type>[A-Za-z_][\w:]*(?:\s*<[^;{}]*?>)?)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*(?:\[(?P<arr>\d+)\])?\s*"
    r"(?:=\s*[^;{}]*|\{[^;{}]*\})?\s*;")
_CPP_CONST = re.compile(
    r"static\s+constexpr\s+std::size_t\s+(k\w+)\s*=\s*([0-9x]+)\s*;")

_CPP_SIMPLE = {
    "uint8_t": "u8", "std::uint8_t": "u8", "unsigned char": "u8",
    "uint16_t": "u16", "std::uint16_t": "u16", "unsigned short": "u16",
    "uint32_t": "u32", "std::uint32_t": "u32", "unsigned int": "u32",
    "uint64_t": "u64", "std::uint64_t": "u64",
    "int8_t": "i8", "std::int8_t": "i8",
    "int16_t": "i16", "std::int16_t": "i16",
    "int32_t": "i32", "std::int32_t": "i32", "int": "i32",
    "int64_t": "i64", "std::int64_t": "i64",
    "size_t": "size", "std::size_t": "size",
    "float": "f32", "double": "f64", "bool": "bool",
    "string": "string", "std::string": "string",
}
_CPP_TEMPLATE = re.compile(r"^std::(array|vector|string|span)\s*<(?P<inner>.*)>$")


def normalize_cpp_type(raw: str, consts: dict[str, int] | None = None) -> str:
    """``std::array<std::uint8_t, 96>`` -> ``array[96]<u8>``."""
    consts = consts or {}
    t = " ".join(raw.split())
    t = t.replace("const ", "").strip()
    m = _CPP_TEMPLATE.match(t)
    if m:
        kind, inner = m.group(1), m.group(2)
        if kind == "vector":
            return "vector"
        if kind == "string":
            return "string"
        if kind == "span":
            return "span"
        if kind == "array":
            parts = [p.strip() for p in inner.rsplit(",", 1)]
            if len(parts) == 2:
                size = parts[1]
                if size in consts:
                    size = str(consts[size])
                return "array[%s]<%s>" % (size, normalize_cpp_type(parts[0], consts))
            return "array<?><%s>" % normalize_cpp_type(parts[0], consts)
    if t in _CPP_SIMPLE:
        return _CPP_SIMPLE[t]
    if t.startswith("struct "):
        t = t[len("struct "):].strip()
    return "struct:" + t


def parse_cpp_structs(text: str,
                      consts: dict[str, int] | None = None) -> dict[str, list[tuple[str, str]]]:
    """Parse simple ``struct NAME { fields };`` bodies (regex, not C++)."""
    text = _COMMENT_BLOCK.sub("", text)
    text = _COMMENT_LINE.sub("", text)
    out: dict[str, list[tuple[str, str]]] = {}
    for m in _CPP_STRUCT.finditer(text):
        name, body = m.group(1), m.group(2)
        fields: list[tuple[str, str]] = []
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith(("static ", "using ", "typedef ")):
                continue
            if "(" in line or line.startswith(("struct ", "enum ", "class ")):
                continue
            dm = _CPP_DECL.match(line)
            if not dm:
                continue
            cat = normalize_cpp_type(dm.group("type"), consts)
            if dm.group("arr"):
                cat = "array[%s]<%s>" % (dm.group("arr"), cat)
            fields.append((dm.group("name"), cat))
        out[name] = fields
    return out


def parse_cpp_constants(text: str) -> dict[str, int]:
    text = _COMMENT_BLOCK.sub("", text)
    text = _COMMENT_LINE.sub("", text)
    out: dict[str, int] = {}
    for m in _CPP_CONST.finditer(text):
        out[m.group(1)] = int(m.group(2), 0)
    return out


_PY_CLASS = re.compile(
    r"@dataclass\b[^\n]*\nclass\s+(\w+)\s*(?:\([^)]*\))?\s*:\s*\n(.*?)"
    r"(?=\n@dataclass\b|\nclass\s|\ndef\s|\Z)", re.S)
_PY_FIELD = re.compile(r"^    ([A-Za-z_]\w*)\s*:\s*([^=\n#]+)")


def normalize_py_type(raw: str, aliases: dict[str, str] | None = None) -> str:
    t = raw.strip()
    aliases = aliases or {}
    if t in aliases:
        return aliases[t]
    if t.startswith(("tuple", "Tuple", "Vec3")):
        return "tuple"
    if t.startswith(("list", "List")):
        return "list"
    if t.startswith(("dict", "Dict")):
        return "dict"
    if t.startswith(("int", "Optional[int]")):
        return "int"
    if t.startswith("float"):
        return "float"
    if t.startswith("bytes"):
        return "bytes"
    if t.startswith("str"):
        return "str"
    if t.startswith("bool"):
        return "bool"
    if t.startswith("object"):
        return "object"
    return t


def parse_py_classes(text: str) -> dict[str, list[tuple[str, str]]]:
    # module-level aliases such as ``Vec3 = tuple[float, float, float]``
    aliases = {m.group(1): normalize_py_type(m.group(2)) for m in
               re.finditer(r"^(\w+)\s*=\s*(tuple|list|dict)\b", text, re.M)}
    out: dict[str, list[tuple[str, str]]] = {}
    for m in _PY_CLASS.finditer(text):
        name, body = m.group(1), m.group(2)
        fields: list[tuple[str, str]] = []
        for line in body.splitlines():
            fm = _PY_FIELD.match(line)
            if fm:
                fields.append((fm.group(1),
                               normalize_py_type(fm.group(2), aliases)))
        out[name] = fields
    return out


def compare_fields(expected: list[tuple[str, str]],
                   actual: list[tuple[str, str]],
                   allow_extra: tuple[str, ...] = ()) -> list[str]:
    """Return human-readable differences between declared and real fields."""
    problems: list[str] = []
    have = dict(actual)
    for name, cat in expected:
        if name not in have:
            problems.append("поле %s отсутствует" % name)
        elif have[name] != cat:
            problems.append("поле %s: ожидался %s, найден %s"
                            % (name, cat, have[name]))
    wanted = {n for n, _ in expected} | set(allow_extra)
    for name, cat in actual:
        if name not in wanted:
            problems.append("лишнее поле %s (%s)" % (name, cat))
    return problems


# --------------------------------------------------------------------------
# independent data readers (stdlib only, no gm.* import)
# --------------------------------------------------------------------------
class DataError(ValueError):
    pass


def read_sbw(data: bytes) -> dict:
    """Walk a .sbw stream exactly as declared by the SbwObject layout."""
    n = len(data)
    off = 0

    def need(k: int, what: str) -> None:
        if k < 0 or off + k > n:
            raise DataError("обрыв на %s: нужно %d, осталось %d"
                            % (what, k, n - off))

    def u32() -> int:
        nonlocal off
        need(4, "u32")
        v = struct.unpack_from("<I", data, off)[0]
        off += 4
        return v

    def u16() -> int:
        nonlocal off
        need(2, "u16")
        v = struct.unpack_from("<H", data, off)[0]
        off += 2
        return v

    def cstr(what: str) -> str:
        nonlocal off
        end = data.find(b"\0", off)
        if end < 0:
            raise DataError("нет NUL в имени (%s)" % what)
        s = data[off:end].decode("latin1")
        off = end + 1
        return s

    tex = []
    for _ in range(u32()):
        tex.append((cstr("texture"), u16(), u32()))

    effects = []
    for _ in range(u32()):
        name = cstr("effect")
        kind, start, cnt = u32(), u32(), u32()
        need(2 * cnt, "effect ids")
        off += 2 * cnt
        effects.append((name, kind, start, cnt))

    sectors = []
    for _ in range(u32()):
        objects = u32()
        kind = u16()
        need(1, "sub")
        sub = data[off]
        off += 1
        rows = []
        for _ in range(objects):
            need(4 + CONST["sbw_object_header"], "object stage 1")
            oid = struct.unpack_from("<I", data, off)[0]
            rows.append(oid)
            off += 4 + CONST["sbw_object_header"]
        sectors.append((kind, sub, rows))

    verts = polys = odd = max_idx = 0
    odd_example = None
    over_doc: list[tuple[int, int]] = []
    for kind, sub, rows in sectors:
        for oid in rows:
            nv, np_ = u16(), u16()
            need(12 * nv + CONST["sbw_stage2_tail"], "object stage 2")
            off += 12 * nv
            off += CONST["sbw_stage2_tail"]
            verts += nv
            for _ in range(np_):
                need(12, "polygon head")
                pstart = off
                # disk order: texture, lightmap, material, overlay, f7a,
                # brightness, n_idx
                _tex, _lm, _mat, _ov, _f7a, _bright, n_idx = struct.unpack_from(
                    "<5HBB", data, off)
                off += 12
                if not 3 <= n_idx <= CONST["sbw_poly_max_idx"]:
                    raise DataError("полигон @0x%x: n_idx=%d вне 3..%d"
                                    % (pstart, n_idx, CONST["sbw_poly_max_idx"]))
                need(2 * n_idx, "polygon indices")
                idx = struct.unpack_from("<%dH" % n_idx, data, off)
                off += 2 * n_idx
                if max(idx) >= nv:
                    raise DataError("полигон @0x%x: индекс %d >= n_verts %d"
                                    % (pstart, max(idx), nv))
                need(CONST["sbw_poly_tail"], "polygon tail")
                off += CONST["sbw_poly_tail"]
                size = off - pstart
                if size != CONST["sbw_poly_fixed"] + 2 * n_idx:
                    raise DataError("полигон @0x%x: размер %d != 124+2*%d"
                                    % (pstart, size, n_idx))
                polys += 1
                max_idx = max(max_idx, n_idx)
                if 2 * n_idx % 2 != 0:
                    raise DataError("полигон @0x%x: 2*n нечётно" % pstart)
                if n_idx % 2 == 1:
                    odd += 1
                    if odd_example is None:
                        odd_example = (pstart, n_idx)
                if n_idx > CONST["sbw_poly_doc_max"]:
                    over_doc.append((pstart, n_idx))
    if off != n:
        raise DataError("хвост %d байт после %d полигонов" % (n - off, polys))
    return {
        "textures": len(tex), "effects": len(effects), "sectors": len(sectors),
        "objects": sum(len(s[2]) for s in sectors), "vertices": verts,
        "polys": polys, "odd_n": odd, "max_n": max_idx,
        "odd_example": odd_example, "over_doc": over_doc,
    }


def _read_sbt_payload(data: bytes, off: int, rtype: int) -> tuple[int, dict]:
    """Validate one .sbt payload; return (bytes consumed, facts)."""
    n = len(data)

    def u32(at: int) -> int:
        if at + 4 > n:
            raise DataError("обрыв u32 в записи %d @0x%x" % (rtype, at))
        return struct.unpack_from("<I", data, at)[0]

    if rtype == 255:
        return 0, {}
    if rtype in (8, 19):
        size = CONST["sbt_sector_stuff" if rtype == 8 else "sbt_item"]
        if off + size > n:
            raise DataError("обрыв записи %d" % rtype)
        return size, {}
    if rtype in (3, 9):
        if off >= n:
            raise DataError("обрыв флага записи %d" % rtype)
        present = data[off] != 0
        size = 1 + (CONST["sbt_thing" if rtype == 3 else "sbt_player"]
                    if present else 0)
        if off + size > n:
            raise DataError("обрыв записи %d" % rtype)
        return size, {"present": present}
    if rtype == 12:
        size = CONST["sbt_sample_count"] * 4 + \
            CONST["sbt_sample_count"] * CONST["sbt_sample_name"]
        if off + size > n:
            raise DataError("обрыв SOUND_SAMPLE_TABLE")
        return size, {"size": size}
    if rtype == 11:
        c = u32(off)
        if not c <= CONST["sbt_script_max_things"]:
            raise DataError("SCRIPT count=%d > 0x%x"
                            % (c, CONST["sbt_script_max_things"]))
        off2 = off + 4 + c * CONST["sbt_script_thing"]
        nh = u32(off2)
        if not nh <= CONST["sbt_script_max_handles"]:
            raise DataError("SCRIPT n=%d > 0x%x"
                            % (nh, CONST["sbt_script_max_handles"]))
        off2 += 4 + 3 * nh * 4
        m = u32(off2)
        if not m <= CONST["sbt_script_max_code"]:
            raise DataError("SCRIPT m=%d > 0x%x"
                            % (m, CONST["sbt_script_max_code"]))
        off2 += 4 + m
        if off2 >= n:
            raise DataError("SCRIPT без флага")
        flag = data[off2]
        size = off2 + 1 - off + (CONST["sbt_script_table"] if flag else 0)
        if off + size > n:
            raise DataError("SCRIPT обрыв таблиц")
        return size, {"things": c, "handles": nh, "code": m, "flag": flag}
    if rtype == 20:
        c = u32(off)
        if c > 0xC8:
            raise DataError("SOB_TABLE n=%d > 0xc8" % c)
        return 4 + c * 20, {"sobs": c}
    if rtype == 5:
        c = u32(off)
        if c > 0x31:
            raise DataError("NAMED_ITEMS n=%d > 0x31" % c)
        return 4 + c * CONST["sbt_named_item"], {}
    if rtype == 4:
        c = u32(off)
        if c > 0x80:
            raise DataError("THING_TYPES n=%d > 0x80" % c)
        return 4 + c * CONST["sbt_thing_type"], {}
    if rtype == 6:
        c = u32(off)
        if c >= 0x1F4:
            raise DataError("SPRITES n=%d >= 0x1f4" % c)
        return 4 + c * CONST["sbt_sprite"], {}
    if rtype == 7:
        c = u32(off)
        if c >= 0x80:
            raise DataError("SOUND_SOURCES n=%d >= 0x80" % c)
        return 4 + c * 4 * 5 + c * 8, {}
    if rtype == 1:
        c = u32(off)
        if c > 0x200:
            raise DataError("SWITCH n=%d > 0x200" % c)
        return 4 + c * CONST["sbt_switch"], {}
    if rtype == 2:
        c = u32(off)
        if c > 100:
            raise DataError("DOOR n=%d > 100" % c)
        return 4 + c * CONST["sbt_door"], {}
    if rtype == 15:
        c = u32(off)
        if c >= 0x1F4:
            raise DataError("POLY_HANDLE n=%d >= 0x1f4" % c)
        return 4 + 4 * 4 * c, {}
    if rtype == 16:
        c = u32(off)
        if c >= 0x12C:
            raise DataError("OBJECT_HANDLE n=%d >= 0x12c" % c)
        return 4 + 3 * 4 * c, {}
    if rtype == 17:
        c = u32(off)
        if c > 0x100:
            raise DataError("SECTOR_HANDLE n=%d > 0x100" % c)
        return 4 + 2 * 4 * c, {}
    if rtype == 18:
        c = u32(off)
        if c > 0x1E:
            raise DataError("LIFT n=%d > 0x1e" % c)
        off2 = off + 4 + c * CONST["sbt_lift"]
        k = u32(off2)
        if k > 0xC8:
            raise DataError("LIFT stops k=%d > 0xc8" % k)
        return off2 + 4 + k * CONST["sbt_lift_stop"] - off, {}
    raise DataError("неизвестный тип записи %d" % rtype)


def read_sbt(data: bytes) -> dict:
    n = len(data)
    if n < CONST["sbt_header"]:
        raise DataError("короче заголовка (%d < 84)" % n)
    if not data.startswith(b"BINARY SAVE\0"):
        raise DataError("нет магии BINARY SAVE")
    version = struct.unpack_from("<I", data, 80)[0]
    if version != 0:
        raise DataError("version=%d (ожидался 0)" % version)
    off = CONST["sbt_header"]
    records = []
    facts: dict = {"script": 0, "samples": 0, "handles": [], "sobs": 0}
    while True:
        if off >= n:
            raise DataError("поток кончился без END_OF_FILE")
        rtype = data[off]
        start = off
        off += 1
        size, info = _read_sbt_payload(data, off, rtype)
        off += size
        records.append((rtype, start, off - start))
        if rtype == 255:
            break
        if rtype == 11:
            facts["script"] += 1
        elif rtype == 12:
            facts["samples"] += 1
        elif rtype in (15, 16, 17):
            facts["handles"].append(rtype)
        elif rtype == 20:
            facts["sobs"] += info.get("sobs", 0)
    if off != n:
        raise DataError("хвост %d байт после END_OF_FILE" % (n - off))
    return {"records": len(records), "types": [r[0] for r in records], **facts}


def read_sbc(data: bytes) -> dict:
    n = len(data)
    if n < CONST["sbc_header"]:
        raise DataError("короче заголовка (%d < 8)" % n)
    if data[:4] not in (b"SBC0", b"SBC1"):
        raise DataError("чужая магия %r" % data[:4])
    count = struct.unpack_from("<I", data, 4)[0]
    off = CONST["sbc_header"]
    sizes = []
    example = None
    for i in range(count):
        if off + 4 > n:
            raise DataError("обрыв desc записи %d" % i)
        desc = struct.unpack_from("<I", data, off)[0]
        off += 4
        if example is None:
            example = (desc & 0xFFFF, desc >> 16, desc)
        w_log, h_log = desc & 0xFFFF, desc >> 16
        if w_log > CONST["sbc_max_log"] or h_log > CONST["sbc_max_log"]:
            raise DataError("запись %d: 2**%d x 2**%d слишком велико"
                            % (i, w_log, h_log))
        size = (1 << w_log) * (1 << h_log)
        if off + size > n:
            raise DataError("запись %d обрезана (%d > %d)"
                            % (i, off + size, n))
        off += size
        sizes.append((w_log, h_log))
    if off != n:
        raise DataError("хвост %d байт после %d записей" % (n - off, count))
    return {"count": count, "sizes": sizes, "magic": data[:4].decode(),
            "log2": sorted({(w, h) for w, h in sizes}), "example": example}


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------
def _row(fmt: str, side: str, verdict: str, detail: str) -> dict:
    return {"format": fmt, "side": side, "verdict": verdict, "detail": detail}


def check_language(schema: dict, cpp: dict[str, list], py: dict[str, list],
                   cpp_consts: dict[str, int]) -> list[dict]:
    rows: list[dict] = []
    for name, entry in schema.items():
        allow = entry.get("allow_extra", {})
        for struct, expected in entry.get("cpp", []):
            actual = cpp.get(struct)
            if actual is None:
                rows.append(_row(name, "cpp", "расходится",
                                 "нет struct %s" % struct))
                continue
            probs = compare_fields(expected, actual, tuple(allow.get(struct, ())))
            rows.append(_row(name, "cpp",
                             "расходится" if probs else "подтверждено",
                             "; ".join(probs) if probs else
                             "%s: %d полей" % (struct, len(expected))))
        for path, cls, expected in entry.get("py", []):
            actual = py.get((path, cls))
            if actual is None:
                rows.append(_row(name, "python", "расходится",
                                 "нет dataclass %s в %s" % (cls, path)))
                continue
            probs = compare_fields(expected, actual, tuple(allow.get(cls, ())))
            rows.append(_row(name, "python",
                             "расходится" if probs else "подтверждено",
                             "; ".join(probs) if probs else
                             "%s: %d полей" % (cls, len(expected))))
        for struct, consts in entry.get("constants", {}).items():
            actual = cpp_consts
            bad = ["%s=%s (схема %s)" % (k, actual.get(k, "нет"), v)
                   for k, v in consts.items() if actual.get(k) != v]
            rows.append(_row(name, "cpp-const",
                             "расходится" if bad else "подтверждено",
                             "; ".join(bad) if bad else "константы совпали"))
    return rows


def check_data(game: str, only_sbw: str | None = None) -> list[dict]:
    rows: list[dict] = []
    sbw_paths = sorted(glob.glob(os.path.join(game, "*.sbw")))
    sbt_paths = sorted(glob.glob(os.path.join(game, "*.sbt")))
    sbc_paths = sorted(glob.glob(os.path.join(game, "*.sbc"))) + \
        sorted(glob.glob(os.path.join(game, "SOBS", "*.sbc")))

    if not sbw_paths:
        rows.append(_row("sbw", "data", "не проверяемо",
                         "в %s нет .sbw" % game))
    else:
        ok = 0
        bad = []
        odd_total = 0
        over_doc: dict[int, int] = {}
        for p in sbw_paths:
            try:
                facts = read_sbw(open(p, "rb").read())
                ok += 1
                odd_total += facts["odd_n"]
                for _, n_idx in facts["over_doc"]:
                    over_doc[n_idx] = over_doc.get(n_idx, 0) + 1
            except (OSError, DataError) as exc:
                bad.append("%s: %s" % (os.path.basename(p), exc))
        detail = "%d/%d файлов, нечётных n_idx: %d" % (ok, len(sbw_paths), odd_total)
        if bad:
            detail += "; " + "; ".join(bad[:4])
        rows.append(_row("sbw", "data",
                         "расходится" if bad else "подтверждено", detail))
        if over_doc:
            detail = ("док §10.1 обещает 3..12, в данных есть n_idx > 12: %s "
                      "(всего %d полигонов)" % (
                          ", ".join("%d x%d" % (n, c)
                                    for n, c in sorted(over_doc.items())),
                          sum(over_doc.values())))
            rows.append(_row("sbw/n_idx", "data", "расходится", detail))
        target = only_sbw or os.path.join(game, "level0.sbw")
        if os.path.exists(target):
            try:
                facts = read_sbw(open(target, "rb").read())
                eoff, en = facts["odd_example"] or (0, 0)
                detail = ("%d полигонов, нечётных n_idx %d (например @0x%x n=%d, "
                          "размер %d = 124+2*%d), max n=%d" % (
                              facts["polys"], facts["odd_n"], eoff, en,
                              CONST["sbw_poly_fixed"] + 2 * en, en,
                              facts["max_n"]))
                rows.append(_row("sbw/level0", "data", "подтверждено", detail))
            except (OSError, DataError) as exc:
                rows.append(_row("sbw/level0", "data", "расходится", str(exc)))

    if not sbt_paths:
        rows.append(_row("sbt", "data", "не проверяемо", "в %s нет .sbt" % game))
    else:
        ok = 0
        bad = []
        scripts = 0
        tables = 0
        for p in sbt_paths:
            try:
                facts = read_sbt(open(p, "rb").read())
                ok += 1
                scripts += facts["script"]
                tables += facts["samples"]
            except (OSError, DataError) as exc:
                bad.append("%s: %s" % (os.path.basename(p), exc))
        detail = "%d/%d файлов, SCRIPT %d, SOUND_SAMPLE_TABLE %d" % (
            ok, len(sbt_paths), scripts, tables)
        if bad:
            detail += "; " + "; ".join(bad[:4])
        rows.append(_row("sbt", "data",
                         "расходится" if bad else "подтверждено", detail))

    if not sbc_paths:
        rows.append(_row("sbc", "data", "не проверяемо", "в %s нет .sbc" % game))
    else:
        ok = 0
        bad = []
        logs: set[tuple[int, int]] = set()
        total_records = 0
        example = None
        for p in sbc_paths:
            try:
                facts = read_sbc(open(p, "rb").read())
                ok += 1
                total_records += facts["count"]
                logs.update(facts["log2"])
                if example is None:
                    example = (os.path.basename(p), facts["magic"],
                               facts["example"])
            except (OSError, DataError) as exc:
                bad.append("%s: %s" % (os.path.relpath(p, game), exc))
        wlogs = sorted({w for w, _ in logs})
        hlogs = sorted({h for _, h in logs})
        detail = ("%d/%d файлов, записей %d, w_log %s, h_log %s; пример %s: "
                  "%s desc=0x%08x -> 2**%d x 2**%d" % (
                      ok, len(sbc_paths), total_records,
                      "%d..%d" % (wlogs[0], wlogs[-1]) if wlogs else "-",
                      "%d..%d" % (hlogs[0], hlogs[-1]) if hlogs else "-",
                      example[0] if example else "-",
                      example[1] if example else "-",
                      example[2][2] if example else 0,
                      example[2][0] if example else 0,
                      example[2][1] if example else 0))
        if bad:
            detail += "; " + "; ".join(bad[:4])
        rows.append(_row("sbc", "data",
                         "расходится" if bad else "подтверждено", detail))
    return rows


# --------------------------------------------------------------------------
# self-test (no game files needed)
# --------------------------------------------------------------------------
_SYNTH_CPP = """
struct Demo {
    std::uint32_t tag = 0;
    std::array<std::uint8_t, 96> header{};
    float radius = 0.0f;
};
"""
_SYNTH_PY = """
@dataclass
class Demo:
    tag: int
    header: bytes
    radius: float
"""


def self_test() -> int:
    ok = 0
    total = 0

    def t(name: str, cond: bool, got="") -> None:
        nonlocal ok, total
        total += 1
        if cond:
            ok += 1
        else:
            print("FAIL %s %s" % (name, got))

    cpp = parse_cpp_structs(_SYNTH_CPP)
    py = parse_py_classes(_SYNTH_PY)
    t("cpp struct parsed", cpp.get("Demo") == [
        ("tag", "u32"), ("header", "array[96]<u8>"), ("radius", "f32")], cpp)
    t("py dataclass parsed", py.get("Demo") == [
        ("tag", "int"), ("header", "bytes"), ("radius", "float")], py)

    expected = [("tag", "u32"), ("header", "array[96]<u8>"),
                ("radius", "f32")]
    t("matching fields", not compare_fields(expected, cpp["Demo"]))
    t("declared extras allowed", not compare_fields(
        [("tag", "u32")], cpp["Demo"], ("header", "radius")))
    t("missing field caught", compare_fields(
        expected + [("colour", "u32")], cpp["Demo"]) == [
            "поле colour отсутствует"], compare_fields(
                expected + [("colour", "u32")], cpp["Demo"]))
    t("width change caught", compare_fields(
        [("tag", "u64")], cpp["Demo"], ("header", "radius")) == [
            "поле tag: ожидался u64, найден u32"], compare_fields(
                [("tag", "u64")], cpp["Demo"], ("header", "radius")))
    t("extra field caught", compare_fields(
        [("tag", "u32")], cpp["Demo"], ("header",)) == [
            "лишнее поле radius (f32)"], compare_fields(
                [("tag", "u32")], cpp["Demo"], ("header",)))

    # corrupted schema: change a declared width -> the comparator must fail.
    corrupted = dict(SCHEMA)
    entry = dict(SCHEMA["SbwObject"])
    entry["cpp"] = [
        ("SbwObjectRef", [("id", "u32"), ("header", "array[95]<u8>")]),
    ]
    corrupt_probs: list[str] = []
    for sname, exp in entry["cpp"]:
        corrupt_probs += compare_fields(exp, cpp.get(sname, []))
    t("corrupted schema caught", any("header" in p for p in corrupt_probs),
      corrupt_probs)

    # corrupted C++: drop the field -> caught.
    broken_cpp = _SYNTH_CPP.replace("    float radius = 0.0f;\n", "")
    broken = parse_cpp_structs(broken_cpp)["Demo"]
    probs = compare_fields(expected, broken)
    t("removed cpp field caught", probs == ["поле radius отсутствует"], probs)

    # corrupted Python: wrong family -> caught.
    broken_py = _SYNTH_PY.replace("radius: float", "radius: str")
    probs = compare_fields([("radius", "float")],
                           parse_py_classes(broken_py)["Demo"],
                           ("tag", "header"))
    t("py family change caught", probs == [
        "поле radius: ожидался float, найден str"], probs)

    # schema must be self-consistent: no duplicated declared fields.
    dup = []
    for name, entry2 in SCHEMA.items():
        for sname, fields in entry2.get("cpp", []):
            names = [f for f, _ in fields]
            if len(names) != len(set(names)):
                dup.append("%s/%s" % (name, sname))
        for path, cls, fields in entry2.get("py", []):
            names = [f for f, _ in fields]
            if len(names) != len(set(names)):
                dup.append("%s/%s" % (name, cls))
    t("schema has no duplicate fields", not dup, dup)

    # independent .sbc reader: good blob and trailing garbage.
    good = b"SBC0" + struct.pack("<I", 2) + struct.pack("<I", 4 | (3 << 16)) + \
        bytes(16 * 8) + struct.pack("<I", 0) + bytes(1)
    facts = read_sbc(good)
    t("sbc good", facts["count"] == 2 and
      facts["log2"] == [(0, 0), (4, 3)], facts)
    try:
        read_sbc(good + b"\0")
    except DataError as exc:
        t("sbc trailing caught", "хвост" in str(exc), str(exc))
    else:
        t("sbc trailing caught", False, "нет ошибки")

    # independent .sbt reader: type 12 + EOF, and a truncated variant.
    sample = b"BINARY SAVE\0" + bytes(68) + struct.pack("<I", 0) + \
        bytes([12]) + bytes(300 * 4 + 300 * 9) + bytes([255])
    try:
        facts = read_sbt(sample)
        t("sbt sample table", facts["samples"] == 1 and
          facts["types"] == [12, 255], facts)
    except DataError as exc:
        t("sbt sample table", False, str(exc))
    try:
        read_sbt(sample[:-1])
    except DataError as exc:
        t("sbt truncation caught", "END_OF_FILE" in str(exc), str(exc))
    else:
        t("sbt truncation caught", False, "нет ошибки")

    # independent .sbw reader: a minimal one-object, one-polygon level, then a
    # truncated copy that must be rejected.
    poly = struct.pack("<5HBB", 0, 0, 0, 0, 0, 0, 3) + \
        struct.pack("<3H", 0, 1, 2) + bytes(CONST["sbw_poly_tail"])
    sbw = struct.pack("<3I", 0, 0, 1) + struct.pack("<IHB", 1, 0, 0) + \
        struct.pack("<I", 7) + bytes(CONST["sbw_object_header"]) + \
        struct.pack("<HH", 3, 1) + bytes(3 * 12) + \
        bytes(CONST["sbw_stage2_tail"]) + poly
    facts = read_sbw(sbw)
    t("sbw synthetic", facts["objects"] == 1 and facts["polys"] == 1 and
      len(sbw) == 12 + 7 + 100 + (4 + 36 + 50) + 130, facts)
    try:
        read_sbw(sbw[:-1])
    except DataError as exc:
        t("sbw truncation caught", "обрыв" in str(exc), str(exc))
    else:
        t("sbw truncation caught", False, "нет ошибки")

    # polygon size rule, including odd n (2*n is still even).
    t("poly sizes", [CONST["sbw_poly_fixed"] + 2 * n for n in (3, 4, 5)] ==
      [130, 132, 134])
    t("object header", CONST["sbw_object_header"] == 12 * 8)

    print("self-test: %d/%d checks passed" % (ok, total))
    return 0 if ok == total else 1


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def gather_sources() -> tuple[dict, dict, dict, dict, list[str]]:
    """Parse all declared headers/modules; raise OSError/ValueError on failure."""
    problems: list[str] = []
    cpp_all: dict[str, list] = {}
    consts_all: dict[str, int] = {}
    cpp_cache: dict[str, str] = {}
    for rel in CPP_FILES:
        path = os.path.join(ROOT, rel)
        text = open(path, encoding="utf-8").read()
        cpp_cache[rel] = text
        file_consts = parse_cpp_constants(text)
        consts_all.update(file_consts)
        cpp_all.update(parse_cpp_structs(text, consts_all))
    py_all: dict[tuple[str, str], list] = {}
    modules = sorted({path for e in SCHEMA.values() for path, _, _ in e.get("py", [])})
    for rel in modules:
        path = os.path.join(ROOT, rel)
        text = open(path, encoding="utf-8").read()
        for cls, fields in parse_py_classes(text).items():
            py_all[(rel, cls)] = fields
    return cpp_all, py_all, consts_all, cpp_cache, problems


def check_roots(args) -> list[dict]:
    roots = [args.game]
    if args.mirror and args.mirror not in roots:
        roots.append(args.mirror)
    rows: list[dict] = []
    for root in roots:
        if not os.path.isdir(root):
            rows.append(_row("game", "data", "не проверяемо",
                             "каталог %s недоступен" % root))
            continue
        for r in check_data(root, args.sbw):
            r["game"] = root
            rows.append(r)
    return rows


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in tests on synthetic data (no game)")
    ap.add_argument("--check", action="store_true",
                    help="check C++ / Python / shipped data against the schema")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 if any row differs or is unverifiable")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--mirror", default=DEFAULT_MIRROR,
                    help="second game root to check ('' to skip)")
    ap.add_argument("--no-mirror", action="store_true")
    ap.add_argument("--sbw", default=None,
                    help="specific .sbw for the odd-n polygon detail")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.check:
        ap.print_help()
        return 0
    if args.no_mirror:
        args.mirror = ""
    if not os.path.isdir(args.game):
        print("schema_check: каталог игры %s недоступен" % args.game,
              file=sys.stderr)
        return 2

    try:
        cpp, py, consts, _, _ = gather_sources()
    except (OSError, ValueError) as exc:
        print("schema_check: не удалось прочитать заголовки/модули: %s" % exc,
              file=sys.stderr)
        return 2

    rows = check_language(SCHEMA, cpp, py, consts)
    rows += check_roots(args)
    order = {"cpp": 0, "cpp-const": 1, "python": 2, "data": 3}
    rows.sort(key=lambda r: order.get(r["side"], 9))

    counts = {"подтверждено": 0, "расходится": 0, "не проверяемо": 0}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1

    if args.json:
        print(json.dumps({
            "schema": {k: {"cpp": [s for s, _ in v.get("cpp", [])],
                           "py": [c for _, c, _ in v.get("py", [])],
                           "layout_fields": len(v.get("layout", []))}
                       for k, v in SCHEMA.items()},
            "checks": rows,
            "summary": counts,
        }, ensure_ascii=False, indent=1))
    else:
        last_side = None
        for r in rows:
            key = r["side"]
            if key != last_side:
                label = {"cpp": "C++ ↔ схема", "cpp-const": "C++ константы ↔ схема",
                         "python": "Python ↔ схема", "data": "данные ↔ схема"}
                print("\n== %s" % label.get(r["side"], r["side"]))
                last_side = key
            prefix = ("[%s] " % r["game"]) if r.get("game") else ""
            print("  %-20s %-18s %-13s %s"
                  % (r["format"], prefix.rstrip() or "", r["verdict"], r["detail"]))
        print("\nитого: подтверждено %d, расходится %d, не проверяемо %d"
              % (counts["подтверждено"], counts["расходится"],
                 counts["не проверяемо"]))

    if args.strict and (counts["расходится"] or counts["не проверяемо"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
