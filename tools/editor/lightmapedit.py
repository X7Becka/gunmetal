#!/usr/bin/env python3
"""Editor for Gunmetal ``.sbc`` level lightmaps (``M11.2``, slice 2).

An ``.sbc`` blob is a flat, self-delimiting container of small grayscale maps
(``gm/sbc.py`` module docstring, ``docs/FORMATS.md`` §4)::

    char magic[4];        // "SBC0" (2-bit levels) or "SBC1" (8-bit levels)
    u32  count;
    count * {
        u32 desc;         // w_log = desc & 0xffff, h_log = desc >> 16
        u8  pixels[];     // (1 << w_log) * (1 << h_log) bytes, 8-bit
    };

Records are stored back to back and ``8 + sum(4 + 2**w * 2**h)`` equals the
file size on every shipped blob.  The bytes are **level lightmaps**: the level
loader builds ``"<level>.SBC"`` next to the ``.sbw`` (``0x442482``) and the
polygon field ``+0x04`` is a **1-based** index into the list.  ``SBC0`` is
shifted left by two bits by the loader (``0x429a2a``), so both magics end up on
the ``0..127`` illumination scale used by the sampler (``0x43ca5a``).

Because a record's size lives in its ``desc`` word and the records have no
offsets, a record can be replaced **in place** only when the new pixels have
exactly the same ``2**w_log x 2**h_log`` shape.  The tool therefore

* walks the blob with its **own** bounds-checked reader (:func:`walk`, no
  ``gm.sbc`` involvement) to locate every record and its pixel span,
* writes a new blob by copying every record's **raw** ``desc`` bytes and pixel
  payload, substituting only the target record's pixels (:func:`serialise`);
  the total size is recomputed from the records,
* re-reads the result with the oracle ``gm.sbc`` *and* with the own reader and
  proves that every other byte stayed identical (:func:`verify_edit`).

Usage::

    python3 tools/editor/lightmapedit.py --list level0 [--limit N] [--json]
    python3 tools/editor/lightmapedit.py --export level0 --record 0 --png FILE
    python3 tools/editor/lightmapedit.py --import level0 --record 0 --png FILE \\
        --out /tmp/opencode/lightmap/level0.sbc
    python3 tools/editor/lightmapedit.py --fill level0 --record 0 --value 128 \\
        --out /tmp/opencode/lightmap/level0.sbc
    python3 tools/editor/lightmapedit.py --scale level0 --record 0 --factor 1.5 \\
        --out /tmp/opencode/lightmap/level0.sbc
    python3 tools/editor/lightmapedit.py --check FILE --against level0 \\
        [--record N --value V | --record N --png FILE]
    python3 tools/editor/lightmapedit.py --roundtrip [--game DIR]
    python3 tools/editor/lightmapedit.py --self-test

Exit codes: ``0`` success, ``1`` verification failed (check/round-trip/
self-test), ``2`` bad input or a refused edit (message on stderr), ``3``
internal error.  No traceback escapes :func:`main`.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import png as pngmod                         # noqa: E402
from gm import sbc as oracle                         # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_OUT_DIR = "/tmp/opencode/lightmap"

MAGICS = (b"SBC0", b"SBC1")
#: Same trust boundary as the oracle: ``w_log``/``h_log`` above 16 are refused
#: (``gm/sbc.py`` ``parse``), so no record can exceed 65536 x 65536 texels.
MAX_LOG = 16
#: No shipped blob comes close; guards against a corrupt count word.
MAX_RECORDS = 1_000_000


class EditError(Exception):
    """Bad input or a refused edit; the message is meant for the user."""


class ScanError(Exception):
    """The blob does not walk as an ``.sbc`` (own reader)."""


# ---------------------------------------------------------------------------
# own reader / writer
# ---------------------------------------------------------------------------

@dataclass
class Rec:
    """One record as it sits on disk (offsets are file-absolute)."""

    index: int
    start: int              # first byte of ``desc``
    offset: int             # first pixel byte
    desc: int
    width: int
    height: int
    pixels: bytes

    @property
    def texels(self) -> int:
        return self.width * self.height

    @property
    def end(self) -> int:
        return self.offset + self.texels

    def stats(self) -> dict:
        px = self.pixels
        return {
            "texels": len(px),
            "min": min(px),
            "max": max(px),
            "mean": round(sum(px) / len(px), 3),
        }


@dataclass
class Blob:
    magic: bytes
    records: list[Rec]
    size: int

    @property
    def count(self) -> int:
        return len(self.records)

    @property
    def texels(self) -> int:
        return sum(r.texels for r in self.records)


def walk(data: bytes) -> Blob:
    """Bitexact reader for ``.sbc``; independent of ``gm.sbc``."""
    if len(data) < 8:
        raise ScanError(f"blob too short for an .sbc header ({len(data)} bytes)")
    magic = data[:4]
    if magic not in MAGICS:
        raise ScanError(f"bad .sbc magic {magic!r} (expected SBC0 or SBC1)")

    count = struct.unpack_from("<I", data, 4)[0]
    if count > MAX_RECORDS:
        raise ScanError(f"implausible record count {count}")

    off = 8
    records: list[Rec] = []
    for i in range(count):
        if off + 4 > len(data):
            raise ScanError(f"truncated descriptor table at record {i} "
                            f"({off + 4} > {len(data)})")
        desc = struct.unpack_from("<I", data, off)[0]
        w_log = desc & 0xFFFF
        h_log = desc >> 16
        if w_log > MAX_LOG or h_log > MAX_LOG:
            raise ScanError(f"record {i}: implausible size 2**{w_log} x "
                            f"2**{h_log} (limit {MAX_LOG})")
        w = 1 << w_log
        h = 1 << h_log
        size = w * h
        if off + 4 + size > len(data):
            raise ScanError(f"record {i}: truncated pixels "
                            f"({off + 4 + size} > {len(data)})")
        records.append(Rec(i, off, off + 4, desc, w, h,
                           data[off + 4:off + 4 + size]))
        off += 4 + size

    if off != len(data):
        raise ScanError(f"{len(data) - off} trailing bytes after {count} records")
    return Blob(magic, records, len(data))


def serialise(blob: Blob, replacements: dict[int, bytes] | None = None) -> bytes:
    """Rebuild the blob, copying raw ``desc`` words and substituting pixels."""
    replacements = replacements or {}
    out = bytearray(blob.magic)
    out += struct.pack("<I", len(blob.records))
    for r in blob.records:
        px = replacements.get(r.index, r.pixels)
        if len(px) != r.texels:
            raise EditError(f"record {r.index}: {len(px)} bytes for "
                            f"{r.width}x{r.height} ({r.texels} texels)")
        out += struct.pack("<I", r.desc) + px
    return bytes(out)


def build_blob(records: list[tuple[int, int, bytes]],
               magic: bytes = b"SBC1") -> bytes:
    """Build a blob from ``(width, height, pixels)`` triples (self-test)."""
    if magic not in MAGICS:
        raise EditError(f"magic must be one of {MAGICS!r}")
    out = bytearray(magic)
    out += struct.pack("<I", len(records))
    for i, (w, h, px) in enumerate(records):
        if w < 1 or h < 1 or w & (w - 1) or h & (h - 1):
            raise EditError(f"record {i}: {w}x{h} is not a power of two")
        if len(px) != w * h:
            raise EditError(f"record {i}: {len(px)} bytes for {w}x{h}")
        desc = (w.bit_length() - 1) | ((h.bit_length() - 1) << 16)
        out += struct.pack("<I", desc) + px
    return bytes(out)


def diff_offsets(a: bytes, b: bytes) -> list[int]:
    """Offsets where two equal-length blobs differ."""
    if len(a) != len(b):
        raise EditError(f"length differs: {len(a)} vs {len(b)}")
    return [i for i, (x, y) in enumerate(zip(a, b)) if x != y]


# ---------------------------------------------------------------------------
# pixel edits
# ---------------------------------------------------------------------------

def fill_pixels(rec: Rec, value: int) -> bytes:
    if not 0 <= value <= 255:
        raise EditError(f"--value must be 0..255 (got {value})")
    return bytes([value]) * rec.texels


def scale_pixels(rec: Rec, factor: float) -> bytes:
    if not (factor == factor) or factor in (float("inf"), float("-inf")):
        raise EditError(f"--factor must be a finite number (got {factor!r})")
    if factor < 0:
        raise EditError(f"--factor must be >= 0 (got {factor})")
    out = bytearray(rec.texels)
    for i, p in enumerate(rec.pixels):
        v = int(p * factor + 0.5)                    # round half up
        out[i] = 0 if v < 0 else (255 if v > 255 else v)
    return bytes(out)


def decode_png(data: bytes, rec: Rec, what: str = "PNG") -> bytes:
    """Decode an 8-bit grayscale PNG whose size matches *rec* exactly."""
    try:
        img = pngmod.decode(data)
    except (ValueError, zlib.error) as exc:
        raise EditError(f"{what}: {exc}") from None
    if img.mode != "L":
        raise EditError(f"{what}: colour type {img.mode!r}, need 8-bit "
                        f"grayscale (mode L); the lightmap has no palette")
    if (img.width, img.height) != (rec.width, rec.height):
        raise EditError(
            f"{what} is {img.width}x{img.height}, record {rec.index} is "
            f"{rec.width}x{rec.height}; .sbc sizes are fixed by `desc`, "
            f"resize the PNG to {rec.width}x{rec.height} instead")
    if len(img.pixels) != rec.texels:
        raise EditError(f"{what}: {len(img.pixels)} bytes for "
                        f"{rec.width}x{rec.height} ({rec.texels} texels)")
    return img.pixels


# ---------------------------------------------------------------------------
# paths and level resolution
# ---------------------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def write_bytes(path: str, data: bytes) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(parent, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    except OSError as exc:
        raise EditError(f"{path}: {exc}") from None


def resolve_blob(name: str, game: str = DEFAULT_GAME) -> str:
    """``level0``/``level0.sbc``/``level0.sbw``/a path -> existing ``.sbc``."""
    if os.path.isfile(name):
        return name
    cand = name
    if cand.lower().endswith((".sbw", ".sbt")):
        cand = cand[:-4] + ".sbc"
    elif not cand.lower().endswith(".sbc"):
        cand += ".sbc"
    direct = os.path.join(game, cand)
    if os.path.isfile(direct):
        return direct
    low = cand.lower()
    try:
        entries = os.listdir(game)
    except OSError as exc:
        raise EditError(f"game directory: {exc}") from None
    for entry in entries:
        if entry.lower() == low:
            return os.path.join(game, entry)
    raise EditError(f"level lightmaps not found: {name} (looked in {game})")


def guard_out(out_path: str, game: str = DEFAULT_GAME) -> None:
    """The installation is read-only: never write next to the shipped files."""
    out_dir = os.path.realpath(os.path.dirname(os.path.abspath(out_path)))
    game_dir = os.path.realpath(os.path.abspath(game))
    if out_dir == game_dir or out_dir.startswith(game_dir + os.sep):
        raise EditError(f"refusing to write inside the game installation: "
                        f"{out_path} (use --out outside {game})")


def default_out(source: str, record: int, op: str) -> str:
    stem = os.path.splitext(os.path.basename(source))[0]
    return os.path.join(DEFAULT_OUT_DIR, f"{stem}_r{record}_{op}.sbc")


def _require_record(blob: Blob, index: int | None, what: str) -> Rec:
    if index is None:
        raise EditError(f"{what} needs --record N")
    if not 0 <= index < len(blob.records):
        raise EditError(f"record {index} does not exist "
                        f"(0..{len(blob.records) - 1})")
    return blob.records[index]


# ---------------------------------------------------------------------------
# verification (oracle + own reader)
# ---------------------------------------------------------------------------

def verify_edit(orig: bytes, new: bytes, target: int,
                expect: bytes | None = None,
                allow_same: bool = False) -> dict:
    """Prove an edit: oracle sees it, every other byte is untouched.

    All steps must pass:

    1. both blobs parse with the oracle and re-serialise byte-exactly;
    2. record count and every record's ``(width, height)`` are unchanged;
    3. the own reader parses both blobs to EOF;
    4. all bytes outside the target record's pixels are identical (header,
       every ``desc`` word, every other record);
    5. only the target record's pixels differ; the file size is unchanged;
    6. the oracle and the own reader see *expect* in the target record.
    """
    checks = []
    problems = []

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})
        if not ok:
            problems.append(text)

    try:
        before = oracle.parse(orig)
        after = oracle.parse(new)
        check(before.to_bytes() == orig, "oracle: source round-trip is exact")
        check(after.to_bytes() == new, "oracle: edited round-trip is exact")
        check(len(before.records) == len(after.records),
              "oracle: record count unchanged")
        check([(r.width, r.height) for r in before.records] ==
              [(r.width, r.height) for r in after.records],
              "oracle: record sizes unchanged")
    except (ValueError, IndexError) as exc:
        check(False, f"oracle: parse failed: {exc}")
        return {"ok": False, "checks": checks, "problems": problems}

    try:
        b0 = walk(orig)
        b1 = walk(new)
    except ScanError as exc:
        check(False, f"own walker: {exc}")
        return {"ok": False, "checks": checks, "problems": problems}

    check(b0.size == len(orig) and b1.size == len(new),
          "own walker: both blobs parse to EOF")
    check(b0.magic == b1.magic, "own walker: magic unchanged")
    check(len(b0.records) == len(b1.records), "own walker: record count unchanged")
    check([(r.width, r.height) for r in b0.records] ==
          [(r.width, r.height) for r in b1.records],
          "own walker: record sizes unchanged")
    check(b0.size == b1.size, "file size unchanged")

    if not 0 <= target < len(b0.records):
        check(False, f"record {target} does not exist")
        return {"ok": False, "checks": checks, "problems": problems}
    rec = b0.records[target]

    outside_ok = (orig[:rec.offset] == new[:rec.offset] and
                  orig[rec.end:] == new[rec.end:])
    check(outside_ok, "bytes outside the target pixels are identical")

    others_ok = all(orig[r.start:r.end] == new[r.start:r.end]
                    for r in b0.records if r.index != target)
    check(others_ok, f"{len(b0.records) - 1} other records are identical")

    changed = diff_offsets(orig, new)
    check(all(rec.offset <= d < rec.end for d in changed),
          f"only record[{target}] pixels differ "
          f"(span {rec.offset}..{rec.end}, {len(changed)} byte(s) changed)")
    if allow_same:
        check(True, "the file actually changed (not required here)")
    else:
        check(bool(changed), "the file actually changed")

    if expect is not None:
        if len(expect) != rec.texels:
            check(False, f"expectation has {len(expect)} bytes for "
                         f"{rec.width}x{rec.height}")
        else:
            check(b1.records[target].pixels == expect,
                  "own walker sees the new pixels")
            check(after.records[target].pixels == expect,
                  "oracle sees the new pixels")

    return {
        "ok": all(c["ok"] for c in checks),
        "checks": checks,
        "problems": problems,
        "changes": changed,
        "record": target,
    }


def check_structure(data: bytes) -> dict:
    """Structural check of one blob (no source to compare against)."""
    checks = []

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})

    path_blob = None
    try:
        path_blob = walk(data)
        check(path_blob.size == len(data), "own walker: parses to EOF")
        check(True, f"own walker: {path_blob.count} records, "
                    f"{path_blob.texels} texels")
    except ScanError as exc:
        check(False, f"own walker: {exc}")

    try:
        w = oracle.parse(data)
        check(w.to_bytes() == data, "oracle: round-trip is byte-exact")
        check(path_blob is None or
              [(r.width, r.height) for r in w.records] ==
              [(r.width, r.height) for r in path_blob.records],
              "own walker and oracle agree on record sizes")
        check(path_blob is None or len(w.records) == path_blob.count,
              "own walker and oracle agree on record count")
    except (ValueError, IndexError) as exc:
        check(False, f"oracle: parse failed: {exc}")

    return {"ok": all(c["ok"] for c in checks), "checks": checks,
            "problems": [c["check"] for c in checks if not c["ok"]]}


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def list_blob(path: str, limit: int = 8) -> dict:
    data = read_bytes(path)
    blob = walk(data)
    try:
        w = oracle.parse(data)
        oracle_end = len(data) if w.to_bytes() == data else -1
        oracle_ok = True
        oracle_records = len(w.records)
    except (ValueError, IndexError):
        oracle_ok = False
        oracle_end = -1
        oracle_records = -1
    shown = blob.records[:limit] if limit else blob.records
    return {
        "path": os.path.abspath(path),
        "bytes": len(data),
        "magic": blob.magic.decode("ascii", "replace"),
        "count": blob.count,
        "texels": blob.texels,
        "oracle_ok": oracle_ok,
        "oracle_records": oracle_records,
        "oracle_end": oracle_end,
        "records_total": blob.count,
        "records": [
            {"index": r.index, "width": r.width, "height": r.height,
             "texels": r.texels, **r.stats()}
            for r in shown
        ],
    }


def print_list(rep: dict, limit: int = 8) -> None:
    tail = f" (showing {len(rep['records'])} of {rep['count']})" if limit else ""
    print(f"level: {rep['path']}")
    print(f"blob: {rep['magic']}, {rep['count']} records, "
          f"{rep['texels']} texels, {rep['bytes']} bytes")
    print(f"oracle: {'ok' if rep['oracle_ok'] else 'PARSE FAILED'}, "
          f"{rep['oracle_records']} records")
    print(f"records{tail}:")
    for r in rep["records"]:
        print(f"  rec[{r['index']:4d}] {r['width']:3d}x{r['height']:<3d} "
              f"{r['texels']:5d} texels  mean={r['mean']:7.2f}  "
              f"min={r['min']:3d}  max={r['max']:3d}")


# ---------------------------------------------------------------------------
# --export / --import / --fill / --scale
# ---------------------------------------------------------------------------

def export_record(level: str, record: int, png_path: str,
                  game: str = DEFAULT_GAME) -> dict:
    path = resolve_blob(level, game)
    data = read_bytes(path)
    blob = walk(data)
    rec = _require_record(blob, record, "--export")

    png_bytes = pngmod.gray(rec.width, rec.height, rec.pixels)
    checks = []

    def check(ok: bool, text: str) -> None:
        checks.append({"check": text, "ok": bool(ok)})

    check(blob.size == len(data), "own walker: parses to EOF")
    try:
        check(oracle.parse(data).to_bytes() == data,
              "oracle: round-trip of the source is exact")
    except (ValueError, IndexError) as exc:
        check(False, f"oracle: parse failed: {exc}")
    try:
        img = pngmod.decode(png_bytes)
        check(img.mode == "L" and (img.width, img.height) ==
              (rec.width, rec.height) and img.pixels == rec.pixels,
              "PNG round-trips through gm.png (mode L, same pixels)")
    except ValueError as exc:
        check(False, f"PNG round-trip failed: {exc}")

    guard_out(png_path, game)
    write_bytes(png_path, png_bytes)
    return {
        "ok": all(c["ok"] for c in checks),
        "checks": checks,
        "problems": [c["check"] for c in checks if not c["ok"]],
        "source": os.path.abspath(path),
        "png": os.path.abspath(png_path),
        "png_bytes": len(png_bytes),
        "magic": blob.magic.decode("ascii", "replace"),
        "count": blob.count,
        "bytes": len(data),
        "record": {"index": rec.index, "width": rec.width,
                   "height": rec.height, **rec.stats()},
    }


def print_export(rep: dict) -> None:
    r = rep["record"]
    print(f"source: {rep['source']} ({rep['magic']}, {rep['count']} records, "
          f"{rep['bytes']} bytes)")
    print(f"record[{r['index']}]: {r['width']}x{r['height']}, "
          f"{r['texels']} texels, mean {r['mean']:.2f}, "
          f"min {r['min']}, max {r['max']}")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    print(f"png: {rep['png']} ({rep['png_bytes']} bytes)")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


def apply_edit(level: str, record: int, op: str, out: str | None = None,
               value: int | None = None, factor: float | None = None,
               png_path: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    """Replace one record's pixels, verify the blob, write to *out*."""
    if op not in ("import", "fill", "scale"):
        raise EditError(f"unknown edit: {op}")
    path = resolve_blob(level, game)
    data = read_bytes(path)
    blob = walk(data)
    rec = _require_record(blob, record, f"--{op}")

    if op == "import":
        if not png_path:
            raise EditError("--import needs --png FILE")
        png_data = read_bytes(png_path)
        new_pixels = decode_png(png_data, rec, os.path.basename(png_path))
        detail = f"import {os.path.abspath(png_path)}"
        extra = {"png": os.path.abspath(png_path), "png_bytes": len(png_data)}
    elif op == "fill":
        if value is None:
            raise EditError("--fill needs --value 0..255")
        new_pixels = fill_pixels(rec, value)
        detail = f"fill value={value}"
        extra = {"value": value}
    else:
        if factor is None:
            raise EditError("--scale needs --factor F")
        new_pixels = scale_pixels(rec, factor)
        detail = f"scale factor={factor}"
        extra = {"factor": factor}

    new = serialise(blob, {record: new_pixels})
    # A no-op edit (identical pixels) is allowed: the result is still verified.
    rep = verify_edit(data, new, record, expect=new_pixels,
                      allow_same=(new == data))
    dest = out or default_out(path, record, op)
    guard_out(dest, game)
    rep.update({
        "source": os.path.abspath(path),
        "out": os.path.abspath(dest),
        "magic": blob.magic.decode("ascii", "replace"),
        "count": blob.count,
        "bytes_before": len(data),
        "bytes_after": len(new),
        "record": {"index": rec.index, "width": rec.width,
                   "height": rec.height, **rec.stats()},
        "record_index": rec.index,
        "op": op,
        "detail": detail,
        "before": rec.stats(),
        "after": {"texels": len(new_pixels),
                  "min": min(new_pixels), "max": max(new_pixels),
                  "mean": round(sum(new_pixels) / len(new_pixels), 3)},
        **extra,
    })
    if rep["ok"]:
        write_bytes(dest, new)
    return rep


