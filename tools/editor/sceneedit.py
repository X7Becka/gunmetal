#!/usr/bin/env python3
"""Editor for **scene records** of a Gunmetal ``.sbt`` level (``M11.3``, slice 1).

The level ``.sbt`` is a compiled "World Things file": an 84-byte header followed
by a bare stream of ``u8 type`` + payload records, terminated by
``255 END_OF_FILE``.  There is **no offset/size table** -- each type's handler
consumes exactly its own payload -- so a record can grow or shrink without any
absolute-offset fixups; only the file offsets of the following records move
(``gm/sbt.py`` module docstring and ``parse``; ``docs/FORMATS.md`` §12;
``docs/SBT_WRITE.md`` §1).

This tool edits everything **except** the compiled script:

* **scene records** = every record whose type is not ``11 SCRIPT`` and not
  ``255 END_OF_FILE``: ``SWITCH`` (1), ``DOOR`` (2), ``THING`` (3),
  ``THING_TYPES`` (4), ``NAMED_ITEMS`` (5), ``SPRITES`` (6),
  ``SOUND_SOURCES`` (7), ``SECTOR_STUFF`` (8), ``PLAYER_STATUS`` (9),
  ``SOUND_SAMPLE_TABLE`` (12), ``POLY_HANDLE`` (15), ``OBJECT_HANDLE`` (16),
  ``SECTOR_HANDLE`` (17), ``LIFT`` (18), ``ITEM`` (19), ``SOB_TABLE`` (20).
  Types ``10``/``13`` are engine stubs with no payload and type ``14`` is the
  "corrupt" path (``docs/SBT_RECORDS.md`` §1); both are rejected.
* ``SCRIPT`` (11) is **refused** on purpose: its code buffer has sub-script
  entry offsets in handle column 2 that must be recomputed -- that is
  :mod:`tools.editor.sbtwrite`'s job (``docs/SBT_WRITE.md`` §2).

How an edit is applied
----------------------
The *raw payload bytes* of the record (``Record.raw``, kept verbatim by the
oracle) are patched **in place** at the documented field offset; every other
byte of that record -- padding, name tails, stale pointers, unknown ``tail``
words -- is preserved because it is never decoded and re-encoded.  The patched
payload is then re-parsed by the oracle (:func:`gm.sbt.parse_payload`, which
requires full consumption), the edited record's ``raw``/``size``/``payload`` are
refreshed, and the container is re-serialised with :meth:`gm.sbt.Sbt.to_bytes`
(header + ``type`` + ``raw`` per record).  After writing, the file is read back
with the oracle **and** with this module's independent walker
(:func:`walk_container`) and every non-target record is compared byte for byte.

Field references
----------------
``NAME`` or ``NAME[INDEX]``:

* the bracketed index is the **entry/element index** inside the record payload
  (``ITEM`` slot, ``SOB_TABLE`` row, handle row, ``LIFT`` stop, sample slot, ...).
  It is required whenever the block has more than one element;
* a vector component is written with a dot: ``pos.x``, ``ids.2``, ``param.0``,
  ``tail.3``, ``move.z``.  Documented aliases exist: ``speed`` = ``param.0``
  (``docs/FORMATS.md`` §12.2 / ``docs/SBT_RECORDS.md`` §2.4) and ``model`` =
  ``tail.3`` (the ``SOB_TABLE`` index at ``+0x1A4``).

Every field name, offset and type in :data:`SCHEMA` cites the document that
proves it; fields documented only as a hypothesis are marked ``[hypothesis]`` in
the ``--list`` output and in ``--fields``.

Usage::

    python3 tools/editor/sceneedit.py --list level0
    python3 tools/editor/sceneedit.py --list level0 --json
    python3 tools/editor/sceneedit.py --get level0 --record 5 --field speed[40]
    python3 tools/editor/sceneedit.py --set level0 --record 5 \\
            --field speed[40]=0.02 --out /tmp/opencode/sceneedit/level0.sbt
    python3 tools/editor/sceneedit.py --replace-record level0 --record 1 \\
            --file /tmp/opencode/sceneedit/sob.bin --out /tmp/opencode/x.sbt
    python3 tools/editor/sceneedit.py --check /tmp/opencode/sceneedit/level0.sbt \\
            --against level0 --record 5 --field speed[40]=0.02
    python3 tools/editor/sceneedit.py --roundtrip
    python3 tools/editor/sceneedit.py --self-test
    python3 tools/editor/sceneedit.py --fields

Exit codes: ``0`` success, ``1`` verification failed (round-trip/check/self-test
negative), ``2`` bad input or a refused edit (message on stderr), ``3`` internal
error.  No traceback escapes :func:`main`.
"""
from __future__ import annotations

import argparse
import copy
import glob
import hashlib
import json
import os
import struct
import sys
import time
from dataclasses import dataclass, field as dc_field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sbt as sbtmod                       # noqa: E402
from tools.editor import sbtwrite as sbtio         # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sceneedit"

MAGIC = sbtmod.MAGIC
HEADER_SIZE = sbtmod.HEADER_SIZE
SCRIPT_TYPE = 11
END = sbtmod.END_OF_FILE
UNSUPPORTED = sbtmod.UNSUPPORTED          # 10, 13
TYPE_NAMES = dict(sbtmod.TYPE_NAMES)

#: Scene records are all non-SCRIPT, non-END records; ``--record`` indexes this
#: list (see :func:`scene_records`).
SKIP_TYPES = (SCRIPT_TYPE, END)

KINDS = {
    "u8": ("<B", 1),
    "u16": ("<H", 2),
    "u32": ("<I", 4),
    "i32": ("<i", 4),
    "f32": ("<f", 4),
    "f64": ("<d", 8),
}
# name slots: fixed byte width, NUL padded (the oracle keeps the full slot)
NAMES = {"name16": 16, "name12": 12, "name9": 9}
INT_RANGE = {
    "u8": (0, 0xFF),
    "u16": (0, 0xFFFF),
    "u32": (0, 0xFFFFFFFF),
    "i32": (-0x80000000, 0x7FFFFFFF),
}


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


# ---------------------------------------------------------------------------
# documentation-backed field maps
#
# Every entry: (field name, offset inside the element, kind, provenance).
# Provenance is printed by ``--fields``/``--list`` so a field can never be
# mistaken for a guess.  Offsets are cross-checked against the element size in
# :func:`_entry_fields`.


def _vec(prefix, base, count, kind, step, prov):
    return [(f"{prefix}.{i}", base + i * step, kind, prov) for i in range(count)]


