#!/usr/bin/env python3
"""Packer/replacer for Gunmetal ``BIGFILE`` archives (``M11.6``).

The *reader* is :mod:`gm.bigfile` (the oracle, unchanged by this tool).  This
module is the writer/verifier side: it repacks a shipped ``.IDX``/``.DAT``
pair byte for byte, replaces one entry (any size) and proves afterwards that
every other entry survived unchanged.

Format rules used here (each one traceable to code or data):

* **12-byte header** ``char magic[7] = "BIGFILE"; u8 0x1A; u32 count`` --
  ``gm/bigfile.py:72-79`` (``len(idx) < 12``, magic at 0, count at 8,
  ``need = 12 + count*12``) and ``docs/FORMATS.md`` §1 (lines 44-61).
  Game: the header is read as one 12-byte record at ``0x40d4db``
  (``fread(0xc, 1)``) into ``0x4d29c8``; only ``+8`` (count) is used later
  (``0x40d513`` and ``0x40d60f``), so the original does **not** validate the
  magic -- the writer still emits it because ``gm/bigfile.py:72`` and
  ``engine/src/res/bigfile.cpp`` (``BigFile::open``) reject anything else.
* **12 bytes per entry**: ``char name[8]`` (NUL padded) + ``u32 offset`` --
  ``gm/bigfile.py:82-86``; game: ``fread`` of ``0x400`` entries at
  ``0x40d531`` (``0x400*12 = 0x3000``), name compared as two dwords with
  stride ``0xc`` (``0x40d6b3``), ``offset`` at ``+8`` feeds ``fseek``
  (``0x40d819`` -> ``0x40e110``).
* **Blob extents follow from the offsets**: ``size[i] = offset[i+1] -
  offset[i]``, the last one runs to EOF -- ``gm/bigfile.py:102-103``,
  ``docs/FORMATS.md`` §1.  Consequently *all* DAT bytes are covered by the
  extents, inter-blob padding included; only a prefix in front of
  ``offset[0]`` would be extra, and it is preserved verbatim.
* **Names are 1..8 bytes, NUL padded, and the game searches them in upper
  case only**: the key buffer is zeroed and the *query* is uppercased
  (``0x40d1b2``, ``0x40d1e6`` -> ``0x480180``; ``docs/SOUND_NAMES.md``
  §3.2), while the in-memory table is compared byte for byte (``0x40d6b3``).
  A lower-case entry in the index is therefore unreachable for the original.
* **Order is preserved** on replacement and a missing name is appended at the
  end -- ``gm/bigfile.py:262-274`` (``replace_entry``); the game searches
  linearly from entry 0, so a duplicate name resolves to the **first** record.
* **4 GiB / entry-count guards**: offsets are ``u32`` and ``build_index``
  refuses a cumulative offset above ``0xFFFFFFFF`` (``gm/bigfile.py:206-207``);
  ``MAX_ENTRIES`` is a trust boundary (``gm/bigfile.py:32``).

What is recalculated by a replacement: nothing but the ``offset`` fields (and
``count`` if ``--add`` appends).  Sizes are implicit in the offsets, so
growing or shrinking one blob shifts every following offset by the delta and
leaves the index byte-identical in the same-size case.  Magic, byte 7, name
bytes, a DAT prefix and bytes after the index table (``.IDX`` tail) are copied
verbatim.

Usage::

    python3 tools/editor/bigpack.py --roundtrip              # all shipped pairs
    python3 tools/editor/bigpack.py --list PICS/SBM.IDX      # table + FNV-1a 64
    python3 tools/editor/bigpack.py --replace sprites/SBM.IDX --name AFLM0001 \\
            --file /tmp/opencode/new.sbm --out /tmp/opencode/sprites
    python3 tools/editor/bigpack.py --check /tmp/opencode/sprites.IDX
    python3 tools/editor/bigpack.py --check /tmp/opencode/sprites.IDX \\
            --against sprites/SBM.IDX --name AFLM0001 --expect /tmp/opencode/new.sbm
    python3 tools/editor/bigpack.py --self-test             # no game files

``--json`` turns every mode into one JSON line.  Exit codes: ``0`` success,
``1`` verification failed (round-trip/check/replace/self-test negative), ``2``
bad input or usage (message on stderr), ``3`` internal error.  Nothing is ever
written to the installation: output paths inside ``GUNMETAL_GAME`` are
refused (``BIGPACK_ALLOW_GAME_WRITE=1`` overrides, for a scratch mirror only).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")

MAGIC = b"BIGFILE"
SEP = 0x1A
HEADER_SIZE = 12
ENTRY_SIZE = 12
UINT32_MAX = 0xFFFFFFFF
MAX_ENTRIES = 1_000_000          # same trust boundary as gm/bigfile.py:32

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
FNV_MASK = (1 << 64) - 1

#: Names accepted by ``--add``: the shipped charset (uppercase, digits and the
#: few separators seen in PICS/sprites/sounds) -- the game only finds uppercase
#: keys, so lower case is refused unless ``--allow-any-name`` is given.
NAME_OK = re.compile(rb"[A-Z0-9_+.\-']{1,8}\Z")
PRINTABLE = re.compile(rb"[\x20-\x7e]{1,8}\Z")


class BigpackError(Exception):
    """Bad input or usage (exit code 2)."""


class VerifyError(Exception):
    """A written or checked archive failed verification (exit code 1)."""


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def fnv1a64(data: bytes, seed: int = FNV_OFFSET) -> int:
    """FNV-1a 64 of a blob, the project's standard data digest."""
    value = seed
    prime = FNV_PRIME
    mask = FNV_MASK
    for byte in data:
        value = ((value ^ byte) * prime) & mask
    return value


def read_bytes(path: str, what: str) -> bytes:
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise BigpackError(f"cannot read {what} {path}: {exc.strerror or exc}") from exc


def write_bytes(path: str, data: bytes) -> None:
    try:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
    except OSError as exc:
        raise BigpackError(f"cannot write {path}: {exc.strerror or exc}") from exc


