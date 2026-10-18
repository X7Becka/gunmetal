"""Palette handling.

Gunmetal's 8-bit art does not carry its own palette inside the .sbm blobs.
Instead the installation ships a VGA-style 256 colour palette that is repeated
(byte-identical) inside almost every .bmp file, so we simply harvest it from a
BMP that is known to contain it.

`load_from_bmp()` reads such a file; `installed()` looks for the usual
candidates inside a Gunmetal installation directory.
"""

from __future__ import annotations

import os

from . import bmp

# The BMPs the game itself loads (flags / level art) all carry the same table.
_CANDIDATES = [
    "FLAG/naUSA.BMP",
    "FLAG/aanataka.bmp",
    "PICS/oaktabl1.bmp",
    "PICS/mine-1c.bmp",
]


def load_from_bmp(path: str) -> bytes:
    data = open(path, "rb").read()
    return bmp.parse(data).palette


def installed(game_dir: str) -> bytes:
    for rel in _CANDIDATES:
        p = os.path.join(game_dir, rel)
        if os.path.exists(p):
            return load_from_bmp(p)
    raise FileNotFoundError(
        f"no palette carrier found under {game_dir!r}; pass one explicitly"
    )
