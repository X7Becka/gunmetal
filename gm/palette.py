"""Palette handling.

Gunmetal's 8-bit art does not carry its own palette inside the .sbm blobs.
The engine loads one shared 256-colour table at startup and expands every
indexed texture, sprite and font through it:

* call site ``0x44af88`` pushes ``"frontend"`` (``0x4abbec``), ``"gamepal"``
  (``0x4abbf8``) and the destination ``0x83d320`` into ``0x47efe0`` (the BMP
  loader); the failure path returns the string
  ``"Unable to load game palette (FRONTEND\\GAMEPAL.BMP)"`` (``0x4abbb8``);
* right after the call ``[0x83d2c4] = 0x83d320`` publishes that table to the
  art code, which reads it as ``mov dl,[reg+0x83d320]`` (e.g. ``0x418763``,
  ``0x41eaff``, ``0x41ee8c`` ...).

``FRONTEND/GAMEPAL.BMP`` lives inside the FRONTEND big-file (``BMP.IDX`` /
``BMP.DAT``), so :func:`installed` reads it from there first.

Many loose BMPs in the installation repeat the same VGA table, but the ones
that never use the upper ramps store magenta ``(255,0,255)`` placeholders in
them.  ``FLAG/naUSA.BMP`` differs from the engine palette in exactly 21 slots:

========================  ==========  ========================
index                     naUSA.BMP   GAMEPAL.BMP
========================  ==========  ========================
0                         (0,255,255) (0,0,0)
173 / 174 / 175           magenta     green ramp 203/127/55
191                       magenta     (0,255,0)
240 / 241 / 242           magenta     light ramp 195/143/87
243 .. 246                magenta     red ramp
247 .. 255                magenta     more ramps / white
========================  ==========  ========================

Harvesting the palette from such a carrier turns every bright or glowing
texture magenta (235 of level0's 378 names touch those slots), which is why
this file now prefers the engine's own archive.  ``load_from_bmp()`` is kept
for installations that ship the BMPs without the archive.
"""

from __future__ import annotations

import os

from . import bmp

#: ``(subdirectory, entry name)`` of the palette the engine loads (``0x47efe0``).
ENGINE_PALETTE = ("FRONTEND", "GAMEPAL")

#: Fallback carriers: loose BMPs whose palette table is byte-identical to the
#: engine's *except* for the placeholder slots listed above.
_CANDIDATES = [
    "FLAG/naUSA.BMP",
    "FLAG/aanataka.bmp",
    "PICS/oaktabl1.bmp",
    "PICS/mine-1c.bmp",
]


def load_from_bmp(path: str) -> bytes:
    data = open(path, "rb").read()
    return bmp.parse(data).palette


def from_archive(game_dir: str) -> bytes | None:
    """The engine palette out of ``FRONTEND/BMP.IDX``, or ``None``."""
    from . import bigfile

    sub, want = ENGINE_PALETTE
    idx = os.path.join(game_dir, sub, "BMP.IDX")
    if not os.path.exists(idx):
        return None
    try:
        archive = bigfile.BigFile(idx)
    except (ValueError, OSError):
        return None
    for entry in archive.entries:
        if entry.name.upper() == want:
            return bmp.parse(archive.read(entry)).palette
    return None


def installed(game_dir: str) -> bytes:
    palette = from_archive(game_dir)
    if palette is not None:
        return palette
    for rel in _CANDIDATES:
        p = os.path.join(game_dir, rel)
        if os.path.exists(p):
            return load_from_bmp(p)
    raise FileNotFoundError(
        f"no palette carrier found under {game_dir!r}; pass one explicitly"
    )
