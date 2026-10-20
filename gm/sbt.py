"""Gunmetal ".sbt" world/level files — a compiled "World Things file".

The engine has two loaders for a world: a *text* loader (``sbt_load``) and the
binary one that reads the shipped ``*.sbt`` ("BINARY SAVE").  The binary file is
simply the compiled text file, and the 1-byte record *type* is the numeric id of
the text keyword:

    "POLY_HANDLE"        -> 15      "SCRIPT"             -> 11
    "OBJECT_HANDLE"      -> 16      "END_OF_FILE"        -> 255
    "SECTOR_HANDLE"      -> 17      (and the rest below)

The text loader/saver at ``0x454b8d``/``0x454d20`` compares/produces those
keywords and reads one number line at a time (``"POLY_HANDLE"`` then
``"%ld %ld %ld %ld"`` per handle, etc. -- see the format strings at ``0x4b9600``),
which is exactly the payload the binary handlers below consume.

File header (84 bytes)::

    char magic[80];     // b"BINARY SAVE\\0" followed by zero padding
    u32  version;       // must be 0

Then a stream of records.  Each record starts with a 1-byte *type*; the
payload immediately follows and is fully consumed by that type's handler before
the next type byte is read.  ``type == 255`` (``END_OF_FILE``) ends the stream
and has no payload, so it is the last byte of the file.

Type table (numbers are the on-disk type byte; names are the engine's own,
taken from the handler error strings)::

    1  SWITCH               u32 n (<=0x200);  n * 32 bytes
    2  DOOR                 u32 n (<=100);    n * 184 bytes
    3  THING                u8 exists;        if exists: 0x528 bytes
    4  THING_TYPES          u32 n (<=0x80);   n * 936-byte types
    5  NAMED_ITEMS          u32 n (<=0x31);   n * 52-byte items
    6  SPRITES              u32 n (<0x1f4);   n * 44 bytes
    7  SOUND_SOURCES        u32 n (<0x80);    5*n*4 + n*8 bytes
    8  SECTOR_STUFF         fixed 0x5800 bytes
    9  PLAYER_STATUS        u8 exists;        if exists: 0x3510 bytes
    11 SCRIPT               see below
    12 SOUND_SAMPLE_TABLE   fixed 0x4b0 + 0xa8c bytes (300 slots)
    15 POLY_HANDLE          u32 n (<0x1f4);   4 parallel u32 columns
    16 OBJECT_HANDLE        u32 n (<0x12c);   3 parallel u32 columns
    17 SECTOR_HANDLE        u32 n (<0x100);   2 parallel u32 columns
    18 LIFT                 u32 n (<=0x1e); n*168; u32 k (<=0xc8); k*12
    19 ITEM                 fixed 0x7080 bytes
    20 SOB_TABLE            u32 n (<=0xc8);   n * 20 bytes
    255 END_OF_FILE

Types 10 and 13 map to the stub ``0x4971b0`` (``mov eax,-1; ret``): the
dispatcher accepts the byte but the type carries no payload and never occurs
in shipped data.  Type 14 jumps to the "corrupt" error path ``0x4555ae``.

The handler for each type is not a guess: ``0x455150`` indexes the byte table
``0x455658`` with ``type-1`` and jumps through ``0x455604``.  The addresses in
the table above are the first instruction of each handler; the full map and
per-field evidence are in ``docs/SBT_RECORDS.md``.

The three handle tables (15/16/17) are the interesting part.  They are stored
*column-major*: for ``n`` handles the file holds ``n`` u32 of the first column,
then ``n`` u32 of the second, and so on -- the loaders read each column straight
into its own global array (poly: ``0x5b21f8``, ``0x618b88``, ``0x5e5680``,
``0x6183b8``).  Columns::

    POLY_HANDLE    tag, sector, object, poly
    OBJECT_HANDLE  tag, sector, object
    SECTOR_HANDLE  tag, sector

``(sector, object, poly)`` is the engine's canonical polygon address: it is what
``resolve_poly`` (``0x434463``) consumes and what the editor's debug overlay
prints as ``SECTOR:%d OBJECT:%d POLY:%d`` (globals ``0x62cae0/e4/e8``).  All
4277 poly handles of the 36 shipped levels resolve inside the matching ``.sbw``.
The leading ``tag`` is an opaque, editor-assigned id (the ``SET_HANDLEPOLY_*`` /
``SET_HANDLESECTOR_*`` script commands address an entity by tag; the ``SET_POLY_*``
family addresses it by ``(sector, object, poly)``); it is not unique and is not a
function of the polygon data across levels.

THING_TYPES entry (936 bytes)::

    char name[16];      // e.g. "bgchunk1"
    double params[49];  // 0x10..0x198
    u32    tail[132];   // 0x198..0x3a8 (flags/ids; opaque)

The engine's "add thing type" console handler (``0x47cb6c``) loads each one from
a *text* file ``thing\\<name>.soe`` (see ``docs/FORMATS.md`` for ``.soe``), which
is then compiled into this binary block.

NAMED_ITEMS (type 5) entries are ``{char name[16]; u32 f[9]}`` and come from a
text file ``item\\<name>.soi`` -- the loader (``0x4689f0``) reads nine tokens with
``"%ld %ld %s %ld %s %ld %s %ld %ld"`` (three of them resolved by name via
``0x45fb90``) and writes them at ``+0x10``..``+0x30``, which is why the name is
16 bytes and the integer block starts at ``+0x10``.

SOUND_SAMPLE_TABLE (type 12) holds 300 ``u32`` reference counts followed by 300
9-byte names; every non-empty name resolves against ``sounds/SOU.IDX`` (5800
references over the shipped levels).  SOB_TABLE (type 20) entries are
``{char name[12]; u32 amount; u32 ptr}``; ``name`` resolves to ``SOBS/<name>.sob``
or ``Sobanim/<name>.soo`` (the loader logs "Unable to load SOB or SOO %s"), and
``ptr`` is a stale heap address that the loader overwrites with the resolved
model pointer (``0x47c845``); ``amount`` is never read back by the loader.

Every shipped level carries the same record sequence
``12, 20, [15, 16,] 17, 5, 4, 11, 255``: the sound/sample name table, the SOB
(model) table, the poly/object/sector handle tables, named items, thing types,
and the compiled script block.  Fields not listed above are still opaque and are
kept as raw bytes with their byte length.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"BINARY SAVE\x00"
HEADER_SIZE = 84  # 80-byte magic block + u32 version

#: Loader-enforced counters (the binary dispatcher ``0x455150`` / table
#: ``0x455604`` jumps to one handler per type; each handler validates its own
#: counter before reading the records).
SWITCH_MAX = 0x200
DOOR_MAX = 100
SPRITE_MAX = 0x1F3
SOUND_SOURCE_MAX = 0x7F
LIFT_MAX = 0x1E
STOP_MAX = 0xC8
SAMPLE_SLOTS = 300

TYPE_NAMES = {
    1: "SWITCH",
    2: "DOOR",
    3: "THING",
    4: "THING_TYPES",
    5: "NAMED_ITEMS",
    6: "SPRITES",
    7: "SOUND_SOURCES",
    8: "SECTOR_STUFF",
    9: "PLAYER_STATUS",
    11: "SCRIPT",
    12: "SOUND_SAMPLE_TABLE",
    15: "POLY_HANDLE",
    16: "OBJECT_HANDLE",
    17: "SECTOR_HANDLE",
    18: "LIFT",
    19: "ITEM",
    20: "SOB_TABLE",
}
END_OF_FILE = 255
UNSUPPORTED = (10, 13)


# --- per-type payloads -------------------------------------------------------


@dataclass
class Switch:
    count: int
    records: list[bytes] = field(default_factory=list)  # 32 bytes each


@dataclass
class Door:
    count: int
    records: list[bytes] = field(default_factory=list)  # 184 bytes each


@dataclass
class Thing:
    exists: bool
    raw: bytes = b""


@dataclass
class ThingType:
    name: str
    params: tuple = ()  # 49 doubles
    tail: bytes = b""  # 528 bytes of opaque u32 fields
    name_raw: bytes = b""  # full 16-byte name slot (trailing bytes preserved)


@dataclass
class ThingTypes:
    entries: list[ThingType] = field(default_factory=list)


@dataclass
class NamedItem:
    name: str
    params: tuple = ()  # 9 u32 (f0..f3 plain, f4..f6 resolved ids, f7..f8)
    name_raw: bytes = b""  # full 16-byte name slot


@dataclass
class NamedItems:
    entries: list[NamedItem] = field(default_factory=list)


@dataclass
class Sprites:
    """``SPRITES`` (6): ``u32 n`` then ``n * 44`` records at ``0x5ab800``.

    The 44-byte record layout is documented in ``docs/SBT_RECORDS.md`` (field
    meaning from ``0x456130``..``0x456185``); it is kept as bytes here because
    the same runtime record is also written by the save chain.
    """

    count: int
    records: list[bytes] = field(default_factory=list)  # 44 bytes each
    entries: list[tuple] = field(default_factory=list)   # decoded view


@dataclass
class SoundSources:
    """``SOUND_SOURCES`` (7): five ``u32`` columns + one ``f64`` column.

    The loader (``0x46b280``) reads five parallel ``u32`` arrays followed by
    one parallel ``double`` array, all of ``count`` elements.  Column order is
    proven by ``ADD_SOUND_SOURCE`` / the saver: ``x, y, z, volume, sample,
    pitch`` (see ``docs/SAVE_FIELDS.md`` §6 and ``docs/SBT_RECORDS.md``).
    """

    count: int
    columns: tuple = ()  # five tuples of count ints
    pitches: tuple = ()  # count doubles (the sixth column)


@dataclass
class SectorSlot:
    """One 0x58-byte ``SECTOR_STUFF`` slot (layout in docs/RECORDS.md §2)."""

    move: tuple = (0.0, 0.0, 0.0)
    extra: tuple = (0.0, 0.0, 0.0)
    sound_param: float = 0.0
    event_a: int = 0
    event_b: int = 0
    damage_lo: int = 0
    damage_hi: int = 0
    sound: int = 0
    sound_count: int = 0
    flags: int = 0
    unused: int = 0
    pad_42: bytes = b"\0\0"
    pad_46: bytes = b"\0\0"

    def encode(self) -> bytes:
        out = bytearray(struct.pack("<3d", *self.move))
        out += struct.pack("<3d", *self.extra)
        out += struct.pack("<d", self.sound_param)
        out += struct.pack("<ii", self.event_a, self.event_b)
        out += struct.pack("<H", self.damage_lo) + self.pad_42
        out += struct.pack("<H", self.damage_hi) + self.pad_46
        out += struct.pack("<iiII", self.sound, self.sound_count,
                           self.flags, self.unused)
        assert len(out) == 0x58
        return bytes(out)

    @classmethod
    def decode(cls, raw: bytes) -> "SectorSlot":
        if len(raw) != 0x58:
            raise ValueError("sector slot is %d bytes, expected 0x58" % len(raw))
        return cls(
            struct.unpack_from("<3d", raw, 0x00),
            struct.unpack_from("<3d", raw, 0x18),
            struct.unpack_from("<d", raw, 0x30)[0],
            struct.unpack_from("<i", raw, 0x38)[0],
            struct.unpack_from("<i", raw, 0x3C)[0],
            struct.unpack_from("<H", raw, 0x40)[0],
            struct.unpack_from("<H", raw, 0x44)[0],
            struct.unpack_from("<i", raw, 0x48)[0],
            struct.unpack_from("<i", raw, 0x4C)[0],
            struct.unpack_from("<I", raw, 0x50)[0],
            struct.unpack_from("<I", raw, 0x54)[0],
            raw[0x42:0x44], raw[0x46:0x48],
        )


@dataclass
class ItemSlot:
    """One 0x60-byte ``ITEM`` slot (layout in docs/RECORDS.md §3)."""

    basis: tuple = (0.0, 0.0, 0.0)
    position: tuple = (0.0, 0.0, 0.0)
    runtime_object: int = 0
    kind: int = 0
    state: int = 0
    op2: int = 0
    sob_index: int = 0
    ops: tuple = (0, 0, 0, 0, 0)
    flags: int = 0
    pad: bytes = b"\0\0\0"
    sentinel: int = 0xFFFF

    def encode(self) -> bytes:
        out = bytearray(struct.pack("<3d", *self.basis))
        out += struct.pack("<3d", *self.position)
        out += struct.pack("<IIiIi", self.runtime_object, self.kind,
                           self.state, self.op2, self.sob_index)
        out += struct.pack("<5I", *self.ops)
        out += bytes((self.flags,)) + self.pad
        out += struct.pack("<I", self.sentinel)
        assert len(out) == 0x60
        return bytes(out)

    @classmethod
    def decode(cls, raw: bytes) -> "ItemSlot":
        if len(raw) != 0x60:
            raise ValueError("item slot is %d bytes, expected 0x60" % len(raw))
        return cls(
            struct.unpack_from("<3d", raw, 0x00),
            struct.unpack_from("<3d", raw, 0x18),
            struct.unpack_from("<I", raw, 0x30)[0],
            struct.unpack_from("<I", raw, 0x34)[0],
            struct.unpack_from("<i", raw, 0x38)[0],
            struct.unpack_from("<I", raw, 0x3C)[0],
            struct.unpack_from("<i", raw, 0x40)[0],
            struct.unpack_from("<5I", raw, 0x44),
            raw[0x58],
            raw[0x59:0x5C],
            struct.unpack_from("<I", raw, 0x5C)[0],
        )


@dataclass
class SectorStuff:
    raw: bytes = b""
    slots: list = field(default_factory=list)


@dataclass
class PlayerStatus:
    exists: bool
    raw: bytes = b""


@dataclass
class Script:
    """The ``SCRIPT`` record (type 11) -- a level's compiled behaviour code.

    Layout (engine loader ``0x462a80``)::

        u32 count;                  // "things", count * 40 bytes follow
        count * 40  things;
        u32 n;                      // parallel handle columns, 3 x n u32
        n * 4 col_a; n * 4 col_b; n * 4 col_c;
        u32 m;                      // compiled code length
        m  code;                    // bytecode, see :mod:`gm.sbtvm`
        u8 flag;
        if flag: 9 * 0x400 tables;

    The three handle columns are stored separately here (not interleaved), so
    the raw payload is split into :attr:`things`, :attr:`handles` (n*12),
    :attr:`code` and :attr:`tables` (0 or 9*0x400 bytes).
    """

    count: int
    n: int
    m: int
    flag: int
    things: bytes = b""
    handles: bytes = b""
    code: bytes = b""
    tables: bytes = b""

    def encode(self) -> bytes:
        out = bytearray(struct.pack("<I", self.count))
        out += self.things
        out += struct.pack("<I", self.n)
        out += self.handles
        out += struct.pack("<I", self.m)
        out += self.code
        out.append(self.flag)
        out += self.tables
        return bytes(out)


@dataclass
class SampleTable:
    """``SOUND_SAMPLE_TABLE`` (12): 300 ``u32`` counts + 300 9-byte names.

    ``name_slots`` keeps the exact ``0xa8c`` name region (including bytes after
    the first NUL, which do occur in shipped files); ``names`` is the decoded
    view used by the rest of the code base.
    """

    counts: tuple = ()  # 300 u32
    names: tuple = ()  # 300 names (<=8 chars + NUL, 9-byte slots)
    name_slots: bytes = b""  # exact 0xa8c bytes


@dataclass
class HandleTable:
    """Editor-assigned handle tags plus the engine address each one names.

    The payload is column-major (see the module docstring), so a table of *n*
    handles is decoded into one tuple per column.  ``(sector, object)`` selects
    a world object and ``poly`` a polygon inside it -- the same triple the
    engine's ``resolve_poly`` (0x434463) accepts.  ``tags`` is an opaque,
    editor-assigned id used by the ``SET_HANDLEPOLY_*``/``SET_HANDLESECTOR_*``
    script commands.
    """

    tags: tuple = ()
    sectors: tuple = ()

    def __len__(self) -> int:
        return len(self.tags)

    def rows(self):
        """Yield ``(tag, sector, ...)`` tuples in file order."""
        return zip(*(getattr(self, f) for f in self._fields))

    _fields = ("tags", "sectors")  # class attr, not a dataclass field


@dataclass
class PolyHandles(HandleTable):
    """``(tag, sector, object, poly)`` -- a polygon selected by the world editor."""

    objects: tuple = ()
    polys: tuple = ()

    _fields = ("tags", "sectors", "objects", "polys")


@dataclass
class ObjectHandles(HandleTable):
    """``(tag, sector, object)`` -- a world object selected by the world editor."""

    objects: tuple = ()

    _fields = ("tags", "sectors", "objects")


@dataclass
class SectorHandles(HandleTable):
    """``(tag, sector)`` -- a sector selected by the world editor."""

    _fields = ("tags", "sectors")


@dataclass
class Handles(HandleTable):
    """Generic column-major handle table (kept for older callers)."""

    objects: tuple = ()
    polys: tuple = ()

    _fields = ("tags", "sectors", "objects", "polys")


@dataclass
class Lift:
    """``LIFT`` (18): ``u32 n`` + ``n * 0xa8`` + ``u32 k`` + three columns.

    The loader/writer is ``0x44ec20``/``0x44eed0``; the 168-byte records and
    the three parallel stop columns are stored verbatim because no shipped file
    contains a type-18 record (the layout is code-proven only).
    """

    lifts: int
    stops: int
    records: list[bytes] = field(default_factory=list)  # 0xa8 bytes each
    stop_columns: tuple = ()  # three tuples of *stops* u32


@dataclass
class Item:
    raw: bytes = b""
    slots: list = field(default_factory=list)


@dataclass
class SobEntry:
    name: str
    amount: int
    ptr: int = 0        # stale runtime heap pointer (kept so the record round-trips)
    name_raw: bytes = b""  # full 12-byte name slot


@dataclass
class SobTable:
    entries: list[SobEntry] = field(default_factory=list)


@dataclass
class Record:
    type: int
    name: str
    offset: int  # file offset of the type byte
    size: int  # total bytes, type byte included
    payload: object
    raw: bytes = b""  # payload bytes exactly as read (no type byte)


@dataclass
class Sbt:
    version: int
    records: list[Record] = field(default_factory=list)
    header_size: int = HEADER_SIZE
    header: bytes = b""  # the 84 raw header bytes (kept for exact re-serialisation)

    def records_of(self, type_: int) -> list[Record]:
        return [r for r in self.records if r.type == type_]

    def to_bytes(self) -> bytes:
        """Re-serialise the file from the stored raw payload bytes.

        Records are emitted from their ``raw`` payload, so this is the exact
        round-trip of the shipped data.  Use :meth:`rebuild` to emit from the
        structured payload fields instead.
        """
        out = bytearray(self.header or self._default_header())
        for r in self.records:
            out.append(r.type)
            if r.type != END_OF_FILE:
                out += r.raw
        return bytes(out)

    def rebuild(self, verify: bool = False) -> bytes:
        """Re-serialise every record from its structured payload.

        The header is preserved verbatim.  For record types whose payload is
        only framed (count + opaque per-record bytes) the stored record bytes
        are emitted; every type whose fields are decoded is re-encoded from
        those fields (see :func:`encode_payload`).  With ``verify`` a record
        whose field encoding differs from the bytes read from disk raises,
        which is how ``tools/re/records.py --check`` detects a modelling bug.
        """
        out = bytearray(self.header or self._default_header())
        for r in self.records:
            out.append(r.type)
            if r.type == END_OF_FILE:
                continue
            data = encode_payload(r.type, r.payload)
            if verify and data != r.raw:
                raise ValueError(
                    f"{r.name} at 0x{r.offset:x}: structural encoding differs "
                    f"from the file ({len(data)} vs {len(r.raw)} bytes)")
            out += data
        return bytes(out)

    def reencode(self) -> int:
        """Replace every ``raw`` with its structural encoding; return the count."""
        n = 0
        for r in self.records:
            if r.type == END_OF_FILE:
                continue
            r.raw = encode_payload(r.type, r.payload)
            r.size = 1 + len(r.raw)
            n += 1
        return n

    def _default_header(self) -> bytes:
        return MAGIC + b"\0" * (80 - len(MAGIC)) + struct.pack("<I", self.version)

    def set_payload(self, rec: Record, payload: object) -> None:
        """Replace a record's payload, re-encoding its raw bytes."""
        rec.payload = payload
        rec.raw = encode_payload(rec.type, payload)
        rec.size = 1 + len(rec.raw)