#: ``type -> {"entry_size", "count_prefix" | "fixed_count", "fields", "doc",
#: "blocks"}``.  Only the types this tool can *address by entry* keep a single
#: list here; types 7/8/12/18/19 have their own block builders below.
SCHEMA = {
    1: {
        "doc": "docs/SBT_RECORDS.md §2.1 (32 bytes, 8 x i32)",
        "count_prefix": (0, sbtmod.SWITCH_MAX),
        "entry_size": 0x20,
        "fields": [
            ("kind", 0x00, "u32", "docs/SBT_RECORDS.md §2.1 @0x44f960"),
            ("id", 0x04, "u32", "docs/SBT_RECORDS.md §2.1 @0x479960"),
            ("radius", 0x08, "u32", "docs/SBT_RECORDS.md §2.1 @0x44fb9e"),
            ("pos.x", 0x0C, "i32", "docs/SBT_RECORDS.md §2.1 @0x44f960"),
            ("pos.y", 0x10, "i32", "docs/SBT_RECORDS.md §2.1 @0x44f960"),
            ("pos.z", 0x14, "i32", "docs/SBT_RECORDS.md §2.1 @0x44f960"),
            ("aux", 0x18, "u32", "docs/SBT_RECORDS.md §2.1 [hypothesis]"),
            ("sound", 0x1C, "i32", "docs/SBT_RECORDS.md §2.1"),
        ],
    },
    2: {
        "doc": "docs/SBT_RECORDS.md §2.2 (184 bytes)",
        "count_prefix": (0, sbtmod.DOOR_MAX),
        "entry_size": 0xB8,
        "fields": (
            _vec("box", 0x00, 9, "f64", 8, "docs/SBT_RECORDS.md §2.2 @0x00")
            + _vec("delta", 0x48, 3, "f64", 8, "docs/SBT_RECORDS.md §2.2 @0x48")
            + [
                ("const", 0x60, "f64", "docs/SBT_RECORDS.md §2.2 @0x44e460"),
                ("flags", 0x68, "i32", "docs/SBT_RECORDS.md §2.2 @0x45068d"),
            ]
            + _vec("ids", 0x6C, 5, "i32", 4, "docs/SBT_RECORDS.md §2.2 @0x44f6c0")
            + [
                ("state", 0x80, "i32", "docs/SBT_RECORDS.md §2.2 @0x450054"),
                ("sector", 0x84, "i32", "docs/SBT_RECORDS.md §2.2 @0x434419"),
                ("object", 0x88, "i32", "docs/SBT_RECORDS.md §2.2 @0x434419"),
                ("phase_prev", 0x8C, "u32", "docs/SBT_RECORDS.md §2.2"),
                ("phase_next", 0x90, "u32", "docs/SBT_RECORDS.md §2.2"),
                ("step", 0x94, "i32", "docs/SBT_RECORDS.md §2.2"),
                ("param5", 0x98, "i32", "docs/SBT_RECORDS.md §2.2"),
                ("param6", 0x9C, "i32", "docs/SBT_RECORDS.md §2.2"),
            ]
            + _vec("v", 0xA0, 3, "i32", 4, "docs/SBT_RECORDS.md §2.2 [hypothesis]")
            + _vec("n", 0xAC, 3, "i32", 4, "docs/SBT_RECORDS.md §2.2 [hypothesis]")
        ),
    },
    3: {
        "doc": "docs/SBT_RECORDS.md §2.3 (u8 exists + optional 0x528)",
        "fixed_count": 1,
        "entry_size": 1,
        # The 0x528 runtime block is not decoded; only the existence flag is a
        # documented field.  ``--list`` hex-dumps the rest.
        "fields": [("exists", 0x00, "u8", "docs/SBT_RECORDS.md §2.3 @0x46fd90")],
        "opaque_after": 1,
    },
    4: {
        "doc": "docs/SBT_RECORDS.md §2.4 (936 bytes)",
        "count_prefix": (0, 0x80),
        "entry_size": 0x3A8,
        "fields": (
            [("name", 0x000, "name16", "docs/SBT_RECORDS.md §2.4")]
            + _vec("param", 0x010, 49, "f64", 8,
                   "docs/SBT_RECORDS.md §2.4 (+0x010..+0x198)")
            + _vec("tail", 0x198, 132, "u32", 4,
                   "docs/SBT_RECORDS.md §2.4 (+0x198..+0x3A8, opaque)")
        ),
        # documented aliases (same bytes as param.0 / tail.3)
        "aliases": {
            "speed": ("param.0",
                      "docs/FORMATS.md §12.2: +0x010 is speed (0x4752d0)"),
            "model": ("tail.3",
                      "docs/SBT_RECORDS.md §2.4: +0x1A4 = SOB_TABLE index, "
                      "proven 2896/2896 (0x47cb6c)"),
        },
    },
    5: {
        "doc": "docs/SBT_RECORDS.md §2.5 (52 bytes)",
        "count_prefix": (0, 0x31),
        "entry_size": 52,
        "fields": (
            [("name", 0x00, "name16", "docs/SBT_RECORDS.md §2.5 (0x468a53)")]
            + _vec("f", 0x10, 9, "u32", 4,
                   "docs/SBT_RECORDS.md §2.5 (%ld ... at +0x10..+0x30)")
        ),
    },
    6: {
        "doc": "docs/SBT_RECORDS.md §2.6 (44 bytes)",
        "count_prefix": (0, sbtmod.SPRITE_MAX),
        "entry_size": 44,
        "fields": [
            ("scale_x", 0x00, "f32", "docs/SBT_RECORDS.md §2.6 @0x456164"),
            ("scale_y", 0x04, "f32", "docs/SBT_RECORDS.md §2.6 @0x456173"),
            ("runtime_object", 0x08, "u32", "docs/SBT_RECORDS.md §2.6 @0x456556"),
            ("x", 0x0C, "i32", "docs/SBT_RECORDS.md §2.6 @0x456136"),
            ("y", 0x10, "i32", "docs/SBT_RECORDS.md §2.6 @0x456140"),
            ("z", 0x14, "i32", "docs/SBT_RECORDS.md §2.6 @0x45614a"),
            ("param", 0x18, "u8", "docs/SBT_RECORDS.md §2.6 [hypothesis]"),
            ("flags", 0x1C, "u32", "docs/SBT_RECORDS.md §2.6 [hypothesis]"),
            ("effect_id", 0x20, "i32", "docs/SBT_RECORDS.md §2.6 @0x456130"),
            ("param16", 0x24, "u16", "docs/SBT_RECORDS.md §2.6 @0x456179"),
            ("spawn_time", 0x28, "u32", "docs/SBT_RECORDS.md §2.6 @0x456185"),
        ],
    },
    8: {
        "doc": "docs/RECORDS.md §2 (256 x 0x58, exactly 0x5800)",
        "fixed_count": 256,
        "entry_size": 0x58,
        "fields": (
            _vec("move", 0x00, 3, "f64", 8, "docs/RECORDS.md §2.2 @0x457815")
            + _vec("extra", 0x18, 3, "f64", 8,
                   "docs/RECORDS.md §2.2 (no reader in this build)")
            + [
                ("sound_param", 0x30, "f64", "docs/RECORDS.md §2.2 @0x46b740"),
                ("event_a", 0x38, "i32", "docs/RECORDS.md §2.2 @0x4579d6"),
                ("event_b", 0x3C, "i32", "docs/RECORDS.md §2.2 @0x4579e0"),
                ("damage_a", 0x40, "u32", "docs/RECORDS.md §2.2 @0x45799a"),
                ("damage_b", 0x44, "i32", "docs/RECORDS.md §2.2 @0x4579a0"),
                ("sound", 0x48, "i32", "docs/RECORDS.md §2.2 @0x46b76a"),
                ("sound_count", 0x4C, "i32", "docs/RECORDS.md §2.2 @0x46b760"),
                ("flags", 0x50, "u32", "docs/RECORDS.md §2.2 @0x457955"),
                ("unused", 0x54, "u32",
                 "docs/RECORDS.md §2.2 [hypothesis: dead field]"),
            ]
        ),
    },
    9: {
        "doc": "docs/SBT_RECORDS.md §2.9 (u8 exists + optional 0x3510)",
        "fixed_count": 1,
        "entry_size": 1,
        "fields": [("exists", 0x00, "u8", "docs/SBT_RECORDS.md §2.9 @0x451c20")],
        "opaque_after": 1,
    },
    18: {
        "doc": "docs/RECORDS.md §4 (u32 n + n x 0xA8 + u32 k + 3 columns)",
        "count_prefix": (0, sbtmod.LIFT_MAX),
        "entry_size": 0xA8,
        "fields": (
            _vec("basis0", 0x00, 3, "f64", 8, "docs/RECORDS.md §4 @0x44ea6a")
            + _vec("basis1", 0x18, 3, "f64", 8, "docs/RECORDS.md §4 @0x44ea6a")
            + [
                ("sob_index", 0x30, "i32", "docs/RECORDS.md §4 @0x44e706"),
                ("kind", 0x34, "i32", "docs/RECORDS.md §4 @0x44f7b6"),
                ("sob_index2", 0x38, "i32",
                 "docs/RECORDS.md §4 @0x44e9d4 [semantics: hypothesis]"),
                ("x", 0x3C, "i32", "docs/RECORDS.md §4 @0x44e743"),
                ("y", 0x40, "i32", "docs/RECORDS.md §4 @0x44e755"),
                ("z", 0x44, "i32", "docs/RECORDS.md §4 @0x44e767"),
                ("speed", 0x54, "i32", "docs/RECORDS.md §4 @0x4501c5"),
                ("height", 0x5C, "i32", "docs/RECORDS.md §4 @0x4501c2"),
                ("target", 0x60, "i32", "docs/RECORDS.md §4 @0x4501ce"),
                ("moving", 0x68, "i32", "docs/RECORDS.md §4 @0x4501a1"),
                ("stop_index", 0x6C, "i32", "docs/RECORDS.md §4 @0x44f7d0"),
                ("call_1", 0x70, "i32", "docs/RECORDS.md §4 @0x44f7e0"),
                ("call_2", 0x74, "i32", "docs/RECORDS.md §4 @0x44f7f7"),
                ("call_3", 0x78, "i32", "docs/RECORDS.md §4 @0x44f7f7"),
                ("stop_count", 0x9C, "i32", "docs/RECORDS.md §4 @0x44ebc3"),
                ("stop_start", 0xA0, "i32", "docs/RECORDS.md §4 @0x44e9c4"),
            ]
        ),
        # after the n * 0xA8 records: u32 k + 3 parallel u32 columns
        "stop_columns": [
            ("floor", "docs/RECORDS.md §4 (0x5a9130, floor/button id)"),
            ("height", "docs/RECORDS.md §4 (0x5a8e10, clamp 0xffff)"),
            ("sound", "docs/RECORDS.md §4 (0x5a8af0, sound/effect id)"),
        ],
    },
    19: {
        "doc": "docs/RECORDS.md §3 (300 x 0x60, exactly 0x7080)",
        "fixed_count": 300,
        "entry_size": 0x60,
        "fields": (
            _vec("basis", 0x00, 3, "f64", 8, "docs/RECORDS.md §3 @0x4688ad")
            + _vec("position", 0x18, 3, "f64", 8,
                   "docs/RECORDS.md §3 @0x468507")
            + [
                ("runtime_object", 0x30, "u32", "docs/RECORDS.md §3 @0x468619"),
                ("kind", 0x34, "u32", "docs/RECORDS.md §3 @0x468710"),
                ("state", 0x38, "i32", "docs/RECORDS.md §3 @0x468e41"),
                ("op2", 0x3C, "u32", "docs/RECORDS.md §3 (operand a2)"),
                ("sob_index", 0x40, "i32", "docs/RECORDS.md §3 @0x4684b0"),
                ("op3", 0x44, "u32", "docs/RECORDS.md §3 (operand a3)"),
                ("op4", 0x48, "u32", "docs/RECORDS.md §3 (operand a4)"),
                ("op5", 0x4C, "u32", "docs/RECORDS.md §3 (operand a5)"),
                ("op6", 0x50, "u32", "docs/RECORDS.md §3 (operand a6)"),
                ("timer_reload", 0x54, "u32", "docs/RECORDS.md §3 @0x4690d8"),
                ("flags", 0x58, "u8", "docs/RECORDS.md §3 @0x468516"),
                ("source_addend", 0x5C, "u32",
                 "docs/RECORDS.md §3 @0x4692d8 (eax & 0xffff)"),
            ]
        ),
    },
    20: {
        "doc": "docs/SBT_RECORDS.md §2.14 (20 bytes)",
        "count_prefix": (0, 0xC8),
        "entry_size": 20,
        "fields": [
            ("name", 0x00, "name12", "docs/SBT_RECORDS.md §2.14"),
            ("amount", 0x0C, "u32",
             "docs/SBT_RECORDS.md §2.14 (never read back by the loader)"),
            ("ptr", 0x10, "u32",
             "docs/SBT_RECORDS.md §2.14 (stale, overwritten at 0x47c845)"),
        ],
    },
}

#: Downloadable key fields shown by ``--list`` per record type.
KEY_FIELDS = {
    1: ["kind", "id", "radius", "pos.x", "pos.y", "pos.z", "sound"],
    2: ["flags", "state", "sector", "object", "phase_prev", "phase_next",
        "step"],
    3: ["exists"],
    4: ["name", "speed", "model"],
    5: ["name", "f.0", "f.1", "f.2", "f.3", "f.4", "f.5", "f.6", "f.7", "f.8"],
    6: ["scale_x", "scale_y", "x", "y", "z", "param", "flags", "effect_id",
        "param16", "spawn_time"],
    7: ["x", "y", "z", "volume", "sample", "atten"],
    8: ["move.x", "move.y", "move.z", "sound_param", "event_a", "event_b",
        "damage_a", "damage_b", "sound", "sound_count", "flags"],
    9: ["exists"],
    12: ["count", "name"],
    15: ["tag", "sector", "object", "poly"],
    16: ["tag", "sector", "object"],
    17: ["tag", "sector"],
    18: ["sob_index", "kind", "sob_index2", "x", "y", "z", "speed", "height",
         "target", "moving", "stop_index", "stop_count", "stop_start"],
    19: ["kind", "state", "sob_index", "op2", "op3", "op4", "op5", "op6",
         "timer_reload", "flags", "source_addend"],
    20: ["name", "amount", "ptr"],
}


# ---------------------------------------------------------------------------
# payload blocks


@dataclass
class Field:
    """One addressable scalar: element 0 payload offset + stride."""

    name: str
    offset: int
    stride: int
    kind: str
    prov: str = ""


@dataclass
class Block:
    label: str
    note: str
    count: int
    fields: dict                     # name -> Field

    def has(self, name):
        return name in self.fields


def _need(payload: bytes, off: int, size: int, what: str) -> None:
    if off < 0 or off + size > len(payload):
        raise EditError("%s: %d bytes at 0x%x are outside the %d-byte payload"
                        % (what, size, off, len(payload)))


def _u32_at(payload: bytes, off: int, what: str) -> int:
    _need(payload, off, 4, what)
    return struct.unpack_from("<I", payload, off)[0]


def _entry_fields(entry_base, entry_size, specs, where) -> dict:
    out = {}
    for name, off, kind, prov in specs:
        size = NAMES[kind] if kind in NAMES else KINDS[kind][1]
        if off + size > entry_size:
            raise EditError("%s: field %s at +0x%x (%d bytes) overflows the "
                            "0x%x-byte entry" % (where, name, off, size,
                                                 entry_size))
        out[name] = Field(name, entry_base + off, entry_size, kind, prov)
    return out


def _column_fields(col_base, count, specs) -> dict:
    """``specs`` = (name, kind, provenance) in file (column) order.

    Element *i* of a column lives at ``col_base + column_offset + i * size``;
    the caller has already checked the total payload length, and every field
    offset is bounds-checked again in :func:`resolve_field`.
    """
    out = {}
    off = col_base
    for name, kind, prov in specs:
        size = KINDS[kind][1]
        out[name] = Field(name, off, size, kind, prov)
        off += count * size
    return out


