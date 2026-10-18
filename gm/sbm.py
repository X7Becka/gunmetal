"""Gunmetal ".sbm" 8-bit palettised texture/sprite blobs.

Verified layout (fully reversed, all 1648 PICS entries + 179 sprite entries
decode byte-exactly):

    struct Sbm {
        u32 zero;             // always 0 in shipped data
        u8  log2_width;       // width  = 1 << log2_width
        u8  log2_height;      // height = 1 << log2_height
        u8  levels_minus_one; // mip chain has levels_minus_one + 1 levels
        u8  zero;
        u32 flags;            // 0 or 1 in shipped data
        u8  pixels[];         // mip chain, level 0 first, 8-bit palette indices
    };

Level k has size (width >> k) x (height >> k) and the chain stops at 1x1
(or at the level set by levels_minus_one).  Total pixel bytes therefore equal
sum((w >> k) * (h >> k) for k in range(levels_minus_one + 1)) — this holds for
every entry in the archives, which is how the format was confirmed.

Palette: not stored in the blob.  The game ships a shared VGA-style 256 colour
palette (identical, modulo a few entries, across all .bmp files in the
installation); gm/palette.py can pull it out of any of them.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class Sbm:
    width: int
    height: int
    levels: int
    flags: int
    levels_data: list[bytes]  # level 0 first, 8-bit indices

    @property
    def pixels(self) -> bytes:
        return self.levels_data[0]

    def mip(self, level: int) -> bytes:
        return self.levels_data[level]

    def to_png(self, palette: bytes, transparent_index: int | None = None, level: int = 0) -> bytes:
        from . import png

        if level:
            p = self.mip(level)
            w = max(1, self.width >> level)
            h = max(1, self.height >> level)
        else:
            p, w, h = self.pixels, self.width, self.height
        return png.from_indexed(p, w, h, palette, transparent_index)


def expected_size(log2w: int, log2h: int, levels: int) -> int:
    total = 0
    for k in range(levels + 1):
        total += max(1, 1 << (log2w - k)) * max(1, 1 << (log2h - k))
    return total


def parse(data: bytes) -> Sbm:
    if len(data) < 12:
        raise ValueError("blob too short for an .sbm header")
    zero, log2w, log2h, lvlm1, zero2, flags = struct.unpack_from("<IBBBBI", data, 0)
    if log2w > 12 or log2h > 12:
        raise ValueError(f"implausible dimensions 2**{log2w} x 2**{log2h}")
    if lvlm1 > 12:
        raise ValueError(f"implausible mip count {lvlm1 + 1}")

    want = expected_size(log2w, log2h, lvlm1)
    body = data[12:]
    if len(body) < want:
        raise ValueError(f"truncated mip chain: have {len(body)}, want {want}")

    levels = []
    o = 0
    for k in range(lvlm1 + 1):
        w = max(1, 1 << (log2w - k))
        h = max(1, 1 << (log2h - k))
        levels.append(body[o : o + w * h])
        o += w * h

    return Sbm(
        width=1 << log2w,
        height=1 << log2h,
        levels=lvlm1 + 1,
        flags=flags,
        levels_data=levels,
    )


def build_png(data: bytes, palette: bytes, transparent_index: int | None = None) -> bytes:
    return parse(data).to_png(palette, transparent_index)