# --- reader ------------------------------------------------------------------


class _R:
    __slots__ = ("d", "o")

    def __init__(self, data: bytes, off: int) -> None:
        self.d = data
        self.o = off

    def u32(self) -> int:
        if self.o + 4 > len(self.d):
            raise ValueError("unexpected end of file reading u32")
        v = struct.unpack_from("<I", self.d, self.o)[0]
        self.o += 4
        return v

    def u8(self) -> int:
        if self.o + 1 > len(self.d):
            raise ValueError("unexpected end of file reading u8")
        v = self.d[self.o]
        self.o += 1
        return v

    def take(self, n: int, what: str) -> bytes:
        if n < 0 or self.o + n > len(self.d):
            raise ValueError(f"unexpected end of file reading {what}")
        b = self.d[self.o : self.o + n]
        self.o += n
        return b

    def bss(self, n: int) -> tuple[bytes, bool]:
        """Read a NUL-padded name occupying a fixed n-byte slot; return (str, used)."""
        b = self.take(n, "name")
        s = b.split(b"\0", 1)[0].decode("latin1")
        return s, bool(s)


def _count(r: _R, lo: int, hi: int, what: str) -> int:
    n = r.u32()
    if not (lo <= n <= hi):
        raise ValueError(f"{what}: count {n} out of range [{lo}, {hi}]")
    return n


