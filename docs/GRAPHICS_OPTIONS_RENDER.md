# Рендер экрана 12 `GRAPHICS OPTIONS` (`engine/src/ui/graphics_options_render.h`)

Срез 19 пункта `M8.8`: один кадр экрана **12** фронтенда (`gundx.exe`,
ImageBase `0x400000`) в RGBA-буфер 320×200. Модуль без состояния — рисует то,
что ему передали в `GraphicsOptionsRenderOptions`, и отдаёт числа (`rows`,
`checks`, `sliders`, `arrows`, `ink_pixels`, `missing_glyphs`). Шрифты,
метрики и атласы — из `render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`),
текст строки режима — из уже доказанного форматтера `0x486500`
(`ui/video_mode_list_render.h::video_mode_list_label`), точный fade — из
`render/hud_lut`. Ничего не измеряется заново.

Код: `engine/src/ui/graphics_options_render.h/.cpp`; тест —
`engine/tests/test_graphics_options_render.cpp` (CTest `graphics_options_render`,
label `quick`). Соседние срезы — `SOUND_OPTIONS_RENDER.md` (11),
`CONTROL_OPTIONS_RENDER.md` (10), `MENU_RENDER.md` (общая механика `0x485dd0` и
виджетов), `VIDEO_MODE_LIST_RENDER.md` (экран 18, форматтер `0x486500`).

## 1. API

```cpp
#include "ui/graphics_options_render.h"

gm::GraphicsOptionsRenderOptions options;   // defaults = .data/.bss движка (§2.6)
options.cursor = 2;                         // [0x68ccf4], id пункта 0..5
options.mode = {800, 600, 32, 0x804};       // дескриптор 0x4c05c0 для строки режима
options.border = 3;                         // [0x4bc1c8], кламп 0..9
options.gamma = 1;                          // [0x5a4950], кламп 0..3
options.reflections = 1;                    // [0x4aac84], 0/1 -> NOCHECK/CHECK
options.light_maps = 2;                     // [0x4aac88], кламп 0..2
options.background = "PLATE";               // "" = чёрный холст

std::vector<std::uint32_t> rgba;            // 0xRRGGBBAA
int width = 0, height = 0;
gm::GraphicsOptionsRenderStats stats;
std::string error;
if (!gm::render_graphics_options(game, options, rgba, width, height, &stats, &error)) {
    // error: текст с префиксом "graphics_options_render: "
}
// width=320 height=200, rgba.size()==64000
// stats: rows=6 checks=1 sliders=1 arrows=0 ink_pixels=1866 missing_glyphs=0
```

| Поле `GraphicsOptionsRenderOptions` | По умолчанию | Смысл |
|---|---|---|
| `std::string background` | `""` | запись `FRONTEND/BMP.IDX`, рисуется в (0,0); пусто — чёрный холст |
| `int cursor` | `0` | `[0x68ccf4]`, **id пункта** 0..5; вне нарисованных id ничего не подсвечивает (`-1` — кадр без подсветки) |
| `VideoModeListEntry mode` | `{800,600,32,0x804}` | 5-dword дескриптор `0x4c05c0`; в метку идут `width`/`height`/`bpp`/`flags` (§2.6 — модель) |
| `int border` | `3` | `[0x4bc1c8]`, кламп 0..9 |
| `int gamma` | `0` | `[0x5a4950]`, кламп 0..3 |
| `int reflections` | `1` | `[0x4aac84]`, ненулевое — `CHECK` |
| `int light_maps` | `1` | `[0x4aac88]`, кламп 0..2 |
| `bool fade_lut` | `false` | точный fade `0x682f90` по палитровым индексам |
| `int fade_lut_level` | `11` | 0..15; 11 — тождество, 12..15 светлеют |
| `std::string fade_lut_dump` | `""` | дамп 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | готовая таблица, приоритетнее дампа |

Публичные помощники (нужны тесту и будущему CLI):
`graphics_options_item_y(item)`, `graphics_options_item_x(item)`,
`graphics_options_border(int)`, `graphics_options_gamma(int)`,
`graphics_options_light_maps(int)`, `graphics_options_reflections_on(int)`,
`graphics_options_gamma_name(index)`, `graphics_options_light_maps_label(value)`,
`graphics_options_knob_x(border)`, `graphics_options_item_text(options, item)`.