def blocks_of(type_: int, payload: bytes) -> list:
    """Decode the payload's *block structure* (counts + field offsets).

    Only counts and offsets are read here -- values stay in ``payload``.  The
    counts/limits are the loader's (``docs/SBT_RECORDS.md`` §1); the oracle has
    already validated the payload, and this function re-checks the bounds it
    needs before touching a byte.
    """
    n = len(payload)
    blocks = []

    if type_ in (1, 2, 4, 5, 6, 20):
        spec = SCHEMA[type_]
        count = _u32_at(payload, 0, spec["doc"])
        limit = spec["count_prefix"][1]
        if count > limit:
            raise EditError("%s: count %d exceeds the loader limit %d"
                            % (spec["doc"], count, limit))
        size = spec["entry_size"]
        _need(payload, 4, count * size, spec["doc"])
        if 4 + count * size != n:
            raise EditError("%s: payload is %d bytes, expected %d (%d x 0x%x)"
                            % (spec["doc"], n, 4 + count * size, count, size))
        blocks.append(Block(TYPE_NAMES[type_], spec["doc"], count,
                            _entry_fields(4, size, spec["fields"],
                                          TYPE_NAMES[type_])))
        return blocks

    if type_ in (3, 9):
        spec = SCHEMA[type_]
        exists = payload[0] if payload else None
        if exists is None:
            raise EditError("%s: empty payload" % TYPE_NAMES[type_])
        body = 0x528 if type_ == 3 else 0x3510
        want = 1 + (body if exists else 0)
        if n != want:
            raise EditError("%s: payload is %d bytes, expected %d (exists=%d)"
                            % (TYPE_NAMES[type_], n, want, exists))
        blocks.append(Block(TYPE_NAMES[type_], spec["doc"], 1,
                            _entry_fields(0, 1, spec["fields"],
                                          TYPE_NAMES[type_])))
        return blocks

    if type_ == 8:
        spec = SCHEMA[8]
        if n != 0x5800:
            raise EditError("SECTOR_STUFF: payload is %d bytes, expected 0x5800"
                            % n)
        blocks.append(Block("SECTOR_STUFF", spec["doc"], 256,
                            _entry_fields(0, 0x58, spec["fields"],
                                          "SECTOR_STUFF")))
        return blocks

    if type_ == 19:
        spec = SCHEMA[19]
        if n != 0x7080:
            raise EditError("ITEM: payload is %d bytes, expected 0x7080" % n)
        blocks.append(Block("ITEM", spec["doc"], 300,
                            _entry_fields(0, 0x60, spec["fields"], "ITEM")))
        return blocks

    if type_ == 12:
        if n != 0x4B0 + 0xA8C:
            raise EditError("SOUND_SAMPLE_TABLE: payload is %d bytes, expected "
                            "0x%x" % (n, 0x4B0 + 0xA8C))
        blocks.append(Block("counts",
                            "docs/SBT_RECORDS.md §2.10 (300 x u32)",
                            300, _entry_fields(0, 4, [
                                ("count", 0, "u32",
                                 "docs/SBT_RECORDS.md §2.10 (reference count)")],
                                "sample counts")))
        blocks.append(Block("names",
                            "docs/SBT_RECORDS.md §2.10 (300 x 9 bytes)",
                            300, _entry_fields(0x4B0, 9, [
                                ("name", 0, "name9",
                                 "docs/SBT_RECORDS.md §2.10 (SOU.IDX name)")],
                                "sample names")))
        return blocks

    if type_ == 7:
        count = _u32_at(payload, 0, "SOUND_SOURCES")
        if count > sbtmod.SOUND_SOURCE_MAX:
            raise EditError("SOUND_SOURCES: count %d exceeds %d"
                            % (count, sbtmod.SOUND_SOURCE_MAX))
        want = 4 + 5 * count * 4 + count * 8
        if n != want:
            raise EditError("SOUND_SOURCES: payload is %d bytes, expected %d"
                            % (n, want))
        blocks.append(Block(
            "SOUND_SOURCES", "docs/SBT_RECORDS.md §2.7 (columns, not structs)",
            count, _column_fields(4, count, [
                ("x", "i32", "docs/SBT_RECORDS.md §2.7 col 1 (0x800340)"),
                ("y", "i32", "docs/SBT_RECORDS.md §2.7 col 2 (0x800540)"),
                ("z", "i32", "docs/SBT_RECORDS.md §2.7 col 3 (0x800140)"),
                ("volume", "i32", "docs/SBT_RECORDS.md §2.7 col 4 (0x7ffd40)"),
                ("sample", "i32", "docs/SBT_RECORDS.md §2.7 col 5 (0x7fff40)"),
                ("atten", "f64", "docs/SBT_RECORDS.md §2.7 col 6 (0x800760, "
                                 "docs/AUDIO_3D.md §2.2)"),
            ])))
        return blocks

    if type_ in (15, 16, 17):
        names = {15: ("tag", "sector", "object", "poly"),
                 16: ("tag", "sector", "object"),
                 17: ("tag", "sector")}[type_]
        count = _u32_at(payload, 0, TYPE_NAMES[type_])
        limit = {15: 0x1F3, 16: 0x12B, 17: 0xFF}[type_]
        if count > limit:
            raise EditError("%s: count %d exceeds the loader limit 0x%x"
                            % (TYPE_NAMES[type_], count, limit))
        want = 4 + count * 4 * len(names)
        if n != want:
            raise EditError("%s: payload is %d bytes, expected %d (%d x %d "
                            "u32 columns)" % (TYPE_NAMES[type_], n, want, count,
                                              len(names)))
        blocks.append(Block(
            TYPE_NAMES[type_],
            "docs/SBT_RECORDS.md §2.11 (column-major, NOT {u32 x %d})"
            % len(names), count,
            _column_fields(4, count, [
                (nm, "u32", "docs/SBT_RECORDS.md §2.11 column %d" % (i + 1))
                for i, nm in enumerate(names)])))
        return blocks

    if type_ == 18:
        spec = SCHEMA[18]
        count = _u32_at(payload, 0, spec["doc"])
        if count > sbtmod.LIFT_MAX:
            raise EditError("LIFT: count %d exceeds %d"
                            % (count, sbtmod.LIFT_MAX))
        base = 4
        _need(payload, base, count * 0xA8, "LIFT")
        stops_off = base + count * 0xA8
        stops = _u32_at(payload, stops_off, "LIFT/stops")
        if stops > sbtmod.STOP_MAX:
            raise EditError("LIFT: stop count %d exceeds %d"
                            % (stops, sbtmod.STOP_MAX))
        want = stops_off + 4 + stops * 12
        if n != want:
            raise EditError("LIFT: payload is %d bytes, expected %d (%d lifts, "
                            "%d stops)" % (n, want, count, stops))
        blocks.append(Block("lifts", spec["doc"], count,
                            _entry_fields(base, 0xA8, spec["fields"], "LIFT")))
        blocks.append(Block(
            "stops", "docs/RECORDS.md §4 (three parallel u32 columns)",
            stops, _column_fields(stops_off + 4, stops, [
                (nm, "u32", prov) for nm, prov in spec["stop_columns"]])))
        return blocks

    if type_ == SCRIPT_TYPE:
        raise EditError("record type 11 SCRIPT is compiled script code, not a "
                        "scene record; edit it with tools/editor/sbtwrite.py "
                        "(sub-script replacement; docs/SBT_WRITE.md §2)")
    if type_ == END:
        raise EditError("END_OF_FILE (255) is the terminator, not a record")
    if type_ in UNSUPPORTED:
        raise EditError("record type %d is an engine stub (0x4971b0, no "
                        "payload) and cannot be edited" % type_)
    raise EditError("unknown record type %d" % type_)


# ---------------------------------------------------------------------------
# container access


@dataclass
class SceneRecord:
    index: int               # index in the scene-record list (``--record N``)
    slot: int                # index in ``Sbt.records``
    rec: object              # gm.sbt.Record


def scene_records(sbt) -> list:
    out = []
    for slot, rec in enumerate(sbt.records):
        if rec.type in SKIP_TYPES:
            continue
        out.append(SceneRecord(len(out), slot, rec))
    return out


def resolve_record(sbt, index: int) -> SceneRecord:
    rows = scene_records(sbt)
    if index < 0 or index >= len(rows):
        raise EditError("record %d does not exist: the file has %d scene "
                        "record(s) (indices 0..%d); use --list"
                        % (index, len(rows), len(rows) - 1))
    return rows[index]


def parse_ref(ref: str):
    """``NAME`` / ``NAME[K]`` -> ``(name, index|None)``."""
    ref = (ref or "").strip()
    if not ref:
        raise EditError("empty field name")
    if ref.endswith("]"):
        head, _, tail = ref[:-1].rpartition("[")
        if not head:
            raise EditError("bad field reference %r (use NAME or NAME[INDEX])"
                            % ref)
        try:
            idx = int(tail, 0)
        except ValueError:
            raise EditError("field index in %r is not a number" % ref)
        if idx < 0:
            raise EditError("field index in %r is negative" % ref)
        return head, idx
    if "[" in ref:
        raise EditError("bad field reference %r (unclosed '[')" % ref)
    return ref, None


def resolve_field(type_: int, payload: bytes, ref: str):
    """Return ``(block, field, index, payload_offset)`` for a field reference."""
    name, index = parse_ref(ref)
    requested = name
    blocks = blocks_of(type_, payload)
    spec = SCHEMA.get(type_, {})
    prefix = None
    if "." in name:                          # optional BLOCK.field prefix
        head, _, rest = name.partition(".")
        if any(b.label == head for b in blocks) and any(
                b.has(rest) for b in blocks if b.label == head):
            prefix, name = head, rest
    alias = spec.get("aliases", {}).get(name)
    if alias:
        name = alias[0]
    pool = [b for b in blocks if prefix is None or b.label == prefix]
    hits = [b for b in pool if b.has(name)]
    if not hits:
        known = sorted({("%s.%s" % (b.label, n)) if sum(
            1 for o in blocks if o.has(n)) > 1 else n
            for b in blocks for n in b.fields})
        shown = ", ".join(known[:16])
        if len(known) > 16:
            shown += " ... (%d fields total; see --fields)" % len(known)
        raise EditError("record type %d %s has no field %r; fields: %s"
                        % (type_, TYPE_NAMES.get(type_, "?"), requested,
                           shown))
    if len(hits) > 1:
        raise EditError("field %r exists in blocks %s; write BLOCK.field[K] "
                        "(e.g. %s.%s[0])"
                        % (ref, " and ".join(repr(b.label) for b in hits),
                           hits[0].label, name))
    block = hits[0]
    fld = block.fields[name]
    if index is None:
        if block.count > 1:
            raise EditError("field %r has %d elements; write %s[INDEX] with "
                            "INDEX in 0..%d" % (requested, block.count,
                                                requested, block.count - 1))
        index = 0
    if index >= block.count:
        raise EditError("%s[%d]: element index out of range, the block has %d "
                        "element(s) (0..%d)" % (requested, index, block.count,
                                                block.count - 1))
    size = NAMES[fld.kind] if fld.kind in NAMES else KINDS[fld.kind][1]
    off = fld.offset + index * fld.stride
    _need(payload, off, size, "field %r" % ref)
    return block, fld, index, off


def read_field(payload: bytes, fld: Field, off: int):
    size = NAMES[fld.kind] if fld.kind in NAMES else KINDS[fld.kind][1]
    raw = payload[off:off + size]
    if fld.kind in NAMES:
        return raw.split(b"\0", 1)[0].decode("latin1")
    return struct.unpack(KINDS[fld.kind][0], raw)[0]


def pack_value(fld: Field, text: str) -> bytes:
    """Parse ``text`` for ``fld.kind`` and return the bytes to store."""
    if fld.kind in NAMES:
        slot = NAMES[fld.kind]
        try:
            raw = text.encode("latin1")
        except UnicodeEncodeError:
            raise EditError("value %r is not latin-1 (the format stores raw "
                            "8-bit bytes)" % text)
        if len(raw) >= slot:
            raise EditError("value %r is %d bytes; a %s slot holds at most %d "
                            "bytes plus NUL" % (text, len(raw), fld.kind,
                                                slot - 1))
        return raw + b"\0" * (slot - len(raw))
    if fld.kind in ("f32", "f64"):
        try:
            value = float(text)
        except ValueError:
            raise EditError("value %r is not a number (%s field)"
                            % (text, fld.kind))
        if value != value or value in (float("inf"), float("-inf")):
            raise EditError("value %r is not finite; the engine has no "
                            "NaN/Inf handling" % text)
        code, size = KINDS[fld.kind]
        try:
            return struct.pack(code, value)
        except (OverflowError, struct.error):
            raise EditError("value %r does not fit a %s" % (text, fld.kind))
    lo, hi = INT_RANGE[fld.kind]
    try:
        value = int(text, 0)
    except ValueError:
        raise EditError("value %r is not an integer (%s field)"
                        % (text, fld.kind))
    if not lo <= value <= hi:
        raise EditError("value %d is outside %s range [%d, %d]"
                        % (value, fld.kind, lo, hi))
    return struct.pack(KINDS[fld.kind][0], value)


