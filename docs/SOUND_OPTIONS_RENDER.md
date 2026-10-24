# Рендер экрана 11 `SOUND OPTIONS` (`engine/src/ui/sound_options_render.h`)

Срез 18 пункта `M8.8`: один кадр экрана **11** фронтенда (`gundx.exe`,
ImageBase `0x400000`) в RGBA-буфер 320×200. Модуль без состояния — рисует то,
что ему передали в `SoundOptionsRenderOptions`, и отдаёт числа (`rows`,
`sliders`, `checks`, `arrows`, `ink_pixels`, `missing_glyphs`). Шрифты,
метрики и атласы — из `render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`),
точный fade — из `render/hud_lut`. Ничего не измеряется заново.

Код: `engine/src/ui/sound_options_render.h/.cpp`; тест —
`engine/tests/test_sound_options_render.cpp` (CTest `sound_options_render`,
label `quick`). Документ фиксирует «доказано / модель / числа / ограничения»;
соседние срезы — `MOUSE_SETUP_RENDER.md` (22), `MATCH_SETUP_RENDER.md` (27),
`MENU_RENDER.md` (общая механика `0x485dd0` и виджетов).

## 1. API

```cpp
#include "ui/sound_options_render.h"

gm::SoundOptionsRenderOptions options;   // defaults = .data/.bss движка (§2.4)
options.cursor = 2;                      // [0x68ccf4], id пункта: 0/2/4/6
options.music_volume = 16;               // [0x4aac74], кламп 0..20
options.effects_volume = 20;             // [0x4aac78], кламп 0..20
options.switch_left_right = 1;           // [0x5a494c], 0/1 -> NOCHECK/CHECK
options.sound_enabled = true;            // [0x4aa384]
options.music_low_memory = false;        // [0x68cc9c]
options.background = "PLATE";            // "" = чёрный холст

std::vector<std::uint32_t> rgba;         // 0xRRGGBBAA
int width = 0, height = 0;
gm::SoundOptionsRenderStats stats;
std::string error;
if (!gm::render_sound_options(game, options, rgba, width, height, &stats, &error)) {
    // error: текст с префиксом "sound_options_render: "
}
// width=320 height=200, rgba.size()==64000
// stats: rows=7 sliders=2 checks=1 ink_pixels=1869 missing_glyphs=0 arrows=0
```

| Поле `SoundOptionsRenderOptions` | По умолчанию | Смысл |
|---|---|---|
| `std::string background` | `""` | запись `FRONTEND/BMP.IDX`, рисуется в (0,0); пусто — чёрный холст |
| `int cursor` | `0` | `[0x68ccf4]`, **id пункта** (0/2/4/6 с звуком, 2..5 без); вне нарисованных id ничего не подсвечивает (`-1` — кадр без подсветки) |
| `int music_volume` | `10` | `[0x4aac74]`, кламп 0..20; при `music_low_memory` слайдер не рисуется |
| `int effects_volume` | `20` | `[0x4aac78]`, кламп 0..20 |
| `int switch_left_right` | `0` | `[0x5a494c]`, ненулевое — `CHECK` |
| `bool sound_enabled` | `true` | `[0x4aa384]` (`.data` = 1); `false` — текстовая заглушка |
| `bool music_low_memory` | `false` | `[0x68cc9c]`; `true` — `Music Disabled: Low Memory` вместо слайдера |
| `bool fade_lut` | `false` | точный fade `0x682f90` по палитровым индексам |
| `int fade_lut_level` | `11` | 0..15; 11 — тождество, 12..15 светлеют |
| `std::string fade_lut_dump` | `""` | дамп 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | готовая таблица, приоритетнее дампа |

Публичные помощники (нужны тесту и будущему CLI):
`sound_options_item_y(item)`, `sound_options_item_x(item)`,
`sound_options_disabled_y(index)`, `sound_options_volume(int)`,
`sound_options_switch_on(int)`, `sound_options_knob_x(volume)`,
`sound_options_label(item, options)`.

`SoundOptionsRenderStats`: `rows` (`0x67f10c`: 7 или 1), `sliders` (пар
`SLIDE`/`KNOB`: 2/1/0), `checks` (рамка `CHECK`/`NOCHECK`: 1/0), `ink_pixels`
(уникальные пиксели глифов и спрайтов; фоновая плата не считается — как в
`menu_render`/`restart_render`), `missing_glyphs` (байты без глифа, не
фатальны), `arrows` (`MENUARRW` на этом экране не рисуется — всегда 0).

Контракт буфера: при отказе `rgba` очищается, `width`/`height` = 0,
статистика обнуляется; `stats`/`error` могут быть `nullptr`. Функция только
читает установку, детерминирована, не зависит от текущего каталога.

## 2. Доказано (листинг + данные)

