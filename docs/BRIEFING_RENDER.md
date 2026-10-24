# PNG-кадр брифинга: `SML/brief/<level>.SME` -> 320×200 RGBA

Модуль `engine/src/ui/briefing_render.h/.cpp` — **склейка** индекса брифингов и
рендера страницы пагера: он находит и раскладывает `.sme` через
[`ui/briefing`](BRIEFING.md) (тот же поиск, декодирование и `SmlPager`), выбирает
одну страницу и отдаёт её в
[`ui/pager_render`](PAGER_RENDER.md), который рисует 320×200 кадр
(`0xRRGGBBAA`) с глифами из `FRONTEND/BMP.IDX`. Своего layout'а, своих шрифтов
и своей модели страницы здесь нет.

Это срез 3 пункта `M8.3`. Срез 1 — индекс и текст ([BRIEFING.md](BRIEFING.md));
срез 2 — CLI `--briefing`; пагер — [SML_PAGER.md](SML_PAGER.md); рендер
страницы, фон и fade — [PAGER_RENDER.md](PAGER_RENDER.md).

## 1. Доказано (числа и паритет)

* **Числа брифинга — из индекса, картинка — из пагера.** `render_briefing`
  вызывает `load_briefing()` (файл, декодирование `parse_sml`, страницы
  `SmlPager`), проверяет номер страницы, второй раз открывает **тот же**
  `briefing.source` пагером и вызывает `render_page(pager, game, ...)`.
  Поэтому `stats.pages` — `Briefing::page_count()` (всего страниц брифинга), а
  `lines/glyphs/missing_glyphs/ink_pixels/background_pixels` — поля
  `PageRenderStats` выбранной страницы. Новых чисел модуль не считает.
* **Паритет с прямым `render_page`: 0 отличающихся пикселей.** Тест на
  `level1` стр. 1 строит кадр двумя путями — `render_briefing()` и
  `load_briefing` → `SmlPager::open(source)` → `show_page(0)` →
  `render_page(pager, game, …)` — и сравнивает буферы побайтово. Совпали и
  байты (0 из 64 000), и `lines/glyphs/ink` (15 / 719 / 7214). Значит слой не
  добавляет ни фона, ни сдвига, ни второго шрифта.
* **Корпус 25/25.** Каждая страница каждого брифинга отрендерена: 58 страниц,
  727 строк, 33 284 глифа, `missing_glyphs=0`, `ink>0` и `ink<64000` на всех
  58 кадрах; суммы совпадают с таблицей [BRIEFING.md](BRIEFING.md) §4.
* **Страницы различаются.** `level0a` стр. 1/2/3: 10/6/6 строк, 180/87/91
  глиф, ink 2214/1334/1370; попиксельные diff'ы 1–2 = 3216, 2–3 = 2333,
  1–3 = 3279. Кадр — не константа и не «первая страница всегда».
* **Фон** `--background` — это ровно слой пагера `HELPSCRN`
  ([PAGER_RENDER.md](PAGER_RENDER.md) §2.4): `background_pixels=64000`,
  меняются 61 786 пикселей кадра, `ink` остаётся 2214 (текст тот же).
* **Fade** — кривая пагера (§2.5): уровень 11 побайтово равен кадру без fade
  (0 отличий), уровень 0 — чёрный кадр (64 000/64 000 = `0x000000FF`).
* **Точный fade (срез 7)** — таблицы `0x682f90` по палитровому индексу
  ([PAGER_RENDER.md](PAGER_RENDER.md) §2.6), проброшенные через
  `BriefingRenderOptions`: на `level1` стр. 1 + `--background` уровень 11 —
  **0 отличий** из 64 000 от кадра без fade (и через таблицу в памяти, и через
  дамп), уровень 0 — 63 992 чёрных против 64 000 у модели (8 пикселей
  расходятся), 12..15 — четыре разных кадра. Числа и API — §8.
* **Детерминизм.** Два вызова `render_briefing` на `level1` стр. 1 дают
  идентичный буфер и те же `lines/glyphs/ink`.
