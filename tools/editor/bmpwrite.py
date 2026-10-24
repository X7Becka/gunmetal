#!/usr/bin/env python3
"""Round-trip, audit, extraction and single-entry replacement for the Gunmetal
``FRONTEND/BMP.IDX`` picture archive.

A "BMP archive" is a shipped BIGFILE pair (``.IDX`` + ``.DAT``) whose blobs are
Windows BMP files.  In the installation there is exactly one:

====================  =======  ===========  ================================
index                   count   DAT bytes  payload
====================  =======  ===========  ================================
``FRONTEND/BMP.IDX``      143      2845582  142 x 8-bit BI_RGB + 1 x 4-bit
====================  =======  ===========  ================================

Format facts live in ``docs/FORMATS.md`` §1 (BIGFILE: 12-byte header, 12-byte
records ``char name[8]`` + ``u32 offset``, DAT is a raw concatenation), §6
(``.bmp``: 8-bit palettised Windows BMP, palette B,G,R,X on disk) and
``docs/UI_SCREENS.md`` §9 (what the frontend loads from the archive).  The
**reader/oracle** is ``gm/bigfile.py`` + ``gm/bmp.py`` -- neither is modified
here.  This module is the writer/verifier side: it rebuilds both files byte for
byte, audits the corpus, extracts one payload verbatim and replaces one payload
on disk while proving that every other entry survived.

Everything the writer keeps verbatim (it is *outside* the oracle's model):

* a DAT prefix before ``offset[0]`` (copied to the front of the new ``.DAT``);
* a ``.IDX`` tail after the record table (copied after the new table);
* padding between blobs (it belongs to the preceding entry's extent
  ``offset[i+1] - offset[i]``, so it travels inside that blob);
* the raw ``name[8]`` bytes, the magic and the ``0x1A`` separator.

Recalculated on replacement: only the ``u32`` offsets (sizes are implicit in
neighbouring offsets) and, never, ``count`` -- this tool does not create or
delete entries.  The payload is stored **byte for byte**: this is not a BMP
encoder, so a replacement image must already be a BMP.

Usage::

    python3 tools/editor/bmpwrite.py --roundtrip [DIR]
    python3 tools/editor/bmpwrite.py --audit [DIR]
    python3 tools/editor/bmpwrite.py --list [ARCHIVE.IDX] [--limit N]
    python3 tools/editor/bmpwrite.py --extract PLATE --out DIR
    python3 tools/editor/bmpwrite.py --replace PLATE --image new.bmp --out-dir DIR
    python3 tools/editor/bmpwrite.py --check out/BMP.IDX --against DIR
    python3 tools/editor/bmpwrite.py --self-test [--json]

``--extract``/``--replace`` default to ``FRONTEND/BMP.IDX`` under ``--game``;
``--archive FILE.IDX`` overrides it.  ``--json`` turns every mode into one JSON
line.  Exit codes: ``0`` success, ``1`` verification failed
(round-trip/check/audit/replace/self-test), ``2`` bad input or a refused write,
``3`` internal error.  Writing into the game installation, its mirror or this
repository is refused unconditionally.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import re
import statistics
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)


def _repo_roots() -> list[str]:
    """This checkout plus the main repository behind a ``.git`` worktree file."""
    roots = [ROOT]
    git_file = os.path.join(ROOT, ".git")
    try:
        if os.path.isfile(git_file):          # worktree: "gitdir: <main>/.git/worktrees/<n>"
            with open(git_file, encoding="utf-8") as fh:
                line = fh.readline().strip()
            if line.startswith("gitdir:"):
                git_dir = line.split(":", 1)[1].strip()
                if not os.path.isabs(git_dir):
                    git_dir = os.path.join(ROOT, git_dir)
                main = os.path.dirname(os.path.dirname(os.path.dirname(git_dir)))
                if main and os.path.isdir(main):
                    roots.append(main)
    except OSError:  # pragma: no cover - unreadable checkout metadata
        pass
    return roots


REPO_ROOTS = _repo_roots()

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")
DEFAULT_ARCHIVE = os.path.join("FRONTEND", "BMP.IDX")

MAGIC = b"BIGFILE"
SEP = 0x1A
HEADER_SIZE = 12
RECORD_SIZE = 12
NAME_SIZE = 8
UINT32_MAX = 0xFFFFFFFF
MAX_ENTRIES = 1_000_000          # same trust boundary as gm/bigfile.py:32

#: BITMAPFILEHEADER is 14 bytes, BITMAPINFOHEADER is 40; the smallest BMP the
#: game ships is 14 + 40 + a palette + one pixel row.
FILE_HEADER = 14
DIB_MIN = 40
BMP_MIN = FILE_HEADER + DIB_MIN

BI_RGB = 0
BI_RLE8 = 1
BI_RLE4 = 2
BI_BITFIELDS = 3
COMPRESSION_NAMES = {
    BI_RGB: "BI_RGB",
    BI_RLE8: "BI_RLE8",
    BI_RLE4: "BI_RLE4",
    BI_BITFIELDS: "BI_BITFIELDS",
}
RLE_COMPRESSIONS = (BI_RLE8, BI_RLE4)


class BmpWriteError(Exception):
    """Bad input, a refused write or a structurally broken archive (exit 2)."""


class VerifyError(Exception):
    """A written/checked archive failed its verification (exit 1)."""


class BmpPayloadError(ValueError):
    """A payload is not a structurally valid Windows BMP."""


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def read_bytes(path: str, what: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise BmpWriteError(
            f"cannot read {what} {path}: {exc.strerror or exc}") from exc


def write_bytes(path: str, data: bytes) -> None:
    try:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
    except OSError as exc:
        raise BmpWriteError(f"cannot write {path}: {exc.strerror or exc}") from exc


def emit(payload: dict, as_json: bool, lines=()) -> None:
    if as_json:
        print(json.dumps(payload, ensure_ascii=False))
        return
    for line in lines:
        print(line)


def inside(path: str, directory: str) -> bool:
    """True when *path* is *directory* or lies below it (real paths)."""
    try:
        root = os.path.join(os.path.realpath(directory), "")
        return os.path.realpath(path).startswith(root)
    except OSError:  # pragma: no cover - realpath rarely fails
        return False


def out_paths(out_dir: str, stem: str) -> tuple[str, str]:
    """``out_dir`` + archive stem -> shipped-style ``.IDX`` / ``.DAT`` pair."""
    idx = os.path.join(out_dir, stem + ".IDX")
    return idx, idx[:-4] + ".DAT"


def safe_name(name: str, fallback: str = "unnamed") -> str:
    """An index name as a filename that cannot escape a directory."""
    cleaned = "".join(ch if ch.isalnum() or ch in "_-." else "_" for ch in name)
    return cleaned or fallback


def median_text(values: list[int]) -> str:
    if not values:
        return "0"
    value = statistics.median(values)
    return str(int(value)) if float(value).is_integer() else f"{value:g}"


def median_value(values: list[int]) -> float | int:
    if not values:
        return 0
    value = statistics.median(values)
    return int(value) if float(value).is_integer() else value


def histogram_text(counts: dict[int, int]) -> str:
    return ", ".join(f"{k}:{v}" for k, v in sorted(counts.items()))


# ---------------------------------------------------------------------------
# BMP header model (the writer never decodes pixels)
# ---------------------------------------------------------------------------

def bmp_header_info(blob: bytes) -> dict:
    """Structural facts from a BMP header; raise ``BmpPayloadError`` if broken.

    This is deliberately independent of ``gm/bmp.py``: the oracle decodes
    8-bit BI_RGB only, while the shipped archive also contains one 4-bit
    image (``ICON2``).  Header validation answers "is this a Windows BMP the
    game could hand to its loader", not "can our decoder render it".
    """
    size = len(blob)
    if size < BMP_MIN:
        raise BmpPayloadError(f"{size} bytes, a BMP header alone needs {BMP_MIN}")
    if blob[:2] != b"BM":
        raise BmpPayloadError(f"magic is {blob[:2]!r}, expected b'BM'")
    declared_file_size = struct.unpack_from("<I", blob, 2)[0]
    data_offset = struct.unpack_from("<I", blob, 10)[0]
    dib_size = struct.unpack_from("<I", blob, 14)[0]
    if dib_size < DIB_MIN:
        raise BmpPayloadError(f"unsupported DIB header size {dib_size} (< {DIB_MIN})")
    if FILE_HEADER + dib_size > size:
        raise BmpPayloadError(
            f"DIB header is {dib_size} bytes but only {size - FILE_HEADER} remain")
    width, raw_height = struct.unpack_from("<ii", blob, 18)
    if width <= 0:
        raise BmpPayloadError(f"width {width} is not positive")
    if raw_height == 0:
        raise BmpPayloadError("height is 0")
    top_down = raw_height < 0
    height = abs(raw_height)
    planes, bpp = struct.unpack_from("<HH", blob, 26)
    if planes != 1:
        raise BmpPayloadError(f"planes {planes} != 1")
    if bpp not in (1, 4, 8, 16, 24, 32):
        raise BmpPayloadError(f"unsupported bit count {bpp}")
    compression = struct.unpack_from("<I", blob, 30)[0]
    if compression not in COMPRESSION_NAMES:
        raise BmpPayloadError(f"unknown compression {compression}")
    size_image = struct.unpack_from("<I", blob, 34)[0]
    x_ppm, y_ppm = struct.unpack_from("<ii", blob, 38)
    clr_used, clr_important = struct.unpack_from("<II", blob, 46)
    if bpp <= 8:
        palette_entries = clr_used or (1 << bpp)
        palette_end = FILE_HEADER + dib_size + palette_entries * 4
        if palette_end > size:
            raise BmpPayloadError(
                f"palette ({palette_entries} entries) runs past the payload: "
                f"{palette_end} > {size}")
    else:
        palette_entries = 0
        palette_end = FILE_HEADER + dib_size
    if data_offset < palette_end:
        raise BmpPayloadError(
            f"pixel data offset {data_offset} lies inside the header/palette "
            f"({palette_end})")
    if data_offset > size:
        raise BmpPayloadError(
            f"pixel data offset {data_offset} is past the payload ({size} bytes)")
    if compression == BI_RGB:
        row_stride = ((width * bpp + 31) // 32) * 4
        pixel_bytes = row_stride * height
        if data_offset + pixel_bytes > size:
            raise BmpPayloadError(
                f"truncated pixel data: need {pixel_bytes} bytes at {data_offset}, "
                f"payload is {size}")
        trailing = size - data_offset - pixel_bytes
    elif compression in RLE_COMPRESSIONS:
        row_stride = None
        pixel_bytes = None
        trailing = 0
        if data_offset >= size:
            raise BmpPayloadError("RLE compression but no data after the header")
    else:  # BITFIELDS: colour masks follow the DIB header
        row_stride = ((width * bpp + 31) // 32) * 4
        pixel_bytes = row_stride * height
        if data_offset + pixel_bytes > size:
            raise BmpPayloadError("truncated BITFIELDS pixel data")
        trailing = size - data_offset - pixel_bytes
    return {
        "declared_file_size": declared_file_size,
        "file_size_ok": declared_file_size in (0, size),
        "data_offset": data_offset,
        "dib_size": dib_size,
        "width": width,
        "height": height,
        "top_down": top_down,
        "planes": planes,
        "bpp": bpp,
        "compression": compression,
        "compression_name": COMPRESSION_NAMES[compression],
        "rle": compression in RLE_COMPRESSIONS,
        "size_image": size_image,
        "size_image_ok": size_image in (0, pixel_bytes),
        "x_ppm": x_ppm,
        "y_ppm": y_ppm,
        "clr_used": clr_used,
        "clr_important": clr_important,
        "palette_entries": palette_entries,
        "row_stride": row_stride,
        "pixel_bytes": pixel_bytes,
        "trailing": trailing,
    }


def looks_like_bmp(blob: bytes) -> bool:
    try:
        bmp_header_info(blob)
    except BmpPayloadError:
        return False
    return True


# ---------------------------------------------------------------------------
# archive model (BIGFILE, oracle-compatible)
# ---------------------------------------------------------------------------

class Record:
    """One index record: the verbatim 8 name bytes, offset and spanned blob."""

    __slots__ = ("name_bytes", "offset", "blob")

    def __init__(self, name_bytes: bytes, offset: int, blob: bytes = b""):
        self.name_bytes = name_bytes
        self.offset = offset
        self.blob = blob

    @property
    def name(self) -> str:
        return self.name_bytes.rstrip(b"\0").decode("latin1")

    @property
    def size(self) -> int:
        return len(self.blob)

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return f"Record({self.name!r}, {self.offset}, {self.size})"


class Archive:
    """A parsed ``.IDX``/``.DAT`` pair that can be rebuilt byte for byte."""

    def __init__(self, idx_path: str, dat_path: str, header: bytes,
                 records: list[Record], idx_tail: bytes, dat_prefix: bytes,
                 dat_size: int, sep_ok: bool):
        self.idx_path = idx_path
        self.dat_path = dat_path
        self.header = header
        self.records = records
        self.idx_tail = idx_tail
        self.dat_prefix = dat_prefix
        self.dat_size = dat_size
        self.sep_ok = sep_ok

    @property
    def count(self) -> int:
        return len(self.records)

    @property
    def stem(self) -> str:
        return os.path.splitext(os.path.basename(self.idx_path))[0]

    def find(self, name: str) -> int | None:
        """Index of the first record whose name matches case-insensitively."""
        key = name.lower()
        for i, record in enumerate(self.records):
            if record.name.lower() == key:
                return i
        return None

    def to_bytes(self, blobs: list[bytes] | None = None,
                 records: list[Record] | None = None) -> tuple[bytes, bytes]:
        """Re-emit ``(idx, dat)``; offsets are recomputed, everything else kept."""
        records = self.records if records is None else records
        blobs = [r.blob for r in records] if blobs is None else list(blobs)
        if len(blobs) != len(records):
            raise BmpWriteError(f"{len(blobs)} blobs for {len(records)} records")
        dat = bytearray(self.dat_prefix)
        idx = bytearray(self.header[:8])              # magic + 0x1A, verbatim
        idx += struct.pack("<I", len(records))
        for record, blob in zip(records, blobs):
            if not isinstance(blob, (bytes, bytearray)):
                raise BmpWriteError(f"blob for {record.name!r} is not bytes")
            offset = len(dat)
            if offset > UINT32_MAX:
                raise BmpWriteError(
                    f"payload exceeds 4 GiB offsets at {record.name!r} "
                    f"(offset {offset} > 0x{UINT32_MAX:08X}); the field is u32")
            idx += record.name_bytes + struct.pack("<I", offset)
            dat += blob
        idx += self.idx_tail
        if len(dat) > UINT32_MAX + 1:
            raise BmpWriteError(f"payload is {len(dat)} bytes; u32 offsets cap it "
                                f"at 4 GiB")
        return bytes(idx), bytes(dat)

    def replaced(self, index: int, blob: bytes) -> tuple[list[Record], list[bytes]]:
        records = list(self.records)
        blobs = [r.blob for r in records]
        blobs[index] = blob
        return records, blobs


def resolve_dat(idx_path: str, dat_path: str | None = None) -> str:
    """Find the ``.DAT`` sibling: explicit path, then ``.DAT``, ``.dat``, any case."""
    if dat_path:
        if not os.path.isfile(dat_path):
            raise BmpWriteError(f"no such data file: {dat_path}")
        return os.path.abspath(dat_path)
    base = os.path.splitext(idx_path)[0]
    for candidate in (base + ".DAT", base + ".dat"):
        if os.path.isfile(candidate):
            return candidate
    directory = os.path.dirname(os.path.abspath(idx_path)) or "."
    stem = os.path.basename(base).lower()
    try:
        names = os.listdir(directory)
    except OSError as exc:
        raise BmpWriteError(
            f"cannot list {directory}: {exc.strerror or exc}") from exc
    for name in sorted(names):
        if os.path.splitext(name)[0].lower() == stem and name.lower().endswith(".dat"):
            return os.path.join(directory, name)
    raise BmpWriteError(
        f"data file not found next to {idx_path}: tried {base}.DAT and {base}.dat "
        f"(docs/FORMATS.md §1: the pair is ``<name>.idx`` + ``<name>.dat``)")


def parse_archive(idx_path: str, dat_path: str | None = None) -> Archive:
    """Parse a BIGFILE pair with the oracle's rules, keeping raw/extra bytes."""
    idx_path = os.path.abspath(idx_path)
    if not os.path.isfile(idx_path):
        raise BmpWriteError(f"no such index: {idx_path}")
    idx = read_bytes(idx_path, "index")
    if len(idx) < HEADER_SIZE:
        raise BmpWriteError(
            f"{idx_path}: not a BIGFILE index: {len(idx)} bytes, the header alone "
            f"needs {HEADER_SIZE} (gm/bigfile.py:72)")
    if idx[:len(MAGIC)] != MAGIC:
        raise BmpWriteError(
            f"{idx_path}: not a BIGFILE index: magic is {idx[:len(MAGIC)]!r}, "
            f"expected {MAGIC!r} (gm/bigfile.py:72)")
    sep_ok = idx[len(MAGIC)] == SEP
    count = struct.unpack_from("<I", idx, 8)[0]
    if count > MAX_ENTRIES:
        raise BmpWriteError(
            f"{idx_path}: implausible entry count {count} (limit {MAX_ENTRIES}, "
            f"gm/bigfile.py:32)")
    need = HEADER_SIZE + count * RECORD_SIZE
    if len(idx) < need:
        raise BmpWriteError(
            f"{idx_path}: index too short for {count} entries: {len(idx)} < {need} "
            f"(gm/bigfile.py:78)")
    dat_path = resolve_dat(idx_path, dat_path)
    dat = read_bytes(dat_path, "data")
    dat_size = len(dat)

    records: list[Record] = []
    previous = 0
    for i in range(count):
        o = HEADER_SIZE + RECORD_SIZE * i
        name_bytes = idx[o:o + NAME_SIZE]
        offset = struct.unpack_from("<I", idx, o + 8)[0]
        if offset > dat_size:
            raise BmpWriteError(
                f"{idx_path}: entry {i} ({name_bytes.rstrip(bytes(1))!r}) offset "
                f"{offset} beyond DAT ({dat_size} bytes) (gm/bigfile.py:95)")
        if offset < previous:
            raise BmpWriteError(
                f"{idx_path}: entry {i} offset {offset} goes backwards "
                f"(previous {previous}) (gm/bigfile.py:98)")
        previous = offset
        records.append(Record(name_bytes, offset))

    prefix = dat[:records[0].offset] if records else dat
    for i, record in enumerate(records):
        end = records[i + 1].offset if i + 1 < len(records) else dat_size
        record.blob = dat[record.offset:end]
    return Archive(idx_path, os.path.abspath(dat_path), idx[:HEADER_SIZE],
                   records, idx[need:], prefix, dat_size, sep_ok)