`GraphicsOptionsRenderStats`: `rows` (`0x67f10c`: всегда 6), `checks` (рамка
`CHECK`/`NOCHECK`: 1), `sliders` (пара `SLIDE`/`KNOB` полосы `Border:`: 1),
`arrows` (`MENUARRW` на этом экране не рисуется — всегда 0), `ink_pixels`
(уникальные пиксели глифов и спрайтов; фоновая плата не считается — как в
`menu_render`/`sound_options_render`), `missing_glyphs` (байты без глифа, не
фатальны).

Контракт буфера: при отказе `rgba` очищается, `width`/`height` = 0,
статистика обнуляется; `stats`/`error` могут быть `nullptr`. Функция только
читает установку, детерминирована, не зависит от текущего каталога.

## 2. Доказано (листинг + данные)

Инструмент: `python3 tools/re/disrange.py`, данные — `tools/re/exelib.py`.
**Обработчик — `0x485ee0`** (тело состояния 12, `sub esp,0x3c`; `ret` в
`0x48604b`). `rows`/`step` — immediate-записи в `0x67f10c`/`0x662d58`.

### 2.1. Пролог, заголовок

| Факт | Где |
|---|---|
| `[0x67f10c] = 6` (rows) | `0x485ee3` |
| `[0x662d58] = 15` (step) | `0x485eed` |
| копия `PLATE` 196×131 в `0x6865f8` | `0x485ef9` → `0x486050` |
| заголовок: шрифт 5, текст `0x4bcf24`, `centered=1`, `x=38`, `row=11`, `item=-1` | `0x485efe`..`0x485f0d` |

`0x485dd0` при `item == -1` трактует `row` как абсолютный `y`, а при
`centered=1` заменяет `x` на `(196 - width)/2` (`0x485de2`..`0x485dfa`) —
поэтому `x = 38` в вызове заголовка не используется, а сам заголовок стоит на
**строке 11**. Текст пишется в копию платы, поэтому **клип — 196×131**.

Аргументы всех вызовов — cdecl справа налево: `item, row, x, centered, text,
font`; для `row >= 0` действует `y = row + item*step`, для `row < 0` —
`y = -row`.

### 2.2. Шесть пунктов — адреса, координаты, значения

| item | текст | VA | x | row | y |
|---:|---|---|---:|---:|---:|
| 0 | строка режима (`0x486500` по дескриптору `0x4c05c0`) | — | 38 | 24 | 24 |
| 1 | `Border:` | `0x4bcf1c` | 38 | 24 | 39 |
| 2 | `%s Gamma Correction` (имя `[0x4bc6d8 + [0x5a4950]*4]`) | `0x4bcf08` | 38 | 24 | 54 |
| 3 | `Reflections` | `0x4bcefc` | 50 | 24 | 69 |
| 4 | `Light Maps Off` / `Low Quality` / `High Quality` | `0x4bcebc` / `0x4bcee4` / `0x4bcecc` | 38 | 24 | 84 |
| 5 | `Done` | `0x4bceb4` | 38 | 23 | 98 |

Строка режима собирается до отрисовки: `0x485f12`..`0x485f2b` копируют 5 dword
структуры `0x4c05c0` на стек и вызывают `0x4864a0(дескриптор)`, затем
`0x486500(buf, index)` (`sprintf`) заполняет буфер, и пункт 0 печатается
(`0x485f39`..`0x485f48`). Гамма: `0x485f89` читает `[0x5a4950]`,
`0x485f96` берёт указатель из `0x4bc6d8 + idx*4` (No / Little / Some / Max —
`0x4bc7a4`/`0x4bc79c`/`0x4bc794`/`0x4bc790`), `0x40d8e0` — `sprintf` с
`0x4bcf08`. Световые карты: `0x485ff0`..`0x486027` — `value == 0` даёт
`0x4bcebc`, `value == 2` — `0x4bcecc`, иначе `0x4bcee4`.