SPRITE_SIZE = 44
SPRITE_FIELDS = (
    (0x00, "scale_x", "f32"),
    (0x04, "scale_y", "f32"),
    (0x08, "runtime_object", "u32"),
    (0x0C, "x", "i32"),
    (0x10, "y", "i32"),
    (0x14, "z", "i32"),
    (0x18, "param", "u8"),
    (0x1C, "flags", "u32"),
    (0x20, "effect_id", "i32"),
    (0x24, "param16", "u16"),
    (0x28, "spawn_time", "u32"),
)


def decode_sprite(raw: bytes) -> dict:
    """Decode one 44-byte sprite record (``0x456130``..``0x456185``)."""
    if len(raw) != SPRITE_SIZE:
        raise ValueError(f"sprite record is {len(raw)} bytes, expected {SPRITE_SIZE}")
    return {
        "scale_x": struct.unpack_from("<f", raw, 0x00)[0],
        "scale_y": struct.unpack_from("<f", raw, 0x04)[0],
        "runtime_object": struct.unpack_from("<I", raw, 0x08)[0],
        "x": struct.unpack_from("<i", raw, 0x0C)[0],
        "y": struct.unpack_from("<i", raw, 0x10)[0],
        "z": struct.unpack_from("<i", raw, 0x14)[0],
        "param": raw[0x18],
        "flags": struct.unpack_from("<I", raw, 0x1C)[0],
        "effect_id": struct.unpack_from("<i", raw, 0x20)[0],
        "param16": struct.unpack_from("<H", raw, 0x24)[0],
        "spawn_time": struct.unpack_from("<I", raw, 0x28)[0],
        "pad": raw[0x19:0x1C] + raw[0x26:0x28],
    }