def write_pair(idx_path: str, dat_path: str, idx_bytes: bytes, dat_bytes: bytes,
               *, force: bool) -> None:
    """Write both files atomically (never a new .IDX next to an old .DAT)."""
    if not force:
        for path in (idx_path, dat_path):
            if os.path.exists(path):
                raise BmpWriteError(f"refusing to overwrite {path} (use --force)")
    temps: list[str] = []
    try:
        for path, payload in ((dat_path, dat_bytes), (idx_path, idx_bytes)):
            directory = os.path.dirname(os.path.abspath(path)) or "."
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, prefix=".gm-bmpwrite-")
            temps.append(tmp)
            with os.fdopen(fd, "wb") as fh:
                fh.write(payload)
            try:
                os.chmod(tmp, 0o644)
            except OSError:
                pass
        os.replace(temps[0], dat_path)
        os.replace(temps[1], idx_path)
    except OSError as exc:
        for tmp in temps:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        raise BmpWriteError(f"cannot write archive: {exc.strerror or exc}") from exc


# ---------------------------------------------------------------------------
# oracles: gm.bigfile and gm.bmp (read-only)
# ---------------------------------------------------------------------------

def bigfile():
    try:
        from gm import bigfile as module
    except Exception as exc:  # pragma: no cover - broken checkout
        raise BmpWriteError(f"cannot import the oracle gm.bigfile: {exc}") from exc
    return module


def gm_bmp():
    try:
        from gm import bmp as module
    except Exception as exc:  # pragma: no cover - broken checkout
        raise BmpWriteError(f"cannot import the oracle gm.bmp: {exc}") from exc
    return module


def oracle_blobs(idx_path: str) -> tuple[list[str], list[bytes], int]:
    """Read a pair through ``gm.bigfile.BigFile`` (independent of our parser)."""
    try:
        with bigfile().BigFile(idx_path) as bf:
            names = [entry.name for entry in bf.entries]
            blobs = [data for _name, data in bf.dat_blobs()]
            return names, blobs, bf.dat_size
    except (ValueError, OSError) as exc:
        raise VerifyError(f"oracle cannot read {idx_path}: {exc}") from exc


def gm_decodes(blob: bytes) -> bool:
    """True when the toolkit decoder (8-bit BI_RGB) accepts the payload."""
    try:
        gm_bmp().parse(blob)
    except (ValueError, OSError, struct.error):
        return False
    return True


def gm_decode_error(blob: bytes) -> str:
    try:
        gm_bmp().parse(blob)
    except (ValueError, OSError, struct.error) as exc:
        return str(exc)
    return ""


# ---------------------------------------------------------------------------
# corpus discovery
# ---------------------------------------------------------------------------

def not_bmp_reason(archive: Archive) -> str | None:
    """Why *archive* is not a BMP archive, or ``None`` when it is one."""
    if archive.stem.lower() == "bmp":
        return None
    parsed = 0
    for i, record in enumerate(archive.records):
        if record.size == 0:
            continue
        try:
            bmp_header_info(record.blob)
        except BmpPayloadError as exc:
            return (f"entry {i} ({record.name!r}) does not look like a BMP: {exc}")
        parsed += 1
    if parsed == 0:
        return "no BMP payload"
    return None


