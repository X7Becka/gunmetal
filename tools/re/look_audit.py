#!/usr/bin/env python3
"""Аудит куба ``LOOK16.TAB`` / ``LOOK162.TAB``: что именно он делает со светом.

Инструмент отвечает на три вопроса (только чтение данных игры, без правок
рендера):

* **структура** — файл ровно ``262144`` Б = ``64**3``, одна ячейка = один байт,
  индекс ``(r << 12) | (g << 6) | b``, оси именно ``r/g/b`` (проверяется
  перебором всех шести перестановок осей: правильная даёт среднюю ошибку
  «цвет ячейки vs цвет палитры по метке» в разы меньше);
* **обратное отображение** — какие ячейки куба указывают на каждый индекс
  палитры (сколько их, какой RGB-объём они занимают): это и есть фактическое
  квантование 6-6-6 → индекс;
* **кривая света** — гипотеза «``L`` (0..127) → цвет ``c``, затем
  ``LOOK16[(c * k(L)) >> 2]``»: для набора множителей ``k`` строится таблица
  ``L → индекс → RGB(индекс)`` для серой рампы и для жёлтого цвета, и
  проверяется, какие формулы непротиворечивы (монотонность, точное попадание
  в исходный цвет при ``L = 127``, отсутствие насыщения раньше ``L = 127``).

Палитры: ``FRONTEND/GAMEPAL`` (из ``FRONTEND/BMP.IDX``) — для ``LOOK16.TAB``;
для ``LOOK162.TAB`` — только слоты, перечисленные в ``NEONS.PIN``.

Проверяемые множители ``k(L)`` (все считаются в 6-битных компонентах):

===========  =========================================================
``lin127``   ``c * L / 127`` (округление к ближайшему) — 8-битный путь
``lin127_6`` ``c6 * L / 127`` (сначала ``>> 2``, потом масштаб)
``lin64``    ``c * L / 64`` с насыщением на 63
``lin128``   ``c * (L + 1) / 128`` с насыщением на 63
``gamma``    ``c * (L / 127) ** 2`` (гамма-кривая)
===========  =========================================================

Использование::

    python3 tools/re/look_audit.py                     # полный аудит
    python3 tools/re/look_audit.py --json > look.json
    python3 tools/re/look_audit.py --level level0      # свет уровня + кривая
    python3 tools/re/look_audit.py --self-test

Детерминизм: выборки берутся фиксированным шагом, никакого времени/``random``;
порядок ключей в JSON фиксирован.

Метрика ядра ``0x4228e0`` (``(sr+0x100)*dr^2 + 512*dg^2 + (0x17f-sr)*db^2``,
где ``sr = pr + r``, все величины 6-битные) воспроизводит **оба куба точно**:
0 расхождений на 262144 ячейках, если кандидаты — метки куба
(``LOOK162.TAB`` — слоты ``NEONS.PIN``). Это и есть главная проверка
«ячейка = ближайший цвет палитры».
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

DIM = 64
SIZE = DIM ** 3
DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
MIRROR = os.path.expanduser("~/.cache/gunmetal-re/game")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

#: уровни света, для которых печатается таблица кривой
LEVELS = (0, 8, 16, 32, 64, 96, 127)

#: «серая рампа» и жёлтый материал (палитровые цвета 143/143/143 и 235/195/75)
SAMPLES = ((143, 143, 143), (235, 195, 75))

VARIANTS = ("lin127", "lin127_6", "lin64", "lin128", "gamma")

#: метрики ближайшего цвета для проверки «куб = ближайший цвет палитры»
METRICS = {
    "euclid6": lambda pr, pg, pb, r, g, b: (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2,
    "2-4-3": lambda pr, pg, pb, r, g, b: 2 * (pr - r) ** 2 + 4 * (pg - g) ** 2 + 3 * (pb - b) ** 2,
    "core0x4228e0": lambda pr, pg, pb, r, g, b: (
        (pr + r + 0x100) * (pr - r) ** 2 + 512 * (pg - g) ** 2
        + (0x17F - pr - r) * (pb - b) ** 2),
}


# ---------------------------------------------------------------------------
# данные
# ---------------------------------------------------------------------------

def find_game(game=None):
    """Каталог игры: явный аргумент, быстрое зеркало, затем установка."""
    for cand in (game, MIRROR, DEFAULT_GAME):
        if cand and os.path.isfile(os.path.join(cand, "LOOK16.TAB")):
            return cand
    raise FileNotFoundError("LOOK16.TAB не найден (--game DIR)")


def load_tables(game=None):
    """Палитра, два куба и список ``NEONS.PIN`` (только чтение)."""
    from gm import look as lookmod
    from gm import palette as palmod

    where = find_game(game)
    raw = palmod.installed(where)
    rgb = [(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]) for i in range(256)]
    neons_path = os.path.join(where, "NEONS.PIN")
    neons = lookmod.parse_neons(neons_path) if os.path.isfile(neons_path) else []
    return {
        "game": where,
        "palette_rgb": rgb,
        "look16": lookmod.load(os.path.join(where, "LOOK16.TAB")),
        "look162": lookmod.load(os.path.join(where, "LOOK162.TAB")),
        "neons": neons,
    }


def _cells(step=1):
    """Детерминированный обход куба: ``offset -> (r, g, b)``."""
    for off in range(0, SIZE, step):
        r, rem = off >> 12, off & 0xFFF
        yield off, r, rem >> 6, rem & 63


def _index_of_color(pal_rgb, color):
    """Индексы палитры с точным совпадением цвета (может быть несколько)."""
    return [i for i, c in enumerate(pal_rgb) if c == color]


# ---------------------------------------------------------------------------
# структура
# ---------------------------------------------------------------------------

def axis_order(data, pal_rgb, step=37):
    """Ошибка «цвет ячейки vs цвет палитры» для всех 6 перестановок осей.

    ``mean_err8_chroma`` считается только по хроматическим меткам
    (``max(r,g,b) - min(r,g,b) >= 8``): на серых цветах перестановка осей
    ничего не меняет, поэтому именно хроматика доказывает, какая ось где.
    """
    cells = [(r, g, b, data[off]) for off, r, g, b in _cells(step)]
    out = []
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        total = 0
        total_c = 0
        count_c = 0
        for r, g, b, idx in cells:
            pr, pg, pb = pal_rgb[idx]
            q = (r, g, b)
            err = (abs(pr - q[perm[0]] * 4) + abs(pg - q[perm[1]] * 4)
                   + abs(pb - q[perm[2]] * 4))
            total += err
            if max(pr, pg, pb) - min(pr, pg, pb) >= 8:
                total_c += err
                count_c += 1
        out.append({"perm": "".join("rgb"[p] for p in perm),
                    "mean_err8": round(total / (3.0 * len(cells)), 3),
                    "mean_err8_chroma": round(total_c / (3.0 * count_c), 3)
                    if count_c else None})
    out.sort(key=lambda row: row["mean_err8_chroma"])
    return out


def nearest_rank(data, pal_rgb, candidates=None, step=97):
    """Ранг выбранного кубом цвета среди кандидатов по простому евклиду."""
    cand = list(candidates) if candidates else list(range(256))
    colors = sorted({(pal_rgb[i][0] >> 2, pal_rgb[i][1] >> 2, pal_rgb[i][2] >> 2)
                     for i in cand})
    cells = [(r, g, b, data[off]) for off, r, g, b in _cells(step)]
    hist = {1: 0, 2: 0, 3: 0, "4+": 0}
    for r, g, b, idx in cells:
        q = (r, g, b)
        want = tuple(v >> 2 for v in pal_rgb[idx])
        ranked = sorted(((pr - q[0]) ** 2 + (pg - q[1]) ** 2 + (pb - q[2]) ** 2, c)
                        for c in colors for pr, pg, pb in (c,))
        rank = next(k + 1 for k, (dd, c) in enumerate(ranked) if c == want)
        hist[1 if rank == 1 else 2 if rank == 2 else 3 if rank == 3 else "4+"] += 1
    n = float(len(cells))
    return {"samples": len(cells),
            "rank1": round(hist[1] / n, 4),
            "rank2": round(hist[2] / n, 4),
            "rank3": round(hist[3] / n, 4),
            "rank4plus": round(hist["4+"] / n, 4)}


def metric_agreement(data, pal_rgb, candidates=None, step=53):
    """Доля ячеек, где куб совпал с ближайшим цветом под данной метрикой."""
    cand = list(candidates) if candidates else list(range(256))
    cells = [(r, g, b, data[off]) for off, r, g, b in _cells(step)]
    out = {}
    for name, fn in METRICS.items():
        ok = 0
        for r, g, b, idx in cells:
            best = min(cand, key=lambda i: (
                fn(pal_rgb[i][0] >> 2, pal_rgb[i][1] >> 2, pal_rgb[i][2] >> 2, r, g, b), i))
            ok += pal_rgb[best] == pal_rgb[idx]
        out[name] = round(ok / float(len(cells)), 4)
    return out


def voronoi_check(data, pal_rgb, candidates, step=1):
    """Точная проверка «куб = ближайший цвет по 0x4228e0» на всех ячейках.

    Кандидаты — индексы палитры, которые движок вообще рассматривает (для
    ``LOOK16.TAB`` это множество его собственных меток, для ``LOOK162.TAB`` —
    слоты ``NEONS.PIN``). Ошибок быть не должно: 0 из 262144.
    """
    c6 = [(c[0] >> 2, c[1] >> 2, c[2] >> 2) for c in pal_rgb]
    cand = list(candidates)
    bad = []
    mismatches = 0
    total = 0
    for off, r, g, b in _cells(step):
        total += 1
        idx = data[off]
        best = min(cand, key=lambda i: (
            (c6[i][0] + r + 0x100) * (c6[i][0] - r) ** 2
            + 512 * (c6[i][1] - g) ** 2
            + (0x17F - c6[i][0] - r) * (c6[i][2] - b) ** 2, i))
        if pal_rgb[best] != pal_rgb[idx]:
            mismatches += 1
            if len(bad) < 8:
                bad.append({"cell": [r, g, b], "table": idx, "nearest": best})
    return {"cells": total, "mismatches": mismatches, "candidates": len(cand),
            "exact": mismatches == 0, "examples": bad}


def components(data):
    """Число 4-связных компонент каждой метки (куб как разбиение Вороного)."""
    seen = bytearray(SIZE)
    comps = [0] * 256
    for start in range(SIZE):
        if seen[start]:
            continue
        label = data[start]
        comps[label] += 1
        stack = [start]
        seen[start] = 1
        while stack:
            pos = stack.pop()
            r = pos >> 12
            g = (pos >> 6) & 63
            b = pos & 63
            if r and not seen[pos - 4096] and data[pos - 4096] == label:
                seen[pos - 4096] = 1
                stack.append(pos - 4096)
            if r < 63 and not seen[pos + 4096] and data[pos + 4096] == label:
                seen[pos + 4096] = 1
                stack.append(pos + 4096)
            if g and not seen[pos - 64] and data[pos - 64] == label:
                seen[pos - 64] = 1
                stack.append(pos - 64)
            if g < 63 and not seen[pos + 64] and data[pos + 64] == label:
                seen[pos + 64] = 1
                stack.append(pos + 64)
            if b and not seen[pos - 1] and data[pos - 1] == label:
                seen[pos - 1] = 1
                stack.append(pos - 1)
            if b < 63 and not seen[pos + 1] and data[pos + 1] == label:
                seen[pos + 1] = 1
                stack.append(pos + 1)
    used = sorted(set(data))
    multi = [i for i in used if comps[i] > 1]
    return {"labels": len(used), "single": len(used) - len(multi),
            "multi": len(multi), "multi_labels": multi[:12]}


def structure(data, pal_rgb, candidates=None, heavy=True, full=True):
    axis = axis_order(data, pal_rgb)
    labels = sorted(set(data))
    report = {
        "size": len(data),
        "dim": DIM,
        "cells": SIZE,
        "entry_bytes": len(data) // SIZE if len(data) % SIZE == 0 else None,
        "distinct": len(labels),
        "min": min(data),
        "max": max(data),
        "labels": labels,
        "axis_order": axis,
        "axis_margin": round(axis[1]["mean_err8_chroma"] / axis[0]["mean_err8_chroma"], 2)
        if axis[0]["mean_err8_chroma"] else None,
        "nearest_rank": nearest_rank(data, pal_rgb, labels),
        "metric_agreement": metric_agreement(data, pal_rgb, labels),
        "voronoi": voronoi_check(data, pal_rgb,
                                 candidates if candidates else labels,
                                 step=1 if full else 13),
    }
    if heavy:
        report["components"] = components(data)
    return report


# ---------------------------------------------------------------------------
# обратное отображение: ячейки -> индекс палитры
# ---------------------------------------------------------------------------

def reverse_map(data, pal_rgb):
    """Для каждого слота палитры — сколько ячеек куба на него указывают."""
    cnt = [0] * 256
    sum6 = [[0, 0, 0] for _ in range(256)]
    box = [[64, -1, 64, -1, 64, -1] for _ in range(256)]
    for off in range(SIZE):
        idx = data[off]
        r, rem = off >> 12, off & 0xFFF
        g, b = rem >> 6, rem & 63
        cnt[idx] += 1
        s = sum6[idx]
        s[0] += r
        s[1] += g
        s[2] += b
        bx = box[idx]
        if r < bx[0]:
            bx[0] = r
        if r > bx[1]:
            bx[1] = r
        if g < bx[2]:
            bx[2] = g
        if g > bx[3]:
            bx[3] = g
        if b < bx[4]:
            bx[4] = b
        if b > bx[5]:
            bx[5] = b
    rows = []
    for i in range(256):
        n = cnt[i]
        row = {"index": i, "rgb": list(pal_rgb[i]), "cells": n,
               "share": round(n / float(SIZE), 6)}
        if n:
            row["centroid6"] = [round(sum6[i][k] / float(n), 2) for k in range(3)]
            row["box6"] = box[i]
        else:
            row["centroid6"] = None
            row["box6"] = None
        rows.append(row)
    used = [row for row in rows if row["cells"]]
    sizes = sorted(row["cells"] for row in used)
    return {
        "rows": rows,
        "used": len(used),
        "empty": [row["index"] for row in rows if not row["cells"]],
        "cells_min": sizes[0] if sizes else 0,
        "cells_median": sizes[len(sizes) // 2] if sizes else 0,
        "cells_max": sizes[-1] if sizes else 0,
        "largest": sorted(used, key=lambda row: -row["cells"])[:8],
    }


# ---------------------------------------------------------------------------
# кривая света
# ---------------------------------------------------------------------------

def scale6(rgb8, level, variant):
    """6-битный цвет после множителя ``k(level)`` по выбранному варианту."""
    level = max(0, min(127, int(level)))
    c6 = [c >> 2 for c in rgb8]
    if variant == "lin127":            # 8-битный путь: c*L/127, затем >>2
        return [min(63, (c * level + 254) // 508) for c in rgb8]
    if variant == "lin127_6":          # 6-битный путь: c6*L/127
        return [min(63, (c * level + 63) // 127) for c in c6]
    if variant == "lin64":
        return [min(63, c * level // 64) for c in c6]
    if variant == "lin128":
        return [min(63, c * (level + 1) // 128) for c in c6]
    if variant == "gamma":
        return [min(63, int(round(c * (level / 127.0) ** 2))) for c in c6]
    raise ValueError("unknown variant %r" % (variant,))


def light_curve(table, pal_rgb, color, variant, levels=tuple(range(128))):
    """``[(L, 6-bit запрос, индекс, RGB(индекс), люма)]``."""
    out = []
    for level in levels:
        q = scale6(color, level, variant)
        idx = table.at(*q)
        rgb = pal_rgb[idx]
        out.append({"L": level, "q6": q, "index": idx, "rgb": list(rgb),
                    "luma": round(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2], 2)})
    return out


def variant_score(table, pal_rgb, color, variant):
    """Насколько кривая варианта непротиворечива (монотонность, края, насыщение)."""
    curve = light_curve(table, pal_rgb, color, variant)
    lumas = [row["luma"] for row in curve]
    drops = sum(1 for a, b in zip(lumas, lumas[1:]) if b < a - 1e-9)
    idx = [row["index"] for row in curve]
    exact = _index_of_color(pal_rgb, color)
    first_exact = next((row["L"] for row in curve if row["index"] in exact), None)
    tail = 0
    for k in range(len(idx) - 1, 0, -1):
        if idx[k - 1] == idx[-1]:
            tail += 1
        else:
            break
    lin = light_curve(table, pal_rgb, color, "lin127")
    dev = [abs(curve[k]["rgb"][c] - lin[k]["rgb"][c])
           for k in range(len(curve)) for c in range(3)]
    return {
        "distinct_indices": len(set(idx)),
        "monotone": drops == 0,
        "luma_drops": drops,
        "endpoint_index": idx[-1],
        "endpoint_exact": idx[-1] in exact,
        "endpoint_rgb": curve[-1]["rgb"],
        "first_exact_L": first_exact,
        "tail_run": tail,
        "plateau_from": (127 - tail) if tail >= 8 else None,
        "dev_vs_lin127_mean": round(sum(dev) / float(len(dev)), 2),
        "dev_vs_lin127_max": max(dev),
        "curve": curve,
    }


def light_report(table, pal_rgb, colors=SAMPLES, levels=LEVELS):
    out = {"levels": list(levels), "colors": [list(c) for c in colors], "variants": {}}
    for variant in VARIANTS:
        per = {}
        for color in colors:
            score = variant_score(table, pal_rgb, color, variant)
            score["table"] = [row for row in score["curve"] if row["L"] in levels]
            per["%d,%d,%d" % color] = score
        out["variants"][variant] = per
    return out


# ---------------------------------------------------------------------------
# уровень: какие L реально встречаются (оценка brightness + лайтмап)
# ---------------------------------------------------------------------------

def _find_level_files(game, name):
    stem = os.path.splitext(os.path.basename(name))[0]
    sbw = sbc = None
    for path in glob.glob(os.path.join(game, "*.sbw")):
        if os.path.splitext(os.path.basename(path))[0].lower() == stem.lower():
            sbw = path
    for path in glob.glob(os.path.join(game, "*.sbc")):
        if os.path.splitext(os.path.basename(path))[0].lower() == stem.lower():
            sbc = path
    return stem, sbw, sbc


def level_report(game, name):
    """Распределение света по граням уровня (``brightness`` + среднее лайтмапа)."""
    from gm import render as rendermod
    from gm import sbc as sbcmod
    from gm import sbw as sbwmod

    stem, sbw_path, sbc_path = _find_level_files(game, name)
    if not sbw_path:
        raise FileNotFoundError("не найден %s.sbw в %s" % (stem, game))
    world = sbwmod.parse_file(sbw_path)
    records = sbcmod.parse_file(sbc_path).records if sbc_path else []
    lms = rendermod.Lightmaps(records)
    hist = [0] * 16
    values = []
    faces = with_lm = 0
    for sector in world.sectors:
        for obj in sector.objects:
            for poly in obj.polys:
                faces += 1
                face = rendermod.LevelFace(poly, "")
                base = int(getattr(poly, "brightness", 0))
                page = lms.page(face)
                if page is not None:
                    with_lm += 1
                    samples = []
                    for vi in face.indices[:8]:
                        uv = lms.uv(face, obj.vertices[vi])
                        if uv is None:
                            continue
                        u, v = uv
                        samples.append(page.pixels[v * page.width + u])
                    if samples:
                        base += int(round(sum(samples) / float(len(samples))))
                level = max(0, min(127, base))
                values.append(level)
                hist[level >> 3] += 1
    n = len(values) or 1
    ordered = sorted(values)
    return {
        "level": stem,
        "faces": faces,
        "with_lightmap": with_lm,
        "hist": {str(k * 8): hist[k] for k in range(16) if hist[k]},
        "min": ordered[0] if ordered else 0,
        "p25": ordered[len(ordered) // 4] if ordered else 0,
        "median": ordered[len(ordered) // 2] if ordered else 0,
        "p75": ordered[(3 * len(ordered)) // 4] if ordered else 0,
        "max": ordered[-1] if ordered else 0,
        "mean": round(sum(values) / float(n), 2),
        "distinct": len(set(values)),
    }


# ---------------------------------------------------------------------------
# вывод
# ---------------------------------------------------------------------------

def _row(fmt, *args):
    print(fmt % args if args else fmt)


def print_structure(tag, rep):
    _row("\n[%s] структура", tag)
    _row("  размер %d Б = %d^3, байт на ячейку %s, значений %d, диапазон %d..%d",
         rep["size"], rep["dim"], rep["entry_bytes"], rep["distinct"],
         rep["min"], rep["max"])
    _row("  оси (r<<12)|(g<<6)|b — средняя ошибка «ячейка vs палитра»:")
    for row in rep["axis_order"]:
        _row("    %-3s %7.2f  (хроматика %7.2f)", row["perm"], row["mean_err8"],
             row["mean_err8_chroma"] if row["mean_err8_chroma"] is not None else -1)
    _row("    запас правильной оси к следующей: x%.2f", rep["axis_margin"])
    rank = rep["nearest_rank"]
    _row("  ранг по простому евклиду (кандидаты = метки): ближайший %.1f%%"
         " (топ-2 %.1f%%, топ-3 %.1f%%, 4+ %.1f%%)",
         100 * rank["rank1"], 100 * (rank["rank1"] + rank["rank2"]),
         100 * (rank["rank1"] + rank["rank2"] + rank["rank3"]),
         100 * rank["rank4plus"])
    _row("  согласие с метриками (кандидаты = метки): %s",
         ", ".join("%s %.1f%%" % (k, 100 * v)
                   for k, v in sorted(rep["metric_agreement"].items())))
    vor = rep["voronoi"]
    _row("  Вороной по 0x4228e0: кандидатов %d, ошибок %d из %d (%s)",
         vor["candidates"], vor["mismatches"], vor["cells"],
         "куб воспроизведён" if vor["exact"] else "расхождение")
    if "components" in rep:
        comp = rep["components"]
        _row("  разбиение: метки %d, односвязных %d, многосвязных %d %s",
             comp["labels"], comp["single"], comp["multi"],
             comp["multi_labels"] if comp["multi"] else "")


def print_reverse(tag, rev, pal_rgb, neons=()):
    _row("\n[%s] обратное отображение (ячейки куба -> индекс палитры)", tag)
    _row("  занятых слотов %d, пустых %d, ячеек на слот: min %d / медиана %d / max %d",
         rev["used"], len(rev["empty"]), rev["cells_min"],
         rev["cells_median"], rev["cells_max"])
    _row("  %-5s %-7s %-7s %-9s %-24s %s", "инд", "ячеек", "доля", "RGB",
         "центроид 6-бит", "box6")
    for row in rev["largest"]:
        _row("  %-5d %-7d %-7s %-9s %-24s %s",
             row["index"], row["cells"], "%.2f%%" % (100 * row["share"]),
             "%d,%d,%d" % tuple(row["rgb"]),
             "%.1f,%.1f,%.1f" % tuple(row["centroid6"]),
             "%d..%d,%d..%d,%d..%d" % tuple(row["box6"]))
    _row("  пустые слоты: %s", rev["empty"])
    if rev["empty"]:
        dup = []
        for i in rev["empty"]:
            twin = next((j for j in range(256)
                         if j != i and pal_rgb[j] == pal_rgb[i]
                         and rev["rows"][j]["cells"]), None)
            if twin is not None:
                dup.append("%d->%d" % (i, twin))
        _row("  из них дубли цвета: %s; неоновые слоты NEONS.PIN: %d",
             ", ".join(dup) if dup else "-",
             sum(1 for i in rev["empty"] if i in set(neons)))


def print_light(tag, light):
    _row("\n[%s] кривая света: L -> RGB(индекс)", tag)
    for color in light["colors"]:
        key = "%d,%d,%d" % tuple(color)
        _row("  исходный цвет (%s):", key)
        _row("    %-9s %-5s %-5s %-7s %-9s %s", "вариант", "точек", "монот",
             "насыщ", "край L=127", "L -> индекс")
        for variant in VARIANTS:
            score = light["variants"][variant][key]
            cells = " ".join("%d:%d" % (row["L"], row["index"]) for row in score["table"])
            _row("    %-9s %-5d %-5s %-7s %-9s %s", variant,
                 score["distinct_indices"],
                 "да" if score["monotone"] else "нет",
                 str(score["plateau_from"]) if score["plateau_from"] else "-",
                 "точный" if score["endpoint_exact"] else "мимо", cells)
        _row("    отклонение от lin127 (среднее/макс, 8-бит): %s",
             ", ".join("%s %.2f/%d" % (variant,
                                       light["variants"][variant][key]["dev_vs_lin127_mean"],
                                       light["variants"][variant][key]["dev_vs_lin127_max"])
                       for variant in ("lin127_6", "lin64", "lin128", "gamma")))
        _row("    первое точное попадание в исходный цвет: %s",
             ", ".join("%s L=%s" % (variant,
                                    light["variants"][variant][key]["first_exact_L"])
                       for variant in VARIANTS))
    _row("\n  таблица рекомендованного варианта (lin127):")
    for color in light["colors"]:
        key = "%d,%d,%d" % tuple(color)
        score = light["variants"]["lin127"][key]
        _row("    (%s)", key)
        _row("      %-4s %-11s %-5s %-13s %s", "L", "6-бит", "инд", "RGB", "люма")
        for row in score["table"]:
            _row("      %-4d %-11s %-5d %-13s %.1f", row["L"],
                 "%d,%d,%d" % tuple(row["q6"]), row["index"],
                 "%d,%d,%d" % tuple(row["rgb"]), row["luma"])


def print_level(rep):
    _row("\n[%s] свет уровня: граней %d, с лайтмапом %d", rep["level"],
         rep["faces"], rep["with_lightmap"])
    _row("  L: min %d / p25 %d / медиана %d / p75 %d / max %d, среднее %.2f, разных %d",
         rep["min"], rep["p25"], rep["median"], rep["p75"], rep["max"],
         rep["mean"], rep["distinct"])
    _row("  гистограмма (шаг 8): %s",
         " ".join("%s:%d" % (k, v) for k, v in sorted(rep["hist"].items(), key=lambda kv: int(kv[0]))))


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _synthetic_palette():
    """Рампа серого как в GAMEPAL + цветные рампы: ось r/g/b различима."""
    grays = (0, 23, 35, 51, 63, 75, 87, 99, 111, 123, 139, 143, 151, 159,
             167, 175, 183, 191, 199, 207, 215, 223, 231, 239, 247, 255)
    pal = [(v, v, v) for v in grays]
    for v in (10, 20, 30, 40, 50, 63):
        pal.append((v * 4, v * 2, v))
        pal.append((v, v * 4, v * 2))
        pal.append((v * 2, v, v * 4))
    pal += [(255, 0, 0), (0, 255, 0), (0, 0, 255), (235, 195, 75)]
    pal += [(0, 0, 0)] * (256 - len(pal))
    return pal


def _synthetic_cube(pal_rgb):
    """Куб = ближайший цвет по метрике 0x4228e0 (как у настоящих таблиц)."""
    sites = []
    seen = set()
    for i, rgb in enumerate(pal_rgb):
        site = (rgb[0] >> 2, rgb[1] >> 2, rgb[2] >> 2)
        if site not in seen:
            seen.add(site)
            sites.append((site[0], site[1], site[2], i))
    data = bytearray(SIZE)
    for off in range(SIZE):
        r, rem = off >> 12, off & 0xFFF
        g, b = rem >> 6, rem & 63
        best_d = None
        best_i = 0
        for pr, pg, pb, i in sites:
            d = ((pr + r + 0x100) * (pr - r) ** 2 + 512 * (pg - g) ** 2
                 + (0x17F - pr - r) * (pb - b) ** 2)
            if best_d is None or d < best_d or (d == best_d and i < best_i):
                best_d = d
                best_i = i
        data[off] = best_i
    return bytes(data)


def self_test():
    from gm.look import LookTable

    pal = _synthetic_palette()
    data = _synthetic_cube(pal)
    table = LookTable(data, "synthetic")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    check("размер куба 262144", len(data) == SIZE)
    axis = axis_order(data, pal, step=11)
    check("ось r/g/b определяется", axis[0]["perm"] == "rgb",
          "%s %.2f vs %s %.2f" % (axis[0]["perm"], axis[0]["mean_err8_chroma"],
                                  axis[1]["perm"], axis[1]["mean_err8_chroma"]))
    check("запас правильной оси > x2",
          axis[1]["mean_err8_chroma"] > 2 * axis[0]["mean_err8_chroma"],
          "x%.1f" % (axis[1]["mean_err8_chroma"] / axis[0]["mean_err8_chroma"]))

    rev = reverse_map(data, pal)
    total = sum(row["cells"] for row in rev["rows"])
    check("обратное отображение покрывает 262144", total == SIZE, str(total))
    check("каждая метка имеет ячейки", rev["cells_min"] > 0,
          "used=%d" % rev["used"])

    labels = sorted(set(data))
    exact_vor = voronoi_check(data, pal, labels, step=3)
    check("куб = ближайший цвет по 0x4228e0", exact_vor["exact"],
          "%d битых из %d" % (exact_vor["mismatches"], exact_vor["cells"]))

    gray = (143, 143, 143)
    lin = [table.at(*scale6(gray, l, "lin127")) for l in range(128)]
    check("детерминизм (два прохода)",
          [table.at(*scale6(gray, l, "lin127")) for l in range(128)] == lin)
    lum = [0.299 * pal[i][0] + 0.587 * pal[i][1] + 0.114 * pal[i][2] for i in lin]
    check("L=0 — самый тёмный индекс", lum[0] == min(lum), "%d" % lin[0])
    check("L=127 — исходный цвет", pal[lin[-1]] == gray, str(pal[lin[-1]]))
    check("кривая монотонна по L", all(b >= a for a, b in zip(lum, lum[1:])))

    lin_score = variant_score(table, pal, gray, "lin127")
    g64 = variant_score(table, pal, gray, "lin64")
    gam = variant_score(table, pal, gray, "gamma")
    check("lin127 попадает в цвет только у верхней границы L", lin_score["first_exact_L"]
          is not None and lin_score["first_exact_L"] >= 120,
          str(lin_score["first_exact_L"]))
    check("lin64 насыщается заметно раньше lin127",
          g64["first_exact_L"] is not None
          and g64["first_exact_L"] < lin_score["first_exact_L"] - 20,
          "%s vs %s" % (g64["first_exact_L"], lin_score["first_exact_L"]))
    check("gamma отличается от lin127 на L=64", gam["dev_vs_lin127_mean"] > 1.0,
          "%.2f" % gam["dev_vs_lin127_mean"])
    check("lin128 на L=127 совпадает с lin127",
          table.at(*scale6(gray, 127, "lin128")) == lin[-1])

    print("self-test:")
    ok = True
    for name, passed, detail in checks:
        ok = ok and passed
        print("  [%s] %s%s" % ("ok" if passed else "FAIL", name,
                               (" (%s)" % detail) if detail else ""))
    print("self-test: %s" % ("успех" if ok else "ПРОВАЛ"))
    return ok


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="каталог установки (по умолчанию зеркало)")
    ap.add_argument("--json", action="store_true", help="вывести JSON")
    ap.add_argument("--level", metavar="NAME",
                    help="добавить распределение света уровня (stem .sbw)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--fast", action="store_true",
                    help="без проверки связности куба (быстро)")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1

    try:
        tables = load_tables(args.game)
    except (OSError, ValueError) as exc:
        print("look_audit: %s" % exc, file=sys.stderr)
        return 1

    pal = tables["palette_rgb"]
    look16 = tables["look16"]
    look162 = tables["look162"]
    neons = tables["neons"]
    neons_ok = set(look162.distinct()) <= set(neons) if neons else False

    level = None
    if args.level:
        try:
            level = level_report(tables["game"], args.level)
        except (OSError, ValueError) as exc:
            print("look_audit: %s" % exc, file=sys.stderr)
            return 1

    if level is not None and not args.json:
        print("look_audit: игра %s" % tables["game"])
        print_level(level)
        print_light("LOOK16.TAB", light_report(look16, pal))
        return 0

    full = not args.fast
    st16 = structure(look16.data, pal, heavy=full, full=full)
    st162 = structure(look162.data, pal, candidates=neons or None,
                      heavy=full, full=full)
    result = {"game": tables["game"], "neons": neons, "tables": {
        "LOOK16": {"structure": st16, "reverse": reverse_map(look16.data, pal),
                   "light": light_report(look16, pal)},
        "LOOK162": {"structure": st162, "reverse": reverse_map(look162.data, pal),
                    "neons_only": neons_ok},
    }}
    if level is not None:
        result["level"] = level

    if args.json:
        print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=1))
        return 0

    print("look_audit: игра %s" % tables["game"])
    print("палитра: FRONTEND/GAMEPAL, 256 слотов; NEONS.PIN: %d индексов" % len(neons))
    print_structure("LOOK16.TAB", st16)
    print_reverse("LOOK16.TAB", result["tables"]["LOOK16"]["reverse"], pal, neons)
    print_structure("LOOK162.TAB", st162)
    print("  значения LOOK162 ⊆ NEONS.PIN: %s" % ("да" if neons_ok else "НЕТ"))
    rev162 = result["tables"]["LOOK162"]["reverse"]
    print("  ячеек на слот: min %d / медиана %d / max %d, занято %d"
          % (rev162["cells_min"], rev162["cells_median"], rev162["cells_max"],
             rev162["used"]))
    for row in rev162["largest"][:3]:
        print("  %-5d %-7d %-7s RGB %-12s центроид %s"
              % (row["index"], row["cells"], "%.2f%%" % (100 * row["share"]),
                 "%d,%d,%d" % tuple(row["rgb"]),
                 "%.1f,%.1f,%.1f" % tuple(row["centroid6"])))
    print_light("LOOK16.TAB", result["tables"]["LOOK16"]["light"])
    if level is not None:
        print_level(level)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