def _columns(r: _R, n: int, width: int, what: str) -> list[tuple]:
    """Read a column-major handle table: ``width`` arrays of ``n`` u32 each."""
    return [struct.unpack(f"<{n}I", r.take(4 * n, what)) for _ in range(width)]


def _read_record(r: _R, type_: int) -> object:
    if type_ == 1:
        n = _count(r, 0, SWITCH_MAX, "SWITCH")
        return Switch(n, [r.take(32, "switch info") for _ in range(n)])
    if type_ == 2:
        n = _count(r, 0, DOOR_MAX, "DOOR")
        return Door(n, [r.take(184, "door info") for _ in range(n)])
    if type_ == 3:
        exists = r.u8() != 0
        return Thing(exists, r.take(0x528, "thing") if exists else b"")
    if type_ == 4:
        n = _count(r, 0, 0x80, "THING_TYPES")
        out: list[ThingType] = []
        for _ in range(n):
            nraw = r.take(16, "thing type name")
            name = nraw.split(b"\0", 1)[0].decode("latin1")
            params = struct.unpack("<49d", r.take(49 * 8, "thing type params"))
            tail = r.take(936 - 16 - 49 * 8, "thing type tail")
            out.append(ThingType(name, params, tail, nraw))
        return ThingTypes(out)
    if type_ == 5:
        n = _count(r, 0, 0x31, "NAMED_ITEMS")
        out = []
        for _ in range(n):
            nraw = r.take(16, "named item name")
            name = nraw.split(b"\0", 1)[0].decode("latin1")
            out.append(NamedItem(name, struct.unpack("<9I", r.take(36, "named item")), nraw))
        return NamedItems(out)
    if type_ == 6:
        n = _count(r, 0, SPRITE_MAX, "SPRITES")
        records = [r.take(SPRITE_SIZE, "sprites") for _ in range(n)]
        return Sprites(n, records, [decode_sprite(x) for x in records])
    if type_ == 7:
        n = _count(r, 0, SOUND_SOURCE_MAX, "SOUND_SOURCES")
        cols = tuple(struct.unpack(f"<{n}i", r.take(4 * n, "sound sources"))
                     for _ in range(5))
        pitches = struct.unpack(f"<{n}d", r.take(8 * n, "sound source pitch"))
        return SoundSources(n, cols, pitches)
    if type_ == 8:
        raw = r.take(0x5800, "sector stuff")
        return SectorStuff(raw, [SectorSlot.decode(raw[i * 0x58:(i + 1) * 0x58])
                                 for i in range(256)])
    if type_ == 9:
        exists = r.u8() != 0
        return PlayerStatus(exists, r.take(0x3510, "player status") if exists else b"")
    if type_ == 11:
        c = _count(r, 0, 0xC8, "SCRIPT/things")
        things = r.take(c * 40, "script info")
        n = _count(r, 0, 0x400, "SCRIPT/named items")
        handles = r.take(n * 12, "script handle arrays")
        m = _count(r, 0, 0x30000, "SCRIPT/compiled code")
        code = r.take(m, "compiled scripts")
        flag = r.u8()
        tables = r.take(9 * 0x400, "script tables") if flag else b""
        return Script(c, n, m, flag, things, handles, code, tables)
    if type_ == 12:
        counts = struct.unpack("<300I", r.take(0x4B0, "sample counts"))
        namesb = r.take(0xA8C, "sample names")
        names = tuple(
            namesb[i * 9 : i * 9 + 9].split(b"\0", 1)[0].decode("latin1") for i in range(300)
        )
        return SampleTable(counts, names, namesb)
    if type_ == 15:
        n = _count(r, 0, 0x1F3, "POLY_HANDLE")
        return PolyHandles(*_columns(r, n, 4, "poly handle"))
    if type_ == 16:
        n = _count(r, 0, 0x12B, "OBJECT_HANDLE")
        return ObjectHandles(*_columns(r, n, 3, "object handle"))
    if type_ == 17:
        n = _count(r, 0, 0xFF, "SECTOR_HANDLE")
        return SectorHandles(*_columns(r, n, 2, "sector handle"))
    if type_ == 18:
        n = _count(r, 0, LIFT_MAX, "LIFT")
        records = [r.take(0xA8, "lift info") for _ in range(n)]
        k = _count(r, 0, STOP_MAX, "LIFT/total stops")
        cols = tuple(struct.unpack(f"<{k}I", r.take(4 * k, "lift stops"))
                     for _ in range(3))
        return Lift(n, k, records, cols)
    if type_ == 19:
        raw = r.take(0x7080, "item")
        return Item(raw, [ItemSlot.decode(raw[i * 0x60:(i + 1) * 0x60])
                          for i in range(300)])
    if type_ == 20:
        n = _count(r, 0, 0xC8, "SOB_TABLE")
        out = []
        for _ in range(n):
            nraw = r.take(12, "sob name")
            name = nraw.split(b"\0", 1)[0].decode("latin1")
            amount = r.u32()
            ptr = r.u32()  # stale runtime object pointer written at save time
            out.append(SobEntry(name, amount, ptr, nraw))
        return SobTable(out)
    if type_ in UNSUPPORTED:
        raise ValueError(f"record type {type_} is not supported by the loader")
    raise ValueError(f"unknown record type {type_}")


