#!/usr/bin/env python3
"""What the engine paints *behind* the geometry: frame clear + sky audit (M4.1.6).

`gm/render.py` fills its canvas with a vertical gradient (`BG_TOP`/`BG_BOTTOM`,
lines 543-544 and 695) that no engine code produces.  This tool answers what the
shipped 3dfx build really does, from bytes only (Ghidra is *not* used):

1. **The Glide entry-point table.**  `0x416920` loads ``glide2x.dll`` with
   ``GetProcAddress`` and stores each pointer at ``0x4d7448 + 4*i``
   (name string -> slot).  Resolving that table is what makes the rest readable:
   a call site ``call DWORD PTR ds:<slot>`` can then be named.  (The table is
   built at runtime; the ``0x4d74xx`` cells are in BSS, so the *loader*, not the
   data, is the evidence.)

2. **The frame clear.**  There is exactly one ``grBufferClear`` call site in the
   whole image: ``0x418859`` inside ``0x4184b6``.  Its arguments are
   ``color=0, alpha=0, depth=0xffff`` and it is *bracketed* by
   ``grColorMask(FALSE, TRUE)`` (``0x41884a``) and ``grColorMask(TRUE, TRUE)``
   (``0x418863``) — i.e. only the **depth and alpha** planes are cleared; the
   colour buffer is never cleared, in any frame.  Per-frame reachability:
   ``0x44c230`` (frame) -> ``0x443690`` -> ``0x41928e`` (Glide scene pass,
   gated on ``ctx+0xec & 0x800``) -> ``0x4184b6`` -> the clear.

3. **The sky is geometry.**  Neither frame pass reads a texture type, an
   object flag or any "sky" state: the only sky-aware code in the exe is two
   loader *warnings* (``0x4a8e64`` / ``0x4244e6``) about a sky texture found on
   a polygon.  Sky is therefore a textured polygon like any other, taken from
   the level's own ``.sbw`` texture-name table (``SKY1``..``SKY5``, plus ``FOG``
   /``WATFOG`` sheets).  This tool tabulates that per level: 36 shipped levels,
   which sky textures they use, where those polygons sit, and which levels have
   none (fully interior).

4. **Distance fog**, for completeness, since it is what makes far geometry look
   like a "sky colour": the frame pass sets ``grFogColorValue(0xffffff)``
   (``0x440a97``, white), ``grFogMode(2)`` (``0x440a8c``, table on Q) and
   ``guFogGenerateExp(table, 0.9)`` (``0x440aa6``); the Glide setup rebuilds a
   linear table from ``ctx+0xc4`` with ``near = 0.25 * far`` (``0x41889c``).
   ``ctx+0xc4`` comes from the global ``0x80d7f0`` (default ``0x7fffffff`` when
   zero, ``0x44a127``), which is written by the level script — SBT command
   ``98 SET_LEVEL_FOG_DISTANCE`` (``0x45b7cd``).

Everything is read-only.  The game directory is never written to; the objdump
listing and exe copy come from ``tools/re/exelib.py``'s persistent cache.

Examples::

    python3 tools/re/sky.py                 # human report
    python3 tools/re/sky.py --json          # machine-readable, ``summary`` last
    python3 tools/re/sky.py --self-test     # synthetic parse + determinism + image
    python3 tools/re/sky.py --level level1  # one level in detail

See ``docs/SKY.md`` for the conclusions and for what ``gm/render.py --bg``
should draw instead of the gradient.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

import exelib  # noqa: E402
from gm import sbw  # noqa: E402
from gm import sbt as sbtmod  # noqa: E402
from gm import sbtvm  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")

#: ``0x416920`` .. ``0x416e21`` — the ``GetProcAddress`` loop for glide2x.dll.
LOADER_LO, LOADER_HI = 0x416920, 0x416E22
#: First cell of the pointer table (``grSstIdle``); stride 4.
SLOT_BASE = 0x4D7448
#: ``mov DWORD PTR ds:0x4d74xx,eax`` right after ``GetProcAddress``.
SLOT_STORE_RANGE = (0x4D7440, 0x4D7500)
#: `.rdata` window that holds the ``_grXxx@N`` names.
NAME_LO, NAME_HI = 0x4A85B4, 0x4A8870

#: Software frame pass: traverses sectors/objects/polys, writes an 8-bit frame
#: buffer at ``ctx+0x104`` and fog-maps it with the material `lighttable`.
FRAME_SW = 0x4409B8
#: Glide frame pass: state setup + traversal + ``guDrawPolygonVertexListWithClip``.
FRAME_GLIDE = 0x41928E
#: Glide state setup called by `FRAME_GLIDE`; contains the only buffer clear.
GLIDE_SETUP = 0x4184B6
#: ``grBufferClear`` call site.
CLEAR_SITE = 0x418859
#: Frame function and the scene-draw function it calls.
FRAME_FN = 0x44C230
SCENE_FN = 0x443690
#: ``grFogColorValue``/``grFogMode``/``guFogGenerateExp`` in the sw pass and
#: ``guFogGenerateLinear`` in the Glide setup.
FOG_COLOR_SITE = 0x440A97
FOG_MODE_SITE = 0x440A8C
FOG_EXP_SITE = 0x440AA6
FOG_LINEAR_SITE = 0x41889C
#: Fog distance plumbing.
FOG_DIST_GLOBAL = 0x80D7F0
FOG_DIST_DEFAULT = 0x7FFFFFFF
FOG_DIST_WRITE = 0x45B7CD
#: ``ctx+0xec & 0x800`` — the "Glide present" gate in `FRAME_GLIDE`.
GLIDE_GATE_SITE = 0x419398
GLIDE_GATE_MASK = 0x800
#: The only full-screen fill besides the clear: a white fade quad (screen fade),
#: depth test off / depth write off, alpha from ``ctx+0xe8``.
FADE_QUAD = {"draw": 0x418C71, "color": 0x418CB8, "alpha_scale": 0x418C85,
             "depth_mask_off": 0x4188D0, "depth_always": 0x418D08,
             "alpha_field": "ctx+0xe8"}

#: Texture names that the data uses for the backdrop.  The *engine* has no such
#: classification (it draws these polys like any other); the split below is a
#: labelling of the shipped names, documented in ``docs/SKY.md``.
SKY_PREFIX = "SKY"
FOG_SHEETS = ("FOG", "WATFOG")
#: Objects whose local extent exceeds this are backdrop shells in the shipped
#: data (the heuristic used by ``tools/re/doors.py``/``regress.py``); the tool
#: reports it next to the *name*-based classification so the two can be compared.
SHELL_EXTENT = 4000


# --------------------------------------------------------------------------
# listing helpers
# --------------------------------------------------------------------------
def insns(lo: int, hi: int) -> list[tuple[int, str]]:
    """``[(va, mnemonic+operands)]`` for ``[lo, hi)`` from the objdump listing."""
    out = []
    for line in exelib.disasm(lo, hi):
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        try:
            va = int(parts[0].strip().rstrip(":"), 16)
        except ValueError:
            continue
        out.append((va, " ".join(parts[2].split())))
    return out


def imm_of(text: str) -> int | None:
    """Immediate operand of a ``push``/``mov`` line, or ``None`` (register)."""
    if not text.startswith(("push ", "mov ")):
        return None
    ops = text.split(None, 1)[1]
    for tok in ops.replace(",", " ").split():
        t = tok.strip()
        if t.startswith("0x"):
            try:
                return int(t, 16)
            except ValueError:
                return None
        if t.lstrip("-").isdigit():
            return int(t)
    return None


def pushes_before(rows: list[tuple[int, str]], idx: int, depth: int = 8):
    """Immediate ``push`` operands directly before ``rows[idx]`` (push order).

    Returns ``(values, first_row_index)``; a non-immediate push contributes
    ``None`` so a caller can refuse to guess instead of silently shifting args.
    """
    vals: list[int | None] = []
    j = idx - 1
    while j >= 0 and len(vals) < depth and rows[j][1].startswith("push "):
        vals.append(imm_of(rows[j][1]))
        j -= 1
    vals.reverse()
    return vals, j + 1


def cdecl_args(pushes: list[int | None]) -> list[int | None]:
    """Arguments in C order: the *first* push is the *last* argument."""
    return list(reversed(pushes))


def _scan(pattern: bytes) -> list[int]:
    d = exelib.data()
    out, pos = [], d.find(pattern)
    while pos >= 0:
        out.append(exelib.off2va(pos))
        pos = d.find(pattern, pos + 1)
    return out


def callers(target: int) -> list[int]:
    """Every ``call rel32`` site that targets ``target``."""
    d = exelib.data()
    out, pos = [], d.find(b"\xe8")
    while pos >= 0:
        rel = int.from_bytes(d[pos + 1:pos + 5], "little", signed=True)
        if (exelib.off2va(pos) + 5 + rel) & 0xFFFFFFFF == target:
            out.append(exelib.off2va(pos))
        pos = d.find(b"\xe8", pos + 1)
    return out


def call_sites(slot: int) -> list[int]:
    """Every ``call DWORD PTR ds:<slot>`` site."""
    return _scan(b"\xff\x15" + slot.to_bytes(4, "little"))


def function_start(va: int, back: int = 0x8000) -> int | None:
    """Nearest preceding ``push ebp; mov ebp,esp`` after a ``ret``/padding."""
    d = exelib.data()
    o = exelib.va2off(va)
    if o is None:
        return None
    for i in range(o, max(0, o - back), -1):
        if d[i:i + 3] == b"\x55\x8b\xec" and d[i - 1] in (0xC3, 0xCC, 0x90):
            return exelib.off2va(i)
    return None


def _va(off: int) -> str:
    return "0x%x" % off


# --------------------------------------------------------------------------
# Glide entry points
# --------------------------------------------------------------------------
def parse_glide_name(raw: str) -> tuple[str, int]:
    """``'_grBufferClear@12'`` -> ``('grBufferClear', 3)``.

    Glide's stdcall decoration carries the *argument byte count*; ``@N`` with
    ``N % 4 == 0`` is the only shape the loader strings use.  A malformed name
    raises instead of producing a half-parsed table.
    """
    name = raw.strip().lstrip("_")
    argc = 0
    if "@" in name:
        name, _, nbytes = name.partition("@")
        if not nbytes.isdigit():
            raise ValueError("bad Glide decoration: %r" % raw)
        nbytes = int(nbytes)
        if nbytes % 4:
            raise ValueError("Glide argument bytes not a multiple of 4: %r" % raw)
        argc = nbytes // 4
    if not name:
        raise ValueError("empty Glide name: %r" % raw)
    return name, argc


def glide_slots() -> dict[int, dict]:
    """Resolve ``0x4d74xx -> {name, argc, name_va}`` from the loader, not from data."""
    rows = insns(LOADER_LO, LOADER_HI)
    slots: dict[int, dict] = {}
    name_va: int | None = None
    for i, (_va_, text) in enumerate(rows):
        if text.startswith("push ") and name_va is None:
            v = imm_of(text)
            if v is not None and NAME_LO <= v < NAME_HI:
                name_va = v
        elif text.startswith("mov ds:0x") and name_va is not None:
            dst = int(text.split("ds:")[1].split(",")[0], 16)
            if SLOT_STORE_RANGE[0] <= dst < SLOT_STORE_RANGE[1]:
                raw = exelib.cstr(name_va, 32)
                name, argc = parse_glide_name(raw)
                slots[dst] = {"slot": dst, "name": name, "argc": argc,
                              "name_va": name_va, "load_site": rows[i - 1][0]}
                name_va = None
    return slots


def slot_of(slots: dict[int, dict], name: str) -> int:
    for slot, info in slots.items():
        if info["name"] == name:
            return slot
    raise KeyError("Glide entry %r not in the loaded table" % name)


def glide_call(slots: dict[int, dict], name: str, site: int) -> dict:
    """Decode one already-known call site: pushes -> C arguments."""
    rows = insns(site - 0x18, site + 8)
    idx = next((i for i, (va, _t) in enumerate(rows) if va == site), None)
    if idx is None:
        raise SystemExit("sky: call site %s not in the listing" % _va(site))
    pushes, _first = pushes_before(rows, idx)
    return {"call": site, "api": name, "pushes": pushes,
            "args": cdecl_args(pushes), "declared_args": slots[slot_of(slots, name)]["argc"]}


# --------------------------------------------------------------------------
# level data
# --------------------------------------------------------------------------
def find_game(explicit: str | None) -> str:
    for cand in (explicit, DEFAULT_GAME, MIRROR_GAME):
        if cand and os.path.isdir(cand) and glob.glob(os.path.join(cand, "*.sbw")):
            return cand
    raise SystemExit("sky: no game directory with .sbw files "
                     "(tried %s, %s)" % (DEFAULT_GAME, MIRROR_GAME))


def classify(name: str) -> str | None:
    """'sky' / 'fog' / None — a *label* of the shipped texture name only."""
    up = name.upper()
    if up.startswith(SKY_PREFIX):
        return "sky"
    if up in FOG_SHEETS:
        return "fog"
    return None


def object_extent(obj) -> float:
    vs = obj.vertices
    if not vs:
        return 0.0
    return max(max(v[k] for v in vs) - min(v[k] for v in vs) for k in range(3))


def _walk(insns):
    """Yield every instruction, recursing into ``IF``/``ELSE`` bodies."""
    for ins in insns:
        yield ins
        body = getattr(ins, "body", None)
        if isinstance(body, list):
            yield from _walk(body)


def fog_distance(sbt_path: str) -> int | None:
    """``SET_LEVEL_FOG_DISTANCE`` (VM op 98) from the level script, if present.

    Same decode as ``tools/re/campaign.py``; the operand is the value the engine
    stores into ``0x80d7f0`` (``0x45b7cd``) and hence into ``ctx+0xc4``.
    """
    if not os.path.isfile(sbt_path):
        return None
    recs = sbtmod.parse_file(sbt_path).records_of(11)
    if not recs:
        return None
    insns, _ = sbtvm.decode(recs[0].payload.code)
    for ins in _walk(insns):
        if ins.name == "SET_LEVEL_FOG_DISTANCE" and ins.args:
            arg = ins.args[0]
            return arg[1] if isinstance(arg, tuple) else arg
    return None


def level_row(path: str) -> dict:
    w = sbw.parse_file(path)
    name_of = [t.name for t in w.tex]
    row = {"level": os.path.basename(path)[:-4],
           "file": os.path.basename(path), "sectors": len(w.sectors),
           "objects": sum(len(s.objects) for s in w.sectors),
           "polys": sum(len(o.polys) for s in w.sectors for o in s.objects),
           "textures": len(w.tex), "sky_textures": [], "fog_textures": [],
           "backdrop_polys": 0, "backdrop_sectors": [], "on_shell": 0,
           "at_poly0": 0, "shells": [], "samples": [],
           "fog_distance": fog_distance(os.path.splitext(path)[0] + ".sbt")}
    sky_names, fog_names = set(), set()
    materials: dict[int, int] = {}
    for si, sec in enumerate(w.sectors):
        for oi, obj in enumerate(sec.objects):
            ext = object_extent(obj)
            is_shell = ext >= SHELL_EXTENT
            sky_here = 0
            for pi, poly in enumerate(obj.polys):
                t = poly.texture
                if not (0 < t <= len(name_of)):
                    continue
                nm = name_of[t - 1]
                kind = classify(nm)
                if kind is None:
                    continue
                (sky_names if kind == "sky" else fog_names).add(nm.upper())
                row["backdrop_polys"] += 1
                sky_here += 1
                if is_shell:
                    row["on_shell"] += 1
                if pi == 0:
                    row["at_poly0"] += 1
                materials[poly.material] = materials.get(poly.material, 0) + 1
                if len(row["samples"]) < 4:
                    row["samples"].append({"sector": si, "object": oi, "poly": pi,
                                           "texture": nm, "kind": kind,
                                           "material": poly.material,
                                           "object_extent": round(ext, 1),
                                           "shell": is_shell})
            if sky_here:
                row["backdrop_sectors"].append(si)
                if is_shell:
                    row["shells"].append({"sector": si, "object": oi,
                                          "extent": round(ext, 1),
                                          "polys": len(obj.polys),
                                          "backdrop_polys": sky_here})
    row["sky_textures"] = sorted(sky_names)
    row["fog_textures"] = sorted(fog_names)
    row["backdrop_materials"] = {str(k): v for k, v in sorted(materials.items())}
    return row


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------
def audit(game: str | None = None) -> dict:
    game = find_game(game)
    slots = glide_slots()
    clear_slot = slot_of(slots, "grBufferClear")
    sites = call_sites(clear_slot)
    clear = glide_call(slots, "grBufferClear", CLEAR_SITE)
    mask_off = glide_call(slots, "grColorMask", 0x41884A)
    mask_on = glide_call(slots, "grColorMask", 0x418863)
    depth_before = None
    for va, text in insns(0x418800, 0x41885A):
        if text.startswith("call") and "0x4d7458" in text:
            depth_before = va
            break
    depth_fn = (glide_call(slots, "grDepthBufferFunction", depth_before)["args"][0]
                if depth_before else None)

    chain = []
    for frm, to, site in ((FRAME_FN, SCENE_FN, 0x44D923),
                          (SCENE_FN, FRAME_GLIDE, 0x4469BC),
                          (FRAME_GLIDE, GLIDE_SETUP, 0x4193A6),
                          (GLIDE_SETUP, None, CLEAR_SITE)):
        entry = {"from": frm, "to": to, "call": site,
                 "verified": site in callers(to)} if to else \
                {"from": frm, "to": None, "call": site,
                 "verified": site in call_sites(clear_slot)}
        if not entry["verified"]:
            raise SystemExit("sky: call edge %s -> %s not found at %s"
                             % (_va(frm), _va(to) if to else "grBufferClear", _va(site)))
        chain.append(entry)

    levels = [level_row(p) for p in
              sorted(glob.glob(os.path.join(game, "*.sbw")))]
    with_sky = [r["level"] for r in levels if r["sky_textures"]]
    without = [r["level"] for r in levels if not r["sky_textures"]]
    fog_levels = {r["level"]: r["fog_distance"] for r in levels
                  if r["fog_distance"]} 

    report = {
        "exe": {"sha256": exelib.exe_sha256(), "listing": exelib.listing_info()},
        "glide": {"entries": len(slots),
                  "slots": [slots[k] for k in sorted(slots)]},
        "clear": {
            "api": clear["api"], "call": clear["call"],
            "function": function_start(clear["call"]),
            "args": {"color": clear["args"][0], "alpha": clear["args"][1],
                     "depth": clear["args"][2]},
            "declared_args": clear["declared_args"],
            "sites_in_image": sites,
            "rgb_mask_off": {"call": mask_off["call"], "args": mask_off["args"],
                             "writes_color": bool(mask_off["args"][0])},
            "rgb_mask_on": {"call": mask_on["call"], "args": mask_on["args"]},
            "depth_buffer_function": {"call": depth_before, "value": depth_fn},
            "clears_color": bool(mask_off["args"][0]),
            "clears_depth": True,
            "clears_alpha": bool(mask_off["args"][1]),
        },
        "frame_chain": chain,
        "fills": {
            "clear": {"call": CLEAR_SITE, "color": clear["args"][0],
                      "writes_color": False},
            "fade_quad": {
                "call": FADE_QUAD["draw"],
                "constant_color": glide_call(slots, "grConstantColorValue",
                                             FADE_QUAD["color"])["args"][0],
                "depth_mask_off": glide_call(slots, "grDepthMask",
                                             FADE_QUAD["depth_mask_off"])["args"][0],
                "depth_function": glide_call(slots, "grDepthBufferFunction",
                                             FADE_QUAD["depth_always"])["args"][0],
                "alpha_field": FADE_QUAD["alpha_field"],
                "purpose": "screen fade/flash (alpha 0..255 from ctx+0xe8)",
            },
        },
        "glide_gate": {"site": GLIDE_GATE_SITE, "mask": GLIDE_GATE_MASK,
                       "field": "ctx+0xec"},
        "fog": {
            "color": {"call": FOG_COLOR_SITE, "value": 0xFFFFFF,
                      "rgb": [255, 255, 255]},
            "mode": {"call": FOG_MODE_SITE, "value": 2},
            "exp_table": {"call": FOG_EXP_SITE, "density": 0.9},
            "linear_table": {"call": FOG_LINEAR_SITE, "near_ratio": 0.25,
                             "far": "ctx+0xc4"},
            "distance_global": FOG_DIST_GLOBAL,
            "distance_default": FOG_DIST_DEFAULT,
            "distance_written_by": {
                "call": FOG_DIST_WRITE,
                "vm_command": "98 SET_LEVEL_FOG_DISTANCE"},
        },
        "sky": {
            "code_paths_that_read_a_sky_flag": [],
            "loader_warnings": [
                {"va": 0x4A8E64, "call": 0x4244E6,
                 "text": exelib.cstr(0x4A8E64, 96),
                 "condition": "texture-type nibble == 2 and sector != 0"},
                {"va": 0x4A8EA8, "call": 0x42458F,
                 "text": exelib.cstr(0x4A8EA8, 96),
                 "condition": "transparent texture (not sky) placed illegally"},
            ],
            "type_byte": {"table": 0x76ED40, "stride": 0x10, "type_offset": 4,
                          "readers": sorted(_xrefs_texinfo()),
                          "read_by_frame_passes": False},
            "content": "textured .sbw polygons; no skybox object, no sky flag",
        },
        "levels": levels,
        "summary": {
            "levels": len(levels),
            "levels_with_backdrop_polys": len(with_sky),
            "levels_without": without,
            "backdrop_polys": sum(r["backdrop_polys"] for r in levels),
            "sky_textures": sorted({n for r in levels for n in r["sky_textures"]}),
            "fog_textures": sorted({n for r in levels for n in r["fog_textures"]}),
            "fog_distances": fog_levels,
            "frame_color_clear": False,
            "clear_color": 0x000000,
            "bg_recommendation": "#000000",
        },
    }
    return report


def print_report(rep: dict, only: str | None = None) -> None:
    cl = rep["clear"]
    print("glide: %d entry points resolved from the loader at 0x416920"
          % rep["glide"]["entries"])
    print("clear: %s at 0x%x (function 0x%x)" % (cl["api"], cl["call"], cl["function"]))
    print("  args      color=0x%06x alpha=%d depth=0x%x"
          % (cl["args"]["color"], cl["args"]["alpha"], cl["args"]["depth"]))
    print("  rgb mask  off at 0x%x -> %s, on at 0x%x -> %s"
          % (cl["rgb_mask_off"]["call"], cl["rgb_mask_off"]["args"],
             cl["rgb_mask_on"]["call"], cl["rgb_mask_on"]["args"]))
    print("  sites in image: %s" % ", ".join("0x%x" % s for s in cl["sites_in_image"]))
    print("  colour cleared: %s   depth: %s   alpha: %s"
          % (cl["clears_color"], cl["clears_depth"], cl["clears_alpha"]))
    print("chain: " + " -> ".join(
        "0x%x" % e["from"] for e in rep["frame_chain"]) + " -> grBufferClear")
    fq = rep["fills"]["fade_quad"]
    print("fills: clear writes colour: %s; only other full-screen fill = fade quad "
          "0x%x (white 0x%06x, depth off/always, alpha <- %s)"
          % (rep["fills"]["clear"]["writes_color"], fq["call"], fq["constant_color"],
             fq["alpha_field"]))
    print("fog:   colour 0x%06x (white), mode %d, exp table density %.1f, "
          "linear near = 0.25*far, far = ctx+0xc4 <- 0x%x (default 0x%x)"
          % (rep["fog"]["color"]["value"], rep["fog"]["mode"]["value"],
             rep["fog"]["exp_table"]["density"],
             rep["fog"]["distance_global"], rep["fog"]["distance_default"]))
    print("sky:   %d backdrop polys over %d/%d levels; textures %s"
          % (rep["summary"]["backdrop_polys"],
             rep["summary"]["levels_with_backdrop_polys"],
             rep["summary"]["levels"], ", ".join(rep["summary"]["sky_textures"])))
    print()
    print("%-10s %5s %6s %7s %6s  %-20s %s"
          % ("level", "secs", "polys", "backdr", "fog", "sky textures", "fog sheets"))
    for row in rep["levels"]:
        if only and row["level"] != only:
            continue
        print("%-10s %5d %6d %7d %6s  %-20s %s"
              % (row["level"], row["sectors"], row["polys"], row["backdrop_polys"],
                 row["fog_distance"] if row["fog_distance"] else "-",
                 ",".join(row["sky_textures"]) or "-",
                 ",".join(row["fog_textures"]) or "-"))
        for s in row["samples"] if only else []:
            print("      sector %d object %d poly %d: %s (%s), material %d, "
                  "extent %.0f%s" % (s["sector"], s["object"], s["poly"],
                                     s["texture"], s["kind"], s["material"],
                                     s["object_extent"],
                                     ", shell" if s["shell"] else ""))
    if not only:
        print()
        print("levels without backdrop polygons: %s"
              % ", ".join(rep["summary"]["levels_without"]))


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------
def self_test() -> int:
    checks = 0

    def ok(cond, msg):
        nonlocal checks
        checks += 1
        if not cond:
            raise AssertionError(msg)
        print("ok %2d - %s" % (checks, msg))

    # --- synthetic: argument decoding from listing-shaped rows -------------
    syn = [(0x1000, "push 0xffff"), (0x1002, "push 0x0"), (0x1004, "push 0x0"),
           (0x1006, "call DWORD PTR ds:0x4d748c")]
    pushes, first = pushes_before(syn, 3)
    ok(pushes == [0xFFFF, 0, 0], "synthetic: three immediate pushes decoded")
    ok(cdecl_args(pushes) == [0, 0, 0xFFFF],
       "synthetic: cdecl order -> (color, alpha, depth) = (0, 0, 0xffff)")
    ok(first == 0, "synthetic: push run starts at the first row")

    syn2 = [(0x2000, "push 0x1"), (0x2002, "push 0x0"),
            (0x2004, "call DWORD PTR ds:0x4d7480")]
    ok(cdecl_args(pushes_before(syn2, 2)[0]) == [0, 1],
       "synthetic: grColorMask(0, 1) means rgb=0, alpha=1")

    syn3 = [(0x3000, "push eax"), (0x3002, "push 0x2"),
            (0x3004, "call DWORD PTR ds:0x4d7470")]
    vals = pushes_before(syn3, 2)[0]
    ok(vals == [None, 0x2], "synthetic: register push is None, not silently zero")

    # --- synthetic: Glide name decoration --------------------------------
    ok(parse_glide_name("_grBufferClear@12") == ("grBufferClear", 3),
       "synthetic: '_grBufferClear@12' -> 3 arguments")
    ok(parse_glide_name("_grColorMask@8") == ("grColorMask", 2),
       "synthetic: '_grColorMask@8' -> 2 arguments")
    ok(parse_glide_name("_grSstIdle@0") == ("grSstIdle", 0),
       "synthetic: '_grSstIdle@0' -> 0 arguments")

    # --- synthetic: level classification ----------------------------------
    ok(classify("SKY1") == "sky" and classify("sky3") == "sky",
       "synthetic: SKY1/sky3 classified as sky")
    ok(classify("FOG") == "fog" and classify("WATFOG") == "fog",
       "synthetic: FOG/WATFOG classified as fog sheets")
    ok(classify("LEV3-5C") is None and classify("SKYLINE") == "sky",
       "synthetic: ordinary levels textures are not sky")

    # --- real image: the Glide table and the single clear -----------------
    slots = glide_slots()
    ok(len(slots) >= 30, "image: %d Glide entry points resolved" % len(slots))
    ok(slots[0x4D748C]["name"] == "grBufferClear" and
       slots[0x4D748C]["argc"] == 3,
       "image: 0x4d748c is grBufferClear with 3 arguments")
    ok(slots[0x4D74C8]["name"] == "guFogGenerateLinear",
       "image: 0x4d74c8 is guFogGenerateLinear (loader order is consistent)")

    clear_slot = slot_of(slots, "grBufferClear")
    sites = call_sites(clear_slot)
    ok(sites == [CLEAR_SITE],
       "image: exactly one grBufferClear call site (0x%x)" % CLEAR_SITE)
    clear = glide_call(slots, "grBufferClear", CLEAR_SITE)
    ok(clear["args"] == [0, 0, 0xFFFF],
       "image: grBufferClear(0, 0, 0xffff) -> depth cleared, colour 0")
    ok(len(clear["pushes"]) == clear["declared_args"],
       "image: %d pushes match the @%d decoration"
       % (len(clear["pushes"]), clear["declared_args"] * 4))

    mask_off = glide_call(slots, "grColorMask", 0x41884A)
    mask_on = glide_call(slots, "grColorMask", 0x418863)
    ok(mask_off["args"] == [0, 1],
       "image: grColorMask(FALSE, TRUE) before the clear -> RGB not written")
    ok(mask_on["args"] == [1, 1],
       "image: grColorMask(TRUE, TRUE) right after the clear")
    ok(mask_off["call"] < CLEAR_SITE < mask_on["call"],
       "image: the clear is bracketed by the two masks (colour survives)")

    ok(function_start(CLEAR_SITE) == GLIDE_SETUP,
       "image: the clear lives in the Glide setup 0x%x" % GLIDE_SETUP)
    ok(0x4469BC in callers(FRAME_GLIDE) and 0x4193A6 in callers(GLIDE_SETUP),
       "image: 0x443690 -> 0x41928e -> 0x4184b6 call chain")
    ok(0x44D923 in callers(SCENE_FN),
       "image: the frame function 0x%x calls 0x%x" % (FRAME_FN, SCENE_FN))
    ok(function_start(0x44D923) == FRAME_FN,
       "image: 0x44d923 is inside the frame function 0x%x" % FRAME_FN)
    ok(function_start(0x4469BC) == SCENE_FN,
       "image: the Glide pass is entered from 0x%x" % SCENE_FN)
    ok(function_start(FOG_COLOR_SITE) == FRAME_SW,
       "image: grFogColorValue sits in the software frame pass 0x%x" % FRAME_SW)

    fog_color = glide_call(slots, "grFogColorValue", FOG_COLOR_SITE)
    ok(fog_color["args"] == [0xFFFFFF], "image: fog colour is white (0xffffff)")

    ok(glide_call(slots, "grConstantColorValue", FADE_QUAD["color"])["args"] == [0xFFFFFF],
       "image: the full-screen fade quad is white (grConstantColorValue 0xffffff)")
    ok(glide_call(slots, "grDepthMask", FADE_QUAD["depth_mask_off"])["args"] == [0],
       "image: the fade quad runs with grDepthMask(FALSE)")
    ok(glide_call(slots, "grDepthBufferFunction", FADE_QUAD["depth_always"])["args"] == [7],
       "image: the fade quad runs with grDepthBufferFunction(ALWAYS)")

    # no sky-specific code: no reference to a texture-type byte in either pass
    texinfo = _xrefs_texinfo()
    ok(bool(texinfo), "image: the texture table 0x76ed40 is referenced somewhere "
                      "(%d immediates)" % len(texinfo))
    ok(not any(FRAME_SW <= c < 0x4420D4 for c in texinfo),
       "image: the software frame pass never reads the texture-type byte")
    ok(not any(FRAME_GLIDE <= c < 0x41A620 for c in texinfo),
       "image: the Glide frame pass never reads the texture-type byte")

    # --- levels -----------------------------------------------------------
    game = find_game(None)
    rep = audit(game)
    ok(rep["levels"] and len(rep["levels"]) == 36,
       "data: 36 shipped .sbw levels parsed (%d)" % len(rep["levels"]))
    ok(rep["summary"]["levels_with_backdrop_polys"] == 30,
       "data: 30/36 levels carry SKY*/FOG backdrop polygons")
    ok(rep["summary"]["levels_without"] ==
       ["level0", "level00", "level000", "level0a", "level0b", "level24"],
       "data: the six backdrop-less levels are the interior ones")
    ok(rep["summary"]["sky_textures"] == ["SKY1", "SKY2", "SKY3", "SKY4", "SKY5"],
       "data: sky textures are SKY1..SKY5 (case-insensitive in the tables)")
    lvl1 = next(r for r in rep["levels"] if r["level"] == "level1")
    ok(lvl1["sky_textures"] == ["SKY1"] and lvl1["backdrop_polys"] == 34,
       "data: level1 uses SKY1 on 34 polygons")
    ok(rep["summary"]["fog_distances"].get("dm2") == 2400 and
       rep["summary"]["fog_distances"].get("level23") == 50,
       "data: SET_LEVEL_FOG_DISTANCE gives dm2=2400, level23=50")
    ok(next(r for r in rep["levels"] if r["level"] == "level0")["fog_distance"] is None,
       "data: level0 has no SET_LEVEL_FOG_DISTANCE (engine default 0x7fffffff)")

    # --- determinism ------------------------------------------------------
    again = audit(game)
    ok(json.dumps(rep, sort_keys=True) == json.dumps(again, sort_keys=True),
       "determinism: two runs produce byte-identical JSON")
    rev = [level_row(p) for p in
           sorted(glob.glob(os.path.join(game, "*.sbw")), reverse=True)]
    ok(json.dumps(sorted(rev, key=lambda r: r["level"]), sort_keys=True) ==
       json.dumps(rep["levels"], sort_keys=True),
       "determinism: the level table does not depend on file order")
    ok(list(rep)[-1] == "summary", "report: 'summary' is the last key")

    print("\nself-test: %d checks, all passed" % checks)
    return 0


def _xrefs_texinfo() -> list[int]:
    """Addresses of ``... ed 76 00`` immediates (texinfo table) in the image."""
    return _scan(b"\x44\xed\x76\x00")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true", help="dump everything as JSON")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic parse + image assertions + determinism")
    ap.add_argument("--level", help="show one level in detail")
    ap.add_argument("--game", default=None, help="game directory (default: %s)"
                    % DEFAULT_GAME)
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    rep = audit(args.game)
    if args.json:
        print(json.dumps(rep, indent=1))
    else:
        print_report(rep, args.level)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
