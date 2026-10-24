# Рендер экрана 22 `MOUSE SETUP` (`engine/src/ui/mouse_setup_render.h`)

Срез 8 пункта `M8.8`: один кадр экрана **22** фронтенда (`gundx.exe`, ImageBase
`0x400000`) в RGBA-буфер 320×200. Модуль без состояния — рисует то, что ему
передали в `MouseSetupRenderOptions`, и отдаёт числа (`rows`, `sliders`,
`ink_pixels`, `missing_glyphs`, `arrows`). Шрифты, метрики и атласы — из
`render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`), точный fade — из
`render/hud_lut`. Ничего не измеряется заново.

Код: `engine/src/ui/mouse_setup_render.h/.cpp`; тест —
`engine/tests/test_mouse_setup_render.cpp` (CTest `mouse_setup_render`, label
`quick`). Документ фиксирует «доказано / модель / числа / ограничения»;
соседние срезы — `RESTART_RENDER.md` (6), `QUIT_RENDER.md` (33),
`VIDEO_MODE_RENDER.md` (37), `MENU_RENDER.md` (общая механика `0x485dd0`).

## 1. API

```cpp
#include "ui/mouse_setup_render.h"

gm::MouseSetupRenderOptions options;   // defaults = .data/.bss движка (см. §2.3)
options.cursor = 1;                    // [0x68ccf4], id пункта 0..11
options.used_by = 1;                   // [0x5a4938] -> "Player 1"
options.axes = {1, 6, 8};              // [0x68cd70/74/78] -> 0x4bc468
options.sensitivity = {10, 8, 8};      // [0x4aac38/40/48] в десятых: 2..22
options.buttons = {0, 1, 5, -1};       // [0x4aac10..1c] -> 0x4bc558, -1 = NOT USED
options.background = "PLATE";          // "" = чёрный холст

std::vector<std::uint32_t> rgba;       // 0xRRGGBBAA
int width = 0, height = 0;
gm::MouseSetupRenderStats stats;
std::string error;
if (!gm::render_mouse_setup(game, options, rgba, width, height, &stats, &error)) {
    // error: текст с префиксом "mouse_setup_render: "
}
// width=320 height=200, rgba.size()==64000
// stats: rows=12 sliders=3 ink_pixels=3986 missing_glyphs=0 arrows=0
```

| Поле `MouseSetupRenderOptions` | По умолчанию | Смысл |
|---|---|---|
| `std::string background` | `""` | запись `FRONTEND/BMP.IDX`, рисуется в (0,0); пусто — чёрный холст |
| `int cursor` | `0` | `[0x68ccf4]`, **id пункта** 0..11; вне диапазона ничего не подсвечивает (`-1` — кадр без подсветки) |
| `int used_by` | `0` | `[0x5a4938]`, индекс в `kMouseSetupUsedByNames` (0..2) |
| `std::array<int,3> axes` | `{0,0,0}` | `[0x68cd70/74/78]`, индексы в `kMouseSetupAxisNames` (0..17) |
| `std::array<int,3> sensitivity` | `{10,8,8}` | `[0x4aac38/40/48]` в **десятых**, кламп 2..22 |
| `std::array<int,4> buttons` | `{0,1,5,15}` | `[0x4aac10..1c]`, индексы в `kMouseSetupKeyNames` (0..30), `-1` = `NOT USED` |
| `bool fade_lut` | `false` | точный fade `0x682f90` по палитровым индексам |
| `int fade_lut_level` | `11` | 0..15; 11 — тождество, 12..15 светлеют |
| `std::string fade_lut_dump` | `""` | дамп 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | готовая таблица, приоритетнее дампа |

Публичные помощники (нужны тесту и будущему CLI):
`mouse_setup_item_y(item)`, `mouse_setup_item_x(item)`,
`mouse_setup_sensitivity_tenths(int)`, `mouse_setup_knob_x(tenths)`,
`mouse_setup_label(item, options)`.