def refresh_offsets(sbt) -> None:
    off = sbt.header_size
    for rec in sbt.records:
        rec.offset = off
        off += rec.size


def replace_record_bytes(sbt, slot: int, payload: bytes):
    """Return a patched deep copy of ``sbt`` with one record's payload replaced."""
    out = copy.deepcopy(sbt)
    rec = out.records[slot]
    if rec.type in SKIP_TYPES:
        blocks_of(rec.type, rec.raw)            # raises the refusal message
    try:
        parsed = sbtmod.parse_payload(rec.type, payload)
    except (ValueError, struct.error) as exc:
        raise EditError("%s: replacement blob is not a valid payload: %s"
                        % (rec.name, exc))
    rec.payload = parsed
    rec.raw = bytes(payload)
    rec.size = 1 + len(rec.raw)
    refresh_offsets(out)
    return out


# ---------------------------------------------------------------------------
# independent container walker (does not use gm.sbt)
#
# Used by --check/--roundtrip as a second opinion on record boundaries and
# payload lengths.  Sizes are from the handler limits in docs/SBT_RECORDS.md §1
# and the parser in gm/sbt.py:647-741; SCRIPT (11) is skipped, not edited.


def _walk_size(data: bytes, pos: int, type_: int) -> int:
    n = len(data)

    def u32(off):
        if off + 4 > n:
            raise EditError("record at 0x%x: EOF reading u32" % pos)
        return struct.unpack_from("<I", data, off)[0]

    def counted(limit, entry, what):
        c = u32(pos)
        if c > limit:
            raise EditError("%s at 0x%x: count %d > limit %d"
                            % (what, pos, c, limit))
        size = 4 + c * entry
        if pos + size > n:
            raise EditError("%s at 0x%x: truncated (%d of %d bytes)"
                            % (what, pos, n - pos, size))
        return size

    if type_ == 1:
        return counted(sbtmod.SWITCH_MAX, 0x20, "SWITCH")
    if type_ == 2:
        return counted(sbtmod.DOOR_MAX, 0xB8, "DOOR")
    if type_ == 3:
        return 1 + (0x528 if data[pos] else 0)
    if type_ == 4:
        return counted(0x80, 0x3A8, "THING_TYPES")
    if type_ == 5:
        return counted(0x31, 52, "NAMED_ITEMS")
    if type_ == 6:
        return counted(sbtmod.SPRITE_MAX, 44, "SPRITES")
    if type_ == 7:
        c = u32(pos)
        if c > sbtmod.SOUND_SOURCE_MAX:
            raise EditError("SOUND_SOURCES at 0x%x: count %d > %d"
                            % (pos, c, sbtmod.SOUND_SOURCE_MAX))
        return 4 + c * 28
    if type_ == 8:
        if pos + 0x5800 > n:
            raise EditError("SECTOR_STUFF at 0x%x: truncated" % pos)
        return 0x5800
    if type_ == 9:
        return 1 + (0x3510 if data[pos] else 0)
    if type_ == 11:
        base = pos
        c = u32(pos)
        if c > 0xC8:
            raise EditError("SCRIPT at 0x%x: things count %d > 0xC8" % (pos, c))
        pos += 4 + c * 40
        cnt = u32(pos)
        if cnt > 0x400:
            raise EditError("SCRIPT at 0x%x: handle count %d > 0x400"
                            % (pos, cnt))
        pos += 4 + cnt * 12
        m = u32(pos)
        if m > 0x30000:
            raise EditError("SCRIPT at 0x%x: code length %d > 0x30000"
                            % (pos, m))
        pos += 4 + m
        if pos + 1 > n:
            raise EditError("SCRIPT at 0x%x: truncated flag" % base)
        flag = data[pos]
        pos += 1
        if flag:
            pos += 9 * 0x400
        if pos > n:
            raise EditError("SCRIPT at 0x%x: truncated tables" % base)
        return pos - base
    if type_ == 12:
        if pos + 0x4B0 + 0xA8C > n:
            raise EditError("SOUND_SAMPLE_TABLE at 0x%x: truncated" % pos)
        return 0x4B0 + 0xA8C
    if type_ in (15, 16, 17):
        cols, limit = {15: (4, 0x1F3), 16: (3, 0x12B), 17: (2, 0xFF)}[type_]
        c = u32(pos)
        if c > limit:
            raise EditError("%s at 0x%x: count %d > 0x%x"
                            % (TYPE_NAMES[type_], pos, c, limit))
        size = 4 + c * 4 * cols
        if pos + size > n:
            raise EditError("%s at 0x%x: truncated" % (TYPE_NAMES[type_], pos))
        return size
    if type_ == 18:
        c = u32(pos)
        if c > sbtmod.LIFT_MAX:
            raise EditError("LIFT at 0x%x: lift count %d > %d"
                            % (pos, c, sbtmod.LIFT_MAX))
        off = pos + 4 + c * 0xA8
        k = u32(off) if off + 4 <= n else None
        if k is None:
            raise EditError("LIFT at 0x%x: truncated stop count" % pos)
        if k > sbtmod.STOP_MAX:
            raise EditError("LIFT at 0x%x: stop count %d > %d"
                            % (pos, k, sbtmod.STOP_MAX))
        size = 4 + c * 0xA8 + 4 + k * 12
        if pos + size > n:
            raise EditError("LIFT at 0x%x: truncated stop columns" % pos)
        return size
    if type_ == 19:
        if pos + 0x7080 > n:
            raise EditError("ITEM at 0x%x: truncated" % pos)
        return 0x7080
    if type_ == 20:
        return counted(0xC8, 20, "SOB_TABLE")
    if type_ in UNSUPPORTED:
        raise EditError("record type %d at 0x%x is an engine stub with no "
                        "payload; refusing to guess its size" % (type_, pos))
    if type_ == 14:
        raise EditError("record type 14 at 0x%x is the 'World Things file (%s) "
                        "corrupt' path, not a record" % pos)
    raise EditError("unknown record type %d at 0x%x" % (type_, pos))


def walk_container(data: bytes) -> list:
    """Split an ``.sbt`` into records **without** using ``gm.sbt``.

    Returns ``[{"type", "offset", "size", "payload"}]``; raises
    :class:`EditError` on a bad header, an unknown type, a truncated payload,
    a missing/short ``END_OF_FILE`` or trailing bytes.
    """
    if len(data) < HEADER_SIZE:
        raise EditError("file is %d bytes, too short for the 84-byte header"
                        % len(data))
    if not data.startswith(MAGIC):
        raise EditError("bad magic %r (expected %r)" % (data[:12], MAGIC))
    version = struct.unpack_from("<I", data, 0x50)[0]
    if version != 0:
        raise EditError("unsupported .sbt version %d (must be 0)" % version)
    out = []
    pos = HEADER_SIZE
    while pos < len(data):
        off = pos
        type_ = data[pos]
        pos += 1
        if type_ == END:
            out.append({"type": END, "offset": off, "size": 1, "payload": b""})
            break
        size = _walk_size(data, pos, type_)
        out.append({"type": type_, "offset": off, "size": 1 + size,
                    "payload": data[pos:pos + size]})
        pos += size
    if not out or out[-1]["type"] != END:
        raise EditError("record stream has no END_OF_FILE (255) terminator")
    if pos != len(data):
        raise EditError("%d trailing byte(s) after END_OF_FILE" % (len(data) - pos))
    return out


# ---------------------------------------------------------------------------
# loading / writing


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_bytes(path: str) -> bytes:
    try:
        return sbtio.read_bytes(path)
    except sbtio.WriteError as exc:                # same user-facing message
        raise EditError(str(exc))


def write_bytes(path: str, data: bytes) -> None:
    try:
        sbtio.write_bytes(path, data)
    except sbtio.WriteError as exc:
        raise EditError(str(exc))


def level_path(name, game=DEFAULT_GAME) -> str:
    """Path or level stem (``level0``/``level0.sbt``) -> existing ``.sbt``."""
    if os.path.isfile(name):
        return name
    for cand in (name, name + ".sbt"):
        path = os.path.join(game, cand)
        if os.path.isfile(path):
            return path
    raise EditError("level %r not found (not a file, and no %s.sbt under %s); "
                    "use --game DIR" % (name, name, game))


def load_level(level, game=DEFAULT_GAME):
    path = level_path(level, game)
    data = read_bytes(path)
    try:
        sbt = sbtmod.parse(data)
    except (ValueError, struct.error) as exc:
        raise EditError("%s: %s" % (path, exc))
    return path, data, sbt


def container_problems(sbt, data: bytes) -> list:
    """Oracle structure check + this tool's own walker agree with ``data``."""
    problems = list(sbtio.structure_problems(sbt, data))
    try:
        walked = walk_container(data)
    except EditError as exc:
        problems.append("own walker: %s" % exc)
        return problems
    if len(walked) != len(sbt.records):
        problems.append("own walker sees %d records, oracle %d"
                        % (len(walked), len(sbt.records)))
    for i, (w, r) in enumerate(zip(walked, sbt.records)):
        if w["type"] != r.type:
            problems.append("record #%d: walker type %d, oracle %d"
                            % (i, w["type"], r.type))
        elif w["payload"] != r.raw:
            problems.append("record #%d %s: walker payload differs from "
                            "oracle raw" % (i, r.name))
        elif w["size"] != r.size:
            problems.append("record #%d %s: walker size %d, oracle %d"
                            % (i, r.name, w["size"], r.size))
    return problems


def compare_records(src_sbt, dst_sbt, target_slots=()) -> list:
    """Problems unless every non-target record is byte-identical."""
    problems = []
    if len(src_sbt.records) != len(dst_sbt.records):
        return ["record count changed: %d -> %d"
                % (len(src_sbt.records), len(dst_sbt.records))]
    for i, (a, b) in enumerate(zip(src_sbt.records, dst_sbt.records)):
        if i in target_slots:
            if a.type != b.type:
                problems.append("record #%d type changed %d -> %d"
                                % (i, a.type, b.type))
            continue
        if a.type != b.type:
            problems.append("record #%d type changed %d -> %d"
                            % (i, a.type, b.type))
        elif a.raw != b.raw:
            problems.append("record #%d %s bytes changed (%d -> %d bytes)"
                            % (i, a.name, len(a.raw), len(b.raw)))
    return problems


def commit_edit(src_path, src_data, src_sbt, edited_sbt, target_slot,
                out_path) -> dict:
    """Serialise, write, re-read and prove only the target record changed."""
    try:
        out_data = edited_sbt.to_bytes()
    except (ValueError, struct.error) as exc:
        raise EditError("cannot re-serialise the container: %s" % exc)
    write_bytes(out_path, out_data)
    back_data = read_bytes(out_path)
    problems = []
    if back_data != out_data:
        problems.append("written file differs from the bytes produced in memory")
    try:
        back_sbt = sbtmod.parse(back_data)
    except (ValueError, struct.error) as exc:
        raise EditError("the written file does not parse: %s" % exc)
    problems.extend(container_problems(back_sbt, back_data))
    other_diffs = compare_records(src_sbt, back_sbt, (target_slot,))
    problems.extend(other_diffs)
    rep = {
        "input": src_path,
        "output": out_path,
        "sha256_in": sha256(src_data),
        "sha256_out": sha256(back_data),
        "bytes_in": len(src_data),
        "bytes_out": len(back_data),
        "size_delta": len(back_data) - len(src_data),
        "records": len(back_sbt.records),
        "problems": problems,
    }
    rep["others_identical"] = not other_diffs
    rep["ok"] = not problems
    return rep, back_sbt, back_data


# ---------------------------------------------------------------------------
# --list


