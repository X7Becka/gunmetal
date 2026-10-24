# Рендер экрана 27 `MULTIPLAY GAME OPTIONS` (`engine/src/ui/match_setup_render.h`)

Срез 9 пункта `M8.8`: один кадр экрана **27** фронтенда (`gundx.exe`, ImageBase
`0x400000`) в RGBA-буфер 320×200. Модуль без состояния — рисует то, что ему
передали в `MatchSetupRenderOptions`, и отдаёт числа (`rows`, `sliders`,
`checks`, `ink_pixels`, `missing_glyphs`, `arrows`). Шрифты, метрики и атласы —
из `render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`), точный fade — из
`render/hud_lut`. Ничего не измеряется заново.

Код: `engine/src/ui/match_setup_render.h/.cpp`; тест —
`engine/tests/test_match_setup_render.cpp` (CTest `match_setup_render`, label
`quick`). Соседние срезы — `MOUSE_SETUP_RENDER.md` (22), `RESTART_RENDER.md`
(6), `QUIT_RENDER.md` (33), `VIDEO_MODE_RENDER.md` (37), `MENU_RENDER.md`
(общая механика `0x485dd0`), `DM_RULES.md` (team/handicap/cash).

## 1. API

```cpp
#include "ui/match_setup_render.h"

gm::MatchSetupRenderOptions options;      // defaults = .data/.bss движка (см. §3.1)
options.cursor = 7;                       // [0x68ccf4], id пункта 0..7
options.match_type = 0;                   // [0x68cd84] -> 0x4bc6e8 (0..2)
options.starting_cash = 7;                // [0x4bc728] -> 0x4bc6f8 (0..11)
options.flags = gm::kMatchSetupFlagTeamplay | gm::kMatchSetupFlagHandicap;
options.background = "PLATE";             // "" = чёрный холст

std::vector<std::uint32_t> rgba;          // 0xRRGGBBAA
int width = 0, height = 0;
gm::MatchSetupRenderStats stats;
std::string error;
if (!gm::render_match_setup(game, options, rgba, width, height, &stats, &error)) {
    // error: текст с префиксом "match_setup_render: "
}
// width=320 height=200, rgba.size()==64000
// stats: rows=8 sliders=0 checks=5 ink_pixels=2239 missing_glyphs=0 arrows=0
```

| Поле `MatchSetupRenderOptions` | По умолчанию | Смысл |
|---|---|---|
| `std::string background` | `""` | запись `FRONTEND/BMP.IDX`, рисуется в (0,0); `"PLATE"` — панель экрана; пусто — чёрный холст |
| `int cursor` | `0` | `[0x68ccf4]`, **id пункта** 0..7; вне диапазона ничего не подсвечивает (`-1` — кадр без подсветки) |
| `int match_type` | `0` | `[0x68cd84]`, индекс в `kMatchSetupMatchTypes` (кламп 0..2) |
| `int starting_cash` | `4` | `[0x4bc728]`, индекс в `kMatchSetupStartingCash` (кламп 0..11); строка печатает **значение** |
| `int flags` | `0` | `[0x68cd88]`, биты `kMatchSetupFlag*` (0x01/0x04/0x10/0x20/0x40); прочие биты игнорируются |
| `bool fade_lut` | `false` | точный fade `0x682f90` по палитровым индексам |
| `int fade_lut_level` | `11` | 0..15; 11 — тождество, 12..15 светлеют |
| `std::string fade_lut_dump` | `""` | дамп 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | готовая таблица, приоритетнее дампа |

Публичные помощники (нужны тесту и будущему CLI): `match_setup_item_y(item)`,
`match_setup_item_x(item)`, `match_setup_match_type(int)`,
`match_setup_cash_index(int)`, `match_setup_starting_cash(index)`,
`match_setup_checked(flags, item)`, `match_setup_checkbox_y(item)`,
`match_setup_label(item, options)`.

`MatchSetupRenderStats`: `rows` (всегда 8), `sliders` (`SLIDE`/`KNOB` на этом
экране нет — всегда 0), `checks` (всегда 5), `ink_pixels` (уникальные пиксели
глифов и чекбоксов; фоновая плата не считается — как в
`menu_render`/`mouse_setup_render`), `missing_glyphs` (байты без глифа, не
фатальны), `arrows` (`MENUARRW` на этом экране не рисуется — всегда 0).

Контракт буфера: при отказе `rgba` очищается, `width`/`height` = 0, статистика
обнуляется; `stats`/`error` могут быть `nullptr`. Функция только читает
установку, детерминирована, не зависит от текущего каталога.

## 2. Доказано (листинг + данные)