`MouseSetupRenderStats`: `rows` (всегда 12), `sliders` (всегда 3), `ink_pixels`
(уникальные пиксели глифов и спрайтов; фоновая плата не считается — как в
`menu_render`/`restart_render`), `missing_glyphs` (байты без глифа, не фатальны),
`arrows` (`MENUARRW` на этом экране не рисуется — всегда 0).

Контракт буфера: при отказе `rgba` очищается, `width`/`height` = 0, статистика
обнуляется; `stats`/`error` могут быть `nullptr`. Функция только читает
установку, детерминирована, не зависит от текущего каталога.

## 2. Доказано (листинг + данные)

Инструмент: `python3 tools/re/disrange.py`, данные — `tools/re/exelib.py`.
**Обработчик — `0x489a10`** (таблица `0x4883d8`, id 22; `ui_screens.py --map`
даёт `rows 12`, `step 8`, `assets plate/slide/knob/fonts`). Указанный в задании
`0x488a80` — середина обработчика SOUND OPTIONS (11): в нём `Music Volume:`
`0x4bd508` и `Effects Volume:` `0x4bd4f8`, см. `MENU_RENDER.md` §3.5.

### 2.1. Пролог и геометрия

| Факт | Где |
|---|---|
| `sub esp,0x3c` … `ret 0x489d57` | тело обработчика 22 |
| `rows = 12` (`0x67f10c`) | `0x489a13` |
| `step = 8` (`0x662d58`) | `0x489a1d` |
| копия `PLATE` 196×131 в `0x6865f8` | `0x489a27` → `0x486050` |
| заголовок: шрифт 5, текст `0x4bd8c0`, `centered=1`, `x=18`, `row=11`, `item=-1` | `0x489a2c`..`0x489a3b` |

`0x485dd0` при `item == -1` трактует `row` как абсолютный `y`, а при
`centered=1` заменяет `x` на `(196 - width)/2` (`0x485de2`..`0x485dfa`) —
поэтому `x = 18` в вызове заголовка не используется, а сам заголовок стоит на
**строке 11** (у соседних экранов 14 или 10). Текст пишется в копию платы,
поэтому **клип — 196×131**.

Все 12 пунктов вызываются с `row = 0x18` (24) и `item = 0..11`;
`y = 24 + 8*item`:

| item | текст | VA | x | y | источник `%s` |
|---:|---|---|---:|---:|---|
| 0 | `Used By: %s` | `0x4bd8b4` | 18 | 24 | `[0x4bc548 + [0x5a4938]*4]` (`0x489a4c`) |
| 1 | `Left/Right: %s` | `0x4bd8a4` | 9 | 32 | `[0x4bc468 + [0x68cd70]*12]` (`0x489a85`) |
| 2 | `Sensitivity:` | `0x4bd894` | 9 | 40 | — |
| 3 | `Forward/Back: %s` | `0x4bd880` | 9 | 48 | `[0x4bc468 + [0x68cd74]*12]` (`0x489b09`) |
| 4 | `Sensitivity:` | `0x4bd894` | 9 | 56 | — |
| 5 | `Up/Down (3D): %s` | `0x4bd86c` | 9 | 64 | `[0x4bc468 + [0x68cd78]*12]` (`0x489b8d`) |
| 6 | `Sensitivity:` | `0x4bd894` | 9 | 72 | — |
| 7 | `Button 1: %s` / `Button 1: NOT USED` | `0x4bd85c` / `0x4bd848` | 9 | 80 | `[0x4bc558 + [0x4aac10]*4]` (`0x489c0e`) |
| 8 | `Button 2: %s` / `Button 2: NOT USED` | `0x4bd838` / `0x4bd824` | 9 | 88 | `[0x4aac14]` (`0x489c5c`) |
| 9 | `Button 3: %s` / `Button 3: NOT USED` | `0x4bd814` / `0x4bd800` | 9 | 96 | `[0x4aac18]` (`0x489caa`) |
| 10 | `Button 4: %s` / `Button 4: NOT USED` | `0x4bd7f0` / `0x4bd7dc` | 9 | 104 | `[0x4aac1c]` (`0x489cf8`) |
| 11 | `Done` | `0x4bceb4` | 9 | 112 | — |

