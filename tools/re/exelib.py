#!/usr/bin/env python3
"""Shared helpers for poking at the shipped ``gundx.exe`` (read-only).

The game executable lives outside the repository, so every tool accepts
``GUNMETAL_EXE`` (default: the installed game).  Nothing here writes to it.

PE layout of this build (derived from the header at run time, listed here so
the addresses quoted across ``docs/FORMATS.md`` / ``NOTES.md`` make sense)::

    .text   0x401000 .. 0x4a1000   file offset = vma - 0x400a00
    .rdata  0x4a1000 .. 0x4a3000   file offset = vma - 0x400a00
    .data   0x4a3000 .. 0x5b0000   file offset = vma - 0x401a00

The disassembly listing used by :func:`disasm` is a plain ``objdump`` dump::

    objdump -D -M intel "gundx.exe" > /tmp/opencode/gundx.asm

Its path can be overridden with ``GUNDX_ASM``.
"""
import os
import re
import struct
import tempfile

DEFAULT_EXE = "/mnt/d/games/Gunmetal 3dfx/gundx.exe"
EXE = os.environ.get("GUNMETAL_EXE", DEFAULT_EXE)

DEFAULT_ASM = os.path.join(tempfile.gettempdir(), "opencode", "gundx.asm")
ASM = os.environ.get("GUNDX_ASM", DEFAULT_ASM)

# operand letters -> byte size (engine table at 0x457204, walker 0x457170):
# A/B/D are 8 bytes, Y is 1, S is a NUL-terminated string, the rest are 4.
LETTER_SIZE = {
    "A": 8, "B": 8, "D": 8, "Y": 1, "S": "str",
    "E": 4, "G": 4, "I": 4, "L": 4, "M": 4, "N": 4,
    "O": 4, "P": 4, "R": 4, "T": 4, "V": 4, "Z": 4,
}

_ASM_LINE = re.compile(r"^\s*([0-9a-f]{6,8}):\t")

_data = None
_sections = None


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


def asm_rows():
    """``[(vma, line)]`` read from the pre-generated objdump listing."""
    if not os.path.exists(ASM):
        raise FileNotFoundError(
            "no disassembly listing at %s -- generate it with:\n"
            "    objdump -D -M intel \"%s\" > %s" % (ASM, EXE, ASM))
    rows = []
    with open(ASM, "r", errors="replace") as fh:
        for line in fh:
            m = _ASM_LINE.match(line)
            if m:
                rows.append((int(m.group(1), 16), line.rstrip("\n")))
    return rows


def disasm(lo, hi=None, lines=None):
    """Lines of the objdump listing inside ``[lo, hi)`` (or the first N)."""
    rows = asm_rows()
    if lines is not None:
        return [ln for v, ln in rows if v >= lo][:lines]
    if hi is None:
        hi = lo + 0x100
    return [ln for v, ln in rows if lo <= v < hi]
