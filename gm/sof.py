"""Gunmetal ``.sof`` / ``.fdx`` — HUD films (proof-carrying reader/writer).

A ``.sof`` is a *headerless* stream of 8192-byte tiles; every tile is one
128x64 8-bit paletted frame stored **transposed** (the file keeps the image as
64 wide x 128 tall).  The reader at ``0x49fd40`` computes ``offset = frame << 13``
and the loader at ``0x49fbc0`` reads exactly ``0x2000`` bytes per frame, so::

    tiles = size // 0x2000           (all 48 shipped files divide exactly)
    src[x*64 + y]  ->  out[y*128 + x]        (mode 0, the only mode used)

The transposed layout is not a guess: all 30 ``Film/Kflm*.bmp`` files are
byte-identical to frames 0..29 of ``Kelfilm.sof`` after this transform (see
``docs/FILM.md``).

``.fdx`` (and the legacy ``Film/*.idx``) is text, one ``"%ld,%ld"`` record per
line (parser ``0x498e20``, format string ``0x4beac0``), i.e. ``count, fps``.
The line index is the numeric suffix of the film name: the HUD loader
``0x4958d0`` formats ``"%s%ld"`` from (prefix, id) *and* passes the same id to
the index parser, so ``WEAPON8.SOF`` uses line 8 of ``weapon.fdx``.  ``fps <= 0``
is rejected by the engine ("invalid FPS for film %s"); ``count`` is the frame
count (exclusive bound, frames ``0 .. count-1``).  The scripted loader
``0x44da80`` takes the same ``(name, count, ms, start, loop, clock)`` arguments
straight from the ``FILM_DEFINE`` opcode (``0xb3``), where ``count`` always
equals the tile count (``comms`` 1755, ``kelfilm`` 30, ``slasher`` 220,
``level12`` 361, ...).

Frame timing is in engine ticks, not milliseconds: the HUD caller computes
``ms = 91 // fps`` (constant ``0x5b`` at ``0x49594a``) and the clock global is
advanced by ``91 // savefilm`` per rendered frame (``0x44d4f0``, default 15).
The film is drawn at screen row 125 with the screen pitch as the destination
stride (``0x495a23``), i.e. a 128x64 overlay near the bottom of the screen.

Layout quirks are preserved: a partial trailing tile (if any) is kept verbatim
in :attr:`Sof.tail` and re-emitted, so ``to_bytes()`` round-trips every shipped
file byte-exactly.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

TILE_BYTES = 0x2000
TILE_W = 128
TILE_H = 64
SRC_W = 64
SRC_H = 128

#: Safety limits (docs/SECURITY.md policy: bound counters before allocating).
MAX_TILES = 1 << 20              # 8 GiB of film data, far above the 90.9 MiB shipped
MAX_FDX_LINES = 1 << 12          # 4096 lines, shipped max is 24
MAX_COUNT = 1 << 20              # a single film cannot exceed MAX_TILES frames
MAX_FPS = 1000                   # engine rejects fps <= 0; cap the sane upper end

#: Film families and the string each caller passes as the name prefix.
#: Proof: ``0x4958d0`` is called with "AMMO" (0x4be724), "WEAPON" (0x4be894),
#: "TANK" (0x4be7e8), "SHIELD" (0x4be7e0), "EQUIP" (0x4be778) and "TEMP"
#: (0x4be7f0); "DEFAULT" (0x4bb344) is the fallback film of the same loader.
PREFIXES = {
    "weapon": "WEAPON",
    "ammo": "AMMO",
    "tank": "TANK",
    "shield": "SHIELD",
    "equip": "EQUIP",
    "default": "DEFAULT",
}

#: BMPs that carry the game's shared 256-colour VGA palette *with* the neon
#: slots resolved (47 BMPs share one byte-identical table, the 30 Kflm dumps
#: among them; ``Film/Kflm0000`` is a dumped film frame, so this is the exact
#: table the films are indexed by).
PALETTE_CARRIERS = (
    "Film/Kflm0000.bmp",
    "PICS/mine-1c.bmp",
    "PICS/mine-1d.bmp",
)

_FDX_RE = re.compile(rb"^[ \t]*([0-9]+)[ \t]*,[ \t]*([0-9]+)[ \t]*$")


# --------------------------------------------------------------- tile codec
def decode_tile(tile: bytes) -> bytes:
    """Return tile bytes as 128x64 palette indices (undo the transpose)."""
    if len(tile) != TILE_BYTES:
        raise ValueError("tile must be %d bytes, got %d" % (TILE_BYTES, len(tile)))
    out = bytearray(TILE_BYTES)
    for x in range(TILE_W):                       # x = 0..127 (source row)
        out[x::TILE_W] = tile[x * SRC_W:(x + 1) * SRC_W]
    return bytes(out)


def encode_tile(pixels: bytes) -> bytes:
    """Inverse of :func:`decode_tile`: 128x64 indices -> stored 64x128 tile."""
    if len(pixels) != TILE_BYTES:
        raise ValueError("frame must be %d bytes, got %d" % (TILE_BYTES, len(pixels)))
    tile = bytearray(TILE_BYTES)
    for y in range(TILE_H):
        tile[y::SRC_W] = pixels[y * TILE_W:(y + 1) * TILE_W]
    return bytes(tile)


# ---------------------------------------------------------------------- .sof
@dataclass
class Sof:
    """A parsed ``.sof`` film: a flat list of 8 KB tiles."""

    tiles: list[bytes] = field(default_factory=list)
    tail: bytes = b""            # bytes after the last whole tile (usually empty)
    path: str | None = None

    def __len__(self) -> int:
        return len(self.tiles)

    @property
    def byte_size(self) -> int:
        return len(self.tiles) * TILE_BYTES + len(self.tail)

    @property
    def aligned(self) -> bool:
        """True when the file is a whole number of 8 KB tiles."""
        return not self.tail

    def tile(self, index: int) -> bytes:
        """Raw stored tile (64x128, transposed)."""
        if not 0 <= index < len(self.tiles):
            raise ValueError("tile %d out of range 0..%d"
                             % (index, len(self.tiles) - 1))
        return self.tiles[index]

    def frame(self, index: int) -> bytes:
        """One decoded 128x64 frame (palette indices)."""
        return decode_tile(self.tile(index))

    def frames(self) -> list[bytes]:
        """All decoded frames; 48 films / 90.9 MiB is a few seconds of work."""
        return [decode_tile(t) for t in self.tiles]

    def raw_frames(self) -> list[bytes]:
        """All stored tiles, still transposed."""
        return list(self.tiles)

    def to_bytes(self) -> bytes:
        """Serialise back; byte-exact for every shipped film."""
        return b"".join(self.tiles) + self.tail

    def to_png(self, palette: bytes, index: int = 0) -> bytes:
        """Encode one decoded frame as a 128x64 indexed PNG."""
        from . import png

        return png.indexed(self.frame(index), TILE_W, TILE_H, palette)


def parse(data: bytes, strict: bool = False) -> Sof:
    """Parse ``.sof`` bytes.

    ``strict`` rejects a size that is not a multiple of 8192; the default keeps
    the trailing bytes in :attr:`Sof.tail` (the engine's ``fread`` would return
    a short tile and reuse the previous buffer).
    """
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("sof data must be bytes")
    data = bytes(data)
    if len(data) % TILE_BYTES and strict:
        raise ValueError(".sof size %d is not a multiple of %d"
                         % (len(data), TILE_BYTES))
    n = len(data) // TILE_BYTES
    if n > MAX_TILES:
        raise ValueError(".sof has %d tiles, limit is %d" % (n, MAX_TILES))
    tiles = [data[i * TILE_BYTES:(i + 1) * TILE_BYTES] for i in range(n)]
    return Sof(tiles=tiles, tail=data[n * TILE_BYTES:])


def parse_file(path: str) -> Sof:
    with open(path, "rb") as fh:
        sof = parse(fh.read())
    sof.path = path
    return sof


# Backwards-compatible helpers used by ``gm.py film``.
def tile_count(path: str) -> tuple[int, int]:
    """Return ``(tiles, size)``; *tiles* is exact only when ``size % 8192 == 0``."""
    size = os.path.getsize(path)
    return size // TILE_BYTES, size


def frame(path: str, index: int) -> bytes:
    """Return tile *index* as 128x64 palette indices (source is transposed)."""
    if index < 0:
        raise ValueError("frame index must be >= 0")
    size = os.path.getsize(path)
    if (index + 1) * TILE_BYTES > size:
        raise ValueError("frame %d is past the end of %s (%d tiles)"
                         % (index, path, size // TILE_BYTES))
    with open(path, "rb") as fh:
        fh.seek(index * TILE_BYTES)
        tile = fh.read(TILE_BYTES)
    if len(tile) != TILE_BYTES:
        raise ValueError("short read for frame %d of %s" % (index, path))
    return decode_tile(tile)


def frames(path: str) -> list[bytes]:
    """Decode every frame of *path*."""
    return parse_file(path).frames()


# ---------------------------------------------------------------------- .fdx
@dataclass
class FdxEntry:
    """One ``count, fps`` record; ``text``/``eol`` keep the original bytes."""

    count: int
    fps: int
    text: bytes = b""
    eol: bytes = b"\r\n"
    line: int = 0                 # 0-based line in :attr:`Fdx.lines`

    @property
    def canonical(self) -> bytes:
        return b"%d, %d" % (self.count, self.fps)

    def to_bytes(self) -> bytes:
        return (self.text or self.canonical) + self.eol


@dataclass
class Fdx:
    """A parsed ``.fdx`` / ``.idx`` index (one record per selectable id)."""

    lines: list[bytes] = field(default_factory=list)   # raw lines incl. EOL
    entries: list[FdxEntry] = field(default_factory=list)
    path: str | None = None

    def __len__(self) -> int:
        return len(self.entries)

    def __getitem__(self, index: int) -> FdxEntry:
        if not 0 <= index < len(self.entries):
            raise IndexError("fdx line %d out of range 0..%d"
                             % (index, len(self.entries) - 1))
        return self.entries[index]

    def lookup(self, index: int) -> FdxEntry:
        """The record used for film ``<PREFIX><index>`` (engine returns -1)."""
        return self[index]

    def to_bytes(self) -> bytes:
        """Serialise back; byte-exact for every shipped ``.fdx`` / ``.idx``."""
        out = list(self.lines)
        for entry in self.entries:
            out[entry.line] = entry.to_bytes()
        return b"".join(out)


def parse_fdx_bytes(data: bytes, name: str = "") -> Fdx:
    """Parse ``.fdx`` bytes.  Bad records raise :class:`ValueError`."""
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("fdx data must be bytes")
    raw = bytes(data)
    # Split into (content, eol) chunks, preserving every byte.
    chunks = []
    start = 0
    for i, byte in enumerate(raw):
        if byte == 0x0A:                          # LF (also ends CRLF)
            body = raw[start:i]
            eol = b"\n"
            if body.endswith(b"\r"):
                body, eol = body[:-1], b"\r\n"
            chunks.append((body, eol))
            start = i + 1
    if start < len(raw):                           # last line without newline
        chunks.append((raw[start:], b""))
    if len(chunks) > MAX_FDX_LINES:
        raise ValueError("%s: %d lines, limit is %d"
                         % (name or "<fdx>", len(chunks), MAX_FDX_LINES))

    lines = [body + eol for body, eol in chunks]
    entries = []
    for lineno, (body, eol) in enumerate(chunks):
        if not body.strip():
            continue                               # blank line: preserved only
        m = _FDX_RE.match(body)
        if not m:
            raise ValueError("%s:%d: expected 'count,fps', got %r"
                             % (name or "<fdx>", lineno + 1, body.decode("latin1")))
        count, fps = int(m.group(1)), int(m.group(2))
        if count > MAX_COUNT:
            raise ValueError("%s:%d: count %d exceeds limit %d"
                             % (name or "<fdx>", lineno + 1, count, MAX_COUNT))
        if fps <= 0:
            # 0x498f30: "invalid FPS for film %s" -> the engine refuses it too.
            raise ValueError("%s:%d: invalid FPS %d for film"
                             % (name or "<fdx>", lineno + 1, fps))
        if fps > MAX_FPS:
            raise ValueError("%s:%d: fps %d exceeds limit %d"
                             % (name or "<fdx>", lineno + 1, fps, MAX_FPS))
        entries.append(FdxEntry(count=count, fps=fps, text=body, eol=eol,
                                line=lineno))
    return Fdx(lines=lines, entries=entries)


def parse_fdx(path: str) -> list[dict]:
    """Parse ``.fdx`` / ``Film/*.idx`` into ``[{"count", "fps"}, ...]``.

    Kept as the ``gm.py film`` interface; use :func:`load_fdx` for the object
    with a byte-exact ``to_bytes()``.
    """
    fdx = load_fdx(path)
    return [{"count": e.count, "fps": e.fps} for e in fdx.entries]


def load_fdx(path: str) -> Fdx:
    with open(path, "rb") as fh:
        fdx = parse_fdx_bytes(fh.read(), path)
    fdx.path = path
    return fdx


def build_fdx(entries: list[tuple[int, int]], eol: bytes = b"\r\n") -> bytes:
    """Canonical writer: ``"%d, %d"`` records, one per line."""
    out = bytearray()
    for count, fps in entries:
        if count < 0 or count > MAX_COUNT:
            raise ValueError("count %d out of range 0..%d" % (count, MAX_COUNT))
        if not 0 < fps <= MAX_FPS:
            raise ValueError("fps %d out of range 1..%d" % (fps, MAX_FPS))
        out += b"%d, %d" % (count, fps) + eol
    return bytes(out)


def find_fdx(sof_path: str) -> str | None:
    """Sibling index of a film: ``WEAPON1.SOF`` -> ``weapon.fdx``."""
    stem = os.path.splitext(os.path.basename(sof_path))[0]
    letters = "".join(c for c in stem if c.isalpha()).lower()
    folder = os.path.dirname(os.path.abspath(sof_path))
    for name in (letters + ".fdx", letters + ".idx"):
        path = os.path.join(folder, name)
        if os.path.exists(path):
            return path
    return None


# ------------------------------------------------------------------ binding
def film_id(path: str) -> tuple[str, int | None]:
    """``WEAPON8.SOF`` -> ``("weapon", 8)``; letters are lower-cased."""
    stem = os.path.splitext(os.path.basename(path))[0]
    prefix = "".join(c for c in stem if c.isalpha()).lower()
    digits = "".join(c for c in stem if c.isdigit())
    return prefix, (int(digits) if digits else None)


def binding(path: str) -> dict:
    """Describe a shipped film: family, id, index record, frame counts.

    ``fdx`` is the sibling ``.fdx``/``.idx`` when one exists; ``index`` is the
    numeric suffix (the line the engine reads).  ``extra_tiles`` is
    ``tiles - count``: the shipped HUD films keep one unused trailing tile,
    while the scripted films define exactly as many frames as they have tiles.
    """
    prefix, ident = film_id(path)
    if ident is None and prefix == "default":
        ident = 0            # 0x49597c: "DEFAULT" is loaded with index 0
    tiles, size = tile_count(path)
    out = {
        "file": os.path.basename(path),
        "prefix": prefix,
        "family": PREFIXES.get(prefix),     # None for scripted/orphan films
        "id": ident,
        "tiles": tiles,
        "bytes": size,
        "aligned": size % TILE_BYTES == 0,
        "fdx": None,
        "line": None,
        "count": None,
        "fps": None,
        "extra_tiles": None,
    }
    fdx_path = find_fdx(path)
    if fdx_path and ident is not None:
        fdx = load_fdx(fdx_path)
        out["fdx"] = fdx_path
        out["line"] = ident
        if ident < len(fdx.entries):
            out["count"] = fdx.entries[ident].count
            out["fps"] = fdx.entries[ident].fps
            out["extra_tiles"] = tiles - fdx.entries[ident].count
    return out


# ------------------------------------------------------------------ palette
def load_palette(game_dir: str) -> bytes:
    """Return the films' 256-colour palette (``R,G,B,X``) from a BMP carrier.

    The table is the game's shared VGA palette; the film-dumped BMPs carry the
    same table with the 20 ``NEONS.PIN`` slots substituted (236/256 entries are
    identical to the ``FLAG/*.bmp`` palette used by ``gm.palette``).
    """
    from . import bmp

    for rel in PALETTE_CARRIERS:
        path = os.path.join(game_dir, rel.replace("/", os.sep))
        if os.path.exists(path):
            with open(path, "rb") as fh:
                return bmp.parse(fh.read()).palette
    raise FileNotFoundError(
        "no film palette carrier found under %r; pass a BMP explicitly"
        % game_dir)


def to_png(src, palette: bytes, index: int = 0) -> bytes:
    """Encode one frame as a 128x64 indexed PNG.

    *src* is a ``.sof`` path, a :class:`Sof`, or already decoded 128x64 pixels.
    """
    from . import png

    if isinstance(src, Sof):
        pixels = src.frame(index)
    elif isinstance(src, (bytes, bytearray)):
        pixels = bytes(src)
    else:
        pixels = frame(os.fspath(src), index)
    return png.indexed(pixels, TILE_W, TILE_H, palette)