Инструмент: `python3 tools/re/disrange.py`, данные — `tools/re/exelib.py`.
**Обработчик — `0x4889f0`** (таблица `0x4883d8`, id 11; stub `0x487eb6`
делает `call 0x4889f0`). `0x488a80` из задания — **середина обработчика**, а
не вход: это `push 0x12` (аргумент `x`) в вызове `Music Volume:`, сразу после
расчёта кноба (`docs/MOUSE_SETUP_RENDER.md` §2 отмечает то же).

### 2.1. Пролог, заголовок и два режима

| Факт | Где |
|---|---|
| копия `PLATE` 196×131 в `0x6865f8` | `0x4889f0` → `0x486050` |
| заголовок: шрифт 5, текст `0x4bd534`, `centered=1`, `x=38`, `row=14`, `item=-1` | `0x4889f5`..`0x488a04` |
| звук включён? `[0x4aa384]` (`.data` = 1) | `0x488a09`..`0x488a13` |
| `rows = 7`, `step = 10` | `0x488a1e`, `0x488a2a` |
| `rows = 1`, `step = 0` (ветка без звука) | `0x488b38`, `0x488b42` |

`0x485dd0` при `item == -1` трактует `row` как абсолютный `y`, а при
`centered=1` заменяет `x` на `(196 - width)/2` (`0x485de2`..`0x485dfa`) —
поэтому `x = 38` в вызове заголовка не используется, а сам заголовок стоит на
**строке 14**. Текст пишется в копию платы, поэтому **клип — 196×131**.

Основной режим (`[0x4aa384] != 0`), `y = row + 10*item`:

| item | текст | VA | x | row | y |
|---:|---|---|---:|---:|---:|
| 0 | `Music Volume:` / `Music Disabled: Low Memory` | `0x4bd508` / `0x4bd518` | 18 | 27 | 27 |
| 2 | `Effects Volume:` | `0x4bd4f8` | 18 | 27 | 47 |
| 4 | `Switch Left Right` | `0x4bd4e4` | 30 | 27 | 67 |
| 6 | `Done` | `0x4bceb4` | 18 | 24 | 84 |

`[0x68cc9c]` (`.bss`, ставится в 1 при нехватке памяти под музыку: `0x491273`
после `cmp [0x4bf290],0xe; jl`) выбирает текст пункта 0 и **убирает** его
слайдер: `0x488a34`..`0x488a43` — ветка без `0x486070`/`0x4860b0`.

Ветка без звука (`[0x4aa384] == 0`, `0x488b29`..`0x488b99`) — четыре строки
`x = 18`, item id 2..5, `step = 0`, то есть абсолютные `y = row`:

| item | текст | VA | y |
|---:|---|---|---:|
| 2 | `Sound is disabled.` | `0x4bd4d0` | 27 |
| 3 | `Rerun Gunmetal without the` | `0x4bd4b4` | 37 |
| 4 | `N command line option to` | `0x4bd498` | 47 |
| 5 | `enjoy sound.` | `0x4bd488` | 57 |

Строка `0x4bd498` в бинаре начинается именно с байта `N` (`4e 20 63 6f …`) —
это остаток подсказки `-nosound`; модуль воспроизводит байты как есть.

Подсветка: `0x485dd0` переключает на шрифт 4, только если
`item == [0x68ccf4]` **и** вызывающий передал шрифт 0 (`0x485e0d`..`0x485e19`),
поэтому заголовок (шрифт 5) не подсвечивается никогда. Полосы/рамки/`MENULIT`
нет. Курсор — id пункта, поэтому `-1` и 99 не подсвечивают ничего; в ветке без
звука сравнение идёт с id 2..5.

### 2.2. Слайдеры — две пары `SLIDE`/`KNOB`

`0x486070(SLIDE)` и `0x4860b0(KNOB)` — маскированные блиты в копию платы (шаг
`0xc4` = 196), аргумент — адрес назначения внутри `0x6865f8`; отсюда
`addr - 0x6865f8 = y*196 + x`:

| # | `SLIDE` 72×8 | `KNOB` база 8×8 | вызовы |
|---:|---|---|---|
| music | (68, 37) `0x688290` | x = 69 `0x688291` | `0x488a45`, `0x488a74` |
| effects | (68, 57) `0x6891e0` | x = 69 `0x6891e1` | `0x488a93`, `0x488ac2` |

Позиция кноба (одинаково для обеих полос, `0x488a4f`..`0x488a73`):

```
ecx = 27 * volume                    ; lea eax,[eax+eax*2]; lea ecx,[eax+eax*8]
edx = (0x66666667 * ecx) >> 32       ; imul ecx
edx = edx >> 2                       ; sar edx,2: знаковое деление на 10
x   = 0x688291 - 0x6865f8 + edx
    = 69 + trunc(27*volume/10)
```

