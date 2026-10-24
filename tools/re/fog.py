#!/usr/bin/env python3
"""Depth fog of ``gundx.exe`` 1.36: what ``z`` is, the LUT, the Glide state (M4.1.6).

Answer in four lines.  The software frame path (``0x4409b8``) replaces every
pixel it draws with::

    fbuf[i] = lighttable[level*0x100 + fbuf[i]]          0x441b2d..0x441b5c

    z     = |(64 / a0) * (e2 . (p_world - eye))|          0x440c98..0x440ed1
    level = clamp(63 - ftol(z), 4, 63)                    0x441921..0x44197d
    ftol  = truncation toward zero (``fistp``, RC=11)     0x40e0e0 ``0x40e0f9``

So ``z`` is **the camera-space forward distance, scaled by ``64/a0``** -- not a
square root, not ``1/z``, not ``far - depth``:

* ``e2`` is the third row of the camera basis, ``ctx+0x48`` (``docs/CAMERA.md``
  §4: the view direction); ``p_world - eye`` is the vertex in camera space
  (``0x440e3a``..``0x440e5a``), rotated by the three rows ``0x440c1d``..;
* ``a0 = ctx+0xa0`` = ``0.8 * W * k(level)`` (``0x44667a``..``0x4466da``):
  ``256`` at 320x200 when ``k = 1``, so the scale is ``64/256 = 0.25``;
* ``0x4804b0`` is **``fabs``, not ``sqrt``**: it masks the sign bit of the high
  dword and returns (``0x48056e``: ``and ecx,0x7fffffff``) and there is not a
  single ``fsqrt`` (``d9 fa``) in ``0x4409b8``..``0x4420d0``.  The old note
  "call 0x4804b0, похоже на CRT sqrt" is wrong; ``docs/WORLD_FIELDS.md`` already
  calls it ``fabs``.
* the interpolation between vertices is plain linear in the major screen axis
  (``0x4417bc``..``0x4418b7``: ``v += (i - i0) * dv/di``), so ``level`` only
  changes along that major axis (constant across the minor one).

``lighttable`` (``ds:0x567104``, built at startup by ``0x4201fe``) is a
256x256 byte LUT ``[row*0x100 + col]``::

    row < 0x40    c' = min(63, (pal6[c]*(0x40 - row) + row*0x40) >> 6)  0x42024c
    0x40..0x7f    white (63,63,63)                                      0x4202f4
    row >= 0x80   identity: table[row][col] = col                       0x4204e2

i.e. a *lerp of the palette colour toward white*, weight ``row/64``; row 0 is
the palette itself, row 0x3f (the last reachable one, level is clamped to 63)
is white.  ``pal6`` is the 6-bit engine palette at ``0x83d320`` (the BMP loader
shifts the 8-bit file palette by 2: ``0x47f0b4``/``0x47f0bd``/``0x47f0c6``).
The final ``(r<<12|g<<6|b) -> palette index`` step goes through the 262144-byte
dither tables ``0x567128``/``0x56712c`` that are built at run time, so this tool
substitutes "nearest palette entry" (exact colour matches in row 0 are checked).

The 3dfx path of the same function uses Glide fog instead: ``grFogMode(2)``
(``0x440a8c``), ``grFogColorValue(0xffffff)`` (``0x440a97``),
``guFogGenerateExp(table, 0.9f)`` (``0x440aa6``) into a 64-byte stack buffer
(``[ebp-0x4c]``) and ``grFogTable(table)`` (``0x440ab0``).  Per-level overrides
live at ``0x41711f``.. (mode ``0x202``, colour ``level+0x30``) and
``0x41889c`` (``guFogGenerateLinear(table, 0.25*c4, c4)``).

Usage::

    python3 tools/re/fog.py                     # отчёт
    python3 tools/re/fog.py --json              # машинночитаемый
    python3 tools/re/fog.py --table             # глубина -> level -> цвет
    python3 tools/re/fog.py --glide             # точки вызова grFog*/guFog*
    python3 tools/re/fog.py --self-test
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for p in (HERE, ROOT):
    if p not in sys.path:
        sys.path.insert(0, p)

import exelib  # noqa: E402

SCHEMA = "gunmetal/fog/1"

# --------------------------------------------------------------------------
# engine constants (addresses are proven by --self-test against the bytes)
# --------------------------------------------------------------------------

FRAME_FN = 0x4409B8          # "draw what 0x4409b8 draws"; holds the fog pass
FABS_FN = 0x4804B0           # fabs, NOT sqrt
FTOL_FN = 0x40E0E0           # double -> int32, round to nearest
LT_BUILDER = 0x4201FE        # lighttable generator
LIGHTTABLE = 0x567104        # ds:0x567104 = 256*256 bytes
PALETTE = 0x83D320           # 6-bit engine palette (768 bytes)
PAL_LOAD_SHR2 = 0x47F0B4     # 8-bit BMP palette -> 6-bit (shr dl,2)
MAT_TABLE = 0x5670E0         # +0x10 of every record = lighttable (0x4409cb)
CONST_64 = 0x4A2298          # 64.0, the numerator of the depth scale
DEPTH_SCALE = 0x83D240       # ctx+0xa0, written at 0x4466b2/0x4466da
FOGTAB64 = 0x4A6A40          # 64-entry ramp of the *other* (4-byte) variant

LEVEL_MIN, LEVEL_MAX = 4, 0x3F
ROW_WHITE, ROW_IDENTITY = 0x40, 0x80
DEFAULT_A0 = 256.0           # 0.8 * 320 * k(level) with k = 1
DEFAULT_EXP_DENSITY = 0.9    # 0x3f666666, guFogGenerateExp at 0x440aa6
FOG_TABLE_SIZE = 64          # the 64-byte buffer at [ebp-0x4c]

#: instruction anchors: (name, va, expected bytes, why)
ANCHORS = (
    ("lighttable from material record", 0x4409CB, "8b5110",
     "mov edx,[ecx+0x10] = material[0].lighttable"),
    ("depth scale 64/a0", 0x440C0B, "dc3d982 24a00",
     "fdivr 64.0 / ctx[0xa0]"),
    ("view axis row e2", 0x440CAA, "dd4050",
     "fld QWORD [eax+0x50] -- e2.y of ctx+0x48"),
    ("depth = fabs(dot)", 0x440EC9, "e8e2f50300",
     "call 0x4804b0 = fabs"),
    ("level = 0x3f - ftol(z) #1", 0x44194D, "b93f000000",
     "mov ecx,0x3f"),
    ("level = 0x3f - ftol(z) #2", 0x441AB6, "ba3f000000",
     "mov edx,0x3f"),
    ("level = 0x3f - ftol(z) #3", 0x441E53, "ba3f000000",
     "mov edx,0x3f"),
    ("level = 0x3f - ftol(z) #4", 0x441FC6, "b93f000000",
     "mov ecx,0x3f"),
    ("clamp low 4", 0x441963, "c785d0fdffff04000000",
     "level = max(level, 4)"),
    ("clamp high 0x3f", 0x441976, "c785d0fdffff3f000000",
     "level = min(level, 0x3f)"),
    ("lut row = level<<8", 0x441B2D, "c1e008",
     "shl eax,8"),
    ("lut read", 0x441B41, "8a1c0a",
     "mov bl,[edx+ecx] = fbuf[idx]"),
    ("lut lookup+store", 0x441B59, "8a0403",
     "mov al,[ebx+eax] = lt[level*256+px]"),
    ("lighttable: row < 0x40", 0x42024C, "837df440",
     "cmp [ebp-0xc],0x40"),
    ("lighttable: weight 0x40-row", 0x420264, "ba40000000",
     "mov edx,0x40"),
    ("lighttable: >>6", 0x420277, "c1f906",
     "sar ecx,6"),
    ("lighttable: clamp 0x3f", 0x4202CB, "837df83f",
     "cmp [ebp-0x8],0x3f"),
    ("lighttable: white from row 0x40", 0x4202F4, "c745e43f000000",
     "mov [ebp-0x1c],0x3f"),
    ("lighttable: identity rows from 0x80", 0x4204E2, "c745f480000000",
     "mov [ebp-0xc],0x80"),
    ("fabs masks the sign bit", 0x48056E, "81e1ffffff7f",
     "and ecx,0x7fffffff -- returns |x|"),
    ("palette 8-bit -> 6-bit", 0x47F0B4, "c0ea02",
     "shr dl,2 while copying the BMP palette"),
)

#: level = 0x3f - ftol(z), four copies in the frame path
LEVEL_SITES = (0x44194D, 0x441AB6, 0x441E53, 0x441FC6)
FABS_SITES = (0x440EC9, 0x441683, 0x4416A3, 0x441940, 0x441AA9, 0x441E46,
              0x441FB9)


# --------------------------------------------------------------------------
# exe access
# --------------------------------------------------------------------------

def use_exe(path: str | None):
    """Point :mod:`exelib` at ``path`` (read-only)."""
    if not path:
        return
    if os.path.abspath(path) != os.path.abspath(exelib.EXE):
        exelib.EXE = path
        exelib._data = None
        exelib._sections = None


def image() -> bytes:
    return exelib.data()


def raw(va: int, n: int) -> bytes:
    off = exelib.va2off(va)
    if off is None:
        return b""
    return image()[off:off + n]


def u32(va: int) -> int:
    b = raw(va, 4)
    return struct.unpack("<I", b)[0] if len(b) == 4 else 0


def f64(va: int) -> float:
    b = raw(va, 8)
    return struct.unpack("<d", b)[0] if len(b) == 8 else float("nan")


def f32(va: int) -> float:
    b = raw(va, 4)
    return struct.unpack("<f", b)[0] if len(b) == 4 else float("nan")


def sha256() -> str:
    return hashlib.sha256(image()).hexdigest()


# --------------------------------------------------------------------------
# the engine maths, reimplemented exactly
# --------------------------------------------------------------------------

def ftol(x: float) -> int:
    """``0x40e0e0``: ``fistp`` with RC = 11 (truncate toward zero).

    ``0x40e0ef`` ORs ``0x0c`` into the high byte of the control word, which is
    rounding control = 11 (truncate), not round-to-nearest; the value is then
    stored by ``fistp`` (``0x40e0f9``).
    """
    return int(x)


def fog_z(depth: float, a0: float = DEFAULT_A0) -> float:
    """``0x440ca7``..``0x440ed1``: ``|(64/a0) * depth|``.

    ``depth`` is the camera-space forward distance ``e2 . (p - eye)`` in world
    units; ``a0`` is ``ctx+0xa0`` (``0.8*W*k``, 256 at 320x200 for ``k = 1``).
    """
    return abs((64.0 / a0) * depth)


def fog_level_z(z: float) -> int:
    """``0x441921``..``0x44197d``: ``clamp(63 - ftol(z), 4, 63)``."""
    lvl = LEVEL_MAX - ftol(z)
    if lvl < LEVEL_MIN:
        lvl = LEVEL_MIN
    if lvl > LEVEL_MAX:
        lvl = LEVEL_MAX
    return lvl


def fog_level(depth: float, a0: float = DEFAULT_A0) -> int:
    """Fog row the engine picks for a camera-space distance ``depth``."""
    return fog_level_z(fog_z(depth, a0))


def fog_apply(rgb, level: int):
    """``0x4201fe`` for one pixel: lerp toward white with weight ``level/64``.

    ``rgb`` is a 6-bit triple (0..63), ``level`` a table row.  Row 0 is the
    identity, rows >= 0x40 are white, rows >= 0x80 are the byte identity (the
    latter two only exist for the material LUTs; the fog pass clamps to 0x3f).
    """
    if level >= ROW_IDENTITY:
        return tuple(rgb)
    if level >= ROW_WHITE:
        return (0x3F, 0x3F, 0x3F)
    w = 0x40 - level
    return tuple(min(0x3F, (c * w + level * 0x40) >> 6) for c in rgb)


def _nearest(pal6, color):
    """Palette index with the smallest squared 6-bit distance (ties: lowest)."""
    best, best_d = 0, None
    r, g, b = color
    for i, (pr, pg, pb) in enumerate(pal6):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best_d is None or d < best_d:
            best, best_d = i, d
            if d == 0:
                break
    return best


def build_lighttable(pal6, rows=None, quantize=True):
    """Rebuild ``lighttable`` (``0x4201fe``) for the given 6-bit palette.

    ``rows`` limits the work (``--self-test`` checks a handful of rows); the
    full table is 256 rows x 256 columns of bytes.  Returns
    ``{row: bytes(256)}``.  ``quantize=False`` returns the 6-bit colours.
    """
    if rows is None:
        rows = range(256)
    exact = {}
    for i, c in enumerate(pal6):
        exact.setdefault(c, i)
    out = {}
    for row in rows:
        line = bytearray(256) if quantize else [None] * 256
        for col in range(256):
            if row >= ROW_IDENTITY:
                line[col] = col
                continue
            c = fog_apply(pal6[col], row)
            if quantize:
                idx = exact.get(c)
                if idx is None:
                    idx = _nearest(pal6, c)
                line[col] = idx
            else:
                line[col] = c
        out[row] = bytes(line) if quantize else line
    return out


def gu_fog_generate_exp(density: float = DEFAULT_EXP_DENSITY,
                        size: int = FOG_TABLE_SIZE) -> list:
    """``guFogGenerateExp`` (Glide 2.x; the exe only passes ``density``).

    ``t[i] = (GrFog_t)(255 * (1 - exp(-density * i/(size-1))))``.
    """
    return [int(255.0 * (1.0 - math.exp(-density * (i / (size - 1)))))
            for i in range(size)]


def gu_fog_generate_linear(near: float, far: float,
                           size: int = FOG_TABLE_SIZE) -> list:
    """``guFogGenerateLinear`` (Glide 2.x; exe: ``0x41889c``)."""
    return [int(255.0 * (near + (far - near) * (i / (size - 1))))
            for i in range(size)]


# --------------------------------------------------------------------------
# palette
# --------------------------------------------------------------------------

def load_palette6(game: str = DEFAULT_GAME, path: str | None = None):
    """6-bit engine palette as 256 ``(r,g,b)`` tuples + provenance dict.

    The BMP carries B,G,R(,A) 8-bit entries; the loader shifts each byte right
    by 2 (``0x47f0b4``), so what sits at ``0x83d320`` is 6-bit.
    """
    info = {"source": None, "bytes": 0, "entry_size": 0, "max6": 0}
    if path:
        data = open(path, "rb").read()
        info["source"] = path
    else:
        try:
            from gm import palette as palmod
            data = palmod.installed(game)
            info["source"] = "%s/%s" % palmod.ENGINE_PALETTE
        except Exception as exc:                      # no palette carrier
            info["source"] = None
            info["error"] = str(exc)
            gray = [(i * 0x3F) // 0xFF for i in range(256)]
            info["fallback"] = "linear gray"
            return [(g, g, g) for g in gray], info
    if len(data) >= 1024 and len(data) % 4 == 0:
        step = 4
    elif len(data) >= 768 and len(data) % 3 == 0:
        step = 3
    else:
        raise ValueError("palette size %d is neither 3- nor 4-byte entries"
                         % len(data))
    info["bytes"] = len(data)
    info["entry_size"] = step
    pal = []
    for i in range(256):
        # `gm/palette.*` returns BMP entries already normalised to RGBX
        # (`gm/bmp.py`: "BMP stores palette entries as B,G,R,X — normalise to
        # R,G,B,X"), and the engine LUT index is channel-order agnostic only
        # because both halves use the same palette.  Reading this as B,G,R
        # swaps red and blue (caught 2026-10-24 by `render/fog.cpp`).
        r = data[i * step + 0] >> 2
        g = data[i * step + 1] >> 2
        b = data[i * step + 2] >> 2
        pal.append((r, g, b))
    info["max6"] = max(max(c) for c in pal)
    return pal, info


def gray_palette() -> list:
    """Synthetic ramp: index ``i`` -> 6-bit ``i*63/255`` on all channels."""
    return [tuple([(i * 0x3F) // 0xFF] * 3) for i in range(256)]


# --------------------------------------------------------------------------
# evidence gathering
# --------------------------------------------------------------------------

def code_anchors() -> list:
    out = []
    for name, va, expect, why in ANCHORS:
        want = bytes.fromhex(expect.replace(" ", ""))
        got = raw(va, len(want))
        out.append({"name": name, "addr": "0x%06x" % va,
                    "expect": want.hex(), "got": got.hex(),
                    "ok": got == want, "why": why})
    return out


def fabs_proof() -> dict:
    """``0x4804b0`` is fabs: no fsqrt, and the tail returns ``x & 0x7fff...``."""
    body = raw(FABS_FN, 0xE5)
    end = FABS_FN + len(body)
    sqrt = [i for i in range(end - FABS_FN - 1)
            if body[i] == 0xD9 and body[i + 1] == 0xFA]
    return {
        "fn": "0x%06x" % FABS_FN,
        "size": len(body),
        "fsqrt_in_body": ["0x%06x" % (FABS_FN + i) for i in sqrt],
        "returns_abs": raw(0x48056E, 6).hex() == "81e1ffffff7f",
        "sign_mask_va": "0x48056e",
        "verdict": "fabs" if not sqrt else "other",
        "note": "clears bit 31 of the mantissa word and returns; no FPU op",
    }


def rasterizer_fsqrt_scan() -> dict:
    """No ``fsqrt`` anywhere in the frame path -> ``z`` is linear."""
    hits = []
    va = FRAME_FN
    end = 0x4420D0
    while va < end:
        b = raw(va, 2)
        if len(b) == 2 and b == b"\xd9\xfa":
            hits.append("0x%06x" % va)
        va += 1
    return {"range": ["0x%06x" % FRAME_FN, "0x%06x" % end],
            "fsqrt": hits}


def depth_chain() -> dict:
    """The three multiplies that build ``e2 * (64/a0)`` (0x440ca7..0x440cc5)."""
    return {
        "scale_va": "0x440c0b",
        "scale_const": f64(CONST_64),
        "scale_const_va": "0x%06x" % CONST_64,
        "a0_va": "0x%06x" % DEPTH_SCALE,
        "a0_builder": "0x44667a..0x4466da (0.4*W*256 >> 7 = 0.8*W, *k(level))",
        "e2_cells": ["0x%06x" % v for v in (0x440C98, 0x440CAA, 0x440CBC)],
        "subtract_eye_va": "0x440e3a..0x440e5a",
        "interpolation": "0x4417bc..0x4418b7 (linear in the major screen axis)",
        "fabs_va": ["0x%06x" % v for v in FABS_SITES],
        "level_va": ["0x%06x" % v for v in LEVEL_SITES],
        "ftol_va": "0x%06x" % FTOL_FN,
        "lut_apply_va": ["0x441b2d", "0x44203d"],
    }


def glide_import_slots() -> dict:
    """name -> IAT slot, parsed out of the GetProcAddress block 0x416900.."""
    rows = list(exelib.disasm(0x416900, 0x416E30))
    slots, last = {}, None
    for line in rows:
        m = re.search(r"push\s+0x([0-9a-f]+)", line)
        if m:
            va = int(m.group(1), 16)
            s = exelib.cstr(va) if 0x400000 <= va < 0x600000 else None
            if s and (s.startswith("_gr") or s.startswith("_gu")):
                last = s.lstrip("_").split("@")[0]
        m = re.search(r"mov\s+ds:0x([0-9a-f]+),eax", line)
        if m and last:
            slots[last] = int(m.group(1), 16)
            last = None
    return slots


def glide_sites(slots: dict) -> list:
    """Every ``call DWORD PTR ds:<slot>`` with the pushes in front of it."""
    rows = list(exelib.disasm(0x401000, 0x4A1000))
    out = []
    for i, line in enumerate(rows):
        m = re.search(r"call\s+DWORD PTR ds:0x([0-9a-f]+)", line)
        if not m:
            continue
        slot = int(m.group(1), 16)
        name = next((n for n, s in slots.items() if s == slot), None)
        if name is None or not (name.startswith("grFog")
                                or name.startswith("guFog")):
            continue
        pushes = []
        for prev in reversed(rows[max(0, i - 8):i]):
            if re.search(r"\b(call|jmp|ret)\b", prev):
                break
            if re.search(r"\bpush\b|\blea\b", prev):
                pushes.append(prev.split("\t")[-1].strip())
        out.append({"addr": "0x%06x" % int(line.split(":")[0], 16),
                    "fn": name, "before": list(reversed(pushes))})
    return out


def glide_config(slots: dict, sites: list) -> dict:
    """Pull the constants that actually reach grFogMode/grFogColorValue."""
    def imm(text):
        m = re.search(r"push\s+0x([0-9a-f]+)", text)
        return int(m.group(1), 16) if m else None

    modes, colors = [], []
    for s in sites:
        vals = [imm(p) for p in s["before"]]
        vals = [v for v in vals if v is not None]
        raw_arg = s["before"][-1] if s["before"] else None
        if s["fn"] == "grFogMode":
            modes.append({"addr": s["addr"], "arg": vals[-1] if vals else None,
                          "arg_expr": raw_arg})
        elif s["fn"] == "grFogColorValue":
            colors.append({"addr": s["addr"],
                           "arg": vals[-1] if vals else None,
                           "arg_expr": raw_arg})
    return {
        "slots": {n: "0x%06x" % v for n, v in sorted(slots.items())},
        "sites": sites,
        "modes": modes,
        "colors": colors,
        "exp_density": DEFAULT_EXP_DENSITY,
        "table_size": FOG_TABLE_SIZE,
        "exp_table": gu_fog_generate_exp(),
    }


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def sample(depths, a0=DEFAULT_A0, pal6=None):
    out = []
    for d in depths:
        z = fog_z(d, a0)
        lvl = fog_level_z(z)
        rec = {"depth": round(d, 3), "z": round(z, 3), "level": lvl}
        for name, c in (("black", (0, 0, 0)), ("gray32", (32, 32, 32)),
                        ("white", (63, 63, 63))):
            rec[name] = list(fog_apply(c, lvl))
        if pal6 is not None:
            rec["idx_gray32"] = _nearest(pal6, fog_apply((32, 32, 32), lvl))
        out.append(rec)
    return out


def report(game=DEFAULT_GAME, palette_path=None, a0=DEFAULT_A0) -> dict:
    pal6, pinfo = load_palette6(game, palette_path)
    slots = glide_import_slots()
    sites = glide_sites(slots)
    rows = build_lighttable(pal6, rows=(0, 1, 2, 4, 8, 16, 32, 48, 63,
                                        64, 127, 128, 255))

    def names(row):
        return [pal6[i] for i in rows[row][:8]]

    depths = [0, 4, 8, 16, 24, 32, 48, 64, 96, 128, 160, 200, 240, 300]
    return {
        "schema": SCHEMA,
        "exe": {"path": exelib.EXE, "sha256": sha256()},
        "z": {
            "expr": "z = |(64/a0) * (e2 . (p_world - eye))|",
            "what": "camera-space forward distance (view axis), scaled by 64/a0",
            "a0": a0,
            "a0_expr": "0.8*W*k(level) -> 256 at 320x200, k=1",
            "scale_64_over_a0": 64.0 / a0,
            "fabs_not_sqrt": fabs_proof(),
            "chain": depth_chain(),
            "a0_live": f32(DEPTH_SCALE),
            "const_64": f64(CONST_64),
        },
        "level": {
            "expr": "level = clamp(63 - ftol(z), 4, 63)",
            "ftol": "fistp with RC=11 -> truncation toward zero (0x40e0ef)",
            "range": [LEVEL_MIN, LEVEL_MAX],
            "sites": ["0x%06x" % v for v in LEVEL_SITES],
        },
        "lighttable": {
            "global": "0x%06x" % LIGHTTABLE,
            "builder": "0x%06x" % LT_BUILDER,
            "size": [256, 256],
            "material_record_field": "material[0]+0x10 (0x4409cb)",
            "rows": {
                "0..0x3f": "c' = min(63,(pal6[c]*(0x40-row) + row*0x40)>>6)",
                "0x40..0x7f": "white",
                "0x80..0xff": "identity (byte = column)",
            },
            "palette": pinfo,
            "pal6_max": max(max(c) for c in pal6),
            "row0_colors": [list(c) for c in names(0)],
            "row0_idx": list(rows[0][:16]),
            "row0_after_lut": [list(pal6[v]) for v in rows[0][:8]],
            "row64": rows[64][:8].hex(),
            "row128": rows[128][:8].hex(),
            "identity_rows": list(rows[255][:8]) == list(range(8)),
            "gray_rows": {
                "%d" % r: list(fog_apply((32, 32, 32), r))
                for r in (0, 8, 16, 32, 48, 63)
            },
        },
        "sample": sample(depths, a0, pal6),
        "glide": glide_config(slots, sites),
        "fogtable_64": {
            "va": "0x%06x" % FOGTAB64,
            "entries": [hex(u32(FOGTAB64 + i * 16)) for i in range(8)],
            "note": "used by the 4-byte/accumulator variant at 0x4419c7 "
                    "(stride 0x10), not by the 8-bit fog pass",
        },
        "anchors_ok": "%d/%d" % (sum(1 for a in code_anchors() if a["ok"]),
                                 len(ANCHORS)),
    }


def print_report(rep: dict):
    z = rep["z"]
    print("z (0x440ca7..0x440ed1): %s" % z["expr"])
    print("  e2 = ctx+0x48 (view axis), a0 = ctx+0xa0 = %s" % z["a0_expr"])
    print("  64/a0 = %.4f (a0 = %.1f)   const 64.0 at 0x4a2298 = %.1f"
          % (z["scale_64_over_a0"], z["a0"], z["const_64"]))
    print("  0x4804b0 = %s (fsqrt в теле: %s); fsqrt в растеризаторе: %s"
          % (z["fabs_not_sqrt"]["verdict"],
             z["fabs_not_sqrt"]["fsqrt_in_body"] or "нет",
             rasterizer_fsqrt_scan()["fsqrt"] or "нет"))
    print("  %s  (ftol = %s)"
          % (rep["level"]["expr"], rep["level"]["ftol"]))
    lt = rep["lighttable"]
    print("\nlighttable ds:%s (builder %s, %dx%d, palette %s)"
          % (lt["global"], lt["builder"], lt["size"][0], lt["size"][1],
             lt["palette"]["source"]))
    print("  row   0 (idx): " + " ".join("%3d" % v
                                         for v in lt["row0_idx"][:8]))
    print("  row   0 (rgb): " + " ".join("%s" % (tuple(c),)
                                         for c in lt["row0_colors"]))
    print("  row  64 (idx): " + " ".join("%3d" % v for v in bytes.fromhex(
        lt["row64"])))
    print("  row 128 (idx): " + " ".join("%3d" % v for v in bytes.fromhex(
        lt["row128"])))
    print("\nglide: " + ", ".join("%s=%s" % (k, v)
                                  for k, v in rep["glide"]["slots"].items()
                                  if k in ("grFogMode", "grFogColorValue",
                                           "grFogTable", "guFogGenerateExp",
                                           "guFogGenerateLinear")))
    for m in rep["glide"]["modes"]:
        a = m["arg"]
        print("  grFogMode(%s) at %s%s"
              % (a, m["addr"],
                 "   [GR_FOG_WITH_TABLE]" if a == 2 else
                 "   [GR_FOG_WITH_TABLE|0x200 ADD2]" if a == 0x202 else
                 "   [disable]" if a == 0 else ""))
    for c in rep["glide"]["colors"]:
        if c["arg"] is None:
            print("  grFogColorValue(%s) at %s   [цвет из данных уровня]"
                  % (c["arg_expr"], c["addr"]))
        else:
            print("  grFogColorValue(0x%06x) at %s" % (c["arg"], c["addr"]))
    t = rep["glide"]["exp_table"]
    print("  guFogGenerateExp(0.9): %s ... %s"
          % (t[:6], t[-4:]))
    print("\nглубина -> level -> цвет (серая палитра, вход (32,32,32)):")
    print("  %8s %7s %5s  %s" % ("depth", "z", "level", "rgb"))
    for s in rep["sample"]:
        print("  %8.1f %7.2f %5d  %s"
              % (s["depth"], s["z"], s["level"], tuple(s["gray32"])))
    print("\nаудит: %d/%d якорей, fabs=%s"
          % (sum(1 for a in code_anchors() if a["ok"]), len(ANCHORS),
             rep["z"]["fabs_not_sqrt"]["verdict"]))


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

def self_test(game=DEFAULT_GAME, palette_path=None) -> bool:
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        if not cond:
            ok = False
        print("[%s] %s%s" % ("ok" if cond else "FAIL", name,
                             (" -- " + detail) if detail else ""))

    # 1. bytes still match the model
    bad = [a for a in code_anchors() if not a["ok"]]
    check("якоря инструкций (%d)" % len(ANCHORS), not bad,
          "" if not bad else "; ".join("%s@%s" % (a["name"], a["addr"])
                                       for a in bad))
    for a in code_anchors():
        if not a["ok"]:
            print("    %s: ждали %s, получили %s"
                  % (a["addr"], a["expect"], a["got"]))

    # 2. 0x4804b0 is fabs, nothing in the fog pass is a sqrt
    fp = fabs_proof()
    check("0x4804b0 = fabs (нет fsqrt, есть маска знака)",
          fp["verdict"] == "fabs" and fp["returns_abs"],
          "fsqrt: %s" % (fp["fsqrt_in_body"] or "нет"))
    scan = rasterizer_fsqrt_scan()
    check("в 0x4409b8..0x4420d0 нет fsqrt (%d..%d)"
          % (FRAME_FN, 0x4420D0), not scan["fsqrt"], str(scan["fsqrt"]))

    # 3. ftol = round-to-nearest, ties to even
    want = {0.0: 0, 0.4: 0, 0.5: 0, 0.99: 0, 1.0: 1, 1.5: 1, 2.6: 2,
            3.9: 3, -0.5: 0, -2.7: -2}
    check("ftol: truncation toward zero (RC=11, не «к ближайшему»)",
          all(ftol(k) == v for k, v in want.items()),
          str({k: (ftol(k), v) for k, v in want.items() if ftol(k) != v}))

    # 4. level: bounds, monotonicity, endpoints
    levels = [fog_level(d / 4.0) for d in range(0, 2001)]
    check("level в [4,63] на depth 0..500", all(
        LEVEL_MIN <= v <= LEVEL_MAX for v in levels))
    check("level(0) = 63, level(огромная) = 4",
          fog_level(0.0) == LEVEL_MAX and fog_level(1e9) == LEVEL_MIN,
          "%d/%d" % (fog_level(0.0), fog_level(1e9)))
    mono = all(levels[i] >= levels[i + 1] for i in range(len(levels) - 1))
    check("level монотонно не растёт по глубине", mono)
    check("z(b) - z(a) = (64/a0)(b-a), a0=256 -> шаг 0.25",
          abs(fog_z(100.0) - 25.0) < 1e-9 and abs(fog_z(100.0, 128.0) - 50.0)
          < 1e-9)

    # 5. table: row 0 identity in colour, rows 0x40.. white, >=0x80 byte id
    pal6, pinfo = load_palette6(game, palette_path)
    rows = build_lighttable(pal6, rows=(0, 1, 4, 32, 63, 64, 127, 128, 255),
                            quantize=False)
    check("lighttable row 0 = палитра (тождество цвета)",
          all(tuple(rows[0][c]) == pal6[c] for c in range(256)),
          "палитра: %s" % pinfo["source"])
    check("lighttable row 0x3f = белый", all(c == (0x3F, 0x3F, 0x3F)
                                            for c in rows[63]))
    check("lighttable rows 0x40..0x7f = белый",
          all(c == (0x3F, 0x3F, 0x3F) for r in (64, 127)
              for c in rows[r]))
    check("lighttable rows >=0x80 = тождество байтов",
          all(rows[r][c] == c for r in (128, 255) for c in range(256)))
    up = True
    for c in range(256):
        prev = rows[1][c]
        for r in (4, 32, 63):
            cur = rows[r][c]
            if any(cur[k] < prev[k] for k in range(3)):
                up = False
            prev = cur
    check("строки тянут цвет к белому (монотонно по row)", up)

    # 6. fog_apply boundaries + determinism
    check("fog_apply(level=0) = исходный цвет",
          all(fog_apply(c, 0) == c for c in
              ((0, 0, 0), (1, 2, 3), (32, 32, 32), (63, 63, 63))))
    check("fog_apply(level=0x3f) = белый (для чёрного)",
          fog_apply((0, 0, 0), 0x3F) == (0x3F, 0x3F, 0x3F))
    check("fog_apply(level=0x40) = белый из ветки >=0x40",
          fog_apply((5, 5, 5), 0x40) == (0x3F, 0x3F, 0x3F))
    check("fog_apply(level>=0x80) = тождество (материальные строки)",
          fog_apply((7, 9, 11), 0x80) == (7, 9, 11))
    a = build_lighttable(pal6, rows=(3, 40))
    b = build_lighttable(pal6, rows=(3, 40))
    check("детерминизм (два прогона совпадают)", a == b)

    # 7. Glide fog table
    t = gu_fog_generate_exp()
    check("guFogGenerateExp: 64 записи, t[0]=0, максимум 151 при 0.9",
          len(t) == FOG_TABLE_SIZE and t[0] == 0 and max(t) == 151
          and t == sorted(t), "min=%d max=%d" % (min(t), max(t)))
    lin = gu_fog_generate_linear(0.5, 1.0)
    check("guFogGenerateLinear(0.5,1): 128..255 монотонно",
          lin[0] == 127 and lin[-1] == 255 and lin == sorted(lin),
          "%d..%d" % (lin[0], lin[-1]))

    # 8. the Glide calls the report will quote
    slots = glide_import_slots()
    check("слоты grFogMode/grFogColorValue/grFogTable найдены",
          all(k in slots for k in ("grFogMode", "grFogColorValue",
                                   "grFogTable")),
          str({k: hex(v) for k, v in slots.items()
               if k.startswith(("grFog", "guFog"))}))
    sites = glide_sites(slots)
    near = {s["fn"] for s in sites if 0x440A80 <= int(s["addr"], 16) <= 0x440AC0}
    check("grFogMode/grFogColorValue grFogTable вызываются из 0x4409b8",
          {"grFogMode", "grFogColorValue", "grFogTable"} <= near, str(near))
    cfg = glide_config(slots, sites)
    check("grFogMode(2) + grFogColorValue(0xffffff) в кадровом пути",
          any(m["arg"] == 2 for m in cfg["modes"])
          and any(c["arg"] == 0xFFFFFF for c in cfg["colors"]),
          "modes=%s colors=%s"
          % ([hex(m["arg"]) for m in cfg["modes"] if m["arg"] is not None],
             [hex(c["arg"]) for c in cfg["colors"] if c["arg"] is not None]))

    # 9. sample table sanity
    s = sample([0, 118, 1000])
    check("сэмпл: level 63 -> 4 с глубиной",
          s[0]["level"] == 63 and s[2]["level"] == 4,
          str([x["level"] for x in s]))
    return ok


# --------------------------------------------------------------------------
# cli
# --------------------------------------------------------------------------

def table(game=DEFAULT_GAME, palette_path=None, a0=DEFAULT_A0, step=8,
          limit=320, gray=True):
    pal6, info = load_palette6(game, palette_path)
    if gray:
        pal6 = gray_palette()
        print("серая палитра: index i -> i*63/255, 6-bit (0 -> %s, 255 -> %s)"
              % (pal6[0], pal6[255]))
    print("a0 = %.1f (64/a0 = %.4f)" % (a0, 64.0 / a0))
    print("%6s %7s %5s  %-9s %-9s %-9s" % ("depth", "z", "level",
                                           "black", "gray32", "white"))
    d = 0
    while d <= limit:
        z = fog_z(d, a0)
        lvl = fog_level_z(z)
        cells = []
        for c in ((0, 0, 0), (32, 32, 32), (0x3F, 0x3F, 0x3F)):
            v = fog_apply(c, lvl)
            cells.append("%2d,%2d,%2d" % v)
        print("%6d %7.2f %5d  %-9s %-9s %-9s" % ((d, z, lvl) + tuple(cells)))
        d += step


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.environ.get(
        "GUNMETAL_EXE", os.path.join(DEFAULT_GAME, "gundx.exe")))
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="каталог игры (только чтение; нужен FRONTEND/GAMEPAL)")
    ap.add_argument("--palette", default=None,
                    help="BMP/raw 768/1024-байтная палитра (по умолчанию GAMEPAL)")
    ap.add_argument("--a0", type=float, default=DEFAULT_A0,
                    help="ctx+0xa0 = 0.8*W*k (256 при 320x200, k=1)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--table", action="store_true",
                    help="глубина -> z -> level -> цвет (серая палитра)")
    ap.add_argument("--step", type=int, default=8, help="шаг таблицы глубин")
    ap.add_argument("--limit", type=int, default=320,
                    help="максимальная глубина в --table")
    ap.add_argument("--glide", action="store_true", help="только Glide-фог")
    ap.add_argument("--anchors", action="store_true", help="только якоря")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.exe):
        print("нет exe: %s" % args.exe, file=sys.stderr)
        return 2
    use_exe(args.exe)

    if args.self_test:
        ok = self_test(args.game, args.palette)
        print("\nитог: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    if args.anchors:
        for a in code_anchors():
            print("%s %-38s %s %s" % ("ok  " if a["ok"] else "FAIL",
                                      a["name"], a["addr"], a["why"]))
        return 0

    if args.table:
        table(args.game, args.palette, args.a0, args.step, args.limit)
        return 0

    if args.glide:
        slots = glide_import_slots()
        sites = glide_sites(slots)
        cfg = glide_config(slots, sites)
        if args.json:
            json.dump(cfg, sys.stdout, ensure_ascii=False, indent=1)
            sys.stdout.write("\n")
            return 0
        for k, v in sorted(cfg["slots"].items()):
            if k.startswith(("grFog", "guFog")):
                print("%-22s %s" % (k, v))
        for s in cfg["sites"]:
            print("%s  %-18s %s" % (s["addr"], s["fn"], " ".join(s["before"])))
        return 0

    rep = report(args.game, args.palette, args.a0)
    if args.json:
        json.dump(rep, sys.stdout, ensure_ascii=False, indent=1,
                  sort_keys=False)
        sys.stdout.write("\n")
        return 0
    print_report(rep)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