def _fmt_value(v):
    if isinstance(v, float):
        return ("%.10g" % v)
    return str(v)


def _is_empty(type_, block, payload, index) -> bool:
    names = block.fields
    if type_ == 19 and "state" in names:
        return read_field(payload, names["state"],
                          names["state"].offset
                          + index * names["state"].stride) == -2
    if type_ == 8:
        raw = payload[index * 0x58:(index + 1) * 0x58]
        return not any(raw)
    if type_ == 20 and "name" in names:
        return read_field(payload, names["name"],
                          names["name"].offset
                          + index * names["name"].stride) == ""
    if type_ == 12:
        fld = names["name"] if "name" in names else names.get("count")
        if fld is None:
            return False
        val = read_field(payload, fld, fld.offset + index * fld.stride)
        return val == "" if isinstance(val, str) else val == 0
    return False


def list_level(path, sbt, limit=8) -> dict:
    records = []
    scene = scene_records(sbt)
    by_slot = {r.slot: r for r in scene}
    for slot, rec in enumerate(sbt.records):
        row = {
            "type": rec.type,
            "name": rec.name,
            "slot": slot,
            "offset": rec.offset,
            "size": rec.size,
            "payload_size": len(rec.raw),
        }
        if rec.type == SCRIPT_TYPE:
            row["editable"] = False
            row["why"] = ("compiled SCRIPT code; use tools/editor/sbtwrite.py")
            sc = rec.payload
            row["script"] = {"things": sc.count, "handles": sc.n,
                             "code_bytes": sc.m, "flag": sc.flag,
                             "tables": len(sc.tables)}
        elif rec.type == END:
            row["editable"] = False
            row["why"] = "terminator"
        else:
            row["record"] = by_slot[slot].index
            row["editable"] = True
            row["doc"] = SCHEMA.get(rec.type, {}).get("doc", "")
            try:
                blocks = blocks_of(rec.type, rec.raw)
            except EditError as exc:
                row["editable"] = False
                row["why"] = str(exc)
                records.append(row)
                continue
            row["blocks"] = []
            aliases = SCHEMA.get(rec.type, {}).get("aliases", {})
            for block in blocks:
                keys = []
                for k in KEY_FIELDS.get(rec.type, []):
                    if k in block.fields:
                        keys.append((k, block.fields[k]))
                    elif k in aliases:
                        target = block.fields.get(aliases[k][0])
                        if target is not None:
                            keys.append((k, target))
                shown = 0
                entries = []
                for idx in range(block.count):
                    if shown >= limit:
                        break
                    if _is_empty(rec.type, block, rec.raw, idx):
                        continue
                    values = {}
                    for k, fld in keys:
                        values[k] = read_field(
                            rec.raw, fld, fld.offset + idx * fld.stride)
                    entries.append({"index": idx, "values": values})
                    shown += 1
                row["blocks"].append({
                    "label": block.label,
                    "note": block.note,
                    "count": block.count,
                    "keys": [k for k, _ in keys],
                    "shown": entries,
                })
            if rec.type in (3, 9) and rec.payload.exists and limit:
                body = rec.raw[1:]
                row["hex"] = body[:64].hex()
        records.append(row)
    return {
        "tool": "sceneedit",
        "mode": "list",
        "file": path,
        "bytes": 84 + sum(r.size for r in sbt.records),
        "sha256": sha256(open(path, "rb").read()),
        "version": sbt.version,
        "records": records,
        "scene_records": len(scene),
    }


def print_list(rep, verbose=False) -> None:
    print("%s: %d bytes, %d records, %d scene record(s), sha256 %s"
          % (rep["file"], rep["bytes"], len(rep["records"]),
             rep["scene_records"], rep["sha256"][:16]))
    for row in rep["records"]:
        idx = ("#%-3d" % row["record"]) if "record" in row else "----"
        print("%s type %-3d %-19s off 0x%-6x size %-7d payload %d%s"
              % (idx, row["type"], row["name"], row["offset"], row["size"],
                 row["payload_size"],
                 "" if row.get("editable") else "  [not editable]"))
        if not row.get("editable"):
            print("      %s" % row.get("why", ""))
            if "script" in row:
                sc = row["script"]
                print("      things %d, handles %d, code %d bytes, flag %d, "
                      "tables %d bytes"
                      % (sc["things"], sc["handles"], sc["code_bytes"],
                         sc["flag"], sc["tables"]))
            continue
        for block in row["blocks"]:
            print("      %-14s %d element(s)  %s"
                  % (block["label"], block["count"], block["note"]))
            for ent in block["shown"]:
                print("        [%d] %s" % (ent["index"], "  ".join(
                    "%s=%s" % (k, _fmt_value(v))
                    for k, v in ent["values"].items())))
        if "hex" in row:
            print("      hex %s" % row["hex"])


# ---------------------------------------------------------------------------
# --get / --set


def field_value_json(rec, block, fld, index, off, value) -> dict:
    kind = fld.kind
    size = NAMES[kind] if kind in NAMES else KINDS[kind][1]
    return {
        "record": None,
        "field": fld.name,
        "index": index,
        "block": block.label,
        "kind": kind,
        "value": value,
        "payload_offset": off,
        "payload_size": size,
        "doc": fld.prov,
        "raw": rec.raw[off:off + size].hex() if off + size <= len(rec.raw) else "",
    }


def _apply_alias_doc(type_: int, ref: str, fld: Field) -> None:
    """Prefer the alias documentation (speed/model) in reports."""
    alias = SCHEMA.get(type_, {}).get("aliases", {}).get(parse_ref(ref)[0])
    if alias:
        fld.prov = alias[1]


def get_field(sbt, index, ref) -> dict:
    row = resolve_record(sbt, index)
    rec = row.rec
    block, fld, idx, off = resolve_field(rec.type, rec.raw, ref)
    _apply_alias_doc(rec.type, ref, fld)
    value = read_field(rec.raw, fld, off)
    out = field_value_json(rec, block, fld, idx, off, value)
    out.update({
        "record": index,
        "record_type": rec.type,
        "record_name": rec.name,
        "requested": parse_ref(ref)[0],
        "canonical_field": fld.name,
        "field": parse_ref(ref)[0],
        "file_offset": rec.offset + 1 + off,
        "payload_offset": off,
    })
    return out


def set_field(src_path, src_data, sbt, index, assignment, out_path) -> dict:
    if "=" not in assignment:
        raise EditError("--set expects FIELD=VALUE, got %r" % assignment)
    ref, _, text = assignment.partition("=")
    row = resolve_record(sbt, index)
    rec = row.rec
    block, fld, idx, off = resolve_field(rec.type, rec.raw, ref)
    _apply_alias_doc(rec.type, ref, fld)
    old_value = read_field(rec.raw, fld, off)
    packed = pack_value(fld, text)
    new_raw = rec.raw[:off] + packed + rec.raw[off + len(packed):]
    try:
        sbtmod.parse_payload(rec.type, new_raw)
    except (ValueError, struct.error) as exc:
        raise EditError("patched %s payload is invalid: %s" % (rec.name, exc))
    edited = copy.deepcopy(sbt)
    target = edited.records[row.slot]
    target.raw = new_raw
    target.payload = sbtmod.parse_payload(rec.type, new_raw)
    target.size = 1 + len(new_raw)
    refresh_offsets(edited)
    rep, back_sbt, _ = commit_edit(src_path, src_data, sbt, edited, row.slot,
                                   out_path)
    back = back_sbt.records[row.slot]
    if back.raw != new_raw:
        rep["problems"].append("written record bytes differ from the patched "
                               "bytes")
    check = get_field(back_sbt, index, ref)
    if check["value"] != read_field(new_raw, fld, off):
        rep["problems"].append("written field value is %r, expected %r"
                               % (check["value"], read_field(new_raw, fld, off)))
    rep.update({
        "mode": "set",
        "record": index,
        "record_type": rec.type,
        "record_name": rec.name,
        "field": parse_ref(ref)[0],
        "canonical_field": fld.name,
        "index_in_block": idx,
        "block": block.label,
        "kind": fld.kind,
        "old_value": old_value,
        "new_value": check["value"],
        "payload_offset": off,
        "file_offset": rec.offset + 1 + off,
        "field_bytes": packed.hex(),
        "doc": fld.prov,
        "ok": not rep["problems"],
    })
    return rep


# ---------------------------------------------------------------------------
# --replace-record


def replace_record(src_path, src_data, sbt, index, blob_path, out_path) -> dict:
    row = resolve_record(sbt, index)
    rec = row.rec
    if rec.type in SKIP_TYPES:
        raise EditError("%s is not a scene record" % rec.name)
    try:
        blob = read_bytes(blob_path)
    except EditError:
        raise
    if not blob:
        raise EditError("%s is empty" % blob_path)
    edited = replace_record_bytes(sbt, row.slot, blob)
    rep, back_sbt, _ = commit_edit(src_path, src_data, sbt, edited, row.slot,
                                   out_path)
    back = back_sbt.records[row.slot]
    if back.raw != blob:
        rep["problems"].append("written record payload differs from %s"
                               % blob_path)
    try:
        blocks = blocks_of(rec.type, back.raw)
        summary = [{"label": b.label, "count": b.count} for b in blocks]
    except EditError as exc:
        summary = []
        rep["problems"].append("replacement does not decode: %s" % exc)
    rep.update({
        "mode": "replace-record",
        "record": index,
        "record_type": rec.type,
        "record_name": rec.name,
        "blob": blob_path,
        "blob_bytes": len(blob),
        "old_payload_bytes": len(rec.raw),
        "payload_delta": len(blob) - len(rec.raw),
        "blocks": summary,
        "ok": not rep["problems"],
    })
    return rep


# ---------------------------------------------------------------------------
# --check


def check_file(path, against=None, game=DEFAULT_GAME, record=None,
               expect_field=None, expect_file=None) -> dict:
    data = read_bytes(path)
    try:
        sbt = sbtmod.parse(data)
    except (ValueError, struct.error) as exc:
        raise EditError("%s: %s" % (path, exc))
    problems = container_problems(sbt, data)
    rep = {
        "tool": "sceneedit",
        "mode": "check",
        "file": path,
        "sha256": sha256(data),
        "bytes": len(data),
        "records": len(sbt.records),
        "scene_records": len(scene_records(sbt)),
        "warnings": sbtio.encoding_drift(sbt),
    }
    if against is not None:
        src_path, src_data, src_sbt = load_level(against, game)
        rep["against"] = src_path
        target_slots = ()
        if record is not None:
            row = resolve_record(src_sbt, record)
            target_slots = (row.slot,)
            if row.slot >= len(sbt.records):
                problems.append("record %d is missing in the checked file"
                                % record)
            else:
                dst = sbt.records[row.slot]
                if dst.type != row.rec.type:
                    problems.append("record %d type is %d, expected %d"
                                    % (record, dst.type, row.rec.type))
                if row.rec.type == SCRIPT_TYPE:
                    problems.append("record %d is SCRIPT; compare it with "
                                    "tools/editor/sbtwrite.py --check" % record)
                elif expect_file:
                    want = read_bytes(expect_file)
                    if dst.raw != want:
                        problems.append("record %d payload differs from %s "
                                        "(%d vs %d bytes)"
                                        % (record, expect_file, len(dst.raw),
                                           len(want)))
                elif expect_field:
                    if "=" not in expect_field:
                        raise EditError("--check --field needs NAME=VALUE (the "
                                        "value the target record must hold); "
                                        "got %r" % expect_field)
                    ref, _, text = expect_field.partition("=")
                    got = get_field(sbt, record, ref)
                    _b, fld, _i, _o = resolve_field(
                        sbt.records[row.slot].type, sbt.records[row.slot].raw,
                        ref)
                    want = read_field(pack_value(fld, text), fld, 0)
                    rep["expect"] = {"field": ref, "expected": want,
                                     "actual": got["value"]}
                    if got["value"] != want:
                        problems.append("record %d field %s is %r, expected %r"
                                        % (record, ref, got["value"], want))
        other_diffs = compare_records(src_sbt, sbt, target_slots)
        rep["other_records_identical"] = not other_diffs
        problems.extend(other_diffs)
        if src_sbt.header != sbt.header:
            problems.append("84-byte header differs from %s" % src_path)
    rep["problems"] = problems
    rep["ok"] = not problems
    return rep


