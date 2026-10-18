"""Windows BMP reader/writer (8-bit palettised only — that is all Gunmetal uses)."""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class Bmp:
    width: int
    height: int          # positive: bottom-up rows, as stored in the file
    bpp: int
    palette: bytes       # RGBX quads (256 * 4 bytes for 8-bit images)
    pixels: bytes        # top-down, 8-bit indices
    top_down: bool

    def to_png(self) -> bytes:
        from . import png

        return png.from_indexed(self.pixels, self.width, self.height, self.palette)


def parse(data: bytes) -> Bmp:
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    hdr_size = struct.unpack_from("<I", data, 14)[0]
    if hdr_size < 40:
        raise ValueError(f"unsupported BMP header size {hdr_size}")
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    ncol = struct.unpack_from("<I", data, 46)[0] or (1 << bpp if bpp <= 8 else 0)

    top_down = height < 0
    height = abs(height)

    palette = b""
    if bpp == 8:
        raw_pal = data[14 + hdr_size : 14 + hdr_size + ncol * 4].ljust(1024, b"\x00")
        # BMP stores palette entries as B,G,R,X — normalise to R,G,B,X so the
        # rest of the toolkit never has to think about it.
        palette = b"".join(
            raw_pal[i + 2 : i + 3] + raw_pal[i + 1 : i + 2] + raw_pal[i : i + 1] + raw_pal[i + 3 : i + 4]
            for i in range(0, len(raw_pal) - 3, 4)
        )

    if bpp != 8:
        raise ValueError(f"unsupported bpp {bpp}")

    stride = (width + 3) & ~3
    raw = data[off : off + stride * height]
    if len(raw) < stride * height:
        raise ValueError("truncated pixel data")

    rows = [raw[y * stride : y * stride + width] for y in range(height)]
    if not top_down:
        rows.reverse()
    return Bmp(width, height, bpp, palette, b"".join(rows), top_down)


def build_png(data: bytes) -> bytes:
    return parse(data).to_png()
