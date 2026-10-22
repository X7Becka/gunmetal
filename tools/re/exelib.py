#!/usr/bin/env python3
"""Shared helpers for poking at the shipped ``gundx.exe`` (read-only).

The game executable lives outside the repository, so every tool accepts
``GUNMETAL_EXE`` (default: the installed game).  Nothing here writes to it.

PE layout of this build (derived from the header at run time, listed here so
the addresses quoted across ``docs/FORMATS.md`` / ``NOTES.md`` make sense)::

    .text   0x401000 .. 0x4a1000   file offset = vma - 0x400a00
    .rdata  0x4a1000 .. 0x4a3000   file offset = vma - 0x400a00
    .data   0x4a3000 .. 0x5b0000   file offset = vma - 0x401a00

Disassembly listing
-------------------

The listing is a plain ``objdump`` dump::

    objdump -D -M intel "gundx.exe" > <cache>/gundx.asm

It lives in the *persistent* cache (default ``~/.cache/gunmetal-re``, override
with ``GUNMETAL_CACHE``), not in ``/tmp``: ``/tmp`` here is tmpfs, so
``wsl --shutdown`` used to erase the listing (and ``/tmp/opencode/gundx.exe``)
and every RE tool then failed until it was regenerated.  A legacy listing in
``/tmp/opencode`` is still honoured when the cache copy is absent.

The first call that needs the listing builds it from ``GUNMETAL_EXE`` (if
``objdump`` is available) and writes a binary sidecar index
``gundx.asm.idx`` (magic, source size/mtime, sorted VMA + line offset arrays).
``disasm()`` then binary-searches the index and reads only the requested
lines: 20 range queries used to re-parse all 283k listing lines per call
(~100 ms each), now they cost a few ms in total.
"""
import array
import bisect
import hashlib
import os
import re
import struct
import subprocess
import tempfile

DEFAULT_EXE = "/mnt/d/games/Gunmetal 3dfx/gundx.exe"
EXE = os.environ.get("GUNMETAL_EXE", DEFAULT_EXE)

_CACHE_DIR = os.environ.get("GUNMETAL_CACHE") or os.path.join(
    os.path.expanduser("~"), ".cache", "gunmetal-re")
_LEGACY_ASM = os.path.join(tempfile.gettempdir(), "opencode", "gundx.asm")


def cache_dir() -> str:
    """Directory for rebuildable RE artefacts (listing, index, exe copy)."""
    return _CACHE_DIR


def cache_path(name: str) -> str:
    return os.path.join(_CACHE_DIR, name)


def asm_path() -> str:
    """Resolved listing path: ``GUNDX_ASM``, cache copy, legacy /tmp copy."""
    env = os.environ.get("GUNDX_ASM")
    if env:
        return env
    cached = cache_path("gundx.asm")
    if os.path.exists(cached):
        return cached
    if os.path.exists(_LEGACY_ASM):
        return _LEGACY_ASM
    return cached


ASM = asm_path()

# operand letters -> byte size (engine table at 0x457204, walker 0x457170):
# A/B/D are 8 bytes, Y is 1, S is a NUL-terminated string, the rest are 4.
LETTER_SIZE = {
    "A": 8, "B": 8, "D": 8, "Y": 1, "S": "str",
    "E": 4, "G": 4, "I": 4, "L": 4, "M": 4, "N": 4,
    "O": 4, "P": 4, "R": 4, "T": 4, "V": 4, "Z": 4,
}

_ASM_LINE = re.compile(r"^\s*([0-9a-f]{6,8}):\t")
_ASM_LINE_B = re.compile(rb"^\s*([0-9a-f]{6,8}):\t")
_IDX_MAGIC = b"GMASM2\0\0"

_data = None
_sections = None
_rows = None
_index = None          # (vmas array, offsets array, listing path)
_ENSURED = False


def data():
    """Return the whole executable image."""
    global _data
    if _data is None:
        with open(EXE, "rb") as fh:
            _data = fh.read()
    return _data