Ветка `test eax,eax; jl` (`0x489c0a`, `0x489c58`, `0x489ca6`, `0x489cf4`)
выбирает `NOT USED`, если индекс отрицателен.

Данные таблиц прочитаны из `.data` (все три — массивы указателей):

* `0x4bc548` — 3 строки: `Not Used` (`0x4bca50`), `Player 1` (`0x4bc944`),
  `Player 2` (`0x4bc938`);
* `0x4bc468` — 18 записей по 12 байт (имя + два dword): `Not Used`,
  `Forward/Back`, `Accel Turn L/R`, `Accel Turn R/L`, `Linear Turn L/R`,
  `Linear Turn R/L`, `Strafe L/R`, `Strafe R/L`, `Look Up/Down`, `Look Down/Up`,
  `Speed +/- Slide`, `Speed -/+ Slide`, `Speed +/- Nudge`, `Speed -/+ Nudge`,
  `Weapons +/-`, `Weapons -/+`, `Equip Cycle +/-`, `Equip Cycle -/+`;
* `0x4bc558` — 31 имя действия (`Move Forward` … `Linear Right`,
  `UI_SCREENS.md` §12.1), индекс 0..30.

### 2.2. Слайдеры — три пары `SLIDE`/`KNOB`

`0x486070(SLIDE)` и `0x4860b0(KNOB)` — маскированные блиты **одного** спрайта
в копию платы (шаг `0xc4` = 196), аргумент — адрес назначения внутри
`0x6865f8`. Отсюда `addr - 0x6865f8 = y*196 + x`:

| # | `SLIDE` (72×8) | `KNOB` база (8×8) | вызовы |
|---:|---|---|---|
| 1 | (105, 40) `0x688501` | x = 108 `0x688504` | `0x489ac9`, `0x489af5` |
| 2 | (105, 56) `0x689141` | x = 108 `0x689144` | `0x489b4d`, `0x489b79` |
| 3 | (105, 72) `0x689d81` | x = 108 `0x689d84` | `0x489bd1`, `0x489bfd` |

Позиция кноба (одинаково для трёх осей, `0x489ad3`..`0x489af5`):

```
eax = [0x4aac38]                     ; double, чувствительность
eax = eax - [0x4a2930]               ; 0.2 = минимум
eax = eax * [0x4a2948]               ; -27.0 = пикселей на единицу
eax = 0x40e0e0(eax)                  ; fistp с RC = truncate (or ah,0x0c)
edx = 0x688504 - eax                 ; база 108 минус отрицательное смещение
0x4860b0(edx)                        ; KNOB
```

То есть `x = 108 + trunc(27*(v-0.2))`: кноб едет **вправо** при росте значения
(множитель отрицательный, а сайт использует `sub`), а не справа налево —
формулировка `MENU_RENDER.md` §3.5 про «реверс» неточна.

Диапазон и шаг доказаны обработчиками кликов:

| факт | адрес |
|---|---|
| минимум `0.2` (`0x4a2930`), при `<` — сброс в 0.2 | `0x48c371`..`0x48c398` |
| максимум `2.2` (`0x4a2970`), при `>` — сброс в 2.2 | `0x48cc85`..`0x48ccb3` |
| шаг `±0.1` (`0x4a2960` = 0.1, `0x4a2968` = -0.1) | `0x48c371`, `0x48cc85` |
| сетка 21 позиция: 0.2, 0.3, … 2.2 | `108`, `110`, … `162` |

Кноб (8 px) при `x = 162` занимает 162..169 и остаётся внутри дорожки
105..176; путешествие — 54 px. Значения хранятся как `double`, но собственный
шаг движка — 0.1, поэтому API несёт **десятые** (2..22): каждая достижимая
позиция — целое число десятых. Проверено тестом: для всех 21 позиций целочисленная
формула `108 + (t-2)*27/10` совпадает с x87-путём (2/8/10/12/22 -> 108/124/129/135/162).