Диспетчер пунктов (Enter/Space) — таблица `0x48e8e0` (6 входов): 0 — экран 18
(копия дескриптора, `state = 0x12`, `0x48d74d`..`0x48d784`), 1 — ничего
(`0x48d7eb`), 2 — гамма `+1` с переходом 3 → 0 (`0x48d789`), 3 — тумблер
`Reflections`, 4 — световые карты `+1`, больше 2 → 0 (`0x48d7c8`), 5 — общий
выход (`0x48e49c`, возврат к предыдущему состоянию). Esc обрабатывается общим
фронтенд-кодом, не этим обработчиком.

### 2.3. Полоса `Border:` и кноб

`0x486070(SLIDE)`/`0x4860b0(KNOB)` — маскированные блиты в копию платы (шаг
строки `0xc4` = 196), аргумент — адрес назначения, отсюда
`addr - 0x6865f8 = y*196 + x`:

| факт | адрес |
|---|---|
| `SLIDE` 72×8 в (88, 39) | `0x68842c - 0x6865f8 = 39*196 + 88`, `0x485f67` |
| `KNOB` 8×8, база `x = 89 + 6*border` | `0x68842d`, `0x485f71`..`0x485f84` |

Арифметика кноба (`0x485f79`..`0x485f84`) — без деления:

```
edx = 3 * [0x4bc1c8]          ; lea edx,[eax+eax*2]
x   = 0x68842d - 0x6865f8 + 2*edx = 89 + 6*border
```

Диапазон доказан обработчиком ввода: `cmp eax,0x9; jge` — без изменения
(`0x48c773`), уменьшение только при `value > 0` (`0x48bf12`), шаг 1. То есть
0..9 двигает кноб 89..143 внутри полосы 88..159.

### 2.4. Чекбокс `Reflections`

`0x4833e0([0x4aac84], 196, 0x6894d4)` (`0x485fc0`..`0x485fd4`), якорь
`0x6894d4 - 0x6865f8 = 61*196 + 40`, т.е. (40, 61). При ненулевом значении
копируется 16-строчный `CHECK` 8×16 в якорь; при нуле — 8-строчный `NOCHECK`
8×8 **на 8 строк ниже** (`lea ecx,[ecx+edi*8]` в `0x483403`), так что низ
рамки всегда на строке подписи `Reflections` (69). Индекс палитры 0 прозрачен
(`test dl,dl; je` в `0x483413`). Флаг — булев: тумблеры считают `1 - value`
(`sub ecx,[0x4aac84]` в `0x48d7b7`, `sub ebx,[0x4aac84]` в `0x48c7d3`).

### 2.5. Подсветка

`0x485dd0` переключает на шрифт 4, только если `item == [0x68ccf4]` **и**
вызывающий передал шрифт 0 (`0x485e0d`..`0x485e19`), поэтому заголовок (шрифт
5) не подсвечивается никогда. Полос/рамок/`MENULIT` нет; курсор — id пункта,
поэтому `-1` и 99 не подсвечивают ничего.

### 2.6. Значения по умолчанию

| поле | до загрузки CFG | поставочный CFG | где |
|---|---|---|---|
| `0x4c05c0` (дескриптор режима) | `.bss` (заполняется при инициализации видео) | 800×600, 8 bpp, 60, flags 0 | docs/CONFIG.md §3 |
| `0x4bc1c8` (border) | `3` | 3 | `.data` |
| `0x5a4950` (gamma) | `0` | 1 | `.bss` |
| `0x4aac84` (reflections) | `1` | 1 | `.data` |
| `0x4aac88` (light maps) | `1` | 2 | `.data` |
| `0x68ccf4` (курсор) | `0` | — | `.bss`; вход из главного меню (`0x48cfa0`) его не пишет |

### 2.7. Ассеты, цвета, fade

`FRONTEND/BMP.IDX` (`docs/UI_SCREENS.md` §9): `PLATE` 196×131, `SLIDE` 72×8,
`KNOB` 8×8, `CHECK` 8×16, `NOCHECK` 8×8, `FONTS4` 512×168 (грани 5/0/4).
Размеры проверяются при загрузке; расхождение — текстовый отказ.