Инструмент: `python3 tools/re/disrange.py`. **Обработчик — `0x4885c0`**
(таблица `0x4883d8`, id 27; `ui_screens.py --map` даёт `rows 8`, `step 16`,
`assets plate/check/nocheck/fonts`), тело `sub esp,0x3c` (`0x4885c0`) … `ret`
(`0x488778`).

### 2.1. Пролог и геометрия

| Факт | Где |
|---|---|
| `rows = 8` (`0x67f10c`) | `0x4885c3` |
| `step = 16` (`0x662d58`) | `0x4885cd` |
| копия `PLATE` 196×131 в `0x6865f8` | `0x4885d7` → `0x486050` |
| заголовок: шрифт 5, текст `0x4bd388`, `centered=1`, `x=38`, `row=11`, `item=-1` | `0x4885dc`..`0x4885eb` |

`0x485dd0` при `item == -1` трактует `row` как абсолютный `y`, при
`centered=1` заменяет `x` на `(196 - width)/2` (`0x485de2`..`0x485dfa`), а при
`row >= 0` кладёт текст в `y = row + 16*item` (`0x485e4b`..`0x485e5c`:
`imul ecx,[0x662d58]; add ecx,edx; … *196`), при `row < 0` — в `y = -row`
(`0x485e2b`..`0x485e38`). Поэтому `y` в таблице — вычисленный, а не immediate.

### 2.2. Все девять вызовов `0x485dd0`

| item | текст | VA / источник | x | row | y |
|---:|---|---|---:|---:|---:|
| — | `MULTIPLAY GAME OPTIONS` (шрифт 5, центр) | `0x4bd388` | (196−w)/2 | 11 | 11 |
| 0 | тип матча `%s` | `[0x4bc6e8 + [0x68cd84]*4]` (`0x4885f8`) | 23 | 22 | 22 |
| 1 | `Starting Cash: %ld` | `0x4bd374` + `[0x4bc6f8 + [0x4bc728]*4]` (`0x488623`) | 23 | 16 | 32 |
| 2 | `Teamplay` | `0x4bd368` | 35 | 13 | 45 |
| 3 | `No Enemies` | `0x4bd35c` | 35 | 12 | 60 |
| 4 | `Can't Buy Ammo` | `0x4bd34c` | 35 | 11 | 75 |
| 5 | `Handicap Better Players` | `0x4bd334` | 35 | 10 | 90 |
| 6 | `Allow Mouse Cheating` | `0x4bd31c` | 35 | 7 | 103 |
| 7 | `START GAME!` | `0x4bd310` | 23 | −113 | 113 |

В `§3.8 UI_SCREENS.md` числа 22/16/13/12/11/10/7 — это **row-иммедиаты**, а не
`y`; фактический `y` первых шести строк = `row + 16*item`, у `START GAME!`
отрицательный `row` даёт абсолютные 113.

### 2.3. Подсветка курсора

`0x485dd0` меняет шрифт вызывающего на 4 (белый) только если
`item == [0x68ccf4]` **и** вызывающий передал шрифт 0 (`0x485e0d`..`0x485e19`).
Строки 0..6 передают 0 (`0x488608`/`0x48863f`/…), поэтому курсор делает их
белыми. `START GAME!` передаёт 5 (`0x48875d`) или 0 при `[0x68ccf4] == 7`
(`0x48874b`, `0x48876b`), поэтому:

* курсор 0..6 — соответствующая строка белая, `START GAME!` тёмно-красный;
* курсор 7 — `START GAME!` белый (внутри `0x485dd0` 0 → 4);
* курсор вне 0..7 — подсветки нет, всё как при `-1`;
* полосы/рамки/`MENULIT` нет. Диапазон 0..7 закреплён обработчиком ввода
  (`cmp eax,7; ja` — `0x48dba9`).

### 2.4. Чекбоксы `0x4833e0`

Пять вызовов `0x4833e0(dst, 196, бит)`; `dst - 0x6865f8 = y*196 + x`:

| адрес | (x, y) | маска | item (текст) |
|---|---|---:|---|
| `0x688264` | (24, 37) | `0x01` | 2 `Teamplay` |
| `0x688de0` | (24, 52) | `0x04` | 3 `No Enemies` |
| `0x68995c` | (24, 67) | `0x10` | 4 `Can't Buy Ammo` |
| `0x68a4d8` | (24, 82) | `0x20` | 5 `Handicap Better Players` |
| `0x68aecc` | (24, 95) | `0x40` | 6 `Allow Mouse Cheating` |

`0x4833e0` при установленном бите копирует **16 строк** `CHECK` (8×16) из
`0x67fc10`; при снятом — **8 строк** `NOCHECK` (8×8) из `0x683fb8`, сдвинутых
на `8` строк ниже (`lea ecx,[ecx+edi*8]`, `0x483403`). Индекс 0 прозрачен
(`test dl,dl; je`, `0x483413`). Поэтому якорь = `(24, text_y − 8)`, а
`NOCHECK` совпадает по строкам с текстом. `CHECK` в поставочном BMP занимает
все 16 строк (в нижних 8 он лежит на строке текста), так что включённая
галочка доходит на 8 строк выше текста — это свойство ассета, а не модели.