`MENU_RENDER.md` §3.5 откладывал MOUSE SETUP, потому что «нужен флаг реверс и
`double`»: реверса нет, а `double` с шагом 0.1 сводится к десятым.

### 2.3. Значения по умолчанию

| поле | до загрузки CFG | где |
|---|---|---|
| `0x4aac38/40/48` (чувствительность) | `1.0` / `0.8` / `0.8` | `.data` (перезапись — `0x4850fc/0x485142/0x48517f`, кламп 0.2 — `0x485272`..) |
| `0x4aac10..1c` (кнопки 1..4) | `0`, `1`, `5`, `15` | `.data` = `Move Forward`, `Move Backward`, `Fire`, `Activate` |
| `0x5a4938` (`Used By`) | `0` = `Not Used` | `.bss` (в файле нет; первая запись — `0x48502b`) |
| `0x68cd70/74/78` (оси) | `0` = `Not Used` | `.bss` (первая запись — `0x485042/0x485056/0x485074`) |
| `0x68ccf4` (курсор) | не пишется при входе в 22 | вход `0x48d292`; значение — модель `0` |

Поэтому кадр по умолчанию: три дорожки с кнобами в `129/124/124`, «Used By:
Not Used», три `Sensitivity:`, три `… : Not Used`, четыре кнопки
`Move Forward/Move Backward/Fire/Activate`, подсвечен пункт 0.

### 2.4. Курсор, подсветка, выход

* Подсветка — переключение грани в `0x485dd0`: если `item == [0x68ccf4]` и
  вызывающий передал шрифт 0, грань становится 4 (`0x485e0d`..`0x485e19`).
  Полос/рамок/`MENULIT` нет.
* Курсор — **id пункта**: таблица кликов `0x48e8b0` (12 входов) обрабатывает
  только `0..11` (`cmp eax,0xb; ja` в `0x48d507`), т.е. «Done» — item 11.
* Выход: `Done` (item 11 → `0x48e49c`) и `Esc` (`0x48e599`) пишут
  `[0x4bc440] = 10` (CONTROL OPTIONS). Строки `ESC TO CANCEL` на этом экране
  нет — последний пункт `Done`.
* Стрелки `MENUARRW` обработчик не вызывает: `arrows == 0`.

### 2.5. Ассеты

`FRONTEND/BMP.IDX` (§9): `PLATE` 196×131 (текст), `SLIDE` 72×8, `KNOB` 8×8,
`FONTS4` (грани 5/0/4). Отдельного полноэкранного арта у экрана 22 нет.

## 3. Модель (и чем она отличается от факта)

* **Позиция кноба** — целочисленная формула §2.2. `ui/menu_render.cpp::slider_knob_x`
  (`engine/src/ui/menu_render.cpp:305`), которую задание предлагало
  переиспользовать, — `static` в анонимном `namespace`, снаружи единицы
  трансляции недоступна, а `ui/menu_render.*` в этом срезе править запрещено.
  Поэтому правило интерполяции повторено с константами, доказанными для
  экрана 22; оно **не** совпадает с §3.5-моделью: `x + value*(72-8)/(max-min)`
  дала бы 105/169 на концах, а исходник даёт 108/162 (база 108, ход 54 px).
  В коде и тесте закреплены числа исходника.
* **Значения в десятых** — движок хранит `double`; шаг 0.1 и все достижимые
  значения целые в десятых, поэтому API принимает `int` (2..22). Кламп, а не
  отказ: экран остаётся рисуемым.
* **Клампы индексов** (`used_by` 0..2, `axes` 0..17, `buttons` -1..30) —
  модель: оригинал циклически оборачивает свои индексы (`idiv 3` в `0x48c31b`,
  `cmp eax,0x11` в `0x48c536`, `cmp eax,0x1f` в `0x48ce48` и
  `cmp eax,0xffffffff` в `0x48c4c8`), но в кадре это неотличимо от клампа на
  границах.