Цвета — из палитры атласа: заголовок (шрифт 5) — (155,15,15) тёмно-красный,
пункты (шрифт 0) — (19,15,15) тёмно-серый, подсветка (шрифт 4) — (239,239,239)
белый (`docs/MENU_RENDER.md` §3). `PLATE`/`SLIDE`/`KNOB`/`CHECK`/`NOCHECK`
используют собственные палитры BMP; индекс 0 — прозрачный ключ спрайтов.

Точный fade `0x682f90`: каждый нарисованный палитровый индекс проходит через
`render/hud_lut.h::map(level, index)` **до** выборки цвета (как в
`sound_options_render`/`mouse_setup_render`). Таблица — готовая
(`fade_lut_table`) или из дампа 4096 Б (`fade_lut_dump`, `docs/HUD_LUT.md` §2);
без обеих — текстовый отказ, а не «молча без fade».

## 3. Числа (тест, реальная установка)

`./build/sandbox/engine/tests/gm_graphics_options_render_tests` — **461
проверка**, `graphics_options_render: ok (461 checks)`, игра только читается.
CTest: `graphics_options_render ... Passed`.

Кадр по умолчанию (`.data`: border 3, gamma 0, reflections 1, light maps 1,
курсор 0, режим 800×600 Glide):

```
title "GRAPHICS OPTIONS": x=55 y=11 ink=251 bbox (55,11)-(139,17) 85x7
row 0: x=38 y=24 "800x600 Glide 3Dfx"     ink=256 bbox (38,24)-(129,30)
row 1: x=38 y=39 "Border:"                ink=84  bbox (38,39)-(67,45)
row 2: x=38 y=54 "No Gamma Correction"    ink=233 bbox (38,54)-(132,60)
row 3: x=50 y=69 "Reflections"            ink=135 bbox (50,69)-(97,75)
row 4: x=38 y=84 "Light Maps Low Quality" ink=250 bbox (38,84)-(139,91)
row 5: x=38 y=98 "Done"                   ink=61  bbox (38,98)-(57,104)
slider: border=3 knob x=107 y=39, SLIDE+KNOB ink=505
check: on CHECK at (40,61) ink=91 (off NOCHECK at (40,69) ink=64)
frame: rows=6 sliders=1 checks=1 ink=1866 missing=0 arrows=0
```

`ink` — объединение: текст 1270 (заголовок 251 + шесть полос 1019) + полоса
505 + чекбокс 91 = **1866**; пересечений нет, отдельная модель `InkModel`
(глифы и спрайты помечаются независимо) даёт то же число.

Полоса: кноб `0/1/3/5/9 -> 89/95/107/119/143`; смена border 0→9 меняет **124
px** и только прямоугольник 72×8 (88..159, 39..46), 0→3 — **125 px**; кадры
сверены с независимым `SLIDE`+`KNOB` композитом. `border = -5` даёт тот же
кадр, что 0; `42` — что 9.

Чекбокс: выключен — `NOCHECK` в (40,69), ink 64; включён — `CHECK` в (40,61),
ink 91; переключение меняет **128 px** и только рамку 8×16 (40..47, 61..76);
`reflections = 7` рисует тот же `CHECK`, что 1.

Гамма: строки 1/2/3 отличаются от 0 на **297/296/279 px**, все — только в
строках 54..61 пункта 2 (`Little Gamma Correction` и т.д.). Световые карты
0/1/2 дают три разных кадра, отличия только в строках 84..91. Смена строки
режима (640×480 вместо 800×600) — **43 px**, только в строках 24..31.

Курсор: id 0..5 даёт белый шрифт 4 только в своей полосе — отличия
**256/84/233/135/250/61 px**; `-1` и 99 не подсвечивают ничего (0 отличий);
заголовок остаётся тёмно-красным.

`background="PLATE"`: 196×131 декодируется в тесте независимо, отличий под
чернилами **0**, весь кадр отличается на **23810 px**, `ink` не меняется
(1866), вне платы — чёрный.

Fade: уровень 11 — **0/64000** отличий от кадра без fade (побайтово); 12..15
отличаются от 11 на **16033/24410/24704/25522 px**, соседние пары —
**17646/22843/20692 px**; `ink` не меняется; уровень 13 детерминирован.