# ---------------------------------------------------------------------------
# --roundtrip


def roundtrip_corpus(game=DEFAULT_GAME, out_dir=None, verbose=False) -> dict:
    out_dir = out_dir or os.path.join(DEFAULT_OUT_DIR, "roundtrip")
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        raise EditError("no .sbt files in %s (use --game DIR)" % game)
    started = time.time()
    rows = []
    identical = mismatch = errors = 0
    total = 0
    for path in files:
        row = {"file": os.path.basename(path)}
        try:
            data = read_bytes(path)
            sbt = sbtmod.parse(data)
            out = sbt.to_bytes()
            out_path = os.path.join(out_dir, os.path.basename(path))
            write_bytes(out_path, out)
            back = read_bytes(out_path)
            problems = container_problems(sbt, data)
            walked = walk_container(data)
            scene = len(scene_records(sbt))
            row.update({
                "bytes": len(data),
                "sha256_in": sha256(data),
                "sha256_out": sha256(back),
                "written": out_path,
                "records": len(sbt.records),
                "scene_records": scene,
                "walked_records": len(walked),
                "problems": problems,
                "encoding_drift": len(sbtio.encoding_drift(sbt)),
            })
            if back == data and out == data:
                identical += 1
                row["identical"] = True
            else:
                mismatch += 1
                row["identical"] = False
                row["first_diff"] = next(
                    (i for i in range(min(len(back), len(data)))
                     if back[i] != data[i]), None)
            row["ok"] = not problems and row["identical"]
            total += len(data)
        except EditError as exc:
            errors += 1
            row.update({"ok": False, "error": str(exc)})
        rows.append(row)
        if verbose:
            print("  %-16s %8d bytes  %s"
                  % (row["file"], row.get("bytes", 0),
                     "identical" if row.get("identical") else
                     ("ERROR" if "error" in row else "MISMATCH")))
    rep = {
        "tool": "sceneedit", "mode": "roundtrip", "game": game,
        "out_dir": out_dir, "files": len(files), "bytes": total,
        "identical": identical, "mismatch": mismatch, "errors": errors,
        "structurally_ok": sum(1 for r in rows if not r.get("problems")),
        "scene_records": sum(r.get("scene_records", 0) for r in rows),
        "encoding_drift": sum(r.get("encoding_drift", 0) for r in rows),
        "seconds": round(time.time() - started, 2),
        "ok": identical == len(files) and errors == 0,
        "results": rows,
    }
    return rep


def print_roundtrip(rep) -> None:
    print("roundtrip: %d files, %d bytes, %.2fs"
          % (rep["files"], rep["bytes"], rep["seconds"]))
    print("  identical (read -> write -> compare): %d/%d, %d mismatch, "
          "%d error" % (rep["identical"], rep["files"], rep["mismatch"],
                        rep["errors"]))
    print("  structural checks (oracle + own walker): %d/%d"
          % (rep["structurally_ok"], rep["files"]))
    print("  scene records: %d, structural-encoder drift: %d record(s)"
          % (rep["scene_records"], rep["encoding_drift"]))


# ---------------------------------------------------------------------------
# synthetic self-test (no game files)


def _synth_sbt(extras) -> bytes:
    header = (MAGIC + b"\0" * (80 - len(MAGIC)) + struct.pack("<I", 0))
    parts = [header]
    for type_, payload in extras:
        parts.append(bytes((type_,)) + payload)
    parts.append(bytes((END,)))
    return b"".join(parts)


def _synth_payloads():
    sob = (struct.pack("<I", 2)
           + b"crate".ljust(12, b"\0") + struct.pack("<II", 7, 0xDEADBEEF)
           + b"mine".ljust(12, b"\0") + struct.pack("<II", 3, 0))
    named = (struct.pack("<I", 1) + b"SMSW803".ljust(16, b"\0")
             + struct.pack("<9I", 0, 10, 0xFFFFFFFF, 0, 41, 803, 0, 128, 0))
    handles = (struct.pack("<I", 2)
               + struct.pack("<2I", 11, 22) + struct.pack("<2I", 1, 2)
               + struct.pack("<2I", 3, 4) + struct.pack("<2I", 5, 6))
    thing = (struct.pack("<I", 1) + b"bgchunk1".ljust(16, b"\0")
             + struct.pack("<49d", *([0.01] * 49))
             + b"\0" * (936 - 16 - 49 * 8))
    thing = bytearray(thing)
    # tail[3] (+0x1A4) = model index 5
    struct.pack_into("<I", thing, 4 + 0x1A4, 5)
    item = bytearray(0x7080)
    for slot in range(300):
        struct.pack_into("<i", item, slot * 0x60 + 0x38, -2)
    struct.pack_into("<i", item, 0 * 0x60 + 0x38, 0)         # active slot 0
    struct.pack_into("<i", item, 0 * 0x60 + 0x40, 41)        # sob_index
    lift_entry = bytearray(0xA8)
    struct.pack_into("<6d", lift_entry, 0x00, *([1.0] * 6))
    struct.pack_into("<i", lift_entry, 0x30, 9)      # sob_index
    struct.pack_into("<i", lift_entry, 0x34, 1)      # kind
    struct.pack_into("<i", lift_entry, 0x3C, -400)   # x
    struct.pack_into("<i", lift_entry, 0x54, 8)      # speed
    struct.pack_into("<i", lift_entry, 0x5C, 0)      # height
    struct.pack_into("<i", lift_entry, 0x60, 120)    # target
    struct.pack_into("<i", lift_entry, 0x9C, 3)      # stop_count
    struct.pack_into("<i", lift_entry, 0xA0, 0)      # stop_start
    lift = (struct.pack("<I", 2) + bytes(lift_entry) * 2
            + struct.pack("<I", 3)
            + struct.pack("<3I", 1, 2, 3)
            + struct.pack("<3I", 10, 20, 30)
            + struct.pack("<3I", 100, 200, 300))
    sector = b"\0" * 0x5800
    counts = struct.pack("<300I", *[0] * 300)
    names = bytearray(0xA8C)
    names[0:9] = b"AMSPACE\0\0"
    names[9 * 130:9 * 131] = b"AMCOMPUT\0"
    sample = (counts + bytes(names))
    sound = (struct.pack("<I", 2)
             + struct.pack("<2i", 1, 2) + struct.pack("<2i", 3, 4)
             + struct.pack("<2i", 5, 6) + struct.pack("<2i", 7, 8)
             + struct.pack("<2i", 9, 10) + struct.pack("<2d", 0.03, 0.05))
    switch = (struct.pack("<I", 2)
              + struct.pack("<8i", 0, 1, 100, 10, 20, 30, 0, -1)
              + struct.pack("<8i", 0x101, 2, 50, 40, 50, 60, 1, 3))
    sprites = (struct.pack("<I", 1) + struct.pack("<2f", 1.5, 2.5)
               + struct.pack("<I", 0) + struct.pack("<3i", 7, 8, 9)
               + bytes((4, 0, 0, 0)) + struct.pack("<I", 2)
               + struct.pack("<i", 44) + struct.pack("<H", 9)
               + b"\0\0" + struct.pack("<I", 1234))
    thing3 = bytes((1,)) + b"\0" * 0x528
    player = bytes((0,))
    door = (struct.pack("<I", 1) + struct.pack("<9d", *range(9))
            + struct.pack("<3d", 0.5, 0.5, 0.5) + struct.pack("<d", 0.03)
            + struct.pack("<i", 1) + struct.pack("<5i", 1, 2, 3, 4, 5)
            + struct.pack("<i", 0) + struct.pack("<2i", 4, 5)
            + struct.pack("<2I", 0, 30) + struct.pack("<i", 10)
            + struct.pack("<2i", 0, 0) + struct.pack("<3i", 1, 2, 3)
            + struct.pack("<3i", 4, 5, 6))
    script = (struct.pack("<I", 0) + struct.pack("<I", 2)
              + struct.pack("<2I", 1, 1) + struct.pack("<2I", 0, 0)
              + struct.pack("<2I", 0, 2) + struct.pack("<I", 3)
              + b"\x01\xff\xff" + b"\0")
    return [
        (1, switch), (2, door), (3, thing3), (4, bytes(thing)),
        (5, named), (6, sprites), (7, sound), (8, sector), (9, player),
        (11, script), (12, sample), (15, handles),
        (18, lift), (19, bytes(item)), (20, sob),
    ]


