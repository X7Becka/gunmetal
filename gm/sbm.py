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
    extra: bytes = b""        # any bytes after the mip chain (kept verbatim)

    @property
    def pixels(self) -> bytes:
        return self.levels_data[0]

    def mip(self, level: int) -> bytes:
        return self.levels_data[level]

    def to_bytes(self) -> bytes:
        """Re-serialise exactly as read (round-trips the shipped blobs)."""
        log2w = self.width.bit_length() - 1
        log2h = self.height.bit_length() - 1
        header = struct.pack("<IBBBBI", 0, log2w, log2h, len(self.levels_data) - 1,
                             0, self.flags)
        return header + b"".join(self.levels_data) + self.extra

    def to_png(self, palette: bytes, transparent_index: int | None = None, level: int = 0) -> bytes:
        from . import png

        if level:
            p = self.mip(level)
            w = max(1, self.width >> level)
            h = max(1, self.height >> level)
        else:
            p, w, h = self.pixels, self.width, self.height
        return png.indexed(p, w, h, palette, transparent_index)


def _downsample(pix: bytes, w: int, h: int, mip: str) -> bytes:
    """Halve an 8-bit image.  ``box`` averages 2x2, ``nearest`` samples the top-left."""
    nw, nh = max(1, w // 2), max(1, h // 2)
    if mip == "nearest" or w < 2 or h < 2:
        return b"".join(
            pix[(y * 2) * w : (y * 2) * w + 2 * nw : 2] for y in range(nh)
        )
    out = bytearray(nw * nh)
    for y in range(nh):
        r0 = (y * 2) * w
        r1 = (y * 2 + 1) * w
        for x in range(nw):
            out[y * nw + x] = (pix[r0 + 2 * x] + pix[r0 + 2 * x + 1]
                               + pix[r1 + 2 * x] + pix[r1 + 2 * x + 1] + 2) >> 2
    return bytes(out)


def encode(width: int, height: int, pixels: bytes, levels: int = 1,
           flags: int = 0, mip: str = "box") -> bytes:
    """Build an ``.sbm`` blob from level-0 pixel indices.

    *width*/*height* must be powers of two; *levels* is the number of mip levels
    to store (1 = none).  ``box`` (2x2 average) or ``nearest`` controls the
    filter used to derive lower levels.
    """
    if width & (width - 1) or height & (height - 1) or width < 1 or height < 1:
        raise ValueError(f"width/height must be powers of two: {width}x{height}")
    if len(pixels) != width * height:
        raise ValueError(f"pixel count {len(pixels)} != {width}x{height}")
    if levels < 1 or levels > width.bit_length():
        raise ValueError(f"bad level count {levels}")
    chain = [pixels]
    w, h = width, height
    for _ in range(1, levels):
        chain.append(_downsample(chain[-1], w, h, mip))
        w, h = max(1, w // 2), max(1, h // 2)
    log2w = width.bit_length() - 1
    log2h = height.bit_length() - 1
    header = struct.pack("<IBBBBI", 0, log2w, log2h, levels - 1, 0, flags)
    return header + b"".join(chain)


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
        extra=body[o:],
    )


def build_png(data: bytes, palette: bytes, transparent_index: int | None = None) -> bytes:
    return parse(data).to_png(palette, transparent_index)
