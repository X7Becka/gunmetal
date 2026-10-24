#!/usr/bin/env python3
"""Round-trip, audit and single-entry replacement for Gunmetal *SBM archives*.

An "SBM archive" is a shipped BIGFILE pair (``.IDX`` + ``.DAT``) whose blobs
are ``.sbm`` textures or sprites.  In the installation there are exactly two:

===============  =======  ===========  ====================================
index              count     DAT bytes  payload
===============  =======  ===========  ====================================
``PICS/SBM.IDX``    1648     45442175   textures, mip chains 1..9 levels
``sprites/SBM.IDX``  179      1083044   sprites, one level, flags 256/257
===============  =======  ===========  ====================================

Format facts live in ``docs/FORMATS.md`` §1 (BIGFILE: 12-byte header, 12-byte
records ``char name[8]`` + ``u32 offset``, DAT is a raw concatenation) and §2
(``.sbm``: 12-byte header, mip chain).  The **reader/oracle** is
``gm/bigfile.py`` + ``gm/sbm.py`` -- neither is modified here.  This module is
the writer/verifier side: it rebuilds both files byte for byte, audits the
corpus, and replaces one entry with an image while proving that every other
entry survived.

Everything the writer keeps verbatim (it is *outside* the oracle's model):

* a DAT prefix before ``offset[0]`` (copied to the front of the new ``.DAT``);
* a ``.IDX`` tail after the record table (copied after the new table);
* padding between blobs (it belongs to the preceding entry's extent
  ``offset[i+1] - offset[i]``, so it travels inside that blob);
* the raw ``name[8]`` bytes, the magic and the ``0x1A`` separator.

Recalculated on replacement: only the ``u32`` offsets (sizes are implicit in
neighbouring offsets) and, never, ``count`` -- this tool does not create or
delete entries.

Usage::

    python3 tools/editor/sbmwrite.py --roundtrip [DIR]          # all SBM archives
    python3 tools/editor/sbmwrite.py --audit [DIR]              # corpus numbers
    python3 tools/editor/sbmwrite.py --list PICS/SBM.IDX        # first/last 5
    python3 tools/editor/sbmwrite.py --check out/SBM.IDX [--dat out/SBM.DAT]
    python3 tools/editor/sbmwrite.py --check out/SBM.IDX --against PICS/SBM.IDX \\
            [--name NAME] [--expect FILE.bmp]
    python3 tools/editor/sbmwrite.py --replace sprites/SBM.IDX --name AFLM0001 \\
            --image new.bmp --out-dir /tmp/opencode/sbmwrite/sprites
    python3 tools/editor/sbmwrite.py --self-test [--json]

``--json`` turns every mode into one JSON line.  Exit codes: ``0`` success,
``1`` verification failed (round-trip/check/audit/replace/self-test), ``2``
bad input or a refused write, ``3`` internal error.  Writing into the game
installation, its mirror or this repository is refused unconditionally.
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

MAGIC = b"BIGFILE"
SEP = 0x1A
HEADER_SIZE = 12
RECORD_SIZE = 12
NAME_SIZE = 8
UINT32_MAX = 0xFFFFFFFF
MAX_ENTRIES = 1_000_000          # same trust boundary as gm/bigfile.py:32

#: A ``.sbm`` blob needs at least this much: 12-byte header + one texel.
SB_HEADER = 12


class SbmWriteError(Exception):
    """Bad input, a refused write or a structurally broken archive (exit 2)."""


class VerifyError(Exception):
    """A written/checked archive failed its verification (exit 1)."""


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def read_bytes(path: str, what: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise SbmWriteError(
            f"cannot read {what} {path}: {exc.strerror or exc}") from exc


def write_bytes(path: str, data: bytes) -> None:
    try:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
    except OSError as exc:
        raise SbmWriteError(f"cannot write {path}: {exc.strerror or exc}") from exc


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


def encode_name(name: str) -> bytes:
    try:
        raw = name.encode("latin1")
    except UnicodeEncodeError as exc:
        raise SbmWriteError(f"name {name!r} is not single-byte (latin1) text") from exc
    if b"\0" in raw:
        raise SbmWriteError(f"name {name!r} contains a NUL byte")
    if not 1 <= len(raw) <= NAME_SIZE:
        raise SbmWriteError(f"name must be 1..{NAME_SIZE} bytes (char name[8], "
                            f"docs/FORMATS.md §1): {name!r} is {len(raw)} bytes")
    return raw + b"\0" * (NAME_SIZE - len(raw))


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
            raise SbmWriteError(f"{len(blobs)} blobs for {len(records)} records")
        dat = bytearray(self.dat_prefix)
        idx = bytearray(self.header[:8])              # magic + 0x1A, verbatim
        idx += struct.pack("<I", len(records))
        for record, blob in zip(records, blobs):
            if not isinstance(blob, (bytes, bytearray)):
                raise SbmWriteError(f"blob for {record.name!r} is not bytes")
            offset = len(dat)
            if offset > UINT32_MAX:
                raise SbmWriteError(
                    f"payload exceeds 4 GiB offsets at {record.name!r} "
                    f"(offset {offset} > 0x{UINT32_MAX:08X}); the field is u32")
            idx += record.name_bytes + struct.pack("<I", offset)
            dat += blob
        idx += self.idx_tail
        if len(dat) > UINT32_MAX + 1:
            raise SbmWriteError(f"payload is {len(dat)} bytes; u32 offsets cap it "
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
            raise SbmWriteError(f"no such data file: {dat_path}")
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
        raise SbmWriteError(
            f"cannot list {directory}: {exc.strerror or exc}") from exc
    for name in sorted(names):
        if os.path.splitext(name)[0].lower() == stem and name.lower().endswith(".dat"):
            return os.path.join(directory, name)
    raise SbmWriteError(
        f"data file not found next to {idx_path}: tried {base}.DAT and {base}.dat "
        f"(docs/FORMATS.md §1: the pair is ``<name>.idx`` + ``<name>.dat``)")


def parse_archive(idx_path: str, dat_path: str | None = None) -> Archive:
    """Parse a BIGFILE pair with the oracle's rules, keeping raw/extra bytes."""
    idx_path = os.path.abspath(idx_path)
    if not os.path.isfile(idx_path):
        raise SbmWriteError(f"no such index: {idx_path}")
    idx = read_bytes(idx_path, "index")
    if len(idx) < HEADER_SIZE:
        raise SbmWriteError(
            f"{idx_path}: not a BIGFILE index: {len(idx)} bytes, the header alone "
            f"needs {HEADER_SIZE} (gm/bigfile.py:72)")
    if idx[:len(MAGIC)] != MAGIC:
        raise SbmWriteError(
            f"{idx_path}: not a BIGFILE index: magic is {idx[:len(MAGIC)]!r}, "
            f"expected {MAGIC!r} (gm/bigfile.py:72)")
    sep_ok = idx[len(MAGIC)] == SEP
    count = struct.unpack_from("<I", idx, 8)[0]
    if count > MAX_ENTRIES:
        raise SbmWriteError(
            f"{idx_path}: implausible entry count {count} (limit {MAX_ENTRIES}, "
            f"gm/bigfile.py:32)")
    need = HEADER_SIZE + count * RECORD_SIZE
    if len(idx) < need:
        raise SbmWriteError(
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
            raise SbmWriteError(
                f"{idx_path}: entry {i} ({name_bytes.rstrip(bytes(1))!r}) offset "
                f"{offset} beyond DAT ({dat_size} bytes) (gm/bigfile.py:95)")
        if offset < previous:
            raise SbmWriteError(
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
                raise SbmWriteError(f"refusing to overwrite {path} (use --force)")
    temps: list[str] = []
    try:
        for path, payload in ((dat_path, dat_bytes), (idx_path, idx_bytes)):
            directory = os.path.dirname(os.path.abspath(path)) or "."
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, prefix=".gm-sbmwrite-")
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
        raise SbmWriteError(f"cannot write archive: {exc.strerror or exc}") from exc


# ---------------------------------------------------------------------------
# oracles: gm.bigfile and gm.sbm (read-only)
# ---------------------------------------------------------------------------

def bigfile():
    try:
        from gm import bigfile as module
    except Exception as exc:  # pragma: no cover - broken checkout
        raise SbmWriteError(f"cannot import the oracle gm.bigfile: {exc}") from exc
    return module


def sbm():
    try:
        from gm import sbm as module
    except Exception as exc:  # pragma: no cover - broken checkout
        raise SbmWriteError(f"cannot import the oracle gm.sbm: {exc}") from exc
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


def sbm_summary(blob: bytes) -> dict:
    """Parse a blob with ``gm.sbm`` and return its measurable fields."""
    parsed = sbm().parse(blob)
    return {
        "width": parsed.width,
        "height": parsed.height,
        "levels": parsed.levels,
        "flags": parsed.flags,
        "pixels": parsed.pixels,
        "extra_bytes": len(parsed.extra),
        "bytes": len(blob),
    }


def looks_like_sbm(blob: bytes) -> bool:
    """Strict content test: 12-byte header with zeros and an exact mip chain.

    ``gm.sbm.parse`` ignores the first dword and tolerates trailing bytes, so
    it accepts unrelated payloads by accident (a Windows BMP header, for
    example, can decode as a tiny mip chain).  For *identification* require
    what the shipped `.sbm` layout guarantees (``docs/FORMATS.md`` §2): the
    leading u32 is zero, both dimensions and the level count are plausible,
    and the blob is exactly ``12 + expected_size(...)`` bytes.
    """
    if len(blob) < SB_HEADER:
        return False
    zero, log2w, log2h, lvlm1, zero2, _flags = struct.unpack_from("<IBBBBI", blob, 0)
    if zero != 0 or zero2 != 0 or log2w > 12 or log2h > 12 or lvlm1 > 12:
        return False
    return len(blob) == SB_HEADER + sbm().expected_size(log2w, log2h, lvlm1)


def not_sbm_reason(archive: Archive) -> str | None:
    """Why *archive* is not an SBM archive, or ``None`` when it is one.

    The shipped archives are named ``SBM``; an archive is also accepted by
    content when every non-empty blob passes :func:`looks_like_sbm` (that
    covers renamed copies and mods).  Empty blobs are reported by ``--audit``.
    """
    if archive.stem.lower() == "sbm":
        return None
    parsed = 0
    for i, record in enumerate(archive.records):
        if record.size == 0:
            continue
        if not looks_like_sbm(record.blob):
            return (f"entry {i} ({record.name!r}) does not look like .sbm "
                    f"(header zeros, dimensions or exact mip-chain size)")
        parsed += 1
    if parsed == 0:
        return "no .sbm payload"
    return None


# ---------------------------------------------------------------------------
# corpus discovery
# ---------------------------------------------------------------------------

def find_archives(directory: str) -> tuple[list[Archive], list[tuple[str, str]]]:
    """Every SBM ``.IDX``/``.DAT`` pair under *directory* (recursive)."""
    if not os.path.isdir(directory):
        raise SbmWriteError(f"directory not found: {directory}")
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
            except SbmWriteError as exc:
                skipped.append((path, str(exc)))
                continue
            reason = not_sbm_reason(archive)
            if reason is not None:
                skipped.append((path, reason))
                continue
            found.append(archive)
    return found, skipped


def parse_failure_reason(archive: Archive) -> str | None:
    """First blob that is not a valid ``.sbm`` (round-trip/audit gate)."""
    for i, record in enumerate(archive.records):
        if record.size == 0:
            return f"entry {i} ({record.name!r}) is empty"
        try:
            sbm().parse(record.blob)
        except ValueError as exc:
            return f"entry {i} ({record.name!r}) is not .sbm: {exc}"
    return None


# ---------------------------------------------------------------------------
# images -> .sbm blobs
# ---------------------------------------------------------------------------

def load_image(path: str):
    """Read a ``.bmp`` (8-bit palettised) or ``.png`` into ``gm.png.Image``."""
    from gm import bmp, png
    data = read_bytes(path, "image")
    low = path.lower()
    try:
        if low.endswith(".bmp"):
            bb = bmp.parse(data)
            return png.Image(bb.width, bb.height, "P", 1, bb.pixels, bb.palette)
        if low.endswith(".png"):
            return png.decode(data)
    except ValueError as exc:
        raise SbmWriteError(f"{path}: cannot decode the image: {exc}") from exc
    raise SbmWriteError(f"image must be .bmp or .png: {path}")


def resolve_palette(palette_path: str | None, game: str | None, image) -> bytes:
    """Palette for index lookup: ``--palette`` > engine palette > image palette.

    Same precedence as ``gm.py sbm`` (the oracle's encoder), so a replacement
    computed here matches what the toolkit would produce.
    """
    from gm import palette
    if palette_path:
        try:
            return palette.load_from_bmp(palette_path)
        except (OSError, ValueError) as exc:
            raise SbmWriteError(f"cannot read palette {palette_path}: {exc}") from exc
    if game and os.path.isdir(game):
        try:
            return palette.installed(game)
        except (FileNotFoundError, OSError, ValueError):
            pass
    image_palette = getattr(image, "palette", b"") or b""
    if image_palette:
        return image_palette
    raise SbmWriteError(
        "no palette: pass --palette BMP or --game DIR (an 8-bit palettised image "
        "carries one; docs/FORMATS.md §6: the .sbm blob stores no palette)")


def image_indices(image, pal: bytes) -> bytes:
    """Map an image onto palette indices (identical palette keeps indices)."""
    if len(pal) < 3:
        raise SbmWriteError("palette has no colours")
    prgb = [(pal[4 * i], pal[4 * i + 1], pal[4 * i + 2])
            for i in range(len(pal) // 4)]
    if not prgb:  # pragma: no cover - guarded above
        raise SbmWriteError("palette has no colours")
    cache: dict[tuple[int, int, int], int] = {}

    def nearest(rgb: tuple[int, int, int]) -> int:
        value = cache.get(rgb)
        if value is None:
            r, g, b = rgb
            best, best_index = 1 << 30, 0
            for i, (pr, pg, pb) in enumerate(prgb):
                distance = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                if distance < best:
                    best, best_index = distance, i
            value = cache[rgb] = best_index
        return value

    out = bytearray()
    if image.mode == "P":
        ncol = len(image.palette) // 4
        remap = []
        for i in range(ncol):
            colour = (image.palette[4 * i], image.palette[4 * i + 1],
                      image.palette[4 * i + 2])
            # An identical palette keeps its indices exactly: the shared VGA
            # table has duplicate colours, so nearest-match would not round-trip.
            remap.append(i if i < len(prgb) and prgb[i] == colour else nearest(colour))
        for value in image.pixels:
            out.append(remap[value] if value < len(remap) else 0)
    elif image.mode in ("L", "LA"):
        step = image.channels
        for i in range(0, len(image.pixels), step):
            value = image.pixels[i]
            out.append(nearest((value, value, value)))
    else:  # RGB / RGBA
        step = image.channels
        for i in range(0, len(image.pixels), step):
            out.append(nearest((image.pixels[i], image.pixels[i + 1],
                                image.pixels[i + 2])))
    return bytes(out)


def image_to_blob(image_path: str, *, palette_path: str | None, game: str | None,
                  levels: int, flags: int, mip: str,
                  image=None) -> tuple[bytes, dict]:
    """Encode an image into an ``.sbm`` blob with ``gm.sbm.encode``."""
    if not 0 <= flags <= UINT32_MAX:
        raise SbmWriteError(f"flags must be 0..0x{UINT32_MAX:08X}, got {flags}")
    image = image if image is not None else load_image(image_path)
    pal = resolve_palette(palette_path, game, image)
    indices = image_indices(image, pal)
    try:
        blob = sbm().encode(image.width, image.height, indices, levels=levels,
                            flags=flags, mip=mip)
    except ValueError as exc:
        raise SbmWriteError(f"{image_path}: {exc}") from exc
    return blob, {"width": image.width, "height": image.height,
                  "pixels": indices, "levels": levels, "flags": flags,
                  "palette_colours": len(pal) // 4}


# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------

def cmd_roundtrip(directory: str, tmp_dir: str, as_json: bool) -> int:
    archives, skipped = find_archives(directory)
    if not archives:
        raise SbmWriteError(f"no SBM .IDX/.DAT pairs under {directory}")
    os.makedirs(tmp_dir, exist_ok=True)
    rows: list[dict] = []
    failures: list[str] = []
    total_entries = total_bytes = 0
    total_seconds = 0.0
    lines = [f"round-trip: {directory}"]
    lines += [f"  skipped: {os.path.relpath(path, directory)}: {why}"
              for path, why in skipped]
    with tempfile.TemporaryDirectory(prefix="sbmwrite-rt-", dir=tmp_dir) as out:
        for archive in archives:
            rel = os.path.relpath(archive.idx_path, directory)
            started = time.perf_counter()
            try:
                idx_bytes, dat_bytes = archive.to_bytes()
            except SbmWriteError as exc:
                failures.append(f"{rel}: {exc}")
                lines.append(f"  FAIL {rel}: {exc}")
                continue
            stem = re.sub(r"[^A-Za-z0-9_.-]", "_", rel.replace(os.sep, "_"))
            out_idx, out_dat = out_paths(out, stem)
            try:
                write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=True)
            except SbmWriteError as exc:
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
                         and [n for n in names] == [r.name for r in archive.records]
                         and b"".join(blobs) == dat_bytes)
            parse_problem = parse_failure_reason(archive)
            ok = idx_same and dat_same and oracle_ok and parse_problem is None
            if not ok:
                reason = ("; ".join(filter(None, [
                    "" if idx_same else ".IDX differs",
                    "" if dat_same else ".DAT differs",
                    "" if oracle_ok else "gm.bigfile disagrees",
                    parse_problem or "",
                ])))
                failures.append(f"{rel}: {reason}")
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
                f"{archive.dat_size:>10} B  idx {os.path.getsize(archive.idx_path):>6} B  "
                f"identical={idx_same}/{dat_same}  {seconds:.2f}s")
            if parse_problem:
                lines.append(f"       PROBLEM: {parse_problem}")
            for extra, label in ((archive.idx_tail, "index tail"),
                                 (archive.dat_prefix, "DAT prefix")):
                if extra:
                    lines.append(f"       note: {len(extra)} extra {label} bytes preserved")
    ok = not failures and all(row["ok"] for row in rows)
    lines.append(f"  totals: {len(rows)} archives, {total_entries} entries, "
                 f"{total_bytes} bytes, identical={sum(r['ok'] for r in rows)}/{len(rows)}, "
                 f"{total_seconds:.2f}s")
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


def cmd_audit(directory: str, as_json: bool) -> int:
    archives, skipped = find_archives(directory)
    if not archives:
        raise SbmWriteError(f"no SBM .IDX/.DAT pairs under {directory}")
    rows: list[dict] = []
    failures: list[str] = []
    total_entries = total_bytes = total_empty = 0
    lines = [f"audit: {directory}",
             f"  {'archive':<20} {'entries':>7} {'bytes':>11} "
             f"{'min/median/max':>26} {'empty':>6} {'names':>5}  flags / levels"]
    for archive in archives:
        rel = os.path.relpath(archive.idx_path, directory)
        sizes = [record.size for record in archive.records]
        empty = sum(1 for size in sizes if size == 0)
        flags: dict[int, int] = {}
        levels: dict[int, int] = {}
        extra = 0
        parse_errors = 0
        max_name = 0
        for record in archive.records:
            max_name = max(max_name, len(record.name_bytes.rstrip(b"\0")))
            if record.size == 0:
                continue                    # reported through ``empty_entries``
            try:
                summary = sbm_summary(record.blob)
            except ValueError:
                parse_errors += 1
                continue
            flags[summary["flags"]] = flags.get(summary["flags"], 0) + 1
            levels[summary["levels"]] = levels.get(summary["levels"], 0) + 1
            extra += summary["extra_bytes"]
        if parse_errors:
            failures.append(f"{rel}: {parse_errors} blob(s) are not valid .sbm")
        flags_text = ", ".join(f"{k}:{v}" for k, v in sorted(flags.items()))
        levels_text = (f"{min(levels)}..{max(levels)}" if levels else "n/a")
        rows.append({
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
            "flags": {str(k): v for k, v in sorted(flags.items())},
            "levels_min": min(levels) if levels else 0,
            "levels_max": max(levels) if levels else 0,
            "extra_bytes": extra,
            "parse_errors": parse_errors,
        })
        total_entries += archive.count
        total_bytes += archive.dat_size
        total_empty += empty
        lines.append(
            f"  {rel:<20} {archive.count:>7} {archive.dat_size:>11} "
            f"{f'{min(sizes) if sizes else 0}/{median_text(sizes)}/{max(sizes) if sizes else 0}':>26} "
            f"{empty:>6} {max_name:>5}  {flags_text} / {levels_text}")
    lines.append(f"  totals: {len(rows)} archives, {total_entries} entries, "
                 f"{total_bytes} bytes, {total_empty} empty entries, "
                 f"name field {NAME_SIZE} bytes")
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
                   "name_field": NAME_SIZE},
        "failed": failures,
        "ok": ok,
    }
    emit(payload, as_json, lines)
    return 0 if ok else 1