* **Цвета** — из палитр атласов/спрайтов: заголовок (грань 5) `(155,15,15)`,
  пункты (0) `(19,15,15)`, подсветка (4) `(239,239,239)`; `PLATE`/`SLIDE`/`KNOB`
  — свои палитры BMP (индекс 0 у спрайтов прозрачный).
* **Плата** — `PLATE` рисуется только по `options.background = "PLATE"` и в
  (0,0). Исходник копирует её в `0x6865f8` как буфер композиции (`0x486050`),
  но экранная позиция этой копии не доказана (`MENU_RENDER.md` §2), поэтому
  неявной платы нет — та же конвенция, что у 6/13/14/17/19/21/23/33/37.
* **Точный fade** — `fade_lut`/`fade_lut_level`/`fade_lut_dump`/`fade_lut_table`
  как в `restart_render`: индекс каждого пикселя проходит `map(level, index)`
  **до** выбора цвета; без таблицы и дампа — отказ.

## 4. Числа (`ctest -R '^mouse_setup_render$'`)

```
  mouse_setup row  0: x=18 y=24 "Used By: Not Used" bbox (18,24)-(98,31) 81x8, 208 px
  mouse_setup row 11: x=9 y=112 "Done" bbox (9,112)-(28,118) 20x7, 61 px
  mouse_setup title: x=65 y=11 bbox (65,11)-(129,17) 65x7, 180 px
  mouse_setup frame: rows=12 sliders=3 ink=3986 (text 2471 + sliders 1515) missing=0 arrows=0
  mouse_setup slider 1: track (105,40) 72x8, knob x=129 (1.0)
  mouse_setup slider 2: track (105,56) 72x8, knob x=124 (0.8)
  mouse_setup slider 3: track (105,72) 72x8, knob x=124 (0.8)
  mouse_setup knob value  2 (0.2): x=108, exact mismatch 0, shifted mismatch 150
  mouse_setup knob value 12 (1.2): x=135, exact mismatch 0, shifted mismatch 159
  mouse_setup knob value 22 (2.2): x=162, exact mismatch 0, shifted mismatch 159
  mouse_setup slider travel: 0.2 vs 2.2 diff=381 px (54 px per knob, 3 sliders)
  mouse_setup cursor  0: band y=24..31, diff vs no highlight=208 px
  mouse_setup cursor  1: band y=32..39, diff vs no highlight=228 px
  mouse_setup cursor 11: band y=112..119, diff vs no highlight=61 px
  mouse_setup plate: 196x131 decoded, black vs PLATE diff=21690 px, text ink 3986 unchanged
  mouse_setup negatives: rows=12 sliders=3 ink=3986 arrows=0 missing=0
  fade: level 11 vs no fade: 0/64000 px differ; adjacent 12..15 diffs 16952/21281/20571; vs 11: 14832/23385/23756/25560
mouse_setup_render: ok (271 checks) screen 22 -> ...
```

Что проверяется:

* **модель** — 12/8/24, x 18/9, заголовок 11, `SLIDE` (105, 40/56/72), база
  кноба 108, диапазон 2..22 и 27 px/ед., клампы, монотонность 21 позиции,
  кноб внутри дорожки; тексты всех 12 пунктов из трёх таблиц (`0x4bc548`,
  `0x4bc468`, `0x4bc558`) и `NOT USED` при `-1`;
* **кадр** — 320×200, все пиксели непрозрачны, `rows=12`, `sliders=3`,
  `arrows=0`, `missing=0`; заголовок по центру на y 11 (bbox `(65,11)-(129,17)`,
  180 px), первый пункт `(18,24)-(98,31)` (208 px), последний `Done`
  `(9,112)-(28,118)` (61 px); каждая из 12 строк и заголовок побайтово равны
  независимому блиту; `ink_pixels = 3986` = текстовые полосы (2471) + три
  прямоугольника слайдеров (1515);
