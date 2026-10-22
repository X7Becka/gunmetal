#!/usr/bin/env python3
"""Texture alpha and frame constant-colour audit (M4.1.5).

Question
--------
``docs/MATERIAL_BLEND.md`` §4 concludes that the shipped 3dfx path draws every
polygon with one Glide state and that "translucency comes from the texture's
alpha" (``grAlphaBlendFunction(ONE, ONE_MINUS_SRC_ALPHA, ZERO, ZERO)`` at
``0x440abe``).  This tool checks the two halves of that sentence against the
data and the executable:

1. **Does a ``.sbm`` texture carry any alpha?**  The format is 8 bits per texel,
   palette indices, with a mip chain (``gm/sbm.py``, ``docs/FORMATS.md`` §2).
   If there were an alpha plane or per-texel alpha bits, the payload would be
   larger than ``expected_size()`` (``extra`` bytes) or the size identity would
   fail.  The only possible transparency is therefore **binary**, through one
   palette index.
2. **What is passed to ``grConstantColorValue`` in the frame pass?**  The import
   thunk is resolved dynamically with ``GetProcAddress`` (slot -> name pins it),
   so the call sites and their immediate arguments are recoverable from the
   listing without guessing.

What the shipped data says (recomputed by this tool)
----------------------------------------------------
``PICS``: 1648 blobs, every payload is exactly ``12 + expected_size()`` bytes
(``extra == 0`` for all 1648) -- there is no alpha plane and no spare channel.
``flags`` (header ``+0x08``) is a transparency bitfield that agrees **exactly**
with the loader's own classification (``0x423947``):

======  ==========================================================  =========
bit     meaning                                                     count
======  ==========================================================  =========
0x1     transparent: level 0 contains palette index 0 often enough   126/1648
0x2     sky (name ``SKY*``, loader sets type 2)                       5/1648
0x10    water (name ``WAT*``, loader ORs 0x10 into the type)          36/1648
======  ==========================================================  =========

The loader's heuristic is ``type = 1`` when index 0 occurs and
``zero_count >= 1 << min(log2w, log2h)``; otherwise a ``PICS`` texture is
demoted with ``"Warning: Illegal transparent texture %s"``.  The set of 126
PICS blobs that pass is byte-for-byte the set with ``flags & 1`` (126 == 126,
no symmetric difference); sprites (179, one mip) have ``flags & 1`` for exactly
the 178 blobs that contain index 0.

The palette is where the transparency is implemented: ``0x4d31a0`` is filled
from the engine palette ``0x83d320`` with the **alpha byte left at 0**, then
entry 0 is overwritten with ``0x010101`` (``0x4187b4``) and that value is handed
to ``grChromakeyValue`` (``0x4187d4``).  The texture type stored in the runtime
slot (``+0x04``) toggles ``grChromakeyMode`` per texture (``0x418ac2``): type 1
enables the key, type 0 disables it.  So transparency is a **chroma key on
palette index 0**, not an alpha channel.

The frame state (all inside the ``ctx+0xec & 0x800`` Glide branch of the frame
function ``0x4409b8``)::

    grColorCombine(1, 0, 1, 2, 0)
      function=GR_COMBINE_FUNCTION_LOCAL(1), factor=NONE(0),
      local=GR_COMBINE_LOCAL_CONSTANT(1), other=GR_COMBINE_OTHER_CONSTANT(2)
      => RGB = grConstantColorValue
    grAlphaCombine(1, 0, 0, 2, 0)
      function=LOCAL(1), factor=NONE(0), local=ITERATED(0), other=CONSTANT(2)
      => alpha = iterated (vertex) alpha; texture alpha is NOT used
    grAlphaBlendFunction(ONE, ONE_MINUS_SRC_ALPHA, ZERO, ZERO)
    grConstantColorValue(0xFFFFFFFF) at 0x440be8  <-- literal, white, opaque

``grConstantColorValue`` is called only four times in the whole image and every
argument is a compile-time immediate: ``0xFFFFFFFF`` in the frame pass, then
``0xFFFFFF`` / ``0xFF0000`` / ``0xFFFFFF`` in the 2D/sprite paths.  No light,
palette or material value ever reaches it.  The per-frame shading is the
``lighttable`` post-pass (``0x441b41``/``0x441b59``), not the constant colour.

So ``docs/MATERIAL_BLEND.md`` §4 is right about "one state for the whole frame"
and wrong about "alpha from the texture": the texture contributes no alpha, the
frame alpha blend uses the iterated alpha, and per-texel transparency is a
chroma key.

Usage
-----
    python3 tools/re/alpha_audit.py --self-test
    python3 tools/re/alpha_audit.py --level level0
    python3 tools/re/alpha_audit.py --level level0 --json > alpha.json

Everything is read-only: the game installation (or its mirror in
``~/.cache/gunmetal-re/game``) is never written, and no Ghidra instance is used.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from gm import bigfile, sbm, sbw  # noqa: E402

try:                                    # RE helpers need the objdump listing
    import exelib                       # noqa: E402
except Exception:                       # pragma: no cover - listing unavailable
    exelib = None

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
CACHE_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")

#: The loader's transparency scan (``docs/MIPMAPS.md`` §2.3).
TRANSPARENT_SCAN_VA = 0x423947
TRANSPARENT_WARN_VA = 0x423a31          # "Warning: Illegal transparent texture"
SKY_CHECK_VA = 0x4238f3
WATER_CHECK_VA = 0x423a40

#: Glide 2.x enums (glide2x/glide.h, verified against the open Glide source).
COMBINE_FUNCTION = {
    0x0: "ZERO/NONE", 0x1: "LOCAL", 0x2: "LOCAL_ALPHA", 0x3: "SCALE_OTHER",
    0x4: "SCALE_OTHER_ADD_LOCAL", 0x5: "SCALE_OTHER_ADD_LOCAL_ALPHA",
    0x6: "SCALE_OTHER_MINUS_LOCAL", 0x7: "SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL",
    0x8: "SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA",
    0x9: "SCALE_MINUS_LOCAL_ADD_LOCAL", 0x10: "SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA",
}
COMBINE_FACTOR = {
    0x0: "NONE/ZERO", 0x1: "LOCAL", 0x2: "OTHER_ALPHA", 0x3: "LOCAL_ALPHA",
    0x4: "TEXTURE_ALPHA", 0x5: "TEXTURE_RGB/LOD_FRACTION", 0x8: "ONE",
    0x9: "ONE_MINUS_LOCAL", 0xa: "ONE_MINUS_OTHER_ALPHA",
    0xb: "ONE_MINUS_LOCAL_ALPHA", 0xc: "ONE_MINUS_TEXTURE_ALPHA",
    0xd: "ONE_MINUS_LOD_FRACTION",
}
COMBINE_LOCAL = {0x0: "ITERATED", 0x1: "CONSTANT", 0x2: "DEPTH"}
COMBINE_OTHER = {0x0: "ITERATED", 0x1: "TEXTURE", 0x2: "CONSTANT/NONE"}
BLEND_FACTOR = {
    0x0: "ZERO", 0x1: "ONE", 0x2: "DST_COLOR", 0x3: "ONE_MINUS_DST_COLOR",
    0x4: "SRC_ALPHA", 0x5: "ONE_MINUS_SRC_ALPHA", 0x6: "DST_ALPHA",
    0x7: "ONE_MINUS_DST_ALPHA", 0x8: "ALPHA_SATURATE",
}

#: Role hints for call sites, keyed by function start VA.
FUNCTION_ROLES = {
    0x4409b8: "frame pass (software rasterizer + Glide present; docs/JOURNAL.md)",
    0x4188b2: "2D/sprite draw path",
    0x4184b6: "2D/sprite draw path",
    0x417eda: "texture upload (GrTexInfo array 0x4d7290)",
}

PUSH_RE = re.compile(r"\bpush\s+0x([0-9a-f]+)\b")
BRANCH_RE = re.compile(r"\b(call|ret|jmp|je|jne|jl|jle|jg|jge|jb|jbe|ja|jae)\b")


# ---------------------------------------------------------------------------
# .sbm alpha capability
# ---------------------------------------------------------------------------


def _zero_share(pixels: bytes) -> float:
    return (pixels.count(0) / len(pixels)) if pixels else 0.0


def texture_alpha(blob: bytes, name: str, kind: str) -> dict:
    """Decode one ``.sbm`` and describe everything alpha-related about it.

    ``kind`` is ``"pics"`` (index 0x76ed40 slot kind 0) or ``"sprites"`` (1);
    the loader's "too few zero texels" heuristic only applies to PICS.
    """
    tex = sbm.parse(blob)
    log2w = tex.width.bit_length() - 1
    log2h = tex.height.bit_length() - 1
    payload = sum(len(level) for level in tex.levels_data)
    zeros = [level.count(0) for level in tex.levels_data]
    shares = [z / len(level) for z, level in zip(zeros, tex.levels_data)]
    threshold = 1 << min(log2w, log2h)
    heuristic = bool(zeros[0]) and zeros[0] >= threshold
    # The loader only demotes PICS textures; sprites keep type 1 outright.
    transparent = heuristic if kind == "pics" else bool(zeros[0])
    up = name.upper()
    return {
        "name": name,
        "kind": kind,
        "width": tex.width,
        "height": tex.height,
        "levels": tex.levels,
        "flags": tex.flags,
        "payload": payload,
        "expected_payload": sbm.expected_size(log2w, log2h, tex.levels - 1),
        "extra": len(tex.extra),
        "texels": tex.width * tex.height,
        "zero_texels": zeros[0],
        # No 8-bit alpha exists: alpha<128 is exactly palette index 0.
        "alpha_low_texels": zeros[0],
        "alpha_low_share": _zero_share(tex.pixels),
        # 1 = every texel opaque, 2 = binary {0, 255} (index 0 keyed).
        "alpha_levels": 1 if zeros[0] == 0 else 2,
        "zero_per_mip": zeros,
        "zero_share_per_mip": shares,
        "mip_zero_growth": (max(shares[1:]) - shares[0]) if len(shares) > 1 else 0.0,
        "threshold": threshold,
        "loader_transparent": heuristic,
        "transparent": transparent,
        "flag_transparent": bool(tex.flags & 0x1),
        "flag_sky": bool(tex.flags & 0x2),
        "flag_water": bool(tex.flags & 0x10),
        "name_sky": up.startswith("SKY"),
        "name_water": up.startswith("WAT"),
        # A payload larger than the mip chain would be the only room left for a
        # separate alpha plane; shipped data has none.
        "alpha_plane_suspect": len(tex.extra) > 0,
    }


def _bucket(share: float) -> str:
    if share == 0:
        return "0"
    if share < 0.02:
        return "<2%"
    if share < 0.10:
        return "2..10%"
    if share < 0.30:
        return "10..30%"
    return ">=30%"


def _summarise(rows: list[dict]) -> dict:
    texels = sum(r["texels"] for r in rows)
    zeros = sum(r["zero_texels"] for r in rows)
    flags = collections.Counter(r["flags"] for r in rows)
    levels = collections.Counter(r["alpha_levels"] for r in rows)
    buckets = collections.Counter(_bucket(r["alpha_low_share"]) for r in rows)
    extra = collections.Counter(r["extra"] for r in rows)
    transparent_heur = {r["name"] for r in rows
                        if r["kind"] == "pics" and r["loader_transparent"]}
    transparent_expected = {r["name"] for r in rows if r["transparent"]}
    transparent_flag = {r["name"] for r in rows if r["flag_transparent"]}
    sky_names = {r["name"] for r in rows if r["name_sky"]}
    sky_flags = {r["name"] for r in rows if r["flag_sky"]}
    water_names = {r["name"] for r in rows if r["name_water"]}
    water_flags = {r["name"] for r in rows if r["flag_water"]}
    return {
        "textures": len(rows),
        "texels": texels,
        "zero_texels": zeros,
        "zero_share": (zeros / texels) if texels else 0.0,
        "payload_exact": sum(1 for r in rows
                             if r["payload"] == r["expected_payload"]),
        "extra_histogram": dict(sorted(extra.items())),
        "alpha_plane_suspects": [r["name"] for r in rows if r["alpha_plane_suspect"]],
        "flags_histogram": dict(sorted(flags.items())),
        "alpha_levels_histogram": {str(k): v for k, v in sorted(levels.items())},
        "zero_share_buckets": dict(buckets),
        "transparent_by_loader": len(transparent_heur),
        "transparent_expected": len(transparent_expected),
        "transparent_by_flag": len(transparent_flag),
        "transparent_sets_equal": transparent_expected == transparent_flag,
        "transparent_flag_only": sorted(transparent_flag - transparent_expected),
        "transparent_expected_only": sorted(transparent_expected - transparent_flag),
        "sky_names": len(sky_names),
        "sky_flags": len(sky_flags),
        "sky_sets_equal": sky_names == sky_flags,
        "water_names": len(water_names),
        "water_flags": len(water_flags),
        "water_sets_equal": water_names == water_flags,
        "mip_growth_over_2pp": sum(1 for r in rows if r["mip_zero_growth"] > 0.02),
        "fully_keyed": [r["name"] for r in rows if r["alpha_low_share"] == 1.0],
    }


def audit_archive(game: str, sub: str) -> tuple[list[dict], dict]:
    idx = os.path.join(game, sub, "SBM.IDX")
    if not os.path.exists(idx):
        raise SystemExit(f"alpha_audit: {idx} not found")
    kind = "sprites" if sub.lower() == "sprites" else "pics"
    bf = bigfile.BigFile(idx)
    rows = []
    try:
        for entry in bf.entries:
            blob = bf.read(entry)
            try:
                rows.append(texture_alpha(blob, entry.name, kind))
            except ValueError as exc:
                rows.append({"name": entry.name, "kind": kind, "error": str(exc)})
    finally:
        bf.close()
    good = [r for r in rows if "error" not in r]
    summary = _summarise(good)
    summary["decode_errors"] = [r["name"] for r in rows if "error" in r]
    summary["archive"] = sub
    summary["idx"] = idx
    return good, summary


# ---------------------------------------------------------------------------
# texture pool (same rules as gm.render.TexturePool)
# ---------------------------------------------------------------------------


class Pool:
    def __init__(self, game: str, archives: tuple[str, ...] = ("PICS", "sprites")):
        self.maps = []
        for sub in archives:
            idx = os.path.join(game, sub, "SBM.IDX")
            if not os.path.exists(idx):
                continue
            bf = bigfile.BigFile(idx)
            kind = "sprites" if sub.lower() == "sprites" else "pics"
            self.maps.append((bf, {e.name: e for e in bf.entries}, kind))

    def resolve(self, name: str):
        if not name or name == "none":
            return None
        for bf, entries, kind in self.maps:
            entry = entries.get(name)
            if entry is None:
                up = name.upper()
                for key, value in entries.items():
                    if key.upper() == up:
                        entry = value
                        break
            if entry is not None:
                return bf.read(entry), kind
        return None

    def close(self):
        for bf, _entries, _kind in self.maps:
            bf.close()


def audit_level(game: str, level: str) -> dict:
    path = level
    if not os.path.isfile(path):
        for cand in (os.path.join(game, level),
                     os.path.join(game, level + ".sbw"),
                     os.path.join(game, level.lower() + ".sbw")):
            if os.path.isfile(cand):
                path = cand
                break
    if not os.path.isfile(path):
        raise SystemExit(f"alpha_audit: level {level!r} not found")

    world = sbw.parse_file(path)
    pool = Pool(game)
    try:
        names = sorted({t.name for t in world.tex})
        rows, missing, kinds = [], [], collections.Counter()
        for name in names:
            got = pool.resolve(name)
            if got is None:
                missing.append(name)
                continue
            blob, kind = got
            row = texture_alpha(blob, name, kind)
            kinds[kind] += 1
            rows.append(row)
    finally:
        pool.close()

    polys = [p for s in world.sectors for o in s.objects for p in o.polys]
    used = collections.Counter(p.texture - 1 for p in polys if p.texture > 0)
    by_name = {r["name"]: r for r in rows}
    tex_names = [t.name for t in world.tex]

    def texture_of(index: int) -> dict | None:
        if 0 <= index < len(tex_names):
            return by_name.get(tex_names[index])
        return None

    transparent_polys = 0
    poly_weights = []
    for index, count in used.items():
        row = texture_of(index)
        if row and row["flag_transparent"]:
            transparent_polys += count
            poly_weights.append((row["name"], count, row["alpha_low_share"]))
    transparent_world = [r for r in rows
                         if r["kind"] == "pics" and r["flag_transparent"]]
    transparent_world.sort(key=lambda r: -r["alpha_low_share"])

    return {
        "level": os.path.basename(path),
        "texture_names": len(world.tex),
        "unique_names": len(names),
        "resolved": len(rows),
        "missing": missing,
        "resolved_by_kind": dict(kinds),
        "texels": sum(r["texels"] for r in rows),
        "zero_texels": sum(r["zero_texels"] for r in rows),
        "zero_share": (sum(r["zero_texels"] for r in rows)
                       / max(1, sum(r["texels"] for r in rows))),
        "transparent_textures": sum(1 for r in rows if r["flag_transparent"]),
        "transparent_pics": len(transparent_world),
        "transparent_world": [
            {"name": r["name"], "width": r["width"], "height": r["height"],
             "zero_texels": r["zero_texels"], "texels": r["texels"],
             "zero_share": r["alpha_low_share"]}
            for r in transparent_world
        ],
        "transparent_sprite_textures": sum(
            1 for r in rows if r["kind"] == "sprites" and r["flag_transparent"]),
        "polys": len(polys),
        "polys_textured": sum(used.values()),
        "polys_with_transparent_texture": transparent_polys,
        "polys_transparent_use": sorted(
            ({"name": n, "polys": c, "zero_share": s} for n, c, s in poly_weights),
            key=lambda x: -x["polys"]),
        "poly_flags_0x2": sum(1 for p in polys if p.flags & 0x2),
        "materials": dict(sorted(collections.Counter(p.material for p in polys).items())),
        "flag_heuristic_mismatch": [
            r["name"] for r in rows
            if r["kind"] == "pics" and r["flag_transparent"] != r["loader_transparent"]],
    }


# ---------------------------------------------------------------------------
# Glide state in the executable
# ---------------------------------------------------------------------------


def _rows():
    return exelib.asm_rows()


def glide_thunks() -> dict:
    """``{function name: (slot, install_va)}`` from the GetProcAddress loop."""
    if exelib is None:
        return {}
    rows = _rows()
    out = {}
    text_to_slot = {}
    # ``mov ds:0x4d74xx,eax`` right after ``GetProcAddress`` pins the slot.
    for i, (_va, line) in enumerate(rows):
        m = re.search(r"mov\s+ds:0x(4d74[0-9a-f]+),eax", line)
        if not m:
            continue
        slot = int(m.group(1), 16)
        name = None
        for j in range(i - 1, max(i - 8, -1), -1):
            pm = PUSH_RE.search(rows[j][1])
            if not pm:
                continue
            cand = exelib.cstr(int(pm.group(1), 16)) or ""
            if cand.startswith("_gr") or cand.startswith("_gu"):
                name = cand.lstrip("_").split("@")[0]
                break
        if name:
            out[name] = (slot, rows[i][0])
            text_to_slot[name] = slot
    return out


def _pushes_since_call(rows, index: int) -> list[int]:
    """Immediate operands pushed since the previous call/branch, in order."""
    pushes = []
    j = index - 1
    while j >= 0:
        line = rows[j][1]
        if BRANCH_RE.search(line):
            break
        m = PUSH_RE.search(line)
        if m:
            pushes.append(int(m.group(1), 16))
        j -= 1
    pushes.reverse()
    return pushes


def byte_xrefs(slot: int) -> list[int]:
    """Every little-endian reference to *slot* in the image (offset of operand)."""
    if exelib is None:
        return []
    needle = struct.pack("<I", slot)
    data = exelib.data()
    out, i = [], 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            return out
        va = exelib.off2va(i)
        if va is not None:
            out.append(va)
        i += 1


def glide_calls(slot: int) -> list[dict]:
    """Every ``call DWORD PTR ds:<slot>`` with the immediates queued for it."""
    rows = _rows()
    out = []
    pat = re.compile(r"call\s+DWORD PTR ds:0x%x\b" % slot)
    for i, (va, line) in enumerate(rows):
        if not pat.search(line):
            continue
        out.append({"va": va, "pushes": _pushes_since_call(rows, i)})
    return out


def decode_combine(pushes: list[int]) -> dict:
    """Decode ``grColorCombine`` / ``grAlphaCombine`` from queued pushes.

    The engine pushes arguments right-to-left (stdcall), so the first pushed
    value is the *last* parameter; reversing the queue restores arg order.
    """
    if len(pushes) < 5:
        return {"error": "need 5 args", "pushes": pushes}
    args = list(reversed(pushes))[-5:]
    fn, factor, local, other, invert = args
    return {
        "args": args,
        "function": COMBINE_FUNCTION.get(fn, f"0x{fn:x}"),
        "factor": COMBINE_FACTOR.get(factor, f"0x{factor:x}"),
        "local": COMBINE_LOCAL.get(local, f"0x{local:x}"),
        "other": COMBINE_OTHER.get(other, f"0x{other:x}"),
        "invert": bool(invert),
    }


def decode_blend(pushes: list[int]) -> dict:
    """Decode ``grAlphaBlendFunction`` from queued pushes (4 args)."""
    if len(pushes) < 4:
        return {"error": "need 4 args", "pushes": pushes}
    src, dst, asrc, adst = list(reversed(pushes))[-4:]
    return {
        "args": [src, dst, asrc, adst],
        "rgb_src": BLEND_FACTOR.get(src, f"0x{src:x}"),
        "rgb_dst": BLEND_FACTOR.get(dst, f"0x{dst:x}"),
        "alpha_src": BLEND_FACTOR.get(asrc, f"0x{asrc:x}"),
        "alpha_dst": BLEND_FACTOR.get(adst, f"0x{adst:x}"),
    }


def function_start(rows, va: int) -> int:
    """Address of the nearest ``push ebp; mov ebp,esp`` prologue at/below va."""
    vmas = [r[0] for r in rows]
    i = max(0, bisect.bisect_right(vmas, va) - 1)
    prologue = re.compile(r"\bpush\s+ebp\b")
    frame = re.compile(r"\bmov\s+ebp\s*,\s*esp\b")
    while i > 0:
        if prologue.search(rows[i][1]) and i + 1 < len(rows) \
                and frame.search(rows[i + 1][1]):
            return rows[i][0]
        i -= 1
    return rows[0][0] if rows else va


def audit_exe() -> dict:
    if exelib is None:
        return {"available": False, "reason": "exelib/objdump listing unavailable"}
    try:
        thunks = glide_thunks()
    except Exception as exc:            # pragma: no cover - broken listing
        return {"available": False, "reason": f"{exc}"}
    if not thunks:
        return {"available": False, "reason": "no Glide thunks found"}

    rows = _rows()
    report = {"available": True, "thunks": {}, "calls": {}, "combine": {}}

    def slot(name):
        item = thunks.get(name)
        return item[0] if item else None

    for name in ("grConstantColorValue", "grColorCombine", "grAlphaCombine",
                 "grAlphaBlendFunction", "grChromakeyValue", "grChromakeyMode",
                 "grTexDownloadTable", "grFogColorValue"):
        item = thunks.get(name)
        if item:
            report["thunks"][name] = {"slot": item[0], "install": item[1]}

    sites = collections.defaultdict(list)
    for name in report["thunks"]:
        for call in glide_calls(slot(name)):
            call["function"] = function_start(rows, call["va"])
            sites[name].append(call)
    report["calls"] = {k: v for k, v in sites.items()}

    for name, key in (("grColorCombine", "color"), ("grAlphaCombine", "alpha")):
        report["combine"][key] = [
            dict(decode_combine(c["pushes"]), va=c["va"],
                 function_start=c["function"], confident=len(c["pushes"]) >= 5)
            for c in sites.get(name, [])]
    report["combine"]["blend"] = [
        dict(decode_blend(c["pushes"]), va=c["va"],
             function_start=c["function"], confident=len(c["pushes"]) >= 4)
        for c in sites.get("grAlphaBlendFunction", [])]

    report["constant_colour"] = [
        {"va": c["va"], "arg": (c["pushes"][-1] if c["pushes"] else None),
         "args": c["pushes"],
         "function_start": c["function"],
         "role": FUNCTION_ROLES.get(c["function"], f"0x{c['function']:x}")}
        for c in sites.get("grConstantColorValue", [])]

    # The palette entry 0 is overwritten with the chroma key itself.
    palette = []
    for va, line in rows:
        m = re.search(r"mov\s+DWORD PTR ds:0x4d31a0,0x([0-9a-f]+)", line)
        if m:
            palette.append({"va": va, "value": int(m.group(1), 16)})
    report["palette_entry0_override"] = palette
    entry0 = palette[0]["value"] if palette else None

    # ``grChromakeyValue`` is called with ``mov ecx,ds:0x4d31a0; push ecx`` --
    # the argument is palette entry 0, not a literal.
    chromakey = []
    for c in sites.get("grChromakeyValue", []):
        arg = c["pushes"][-1] if c["pushes"] else None
        source = "immediate" if arg is not None else None
        if arg is None:
            i = _index_of(rows, c["va"])
            for j in range(i - 1, max(i - 8, -1), -1):
                if re.search(r"\bmov\s+[a-z0-9]+,(?:DWORD PTR )?ds:0x4d31a0\b",
                             rows[j][1]):
                    arg, source = entry0, "palette[0] (0x4d31a0)"
                    break
        chromakey.append({"va": c["va"], "arg": arg, "source": source})
    report["chromakey_value"] = chromakey

    # grChromakeyMode toggled from the runtime texture type at slot+4.
    toggles = []
    for c in sites.get("grChromakeyMode", []):
        window = [rows[j][1] for j in range(max(0, _index_of(rows, c["va"]) - 14),
                                            _index_of(rows, c["va"]))]
        typed = any(re.search(r"mov\s+cl,BYTE PTR \[[a-z0-9]+\+0x4\]", w)
                    or re.search(r"mov\s+[a-z]{2},BYTE PTR \[[a-z0-9]+\+0x4\]", w)
                    for w in window)
        toggles.append({"va": c["va"],
                        "arg": (c["pushes"][-1] if c["pushes"] else None),
                        "uses_slot_plus_4": typed})
    report["chromakey_mode_toggles"] = toggles

    # The texture sampler state lives next to the palette upload (docs/MIPMAPS).
    report["frame_colour_combine"] = [
        c for c in report["combine"]["color"]
        if c["function_start"] == 0x4409b8]
    report["frame_alpha_combine"] = [
        c for c in report["combine"]["alpha"]
        if c["function_start"] == 0x4409b8]
    report["frame_blend"] = [
        c for c in report["combine"]["blend"] if c["function_start"] == 0x4409b8]
    report["constant_colour_xrefs"] = byte_xrefs(slot("grConstantColorValue"))
    report["anchors"] = {
        "transparent_scan": TRANSPARENT_SCAN_VA,
        "warning": exelib.cstr(0x4a8d88),
        "sky_check": SKY_CHECK_VA,
        "water_check": WATER_CHECK_VA,
    }
    return report


def _index_of(rows, va: int) -> int:
    vmas = [r[0] for r in rows]
    return bisect.bisect_left(vmas, va)


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------


def print_archive(summary: dict) -> None:
    print(f"\n== {summary['archive']} ==")
    print(f"  blobs                 {summary['textures']}")
    print(f"  12 + expected_size    {summary['payload_exact']} / {summary['textures']}")
    print(f"  extra bytes after mip {summary['extra_histogram']}")
    print(f"  flags                 {summary['flags_histogram']}")
    print(f"  alpha levels          {summary['alpha_levels_histogram']}"
          "   (1 = all opaque, 2 = binary {0,255})")
    print(f"  texels                {summary['texels']:,}")
    print(f"  index-0 texels        {summary['zero_texels']:,}"
          f"  ({100 * summary['zero_share']:.3f}%)  <- alpha<128")
    print(f"  zero-share buckets    {summary['zero_share_buckets']}")
    print(f"  transparent (loader)  {summary['transparent_by_loader']}"
          f"   expected {summary['transparent_expected']}"
          f"   flags bit 0: {summary['transparent_by_flag']}"
          f"   sets equal: {summary['transparent_sets_equal']}")
    print(f"  sky / water flags     {summary['sky_flags']}/{summary['sky_names']}"
          f" exact={summary['sky_sets_equal']}, "
          f"{summary['water_flags']}/{summary['water_names']}"
          f" exact={summary['water_sets_equal']}")
    print(f"  alpha-plane suspects  {len(summary['alpha_plane_suspects'])}")
    print(f"  lower-mip zero growth {summary['mip_growth_over_2pp']} textures"
          " (>= +2pp vs level 0)")
    if summary["fully_keyed"]:
        print(f"  fully keyed (100% 0)  {', '.join(summary['fully_keyed'])}")


def print_level(level: dict, top: int) -> None:
    print(f"\n== level {level['level']} ==")
    print(f"  names {level['texture_names']} ({level['unique_names']} unique),"
          f" resolved {level['resolved']}  {level['resolved_by_kind']}")
    if level["missing"]:
        print(f"  missing               {', '.join(level['missing'])}")
    print(f"  texels {level['texels']:,}   index-0 {level['zero_texels']:,}"
          f" ({100 * level['zero_share']:.3f}%)")
    print(f"  transparent textures  {level['transparent_textures']}"
          f"  (world/PICS {level['transparent_pics']},"
          f" sprites {level['transparent_sprite_textures']})")
    print(f"  polygons {level['polys']} ({level['polys_textured']} textured),"
          f" flags&2 {level['poly_flags_0x2']},"
          f" with a transparent texture {level['polys_with_transparent_texture']}")
    print(f"  materials             {level['materials']}")
    print("  transparent world textures (PICS, share of keyed texels):")
    for row in level["transparent_world"][:top]:
        print(f"    {row['name']:<12} {row['width']:>4}x{row['height']:<4}"
              f" {row['zero_texels']:>7}/{row['texels']:<7}"
              f" {100 * row['zero_share']:6.2f}%")
    if level["polys_transparent_use"]:
        print("  polygon use of keyed textures:")
        for row in level["polys_transparent_use"][:top]:
            print(f"    {row['name']:<12} {row['polys']:>4} polys"
                  f"  {100 * row['zero_share']:6.2f}% keyed")


def print_exe(exe: dict) -> None:
    print("\n== Glide state (gundx.exe) ==")
    if not exe.get("available"):
        print(f"  skipped: {exe.get('reason')}")
        return
    for name, item in sorted(exe["thunks"].items()):
        print(f"  {name:<22} slot 0x{item['slot']:x}"
              f"  installed at 0x{item['install']:x}")
    print("  grConstantColorValue call sites (literal argument):")
    for call in exe["constant_colour"]:
        arg = call["arg"]
        shown = f"0x{arg:08x}" if arg is not None else "?"
        print(f"    0x{call['va']:x}  arg {shown}  in 0x{call['function_start']:x}"
              f"  ({call['role']})")
    print("  frame colour combine:")
    for call in exe["frame_colour_combine"]:
        print(f"    0x{call['va']:x}  {call['args']} -> function={call['function']},"
              f" local={call['local']}, other={call['other']},"
              f" factor={call['factor']}")
    print("  frame alpha combine:")
    for call in exe["frame_alpha_combine"]:
        print(f"    0x{call['va']:x}  {call['args']} -> function={call['function']},"
              f" local={call['local']}, other={call['other']}")
    print("  frame alpha blend:")
    for call in exe["frame_blend"]:
        print(f"    0x{call['va']:x}  {call['args']} ->"
              f" src={call['rgb_src']}, dst={call['rgb_dst']}")
    print(f"  byte-scan xrefs to the slot: "
          f"{', '.join('0x%x' % x for x in exe['constant_colour_xrefs'])}")
    print("  palette entry 0 override:",
          ", ".join(f"0x{p['va']:x}=0x{p['value']:06x}"
                    for p in exe["palette_entry0_override"]) or "not found")
    print(f"  loader type scan:   0x{TRANSPARENT_SCAN_VA:x},"
          f" warning \"{exe.get('anchors', {}).get('warning', '').strip()}\""
          f" at 0x{TRANSPARENT_WARN_VA:x}")
    print("  grChromakeyValue:",
          ", ".join(f"0x{c['va']:x} arg "
                    f"{('0x%06x' % c['arg']) if c['arg'] is not None else '?'}"
                    for c in exe["chromakey_value"]) or "no call")
    typed = [t for t in exe["chromakey_mode_toggles"] if t["uses_slot_plus_4"]]
    print(f"  grChromakeyMode toggles: {len(exe['chromakey_mode_toggles'])}"
          f" ({len(typed)} read the texture type at slot+4)")


def print_verdict(archives: dict, exe: dict | None, level: dict | None) -> None:
    pics = archives.get("PICS", {})
    print("\n== verdict ==")
    print("  .sbm alpha:        none (8 bpp palette index, extra bytes == 0);"
          " transparency is binary, via palette index 0")
    print(f"  PICS:              {pics.get('transparent_by_flag', 0)}/"
          f"{pics.get('textures', 0)} textures keyed (flags bit 0),"
          f" {100 * pics.get('zero_share', 0):.3f}% of texels")
    if level:
        print(f"  {level['level']}:{' ' * (17 - len(level['level']))}"
              f"{level['transparent_textures']}/{level['resolved']} resolved"
              f" textures keyed, {100 * level['zero_share']:.2f}% of texels")
    if exe and exe.get("available"):
        constants = sorted({c["arg"] for c in exe["constant_colour"]
                            if c["arg"] is not None})
        print("  grConstantColorValue: literals "
              + ", ".join(f"0x{a:08x}" for a in constants)
              + " -- never a light/palette colour")
        frame = exe["frame_colour_combine"][0] if exe["frame_colour_combine"] else None
        if frame:
            print(f"  frame colour:      local={frame['local']} -> the constant"
                  " colour is the RGB source; alpha combine local="
                  f"{exe['frame_alpha_combine'][0]['local'] if exe['frame_alpha_combine'] else '?'}"
                  " (iterated, not texture)")
    print("  shading:           lighttable post-pass 0x441b41/0x441b59,"
          " not the constant colour")


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------


def _self_test() -> int:
    checks = []

    def check(cond, text):
        checks.append((bool(cond), text))
        print(("ok   " if cond else "FAIL ") + text)

    # --- synthetic .sbm: no alpha, binary alpha, boundary, trailing plane ---
    blank = bytes(8 * 8)
    opaque = bytes([1]) * (8 * 8)
    seven = bytes([0]) * 7 + bytes([1]) * (8 * 8 - 7)
    row = texture_alpha(sbm.encode(8, 8, opaque, levels=1), "WALL", "pics")
    check(row["alpha_levels"] == 1 and row["zero_texels"] == 0,
          "opaque 8x8: alpha_levels=1, zero_texels=0")
    check(not row["loader_transparent"] and not row["flag_transparent"],
          "opaque 8x8: not transparent")
    row = texture_alpha(sbm.encode(8, 8, blank, levels=1), "MASK", "pics")
    check(row["alpha_levels"] == 2 and row["zero_texels"] == 64,
          "all-zero 8x8: binary alpha, 64 keyed texels")
    check(row["loader_transparent"], "all-zero 8x8: passes the loader heuristic")
    row = texture_alpha(sbm.encode(8, 8, seven, levels=1), "FEW", "pics")
    check(not row["loader_transparent"] and row["threshold"] == 8,
          "7 zeros < threshold 8: loader would demote (boundary)")
    check(texture_alpha(sbm.encode(8, 8, blank[:63] + b"\x01", levels=1),
                        "MASK2", "pics")["loader_transparent"],
          "8 zeros == threshold 8: loader accepts (boundary)")
    with_plane = sbm.encode(8, 8, opaque, levels=1) + bytes([0xAA]) * 16
    row = texture_alpha(with_plane, "PLANE", "pics")
    check(row["extra"] == 16 and row["alpha_plane_suspect"],
          "trailing bytes: reported as a possible alpha plane")

    # --- mip growth ---
    mips = sbm.encode(8, 8, opaque, levels=3)
    check(texture_alpha(mips, "MIPS", "pics")["levels"] == 3,
          "mip chain of 3 levels decodes")

    # --- name classification mirrors the loader (0x4238f3 / 0x423a40) ---
    small = bytes([1]) * (4 * 4)
    sky = texture_alpha(sbm.encode(4, 4, small, levels=1), "sky7", "pics")
    wat = texture_alpha(sbm.encode(4, 4, small, levels=1), "WatFloor", "pics")
    check(sky["name_sky"] and not sky["name_water"], "SKY* detected (type 2)")
    check(wat["name_water"] and not wat["name_sky"], "WAT* detected (water bit)")

    # --- determinism ---
    a = texture_alpha(sbm.encode(8, 8, blank, levels=2), "DET", "pics")
    b = texture_alpha(sbm.encode(8, 8, blank, levels=2), "DET", "pics")
    check(a == b, "scan is deterministic")

    # --- Glide enum decoding (argument pushes are right-to-left) ---
    colour = decode_combine([0, 2, 1, 0, 1])       # -> grColorCombine(1,0,1,2,0)
    check(colour["function"] == "LOCAL" and colour["local"] == "CONSTANT"
          and colour["other"].startswith("CONSTANT"),
          "grColorCombine(1,0,1,2,0): RGB = LOCAL(CONSTANT)")
    alpha = decode_combine([0, 2, 0, 0, 1])        # -> grAlphaCombine(1,0,0,2,0)
    check(alpha["function"] == "LOCAL" and alpha["local"] == "ITERATED",
          "grAlphaCombine(1,0,0,2,0): alpha = LOCAL(ITERATED)")
    blend = decode_blend([0, 0, 5, 1])             # -> (1,5,0,0)
    check(blend["rgb_src"] == "ONE" and blend["rgb_dst"] == "ONE_MINUS_SRC_ALPHA",
          "grAlphaBlendFunction(1,5,0,0) decodes to ONE / ONE_MINUS_SRC_ALPHA")

    # --- argument extraction from a synthetic listing ---
    fake = [
        (0x1000, "  1000:\t6a ff               push   0xffffffff"),
        (0x1002, "  1002:\tff 15 68 74 4d 00   call   DWORD PTR ds:0x4d7468"),
    ]
    check(_pushes_since_call(fake, 1) == [0xffffffff],
          "push-argument extraction from a synthetic listing")

    # --- shipped exe (skipped when the installation is unavailable) ---
    exe = audit_exe()
    if exe.get("available"):
        frame = exe["frame_colour_combine"]
        check(bool(frame) and frame[0]["local"] == "CONSTANT",
              "shipped frame colour combine selects local=CONSTANT")
        consts = {c["arg"] for c in exe["constant_colour"]}
        check(0xFFFFFFFF in consts, "shipped grConstantColorValue receives 0xFFFFFFFF")
        check(any(p["value"] == 0x010101 for p in exe["palette_entry0_override"]),
              "palette entry 0 is overwritten with 0x010101 (chroma key)")
        check(any(c.get("arg") == 0x010101 for c in exe["chromakey_value"]),
              "grChromakeyValue is set to the same 0x010101")
        xrefs = exe.get("constant_colour_xrefs", [])
        check(len([x for x in xrefs if x - 2 in
                   {c["va"] for c in exe["constant_colour"]}]) ==
              len(exe["constant_colour"]),
              "byte scan finds only the listed grConstantColorValue calls")
        check(exe.get("anchors", {}).get("warning", "").startswith(
                  "Warning: Illegal transparent texture"),
              "loader warning string is present (0x4a8d88)")
        code = "".join(exelib.disasm(0x4239cc, 0x4239d8))
        check("test" in code and "[eax+0x4],0x1" in code,
              "loader sets slot+4 = 1 on the first index-0 texel (0x4239cc)")
    else:
        print("skip shipped-exe checks:", exe.get("reason"))

    bad = [text for ok, text in checks if not ok]
    print(f"\nself-test: {len(checks) - len(bad)}/{len(checks)} checks passed")
    return 0 if not bad else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def find_game(explicit: str | None) -> str:
    for cand in (explicit, os.environ.get("GUNMETAL_GAME"), CACHE_GAME,
                 DEFAULT_GAME):
        if cand and os.path.isdir(os.path.join(cand, "PICS")):
            return cand
    raise SystemExit("alpha_audit: no game directory with PICS/ found; use --game")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Audit .sbm texture alpha and the Glide constant colour.")
    ap.add_argument("--game", help="game directory (default: mirror, then install)")
    ap.add_argument("--level", action="append", default=[],
                    help="level name or path (repeatable); audits only the "
                         "textures that level references")
    ap.add_argument("--no-exe", action="store_true",
                    help="skip the gundx.exe/Glide scan")
    ap.add_argument("--top", type=int, default=10,
                    help="rows in the per-texture tables (default 10)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic format/boundary/determinism checks")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    game = find_game(args.game)
    archives = {}
    rows = {}
    for sub in ("PICS", "sprites"):
        try:
            good, summary = audit_archive(game, sub)
        except SystemExit:
            continue
        rows[sub] = good
        archives[sub] = summary

    levels = [audit_level(game, name) for name in args.level]
    exe = {"available": False, "reason": "--no-exe"} if args.no_exe else audit_exe()

    report = {
        "game": game,
        "archives": archives,
        "levels": levels,
        "exe": exe,
        "textures": rows,
    }
    if args.json:
        print(json.dumps(report, indent=1, sort_keys=True))
        return 0

    print("alpha_audit: texture alpha / constant colour (read-only)")
    print(f"game: {game}")
    for sub in ("PICS", "sprites"):
        if sub in archives:
            print_archive(archives[sub])
    for level in levels:
        print_level(level, max(1, args.top))
    print_exe(exe)
    print_verdict(archives, exe, levels[0] if levels else None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