def _entry_row(index: int, record: Record) -> dict:
    row = {"index": index, "name": record.name, "offset": record.offset,
           "size": record.size, "width": None, "height": None,
           "flags": None, "levels": None}
    if record.size:
        try:
            summary = sbm_summary(record.blob)
        except ValueError:
            row["note"] = "not .sbm"
        else:
            row.update({k: summary[k] for k in ("width", "height", "flags",
                                                "levels")})
            if summary["extra_bytes"]:
                row["extra_bytes"] = summary["extra_bytes"]
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
    lines.append(f"  {'#':>5}  {'name':<8} {'offset':>10} {'size':>10}  "
                 f"{'dims':>9} {'flags':>5} {'lvl':>3}")
    previous = None
    for row in rows:
        if limit > 0 and previous is not None and row["index"] != previous + 1:
            lines.append(f"  ... {row['index'] - previous - 1} entries omitted "
                         f"(--limit 0 for all)")
        dims = (f"{row['width']}x{row['height']}"
                if row["width"] is not None else "?")
        flags = row["flags"] if row["flags"] is not None else "?"
        levels = row["levels"] if row["levels"] is not None else "?"
        lines.append(f"  {row['index']:>5}  {row['name']:<8} {row['offset']:>10} "
                     f"{row['size']:>10}  {dims:>9} {flags:>5} {levels:>3}")
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


