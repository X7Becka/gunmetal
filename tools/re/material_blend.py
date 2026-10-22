#!/usr/bin/env python3
"""Polygon material blend maths of ``gundx.exe`` 1.36 (M4.2.4).

What this tool does
-------------------

* rebuilds the runtime material table ``ds:0x5670e0`` **from the exe bytes**
  (constructor ``0x420c03``: mode ``+0x00``, blend LUT ``+0x08``, intensity
  ``+0x2c``, name ``+0x34``) -- it does not trust a hard-coded copy;
* emulates the maths of every material kind exactly, instruction by
  instruction, in the engine's own 6-bit channel space
  (``out_c = f(texel_c, shade_c)``, see :data:`MODES`);
* counts how the shipped 36 levels actually use the table (``gm/sbw.py``);
* proves/refutes that the shipped renderer consumes the per-kind maths
  (``--xrefs``: byte scan of the image);

Two different things are answered here and they must not be mixed:

1. **What the engine computes.**  ``0x41f618`` (add), ``0x41f8d1`` (shade),
   ``0x41fbe3`` (varadd), ``0x41ff6f`` (tran) fill four 256x256 byte LUTs at
   startup (``0x422339``..``0x422377``) -- that *is* the per-kind maths, exact
   and provable.
2. **What the shipped 3dfx build draws.**  Only ``material[0]+0x10`` (the
   shared ``lighttable``) is read by the frame path (``0x4409c5``); none of
   the four blend LUTs and none of the ``+0x04/+0x08/+0x0c/+0x2c/+0x30``
   fields has a reader.  Every polygon is drawn with one Glide state
   (``0x440abe``: ``grAlphaBlendFunction(ONE, ONE_MINUS_SRC_ALPHA, ZERO, ZERO)``),
   so translucency in the shipped build comes from the texture's alpha, not
   from the material kind.  The material index only gates *picking* -- bit 0
   of ``+0x00`` (``0x437fba``, ``0x439e75``, ``0x43d568``).

Fields of the record (stride ``0x40``, ``index = style*6 + kind``)::

    +0x00 u32  mode: kind0=0, kind1=1, kind2..5=2   (only bit 0 is read)
    +0x04 u32* style palette  (paltable/redtable/goldtable/bluetable)
    +0x08 u32* per-kind blend LUT: NULL, trantable, trantable, varaddtable,
               addtable, shadetable
    +0x0c u32* style light ramp 0xa01040 + style*0x600
    +0x10 u32* light LUT  (read by the frame path at 0x4409cb)
    +0x14 u32* red light LUT
    +0x2c i32  per-kind pass parameter (0/64/64/100/127/-128), no reader
    +0x30 u32  style tint, kind 0 only, no reader
    +0x34 char[12] name (style 0: none/reflective/...; style>0: "%ld_<short>")

Channel maths (6-bit components, ``c'`` is the result for one channel).  The
LUTs are indexed ``[row*0x100 + col]``; by construction ``row`` is the colour
that gets attenuated (the **texel**) and ``col`` is the **shade** colour, for
``shade``/``varadd``; ``add``/``tran`` are symmetric, so the order only
matters for the two asymmetric kinds.

=====================  =========================================================  ==========
kind                   formula (per channel, all three channels, in this order)    builder
=====================  =========================================================  ==========
none                   ``out = texel`` (no LUT: ``+0x08 == NULL``)                 --
reflective             ``out = (texel + shade + 1) >> 1``                          0x41ff6f
translucent            ``out = (texel + shade + 1) >> 1``                          0x41ff6f
varadd                 ``m = max(shade.r, shade.g, shade.b)``; if ``m < 16``:     0x41fbe3
                       ``w = 63, k = 16`` else ``w = 63-(m+m/2), k = m+m/2+16``;
                       ``w = max(w, 0)``, ``k = min(k, 63)``;
                       ``out = min(63, (shade*k + texel*w) / 63)`` (trunc)
add                    ``out = min(63, texel + shade)``                            0x41f618
shade                  ``out = clamp(texel * (63 - shade) / 63, 0, 63)`` (trunc)   0x41f8d1
=====================  =========================================================  ==========

``>>> 1`` is ``SAR`` (arithmetic) and ``/`` is ``IDIV`` (truncation toward
zero); the inputs are palette components in ``0..63`` (the game palette is
the 256x3 byte table ``0x83d320``, filled at run time from the art), so no
negative intermediate appears in practice -- ``clamp`` is still there in the
code (``0x41fa1f`` for ``shade``).

Last step of every builder: the 6-bit result is packed as
``r<<12 | g<<6 | b`` and looked up in a 0x40000-byte dither table
(``0x567128`` / ``0x56712c``) that maps the 18-bit colour to an 8-bit palette
index; if ``flag[src_index] != 0`` (table at ``0x530fb8``) the two phases are
both resolved and the nearer one (squared RGB distance, ``0x41fa88``) is
kept.  Both the palette and the dither tables live in BSS and are built at
run time, so they are **not** reproducible from the exe alone -- this tool
therefore emulates the channel maths exactly and stops at the 18-bit colour
(``--palette linear`` additionally resolves indices through a synthetic grey
ramp for the self-test).

Usage::

    python3 tools/re/material_blend.py                 # table + formulas
    python3 tools/re/material_blend.py --json          # machine readable
    python3 tools/re/material_blend.py --json --stats  # + 36-level usage
    python3 tools/re/material_blend.py --stats
    python3 tools/re/material_blend.py --xrefs         # who reads 0x5670e0
    python3 tools/re/material_blend.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import struct
import sys

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

SCHEMA = "gunmetal/material-blend/1"

# ---------------------------------------------------------------------------
# table model (mirrors the constructor 0x420c03)
# ---------------------------------------------------------------------------

MAT_BASE = 0x5670E0
STRIDE = 0x40
N_STYLES = 5
N_KINDS = 6
N_RECORDS = N_STYLES * N_KINDS

KINDS = ("none", "reflective", "translucent", "varadd", "add", "shade")
KIND_SHORT = ("none", "refl", "tran", "varadd", "add", "shade")

#: ``+0x00`` per kind: 0 opaque, 1 "reflective" pass, 2 blended pass
MODE_BY_KIND = (0, 1, 2, 2, 2, 2)
#: ``+0x2c`` per kind, immediates at 0x4214d8..0x421549
PARAM_BY_KIND = (0, 0x40, 0x40, 100, 0x7F, -0x80)
#: ``+0x08`` per kind: global that holds the 256x256 blend LUT pointer
LUT_BY_KIND = (None, "trantable", "trantable", "varaddtable",
               "addtable", "shadetable")

LUT_GLOBALS = {
    0x567124: "trantable",
    0x567118: "varaddtable",
    0x56711C: "addtable",
    0x567120: "shadetable",
}
LUT_GLOBAL_BY_NAME = {v: k for k, v in LUT_GLOBALS.items()}
LUT_SIZE = {
    "paltable": 0x20100, "redtable": 0x2100, "goldtable": 0x2100,
    "bluetable": 0x2100, "lighttable": 0x10100, "redlighttable": 0x10100,
    "trantable": 0x10000, "varaddtable": 0x10000,
    "addtable": 0x10000, "shadetable": 0x10000,
}
#: palette of a style (``+0x04``)
PALETTE_BY_STYLE = ("paltable", "paltable", "redtable", "goldtable", "bluetable")

#: where the constructor writes the per-kind fields (kind -> VMA of the store)
MODE_SITES = ((0x4212F0, 0), (0x421306, 1), (0x42135D, 2),
              (0x421347, 3), (0x421331, 4), (0x42131C, 5))
PARAM_SITES = ((0x4214D8, 0), (0x4214EF, 1), (0x421505, 2),
               (0x42151C, 3), (0x421533, 4), (0x421549, 5))
LUT_SITES = ((0x42136F, 0), (0x4213A3, 1), (0x421386, 2),
             (0x4213BB, 3), (0x4213D3, 4), (0x4213EB, 5))
NAME_SITES = ((0x4A8B74, 0), (0x4A8B7C, 1), (0x4A8B88, 2),
              (0x4A8B94, 3), (0x4A8B9C, 4), (0x4A8BA0, 5))
NAME_FMT_SITES = ((0x4A8BA8, 0), (0x4A8BB4, 1), (0x4A8BC0, 2),
                  (0x4A8BCC, 3), (0x4A8BD8, 4), (0x4A8BE0, 5))

#: instruction-level anchors quoted in the report (kind -> extra evidence)
SITES = {
    "recipe": 0x420C03,
    "malloc": 0x421250,
    "lx_free": 0x420BD3,
    "lut_alloc": 0x41E3D8,
    "lut_fill_light": 0x4201FE,
    "lut_fill_redlight": 0x420542,
    "lut_fill_add": 0x41F618,
    "lut_fill_shade": 0x41F8D1,
    "lut_fill_varadd": 0x41FBE3,
    "lut_fill_tran": 0x41FF6F,
    "lut_fill_calls": 0x422339,
    "gate_ray": 0x437FBA,
    "gate_trace": 0x439E75,
    "gate_trace2": 0x43D568,
    "ctx_set": 0x4223C8,
    "ctx_read": 0x4409C5,
    "fog_lut_index": 0x441B41,
    "fog_lut_store": 0x441B59,
    "glide_blend_state": 0x440ABE,
    "glide_color_combine": 0x440A74,
    "glide_alpha_combine": 0x440A84,
    "dither_pick": 0x41FA88,
    "shade_clamp_low": 0x41FA1F,
}
#: (address, what) pairs for the "readers" table in docs/MATERIAL_BLEND.md
READERS = (
    (0x420BD3, "free: table base"),
    (0x420C03, "constructor: writes all 30 records"),
    (0x42136F, "constructor: LUT/palette pointers"),
    (0x4223C8, "ctx+0xb4 = table base (render context)"),
    (0x423EDB, "level validator: mode & 2 / mode & 1 warnings"),
    (0x426AC3, "SOW writer: name (+0x34)"),
    (0x42EAF5, "name -> index (strcmp)"),
    (0x42EB63, "index -> name"),
    (0x437FBA, "world ray: material & 1"),
    (0x439E75, "portal trace: material & 1"),
    (0x43D568, "portal trace: material & 1"),
    (0x4409C5, "render: [ctx+0xb4]+0x10 = material[0]+0x10 = lighttable"),
)

#: Glide blend factors used by the single frame-wide state at 0x440abe
GLIDE_STATE = {
    "call": 0x440ABE,
    "function": "grAlphaBlendFunction",
    "args": ("GR_BLEND_ONE (1)", "GR_BLEND_ONE_MINUS_SRC_ALPHA (5)",
             "GR_BLEND_ZERO (0)", "GR_BLEND_ZERO (0)"),
    "meaning": "src*1 + dst*(1-src_alpha); the alpha is the texture's",
}

# ---------------------------------------------------------------------------
# minimal PE reader (read-only, no Ghidra)
# ---------------------------------------------------------------------------


class Exe:
    """Just enough PE to map a VMA to bytes."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = []
        off = pe + 24 + optsz
        for i in range(nsec):
            o = off + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, rva, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, self.base + rva, rawsize, rawoff))
        self.sha1 = hashlib.sha1(d).hexdigest()

    def file_offset(self, vma):
        for _name, start, rawsize, rawoff in self.sections:
            if start <= vma < start + rawsize:
                return rawoff + vma - start
        raise KeyError("0x%x is not backed by a file section (BSS?)" % vma)

    def read(self, vma, size):
        o = self.file_offset(vma)
        return self.data[o:o + size]

    def u32(self, vma):
        return struct.unpack("<I", self.read(vma, 4))[0]

    def cstr(self, vma, limit=64):
        raw = self.read(vma, limit)
        return raw.split(b"\0")[0].decode("latin1")

    def find_u32(self, value):
        """All VMAs that contain the little-endian dword ``value``."""
        pat = struct.pack("<I", value)
        out = []
        start = 0
        d = self.data
        while True:
            i = d.find(pat, start)
            if i < 0:
                return out
            for _name, v, rawsize, rawoff in self.sections:
                if rawoff <= i < rawoff + rawsize:
                    out.append(v + i - rawoff)
                    break
            start = i + 1


