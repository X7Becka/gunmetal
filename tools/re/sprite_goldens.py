#!/usr/bin/env python3
"""Python-эталон для «реальных» спрайтовых golden'ов `engine/tests/test_sprite.cpp`.

Тест блиттит настоящие `.sbm` из `PICS/` и `sprites/` в детерминированный
RGB-кадр и сравнивает весь буфер (вместе с padding строк) с FNV-1a64.  Скрипт
независимо (никакого C++ в пути вычисления) повторяет те же кадры по правилам
`docs/SPRITE.md`:

* палитра — таблица движка `FRONTEND/GAMEPAL` из `FRONTEND/BMP.IDX`
  (`gm/palette.py::from_archive`; локальный фолбэк — `gm/bigfile.py` +
  `gm/bmp.py`), а не «жёлто-розовые» плейсхолдеры `FLAG/naUSA.BMP`;
* спрайт — уровень 0 `gm/sbm.py` (width/height/flags из заголовка);
* nearest: `sx = (px - x) // scale`, flip после деления, прозрачный индекс
  вырезается до выборки палитры;
* bilinear: `u = (dx + 0.5) / scale - 0.5`, flip по `width-1-u`, wrap по
  маске степени двойки, веса `(1-ax)(1-ay) …`, порог покрытия `0.5`;
* цвет: `mix`/`add` из `gm/render.py` с весом `alpha/255` и `_clamp`
  (усечение к нулю, затем 0..255); `alpha == 0` не трогает кадр;
* фон кадра: `r = (7x + 3y + 11) & 255`, `g = (2x + 11y + 29) & 255`,
  `b = (13x + 5y + 47) & 255`, padding — нули.

    python3 tools/re/sprite_goldens.py            # таблица + сверка с тестом
    python3 tools/re/sprite_goldens.py --json     # машинный вывод
    python3 tools/re/sprite_goldens.py --check    # код возврата != 0 при расхождении
    python3 tools/re/sprite_goldens.py --palette carrier   # что было с naUSA.BMP

Только чтение установки игры; golden'ы в тесте остаются константами.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from gm import bigfile, bmp, sbm  # noqa: E402
from gm import palette as gm_palette  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
TEST_FILE = os.path.join(ROOT, "engine", "tests", "test_sprite.cpp")

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a64(data: bytes) -> int:
    digest = FNV_OFFSET
    for byte in data:
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


# --- палитра ---------------------------------------------------------------

def engine_palette(game_dir: str) -> bytes:
    """256 RGBA-квадов `FRONTEND/GAMEPAL` — таблица, которую грузит движок.

    Предпочитает `gm.palette.from_archive` (появился вместе с переходом на
    GAMEPAL); если модуль ещё старый — читает архив сам, не меняя `gm/`.
    """
    from_archive = getattr(gm_palette, "from_archive", None)
    if from_archive is not None:
        table = from_archive(game_dir)
        if table is not None:
            return table
    idx = os.path.join(game_dir, "FRONTEND", "BMP.IDX")
    if not os.path.exists(idx):
        raise FileNotFoundError(f"{idx}: нет индекса FRONTEND/BMP.IDX")
    with bigfile.BigFile(idx) as archive:
        for entry in archive:
            if entry.name.upper() == "GAMEPAL":
                return bmp.parse(archive.read(entry)).palette
    raise FileNotFoundError(f"{idx}: нет записи GAMEPAL")


def carrier_palette(game_dir: str) -> bytes:
    """Старый источник: свободный `FLAG/naUSA.BMP` (21 слот — magenta)."""
    for rel in ("FLAG/naUSA.BMP", "FLAG/aanataka.bmp", "PICS/oaktabl1.bmp"):
        path = os.path.join(game_dir, rel)
        if os.path.exists(path):
            return gm_palette.load_from_bmp(path)
    raise FileNotFoundError(f"{game_dir}: нет свободного BMP-носителя палитры")


# --- спрайты и кадр --------------------------------------------------------

@dataclass
class Sprite:
    width: int
    height: int
    pixels: bytes


def load_sprite(game_dir: str, sub: str, name: str) -> Sprite:
    idx = os.path.join(game_dir, sub, "SBM.IDX")
    with bigfile.BigFile(idx) as archive:
        for entry in archive:
            if entry.name == name:          # тест сравнивает имена точно
                blob = sbm.parse(archive.read(entry))
                break
        else:
            raise KeyError(f"{name} отсутствует в {sub}/SBM.IDX")
    return Sprite(blob.width, blob.height, blob.pixels)


def background(width: int, height: int, stride: int) -> bytearray:
    """Тот же градиент, что `make_target` в тесте; padding остаётся нулевым."""
    rgb = bytearray(stride * height)
    for y in range(height):
        row = y * stride
        for x in range(width):
            o = row + x * 3
            rgb[o + 0] = (x * 7 + y * 3 + 11) & 0xFF
            rgb[o + 1] = (x * 2 + y * 11 + 29) & 0xFF
            rgb[o + 2] = (x * 13 + y * 5 + 47) & 0xFF
    return rgb


# --- цвет ------------------------------------------------------------------

def clamp_channel(value: float) -> int:
    """`gm/render.py::_clamp`: усечение к нулю, затем 0..255."""
    if value < 0.0:
        return 0
    if value > 255.0:
        return 255
    return int(value)


def blend_channel(dst: int, src: int, alpha: int, additive: bool) -> int:
    if alpha == 255:
        return min(255, dst + src) if additive else src
    weight = alpha / 255.0
    if additive:
        return clamp_channel(dst + src * weight)
    return clamp_channel(dst * (1.0 - weight) + src * weight)


def store_pixel(rgb, stride: int, x: int, y: int, color, alpha: int, additive: bool) -> None:
    o = y * stride + x * 3
    rgb[o + 0] = blend_channel(rgb[o + 0], color[0], alpha, additive)
    rgb[o + 1] = blend_channel(rgb[o + 1], color[1], alpha, additive)
    rgb[o + 2] = blend_channel(rgb[o + 2], color[2], alpha, additive)


# --- параметры блита -------------------------------------------------------

@dataclass
class Blit:
    x: int = 0
    y: int = 0
    scale: int = 1
    flip_x: bool = False
    flip_y: bool = False
    alpha: int = 255
    additive: bool = False
    transparent_index: int = 0


def clip_rect(sprite: Sprite, p: Blit, width: int, height: int):
    left = max(p.x, 0)
    top = max(p.y, 0)
    right = min(p.x + sprite.width * p.scale, width)
    bottom = min(p.y + sprite.height * p.scale, height)
    return left, top, right, bottom


def blit_nearest(rgb, width: int, height: int, stride: int, colors, sprite: Sprite,
                 p: Blit) -> None:
    if p.alpha == 0:
        return
    left, top, right, bottom = clip_rect(sprite, p, width, height)
    if right <= left or bottom <= top:
        return
    for py in range(top, bottom):
        sy = (py - p.y) // p.scale
        if p.flip_y:
            sy = sprite.height - 1 - sy
        row = sy * sprite.width
        for px in range(left, right):
            sx = (px - p.x) // p.scale
            if p.flip_x:
                sx = sprite.width - 1 - sx
            index = sprite.pixels[row + sx]
            if p.transparent_index >= 0 and index == p.transparent_index:
                continue
            store_pixel(rgb, stride, px, py, colors[index], p.alpha, p.additive)


def blit_bilinear(rgb, width: int, height: int, stride: int, colors, sprite: Sprite,
                  p: Blit) -> None:
    if p.alpha == 0:
        return
    left, top, right, bottom = clip_rect(sprite, p, width, height)
    if right <= left or bottom <= top:
        return
    sw, sh, pix = sprite.width, sprite.height, sprite.pixels
    scale = float(p.scale)
    for py in range(top, bottom):
        fv = (py - p.y + 0.5) / scale - 0.5
        if p.flip_y:
            fv = (sh - 1) - fv
        fy0 = math.floor(fv)
        ay = fv - fy0
        ty0 = fy0 % sh
        ty1 = (ty0 + 1) % sh
        row0 = ty0 * sw
        row1 = ty1 * sw
        for px in range(left, right):
            fu = (px - p.x + 0.5) / scale - 0.5
            if p.flip_x:
                fu = (sw - 1) - fu
            fx0 = math.floor(fu)
            ax = fu - fx0
            tx0 = fx0 % sw
            tx1 = (tx0 + 1) % sw

            i00 = pix[row0 + tx0]
            i10 = pix[row0 + tx1]
            i01 = pix[row1 + tx0]
            i11 = pix[row1 + tx1]

            w00 = (1.0 - ax) * (1.0 - ay)
            w10 = ax * (1.0 - ay)
            w01 = (1.0 - ax) * ay
            w11 = ax * ay

            if p.transparent_index >= 0:
                covered = (w00 * (i00 != p.transparent_index)
                           + w10 * (i10 != p.transparent_index)
                           + w01 * (i01 != p.transparent_index)
                           + w11 * (i11 != p.transparent_index))
                if covered <= 0.5:
                    continue

            c00, c10 = colors[i00], colors[i10]
            c01, c11 = colors[i01], colors[i11]
            color = tuple(
                clamp_channel(c00[k] * w00 + c10[k] * w10 + c01[k] * w01 + c11[k] * w11)
                for k in range(3)
            )
            store_pixel(rgb, stride, px, py, color, p.alpha, p.additive)


# --- кейсы из engine/tests/test_sprite.cpp (только чтение) ------------------

@dataclass
class Case:
    name: str
    sub: str
    sprite: str
    width: int
    height: int
    stride_extra: int
    blit: Blit
    bilinear: bool = False


CASES = [
    Case("real_aflm1_nearest", "sprites", "AFLM0001", 160, 120, 0,
         Blit(-10, -20, 1, False, False, 255, False, 0)),
    Case("real_blam10_scale2_flip", "sprites", "BLAM0010", 200, 160, 0,
         Blit(17, 9, 2, True, False, 255, False, 0)),
    Case("real_expl1_scale3_flipy_alpha", "sprites", "EXPL0001", 210, 190, 0,
         Blit(-25, 40, 3, False, True, 128, False, 0)),
    Case("real_baracuda_pics", "PICS", "BARACUDA", 128, 96, 13,
         Blit(-17, 3, 1, False, False, 255, False, 0)),
    Case("real_aflm1_bilinear", "sprites", "AFLM0001", 160, 140, 0,
         Blit(5, 7, 2, False, True, 255, False, 0), True),
    Case("real_expl1_additive", "sprites", "EXPL0001", 180, 150, 0,
         Blit(11, 13, 2, False, False, 255, True, 0)),
    Case("real_blam1_bilinear_alpha", "sprites", "BLAM0001", 256, 192, 0,
         Blit(-4, -6, 3, True, False, 128, False, 0), True),
]


def cpp_goldens(path: str = TEST_FILE) -> dict[str, int]:
    """FNV-литералы из `real_cases` теста — для сверки, не для вычисления."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return {}
    block = text.split("void real_cases", 1)
    if len(block) != 2:
        return {}
    return {name: int(value, 16)
            for name, value in re.findall(r'"(real_[a-z0-9_]+)".*?0x([0-9a-f]{16})ull',
                                          block[1], re.S)}