def cmd_check(idx_path: str, dat_path: str | None, against: str | None,
              name: str | None, expect: str | None, as_json: bool) -> int:
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
    parsed_ok = 0
    for i, record in enumerate(archive.records):
        key = record.name.lower()
        if key in seen:
            warnings.append(f"duplicate name {record.name!r} at {seen[key]} and {i} "
                            f"(the game finds the first, 0x40d6b3)")
        else:
            seen[key] = i
        if not record.name:
            warnings.append(f"entry {i} has an empty name")
        elif record.name.upper() != record.name:
            warnings.append(f"entry {i} {record.name!r} is not upper case: the game "
                            f"uppercases the query only (0x480180)")
        if len(record.name_bytes.rstrip(b"\0")) > NAME_SIZE:
            problems.append(f"entry {i} name exceeds {NAME_SIZE} bytes")
        if record.offset + record.size > archive.dat_size:
            problems.append(f"entry {i} ({record.name!r}) runs past EOF")
        if record.size == 0:
            warnings.append(f"entry {i} ({record.name!r}) is empty")
        else:
            try:
                sbm().parse(record.blob)
                parsed_ok += 1
            except ValueError as exc:
                problems.append(f"entry {i} ({record.name!r}) is not .sbm: {exc}")
    last_end = (archive.records[-1].offset + archive.records[-1].size
                if archive.records else 0)
    if last_end != archive.dat_size:
        problems.append(f"last entry ends at {last_end}, DAT is "
                        f"{archive.dat_size} bytes")
    idx_again, dat_again = archive.to_bytes()
    if idx_again != read_bytes(archive.idx_path, "index"):
        problems.append("rebuilding the index does not reproduce the file")
    if dat_again != read_bytes(archive.dat_path, "data"):
        problems.append("rebuilding the DAT does not reproduce the file")

    names, blobs, dat_size = oracle_blobs(archive.idx_path)
    info.update({"dat": archive.dat_path, "count": archive.count,
                 "dat_size": archive.dat_size, "idx_size": idx_size,
                 "idx_tail_bytes": len(archive.idx_tail),
                 "dat_prefix_bytes": len(archive.dat_prefix)})
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
        source = parse_archive(against)
        if source.count != archive.count:
            problems.append(f"entry count {archive.count} != {source.count} in "
                            f"{against}")
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
        info.update({"against": os.path.abspath(against),
                     "idx_identical": idx_diff["delta"] == 0
                     and idx_diff["changed_bytes"] == 0,
                     "dat_identical": dat_diff["delta"] == 0
                     and dat_diff["changed_bytes"] == 0,
                     "idx_diff": idx_diff, "dat_diff": dat_diff,
                     "changed_entries": changed,
                     "others_identical": compared})
        target = archive.find(name) if name else None
        if name and target is None:
            problems.append(f"--against archive has no entry {name!r}")
        if expect:
            want = read_bytes(expect, "expect file")
            target_blob = archive.records[target].blob if target is not None else None
            if target_blob != want:
                problems.append(f"target entry differs from {expect}")
            else:
                info["target_equals_expect"] = True
    elif name or expect:
        problems.append("--name/--expect need --against")

    ok = not problems
    info.update({"problems": problems, "warnings": warnings, "ok": ok})
    lines = [f"check: {archive.idx_path}",
             f"  {archive.count} entries, {archive.dat_size} bytes of data, "
             f"index {idx_size} bytes, .sbm blobs parsed: {parsed_ok}/{archive.count}"]
    if against:
        lines.append(f"  against {against}: idx identical={info['idx_identical']}, "
                     f"DAT identical={info['dat_identical']}")
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
        if expect:
            lines.append(f"  target equals {expect}: "
                         f"{info.get('target_equals_expect', False)}")
    for problem in problems:
        lines.append(f"  PROBLEM: {problem}")
    for warning in warnings[:12]:
        lines.append(f"  warning: {warning}")
    if len(warnings) > 12:
        lines.append(f"  ... {len(warnings) - 12} more warnings (see --json)")
    lines.append(f"  result: {'ok' if ok else 'FAILED'}")
    emit(info, as_json, lines)
    return 0 if ok else 1