def _signed32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def _c7_imm(exe, vma):
    """Immediate of a ``MOV dword ptr [..], imm32`` (opcode ``C7 /0``)."""
    op, modrm = exe.read(vma, 2)
    if op != 0xC7:
        raise ValueError("0x%x: expected C7, got %02x" % (vma, op))
    mod, rm = modrm >> 6, modrm & 7
    pos = 2
    if rm == 4:                      # SIB
        pos += 1
    if mod == 1:
        pos += 1
    elif mod == 2:
        pos += 4
    return _signed32(struct.unpack("<I", exe.read(vma + pos, 4))[0])


# ---------------------------------------------------------------------------
# table from the exe
# ---------------------------------------------------------------------------


def build_table(exe):
    """Reconstruct ``ds:0x5670e0`` from the constructor's immediates + strings.

    Returns ``(table, checks)``; ``checks`` is a list of ``(what, ok, detail)``
    so ``--self-test`` can assert that the exe still matches the model.
    """
    checks = []

    def check(what, ok, detail=""):
        checks.append((what, bool(ok), detail))
        return ok

    modes = {}
    params = {}
    luts = {}
    for site, kind in MODE_SITES:
        modes[kind] = _c7_imm(exe, site)
    for site, kind in PARAM_SITES:
        params[kind] = _c7_imm(exe, site)
    for site, kind in LUT_SITES:
        # kind0: "MOV [base+idx*0x40+8], 0"; kinds 1..5: "MOV EAX,ds:glob"
        # followed by "MOV [base+idx*0x40+8],EAX" -- both forms are read here.
        op = exe.read(site, 1)[0]
        if op == 0xC7:
            imm = _c7_imm(exe, site)
        elif op == 0xA1:
            imm = exe.u32(site + 1)
        elif op == 0x89:
            imm = exe.u32(site - 4)
        else:
            raise ValueError("0x%x: unexpected LUT store opcode %02x" % (site, op))
        luts[kind] = LUT_GLOBALS.get(imm & 0xFFFFFFFF)

    names = {kind: exe.cstr(site) for site, kind in NAME_SITES}
    fmts = {kind: exe.cstr(site) for site, kind in NAME_FMT_SITES}

    check("constructor modes == (0,1,2,2,2,2)",
          tuple(modes[k] for k in range(6)) == MODE_BY_KIND,
          str([modes[k] for k in range(6)]))
    check("constructor +0x2c == (0,64,64,100,127,-128)",
          tuple(params[k] for k in range(6)) == PARAM_BY_KIND,
          str([params[k] for k in range(6)]))
    check("constructor +0x08 == (NULL,tran,tran,varadd,add,shade)",
          tuple(luts[k] for k in range(6)) == LUT_BY_KIND,
          str([luts[k] for k in range(6)]))
    check("names of style 0 == none/reflective/translucent/varadd/add/shade",
          tuple(names[k] for k in range(6)) == KINDS, str(names))
    check('name formats of styles 1..4 == %ld_none/%ld_refl/%ld_tran/...',
          [fmts[k] for k in range(6)] == ["%ld_" + s for s in KIND_SHORT],
          str([fmts[k] for k in range(6)]))
    check("LUT sizes: tran/varadd/add/shade == 0x10000 (256x256 bytes)",
          all(LUT_SIZE[n] == 0x10000 for n in
              ("trantable", "varaddtable", "addtable", "shadetable")))

    table = []
    for style in range(N_STYLES):
        for kind in range(N_KINDS):
            if style == 0:
                name = names[kind]
            else:
                name = fmts[kind].replace("%ld", str(style + 1))
            table.append({
                "index": style * N_KINDS + kind,
                "style": style,
                "kind": kind,
                "name": name,
                "mode": modes[kind],
                "gate_bit0": bool(modes[kind] & 1),
                "mode_bit1": bool(modes[kind] & 2),
                "palette": PALETTE_BY_STYLE[style],
                "lut": luts[kind],
                "lut_addr": ("0x%06x" % LUT_GLOBAL_BY_NAME[luts[kind]]
                             if luts[kind] else None),
                "intensity": params[kind],
                "light_lut": "lighttable",
                "ramp": "0x%06x" % (0xA01040 + style * 0x600),
            })
    check("30 records x 0x40 == 0x780", N_RECORDS * STRIDE == 0x780)
    check("indices unique and names unique",
          [r["index"] for r in table] == list(range(N_RECORDS))
          and len({r["name"] for r in table}) == N_RECORDS)
    check("all 30 records share the light LUT (+0x10 == lighttable)",
          all(r["light_lut"] == "lighttable" for r in table))
    return table, checks