def print_edit(rep: dict) -> None:
    b, a = rep["before"], rep["after"]
    r = rep["record"]
    print(f"source: {rep['source']} ({rep['magic']}, {rep['count']} records, "
          f"{rep['bytes_before']} bytes)")
    print(f"edit: record[{r['index']}] {r['width']}x{r['height']} "
          f"{rep['detail']}")
    print(f"  mean {b['mean']:.2f} -> {a['mean']:.2f}, "
          f"min {b['min']} -> {a['min']}, max {b['max']} -> {a['max']}")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    note = " (identical to the source)" if rep["bytes_before"] == \
        rep["bytes_after"] and not rep.get("changes") else ""
    print(f"out: {rep['out']} ({rep['bytes_after']} bytes){note}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def check_blob(path: str, against: str | None = None,
               record: int | None = None, value: int | None = None,
               png_path: str | None = None,
               game: str = DEFAULT_GAME) -> dict:
    data = read_bytes(path)
    rep: dict = {"path": os.path.abspath(path), "bytes": len(data),
                 "against": None, "record": record, "ok": False}
    if against:
        source = resolve_blob(against, game)
        orig = read_bytes(source)
        rep["against"] = os.path.abspath(source)
        rep["bytes_against"] = len(orig)
        if record is not None:
            blob = walk(orig)
            rec = _require_record(blob, record, "--check --record")
            expect = None
            if value is not None:
                expect = fill_pixels(rec, value)
                rep["value"] = value
            elif png_path:
                expect = decode_png(read_bytes(png_path), rec,
                                    os.path.basename(png_path))
                rep["png"] = os.path.abspath(png_path)
            v = verify_edit(orig, data, record, expect)
            v.update({"path": rep["path"], "against": rep["against"],
                      "bytes": len(data), "bytes_against": len(orig),
                      "record": record})
            return v
        # no --record: the whole blob must be identical to the source
        checks = []

        def check(ok: bool, text: str) -> None:
            checks.append({"check": text, "ok": bool(ok)})

        check(orig == data, "files are byte-identical to the source")
        st = check_structure(data)
        checks.extend(st["checks"])
        ok = all(c["ok"] for c in checks)
        return {"ok": ok, "checks": checks,
                "problems": [c["check"] for c in checks if not c["ok"]],
                "path": rep["path"], "against": rep["against"],
                "bytes": len(data), "bytes_against": len(orig),
                "record": None,
                "changes": diff_offsets(orig, data)
                if len(orig) == len(data) else []}
    if record is not None:
        raise EditError("--check --record needs --against LEVEL")
    st = check_structure(data)
    st.update({"path": rep["path"], "against": None, "bytes": len(data),
               "record": None})
    return st