def _selftest() -> bool:
    results = []

    def case(name, fn):
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except AssertionError as exc:
            results.append((name, False, "assertion: %s" % exc))
        except EditError as exc:
            results.append((name, False, "EditError: %s" % exc))
        except Exception as exc:                       # noqa: BLE001
            results.append((name, False, "%s: %s" % (type(exc).__name__, exc)))

    def expect_error(fn, needle, label):
        try:
            fn()
        except EditError as exc:
            if needle not in str(exc):
                raise AssertionError("%s: message %r lacks %r"
                                     % (label, str(exc), needle))
            return str(exc)
        raise AssertionError("%s: no EditError raised" % label)

    base = _synth_sbt(_synth_payloads())
    sbt = sbtmod.parse(base)
    tmp = os.path.join(DEFAULT_OUT_DIR, "selftest")
    src = os.path.join(tmp, "base.sbt")
    write_bytes(src, base)

    def t_walk():
        walked = walk_container(base)
        assert len(walked) == len(sbt.records), (len(walked), len(sbt.records))
        for w, r in zip(walked, sbt.records):
            assert w["type"] == r.type, (w["type"], r.type)
            assert w["payload"] == r.raw, r.name
            assert w["size"] == r.size, (w["size"], r.size)
        assert sbt.to_bytes() == base, "oracle round-trip differs"
        return "%d records, %d bytes, own walker agrees" % (len(walked), len(base))

    def t_scene_index():
        rows = scene_records(sbt)
        assert len(rows) == len(sbt.records) - 2, len(rows)   # SCRIPT + END
        assert [r.rec.type for r in rows] == [1, 2, 3, 4, 5, 6, 7, 8, 9,
                                              12, 15, 18, 19, 20], \
            [r.rec.type for r in rows]
        return "%d scene records, SCRIPT/END skipped" % len(rows)

    # scene indices in the synthetic container (types 1..9, 12, 15, 18, 19, 20):
    # 0 SWITCH  1 DOOR  2 THING  3 THING_TYPES  4 NAMED_ITEMS  5 SPRITES
    # 6 SOUND_SOURCES  7 SECTOR_STUFF  8 PLAYER_STATUS  9 SAMPLE_TABLE
    # 10 POLY_HANDLE  11 LIFT  12 ITEM  13 SOB_TABLE
    ITEM, SOB, LIFT, SAMPLE, SOUND, SPRITES = 12, 13, 11, 9, 6, 5

    def t_get():
        assert get_field(sbt, ITEM, "state[0]")["value"] == 0
        assert get_field(sbt, ITEM, "state[299]")["value"] == -2
        assert get_field(sbt, 3, "speed[0]")["value"] == 0.01
        assert get_field(sbt, 3, "model[0]")["value"] == 5
        assert get_field(sbt, ITEM, "sob_index[0]")["value"] == 41
        return "ITEM state/sob_index, THING_TYPES speed/model"

    def t_alias_and_kinds():
        assert get_field(sbt, 3, "param.0[0]")["value"] == 0.01
        assert get_field(sbt, 3, "tail.3[0]")["value"] == 5
        assert get_field(sbt, 0, "sound[1]")["value"] == 3
        assert get_field(sbt, SAMPLE, "count[130]")["value"] == 0
        assert get_field(sbt, SAMPLE, "name[130]")["value"] == "AMCOMPUT"
        assert get_field(sbt, SAMPLE, "count[0]")["value"] == 0
        assert get_field(sbt, SOUND, "atten[1]")["value"] == 0.05
        assert abs(get_field(sbt, SPRITES, "scale_x[0]")["value"] - 1.5) < 1e-6
        return "aliases, columns (7/12/15), f32, f64"

    def t_set_u32():
        out = os.path.join(tmp, "set_u32.sbt")
        rep = set_field(src, base, sbt, ITEM, "sob_index[0]=77", out)
        assert rep["ok"], rep["problems"]
        assert rep["old_value"] == 41 and rep["new_value"] == 77, rep
        assert rep["others_identical"], rep["problems"]
        back = sbtmod.parse(read_bytes(out))
        assert back.records_of(19)[0].raw[0x40:0x44] == struct.pack("<i", 77)
        return "ITEM sob_index 41 -> 77, others identical, bytes at +0x40"

    def t_set_f64():
        out = os.path.join(tmp, "set_f64.sbt")
        rep = set_field(src, base, sbt, 3, "speed[0]=0.02", out)
        assert rep["ok"], rep["problems"]
        back = sbtmod.parse(read_bytes(out))
        assert struct.unpack_from("<d", back.records_of(4)[0].raw, 4 + 0x10)[0] == 0.02
        return "THING_TYPES speed 0.01 -> 0.02 at +0x010"

    def t_set_u8_no_growth():
        out = os.path.join(tmp, "set_u8.sbt")
        rep = set_field(src, base, sbt, ITEM, "flags[2]=255", out)
        assert rep["ok"], rep["problems"]
        assert rep["size_delta"] == 0, rep["size_delta"]
        return "ITEM flags[2] = 255 (u8), file size unchanged"

    def t_set_name():
        out = os.path.join(tmp, "set_name.sbt")
        rep = set_field(src, base, sbt, SOB, "name[0]=ABCDEF", out)
        assert rep["ok"], rep["problems"]
        back = sbtmod.parse(read_bytes(out))
        assert back.records_of(20)[0].raw[4:20].split(b"\0")[0] == b"ABCDEF"
        return "SOB_TABLE name[0] = 'ABCDEF'"

    def t_set_hex():
        out = os.path.join(tmp, "set_hex.sbt")
        rep = set_field(src, base, sbt, 0, "kind[1]=0x102", out)
        assert rep["ok"], rep["problems"]
        assert rep["new_value"] == 0x102, rep["new_value"]
        return "SWITCH kind[1] = 0x102 (hex input)"

    def t_set_counts():
        out = os.path.join(tmp, "set_counts.sbt")
        rep = set_field(src, base, sbt, SAMPLE, "count[130]=9", out)
        assert rep["ok"], rep["problems"]
        return "SOUND_SAMPLE_TABLE count[130] = 9"

    def t_set_lift_stop():
        out = os.path.join(tmp, "set_lift.sbt")
        rep = set_field(src, base, sbt, LIFT, "stops.height[1]=65535", out)
        assert rep["ok"], rep["problems"]
        return "LIFT stop height[1] = 65535"

    def t_set_sound_source():
        out = os.path.join(tmp, "set_sound.sbt")
        rep = set_field(src, base, sbt, SOUND, "atten[0]=0.07", out)
        assert rep["ok"], rep["problems"]
        return "SOUND_SOURCES atten[0] = 0.07 (f64 column)"

    def t_bad_index():
        return expect_error(lambda: get_field(sbt, ITEM, "state[300]"),
                            "out of range", "state[300]")
    def t_missing_index():
        return expect_error(lambda: get_field(sbt, ITEM, "state"),
                            "has 300 elements", "state without index")
    def t_unknown_field():
        return expect_error(lambda: get_field(sbt, ITEM, "nope"), "no field",
                            "unknown field")
    def t_bad_record():
        return expect_error(lambda: get_field(sbt, 99, "state[0]"),
                            "does not exist", "record 99")
    def t_script_refused():
        return expect_error(lambda: blocks_of(11, sbt.records[9].raw),
                            "sbtwrite.py", "SCRIPT")
    def t_end_refused():
        return expect_error(lambda: blocks_of(END, b""), "terminator", "END")
    def t_u8_overflow():
        return expect_error(
            lambda: set_field(src, base, sbt, ITEM, "flags[0]=256",
                              os.path.join(tmp, "bad.sbt")),
            "outside u8 range", "u8 overflow")
    def t_name_too_long():
        return expect_error(
            lambda: set_field(src, base, sbt, SOB, "name[0]=ABCDEFGHIJKLMNOPQ",
                              os.path.join(tmp, "bad2.sbt")),
            "at most 11 bytes", "name overflow")
    def t_nan():
        return expect_error(
            lambda: set_field(src, base, sbt, 3, "speed[0]=nan",
                              os.path.join(tmp, "bad3.sbt")),
            "not finite", "nan")
    def t_bad_value():
        return expect_error(
            lambda: set_field(src, base, sbt, ITEM, "sob_index[0]=x",
                              os.path.join(tmp, "bad4.sbt")),
            "not an integer", "bad integer")

    def t_replace_same_size():
        out = os.path.join(tmp, "repl_same.sbt")
        blob = bytearray(sbt.records_of(20)[0].raw)       # SOB_TABLE
        blob[4:16] = b"blast".ljust(12, b"\0")
        path = os.path.join(tmp, "sob.blob")
        write_bytes(path, bytes(blob))
        rep = replace_record(src, base, sbt, SOB, path, out)
        assert rep["ok"], rep["problems"]
        assert rep["size_delta"] == 0, rep["size_delta"]
        assert rep["others_identical"], rep["problems"]
        back = sbtmod.parse(read_bytes(out))
        assert back.records_of(20)[0].raw == bytes(blob)
        return "SOB_TABLE replaced (same size), others identical"

    def t_replace_grow():
        out = os.path.join(tmp, "repl_grow.sbt")
        blob = (struct.pack("<I", 3)
                + b"a".ljust(12, b"\0") + struct.pack("<II", 1, 0)
                + b"bb".ljust(12, b"\0") + struct.pack("<II", 2, 0)
                + b"ccc".ljust(12, b"\0") + struct.pack("<II", 3, 0))
        path = os.path.join(tmp, "sob3.blob")
        write_bytes(path, blob)
        rep = replace_record(src, base, sbt, SOB, path, out)
        assert rep["ok"], rep["problems"]
        assert rep["payload_delta"] == 20, rep["payload_delta"]
        assert rep["bytes_out"] == len(base) + 20, rep["bytes_out"]
        assert rep["others_identical"], rep["problems"]
        back = sbtmod.parse(read_bytes(out))
        assert back.records_of(20)[0].raw == blob
        off = 84
        for r in back.records:                    # offsets re-derived
            assert r.offset == off, (r.offset, off)
            off += r.size
        assert off == len(read_bytes(out))
        return "+20 bytes, record offsets re-derived, others identical"

    def t_replace_bad_blob():
        path = os.path.join(tmp, "bad.blob")
        write_bytes(path, struct.pack("<I", 0xC9) + b"\0" * (0xC9 * 20))
        return expect_error(
            lambda: replace_record(src, base, sbt, SOB, path,
                                   os.path.join(tmp, "bad5.sbt")),
            "not a valid payload", "SOB_TABLE count 0xC9")

    def t_replace_script():
        # ``--record`` indexes scene records only, so SCRIPT can never be a
        # target: index 14 of 14 records does not exist.
        path = os.path.join(tmp, "script.blob")
        write_bytes(path, b"\0" * 13)
        expect_error(lambda: resolve_record(sbt, 14), "does not exist",
                     "record 14")
        assert SCRIPT_TYPE not in [r.rec.type for r in scene_records(sbt)]
        return "scene index excludes SCRIPT; index 14 rejected"

    def t_check_ok():
        rep = check_file(src)
        assert rep["ok"], rep["problems"]
        assert rep["records"] == len(sbt.records)
        return "own+oracle walk, %d records" % rep["records"]

    def t_check_against():
        out = os.path.join(tmp, "chk.sbt")
        rep = set_field(src, base, sbt, ITEM, "sob_index[0]=77", out)
        assert rep["ok"], rep["problems"]
        good = check_file(out, against=src, record=ITEM,
                          expect_field="sob_index[0]=77")
        assert good["ok"], good["problems"]
        assert good["other_records_identical"]
        bad = check_file(out, against=src, record=ITEM,
                         expect_field="sob_index[0]=78")
        assert not bad["ok"], "check accepted a wrong expected value"
        return "against+expect-field accepted 77, rejected 78"

    def t_check_detects_tamper():
        out = os.path.join(tmp, "chk2.sbt")
        rep = set_field(src, base, sbt, ITEM, "sob_index[0]=77", out)
        assert rep["ok"], rep["problems"]
        data = bytearray(read_bytes(out))
        # flip one byte inside the SOB_TABLE name ("crate" -> "cRatE"), which
        # keeps the container parseable but must be reported as a difference
        sob_off = sbt.records_of(20)[0].offset + 1 + 4
        data[sob_off + 2] ^= 0x20
        bad_path = os.path.join(tmp, "chk2bad.sbt")
        write_bytes(bad_path, bytes(data))
        bad = check_file(bad_path, against=src, record=ITEM,
                         expect_field="sob_index[0]=77")
        assert not bad["ok"], "check missed a change in another record"
        assert not bad["other_records_identical"]
        return "detects a flipped byte in another record"

    def t_header_kept():
        out = os.path.join(tmp, "hdr.sbt")
        rep = set_field(src, base, sbt, ITEM, "sob_index[0]=1", out)
        assert rep["ok"], rep["problems"]
        back = read_bytes(out)
        assert back[:84] == base[:84], "header changed"
        return "84-byte header preserved"

    def t_limits():
        return expect_error(
            lambda: blocks_of(20, struct.pack("<I", 0xC9) + b"\0" * (0xC9 * 20)),
            "exceeds", "SOB_TABLE count 0xC9")

    def t_walk_bad():
        bad = base + b"\0"                    # trailing byte
        return expect_error(lambda: walk_container(bad), "trailing",
                            "trailing byte")
    def t_walk_no_eof():
        bad = base[:-1]                       # drop END_OF_FILE
        return expect_error(lambda: walk_container(bad), "END_OF_FILE",
                            "missing EOF")
    def t_walk_bad_magic():
        bad = b"X" + base[1:]
        return expect_error(lambda: walk_container(bad), "magic", "bad magic")
    def t_missing_file():
        return expect_error(lambda: check_file(os.path.join(tmp, "nope.sbt")),
                            "cannot read", "missing file")

    def t_roundtrip_synthetic():
        out = os.path.join(tmp, "rt.sbt")
        write_bytes(out, sbt.to_bytes())
        assert read_bytes(out) == base
        return "synthetic container re-serialises byte-identically"

    def t_exists_is_sized():
        # THING/PLAYER_STATUS payload length depends on the flag: flipping
        # exists 0 -> 1 without the 0x528/0x3510 body must be refused, and a
        # 1-byte payload is a valid replacement for exists=0.
        sob1 = (struct.pack("<I", 1) + b"x".ljust(12, b"\0")
                + struct.pack("<II", 0, 0))
        tiny = _synth_sbt([(3, b"\0"), (9, b"\0"), (20, sob1)])
        tiny_sbt = sbtmod.parse(tiny)
        tiny_src = os.path.join(tmp, "tiny.sbt")
        write_bytes(tiny_src, tiny)
        assert get_field(tiny_sbt, 0, "exists")["value"] == 0
        expect_error(
            lambda: set_field(tiny_src, tiny, tiny_sbt, 0, "exists=1",
                              os.path.join(tmp, "tiny_bad.sbt")),
            "patched THING payload is invalid", "exists=1 without body")
        out = os.path.join(tmp, "tiny_ok.sbt")
        rep = set_field(tiny_src, tiny, tiny_sbt, 2, "amount[0]=5", out)
        assert rep["ok"], rep["problems"]
        blob = os.path.join(tmp, "thing0.bin")
        write_bytes(blob, b"\x01" + b"\0" * 0x528)
        rep2 = replace_record(tiny_src, tiny, tiny_sbt, 0, blob,
                              os.path.join(tmp, "tiny_repl.sbt"))
        assert rep2["ok"], rep2["problems"]
        assert rep2["payload_delta"] == 0x528, rep2["payload_delta"]
        back = sbtmod.parse(read_bytes(rep2["output"]))
        assert back.records_of(3)[0].payload.exists
        return "exists=1 without body refused; THING 1 -> 0x529 bytes ok"

    def t_json_ready():
        rep = list_level(src, sbt, limit=2)
        json.dumps(rep)
        assert rep["scene_records"] == len(scene_records(sbt))
        assert rep["bytes"] == len(base), (rep["bytes"], len(base))
        g = get_field(sbt, ITEM, "state[0]")
        json.dumps(g)
        return "list/get JSON-serialisable"

    tests = [
        ("own walker == oracle", t_walk),
        ("scene record indexing", t_scene_index),
        ("get documented fields", t_get),
        ("aliases/columns/kinds", t_alias_and_kinds),
        ("set u32 (ITEM sob_index)", t_set_u32),
        ("set f64 (THING_TYPES speed)", t_set_f64),
        ("set u8 keeps size", t_set_u8_no_growth),
        ("set name slot", t_set_name),
        ("set hex value", t_set_hex),
        ("set sample count", t_set_counts),
        ("set LIFT stop", t_set_lift_stop),
        ("set sound column", t_set_sound_source),
        ("error: entry index OOB", t_bad_index),
        ("error: index required", t_missing_index),
        ("error: unknown field", t_unknown_field),
        ("error: unknown record", t_bad_record),
        ("error: SCRIPT refused", t_script_refused),
        ("error: END refused", t_end_refused),
        ("error: u8 overflow", t_u8_overflow),
        ("error: name too long", t_name_too_long),
        ("error: non-finite float", t_nan),
        ("error: bad integer", t_bad_value),
        ("replace-record same size", t_replace_same_size),
        ("replace-record larger", t_replace_grow),
        ("error: bad replacement blob", t_replace_bad_blob),
        ("error: SCRIPT replace refused", t_replace_script),
        ("check ok", t_check_ok),
        ("check --against/--field", t_check_against),
        ("check detects tampering", t_check_detects_tamper),
        ("header preserved", t_header_kept),
        ("error: loader count limit", t_limits),
        ("error: trailing bytes", t_walk_bad),
        ("error: missing EOF", t_walk_no_eof),
        ("error: bad magic", t_walk_bad_magic),
        ("error: missing file", t_missing_file),
        ("synthetic byte round-trip", t_roundtrip_synthetic),
        ("exists flag sizes the payload", t_exists_is_sized),
        ("JSON output", t_json_ready),
    ]
    for name, fn in tests:
        case(name, fn)
    failed = [r for r in results if not r[1]]
    width = max(len(r[0]) for r in results)
    for name, ok, detail in results:
        print("  %-*s  %s%s" % (width, name, "ok" if ok else "FAIL",
                                ("  " + detail) if detail else ""))
    print("self-test: %d/%d passed" % (len(results) - len(failed), len(results)))
    return not failed