Диапазон и шаг доказаны обработчиками ввода:

| факт | адрес |
|---|---|
| максимум `0x14` = 20, дальше `jge` — без изменения | `0x48c711` (music), `0x48c6da` (effects) |
| минимум 0, `jle` — без изменения | `0x48beb9` (music), `0x48be83` (effects) |
| шаг `±1` | `0x48c716`, `0x48c6df`, `0x48bebd`, `0x48be87` |
| позиции кноба 0..20 | `69, 71, 74, …, 120, 123` (54 px) |

То есть хранимое значение — **0..20**, а не 0..32 «сырого» поля CFG
(`docs/GAME_CFG.md` §1). В микшер уходит `5*volume` (`lea eax,[eax+eax*4]`
перед `0x40b860` для музыки и `0x40b900` для эффектов), то есть 0..100; сам
API несёт 0..20 и ничего не применяет.

### 2.3. Чекбокс `Switch Left Right`

`0x4833e0([0x5a494c], 196, 0x689338)` (`0x488ae7`..`0x488af2`), якорь
`0x689338 - 0x6865f8 = (20, 59)`. При ненулевом значении копируется 16-строчный
`CHECK` 8×16 в якорь; при нуле — 8-строчный `NOCHECK` 8×8 **на 8 строк ниже**
(`lea ecx,[ecx+edi*8]` в `0x483403`), так что низ рамки всегда на строке
подписи 67. Индекс палитры 0 прозрачен (`test dl,dl; je` в `0x483413`).

### 2.4. Значения по умолчанию

| поле | до загрузки CFG | где |
|---|---|---|
| `0x4aac74` (music volume) | `10` | `.data`; в поставочном `GUNMETAL.CFG` — 16 |
| `0x4aac78` (effects volume) | `20` | `.data`; в поставочном CFG — 20 |
| `0x5a494c` (swap left/right) | `0` | `.bss` |
| `0x4aa384` (sound enabled) | `1` | `.data` (проверено чтением образа) |
| `0x68cc9c` (music low memory) | `0` | `.bss`, ставится в 1 только при сбое |
| `0x68ccf4` (курсор) | `0` | `.bss`; вход из главного меню (`0x48cf78`) его не пишет |

### 2.5. Ассеты, цвета, fade

`FRONTEND/BMP.IDX` (`docs/UI_SCREENS.md` §9): `PLATE` 196×131, `SLIDE` 72×8,
`KNOB` 8×8, `CHECK` 8×16, `NOCHECK` 8×8, `FONTS4` 512×168 (грани 5/0/4).
Размеры проверяются при загрузке; расхождение — текстовый отказ.

Цвета — из палитры атласа: заголовок (шрифт 5) — (155,15,15) тёмно-красный,
пункты (шрифт 0) — (19,15,15) тёмно-серый, подсветка (шрифт 4) — (239,239,239)
белый (`docs/MENU_RENDER.md` §3). `PLATE`/`SLIDE`/`KNOB`/`CHECK`/`NOCHECK`
используют собственные палитры BMP; индекс 0 — прозрачный ключ спрайтов.

Точный fade `0x682f90`: каждый нарисованный палитровый индекс проходит через
`render/hud_lut.h::map(level, index)` **до** выборки цвета (как в
`mouse_setup_render`/`restart_render`). Таблица — готовая
(`fade_lut_table`) или из дампа 4096 Б (`fade_lut_dump`, `docs/HUD_LUT.md` §2);
без обеих — текстовый отказ, а не «молча без fade».

## 3. Числа (тест, реальная установка)

`./build/sandbox/engine/tests/gm_sound_options_render_tests` — **430 проверок**,
`sound_options_render: ok (430 checks)`, игра только читается.

Кадр по умолчанию (`.data`: music 10, effects 20, swap 0, звук включён,
курсор 0):

```
title "SOUND OPTIONS": x=62 y=14 ink=212 bbox (62,14)-(132,20) 71x7
row 0: x=18 y=27 "Music Volume:"  ink=155 bbox (18,27)-(79,33)
row 2: x=18 y=47 "Effects Volume:" ink=177 bbox (18,47)-(89,53)
row 4: x=30 y=67 "Switch Left Right" ink=190 bbox (30,67)-(107,74)
row 6: x=18 y=84 "Done"          ink=61  bbox (18,84)-(37,90)
slider 0: volume=10 knob x=96 y=37, SLIDE+KNOB ink=505
slider 1: volume=20 knob x=123 y=57, SLIDE+KNOB ink=505
frame: rows=7 sliders=2 checks=1 ink=1869 missing=0 arrows=0
```