def emit(payload: dict, as_json: bool, lines=()) -> None:
    if as_json:
        print(json.dumps(payload, ensure_ascii=False))
        return
    for line in lines:
        print(line)


def inside(path: str, directory: str) -> bool:
    try:
        p = os.path.join(os.path.realpath(directory), "")
        return os.path.realpath(path).startswith(p)
    except OSError:
        return False


def out_paths(prefix: str) -> tuple[str, str]:
    """``PREFIX`` -> ``(PREFIX.IDX, PREFIX.DAT)``, shipped upper case."""
    low = prefix.lower()
    if low.endswith(".idx"):
        idx = prefix
    elif low.endswith(".dat"):
        idx = prefix[:-4] + ".IDX"
    else:
        idx = prefix + ".IDX"
    return idx, idx[:-4] + ".DAT"


def encode_name_bytes(name: str, *, allow_any: bool = False) -> bytes:
    """Validate a *new* entry name and return its 8-byte NUL-padded field."""
    try:
        raw = name.encode("latin1")
    except UnicodeEncodeError as exc:
        raise BigpackError(f"name {name!r} is not single-byte (latin1) text") from exc
    if b"\0" in raw:
        raise BigpackError(f"name {name!r} contains a NUL byte")
    if not 1 <= len(raw) <= 8:
        raise BigpackError(
            f"name must be 1..8 bytes (the field is char[8], the game keys are "
            f"8 bytes at 0x40d1f0): {name!r} is {len(raw)} bytes")
    pattern = PRINTABLE if allow_any else NAME_OK
    if not pattern.match(raw):
        want = ("printable ASCII" if allow_any
                else "upper-case [A-Z0-9_+.-'] (the game uppercases the query at "
                     "0x480180 and compares raw bytes at 0x40d6b3)")
        raise BigpackError(f"name {name!r} is outside {want}; "
                           f"use --allow-any-name to override")
    return raw + b"\0" * (8 - len(raw))


# ---------------------------------------------------------------------------
# archive model
# ---------------------------------------------------------------------------

class RawEntry:
    """One index record: verbatim name bytes, offset, and the blob it spans."""

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
        return f"RawEntry({self.name!r}, {self.offset}, {self.size})"


class Archive:
    """A parsed ``.IDX``/``.DAT`` pair that can be rebuilt byte for byte."""

    def __init__(self, idx_path: str, dat_path: str, header: bytes,
                 entries: list[RawEntry], idx_tail: bytes, dat_prefix: bytes,
                 dat_size: int, sep_ok: bool):
        self.idx_path = idx_path
        self.dat_path = dat_path
        self.header = header
        self.entries = entries
        self.idx_tail = idx_tail
        self.dat_prefix = dat_prefix
        self.dat_size = dat_size
        self.sep_ok = sep_ok

    @property
    def count(self) -> int:
        return len(self.entries)

    def find(self, name: str) -> int | None:
        """Index of the first record whose name matches case-insensitively."""
        key = name.lower()
        for i, entry in enumerate(self.entries):
            if entry.name.lower() == key:
                return i
        return None

    def to_bytes(self, blobs: list[bytes] | None = None,
                 entries: list[RawEntry] | None = None) -> tuple[bytes, bytes]:
        """Re-emit ``(idx, dat)``; offsets are recomputed, everything else kept."""
        entries = self.entries if entries is None else entries
        blobs = [e.blob for e in entries] if blobs is None else list(blobs)
        if len(blobs) != len(entries):
            raise BigpackError(f"{len(blobs)} blobs for {len(entries)} entries")
        dat = bytearray(self.dat_prefix)
        idx = bytearray(self.header[:8])          # magic + 0x1A, verbatim
        idx += struct.pack("<I", len(entries))
        for entry, blob in zip(entries, blobs):
            if not isinstance(blob, (bytes, bytearray)):
                raise BigpackError(f"blob for {entry.name!r} is not bytes")
            offset = len(dat)
            if offset > UINT32_MAX:
                raise BigpackError(
                    f"payload exceeds 4 GiB offsets at {entry.name!r} "
                    f"(offset {offset} > 0x{UINT32_MAX:08X}); the index field is u32")
            idx += entry.name_bytes + struct.pack("<I", offset)
            dat += blob
        idx += self.idx_tail
        if len(dat) > UINT32_MAX + 1:
            raise BigpackError(f"payload is {len(dat)} bytes; u32 offsets cap it at 4 GiB")
        return bytes(idx), bytes(dat)

    def replaced(self, index: int, blob: bytes) -> tuple[list[RawEntry], list[bytes]]:
        entries = list(self.entries)
        blobs = [e.blob for e in entries]
        blobs[index] = blob
        return entries, blobs

    def appended(self, name_bytes: bytes, blob: bytes) -> tuple[list[RawEntry], list[bytes]]:
        entries = list(self.entries)
        blobs = [e.blob for e in entries]
        # The new record sits after every existing blob, so the previous last
        # entry keeps its extent (offset[i+1] - offset[i] is recomputed from the
        # new offsets; nothing is truncated).
        entries.append(RawEntry(name_bytes, -1, blob))
        blobs.append(blob)
        return entries, blobs


def resolve_dat(idx_path: str) -> str:
    """Find the ``.DAT`` sibling: ``.DAT`` first, then ``.dat``, then any case."""
    base = os.path.splitext(idx_path)[0]
    for candidate in (base + ".DAT", base + ".dat"):
        if os.path.isfile(candidate):
            return candidate
    directory = os.path.dirname(os.path.abspath(idx_path)) or "."
    stem = os.path.basename(base).lower()
    try:
        names = os.listdir(directory)
    except OSError as exc:
        raise BigpackError(f"cannot list {directory}: {exc.strerror or exc}") from exc
    for name in names:
        if os.path.splitext(name)[0].lower() == stem and name.lower().endswith(".dat"):
            return os.path.join(directory, name)
    raise BigpackError(
        f"data file not found next to {idx_path}: tried {base}.DAT and {base}.dat")