def find_archives(directory: str) -> tuple[list[Archive], list[tuple[str, str]]]:
    """Every BMP ``.IDX``/``.DAT`` pair under *directory* (recursive)."""
    if not os.path.isdir(directory):
        raise BmpWriteError(f"directory not found: {directory}")
    found: list[Archive] = []
    skipped: list[tuple[str, str]] = []
    for root, dirs, files in os.walk(directory):
        dirs.sort()
        for name in sorted(files):
            if not name.lower().endswith(".idx"):
                continue
            path = os.path.join(root, name)
            try:
                archive = parse_archive(path)
            except BmpWriteError as exc:
                skipped.append((path, str(exc)))
                continue
            reason = not_bmp_reason(archive)
            if reason is not None:
                skipped.append((path, reason))
                continue
            found.append(archive)
    return found, skipped


# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------

def cmd_roundtrip(directory: str, tmp_dir: str, as_json: bool) -> int:
    archives, skipped = find_archives(directory)
    if not archives:
        raise BmpWriteError(f"no BMP .IDX/.DAT pairs under {directory}")
    os.makedirs(tmp_dir, exist_ok=True)
    rows: list[dict] = []
    failures: list[str] = []
    total_entries = total_bytes = 0
    total_seconds = 0.0
    lines = [f"round-trip: {directory}"]
    lines += [f"  skipped: {os.path.relpath(path, directory)}: {why}"
              for path, why in skipped]
    with tempfile.TemporaryDirectory(prefix="bmpwrite-rt-", dir=tmp_dir) as out:
        for archive in archives:
            rel = os.path.relpath(archive.idx_path, directory)
            started = time.perf_counter()
            try:
                idx_bytes, dat_bytes = archive.to_bytes()
            except BmpWriteError as exc:
                failures.append(f"{rel}: {exc}")
                lines.append(f"  FAIL {rel}: {exc}")
                continue
            stem = re.sub(r"[^A-Za-z0-9_.-]", "_", rel.replace(os.sep, "_"))
            out_idx, out_dat = out_paths(out, stem)
            try:
                write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=True)
            except BmpWriteError as exc:
                failures.append(f"{rel}: {exc}")
                lines.append(f"  FAIL {rel}: {exc}")
                continue
            idx_same = read_bytes(out_idx, "index") == read_bytes(archive.idx_path,
                                                                  "index")
            dat_same = read_bytes(out_dat, "data") == read_bytes(archive.dat_path,
                                                                 "data")
            # Independent reader: the oracle must see the same entry table and
            # the same blob stream in the rebuilt pair.
            names, blobs, dat_size = oracle_blobs(out_idx)
            oracle_ok = (len(names) == archive.count
                         and dat_size == len(dat_bytes)
                         and names == [r.name for r in archive.records]
                         and b"".join(blobs) == dat_bytes)
            ok = idx_same and dat_same and oracle_ok
            if not ok:
                failures.append(f"{rel}: " + "; ".join(filter(None, [
                    "" if idx_same else ".IDX differs",
                    "" if dat_same else ".DAT differs",
                    "" if oracle_ok else "gm.bigfile disagrees",
                ])))
            seconds = time.perf_counter() - started
            total_seconds += seconds
            rows.append({
                "archive": rel,
                "count": archive.count,
                "dat_size": archive.dat_size,
                "idx_size": os.path.getsize(archive.idx_path),
                "idx_identical": idx_same,
                "dat_identical": dat_same,
                "oracle_reads": oracle_ok,
                "idx_tail_bytes": len(archive.idx_tail),
                "dat_prefix_bytes": len(archive.dat_prefix),
                "seconds": round(seconds, 3),
                "ok": ok,
            })
            total_entries += archive.count
            total_bytes += archive.dat_size
            lines.append(
                f"  {'ok  ' if ok else 'FAIL'} {rel:<20} {archive.count:>5} entries  "
                f"{archive.dat_size:>8} B  idx {os.path.getsize(archive.idx_path):>6} B  "
                f"identical={idx_same}/{dat_same}  {seconds:.3f}s")
            for extra, label in ((archive.idx_tail, "index tail"),
                                 (archive.dat_prefix, "DAT prefix")):
                if extra:
                    lines.append(f"       note: {len(extra)} extra {label} bytes preserved")
    ok = not failures and all(row["ok"] for row in rows)
    lines.append(f"  totals: {len(rows)} archive(s), {total_entries} entries, "
                 f"{total_bytes} bytes, identical={sum(r['ok'] for r in rows)}/"
                 f"{len(rows)}, {total_seconds:.3f}s")
    payload = {
        "mode": "roundtrip",
        "dir": os.path.abspath(directory),
        "archives": rows,
        "skipped": [{"path": path, "reason": why} for path, why in skipped],
        "totals": {"archives": len(rows),
                   "identical": sum(row["ok"] for row in rows),
                   "entries": total_entries, "bytes": total_bytes,
                   "seconds": round(total_seconds, 3)},
        "failed": failures,
        "ok": ok,
    }
    emit(payload, as_json, lines)
    return 0 if ok else 1


def archive_bmp_stats(archive: Archive) -> dict:
    """Per-payload BMP classification for ``--audit``/``--list``."""
    bpp: dict[int, int] = {}
    bpp_all = {1: 0, 4: 0, 8: 0, 16: 0, 24: 0, 32: 0}
    compression: dict[int, int] = {}
    clr_used: dict[int, int] = {}
    dims: dict[tuple[int, int], int] = {}
    header_ok = header_fail = 0
    decoded = decode_fail = 0
    rle = 0
    top_down = 0
    trailing = 0
    size_image_zero = 0
    file_size_mismatch = 0
    failures: list[dict] = []
    for record in archive.records:
        if record.size == 0:
            continue
        try:
            info = bmp_header_info(record.blob)
        except BmpPayloadError as exc:
            header_fail += 1
            failures.append({"name": record.name, "size": record.size,
                             "reason": str(exc)})
            continue
        header_ok += 1
        bpp[info["bpp"]] = bpp.get(info["bpp"], 0) + 1
        if info["bpp"] in bpp_all:
            bpp_all[info["bpp"]] += 1
        compression[info["compression"]] = compression.get(info["compression"], 0) + 1
        rle += int(info["rle"])
        clr_used[info["clr_used"]] = clr_used.get(info["clr_used"], 0) + 1
        dims[(info["width"], info["height"])] = dims.get((info["width"], info["height"]), 0) + 1
        top_down += int(info["top_down"])
        trailing += info["trailing"]
        size_image_zero += int(info["size_image"] == 0)
        file_size_mismatch += int(not info["file_size_ok"])
        if gm_decodes(record.blob):
            decoded += 1
        else:
            decode_fail += 1
    return {
        "bmp_header_ok": header_ok,
        "bmp_header_fail": header_fail,
        "gm_bmp_decode_ok": decoded,
        "gm_bmp_decode_fail": decode_fail,
        "bpp": bpp,
        "bpp_all": bpp_all,
        "rle": rle,
        "compression": compression,
        "clr_used": clr_used,
        "top_down": top_down,
        "trailing_bytes": trailing,
        "size_image_zero": size_image_zero,
        "file_size_mismatch": file_size_mismatch,
        "unique_dimensions": len(dims),
        "dimensions": {"x".join(map(str, k)): v
                       for k, v in sorted(dims.items(), key=lambda kv: (-kv[1], kv[0]))},
        "not_bmp": failures,
    }


def cmd_audit(directory: str, as_json: bool) -> int:
    archives, skipped = find_archives(directory)
    if not archives:
        raise BmpWriteError(f"no BMP .IDX/.DAT pairs under {directory}")
    rows: list[dict] = []
    failures: list[str] = []
    total_entries = total_bytes = total_empty = 0
    lines = [f"audit: {directory}",
             f"  {'archive':<20} {'entries':>7} {'bytes':>9} "
             f"{'min/median/max':>22} {'empty':>5} {'name':>4} "
             f"{'bpp':>14} {'dims':>4} {'bmp':>7} {'gm.bmp':>7}"]
    for archive in archives:
        rel = os.path.relpath(archive.idx_path, directory)
        sizes = [record.size for record in archive.records]
        empty = sum(1 for size in sizes if size == 0)
        max_name = max((len(r.name_bytes.rstrip(b"\0")) for r in archive.records),
                       default=0)
        stats = archive_bmp_stats(archive)
        if stats["bmp_header_fail"]:
            failures.append(f"{rel}: {stats['bmp_header_fail']} payload(s) are not "
                            f"header-valid BMP")
        row = {
            "archive": rel,
            "count": archive.count,
            "dat_size": archive.dat_size,
            "idx_size": os.path.getsize(archive.idx_path),
            "size_min": min(sizes) if sizes else 0,
            "size_median": median_value(sizes),
            "size_max": max(sizes) if sizes else 0,
            "size_total": sum(sizes),
            "empty_entries": empty,
            "name_field": NAME_SIZE,
            "name_max_len": max_name,
            **{key: ({str(k): v for k, v in value.items()}
                     if isinstance(value, dict) else value)
               for key, value in stats.items()},
        }
        rows.append(row)
        total_entries += archive.count
        total_bytes += archive.dat_size
        total_empty += empty
        size_text = (f"{min(sizes) if sizes else 0}/{median_text(sizes)}/"
                     f"{max(sizes) if sizes else 0}")
        lines.append(
            f"  {rel:<20} {archive.count:>7} {archive.dat_size:>9} "
            f"{size_text:>22} {empty:>5} {max_name:>4} "
            f"{histogram_text(stats['bpp']):>14} "
            f"{stats['unique_dimensions']:>4} "
            f"{stats['bmp_header_ok']:>4}/{archive.count:<2} "
            f"{stats['gm_bmp_decode_ok']:>4}/{archive.count:<2}")
        lines.append(
            f"       bpp 8:{stats['bpp_all'][8]}, 24:{stats['bpp_all'][24]}, "
            f"32:{stats['bpp_all'][32]}, 1:{stats['bpp_all'][1]}, "
            f"4:{stats['bpp_all'][4]}, 16:{stats['bpp_all'][16]}; "
            f"RLE {stats['rle']}")
        lines.append(
            f"       compression {histogram_text(stats['compression'])}; "
            f"biClrUsed {histogram_text(stats['clr_used'])}; "
            f"top-down {stats['top_down']}; biSizeImage 0 in "
            f"{stats['size_image_zero']}; trailing {stats['trailing_bytes']} B; "
            f"bfSize mismatch {stats['file_size_mismatch']}")
        for item in stats["not_bmp"]:
            lines.append(f"       not BMP: {item['name']} ({item['size']} B): "
                         f"{item['reason']}")
        if stats["gm_bmp_decode_fail"] and not stats["bmp_header_fail"]:
            for record in archive.records:
                if record.size and looks_like_bmp(record.blob) \
                        and not gm_decodes(record.blob):
                    lines.append(
                        f"       gm.bmp cannot decode {record.name} "
                        f"({len(record.blob)} B): {gm_decode_error(record.blob)}")
    lines.append(f"  totals: {len(rows)} archive(s), {total_entries} entries, "
                 f"{total_bytes} bytes, {total_empty} empty entries, "
                 f"header-valid BMP {sum(r['bmp_header_ok'] for r in rows)}/"
                 f"{total_entries}, gm.bmp decodes "
                 f"{sum(r['gm_bmp_decode_ok'] for r in rows)}/{total_entries}, "
                 f"{sum(r['unique_dimensions'] for r in rows)} unique sizes")
    for path, why in skipped:
        lines.append(f"  skipped: {os.path.relpath(path, directory)}: {why}")
    for failure in failures:
        lines.append(f"  PROBLEM: {failure}")
    ok = not failures
    payload = {
        "mode": "audit",
        "dir": os.path.abspath(directory),
        "archives": rows,
        "skipped": [{"path": path, "reason": why} for path, why in skipped],
        "totals": {"archives": len(rows), "entries": total_entries,
                   "bytes": total_bytes, "empty_entries": total_empty,
                   "name_field": NAME_SIZE,
                   "bmp_header_ok": sum(r["bmp_header_ok"] for r in rows),
                   "gm_bmp_decode_ok": sum(r["gm_bmp_decode_ok"] for r in rows),
                   "unique_dimensions": sum(r["unique_dimensions"] for r in rows)},
        "failed": failures,
        "ok": ok,
    }
    emit(payload, as_json, lines)
    return 0 if ok else 1


