#!/usr/bin/env python3
"""Byte-preserving writer and pixel editor for Gunmetal ``.sof`` HUD films
(``M11.5``, slice 7).

``gm/sof.py`` (``M1.6``, ``docs/FORMATS.md`` §11.1) already reads every shipped
``.sof`` and ``Sof.to_bytes()`` re-joins the stored tiles, so the reader half is
proven (48/48 byte-identical, ``docs/SOF_WRITE.md``).  What is missing is a
*writer* that can change a film without rebuilding it: ``.sof`` has no header
and no per-frame metadata, so a film is just a flat stream of 8192-byte tiles
and "editing" means patching bytes inside one tile.

This tool adds that half.  Every shipped film is a whole number of 8192-byte
tiles (95 330 304 bytes = 11 637 tiles, 48 files) and a tile is one 128x64
frame of palette indices stored **transposed**::

    tile[x * 64 + y]  ->  decoded[y * 128 + x]        (x = 0..127, y = 0..63)

so a pixel ``(x, y)`` of frame ``N`` lives at exactly one byte::

    offset = N * 8192 + x * 64 + y

``--set-pixel`` rewrites that byte and leaves every other byte of the film
untouched: no header, no size change, no re-encoding, nothing to normalise.
The tool then proves the edit: one byte differs, the oracle (``gm/sof.py``) sees
the new index at that pixel, the other 8191 pixels of the frame and all other
frames are unchanged.

Per-frame statistics (index range, unique colours, "empty" frames) are computed
on the **stored** tile, not on the decoded frame: the transpose is a bijection,
so the multiset of indices -- hence the minimum, the maximum, the number of
unique colours and "all bytes equal" -- is identical before and after it.

Command surface
---------------
``--roundtrip [DIR]``      read -> write -> compare every ``.sof`` under DIR
                           (default: the game installation).
``--audit [DIR]``          corpus statistics (sizes, frames, indices, colours).
``--list FILE [--limit N]`` frame table: index, size, unique colours, range.
``--check FILE [--against FILE]`` structure + byte comparison; with
                           ``--frame/--x/--y/--index`` it also proves the
                           patched pixel.
``--set-pixel FILE --frame N --x X --y Y --index I --out OUT``
                           patch one palette index; refuses the game, the
                           regress mirror and this repository.
``--png FILE --frame N --out OUT.png``
                           export one 128x64 frame as an indexed PNG
                           (``gm/png.py`` + the shared palette of
                           ``gm/palette.py``); diagnostic, not required.
``--self-test``            synthetic tests, no game data.
``--json``                 machine-readable report for every mode.

Exit codes: ``0`` success, ``1`` verification failed, ``2`` bad input or a
refused edit (message on stderr), ``3`` internal error.  No traceback escapes
:func:`main`.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import sof as oracle                      # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/sofwrit"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")

#: One stored tile is exactly one decoded 128x64 frame (``0x49fbc0`` reads
#: ``0x2000`` bytes, ``0x49fd40`` seeks ``frame << 13``).
TILE_BYTES = oracle.TILE_BYTES                    # 0x2000
FRAME_W = oracle.TILE_W                           # 128
FRAME_H = oracle.TILE_H                           # 64
SRC_W = oracle.SRC_W                              # 64, the stored stride

#: A palette index is one byte; the film carries no palette of its own.
MAX_INDEX = 255
MAX_TILES = oracle.MAX_TILES                      # 1 << 20


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The bytes do not walk as a ``.sof`` (own reader)."""


# ---------------------------------------------------------------------------
# tile codec
# ---------------------------------------------------------------------------

def decode_tile(tile: bytes) -> bytes:
    """Undo the transpose: stored 64x128 -> decoded 128x64 palette indices."""
    if len(tile) != TILE_BYTES:
        raise ScanError("tile must be %d bytes, got %d" % (TILE_BYTES, len(tile)))
    out = bytearray(TILE_BYTES)
    for x in range(FRAME_W):                      # x = column of the frame
        out[x::FRAME_W] = tile[x * SRC_W:(x + 1) * SRC_W]
    return bytes(out)


def encode_tile(pixels: bytes) -> bytes:
    """Inverse of :func:`decode_tile`: 128x64 indices -> stored 64x128 tile."""
    if len(pixels) != TILE_BYTES:
        raise ScanError("frame must be %d bytes, got %d"
                        % (TILE_BYTES, len(pixels)))
    tile = bytearray(TILE_BYTES)
    for y in range(FRAME_H):                      # y = row of the frame
        tile[y::SRC_W] = pixels[y * FRAME_W:(y + 1) * FRAME_W]
    return bytes(tile)


def pixel_offset(frame: int, x: int, y: int) -> int:
    """Absolute file offset of pixel ``(x, y)`` in frame ``frame``.

    Proof: ``decode_tile`` copies ``tile[x*64 : (x+1)*64]`` to positions
    ``x, x+128, ...`` of the frame, i.e. ``decoded[y*128 + x] = tile[x*64 + y]``.
    """
    if frame < 0:
        raise EditError("frame index must be >= 0, got %d" % frame)
    if not 0 <= x < FRAME_W:
        raise EditError("x=%d is outside the 128-pixel frame width" % x)
    if not 0 <= y < FRAME_H:
        raise EditError("y=%d is outside the 64-pixel frame height" % y)
    return frame * TILE_BYTES + x * SRC_W + y


def offset_pixel(offset: int) -> dict:
    """Inverse lookup: absolute offset -> ``(frame, x, y)``."""
    frame, within = divmod(offset, TILE_BYTES)
    x, y = divmod(within, SRC_W)
    return {"frame": frame, "x": x, "y": y, "tile_offset": within}


# ---------------------------------------------------------------------------
# film document
# ---------------------------------------------------------------------------

@dataclass
class SofDoc:
    """A parsed ``.sof``: a flat list of 8192-byte tiles plus an optional tail."""

    data: bytes
    tiles: list[bytes]
    tail: bytes = b""
    path: str | None = None

    def __len__(self) -> int:
        return len(self.tiles)

    @property
    def byte_size(self) -> int:
        return len(self.tiles) * TILE_BYTES + len(self.tail)

    @property
    def aligned(self) -> bool:
        return not self.tail

    def tile(self, index: int) -> bytes:
        if not 0 <= index < len(self.tiles):
            raise EditError("frame %d is outside 0..%d (%d frame(s))"
                            % (index, len(self.tiles) - 1, len(self.tiles)))
        return self.tiles[index]

    def frame(self, index: int) -> bytes:
        """Decoded 128x64 palette indices of frame *index*."""
        return decode_tile(self.tile(index))

    def to_bytes(self) -> bytes:
        """Serialise back; byte-exact for every shipped film."""
        return b"".join(self.tiles) + self.tail

    def stats(self, index: int) -> dict:
        return frame_stats(self.tile(index), index)