def print_check(rep: dict) -> None:
    print(f"check: {rep['path']} ({rep['bytes']} bytes)")
    if rep.get("against"):
        print(f"against: {rep['against']}")
    if rep.get("record") is not None:
        target = f"record[{rep['record']}]"
        if rep.get("value") is not None:
            target += f" value={rep['value']}"
        elif rep.get("png"):
            target += f" png={rep['png']}"
        print(f"target: {target}")
    for c in rep["checks"]:
        print(f"  [{'ok' if c['ok'] else 'FAIL'}] {c['check']}")
    if rep.get("changes"):
        print(f"  differing bytes: {len(rep['changes'])}")
    print(f"result: {'OK' if rep['ok'] else 'FAILED'}")


# ---------------------------------------------------------------------------
# --roundtrip
# ---------------------------------------------------------------------------

def _shipped_blobs(game: str) -> list[str]:
    """Every ``.sbc`` in the installation root and ``SOBS/`` (case-insensitive)."""
    found: list[str] = []
    for folder in (game, os.path.join(game, "SOBS")):
        try:
            entries = os.listdir(folder)
        except OSError:
            continue
        for entry in entries:
            if entry.lower().endswith(".sbc"):
                path = os.path.join(folder, entry)
                if os.path.isfile(path):
                    found.append(path)
    return sorted(found)