Порядок отрисовки в обработчике: текст item 1 → чекбокс 1 → текст item 2 →
чекбокс 2 → … → текст item 6 → `START GAME!`; поэтому `CHECK` перекрывает
нижние строки предыдущего текста в колонках 24..31 (тест это учитывает).

### 2.5. Значения, диапазоны, умолчания

* **Тип матча** `0x68cd84`: индекс 0..2 в таблицу указателей `0x4bc6e8`
  (`Normal Deathmatch` `0x4bc77c`, `Scavenger Hunt Deathmatch` `0x4bc760`,
  `Deathmatch Classic` `0x4bc74c`); ввод заворачивает `idiv 3`
  (`0x48dbc6`). `.bss`, умолчание 0.
* **Стартовые кредиты** `0x4bc728`: индекс 0..11 в таблицу `long`
  `0x4bc6f8` = 36000, 50000, 75000, 100000, 150000, 200000, 350000, 500000,
  750000, 1000000, 2000000, 5000000; ввод заворачивает `idiv 12`
  (`0x48dbdd`). `.data`-значение 4 (150000); в поставочном `GUNMETAL.CFG`
  лежит 7 (500000, `docs/CONFIG.md` §3), загрузчик `0x484fcc` его
  перезаписывает.
* **Биты опций** `0x68cd88` (`0x488646`..`0x488710`): бит 0 `Teamplay` (0x01),
  бит 2 `No Enemies` (0x04), бит 4 `Can't Buy Ammo` (0x10), бит 5
  `Handicap Better Players` (0x20), бит 6 `Allow Mouse Cheating` (0x40) —
  порядок и смысл совпадают с `docs/CONFIG.md` §3 (`deathmatch_flags`,
  смещение `0x1dc`). `.bss`, умолчание 0.
* **Курсор входа** — 7: обе ветки перехода из экрана 19 пишут
  `[0x68ccf4] = 7` перед вызовом построителя (`0x48dcee`, `0x48dd81`), т.е.
  первый кадр подсвечивает `START GAME!`. Умолчание API — 0 (нейтральная
  первая строка, как у соседних рендереров); кадр входа — `kMatchSetupEntryCursor`.
* `sliders`/`KNOB` на экране **нет**: в `0x4885c0` нет вызовов `0x486070` и
  `0x4860b0`, ассеты `SLIDE`/`KNOB` не читаются (тест рисует архив без них).

## 3. Модель

1. **Холст** 320×200, непрозрачный `#000`; опционально `background`
   (`FRONTEND/BMP.IDX`) в (0,0), клип по холсту.
2. **Плата.** `PLATE` (196×131) загружается и проверяется всегда: обработчик
   копирует её в `0x6865f8` (`0x486050`), и без ассета кадр пуст. Куда движок
   блитит этот буфер на 320×200, не задокументировано
   (`docs/MENU_RENDER.md` §2), поэтому панель не рисуется неявно: `background
   = "PLATE"` кладёт её в (0,0), пусто — чёрный холст.
3. **Клип текста и спрайтов** — 196×131 (окно записи `0x485dd0`).
4. **Шрифты**: заголовок 5 (`155,15,15`), строки 0 (`19,15,15`), подсветка 4
   (`239,239,239`) — цвета палитры `FONTS4` (docs/MENU_RENDER.md §3).
   `START GAME!` — шрифт 5, кроме курсора 7 (шрифт 4).
5. **Чекбоксы**: `CHECK` 8×16 в `(24, y−8)` или `NOCHECK` 8×8 в `(24, y)`;
   индекс 0 спрайта прозрачен, `ink_pixels` считает уникальные пиксели.
6. **Клампы** (не отказ): тип 0..2, кредиты 0..11, курсор вне 0..7 ничего не
   подсвечивает, неизвестные биты `flags` игнорируются. Оригинал держит
   индексы в диапазоне сам (`idiv 3`, `idiv 12`), поэтому отрицательные и
   большие значения кадр не ломают.
7. **Точный fade** `0x682f90`: индекс палитры каждого нарисованного пикселя
   проходит через `render/hud_lut.h::map(level, index)` **до** выборки цвета;
   уровень 11 — тождество, 12..15 — реальные таблицы. Нужна таблица
   (`fade_lut_table`) или дамп 4096 Б (`fade_lut_dump`), иначе отказ.
8. **Отказы** (текст с префиксом `match_setup_render:`): пустой каталог игры,
   нет/битый `FRONTEND/BMP.IDX`, нет шрифта 5/0/4, нет `PLATE` или размер не
   196×131, нет `CHECK` (не 8×16), нет `NOCHECK` (не 8×8), нет `background`-
   записи/не 8 bpp, `fade_lut` без таблицы и дампа, нечитаемый дамп.

