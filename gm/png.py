"""Minimal PNG writer (no third-party deps).

Only what the toolkit needs: 8-bit RGB, optional 8-bit indexed with PLTE.
"""

from __future__ import annotations

import struct
import zlib


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def _ihdr(w: int, h: int, depth: int, color: int) -> bytes:
    return _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, 0))


def _pack(rows: list[bytes]) -> bytes:
    return zlib.compress(b"".join(b"\x00" + r for r in rows), 9)


def from_indexed(
    pixels: bytes | bytearray,
    width: int,
    height: int,
    palette: bytes,
    transparent_index: int | None = None,
) -> bytes:
    """Expand 8-bit indexed pixels to 8-bit RGB (or RGBA) PNG."""
    ncol = len(palette) // 4
    if transparent_index is None:
        out = bytearray()
        for y in range(height):
            row = pixels[y * width : (y + 1) * width]
            for idx in row:
                c = palette[idx * 4 : idx * 4 + 3] if idx < ncol else b"\x00\x00\x00"
                out += c
        return rgb(width, height, bytes(out))

    out = bytearray()
    for y in range(height):
        row = pixels[y * width : (y + 1) * width]
        for idx in row:
            if idx == transparent_index:
                out += b"\x00\x00\x00\x00"
            else:
                c = palette[idx * 4 : idx * 4 + 3] if idx < ncol else b"\x00\x00\x00"
                out += c + b"\xff"
    return rgba(width, height, bytes(out))


def rgb(width: int, height: int, pixels: bytes) -> bytes:
    assert len(pixels) == width * height * 3, (len(pixels), width * height * 3)
    rows = [pixels[y * width * 3 : (y + 1) * width * 3] for y in range(height)]
    return b"\x89PNG\r\n\x1a\n" + _ihdr(width, height, 8, 2) + _chunk(b"IDAT", _pack(rows)) + _chunk(b"IEND", b"")


def rgba(width: int, height: int, pixels: bytes) -> bytes:
    assert len(pixels) == width * height * 4, (len(pixels), width * height * 4)
    rows = [pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height)]
    return b"\x89PNG\r\n\x1a\n" + _ihdr(width, height, 8, 6) + _chunk(b"IDAT", _pack(rows)) + _chunk(b"IEND", b"")


def gray(width: int, height: int, pixels: bytes) -> bytes:
    assert len(pixels) == width * height
    rows = [pixels[y * width : (y + 1) * width] for y in range(height)]
    return b"\x89PNG\r\n\x1a\n" + _ihdr(width, height, 8, 0) + _chunk(b"IDAT", _pack(rows)) + _chunk(b"IEND", b"")