def roundtrip_corpus(game: str = DEFAULT_GAME, verbose: bool = False) -> dict:
    files = _shipped_blobs(game)
    rep = {"game": os.path.abspath(game), "files": len(files), "identical": 0,
           "mismatched": [], "failures": [], "records": 0, "texels": 0,
           "bytes": 0, "magic": {}, "ok": False}
    for path in files:
        name = os.path.basename(path)
        try:
            data = read_bytes(path)
            blob = walk(data)
            built = serialise(blob)
            w = oracle.parse(data)
        except (EditError, ScanError, ValueError, IndexError) as exc:
            rep["failures"].append(f"{name}: {exc}")
            if verbose:
                print(f"  FAIL {name}: {exc}")
            continue
        rep["bytes"] += len(data)
        rep["records"] += blob.count
        rep["texels"] += blob.texels
        magic = blob.magic.decode("ascii", "replace")
        rep["magic"][magic] = rep["magic"].get(magic, 0) + 1
        problems = []
        if built != data:
            problems.append("own serialise differs")
        if w.to_bytes() != data:
            problems.append("oracle to_bytes differs")
        if blob.size != len(data):
            problems.append("own walker does not end at EOF")
        if len(w.records) != blob.count:
            problems.append(f"count {len(w.records)} != {blob.count}")
        if [(r.width, r.height) for r in w.records] != \
                [(r.width, r.height) for r in blob.records]:
            problems.append("record sizes differ")
        if problems:
            rep["mismatched"].append({"file": name, "problems": problems})
            if verbose:
                print(f"  FAIL {name}: {'; '.join(problems)}")
        else:
            rep["identical"] += 1
            if verbose:
                print(f"  ok   {name:20s} {len(data):8d} bytes "
                      f"{blob.count:5d} records")
    rep["ok"] = (rep["identical"] == rep["files"] and rep["files"] > 0
                 and not rep["failures"])
    return rep