Слайдеры: кноб `0/5/10/15/20 -> 69/82/96/109/123`; сдвиг музыки
0→10 и 10→20 меняет по **122 px** и только прямоугольник 72×8
(68..139); эффекты 0→20 — **124 px**, тоже только свой прямоугольник; чужие
пиксели не меняются (проверяется `differing_outside_rect`).

Чекбокс: выключен — `NOCHECK` в (20,67), ink 64; включён — `CHECK` в (20,59),
ink 91; переключение меняет **128 px** и только рамку 8×16 (20..27, 59..74).

`music_low_memory = true`: `sliders` 2→1, кадр отличается на **772 px**,
все — в строках 27..44 (подпись `Music Disabled: Low Memory` + исчезнувшая
полоса); эффекты, чекбокс и `Done` не меняются.

`sound_enabled = false`: `rows=1 sliders=0 checks=0`; четыре строки на
(18, 27/37/47/57), каждая сверена с независимым блитом; заголовок тот же.

Курсор: 0/2/4/6 даёт белый шрифт 4 только в своей полосе — отличия
**155/177/190/61 px**; `-1` и 99 не подсвечивают ничего (0 отличий); заголовок
остаётся тёмно-красным.

`background="PLATE"`: 196×131 декодируется в тесте независимо, отличий под
чернилами **0**, весь кадр отличается на **23807 px**, `ink` не меняется
(1869), вне платы — чёрный.

Fade: уровень 11 — **0/64000** отличий от кадра без fade (побайтово); 12..15
отличаются от 11 на **16154/24750/25084/25560 px**, соседние пары —
**17769/23158/20418 px**; `ink` не меняется; уровень 13 детерминирован.

Негативы (все тексты `sound_options_render:` с очищенным буфером): пустой
каталог, нет `BMP.IDX`, архив без `FONTS4`, без `PLATE`, без `SLIDE`, без
`KNOB`, без `CHECK`, без `NOCHECK`, `PLATE` 8×16, `SLIDE` 8×8, неизвестный
фон, `fade_lut` без таблицы/дампа и отсутствующий дамп. Полный синтетический
архив из шести записей рисует тот же кадр (ink=1869), два прогона
побайтово равны.

## 4. Модель (не доказано кодом)

* **Плата на экране.** Оригинал копирует `PLATE` в `0x6865f8` как буфер
  композиции; куда этот буфер блитится на 320×200, в документации не
  зафиксировано (`docs/MENU_RENDER.md` §2). Поэтому `PLATE` не рисуется
  неявно: `background` — выбор вызывающего, по умолчанию чёрный холст.
  Текст и спрайты клипаются к 196×131 — это доказано адресами назначения.
* **Значения по умолчанию** взяты из `.data` (10/20), а не из поставочного
  CFG (16/20): модуль не читает конфиг, как и соседние рендереры. Оба числа
  вынесены в константы (`kSoundOptionsMusicDefault`,
  `kSoundOptionsMusicShippedCfg`).
* **Варианты строк без звука** — модель по байтам; что именно печатает
  оригинал при `[0x4aa384] == 0`, проверено только листингом (игра без
  звуковой карты в тесте не запускается).
* **Курсор по умолчанию 0** — `.bss`; вход в состояние 11 его не пишет,
  поэтому «первый кадр» зависит от предыдущего экрана. API даёт нейтральные 0.

## 5. Ограничения

* Нет ввода: `+/-1` громкостей, тумблер `Switch Left Right`, `Esc`/переходы в
  другие состояния, навигация курсором — не реализованы и не читаются.
* Нет звука: `updown`/`vleft`/`select` (`docs/UI_SOUNDS.md` §3.2) не
  проигрываются, микшерные вызовы `0x40b860`/`0x40b900` с `5*volume` не
  делаются.
* Нет `GUNMETAL.CFG`: значения не читаются и не пишутся; громкости из
  `options` **никуда не применяются** — это только пиксели кадра.
* **CLI — есть (срез 19).** `--sound-options [--sound-options-cursor N]
  [--sound-options-music N] [--sound-options-effects N] [--sound-options-swap]
  [--sound-options-off] [--sound-options-low-memory] [--no-plate] [--png OUT]
  [--fade-lut N]`: по умолчанию
  `sound_options: rows=7 sliders=2 checks=1 cursor=0 arrows=0 ink=1869
  missing=0`; `--sound-options-off` — `rows=1 ink=1098`; low memory —
  `sliders=1 ink=1524`; музыки/эффекты 0 + swap, курсор 6 — `ink=1966`.
  CTest `cli_sound_options*`; `cli_fade_lut` (11 = без fade).
* `arrows` всегда 0: в `0x4889f0` нет вызова `MENUARRW`; поле оставлено ради
  единой формы статистики соседних рендереров.
* Шрифты/палитры — только из поставочного `FONTS4`; синтетических атласов
  модуль не принимает.
