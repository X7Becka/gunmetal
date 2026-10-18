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
    6  SPRITES              u32 n (<=0x1f3);  n * 44 bytes
    7  SOUND_SOURCES        u32 n (<=0x7f);   5*n*4 + n*8 bytes
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

Types 10 and 13 map to a stub that returns -1 (unsupported); they never occur
in shipped data and are reported as corrupt here.

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
    count: int


@dataclass
class SoundSources:
    count: int


@dataclass
class SectorStuff:
    raw: bytes = b""


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
    counts: tuple = ()  # 300 u32
    names: tuple = ()  # 300 names (<=8 chars + NUL, 9-byte slots)


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
    lifts: int
    stops: int


@dataclass
class Item:
    raw: bytes = b""


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
        """Re-serialise the file (byte-exact round-trip of the shipped data).

        Records are emitted from their stored ``raw`` payload, so an edit is
        made either by writing ``record.raw`` directly or through
        :meth:`set_payload` for the fully-modelled record types.
        """
        out = bytearray(self.header or self._default_header())
        for r in self.records:
            out.append(r.type)
            if r.type != END_OF_FILE:
                out += r.raw
        return bytes(out)

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


def _columns(r: _R, n: int, width: int, what: str) -> list[tuple]:
    """Read a column-major handle table: ``width`` arrays of ``n`` u32 each."""
    return [struct.unpack(f"<{n}I", r.take(4 * n, what)) for _ in range(width)]


def _read_record(r: _R, type_: int) -> object:
    if type_ == 1:
        n = _count(r, 0, 0x200, "SWITCH")
        return Switch(n, [r.take(32, "switch info") for _ in range(n)])
    if type_ == 2:
        n = _count(r, 0, 100, "DOOR")
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
        n = _count(r, 0, 0x1F3, "SPRITES")
        r.take(n * 44, "sprites")
        return Sprites(n)
    if type_ == 7:
        n = _count(r, 0, 0x7F, "SOUND_SOURCES")
        r.take(n * 28, "sound sources")
        return SoundSources(n)
    if type_ == 8:
        return SectorStuff(r.take(0x5800, "sector stuff"))
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
        return SampleTable(counts, names)
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
        n = _count(r, 0, 0x1E, "LIFT")
        r.take(n * 168, "lift info")
        return Lift(n, _count(r, 0, 0xC8, "LIFT/total stops"))
    if type_ == 19:
        return Item(r.take(0x7080, "item"))
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

    Implemented for the fully-modelled record types (handle tables 15/16/17,
    NAMED_ITEMS 5, THING_TYPES 4, SOB_TABLE 20); for every other type the raw
    bytes kept on the payload/record are returned unchanged.
    """
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
    if isinstance(payload, SobTable):
        out = bytearray(struct.pack("<I", len(payload.entries)))
        for e in payload.entries:
            out += e.name_raw or e.name.encode("latin1")[:12].ljust(12, b"\0")
            out += struct.pack("<II", e.amount, e.ptr)
        return bytes(out)
    if isinstance(payload, Script):
        return payload.encode()
    raw = getattr(payload, "raw", None)
    if raw is not None:
        return raw
    raise ValueError(f"cannot re-encode a {type(payload).__name__} payload")


def build_file(sbt: Sbt, path: str) -> int:
    data = sbt.to_bytes()
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def parse_file(path: str) -> Sbt:
    with open(path, "rb") as f:
        return parse(f.read())