def parse_archive(idx_path: str, dat_path: str | None = None) -> Archive:
    """Parse an archive with the oracle's rules, keeping raw/extra bytes."""
    idx_path = os.path.abspath(idx_path)
    if not os.path.isfile(idx_path):
        raise BigpackError(f"no such index: {idx_path}")
    idx = read_bytes(idx_path, "index")
    if len(idx) < HEADER_SIZE:
        raise BigpackError(
            f"{idx_path}: not a BIGFILE index: {len(idx)} bytes, the header alone "
            f"needs {HEADER_SIZE} (gm/bigfile.py:72)")
    if idx[:len(MAGIC)] != MAGIC:
        raise BigpackError(
            f"{idx_path}: not a BIGFILE index: magic is {idx[:len(MAGIC)]!r}, "
            f"expected {MAGIC!r} (gm/bigfile.py:72)")
    sep_ok = idx[len(MAGIC)] == SEP
    count = struct.unpack_from("<I", idx, 8)[0]
    if count > MAX_ENTRIES:
        raise BigpackError(
            f"{idx_path}: implausible entry count {count} (limit {MAX_ENTRIES}, "
            f"gm/bigfile.py:32)")
    need = HEADER_SIZE + count * ENTRY_SIZE
    if len(idx) < need:
        raise BigpackError(
            f"{idx_path}: index too short for {count} entries: {len(idx)} < {need} "
            f"(gm/bigfile.py:78)")
    dat_path = dat_path or resolve_dat(idx_path)
    dat = read_bytes(dat_path, "data")
    dat_size = len(dat)

    raw: list[RawEntry] = []
    previous = 0
    for i in range(count):
        o = HEADER_SIZE + ENTRY_SIZE * i
        name_bytes = idx[o:o + 8]
        offset = struct.unpack_from("<I", idx, o + 8)[0]
        if offset > dat_size:
            raise BigpackError(
                f"{idx_path}: entry {i} ({name_bytes.rstrip(bytes(1))!r}) offset "
                f"{offset} beyond DAT ({dat_size} bytes) (gm/bigfile.py:95)")
        if offset < previous:
            raise BigpackError(
                f"{idx_path}: entry {i} offset {offset} goes backwards "
                f"(previous {previous}) (gm/bigfile.py:98)")
        previous = offset
        raw.append(RawEntry(name_bytes, offset))

    prefix = dat[:raw[0].offset] if raw else dat
    for i, entry in enumerate(raw):
        end = raw[i + 1].offset if i + 1 < len(raw) else dat_size
        entry.blob = dat[entry.offset:end]
    return Archive(idx_path, os.path.abspath(dat_path), idx[:HEADER_SIZE], raw,
                   idx[need:], prefix, dat_size, sep_ok)


def write_pair(idx_path: str, dat_path: str, idx_bytes: bytes, dat_bytes: bytes,
               *, force: bool, game: str | None) -> None:
    """Write both files atomically; refuse the installation and overwrites."""
    if game and not os.environ.get("BIGPACK_ALLOW_GAME_WRITE"):
        for path in (idx_path, dat_path):
            if inside(path, game):
                raise BigpackError(
                    f"refusing to write into the game installation ({path}); "
                    f"write to build/, out/, /tmp or /var/tmp -- the installation "
                    f"is read-only for this project")
    if not force:
        for path in (idx_path, dat_path):
            if os.path.exists(path):
                raise BigpackError(f"refusing to overwrite {path} (use --force)")
    temps: list[str] = []
    try:
        for path, payload in ((dat_path, dat_bytes), (idx_path, idx_bytes)):
            directory = os.path.dirname(os.path.abspath(path)) or "."
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, prefix=".gm-bigpack-")
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
        raise BigpackError(f"cannot write archive: {exc.strerror or exc}") from exc


# ---------------------------------------------------------------------------
# oracle cross-check
# ---------------------------------------------------------------------------

def oracle():
    try:
        from gm import bigfile  # noqa: PLC0415 - imported lazily for clear errors
    except Exception as exc:  # pragma: no cover - broken checkout
        raise BigpackError(f"cannot import the oracle gm.bigfile: {exc}") from exc
    return bigfile


def oracle_blobs(idx_path: str) -> tuple[list[str], list[bytes], int]:
    bigfile = oracle()
    try:
        with bigfile.BigFile(idx_path) as bf:
            names = [e.name for e in bf.entries]
            # dat_blobs() yields (name, data) pairs, in index order
            blobs = [data for _name, data in bf.dat_blobs()]
            return names, blobs, bf.dat_size
    except (ValueError, OSError) as exc:
        raise VerifyError(f"oracle cannot read {idx_path}: {exc}") from exc


# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------

def find_archives(game: str) -> tuple[list[str], list[tuple[str, str]]]:
    """Every ``*.idx`` with a sibling ``.dat``; ``.idx`` only files are reported."""
    archives: list[str] = []
    skipped: list[tuple[str, str]] = []
    if not os.path.isdir(game):
        raise BigpackError(f"game directory not found: {game}")
    for root, dirs, files in os.walk(game):
        dirs.sort()
        for name in sorted(files):
            if not name.lower().endswith(".idx"):
                continue
            path = os.path.join(root, name)
            try:
                resolve_dat(path)
            except BigpackError as exc:
                skipped.append((path, str(exc)))
                continue
            archives.append(path)
    return archives, skipped