def check_out_dir(out_dir: str, game: str | None) -> None:
    """Refuse writes into the installation, its mirror or this repository."""
    if not out_dir:
        raise SbmWriteError("--out-dir is empty")
    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        raise SbmWriteError(f"--out-dir is a file, not a directory: {out_dir}")
    guards = [("game installation", game or DEFAULT_GAME),
              ("game mirror", DEFAULT_MIRROR)]
    guards += [("repository", root) for root in REPO_ROOTS]
    for label, root in guards:
        if root and inside(out_dir, root):
            raise SbmWriteError(
                f"refusing to write into the {label} ({os.path.realpath(out_dir)} "
                f"is inside {os.path.realpath(root)}); write to /tmp/opencode, "
                f"/var/tmp, build/ or out/")


def replace_entry(archive: Archive, name: str, image_path: str, out_dir: str, *,
                  palette_path: str | None = None, game: str | None = None,
                  levels: int = 0, flags: int | None = None, mip: str = "box",
                  force: bool = False) -> dict:
    """Replace one entry with an encoded image; verify every other entry."""
    index = archive.find(name)
    if index is None:
        raise SbmWriteError(
            f"entry {name!r} not found in {archive.idx_path} "
            f"({archive.count} entries); this tool never creates entries")
    old = archive.records[index]
    try:
        old_summary = sbm_summary(old.blob)
    except ValueError as exc:
        raise SbmWriteError(f"source entry {old.name!r} is not a valid .sbm: "
                            f"{exc}") from exc
    check_out_dir(out_dir, game)

    # Defaults follow the archive: a new image of the same size keeps the
    # source's mip count, otherwise one level; flags are always preserved.
    image = load_image(image_path)
    if levels:
        wanted = levels
    elif (image.width, image.height) == (old_summary["width"], old_summary["height"]):
        wanted = old_summary["levels"]
    else:
        wanted = 1
    wanted = max(1, min(wanted, image.width.bit_length(), image.height.bit_length()))
    chosen_flags = old_summary["flags"] if flags is None else flags
    blob, encoded = image_to_blob(image_path, palette_path=palette_path, game=game,
                                  levels=wanted, flags=chosen_flags, mip=mip,
                                  image=image)

    records, blobs = archive.replaced(index, blob)
    idx_bytes, dat_bytes = archive.to_bytes(blobs, records)
    out_idx, out_dat = out_paths(out_dir, archive.stem)
    if os.path.abspath(out_idx) == archive.idx_path or \
            os.path.abspath(out_dat) == archive.dat_path:
        raise SbmWriteError(f"output pair would overwrite the input archive "
                            f"({out_idx}); choose another --out-dir")
    write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=force)

    problems: list[str] = []
    out = parse_archive(out_idx)
    others = 0
    if out.count != archive.count:
        problems.append(f"entry count {out.count} != {archive.count}")
    for i, record in enumerate(archive.records):
        if i == index:
            if out.records[i].blob != blob:
                problems.append("target entry differs from the encoded image")
            continue
        if out.records[i].name_bytes != record.name_bytes:
            problems.append(f"entry {i} name changed")
        if out.records[i].blob != record.blob:
            problems.append(f"entry {i} ({record.name!r}) changed")
        else:
            others += 1
    # The oracle must read the new pair, and the new target must decode to the
    # image's dimensions and pixels.
    out_names, out_blobs, out_size = oracle_blobs(out_idx)
    if out_names != [record.name for record in archive.records]:
        problems.append("oracle: name table differs")
    if out_size != len(dat_bytes):
        problems.append(f"oracle: DAT size {out_size} != {len(dat_bytes)}")
    if len(out_blobs) <= index:
        problems.append(f"oracle: only {len(out_blobs)} blobs, no target at {index}")
    else:
        if out_blobs[index] != blob:
            problems.append("oracle: target blob differs from the encoded image")
        if out_names[index] != old.name:
            problems.append(f"oracle: target name {out_names[index]!r} != {old.name!r}")
    try:
        parsed = sbm_summary(out.records[index].blob)
    except ValueError as exc:
        problems.append(f"target blob does not parse: {exc}")
        parsed = {}
    pixels_match = (parsed.get("pixels") == encoded["pixels"]
                    and parsed.get("width") == encoded["width"]
                    and parsed.get("height") == encoded["height"])
    if not pixels_match:
        problems.append("target entry does not decode to the image's pixels/dimensions")
    if parsed.get("flags") != chosen_flags:
        problems.append(f"target flags {parsed.get('flags')} != {chosen_flags}")
    idx_again, dat_again = out.to_bytes()
    idempotent = (idx_again == read_bytes(out_idx, "index")
                  and dat_again == read_bytes(out_dat, "data"))
    if not idempotent:
        problems.append("re-reading the output and rebuilding it is not byte-identical")
    return {
        "source": archive.idx_path,
        "name": old.name,
        "index": index,
        "old_size": old.size,
        "new_size": len(blob),
        "delta": len(blob) - old.size,
        "out_idx": out_idx,
        "out_dat": out_dat,
        "count": out.count,
        "others_identical": others,
        "target_identical": out.records[index].blob == blob,
        "oracle_agrees": out_names == [r.name for r in archive.records],
        "pixels_match": pixels_match,
        "width": encoded["width"],
        "height": encoded["height"],
        "levels": wanted,
        "flags": chosen_flags,
        "idempotent": idempotent,
        "problems": problems,
        "ok": not problems,
    }