* **Отказы — текст, а не пустая картинка.** Все негативы возвращают `false`,
  префикс `briefing_render:`, очищенный `rgba` (`size()==0`), `width=height=0`
  и обнулённые `stats`; проверены: `level0` (файла нет — миссия 0 поставляется
  как `level0a`), `page=0`, `page=99`, пустой/несуществующий `game`, `game` без
  `SML/brief`, `game`-файл, имя-путь, `.sme` без `FRONTEND/BMP.IDX`, битые байты
  (`sml: no tags`), пустой файл.
* **Только чтение.** Установка не меняется; тест читает реальный `SML/brief` и
  `FRONTEND/BMP.IDX`, синтетические негативы живут во временном каталоге.

## 2. Модель (что пришлось решить)

1. **Нумерация страниц — 1-based.** `BriefingRenderOptions::page` = 1 — первая
   страница, `stats.pages` — последняя; 0, отрицательное и `> pages` — отказ
   `briefing_render: no page N (1..M)`. Так же нумерует `--page N` у пагера
   ([PAGER_RENDER.md](PAGER_RENDER.md) §3), и так же тест формулирует негатив
   `page=0`. Поле по умолчанию — 0 (структура должна оставаться
   value-initialisable), поэтому **забытое поле — это отказ, а не «первая
   страница»**: вызывающий обязан сказать `page = 1` явно.
2. **`show_page(0)` — не ошибка.** Свежеоткрытый пагер уже стоит на странице 0,
   а `show_page()` возвращает `true` только при реальном переходе
   ([SML_PAGER.md](SML_PAGER.md) §1), поэтому модуль вызывает его лишь когда
   `page - 1 != pager.page()`. Иначе первая страница ошибочно считалась бы
   отказом (этот баг был в первой версии и поймал тест).
3. **Фон — `HELPSCRN`, как у `--pager`.** `briefing.background` включает
   `PageRenderOptions::background` с `kPagerBackgroundAsset`. У брифинга в
   оригинале есть свой экран `BRIEFBG2` (320×200, грузится в тот же буфер
   `0x6d8fc0`, [UI_SCREENS.md](UI_SCREENS.md) §9), но привязка `BRIEFBG2` к
   брифинг-экрану не доказана, а срез по заданию повторяет пагер. Это
   **открытый вопрос**, а не утверждение, что игра рисует `HELPSCRN` под
   брифингом.
4. **Fade — модель пагера, не новая.** `fade`/`fade_level` пробрасываются в
   `PageRenderOptions` без изменений: 0 = чёрный, 11 = тождество, 12..15
   клампятся к 11; пересчёт «цвет → индекс палитры» в движке не снят дампом,
   поэтому кривая `k/11` — модель ([PAGER_RENDER.md](PAGER_RENDER.md) §2.5).
   У `BriefingRenderOptions` `fade_level` по умолчанию 0, то есть
   `fade=true` без уровня даёт чёрный кадр — это то же поведение, что у
   `--fade 0`, и оно задокументировано, а не «магия».
5. **Ошибки вкладываются без двойного префикса.** Текст `load_briefing`'а
   (`briefing: …`) и `render_page` (`pager_render: …`) перевыпускается как
   `briefing_render: <причина>`; внутренние `sml: …`, `sml_pager: …`,
   `text_layout: …` остаются видны и указывают на настоящий источник.
6. **Двойное чтение файла.** Индекс и пагер открывают `.sme` независимо
   (второго API «дай пагер из индекса» нет). Это осознанная цена простоты:
   оба чтения идут по одним байтам, а тест сверяет `page_count` и паритет
   кадра, так что расхождение не пройдёт молча.
7. **Точный fade — пагерский, а не второй.** `fade_lut`, `fade_lut_level`,
   `fade_lut_dump`, `fade_lut_table` копируются в `PageRenderOptions` без
   изменений (§8): приоритет LUT над `fade`, отказ при отсутствии таблицы и
   дампа, уровни 0..15 и палитры источников — всё это уже в `render_page`,
   поэтому кадр брифинга не может затухать иначе, чем страница `--pager`.

## 3. API