def cmd_roundtrip(game: str, tmp_dir: str, as_json: bool) -> int:
    archives, skipped = find_archives(game)
    if not archives:
        raise BigpackError(f"no BIGFILE .IDX/.DAT pairs under {game}")
    rows: list[dict] = []
    failures: list[str] = []
    total_entries = total_bytes = 0
    lines = [f"round-trip: {game}"]
    lines += [f"  skipped (no .DAT): {os.path.relpath(p, game)}"
              for p, _why in skipped]
    with tempfile.TemporaryDirectory(prefix="bigpack-rt-", dir=tmp_dir) as out:
        for path in archives:
            rel = os.path.relpath(path, game)
            started = time.perf_counter()
            try:
                archive = parse_archive(path)
                idx_bytes, dat_bytes = archive.to_bytes()
            except BigpackError as exc:
                failures.append(f"{rel}: {exc}")
                lines.append(f"  FAIL {rel}: {exc}")
                continue
            stem = re.sub(r"[^A-Za-z0-9_.-]", "_", rel.replace(os.sep, "_"))
            out_idx, out_dat = out_paths(os.path.join(out, stem))
            write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=True, game=None)
            idx_same = read_bytes(out_idx, "index") == read_bytes(path, "index")
            orig_dat = read_bytes(archive.dat_path, "data")
            dat_same = read_bytes(out_dat, "data") == orig_dat
            # the oracle must read the repacked pair as well
            names, blobs, dat_size = oracle_blobs(out_idx)
            oracle_ok = (len(names) == archive.count
                         and dat_size == len(dat_bytes)
                         and b"".join(blobs) == dat_bytes)
            ok = idx_same and dat_same and oracle_ok
            if not ok:
                failures.append(f"{rel}: idx={idx_same} dat={dat_same} "
                                f"oracle={oracle_ok}")
            seconds = time.perf_counter() - started
            rows.append({
                "archive": rel,
                "count": archive.count,
                "dat_size": archive.dat_size,
                "idx_size": os.path.getsize(path),
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
                f"{archive.dat_size:>10} B  idx {os.path.getsize(path):>7} B  "
                f"identical={idx_same}/{dat_same}  {seconds:.2f}s")
            for extra, label in ((archive.idx_tail, "index tail"),
                                 (archive.dat_prefix, "DAT prefix")):
                if extra:
                    lines.append(f"       note: {len(extra)} extra {label} bytes preserved")
    ok = not failures and all(r["ok"] for r in rows)
    lines.append(f"  totals: {len(rows)} archives, {total_entries} entries, "
                 f"{total_bytes} bytes, all identical={ok}")
    payload = {
        "mode": "roundtrip",
        "game": game,
        "archives": rows,
        "skipped": [{"path": p, "reason": w} for p, w in skipped],
        "totals": {"archives": len(rows), "identical": sum(r["ok"] for r in rows),
                   "entries": total_entries, "bytes": total_bytes},
        "failed": failures,
        "ok": ok,
    }
    emit(payload, as_json, lines)
    return 0 if ok else 1


def cmd_list(idx_path: str, as_json: bool, limit: int) -> int:
    archive = parse_archive(idx_path)
    rows = []
    entries = archive.entries[:limit] if limit > 0 else archive.entries
    for i, entry in enumerate(entries):
        rows.append({"index": i, "name": entry.name, "offset": entry.offset,
                     "size": entry.size, "fnv1a64": "0x%016x" % fnv1a64(entry.blob)})
    lines = [f"{idx_path}: {archive.count} entries, {archive.dat_size} bytes of data, "
             f"index {os.path.getsize(archive.idx_path)} bytes"]
    if not archive.sep_ok:
        lines.append(f"  WARNING: byte 7 is 0x{archive.header[7]:02X}, not 0x1A")
    if archive.dat_prefix:
        lines.append(f"  note: {len(archive.dat_prefix)} bytes before offset[0] preserved")
    if archive.idx_tail:
        lines.append(f"  note: {len(archive.idx_tail)} bytes after the table preserved")
    lines.append(f"  {'#':>5}  {'name':<8} {'offset':>10} {'size':>10}  {'fnv1a64':<18}")
    for row in rows:
        lines.append(f"  {row['index']:>5}  {row['name']:<8} {row['offset']:>10} "
                     f"{row['size']:>10}  {row['fnv1a64']}")
    if limit > 0 and limit < archive.count:
        lines.append(f"  ... {archive.count - limit} more entries (--limit 0 for all)")
    payload = {"mode": "list", "idx": archive.idx_path, "dat": archive.dat_path,
               "count": archive.count, "dat_size": archive.dat_size,
               "idx_size": os.path.getsize(archive.idx_path),
               "idx_tail_bytes": len(archive.idx_tail),
               "dat_prefix_bytes": len(archive.dat_prefix),
               "entries": rows, "ok": True}
    emit(payload, as_json, lines)
    return 0


def verify_replace(source: Archive, out_idx: str, target: int, new_blob: bytes,
                   *, added: bool) -> dict:
    """Prove: same count, target == new bytes, every other entry byte-identical."""
    out = parse_archive(out_idx)
    problems: list[str] = []
    expected_count = source.count + (1 if added else 0)
    if out.count != expected_count:
        problems.append(f"entry count {out.count} != expected {expected_count}")
    if out.entries[target].blob != new_blob:
        problems.append("target entry differs from the new file")
    others = 0
    for i, entry in enumerate(source.entries):
        if i == target:
            continue
        if i >= out.count:
            problems.append(f"entry {i} ({entry.name!r}) missing from the output")
            continue
        if out.entries[i].name_bytes != entry.name_bytes:
            problems.append(f"entry {i} name changed: {entry.name_bytes!r} -> "
                            f"{out.entries[i].name_bytes!r}")
        if out.entries[i].blob != entry.blob:
            problems.append(f"entry {i} ({entry.name!r}) changed")
        else:
            others += 1
    # Independent reader: the oracle must agree, entry by entry.
    src_names, src_blobs, src_size = oracle_blobs(source.idx_path)
    out_names, out_blobs, out_size = oracle_blobs(out.idx_path)
    if added:
        src_names = src_names + [out_names[-1]]
        src_blobs = src_blobs + [new_blob]
    else:
        src_blobs[target] = new_blob
    if out_names != src_names:
        problems.append("oracle: name table differs")
    if src_size != source.dat_size:
        problems.append("oracle: source DAT size changed while reading")
    if out_blobs != src_blobs:
        problems.append("oracle: blob stream differs")
    if out_size != len(out.dat_prefix) + sum(len(b) for b in out_blobs):
        problems.append("oracle: DAT size is inconsistent with the blobs")
    # Rebuild must be idempotent (the index is the writer's own canonical form).
    idx_again, dat_again = out.to_bytes()
    idempotent = (idx_again == read_bytes(out.idx_path, "index")
                  and dat_again == read_bytes(out.dat_path, "data"))
    if not idempotent:
        problems.append("re-reading the output and rebuilding it is not byte-identical")
    return {
        "out_count": out.count,
        "others_identical": others,
        "target_identical": out.entries[target].blob == new_blob,
        "oracle_agrees": out_blobs == src_blobs and out_names == src_names,
        "idempotent": idempotent,
        "problems": problems,
    }


def cmd_replace(idx_path: str, name: str, file_path: str, prefix: str, *,
                add: bool, force: bool, game: str | None, as_json: bool,
                allow_any: bool) -> int:
    archive = parse_archive(idx_path)
    new_blob = read_bytes(file_path, "payload")
    index = archive.find(name)
    added = False
    if index is None:
        if not add:
            raise BigpackError(
                f"entry {name!r} not found in {idx_path} "
                f"({archive.count} entries); use --add to append it")
        name_bytes = encode_name_bytes(name, allow_any=allow_any)
        if archive.find(name_bytes.rstrip(b"\0").decode("latin1")) is not None:
            raise BigpackError(f"entry {name!r} already exists (case-insensitive)")
        entries, blobs = archive.appended(name_bytes, new_blob)
        index = len(entries) - 1
        added = True
        old_size = None
    else:
        old_size = archive.entries[index].size
        entries, blobs = archive.replaced(index, new_blob)
    idx_bytes, dat_bytes = archive.to_bytes(blobs, entries)

    out_idx, out_dat = out_paths(prefix)
    if os.path.abspath(out_idx) == archive.idx_path:
        raise BigpackError(
            f"output index is the input archive ({out_idx}); write to another prefix")
    write_pair(out_idx, out_dat, idx_bytes, dat_bytes, force=force, game=game)

    result = verify_replace(archive, out_idx, index, new_blob, added=added)
    ok = not result["problems"]
    delta = None if added else len(new_blob) - old_size
    lines = [
        f"replace: {archive.idx_path} -> {out_idx}",
        f"  entry {index} {archive.entries[index].name if not added else name!r}: "
        f"{'added' if added else f'{old_size} -> {len(new_blob)} bytes'}"
        + ("" if added else f" (delta {delta:+d})"),
        f"  NEW:     {out_idx} ({len(idx_bytes)} bytes)",
        f"           {out_dat} ({len(dat_bytes)} bytes)",
        f"  entries: {result['out_count']} (was {archive.count})",
        f"  others byte-identical: {result['others_identical']}/{len(archive.entries) - (0 if added else 1)}",
        f"  target identical: {result['target_identical']}  "
        f"oracle agrees: {result['oracle_agrees']}  idempotent: {result['idempotent']}",
    ]
    for problem in result["problems"]:
        lines.append(f"  PROBLEM: {problem}")
    payload = {
        "mode": "replace",
        "source": archive.idx_path,
        "name": archive.entries[index].name if not added else name,
        "added": added,
        "index": index,
        "old_size": old_size,
        "new_size": len(new_blob),
        "delta": delta,
        "out_idx": out_idx,
        "out_dat": out_dat,
        "count": result["out_count"],
        "others_identical": result["others_identical"],
        "target_identical": result["target_identical"],
        "oracle_agrees": result["oracle_agrees"],
        "idempotent": result["idempotent"],
        "problems": result["problems"],
        "ok": ok,
    }
    emit(payload, as_json, lines)
    return 0 if ok else 1


def cmd_check(idx_path: str, against: str | None, name: str | None,
              expect: str | None, as_json: bool) -> int:
    problems: list[str] = []
    warnings: list[str] = []
    info: dict = {"mode": "check", "idx": os.path.abspath(idx_path)}
    try:
        archive = parse_archive(idx_path)
    except BigpackError as exc:
        payload = {"mode": "check", "idx": os.path.abspath(idx_path),
                   "problems": [str(exc)], "warnings": [], "ok": False}
        emit(payload, as_json, [f"check: {idx_path}", f"  PROBLEM: {exc}"])
        return 1

    if not archive.sep_ok:
        problems.append(f"header byte 7 is 0x{archive.header[7]:02X}, expected 0x1A "
                        f"(docs/FORMATS.md §1)")
    need = HEADER_SIZE + archive.count * ENTRY_SIZE
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
    for i, entry in enumerate(archive.entries):
        key = entry.name.lower()
        if key in seen:
            warnings.append(f"duplicate name {entry.name!r} at {seen[key]} and {i} "
                            f"(the game finds the first, 0x40d6b3)")
        else:
            seen[key] = i
        if not entry.name:
            warnings.append(f"entry {i} has an empty name")
        elif entry.name.upper() != entry.name:
            warnings.append(f"entry {i} {entry.name!r} is not upper case: the game "
                            f"uppercases the query only (0x480180), so this record "
                            f"cannot be found")
        if entry.offset + entry.size > archive.dat_size:
            problems.append(f"entry {i} ({entry.name!r}) runs past EOF")
    last_end = (archive.entries[-1].offset + archive.entries[-1].size
                if archive.entries else 0)
    if last_end != archive.dat_size:
        problems.append(f"last entry ends at {last_end}, DAT is {archive.dat_size} bytes")
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
        problems.append(f"oracle sees {len(names)} entries, the table has {archive.count}")
    if dat_size != archive.dat_size:
        problems.append(f"oracle sees {dat_size} DAT bytes, file is {archive.dat_size}")
    for i, (blob, entry) in enumerate(zip(blobs, archive.entries)):
        if blob != entry.blob:
            problems.append(f"oracle blob {i} ({entry.name!r}) differs from the table span")

    compared = 0
    if against:
        try:
            source = parse_archive(against)
        except BigpackError as exc:
            problems.append(f"cannot read --against {against}: {exc}")
            source = None
        if source is not None:
            if name is None:
                problems.append("--against needs --name to know which entry changed")
                source = None
            else:
                target = source.find(name)
                if target is None:
                    problems.append(f"--against has no entry {name!r}")
                    source = None
                else:
                    if source.count != archive.count:
                        problems.append(f"entry count {archive.count} != "
                                        f"{source.count} in {against}")
                    for i, entry in enumerate(source.entries):
                        if i == target:
                            continue
                        if i >= len(archive.entries):
                            problems.append(f"entry {i} ({entry.name!r}) missing")
                        elif archive.entries[i].blob != entry.blob:
                            problems.append(f"entry {i} ({entry.name!r}) differs "
                                            f"from {against}")
                        else:
                            compared += 1
                    info["changed_entry"] = archive.entries[target].name
                    info["changed_size"] = archive.entries[target].size
                    info["source_changed_size"] = source.entries[target].size
                    if expect:
                        want = read_bytes(expect, "expect file")
                        if archive.entries[target].blob != want:
                            problems.append(f"target entry differs from {expect}")
                        else:
                            info["target_equals_expect"] = True
    elif expect:
        problems.append("--expect needs --against and --name")

    ok = not problems
    info.update({"problems": problems, "warnings": warnings,
                 "others_identical": compared, "ok": ok})
    lines = [f"check: {archive.idx_path}",
             f"  {archive.count} entries, {archive.dat_size} bytes of data, "
             f"index {idx_size} bytes"]
    if against:
        lines.append(f"  compared with {against}: {compared} other entries identical")
    if expect:
        lines.append(f"  target equals {expect}: {info.get('target_equals_expect', False)}")
    for problem in problems:
        lines.append(f"  PROBLEM: {problem}")
    for warning in warnings[:12]:
        lines.append(f"  warning: {warning}")
    if len(warnings) > 12:
        lines.append(f"  ... {len(warnings) - 12} more warnings (see --json)")
    lines.append(f"  result: {'ok' if ok else 'FAILED'}")
    emit(info, as_json, lines)
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# self-test (synthetic archives, no game files)
# ---------------------------------------------------------------------------

def _synth(entries, *, prefix: bytes = b"", pad_after: dict | None = None,
           idx_tail: bytes = b"", sep: int = SEP,
           extra_header: bytes = b"") -> tuple[bytes, bytes]:
    """Build ``(idx, dat)`` by hand so odd layouts can be tested."""
    pad_after = pad_after or {}
    count = len(entries)
    header = MAGIC + bytes([sep]) + struct.pack("<I", count) + extra_header
    idx = bytearray(header)
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


def _expect_error(fn, needle: str) -> tuple[bool, str]:
    try:
        fn()
    except BigpackError as exc:
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

    with tempfile.TemporaryDirectory(prefix="bigpack-selftest-") as tmp:
        # ---- round-trip of odd layouts -----------------------------------
        ent = [(b"ALPHA\0\0\0", b"a" * 10), (b"BETA\0\0\0\0", b""),
               (b"GAMMA\0\0\0", b"\x00\x01\x02\x03\x04")]
        idx, dat = _synth(ent, prefix=b"PRE!", pad_after={0: b"\xAA\xBB\xCC"},
                          idx_tail=b"\xFF\xFF\xFF\xFF")
        path = _write_pair_raw(tmp, "odd", idx, dat)

        def rt_odd():
            archive = parse_archive(path)
            assert archive.count == 3, archive.count
            assert archive.dat_prefix == b"PRE!", archive.dat_prefix
            assert archive.idx_tail == b"\xFF" * 4
            assert archive.entries[0].size == 13, archive.entries[0].size
            assert archive.entries[1].size == 0, archive.entries[1].size
            out_idx, out_dat = archive.to_bytes()
            assert out_idx == idx, "IDX differs"
            assert out_dat == dat, "DAT differs"
            return "idx/dat byte-identical, prefix+padding+tail preserved"
        check("roundtrip: odd layout (prefix, pad, empty entry, idx tail)", rt_odd)

        # ---- round-trip of an archive built by the oracle writer ---------
        def rt_oracle():
            bigfile = oracle()
            stem = os.path.join(tmp, "oracle")
            pairs = [("ONE", b"1" * 100), ("TWO", b"2" * 7), ("THREE", b"")]
            bigfile.pack(pairs, stem + ".IDX", stem + ".DAT", overwrite=True)
            archive = parse_archive(stem + ".IDX")
            out_idx, out_dat = archive.to_bytes()
            same = (out_idx == read_bytes(stem + ".IDX", "idx")
                    and out_dat == read_bytes(stem + ".DAT", "dat"))
            assert same, "gm.bigfile.pack output is not reproduced"
            return "3 entries from gm.bigfile.pack reproduced byte for byte"
        check("roundtrip: archive written by gm.bigfile.pack", rt_oracle)

        # ---- empty archive ----------------------------------------------
        def rt_empty():
            idx0, dat0 = _synth([])
            p = _write_pair_raw(tmp, "empty", idx0, dat0)
            archive = parse_archive(p)
            assert archive.count == 0 and archive.dat_size == 0
            assert archive.to_bytes() == (idx0, dat0)
            return "count 0, empty DAT, index identical"
        check("roundtrip: empty archive", rt_empty)

        # ---- replacement: grow / shrink / same size ----------------------
        base_entries = [(b"KEEP1\0\0\0", b"k" * 32), (b"MID\0\0\0\0\0", b"m" * 64),
                        (b"KEEP2\0\0\0", b"n" * 16)]
        base_idx, base_dat = _synth(base_entries)
        base = _write_pair_raw(tmp, "base", base_idx, base_dat)

        def replace(size: int, tag: str, same_index: bool = False):
            archive = parse_archive(base)
            blob = bytes((i * 7) & 0xFF for i in range(size))
            entries, blobs = archive.replaced(1, blob)
            out_idx_bytes, out_dat_bytes = archive.to_bytes(blobs, entries)
            out = os.path.join(tmp, tag)
            write_pair(out + ".IDX", out + ".DAT", out_idx_bytes, out_dat_bytes,
                       force=True, game=None)
            result = verify_replace(archive, out + ".IDX", 1, blob, added=False)
            assert not result["problems"], result["problems"]
            assert result["others_identical"] == 2, result["others_identical"]
            reread = parse_archive(out + ".IDX")
            assert reread.entries[1].size == size
            assert reread.entries[0].blob == base_entries[0][1]
            assert reread.entries[2].blob == base_entries[2][1]
            if same_index:
                assert out_idx_bytes == base_idx, "same-size replace changed the index"
            else:
                assert reread.entries[2].offset == 32 + size, reread.entries[2].offset
            return f"size 64 -> {size}, index {'identical' if same_index else 'offsets shifted'}"
        check("replace: grow (64 -> 200)", lambda: replace(200, "grow"))
        check("replace: shrink (64 -> 10)", lambda: replace(10, "shrink"))
        check("replace: same size keeps the index byte-identical",
              lambda: replace(64, "same", same_index=True))

        # ---- empty payload / empty entry ---------------------------------
        def replace_empty():
            archive = parse_archive(base)
            entries, blobs = archive.replaced(0, b"")
            oi, od = archive.to_bytes(blobs, entries)
            p = os.path.join(tmp, "zero")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True, game=None)
            result = verify_replace(archive, p + ".IDX", 0, b"", added=False)
            assert not result["problems"], result["problems"]
            reread = parse_archive(p + ".IDX")
            assert reread.entries[0].size == 0
            assert reread.entries[1].offset == reread.entries[0].offset
            return "entry 0 is 0 bytes, entry 1 shares its offset"
        check("replace: empty payload", replace_empty)

        def fill_empty():
            archive = parse_archive(base)
            entries, blobs = archive.replaced(1, b"x")
            assert len(blobs) == 3
            return "empty entry had size 0 and accepted a 1-byte payload"
        check("replace: previously empty entry accepts data", fill_empty)

        # ---- duplicates: first match only --------------------------------
        dup_entries = [(b"DUP\0\0\0\0\0", b"first"), (b"DUP\0\0\0\0\0", b"second")]
        dup_idx, dup_dat = _synth(dup_entries)
        dup_path = _write_pair_raw(tmp, "dup", dup_idx, dup_dat)

        def dup_replace():
            archive = parse_archive(dup_path)
            assert archive.find("dup") == 0
            entries, blobs = archive.replaced(0, b"NEW!")
            oi, od = archive.to_bytes(blobs, entries)
            p = os.path.join(tmp, "dupout")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True, game=None)
            result = verify_replace(archive, p + ".IDX", 0, b"NEW!", added=False)
            assert not result["problems"], result["problems"]
            assert parse_archive(p + ".IDX").entries[1].blob == b"second"
            return "only the first DUP changed, the second stayed"
        check("duplicates: replace hits the first record", dup_replace)

        # ---- append a new entry ------------------------------------------
        def add_entry():
            archive = parse_archive(base)
            name = encode_name_bytes("NEWONE")
            entries, blobs = archive.appended(name, b"payload!")
            oi, od = archive.to_bytes(blobs, entries)
            p = os.path.join(tmp, "add")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True, game=None)
            result = verify_replace(archive, p + ".IDX", 3, b"payload!", added=True)
            assert not result["problems"], result["problems"]
            reread = parse_archive(p + ".IDX")
            assert reread.count == 4 and reread.entries[3].name == "NEWONE"
            assert reread.entries[3].offset + reread.entries[3].size == reread.dat_size
            return "count 3 -> 4, new name appended, others identical"
        check("append: --add grows count and keeps entries", add_entry)

        # ---- name rules ---------------------------------------------------
        def name_rules():
            for good in ("A", "ABC12345", "A-B_C.D+", "X'Y"):
                raw = encode_name_bytes(good)
                assert len(raw) == 8 and raw.rstrip(b"\0") == good.encode("latin1")
            ok, _ = _expect_error(lambda: encode_name_bytes("TOOLONG99"), "1..8 bytes")
            assert ok, "9-byte name accepted"
            ok, _ = _expect_error(lambda: encode_name_bytes(""), "1..8 bytes")
            assert ok, "empty name accepted"
            ok, _ = _expect_error(lambda: encode_name_bytes("lower"), "upper-case")
            assert ok, "lower-case name accepted without --allow-any-name"
            assert len(encode_name_bytes("lower", allow_any=True)) == 8
            return "1..8 bytes; lower case needs --allow-any-name"
        check("names: length, charset and case", name_rules)

        # ---- corrupt inputs ----------------------------------------------
        def corrupt():
            cases = []
            idx_bad = b"BIGFILX" + bytes([SEP]) + struct.pack("<I", 1)
            p = _write_pair_raw(tmp, "badmagic", idx_bad, b"")
            cases.append((p, "magic", "not a BIGFILE index"))

            p = _write_pair_raw(tmp, "short", MAGIC + bytes([SEP]) + b"\x00", b"")
            cases.append((p, "short header", "needs 12"))

            idx_short = MAGIC + bytes([SEP]) + struct.pack("<I", 5) + b"A" * 12
            p = _write_pair_raw(tmp, "shorttab", idx_short, b"x")
            cases.append((p, "short table", "index too short"))

            idx_huge = MAGIC + bytes([SEP]) + struct.pack("<I", 5_000_000)
            p = _write_pair_raw(tmp, "huge", idx_huge, b"")
            cases.append((p, "implausible count", "implausible"))

            idx_back = (MAGIC + bytes([SEP]) + struct.pack("<I", 2)
                        + b"A\0\0\0\0\0\0\0" + struct.pack("<I", 10)
                        + b"B\0\0\0\0\0\0\0" + struct.pack("<I", 4))
            p = _write_pair_raw(tmp, "back", idx_back, b"x" * 20)
            cases.append((p, "offsets backwards", "goes backwards"))

            idx_far = (MAGIC + bytes([SEP]) + struct.pack("<I", 1)
                       + b"A\0\0\0\0\0\0\0" + struct.pack("<I", 99))
            p = _write_pair_raw(tmp, "far", idx_far, b"x" * 4)
            cases.append((p, "offset past EOF", "beyond DAT"))

            for path, label, needle in cases:
                ok, text = _expect_error(lambda path=path: parse_archive(path), needle)
                assert ok, f"{label}: {text}"
            missing = os.path.join(tmp, "nodat")
            write_bytes(missing + ".IDX",
                        MAGIC + bytes([SEP]) + struct.pack("<I", 0))
            ok, text = _expect_error(lambda: parse_archive(missing + ".IDX"),
                                     "data file not found")
            assert ok, f"missing DAT: {text}"
            return f"{len(cases) + 1} malformed cases rejected with clear messages"
        check("errors: truncated/forged indexes and a missing DAT", corrupt)

        # ---- check mode ---------------------------------------------------
        def check_mode():
            good = parse_archive(base)
            assert not check_mode_problems(base), check_mode_problems(base)
            idx_warn, dat_warn = _synth([(b"low\0\0\0\0\0", b"x"),
                                         (b"low\0\0\0\0\0", b"y")],
                                        prefix=b"ZZ", idx_tail=b"T")
            p = _write_pair_raw(tmp, "warn", idx_warn, dat_warn)
            problems = check_mode_problems(p)
            assert not problems, problems
            text = check_mode_text(p)
            assert "warning" in text and "duplicate" in text and "upper case" in text
            return "clean archive passes; lower case/duplicates become warnings"
        check("--check: structural and oracle verification", check_mode)

        # ---- verification must fail on a tampered output ------------------
        def tamper():
            archive = parse_archive(base)
            entries, blobs = archive.replaced(1, b"m" * 64)
            oi, od = archive.to_bytes(blobs, entries)
            p = os.path.join(tmp, "tamper")
            write_pair(p + ".IDX", p + ".DAT", oi, od, force=True, game=None)
            # flip one byte in entry 2's payload: verification must notice
            broken = bytearray(od)
            broken[32 + 64 + 5] ^= 0xFF
            write_bytes(p + ".DAT", bytes(broken))
            result = verify_replace(archive, p + ".IDX", 1, b"m" * 64, added=False)
            assert result["problems"], "tampered DAT passed verification"
            return f"{len(result['problems'])} problem(s) reported for a flipped byte"
        check("verify: a flipped DAT byte is detected", tamper)

        # ---- idempotence of repeated rebuilds ------------------------------
        def twice():
            archive = parse_archive(base)
            first = archive.to_bytes()
            second = parse_archive(base).to_bytes()
            assert first == second
            return "two independent rebuilds agree"
        check("determinism: rebuild is idempotent", twice)

    passed = sum(1 for c in checks if c["ok"])
    total = len(checks)
    lines = [f"{'ok  ' if c['ok'] else 'FAIL'} {c['check']}: {c['detail']}"
             for c in checks]
    lines.append(f"self-test: {passed}/{total} checks passed")
    payload = {"mode": "self-test", "checks": checks, "passed": passed,
               "total": total, "ok": passed == total}
    emit(payload, as_json, lines)
    return 0 if passed == total else 1


