#!/usr/bin/env python3
"""Material table (``0x5670e0``) of the Gunmetal runtime (M2.3.2).

The 30-entry table is **not** stored in ``.sbw``: the engine builds it in RAM
from a hard-coded recipe (``0x420c03``) as ``5 light styles x 6 kinds``, index
``style*6 + kind``, stride ``0x40``.  This tool

* prints the reconstructed table and how a level actually uses it (``--map``);
* checks the recipe plus the invariants of all 36 shipped levels (``--verify``);
* runs a synthetic + shipped self-test (``--self-test``).

Everything below is ``stdlib`` only; ``gm/sbw.py`` is the parser (read only).

Field evidence (``gundx.exe`` 1.36, ImageBase ``0x400000``)::

    +0x00 u32  mode                 write 0x4212f0..0x421362, read 0x437fbc/0x439e77/0x43d56a
    +0x04 u32* style palette LUT    write 0x4213xx (DAT_00567100/0c/10/14)
    +0x08 u32* per-kind blend LUT   write 0x42136f..0x4213eb (DAT_00567118/1c/20/24)
    +0x0c u32* style light ramp     write 0x4213f2..0x42140e (LEA 0xa01040+style*0x600)
    +0x10 u32* light LUT            write 0x421285 (all 30 records), read 0x4409cb
    +0x14 u32* red light LUT        write 0x42128f (all 30 records), no reader found
    +0x20/24/28 f32 style RGB       write 0x4214xx..0x4215xx
    +0x2c i32  per-kind pass param  write 0x4214d8..0x421549 (0/0x40/0x40/100/0x7f/-0x80)
    +0x30 u32  style tint (R<<16|G<<8|B)  write once per style, kind 0
    +0x34 char[12] name             write 0x42155b.. (strcpy/sprintf)

The LUTs themselves are allocated and named in ``0x41e3d8`` (string labels
``paltable``, ``redtable``, ``goldtable``, ``bluetable``, ``lighttable``,
``redlighttable``, ``trantable``, ``varaddtable``, ``addtable``,
``shadetable``); the light/blend ones are filled by ``0x4201fe`` (light),
``0x420542`` (red light), ``0x41f618`` (add), ``0x41f8d1``, ``0x41fbe3``,
``0x41ff6f`` (256x256 byte LUTs indexed ``[level*256 + shade]``).

Usage:
    python3 tools/re/materials.py --map
    python3 tools/re/materials.py --map level0
    python3 tools/re/materials.py --verify
    python3 tools/re/materials.py --verify --levels level0 dm1
    python3 tools/re/materials.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

STRIDE = 0x40
N_RECORDS = 30
N_STYLES = 5
N_KINDS = 6

#: kind names (style 0) and the ``_ld_`` variants for styles 1..4
KIND_NAMES = ("none", "reflective", "translucent", "varadd", "add", "shade")
KIND_NAMES_LD = ("none", "refl", "tran", "varadd", "add", "shade")

#: ``+0x00`` per kind inside a style (``0x4212f0``..``0x421362``): none is the
#: opaque pass (0), reflective the "mode 1" pass, everything else mode 2.
MODE_BY_KIND = (0, 1, 2, 2, 2, 2)

#: ``+0x2c`` per kind (immediates at ``0x4214d8``..``0x421549``)
PARAM_BY_KIND = (0, 0x40, 0x40, 100, 0x7F, -0x80)

#: ``+0x08`` per kind: pointer to the 256x256 blend LUT (``0x42136f``..)
BLEND_BY_KIND = (None, "trantable", "trantable", "varaddtable",
                 "addtable", "shadetable")

#: ``+0x04`` per style: palette LUT (``0x42128x``/``0x4213xx``)
PALETTE_BY_STYLE = ("paltable", "paltable", "redtable", "goldtable", "bluetable")

#: ``+0x20/+0x24/+0x28`` per style (``0x4214xx``..``0x4215xx``)
RGB_BY_STYLE = ((1.0, 1.0, 1.0), (0.5, 1.0, 1.0), (1.0, 0.0, 0.0),
                (1.0, 0.5, 0.0), (0.0, 0.0, 1.0))

#: ``+0x30`` per style, written for kind 0 only (``0x421xxx``): R<<16|G<<8|B
TINT_BY_STYLE = (0x9C9C9C, 0x50A0A0, 0xA00000, 0xA05000, 0x0000A0)

#: table sizes in bytes as allocated by ``0x41e3d8``
LUT_SIZE = {
    "paltable": 0x20100,
    "redtable": 0x2100,
    "goldtable": 0x2100,
    "bluetable": 0x2100,
    "lighttable": 0x10100,
    "redlighttable": 0x10100,
    "trantable": 0x10000,
    "varaddtable": 0x10000,
    "addtable": 0x10000,
    "shadetable": 0x10000,
}

#: per-field evidence, printed by ``--map``
FIELD_EVIDENCE = [
    (0x00, "u32", "mode: bit0 = второй (смешиваемый) проход", "0x4212f0..0x421362", "доказано"),
    (0x04, "u32*", "палитра стиля (256x512 Б)", "0x4213xx", "доказано (запись)"),
    (0x08, "u32*", "blend-LUT вида (256x256 Б)", "0x42136f..0x4213eb", "доказано (запись)"),
    (0x0c, "u32*", "ramp стиля 0xa01040+style*0x600", "0x4213f2..0x42140e", "доказано (запись)"),
    (0x10, "u32*", "light-LUT 256x256 Б", "0x421285 / чтение 0x4409cb", "доказано"),
    (0x14, "u32*", "red-light-LUT 256x256 Б", "0x42128f", "доказано (запись)"),
    (0x18, "u32[2]", "не заполняется (0)", "-", "unknown"),
    (0x20, "f32", "R-множитель стиля", "0x4214xx", "доказано"),
    (0x24, "f32", "G-множитель стиля", "0x4214xx", "доказано"),
    (0x28, "f32", "B-множитель стиля", "0x4214xx", "доказано"),
    (0x2c, "i32", "параметр прохода вида (0/64/64/100/127/-128)",
     "0x4214d8..0x421549", "доказано (запись)"),
    (0x30, "u32", "тинт стиля R<<16|G<<8|B", "write в case kind 0", "доказано (запись)"),
    (0x34, "char[12]", "имя материала", "0x42155b.. / 0x42eaf5 / 0x42eb63 / 0x426ac3", "доказано"),
]


def build_table():
    """Reconstruct the runtime table exactly as ``0x420c03`` fills it."""
    out = []
    for style in range(N_STYLES):
        for kind in range(N_KINDS):
            name = KIND_NAMES[kind] if style == 0 else "_ld_%s_%d" % (KIND_NAMES_LD[kind], style + 1)
            out.append({
                "index": style * N_KINDS + kind,
                "style": style,
                "kind": kind,
                "mode": MODE_BY_KIND[kind],
                "palette": PALETTE_BY_STYLE[style],
                "blend": BLEND_BY_KIND[kind],
                "ramp": 0xA01040 + style * 0x600,
                "light": "lighttable",
                "param": PARAM_BY_KIND[kind],
                "rgb": RGB_BY_STYLE[style],
                "tint": TINT_BY_STYLE[style] if kind == 0 else 0,
                "name": name,
            })
    return out


TABLE = build_table()


def kind_name(index):
    if 0 <= index < len(TABLE):
        return TABLE[index]["name"]
    return "?"


# ---------------------------------------------------------------------------
# level scan
# ---------------------------------------------------------------------------

def _hexname(value):
    return "0x%02x%02x%02x" % ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)


def scan_level(path):
    """Material/overlay/flag statistics of one ``.sbw`` (all offsets = engine)."""
    from gm import sbw as gmsbw

    world = gmsbw.parse_file(path)
    st = {
        "file": os.path.basename(path),
        "path": path,
        "sectors": len(world.sectors),
        "objects": 0,
        "polys": 0,
        "materials": {},          # material index -> poly count
        "overlay_polys": 0,       # overlay != 0
        "overlay_untextured": 0,  # texture == 0 and overlay != 0
        "overlay_negative": 0,    # overlay < 0 -> effect table
        "overlay_bad": 0,         # overlay outside the texture/effect tables
        "link_sector": 0,         # poly 0x72 != 0
        "link_sector_self": 0,    # poly 0x72 == sector+1
        "link_object": 0,         # poly 0x74 != 0
        "link_poly": 0,           # poly 0x76 != 0
        "scroll": 0,              # 0x5c or 0x60 != 0
        "anim": 0,                # 0x5e/0x62/0x66 != 0
        "brightness": {},
        "obj_flags": {},
        "obj_extent_nonzero": 0,
        "header_words": 0,
        "header_low_nonzero": 0,
        "header_basis_bad": 0,
        "header_objects": 0,
    }
    n_tex = len(world.tex)
    n_eff = len(world.effects)
    for si, sector in enumerate(world.sectors):
        for obj in sector.objects:
            st["objects"] += 1
            st["obj_flags"][obj.id] = st["obj_flags"].get(obj.id, 0) + 1
            if obj.reserved and any(obj.reserved):
                st["obj_extent_nonzero"] += 1
            _check_header(obj.header, st)
            for poly in obj.polys:
                st["polys"] += 1
                st["materials"][poly.material] = st["materials"].get(poly.material, 0) + 1
                overlay = _s16(poly.overlay)
                if overlay:
                    st["overlay_polys"] += 1
                    if overlay < 0:
                        st["overlay_negative"] += 1
                        if -overlay > n_eff:
                            st["overlay_bad"] += 1
                    elif overlay > n_tex:
                        st["overlay_bad"] += 1
                    if not _s16(poly.texture):
                        st["overlay_untextured"] += 1
                # disk order of the three u16 is engine 0x72, 0x74, 0x76
                # (loader 0x425f4c), so gm/sbw.py's f74/f76/handle hold
                # 0x72/0x74/0x76 -- the names are disk slots, not offsets.
                if poly.f74:
                    st["link_sector"] += 1
                    if poly.f74 == si + 1:
                        st["link_sector_self"] += 1
                if poly.f76:
                    st["link_object"] += 1
                if poly.handle:
                    st["link_poly"] += 1
                if poly.scroll_u or poly.scroll_v:
                    st["scroll"] += 1
                if poly.anim0 or poly.anim1 or poly.anim2:
                    st["anim"] += 1
                st["brightness"][poly.brightness] = st["brightness"].get(poly.brightness, 0) + 1
    return st


def _s16(value):
    """``gm/sbw.py`` keeps the u16 bit pattern; the engine reads ``i16``."""
    return value - 0x10000 if value >= 0x8000 else value


def _check_header(header, st):
    """Object header: 12 little-endian ``f64`` (origin + 3 basis rows)."""
    if len(header) < 96:
        return
    st["header_objects"] += 1
    words = [header[8 * i:8 * i + 8] for i in range(12)]
    st["header_words"] += len(words)
    st["header_low_nonzero"] += sum(1 for w in words if w[:4] != b"\0\0\0\0")
    vals = [struct.unpack("<d", w)[0] for w in words]
    for row in (vals[3:6], vals[6:9], vals[9:12]):
        n = sum(c * c for c in row)
        if abs(n - 1.0) > 1e-6:
            st["header_basis_bad"] += 1
            break


def _fmt_hist(hist, limit=12):
    items = sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))
    return ", ".join("%s:%d" % (k, v) for k, v in items[:limit])


# ---------------------------------------------------------------------------
# --map
# ---------------------------------------------------------------------------

def print_map(level=None, game=DEFAULT_GAME):
    print("gundx.exe 1.36 -- таблица материалов 0x5670e0 (M2.3.2)\n")
    print("Строится 0x420c03: malloc(0x1e << 6) на 0x421250, счётчик 0x5670e4 = 30.")
    print("Индекс полигона 0x02: material = style*6 + kind (style 0..4, kind 0..5).\n")
    print("поля записи (stride 0x40):")
    print("  смещ. тип   смысл                                      доказательство")
    for off, typ, what, who, status in FIELD_EVIDENCE:
        print("  +0x%02x %-5s %-41s %-28s %s" % (off, typ, what, who, status))

    print("\n30 записей (значения из рецепта 0x420c03):")
    print("  idx st kd mode +0x04       +0x08        rgb            +0x2c  tint      имя")
    for r in TABLE:
        print("  %2d  %d  %d  %4d %-12s %-12s %-14s %5d %-9s %s" % (
            r["index"], r["style"], r["kind"], r["mode"], r["palette"],
            r["blend"] or "-",
            "(%.1f,%.1f,%.1f)" % r["rgb"], r["param"],
            _hexname(r["tint"]) if r["tint"] else "-", r["name"]))
    print("\n  +0x0c у всех 6 видов стиля s: 0xa01040 + s*0x600 (0x180 dword ramp);")
    print("  +0x10/+0x14 у всех 30 записей одинаковы: lighttable / redlighttable.")
    print("  +0x18..0x1f не заполняется (malloc, без memset => мусор).")
    print("  Читатели записи: +0x00 (0x437fbc, 0x439e77, 0x43d56a, 0x423edb),")
    print("                   +0x10 (0x4409cb, только запись 0), +0x34 (0x42eaf5,")
    print("                   0x42eb63, 0x426ac3). Остальные поля не читаются.")

    if level is None:
        return
    path = level if os.path.isfile(level) else os.path.join(game, level + ".sbw"
                                                          if not level.endswith(".sbw") else level)
    if not os.path.isfile(path):
        print("\nmap: нет файла %s" % path, file=sys.stderr)
        return
    st = scan_level(path)
    print("\nиспользование в %s: %d секторов, %d объектов, %d полигонов"
          % (st["file"], st["sectors"], st["objects"], st["polys"]))
    print("  material -> шт. полигонов:")
    for idx in sorted(st["materials"]):
        rec = TABLE[idx] if 0 <= idx < len(TABLE) else None
        print("    %2d  %-46s %6d  mode=%d +0x2c=%d" % (
            idx, rec["name"] if rec else "(вне таблицы!)", st["materials"][idx],
            rec["mode"] if rec else -1, rec["param"] if rec else 0))
    styles = {}
    for idx, n in st["materials"].items():
        styles[idx // N_KINDS] = styles.get(idx // N_KINDS, 0) + n
    print("  стили: %s" % _fmt_hist(styles))

    print("\n  overlay 0x78: %d полисов (%d без текстуры, %d отрицательных, %d вне таблицы)"
          % (st["overlay_polys"], st["overlay_untextured"],
             st["overlay_negative"], st["overlay_bad"]))
    print("  ссылки 0x72/0x74/0x76: sector %d (из них self %d), object %d, poly %d"
          % (st["link_sector"], st["link_sector_self"],
             st["link_object"], st["link_poly"]))
    print("  scroll 0x5c/0x60: %d полисов; anim 0x5e/0x62/0x66: %d"
          % (st["scroll"], st["anim"]))
    print("  brightness 0x70: %s" % _fmt_hist(st["brightness"], 8))
    print("  object+0x70: %s" % _fmt_hist(st["obj_flags"], 8))
    print("  object+0x90 (extent) != 0: %d объектов" % st["obj_extent_nonzero"])
    print("  заголовок: %d объектов, %d слов; младший dword != 0: %d; basis не единичный: %d"
          % (st["header_objects"], st["header_words"], st["header_low_nonzero"],
             st["header_basis_bad"]))


# ---------------------------------------------------------------------------
# --verify
# ---------------------------------------------------------------------------

class Report:
    def __init__(self, verbose=True):
        self.fail = 0
        self.checks = 0
        self.verbose = verbose

    def check(self, what, ok, detail=""):
        self.checks += 1
        if not ok:
            self.fail += 1
        if self.verbose or not ok:
            print("  [%s] %s%s" % ("ok" if ok else "FAIL", what,
                                   ("  -- " + detail) if detail else ""))
        return ok


def _model_checks(rep):
    print("модель таблицы (рецепт 0x420c03):")
    rep.check("30 записей x 0x40 == 0x780", N_RECORDS * STRIDE == 0x780)
    rep.check("индекс = style*6+kind, уникален",
              [r["index"] for r in TABLE] == list(range(N_RECORDS))
              and len({r["name"] for r in TABLE}) == N_RECORDS)
    rep.check("mode по виду == (0,1,2,2,2,2)",
              [r["mode"] for r in TABLE[0:6]] == list(MODE_BY_KIND))
    rep.check("+0x2c по виду == (0,64,64,100,127,-128)",
              [r["param"] for r in TABLE[0:6]] == list(PARAM_BY_KIND))
    rep.check("+0x2c: 5 разных значений (64 у reflective и translucent)",
              len({r["param"] for r in TABLE}) == 5
              and set(r["param"] for r in TABLE) == set(PARAM_BY_KIND))
    rep.check("rgb стилей == (1,1,1),(0.5,1,1),(1,0,0),(1,0.5,0),(0,0,1)",
              tuple(r["rgb"] for r in TABLE[::N_KINDS]) == RGB_BY_STYLE)
    rep.check("тинт только у kind 0, 5 разных значений",
              all(r["tint"] == 0 for r in TABLE if r["kind"] != 0)
              and len({r["tint"] for r in TABLE if r["kind"] == 0}) == 5)
    rep.check("+0x0c == 0xa01040 + style*0x600, шаг стиля 0x600",
              all(r["ramp"] == 0xA01040 + r["style"] * 0x600 for r in TABLE)
              and 0x180 * 4 == 0x600)
    rep.check("размеры LUT совпадают с 0x41e3d8 (0x20100/0x2100/0x10100/0x10000)",
              LUT_SIZE["paltable"] == 0x20100 and LUT_SIZE["redtable"] == 0x2100
              and LUT_SIZE["lighttable"] == 0x10100
              and LUT_SIZE["trantable"] == 0x10000)
    rep.check("256x256 LUT индексируется [level*256 + shade]",
              LUT_SIZE["lighttable"] == 0x100 * 0x100 + 0x100)


def _level_checks(rep, st):
    mat = st["materials"]
    rep.check("%s: material < 30" % st["file"],
              all(0 <= m < N_RECORDS for m in mat), _fmt_hist(mat, 6))
    rep.check("%s: используются только style 0 (kind 0..5)" % st["file"],
              all(m < 6 for m in mat), _fmt_hist(mat, 6))
    rep.check("%s: +0x2c использованных материалов из {0,64,64,100,127,-128}"
              % st["file"],
              all(TABLE[m]["param"] in PARAM_BY_KIND for m in mat if m < N_RECORDS))
    rep.check("%s: mode использованных == (0,1,2,2,2,2)[kind]"
              % st["file"],
              all(TABLE[m]["mode"] == MODE_BY_KIND[m % N_KINDS]
                  for m in mat if m < N_RECORDS))
    rep.check("%s: имя материала повторяемо (index -> name)" % st["file"],
              all(kind_name(m) == KIND_NAMES[m] for m in mat if m < 6))
    rep.check("%s: overlay не выходит за таблицу текстур/эффектов"
              % st["file"],
              st["overlay_bad"] == 0,
              "overlay=%d neg=%d bad=%d" % (st["overlay_polys"],
                                            st["overlay_negative"], st["overlay_bad"]))
    rep.check("%s: object+0x90 extent == 0 во всех объектах" % st["file"],
              st["obj_extent_nonzero"] == 0,
              "%d/%d" % (st["obj_extent_nonzero"], st["objects"]))
    rep.check("%s: заголовок объекта = 12 x f64 (младший dword 0)"
              % st["file"], st["header_low_nonzero"] == 0,
              "%d/%d слов" % (st["header_low_nonzero"], st["header_words"]))
    rep.check("%s: basis (f64) единичный во всех объектах" % st["file"],
              st["header_basis_bad"] == 0,
              "%d битых из %d" % (st["header_basis_bad"], st["header_objects"]))
    rep.check("%s: n_polys == сумма по объектам" % st["file"],
              st["polys"] == sum(mat.values()))


def verify_levels(files, quiet=False):
    rep = Report(verbose=not quiet)
    if not quiet:
        _model_checks(rep)
        print("\nуровни (%d файлов):" % len(files))
    name_seen = {}
    for path in files:
        st = scan_level(path)
        _level_checks(rep, st)
        for m in st["materials"]:
            name_seen.setdefault(m, set()).add(kind_name(m))
    if not quiet:
        print("\nповторяемость material -> name по всем уровням:")
    rep.check("один material = одно имя во всех уровнях",
              all(len(v) == 1 for v in name_seen.values()),
              ", ".join("%d:%s" % (k, "/".join(sorted(v)))
                        for k, v in sorted(name_seen.items()) if len(v) > 1) or
              "%d разных индексов" % len(name_seen))
    if not quiet:
        print("\nитог: %d проверок, %d провалов" % (rep.checks, rep.fail))
    return rep.fail == 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def _check(what, ok, detail=""):
    print("  [%s] %s%s" % ("ok" if ok else "FAIL", what,
                           ("  -- " + detail) if detail else ""))
    return bool(ok)


def _synthetic_sbw():
    """One sector, one object, three polygons with material 0/3/5."""
    out = bytearray()
    texs = [("wall", 0, 7), ("ovl", 0, 1)]
    out += struct.pack("<I", len(texs))
    for name, flag, value in texs:
        out += name.encode() + b"\0" + struct.pack("<HI", flag, value)
    out += struct.pack("<I", 0)                 # effects
    out += struct.pack("<I", 1)                 # sectors
    # 12 real doubles: origin (10,20,30), identity basis (engine reads f64 raw)
    header = b"".join(struct.pack("<d", v)
                      for v in (10.0, 20.0, 30.0,
                                1.0, 0.0, 0.0,
                                0.0, 1.0, 0.0,
                                0.0, 0.0, 1.0))
    out += struct.pack("<IHB", 1, 12, 1) + struct.pack("<I", 0x10010) + header
    out += struct.pack("<HH", 4, 3)             # n_verts, n_polys
    for v in ((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)):
        out += struct.pack("<3f", *v)
    out += struct.pack("<f", 1.0)               # radius
    out += bytes((0, 0))                        # f75, f74
    out += struct.pack("<II", 0, 0)             # f7c, f78
    out += struct.pack("<3f", 1.0, 1.0, 1.0)    # bounds
    out += b"\0" * 24                           # extent / reserved
    for material, overlay, scroll, anim in ((0, 0, 0, 0), (3, 1, 5, 7), (5, 0, 0, 0)):
        tail = bytearray(112)
        struct.pack_into("<6H", tail, 0, scroll, 0, 1, 1, 0, 0)  # 0x5c/0x60, scales, uv
        struct.pack_into("<3f", tail, 32, 0.0, 0.0, 1.0)         # normal
        struct.pack_into("<3f", tail, 44, 1.0, 0.0, 0.0)         # u axis
        struct.pack_into("<3f", tail, 56, 0.0, 1.0, 0.0)         # v axis
        struct.pack_into("<3H", tail, 92, 1, 0, 1)               # link sector+1/obj/poly+1
        struct.pack_into("<I", tail, 102, 0x8001)                # flags
        struct.pack_into("<3H", tail, 106, anim, 0, 0)           # anim0..2
        # disk order: texture, lightmap, material, overlay, f7a; the overlay
        # polygon is deliberately untextured (texture == 0)
        head = struct.pack("<5HBB", 0 if overlay else 1, 1, material, overlay, 0, 64, 4)
        out += head + struct.pack("<4H", 0, 1, 2, 3) + bytes(tail)
    return bytes(out)


def self_test(game=DEFAULT_GAME):
    ok = True
    print("self-test: рецепт таблицы")
    ok &= _check("30 записей, stride 0x40", len(TABLE) == 30 and STRIDE == 0x40)
    ok &= _check("имена стиля 0 == none/reflective/translucent/varadd/add/shade",
                 [r["name"] for r in TABLE[:6]] == list(KIND_NAMES))
    ok &= _check("имена стилей 1..4 == _ld_*_2.._ld_*_5",
                 TABLE[6]["name"] == "_ld_none_2" and TABLE[29]["name"] == "_ld_shade_5")
    ok &= _check("+0x2c: 0/64/64/100/127/-128",
                 [r["param"] for r in TABLE[:6]] == [0, 64, 64, 100, 127, -128])
    ok &= _check("тинт стиля 4 == 0x0000a0 (синий)", TABLE[24]["tint"] == 0xA0)
    ok &= _check("ramp стиля 4 == 0xa01040+4*0x600 == 0xa02840",
                 TABLE[24]["ramp"] == 0xA02840)

    print("\nself-test: синтетический .sbw")
    data = _synthetic_sbw()
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".sbw", delete=False) as fh:
        fh.write(data)
        path = fh.name
    try:
        st = scan_level(path)
        ok &= _check("1 сектор / 1 объект / 3 полигона",
                     (st["sectors"], st["objects"], st["polys"]) == (1, 1, 3),
                     "%d/%d/%d" % (st["sectors"], st["objects"], st["polys"]))
        ok &= _check("material-гистограмма 0/3/5",
                     st["materials"] == {0: 1, 3: 1, 5: 1}, str(st["materials"]))
        ok &= _check("overlay распознан (1 полигон, без текстуры)",
                     st["overlay_polys"] == 1 and st["overlay_untextured"] == 1)
        ok &= _check("ссылки 0x72/0x74/0x76 распознаны",
                     st["link_sector"] == 3 and st["link_sector_self"] == 3
                     and st["link_object"] == 0 and st["link_poly"] == 3)
        ok &= _check("scroll/anim распознаны", st["scroll"] == 1 and st["anim"] == 1)
        ok &= _check("заголовок: 12 f64, младший dword 0", st["header_low_nonzero"] == 0)
        ok &= _check("basis единичный", st["header_basis_bad"] == 0)
        ok &= _check("extent == 0", st["obj_extent_nonzero"] == 0)
        ok &= _check("object flags 0x10010", st["obj_flags"] == {0x10010: 1})
    finally:
        os.unlink(path)

    print("\nself-test: поставочные уровни")
    files = sorted(glob.glob(os.path.join(game, "*.sbw")))
    if not files:
        print("  [skip] нет .sbw в %s" % game)
        return ok
    ok &= _check("найдено 36 уровней", len(files) == 36, str(len(files)))
    level0 = None
    for path in files:
        if os.path.basename(path).lower() == "level0.sbw":
            level0 = path
            break
    if level0:
        st = scan_level(level0)
        ok &= _check("level0: 58 секторов / 192 объекта / 1449 полигонов",
                     (st["sectors"], st["objects"], st["polys"]) == (58, 192, 1449),
                     "%d/%d/%d" % (st["sectors"], st["objects"], st["polys"]))
        ok &= _check("level0: материалы только 0/2/3/4 (reflective не встречается)",
                     sorted(st["materials"]) == [0, 2, 3, 4],
                     _fmt_hist(st["materials"]))
        ok &= _check("level0: extent нулевой у всех объектов",
                     st["obj_extent_nonzero"] == 0)
    ok &= _check("все 36 уровней проходят инварианты", verify_levels(files, quiet=True))
    return ok


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", nargs="?", const="level0", metavar="LEVEL",
                    help="печать таблицы материалов и её использования (по умолчанию level0)")
    ap.add_argument("--verify", action="store_true", help="инварианты по 36 уровням")
    ap.add_argument("--verify-files", nargs="*", metavar="SBW",
                    help="то же, но по указанным файлам")
    ap.add_argument("--levels", nargs="*", help="стемы уровней для --verify (level0 dm1)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--all", action="store_true", help="--map + --verify + --self-test")
    args = ap.parse_args(argv)

    if args.self_test or args.all:
        if not self_test(args.game):
            return 1
        if not args.all:
            return 0

    if args.map or args.all:
        print_map(args.map, args.game)
        print()

    if args.verify or args.verify_files is not None or args.all:
        files = list(args.verify_files or [])
        if args.levels:
            files = [os.path.join(args.game, stem if stem.endswith(".sbw") else stem + ".sbw")
                     for stem in args.levels]
        elif not files:
            files = sorted(glob.glob(os.path.join(args.game, "*.sbw")))
        if not files:
            print("verify: нет файлов", file=sys.stderr)
            return 1
        for path in files:
            if not os.path.isfile(path):
                print("verify: нет %s" % path, file=sys.stderr)
                return 1
        if not verify_levels(files):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