def cmd_replace(idx_path: str, name: str, image_path: str, out_dir: str, *,
                dat_path: str | None, palette_path: str | None, game: str | None,
                levels: int, flags: int | None, mip: str, force: bool,
                as_json: bool) -> int:
    archive = parse_archive(idx_path, dat_path)
    result = replace_entry(archive, name, image_path, out_dir,
                           palette_path=palette_path, game=game, levels=levels,
                           flags=flags, mip=mip, force=force)
    lines = [
        f"replace: {archive.idx_path} -> {result['out_idx']}",
        f"  entry {result['index']} {result['name']}: {result['old_size']} -> "
        f"{result['new_size']} bytes (delta {result['delta']:+d}), "
        f"{result['width']}x{result['height']}, {result['levels']} level(s), "
        f"flags {result['flags']}",
        f"  NEW: {result['out_idx']} ({os.path.getsize(result['out_idx'])} bytes)",
        f"       {result['out_dat']} ({os.path.getsize(result['out_dat'])} bytes)",
        f"  entries: {result['count']} (was {archive.count})",
        f"  others byte-identical: {result['others_identical']}/"
        f"{archive.count - 1}",
        f"  target identical: {result['target_identical']}  "
        f"pixels match: {result['pixels_match']}  "
        f"oracle agrees: {result['oracle_agrees']}  "
        f"idempotent: {result['idempotent']}",
    ]
    for problem in result["problems"]:
        lines.append(f"  PROBLEM: {problem}")
    payload = {"mode": "replace", **result}
    emit(payload, as_json, lines)
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


def _bmp_bytes(width: int, height: int, pixels: bytes, palette: bytes) -> bytes:
    """Minimal 8-bit bottom-up BMP (the format ``gm/bmp.py`` reads)."""
    if len(pixels) != width * height:
        raise SbmWriteError(f"{len(pixels)} pixels for {width}x{height}")
    stride = (width + 3) & ~3
    rows = [pixels[y * width:(y + 1) * width].ljust(stride, b"\0")
            for y in range(height)]
    data = b"".join(reversed(rows))          # positive height = bottom-up
    raw_palette = palette[:1024].ljust(1024, b"\0")
    # BMP stores palette entries as B,G,R,X.
    pal_bgra = b"".join(raw_palette[i + 2:i + 3] + raw_palette[i + 1:i + 2]
                        + raw_palette[i:i + 1] + raw_palette[i + 3:i + 4]
                        for i in range(0, 1024, 4))
    offset = 14 + 40 + len(pal_bgra)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0, len(data),
                       0, 0, 0, 0)
    return b"BM" + struct.pack("<IHHI", offset + len(data), 0, 0, offset) \
        + info + pal_bgra + data