def check_mode_problems(idx_path: str) -> list[str]:
    """Thin helper for the self-test: run ``--check`` in JSON mode in-process."""
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        cmd_check(idx_path, None, None, None, True)
    payload = json.loads(buf.getvalue())
    return payload["problems"]


def check_mode_text(idx_path: str) -> str:
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        cmd_check(idx_path, None, None, None, False)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bigpack.py",
        description="Pack, list and verify Gunmetal BIGFILE (.IDX + .DAT) archives.")
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--roundtrip", action="store_true",
                      help="repack every shipped .IDX/.DAT and compare byte for byte")
    mode.add_argument("--list", metavar="ARCHIVE.IDX",
                      help="print the entry table (name, size, offset, FNV-1a 64)")
    mode.add_argument("--replace", metavar="ARCHIVE.IDX",
                      help="replace one entry and write a new .IDX/.DAT pair")
    mode.add_argument("--check", metavar="PREFIX.IDX",
                      help="verify a written archive (table, oracle, other entries)")
    mode.add_argument("--self-test", action="store_true",
                      help="synthetic archives only, no game files")
    p.add_argument("--name", help="entry name for --replace/--check")
    p.add_argument("--file", help="new payload for --replace")
    p.add_argument("--out", metavar="PREFIX",
                   help="output prefix for --replace (writes PREFIX.IDX/PREFIX.DAT)")
    p.add_argument("--add", action="store_true",
                   help="append the entry when --name is not present")
    p.add_argument("--allow-any-name", action="store_true",
                   help="with --add: accept printable ASCII instead of [A-Z0-9_+.-']")
    p.add_argument("--against", metavar="SOURCE.IDX",
                   help="with --check: compare every non-target entry with this archive")
    p.add_argument("--expect", metavar="FILE",
                   help="with --check --against: the target entry must equal this file")
    p.add_argument("--game", default=DEFAULT_GAME,
                   help=f"installation directory (default {DEFAULT_GAME}, "
                        f"$GUNMETAL_GAME)")
    p.add_argument("--tmp-dir", default=os.environ.get("BIGPACK_TMP", "/tmp"),
                   help="scratch directory for --roundtrip (default /tmp)")
    p.add_argument("--limit", type=int, default=0,
                   help="--list: print at most N entries (0 = all)")
    p.add_argument("--force", action="store_true",
                   help="overwrite existing output files")
    p.add_argument("--json", action="store_true", help="one JSON line of output")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            return run_self_test(args.json)
        if args.roundtrip:
            return cmd_roundtrip(args.game, args.tmp_dir, args.json)
        if args.list:
            return cmd_list(args.list, args.json, args.limit)
        if args.replace:
            for flag, value in (("--name", args.name), ("--file", args.file),
                                ("--out", args.out)):
                if not value:
                    raise BigpackError(f"--replace needs {flag}")
            return cmd_replace(args.replace, args.name, args.file, args.out,
                               add=args.add, force=args.force,
                               game=args.game if os.path.isdir(args.game) else None,
                               as_json=args.json, allow_any=args.allow_any_name)
        if args.check:
            if args.name is None and (args.against or args.expect):
                raise BigpackError("--check with --against/--expect needs --name")
            return cmd_check(args.check, args.against, args.name, args.expect, args.json)
        raise BigpackError("no mode selected")  # pragma: no cover - argparse
    except VerifyError as exc:
        print(f"bigpack: verification failed: {exc}", file=sys.stderr)
        return 1
    except BigpackError as exc:
        print(f"bigpack: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:  # pragma: no cover
        print("bigpack: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