def print_roundtrip(rep: dict) -> None:
    print(f"round-trip: {rep['identical']}/{rep['files']} identical, "
          f"{rep['records']} records, {rep['texels']} texels, "
          f"{rep['bytes']} bytes ({rep['game']})")
    if rep["magic"]:
        print("magic: " + ", ".join(f"{k}={v}" for k, v in
                                    sorted(rep["magic"].items())))
    for m in rep["mismatched"]:
        print(f"  MISMATCH {m['file']}: {'; '.join(m['problems'])}")
    for f in rep["failures"]:
        print(f"  FAILURE {f}")


# ---------------------------------------------------------------------------
# --self-test (synthetic, no game files)
# ---------------------------------------------------------------------------

def _selftest() -> bool:
    results: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, bool(ok), detail))

    def expect_refused(name: str, fn) -> None:
        try:
            fn()
        except (EditError, ScanError) as exc:
            check(name, True, str(exc))
        except Exception as exc:                       # noqa: BLE001
            check(name, False, f"wrong exception {type(exc).__name__}: {exc}")
        else:
            check(name, False, "no error raised")

    synth = [
        (1, 1, bytes([0])),
        (2, 2, bytes([0, 1, 2, 3])),
        (4, 2, bytes(range(8))),
        (8, 8, bytes((x * 8 + y) % 256 for y in range(8) for x in range(8))),
    ]
    data = build_blob(synth, b"SBC1")

    # 1: own reader
    try:
        blob = walk(data)
        check("synth: own walker parses", True)
        check("synth: own walker reaches EOF", blob.size == len(data),
              f"{blob.size}/{len(data)}")
        check("synth: own count/sizes",
              blob.count == 4 and
              [(r.width, r.height) for r in blob.records] ==
              [(w, h) for w, h, _ in synth])
        check("synth: own texels", blob.texels == 1 + 4 + 8 + 64,
              str(blob.texels))
        check("synth: own pixels",
              all(r.pixels == px for r, (_w, _h, px) in zip(blob.records, synth)))
    except Exception as exc:                           # noqa: BLE001
        check("synth: own walker parses", False, str(exc))
        blob = None

    # 2: the byte layout itself
    check("layout: magic at +0x00", data[:4] == b"SBC1")
    check("layout: count at +0x04",
          struct.unpack_from("<I", data, 4)[0] == 4)
    check("layout: first desc at +0x08 is 1x1",
          struct.unpack_from("<I", data, 8)[0] == 0)
    check("layout: first pixel at +0x0c", data[12] == 0)
    check("layout: second desc is 2x2",
          struct.unpack_from("<I", data, 8 + 4 + 1)[0] == 0x0001_0001)
    check("layout: record 2 desc is 4x2",
          struct.unpack_from("<I", data, 8 + 4 + 1 + 4 + 4)[0] == 0x0001_0002)

    # 3: oracle agrees with the synthetic blob
    try:
        w = oracle.parse(data)
        check("synth: oracle parses", True)
        check("synth: oracle round-trip", w.to_bytes() == data)
        check("synth: oracle pixels",
              all(r.pixels == px for r, (_w, _h, px) in
                  zip(w.records, synth)))
        check("synth: oracle.encode equals build_blob",
              oracle.encode([oracle.Record(wd, ht, px)
                             for wd, ht, px in synth], b"SBC1") == data)
    except Exception as exc:                           # noqa: BLE001
        check("synth: oracle parses", False, str(exc))

    check("synth: own serialise is byte-exact",
          blob is not None and serialise(blob) == data)

    # 4: PNG export/import through gm.png
    try:
        png_bytes = pngmod.gray(4, 2, synth[2][2])
        img = pngmod.decode(png_bytes)
        check("png: export is mode L 4x2",
              img.mode == "L" and (img.width, img.height) == (4, 2))
        check("png: export pixels round-trip", img.pixels == synth[2][2])
        check("png: import equals the record",
              blob is not None and
              decode_png(png_bytes, blob.records[2]) == synth[2][2])
        same = serialise(blob, {2: decode_png(png_bytes, blob.records[2])})
        check("png: re-import of an export is a no-op", same == data)
    except Exception as exc:                           # noqa: BLE001
        check("png: export is mode L 4x2", False, str(exc))

    # 5: fill
    try:
        px = fill_pixels(blob.records[1], 128)
        new = serialise(blob, {1: px})
        rep = verify_edit(data, new, 1, px)
        check("fill: verification passes", rep["ok"],
              "; ".join(rep.get("problems", [])))
        check("fill: all pixels are 128", set(px) == {128})
        start, end = blob.records[1].start, blob.records[1].end
        check("fill: other records identical",
              new[:start] == data[:start] and new[end:] == data[end:])
        check("fill: size unchanged", len(new) == len(data))
    except Exception as exc:                           # noqa: BLE001
        check("fill: verification passes", False, str(exc))

    # 6: scale, including rounding and clipping
    try:
        px = scale_pixels(blob.records[1], 0.5)
        check("scale: 0.5 rounds [0,1,2,3] -> [0,1,1,2]",
              px == bytes([0, 1, 1, 2]), list(px))
        check("scale: verification passes",
              verify_edit(data, serialise(blob, {1: px}), 1, px)["ok"])
        px = scale_pixels(blob.records[3], 2.0)
        check("scale: 2.0 clips at 255",
              px == bytes(min(255, 2 * p) for p in synth[3][2]))
        check("scale: 0.0 is black", set(scale_pixels(blob.records[3], 0.0))
              == {0})
        px = scale_pixels(blob.records[2], 1.0)
        check("scale: 1.0 is identity", px == synth[2][2])
        rep = verify_edit(data, serialise(blob, {2: px}), 2, px,
                          allow_same=True)
        check("scale: identity is accepted with allow_same", rep["ok"],
              "; ".join(rep.get("problems", [])))
    except Exception as exc:                           # noqa: BLE001
        check("scale: 0.5 rounds", False, str(exc))

    # 7: refusals -- values, indices, PNGs
    if blob is not None:
        expect_refused("refuse: record out of range",
                       lambda: _require_record(blob, 4, "--fill"))
        expect_refused("refuse: negative record",
                       lambda: _require_record(blob, -1, "--fill"))
        expect_refused("refuse: missing record",
                       lambda: _require_record(blob, None, "--export"))
        expect_refused("refuse: fill 256",
                       lambda: fill_pixels(blob.records[0], 256))
        expect_refused("refuse: fill -1",
                       lambda: fill_pixels(blob.records[0], -1))
        expect_refused("refuse: scale negative",
                       lambda: scale_pixels(blob.records[0], -0.1))
        expect_refused("refuse: scale nan",
                       lambda: scale_pixels(blob.records[0], float("nan")))
        expect_refused("refuse: scale inf",
                       lambda: scale_pixels(blob.records[0], float("inf")))
        expect_refused("refuse: PNG 2x2 for a 4x2 record",
                       lambda: decode_png(pngmod.gray(2, 2, bytes(4)),
                                          blob.records[2]))
        expect_refused("refuse: colour PNG",
                       lambda: decode_png(pngmod.rgb(4, 2, bytes(24)),
                                          blob.records[2]))
        expect_refused("refuse: interlaced/truncated PNG",
                       lambda: decode_png(pngmod.gray(4, 2, bytes(8))[:40],
                                          blob.records[2]))
        expect_refused("refuse: serialise with the wrong pixel count",
                       lambda: serialise(blob, {0: bytes(3)}))

    # 8: corruption is reported, not guessed
    for cut, what in ((4, "header"), (9, "descriptor"), (20, "pixels"),
                      (len(data) - 1, "tail")):
        try:
            walk(data[:-cut] if cut != len(data) - 1 else data[:-1])
        except ScanError:
            own_ok = True
        except Exception:                              # noqa: BLE001
            own_ok = False
        else:
            own_ok = False
        try:
            oracle.parse(data[:-cut] if cut != len(data) - 1 else data[:-1])
        except (ValueError, IndexError):
            oracle_ok = True
        except Exception:                              # noqa: BLE001
            oracle_ok = False
        else:
            oracle_ok = False
        check(f"truncation ({what}): own walker refuses", own_ok)
        check(f"truncation ({what}): oracle refuses", oracle_ok)

    bad = bytearray(data)
    bad[0:4] = b"SBC2"
    try:
        walk(bytes(bad))
        own_bad = False
    except ScanError:
        own_bad = True
    check("corrupt magic: own walker refuses", own_bad)

    bad = bytearray(data)
    struct.pack_into("<I", bad, 8, 17)                 # w_log = 17 (> limit)
    try:
        walk(bytes(bad))
        own_bad = False
    except ScanError:
        own_bad = True
    check("implausible desc: own walker refuses", own_bad)

    bad = data + b"\x00"
    try:
        walk(bad)
        own_bad = False
    except ScanError:
        own_bad = True
    check("trailing byte: own walker refuses", own_bad)

    # 9: verification is not vacuous
    if blob is not None:
        new = serialise(blob, {1: fill_pixels(blob.records[1], 128)})
        tampered = bytearray(new)
        tampered[blob.records[0].offset] ^= 0xFF        # another record
        rep = verify_edit(data, bytes(tampered), 1, bytes([128]) * 4)
        check("verify: detects a change in another record", not rep["ok"])
        tampered = bytearray(new)
        tampered[blob.records[1].offset] = 127          # only 1 of 4 changed
        rep = verify_edit(data, bytes(tampered), 1, bytes([128]) * 4)
        check("verify: detects a half-done edit", not rep["ok"])

    # 10: full export/import/fill/check cycle through temporary files
    tmpdir = None
    try:
        tmpdir = tempfile.mkdtemp(prefix="lightmapedit-")
        game_dir = os.path.join(tmpdir, "game")     # not the output folder
        src = os.path.join(tmpdir, "synthetic.sbc")
        png_path = os.path.join(tmpdir, "rec2.png")
        dst = os.path.join(tmpdir, "synthetic_edit.sbc")
        write_bytes(src, data)
        exp = export_record(src, 2, png_path, game=game_dir)
        check("export: writes and verifies", exp["ok"],
              "; ".join(exp.get("problems", [])))
        check("export: PNG exists",
              os.path.isfile(png_path))
        ed = apply_edit(src, 2, "import", dst, png_path=png_path,
                        game=game_dir)
        check("import: same PNG is a no-op", ed["ok"] and ed["changes"] == [],
              "; ".join(ed.get("problems", [])))
        fill = apply_edit(src, 1, "fill", dst, value=128, game=game_dir)
        check("fill: writes and verifies", fill["ok"],
              "; ".join(fill.get("problems", [])))
        chk = check_blob(dst, src, record=1, value=128, game=game_dir)
        check("check: accepts the edit", chk["ok"],
              "; ".join(chk.get("problems", [])))
        chk_bad = check_blob(dst, src, record=1, value=127, game=game_dir)
        check("check: rejects a wrong value", not chk_bad["ok"])
        chk_png = check_blob(dst, src, record=2, png_path=png_path,
                             game=game_dir)
        check("check: rejects an untouched record", not chk_png["ok"])
        same = check_blob(src, src, game=game_dir)
        check("check: identical file passes", same["ok"],
              "; ".join(same.get("problems", [])))
        diff = check_blob(dst, src, game=game_dir)
        check("check: full identity rejects the edit", not diff["ok"])
        struct_ok = check_blob(dst, game=game_dir)
        check("check: structure-only accepts a valid blob", struct_ok["ok"],
              "; ".join(struct_ok.get("problems", [])))
        expect_refused(
            "refuse: writing into the game directory",
            lambda: guard_out(os.path.join(tmpdir, "x.sbc"), game=tmpdir))
        expect_refused(
            "refuse: --check --record without --against",
            lambda: check_blob(dst, record=1, game=game_dir))
        expect_refused(
            "refuse: unknown level name",
            lambda: resolve_blob("nosuchlevel", game=game_dir))
        expect_refused("refuse: truncated .sbc read",
                       lambda: walk(data[:-1]))
        # default output path never lands in the installation
        out = default_out(src, 1, "fill")
        check("default --out is under /tmp/opencode/lightmap",
              out.startswith(DEFAULT_OUT_DIR + os.sep), out)
    except Exception as exc:                           # noqa: BLE001
        check("export/import/fill/check cycle", False, str(exc))
    finally:
        if tmpdir:
            for name in os.listdir(tmpdir):
                try:
                    os.remove(os.path.join(tmpdir, name))
                except OSError:
                    pass
            try:
                os.rmdir(tmpdir)
            except OSError:
                pass

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    for name, ok, detail in results:
        if not ok or os.environ.get("LIGHTMAPEDIT_VERBOSE"):
            print(f"  [{'ok' if ok else 'FAIL'}] {name}"
                  + (f" -- {detail}" if detail else ""))
    print(f"self-test: {passed}/{total} passed")
    return passed == total


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _cmd_list(args) -> int:
    path = resolve_blob(args.list_level, args.game)
    limit = max(args.limit, 0)
    rep = list_blob(path, limit)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_list(rep, limit)
    return 0 if rep["oracle_ok"] else 1