def sections():
    """``[(name, vma, rawsize, fileoff)]`` parsed from the PE header.

    Virtual addresses = ``ImageBase`` (0x400000) + the header's RVA.
    """
    global _sections
    if _sections is None:
        d = data()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        image_base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        base = pe + 24 + optsz
        out = []
        for i in range(nsec):
            o = base + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            _vsize, rva, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
            out.append((name, image_base + rva, rawsize, rawoff))
        _sections = out
    return _sections


def virtual_sections():
    """``[(name, vma, vsize, rawsize, fileoff)]`` — like :func:`sections` but
    with the *virtual* size, which is what the loader maps (``.data`` keeps its
    ``.bss`` tail in memory only: ``0x4bf000..0x840b60`` is not in the file).
    """
    d = data()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    image_base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    base = pe + 24 + optsz
    out = []
    for i in range(nsec):
        o = base + 40 * i
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, rva, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, image_base + rva, vsize or rawsize, rawsize, rawoff))
    return out


def va2off(v):
    """Virtual address -> file offset, or ``None`` when unmapped."""
    for _name, vma, vsize, foff in sections():
        if vma <= v < vma + vsize:
            return foff + (v - vma)
    return None


def off2va(o):
    """File offset -> virtual address, or ``None`` when outside any section."""
    for _name, vma, vsize, foff in sections():
        if foff <= o < foff + vsize:
            return vma + (o - foff)
    return None


def cstr(v, limit=200):
    """NUL-terminated string at VMA ``v`` (latin1), decoded leniently."""
    o = va2off(v)
    if o is None:
        return None
    return data()[o:o + limit].split(b"\0")[0].decode("latin1")


def strings_in(lo, hi, limit=200):
    """``[(vma, text)]`` for every NUL-terminated string in ``[lo, hi)``."""
    out = []
    a = lo
    while a < hi:
        o = va2off(a)
        if o is None:
            a += 1
            continue
        s = data()[o:o + limit].split(b"\0")[0].decode("latin1")
        if s:
            out.append((a, s))
            a += len(s) + 1
        else:
            a += 1
    return out


def sig_size(sig):
    """Total byte size of one record described by an operand signature."""
    total = 0
    for ch in sig:
        size = LETTER_SIZE.get(ch)
        if size is None:
            raise ValueError("bad signature letter %r" % ch)
        if size != "str":
            total += size
    return total


# --- disassembly listing -------------------------------------------------

def _stamp(path):
    st = os.stat(path)
    return st.st_size, st.st_mtime_ns


def ensure_asm(quiet=True):
    """Make sure the listing exists; build it once into the cache.

    Returns the path.  Raises ``FileNotFoundError`` when neither the listing
    nor ``gundx.exe``/``objdump`` is available.
    """
    global _ENSURED
    path = asm_path()
    if os.path.exists(path):
        _ENSURED = True
        return path
    if not quiet:
        print("exelib: building %s from %s" % (path, EXE))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp.%d" % os.getpid()
    try:
        with open(tmp, "wb") as fh:
            rc = subprocess.run(["objdump", "-D", "-M", "intel", EXE],
                                stdout=fh, stderr=subprocess.DEVNULL).returncode
        if rc != 0:
            raise RuntimeError("objdump failed with status %d" % rc)
        os.replace(tmp, path)
    except FileNotFoundError as exc:
        raise FileNotFoundError(
            "no disassembly listing at %s and cannot build it from %s: %s\n"
            "    install binutils or set GUNDX_ASM to an existing listing"
            % (path, EXE, exc)) from exc
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    _ENSURED = True
    return path


def _index_path(path):
    return path + ".idx"


def _load_index(path):
    """Load the sidecar index; ``None`` when it is missing or stale."""
    ipath = _index_path(path)
    try:
        with open(ipath, "rb") as fh:
            blob = fh.read()
    except OSError:
        return None
    if len(blob) < 32 or blob[:8] != _IDX_MAGIC:
        return None
    size, mtime, count = struct.unpack_from("<QQI", blob, 8)
    if (size, mtime) != _stamp(path):
        return None
    want = 32 + 8 * count
    if len(blob) != want:
        return None
    vmas = array.array("I")
    offs = array.array("I")
    vmas.frombytes(blob[32:32 + 4 * count])
    offs.frombytes(blob[32 + 4 * count:])
    return vmas, offs