def parse_payload(type_: int, data: bytes) -> object:
    """Decode one payload (without its type byte) and require full consumption.

    Used by ``tools/re/records.py`` to rebuild the save-chain blocks, which
    carry the same payloads as the corresponding ``.sbt`` records.
    """
    r = _R(data, 0)
    payload = _read_record(r, type_)
    if r.o != len(data):
        raise ValueError(
            f"record type {type_}: {len(data) - r.o} trailing payload bytes")
    return payload


def parse(data: bytes) -> Sbt:
    if len(data) < HEADER_SIZE:
        raise ValueError("too short for an .sbt header")
    if not data.startswith(MAGIC):
        raise ValueError(f"bad .sbt magic {data[:12]!r}")
    version = struct.unpack_from("<I", data, 0x50)[0]
    if version != 0:
        # A different version would need the extra header block; refuse rather
        # than misparse.  (The shipped 3dfx/DX files are all version 0.)
        raise ValueError(f"unsupported .sbt version {version}")

    records: list[Record] = []
    r = _R(data, HEADER_SIZE)
    while r.o < len(data):
        off = r.o
        type_ = r.u8()
        if type_ == END_OF_FILE:
            records.append(Record(END_OF_FILE, "END_OF_FILE", off, 1, None, b""))
            break
        p_off = r.o
        payload = _read_record(r, type_)
        records.append(Record(type_, TYPE_NAMES[type_], off, r.o - off,
                              payload, data[p_off : r.o]))

    if not records or records[-1].type != END_OF_FILE:
        raise ValueError("record stream has no END_OF_FILE terminator")
    if r.o != len(data):
        raise ValueError(f"{len(data) - r.o} trailing bytes after END_OF_FILE")
    return Sbt(version, records, HEADER_SIZE, data[:HEADER_SIZE])


