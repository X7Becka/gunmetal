"""Gunmetal ".sbc" blobs — per-level block of small grayscale maps.

Verified layout (fits byte-exactly for all 51 shipped ``*.sbc``, both the 34
files in the game root and the 17 object sets in ``SOBS/``):

    struct Sbc {
        char magic[4];        // "SBC0" or "SBC1"
        u32  count;           // number of records
        struct {
            u32 desc;         // w_log = desc & 0xffff, h_log = desc >> 16
            u8  pixels[];     // (1 << w_log) * (1 << h_log) bytes, 8-bit
        } records[count];
    };

The records are stored back to back; ``sum(4 + 2**w * 2**h) + 8`` equals the
file size exactly for every shipped file, which is how the layout was pinned
down.

What the data *is*: each record is a small rectangular 8-bit map with arbitrary
power-of-two sides (``2x4``, ``4x8``, ``64x8``, ``128x2``, ``128x128``); the
**values all lie in the range 0..127** and are very smooth (mean horizontal
gradient ≈ 2.4–4.5 on the 0..127 scale).  That points at lighting/lightmap
data — the engine logs a ``Lightmap mem`` budget — rather than textures.
``gm.py sbc`` dumps them as grayscale PNG so the guess can be checked by eye.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGICS = (b"SBC0", b"SBC1")


@dataclass
class Record:
    width: int
    height: int
    pixels: bytes

    def to_png(self) -> bytes:
        from . import png

        return png.gray(self.width, self.height, self.pixels)


@dataclass
class Sbc:
    magic: bytes
    records: list[Record] = field(default_factory=list)

    @property
    def count(self) -> int:
        return len(self.records)

    def to_bytes(self) -> bytes:
        """Re-serialise the blob (byte-exact round-trip of the shipped data)."""
        out = bytearray(self.magic)
        out += struct.pack("<I", len(self.records))
        for r in self.records:
            desc = (r.width.bit_length() - 1) | ((r.height.bit_length() - 1) << 16)
            out += struct.pack("<I", desc) + r.pixels
        return bytes(out)


def encode(records: list[Record], magic: bytes = b"SBC0") -> bytes:
    """Build an ``.sbc`` blob from grayscale records."""
    if magic not in MAGICS:
        raise ValueError(f"magic must be one of {MAGICS!r}")
    return Sbc(bytes(magic), list(records)).to_bytes()


def parse(data: bytes) -> Sbc:
    if len(data) < 8:
        raise ValueError("blob too short for an .sbc header")
    if data[:4] not in MAGICS:
        raise ValueError(f"bad .sbc magic {data[:4]!r}")

    count = struct.unpack_from("<I", data, 4)[0]
    off = 8
    records: list[Record] = []
    for i in range(count):
        if off + 4 > len(data):
            raise ValueError(f"truncated descriptor table at record {i}")
        desc = struct.unpack_from("<I", data, off)[0]
        off += 4
        w_log = desc & 0xFFFF
        h_log = desc >> 16
        if w_log > 16 or h_log > 16:
            raise ValueError(f"record {i}: implausible size 2**{w_log} x 2**{h_log}")
        w = 1 << w_log
        h = 1 << h_log
        size = w * h
        if off + size > len(data):
            raise ValueError(f"record {i}: truncated ({off + size} > {len(data)})")
        records.append(Record(w, h, data[off : off + size]))
        off += size

    if off != len(data):
        raise ValueError(f"{len(data) - off} trailing bytes after {count} records")
    return Sbc(data[:4], records)


def parse_file(path: str) -> Sbc:
    with open(path, "rb") as f:
        return parse(f.read())