def _entry_row(index: int, record: Record) -> dict:
    row = {"index": index, "name": record.name, "offset": record.offset,
           "size": record.size, "width": None, "height": None, "bpp": None,
           "compression": None, "top_down": None}
    if record.size:
        try:
            info = bmp_header_info(record.blob)
        except BmpPayloadError as exc:
            row["note"] = f"not a BMP: {exc}"
        else:
            row.update({key: info[key] for key in
                        ("width", "height", "bpp", "compression", "top_down")})
    return row


def cmd_list(idx_path: str, limit: int, as_json: bool) -> int:
    archive = parse_archive(idx_path)
    count = archive.count
    if limit > 0:
        chosen = list(range(min(limit, count)))
        chosen += [i for i in range(max(0, count - limit), count) if i not in chosen]
        chosen.sort()
    else:
        chosen = list(range(count))
    rows = [_entry_row(i, archive.records[i]) for i in chosen]
    lines = [f"{archive.idx_path}: {count} entries, {archive.dat_size} bytes of data, "
             f"index {os.path.getsize(archive.idx_path)} bytes"]
    if not archive.sep_ok:
        lines.append(f"  WARNING: byte 7 is 0x{archive.header[7]:02X}, not 0x1A")
    if archive.dat_prefix:
        lines.append(f"  note: {len(archive.dat_prefix)} bytes before offset[0] preserved")
    if archive.idx_tail:
        lines.append(f"  note: {len(archive.idx_tail)} bytes after the table preserved")
    lines.append(f"  {'#':>5}  {'name':<8} {'offset':>10} {'size':>8}  "
                 f"{'dims':>9} {'bpp':>3} {'comp':>8}")
    previous = None
    for row in rows:
        if limit > 0 and previous is not None and row["index"] != previous + 1:
            lines.append(f"  ... {row['index'] - previous - 1} entries omitted "
                         f"(--limit 0 for all)")
        dims = (f"{row['width']}x{row['height']}"
                if row["width"] is not None else "?")
        comp = (COMPRESSION_NAMES.get(row["compression"], "?")
                if row["compression"] is not None else "?")
        bpp = row["bpp"] if row["bpp"] is not None else "?"
        lines.append(f"  {row['index']:>5}  {row['name']:<8} {row['offset']:>10} "
                     f"{row['size']:>8}  {dims:>9} {bpp:>3} {comp:>8}")
        previous = row["index"]
    payload = {"mode": "list", "idx": archive.idx_path, "dat": archive.dat_path,
               "count": count, "dat_size": archive.dat_size,
               "idx_size": os.path.getsize(archive.idx_path),
               "idx_tail_bytes": len(archive.idx_tail),
               "dat_prefix_bytes": len(archive.dat_prefix),
               "entries": rows, "ok": True}
    emit(payload, as_json, lines)
    return 0


def _positional_diff(a: bytes, b: bytes) -> dict:
    """Byte counts for ``a`` (new) vs ``b`` (old), plus the first difference."""
    changed = sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])
    first = None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            first = i
            break
    if first is None and len(a) != len(b):
        first = min(len(a), len(b))
    return {"delta": len(a) - len(b), "changed_bytes": changed, "first_diff": first}


def resolve_against(against: str, want_stem: str) -> str:
    """``--against`` is a directory (or an ``.IDX``): return its index path."""
    if os.path.isdir(against):
        candidates = [os.path.join(against, want_stem + ".IDX"),
                      os.path.join(against, want_stem + ".idx"),
                      os.path.join(against, want_stem.upper() + ".IDX")]
        for candidate in candidates:
            if os.path.isfile(candidate):
                return candidate
        try:
            names = sorted(name for name in os.listdir(against)
                           if name.lower().endswith(".idx"))
        except OSError as exc:
            raise BmpWriteError(
                f"cannot list {against}: {exc.strerror or exc}") from exc
        if len(names) == 1:
            return os.path.join(against, names[0])
        raise BmpWriteError(
            f"--against {against}: expected {want_stem}.IDX or exactly one .idx "
            f"in the directory, found {len(names)}")
    if os.path.isfile(against):
        return against
    raise BmpWriteError(f"--against: no such directory or index: {against}")


def cmd_check(idx_path: str, dat_path: str | None, against: str | None,
              as_json: bool) -> int:
    problems: list[str] = []
    warnings: list[str] = []
    info: dict = {"mode": "check", "idx": os.path.abspath(idx_path)}
    archive = parse_archive(idx_path, dat_path)     # structural refusal -> exit 2

    if not archive.sep_ok:
        problems.append(f"header byte 7 is 0x{archive.header[7]:02X}, expected 0x1A "
                        f"(docs/FORMATS.md §1)")
    need = HEADER_SIZE + archive.count * RECORD_SIZE
    idx_size = os.path.getsize(archive.idx_path)
    if idx_size < need:
        problems.append(f"index is {idx_size} bytes, the table needs {need}")
    if archive.idx_tail:
        warnings.append(f"{len(archive.idx_tail)} bytes after the index table "
                        f"(preserved, ignored by the reader)")
    if archive.dat_prefix:
        warnings.append(f"{len(archive.dat_prefix)} bytes before offset[0] "
                        f"(preserved, not addressable)")
    if archive.count == 0:
        warnings.append("archive has no entries")
    seen: dict[str, int] = {}
    bmp_ok = 0
    # An archive is treated as a BMP archive when its stem says so or when at
    # least one payload has a valid BMP header; then every payload must be a
    # BMP and no payload may carry bytes after the pixel array.  A non-BMP
    # archive still gets a structural check, with those facts as warnings.
    bmp_check = archive.stem.lower() == "bmp" or any(
        record.size and looks_like_bmp(record.blob)
        for record in archive.records)
    for i, record in enumerate(archive.records):
        key = record.name.lower()
        if key in seen:
            warnings.append(f"duplicate name {record.name!r} at {seen[key]} and {i}")
        else:
            seen[key] = i
        if not record.name:
            warnings.append(f"entry {i} has an empty name")
        elif record.name.upper() != record.name:
            warnings.append(f"entry {i} {record.name!r} is not upper case")
        name_head, _, name_tail = record.name_bytes.partition(b"\0")
        if len(record.name_bytes) > NAME_SIZE:
            problems.append(f"entry {i} name exceeds {NAME_SIZE} bytes")
        elif name_tail.strip(b"\0"):
            warnings.append(f"entry {i} name {name_head!r} has non-NUL bytes after "
                            f"the terminator (the reader stops at the first NUL)")
        # Bounds and monotonicity are enforced by parse_archive (exit 2); the
        # checks below are the model-level restatements the writer relies on.
        if record.offset + record.size > archive.dat_size:
            problems.append(f"entry {i} ({record.name!r}) runs past EOF")
        if record.size == 0:
            warnings.append(f"entry {i} ({record.name!r}) is empty")
            continue
        try:
            info = bmp_header_info(record.blob)
            bmp_ok += 1
        except BmpPayloadError as exc:
            text = f"entry {i} ({record.name!r}) is not a BMP: {exc}"
            if bmp_check:
                problems.append(text)
            else:
                warnings.append(text)
        else:
            if info["trailing"]:
                text = (f"entry {i} ({record.name!r}) has {info['trailing']} "
                        f"trailing byte(s) after the pixel array")
                if bmp_check:
                    problems.append(text)
                else:
                    warnings.append(text)
    # "the last entry ends at EOF": sizes are derived from neighbouring
    # offsets, so the invariant is that the prefix plus every extent covers the
    # DAT exactly -- and a corrupt index is caught earlier by the offset rules.
    covered = len(archive.dat_prefix) + sum(r.size for r in archive.records)
    if covered != archive.dat_size:
        problems.append(f"entries cover {covered} of {archive.dat_size} DAT bytes "
                        f"(the last entry must end at EOF)")
    idx_again, dat_again = archive.to_bytes()
    if idx_again != read_bytes(archive.idx_path, "index"):
        problems.append("rebuilding the index does not reproduce the file")
    if dat_again != read_bytes(archive.dat_path, "data"):
        problems.append("rebuilding the DAT does not reproduce the file")

    names, blobs, dat_size = oracle_blobs(archive.idx_path)
    info.update({"dat": archive.dat_path, "count": archive.count,
                 "dat_size": archive.dat_size, "idx_size": idx_size,
                 "idx_tail_bytes": len(archive.idx_tail),
                 "dat_prefix_bytes": len(archive.dat_prefix),
                 "bmp_payloads_ok": bmp_ok,
                 "bmp_archive": bmp_check})
    if len(names) != archive.count:
        problems.append(f"oracle sees {len(names)} entries, the table has "
                        f"{archive.count}")
    if dat_size != archive.dat_size:
        problems.append(f"oracle sees {dat_size} DAT bytes, file is "
                        f"{archive.dat_size}")
    for i, (blob, record) in enumerate(zip(blobs, archive.records)):
        if blob != record.blob:
            problems.append(f"oracle blob {i} ({record.name!r}) differs from the "
                            f"table span")

    compared = 0
    if against:
        against_idx = resolve_against(against, archive.stem)
        source = parse_archive(against_idx)
        if source.count != archive.count:
            problems.append(f"entry count {archive.count} != {source.count} in "
                            f"{against_idx}")
        idx_diff = _positional_diff(read_bytes(archive.idx_path, "index"),
                                    read_bytes(source.idx_path, "index"))
        dat_diff = _positional_diff(read_bytes(archive.dat_path, "data"),
                                    read_bytes(source.dat_path, "data"))
        changed: list[dict] = []
        for i in range(min(archive.count, source.count)):
            new, old = archive.records[i], source.records[i]
            if new.name_bytes != old.name_bytes or new.blob != old.blob:
                changed.append({"index": i, "name": old.name,
                                "old_size": old.size, "new_size": new.size})
            else:
                compared += 1
        info.update({"against": os.path.abspath(against_idx),
                     "idx_identical": idx_diff["delta"] == 0
                     and idx_diff["changed_bytes"] == 0,
                     "dat_identical": dat_diff["delta"] == 0
                     and dat_diff["changed_bytes"] == 0,
                     "idx_diff": idx_diff, "dat_diff": dat_diff,
                     "changed_entries": changed,
                     "others_identical": compared})
    ok = not problems
    info.update({"problems": problems, "warnings": warnings, "ok": ok})
    lines = [f"check: {archive.idx_path}",
             f"  {archive.count} entries, {archive.dat_size} bytes of data, "
             f"index {idx_size} bytes, BMP payloads: {bmp_ok}/{archive.count}"]
    if against:
        lines.append(f"  against {info['against']}: idx identical="
                     f"{info['idx_identical']}, DAT identical="
                     f"{info['dat_identical']}")
        if not info["dat_identical"]:
            lines.append(f"  DAT: {info['dat_diff']['delta']:+d} bytes, "
                         f"{info['dat_diff']['changed_bytes']} positional byte(s) "
                         f"differ, first at {info['dat_diff']['first_diff']}")
        if not info["idx_identical"]:
            lines.append(f"  IDX: {info['idx_diff']['delta']:+d} bytes, "
                         f"{info['idx_diff']['changed_bytes']} positional byte(s) "
                         f"differ, first at {info['idx_diff']['first_diff']}")
        for change in info["changed_entries"][:6]:
            lines.append(f"  entry {change['index']} {change['name']} changed: "
                         f"{change['old_size']} -> {change['new_size']} bytes")
        if len(info["changed_entries"]) > 6:
            lines.append(f"  ... {len(info['changed_entries']) - 6} more changed entries")
        lines.append(f"  other entries identical: {compared}/"
                     f"{min(archive.count, source.count) - len(info['changed_entries'])}")
    for problem in problems:
        lines.append(f"  PROBLEM: {problem}")
    for warning in warnings[:12]:
        lines.append(f"  warning: {warning}")
    if len(warnings) > 12:
        lines.append(f"  ... {len(warnings) - 12} more warnings (see --json)")
    lines.append(f"  result: {'ok' if ok else 'FAILED'}")
    emit(info, as_json, lines)
    return 0 if ok else 1


