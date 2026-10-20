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


def apng_indexed(frames, width: int, height: int, palette: bytes,
                 fps: int = 24, loops: int = 0) -> bytes:
    """Animated PNG (colour type 3) from 8-bit palette frames.

    Each frame is one full-canvas fcTL/IDAT|fdAT pair; ``fps`` becomes the
    APNG delay fraction ``1/fps`` and ``loops=0`` means loop forever.  Frames
    must all have the same size and palette (HUD films do).
    """
    if not frames:
        raise ValueError("apng: no frames")
    if fps <= 0:
        raise ValueError("apng: fps must be positive")
    ncol = min(256, len(palette) // 4)
    plte = bytearray()
    for i in range(ncol):
        plte += palette[4 * i:4 * i + 3]
    out = bytearray(b"\x89PNG\r\n\x1a\n" + _ihdr(width, height, 8, 3))
    out += _chunk(b"PLTE", bytes(plte))
    out += _chunk(b"acTL", struct.pack(">II", len(frames), loops))
    seq = 0
    for i, pixels in enumerate(frames):
        if len(pixels) != width * height:
            raise ValueError("apng: frame %d has %d pixels, expected %d"
                             % (i, len(pixels), width * height))
        rows = [bytes(pixels[y * width:(y + 1) * width]) for y in range(height)]
        data = _pack(rows)
        fctl = struct.pack(">IIIIIHHBB", seq, width, height, 0, 0, 1, fps, 0, 0)
        out += _chunk(b"fcTL", fctl)
        seq += 1
        if i == 0:
            out += _chunk(b"IDAT", data)
        else:
            out += _chunk(b"fdAT", struct.pack(">I", seq) + data)
            seq += 1
    out += _chunk(b"IEND", b"")
    return bytes(out)


def indexed(
    pixels: bytes | bytearray,
    width: int,
    height: int,
    palette: bytes,
    transparent_index: int | None = None,
) -> bytes:
    """Write an 8-bit **indexed** PNG (colour type 3 + PLTE, optional tRNS).

    Keeping the palette indices (instead of expanding to RGB) makes the result
    round-trip losslessly through :func:`decode`, which matters when a texture is
    edited and written back; the shared VGA palette has duplicate colours.
    """
    assert len(pixels) == width * height, (len(pixels), width * height)
    ncol = min(256, len(palette) // 4)
    plte = bytearray()
    for i in range(ncol):
        plte += palette[4 * i : 4 * i + 3]
    body = _chunk(b"PLTE", bytes(plte))
    if transparent_index is not None and 0 <= transparent_index < ncol:
        trns = bytes(0 if i == transparent_index else 0xFF for i in range(transparent_index + 1))
        body += _chunk(b"tRNS", trns)
    rows = [bytes(pixels[y * width : (y + 1) * width]) for y in range(height)]
    return (b"\x89PNG\r\n\x1a\n" + _ihdr(width, height, 8, 3) + body
            + _chunk(b"IDAT", _pack(rows)) + _chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# reader: only what the toolkit needs to import edited art (8-bit,
# non-interlaced, colour types 0/2/3/4/6).  Round-trips this module's output.
# ---------------------------------------------------------------------------

from dataclasses import dataclass  # noqa: E402


@dataclass
class Image:
    width: int
    height: int
    mode: str                 # "L" | "LA" | "RGB" | "RGBA" | "P"
    channels: int
    pixels: bytes             # raw samples; palette indices when mode == "P"
    palette: bytes = b""      # RGBX quads when mode == "P", else b""


_COLOR = {0: ("L", 1), 4: ("LA", 2), 2: ("RGB", 3), 6: ("RGBA", 4), 3: ("P", 1)}


def decode(data: bytes) -> Image:
    """Decode an 8-bit non-interlaced PNG (colour types 0/2/3/4/6)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    o = 8
    idat = bytearray()
    plte = b""
    ihdr = None
    while o + 8 <= len(data):
        ln = struct.unpack_from(">I", data, o)[0]
        tag = data[o + 4 : o + 8]
        body = data[o + 8 : o + 8 + ln]
        o += 12 + ln
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    if ihdr is None:
        raise ValueError("PNG without IHDR")
    w, h, depth, color, _comp, _filt, inter = ihdr
    if inter:
        raise ValueError("interlaced PNG not supported")
    if depth != 8:
        raise ValueError(f"only 8-bit PNG supported (got depth {depth})")
    if color not in _COLOR:
        raise ValueError(f"unsupported PNG colour type {color}")
    mode, ch = _COLOR[color]
    raw = zlib.decompress(bytes(idat))
    stride = w * ch
    prev = bytearray(stride)
    out = bytearray()
    p = 0
    for _y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p : p + stride])
        p += stride
        if ft == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError(f"bad PNG filter {ft}")
        out += line
        prev = line
    palette = b""
    if mode == "P":
        palette = b"".join(bytes((plte[i], plte[i + 1], plte[i + 2], 0))
                           for i in range(0, len(plte) - 2, 3))
    return Image(w, h, mode, ch, bytes(out), palette)
