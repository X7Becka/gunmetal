#!/usr/bin/env python3
"""Rebuild a Gunmetal save (``SAVEGAME/saved<N>.sbt``) from parsed state.

A save is a **self-contained** container (no side files are needed to parse or
rebuild it)::

    0x000  char name[0x50]     save name; the display title is the first 12
                               bytes ("A SAVED GAME", "2", ...).  It is *not*
                               a magic number and the dword at +0x0c is not a
                               version (0x34 in saved0, 0 in saved1).
    0x050  u32  zero           (reserved; always 0 in the shipped saves)
    0x054  u8   level[0x50]    level name at +0 ("LEVEL0"), rest zero
    0x0a4  22 x u32            counters/timers, see docs/SAVE.md
    0x0fc  4 x char[16]        "K-9", "", "coargus", "none"
    0x13c  record stream       same record framing as a .sbt ("BINARY SAVE"),
                               with runtime records inserted after SCRIPT.

The stream is ``[u8 type][payload]`` and ends with ``0xff`` + EOF; there is no
trailing data, no alignment and no checksum.  Level records (12/20/15/16/17/
5/4/11) are byte-identical to the ``.sbt`` writer's; state records are the same
types but have save-specific layouts (things = 260 slots, player = 8 slots).

Every installed save is parsed into typed blocks and serialised again from
those blocks (counts and strides, not "copy the whole file"), then compared
byte for byte.  Payloads that are still opaque (raw runtime structs) are kept
verbatim and reported as such - the tool never silently substitutes bytes.

Usage::

    python3 tools/re/save_rebuild.py --check [SAVE ...]
    python3 tools/re/save_rebuild.py --out DIR [--force] [SAVE ...]
    python3 tools/re/save_rebuild.py --diff [SAVE ...]
    python3 tools/re/save_rebuild.py --self-test [SAVE ...]

With no ``SAVE`` argument the installed ``--game`` save directory is used.
Exit status: 0 = all rebuilt byte-exactly, 2 = mismatch, 1 = parse/IO error.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import sbt as sbtmod  # noqa: E402  (record codec shared with .sbt)

# --- container limits (see docs/SAVE.md, docs/SECURITY.md) ------------------
HEADER_SIZE = 0x13C
MAX_FILE = 64 << 20          # a shipped save is ~240 KiB
MAX_BLOCKS = 32
MAX_DOORS = 100              # gm.sbt DOOR loader: count <= 100
MAX_SWITCHES = 0x200         # gm.sbt SWITCH loader
MAX_SPRITES = 0x1F3          # gm.sbt SPRITES loader
MAX_SOUNDS = 0x7F            # gm.sbt SOUND_SOURCES loader
MAX_LIFTS = 0x1E
MAX_STOPS = 0xC8
NAME_SLOTS = 4
NAME_SLOT = 16

# state blocks, in the order the save driver (0x455a80) writes them
ORDER = (12, 20, 15, 16, 17, 5, 4, 11, 2, 18, 1, 6, 19, 8, 7, 3, 9)

# blocks a save cannot omit (level record stream + things + player)
REQUIRED = (12, 20, 15, 17, 5, 4, 11, 3, 9)

SAMPLE_COUNTS = 300
SAMPLE_NAME = 9

THING_SLOTS = 260
THING_RECORD = 0x528
PLAYER_SLOTS = 8
PLAYER_STRUCT = 0x3510
ATTACH_AT = 0x317C          # u32[16] ids of attached objects
ATTACH_MAX = 16
ATTACH_RECORD = 16
PLAYER_SUB_HEADER = 0x50

#: short, stable names used by ``--blocks`` and docs/SAVE.md
BLOCK_NAMES = {
    1: "switches",
    2: "doors",
    3: "things",
    4: "thing_types",
    5: "named_items",
    6: "sprites",
    7: "sound_sources",
    8: "sector_stuff",
    9: "player",
    11: "script",
    12: "sound_table",
    15: "poly_handles",
    16: "object_handles",
    17: "sector_handles",
    18: "lifts",
    19: "item",
    20: "sob_table",
}


class SaveError(ValueError):
    """Corrupt, truncated or otherwise unsupported save."""


def _u32(data: bytes, off: int, what: str = "u32") -> int:
    if off < 0 or off + 4 > len(data):
        raise SaveError("unexpected end of file reading %s at 0x%x" % (what, off))
    return struct.unpack_from("<I", data, off)[0]


class _R:
    """Bounds-checked reader over the record stream."""

    __slots__ = ("d", "o")

    def __init__(self, data: bytes, off: int = 0) -> None:
        self.d, self.o = data, off

    def u8(self, what: str = "u8") -> int:
        if self.o >= len(self.d):
            raise SaveError("unexpected end of file reading %s at 0x%x"
                            % (what, self.o))
        v = self.d[self.o]
        self.o += 1
        return v

    def u32(self, what: str = "u32") -> int:
        v = _u32(self.d, self.o, what)
        self.o += 4
        return v

    def take(self, n: int, what: str) -> bytes:
        if n < 0 or self.o + n > len(self.d):
            raise SaveError("truncated %s: need %d bytes at 0x%x, have %d"
                            % (what, n, self.o, len(self.d) - self.o))
        b = self.d[self.o:self.o + n]
        self.o += n
        return b


# --- header -----------------------------------------------------------------


class Header:
    """The fixed 0x13c-byte save header, split into named fields."""

    __slots__ = ("name", "flags", "level", "fields", "names")

    def __init__(self, name: bytes, flags: int, level: bytes,
                 fields: list[int], names: bytes) -> None:
        self.name = name
        self.flags = flags
        self.level = level
        self.fields = fields
        self.names = names

    @classmethod
    def parse(cls, data: bytes) -> "Header":
        if len(data) < HEADER_SIZE:
            raise SaveError("too short for a save header: %d < 0x%x"
                            % (len(data), HEADER_SIZE))
        name = data[0:0x50]
        flags = _u32(data, 0x50, "header flags")
        level = data[0x54:0xA4]
        fields = list(struct.unpack_from("<22I", data, 0xA4))
        names = data[0xFC:HEADER_SIZE]
        if len(names) != NAME_SLOTS * NAME_SLOT:
            raise SaveError("short header name slots")
        title = name[:12].split(b"\0")[0]
        if any(c < 0x20 or c > 0x7E for c in title):
            raise SaveError("save title is not printable ASCII: %r" % title)
        lvl = level[:NAME_SLOT].split(b"\0")[0]
        if not lvl or any(not (48 <= c <= 57 or 65 <= c <= 90 or 97 <= c <= 122
                               or c in b"_-") for c in lvl):
            raise SaveError("bad level name in header: %r" % lvl)
        return cls(name, flags, level, fields, names)

    @property
    def title(self) -> str:
        return self.name[:12].split(b"\0")[0].decode("latin1")

    @property
    def level_name(self) -> str:
        return self.level[:NAME_SLOT].split(b"\0")[0].decode("latin1")

    def name_slots(self) -> list[str]:
        return [self.names[i:i + NAME_SLOT].split(b"\0")[0].decode("latin1")
                for i in range(0, len(self.names), NAME_SLOT)]

    def to_bytes(self) -> bytes:
        if len(self.name) != 0x50 or len(self.level) != 0x50:
            raise SaveError("internal: header name/level must be 0x50 bytes")
        if len(self.fields) != 22:
            raise SaveError("internal: 22 header fields expected")
        if len(self.names) != NAME_SLOTS * NAME_SLOT:
            raise SaveError("internal: 4 x 16 header name bytes expected")
        return (self.name + struct.pack("<I", self.flags) + self.level
                + struct.pack("<22I", *self.fields) + self.names)

    def set_name_byte(self, off: int, value: int) -> None:
        b = bytearray(self.name)
        b[off] = value & 0xFF
        self.name = bytes(b)


# --- record payloads --------------------------------------------------------


class Record:
    type: int
    offset: int
    status: str = "raw"
    detail: str = ""

    def encode(self) -> bytes:
        raise NotImplementedError

    def info(self) -> dict:
        return {"type": self.type, "name": BLOCK_NAMES.get(self.type, "?"),
                "offset": self.offset, "size": len(self.encode()) + 1,
                "status": self.status, "detail": self.detail}


class SampleTable(Record):
    """Type 12: 300 reference counts + 300 nine-byte sample names."""

    status = "field"

    def __init__(self, offset: int, counts: list[int], names: list[bytes]):
        self.type, self.offset = 12, offset
        self.counts, self.names = counts, names
        self.detail = "%d samples" % sum(1 for n in names if n.split(b"\0")[0])

    @classmethod
    def parse(cls, r: _R, offset: int) -> "SampleTable":
        counts = list(struct.unpack("<%dI" % SAMPLE_COUNTS,
                                    r.take(4 * SAMPLE_COUNTS, "sample counts")))
        blob = r.take(SAMPLE_NAME * SAMPLE_COUNTS, "sample names")
        names = [blob[i * SAMPLE_NAME:(i + 1) * SAMPLE_NAME]
                 for i in range(SAMPLE_COUNTS)]
        return cls(offset, counts, names)

    def encode(self) -> bytes:
        if len(self.counts) != SAMPLE_COUNTS or len(self.names) != SAMPLE_COUNTS:
            raise SaveError("sample table must hold 300 entries")
        for n in self.names:
            if len(n) != SAMPLE_NAME:
                raise SaveError("sample name must be 9 bytes")
        return struct.pack("<%dI" % SAMPLE_COUNTS, *self.counts) + b"".join(self.names)


class SbtPayload(Record):
    """Types 4/5/11/15/16/17/20: parsed and encoded by :mod:`gm.sbt`."""

    status = "field"

    def __init__(self, type_: int, offset: int, payload: object,
                 encoded: bytes) -> None:
        self.type, self.offset = type_, offset
        self.payload, self._enc = payload, encoded
        self.detail = "%d bytes" % len(encoded)

    @classmethod
    def parse(cls, r: _R, offset: int, type_: int) -> "SbtPayload":
        start = r.o
        try:
            payload = sbtmod._read_record(r, type_)
        except SaveError:
            raise
        except Exception as exc:                    # gm.sbt raises ValueError
            raise SaveError("block %d: %s" % (type_, exc))
        body = r.d[start:r.o]
        try:
            encoded = sbtmod.encode_payload(type_, payload)
        except Exception as exc:
            raise SaveError("block %d cannot be re-encoded: %s" % (type_, exc))
        if encoded != body:
            raise SaveError("block %d re-encode mismatch (%d vs %d bytes)"
                            % (type_, len(encoded), len(body)))
        return cls(type_, offset, payload, encoded)

    def encode(self) -> bytes:
        out = sbtmod.encode_payload(self.type, self.payload)
        if out != self._enc:
            raise SaveError("block %d changed while rebuilding" % self.type)
        return out


class ArrayBlock(Record):
    """Types 1/2/6/7: ``u32 count`` + fixed-stride opaque records.

    The count and the stride are structural (taken from the saver code); the
    record bodies are raw runtime structs and are kept verbatim.
    """

    status = "count"

    def __init__(self, type_: int, offset: int, stride: int, count: int,
                 records: list[bytes]) -> None:
        self.type, self.offset, self.stride = type_, offset, stride
        self.count, self.records = count, records
        self.detail = "%d x 0x%x" % (count, stride)

    @classmethod
    def parse(cls, r: _R, offset: int, type_: int, stride: int,
              limit: int) -> "ArrayBlock":
        count = r.u32("block %d count" % type_)
        if count > limit:
            raise SaveError("block %d: count %d out of range [0, %d]"
                            % (type_, count, limit))
        recs = [r.take(stride, "block %d record" % type_) for _ in range(count)]
        return cls(type_, offset, stride, count, recs)

    def encode(self) -> bytes:
        if self.count != len(self.records):
            raise SaveError("block %d: count %d != %d records"
                            % (self.type, self.count, len(self.records)))
        for rec in self.records:
            if len(rec) != self.stride:
                raise SaveError("block %d: record must be 0x%x bytes"
                                % (self.type, self.stride))
        return struct.pack("<I", self.count) + b"".join(self.records)


class LiftBlock(ArrayBlock):
    """Type 18: ``u32 lifts`` + lifts*168, then ``u32 stops`` + stops*12."""

    def __init__(self, type_: int, offset: int, stride: int, count: int,
                 records: list[bytes], stops: bytes) -> None:
        super().__init__(type_, offset, stride, count, records)
        self.stops = stops
        self.detail = "%d lifts + %d stop bytes" % (count, len(stops))

    @classmethod
    def parse(cls, r: _R, offset: int) -> "LiftBlock":
        count = r.u32("lift count")
        if count > MAX_LIFTS:
            raise SaveError("lifts: count %d out of range [0, %d]"
                            % (count, MAX_LIFTS))
        recs = [r.take(168, "lift") for _ in range(count)]
        stops = r.u32("lift stop count")
        if stops > MAX_STOPS:
            raise SaveError("lifts: stop count %d out of range [0, %d]"
                            % (stops, MAX_STOPS))
        extra = struct.pack("<I", stops) + r.take(12 * stops, "lift stops")
        return cls(18, offset, 168, count, recs, extra)

    def encode(self) -> bytes:
        return super().encode() + self.stops


class FixedBlock(Record):
    """Types 8/19: one fixed-size opaque payload."""

    status = "raw"

    def __init__(self, type_: int, offset: int, body: bytes) -> None:
        self.type, self.offset = type_, offset
        self.body = body
        self.detail = "%d opaque bytes" % len(body)

    @classmethod
    def parse(cls, r: _R, offset: int, type_: int, size: int) -> "FixedBlock":
        return cls(type_, offset, r.take(size, BLOCK_NAMES[type_]))

    def encode(self) -> bytes:
        return self.body


class ThingBlock(Record):
    """Type 3 (save layout): marker once, then 260 slots ``u8 + [0x528]``."""

    status = "field"

    def __init__(self, offset: int, present: list[bool],
                 records: list[bytes | None]) -> None:
        self.type, self.offset = 3, offset
        self.present, self.records = present, records
        self.detail = "%d/%d present" % (sum(present), THING_SLOTS)

    @classmethod
    def parse(cls, r: _R, offset: int) -> "ThingBlock":
        present, records = [], []
        for _ in range(THING_SLOTS):
            flag = r.u8("thing slot flag")
            if flag not in (0, 1):
                raise SaveError("things: presence flag %d is not 0/1" % flag)
            present.append(bool(flag))
            records.append(r.take(THING_RECORD, "thing record") if flag else None)
        return cls(offset, present, records)

    def encode(self) -> bytes:
        if len(self.present) != THING_SLOTS or len(self.records) != THING_SLOTS:
            raise SaveError("things: exactly 260 slots are expected")
        out = bytearray()
        for flag, rec in zip(self.present, self.records):
            out.append(1 if flag else 0)
            if flag:
                if rec is None or len(rec) != THING_RECORD:
                    raise SaveError("things: present slot needs 0x528 bytes")
                out += rec
        return bytes(out)


class PlayerBlock(Record):
    """Type 9: marker, 8 x (present + 0x3510 + attachments), 2 sprite blocks."""

    status = "field"

    def __init__(self, offset: int, present: list[int],
                 structs: list[bytes | None],
                 attachments: list[list[bytes]],
                 sub_a: tuple[bytes, bytes], sub_b: tuple[bytes, bytes]) -> None:
        self.type, self.offset = 9, offset
        self.present, self.structs = present, structs
        self.attachments = attachments
        self.sub_a, self.sub_b = sub_a, sub_b
        n_att = sum(len(a) for a in attachments)
        self.detail = "%d/%d present, %d attachments" % (
            sum(present), PLAYER_SLOTS, n_att)

    @classmethod
    def parse(cls, r: _R, offset: int) -> "PlayerBlock":
        present, structs, attachments = [], [], []
        for i in range(PLAYER_SLOTS):
            flag = r.u8("player %d flag" % i)
            if flag not in (0, 1):
                raise SaveError("player %d: presence flag %d is not 0/1"
                                % (i, flag))
            present.append(flag)
            if not flag:
                structs.append(None)
                attachments.append([])
                continue
            st = r.take(PLAYER_STRUCT, "player %d struct" % i)
            att = []
            for k in range(ATTACH_MAX):
                if _u32_unchecked(st, ATTACH_AT + 4 * k):
                    att.append(r.take(ATTACH_RECORD, "player attachment"))
            structs.append(st)
            attachments.append(att)
        sub_a = (r.take(PLAYER_SUB_HEADER, "player sub-header A"),
                 r.take(PLAYER_STRUCT, "player sub-struct A"))
        sub_b = (r.take(PLAYER_SUB_HEADER, "player sub-header B"),
                 r.take(PLAYER_STRUCT, "player sub-struct B"))
        return cls(offset, present, structs, attachments, sub_a, sub_b)

    def encode(self) -> bytes:
        if len(self.present) != PLAYER_SLOTS:
            raise SaveError("player: exactly 8 slots are expected")
        out = bytearray()
        for i, flag in enumerate(self.present):
            out.append(1 if flag else 0)
            if not flag:
                continue
            st = self.structs[i]
            if st is None or len(st) != PLAYER_STRUCT:
                raise SaveError("player %d: struct must be 0x3510 bytes" % i)
            out += st
            want = [k for k in range(ATTACH_MAX)
                    if _u32_unchecked(st, ATTACH_AT + 4 * k)]
            att = self.attachments[i]
            if len(att) != len(want):
                raise SaveError("player %d: %d attachments for %d ids"
                                % (i, len(att), len(want)))
            for rec in att:
                if len(rec) != ATTACH_RECORD:
                    raise SaveError("player attachment must be 16 bytes")
                out += rec
        for header, st in (self.sub_a, self.sub_b):
            if len(header) != PLAYER_SUB_HEADER or len(st) != PLAYER_STRUCT:
                raise SaveError("player sprite block has a bad size")
            out += header + st
        return bytes(out)


def _u32_unchecked(data: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(data):
        raise SaveError("struct offset 0x%x out of range" % off)
    return struct.unpack_from("<I", data, off)[0]


# --- container --------------------------------------------------------------


class Save:
    __slots__ = ("header", "blocks", "size", "path")

    def __init__(self, header: Header, blocks: dict[int, Record], size: int,
                 path: str = "") -> None:
        self.header, self.blocks, self.size, self.path = header, blocks, size, path

    @classmethod
    def parse(cls, data: bytes, path: str = "") -> "Save":
        if len(data) > MAX_FILE:
            raise SaveError("%s: %d bytes exceeds the %d-byte limit"
                            % (path or "save", len(data), MAX_FILE))
        header = Header.parse(data)
        r = _R(data, HEADER_SIZE)
        blocks: dict[int, Record] = {}
        while True:
            if r.o >= len(data):
                raise SaveError("record stream has no 0xff terminator")
            offset = r.o
            type_ = r.u8("record type")
            if type_ == 255:
                if r.o != len(data):
                    raise SaveError("%d trailing bytes after 0xff"
                                    % (len(data) - r.o))
                break
            if type_ in blocks:
                raise SaveError("duplicate block %d at 0x%x" % (type_, offset))
            if len(blocks) >= MAX_BLOCKS:
                raise SaveError("too many blocks (> %d)" % MAX_BLOCKS)
            blocks[type_] = _parse_block(r, offset, type_)
        missing = [t for t in REQUIRED if t not in blocks]
        if missing:
            raise SaveError("missing block(s): %s"
                            % ", ".join("%d(%s)" % (t, BLOCK_NAMES.get(t, "?"))
                                        for t in missing))
        return cls(header, blocks, len(data), path)

    def to_bytes(self) -> bytes:
        out = bytearray(self.header.to_bytes())
        for type_ in ORDER:
            rec = self.blocks.get(type_)
            if rec is not None:
                out.append(type_)
                out += rec.encode()
        out.append(255)
        if len(out) > MAX_FILE:
            raise SaveError("rebuilt save is too large (%d bytes)" % len(out))
        return bytes(out)

    def info(self) -> dict:
        return {"file": os.path.basename(self.path) if self.path else "",
                "bytes": self.size,
                "title": self.header.title,
                "level": self.header.level_name,
                "header_fields": ["0x%x" % v for v in self.header.fields],
                "name_slots": self.header.name_slots(),
                "blocks": [self.blocks[t].info() for t in ORDER if t in self.blocks]}


def _parse_block(r: _R, offset: int, type_: int) -> Record:
    if type_ == 12:
        return SampleTable.parse(r, offset)
    if type_ in (4, 5, 11, 15, 16, 17, 20):
        return SbtPayload.parse(r, offset, type_)
    if type_ == 2:
        return ArrayBlock.parse(r, offset, 2, 0xB8, MAX_DOORS)
    if type_ == 1:
        return ArrayBlock.parse(r, offset, 1, 0x20, MAX_SWITCHES)
    if type_ == 6:
        return ArrayBlock.parse(r, offset, 6, 44, MAX_SPRITES)
    if type_ == 7:
        return ArrayBlock.parse(r, offset, 7, 28, MAX_SOUNDS)
    if type_ == 18:
        return LiftBlock.parse(r, offset)
    if type_ == 19:
        return FixedBlock.parse(r, offset, 19, 0x7080)
    if type_ == 8:
        return FixedBlock.parse(r, offset, 8, 0x5800)
    if type_ == 3:
        return ThingBlock.parse(r, offset)
    if type_ == 9:
        return PlayerBlock.parse(r, offset)
    raise SaveError("unknown record type %d at 0x%x" % (type_, offset))


# --- reporting --------------------------------------------------------------


def load(path: str) -> Save:
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_FILE + 1)
    except OSError as exc:
        raise SaveError("%s: %s" % (path, exc))
    return Save.parse(data, path)


def diff_runs(a: bytes, b: bytes, limit: int = 16) -> list[tuple[int, int]]:
    runs, start = [], None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i))
            start = None
            if len(runs) >= limit:
                return runs
    if start is not None:
        runs.append((start, min(len(a), len(b))))
    return runs


def print_blocks(save: Save) -> None:
    print("  %-4s %-16s %8s %-7s %s"
          % ("id", "block", "offset", "size", "status/detail"))
    for type_ in ORDER:
        rec = save.blocks.get(type_)
        if rec is None:
            continue
        info = rec.info()
        print("  %-4d %-16s 0x%06x %8d %-7s %s"
              % (type_, info["name"], info["offset"], info["size"],
                 info["status"], info["detail"]))


def check(paths: list[str], as_json: bool, blocks: bool) -> int:
    bad = 0
    rows = []
    for path in paths:
        try:
            save = load(path)
            rebuilt = save.to_bytes()
            with open(path, "rb") as f:
                orig = f.read()
        except (SaveError, OSError) as exc:
            print("%s: %s" % (os.path.basename(path), exc), file=sys.stderr)
            bad += 1
            continue
        ok = rebuilt == orig
        rows.append({"file": os.path.basename(path), "bytes": save.size,
                     "blocks": len(save.blocks), "byte_exact": ok,
                     "info": save.info()})
        if not as_json:
            print("%s: %s, %d bytes, %d blocks"
                  % ("ok  " if ok else "FAIL", os.path.basename(path),
                     save.size, len(save.blocks)))
            if blocks:
                print_blocks(save)
            if not ok:
                runs = diff_runs(orig, rebuilt)
                print("  first differences: %s"
                      % ", ".join("0x%x..0x%x" % r for r in runs))
                bad += 1
    if as_json:
        print(json.dumps(rows, indent=2))
    total = len(paths)
    good = total - bad
    if not as_json:
        print("%d/%d saves round-trip byte-exact" % (good, total))
    return 0 if bad == 0 else 2


def write_out(save: Save, out_dir: str, force: bool) -> str:
    name = os.path.basename(save.path)
    if not name:
        raise SaveError("cannot derive an output name")
    dst = os.path.join(out_dir, name)
    data = save.to_bytes()
    if len(data) > MAX_FILE:
        raise SaveError("%s: rebuilt is too large" % name)
    if os.path.exists(dst) and not force:
        raise SaveError("%s exists (use --force to overwrite)" % dst)
    os.makedirs(out_dir, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix="." + name + ".", suffix=".tmp", dir=out_dir)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dst)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return dst


def run_diff(paths: list[str]) -> int:
    bad = 0
    for path in paths:
        try:
            save = load(path)
            rebuilt = save.to_bytes()
            orig = open(path, "rb").read()
        except (SaveError, OSError) as exc:
            print("%s: %s" % (os.path.basename(path), exc), file=sys.stderr)
            bad += 1
            continue
        runs = diff_runs(orig, rebuilt)
        if not runs and len(orig) == len(rebuilt):
            print("%s: rebuild is identical (%d bytes)"
                  % (os.path.basename(path), len(orig)))
        else:
            bad += 1
            print("%s: %d byte(s) differ, %d run(s)"
                  % (os.path.basename(path),
                     sum(e - s for s, e in runs) + abs(len(orig) - len(rebuilt)),
                     len(runs)))
            for start, end in runs:
                print("  0x%06x..0x%06x  orig %s  rebuilt %s"
                      % (start, end, orig[start:start + 8].hex(),
                         rebuilt[start:start + 8].hex()))
    return 0 if bad == 0 else 2


# --- self tests -------------------------------------------------------------


def _expect_error(label: str, fn, results: list[bool]) -> None:
    try:
        fn()
    except SaveError:
        results.append(True)
        print("  ok   %s" % label)
    except Exception as exc:                        # noqa: BLE001
        results.append(False)
        print("  FAIL %s (wrong exception %s: %s)"
              % (label, type(exc).__name__, exc))
    else:
        results.append(False)
        print("  FAIL %s (accepted)" % label)


def self_test(paths: list[str]) -> int:
    results: list[bool] = []
    if not paths:
        print("self-test: no save supplied", file=sys.stderr)
        return 1
    sample = paths[0]
    data = open(sample, "rb").read()
    save = Save.parse(data, sample)
    rebuilt = save.to_bytes()

    print("round-trip")
    results.append(rebuilt == data)
    print("  %s %s" % ("ok  " if rebuilt == data else "FAIL",
                       os.path.basename(sample)))

    print("order independence (writer emits the driver's canonical order)")
    rev = Save(save.header, dict(reversed(list(save.blocks.items()))), save.size,
               sample)
    ok = rev.to_bytes() == data
    results.append(ok)
    print("  %s reversed in-memory block order still rebuilds identically"
          % ("ok  " if ok else "FAIL"))

    print("field independence (no hard-coded header/version bytes)")
    mut = Save.parse(data, sample)
    mut.header.set_name_byte(0x0C, mut.header.name[0x0C] ^ 0xFF)
    other = mut.to_bytes()
    runs = diff_runs(data, other)
    ok = runs == [(0x0C, 0x0D)] and len(other) == len(data)
    results.append(ok)
    print("  %s changing header byte 0x0c changes only that byte" %
          ("ok  " if ok else "FAIL"))

    mut = Save.parse(data, sample)
    mut.header.fields[0] = (mut.header.fields[0] + 1) & 0xFFFFFFFF
    other = mut.to_bytes()
    runs = diff_runs(data, other)
    ok = runs == [(0xA4, 0xA5)]
    results.append(ok)
    print("  %s changing header field 0 at 0xa4 changes only that field"
          % ("ok  " if ok else "FAIL"))

    print("negative cases")
    _expect_error("empty file", lambda: Save.parse(b""), results)
    _expect_error("short header", lambda: Save.parse(data[:0x40]), results)
    _expect_error("truncated in the header", lambda: Save.parse(data[:0x100]),
                  results)
    _expect_error("truncated in the first block",
                  lambda: Save.parse(data[:HEADER_SIZE + 8]), results)
    door = save.blocks[2]
    _expect_error("truncated door record",
                  lambda: Save.parse(data[:door.offset + 8]), results)

    giant = bytearray(data)
    struct.pack_into("<I", giant, door.offset + 1, 0xFFFFFFFF)
    _expect_error("giant door count (0xffffffff)",
                  lambda: Save.parse(bytes(giant)), results)

    zero = bytearray(data)
    struct.pack_into("<I", zero, door.offset + 1, 0)
    _expect_error("door count zero with the records still present",
                  lambda: Save.parse(bytes(zero)), results)

    thing = save.blocks[3]
    bad_flag = bytearray(data)
    bad_flag[thing.offset + 1] = 2
    _expect_error("thing presence flag 2",
                  lambda: Save.parse(bytes(bad_flag)), results)

    player = save.blocks[9]
    bad_player = bytearray(data)
    bad_player[player.offset + 1] = 7
    _expect_error("player presence flag 7",
                  lambda: Save.parse(bytes(bad_player)), results)

    bad_type = bytearray(data)
    bad_type[door.offset] = 0xEE
    _expect_error("unknown block type 0xee",
                  lambda: Save.parse(bytes(bad_type)), results)

    _expect_error("trailing byte after 0xff",
                  lambda: Save.parse(data + b"\x00"), results)

    print("atomic --out")
    with tempfile.TemporaryDirectory(prefix="savere-") as tmp:
        first = write_out(save, tmp, force=False)
        ok = open(first, "rb").read() == data
        results.append(ok)
        print("  %s writes the rebuilt bytes" % ("ok  " if ok else "FAIL"))
        try:
            write_out(save, tmp, force=False)
        except SaveError:
            results.append(True)
            print("  ok   refuses to overwrite without --force")
        else:
            results.append(False)
            print("  FAIL overwrote without --force")
        again = write_out(save, tmp, force=True)
        results.append(open(again, "rb").read() == data)
        print("  %s --force overwrites atomically" %
              ("ok  " if results[-1] else "FAIL"))
        leftovers = [n for n in os.listdir(tmp) if n.endswith(".tmp")]
        results.append(not leftovers)
        print("  %s no temporary files left behind"
              % ("ok  " if not leftovers else "FAIL"))

    good = sum(1 for r in results if r)
    print("%d/%d self-test checks passed" % (good, len(results)))
    return 0 if good == len(results) else 1


# --- CLI --------------------------------------------------------------------


def default_saves(game: str) -> list[str]:
    return sorted(glob.glob(os.path.join(game, "SAVEGAME", "*.sbt")))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saves", nargs="*", help="SAVEGAME/saved<N>.sbt files")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx",
                    help="installation used when no save is given")
    ap.add_argument("--check", action="store_true",
                    help="parse + serialise in memory, compare with the file")
    ap.add_argument("--out", metavar="DIR",
                    help="atomically write rebuilt saves into DIR")
    ap.add_argument("--force", action="store_true",
                    help="with --out: overwrite existing files")
    ap.add_argument("--diff", action="store_true",
                    help="print byte differences between original and rebuild")
    ap.add_argument("--blocks", action="store_true",
                    help="with --check: print the per-block map")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--self-test", action="store_true",
                    help="run round-trip, order/version and corruption tests")
    args = ap.parse_args(argv)

    paths = list(args.saves) or default_saves(args.game)
    if not paths:
        print("no saves found (pass a file or --game)", file=sys.stderr)
        return 1

    try:
        if args.self_test:
            return self_test(paths)
        if args.out:
            bad = 0
            seen = set()
            for path in paths:
                base = os.path.basename(path)
                if base in seen:
                    print("%s: duplicate output name, skipping" % base,
                          file=sys.stderr)
                    bad += 1
                    continue
                seen.add(base)
                try:
                    dst = write_out(load(path), args.out, args.force)
                    print("%s -> %s" % (os.path.basename(path), dst))
                except (SaveError, OSError) as exc:
                    print("%s: %s" % (os.path.basename(path), exc),
                          file=sys.stderr)
                    bad += 1
            return 0 if bad == 0 else 1
        if args.diff:
            return run_diff(paths)
        return check(paths, args.json, args.blocks)
    except SaveError as exc:
        print("save: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