## 4. Числа (поставочная установка, тест `match_setup_render`)

Кадр с умолчаниями API (`cursor = 0`, тип 0, кредиты 4, флаги 0, холст чёрный):

| величина | значение |
|---|---|
| `rows` / `sliders` / `checks` / `arrows` / `missing_glyphs` | 8 / 0 / 5 / 0 / 0 |
| `ink_pixels` | **2239** = текст 1919 + чекбоксы 320 |
| заголовок (шрифт 5, центр) | x 37, y 11; bbox (37,11)-(157,17), 327 px |
| item 0 `Normal Deathmatch` | (23,22), ink 222, bbox (23,22)-(107,28) |
| item 1 `Starting Cash: 150000` | (23,32), ink 260, bbox (23,32)-(120,39) |
| item 2 `Teamplay` | (35,45), ink 110, bbox (35,45)-(74,52) |
| item 3 `No Enemies` | (35,60), ink 134, bbox (35,60)-(84,66) |
| item 4 `Can't Buy Ammo` | (35,75), ink 175, bbox (35,75)-(106,82) |
| item 5 `Handicap Better Players` | (35,90), ink 273, bbox (35,90)-(139,97) |
| item 6 `Allow Mouse Cheating` | (35,103), ink 249, bbox (35,103)-(129,110) |
| item 7 `START GAME!` | (23,113), ink 169, bbox (23,113)-(83,119) |
| значение типа 0 → 2 | 269 px разницы (только строка y 22..29) |
| кредиты 4 → 11 | 190 px разницы (только строка y 32..39) |
| чекбоксы вкл/выкл (по одному) | 127 / 128 / 128 / 128 / 124 px |
| все 5 включены vs выключены | 587 px; ink 2239 → 2362 |
| `CHECK` (вкл) / `NOCHECK` (выкл) | 91 / 64 px на коробку |
| курсор 0 / 6 / 7 vs без подсветки | 222 / 249 / 169 px |
| `background=""` vs `"PLATE"` | 23437 px; ink не меняется |
| fade 11 vs без fade | **0 / 64000** px |
| fade 12/13/14/15 vs 11 | 15843 / 24135 / 24409 / 25560 px |
| fade 12↔13↔14↔15 | 17346 / 22354 / 20806 px |
| архив `FONTS4`+`PLATE`+`CHECK`+`NOCHECK` без `SLIDE`/`KNOB` | рисуется, ink 2239 |

Тест печатает эти числа; `docs/MATCH_SETUP_RENDER.md` их фиксирует. Проверки
негативов: пустой путь, каталог без `BMP.IDX`, архив без `FONTS4`, без
`PLATE`, без `CHECK`, без `NOCHECK`, `PLATE` 8×16 вместо 196×131, неизвестный
`background`, `fade_lut` без таблицы/дампа, отсутствующий дамп — все с
очищенным буфером и префиксом `match_setup_render:`. Два прогона совпадают
побайтово.

## 5. Ограничения

* Только кадр: нет ввода (стрелки/клики, `Esc` → состояние 5 `0x48db8c`,
  `START GAME!` → `0x48b0c0`), звуков интерфейса, анимаций и таймеров.
* Настройки **не пишутся**: модуль не трогает `GUNMETAL.CFG`
  (`0x1d4`/`0x1d8`/`0x1dc`) и не вызывает загрузчик/писатель.
* Нет преобразования индекса типа в `game_mode` (`0x48b0c0`: 0→1, 1→3, 2→2,
  `docs/DM_RULES.md` §2.2) и нет применения `mp_flags`
  (`docs/DM_RULES.md` §5–6) — это дело матча, не кадра.
* **CLI — есть (срез 12).** `--match-setup [--match-setup-cursor N]
  [--match-setup-type N] [--match-setup-cash N] [--match-setup-flags HEX]
  [--no-plate] [--png OUT] [--fade-lut N]`: по умолчанию
  `match_setup: rows=8 sliders=0 checks=5 flags=0x00 cursor=7 ink=2239
  missing=0 arrows=0`; все пять галочек (`0x75`) — `ink=2362`. CTest
  `cli_match_setup`, `_checks`, `_png`; `cli_fade_lut` (11 = без fade).
* `SLIDE`/`KNOB` не читаются, `MENUARRW` не рисуется; `arrows` всегда 0.
* Положение платы на экране (196×131 внутри 320×200) не доказано — поэтому
  `background` явный.
* `ink_pixels` — уникальные пиксели глифов/спрайтов; фоновая плата не в счёте
  (то же правило, что в `menu_render`/`mouse_setup_render`).