```cpp
#include "ui/briefing_render.h"

gm::BriefingRenderOptions options;
options.page = 1;                 // 1-based, обязателен (0 — отказ)
options.background = false;       // true: HELPSCRN под текстом
options.fade = false;             // true: кривая 0x682f90 по готовому кадру
options.fade_level = 0;           // 0..15, 0 = чёрный, 11 = тождество
options.fade_lut = false;         // true: точные таблицы 0x682f90 по индексу (§8)
options.fade_lut_level = 11;      // 0..15; 11 = тождество, 12..15 осветляют
options.fade_lut_dump = "";       // путь к hud_lut.bin (4096 Б)
options.fade_lut_table = nullptr; // уже загруженная таблица, приоритетнее дампа

std::vector<std::uint32_t> rgba;  // 0xRRGGBBAA
int width = 0, height = 0;
gm::BriefingRenderStats stats;
std::string error;
if (!gm::render_briefing("/mnt/d/games/Gunmetal 3dfx", "level1", options,
                         rgba, width, height, &stats, &error)) {
    // error: "briefing_render: ..." (см. таблицу)
}
// width=320 height=200 rgba.size()==64000
// stats: pages=2 lines=15 glyphs=719 missing=0 ink=7214 background=0
```

`BriefingRenderStats`:

| Поле | Смысл |
|---|---|
| `pages` | страниц во всём брифинге (`Briefing::page_count()`) |
| `lines` | визуальных строк отрендеренной страницы |
| `glyphs` | символов страницы, для которых нашёлся глиф (пробелы входят) |
| `missing_glyphs` | символов без глифа (пропущены, не фатальны) |
| `ink_pixels` | пикселей холста под глифами (уникальные) |
| `background_pixels` | нечёрных пикселей экранного BMP (0 без `background`) |

`width`/`height`/`rgba` выставляются на каждом успешном вызове; при отказе
буфер очищается, размеры — 0, `stats` — нули. `stats`/`error` могут быть
`nullptr`. Функция только читает установку, детерминирована и не зависит от
текущего каталога.

Отказы (все — `false` + текст):

| Ситуация | Текст (пример) |
|---|---|
| пустой/несуществующий `game` | `briefing_render: game directory not found: <path>` |
| нет `SML/brief` | `briefing_render: no SML/brief directory under <game>` |
| пустое имя / имя-путь / не `.sme` | `briefing_render: empty level name` / `… not a path` / `… not a .sme briefing` |
| файла нет (`level0`) | `briefing_render: file not found: level0.SME under <…/SML/brief>` |
| пустой / битый `.sme` | `briefing_render: empty file: <path>` / `briefing_render: <path>: sml: no tags` |
| нет `FRONTEND/BMP.IDX` | `briefing_render: <path>: text_layout: FRONTEND/BMP.IDX not found under <game>` |
| `page` вне `1..pages` | `briefing_render: no page 0 (1..2)` |
| `fade_lut` без таблицы и дампа | `briefing_render: fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)` |
| `fade_lut_dump` не читается / не 4096 Б | `briefing_render: hud_lut: cannot read '<путь>'` / `… is N bytes, expected 4096 (16 x 256)` |
| ошибка пагера/рендера | `briefing_render: <причина pager_render>` |

## 4. Числа теста

`ctest -R '^briefing_render$'` (`engine/tests/test_briefing_render.cpp`, label
`quick`), реальный `SML/brief/`; тест печатает сам:

```
level1 page 1/2: lines=15 glyphs=719 missing=0 ink=7214
level1 page 1 parity: 0 differing pixels against SmlPager+render_page (719 glyphs, ink 7214)
level1 page 1 rerun: byte-identical
  level0a page 1/5: lines=10 glyphs=180 missing=0 ink=2214
  level0a page 2/5: lines=6 glyphs=87 missing=0 ink=1334
  level0a page 3/5: lines=6 glyphs=91 missing=0 ink=1370
level0a page diffs: 1-2=3216, 2-3=2333, 1-3=3279 differing pixels
level0a page 1 --background: background=64000 pixels, 61786 pixels changed, ink 2214
level0a page 1 fade: 11 = identity, 0 = black (64000/64000 pixels)
  fade LUT briefing level  0: black 63992/64000 (model 64000), 8 px differ from the model, background pixels 64000
  fade LUT briefing level 12..15: adjacent diffs 12/13 39238, 13/14 52240, 14/15 46118; vs level 11: 35332/57908/58031/63846 px
  fade LUT briefing level 11: 0 differing pixels against the frame without fade (table and dump paths agree)
briefing_render levels: 25
briefing_render corpus: 25 levels, 58 pages, 727 lines, 33284 glyphs, missing=0, ink 344505 (min 783, max 9099 per page)
briefing_render: SML/brief page -> 320x200 RGBA through the pager (parity with render_page, 25 briefings, negatives); OK
```