def walk(data: bytes, path: str = "") -> SofDoc:
    """Own reader: split a film into tiles, keeping a partial tail verbatim."""
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("sof data must be bytes")
    data = bytes(data)
    if not data:
        raise ScanError("empty .sof (0 bytes, expected whole 8192-byte tiles)")
    count = len(data) // TILE_BYTES
    if count > MAX_TILES:
        raise ScanError(".sof has %d tiles, limit is %d" % (count, MAX_TILES))
    tiles = [data[i * TILE_BYTES:(i + 1) * TILE_BYTES] for i in range(count)]
    return SofDoc(data=data, tiles=tiles, tail=data[count * TILE_BYTES:],
                  path=path or None)


def frame_stats(tile: bytes, index: int) -> dict:
    """Per-frame numbers, computed on the stored tile (transpose-invariant)."""
    seen = set(tile)
    return {
        "index": index,
        "size": len(tile),
        "unique": len(seen),
        "min": min(seen),
        "max": max(seen),
        "empty": len(seen) == 1,
        "fill": next(iter(seen)) if len(seen) == 1 else None,
    }


# ---------------------------------------------------------------------------
# paths and refusal rules
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise EditError("%s: %s" % (path, exc)) from None


def atomic_write(path: str, data: bytes) -> None:
    """Write through a temporary file in the target directory, then replace."""
    directory = os.path.dirname(os.path.abspath(path)) or "."
    try:
        os.makedirs(directory, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix=".sofwrit-", dir=directory)
        try:
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(temp, path)
        except BaseException:
            try:
                os.unlink(temp)
            except OSError:
                pass
            raise
    except OSError as exc:
        raise EditError("%s: %s" % (path, exc)) from None


def require_sof(path: str) -> str:
    """Input files must be ``.sof`` -- everything else is a mistyped call."""
    if not path.lower().endswith(".sof"):
        raise EditError("not a .sof file: %s" % path)
    if not os.path.isfile(path):
        raise EditError("no such file: %s" % path)
    return path


def _real(path: str) -> str:
    return os.path.realpath(os.path.abspath(path))


def protected_roots(game: str) -> list[str]:
    """Directories a write must never touch: game, mirror and this checkout."""
    roots = []
    for candidate in (game, DEFAULT_GAME, MIRROR, ROOT):
        if candidate:
            roots.append(_real(candidate))
    return sorted(set(roots))


def refuse_unsafe_out(out_path: str, game: str, suffix: str = ".sof") -> None:
    """Never write into the game, the regress mirror or this repository."""
    if not out_path.lower().endswith(suffix):
        raise EditError("--out must be a %s path: %s" % (suffix, out_path))
    target = _real(out_path)
    game_real, mirror_real = _real(game), _real(MIRROR)
    for root in protected_roots(game):
        if target == root or target.startswith(root + os.sep):
            if root in (game_real, _real(DEFAULT_GAME)):
                kind = "the game installation"
            elif root == mirror_real:
                kind = "the regress mirror"
            else:
                kind = "the repository"
            raise EditError(
                "refusing to write inside %s: %s "
                "(write to /tmp/opencode or /var/tmp)" % (kind, out_path))


def resolve_input(name: str, game: str = DEFAULT_GAME) -> str:
    """``AMMO4`` / ``AMMO4.SOF`` / a path -> an existing ``.sof``."""
    if os.path.isfile(name):
        return require_sof(name)
    base = name if name.lower().endswith(".sof") else name + ".sof"
    direct = os.path.join(game, "Film", base)
    if os.path.isfile(direct):
        return direct
    low = base.lower()
    for dirpath, dirnames, filenames in os.walk(game):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower() == low:
                return os.path.join(dirpath, filename)
    raise EditError("film not found: %s (looked in %s)" % (name, game))


def corpus_files(root: str) -> list[str]:
    """Every ``.sof`` under *root* (recursive; shipped: all in ``Film/``)."""
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for filename in sorted(filenames):
            if filename.lower().endswith(".sof"):
                found.append(os.path.join(dirpath, filename))
    return found


def shipped_same_relative(path: str, game: str) -> str | None:
    """The shipped film with the same relative path or the same base name."""
    parts = os.path.abspath(path).split(os.sep)
    for i, part in enumerate(parts[:-1]):
        if part.lower() == "film":
            candidate = os.path.join(game, *parts[i:])
            if os.path.isfile(candidate):
                return candidate
    base = os.path.basename(path)
    for candidate in (os.path.join(game, "Film", base),
                      os.path.join(game, base)):
        if os.path.isfile(candidate):
            return candidate
    return None


def default_against(path: str, game: str) -> str | None:
    candidate = shipped_same_relative(path, game)
    if candidate is None or _real(candidate) == _real(path):
        return None
    return candidate


# ---------------------------------------------------------------------------
# structure and byte comparison
# ---------------------------------------------------------------------------

def structure_report(doc: SofDoc) -> dict:
    """Frames, sizes, index range and unique colours of one film."""
    items: list[dict] = []
    items.append(check_item(
        "own reader parses the file",
        True, "%d frame(s), %d tail byte(s)" % (len(doc), len(doc.tail))))
    items.append(check_item(
        "the size is a whole number of 8192-byte frames",
        doc.aligned,
        "%d = %d * %d" % (doc.byte_size, len(doc), TILE_BYTES)
        if doc.aligned else
        "%d bytes = %d frame(s) + %d tail byte(s)"
        % (doc.byte_size, len(doc), len(doc.tail))))
    oracle_ok = False
    oracle_error = None
    try:
        model = oracle.parse(doc.data)
        oracle_ok = len(model) == len(doc) and model.to_bytes() == doc.data
    except (ValueError, TypeError) as exc:
        oracle_error = str(exc)
    items.append(check_item(
        "the oracle re-emits the file byte-exactly (gm/sof.py)",
        oracle_ok, oracle_error or "%d tile(s)" % len(doc)))

    decoded_ok = True
    for index in range(len(doc)):
        try:
            if decode_tile(doc.tiles[index]) != oracle.decode_tile(doc.tiles[index]):
                decoded_ok = False
                break
        except (ScanError, ValueError):
            decoded_ok = False
            break
    items.append(check_item(
        "own decoder agrees with the oracle on every frame",
        decoded_ok, "%d frame(s)" % len(doc)))

    lowest, highest, unique = 255, 0, set()
    empty = []
    for index, tile in enumerate(doc.tiles):
        seen = set(tile)
        lowest = min(lowest, min(seen))
        highest = max(highest, max(seen))
        unique |= seen
        if len(seen) == 1:
            empty.append(index)
    if not doc.tiles:
        lowest = highest = 0
    items.append(check_item(
        "palette indices fit one byte",
        highest <= MAX_INDEX,
        "0..%d in this film, %d unique colour(s)%s"
        % (highest, len(unique),
           "" if not empty else "; empty frames: %s%s"
           % (", ".join(map(str, empty[:8])),
              "…" if len(empty) > 8 else ""))))
    return {
        "frames": len(doc),
        "bytes": doc.byte_size,
        "aligned": doc.aligned,
        "tail": len(doc.tail),
        "index_min": lowest,
        "index_max": highest,
        "unique_colours": len(unique),
        "empty_frames": empty,
        "items": items,
        "ok": all(item["ok"] for item in items),
    }


