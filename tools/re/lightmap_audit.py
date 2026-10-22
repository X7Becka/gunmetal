#!/usr/bin/env python3
"""Покрытие лайтмапами и яркость страниц ``.sbc`` — аудит «тёмного» рендера (M4).

Инструмент отвечает на один вопрос: объясняется ли тёмный кадр уровня тем, что
часть граней вообще не имеет лайтмапа и рисуется с нулевой/крошечной
``brightness``, или же лайтмапы есть и просто тусклые.

Что считается (только чтение, ``gm/sbw.py`` + ``gm/sbc.py`` +
``gm/render.py::Lightmaps``):

* по уровню: секторы, объекты, грани; грани с лайтмапом (``poly.lightmap > 0``,
  что для ``LevelFace`` то же самое, что ``lightmap >= 0``) и без; грани с
  индексом страницы вне ``.sbc``; доля площади по числу вершин;
* по каждой странице ``.sbc``: размер, пиксели (min/max/mean), доля нулей, доля
  255, гистограмма по 4 бакетам 0..63/64..127/128..191/192..255;
* по граням БЕЗ лайтмапа — заполнена ли ``brightness`` (полигон +0x70,
  ``face.brightness``): распределение, доля нулей, средняя;
* сколько страниц реально никем не адресуется.

Соглашения (см. ``gm/sbc.py`` и ``gm/sbw.py``): ``+0x04`` — знаковый i16,
1-based, 0 = нет лайтмапа. ``gm/sbw.Poly.lightmap`` хранит u16 как есть, здесь
он приводится к i16. Движок складывает свет как
``clamp(poly.brightness + object[0x74] + level[0x16] + texel, -128, 127)``
(``0x43ca5a``..``0x43cc9c``), поэтому ``brightness`` сравнивается с типичным
текселем страницы.

Детерминизм: порядок уровней/страниц/бакетов фиксирован, средние округлены,
никакого времени/путей кроме указанных.

Использование::

    python3 tools/re/lightmap_audit.py                 # level0 подробно + таблица всех
    python3 tools/re/lightmap_audit.py --level dm1
    python3 tools/re/lightmap_audit.py --json > audit.json
    python3 tools/re/lightmap_audit.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

#: границы 4 бакетов по сырому байту 0..255 (``.sbc`` хранит 7-битные уровни,
#: поэтому бакеты 2..3 у поставочных файлов почти пусты — это и есть измерение)
BUCKETS = ((0, 63), (64, 127), (128, 191), (192, 255))

BUCKET_NAMES = ("0-63", "64-127", "128-191", "192-255")


def _s16(value):
    """``gm/sbw.py`` держит u16-биты; движок читает поле как i16."""
    return value - 0x10000 if value >= 0x8000 else value


def _frac(part, whole):
    return round(part / whole, 4) if whole else 0.0


# ---------------------------------------------------------------------------
# страницы .sbc
# ---------------------------------------------------------------------------

def page_stats(index, rec):
    """Статистика одной страницы (8-битные пиксели как они лежат на диске)."""
    px = rec.pixels
    n = len(px)
    hist = [0, 0, 0, 0]
    for v in px:
        for b, (lo, hi) in enumerate(BUCKETS):
            if lo <= v <= hi:
                hist[b] += 1
                break
    return {
        "index": index,
        "width": rec.width,
        "height": rec.height,
        "bytes": n,
        "min": min(px) if px else 0,
        "max": max(px) if px else 0,
        "mean": round(sum(px) / n, 3) if n else 0.0,
        "zeros": px.count(0),
        "zero_frac": _frac(px.count(0), n),
        "full": px.count(255),
        "full_frac": _frac(px.count(255), n),
        "buckets": dict(zip(BUCKET_NAMES, hist)),
        "bucket_frac": {k: _frac(v, n) for k, v in zip(BUCKET_NAMES, hist)},
    }


def pages_summary(pages):
    """Агрегат по всем страницам файла: пиксельно-взвешенные min/max/mean/доли."""
    total = sum(p["bytes"] for p in pages)
    hist = [0, 0, 0, 0]
    for p in pages:
        for b, name in enumerate(BUCKET_NAMES):
            hist[b] += p["buckets"][name]
    zeros = sum(p["zeros"] for p in pages)
    full = sum(p["full"] for p in pages)
    mean = (sum(p["mean"] * p["bytes"] for p in pages) / total) if total else 0.0
    sizes = sorted({(p["width"], p["height"]) for p in pages})
    return {
        "pages": len(pages),
        "bytes": total,
        "min": min((p["min"] for p in pages), default=0),
        "max": max((p["max"] for p in pages), default=0),
        "mean": round(mean, 3),
        "zeros": zeros,
        "zero_frac": _frac(zeros, total),
        "full": full,
        "full_frac": _frac(full, total),
        "buckets": dict(zip(BUCKET_NAMES, hist)),
        "bucket_frac": {k: _frac(v, total) for k, v in zip(BUCKET_NAMES, hist)},
        "sizes": len(sizes),
        "max_width": max((p["width"] for p in pages), default=0),
        "max_height": max((p["height"] for p in pages), default=0),
    }


# ---------------------------------------------------------------------------
# уровень
# ---------------------------------------------------------------------------

def _brightness_block(values):
    """Распределение ``brightness`` (+0x70) по набору граней."""
    from collections import Counter

    cnt = Counter(values)
    n = len(values)
    zeros = cnt.get(0, 0)
    nonzero = [v for v in values if v]
    return {
        "faces": n,
        "zeros": zeros,
        "zero_frac": _frac(zeros, n),
        "mean": round(sum(values) / n, 3) if n else 0.0,
        "mean_nonzero": round(sum(nonzero) / len(nonzero), 3) if nonzero else 0.0,
        "min": min(values) if values else 0,
        "max": max(values) if values else 0,
        "distinct": len(cnt),
        "hist": {str(k): cnt[k] for k in sorted(cnt)},
    }


def audit_level(name, sbw_path, sbc_path=None):
    """Полный аудит одного уровня. ``sbc_path=None`` — страниц нет."""
    from gm import render as rendermod
    from gm import sbc as sbcmod
    from gm import sbw as sbwmod

    world = sbwmod.parse_file(sbw_path)
    records = []
    sbc_magic = None
    sbc_bytes = 0
    if sbc_path and os.path.isfile(sbc_path):
        with open(sbc_path, "rb") as fh:
            blob = fh.read()
        sbc_bytes = len(blob)
        parsed = sbcmod.parse(blob)
        sbc_magic = parsed.magic.decode("latin1")
        records = list(parsed.records)
    pages = [page_stats(i, r) for i, r in enumerate(records)]
    summary = pages_summary(pages)

    # грани: LevelFace даёт 0-based индекс, свет выключен при lightmap -1
    lm = rendermod.Lightmaps(records)
    refs = [0] * len(pages)
    n_objects = 0
    n_objects_light = 0
    n_faces = 0
    with_lm = 0
    without_lm = 0
    negative_lm = 0
    out_of_range = 0
    verts_with = 0
    verts_without = 0
    bright_with = []
    bright_without = []
    lm_raw = {}

    for sector in world.sectors:
        for obj in sector.objects:
            n_objects += 1
            if obj.f74:
                n_objects_light += 1
            for poly in obj.polys:
                n_faces += 1
                face = rendermod.LevelFace(poly, "")
                blank = len(face.indices)
                signed = _s16(poly.lightmap)
                lm_raw[str(signed)] = lm_raw.get(str(signed), 0) + 1
                if signed < 0:
                    negative_lm += 1
                if face.lightmap >= 0:
                    with_lm += 1
                    verts_with += blank
                    bright_with.append(poly.brightness)
                    if face.lightmap < len(refs):
                        refs[face.lightmap] += 1
                    if lm.page(face) is None:
                        out_of_range += 1
                else:
                    without_lm += 1
                    verts_without += blank
                    bright_without.append(poly.brightness)

    total_verts = verts_with + verts_without
    referenced = sum(1 for r in refs if r)
    return {
        "level": name,
        "sbw": os.path.basename(sbw_path),
        "sbc": os.path.basename(sbc_path) if sbc_path else None,
        "sbc_present": bool(pages),
        "sbc_magic": sbc_magic,
        "sbc_bytes": sbc_bytes,
        "sectors": len(world.sectors),
        "objects": n_objects,
        "objects_aux_light_nonzero": n_objects_light,
        "faces": n_faces,
        "faces_with_lightmap": with_lm,
        "faces_without_lightmap": without_lm,
        "faces_lightmap_frac": _frac(with_lm, n_faces),
        "faces_lightmap_signed_negative": negative_lm,
        "faces_lightmap_out_of_range": out_of_range,
        "lightmap_values": {k: lm_raw[k] for k in sorted(lm_raw, key=int)},
        "vertices_with_lightmap": verts_with,
        "vertices_without_lightmap": verts_without,
        "vertex_area_frac_with_lightmap": _frac(verts_with, total_verts),
        "pages": summary,
        "pages_referenced": referenced,
        "pages_unreferenced": len(pages) - referenced,
        "brightness_all": _brightness_block(bright_with + bright_without),
        "brightness_with_lightmap": _brightness_block(bright_with),
        "brightness_without_lightmap": _brightness_block(bright_without),
        "page_stats": pages,
        "page_refs": refs,
    }


def paired_levels(game):
    """Все ``*.sbw``, у которых есть парный ``*.sbc`` (по имени, без учёта регистра).

    Возвращает список ``(stem, sbw_path, sbc_path)``, отсортированный по stem.
    """
    levels = {}
    for path in glob.glob(os.path.join(game, "*.sbw")):
        levels.setdefault(os.path.basename(path)[:-4].lower(), {})["sbw"] = path
    for path in glob.glob(os.path.join(game, "*.sbc")):
        levels.setdefault(os.path.basename(path)[:-4].lower(), {})["sbc"] = path
    out = []
    for stem in sorted(levels):
        item = levels[stem]
        if "sbw" in item and "sbc" in item:
            out.append((os.path.basename(item["sbw"])[:-4], item["sbw"], item["sbc"]))
    return out


def audit(game, level=None):
    """``level=None`` — все уровни с парным ``.sbc``; иначе один уровень."""
    if level:
        stem = os.path.basename(level)
        for ext in (".sbw", ".SBW"):
            if stem.endswith(ext):
                stem = stem[:-4]
        sbw_path = level if os.path.isfile(level) else os.path.join(game, stem + ".sbw")
        if not os.path.isfile(sbw_path):
            raise SystemExit("нет файла .sbw: %s" % sbw_path)
        sbc_path = os.path.splitext(sbw_path)[0] + ".sbc"
        if not os.path.isfile(sbc_path):
            sbc_path = None
        return {"game": game, "levels": [audit_level(stem, sbw_path, sbc_path)],
                "pairing": {"sbw": 1, "sbc": 1 if sbc_path else 0}}
    pairs = paired_levels(game)
    levels = [audit_level(stem, sbw_path, sbc_path) for stem, sbw_path, sbc_path in pairs]
    n_sbw = len(glob.glob(os.path.join(game, "*.sbw")))
    n_sbc = len(glob.glob(os.path.join(game, "*.sbc")))
    return {"game": game, "levels": levels,
            "pairing": {"sbw": n_sbw, "sbc": n_sbc, "paired": len(pairs)}}


def totals(result):
    """Сводка по всем уровням (пиксельно- и гранно-взвешенная)."""
    lv = result["levels"]
    faces = sum(x["faces"] for x in lv)
    with_lm = sum(x["faces_with_lightmap"] for x in lv)
    verts = sum(x["vertices_with_lightmap"] + x["vertices_without_lightmap"] for x in lv)
    verts_with = sum(x["vertices_with_lightmap"] for x in lv)
    px = sum(x["pages"]["bytes"] for x in lv)
    zeros = sum(x["pages"]["zeros"] for x in lv)
    full = sum(x["pages"]["full"] for x in lv)
    mean = sum(x["pages"]["mean"] * x["pages"]["bytes"] for x in lv) / px if px else 0.0
    bright_without = sum(x["brightness_without_lightmap"]["faces"] for x in lv)
    bright_without_zero = sum(x["brightness_without_lightmap"]["zeros"] for x in lv)
    bright_all = sum(x["brightness_all"]["faces"] for x in lv)
    bright_all_zero = sum(x["brightness_all"]["zeros"] for x in lv)
    return {
        "levels": len(lv),
        "faces": faces,
        "faces_with_lightmap": with_lm,
        "faces_lightmap_frac": _frac(with_lm, faces),
        "vertex_area_frac_with_lightmap": _frac(verts_with, verts),
        "pages": sum(x["pages"]["pages"] for x in lv),
        "pixels": px,
        "pixel_min": min((x["pages"]["min"] for x in lv), default=0),
        "pixel_max": max((x["pages"]["max"] for x in lv), default=0),
        "pixel_mean": round(mean, 3),
        "zero_frac": _frac(zeros, px),
        "full_frac": _frac(full, px),
        "brightness_all_faces": bright_all,
        "brightness_all_zeros": bright_all_zero,
        "brightness_all_zero_frac": _frac(bright_all_zero, bright_all),
        "brightness_without_lm_faces": bright_without,
        "brightness_without_lm_zeros": bright_without_zero,
        "brightness_without_lm_zero_frac": _frac(bright_without_zero, bright_without),
    }


# ---------------------------------------------------------------------------
# текстовый отчёт
# ---------------------------------------------------------------------------

def _pct(value, digits=1):
    return ("%." + str(digits) + "f%%") % (100.0 * value)


def print_level(lv, detailed=True):
    print("%s  (%s + %s)" % (lv["level"], lv["sbw"], lv["sbc"] or "нет .sbc"))
    print("  секторов/объектов/граней: %d/%d/%d"
          % (lv["sectors"], lv["objects"], lv["faces"]))
    print("  граней с лайтмапом: %d (%s), без: %d (%s)"
          % (lv["faces_with_lightmap"], _pct(lv["faces_lightmap_frac"]),
             lv["faces_without_lightmap"], _pct(1.0 - lv["faces_lightmap_frac"])))
    if lv["faces_lightmap_signed_negative"] or lv["faces_lightmap_out_of_range"]:
        print("  ! знаковых отрицательных lightmap: %d, вне диапазона страниц: %d"
              % (lv["faces_lightmap_signed_negative"],
                 lv["faces_lightmap_out_of_range"]))
    print("  площадь по вершинам с лайтмапом: %s (%d/%d вершинных ссылок)"
          % (_pct(lv["vertex_area_frac_with_lightmap"]),
             lv["vertices_with_lightmap"],
             lv["vertices_with_lightmap"] + lv["vertices_without_lightmap"]))
    print("  object+0x74 (добавка света объекта) != 0: %d из %d объектов"
          % (lv["objects_aux_light_nonzero"], lv["objects"]))

    pg = lv["pages"]
    print("  страниц: %d (%s), пикселей %d, размеров %d, максимум %dx%d, байт в файле %d"
          % (pg["pages"], lv["sbc_magic"] or "-", pg["bytes"], pg["sizes"],
             pg["max_width"], pg["max_height"], lv["sbc_bytes"]))
    if pg["pages"]:
        print("  пиксели: min %d, max %d, mean %.1f (%s от 127), нулей %s, 255: %s"
              % (pg["min"], pg["max"], pg["mean"], _pct(pg["mean"] / 127.0),
                 _pct(pg["zero_frac"]), _pct(pg["full_frac"])))
        print("  бакеты: " + ", ".join(
            "%s %s" % (name, _pct(pg["bucket_frac"][name]))
            for name in BUCKET_NAMES))
        print("  страниц используется / не используется: %d / %d"
              % (lv["pages_referenced"], lv["pages_unreferenced"]))

    for tag, key in (("все грани", "brightness_all"),
                     ("без лайтмапа", "brightness_without_lightmap"),
                     ("с лайтмапом", "brightness_with_lightmap")):
        b = lv[key]
        print("  brightness +0x70 (%s): %d граней, нулей %d (%s), mean %.1f, "
              "ненулевых mean %.1f, диапазон %d..%d, значений %d"
              % (tag, b["faces"], b["zeros"], _pct(b["zero_frac"]), b["mean"],
                 b["mean_nonzero"], b["min"], b["max"], b["distinct"]))
        if detailed and b["faces"]:
            print("    распределение: " + ", ".join(
                "%s:%d" % (k, v) for k, v in list(b["hist"].items())[:12]))

    if detailed and lv["page_stats"]:
        top = sorted(lv["page_stats"], key=lambda p: (-p["bytes"], p["index"]))[:8]
        print("  крупнейшие страницы (idx, w x h, байт, min/max/mean, нулей, 255):")
        for p in top:
            print("    %4d  %3dx%-3d %6d  %3d/%3d/%6.1f  %s  %s"
                  % (p["index"], p["width"], p["height"], p["bytes"],
                     p["min"], p["max"], p["mean"],
                     _pct(p["zero_frac"]), p["full"]))


def print_report(result, detail_level="level0"):
    lv = result["levels"]
    pairing = result["pairing"]
    print("gundx.exe 1.36 -- покрытие лайтмапами и яркость .sbc (M4)\n")
    print("Метод: poly+0x04 = 1-based i16 (0 = нет), poly+0x70 = brightness;")
    print("свет = clamp(brightness + obj[0x74] + sector[0x16] + texel, -128, 127),")
    print("texel -- байт страницы .sbc (0..127 у поставочных уровней).")
    print("Площадь оценивается суммой n_idx по граням (вершинные ссылки).\n")

    if len(lv) > 1:
        print("спаривание файлов: .sbw %d, .sbc %d, пар %d"
              % (pairing["sbw"], pairing["sbc"],
                 pairing.get("paired", pairing.get("sbw"))))
    detail = None
    for item in lv:
        if item["level"].lower() == detail_level.lower():
            detail = item
            break
    if detail is not None:
        print("\n-- подробно: %s --" % detail["level"])
        print_level(detail, detailed=True)
    if len(lv) > 1:
        print("\n-- все уровни с парным .sbc (%d) --" % len(lv))
        print("  %-9s %6s %6s %7s %7s %6s %6s %5s  %s"
              % ("уровень", "граней", "с lm", "доля", "площ.", "страниц",
                 "mean", "нули пикс.", "brightness=0 (без lm)"))
        for item in lv:
            no = item["brightness_without_lightmap"]
            print("  %-9s %6d %6d %6s %6s %6d %6.1f %6s  %5s (%d/%d)"
                  % (item["level"], item["faces"], item["faces_with_lightmap"],
                     _pct(item["faces_lightmap_frac"]),
                     _pct(item["vertex_area_frac_with_lightmap"]),
                     item["pages"]["pages"], item["pages"]["mean"],
                     _pct(item["pages"]["zero_frac"], 0),
                     _pct(no["zero_frac"], 0), no["zeros"], no["faces"]))
        t = totals(result)
        print("  %-9s %6d %6d %6s %6s %6d %6.1f %6s  %5s (%d/%d)"
              % ("ИТОГО", t["faces"], t["faces_with_lightmap"],
                 _pct(t["faces_lightmap_frac"]),
                 _pct(t["vertex_area_frac_with_lightmap"]),
                 t["pages"], t["pixel_mean"], _pct(t["zero_frac"], 0),
                 _pct(t["brightness_without_lm_zero_frac"], 0),
                 t["brightness_without_lm_zeros"], t["brightness_without_lm_faces"]))
    print()


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _check(what, ok, detail=""):
    print("  [%s] %s%s" % ("ok" if ok else "FAIL", what,
                           ("  -- " + detail) if detail else ""))
    return bool(ok)


def _synthetic_sbw():
    """1 сектор, 1 объект, 4 грани: 1-я и 3-я с лайтмапом, 2-я и 4-я без.

    Вершинные ссылки 3+4+3+4 = 14, с лайтмапом 3+3 = 6 (0.4286).  Значения
    brightness 32/0/16/0: у граней без лайтмапа одна нулевая.
    """
    from gm import sbw as sbwmod

    polys = []
    for lm, bright, n_idx in ((1, 32, 3), (0, 0, 4), (1, 16, 3), (0, 0, 4)):
        polys.append(sbwmod.Poly(
            texture=1, material=0, lightmap=lm, u_scale=1,
            centre=(0.0, 0.0, 0.0), normal=(0.0, 0.0, 1.0),
            u_axis=(1.0, 0.0, 0.0), v_axis=(0.0, 1.0, 0.0),
            corner_a=(0.0, 0.0, 0.0), corner_b=(1.0, 0.0, 0.0),
            uv_u=0, uv_v=0, uv_u_fp=0, uv_v_fp=0,
            scroll_u=0, anim0=0, scroll_v=0, anim1=0, v_scale=1, anim2=0,
            reserved=0, brightness=bright, indices=tuple(range(n_idx)),
            handle=0, f74=0, f76=0, overlay=0, f7a=0, flags=0x8000,
            raw=b"\0" * 112))
    header = b"".join(__import__("struct").pack("<d", v) for v in (
        0.0, 0.0, 0.0,
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0))
    obj = sbwmod.Object(id=0x10010, header=header, points=(),
                        n_verts=4, vertices=((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)),
                        raw_tail=b"\0" * 50,
                        polys=tuple(polys))
    world = sbwmod.Sbw(tex=[sbwmod.TexRef("wall", 0, 1)], effects=[],
                       sectors=[sbwmod.Sector(12, 1, [obj])])
    return world.to_bytes()


def _synthetic_sbc():
    """Две страницы: градиент 4x4 (0..15) и 2x2 из 255 (никем не адресуется)."""
    from gm import sbc as sbcmod

    return sbcmod.encode([sbcmod.Record(4, 4, bytes(range(16))),
                          sbcmod.Record(2, 2, b"\xff" * 4)])


def self_test(quiet=False):
    import tempfile

    ok = True
    tmpdir = tempfile.mkdtemp(prefix="lm-audit-")
    sbw_path = os.path.join(tmpdir, "synth.sbw")
    sbc_path = os.path.join(tmpdir, "synth.sbc")
    try:
        with open(sbw_path, "wb") as fh:
            fh.write(_synthetic_sbw())
        with open(sbc_path, "wb") as fh:
            fh.write(_synthetic_sbc())

        lv = audit_level("synth", sbw_path, sbc_path)
        print("self-test: синтетический уровень (4 грани, 2 страницы)")
        ok &= _check("1 сектор / 1 объект / 4 грани",
                     (lv["sectors"], lv["objects"], lv["faces"]) == (1, 1, 4),
                     "%d/%d/%d" % (lv["sectors"], lv["objects"], lv["faces"]))
        ok &= _check("грани с лайтмапом = 2, без = 2",
                     (lv["faces_with_lightmap"], lv["faces_without_lightmap"]) == (2, 2))
        ok &= _check("object+0x74 == 0 (свет только brightness + лайтмап)",
                     lv["objects_aux_light_nonzero"] == 0)
        ok &= _check("доля площади по вершинам = 6/14",
                     lv["vertices_with_lightmap"] == 6
                     and lv["vertex_area_frac_with_lightmap"] == round(6 / 14, 4),
                     str(lv["vertex_area_frac_with_lightmap"]))
        ok &= _check("brightness без лайтмапа: 2 грани, обе нулевые",
                     lv["brightness_without_lightmap"]["faces"] == 2
                     and lv["brightness_without_lightmap"]["zeros"] == 2)
        ok &= _check("brightness с лайтмапом: 32 и 16, нулей нет",
                     lv["brightness_with_lightmap"]["hist"] == {"16": 1, "32": 1})
        ok &= _check("свет не путает наборы (без lm: mean 0.0)",
                     lv["brightness_without_lightmap"]["mean"] == 0.0)

        pg = lv["pages"]
        print("\nself-test: синтетические страницы (.sbc)")
        ok &= _check("2 страницы, 20 пикселей, magic SBC0",
                     (pg["pages"], pg["bytes"], lv["sbc_magic"]) == (2, 20, "SBC0"),
                     "%d/%d/%s" % (pg["pages"], pg["bytes"], lv["sbc_magic"]))
        ok &= _check("min 0, max 255", (pg["min"], pg["max"]) == (0, 255),
                     "%d/%d" % (pg["min"], pg["max"]))
        ok &= _check("нулей 1 (градиент), 255: 4 (вторая страница)",
                     (pg["zeros"], pg["full"]) == (1, 4),
                     "%d/%d" % (pg["zeros"], pg["full"]))
        ok &= _check("mean = (0..15 + 4*255)/20 = 57.0", pg["mean"] == 57.0,
                     str(pg["mean"]))
        ok &= _check("бакеты 0-63: 16, 192-255: 4",
                     pg["buckets"]["0-63"] == 16 and pg["buckets"]["192-255"] == 4,
                     str(pg["buckets"]))
        ok &= _check("вторая страница не адресуется (1 из 2)",
                     lv["pages_referenced"] == 1 and lv["pages_unreferenced"] == 1,
                     "%d/%d" % (lv["pages_referenced"], lv["pages_unreferenced"]))
        ok &= _check("page_refs == [2, 0]", lv["page_refs"] == [2, 0], str(lv["page_refs"]))

        print("\nself-test: детерминизм и границы")
        again = audit_level("synth", sbw_path, sbc_path)
        ok &= _check("повторный прогон даёт тот же результат",
                     json.dumps(lv, sort_keys=True) == json.dumps(again, sort_keys=True))
        os.unlink(sbc_path)
        no_pages = audit_level("synth", sbw_path, None)
        ok &= _check("без .sbc: страниц 0, грани с lm вне диапазона",
                     no_pages["pages"]["pages"] == 0
                     and no_pages["faces_lightmap_out_of_range"] == 2
                     and no_pages["pages"]["mean"] == 0.0,
                     "out_of_range=%d" % no_pages["faces_lightmap_out_of_range"])
        ok &= _check("без .sbc: brightness-блоки не меняются",
                     no_pages["brightness_without_lightmap"]
                     == lv["brightness_without_lightmap"])
    finally:
        for path in (sbw_path, sbc_path):
            if os.path.exists(path):
                os.unlink(path)
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass

    print("\nself-test: %s" % ("успех" if ok else "ПРОВАЛ"))
    return ok


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--level", metavar="NAME",
                    help="только этот уровень (stem или путь к .sbw)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--json", action="store_true",
                    help="вывести результат в JSON вместо текста")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1

    try:
        result = audit(args.game, args.level)
    except (OSError, ValueError) as exc:
        print("lightmap_audit: %s" % exc, file=sys.stderr)
        return 1

    if args.json:
        data = dict(result)
        data["totals"] = totals(result)
        print(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=1))
    else:
        default_detail = os.path.basename(args.level) if args.level else "level0"
        default_detail = os.path.splitext(default_detail)[0]
        print_report(result, default_detail)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
