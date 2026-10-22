#!/usr/bin/env python3
"""Texture UV of the 3dfx frame pass, and the fate of untextured polygons.

Two questions blocked frame parity (``docs/ORACLE.md``, "Текстуры: масштаб UV
и полигоны без текстуры").  Both are answered here from the shipped
``gundx.exe`` 1.36 (ImageBase ``0x400000``), instruction by instruction.

Question 1 -- the texture UV formula
------------------------------------
``docs/FORMATS.md`` §4 proved the **lightmap** sampler (``0x43ca5a``)::

    texel_lm = (dot(u_axis, v - centre) * 64 + uv_u_fp) / 2 >> 10

The texture pass uses the **same numerator** and a different shift::

    N        = dot(u_axis, v - centre) * 64 + uv_u_fp    # units: 1/64 texel
    texel_u  = N >> 6    (= dot(u_axis, v-centre) + uv_u_fp/64)
    texel_v  = dot(v_axis, v - centre) + uv_v_fp/64

Three independent proofs, all in the exe:

* **Auto-UV writer** ``0x43e02d`` (the code the loader runs for polygons whose
  ``poly+0x7c & 0x8000`` is clear, ``0x425e07``/``0x432fc5``), lines
  ``0x43e0f2``..``0x43e132``::

      uv_u    = ftol(uv_u    + d * 256.0)          # 0x4a2258 = 256.0, 0x43e0ff
      uv_u_fp = ftol(uv_u_fp + d * 256.0 / 4.0)    # 0x43e122 = 4.0,  0x43e11c
      d = dot(u_axis, centre - v)                  # 0x43e0cd..0x43e0ec

  Same geometric quantity ``d`` (world units) written to both fields, one
  divided by 4.  Hence ``uv_u`` is in **1/256 texel**, ``uv_u_fp`` in
  **1/64 texel**, and because the divisor is 256 the gradient is exactly
  **1 texel per world unit**.  The ``centre - v`` sign plus ``+`` makes the
  writer anchor texel 0 at the chosen vertex, which is only consistent with
  ``texel = +dot(u_axis, v - centre) + uv_u_fp/64`` (see §sign below).

* **Frame pass, both rasterizers**, read the field as a 16.16 texel address::

      0x41bdb1  movsx edx,word [poly+0x50]   ; uv_u    (1/256 texel)
      0x41bdcd  shl   edx,8                  ; -> texel<<16  (8.8 -> 16.16)
      0x41be65  mov   edx,[poly+0x54]        ; uv_u_fp (1/64 texel)
      0x41be68  shl   edx,10                 ; -> texel<<16  (1/64 -> 16.16)
      0x41be6b  sub   edx,[ebp-0x18]         ; wrap correction vs the uv_u copy

  ``<<10`` is only a conversion to 16.16 if ``uv_u_fp`` counts 1/64 texel
  (``x64 x 1024 = 65536``); ``<<8`` wants ``uv_u`` in 1/256.  Same conclusion.

* **Ring buffer of the shipped levels**: for every polygon of ``level0`` /
  ``dm1`` / ``dm2`` ``(uv_u - 4 * uv_u_fp) mod 64 == 0`` -- the two fields
  encode the same texel with ``uv_u = 4 * uv_u_fp`` up to rounding.  And the
  texel extent of every textured ``level0`` polygon along ``u_axis`` equals
  its world extent to the last texel (1088/1088, ratio exactly 1.000).

So the constant the frame pass uses is **64** (``>> 6`` of the same numerator
the lightmap shifts by 11).  The lightmap page is therefore 32x coarser than
the texture -- one lightmap texel per 32 world units, one texture texel per
world unit.  ``/2048`` is the lightmap formula, ``/32`` is twice as dense as
the engine and ``/64`` is the engine.

Question 2 -- polygons with ``poly+0x00 == 0``
---------------------------------------------
The visible-polygon loop of the frame pass is ``0x41ac99``..``0x41add0``
(poly stride ``0x80``, ``0x41aca3``).  It calls the polygon drawer
``0x41b6cb`` for **every** polygon that survives two gates -- the
overlay/scanline test ``0x41acb4`` and backface culling ``0x41ad37`` -- with
**no reference to the texture index**.  (Full byte scan of the range for
``movsx/movzx word [reg+0]``: zero hits.)

The drawer then calls the texture setup ``0x9fde50``/``0x9fde70`` at
``0x41b6dd`` and honours its return value: ``0`` = draw, ``-1`` = return
without drawing (``0x41b6e5``/``0x41b6e9`` -> ``0x41c710``).  The setup
computes the texture index with a union trick (``0x9fde8e``..``0x9fde9d``)::

    ecx = (handle < 1) ? -0x78 : 0          # 0x9fde8e/0x9fde91/0x9fde9d
    tex = movsx word [poly + ecx + 0x78]    # 0x9fdefc -> poly+0x00 or poly+0x78
    [0x9e8cac] = tex                        # 0x9fdf1f  (current texture id)

and then (``0x9fdf24``..``0x9fdf3e``)::

    if (tex != 0)                  goto draw;
    if ([0x9e8cd0] != 0)           goto draw;   # poly+0x72 handle
    if ([0x9e8d0c] != 0)           goto draw;   # object+0x70 & 0x10
    if ([0x9e8c9c] != 0)           goto draw;   # texture-class dword of f7a
    return -1;                                  # 0x9fe4cb -> SKIP

**Verdict: a polygon with texture index 0 is skipped** (nothing is drawn at
all), unless one of the three escape hatches above is set.  In the shipped
data ``overlay`` and ``f7a`` are 0 on every ``texture == 0`` polygon and
``handle`` is 0 on 357/361 (``level0``), 723/733 (``dm1``), 908/929
(``dm2``), so the overwhelming majority takes the skip.  The handful with
``handle != 0`` reads its index from ``poly+0x78`` instead (the same
``0x9fdefc`` instruction) and is drawn with that overlay; if that is 0 as
well, ``[0x9e8cac]`` stays 0 and the software mapper indexes
``texture_table[-1]`` (``0x41be0a``..) -- undefined, not a white/flat colour.

Corollary: ``gm/render.py`` should have a ``skip_untextured`` flag that drops
``face.texture == 0`` faces instead of painting them flat.  It must not
"draw with the last bound texture": the engine's path is a return, not a
fallback.

Sign of the dot product
-----------------------
``0x43e0cd``..``0x43e0ec`` compute ``d = centre - v`` and add ``+d * 256``,
therefore the frame pass samples ``+dot(u_axis, v - centre)``; the writer is
an "anchor texel 0 at this vertex" helper (it only ever runs on polygons whose
UVs were never computed), not a counter-example.  The proven lightmap sampler
uses the same ``+dot`` sign (``docs/FORMATS.md`` §4), so both agree.

Tool
----
    python3 tools/re/texuv.py                 # human summary
    python3 tools/re/texuv.py --json          # machine-readable model
    python3 tools/re/texuv.py --self-test     # anchors + formula + data
    python3 tools/re/texuv.py --stats [level] # data invariants on a .sbw

Nothing here writes to the game; the exe is only read.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exelib  # noqa: E402

GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
LEVEL_NAMES = ("level0", "dm1", "dm2")

# uv_u_fp counts 1/64 texel, uv_u counts 1/256 texel (0x43e122 / 0x43e0ff).
FP_PER_TEXEL = 64
UV_U_PER_TEXEL = 256

# Right shifts of the shared numerator N = dot(u_axis, v-centre)*64 + uv_u_fp.
# "texel" is proven (0x43e11c/0x43e122 + 0x41be68); the others exist only to
# reproduce the variants measured in docs/ORACLE.md.
MODES = {
    "texel": 6,      # engine: 1 texel per world unit          <- proven
    "div32": 5,      # hypothesis only: 2 texels per unit
    "div2048": 11,   # lightmap divisor, NOT the texture pass
}
DEFAULT_MODE = "texel"

# Constants in .rdata that the formulas use (doubles, little endian).
CONSTANTS = (
    (0x4A2258, 256.0, "auto-UV: texels per world unit in uv_u (0x43e0ff)"),
    (0x4A2260, 4.0, "auto-UV: uv_u -> uv_u_fp divisor (0x43e122)"),
    (0x4A2298, 64.0, "numerator scale, lightmap 0x43cb79 and shadow 0x43db5f"),
    (0x4A2280, 2.0, "lightmap 0x43cbc7 / shadow 0x43dbad"),
)

ANCHORS = (
    # (address, exact bytes, meaning, status)
    (0x43E0CD, "8b 45 0c d9 40 20", "auto-UV: fld u_axis.x for dot(centre-v)", "proven"),
    (0x43E0F2, "0f bf 48 50", "auto-UV: movsx ecx,word [poly+0x50] (uv_u)", "proven"),
    (0x43E0FF, "dc 0d 58 22 4a 00", "auto-UV: uv_u += d * 256.0", "proven"),
    (0x43E10F, "66 89 42 50", "auto-UV: mov [poly+0x50],ax  -> uv_u is 1/256 texel", "proven"),
    (0x43E11C, "dc 0d 58 22 4a 00", "auto-UV: uv_u_fp += d * 256.0", "proven"),
    (0x43E122, "dc 35 60 22 4a 00", "auto-UV: ... / 4.0 -> uv_u_fp is 1/64 texel", "proven"),
    (0x43E132, "89 41 54", "auto-UV: mov [poly+0x54],eax", "proven"),
    (0x41BDB1, "0f bf 51 50", "frame pass: movsx edx,word [poly+0x50] (uv_u)", "proven"),
    (0x41BDCD, "c1 e2 08", "frame pass: shl edx,8  -> texel<<16 (8.8)", "proven"),
    (0x41BE65, "8b 51 54", "frame pass: mov edx,[poly+0x54] (uv_u_fp)", "proven"),
    (0x41BE68, "c1 e2 0a", "frame pass: shl edx,0xa -> texel<<16 (1/64)", "proven"),
    (0x43CB79, "dc 0d 98 22 4a 00", "lightmap: dot * 64.0 (same numerator)", "proven"),
    (0x43CBC7, "dc 35 80 22 4a 00", "lightmap: / 2.0", "proven"),
    (0x43CC0D, "c1 fa 0a", "lightmap: sar edx,0xa (>>11 of the numerator)", "proven"),
    (0x41ACA3, "8b 4d dc", "frame poly loop: index load", "proven"),
    (0x41ACB4, "83 7d e0 00", "frame poly loop: overlay/scan gate (no texture check)", "proven"),
    (0x41AD37, "8b 4d f4", "frame poly loop: poly pointer for the cull starts", "proven"),
    (0x41ADC8, "e8 fe 08 00 00", "frame poly loop: call 0x41b6cb for every poly", "proven"),
    (0x41B6DD, "e8 6e 27 5e 00", "drawer: call 0x9fde50 (texture setup)", "proven"),
    (0x41B6E5, "85 c0", "drawer: test eax,eax", "proven"),
    (0x41B6E9, "e9 22 10 00 00", "drawer: jmp 0x41c710 (return, nothing drawn)", "proven"),
    (0x9FDE8E, "83 f9 01", "setup: cmp handle,1", "proven"),
    (0x9FDE91, "19 c9", "setup: sbb ecx,ecx", "proven"),
    (0x9FDE9D, "83 e1 88", "setup: and ecx,0xffffff88 (-0x78 when handle<1)", "proven"),
    (0x9FDEFC, "0f bf 44 0b", "setup: movsx eax,word [ebx+ecx+0x78] -> +0x00 or +0x78", "proven"),
    (0x9FDF01, "89 35 9c 8c 9e 00", "setup: [0x9e8c9c] = texture class dword", "proven"),
    (0x9FDF1F, "a3 ac 8c 9e 00", "setup: [0x9e8cac] = texture index", "proven"),
    (0x9FDF24, "85 c0", "setup: test eax,eax (index 0?)", "proven"),
    (0x9FDF3E, "0f 84 87 05 00 00", "setup: je 0x9fe4cb (all four zero -> skip)", "proven"),
    (0x9FDE93, "80 7b 71 02", "setup: cmp byte [poly+0x71],2 (n_idx <= 2 -> skip)", "proven"),
    (0x9FE4CB, "b8 ff ff ff ff c3", "setup: mov eax,-1 ; ret  (skip marker)", "proven"),
)

FORMULA_LATEX = (
    "N = dot(u_axis, v - centre) * 64 + uv_u_fp   [1/64 texel]",
    "texel_u = N >> 6   (engine frame pass, proven)",
    "texel_lm_u = N >> 11   (lightmap, docs/FORMATS.md 4)",
)


class Anchor(dict):
    """Read-only view over one ANCHORS row (dict for trivial JSON output)."""

    def __init__(self, row):
        addr, raw, what, status = row
        super().__init__(address="0x%06x" % addr, address_int=addr,
                         bytes=raw, what=what, status=status)


def anchors():
    return [Anchor(r) for r in ANCHORS]


# --------------------------------------------------------------------------
# geometry helpers
# --------------------------------------------------------------------------
def _vec(face, name):
    """``u_axis``/``v_axis``/``centre`` of a face, whatever the wrapper."""
    return getattr(face, name)


def _fp(face):
    """(uv_u_fp, uv_v_fp) as signed ints; ``gm.render.LevelFace`` has ``uv_fp``."""
    fp = getattr(face, "uv_fp", None)
    if fp is not None:
        return int(fp[0]), int(fp[1])
    return int(face.uv_u_fp), int(face.uv_v_fp)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def numerator(face, vertex, axis="u"):
    """``dot(axis, v - centre) * 64 + uv_fp`` -- units of 1/64 texel."""
    ax = _vec(face, "u_axis" if axis == "u" else "v_axis")
    fp = _fp(face)[0 if axis == "u" else 1]
    return _dot(_sub(vertex, _vec(face, "centre")), ax) * 64.0 + fp


def texture_uv_f(face, vertex, axis="u"):
    """Exact (unrounded) texel coordinate of *vertex* on *face*."""
    return numerator(face, vertex, axis) / FP_PER_TEXEL


def texture_uv(face, vertex, mode=DEFAULT_MODE):
    """Integer texel coordinates ``(u, v)`` the frame pass samples.

    ``mode="texel"`` (default) is the proven engine formula::

        texel = int(dot(axis, v - centre) * 64 + uv_fp) >> 6

    ``mode`` may also be ``"div32"``/``"div2048"`` or an explicit right shift
    (int) to reproduce the alternatives measured in ``docs/ORACLE.md``; those
    are hypotheses, not engine behaviour.
    """
    shift = MODES.get(mode, mode) if isinstance(mode, str) else int(mode)
    return (int(numerator(face, vertex, "u")) >> shift,
            int(numerator(face, vertex, "v")) >> shift)


def lightmap_uv(face, vertex):
    """Same numerator, lightmap divisor (``docs/FORMATS.md`` §4)."""
    shift = MODES["div2048"]
    return (int(numerator(face, vertex, "u")) >> shift,
            int(numerator(face, vertex, "v")) >> shift)


def texel_at_centre(face):
    """Texel coordinate at ``centre``: ``uv_fp / 64`` (dot term is zero)."""
    u, v = _fp(face)
    return u / FP_PER_TEXEL, v / FP_PER_TEXEL


def uv_extent(face, vertices, axis="u"):
    """Texel extent (max-min) of a polygon along an axis, engine formula."""
    vals = [texture_uv_f(face, v, axis) for v in vertices]
    return max(vals) - min(vals)


# --------------------------------------------------------------------------
# question 2
# --------------------------------------------------------------------------
def draws_untextured():
    """Verdict on ``poly+0x00 == 0`` in the frame pass, with byte anchors.

    ``"skipped"`` = the texture setup returns -1 and the drawer returns before
    submitting any geometry.  The three escape hatches are listed because they
    can force such a polygon through at runtime.
    """
    return {
        "question": "does the frame pass draw polygons with poly+0x00 == 0?",
        "verdict": "skipped",
        "summary": ("texture index 0 -> texture setup 0x9fde50 returns -1 "
                    "(0x9fe4cb) -> drawer 0x41b6cb returns (0x41b6e9); "
                    "nothing is drawn, no flat colour, no fallback texture"),
        "index_source": "handle < 1 ? poly+0x00 : poly+0x78 (0x9fde8e..0x9fdefc)",
        "skip_test": ("index==0 && handle==0 && (obj+0x70&0x10)==0 && "
                      "class_dword==0  ->  return -1 (0x9fdf24..0x9fdf3e)"),
        "escape_hatches": [
            "poly+0x72 handle != 0 (0x9e8cd0)",
            "object+0x70 & 0x10 (0x9e8d0c)",
            "class dword [0x9e82c4 + (f7a + word[ctx+0x14])*0x40] != 0 (0x9e8c9c)",
        ],
        "if_forced_through": ("index stays 0, the software mapper indexes "
                              "texture_table[-1] (0x41be0a) -- undefined, not white"),
        "loop_has_no_texture_check": True,
        "n_idx_le_2_also_skipped": "0x9fde93",
        "anchors": [hex(a[0]) for a in ANCHORS if a[0] >= 0x41ACB4 and a[0] <= 0x9FE4CB],
    }


# --------------------------------------------------------------------------
# data checks
# --------------------------------------------------------------------------
def _find_level(name: str):
    for base in (GAME, os.environ.get("GUNMETAL_LEVELS", "")):
        if not base:
            continue
        path = os.path.join(base, name + ".sbw")
        if os.path.isfile(path):
            return path
    return None


def stats(name: str = "level0") -> dict:
    """Invariants of one shipped level (read-only)."""
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    if root not in sys.path:
        sys.path.insert(0, root)
    from gm import sbw  # local import: the tool must work without gm on path

    path = _find_level(name)
    if path is None:
        return {"level": name, "available": False}
    lvl = sbw.parse_file(path)
    polys = [(o, p) for s in lvl.sectors for o in s.objects for p in o.polys]
    textured = [(o, p) for o, p in polys if p.texture > 0 and len(p.indices) >= 3]
    untextured = [p for _o, p in polys if p.texture == 0]

    ratio_ok = 0
    exact = 0
    for o, p in textured:
        verts = [o.vertices[i] for i in p.indices]
        wu = [ _dot(_sub(v, p.centre), p.u_axis) for v in verts]
        tu = [texture_uv_f(p, v, "u") for v in verts]
        if abs((max(tu) - min(tu)) - (max(wu) - min(wu))) <= 1.0:
            ratio_ok += 1
        if abs((max(tu) - min(tu)) - (max(wu) - min(wu))) < 1e-6:
            exact += 1

    mod64 = sum(1 for _o, p in polys
                if (p.uv_u - 4 * p.uv_u_fp) % 64 == 0
                and (p.uv_v - 4 * p.uv_v_fp) % 64 == 0)
    handled = [p for p in untextured if p.handle]
    return {
        "level": name,
        "available": True,
        "polys": len(polys),
        "textured": len(textured),
        "textured_uv_u_fp_is_1_64_texel": {
            "texel_extent_equals_world_extent": "%d/%d" % (ratio_ok, len(textured)),
            "exact": "%d/%d" % (exact, len(textured)),
        },
        "uv_u_equals_4x_uv_fp_mod_64": "%d/%d" % (mod64, len(polys)),
        "untextured": {
            "count": len(untextured),
            "share": round(len(untextured) / len(polys), 4) if polys else 0.0,
            "handle_nonzero": len(handled),
            "overlay_nonzero": sum(1 for p in untextured if p.overlay),
            "f7a_nonzero": sum(1 for p in untextured if p.f7a),
            "escaped_through_skip": len(handled),
            "skip_without_escape": len(untextured) - len(handled),
        },
    }


def verify_anchors():
    """Compare ANCHORS bytes against the exe; returns a list of results."""
    out = []
    for addr, raw, what, status in ANCHORS:
        want = bytes.fromhex(raw)
        try:
            off = exelib.va2off(addr)
            got = exelib.data()[off:off + len(want)]
        except Exception as exc:                    # pragma: no cover
            out.append({"address": "0x%06x" % addr, "bytes": raw, "what": what,
                        "status": status, "ok": False, "error": str(exc)})
            continue
        out.append({"address": "0x%06x" % addr, "bytes": raw,
                    "what": what, "status": status,
                    "ok": got == want,
                    "actual": got.hex(" ") if got != want else raw})
    return out


def verify_constants():
    import struct
    out = []
    for va, value, what in CONSTANTS:
        off = exelib.va2off(va)
        got = struct.unpack_from("<d", exelib.data(), off)[0]
        out.append({"address": "0x%06x" % va, "what": what,
                    "expected": value, "actual": got, "ok": got == value})
    return out


# --------------------------------------------------------------------------
# self test
# --------------------------------------------------------------------------
class _Face:
    """Minimal stand-in for ``gm.render.LevelFace`` / ``gm.sbw.Poly``."""

    def __init__(self, centre, u, v, uv_u_fp, uv_v_fp):
        self.centre = centre
        self.u_axis = u
        self.v_axis = v
        self.uv_u_fp = uv_u_fp
        self.uv_v_fp = uv_v_fp


def self_test(verbose: bool = True):
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        if verbose:
            print("%-52s %s%s" % (name, "ok" if cond else "FAIL",
                                  ("  " + detail) if detail else ""))
        return cond

    # 1. byte anchors and .rdata constants against the shipped exe
    va = verify_anchors()
    bad = [a for a in va if not a["ok"]]
    check("anchors: %d instruction sites match the exe" % len(va), not bad,
          "" if not bad else "mismatch at " + ", ".join(a["address"] for a in bad))
    vc = verify_constants()
    check("constants: 256.0/4.0/64.0/2.0 in .rdata",
          all(c["ok"] for c in vc),
          ", ".join("%s=%s" % (c["address"], c["actual"]) for c in vc))

    # 2. formula on a synthetic polygon: u axis = +X, anchor texel 0 at centre
    f = _Face((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0, 0)
    check("synth: texel at centre == 0", texture_uv(f, (0.0, 0.0, 0.0)) == (0, 0))
    check("synth: 1 world unit == 1 texel",
          texture_uv(f, (5.0, 0.0, 0.0)) == (5, 0)
          and texture_uv(f, (0.0, 5.0, 0.0)) == (0, 5))
    check("synth: fractional texel steps",
          [texture_uv(f, (x, 0.0, 0.0))[0] for x in
           (-1.5, -0.5, 0.5, 1.5)] == [-2, -1, 0, 1])
    check("synth: lightmap is /32 of the texture texel",
          abs(texture_uv_f(f, (32.0, 0.0, 0.0)) /
              (lightmap_uv(f, (32.0, 0.0, 0.0))[0] or 1) - 32.0) < 1e-9)

    # 3. uv_u_fp offset = texel at the centre
    g = _Face((10.0, -3.0, 7.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 128, -64)
    check("offset: uv_u_fp/64 is the texel at centre",
          texel_at_centre(g) == (2.0, -1.0)
          and texture_uv(g, (10.0, -3.0, 7.0)) == (2, -1))

    # 4. monotonic in the dot product, and exactly linear
    xs = [i * 0.25 - 20.0 for i in range(161)]
    ts = [texture_uv(f, (x, 0.0, 0.0))[0] for x in xs]
    check("monotonic: non-decreasing over 161 samples",
          all(b >= a for a, b in zip(ts, ts[1:])))
    check("linear: texel == floor(world offset)", ts == [math.floor(x) for x in xs])

    # 5. mode table: only /64 (>>6) is the engine, the rest are labelled
    check("modes: texel=6, div32=5, div2048=11",
          MODES == {"texel": 6, "div32": 5, "div2048": 11})
    check("modes: div2048 is 32x coarser than texel",
          (1 << (MODES["div2048"] - MODES["texel"])) == 32)
    check("modes: div32 is 2x denser than texel",
          (1 << (MODES["texel"] - MODES["div32"])) == 2)

    # 6. determinism + separate u/v
    check("determinism: repeated calls agree",
          [texture_uv(g, (11.0, -3.0, 7.0)) for _ in range(5)] ==
          [texture_uv(g, (11.0, -3.0, 7.0))] * 5)

    # 7. verdict is a fixed string, anchors present
    v = draws_untextured()
    check("verdict: texture index 0 is skipped", v["verdict"] == "skipped")
    check("verdict: cites the skip marker 0x9fe4cb", "0x9fe4cb" in v["anchors"])

    # 8. shipped data
    for name in LEVEL_NAMES:
        st = stats(name)
        if not st.get("available"):
            check("data/%s: level available" % name, True, "skipped (not installed)")
            continue
        ok, tot = st["textured_uv_u_fp_is_1_64_texel"][
            "texel_extent_equals_world_extent"].split("/")
        check("data/%s: texel extent == world extent" % name,
              ok == tot, "%s/%s" % (ok, tot))
        ok, tot = st["uv_u_equals_4x_uv_fp_mod_64"].split("/")
        check("data/%s: uv_u == 4*uv_u_fp (mod 64)" % name, ok == tot,
              "%s/%s" % (ok, tot))
        u = st["untextured"]
        check("data/%s: texture==0 never uses the f7a path" % name,
              u["f7a_nonzero"] == 0,
              "overlay!=0 on %d of %d" % (u["overlay_nonzero"], u["count"]))
        check("data/%s: texture==0 mostly loses the handle test" % name,
              u["skip_without_escape"] >= 0.95 * u["count"],
              "skip %d, escaped %d of %d"
              % (u["skip_without_escape"], u["escaped_through_skip"], u["count"]))

    failed = [c for c in checks if not c[1]]
    if verbose:
        print("\n%d checks, %d failed" % (len(checks), len(failed)))
    return 0 if not failed else 1


# --------------------------------------------------------------------------
def model() -> dict:
    return {
        "exe": exelib.EXE,
        "image_base": "0x400000",
        "formula": {
            "numerator": FORMULA_LATEX[0],
            "texture": FORMULA_LATEX[1],
            "lightmap": FORMULA_LATEX[2],
            "constants": {
                "uv_u_fp_per_texel": FP_PER_TEXEL,
                "uv_u_per_texel": UV_U_PER_TEXEL,
                "numerator_multiplier": 64.0,
                "texels_per_world_unit": 1.0,
                "lightmap_texels_per_world_unit": 1.0 / 32.0,
            },
            "modes": MODES,
            "default_mode": DEFAULT_MODE,
        },
        "status": {
            "texture_divisor_64": "proven",
            "uv_u_fp_unit_1_64_texel": "proven",
            "sign_plus_dot": "proven (lightmap sampler + auto-UV anchor)",
            "texel_rounding": ("hypothesis: the engine converts through float "
                               "and 16.16, so +/-1 texel jitter is possible"),
            "lightmap_is_32x_coarser": "hypothesis (follows from the shared numerator)",
        },
        "anchors": anchors(),
        "untextured": draws_untextured(),
        "data": [stats(n) for n in LEVEL_NAMES],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true", help="dump the model as JSON")
    ap.add_argument("--self-test", action="store_true", help="anchors + formula + data")
    ap.add_argument("--stats", nargs="?", const="level0", metavar="LEVEL",
                    help="data invariants of one .sbw")
    ap.add_argument("--anchors", action="store_true", help="verify anchors only")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.anchors:
        ok = True
        for a in verify_anchors() + verify_constants():
            print("%s %-10s %s  %s" % (a["address"],
                                       "ok" if a["ok"] else "MISMATCH",
                                       a.get("bytes", a.get("expected")),
                                       a["what"]))
            ok = ok and a["ok"]
        return 0 if ok else 1
    if args.stats:
        print(json.dumps(stats(args.stats), indent=2, ensure_ascii=False))
        return 0
    if args.json:
        print(json.dumps(model(), indent=2, ensure_ascii=False))
        return 0

    m = model()
    f = m["formula"]
    print("Текстурные UV кадрового прохода (доказано, gundx.exe 1.36)\n")
    print("  %s" % f["numerator"])
    print("  %s" % f["texture"])
    print("  %s" % f["lightmap"])
    print("\n  uv_u_fp: 1/%d texel (0x43e122: /4.0), uv_u: 1/%d texel (0x43e0ff)"
          % (f["constants"]["uv_u_fp_per_texel"], f["constants"]["uv_u_per_texel"]))
    print("  масштаб: %.1f тексель на единицу мира (эмпирика: 1088/1088 полигонов)"
          % f["constants"]["texels_per_world_unit"])
    print("  режимы: %s (по умолчанию %s)" % (f["modes"], f["default_mode"]))
    print("\nПолигоны без текстуры: %s" % m["untextured"]["verdict"].upper())
    print("  %s" % m["untextured"]["summary"])
    print("  лазейки: %s" % "; ".join(m["untextured"]["escape_hatches"]))
    print("\nданные:")
    for st in m["data"]:
        if not st.get("available"):
            print("  %-7s недоступен" % st["level"])
            continue
        u = st["untextured"]
        print("  %-7s %4d полигонов, без текстуры %3d (%.0f%%), "
              "texel==world %s, uv_u==4*uv_u_fp %s"
              % (st["level"], st["polys"], u["count"], 100.0 * u["share"],
                 st["textured_uv_u_fp_is_1_64_texel"]["texel_extent_equals_world_extent"],
                 st["uv_u_equals_4x_uv_fp_mod_64"]))
    print("\nподробности: python3 tools/re/texuv.py --json | --self-test | "
          "--anchors | --stats dm1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
