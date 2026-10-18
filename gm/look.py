"""Gunmetal lookup tables ``LOOK16.TAB`` / ``LOOK162.TAB``.

Both files are exactly ``262144`` bytes = ``64**3``: a 6-6-6 RGB cube stored as
``data[(r << 12) | (g << 6) | b]`` with one byte per entry (``r``/``g``/``b`` in
``0..63``).  The engine generates them when missing and otherwise loads them
(``gundx.exe`` VMA ``0x41e771`` = table 1, ``0x41e8ec`` = table 2; the loops
walk ``r``, ``g``, ``b`` over ``0x40`` values and write one byte each).

Each byte is a **palette index**: for a colour ``(r,g,b)`` the entry is the
nearest palette colour, found by the engine's weighted-distance search at
``0x4228e0``.  The two files use different palettes:

* ``LOOK16.TAB`` — the default game palette (working copy at ``0x5309b8``,
  built from the master palette at ``0x83d320``; slot remap at ``0x5620c0``).
  Values are palette slots (shipped file: 1..239, 234 distinct).
* ``LOOK162.TAB`` — the palette selected by ``NEONS.PIN`` (working copy at
  ``0x530cb8``, remap at ``0x5641c0``).  ``NEONS.PIN`` is a text list
  ``count`` + ``count`` palette indices; the shipped one lists 20 indices
  ``{173,174,175,191,240..255}`` — exactly the value set of ``LOOK162.TAB``.

The engine note "Generating 16-bit table N..." refers to this regeneration: on
first run the tables are computed (slowly, printing progress) and written so
later runs just read them.  ``apply8`` maps an ordinary 8-bit RGB colour to its
palette index through the cube.
"""

from __future__ import annotations

import os
from dataclasses import dataclass

DIM = 64
SIZE = DIM ** 3                      # 262144
_MONTAGE = 8                          # 8x8 tiles of 64x64 -> 512x512


@dataclass
class LookTable:
    """A 64x64x64 byte cube indexed by 6-bit ``(r, g, b)``."""

    data: bytes
    name: str = ""

    def __post_init__(self) -> None:
        if len(self.data) != SIZE:
            raise ValueError(f"{self.name or 'table'}: expected {SIZE} bytes, got {len(self.data)}")

    @property
    def dim(self) -> int:
        return DIM

    def at(self, r: int, g: int, b: int) -> int:
        """Entry for the 6-bit colour (r, g, b); each component 0..63."""
        return self.data[(r << 12) | (g << 6) | b]

    def apply8(self, r: int, g: int, b: int) -> int:
        """Entry for an 8-bit RGB colour (components are shifted right by 2)."""
        return self.data[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)]

    def slice(self, r: int) -> bytes:
        """The 64x64 (g, b) slice for red level *r*."""
        base = r << 12
        return self.data[base : base + (DIM << 6)]

    def distinct(self) -> list[int]:
        return sorted(set(self.data))

    def stats(self) -> dict:
        d = self.distinct()
        return {
            "size": len(self.data),
            "dim": DIM,
            "distinct": len(d),
            "min": d[0],
            "max": d[-1],
            "values": d,
        }

    def to_png(self, path: str, palette: bytes | None = None, scale: int = 1) -> tuple[int, int]:
        """Write the cube as a montage of the 64 red slices (8x8 tiles).

        Each tile is the 64x64 (g, b) plane of one red level, so the montage is
        512x512 (times *scale*).  *palette* is the 1024-byte RGBX palette used
        to colourise the indices; without it a grey ramp is used.
        """
        from . import png

        if palette is None:
            palette = bytes(v for i in range(256) for v in (i, i, i, 0))
        s = max(1, scale)
        dim = DIM * _MONTAGE
        rows: list[bytes] = []
        for y in range(dim):
            ty, gy = divmod(y, DIM)
            row = bytearray()
            for tx in range(_MONTAGE):
                r = ty * _MONTAGE + tx
                chunk = self.slice(r)[gy * DIM : gy * DIM + DIM]
                if s > 1:
                    for px in chunk:
                        row += bytes((px,)) * s
                else:
                    row += chunk
            rows.append(bytes(row))
        if s > 1:
            expanded: list[bytes] = []
            for r in rows:
                expanded.extend([r] * s)
            rows = expanded
        data = png.from_indexed(b"".join(rows), dim * s, dim * s, palette)
        with open(path, "wb") as f:
            f.write(data)
        return dim * s, dim * s


def load(path: str) -> LookTable:
    with open(path, "rb") as f:
        return LookTable(f.read(), os.path.basename(path))


def parse_neons(path: str) -> list[int]:
    """Parse a ``NEONS.PIN`` text list: an integer count then that many indices."""
    with open(path, "r", encoding="latin1") as f:
        toks = f.read().split()
    if not toks:
        return []
    n = int(toks[0])
    return [int(t) for t in toks[1 : 1 + n]]


def write_png(table: LookTable, path: str, palette: bytes | None = None, scale: int = 1) -> tuple[int, int]:
    return table.to_png(path, palette, scale)