* **слайдеры** — каждый прямоугольник 72×8 равен независимой композиции
  `SLIDE`+`KNOB` (0 расхождений); кноб при 0.2/1.2/2.2 стоит в 108/135/162
  (композиция со сдвигом на 1 px не совпадает: 150/159/159 px), кадры 0.2 и
  2.2 отличаются на 381 px;
* **курсор** — 0/1/11 меняют только свою полосу (font 4, белый `EFEFEFFF`:
  208/228/61 px), `-1` и `99` подсветку не рисуют (0 отличий), кадры попарно
  различны;
* **плата** — `PLATE` 196×131 декодируется независимо, под чернилами совпадает
  пиксель в пиксель, вне полосы — чёрный, `ink` не меняется, отличие от
  чёрного кадра 21 690 px;
* **fade** — уровень 11 = 0/64 000 отличий от кадра без fade; 12..15 различны
  (соседние 16 952/21 281/20 571, против 11 — 14 832/23 385/23 756/25 560),
  вне `PLATE` чёрно, `ink` не меняется, повтор детерминирован;
* **негативы** — пустой путь, каталог без `BMP.IDX`, архив без `FONTS4`, архив
  с `FONTS4` без `SLIDE`, архив с `FONTS4`+`SLIDE` без `KNOB`, неизвестный
  `background`, `fade_lut` без таблицы/дампа, несуществующий дамп — все с
  префиксом `mouse_setup_render:` и очищенным буфером; два прогона побайтово
  равны.

Полный оракул: `python3 regress.py --mirror --json` → `115/115` (97.8 с).

## 5. Ограничения

* **CLI — есть (срез 8), ввода и звука нет.** `--mouse-setup
  [--mouse-setup-cursor N] [--no-plate] [--png OUT] [--fade-lut N]`:
  `mouse_setup: rows=12 sliders=3 cursor=0 ink=3986 missing=0 arrows=0`;
  курсор 11 — `cursor=11`. CTest `cli_mouse_setup`, `_png`;
  `cli_fade_lut` (11 = без fade). Клики (`0x48e8b0`), `Esc`/`Done`
  (переход в состояние 10) и UI-звуки по-прежнему не моделируются.
* **Настройки не читаются и не пишутся.** `GUNMETAL.CFG`/`KEYCFG` не
  открываются: `used_by`, оси, кнопки и чувствительности приходят из
  `MouseSetupRenderOptions` (значения по умолчанию — `.data`/`.bss`, §2.3).
  Раунд-трип конфига — отдельный срез (`config.*`).
* **Чувствительность в десятых.** Движок хранит `double`; модель шага 0.1
  бит-в-бит совпадает на всех 21 достижимой позиции, но произвольное `double`
  (например, накопленное сотней прибавлений 0.1) в API не выразить.
* **`menu_render::slider_knob_x` не переиспользована** — см. §3: функция
  `static` внутри `menu_render.cpp`, модуль в этом срезе не менялся; вместо
  неё — доказанная арифметика экрана 22.
* **Курсор по умолчанию — модель.** При входе в состояние 22 `[0x68ccf4]` не
  инициализируется (`0x48d292`), «первого кадра» в исходнике нет; 0 — первый
  пункт.
* **Позиция копии `PLATE` не доказана**, поэтому плата рисуется в (0,0) и
  только по запросу (`background = "PLATE"`).
* **Текст — литералы таблиц.** `%s`-значения подставляются из индексов
  (таблицы `.data` целиком в модуле); имена джойстиков/мыши из `0x4bc548`
  берутся как есть (`Not Used`/`Player 1`/`Player 2`), локализации нет.
* **Точный fade — по палитре источника** (плата/спрайт/атлас), как в
  `menu_render`/`restart_render`; чёрный холст индекса не имеет и остаётся
  чёрным на всех уровнях.
* **Модельные оговорки** помечены в коде: клампы индексов, курсор 0, позиция
  платы, десятые.

## 6. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_mouse_setup_render_tests -j 4
ctest --test-dir build/sandbox -R '^mouse_setup_render$' --output-on-failure
python3 regress.py --mirror --json      # {"passed": 115, "total": 115, ...}
```