def _build_index(path):
    """Parse the listing once and write ``<listing>.idx`` atomically."""
    vmas = array.array("I")
    offs = array.array("I")
    pos = 0
    with open(path, "rb") as fh:
        for line in fh:
            m = _ASM_LINE_B.match(line)
            if m:
                vmas.append(int(m.group(1), 16))
                offs.append(pos)
            pos += len(line)
    size, mtime = _stamp(path)
    header = _IDX_MAGIC + struct.pack("<QQI", size, mtime, len(vmas)) + b"\0" * 4
    tmp = _index_path(path) + ".tmp.%d" % os.getpid()
    with open(tmp, "wb") as fh:
        fh.write(header)
        fh.write(vmas.tobytes())
        fh.write(offs.tobytes())
    os.replace(tmp, _index_path(path))
    return vmas, offs


def asm_index():
    """``(vmas, offsets, listing_path)`` for the disassembly listing.

    The sidecar is rebuilt automatically when the listing changes (size or
    mtime), so the index can never go stale silently.
    """
    global _index
    path = ensure_asm()
    key = os.path.abspath(path)
    if _index is not None and _index[2] == key:
        return _index
    loaded = _load_index(path)
    if loaded is None:
        loaded = _build_index(path)
    _index = (loaded[0], loaded[1], key)
    return _index


def asm_rows():
    """``[(vma, line)]`` for the whole listing (parsed once per process).

    Tools that scan every operand use this; address-range queries should use
    :func:`disasm`, which touches only the requested lines.
    """
    global _rows
    if _rows is not None:
        return _rows
    path = ensure_asm()
    vmas, offs, _key = asm_index()
    with open(path, "rb") as fh:
        blob = fh.read()
    rows = []
    for i, vma in enumerate(vmas):
        end = offs[i + 1] if i + 1 < len(vmas) else len(blob)
        line = blob[offs[i]:end].split(b"\n", 1)[0].rstrip(b"\r")
        rows.append((vma, line.decode("latin1")))
    _rows = rows
    return rows


def asm_iter():
    """Iterate ``(vma, line)`` without materialising a list of tuples."""
    path = ensure_asm()
    vmas, _offs, _key = asm_index()
    with open(path, "rb") as fh:
        for vma, line in zip(vmas, fh):
            yield vma, line.rstrip(b"\n").decode("latin1", "replace")


def disasm(lo, hi=None, lines=None):
    """Lines of the objdump listing inside ``[lo, hi)`` (or the first N).

    Binary search + seek: only the requested part of the listing is read.
    """
    path = ensure_asm()
    vmas, offs, _key = asm_index()
    start = bisect.bisect_left(vmas, lo)
    if lines is not None:
        stop = min(len(vmas), start + max(0, lines))
    else:
        if hi is None:
            hi = lo + 0x100
        stop = bisect.bisect_left(vmas, hi, start)
    if stop <= start:
        return []
    begin = offs[start]
    end = offs[stop] if stop < len(vmas) else None
    with open(path, "rb") as fh:
        fh.seek(begin)
        blob = fh.read() if end is None else fh.read(end - begin)
    return blob.decode("latin1").splitlines()


def listing_info():
    """Compact provenance: listing path, size, line count, index state."""
    path = ensure_asm()
    vmas, _offs, _key = asm_index()
    size, mtime = _stamp(path)
    return {"listing": path, "bytes": size, "mtime_ns": mtime,
            "lines": len(vmas),
            "index": _index_path(path),
            "index_ok": os.path.exists(_index_path(path))}


def exe_sha256():
    """SHA-256 of ``GUNMETAL_EXE`` (provenance for every claim in the docs)."""
    h = hashlib.sha256()
    with open(EXE, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def asm_rows_legacy():  # pragma: no cover - kept for old callers
    return asm_rows()