# ---------------------------------------------------------------------------
# --fields (the whole map, with provenance)


def print_fields(as_json=False) -> None:
    rows = []
    for type_ in sorted(SCHEMA):
        spec = SCHEMA[type_]
        row = {"type": type_, "name": TYPE_NAMES[type_], "doc": spec["doc"],
               "entry_size": spec.get("entry_size"),
               "count": spec.get("fixed_count") or spec.get("count_prefix"),
               "fields": [{"name": n, "offset": off, "kind": k,
                           "doc": prov}
                          for n, off, k, prov in spec["fields"]]}
        if spec.get("aliases"):
            row["aliases"] = {a: {"field": v[0], "doc": v[1]}
                              for a, v in spec["aliases"].items()}
        if type_ == 18:
            row["stop_columns"] = [{"name": n, "doc": d}
                                   for n, d in spec["stop_columns"]]
        if type_ == 7:
            row["fields"] = []
            row["note"] = "columns: x,y,z,volume,sample (i32), atten (f64)"
        if type_ == 12:
            row["fields"] = []
            row["note"] = "counts[300] u32 at 0x000; names[300] 9 bytes at 0x4B0"
        rows.append(row)
    if as_json:
        print(json.dumps(rows, indent=1))
        return
    for row in rows:
        print("type %-3d %-19s %s" % (row["type"], row["name"], row["doc"]))
        for f in row["fields"]:
            print("  +0x%03x %-10s %-18s %s"
                  % (f["offset"], f["kind"], f["name"], f["doc"]))
        for alias, info in row.get("aliases", {}).items():
            print("  alias %-14s = %-16s %s"
                  % (alias, info["field"], info["doc"]))
        for col in row.get("stop_columns", []):
            print("  column %-11s %s" % (col["name"], col["doc"]))
        if row.get("note"):
            print("  %s" % row["note"])


# ---------------------------------------------------------------------------
# CLI


def _cmd_list(args) -> int:
    path, data, sbt = load_level(args.list, args.game)
    rep = list_level(path, sbt, limit=args.limit)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_list(rep)
    return 0


def _cmd_get(args) -> int:
    path, data, sbt = load_level(args.get, args.game)
    if args.record is None:
        raise EditError("--get needs --record N (see --list)")
    if not args.field:
        raise EditError("--get needs --field NAME (see --fields)")
    rep = get_field(sbt, args.record, args.field)
    rep.update({"mode": "get", "file": path, "ok": True})
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print("%s record #%d %s: %s[%d] = %s (%s, payload +0x%x, file 0x%x)"
              % (path, rep["record"], rep["record_name"], rep["field"],
                 rep["index"], _fmt_value(rep["value"]), rep["kind"],
                 rep["payload_offset"], rep["file_offset"]))
        print("  doc: %s" % rep["doc"])
    return 0


def _cmd_set(args) -> int:
    if args.out is None:
        raise EditError("--set writes a file: pass --out FILE (the game "
                        "installation is read-only)")
    path, data, sbt = load_level(args.set, args.game)
    if args.record is None:
        raise EditError("--set needs --record N (see --list)")
    rep = set_field(path, data, sbt, args.record, args.field or "", args.out)
    rc = 0 if rep["ok"] else 1
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print("set: record #%d %s.%s[%d] %s -> %s  (payload +0x%x, %s)"
              % (rep["record"], rep["record_name"], rep["field"],
                 rep["index_in_block"], _fmt_value(rep["old_value"]),
                 _fmt_value(rep["new_value"]), rep["payload_offset"],
                 rep["kind"]))
        print("  %s -> %s (%+d bytes), sha256 %s..%s"
              % (rep["input"], rep["output"], rep["size_delta"],
                 rep["sha256_in"][:12], rep["sha256_out"][:12]))
        print("  other records identical: %s, %d records"
              % ("yes" if rep["others_identical"] else "NO", rep["records"]))
        if rep["problems"]:
            for p in rep["problems"]:
                print("  - %s" % p)
    return rc


def _cmd_replace(args) -> int:
    if args.out is None:
        raise EditError("--replace-record writes a file: pass --out FILE")
    path, data, sbt = load_level(args.replace_record, args.game)
    if args.record is None:
        raise EditError("--replace-record needs --record N (see --list)")
    if not args.file:
        raise EditError("--replace-record needs --file BLOB (raw payload, no "
                        "type byte)")
    rep = replace_record(path, data, sbt, args.record, args.file, args.out)
    rc = 0 if rep["ok"] else 1
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print("replace: record #%d %s  %d -> %d payload bytes (%+d), file "
              "%+d bytes" % (rep["record"], rep["record_name"],
                             rep["old_payload_bytes"], rep["blob_bytes"],
                             rep["payload_delta"], rep["size_delta"]))
        print("  %s -> %s" % (rep["input"], rep["output"]))
        print("  blocks: %s" % ", ".join("%s[%d]" % (b["label"], b["count"])
                                         for b in rep["blocks"]))
        print("  other records identical: %s"
              % ("yes" if rep["others_identical"] else "NO"))
        if rep["problems"]:
            for p in rep["problems"]:
                print("  - %s" % p)
    return rc


def _cmd_check(args) -> int:
    rep = check_file(args.check, against=args.against, game=args.game,
                     record=args.record, expect_field=args.field,
                     expect_file=args.expect_file)
    rc = 0 if rep["ok"] else 1
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        if rep["ok"]:
            print("check: %s ok (%d records, %d scene, %d bytes, sha256 %s)"
                  % (rep["file"], rep["records"], rep["scene_records"],
                     rep["bytes"], rep["sha256"][:16]))
            if "against" in rep:
                print("  against %s: other records identical: %s"
                      % (rep["against"],
                         "yes" if rep["other_records_identical"] else "NO"))
            if "expect" in rep:
                print("  expected %s = %r, actual %r"
                      % (rep["expect"]["field"], rep["expect"]["expected"],
                         rep["expect"]["actual"]))
        else:
            print("check: %s FAILED" % rep["file"])
            for p in rep["problems"]:
                print("  - %s" % p)
        for w in rep.get("warnings", []):
            print("  warning: %s" % w)
    return rc


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
        elif head in ("check", "--check") and len(argv) > 1:
            argv = ["--check", argv[1]] + argv[2:]
        elif head in ("list", "--list") and len(argv) > 1:
            argv = ["--list", argv[1]] + argv[2:]
    ap = argparse.ArgumentParser(
        description="Edit scene records (everything but SCRIPT) of a "
                    "Gunmetal .sbt level (M11.3).")
    ap.add_argument("--list", metavar="LEVEL",
                    help="list records and documented fields")
    ap.add_argument("--get", metavar="LEVEL", help="read one field")
    ap.add_argument("--set", metavar="LEVEL", help="edit one field")
    ap.add_argument("--replace-record", metavar="LEVEL",
                    help="replace a whole record payload from --file")
    ap.add_argument("--check", metavar="FILE",
                    help="re-read FILE (oracle + own walker) and verify")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source level")
    ap.add_argument("--roundtrip", action="store_true",
                    help="parse -> write -> compare every shipped .sbt")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests (no game files)")
    ap.add_argument("--fields", action="store_true",
                    help="print the field map with provenance")
    ap.add_argument("--record", type=int, metavar="N",
                    help="scene record index (from --list); SCRIPT/END are "
                         "not addressable")
    ap.add_argument("--field", metavar="NAME[=VALUE]",
                    help="field reference; --set needs NAME=VALUE")
    ap.add_argument("--file", metavar="BLOB",
                    help="--replace-record: raw payload bytes (no type byte)")
    ap.add_argument("--expect-file", metavar="BLOB",
                    help="--check: the target record must equal this payload")
    ap.add_argument("--out", metavar="PATH", help="output .sbt")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=8,
                    help="--list: max elements shown per block (default 8)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if _selftest() else 1
    if args.fields:
        print_fields(args.json)
        return 0
    if args.roundtrip:
        rep = roundtrip_corpus(args.game, args.out, args.verbose)
        if args.json:
            print(json.dumps(rep, indent=1))
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
    if args.replace_record:
        return _cmd_replace(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except EditError as exc:
        print("sceneedit: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except (ValueError, struct.error, OSError) as exc:
        print("sceneedit: error: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except Exception as exc:                           # noqa: BLE001
        print("sceneedit: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        sys.exit(3)