def check_item(name: str, ok: bool, detail: str = "") -> dict:
    return {"check": name, "ok": bool(ok), "detail": detail}


def byte_diff(old: bytes, new: bytes) -> dict:
    """Count differing bytes; report the first eight offsets (0-based).

    The count is streamed rather than materialised: ``--check --against`` may
    compare two 14 MB films, and a list of 14 million offsets would cost
    hundreds of megabytes.
    """
    shared = min(len(old), len(new))
    changed = 0
    first: list[int] = []
    for offset in range(shared):
        if old[offset] != new[offset]:
            changed += 1
            if len(first) < 8:
                first.append(offset)
    extra = max(len(old), len(new)) - shared
    if extra:
        changed += extra
        for offset in range(shared, shared + min(extra, 8 - len(first))):
            first.append(offset)
    return {
        "changed": changed,
        "first": first,
        "size_delta": len(new) - len(old),
        "old_size": len(old),
        "new_size": len(new),
    }


def diff_summary(diff: dict) -> str:
    if not diff["changed"]:
        return "identical (%d byte(s))" % diff["new_size"]
    if diff["changed"] == 1:
        spot = offset_pixel(diff["first"][0])
        return ("1 byte at offset %d (frame %d, pixel x=%d y=%d)"
                % (diff["first"][0], spot["frame"], spot["x"], spot["y"]))
    return "%d byte(s) differ, first at %s%s" % (
        diff["changed"], ", ".join(map(str, diff["first"][:4])),
        " …" if diff["changed"] > 4 else "")


# ---------------------------------------------------------------------------
# list / check / set-pixel / png
# ---------------------------------------------------------------------------

