"""Gunmetal ".sbc" blobs — per-level block of small grayscale maps.

Verified layout (fits byte-exactly for all 51 shipped ``*.sbc``, both the 36
files in the game root and the 15 object sets in ``SOBS/``):

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

What the data *is*: **level lightmaps**.  The level loader builds
``"<level>.SBC"`` next to the ``.sbw`` and warns
``Warning, unable to load level lightmaps (SBC file missing).`` when it is
absent (0x442482); the records land in one global array (``[0x5a45cc]``,
8-byte entries: pixel pointer + ``(w-1) | ((h-1) << 8)``, count at
``0x5a45c8``) that the render context hands to the rasteriser (0x4223b9).
``SBC0`` blobs store 2-bit levels (the loader shifts every byte by 2), so
both magics end up on the same 0..127 scale the engine shades with.
Records are stored with a packed row pitch on disk (``w`` bytes per row)
while the engine's buffers use a 256-byte pitch.

Sampling (proven, ``0x43ca5a``..``0x43cc9c``) — see ``gm/render.py``
(``Lightmaps``) for the viewer side::

    u = (dot(u_axis, vertex - centre) * 64 + (int)uv_u_fp) / 2 ; texel = ftol(u) >> 10
    v = (dot(v_axis, vertex - centre) * 64 + (int)uv_v_fp) / 2 ; texel = ftol(v) >> 10
    light = clamp(poly.brightness + object[0x74] + level[0x16]
                  + pixels[(v & (h-1)) * 256 + (u & (w-1))], -128, 127)

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