def _cmd_export(args) -> int:
    if args.record is None:
        raise EditError("--export needs --record N")
    if not args.png:
        raise EditError("--export needs --png FILE")
    rep = export_record(args.export_level, args.record, args.png, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_export(rep)
    return 0 if rep["ok"] else 1


def _cmd_edit(args) -> int:
    if args.import_level:
        if not args.png:
            raise EditError("--import needs --png FILE")
        rep = apply_edit(args.import_level, args.record, "import", args.out,
                         png_path=args.png, game=args.game)
    elif args.fill_level:
        rep = apply_edit(args.fill_level, args.record, "fill", args.out,
                         value=args.value, game=args.game)
    else:
        rep = apply_edit(args.scale_level, args.record, "scale", args.out,
                         factor=args.factor, game=args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_edit(rep)
    return 0 if rep["ok"] else 1


def _cmd_check(args) -> int:
    rep = check_blob(args.check_file, args.against, args.record,
                     args.value, args.png, args.game)
    if args.json:
        print(json.dumps(rep, indent=1, ensure_ascii=False))
    else:
        print_check(rep)
    return 0 if rep["ok"] else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Edit .sbc level lightmaps of a Gunmetal level (M11.2).")
    act = ap.add_mutually_exclusive_group()
    act.add_argument("--list", dest="list_level", metavar="LEVEL",
                     help="list records: size, texels, mean, min, max")
    act.add_argument("--export", dest="export_level", metavar="LEVEL",
                     help="export one record to a grayscale PNG")
    act.add_argument("--import", dest="import_level", metavar="LEVEL",
                     help="replace one record with an 8-bit grayscale PNG")
    act.add_argument("--fill", dest="fill_level", metavar="LEVEL",
                     help="set every texel of one record to --value 0..255")
    act.add_argument("--scale", dest="scale_level", metavar="LEVEL",
                     help="scale one record's brightness by --factor")
    act.add_argument("--check", dest="check_file", metavar="FILE",
                     help="re-read FILE (oracle + own reader) and verify")
    act.add_argument("--roundtrip", action="store_true",
                     help="read -> write -> compare every shipped .sbc")
    act.add_argument("--self-test", action="store_true",
                     help="synthetic tests (no game files)")
    ap.add_argument("--against", metavar="LEVEL",
                    help="--check: compare against this source blob")
    ap.add_argument("--record", type=int, metavar="N",
                    help="record index (0-based, order of --list)")
    ap.add_argument("--png", metavar="FILE", help="PNG to export to/import from")
    ap.add_argument("--value", type=int, metavar="V",
                    help="--fill: texel value 0..255")
    ap.add_argument("--factor", type=float, metavar="F",
                    help="--scale: brightness factor (0.., clamped to 255)")
    ap.add_argument("--out", metavar="PATH",
                    help="output .sbc (default /tmp/opencode/lightmap/"
                         "<level>_r<N>_<op>.sbc)")
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game installation (default %s)" % DEFAULT_GAME)
    ap.add_argument("--limit", type=int, default=8,
                    help="--list: max records shown (0 = all)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.self_test:
            return 0 if _selftest() else 1
        if args.roundtrip:
            rep = roundtrip_corpus(args.game, args.verbose)
            if args.json:
                print(json.dumps(rep, indent=1, ensure_ascii=False))
            else:
                print_roundtrip(rep)
            return 0 if rep["ok"] else 1
        if args.list_level:
            return _cmd_list(args)
        if args.export_level:
            return _cmd_export(args)
        if args.import_level or args.fill_level or args.scale_level:
            return _cmd_edit(args)
        if args.check_file:
            return _cmd_check(args)
        ap.print_help()
        return 2
    except EditError as exc:
        print(f"lightmapedit: {exc}", file=sys.stderr)
        return 2
    except ScanError as exc:
        print(f"lightmapedit: corrupt .sbc: {exc}", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"lightmapedit: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:                           # noqa: BLE001
        print(f"lightmapedit: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