def list_file(path: str, limit: int = 0, game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    doc = walk(read_bytes(path), path)
    frames = [doc.stats(i) for i in range(len(doc))]
    unique: set[int] = set()
    for item in frames:
        unique |= set(doc.tiles[item["index"]])
    rep = {
        "file": path,
        "bytes": doc.byte_size,
        "frames": len(doc),
        "frame_bytes": TILE_BYTES,
        "tail": len(doc.tail),
        "aligned": doc.aligned,
        "index_min": min((f["min"] for f in frames), default=0),
        "index_max": max((f["max"] for f in frames), default=0),
        "unique_colours": len(unique),
        "empty_frames": [f["index"] for f in frames if f["empty"]],
        "frame_list": frames,
        "ok": doc.aligned,
    }
    return rep


def print_list(rep: dict, limit: int = 0) -> None:
    print("file: %s (%d bytes)" % (rep["file"], rep["bytes"]))
    print("frames: %d x %d bytes (128x64 palette indices, stored transposed)"
          % (rep["frames"], rep["frame_bytes"]))
    print("palette indices: %d..%d, unique colours %d%s"
          % (rep["index_min"], rep["index_max"], rep["unique_colours"],
             "" if rep["aligned"]
             else ", tail %d byte(s) (not a whole frame)" % rep["tail"]))
    empty = rep["empty_frames"]
    print("empty frames (a single palette index): %d%s"
          % (len(empty), " -> %s%s" % (", ".join(map(str, empty[:8])),
                                       "…" if len(empty) > 8 else "")
             if empty else ""))
    frames = rep["frame_list"]
    if limit and len(frames) > 2 * limit:
        shown = frames[:limit] + [None] + frames[-limit:]
    else:
        shown = list(frames)
    print("  frame   bytes  unique   range      fill")
    for item in shown:
        if item is None:
            print("    …")
            continue
        print("  %5d  %6d  %6d   %3d..%-3d  %s"
              % (item["index"], item["size"], item["unique"],
                 item["min"], item["max"],
                 ("all %d" % item["fill"]) if item["empty"] else "-"))


def set_pixel(path: str, frame: int, x: int, y: int, index: int,
              out_path: str, game: str = DEFAULT_GAME) -> dict:
    """Patch one palette index of one frame into *out_path*."""
    path = resolve_input(path, game)
    if not 0 <= index <= MAX_INDEX:
        raise EditError("--index %d is outside 0..%d (one palette index is "
                        "one byte)" % (index, MAX_INDEX))
    offset = pixel_offset(frame, x, y)
    doc = walk(read_bytes(path), path)
    if not doc.aligned:
        raise ScanError(
            "%d bytes is not a whole number of %d-byte frames (%d tail "
            "byte(s)); pixel edits are only defined on whole frames"
            % (doc.byte_size, TILE_BYTES, len(doc.tail)))
    if not 0 <= frame < len(doc):
        raise EditError("frame %d is outside 0..%d (%d frame(s))"
                        % (frame, len(doc) - 1, len(doc)))
    if _real(out_path) == _real(path):
        raise EditError("--out must differ from the input file: %s" % out_path)
    refuse_unsafe_out(out_path, game, ".sof")

    old = doc.data[offset]
    edited = bytearray(doc.data)
    edited[offset] = index
    new = bytes(edited)

    # ---- prove the edit -------------------------------------------------
    items: list[dict] = []
    diff = byte_diff(doc.data, new)
    items.append(check_item(
        "exactly the requested bytes differ from the source",
        diff["changed"] == (0 if old == index else 1),
        "1 byte changed (0x%02x -> 0x%02x)" % (old, index) if old != index
        else "the pixel already held %d (0 bytes changed)" % index))
    items.append(check_item(
        "file size is unchanged",
        len(new) == len(doc.data),
        "%d bytes" % len(new)))
    items.append(check_item(
        "the differing byte is pixel (x=%d, y=%d) of frame %d"
        % (x, y, frame),
        not diff["changed"] or diff["first"] == [offset],
        "offset %d (tile offset %d)" % (offset, offset % TILE_BYTES)))

    fresh = walk(new, path)
    decoded = fresh.frame(frame)
    items.append(check_item(
        "own decoder reads the new index at that pixel",
        decoded[y * FRAME_W + x] == index,
        "%d" % decoded[y * FRAME_W + x]))
    old_decoded = decode_tile(doc.tiles[frame])
    changed_pixels = [i for i in range(TILE_BYTES)
                      if decoded[i] != old_decoded[i]]
    items.append(check_item(
        "only that one pixel of the frame changed",
        changed_pixels == ([] if old == index else [y * FRAME_W + x]),
        "%d pixel(s)" % len(changed_pixels)))

    other_frames = all(
        fresh.tiles[i] == doc.tiles[i]
        for i in range(len(doc)) if i != frame)
    items.append(check_item(
        "every other frame is byte-identical",
        other_frames, "%d frame(s)" % max(len(doc) - 1, 0)))
    items.append(check_item(
        "the edited film keeps the tail verbatim",
        fresh.tail == doc.tail, "%d tail byte(s)" % len(fresh.tail)))

    oracle_tile = oracle.decode_tile(fresh.tiles[frame])
    items.append(check_item(
        "the oracle sees the new index at that pixel",
        oracle_tile[y * FRAME_W + x] == index,
        "%d (gm/sof.py)" % oracle_tile[y * FRAME_W + x]))

    if not all(item["ok"] for item in items):
        raise EditError("the edit failed verification, nothing written:\n"
                        + "\n".join("  [FAIL] %s (%s)"
                                    % (i["check"], i["detail"])
                                    for i in items if not i["ok"]))
    atomic_write(out_path, new)
    return {
        "src": path, "out": out_path, "frame": frame, "frames": len(doc),
        "x": x, "y": y, "offset": offset, "tile_offset": offset % TILE_BYTES,
        "old": old, "new": index, "changed": diff["changed"],
        "bytes": len(new), "items": items,
        "ok": True,
    }


def print_set(rep: dict) -> None:
    print("%s -> %s" % (rep["src"], rep["out"]))
    print("frame %d of %d, pixel (x=%d, y=%d): %d -> %d"
          % (rep["frame"], rep["frames"], rep["x"], rep["y"],
             rep["old"], rep["new"]))
    print("  file offset %d (tile offset %d, tile byte [x*64 + y])"
          % (rep["offset"], rep["tile_offset"]))
    for item in rep["items"]:
        mark = "ok  " if item["ok"] else "FAIL"
        print("  [%s] %s%s" % (mark, item["check"],
                               " (%s)" % item["detail"] if item["detail"] else ""))
    print("bytes changed: %d of %d, size %d"
          % (rep["changed"], rep["bytes"], rep["bytes"]))
    print("result: %s" % ("OK" if rep["ok"] else "FAIL"))


def check_file(path: str, against: str | None = None,
               expectation: dict | None = None,
               game: str = DEFAULT_GAME) -> dict:
    path = resolve_input(path, game)
    doc = walk(read_bytes(path), path)
    structure = structure_report(doc)
    items = list(structure["items"])
    rep = {
        "file": path, "structure": structure, "items": items,
        "against": None, "diff": None, "pixel": None, "ok": structure["ok"],
    }
    if against is None:
        against = default_against(path, game)
    if against is not None:
        if not os.path.isfile(against):
            raise EditError("no such file: %s" % against)
        other = walk(read_bytes(against), against)
        diff = byte_diff(other.data, doc.data)
        rep["against"] = against
        rep["diff"] = diff
        if diff["changed"] == 0:
            source_ok = True
            source_detail = "identical (%d bytes)" % diff["new_size"]
        elif diff["changed"] == 1:
            source_ok = True
            spot = offset_pixel(diff["first"][0])
            source_detail = ("1 byte at offset %d (frame %d, pixel x=%d y=%d)"
                             % (diff["first"][0], spot["frame"], spot["x"],
                                spot["y"]))
        else:
            source_ok = False
            source_detail = diff_summary(diff)
        items.append(check_item(
            "size matches the source" if diff["size_delta"] == 0
            else "size differs from the source by %+d byte(s)"
                 % diff["size_delta"],
            diff["size_delta"] == 0,
            "%d vs %d bytes" % (diff["new_size"], diff["old_size"])))
        items.append(check_item(
            "the file is byte-identical to the source" if diff["changed"] == 0
            else ("one byte differs from the source (as --set-pixel writes)"
                  if diff["changed"] == 1
                  else "too many bytes differ from the source"),
            source_ok, source_detail))
        if diff["changed"] == 1:
            spot = offset_pixel(diff["first"][0])
            rep["pixel"] = {
                "frame": spot["frame"], "x": spot["x"], "y": spot["y"],
                "offset": diff["first"][0],
                "old": other.data[diff["first"][0]],
                "new": doc.data[diff["first"][0]],
            }
    if expectation is not None:
        pixel = rep["pixel"]
        wanted = expectation
        items.append(check_item(
            "the source and the file differ in exactly one pixel",
            pixel is not None,
            "%d differing byte(s)" % (rep["diff"]["changed"]
                                      if rep["diff"] else 0)))
        if pixel is not None:
            offset = pixel_offset(wanted["frame"], wanted["x"], wanted["y"])
            items.append(check_item(
                "the changed byte is the requested pixel (frame %d, x=%d, y=%d)"
                % (wanted["frame"], wanted["x"], wanted["y"]),
                pixel["offset"] == offset,
                "offset %d, expected %d" % (pixel["offset"], offset)))
            items.append(check_item(
                "the requested pixel carries the expected index",
                pixel["new"] == wanted["index"],
                "%d, expected %d" % (pixel["new"], wanted["index"])))
        else:
            items.append(check_item(
                "the changed byte is the requested pixel", False, "no pixel"))
            items.append(check_item(
                "the requested pixel carries the expected index", False,
                "no pixel"))
    rep["ok"] = all(item["ok"] for item in items) and structure["ok"]
    return rep


def print_check(rep: dict) -> None:
    structure = rep["structure"]
    print("check: %s (%d bytes)" % (rep["file"], structure["bytes"]))
    print("frames: %d, frame bytes %d, tail %d, indices %d..%d, colours %d"
          % (structure["frames"], TILE_BYTES, structure["tail"],
             structure["index_min"], structure["index_max"],
             structure["unique_colours"]))
    for item in structure["items"]:
        mark = "ok  " if item["ok"] else "FAIL"
        print("  [%s] %s%s" % (mark, item["check"],
                               " (%s)" % item["detail"] if item["detail"] else ""))
    if rep["against"]:
        print("against: %s" % rep["against"])
        for item in rep["items"][len(structure["items"]):]:
            mark = "ok  " if item["ok"] else "FAIL"
            print("  [%s] %s%s" % (mark, item["check"],
                                   " (%s)" % item["detail"]
                                   if item["detail"] else ""))
        pixel = rep["pixel"]
        if pixel:
            print("changed byte: offset %d = frame %d, pixel (x=%d, y=%d), "
                  "%d -> %d" % (pixel["offset"], pixel["frame"], pixel["x"],
                                pixel["y"], pixel["old"], pixel["new"]))
    print("result: %s" % ("OK" if rep["ok"] else "FAIL"))


def load_palette(game: str) -> tuple[bytes, str]:
    """The shared 256-colour table (``gm/palette.py``), with the film fallback."""
    try:
        from gm import palette
        return palette.installed(game), "gm.palette.installed(%s)" % game
    except (ImportError, OSError, FileNotFoundError, ValueError):
        pass
    try:
        return oracle.load_palette(game), "gm.sof.load_palette(%s)" % game
    except (OSError, FileNotFoundError, ValueError) as exc:
        raise EditError("no palette carrier under %s: %s" % (game, exc)) from None


def png_frame(path: str, frame: int, out_path: str,
              game: str = DEFAULT_GAME, palette: bytes | None = None) -> dict:
    """Export one frame as a 128x64 indexed PNG (diagnostic only)."""
    from gm import png

    path = resolve_input(path, game)
    doc = walk(read_bytes(path), path)
    if not doc.aligned:
        raise ScanError(
            "%d bytes is not a whole number of %d-byte frames (%d tail "
            "byte(s)); export needs whole frames"
            % (doc.byte_size, TILE_BYTES, len(doc.tail)))
    if not 0 <= frame < len(doc):
        raise EditError("frame %d is outside 0..%d (%d frame(s))"
                        % (frame, len(doc) - 1, len(doc)))
    if _real(out_path) == _real(path):
        raise EditError("--out must differ from the input file: %s" % out_path)
    refuse_unsafe_out(out_path, game, ".png")
    if palette is None:
        palette, source = load_palette(game)
    else:
        source = "explicit palette"
    if len(palette) < 256 * 4:
        raise EditError("palette must be 256 RGBA quads, got %d bytes"
                        % len(palette))
    pixels = doc.frame(frame)
    blob = png.indexed(pixels, FRAME_W, FRAME_H, palette)
    atomic_write(out_path, blob)
    return {
        "src": path, "out": out_path, "frame": frame, "frames": len(doc),
        "png_bytes": len(blob), "width": FRAME_W, "height": FRAME_H,
        "nonempty": sum(1 for value in pixels if value),
        "empty": sum(1 for value in pixels if not value),
        "unique": len(set(pixels)),
        "palette": source,
        "ok": True,
    }


def print_png(rep: dict) -> None:
    print("%s (frame %d of %d) -> %s" % (rep["src"], rep["frame"],
                                         rep["frames"], rep["out"]))
    print("png: %dx%d indexed, %d bytes; palette from %s"
          % (rep["width"], rep["height"], rep["png_bytes"], rep["palette"]))
    print("pixels: %d with a non-zero index, %d at index 0, %d unique colour(s)"
          % (rep["nonempty"], rep["empty"], rep["unique"]))
    print("result: OK")


# ---------------------------------------------------------------------------
# corpus: round-trip and audit
# ---------------------------------------------------------------------------

def _scratch_dir(tag: str) -> str:
    base = os.environ.get("SOFWRIT_TMP", DEFAULT_OUT_DIR)
    try:
        os.makedirs(base, exist_ok=True)
    except OSError:
        base = tempfile.gettempdir()
    return tempfile.mkdtemp(prefix="sofwrit-%s-" % tag, dir=base)


def roundtrip_corpus(root: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    if os.path.isfile(root):
        files = [require_sof(root)]
    elif os.path.isdir(root):
        files = corpus_files(root)
    else:
        raise EditError("no such directory: %s" % root)
    if not files:
        raise EditError("no .sof files under %s" % root)
    started = time.perf_counter()
    scratch = _scratch_dir("rt")
    identical = walked = own_written = oracle_written = on_disk = 0
    frames_total = sizes_total = 0
    failures: list[dict] = []
    try:
        for path in files:
            try:
                data = read_bytes(path)
                sizes_total += len(data)
                doc = walk(data, path)
                walked += 1
                frames_total += len(doc)
                if doc.to_bytes() == data:
                    own_written += 1
                if oracle.parse(data).to_bytes() == data:
                    oracle_written += 1
                temp = os.path.join(scratch, os.path.basename(path))
                atomic_write(temp, doc.to_bytes())
                back = read_bytes(temp)
                os.unlink(temp)
                if back == data:
                    on_disk += 1
                    identical += 1
                else:
                    failures.append({"file": path, "why": "bytes differ",
                                     "detail": diff_summary(byte_diff(data, back))})
            except (EditError, ScanError, ValueError, OSError) as exc:
                failures.append({"file": path, "why": "error",
                                 "detail": str(exc)})
            if verbose and failures and failures[-1]["file"] == path:
                print("  FAIL %s: %s" % (os.path.basename(path),
                                         failures[-1]["detail"]),
                      file=sys.stderr)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    elapsed = time.perf_counter() - started
    total = len(files)
    return {"root": root, "files": total, "identical": identical,
            "bytes": sizes_total, "frames": frames_total,
            "walked": walked, "own_writer": own_written,
            "oracle_writer": oracle_written, "on_disk": on_disk,
            "seconds": round(elapsed, 3), "failures": failures,
            "ok": (identical == total and walked == total
                   and own_written == total and oracle_written == total
                   and on_disk == total and not failures)}


def print_roundtrip(rep: dict) -> None:
    total = rep["files"]
    print("%d/%d identical, %d bytes, %d frame(s), %.2f s (%s)"
          % (rep["identical"], total, rep["bytes"], rep["frames"],
             rep["seconds"], rep["root"]))
    print("  own reader: %d/%d parsed; own writer re-emits %d/%d; "
          "oracle writer %d/%d"
          % (rep["walked"], total, rep["own_writer"], total,
             rep["oracle_writer"], total))
    print("  written to disk and read back byte-exactly: %d/%d"
          % (rep["on_disk"], total))
    if rep["failures"]:
        print("  failures: %d" % len(rep["failures"]))
        for item in rep["failures"][:10]:
            print("    %s: %s %s" % (item["file"], item["why"], item["detail"]))
    print("result: %s" % ("OK" if rep["ok"] else "FAIL"))


def _median(values: list[int]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def audit_corpus(root: str = DEFAULT_GAME) -> dict:
    files = corpus_files(root) if os.path.isdir(root) else [require_sof(root)]
    if not files:
        raise EditError("no .sof files under %s" % root)
    per_file: list[dict] = []
    frames_total = bytes_total = tiles_total = 0
    lowest, highest = 255, 0
    corpus_unique: set[int] = set()
    empty_total = 0
    empty_files: list[str] = []
    unaligned: list[str] = []
    frame_counts: list[int] = []
    uniques: list[int] = []
    biggest = None
    for path in files:
        data = read_bytes(path)
        doc = walk(data, path)
        bytes_total += doc.byte_size
        frames_total += len(doc)
        tiles_total += len(doc)
        frame_counts.append(len(doc))
        if not doc.aligned:
            unaligned.append(os.path.basename(path))
        file_unique: set[int] = set()
        file_min, file_max = 255, 0
        empty_here = []
        for index, tile in enumerate(doc.tiles):
            seen = set(tile)
            file_unique |= seen
            file_min = min(file_min, min(seen))
            file_max = max(file_max, max(seen))
            if len(seen) == 1:
                empty_here.append(index)
        corpus_unique |= file_unique
        lowest = min(lowest, file_min)
        highest = max(highest, file_max)
        uniques.append(len(file_unique))
        empty_total += len(empty_here)
        if empty_here:
            empty_files.append("%s (%d: %s%s)"
                               % (os.path.basename(path), len(empty_here),
                                  ", ".join(map(str, empty_here[:6])),
                                  "…" if len(empty_here) > 6 else ""))
        row = {
            "file": os.path.basename(path),
            "bytes": doc.byte_size,
            "frames": len(doc),
            "tail": len(doc.tail),
            "index_min": file_min,
            "index_max": file_max,
            "unique_colours": len(file_unique),
            "empty_frames": empty_here,
        }
        per_file.append(row)
        if biggest is None or doc.byte_size > biggest["bytes"]:
            biggest = row
    if not per_file:
        lowest = highest = 0
    return {
        "root": root, "files": len(files), "bytes": bytes_total,
        "frames": frames_total, "tiles": tiles_total,
        "frame_bytes": TILE_BYTES,
        "frames_per_file": {"min": min(frame_counts, default=0),
                            "median": _median(frame_counts),
                            "max": max(frame_counts, default=0)},
        "index_min": lowest, "index_max": highest,
        "unique_per_file": {"min": min(uniques, default=0),
                            "median": _median(uniques),
                            "max": max(uniques, default=0)},
        "corpus_unique": len(corpus_unique),
        "empty_frames": empty_total, "empty_files": empty_files,
        "unaligned": unaligned,
        "biggest": biggest,
        "per_file": per_file,
        "ok": not unaligned and bool(files),
    }


def print_audit(rep: dict) -> None:
    print("audit: %d files, %d bytes, %d frame(s) of %d bytes (%s)"
          % (rep["files"], rep["bytes"], rep["frames"], rep["frame_bytes"],
             rep["root"]))
    spread = rep["frames_per_file"]
    print("  frames/file: min %d, median %g, max %d; unaligned sizes: %d file(s)"
          % (spread["min"], spread["median"], spread["max"],
             len(rep["unaligned"])))
    print("  palette indices: %d..%d corpus-wide" % (rep["index_min"],
                                                     rep["index_max"]))
    uni = rep["unique_per_file"]
    print("  unique colours/file: min %d, median %g, max %d; "
          "corpus-wide %d of 256"
          % (uni["min"], uni["median"], uni["max"], rep["corpus_unique"]))
    if rep["empty_frames"]:
        print("  empty frames (a single palette index): %d in %d file(s)"
              % (rep["empty_frames"], len(rep["empty_files"])))
        for line in rep["empty_files"][:10]:
            print("    %s" % line)
    else:
        print("  empty frames (a single palette index): 0")
    if rep["biggest"]:
        big = rep["biggest"]
        print("  largest: %s, %d frames, %d bytes, %d colour(s)"
              % (big["file"], big["frames"], big["bytes"],
                 big["unique_colours"]))
    print("result: %s" % ("OK" if rep["ok"] else "FAIL"))


# ---------------------------------------------------------------------------
# self-test (no game data)
# ---------------------------------------------------------------------------

def _frame_from(fill: int, marks: dict | None = None) -> bytes:
    pixels = bytearray([fill] * TILE_BYTES)
    for (x, y), value in (marks or {}).items():
        pixels[y * FRAME_W + x] = value
    return bytes(pixels)


def _expect_error(label: str, call, results: list[dict],
                  kinds=(EditError, ScanError)) -> None:
    try:
        call()
    except kinds as exc:
        results.append(check_item(label, True, type(exc).__name__))
    except Exception as exc:                            # noqa: BLE001
        results.append(check_item(label, False,
                                  "wrong error: %s: %s"
                                  % (type(exc).__name__, exc)))
    else:
        results.append(check_item(label, False, "no error raised"))


def selftest() -> tuple[bool, list[dict]]:
    from gm import png as pngmod

    results: list[dict] = []
    check = lambda name, ok, detail="": results.append(  # noqa: E731
        check_item(name, ok, detail))

    # --- codec -----------------------------------------------------------
    tile = encode_tile(_frame_from(3, {(0, 0): 9, (127, 63): 7,
                                       (10, 20): 200, (5, 0): 1}))
    decoded = decode_tile(tile)
    check("codec: decode(encode(frame)) is the identity",
          decoded == _frame_from(3, {(0, 0): 9, (127, 63): 7,
                                     (10, 20): 200, (5, 0): 1}),
          "8192 bytes")
    check("codec: encode(decode(tile)) is the identity",
          encode_tile(decoded) == tile, "8192 bytes")
    check("codec: stored tile[x*64 + y] is decoded pixel (x, y)",
          tile[10 * SRC_W + 20] == 200 and decoded[20 * FRAME_W + 10] == 200,
          "x=10 y=20 -> tile offset %d" % (10 * SRC_W + 20))
    check("codec: the transpose is a bijection (same byte multiset)",
          sorted(tile) == sorted(decoded), "8192 bytes")
    check("codec: own decoder equals gm/sof.py on every synthetic frame",
          all(decode_tile(encode_tile(_frame_from(fill))) ==
              oracle.decode_tile(encode_tile(_frame_from(fill)))
              for fill in (0, 7, 255)), "3 frames")
    _expect_error("codec: a short tile is refused",
                  lambda: decode_tile(b"\0" * 100), results)

    # --- document and writer --------------------------------------------
    frames = [_frame_from(0, {(x, 0): x % 256 for x in range(128)}),
              _frame_from(9),
              _frame_from(4, {(1, 2): 3})]
    blob = b"".join(encode_tile(f) for f in frames)
    doc = walk(blob, "synthetic.sof")
    check("parse: three whole tiles are three frames",
          len(doc) == 3 and doc.aligned and doc.byte_size == 3 * TILE_BYTES,
          "%d frame(s), %d bytes" % (len(doc), doc.byte_size))
    check("parse: own writer re-emits the bytes exactly",
          doc.to_bytes() == blob and oracle.parse(blob).to_bytes() == blob,
          "%d bytes" % len(blob))
    check("parse: frame 0 decodes to the synthetic pixels",
          doc.frame(0) == frames[0], "128x64")
    tail_doc = walk(blob + b"\x11\x22", "tail.sof")
    check("parse: a partial trailing tile is kept verbatim, not a frame",
          len(tail_doc) == 3 and tail_doc.tail == b"\x11\x22"
          and not tail_doc.aligned
          and tail_doc.to_bytes() == blob + b"\x11\x22",
          "%d tail byte(s)" % len(tail_doc.tail))
    _expect_error("parse: an empty file is refused",
                  lambda: walk(b""), results)
    check("stats: a single-index frame is reported as empty",
          frame_stats(encode_tile(_frame_from(9)), 1)["empty"]
          and frame_stats(encode_tile(_frame_from(9)), 1)["fill"] == 9,
          "fill 9")
    stats = frame_stats(encode_tile(frames[0]), 0)
    check("stats: index range and unique colours of a frame",
          stats["min"] == 0 and stats["max"] == 127 and stats["unique"] == 128,
          "%d..%d, %d unique" % (stats["min"], stats["max"], stats["unique"]))

    # --- pixel edits ----------------------------------------------------
    work = _scratch_dir("st")
    try:
        src = os.path.join(work, "A.sof")
        atomic_write(src, blob)
        out = os.path.join(work, "A-edit.sof")
        set_pixel(src, 1, 10, 20, 200, out)
        new = read_bytes(out)
        diff = byte_diff(blob, new)
        wanted = pixel_offset(1, 10, 20)
        check("set-pixel: exactly one byte differs",
              diff["changed"] == 1 and diff["first"] == [wanted],
              "offset %d" % wanted)
        check("set-pixel: the file size is unchanged",
              len(new) == len(blob), "%d bytes" % len(new))
        check("set-pixel: the oracle reads the new index at (10, 20)",
              oracle.decode_tile(new[TILE_BYTES:2 * TILE_BYTES])[20 * FRAME_W + 10]
              == 200, "200")
        check("set-pixel: every other pixel of the frame is unchanged",
              sum(1 for i in range(TILE_BYTES)
                  if oracle.decode_tile(new[TILE_BYTES:2 * TILE_BYTES])[i]
                  != frames[1][i]) == 1, "1 pixel")
        check("set-pixel: every other frame is byte-identical",
              new[:TILE_BYTES] == blob[:TILE_BYTES]
              and new[2 * TILE_BYTES:] == blob[2 * TILE_BYTES:],
              "2 frame(s)")
        edge = os.path.join(work, "A-edge.sof")
        set_pixel(src, 2, 127, 63, 1, edge)
        check("set-pixel: the last pixel of the last frame is the last byte",
              byte_diff(blob, read_bytes(edge))["first"] == [len(blob) - 1],
              "offset %d" % (len(blob) - 1))
        same = os.path.join(work, "A-same.sof")
        rep_same = set_pixel(src, 0, 0, 0, 0, same)
        check("set-pixel: writing the value already there changes 0 bytes",
              rep_same["changed"] == 0 and read_bytes(same) == blob,
              "0 bytes")
        _expect_error("set-pixel: palette index 256 is refused",
                      lambda: set_pixel(src, 0, 0, 0, 256,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: palette index -1 is refused",
                      lambda: set_pixel(src, 0, 0, 0, -1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: a frame past the end is refused",
                      lambda: set_pixel(src, 3, 0, 0, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: a negative frame is refused",
                      lambda: set_pixel(src, -1, 0, 0, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: x=128 is refused",
                      lambda: set_pixel(src, 0, 128, 0, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: y=64 is refused",
                      lambda: set_pixel(src, 0, 0, 64, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        unaligned = os.path.join(work, "tail.sof")
        atomic_write(unaligned, blob + b"\x11\x22")
        _expect_error("set-pixel: an unaligned (partial tail) film is refused",
                      lambda: set_pixel(unaligned, 0, 0, 0, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: a non-.sof input is refused",
                      lambda: set_pixel(os.path.join(work, "a.txt"), 0, 0, 0, 1,
                                        os.path.join(work, "x.sof")),
                      results)
        _expect_error("set-pixel: --out equal to the input is refused",
                      lambda: set_pixel(src, 0, 0, 0, 1, src), results)
        _expect_error("set-pixel: --out that is not .sof is refused",
                      lambda: set_pixel(src, 0, 0, 0, 1,
                                        os.path.join(work, "x.soa")),
                      results)
        for protected, label in (
                (os.path.join(DEFAULT_GAME, "Film", "EDIT.sof"), "game"),
                (os.path.join(MIRROR, "Film", "EDIT.sof"), "mirror"),
                (os.path.join(ROOT, "tmp-edit.sof"), "repository")):
            _expect_error("set-pixel: --out inside the %s is refused" % label,
                          lambda p=protected: set_pixel(src, 0, 0, 0, 1, p),
                          results)

        # --- check ------------------------------------------------------
        rep = check_file(out, src)
        check("check: the edited film passes and reports 1 differing byte",
              rep["ok"] and rep["diff"]["changed"] == 1
              and rep["pixel"]["x"] == 10 and rep["pixel"]["y"] == 20
              and rep["pixel"]["new"] == 200,
              "%d byte(s) differ" % rep["diff"]["changed"])
        rep = check_file(out, src, {"frame": 1, "x": 10, "y": 20,
                                    "index": 200})
        check("check: the --frame/--x/--y/--index expectation passes",
              rep["ok"], "frame 1 (10, 20) = 200")
        rep = check_file(out, src, {"frame": 1, "x": 10, "y": 21,
                                    "index": 200})
        check("check: a wrong expectation fails", not rep["ok"],
              "x=10 y=21")
        rep = check_file(src, src)
        check("check: a film against itself passes with 0 differing bytes",
              rep["ok"] and rep["diff"]["changed"] == 0, "0 byte(s)")
        bad = os.path.join(work, "tail2.sof")
        atomic_write(bad, blob + b"\x11")
        rep = check_file(bad, None)
        check("check: an unaligned film is reported and fails",
              not rep["ok"] and rep["structure"]["tail"] == 1,
              "tail 1 byte")

        # --- png (diagnostic) -------------------------------------------
        palette = bytes([(i * 7) % 256 for i in range(1024)])
        png_out = os.path.join(work, "frame.png")
        rep = png_frame(src, 0, png_out, palette=palette)
        image = pngmod.decode(read_bytes(png_out))
        check("png: the exported frame decodes back to the same pixels",
              image.width == 128 and image.height == 64
              and image.pixels == doc.frame(0) and image.mode == "P",
              "%dx%d, %d bytes" % (image.width, image.height,
                                   rep["png_bytes"]))
        check("png: the palette travels in the PNG",
              len(image.palette) == 1024
              and all(image.palette[i * 4:i * 4 + 3]
                      == palette[i * 4:i * 4 + 3] for i in range(256)),
              "256 RGB quads")
        _expect_error("png: a frame past the end is refused",
                      lambda: png_frame(src, 3, os.path.join(work, "x.png"),
                                        palette=palette), results)
        _expect_error("png: an unaligned film is refused",
                      lambda: png_frame(unaligned, 0,
                                        os.path.join(work, "x.png"),
                                        palette=palette), results)
        _expect_error("png: --out that is not .png is refused",
                      lambda: png_frame(src, 0, os.path.join(work, "x.sof"),
                                        palette=palette), results)

        # --- round-trip and audit on a synthetic corpus -----------------
        rt = _scratch_dir("rt")
        try:
            atomic_write(os.path.join(rt, "A.sof"), blob)
            atomic_write(os.path.join(rt, "B.SOF"),
                         b"".join(encode_tile(_frame_from(1)) for _ in range(4)))
            atomic_write(os.path.join(rt, "C.sof"), blob + b"\x33")
            report = roundtrip_corpus(rt)
            check("roundtrip: 3/3 synthetic files are byte-identical",
                  report["identical"] == 3 and report["files"] == 3
                  and report["ok"],
                  "%d/%d, %d bytes"
                  % (report["identical"], report["files"], report["bytes"]))
            audit = audit_corpus(rt)
            check("audit: the synthetic corpus is counted",
                  audit["files"] == 3 and audit["frames"] == 10
                  and audit["bytes"] == 10 * TILE_BYTES + 1
                  and audit["index_min"] == 0 and audit["index_max"] == 127,
                  "%d frames, %d bytes" % (audit["frames"], audit["bytes"]))
            check("audit: the single-index frame and the tail are reported",
                  audit["empty_frames"] == 6
                  and len(audit["empty_files"]) == 3
                  and audit["unaligned"] == ["C.sof"],
                  "%d empty frame(s)" % audit["empty_frames"])
            listing = list_file(os.path.join(rt, "A.sof"))
            check("list: 3 frames with sizes and unique colours",
                  listing["frames"] == 3
                  and [f["size"] for f in listing["frame_list"]]
                  == [TILE_BYTES] * 3
                  and listing["empty_frames"] == [1],
                  "%d frame(s)" % listing["frames"])
        finally:
            shutil.rmtree(rt, ignore_errors=True)

        # --- CLI exit codes ---------------------------------------------
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            refused = main(["--set-pixel", src, "--frame", "0", "--x", "0",
                            "--y", "0", "--index", "300",
                            "--out", os.path.join(work, "y.sof")])
            edited = main(["--set-pixel", src, "--frame", "0", "--x", "1",
                           "--y", "1", "--index", "5",
                           "--out", os.path.join(work, "cli.sof")])
            failed = main(["--check", os.path.join(work, "cli.sof"),
                           "--against", src, "--frame", "0", "--x", "2",
                           "--y", "1", "--index", "5"])
        check("cli: a refused edit exits 2, a successful one exits 0",
              refused == 2 and edited == 0, "2 / 0")
        check("cli: a failed expectation exits 1", failed == 1, "1")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    ok = all(item["ok"] for item in results)
    return ok, results


def print_selftest(ok: bool, results: list[dict]) -> None:
    for item in results:
        mark = "ok  " if item["ok"] else "FAIL"
        print("  [%s] %s%s" % (mark, item["check"],
                               " (%s)" % item["detail"]
                               if item["detail"] else ""))
    passed = sum(1 for item in results if item["ok"])
    print("self-test: %d/%d passed" % (passed, len(results)))
    print("result: %s" % ("OK" if ok else "FAIL"))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_methods(ap) -> None:
    ap.add_argument("file", nargs="?",
                    help=".sof file (or DIR for --roundtrip/--audit)")
    ap.add_argument("--roundtrip", action="store_true",
                    help="read -> write -> compare every .sof under DIR/game")
    ap.add_argument("--audit", action="store_true", help="corpus statistics")
    ap.add_argument("--list", action="store_true", help="frame table")
    ap.add_argument("--check", action="store_true",
                    help="structure + byte comparison")
    ap.add_argument("--set-pixel", action="store_true",
                    help="patch one palette index into --out")
    ap.add_argument("--png", action="store_true",
                    help="export one frame as an indexed PNG")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic tests, no game data")
    ap.add_argument("--against", metavar="FILE",
                    help="--check: source film to compare with")
    ap.add_argument("--frame", type=int, metavar="N",
                    help="--set-pixel/--png/--check: frame index (0-based)")
    ap.add_argument("--x", type=int, metavar="X", help="pixel column 0..127")
    ap.add_argument("--y", type=int, metavar="Y", help="pixel row 0..63")
    ap.add_argument("--index", type=int, metavar="I",
                    help="palette index 0..255")
    ap.add_argument("--out", metavar="PATH", help="--set-pixel/--png output")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL", DEFAULT_GAME),
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--mirror", action="store_true",
                    help="--roundtrip/--audit: use the regress mirror as root")
    ap.add_argument("--limit", type=int, default=0,
                    help="--list: show first/last N frames (0 = all)")
    ap.add_argument("--json", action="store_true", help="JSON report")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv:
        head = argv[0].lower()
        aliases = {
            "self-test": "--self-test", "selftest": "--self-test",
            "roundtrip": "--roundtrip", "audit": "--audit", "list": "--list",
            "check": "--check", "set-pixel": "--set-pixel", "png": "--png",
        }
        if head in aliases:
            argv = [aliases[head]] + argv[1:]
    ap = argparse.ArgumentParser(
        description="Byte-preserving .sof writer and pixel editor "
                    "(M11.5, slice 7).")
    _add_methods(ap)
    args = ap.parse_args(argv)

    modes = [args.list, args.check, args.set_pixel, args.png, args.roundtrip,
             args.audit, args.self_test]
    if sum(bool(m) for m in modes) != 1:
        ap.print_help()
        return 2
    try:
        if args.self_test:
            ok, results = selftest()
            if args.json:
                print(json.dumps({"check": "self-test", "ok": ok,
                                  "passed": sum(1 for r in results if r["ok"]),
                                  "total": len(results), "results": results},
                                 indent=1, ensure_ascii=False))
            else:
                print_selftest(ok, results)
            return 0 if ok else 1

        root = args.file or args.game
        if args.mirror:
            if not os.path.isdir(MIRROR):
                raise EditError("the mirror does not exist: %s" % MIRROR)
            if args.file is None:
                root = MIRROR

        if args.roundtrip:
            rep = roundtrip_corpus(root, verbose=False)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1

        if args.audit:
            rep = audit_corpus(root)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_audit(rep)
            return 0 if rep["ok"] else 1

        if not args.file:
            raise EditError("this mode needs a .sof file")

        if args.list:
            rep = list_file(args.file, max(args.limit, 0), args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_list(rep, max(args.limit, 0))
            return 0 if rep["ok"] else 1

        if args.set_pixel:
            if args.frame is None or args.x is None or args.y is None \
                    or args.index is None:
                raise EditError("--set-pixel needs --frame N --x X --y Y "
                                "--index I")
            if not args.out:
                raise EditError("--set-pixel needs --out FILE")
            rep = set_pixel(args.file, args.frame, args.x, args.y, args.index,
                            args.out, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_set(rep)
            return 0 if rep["ok"] else 1

        if args.png:
            if args.frame is None:
                raise EditError("--png needs --frame N")
            if not args.out:
                raise EditError("--png needs --out OUT.png")
            rep = png_frame(args.file, args.frame, args.out, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_png(rep)
            return 0 if rep["ok"] else 1

        if args.check:
            expectation = None
            if any(value is not None
                   for value in (args.frame, args.x, args.y, args.index)):
                if None in (args.frame, args.x, args.y, args.index):
                    raise EditError("--check expects all of --frame --x --y "
                                    "--index together")
                expectation = {"frame": args.frame, "x": args.x,
                               "y": args.y, "index": args.index}
            rep = check_file(args.file, args.against, expectation, args.game)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_check(rep)
            return 0 if rep["ok"] else 1

        ap.print_help()
        return 2
    except EditError as exc:
        print("sofwrit: %s" % exc, file=sys.stderr)
        return 2
    except ScanError as exc:
        print("sofwrit: corrupt .sof: %s" % exc, file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print("sofwrit: %s" % exc, file=sys.stderr)
        return 2
    except Exception as exc:                            # noqa: BLE001
        print("sofwrit: internal error: %s: %s"
              % (type(exc).__name__, exc), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