Негативы (все тексты `graphics_options_render:` с очищенным буфером): пустой
каталог, нет `BMP.IDX`, архив без `FONTS4`, без `PLATE`, без `SLIDE`, без
`KNOB`, без `CHECK`, без `NOCHECK`, `PLATE` 8×16, `SLIDE` 8×8, неизвестный
фон, `fade_lut` без таблицы/дампа и отсутствующий дамп. Полный синтетический
архив из шести записей рисует тот же кадр (ink=1866), два прогона побайтово
равны.

## 4. Модель (не доказано кодом)

* **Строка режима (пункт 0).** Текст собирается форматтером `0x486500` из
  записи таблицы `0x4bfd18` (stride 20, счётчик `[0x4c05bc]`), которую
  заполняют `EnumDisplayModes`/Glide при старте (`docs/INIT.md` §6.1) — из
  поставочных файлов это не воспроизводится. `0x4864a0` ищет дескриптор
  `0x4c05c0` в таблице и возвращает индекс (0, если не найден). Поэтому сам
  **форматтер** — доказанный код (`video_mode_list_label`), а **запись** —
  модель: по умолчанию `{800,600,32,0x804}` (поставочный CFG 800×600 + флаг
  Glide 3dfx-сборки). Вызывающий может передать любую запись.
* **Плата на экране.** Оригинал копирует `PLATE` в `0x6865f8` как буфер
  композиции; куда этот буфер блитится на 320×200, в документации не
  зафиксировано (`docs/MENU_RENDER.md` §2). Поэтому `PLATE` не рисуется
  неявно: `background` — выбор вызывающего, по умолчанию чёрный холст. Текст и
  спрайты клипаются к 196×131 — это доказано адресами назначения.
* **Значения по умолчанию** взяты из `.data`/`.bss` (3/0/1/1), а не из
  поставочного CFG (3/1/1/2): модуль не читает конфиг, как и соседние
  рендереры. Оба набора вынесены в константы (`…Default`, `…ShippedCfg`).
* **Курсор по умолчанию 0** — `.bss`; вход в состояние 12 его не пишет,
  поэтому «первый кадр» зависит от предыдущего экрана. API даёт нейтральные 0.
* **`x = 38` у заголовка** — аргумент вызова, который `centered=1` игнорирует;
  фактический `x` зависит от ширины строки в шрифте 5 (в поставочном
  `FONTS4` — 55).
* **Флаг `light_maps = 1`** — значение `.data`; поставочный CFG содержит 2,
  поэтому эталонный кадр из игры покажет `Light Maps High Quality`, а кадр по
  умолчанию API — `Low Quality`.

## 5. Ограничения

* Нет ввода: `border ±1`, шаги гаммы и световых карт, тумблер `Reflections`,
  `Esc`/переходы в другие состояния (в т.ч. экран 18 по пункту 0), навигация
  курсором — не реализованы и не читаются.
* Нет звука: `updown`/`vleft`/`select` (`docs/UI_SOUNDS.md` §3.2) не
  проигрываются.
* Нет `GUNMETAL.CFG`: значения не читаются и не пишутся; переданные значения
  **никуда не применяются** — это только пиксели кадра. Применение гаммы
  (`0x4847b0`), смена режима и подтверждение экрана 37 не делаются.
* **CLI — есть (срез 21).** `--graphics-options [--graphics-options-cursor N]
  [--graphics-options-border N] [--graphics-options-gamma N]
  [--graphics-options-reflections N] [--graphics-options-light-maps N]
  [--no-plate] [--png OUT] [--fade-lut N]`: по умолчанию
  `graphics_options: rows=6 sliders=1 checks=1 cursor=0 arrows=0 ink=1866
  missing=0`; border 9, gamma 3, без отражений, light maps 2, курсор 5 —
  `ink=1862`. CTest `cli_graphics_options*`; `cli_fade_lut` (11 = без fade).
* `arrows` всегда 0: в `0x485ee0` нет вызова `MENUARRW`; поле оставлено ради
  единой формы статистики соседних рендереров.
* Шрифты/палитры — только из поставочного `FONTS4`; синтетических атласов
  модуль не принимает.