def check_out_dir(out_dir: str, game: str | None, flag: str) -> None:
    """Refuse writes into the installation, its mirror or this repository."""
    if not out_dir:
        raise BmpWriteError(f"{flag} is empty")
    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        raise BmpWriteError(f"{flag} is a file, not a directory: {out_dir}")
    guards = [("game installation", game or DEFAULT_GAME),
              ("game mirror", DEFAULT_MIRROR)]
    guards += [("repository", root) for root in REPO_ROOTS]
    for label, root in guards:
        if root and inside(out_dir, root):
            raise BmpWriteError(
                f"refusing to write into the {label} ({os.path.realpath(out_dir)} "
                f"is inside {os.path.realpath(root)}); write to /tmp/opencode, "
                f"/var/tmp, build/ or out/")


def default_archive(game: str | None) -> str:
    if game:
        candidate = os.path.join(game, DEFAULT_ARCHIVE)
        if os.path.isfile(candidate):
            return candidate
    raise BmpWriteError(
        f"no archive given and {DEFAULT_ARCHIVE} is not under --game "
        f"({game or DEFAULT_GAME}); pass --archive FILE.IDX")


def extract_entry(archive: Archive, name: str, out_dir: str, *,
                  game: str | None, force: bool) -> dict:
    """Write one payload verbatim as ``out_dir/<NAME>.bmp``."""
    index = archive.find(name)
    if index is None:
        raise BmpWriteError(
            f"entry {name!r} not found in {archive.idx_path} "
            f"({archive.count} entries); this tool never creates entries")
    record = archive.records[index]
    check_out_dir(out_dir, game, "--out")
    out_path = os.path.join(out_dir, safe_name(record.name) + ".bmp")
    if os.path.exists(out_path) and not force:
        raise BmpWriteError(f"refusing to overwrite {out_path} (use --force)")
    write_bytes(out_path, record.blob)
    written = read_bytes(out_path, "extracted payload")
    if written != record.blob:
        raise VerifyError(f"{out_path} does not match the archive payload")
    try:
        info = bmp_header_info(record.blob)
    except BmpPayloadError as exc:
        info = {}
        note = f"not a BMP: {exc}"
    else:
        note = ""
    return {
        "source": archive.idx_path,
        "name": record.name,
        "index": index,
        "size": record.size,
        "out": out_path,
        "width": info.get("width"),
        "height": info.get("height"),
        "bpp": info.get("bpp"),
        "compression": info.get("compression"),
        "bytes_written": len(written),
        "note": note,
        "verbatim": True,
        "ok": True,
    }


def cmd_extract(archive: Archive, name: str, out_dir: str, *,
                game: str | None, force: bool, as_json: bool) -> int:
    result = extract_entry(archive, name, out_dir, game=game, force=force)
    entry_line = (f"  entry {result['index']} {result['name']}: "
                  f"{result['size']} bytes")
    if result["width"] is not None:
        entry_line += (f", {result['width']}x{result['height']}, bpp "
                       f"{result['bpp']}, "
                       f"{COMPRESSION_NAMES.get(result['compression'], '?')}")
    lines = [
        f"extract: {archive.idx_path} -> {result['out']}",
        entry_line,
        f"  bytes written: {result['bytes_written']} (verbatim, no re-encoding)",
    ]
    if result["note"]:
        lines.append(f"  note: {result['note']}")
    emit({"mode": "extract", **result}, as_json, lines)
    return 0


def replace_entry(archive: Archive, name: str, image_path: str, out_dir: str, *,
                  game: str | None, force: bool) -> dict:
    """Replace one payload with raw BMP bytes; verify every other entry."""
    index = archive.find(name)
    if index is None:
        raise BmpWriteError(
            f"entry {name!r} not found in {archive.idx_path} "
            f"({archive.count} entries); this tool never creates entries")
    old = archive.records[index]
    blob = read_bytes(image_path, "image")
    try:
        info = bmp_header_info(blob)
    except BmpPayloadError as exc:
        raise BmpWriteError(
            f"{image_path}: not a BMP payload ({exc}); this tool stores bytes "
            f"verbatim and does not re-encode images") from exc
    check_out_dir(out_dir, game, "--out-dir")
    try:
        old_info = bmp_header_info(old.blob)
    except BmpPayloadError:
        old_info = {}

    records, blobs = archive.replaced(index, blob)
    idx_bytes, dat_bytes = archive.to_bytes(blobs, records)
    out_idx, out_dat = out_paths(out_dir, archive.stem)
    if os.path.abspath(out_idx) == archive.idx_path or \
            os.path.abspath(out_dat) == archive.dat_path:
        raise BmpWriteError(f"output pair would overwrite the input archive "
                            f"({out_idx}); choose another --out-dir")
    write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=force)
    src_idx_bytes = read_bytes(archive.idx_path, "index")
    src_dat_bytes = read_bytes(archive.dat_path, "data")
    idx_diff = _positional_diff(idx_bytes, src_idx_bytes)
    dat_diff = _positional_diff(dat_bytes, src_dat_bytes)

    problems: list[str] = []
    out = parse_archive(out_idx)
    others = 0
    if out.count != archive.count:
        problems.append(f"entry count {out.count} != {archive.count}")
    for i, record in enumerate(archive.records):
        if i >= out.count:
            problems.append(f"entry {i} ({record.name!r}) is missing from the output")
            continue
        if i == index:
            if out.records[i].blob != blob:
                problems.append("target entry differs from the image file")
            continue
        if out.records[i].name_bytes != record.name_bytes:
            problems.append(f"entry {i} name changed")
        if out.records[i].blob != record.blob:
            problems.append(f"entry {i} ({record.name!r}) changed")
        else:
            others += 1
    # The oracle must read the new pair and see the same names/blobs.
    out_names, out_blobs, out_size = oracle_blobs(out_idx)
    if out_names != [record.name for record in archive.records]:
        problems.append("oracle: name table differs")
    if out_size != len(dat_bytes):
        problems.append(f"oracle: DAT size {out_size} != {len(dat_bytes)}")
    if len(out_blobs) <= index:
        problems.append(f"oracle: only {len(out_blobs)} blobs, no target at {index}")
    else:
        if out_blobs[index] != blob:
            problems.append("oracle: target blob differs from the image file")
        if out_names[index] != old.name:
            problems.append(f"oracle: target name {out_names[index]!r} != "
                            f"{old.name!r}")
    idx_again, dat_again = out.to_bytes()
    idempotent = (idx_again == read_bytes(out_idx, "index")
                  and dat_again == read_bytes(out_dat, "data"))
    if not idempotent:
        problems.append("re-reading the output and rebuilding it is not "
                        "byte-identical")
    return {
        "source": archive.idx_path,
        "name": old.name,
        "index": index,
        "old_size": old.size,
        "new_size": len(blob),
        "delta": len(blob) - old.size,
        "old_width": old_info.get("width"),
        "old_height": old_info.get("height"),
        "width": info["width"],
        "height": info["height"],
        "bpp": info["bpp"],
        "compression": info["compression"],
        "dimensions_changed": (old_info.get("width"), old_info.get("height"))
                              != (info["width"], info["height"]),
        "out_idx": out_idx,
        "out_dat": out_dat,
        "count": out.count,
        "others_identical": others,
        "target_identical": index < out.count and out.records[index].blob == blob,
        "oracle_agrees": out_names == [r.name for r in archive.records],
        "idempotent": idempotent,
        "idx_changed_bytes": idx_diff["changed_bytes"],
        "dat_changed_bytes": dat_diff["changed_bytes"],
        "idx_delta": idx_diff["delta"],
        "dat_delta": dat_diff["delta"],
        "idx_identical": idx_diff["delta"] == 0 and idx_diff["changed_bytes"] == 0,
        "dat_identical": dat_diff["delta"] == 0 and dat_diff["changed_bytes"] == 0,
        "problems": problems,
        "ok": not problems,
    }


def cmd_replace(archive: Archive, name: str, image_path: str, out_dir: str, *,
                game: str | None, force: bool, as_json: bool) -> int:
    result = replace_entry(archive, name, image_path, out_dir, game=game,
                           force=force)
    lines = [
        f"replace: {archive.idx_path} -> {result['out_idx']}",
        f"  entry {result['index']} {result['name']}: {result['old_size']} -> "
        f"{result['new_size']} bytes (delta {result['delta']:+d}), "
        f"{result['width']}x{result['height']}, bpp {result['bpp']}, "
        f"{COMPRESSION_NAMES.get(result['compression'], '?')}",
        f"  NEW: {result['out_idx']} ({os.path.getsize(result['out_idx'])} bytes)",
        f"       {result['out_dat']} ({os.path.getsize(result['out_dat'])} bytes)",
        f"  entries: {result['count']} (was {archive.count})",
        f"  others byte-identical: {result['others_identical']}/"
        f"{archive.count - 1}",
        f"  target identical: {result['target_identical']}  "
        f"oracle agrees: {result['oracle_agrees']}  "
        f"idempotent: {result['idempotent']}",
        f"  changed bytes vs source: IDX {result['idx_changed_bytes']} "
        f"({result['idx_delta']:+d}), DAT {result['dat_changed_bytes']} "
        f"({result['dat_delta']:+d})",
        f"  files identical to source: IDX {result['idx_identical']}, "
        f"DAT {result['dat_identical']}",
    ]
    for problem in result["problems"]:
        lines.append(f"  PROBLEM: {problem}")
    emit({"mode": "replace", **result}, as_json, lines)
    return 0 if result["ok"] else 1