# --- writer ------------------------------------------------------------------


def encode_payload(type_: int, payload: object) -> bytes:
    """Re-encode a record payload from its structured form.

    Every supported record type has a writer: types whose layout is proven are
    rebuilt from named fields, types whose per-record fields are not known are
    rebuilt from the count plus the exact record bytes (framing only, no
    normalisation).  :func:`gm.sbt.parse` fills both representations.
    """
    if isinstance(payload, Switch):
        if len(payload.records) != payload.count:
            raise ValueError("SWITCH: count/records mismatch")
        return struct.pack("<I", payload.count) + b"".join(payload.records)
    if isinstance(payload, Door):
        if len(payload.records) != payload.count:
            raise ValueError("DOOR: count/records mismatch")
        return struct.pack("<I", payload.count) + b"".join(payload.records)
    if isinstance(payload, Thing):
        body = payload.raw if payload.exists else b""
        if payload.exists and len(body) != 0x528:
            raise ValueError("THING: record is %d bytes, expected 0x528" % len(body))
        return bytes((1 if payload.exists else 0,)) + body
    if isinstance(payload, HandleTable):
        n = len(payload)
        out = bytearray(struct.pack("<I", n))
        for f in payload._fields:
            out += struct.pack("<%dI" % n, *getattr(payload, f))
        return bytes(out)
    if isinstance(payload, NamedItems):
        out = bytearray(struct.pack("<I", len(payload.entries)))
        for e in payload.entries:
            out += e.name_raw or e.name.encode("latin1")[:16].ljust(16, b"\0")
            out += struct.pack("<9I", *e.params)
        return bytes(out)
    if isinstance(payload, ThingTypes):
        out = bytearray(struct.pack("<I", len(payload.entries)))
        for e in payload.entries:
            out += e.name_raw or e.name.encode("latin1")[:16].ljust(16, b"\0")
            out += struct.pack("<49d", *e.params) + e.tail
        return bytes(out)
    if isinstance(payload, Sprites):
        if len(payload.records) != payload.count:
            raise ValueError("SPRITES: count/records mismatch")
        for rec in payload.records:
            if len(rec) != SPRITE_SIZE:
                raise ValueError("SPRITES: record is %d bytes, expected 44" % len(rec))
        return struct.pack("<I", payload.count) + b"".join(payload.records)
    if isinstance(payload, SoundSources):
        n = payload.count
        if len(payload.columns) != 5 or any(len(c) != n for c in payload.columns):
            raise ValueError("SOUND_SOURCES: expected five columns of %d" % n)
        if len(payload.pitches) != n:
            raise ValueError("SOUND_SOURCES: expected %d pitch values" % n)
        out = bytearray(struct.pack("<I", n))
        for col in payload.columns:
            out += struct.pack("<%di" % n, *col)
        out += struct.pack("<%dd" % n, *payload.pitches)
        return bytes(out)
    if isinstance(payload, SectorStuff):
        if len(payload.slots) == 256:
            return b"".join(s.encode() for s in payload.slots)
        if len(payload.raw) != 0x5800:
            raise ValueError("SECTOR_STUFF: payload is %d bytes, expected 0x5800"
                             % len(payload.raw))
        return payload.raw
    if isinstance(payload, PlayerStatus):
        body = payload.raw if payload.exists else b""
        if payload.exists and len(body) != 0x3510:
            raise ValueError("PLAYER_STATUS: record is %d bytes, expected 0x3510"
                             % len(body))
        return bytes((1 if payload.exists else 0,)) + body
    if isinstance(payload, Script):
        return payload.encode()
    if isinstance(payload, SampleTable):
        if len(payload.counts) != SAMPLE_SLOTS:
            raise ValueError("SOUND_SAMPLE_TABLE: expected 300 counts")
        slots = payload.name_slots
        if len(slots) != 0xA8C:
            if len(payload.names) != SAMPLE_SLOTS:
                raise ValueError("SOUND_SAMPLE_TABLE: expected 300 names")
            slots = b"".join(
                n.encode("latin1")[:8].ljust(9, b"\0") for n in payload.names)
        return struct.pack("<300I", *payload.counts) + slots
    if isinstance(payload, Lift):
        if len(payload.records) != payload.lifts:
            raise ValueError("LIFT: lift count/records mismatch")
        if any(len(r) != 0xA8 for r in payload.records):
            raise ValueError("LIFT: record is not 0xa8 bytes")
        if len(payload.stop_columns) != 3:
            raise ValueError("LIFT: expected three stop columns")
        for col in payload.stop_columns:
            if len(col) != payload.stops:
                raise ValueError("LIFT: stop column length mismatch")
        out = bytearray(struct.pack("<I", payload.lifts))
        out += b"".join(payload.records)
        out += struct.pack("<I", payload.stops)
        for col in payload.stop_columns:
            out += struct.pack("<%dI" % payload.stops, *col)
        return bytes(out)
    if isinstance(payload, Item):
        if len(payload.slots) == 300:
            return b"".join(s.encode() for s in payload.slots)
        if len(payload.raw) != 0x7080:
            raise ValueError("ITEM: payload is %d bytes, expected 0x7080"
                             % len(payload.raw))
        return payload.raw
    if isinstance(payload, SobTable):
        out = bytearray(struct.pack("<I", len(payload.entries)))
        for e in payload.entries:
            out += e.name_raw or e.name.encode("latin1")[:12].ljust(12, b"\0")
            out += struct.pack("<II", e.amount, e.ptr)
        return bytes(out)
    raise ValueError(f"cannot re-encode a {type(payload).__name__} payload")


def build_file(sbt: Sbt, path: str) -> int:
    data = sbt.to_bytes()
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def parse_file(path: str) -> Sbt:
    with open(path, "rb") as f:
        return parse(f.read())