Что закреплено:

* `level1` стр. 1: `pages=2`, `lines=15`, `glyphs=719`, `missing=0`,
  `ink=7214`, кадр 320×200 на 64 000 пикселей, `background_pixels=0`;
* паритет прямого пути — 0 отличий, плюс равенство `lines/glyphs/ink` и
  размеров;
* `level0a` стр. 1/2/3: числа строк/глифов/ink и три ненулевых diff'а (какие
  именно — печатается; в тесте проверяется `> 0`, чтобы не ломаться от
  будущего исправления ориентации/фона);
* `--background`: 64 000 нечёрных пикселей, кадр меняется, `ink` не меняется;
* fade 11 = 0 отличий, fade 0 = 64 000 чёрных пикселей;
* точный fade (§8): уровень 11 = 0 отличий от кадра без fade (таблица и дамп),
  уровень 0 = 63 992 чёрных и 8 отличий от модели, 12..15 — четыре разных
  кадра (diff'ы соседних и против 11 печатаются), отказы без дампа/таблицы;
* корпус: 25 уровней, 58 страниц, 727 строк, 33 284 глифа, `missing=0`,
  суммарный ink 344 505 (мин/макс на страницу 783/9099) — печатается;
  страницы/строки/глифы сверяются с [BRIEFING.md](BRIEFING.md) §4;
* негативы (реальные и синтетические): см. §1; каждый — очищенный буфер и
  нулевые `stats`.

## 5. Ограничения (что срез 3 сознательно не делает)

* **CLI — есть (срез 4).** `--briefing LEVEL --png OUT [--page N]
  [--background] [--fade K] [--fade-lut N]
  [--background] [--fade K] [--png-scale N]`: `show_briefing` сначала печатает
  `pages/lines/glyphs`, затем кадр (`render: page 1/2 320x200 lines=15
  glyphs=719 missing=0 ink=7214`, PNG 256 283 Б). CTest `cli_briefing_png`,
  `cli_briefing_png_page2`, `cli_briefing_png_bad_page`, `cli_briefing_png_scale`;
  `--fade-lut N` (точные таблицы `0x682f90`, §8) проверяется `cli_fade_lut`
  через `cmake -E compare_files`: уровень 11 даёт кадр, побайтово равный
  кадру без fade, уровень 13 — другой.
  `--briefing-png` как отдельного флага нет — используется общий `--png`.
* **Нет `<I>`-битмапов.** `LayoutImage`/`SML/<f>.bmp` не декодируются; слоты
  float'ов раскладка уже учла, поэтому текст стоит на своих местах, а
  иллюстраций брифинга в кадре нет ([PAGER_RENDER.md](PAGER_RENDER.md) §5).
* **Нет par и `YOUR BEST`.** Это состояние VM (`SET_LEVEL_BEST_TIME`, опкод
  93, `0x80d88c`) и файл `GUNMETAL.HI` (`0x45c880`), а не `.sme`
  ([BRIEFING.md](BRIEFING.md) §5); модуль их не читает и не рисует.
* **Фон брифинга — вопрос.** Рисуется `HELPSCRN` (путь `--pager`).
  `BRIEFBG2` — очевидный кандидат для экрана брифинга (он 320×200 и грузится в
  тот же буфер), но привязка не доказана; чтобы выбрать его, нужен кадр игры
  или трасса загрузчика.
* **Origin страницы не доказан.** Кадр кладёт 267×167 в `(0,0)` — то же
  допущение, что у пагера ([PAGER_RENDER.md](PAGER_RENDER.md) §2.1); при
  снятом origin текст/фон сдвинутся.
* **Fade 12..15 — кламп только в модели.** Модель пагера не воспроизводит
  осветление старших уровней (там движок, по дампу, осветляет кадр); точный
  путь `fade_lut` (§8) их воспроизводит, модель — нет, и это остаётся как
  есть: менялся только проброс, а не кривая.
* **Только `.sme`.** `.sml`-редакции брифингов (24 файла, местами другой
  текст) не читаются — как и загрузчик, предпочитающий `.sme`
  ([BRIEFING.md](BRIEFING.md) §5).
* **Двойное чтение/декомпозиция на вызов.** Нет API «отрендерь из уже
  загруженного `Briefing`/`SmlPager`»: каждый вызов заново читает файл,
  декодирует, раскладывает и загружает атласы (58 кадров корпуса — ~7,6 с на
  установке в `/mnt/d`).
* **Один кадр за вызов.** Листание — за вызывающим (`page`); `stats.pages`
  говорит, сколько всего страниц.
* **`stats` — не сумма по документу.** Кроме `pages`, все поля относятся
  только к выбранной странице; суммарные строки/глифы считает вызывающий.
* **Строка `BRIEFING.md` §5 «Рендера в PNG у брифинга нет» устарела** после
  этого среза; сам `docs/BRIEFING.md` в список файлов среза не входил и не
  правился.

## 6. Проверка (выполненные команды)

Сборка только своей цели (портативный тулчейн, [BUILD.md](BUILD.md)):

```
$ export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
$ cmake --build build/sandbox --target gm_briefing_render_tests -j 4
[1/2] Building CXX object engine/tests/CMakeFiles/gm_briefing_render_tests.dir/test_briefing_render.cpp.o
[2/2] Linking CXX executable engine/tests/gm_briefing_render_tests
```

CTest (только свой тест; полный `ctest` по условию среза не запускался):

```
$ ctest --test-dir build/sandbox -R '^briefing_render$' --output-on-failure
1/1 Test #132: briefing_render ..................   Passed    7.57 sec
100% tests passed out of 1
Label Time Summary:
quick    =   7.57 sec*proc (1 test)
```

Python-оракул (изменений в Python нет, прогон для ворот):

```
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 81.3, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```

## 7. Что осталось

1. CLI `--briefing LEVEL --png OUT [--briefing-page N] [--background]
   [--fade N]` в `engine/src/main.cpp` — **есть (срез 4)**, см. §5. Осталось
   вывести в него **точный** путь: `--briefing …. --fade-lut N` (следующий
   срез; `main.cpp` в срезе 7 не менялся, API уже готов — §8).
2. Привязка `BRIEFBG2` к брифинг-экрану (кадр/трасса загрузчика) — тогда фон
   перестанет быть «пагерским по умолчанию».
3. `<I>`-битмапы брифинга поверх текста (`res/bmp` + `render/sprite`).
4. Par/`YOUR BEST`: читать `GUNMETAL.HI`/VM-состояние на уровне экрана, а не
   рендера `.sme`.
5. Снять экранный origin страницы с кадра игры (общая задача с пагером,
   [PAGER_RENDER.md](PAGER_RENDER.md) §6).

## 8. Точный fade (`fade_lut`, срез 7)

Модель §2.5 работает по готовому RGBA и потому клампит 12..15 к 11, а на 0
гасит **все** пиксели. Точный путь делает то же, что движок: композитный кадр
остаётся в палитровых индексах, каждый индекс идёт через
`HudLut::map(level, index)` **до** выбора цвета из палитры своего источника
(экранный BMP или атлас шрифта), см. [PAGER_RENDER.md](PAGER_RENDER.md) §2.6 и
[HUD_LUT.md](HUD_LUT.md) §2. Своей реализации у брифинга нет: четыре поля
копируются в `PageRenderOptions`, и кадр брифинга затухает ровно как страница
`--pager`.

### 8.1 API

```cpp
gm::BriefingRenderOptions options;
options.page = 1;
options.background = true;          // без фона нечему давать индексы (чёрный холст)
options.fade_lut = true;            // точный путь; `fade` игнорируется
options.fade_lut_level = 11;        // 0..15, 11 = тождество, 12..15 реальные таблицы
options.fade_lut_table = &lut;      // вариант A: таблица в памяти (файл не нужен)
// options.fade_lut_dump = "…/hud_lut.bin";  // вариант B: дамп 4096 Б, читает render_page
```

* `fade_lut_table` приоритетнее `fade_lut_dump`; `fade_lut` приоритетнее `fade`.
* Нет ни таблицы, ни дампа — **отказ**, а не кадр без fade:
  `briefing_render: fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)`.
* Дамп не читается или не 4096 Б — тоже отказ:
  `briefing_render: hud_lut: cannot read '<путь>'` (или `… is N bytes, expected 4096 (16 x 256)`).
* На отказе `rgba` очищен, `width=height=0`, `stats` обнулены — контракт §3
  не изменился.

### 8.2 Доказано тестом (`ctest -R '^briefing_render$'`)

`level1` стр. 1 + `background=true` (HELPSCRN под текстом), 64 000 пикселей,
дамп `~/.cache/gunmetal-re/hud_lut.bin` (FNV-1a64 `0x6330406254771f3d`):

| Проверка | Число |
|---|---|
| уровень 11 через таблицу против кадра без fade | **0** отличий из 64 000 |
| уровень 11 через дамп против кадра без fade | 0 |
| дамп против таблицы в памяти (уровень 11) | 0 |
| уровень 11: `ink_pixels` / `background_pixels` | 7214 / 64 000 (как без fade) |
| `fade=true, fade_level=0` + LUT 11 против кадра без fade | 0 (LUT победил модель) |
| уровень 0: чёрных пикселей | **63 992** из 64 000 (модель: 64 000) |
| уровень 0: отличий от модели | **8** пикселей (индексы вне рамп таблица не трогает) |
| 12/13, 13/14, 14/15: отличий между соседними | 39 238 / 52 240 / 46 118 |
| 12/13/14/15 против уровня 11 | 35 332 / 57 908 / 58 031 / 63 846 |
| модель при `fade_level=12` против кадра без fade | 0 (кламп к 11, поведение не менялось) |
| уровень 12 точного пути против этого клампа | > 0 (кадры разные) |

Уровни 0 и 11 совпали с числами пагерского теста (`test_fade_lut`:
63 992 чёрных, 8 отличий от модели) — это тот же путь, не вторая реализация.
Тест печатает все числа сам (§4); при отсутствии дампа он печатает `SKIP` и
остаётся зелёным, а отказы «нет таблицы/дампа» проверяются всегда — им файл не
нужен.

### 8.3 Ограничения

* **Дамп — внешний артефакт.** Таблицы `0x682f90` строятся в рантайме
  (`0x483290`), в поставке их нет: нужен `hud_lut.bin` из
  `tools/win/dump_lut.lua` (`$GM_HUD_LUT` или `~/.cache/gunmetal-re/hud_lut.bin`).
  Без него точный путь отказывает текстом; модель `fade` остаётся рабочим
  путём без файлов.
* **Чёрный холст без `background` индексов не имеет** — такие пиксели LUT не
  трогает и они остаются чёрными на любом уровне (в движке буфер всегда 8 bpp).
  Поэтому проверки §8.2 идут с `background=true`.
* **Палитра — палитра источника** (`BmpImage::palette` / `FontAtlas::palette`),
  а движок читает весь буфер одной `FRONTEND/GAMEPAL`; на этом кадре они
  совпадают, на других единичные глифовые пиксели могут отличаться
  ([PAGER_RENDER.md](PAGER_RENDER.md) §2.6).
* **CLI — следующим срезом.** `--briefing … --fade-lut N` в
  `engine/src/main.cpp` здесь не добавлялся; API и тест готовы, флага нет
  (§7 п. 1).
* **`ink_pixels`/`contrast` считаются до таблицы** (свойство текста, а не
  кадра), `background_pixels` от fade не зависит — как и в пагере.

### 8.4 Проверка (выполненные команды)

```
$ cmake --build build/sandbox --target gm_briefing_render_tests -j 4
$ ctest --test-dir build/sandbox -R '^briefing_render$' --output-on-failure
1/1 Test #133: briefing_render ..................   Passed    8.69 sec
$ ./build/sandbox/engine/tests/gm_briefing_render_tests      # числа §4
$ GM_HUD_LUT=/tmp/opencode/no-such-dump.bin \
    ./build/sandbox/engine/tests/gm_briefing_render_tests    # SKIP-путь, OK
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 93.0, "full": false, "failed": [],
 "game": "/home/nooverload/.cache/gunmetal-re/game", "cached": 0}
```