# ---------------------------------------------------------------------------
# self-test (synthetic archives, no game files)
# ---------------------------------------------------------------------------

def _synth_pair(entries, *, prefix: bytes = b"", pad_after: dict | None = None,
                idx_tail: bytes = b"", sep: int = SEP) -> tuple[bytes, bytes]:
    """Build ``(idx, dat)`` by hand so odd layouts can be tested."""
    pad_after = pad_after or {}
    idx = bytearray(MAGIC + bytes([sep]) + struct.pack("<I", len(entries)))
    dat = bytearray(prefix)
    for i, (name_bytes, blob) in enumerate(entries):
        idx += name_bytes + struct.pack("<I", len(dat))
        dat += blob
        dat += pad_after.get(i, b"")
    idx += idx_tail
    return bytes(idx), bytes(dat)


def _write_pair_raw(directory: str, stem: str, idx: bytes, dat: bytes) -> str:
    idx_path = os.path.join(directory, stem + ".IDX")
    write_bytes(idx_path, idx)
    write_bytes(os.path.join(directory, stem + ".DAT"), dat)
    return idx_path


def _synth_palette() -> bytes:
    """A 256-entry RGBX table (BMP stores it as B,G,R,X)."""
    return bytes(value for i in range(256)
                 for value in (i, (i * 3) & 0xFF, 255 - i, 0))


def _bmp_palette_bgra(palette: bytes) -> bytes:
    raw = palette[:1024].ljust(1024, b"\0")
    return b"".join(raw[i + 2:i + 3] + raw[i + 1:i + 2] + raw[i:i + 1]
                    + raw[i + 3:i + 4] for i in range(0, 1024, 4))