def run_case(game_dir: str, colors, case: Case) -> int:
    sprite = load_sprite(game_dir, case.sub, case.sprite)
    if len(sprite.pixels) != sprite.width * sprite.height:
        raise ValueError(f"{case.name}: уровень 0 не совпадает с заголовком")
    stride = case.width * 3 + case.stride_extra
    rgb = background(case.width, case.height, stride)
    blit = blit_bilinear if case.bilinear else blit_nearest
    blit(rgb, case.width, case.height, stride, colors, sprite, case.blit)
    return fnv1a64(bytes(rgb))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", default=DEFAULT_GAME, help="каталог установки")
    parser.add_argument("--json", action="store_true", help="машинный вывод")
    parser.add_argument("--check", action="store_true",
                        help="код возврата 1, если FNV не совпал с тестом")
    parser.add_argument("--palette", choices=("engine", "carrier"), default="engine",
                        help="engine = FRONTEND/GAMEPAL (по умолчанию), "
                             "carrier = старый FLAG/naUSA.BMP")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.game):
        print(f"sprite_goldens: нет установки {args.game}", file=sys.stderr)
        return 2

    try:
        table = engine_palette(args.game) if args.palette == "engine" \
            else carrier_palette(args.game)
    except (FileNotFoundError, ValueError) as exc:
        print(f"sprite_goldens: палитра: {exc}", file=sys.stderr)
        return 2
    if len(table) < 1024:
        print(f"sprite_goldens: палитра {len(table)} байт, нужно 1024", file=sys.stderr)
        return 2
    colors = [tuple(table[i * 4:i * 4 + 3]) for i in range(256)]

    goldens = cpp_goldens()
    rows = []
    for case in CASES:
        try:
            fnv = run_case(args.game, colors, case)
        except (FileNotFoundError, KeyError, ValueError) as exc:
            print(f"sprite_goldens: {case.name}: {exc}", file=sys.stderr)
            return 2
        want = goldens.get(case.name)
        rows.append({
            "case": case.name,
            "fnv": f"0x{fnv:016x}",
            "cpp_literal": f"0x{fnv:016x}ull",
            "cpp_golden": None if want is None else f"0x{want:016x}",
            "matches": None if want is None else fnv == want,
        })

    matched = sum(1 for row in rows if row["matches"])
    compared = sum(1 for row in rows if row["matches"] is not None)

    if args.json:
        print(json.dumps({
            "game": args.game,
            "palette": args.palette,
            "cases": rows,
            "compared": compared,
            "matched": matched,
            "all_match": compared == len(rows) and matched == len(rows),
        }, indent=2, ensure_ascii=False))
    else:
        print(f"# палитра: {args.palette}, игра: {args.game}")
        print(f"{'кейс':32} {'FNV-1a64':20} {'C++':22} итог")
        for row in rows:
            verdict = "—" if row["matches"] is None else ("OK" if row["matches"] else "MISMATCH")
            print(f"{row['case']:32} {row['fnv']:20} "
                  f"{row['cpp_golden'] or '(нет в тесте)':22} {verdict}")
        print(f"\n{matched}/{compared} совпало с engine/tests/test_sprite.cpp")

    if args.check and matched != len(rows):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