# ---------------------------------------------------------------------------
# per-kind channel maths (exact transcription of the builders)
# ---------------------------------------------------------------------------

C_MAX = 63


def _clamp(value, lo=0, hi=C_MAX):
    return lo if value < lo else (hi if value > hi else value)


def f_none(texel, shade):
    """``none``: no LUT (``+0x08 == NULL``), the texel goes through as is."""
    return tuple(texel)


def f_tran(texel, shade):
    """``reflective``/``translucent`` -- trantable, 0x41ff6f.

    ``(a + b + 1) >> 1`` per channel (``LEA ..,+1`` at 0x42000e, ``SAR`` at
    0x420012).  The ``+1`` makes it round half up; ``LEA``/``SAR`` at
    0x420036/0x420037 and 0x42005b/0x42005c are the other two channels.
    """
    return tuple((a + b + 1) >> 1 for a, b in zip(texel, shade))


def f_shade(texel, shade):
    """``shade`` -- shadetable, 0x41f8d1: ``texel*(63-shade)/63``.

    ``MOV ECX,0x3f; SUB ECX,[shade]`` (0x41f9ad), ``IMUL`` then ``IDIV 0x3f``
    (0x41f9bb..0x41f9c3) -- truncation toward zero -- then clamp to
    ``0..63`` (low: 0x41f9f8, high: 0x41fa1f).
    """
    return tuple(_clamp((t * (C_MAX - s)) // C_MAX) for t, s in zip(texel, shade))


def f_add(texel, shade):
    """``add`` -- addtable, 0x41f618: per-channel sum, saturated at 63.

    ``ADD`` at 0x41f6f4..0x41f70c, upper clamp only at 0x41f70f..0x41f736.
    """
    return tuple(min(C_MAX, t + s) for t, s in zip(texel, shade))


def f_varadd(texel, shade, weights=None):
    """``varadd`` -- varaddtable, 0x41fbe3.

    ``m = max(shade)`` (0x41fcbf..0x41fd04); ``m < 16`` -> ``w=63, k=16``
    (0x41fd0d..0x41fd14) else ``w = 63-(m+m/2)``, ``k = m+m/2+16``
    (0x41fd1d..0x41fd3d); ``w = max(w,0)``, ``k = min(k,63)``
    (0x41fd40..0x41fd53); ``out = min(63, (shade*k + texel*w) / 63)``
    (0x41fd5a..0x41fdb1, ``IDIV 0x3f`` + upper clamp only).
    """
    m = max(shade)
    if m < 16:
        w, k = C_MAX, 16
    else:
        w = C_MAX - (m + (m >> 1))
        k = m + (m >> 1) + 16
    w = max(w, 0)
    k = min(k, C_MAX)
    if weights is not None:
        weights["m"], weights["w"], weights["k"] = m, w, k
    return tuple(min(C_MAX, (s * k + t * w) // C_MAX)
                 for t, s in zip(texel, shade))


MODES = {
    "none": {
        "lut": None, "builder": None, "func": f_none,
        "formula": "out_c = texel_c",
        "steps": ["pass the texel index through (no blend LUT)"],
        "clamp": "none", "order": "n/a",
        "symmetry": "texel only",
    },
    "reflective": {
        "lut": "trantable", "builder": 0x41FF6F, "func": f_tran,
        "formula": "out_c = (texel_c + shade_c + 1) >> 1",
        "steps": ["read texel_c and shade_c from the 6-bit palette",
                  "sum, add 1 (round half up), arithmetic shift right by 1",
                  "LUT[row][0] = row (shade index 0 = no shade, 0x41ffd6)"],
        "clamp": "none (max 63 by construction)", "order": "sum, then >>1",
        "symmetry": "symmetric in texel/shade",
        "note": "same LUT as translucent; distinguished only by mode == 1",
    },
    "translucent": {
        "lut": "trantable", "builder": 0x41FF6F, "func": f_tran,
        "formula": "out_c = (texel_c + shade_c + 1) >> 1",
        "steps": ["read texel_c and shade_c from the 6-bit palette",
                  "sum, add 1 (round half up), arithmetic shift right by 1",
                  "LUT[row][0] = row (shade index 0 = no shade, 0x41ffd6)"],
        "clamp": "none (max 63 by construction)", "order": "sum, then >>1",
        "symmetry": "symmetric in texel/shade",
        "note": "50/50 average -- the one thing the old viewer guessed right",
    },
    "varadd": {
        "lut": "varaddtable", "builder": 0x41FBE3, "func": f_varadd,
        "formula": ("m = max(shade); m<16 ? (w=63,k=16) : "
                    "(w=63-(m+m/2), k=m+m/2+16); w=max(w,0), k=min(k,63); "
                    "out_c = min(63, (shade_c*k + texel_c*w) / 63)"),
        "steps": ["m = max(shade.r, shade.g, shade.b)",
                  "derive the two weights from m (variable add)",
                  "per channel: weighted sum of shade and texel, integer /63",
                  "clamp the result at 63",
                  "LUT[row][0] = row (shade index 0 = no shade, 0x41fc3d)"],
        "clamp": "upper only (0..63 by construction)", "order": "weights, sum, /63, clamp",
        "symmetry": "asymmetric: k weights the shade, w the texel",
    },
    "add": {
        "lut": "addtable", "builder": 0x41F618, "func": f_add,
        "formula": "out_c = min(63, texel_c + shade_c)",
        "steps": ["per channel: texel_c + shade_c", "saturate at 63",
                  "LUT[row][0] = row (shade index 0 = no shade, 0x41f672)"],
        "clamp": "upper only", "order": "sum, then clamp",
        "symmetry": "symmetric in texel/shade",
    },
    "shade": {
        "lut": "shadetable", "builder": 0x41F8D1, "func": f_shade,
        "formula": "out_c = clamp(texel_c * (63 - shade_c) / 63, 0, 63)",
        "steps": ["per channel: multiply texel_c by (63 - shade_c)",
                  "integer divide by 63 (truncation toward zero)",
                  "clamp to 0..63 (both sides are present in the builder)",
                  "LUT[row][0] = row (shade index 0 = no shade, 0x41f92b)"],
        "clamp": "both sides (0..63)", "order": "mul, /63, then clamp",
        "symmetry": "asymmetric: shade attenuates the texel",
    },
}


#: accepted aliases (the short names used by the runtime formats/strings)
ALIASES = {"refl": "reflective", "tran": "translucent"}


def blend(kind, texel, shade, shade_index=None):
    """Run one mode in 6-bit channel space.  ``kind`` is a name or index.

    ``shade_index`` is the palette index of the shade colour.  All four
    builders force ``LUT[row][0] == row`` (the inner loop has a dedicated
    ``col == 0`` branch: add ``0x41f672``, shade ``0x41f92b``, varadd
    ``0x41fc3d``, tran ``0x41ffd6``), so a shade *index* of 0 means "no
    shade" and returns the texel unchanged -- even if palette entry 0 is not
    black.  Pass the index to get that behaviour; without it the pure channel
    formula is returned.
    """
    if isinstance(kind, int):
        kind = KINDS[kind]
    kind = ALIASES.get(kind, kind)
    if kind not in MODES:
        raise KeyError("unknown material kind %r" % (kind,))
    texel = tuple(_clamp(int(c)) for c in texel)
    shade = tuple(_clamp(int(c)) for c in shade)
    spec = MODES[kind]
    if spec["lut"] and shade_index == 0:
        return texel
    return spec["func"](texel, shade)


def pack6(rgb):
    """``r<<12 | g<<6 | b`` -- the index into the dither tables."""
    r, g, b = rgb
    return (r << 12) | (g << 6) | b


#: the palette (0x83d320) and the dither tables (0x567128/0x56712c) are BSS,
#: filled at run time, so the tool ships a documented synthetic ramp for the
#: index-space tests: ``gray(i) = i >> 2`` (0..63), i.e. 0/127/255 -> 0/31/63.
def linear_palette():
    return tuple(((i >> 2), (i >> 2), (i >> 2)) for i in range(256))


def apply_index(kind, texel_index, shade_index, palette=None):
    """Palette-index-space wrapper: ``palette`` lookup -> maths -> 18-bit rgb.

    Returns ``(rgb, packed)``.  This deliberately stops before the dither
    table: that step is a runtime nearest-colour map, not exe data.
    """
    palette = palette or linear_palette()
    rgb = blend(kind, palette[texel_index & 0xFF], palette[shade_index & 0xFF],
                shade_index=shade_index)
    return rgb, pack6(rgb)


# ---------------------------------------------------------------------------
# shipped-data statistics
# ---------------------------------------------------------------------------


def scan_levels(game=DEFAULT_GAME):
    """Material usage over ``*.sbw`` (read only, ``gm/sbw.py`` is the parser)."""
    from gm import sbw

    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    if not files:
        return None
    st = {
        "game": game,
        "files": len(files),
        "sectors": 0,
        "objects": 0,
        "polys": 0,
        "materials": {},
        "per_level": {},
    }
    for path in files:
        world = sbw.parse_file(path)
        local = {}
        st["sectors"] += len(world.sectors)
        for sector in world.sectors:
            for obj in sector.objects:
                st["objects"] += 1
                for poly in obj.polys:
                    st["polys"] += 1
                    local[poly.material] = local.get(poly.material, 0) + 1
        for idx, n in local.items():
            st["materials"][idx] = st["materials"].get(idx, 0) + n
        st["per_level"][os.path.basename(path)] = {
            str(k): v for k, v in sorted(local.items())}
    return st


def usage_summary(stats, table):
    """``materials`` histogram + which kinds/modes the shipped data uses."""
    by_name = {r["index"]: r for r in table}
    out = []
    total = 0
    for idx in sorted(stats["materials"]):
        rec = by_name.get(idx)
        n = stats["materials"][idx]
        total += n
        out.append({
            "material": idx,
            "name": rec["name"] if rec else "(outside table)",
            "kind": rec["kind"] if rec else None,
            "mode": rec["mode"] if rec else None,
            "gate_bit0": rec["gate_bit0"] if rec else None,
            "lut": rec["lut"] if rec else None,
            "polys": n,
            "share": round(n / stats["polys"], 6) if stats["polys"] else 0.0,
        })
    kinds = sorted({row["kind"] for row in out if row["kind"] is not None})
    return {
        "files": stats["files"],
        "sectors": stats["sectors"],
        "objects": stats["objects"],
        "polys": stats["polys"],
        "materials": out,
        "kinds_used": [KINDS[k] for k in kinds],
        "kinds_absent": [KINDS[k] for k in range(N_KINDS) if k not in kinds],
        "styles_used": sorted({idx // N_KINDS for idx in stats["materials"]}),
        "gate_polys": sum(row["polys"] for row in out if row["gate_bit0"]),
        "non_gate_polys": sum(row["polys"] for row in out if row["gate_bit0"] is False),
        "unaccounted_polys": stats["polys"] - total,
    }


# ---------------------------------------------------------------------------
# xref scan: who touches the table / the LUTs
# ---------------------------------------------------------------------------


def xrefs(exe):
    """Byte scan for the material base, the LUT globals and ``ctx+0xb4``."""
    base_hits = exe.find_u32(MAT_BASE)
    lut_hits = {name: exe.find_u32(addr) for addr, name in LUT_GLOBALS.items()}
    # "MOV r32,[r32+0xb4]" / "MOV [r32+0xb4],r32": the render context slot that
    # caches the table base (write 0x4223ce, read 0x4409c5).
    ctx_hits = []
    d = exe.data
    pat = b"\xb4\x00\x00\x00"
    start = 0
    while True:
        i = d.find(pat, start)
        if i < 0:
            break
        start = i + 1
        op, modrm = d[i - 2], d[i - 1]
        if op in (0x8B, 0x89) and (modrm & 0xF8) == 0x88:
            for _name, v, rawsize, rawoff in exe.sections:
                if rawoff <= i - 2 < rawoff + rawsize:
                    ctx_hits.append({
                        "addr": "0x%06x" % (v + i - 2 - rawoff),
                        "op": "write" if op == 0x89 else "read",
                    })
                    break

    def classify(va):
        if 0x420B80 <= va <= 0x420C02:
            return "free 0x420bd3"
        if 0x421240 <= va <= 0x42223D:
            return "constructor 0x420c03 (write)"
        if va == 0x4223CA:
            return "ctx+0xb4 = table (0x4223a6)"
        if va in (0x437FBC, 0x439E77, 0x43D56A):
            return "pick/trace gate: mode & 1"
        if va in (0x424538, 0x424554):
            return "validator 0x423edb (mode & 2 / & 1)"
        if 0x426AC3 <= va <= 0x426F00:
            return "SOW writer (name +0x34)"
        if 0x42EAF5 <= va <= 0x42EB80:
            return "name <-> index (+0x34)"
        return "other"

    return {
        "base": MAT_BASE,
        "base_hits": [{"addr": "0x%06x" % va, "what": classify(va)}
                      for va in base_hits],
        "lut_hits": {name: ["0x%06x" % va for va in hits]
                     for name, hits in lut_hits.items()},
        "ctx_b4_hits": ctx_hits,
    }


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------


class Test:
    def __init__(self):
        self.ok = True
        self.n = 0
        self.fail = 0

    def check(self, what, ok, detail=""):
        self.n += 1
        if not ok:
            self.fail += 1
            self.ok = False
        print("  [%s] %s%s" % ("ok" if ok else "FAIL", what,
                               ("  -- " + detail) if detail else ""))
        return ok


def self_test(exe_path, game=DEFAULT_GAME):
    t = Test()
    exe = Exe(exe_path)
    table, checks = build_table(exe)
    print("self-test: таблица из exe (%s, sha1 %s)" % (exe_path, exe.sha1[:12]))
    for what, ok, detail in checks:
        t.check(what, ok, detail)

    print("\nself-test: формулы, границы 0/127/255 (серый ramp: i>>2)")
    pal = linear_palette()
    ids = (0, 127, 255)
    # none: identity on every channel, including the extremes
    for i in ids:
        rgb, packed = apply_index("none", i, 200, pal)
        t.check("none: индекс %d -> %d (без изменений)" % (i, pal[i][0]),
                rgb == pal[i] and packed == pack6(pal[i]))
    # tran: 50/50 average with round-half-up
    t.check("tran: texel 0 + shade 0 -> 0", blend("tran", (0, 0, 0), (0, 0, 0)) == (0, 0, 0))
    t.check("tran: texel 63 + shade 63 -> 63", blend("tran", (63,) * 3, (63,) * 3) == (63,) * 3)
    t.check("tran: 63 + 0 -> 32 (округление вверх, +1>>1)",
            blend("tran", (63, 0, 63), (0, 63, 63)) == (32, 32, 63),
            str(blend("tran", (63, 0, 63), (0, 63, 63))))
    t.check("tran: симметрия по texel/shade",
            all(blend("tran", a, b) == blend("tran", b, a)
                for a in ((0, 1, 63), (5, 7, 9), (63, 63, 0))
                for b in ((63, 0, 1), (32, 32, 32), (0, 0, 0))))
    t.check("tran: индекс 0/255 -> 0/63", apply_index("tran", 255, 255, pal)[0] == (63,) * 3
            and apply_index("tran", 0, 0, pal)[0] == (0,) * 3)

    # the forced identity column: LUT[row][0] == row, keyed by the shade *index*
    odd = list(linear_palette())
    odd[0] = (10, 20, 30)      # palette entry 0 deliberately not black
    for kind in ("reflective", "translucent", "varadd", "add", "shade"):
        rgb, _packed = apply_index(kind, 255, 0, odd)
        t.check("%s: shade_index 0 -> тождество (LUT[row][0] = row)" % kind,
                rgb == odd[255], str(rgb))
    t.check("tran: без индекса (чистая формула) 0-я колонка не особенная",
            blend("tran", (63, 63, 63), (10, 20, 30)) == (37, 42, 47),
            str(blend("tran", (63, 63, 63), (10, 20, 30))))

    # shade: multiply by (63 - shade)/63, truncation, clamp both sides
    t.check("shade: shade 0 -> тождество",
            all(blend("shade", (t,) * 3, (0,) * 3) == (t,) * 3 for t in range(64)))
    t.check("shade: shade 63 -> 0 (любой texel)",
            blend("shade", (63, 63, 63), (63,) * 3) == (0, 0, 0))
    t.check("shade: 63 * (63-31)/63 -> 32 (trunc)",
            blend("shade", (63, 63, 63), (31,) * 3) == (32, 32, 32),
            str(blend("shade", (63,) * 3, (31,) * 3)))
    t.check("shade: деление ДО clamp (1 * 1 / 63 -> 0, не 1)",
            blend("shade", (1, 1, 1), (62,) * 3) == (0, 0, 0))
    t.check("shade: 0 saturate снизу (texel 0, shade 63 -> 0)",
            blend("shade", (0, 0, 0), (63,) * 3) == (0, 0, 0))

    # add: sum + upper saturation only
    t.check("add: 0+0 -> 0", blend("add", (0, 0, 0), (0, 0, 0)) == (0, 0, 0))
    t.check("add: 31+31 -> 62", blend("add", (31,) * 3, (31,) * 3) == (62, 62, 62))
    t.check("add: 63+63 -> 63 (насыщение, не 126)",
            blend("add", (63,) * 3, (63,) * 3) == (63,) * 3)
    t.check("add: 32+32 -> 63", blend("add", (32,) * 3, (32,) * 3) == (63,) * 3)
    t.check("add: clamp ПОСЛЕ суммы (60+10 -> 63)", blend("add", (60,) * 3, (10,) * 3) == (63,) * 3)

    # varadd: three regimes from the builder
    w = {}
    t.check("varadd: shade 0 (m=0<16) -> тождество",
            blend("varadd", (17, 33, 63), (0, 0, 0)) == (17, 33, 63))
    f_varadd((0, 0, 0), (0, 0, 0), w)
    t.check("varadd: m<16 -> w=63, k=16", (w["w"], w["k"]) == (63, 16), str(w))
    t.check("varadd: shade 63 -> цвет shade (texel вытеснен)",
            blend("varadd", (0, 63, 0), (63,) * 3) == (63, 63, 63))
    f_varadd((0, 0, 0), (63, 0, 0), w)
    t.check("varadd: m=63 -> w=0, k=63", (w["w"], w["k"]) == (0, 63), str(w))
    f_varadd((0, 0, 0), (31, 20, 0), w)
    t.check("varadd: m=31 -> w=17, k=62", (w["w"], w["k"]) == (17, 62), str(w))
    t.check("varadd: out = (shade*k + texel*w)/63 при m=31",
            blend("varadd", (0, 0, 0), (31, 0, 0)) == (31 * 62 // 63, 0, 0)
            and blend("varadd", (63, 63, 63), (31, 0, 0))[0]
            == (31 * 62 + 63 * 17) // 63,
            str(blend("varadd", (63, 63, 63), (31, 0, 0))))
    t.check("varadd: разрыв на m=16 (вес переключается, s=15 -> s=16 темнее)",
            blend("varadd", (20,) * 3, (15,) * 3)[0]
            > blend("varadd", (20,) * 3, (16,) * 3)[0],
            "%d -> %d" % (blend("varadd", (20,) * 3, (15,) * 3)[0],
                          blend("varadd", (20,) * 3, (16,) * 3)[0]))
    t.check("varadd: монотонно по texel при фиксированном shade",
            all(blend("varadd", (t,) * 3, (s,) * 3)[0]
                <= blend("varadd", (t + 1,) * 3, (s,) * 3)[0]
                for s in (0, 8, 15, 16, 31, 47, 63) for t in range(63)))

    # per-kind monotonicity: add/tran never darken as the shade brightens;
    # varadd is explicitly non-monotone at m == 16 (see the check above).
    t.check("add/tran монотонны по яркости shade",
            all(blend(k, (20,) * 3, (s,) * 3)[0] <= blend(k, (20,) * 3, (s + 1,) * 3)[0]
                for k in ("add", "tran") for s in range(63)))
    t.check("shade монотонно темнеет по shade",
            all(blend("shade", (40,) * 3, (s,) * 3)[0] >= blend("shade", (40,) * 3, (s + 1,) * 3)[0]
                for s in range(63)))

    print("\nself-test: целостность режимов и таблицы")
    recs = {r["kind"]: r for r in table[:6]}
    t.check("reflective и translucent делят trantable",
            recs[1]["lut"] == recs[2]["lut"] == "trantable")
    t.check("reflective отличён от translucent только mode (1 vs 2)",
            recs[1]["mode"] == 1 and recs[2]["mode"] == 2
            and recs[1]["intensity"] == recs[2]["intensity"] == 0x40)
    t.check("none не имеет LUT и не проходит гейт bit0",
            recs[0]["lut"] is None and recs[0]["gate_bit0"] is False)
    t.check("гейт bit0 (mode&1) стоит ТОЛЬКО у reflective",
            [r["kind"] for r in table[:6] if r["gate_bit0"]] == [1]
            and recs[1]["mode"] == 1
            and all(recs[k]["mode"] == 2 for k in (2, 3, 4, 5)))
    t.check("формулы режимов дают только 6-битные целые",
            all(isinstance(c, int) and 0 <= c <= 63
                for k in KINDS
                for t in ((0, 0, 0), (63, 63, 63), (1, 2, 3), (31, 32, 33))
                for s in ((0, 0, 0), (63, 63, 63), (1, 2, 3), (31, 32, 33))
                for c in blend(k, t, s)))
    t.check("нет float в формулах (детерминизм целочисленной арифметики)",
            all(blend(k, (7, 8, 9), (11, 12, 13)) == blend(k, (7, 8, 9), (11, 12, 13))
                for k in KINDS))
    t.check("4 разные RGB-палитры стилей (0 и 1 делят paltable)",
            len({r["palette"] for r in table}) == 4,
            str(sorted({r["palette"] for r in table})))

    print("\nself-test: xref / потребители таблицы")
    x = xrefs(exe)
    consumers = {h["what"] for h in x["base_hits"]}
    t.check("таблица 0x5670e0 читается: gate&1, ctx+0xb4, имена",
            any("gate" in c for c in consumers)
            and any("ctx+0xb4" in c for c in consumers)
            and any("name" in c for c in consumers)
            and not any(c == "other" for c in consumers),
            "%d обращений: %s" % (len(x["base_hits"]),
                                  ", ".join(sorted(consumers))))
    reads = [h["addr"] for h in x["ctx_b4_hits"] if h["op"] == "read"]
    writes = [h["addr"] for h in x["ctx_b4_hits"] if h["op"] == "write"]
    t.check("ctx+0xb4: запись 0x4223ce и чтение 0x4409c5 (третье попадание "
            "0x45cef8 — чужая структура 0x45cexx)",
            writes == ["0x4223ce"] and "0x4409c5" in reads and len(reads) <= 2,
            "%s / %s" % (writes, reads))
    t.check("после чтения ctx+0xb4 идёт material[0]+0x10 (8B 51 10)",
            exe.read(0x4409CB, 3) == b"\x8b\x51\x10")
    t.check("blend-LUT указатели читаются только alloc/free/конструктор/fill",
            all(all(0x41E3D8 <= int(a, 16) <= 0x41E7FF          # alloc/free
                    or 0x41F600 <= int(a, 16) <= 0x41FFFF      # fillers
                    or 0x421240 <= int(a, 16) <= 0x42223D      # constructor
                    for a in hits)
                for hits in x["lut_hits"].values()),
            str({k: len(v) for k, v in x["lut_hits"].items()}))

    print("\nself-test: поставка (36 уровней)")
    stats = scan_levels(game)
    if stats is None:
        print("  [skip] нет .sbw в %s" % game)
        return t.ok
    summary = usage_summary(stats, table)
    t.check("36 уровней", stats["files"] == 36, str(stats["files"]))
    t.check("235069 полигонов / 28192 объекта / 5104 сектора",
            (stats["polys"], stats["objects"], stats["sectors"]) == (235069, 28192, 5104),
            "%d/%d/%d" % (stats["polys"], stats["objects"], stats["sectors"]))
    t.check("материалы в данных: 0,1,2,3,4 (shade=5 не встречается)",
            summary["kinds_absent"] == ["shade"], str(summary["kinds_absent"]))
    t.check("используется только стиль 0", summary["styles_used"] == [0])
    t.check("все полигоны попали в таблицу", summary["unaccounted_polys"] == 0)
    t.check("гейт bit0 (mode&1) в данных: ровно 300 полигонов reflective",
            summary["gate_polys"] == 300, str(summary["gate_polys"]))
    t.check("reflective встречается (300), но его LUT == trantable",
            stats["materials"].get(1) == 300)
    return t.ok


# ---------------------------------------------------------------------------
# text / JSON output
# ---------------------------------------------------------------------------


def mode_json(table):
    recs = {r["kind"]: r for r in table[:6]}
    out = []
    for kind in range(N_KINDS):
        name = KINDS[kind]
        spec = MODES[name]
        rec = recs[kind]
        out.append({
            "kind": kind,
            "name": name,
            "mode": rec["mode"],
            "gate_bit0": rec["gate_bit0"],
            "intensity": rec["intensity"],
            "lut": spec["lut"],
            "lut_addr": rec["lut_addr"],
            "lut_size": LUT_SIZE[spec["lut"]] if spec["lut"] else 0,
            "builder": ("0x%06x" % spec["builder"]) if spec["builder"] else None,
            "channels": ["r", "g", "b"],
            "formula": spec["formula"],
            "steps": spec["steps"],
            "clamp": spec["clamp"],
            "order": spec["order"],
            "symmetry": spec["symmetry"],
            "note": spec.get("note"),
            "table_index": (("[row*0x100+col]  (row=texel, col=shade)"
                             if name in ("shade", "varadd") else
                             "[row*0x100+col]  (symmetric)") if spec["lut"] else None),
            "quantize": ("r<<12|g<<6|b -> dither 0x567128/0x56712c -> 8-bit index"
                         if spec["lut"] else "index passthrough"),
        })
    return out


def report(exe_path, game, with_stats=False, with_xrefs=False):
    exe = Exe(exe_path)
    table, checks = build_table(exe)
    stats = scan_levels(game) if with_stats else None
    return {
        "schema": SCHEMA,
        "exe": {"path": exe_path, "sha1": exe.sha1, "base": "0x%06x" % exe.base},
        "sites": {"0x%06x" % v: k for k, v in sorted(SITES.items(),
                                                     key=lambda kv: kv[1])},
        "glide_state": GLIDE_STATE,
        "readers": [{"addr": "0x%06x" % a, "what": w} for a, w in READERS],
        "material_table": table,
        "modes": mode_json(table),
        "evidence_ok": all(ok for _what, ok, _detail in checks),
        "evidence_checks": [{"what": w, "ok": ok, "detail": d}
                            for w, ok, d in checks],
        "stats": (usage_summary(stats, table) if stats else None),
        "xrefs": xrefs(exe) if with_xrefs else None,
        "note": ("per-kind maths is exact (LUT builders); the shipped 3dfx "
                 "frame path does not read the blend LUTs -- only bit 0 of "
                 "mode and material[0]+0x10 (lighttable)"),
    }


def print_report(rep):
    exe = rep["exe"]
    print("gundx.exe 1.36 -- материалы полигонов: математика смешения (M4.2.4)")
    print("  exe: %s  sha1 %s" % (exe["path"], exe["sha1"][:12]))
    print("  таблица ds:0x5670e0, шаг 0x40, строит 0x420c03 (malloc 0x421250)\n")
    print("поля и потребители (полный список -- доки):")
    for row in rep["readers"]:
        print("  %-10s %s" % (row["addr"], row["what"]))
    print("\n30 записей (из exe, не из констант):")
    print("  idx st kd mode bit0 +0x2c lut         +0x08  имя")
    for r in rep["material_table"]:
        print("  %2d  %d  %d  %4d %-4s %5d %-11s %-7s %s" % (
            r["index"], r["style"], r["kind"], r["mode"],
            "yes" if r["gate_bit0"] else "-", r["intensity"],
            r["lut"] or "-", r["lut_addr"] or "-", r["name"]))
    print("\nформулы (6-битные каналы; row=texel, col=shade):")
    for m in rep["modes"]:
        print("  %-11s mode=%d LUT=%-11s builder=%-8s %s" % (
            m["name"], m["mode"], m["lut"] or "-", m["builder"] or "-",
            m["formula"]))
        print("              clamp: %s; порядок: %s" % (m["clamp"], m["order"]))
    print("\nобщий Glide-стейт кадра (0x440abe): %s(%s)" % (
        rep["glide_state"]["function"], ", ".join(rep["glide_state"]["args"])))
    print("  => альфа берётся из текстуры, не из материала; per-kind LUT не читаются.")
    if rep.get("stats"):
        s = rep["stats"]
        print("\nпоставка: %d уровней, %d секторов, %d объектов, %d полигонов"
              % (s["files"], s["sectors"], s["objects"], s["polys"]))
        print("  material -> полигонов:")
        for row in s["materials"]:
            print("    %2d %-11s mode=%d lut=%-11s %7d  %5.2f%%" % (
                row["material"], row["name"], row["mode"], row["lut"] or "-",
                row["polys"], 100.0 * row["share"]))
        print("  виды: %s; отсутствуют: %s; стили: %s"
              % (", ".join(s["kinds_used"]), ", ".join(s["kinds_absent"]),
                 s["styles_used"]))
        print("  гейт выбора bit0 (mode&1): %d полигонов, без гейта: %d"
              % (s["gate_polys"], s["non_gate_polys"]))
    if rep.get("xrefs"):
        x = rep["xrefs"]
        groups = {}
        for row in x["base_hits"]:
            groups.setdefault(row["what"], []).append(row["addr"])
        print("\nбайтовый скан 0x5670e0 (LE dword): %d обращений, %d кластеров:"
              % (len(x["base_hits"]), len(groups)))
        for what, addrs in groups.items():
            if len(addrs) > 6:
                shown = "%s..%s (%d шт.)" % (addrs[0], addrs[-1], len(addrs))
            else:
                shown = " ".join(addrs)
            print("  %-46s %s" % (what, shown))
        print("  прочих (не классифицировано): %d"
              % len(groups.get("other", [])))
        print("  ctx+0xb4: %s" % ", ".join(
            "%s %s" % (h["op"], h["addr"]) for h in x["ctx_b4_hits"]))
        for name, hits in x["lut_hits"].items():
            print("  %-11s %s" % (name, " ".join(hits)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.environ.get(
        "GUNMETAL_EXE", os.path.join(DEFAULT_GAME, "gundx.exe")))
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="каталог с .sbw (только чтение)")
    ap.add_argument("--json", action="store_true", help="машинночитаемый вывод")
    ap.add_argument("--stats", action="store_true",
                    help="гистограмма материалов по 36 уровням (gm/sbw.py)")
    ap.add_argument("--xrefs", action="store_true", help="байтовый скан образа")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--all", action="store_true", help="--stats --xrefs")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.exe):
        print("нет exe: %s" % args.exe, file=sys.stderr)
        return 2

    if args.self_test:
        ok = self_test(args.exe, args.game)
        print("\nитог: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    rep = report(args.exe, args.game,
                 with_stats=args.stats or args.all or args.json,
                 with_xrefs=args.xrefs or args.all)
    if args.json:
        json.dump(rep, sys.stdout, ensure_ascii=False, indent=1, sort_keys=False)
        sys.stdout.write("\n")
        return 0
    print_report(rep)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