def _synth_bmp(width: int, height: int, *, bpp: int = 8,
               top_down: bool = False, declared_size: int | None = None,
               size_image: int | None = None, clr_used: int | None = None,
               planes: int = 1, compression: int = BI_RGB,
               trailing: bytes = b"", magic: bytes = b"BM") -> bytes:
    """Minimal, structurally valid BMP (8- or 4-bit) for the self-test."""
    if width <= 0 or height <= 0:
        raise BmpWriteError(f"{width}x{height} is not a BMP size")
    if bpp == 8:
        stride = (width + 3) & ~3
        rows = []
        for y in range(height):
            pixels = bytes(((x * 7 + y * 13 + x * y * 3) & 0xFF)
                           for x in range(width))
            rows.append(pixels.ljust(stride, b"\0"))
        palette = _bmp_palette_bgra(_synth_palette())
        clr = 256 if clr_used is None else clr_used
    elif bpp == 4:
        stride = ((width * 4 + 31) // 32) * 4
        rows = []
        for y in range(height):
            raw = bytearray(stride)
            for x in range(width):
                value = (x * 3 + y * 5) & 0x0F
                if x % 2 == 0:
                    raw[x // 2] |= value << 4
                else:
                    raw[x // 2] |= value
            rows.append(bytes(raw))
        palette = _bmp_palette_bgra(_synth_palette()[:64])
        clr = 16 if clr_used is None else clr_used
    else:
        raise BmpWriteError(f"self-test writer does not do {bpp} bpp")
    data = b"".join(rows if top_down else reversed(rows))
    palette_bytes = b"" if bpp > 8 else palette
    offset = FILE_HEADER + DIB_MIN + len(palette_bytes)
    raw_height = -height if top_down else height
    total = offset + len(data) + len(trailing)
    info = struct.pack("<IiiHHIIiiII", DIB_MIN, width, raw_height, planes, bpp,
                       compression,
                       (len(data) if size_image is None else size_image),
                       2835, 2835, clr, 0)
    file_size = total if declared_size is None else declared_size
    return (magic + struct.pack("<IHHI", file_size, 0, 0, offset) + info
            + palette_bytes + data + trailing)


def _expect_error(fn, needle: str) -> tuple[bool, str]:
    try:
        fn()
    except (BmpWriteError, BmpPayloadError) as exc:
        text = str(exc)
        return needle.lower() in text.lower(), text
    except Exception as exc:  # noqa: BLE001 - the point is to catch the wrong type
        return False, f"wrong exception {type(exc).__name__}: {exc}"
    return False, "no error raised"


def run_json(fn) -> dict:
    """Run a mode in-process with ``--json`` and return the parsed payload."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = fn()
    payload = json.loads(buf.getvalue())
    payload["_exit"] = code
    return payload


def run_self_test(as_json: bool) -> int:
    checks: list[dict] = []

    def check(name: str, fn):
        try:
            detail = fn()
            ok = True
        except Exception as exc:  # noqa: BLE001 - every failure is a failed check
            detail = f"{type(exc).__name__}: {exc}"
            ok = False
        checks.append({"check": name, "ok": ok, "detail": detail})
        return ok

    with tempfile.TemporaryDirectory(prefix="bmpwrite-selftest-",
                                     dir="/var/tmp") as tmp:
        tex = _synth_bmp(8, 8)
        icon = _synth_bmp(4, 4, bpp=4)
        flip = _synth_bmp(6, 3, top_down=True)
        tiny = _synth_bmp(1, 1)

        # ---- round-trip ---------------------------------------------------
        base_entries = [(b"TEXTURE\0", tex), (b"ICON\0\0\0\0", icon),
                        (b"FLIP\0\0\0\0", flip)]
        base_idx, base_dat = _synth_pair(base_entries)
        base = _write_pair_raw(tmp, "base", base_idx, base_dat)

        def rt_plain():
            archive = parse_archive(base)
            assert archive.count == 3, archive.count
            out_idx, out_dat = archive.to_bytes()
            assert out_idx == base_idx, "IDX differs"
            assert out_dat == base_dat, "DAT differs"
            return "idx/dat byte-identical (3 entries, 8-bit, 4-bit, top-down)"
        check("roundtrip: synthetic BMP archive", rt_plain)

        def rt_prefix():
            idx, dat = _synth_pair(base_entries, prefix=b"PRE!")
            p = _write_pair_raw(tmp, "prefix", idx, dat)
            archive = parse_archive(p)
            assert archive.dat_prefix == b"PRE!"
            assert archive.to_bytes() == (idx, dat)
            return "4-byte DAT prefix preserved"
        check("roundtrip: DAT prefix preserved", rt_prefix)

        def rt_tail():
            idx, dat = _synth_pair(base_entries, idx_tail=b"\xFF" * 4)
            p = _write_pair_raw(tmp, "tail", idx, dat)
            archive = parse_archive(p)
            assert archive.idx_tail == b"\xFF" * 4
            assert archive.to_bytes() == (idx, dat)
            return "4-byte IDX tail preserved"
        check("roundtrip: IDX tail preserved", rt_tail)

        def rt_padding():
            idx, dat = _synth_pair(base_entries, pad_after={0: b"\xAA\xBB"})
            p = _write_pair_raw(tmp, "pad", idx, dat)
            archive = parse_archive(p)
            assert archive.records[0].size == len(tex) + 2
            assert archive.to_bytes() == (idx, dat)
            return "inter-blob padding stays inside the preceding extent"
        check("roundtrip: inter-blob padding preserved", rt_padding)

        def rt_empty():
            idx, dat = _synth_pair([])
            p = _write_pair_raw(tmp, "empty", idx, dat)
            archive = parse_archive(p)
            assert archive.count == 0 and archive.dat_size == 0
            assert archive.to_bytes() == (idx, dat)
            return "count 0, empty DAT, index identical"
        check("roundtrip: empty archive", rt_empty)

        def rt_oracle():
            stem = os.path.join(tmp, "oracle")
            pairs = [("ONE", tiny), ("TWO", icon), ("THREE", b"")]
            bigfile().pack(pairs, stem + ".IDX", stem + ".DAT", overwrite=True)
            archive = parse_archive(stem + ".IDX")
            out_idx, out_dat = archive.to_bytes()
            assert out_idx == read_bytes(stem + ".IDX", "idx")
            assert out_dat == read_bytes(stem + ".DAT", "dat")
            return "3 entries from gm.bigfile.pack reproduced byte for byte"
        check("roundtrip: archive written by gm.bigfile.pack", rt_oracle)

        def rt_determinism():
            archive = parse_archive(base)
            assert archive.to_bytes() == parse_archive(base).to_bytes()
            return "two independent rebuilds agree"
        check("roundtrip: rebuild is deterministic", rt_determinism)

        # ---- corpus discovery ---------------------------------------------
        def detect_content():
            sub = os.path.join(tmp, "scan", "deeper")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"RENAMED\0", icon)])
            path = _write_pair_raw(sub, "NOTBMPIDX", idx, dat)
            found, skipped = find_archives(os.path.join(tmp, "scan"))
            assert [a.idx_path for a in found] == [os.path.abspath(path)], found
            assert not skipped, skipped
            return "renamed archive accepted because every payload is a BMP"
        check("detect: BMP archive by content", detect_content)

        def detect_skip():
            sub = os.path.join(tmp, "scan2")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"JUNK\0\0\0\0", b"not a bmp at all")])
            _write_pair_raw(sub, "JUNKIDX", idx, dat)
            found, skipped = find_archives(sub)
            assert not found, found
            assert skipped and "does not look like a BMP" in skipped[0][1], skipped
            return "non-BMP archive skipped with a reason"
        check("detect: non-BMP archive is skipped", detect_skip)

        def detect_not_bigfile():
            sub = os.path.join(tmp, "scan3")
            os.makedirs(sub, exist_ok=True)
            write_bytes(os.path.join(sub, "ammo.idx"), b"499, 24\x00\x00\x00")
            found, skipped = find_archives(sub)
            assert not found, found
            assert skipped and "not a BIGFILE" in skipped[0][1], skipped
            return "a non-BIGFILE *.idx is skipped, not parsed as an archive"
        check("detect: non-BIGFILE index is skipped", detect_not_bigfile)

        # ---- structural refusals -----------------------------------------
        def bad_magic():
            idx = b"BIGFILX" + bytes([SEP]) + struct.pack("<I", 1)
            p = _write_pair_raw(tmp, "badmagic", idx, b"")
            ok, text = _expect_error(lambda: parse_archive(p), "not a BIGFILE index")
            assert ok, text
            return "wrong magic refused with the gm/bigfile.py:72 rule"
        check("error: wrong magic is refused", bad_magic)

        def short_header():
            p = _write_pair_raw(tmp, "short", MAGIC + bytes([SEP]) + b"\x00", b"")
            ok, text = _expect_error(lambda: parse_archive(p), "needs 12")
            assert ok, text
            return "short header refused"
        check("error: short header is refused", short_header)

        def short_table():
            idx = MAGIC + bytes([SEP]) + struct.pack("<I", 5) + b"A" * 12
            p = _write_pair_raw(tmp, "shorttab", idx, b"x")
            ok, text = _expect_error(lambda: parse_archive(p), "index too short")
            assert ok, text
            return "table shorter than count*12 refused"
        check("error: short index table is refused", short_table)

        def huge_count():
            idx = MAGIC + bytes([SEP]) + struct.pack("<I", 5_000_000)
            p = _write_pair_raw(tmp, "huge", idx, b"")
            ok, text = _expect_error(lambda: parse_archive(p), "implausible")
            assert ok, text
            return "count 5e6 refused"
        check("error: implausible count is refused", huge_count)

        def backwards():
            idx = (MAGIC + bytes([SEP]) + struct.pack("<I", 2)
                   + b"A\0\0\0\0\0\0\0" + struct.pack("<I", 10)
                   + b"B\0\0\0\0\0\0\0" + struct.pack("<I", 4))
            p = _write_pair_raw(tmp, "back", idx, b"x" * 20)
            ok, text = _expect_error(lambda: parse_archive(p), "goes backwards")
            assert ok, text
            return "non-monotonic offsets refused"
        check("error: backwards offsets are refused", backwards)

        def past_eof():
            idx = (MAGIC + bytes([SEP]) + struct.pack("<I", 1)
                   + b"A\0\0\0\0\0\0\0" + struct.pack("<I", 99))
            p = _write_pair_raw(tmp, "far", idx, b"x" * 4)
            ok, text = _expect_error(lambda: parse_archive(p), "beyond DAT")
            assert ok, text
            return "offset past EOF refused"
        check("error: offset past EOF is refused", past_eof)

        def missing_dat():
            p = os.path.join(tmp, "nodat.IDX")
            write_bytes(p, MAGIC + bytes([SEP]) + struct.pack("<I", 0))
            ok, text = _expect_error(lambda: parse_archive(p), "data file not found")
            assert ok, text
            return "missing .DAT reported"
        check("error: missing DAT is refused", missing_dat)

        # ---- BMP header model ---------------------------------------------
        def headers():
            info = bmp_header_info(tex)
            assert (info["bpp"], info["width"], info["height"]) == (8, 8, 8), info
            icon_info = bmp_header_info(icon)
            assert (icon_info["bpp"], icon_info["palette_entries"]) == (4, 16), icon_info
            flip_info = bmp_header_info(flip)
            assert flip_info["top_down"] and flip_info["height"] == 3, flip_info
            assert flip_info["trailing"] == 0
            return "8-bit, 4-bit and top-down headers classified"
        check("bmp: header model covers 8/4-bit and top-down", headers)

        def truncated_pixels():
            ok, text = _expect_error(
                lambda: bmp_header_info(tex[:-5]), "truncated pixel data")
            assert ok, text
            return "pixel data shorter than row_stride*height refused"
        check("bmp: truncated pixel data is refused", truncated_pixels)

        def bad_bits():
            broken = bytearray(tex)
            struct.pack_into("<H", broken, 28, 7)
            ok, text = _expect_error(lambda: bmp_header_info(bytes(broken)),
                                     "unsupported bit count")
            assert ok, text
            return "bit count 7 refused"
        check("bmp: invalid bit count is refused", bad_bits)

        # ---- extraction ----------------------------------------------------
        def extract_ok():
            archive = parse_archive(base)
            out = os.path.join(tmp, "ex")
            result = extract_entry(archive, "icon", out, game=None, force=False)
            assert result["out"].endswith("ICON.bmp"), result["out"]
            assert read_bytes(result["out"], "bmp") == icon
            assert result["bpp"] == 4 and result["width"] == 4
            return "payload written verbatim (case-insensitive name lookup)"
        check("extract: payload is byte-identical", extract_ok)

        def extract_missing():
            archive = parse_archive(base)
            out = os.path.join(tmp, "ex-missing")
            ok, text = _expect_error(
                lambda: extract_entry(archive, "NOSUCH", out, game=None,
                                      force=False), "not found")
            assert ok, text
            assert not os.path.exists(os.path.join(out, "NOSUCH.bmp"))
            return "unknown name refused, nothing written"
        check("extract: unknown name is refused", extract_missing)

        def extract_protected():
            archive = parse_archive(base)
            targets = [("game", os.path.join(DEFAULT_GAME, "PICS")),
                       ("mirror", os.path.join(DEFAULT_MIRROR, "PICS"))]
            targets += [(f"repo {root}", os.path.join(root, "tmp"))
                        for root in REPO_ROOTS]
            for label, path in targets:
                ok, text = _expect_error(
                    lambda p=path: extract_entry(archive, "TEXTURE", p,
                                                 game=DEFAULT_GAME, force=True),
                    "refusing to write")
                assert ok, f"{label}: {text}"
            return (f"game, mirror and {len(REPO_ROOTS)} repository root(s) refused")
        check("extract: protected output directories", extract_protected)

        def extract_overwrite():
            archive = parse_archive(base)
            out = os.path.join(tmp, "ex-twice")
            extract_entry(archive, "TEXTURE", out, game=None, force=False)
            ok, text = _expect_error(
                lambda: extract_entry(archive, "TEXTURE", out, game=None,
                                      force=False), "refusing to overwrite")
            assert ok, text
            result = extract_entry(archive, "TEXTURE", out, game=None, force=True)
            assert result["ok"]
            return "existing file needs --force"
        check("extract: existing output needs --force", extract_overwrite)

        # ---- replacement ----------------------------------------------------
        def replace_same():
            archive = parse_archive(base)
            out = os.path.join(tmp, "same")
            image = os.path.join(tmp, "same.bmp")
            write_bytes(image, tex)                 # the source payload itself
            result = replace_entry(archive, "TEXTURE", image, out, game=None,
                                   force=False)
            assert result["problems"] == [], result["problems"]
            assert result["idx_changed_bytes"] == 0, result["idx_changed_bytes"]
            assert result["dat_changed_bytes"] == 0, result["dat_changed_bytes"]
            assert result["idx_identical"] and result["dat_identical"]
            assert read_bytes(result["out_idx"], "idx") == base_idx
            assert read_bytes(result["out_dat"], "dat") == base_dat
            return "same payload: 0 changed bytes, both files identical"
        check("replace: same payload keeps both files identical", replace_same)

        def replace_grow():
            archive = parse_archive(base)
            image = os.path.join(tmp, "grow.bmp")
            write_bytes(image, _synth_bmp(16, 16))
            out = os.path.join(tmp, "grow")
            result = replace_entry(archive, "ICON", image, out, game=None,
                                   force=False)
            assert result["problems"] == [], result["problems"]
            assert result["others_identical"] == 2, result["others_identical"]
            assert result["new_size"] > result["old_size"]
            assert result["delta"] > 0
            assert result["target_identical"]
            out_archive = parse_archive(result["out_idx"])
            assert out_archive.records[0].blob == tex
            assert out_archive.records[2].blob == flip
            assert out_archive.records[1].blob == read_bytes(image, "image")
            return (f"{result['old_size']} -> {result['new_size']} bytes, "
                    f"2/2 others identical")
        check("replace: grow keeps every other entry", replace_grow)

        def replace_shrink():
            archive = parse_archive(base)
            image = os.path.join(tmp, "shrink.bmp")
            write_bytes(image, _synth_bmp(2, 2, bpp=4))
            out = os.path.join(tmp, "shrink")
            result = replace_entry(archive, "TEXTURE", image, out, game=None,
                                   force=False)
            assert result["problems"] == [], result["problems"]
            assert result["others_identical"] == 2
            assert result["new_size"] < result["old_size"]
            return f"{result['old_size']} -> {result['new_size']} bytes"
        check("replace: shrink keeps every other entry", replace_shrink)

        def replace_missing():
            archive = parse_archive(base)
            out = os.path.join(tmp, "missing")
            ok, text = _expect_error(
                lambda: replace_entry(archive, "NOSUCH", os.path.join(tmp,
                                                                      "grow.bmp"),
                                      out, game=None, force=False), "not found")
            assert ok, text
            assert not os.path.exists(os.path.join(out, "BMP.IDX")), \
                "an output pair was written for a refused replacement"
            return "unknown name refused, nothing written"
        check("replace: unknown name is refused", replace_missing)

        def replace_bad_image():
            archive = parse_archive(base)
            png = os.path.join(tmp, "notbmp.bin")
            write_bytes(png, b"\x89PNG\r\n\x1a\n" + bytes(64))
            ok, text = _expect_error(
                lambda: replace_entry(archive, "TEXTURE", png,
                                      os.path.join(tmp, "png"), game=None,
                                      force=False), "not a BMP payload")
            assert ok, text
            return "a non-BMP payload is refused (no re-encoding)"
        check("replace: non-BMP image is refused", replace_bad_image)

        def replace_overwrite():
            archive = parse_archive(base)
            image = os.path.join(tmp, "twice.bmp")
            write_bytes(image, _synth_bmp(4, 4))
            out = os.path.join(tmp, "twice")
            replace_entry(archive, "ICON", image, out, game=None, force=False)
            ok, text = _expect_error(
                lambda: replace_entry(archive, "ICON", image, out, game=None,
                                      force=False), "refusing to overwrite")
            assert ok, text
            result = replace_entry(archive, "ICON", image, out, game=None,
                                   force=True)
            assert result["ok"], result["problems"]
            return "existing pair needs --force"
        check("replace: existing output needs --force", replace_overwrite)

        # ---- check mode ------------------------------------------------------
        def check_clean():
            payload = run_json(lambda: cmd_check(base, None, None, True))
            assert payload["ok"], payload["problems"]
            assert payload["count"] == 3
            assert payload["bmp_payloads_ok"] == 3
            return "clean archive: 0 problems, 3 entries, 3 BMP payloads"
        check("--check: clean archive passes", check_clean)

        def check_against():
            archive = parse_archive(base)
            image = os.path.join(tmp, "against.bmp")
            write_bytes(image, _synth_bmp(16, 16))
            result = replace_entry(archive, "ICON", image,
                                   os.path.join(tmp, "against"), game=None,
                                   force=False)
            assert result["ok"], result["problems"]
            payload = run_json(lambda: cmd_check(
                result["out_idx"], None, base, True))
            assert payload["ok"], payload["problems"]
            assert payload["others_identical"] == 2, payload["others_identical"]
            assert len(payload["changed_entries"]) == 1, payload["changed_entries"]
            assert not payload["dat_identical"] and not payload["idx_identical"]
            return "exactly 1 changed entry, 2/2 others identical"
        check("--check --against: entry-level diff", check_against)

        def check_truncated():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "truncated")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True)
            write_bytes(p + ".DAT", od[:-5])
            payload = run_json(lambda: cmd_check(p + ".IDX", None, None, True))
            assert not payload["ok"], "a truncated DAT passed --check"
            assert any("not a BMP" in text or "truncated" in text
                       for text in payload["problems"]), payload["problems"]
            return "a truncated DAT is detected through the BMP header"
        check("--check: truncated DAT detected", check_truncated)

        def check_tail_bytes():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "extra")
            write_pair(p + ".IDX", p + ".DAT", oi, od + b"\x00" * 8, force=True)
            payload = run_json(lambda: cmd_check(p + ".IDX", None, None, True))
            assert not payload["ok"], "extra DAT bytes passed --check"
            assert any("trailing byte(s)" in text
                       for text in payload["problems"]), payload["problems"]
            return "bytes appended to the last entry are reported"
        check("--check: trailing bytes after the pixel array", check_tail_bytes)

        def check_tamper():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "tamper")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True)
            broken = bytearray(oi)
            off = struct.unpack_from("<I", broken, HEADER_SIZE + RECORD_SIZE + 8)[0]
            struct.pack_into("<I", broken, HEADER_SIZE + RECORD_SIZE + 8, off + 1)
            write_bytes(p + ".IDX", bytes(broken))
            payload = run_json(lambda: cmd_check(p + ".IDX", None, base, True))
            # The shifted boundary breaks the BMP extents *and* shows up in
            # --against: two independent detections of the same forgery.
            assert not payload["ok"], payload["problems"]
            assert not payload["idx_identical"], payload["idx_diff"]
            assert payload["changed_entries"], payload
            assert any("BMP" in text or "trailing" in text
                       for text in payload["problems"]), payload["problems"]
            return "a forged offset breaks the BMP extents and --against sees it"
        check("--check: forged offset is detected", check_tamper)

        def check_rename():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "rename")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True)
            broken = bytearray(oi)
            broken[HEADER_SIZE + RECORD_SIZE:HEADER_SIZE + RECORD_SIZE + 4] = b"ICOX"
            write_bytes(p + ".IDX", bytes(broken))
            payload = run_json(lambda: cmd_check(p + ".IDX", None, base, True))
            assert payload["ok"], payload["problems"]     # structure is intact
            assert not payload["idx_identical"], payload["idx_diff"]
            assert len(payload["changed_entries"]) == 1, payload["changed_entries"]
            return "a renamed entry passes the structure check, --against names it"
        check("--check --against: renamed entry is visible", check_rename)

        # ---- list / audit ----------------------------------------------------
        def list_mode():
            payload = run_json(lambda: cmd_list(base, 5, True))
            assert payload["count"] == 3
            indexes = [row["index"] for row in payload["entries"]]
            assert indexes == [0, 1, 2], indexes      # both ends, no duplicates
            assert payload["entries"][0]["width"] == 8
            assert payload["entries"][1]["bpp"] == 4
            return "first/last entries, no duplicate rows on a short archive"
        check("--list: entries and dimensions", list_mode)

        def audit_mode():
            sub = os.path.join(tmp, "audit")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"A\0\0\0\0\0\0\0", tex),
                                    (b"B\0\0\0\0\0\0\0", b""),
                                    (b"C\0\0\0\0\0\0\0", icon)])
            _write_pair_raw(sub, "BMP", idx, dat)
            payload = run_json(lambda: cmd_audit(sub, True))
            assert payload["ok"], payload["failed"]
            row = payload["archives"][0]
            assert row["count"] == 3 and row["empty_entries"] == 1, row
            assert row["size_min"] == 0 and row["size_max"] == len(tex), row
            assert row["size_median"] == len(icon), row
            assert row["bpp"] == {"4": 1, "8": 1}, row["bpp"]
            assert row["bpp_all"] == {"1": 0, "4": 1, "8": 1, "16": 0, "24": 0,
                                      "32": 0}, row["bpp_all"]
            assert row["rle"] == 0
            assert row["compression"] == {"0": 2}, row["compression"]
            assert row["bmp_header_ok"] == 2 and row["unique_dimensions"] == 2
            assert row["gm_bmp_decode_fail"] == 1, row
            return (f"3 entries, empty=1, median={row['size_median']}, "
                    f"bpp {row['bpp']}, gm.bmp decoded {row['gm_bmp_decode_ok']}")
        check("--audit: min/median/max, bpp and decoder counts", audit_mode)

        def audit_not_bmp():
            sub = os.path.join(tmp, "audit2")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"JUNK\0\0\0\0", b"junk payload")])
            _write_pair_raw(sub, "NOTBMP", idx, dat)
            found, skipped = find_archives(sub)
            assert not found and skipped, (found, skipped)
            idx, dat = _synth_pair([(b"JUNK\0\0\0\0", b"junk payload")])
            _write_pair_raw(sub, "BMP", idx, dat)
            payload = run_json(lambda: cmd_audit(sub, True))
            assert not payload["ok"], "a non-BMP payload passed --audit"
            row = payload["archives"][0]
            assert row["bmp_header_fail"] == 1 and row["not_bmp"], row
            return "an archive named BMP still reports a non-BMP payload"
        check("--audit: non-BMP payload is reported", audit_not_bmp)

        def json_every_mode():
            sub = os.path.join(tmp, "json")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"ONE\0\0\0\0\0", tex)])
            _write_pair_raw(sub, "BMP", idx, dat)
            for payload in (run_json(lambda: cmd_list(base, 5, True)),
                            run_json(lambda: cmd_check(base, None, None, True)),
                            run_json(lambda: cmd_audit(sub, True)),
                            run_json(lambda: cmd_roundtrip(sub, tmp, True))):
                assert isinstance(payload, dict) and "ok" in payload, payload
            return "--list/--check/--audit/--roundtrip emit one JSON object with ok"
        check("--json: machine-readable output", json_every_mode)

    passed = sum(1 for item in checks if item["ok"])
    total = len(checks)
    lines = [f"{'ok  ' if item['ok'] else 'FAIL'} {item['check']}: {item['detail']}"
             for item in checks]
    lines.append(f"self-test: {passed}/{total} checks passed")
    payload = {"mode": "self-test", "checks": checks, "passed": passed,
               "total": total, "ok": passed == total}
    emit(payload, as_json, lines)
    return 0 if passed == total else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bmpwrite.py",
        description="Round-trip, audit and edit the Gunmetal FRONTEND/BMP.IDX "
                    "picture archive (a BIGFILE pair of Windows BMP files).")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--roundtrip", nargs="?", const="", metavar="DIR",
                      help="repack every BMP archive under DIR (default: --game) "
                           "and compare both files byte for byte")
    mode.add_argument("--audit", nargs="?", const="", metavar="DIR",
                      help="corpus numbers: entries, sizes, names, BMP variants")
    mode.add_argument("--list", nargs="?", const="", metavar="ARCHIVE.IDX",
                      help="print the first and last entries")
    mode.add_argument("--extract", metavar="NAME",
                      help="write one payload verbatim as NAME.bmp")
    mode.add_argument("--replace", metavar="NAME",
                      help="replace one payload with a raw BMP, write a new pair")
    mode.add_argument("--check", metavar="ARCHIVE.IDX",
                      help="verify the structure and (with --against) a rewrite")
    mode.add_argument("--self-test", action="store_true",
                      help="synthetic archives only, no game files")
    parser.add_argument("--archive", metavar="ARCHIVE.IDX",
                        help="index for --extract/--replace (default "
                             "FRONTEND/BMP.IDX under --game)")
    parser.add_argument("--image", metavar="FILE.bmp",
                        help="new BMP payload for --replace (stored verbatim)")
    parser.add_argument("--out", metavar="DIR",
                        help="output directory for --extract")
    parser.add_argument("--out-dir", metavar="DIR",
                        help="output directory for --replace (DIR/BMP.IDX+.DAT)")
    parser.add_argument("--dat", metavar="FILE.DAT",
                        help="explicit data file (default: the .IDX sibling)")
    parser.add_argument("--against", metavar="DIR",
                        help="--check: compare both files and entries with the "
                             "pair in DIR (or an .IDX)")
    parser.add_argument("--game", default=DEFAULT_GAME,
                        help=f"installation directory (default {DEFAULT_GAME}, "
                             f"$GUNMETAL_GAME)")
    parser.add_argument("--tmp-dir",
                        default=os.environ.get("BMPWRITE_TMP")
                        or ("/tmp/opencode" if os.path.isdir("/tmp/opencode")
                            else "/var/tmp"),
                        help="scratch directory for --roundtrip (default "
                             "/tmp/opencode or /var/tmp)")
    parser.add_argument("--limit", type=int, default=5,
                        help="--list: entries from each end (0 = all, default 5)")
    parser.add_argument("--force", action="store_true",
                        help="overwrite existing output files")
    parser.add_argument("--json", action="store_true",
                        help="one JSON line of output")
    return parser


def resolve_archive_path(path: str | None, game: str | None) -> str | None:
    """``FRONTEND/BMP.IDX`` means the file in the *installation*, like the engine."""
    if not path or os.path.isabs(path) or os.path.exists(path):
        return path
    if game and os.path.isfile(os.path.join(game, path)):
        return os.path.join(game, path)
    return path


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    game = args.game if args.game and os.path.isdir(args.game) else None
    args.list = resolve_archive_path(args.list, game)
    args.check = resolve_archive_path(args.check, game)
    args.archive = resolve_archive_path(args.archive, game)
    args.against = resolve_archive_path(args.against, game)
    args.dat = resolve_archive_path(args.dat, game)
    args.image = resolve_archive_path(args.image, game)
    try:
        if args.self_test:
            return run_self_test(args.json)
        if args.roundtrip is not None:
            directory = args.roundtrip or args.game or DEFAULT_GAME
            return cmd_roundtrip(directory, args.tmp_dir, args.json)
        if args.audit is not None:
            directory = args.audit or args.game or DEFAULT_GAME
            return cmd_audit(directory, args.json)
        if args.list is not None:
            path = args.list or args.archive or default_archive(game)
            return cmd_list(path, args.limit, args.json)
        if args.extract is not None:
            archive = parse_archive(args.archive or default_archive(game))
            if not args.out:
                raise BmpWriteError("--extract needs --out DIR")
            return cmd_extract(archive, args.extract, args.out, game=game,
                               force=args.force, as_json=args.json)
        if args.replace is not None:
            archive = parse_archive(args.archive or default_archive(game))
            for flag, value in (("--image", args.image),
                                ("--out-dir", args.out_dir)):
                if not value:
                    raise BmpWriteError(f"--replace needs {flag}")
            return cmd_replace(archive, args.replace, args.image, args.out_dir,
                               game=game, force=args.force, as_json=args.json)
        if args.check:
            return cmd_check(args.check, args.dat, args.against, args.json)
        raise BmpWriteError("no mode selected")  # pragma: no cover - argparse
    except VerifyError as exc:
        print(f"bmpwrite: verification failed: {exc}", file=sys.stderr)
        return 1
    except BmpWriteError as exc:
        print(f"bmpwrite: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:  # pragma: no cover
        print("bmpwrite: interrupted", file=sys.stderr)
        return 130
    except Exception as exc:  # noqa: BLE001 - internal error, not a refusal
        print(f"bmpwrite: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