def _synth_palette() -> bytes:
    return bytes(value for i in range(256)
                 for value in (i, (i * 3) & 0xFF, 255 - i, 0))


def _synth_sbm(width: int = 4, height: int = 4, *, levels: int = 1,
               flags: int = 0, extra: bytes = b"") -> bytes:
    pixels = bytes((i * 37 + 5) & 0xFF for i in range(width * height))
    blob = sbm().encode(width, height, pixels, levels=levels, flags=flags)
    return blob + extra


def _expect_error(fn, needle: str) -> tuple[bool, str]:
    try:
        fn()
    except SbmWriteError as exc:
        text = str(exc)
        return needle.lower() in text.lower(), text
    except Exception as exc:  # noqa: BLE001 - the point is to catch the wrong type
        return False, f"wrong exception {type(exc).__name__}: {exc}"
    return False, "no error raised"


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

    with tempfile.TemporaryDirectory(prefix="sbmwrite-selftest-",
                                     dir="/var/tmp") as tmp:
        pal = _synth_palette()
        tex = _synth_sbm(8, 8, levels=3, flags=0)
        spr = _synth_sbm(4, 4, levels=1, flags=257)
        tiny = _synth_sbm(1, 1, levels=1, flags=1)

        # ---- round-trip ---------------------------------------------------
        base_entries = [(b"TEXTURE\0", tex), (b"SPRITE\0\0", spr),
                        (b"TINY\0\0\0\0", tiny)]
        base_idx, base_dat = _synth_pair(base_entries)
        base = _write_pair_raw(tmp, "base", base_idx, base_dat)

        def rt_plain():
            archive = parse_archive(base)
            assert archive.count == 3, archive.count
            out_idx, out_dat = archive.to_bytes()
            assert out_idx == base_idx, "IDX differs"
            assert out_dat == base_dat, "DAT differs"
            return "idx/dat byte-identical (3 entries, mip chains)"
        check("roundtrip: synthetic SBM archive", rt_plain)

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
            pairs = [("ONE", _synth_sbm(2, 2)), ("TWO", spr), ("THREE", b"")]
            bigfile().pack(pairs, stem + ".IDX", stem + ".DAT", overwrite=True)
            archive = parse_archive(stem + ".IDX")
            out_idx, out_dat = archive.to_bytes()
            assert out_idx == read_bytes(stem + ".IDX", "idx")
            assert out_dat == read_bytes(stem + ".DAT", "dat")
            return "3 entries from gm.bigfile.pack reproduced byte for byte"
        check("roundtrip: archive written by gm.bigfile.pack", rt_oracle)

        def rt_extra():
            blob = _synth_sbm(2, 2, extra=b"\xDE\xAD\xBE\xEF")
            idx, dat = _synth_pair([(b"EXTRA\0\0\0", blob)])
            p = _write_pair_raw(tmp, "extra", idx, dat)
            archive = parse_archive(p)
            summary = sbm_summary(archive.records[0].blob)
            assert summary["extra_bytes"] == 4, summary["extra_bytes"]
            assert archive.to_bytes() == (idx, dat)
            return "bytes after the mip chain kept in the blob"
        check("roundtrip: .sbm extra bytes preserved", rt_extra)

        def rt_determinism():
            archive = parse_archive(base)
            assert archive.to_bytes() == parse_archive(base).to_bytes()
            return "two independent rebuilds agree"
        check("roundtrip: rebuild is deterministic", rt_determinism)

        # ---- corpus discovery ---------------------------------------------
        def detect_content():
            sub = os.path.join(tmp, "scan", "deeper")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"RENAMED\0", tex)])
            path = _write_pair_raw(sub, "NOTSBMIDX", idx, dat)
            found, skipped = find_archives(os.path.join(tmp, "scan"))
            assert [a.idx_path for a in found] == [os.path.abspath(path)], found
            assert not skipped, skipped
            return "renamed archive accepted because every blob parses as .sbm"
        check("detect: SBM archive by content", detect_content)

        def detect_skip():
            sub = os.path.join(tmp, "scan2")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"JUNK\0\0\0\0", b"not an sbm at all")])
            _write_pair_raw(sub, "JUNKIDX", idx, dat)
            found, skipped = find_archives(sub)
            assert not found, found
            assert skipped and "does not look like .sbm" in skipped[0][1], skipped
            return "non-SBM archive skipped with a reason"
        check("detect: non-SBM archive is skipped", detect_skip)

        def detect_bmp_lookalike():
            # ``gm.sbm.parse`` accepts this by accident (bytes 4..7 look like a
            # 1x1 one-level header), so identification must be stricter.
            fake = b"BM" + bytes(30)
            assert sbm().parse(fake)          # the oracle is fooled
            sub = os.path.join(tmp, "scan3")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"BMPISH\0\0", fake)])
            _write_pair_raw(sub, "BMPISH", idx, dat)
            found, skipped = find_archives(sub)
            assert not found, found
            assert skipped and "does not look like .sbm" in skipped[0][1], skipped
            return "a BMP-like payload that gm.sbm accepts is not an SBM archive"
        check("detect: false positive rejected", detect_bmp_lookalike)

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

        # ---- replacement ---------------------------------------------------
        def make_bmp(name: str, w: int, h: int, shift: int = 0) -> str:
            pixels = bytes((y * w + x + shift) & 0xFF for y in range(h)
                           for x in range(w))
            path = os.path.join(tmp, name + ".bmp")
            write_bytes(path, _bmp_bytes(w, h, pixels, pal))
            return path

        def replace(delta: str, width: int, height: int, expected: int):
            archive = parse_archive(base)
            image = make_bmp(f"new{delta}{width}", width, height)
            result = replace_entry(archive, "SPRITE", image, os.path.join(tmp, delta),
                                   levels=1, game=None)
            assert result["problems"] == [], result["problems"]
            assert result["count"] == 3
            assert result["others_identical"] == 2
            assert result["new_size"] == expected, result["new_size"]
            assert result["width"] == width and result["height"] == height
            out = parse_archive(result["out_idx"])
            assert out.records[1].size == expected
            return (f"{result['old_size']} -> {result['new_size']} bytes, "
                    f"2/2 others identical")
        # 4x4 sprite blobs are 12 + 16 = 28 bytes; 8x8 one level is 12 + 64.
        check("replace: grow (28 -> 76)",
              lambda: replace("grow", 8, 8, 12 + 64))
        check("replace: shrink (28 -> 16)",
              lambda: replace("shrink", 2, 2, 12 + 4))
        check("replace: same size", lambda: replace("same", 4, 4, 28))

        def replace_same_index():
            archive = parse_archive(base)
            image = make_bmp("sameidx", 4, 4)
            result = replace_entry(archive, "sprite", image, os.path.join(tmp, "sameidx"),
                                   levels=1, game=None)
            assert result["ok"], result["problems"]
            # Flags of the source entry (257) are preserved by default, so the
            # blob -- and therefore the whole index -- must be byte-identical.
            assert result["flags"] == 257, result["flags"]
            assert read_bytes(result["out_idx"], "idx") == base_idx, \
                "same-size replacement changed the index"
            return "IDX byte-identical, source flags 257 preserved"
        check("replace: same size keeps the index identical", replace_same_index)

        def replace_flags():
            archive = parse_archive(base)
            image = make_bmp("flagimg", 4, 4)
            result = replace_entry(archive, "TEXTURE", image, os.path.join(tmp, "flags"),
                                   levels=1, flags=0, game=None)
            assert result["ok"], result["problems"]
            assert result["flags"] == 0
            assert sbm_summary(parse_archive(result["out_idx"]).records[0].blob)["flags"] == 0
            return "--flags overrides the source flags"
        check("replace: --flags override", replace_flags)

        def replace_levels():
            archive = parse_archive(base)
            # Same size as TEXTURE (8x8), so the source's 3 levels are kept.
            image = make_bmp("lvl", 8, 8)
            result = replace_entry(archive, "TEXTURE", image, os.path.join(tmp, "lvl"),
                                   game=None)
            assert result["ok"], result["problems"]
            assert result["levels"] == 3, result["levels"]
            parsed = sbm_summary(parse_archive(result["out_idx"]).records[0].blob)
            assert parsed["levels"] == 3, parsed["levels"]
            return "same-size image keeps the source mip count (3)"
        check("replace: mip count follows the source", replace_levels)

        def replace_pixels():
            archive = parse_archive(base)
            image = make_bmp("pix", 4, 4, shift=100)
            result = replace_entry(archive, "SPRITE", image, os.path.join(tmp, "pix"),
                                   levels=1, game=None)
            assert result["ok"], result["problems"]
            assert result["pixels_match"], "pixels do not match"
            want = bytes((y * 4 + x + 100) & 0xFF for y in range(4) for x in range(4))
            got = sbm_summary(parse_archive(result["out_idx"]).records[1].blob)["pixels"]
            assert got == want, (got[:4], want[:4])
            return "gm.sbm decodes the new pixels exactly"
        check("replace: image pixels decode exactly", replace_pixels)

        def replace_missing():
            archive = parse_archive(base)
            image = make_bmp("missing", 4, 4)
            ok, text = _expect_error(
                lambda: replace_entry(archive, "NOSUCH", image,
                                      os.path.join(tmp, "missing"), game=None),
                "not found")
            assert ok, text
            assert not os.path.exists(os.path.join(tmp, "missing", "SBM.IDX"))
            return "unknown name refused, nothing written"
        check("replace: unknown name is refused", replace_missing)

        def replace_bad_image():
            archive = parse_archive(base)
            image = make_bmp("odd", 3, 5)
            ok, text = _expect_error(
                lambda: replace_entry(archive, "SPRITE", image,
                                      os.path.join(tmp, "odd"), game=None),
                "powers of two")
            assert ok, text
            return "3x5 image refused (dimensions must be powers of two)"
        check("replace: non-power-of-two image is refused", replace_bad_image)

        def replace_protected():
            archive = parse_archive(base)
            image = make_bmp("guard", 4, 4)
            targets = [("game", os.path.join(DEFAULT_GAME, "PICS")),
                       ("mirror", os.path.join(DEFAULT_MIRROR, "PICS"))]
            targets += [(f"repo {root}", os.path.join(root, "tmp"))
                        for root in REPO_ROOTS]
            for label, path in targets:
                ok, text = _expect_error(
                    lambda p=path: replace_entry(archive, "SPRITE", image, p,
                                                 game=DEFAULT_GAME),
                    "refusing to write")
                assert ok, f"{label}: {text}"
            return (f"game, mirror and {len(REPO_ROOTS)} repository root(s) refused")
        check("replace: protected output directories", replace_protected)

        def replace_overwrite():
            archive = parse_archive(base)
            image = make_bmp("twice", 4, 4)
            out = os.path.join(tmp, "twice")
            replace_entry(archive, "SPRITE", image, out, levels=1, game=None)
            ok, text = _expect_error(
                lambda: replace_entry(archive, "SPRITE", image, out, levels=1,
                                      game=None), "refusing to overwrite")
            assert ok, text
            result = replace_entry(archive, "SPRITE", image, out, levels=1,
                                   game=None, force=True)
            assert result["ok"], result["problems"]
            return "existing pair needs --force"
        check("replace: existing output needs --force", replace_overwrite)

        # ---- check mode ----------------------------------------------------
        def check_clean():
            payload = run_json(lambda: cmd_check(base, None, None, None, None, True))
            assert payload["ok"], payload["problems"]
            assert payload["count"] == 3
            return "clean archive: 0 problems, 3 entries"
        check("--check: clean archive passes", check_clean)

        def check_tamper():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "tamper")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True)
            # A monotonic offset shift keeps the archive structurally valid
            # (extents are derived from offsets), so only --against can see it:
            # the index differs and the two neighbouring blobs change extent.
            broken = bytearray(oi)
            off = struct.unpack_from("<I", broken, HEADER_SIZE + RECORD_SIZE + 8)[0]
            struct.pack_into("<I", broken, HEADER_SIZE + RECORD_SIZE + 8, off + 1)
            write_bytes(p + ".IDX", bytes(broken))
            payload = run_json(lambda: cmd_check(p + ".IDX", None, base, None, None, True))
            assert payload["ok"], payload["problems"]     # structure is still valid
            assert not payload["idx_identical"], payload["idx_diff"]
            assert payload["changed_entries"], payload
            return "a forged offset shows up in --against (index + extents)"
        check("--check --against: forged offset is visible", check_tamper)

        def check_truncated():
            archive = parse_archive(base)
            oi, od = archive.to_bytes()
            p = os.path.join(tmp, "truncated")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True)
            write_bytes(p + ".DAT", od[:-10])
            payload = run_json(lambda: cmd_check(p + ".IDX", None, None, None, None, True))
            assert not payload["ok"], "a truncated DAT passed --check"
            assert any("not .sbm" in text for text in payload["problems"]), \
                payload["problems"]
            return "a truncated DAT is detected through the blob parser"
        check("--check: truncated DAT detected", check_truncated)

        def check_against():
            archive = parse_archive(base)
            image = make_bmp("against", 8, 8)
            result = replace_entry(archive, "SPRITE", image, os.path.join(tmp, "against"),
                                   levels=1, game=None, force=True)
            assert result["ok"], result["problems"]
            payload = run_json(lambda: cmd_check(result["out_idx"], None, base, None,
                                                 None, True))
            assert payload["ok"], payload["problems"]
            assert payload["others_identical"] == 2, payload["others_identical"]
            assert len(payload["changed_entries"]) == 1, payload["changed_entries"]
            assert not payload["dat_identical"]
            return "1 changed entry, 2/2 others identical"
        check("--check --against: entry-level diff", check_against)

        def check_against_expect():
            archive = parse_archive(base)
            image = make_bmp("expect", 4, 4)
            result = replace_entry(archive, "SPRITE", image, os.path.join(tmp, "expect"),
                                   levels=1, game=None, force=True)
            want = os.path.join(tmp, "expect.sbm")
            # Independently encode the same image, then require the written
            # entry to match that blob byte for byte.
            blob, _info = image_to_blob(image, palette_path=None, game=None,
                                        levels=1, flags=257, mip="box")
            write_bytes(want, blob)
            payload = run_json(lambda: cmd_check(result["out_idx"], None, base,
                                                 "SPRITE", want, True))
            assert payload["ok"], payload["problems"]
            assert payload.get("target_equals_expect"), payload
            return "--expect compares the new entry with a raw .sbm blob"
        check("--check: --expect target comparison", check_against_expect)

        # ---- list / audit --------------------------------------------------
        def list_mode():
            payload = run_json(lambda: cmd_list(base, 5, True))
            assert payload["count"] == 3
            indexes = [row["index"] for row in payload["entries"]]
            assert indexes == [0, 1, 2], indexes      # both ends, no duplicates
            assert payload["entries"][0]["width"] == 8
            return "first/last entries, no duplicate rows on a short archive"
        check("--list: entries and dimensions", list_mode)

        def audit_mode():
            sub = os.path.join(tmp, "audit")
            os.makedirs(sub, exist_ok=True)
            idx, dat = _synth_pair([(b"A\0\0\0\0\0\0\0", tex),
                                    (b"B\0\0\0\0\0\0\0", b""),
                                    (b"C\0\0\0\0\0\0\0", spr)])
            _write_pair_raw(sub, "SBM", idx, dat)
            payload = run_json(lambda: cmd_audit(sub, True))
            assert payload["ok"], payload["failed"]
            row = payload["archives"][0]
            assert row["count"] == 3 and row["empty_entries"] == 1, row
            assert row["size_min"] == 0 and row["size_max"] == len(tex), row
            assert row["size_median"] == len(spr), row
            assert row["flags"] == {"0": 1, "257": 1}, row["flags"]
            return (f"3 entries, empty=1, median={row['size_median']}, "
                    f"flags {row['flags']}")
        check("--audit: min/median/max and empty entries", audit_mode)

        def json_every_mode():
            for payload in (run_json(lambda: cmd_list(base, 5, True)),
                            run_json(lambda: cmd_check(base, None, None, None,
                                                       None, True))):
                assert isinstance(payload, dict) and "ok" in payload, payload
            return "--list/--check emit one JSON object with ok"
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


