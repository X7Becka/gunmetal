#!/usr/bin/env python3
"""Инвентарь кокпита (HUD) для уровня: элементы, ассеты, координаты, поля (M4.4).

Инструмент отвечает на вопрос «что именно рисует кокпит и от чего зависит
каждое число», чтобы потом нарисовать его в Python и сравнивать полный кадр:

* статическая таблица элементов кокпита ``level0`` — координаты в 320x200,
  ассет (``FRONTEND/BMP.IDX`` / атлас иконок / шрифт), что показывается и из
  какого поля состояния (``docs/PLAYER_FIELDS.md``), с адресом кода, где это
  видно (`gundx.exe`);
* измерение по эталонным кадрам ``out/oracle/win{1,2,3}.png`` (только чтение):
  маска «пиксель не меняется между кадрами» в нижней трети — это и есть
  статичные элементы кокпита; кластеры маски дают bounding box'ы, которые
  сверяются с таблицей;
* яркая часть маски (текст/цифры/шкалы) отделяется от чёрного фона, поэтому
  кластеры читаются как отдельные элементы, а не как один чёрный блок.

Что важно про кадры: игра рисует 320x200, dgVoodoo растягивает до 1920x1080
(6.0 по X, 5.4 по Y) с фильтрацией, поэтому кадры приводятся к 320x200
усреднением по блокам (``downscale``) — это детерминированно и не зависит от
`PIL`. Палитра кадра — игровая (``FRONTEND/GAMEPAL``), а не палитра BMP,
поэтому побитовое сравнение с ассетом не используется: совпадение проверяется
по геометрии (bounding box'ы), а не по цвету пикселя.

Использование::

    python3 tools/re/hud_audit.py --level level0
    python3 tools/re/hud_audit.py --level level0 --json > /var/tmp/hud.json
    python3 tools/re/hud_audit.py --self-test
    python3 tools/re/hud_audit.py --frames /path/to/oracle --threshold 18

Кадры ищутся по порядку: ``--frames``, ``$GUNMETAL_ORACLE``, ``out/oracle``
текущего дерева и ``out/oracle`` основного репозитория (через
``git rev-parse --git-common-dir``; в git-worktree кадры лежат там). Если
кадров нет, инструмент печатает только инвентарь и честно пишет
``frames: not found``.

Ничего не пишется: игра, кадры и репозиторий открываются только на чтение.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import png  # noqa: E402

#: Логический экран игры: всё, что ниже, задано в этих координатах.
SCREEN_W = 320
SCREEN_H = 200

#: Полоса кокпита: ``PANEL`` — 320x78, нижние 78 строк кадра.
PANEL_BAND = (0, 122, 320, 78)

#: Нижняя треть кадра — область, где вообще может быть кокпит.
BAND_Y = 120

#: Порог «пиксель не изменился» по максимальному каналу (0..255).
STATIC_THRESHOLD = 14

#: Пиксель считается «ярким» (текст/шкала/цифра), если max(R,G,B) >= этого.
BRIGHT_THRESHOLD = 70

#: Кластер меньше этого числа пикселей игнорируется (шум фильтрации).
MIN_CLUSTER = 12

FRAME_NAMES = ("win1.png", "win2.png", "win3.png")

#: Статическая таблица кокпита. Поля:
#:   id, kind, rect, asset, value, field, status, evidence
#:
#: ``status``: ``proven`` — координата/смысл доказаны кодом (`gundx.exe`) или
#: совпадением с ассетом; ``hypothesis`` — вывод по кадру/докам без xref.
#: Прямоугольники — измеренные bbox'ы в 320x200 (см. ``docs/HUD_AUDIT.md``).
ELEMENTS = (
    dict(
        id="panel", kind="panel", rect=(0, 122, 320, 78),
        asset="FRONTEND/BMP.IDX:PANEL (320x78, ключ — индекс 0)",
        value="фон кокпита целиком",
        field="-",
        status="proven",
        evidence="высота 78 = 200-122; гексагон DAMAGE и полосы LED совпадают "
                 "с кадром (overlay); блит панели — 0x491xxx рядом с 0x4926a0",
    ),
    dict(
        id="led_top_left", kind="led", rect=(13, 129, 33, 8),
        asset="PANEL (арт, индексы LED)",
        value="индикаторы слева сверху", field="-",
        status="proven",
        evidence="PANEL: яркие пиксели y=8..14, x=13..45 -> кадр 129..137",
    ),
    dict(
        id="led_top_right", kind="led", rect=(195, 129, 39, 8),
        asset="PANEL (арт, индексы LED)",
        value="индикаторы по центру сверху", field="-",
        status="proven",
        evidence="PANEL: яркие пиксели y=8..14, x=195..233",
    ),
    dict(
        id="led_top_far", kind="led", rect=(246, 131, 56, 6),
        asset="PANEL (арт, индексы LED)",
        value="индикаторы справа сверху", field="-",
        status="proven",
        evidence="PANEL: яркие пиксели y=10..14, x=246..301",
    ),
    dict(
        id="led_left_column", kind="led", rect=(11, 162, 6, 21),
        asset="PANEL (арт) + BLINK0..3 для анимации",
        value="столбик индикаторов слева", field="-",
        status="proven",
        evidence="стабильный кластер кадра x=11..16, y=163..182; "
                 "BLINK0..3 грузятся HUD-загрузчиком (строки 0x4be2xx)",
    ),
    dict(
        id="led_bottom", kind="led", rect=(13, 192, 290, 8),
        asset="PANEL (арт, индексы LED)",
        value="нижняя полоса индикаторов", field="-",
        status="proven",
        evidence="стабильные кластеры кадра y=194..199",
    ),
    dict(
        id="label_weapon", kind="label", rect=(22, 154, 62, 8),
        asset="FONTS1/FONTS4 (глифы 8x8, шрифт 6 у 0x47faf0)",
        value="имя оружия («MACHINE GUN»; «NO WEAPON» если слота нет)",
        field="weapon_slot +0x3294 -> запись оружия 0x80FC78 + slot*0x2A8",
        status="proven",
        evidence="0x49291e/0x492b60: строка оружия рисуется на x=0x17, y=0xa2; "
                 "кадр: 22..83 x 155..160",
    ),
    dict(
        id="icon_weapon", kind="icon", rect=(21, 161, 28, 27),
        asset="атлас иконок 24x24 (WCON*/WICON* в FRONTEND/BMP.IDX)",
        value="иконка текущего оружия",
        field="weapon_slot +0x3294 (id иконки — из записи оружия)",
        status="hypothesis",
        evidence="в кадре — растровая вставка 26x25 с рамкой; конкретная "
                 "запись атласа не сопоставлена",
    ),
    dict(
        id="value_ammo", kind="value", rect=(49, 162, 36, 11),
        asset="NUMFONTS (180x48), шрифт 10 (красные 8x10)",
        value="патроны (в кадре 1999)",
        field="ammo[weapon_slot] +0x329C, делитель типа патрона 0x818980",
        status="proven",
        evidence="0x4929cd: ammo[slot] / table[7*ammo_type*8+0x818980]; "
                 "0x492a80 рисует шрифтом 10; кадр x=50..83, y=163..172",
    ),
    dict(
        id="label_kill", kind="label", rect=(51, 178, 18, 7),
        asset="FONTS1/FONTS4 (8x8)",
        value="«KILL» — подпись счётчика",
        field="-",
        status="proven",
        evidence="0x49289f: push 'KILL' (0x4be5e0), x=0x34, y=0xba",
    ),
    dict(
        id="value_kill", kind="value", rect=(72, 178, 8, 10),
        asset="NUMFONTS, шрифт 10 (красные 8x10)",
        value="счётчик убийств (в кадре 0)",
        field="? счётчик игрока (поля в PLAYER_FIELDS.md не найдено)",
        status="hypothesis",
        evidence="в кадре одна красная цифра справа от KILL; "
                 "формат '%d' (0x4be5f0) в 0x4926a0, источник не локализован",
    ),
    dict(
        id="plate_damage", kind="panel", rect=(136, 139, 45, 53),
        asset="PANEL (арт, индекс 114 — зелёный гексагон)",
        value="рамка индикатора урона", field="-",
        status="proven",
        evidence="PANEL: индекс 114 ограничен x=124..190, y=5..66; "
                 "кадр: зелёный bbox 136..180 x 139..191",
    ),
    dict(
        id="label_damage", kind="label", rect=(139, 142, 37, 7),
        asset="FONTS1/FONTS4 (8x8)",
        value="«DAMAGE»",
        field="-",
        status="proven",
        evidence="0x4928c6: push 'DAMAGE' (0x4be5d8), x=0x8d, y=0x96",
    ),
    dict(
        id="bar_damage_top", kind="gauge", rect=(146, 150, 21, 3),
        asset="PANEL (арт) + заливка 0xf3 (красный 243)",
        value="верхняя шкала гексагона",
        field="charge_accumulated +0x2FA8 (гипотеза: шкала заряда оружия)",
        status="hypothesis",
        evidence="0x492ab5..0x492b12: ftol(+0x2FA8*const), заливка 0xf3; "
                 "координаты в буфере панели не восстановлены",
    ),
    dict(
        id="icon_damage", kind="icon", rect=(149, 153, 14, 19),
        asset="PANEL (арт, силуэт машины)",
        value="силуэт танка в гексагоне", field="-",
        status="proven",
        evidence="PANEL индекс 117/128 в области гексагона; кадр 150..162 x 154..171",
    ),
    dict(
        id="bar_damage_bottom", kind="gauge", rect=(147, 176, 20, 3),
        asset="PANEL (арт) + заливка",
        value="нижняя шкала гексагона",
        field="charge_accumulated +0x2FA8 (гипотеза)",
        status="hypothesis",
        evidence="вторая полоса в гексагоне; та же заливка, что и верхняя",
    ),
    dict(
        id="value_damage", kind="value", rect=(155, 180, 21, 11),
        asset="NUMFONTS, шрифт 11/12 (зелёные 8x8) + «%» из FONTS",
        value="урон в процентах (в кадре 0 %)", 
        field="100 - 100*armor/armor_max; armor +0x3248, max +0x3244",
        status="proven",
        evidence="0x4926d6: fild [+0x3248], fmul 100, fidiv [+0x3244], "
                 "fsubr 100; выбор шрифта по порогам 100/70 (0x49270e..0x4927b3)",
    ),
    dict(
        id="label_throttle", kind="label", rect=(203, 152, 48, 8),
        asset="FONTS1/FONTS4 (8x8)",
        value="«THROTTLE»",
        field="-",
        status="proven",
        evidence="0x492fbf: push 'THROTTLE' (0x4be5a0)",
    ),
    dict(
        id="bar_throttle", kind="gauge", rect=(201, 155, 55, 4),
        asset="заливка прямоугольником (палитра 243 = красный), не спрайт",
        value="тяга: красная заливка длиной 0..53 px",
        field="engine_power +0x2F48 (0..1); flags +0x3200 бит 0x2 = полная",
        status="proven",
        evidence="0x492ef8: test [flags]&2 -> полная; иначе ftol(+0x2F48*const), "
                 "clamp 0x35 (53); три строки memset 0xf3 (0x492f40..0x492f7f)",
    ),
    dict(
        id="label_headlights", kind="label", rect=(226, 160, 54, 7),
        asset="FONTS1/FONTS4 (8x8)",
        value="имя экипировки («HEADLIGHTS»)",
        field="equipment index +0x3320 -> таблица имён 0x814C8C + idx*0x40; "
              "вкл/выкл — бит active_mask +0x3328 -> 0x68cd9c",
        status="proven",
        evidence="0x492b53: 0x814c8c + idx*0x40, формат '%s' (0x4a6874); "
                 "0x4919f0 ставит 0x68cd9c по биту [+0x3328]; "
                 "поле +0x3320 в PLAYER_FIELDS.md помечено как fire_anim",
    ),
    dict(
        id="icon_equipment", kind="icon", rect=(199, 161, 28, 27),
        asset="атлас иконок 24x24 (WICON*)",
        value="иконка экипировки (в кадре — «фара»/детектор)",
        field="equipment index +0x3320",
        status="hypothesis",
        evidence="растровая вставка 26x25 в кадре; запись атласа не сопоставлена",
    ),
    dict(
        id="value_equipment", kind="value", rect=(245, 172, 10, 11),
        asset="NUMFONTS, шрифт 10/11",
        value="число экипировки (в кадре 0)",
        field="? (рядом с именем экипировки)",
        status="hypothesis",
        evidence="красная цифра в правой панели; в 0x4930xx '%d' после меток",
    ),
    dict(
        id="compass", kind="compass", rect=(76, 0, 54, 20),
        asset="динамическая лента курса (не BMP.IDX: рисуется по камере)",
        value="курс/азимут (меняется между win1..win3)",
        field="yaw +0x0028 (камера/корпус)",
        status="hypothesis",
        evidence="в статической маске остаётся только чёрная рамка; "
                 "тики/буквы W/N сдвигаются с поворотом (win1 yaw=4.82, win2 6.01)",
    ),
)

VALID_STATUS = ("proven", "hypothesis")
VALID_KINDS = ("panel", "label", "value", "gauge", "icon", "led", "compass")


# --------------------------------------------------------------------------
# изображения
# --------------------------------------------------------------------------
def load_rgb(path):
    """Читает PNG через ``gm/png.py`` и отдаёт ``(w, h, bytes RGB)``."""
    with open(path, "rb") as fh:
        img = png.decode(fh.read())
    w, h = img.width, img.height
    px = img.pixels
    out = bytearray(w * h * 3)
    if img.mode == "P":
        pal = img.palette
        for i, idx in enumerate(px):
            out[3 * i] = pal[4 * idx]
            out[3 * i + 1] = pal[4 * idx + 1]
            out[3 * i + 2] = pal[4 * idx + 2]
    elif img.mode == "RGB":
        out[:] = px
    elif img.mode == "RGBA":
        for i in range(w * h):
            out[3 * i : 3 * i + 3] = px[4 * i : 4 * i + 3]
    elif img.mode == "L":
        for i, v in enumerate(px):
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    elif img.mode == "LA":
        for i in range(w * h):
            v = px[2 * i]
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    else:  # pragma: no cover - png.decode уже отбрасывает прочие режимы
        raise ValueError("unsupported PNG mode %r" % (img.mode,))
    return w, h, bytes(out)


def downscale(img, width=SCREEN_W, height=SCREEN_H):
    """Приводит кадр к 320x200 усреднением по блокам (без PIL).

    Границы блока считаются целочисленно: ``x0 = x*W//width``,
    ``x1 = (x+1)*W//width`` — при 1920x1080 это ровно 6x5/6 строк, то есть
    обратное преобразование к тому, что делает апскейл dgVoodoo. Результат
    детерминирован и не зависит от фильтра исходного растяжения.
    """
    sw, sh, px = img
    out = bytearray(width * height * 3)
    for y in range(height):
        y0 = y * sh // height
        y1 = max(y0 + 1, (y + 1) * sh // height)
        if y1 > sh:
            y1 = sh
        for x in range(width):
            x0 = x * sw // width
            x1 = max(x0 + 1, (x + 1) * sw // width)
            if x1 > sw:
                x1 = sw
            r = g = b = n = 0
            for yy in range(y0, y1):
                base = (yy * sw) * 3
                for xx in range(x0, x1):
                    i = base + xx * 3
                    r += px[i]
                    g += px[i + 1]
                    b += px[i + 2]
                    n += 1
            o = (y * width + x) * 3
            out[o] = r // n
            out[o + 1] = g // n
            out[o + 2] = b // n
    return bytes(out)


def static_mask(frames, threshold=STATIC_THRESHOLD):
    """Маска «пиксель одинаков во всех кадрах» по максимальному каналу."""
    if not frames:
        raise ValueError("static_mask: no frames")
    n = len(frames[0]) // 3
    for f in frames:
        if len(f) != n * 3:
            raise ValueError("static_mask: frames differ in size")
    mask = bytearray(n)
    for i in range(n):
        j = i * 3
        static = True
        for a in range(len(frames)):
            for b in range(a + 1, len(frames)):
                fa, fb = frames[a], frames[b]
                d = fa[j] - fb[j]
                if d < 0:
                    d = -d
                for k in (1, 2):
                    dk = fa[j + k] - fb[j + k]
                    if dk < 0:
                        dk = -dk
                    if dk > d:
                        d = dk
                if d > threshold:
                    static = False
                    break
            if not static:
                break
        if static:
            mask[i] = 1
    return mask


def bright_mask(rgb, threshold=BRIGHT_THRESHOLD):
    """Пиксели, у которых max(R,G,B) >= threshold."""
    n = len(rgb) // 3
    mask = bytearray(n)
    for i in range(n):
        j = i * 3
        if max(rgb[j], rgb[j + 1], rgb[j + 2]) >= threshold:
            mask[i] = 1
    return mask


def _dilate(mask, bridge_x, bridge_y, width=SCREEN_W, height=SCREEN_H):
    """Расширяет маску на ``bridge_x``/``bridge_y`` пикселей (8-связность)."""
    if bridge_x <= 0 and bridge_y <= 0:
        return bytearray(mask)
    out = bytearray(mask)
    for y in range(height):
        row = y * width
        for x in range(width):
            if not mask[row + x]:
                continue
            for dy in range(-bridge_y, bridge_y + 1):
                yy = y + dy
                if not (0 <= yy < height):
                    continue
                base = yy * width
                for dx in range(-bridge_x, bridge_x + 1):
                    xx = x + dx
                    if 0 <= xx < width:
                        out[base + xx] = 1
    return out


def clusters(mask, bridge_x=1, bridge_y=1, min_area=MIN_CLUSTER,
             width=SCREEN_W, height=SCREEN_H):
    """Связные компоненты маски; bbox считается по исходным (не расширенным).

    Возвращает список словарей ``{pixels,x,y,w,h,cx,cy}``, отсортированный
    по (y, x) — порядок детерминирован.
    """
    grown = _dilate(mask, bridge_x, bridge_y, width, height)
    seen = bytearray(width * height)
    out = []
    for start in range(width * height):
        if not grown[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        xs = []
        ys = []
        pixels = 0
        while stack:
            cur = stack.pop()
            cy, cx = divmod(cur, width)
            if mask[cur]:
                pixels += 1
                xs.append(cx)
                ys.append(cy)
            for dy in (-1, 0, 1):
                yy = cy + dy
                if not (0 <= yy < height):
                    continue
                base = yy * width
                for dx in (-1, 0, 1):
                    xx = cx + dx
                    if not (0 <= xx < width):
                        continue
                    idx = base + xx
                    if grown[idx] and not seen[idx]:
                        seen[idx] = 1
                        stack.append(idx)
        if pixels < min_area or not xs:
            continue
        x0, x1 = min(xs), max(xs)
        y0, y1 = min(ys), max(ys)
        out.append(dict(pixels=pixels, x=x0, y=y0,
                        w=x1 - x0 + 1, h=y1 - y0 + 1,
                        cx=(x0 + x1) // 2, cy=(y0 + y1) // 2))
    out.sort(key=lambda c: (c["y"], c["x"]))
    return out


def _overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    ix = min(ax + aw, bx + bw) - max(ax, bx)
    iy = min(ay + ah, by + bh) - max(ay, by)
    if ix <= 0 or iy <= 0:
        return 0
    return ix * iy


def match_elements(cl, include_panel=True):
    """Какие элементы таблицы пересекает кластер (по площади пересечения).

    ``include_panel=False`` отбрасывает фон ``panel``: он покрывает всю полосу
    и иначе «матчится» с любым кластером, скрывая неопознанные элементы.
    """
    rect = (cl["x"], cl["y"], cl["w"], cl["h"])
    hits = []
    for el in ELEMENTS:
        if not include_panel and el["id"] == "panel":
            continue
        area = _overlap(rect, el["rect"])
        if area:
            hits.append((area, el["id"]))
    hits.sort(reverse=True)
    return [name for _, name in hits]


def element_checks(static, bright):
    """Проверка каждого элемента по маскам: доля статичных/ярко-статичных px."""
    out = []
    for el in ELEMENTS:
        x, y, w, h = el["rect"]
        inside = 0
        static_px = 0
        bright_px = 0
        for yy in range(y, min(y + h, SCREEN_H)):
            base = yy * SCREEN_W
            for xx in range(x, min(x + w, SCREEN_W)):
                idx = base + xx
                inside += 1
                if static[idx]:
                    static_px += 1
                    if bright[idx]:
                        bright_px += 1
        out.append(dict(
            id=el["id"],
            rect=el["rect"],
            pixels=inside,
            static=static_px,
            static_bright=bright_px,
            static_frac=round(static_px / inside, 3) if inside else 0.0,
            bright_frac=round(bright_px / inside, 3) if inside else 0.0,
        ))
    return out


# --------------------------------------------------------------------------
# поиск кадров
# --------------------------------------------------------------------------
def _git_common_root():
    """Каталог основного репозитория (из git-worktree) или ``None``."""
    try:
        res = subprocess.run(
            ["git", "rev-parse", "--git-common-dir"],
            cwd=ROOT, capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if res.returncode != 0:
        return None
    path = res.stdout.strip()
    if not path:
        return None
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    path = os.path.abspath(path)
    if os.path.basename(path) == ".git":
        path = os.path.dirname(path)
    return path


def find_frames_dir(explicit=None):
    """Ищет каталог с ``win1..win3.png``; возвращает (dir, source) или (None, why)."""
    candidates = []
    if explicit:
        candidates.append((explicit, "--frames"))
    env = os.environ.get("GUNMETAL_ORACLE")
    if env:
        candidates.append((env, "$GUNMETAL_ORACLE"))
    candidates.append((os.path.join(ROOT, "out", "oracle"), "out/oracle"))
    common = _git_common_root()
    if common:
        candidates.append((os.path.join(common, "out", "oracle"),
                           "main worktree out/oracle"))
    for path, source in candidates:
        if all(os.path.exists(os.path.join(path, n)) for n in FRAME_NAMES):
            return os.path.abspath(path), source
    return None, "no win1..win3.png in %s" % ", ".join(p for p, _ in candidates)


def load_frames(frames_dir):
    imgs = []
    for name in FRAME_NAMES:
        w, h, rgb = load_rgb(os.path.join(frames_dir, name))
        imgs.append((name, w, h, downscale((w, h, rgb))))
    sizes = {(w, h) for _, w, h, _ in imgs}
    if len(sizes) != 1:
        raise ValueError("frames differ in size: %r" % (sorted(sizes),))
    return imgs


# --------------------------------------------------------------------------
# отчёт
# --------------------------------------------------------------------------
def audit(level="level0", frames_dir=None, threshold=STATIC_THRESHOLD,
          bright=BRIGHT_THRESHOLD):
    """Собирает результат: инвентарь + (если есть кадры) маску и кластеры."""
    result = dict(
        level=level,
        screen=[SCREEN_W, SCREEN_H],
        panel_band=list(PANEL_BAND),
        band_y=BAND_Y,
        threshold=threshold,
        bright_threshold=bright,
        elements=[dict(el, rect=list(el["rect"])) for el in ELEMENTS],
        frames=None,
        mask=None,
        clusters=[],
        outside_band=[],
        checks=[],
        unclassified=[],
    )
    if frames_dir is None:
        frames_dir, why = find_frames_dir()
    elif not all(os.path.exists(os.path.join(frames_dir, n)) for n in FRAME_NAMES):
        why = "no %s in %s" % ("/".join(FRAME_NAMES), frames_dir)
        frames_dir = None
    else:
        why = ""
    if frames_dir is None:
        result["frames"] = dict(found=False, note=why)
        return result
    imgs = load_frames(frames_dir)
    w, h = imgs[0][1], imgs[0][2]
    result["frames"] = dict(
        found=True, dir=os.path.abspath(frames_dir),
        files=[n for n, _, _, _ in imgs], size=[w, h],
        downscale=[SCREEN_W, SCREEN_H],
    )
    static = static_mask([f for _, _, _, f in imgs], threshold)
    brightm = bright_mask(imgs[0][3], bright)
    band = bytearray(SCREEN_W * SCREEN_H)
    for y in range(BAND_Y, SCREEN_H):
        band[y * SCREEN_W : (y + 1) * SCREEN_W] = \
            static[y * SCREEN_W : (y + 1) * SCREEN_W]
    static_bright = bytearray(SCREEN_W * SCREEN_H)
    for i in range(SCREEN_W * SCREEN_H):
        if static[i] and brightm[i]:
            static_bright[i] = 1
    cl = clusters(static_bright)
    checks = element_checks(static, brightm)
    in_band = []
    outside = []
    unclassified = []
    for c in cl:
        names = match_elements(c, include_panel=False)
        entry = dict(c, elements=names, on_panel=bool(match_elements(c)),
                     in_band=bool(c["y"] >= BAND_Y))
        if entry["in_band"]:
            in_band.append(entry)
            if not names:
                unclassified.append(entry)
        else:
            outside.append(entry)
    result["mask"] = dict(
        static_total=sum(static),
        static_band=sum(band),
        static_outside=sum(static) - sum(band),
        static_bright=sum(static_bright),
    )
    result["clusters"] = in_band
    result["outside_band"] = outside
    result["unclassified"] = unclassified
    result["checks"] = checks
    result["totals"] = dict(
        elements=len(ELEMENTS),
        proven=sum(1 for e in ELEMENTS if e["status"] == "proven"),
        hypothesis=sum(1 for e in ELEMENTS if e["status"] == "hypothesis"),
        clusters=len(in_band),
        matched=sum(1 for c in in_band if c["elements"]),
        unclassified=len(unclassified),
        outside_band=len(outside),
    )
    return result


def _fmt_rect(r):
    return "(%d,%d %dx%d)" % (r[0], r[1], r[2], r[3])


def print_report(res):
    level = res["level"]
    print("hud_audit: %s — инвентарь кокпита (320x200)" % level)
    fr = res["frames"]
    if not fr or not fr.get("found"):
        note = fr.get("note") if fr else "frames not found"
        print("  frames: not found (%s)" % note)
        print("  анализ кадров пропущен; инвентарь ниже — по коду и ассетам")
    else:
        print("  frames: %s (%s, %dx%d -> 320x200)"
              % (fr["dir"], ", ".join(fr["files"]), fr["size"][0], fr["size"][1]))
        m = res["mask"]
        print("  mask: static %d px (band %d, outside %d), bright-static %d"
              % (m["static_total"], m["static_band"],
                 m["static_outside"], m["static_bright"]))
        print("  band: y>=%d, панель %s"
              % (res["band_y"], _fmt_rect(res["panel_band"])))
        print("  clusters in band: %d (matched %d, unclassified %d), outside %d"
              % (res["totals"]["clusters"], res["totals"]["matched"],
                 res["totals"]["unclassified"], res["totals"]["outside_band"]))
        for c in res["clusters"]:
            names = ",".join(c["elements"]) or "unclassified"
            if c.get("on_panel"):
                names += " [panel]"
            print("    %-34s px=%-5d %s"
                  % (names, c["pixels"],
                     _fmt_rect((c["x"], c["y"], c["w"], c["h"]))))
        for c in res["outside_band"]:
            print("    [outside band] px=%-4d %s"
                  % (c["pixels"], _fmt_rect((c["x"], c["y"], c["w"], c["h"]))))
    t = res["totals"] if "totals" in res else None
    if t is None:
        t = dict(elements=len(ELEMENTS),
                 proven=sum(1 for e in ELEMENTS if e["status"] == "proven"),
                 hypothesis=sum(1 for e in ELEMENTS if e["status"] == "hypothesis"))
    print("  elements: %d (proven %d, hypothesis %d)"
          % (t["elements"], t["proven"], t["hypothesis"]))
    print("  %-20s %-8s %-16s %-9s %s"
          % ("id", "kind", "rect", "status", "asset / value"))
    for el in res["elements"]:
        print("  %-20s %-8s %-16s %-9s %s | %s"
              % (el["id"], el["kind"], _fmt_rect(el["rect"]), el["status"],
                 el["asset"], el["value"]))
    if res["checks"]:
        covered = [c for c in res["checks"] if c["static"] > 0]
        print("  checks: %d/%d элементов имеют статичные пиксели в кадре"
              % (len(covered), len(res["checks"])))
        weak = [c["id"] for c in res["checks"] if c["static_frac"] < 0.2]
        if weak:
            print("  слабо подтверждены маской (<20%% статичных): %s"
                  % ", ".join(weak))


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------
def _solid(w, h, rgb):
    return bytes(rgb) * (w * h)


def self_test():
    ok = 0
    fail = 0

    def check(name, cond, detail=""):
        nonlocal ok, fail
        if cond:
            ok += 1
            print("ok   %s" % name)
        else:
            fail += 1
            print("FAIL %s%s" % (name, (" — " + detail) if detail else ""))

    # 1. downscale: константа остаётся константой
    img = (8, 6, _solid(8, 6, (10, 20, 30)))
    small = downscale(img, 4, 3)
    check("downscale constant", small == _solid(4, 3, (10, 20, 30)))

    # 2. downscale: усреднение по блоку, а не выборка
    row = bytes([255, 0, 0] * 2 + [0, 0, 255] * 2)
    img = (4, 2, row + row)
    small = downscale(img, 2, 1)
    check("downscale box average",
          small == bytes([255, 0, 0, 0, 0, 255]), small.hex())

    # 3. маска: статичный блок и движущийся пиксель
    w, h = 12, 10
    frames = []
    for shift in (0, 1, 2):
        px = bytearray(_solid(w, h, (0, 0, 0)))
        for y in range(2, 5):
            for x in range(1, 4):
                i = (y * w + x) * 3
                px[i : i + 3] = bytes((200, 10, 10))
        i = (8 * w + 9) * 3
        px[i : i + 3] = bytes((0, 40 * shift, 0))
        frames.append(bytes(px))
    m = static_mask(frames, 14)
    check("mask static block", all(m[y * w + x] for y in range(2, 5)
                                   for x in range(1, 4)))
    check("mask moving pixel", m[8 * w + 9] == 0)

    # 4. порог: разница 10 статична, 30 — нет
    a = bytes([100, 100, 100])
    b = bytes([110, 100, 100])
    c = bytes([130, 100, 100])
    check("mask threshold pass", static_mask([a, b], 14) == bytearray([1]))
    check("mask threshold fail", static_mask([a, c], 14) == bytearray([0]))

    # 5. кластеры: два блоба, bbox точный, порядок детерминирован
    w, h = 20, 10
    mask = bytearray(w * h)
    for y in range(1, 4):
        for x in range(2, 5):
            mask[y * w + x] = 1
    for y in range(6, 8):
        for x in range(14, 17):
            mask[y * w + x] = 1
    cl = clusters(mask, bridge_x=0, bridge_y=0, min_area=1,
                  width=w, height=h)
    check("clusters count", len(cl) == 2, str([c["pixels"] for c in cl]))
    check("clusters bbox",
          [(c["x"], c["y"], c["w"], c["h"]) for c in cl]
          == [(2, 1, 3, 3), (14, 6, 3, 2)])
    check("clusters pixels", [c["pixels"] for c in cl] == [9, 6])

    # 6. мост: зазор 1 px склеивается при bridge=1 и не склеивается при 0
    mask = bytearray(w * h)
    for y in range(2, 5):
        mask[y * w + 3] = 1
        mask[y * w + 5] = 1
    one = clusters(mask, bridge_x=1, bridge_y=0, min_area=1,
                   width=w, height=h)
    two = clusters(mask, bridge_x=0, bridge_y=0, min_area=1,
                   width=w, height=h)
    check("cluster bridge merges", len(one) == 1, str(len(one)))
    check("cluster no bridge splits", len(two) == 2, str(len(two)))

    # 7. min_area
    mask = bytearray(w * h)
    mask[4 * w + 4] = 1
    check("min_area filters",
          clusters(mask, 0, 0, min_area=2, width=w, height=h) == [])

    # 8. таблица элементов: границы, уникальность, обязательные поля
    ids = [el["id"] for el in ELEMENTS]
    check("elements unique ids", len(ids) == len(set(ids)))
    bad = []
    for el in ELEMENTS:
        x, y, w, h = el["rect"]
        if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > SCREEN_W or y + h > SCREEN_H:
            bad.append(el["id"])
        if el["status"] not in VALID_STATUS or el["kind"] not in VALID_KINDS:
            bad.append(el["id"])
        if not el["asset"] or not el["evidence"]:
            bad.append(el["id"])
    check("elements rects/status/asset", not bad, ",".join(sorted(set(bad))))
    panel = [el for el in ELEMENTS if el["id"] == "panel"][0]
    check("elements panel == PANEL_BAND", tuple(panel["rect"]) == PANEL_BAND)

    # 9. проверка элемента по маскам
    w, h = SCREEN_W, SCREEN_H
    static = bytearray(w * h)
    bright = bytearray(w * h)
    target = [el for el in ELEMENTS if el["id"] == "value_ammo"][0]["rect"]
    for y in range(target[1], target[1] + target[3]):
        for x in range(target[0], target[0] + target[2]):
            static[y * w + x] = 1
            bright[y * w + x] = 1
    checks = {c["id"]: c for c in element_checks(static, bright)}
    check("element covered", checks["value_ammo"]["static_frac"] == 1.0)
    check("element uncovered", checks["label_kill"]["static_frac"] == 0.0)

    # 10. детерминизм и JSON
    r1 = audit("level0", frames_dir="/nonexistent")
    r2 = audit("level0", frames_dir="/nonexistent")
    j1 = json.dumps(r1, sort_keys=True, ensure_ascii=False)
    j2 = json.dumps(r2, sort_keys=True, ensure_ascii=False)
    check("determinism", j1 == j2)
    check("json roundtrip", json.loads(j1) == json.loads(j2))
    check("json no frames", r1["frames"]["found"] is False)

    # 11. матчинг кластера с элементом
    cl = dict(x=50, y=163, w=10, h=5, pixels=10)
    check("match element", "value_ammo" in match_elements(cl, include_panel=False),
          str(match_elements(cl)))
    check("match panel only", match_elements(cl, include_panel=False) != [], "")
    cl = dict(x=300, y=125, w=5, h=5, pixels=10)
    check("match panel excluded",
          match_elements(cl, include_panel=False) == [], str(match_elements(cl)))
    cl = dict(x=300, y=20, w=5, h=5, pixels=10)
    check("match none", match_elements(cl) == [], str(match_elements(cl)))

    print("\nself-test: %d ok, %d failed" % (ok, fail))
    return fail == 0


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="инвентарь кокпита level0: элементы, ассеты, координаты, поля")
    ap.add_argument("--level", default="level0",
                    help="имя уровня (по умолчанию level0)")
    ap.add_argument("--frames", metavar="DIR",
                    help="каталог с win1..win3.png (по умолчанию out/oracle)")
    ap.add_argument("--threshold", type=int, default=STATIC_THRESHOLD,
                    help="порог «не изменился», max канал (по умолчанию %d)"
                         % STATIC_THRESHOLD)
    ap.add_argument("--bright", type=int, default=BRIGHT_THRESHOLD,
                    help="порог яркости (по умолчанию %d)" % BRIGHT_THRESHOLD)
    ap.add_argument("--json", action="store_true",
                    help="вывести результат в JSON")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1
    if not (0 <= args.threshold <= 255) or not (0 <= args.bright <= 255):
        print("hud_audit: thresholds must be 0..255", file=sys.stderr)
        return 2
    try:
        res = audit(args.level, args.frames, args.threshold, args.bright)
    except (OSError, ValueError) as exc:
        print("hud_audit: %s" % exc, file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(res, ensure_ascii=False, sort_keys=True, indent=1))
    else:
        print_report(res)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
