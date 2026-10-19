"""Gunmetal ``.sof`` / ``.fdx`` — HUD films (research stage).

``.sof`` has no header: it is a flat stream of 8192-byte tiles, each holding one
128x64 frame stored transposed (a 64-wide, 128-tall block).  The reader in the
exe seeks to ``frame * 0x2000`` (``0x49fd40``: ``shl eax, 0xd`` then the seek
wrapper) and reads 0x2000 bytes at a time, so a file's tile count is
``size / 8192``; all 48 shipped ``.sof`` files divide exactly.

``.fdx`` (and the older ``Film/*.idx``) are text: lines ``%ld,%ld`` parsed by
``0x498e20`` as ``(count, fps)``; the second value must be positive or the game
logs ``invalid FPS for film %s``.  The exact meaning of the first value is not
proven yet (it is close to, but not always, the tile count).

The tile layout is confirmed visually: transposing the 64x128 source into
128x64 renders the ammo/weapon HUD icons correctly.
"""

from __future__ import annotations

import os

TILE_BYTES = 0x2000
TILE_W = 128
TILE_H = 64


def tile_count(path: str) -> tuple[int, int]:
    """Return ``(tiles, size)``; *tiles* is exact only when ``size % 8192 == 0``."""
    size = os.path.getsize(path)
    return size // TILE_BYTES, size


def frame(path: str, index: int) -> bytes:
    """Return tile *index* as 128x64 palette indices (source is transposed)."""
    if index < 0:
        raise ValueError("frame index must be >= 0")
    with open(path, "rb") as fh:
        fh.seek(index * TILE_BYTES)
        tile = fh.read(TILE_BYTES)
    if len(tile) != TILE_BYTES:
        raise ValueError("frame %d is past the end of %s" % (index, path))
    out = bytearray(TILE_BYTES)
    for y in range(TILE_H):
        for x in range(TILE_W):
            out[y * TILE_W + x] = tile[x * TILE_H + y]
    return bytes(out)


def parse_fdx(path: str) -> list[dict]:
    """Parse ``.fdx`` / ``Film/*.idx`` lines ``count,fps``."""
    out = []
    for line in open(path, "r", encoding="latin1").read().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 2:
            raise ValueError("%s: expected 'count,fps', got %r" % (path, line))
        out.append({"count": int(parts[0]), "fps": int(parts[1])})
    return out


def find_fdx(sof_path: str) -> str | None:
    """Guess the sibling index: ``WEAPON1.SOF`` -> ``weapon.fdx``."""
    stem = os.path.splitext(os.path.basename(sof_path))[0]
    letters = "".join(c for c in stem if c.isalpha()).lower()
    folder = os.path.dirname(os.path.abspath(sof_path))
    for name in (letters + ".fdx", letters + ".idx"):
        path = os.path.join(folder, name)
        if os.path.exists(path):
            return path
    return None