def run_json(fn) -> dict:
    """Run a mode in-process with ``--json`` and return the parsed payload."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = fn()
    payload = json.loads(buf.getvalue())
    payload["_exit"] = code
    return payload


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sbmwrite.py",
        description="Round-trip, audit and edit Gunmetal SBM archives "
                    "(BIGFILE pairs of .sbm blobs).")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--roundtrip", nargs="?", const="", metavar="DIR",
                      help="repack every SBM archive under DIR (default: --game) "
                           "and compare both files byte for byte")
    mode.add_argument("--audit", nargs="?", const="", metavar="DIR",
                      help="corpus numbers: entries, sizes, names, empty records")
    mode.add_argument("--list", metavar="ARCHIVE.IDX",
                      help="print the first and last entries")
    mode.add_argument("--check", metavar="ARCHIVE.IDX",
                      help="verify the structure and (with --against) a rewrite")
    mode.add_argument("--replace", metavar="ARCHIVE.IDX",
                      help="replace one entry with an image, write a new pair")
    mode.add_argument("--self-test", action="store_true",
                      help="synthetic archives only, no game files")
    parser.add_argument("--name", help="entry name for --replace/--check")
    parser.add_argument("--image", metavar="FILE.bmp",
                        help="new image for --replace (.bmp or .png)")
    parser.add_argument("--out-dir", metavar="DIR",
                        help="output directory for --replace (DIR/SBM.IDX+.DAT)")
    parser.add_argument("--dat", metavar="FILE.DAT",
                        help="explicit data file (default: the .IDX sibling)")
    parser.add_argument("--against", metavar="ORIG.IDX",
                        help="--check: compare both files and entries with ORIG")
    parser.add_argument("--expect", metavar="FILE",
                        help="--check --against --name: the target entry must "
                             "equal this raw payload file")
    parser.add_argument("--palette", metavar="FILE.bmp",
                        help="--replace: palette carrier (default: the engine "
                             "palette, then the image's own palette)")
    parser.add_argument("--levels", type=int, default=0,
                        help="--replace: mip levels to store (0 = keep the "
                             "source count for a same-size image, else 1)")
    parser.add_argument("--flags", type=int, default=None,
                        help="--replace: .sbm header flags (default: keep the "
                             "source entry's value)")
    parser.add_argument("--filter", choices=("box", "nearest"), default="box",
                        help="--replace: mip filter (default box)")
    parser.add_argument("--game", default=DEFAULT_GAME,
                        help=f"installation directory (default {DEFAULT_GAME}, "
                             f"$GUNMETAL_GAME)")
    parser.add_argument("--tmp-dir",
                        default=os.environ.get("SBMWRITE_TMP")
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
    """``PICS/SBM.IDX`` means the file in the *installation*, like the engine.

    Absolute paths and paths that exist relative to the shell are used as they
    are; otherwise the same relative name is looked up under ``--game``.
    """
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
    args.replace = resolve_archive_path(args.replace, game)
    args.against = resolve_archive_path(args.against, game)
    args.dat = resolve_archive_path(args.dat, game)
    args.image = resolve_archive_path(args.image, game)
    args.palette = resolve_archive_path(args.palette, game)
    args.expect = resolve_archive_path(args.expect, game)
    try:
        if args.self_test:
            return run_self_test(args.json)
        if args.roundtrip is not None:
            directory = args.roundtrip or args.game or DEFAULT_GAME
            return cmd_roundtrip(directory, args.tmp_dir, args.json)
        if args.audit is not None:
            directory = args.audit or args.game or DEFAULT_GAME
            return cmd_audit(directory, args.json)
        if args.list:
            return cmd_list(args.list, args.limit, args.json)
        if args.check:
            if (args.name or args.expect) and not args.against:
                raise SbmWriteError("--name/--expect need --against")
            return cmd_check(args.check, args.dat, args.against, args.name,
                             args.expect, args.json)
        if args.replace:
            for flag, value in (("--name", args.name), ("--image", args.image),
                                ("--out-dir", args.out_dir)):
                if not value:
                    raise SbmWriteError(f"--replace needs {flag}")
            if args.levels < 0:
                raise SbmWriteError(f"--levels must be >= 0, got {args.levels}")
            return cmd_replace(args.replace, args.name, args.image, args.out_dir,
                               dat_path=args.dat, palette_path=args.palette,
                               game=game, levels=args.levels, flags=args.flags,
                               mip=args.filter, force=args.force,
                               as_json=args.json)
        raise SbmWriteError("no mode selected")  # pragma: no cover - argparse
    except VerifyError as exc:
        print(f"sbmwrite: verification failed: {exc}", file=sys.stderr)
        return 1
    except SbmWriteError as exc:
        print(f"sbmwrite: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:  # pragma: no cover
        print("sbmwrite: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
