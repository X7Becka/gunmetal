# Рендер страницы SML-пагера (`engine/src/ui/pager_render.h`)

Срез 3 пункта `M8.1`: [`SmlPager`](SML_PAGER.md) знает строки и координаты
текущей страницы, [`render/text_layout`](TEXT_LAYOUT.md) — метрики и атласы,
этот модуль **рисует одну страницу** в RGBA-буфер 320×200 и отдаёт числа
(`lines`, `glyphs`, `missing_glyphs`, `ink_pixels`). Ничего не раскладывается
заново: `x`/`y`/`font` берутся из `LayoutRun`/`LayoutLine`, ширина пера — из
`FontAtlas::advance_of`.

Код: `engine/src/ui/pager_render.h/.cpp`; CLI — `--pager FILE --png OUT`
(`engine/src/main.cpp`); тест — `engine/tests/test_pager_render.cpp`
(CTest `pager_render`, label `quick`).

## 1. API

```cpp
#include "ui/pager_render.h"

gm::SmlPager pager(game);                       // атласы из FRONTEND/BMP.IDX
std::string error;
pager.open("help.sme", &error);
pager.show_page(1);                             // 0-based: вторая страница

std::vector<std::uint32_t> rgba;                // 0xRRGGBBAA
int width = 0, height = 0;
gm::PageRenderStats stats;
if (!gm::render_page(pager, game, rgba, width, height, &stats, &error)) {
    // error: текст причины (закрытый пагер, нет атласа, ...)
}
// width=320 height=200, rgba.size() == 64000
// stats: lines=15 glyphs=322 missing=0 ink_pixels=3905
```

| Перегрузка | Источник атласов |
|---|---|
| `render_page(pager, FontLookup, rgba, width, height, stats, error)` | провайдер вызывающего (тот же, что у `SmlPager`, синтетика в тестах) |
| `render_page(pager, game, rgba, width, height, stats, error)` | `load_font` из `<game>/FRONTEND/BMP.IDX` (путь CLI), только чтение |

`width`/`height`/`rgba` выставляются всегда (при ошибке буфер — сплошной фон,
так что вызывающий не видит старых пикселей). `stats` и `error` могут быть
`nullptr`.

Ошибки (все — текст, CLI печатает `[error] pager: ...` и возвращает 2):

| Ситуация | Текст |
|---|---|
| ничего не открыто | `pager_render: no page (the pager is closed)` |
| провайдер не передан | `pager_render: no font source (pass a FontLookup)` |
| у провайдера нет шрифта строки | `pager_render: no atlas for font N` |
| атлас без битмапов | `pager_render: font N is metrics-only` |
| атлас без палитры | `pager_render: font N has no palette` |
| файл атласа не читается | `pager_render: font N: <ошибка load_font>` |
| `--png` без каталога игры | `pager_render: no game directory for the font atlases` |

## 2. Что и как рисуется

### 2.1. Холст и координаты

* Холст — **320×200**, как буфер фронтенда `0x6d8fc0` ([UI.md](UI.md) §6.3);
  пиксель упакован `0xRRGGBBAA`, альфа 255, фон `#000`
  (`kPagerBackground = 0x000000FF`).
* Страница — **267×167** (`0x4972f0(0x10b, 0xa7)`, [UI.md](UI.md) §5.5) и
  кладётся в левый верхний угол: справа 53 px и снизу 33 px остаются фоном.
  Почему не «267×167 с полями»: координаты экранного origin пагера в бинарнике
  не доказаны ([UI.md](UI.md) §8, п. 5 — «точные координаты экранного origin …
  нужна сверка с кадром игры»). Выдуманное смещение сдвинуло бы весь текст;
  нулевое смещение — единственное, что не добавляет гипотезу. Когда origin
  будет снят с кадра, он станет константой `kPagerPageX`/`kPagerPageY`.
* Текст рисуется в координатах страницы: `x = kPagerPageX + run.x`,
  `y = kPagerPageY + run.y` — ровно те числа, что печатает `--pager`.

### 2.2. Атлас и глифы

| Что | Значение (поставка) | Источник |
|---|---|---|
| атласы | `FONTS1/2/4` в `FRONTEND/BMP.IDX` | `load_font`, [TEXT_LAYOUT.md](TEXT_LAYOUT.md) §2.3 |
| шрифт 1 (по умолчанию) | слот 0 → `FONTS4`, ячейка 8×8 | `0x497a99`, отображение 1→0 |
| шрифт 2 | слот 5 → `FONTS4` (y=84), ячейка 8×8 | отображение 2→5 |
| шрифт 3 | `FONTS2`, ячейка 16×16 | `kBigFont`, высота строки 18 |
| ширина глифа | `FontGlyph::width` — обрезанная по чернилам | `0x47fe30` в `load_font` |
| высота глифа | `FontAtlas::cell_height` (8 или 16) | таблица `0x65ddd0` |
| шаг пера | `advance_of(ch)` (уже с `extra_before/after`) | `0x47fa50` |
| прозрачный индекс | 0 | `0x47faf0` |

Порядок строк в глифе — **снизу вверх** (атлас грузится без переворота,
`atlas_pixel` в `text_layout.cpp`): строка `row=0` пишется в
`y + cell_height - 1`, строка `row=cell_height-1` — в `y`. Это тот же блит,
что `blit_run`/`draw_text` и `gm/hud.py::draw_text` (`0x47faf0` пишет строку 0
в текущую и идёт вниз).

Цвет — палитра атласа шрифта (`FontAtlas::palette`, RGBA из BMP, альфа
принудительно 255, как `bmp_to_rgba` в `main.cpp`). Отдельной «палитры текста»
в коде нет: **все 10–14 индексов, которые реально используют `FONTS1`,
`FONTS2` и `FONTS4`, совпадают с общей `GAMEPAL` побайтово** (проверено
скриптом на поставке), так что путь «индекс → `GAMEPAL`» (`gm/hud.py`,
[HUD_CPP.md](HUD_CPP.md)) и путь «индекс → палитра атласа» дают один и тот же
пиксель. Глиф шрифта 1 — тонкий штрих: тело `(19,15,15)`, тень `(75,75,75)`;
на фоне `HELPSCRN` он читается, на нашем чёрном фоне выглядит тускло — это
следствие §5, а не ошибки палитры.

### 2.3. Текст строки

Для каждой строки страницы рисуются её прогоны (`LayoutRun`, байтовый диапазон
`[first, last)` в `document().text`):

* прогоны текущей страницы отбираются по `LayoutPage::start`/`end`
  (`end == 0` — конец документа); страницы идут подряд, прогон не пересекает
  границу, поэтому отбор — точное разбиение;
* `run.text` — уже нормализованное тело строки: `\r\n` → пробел, `<k>` →
  `Soldier`, `<s>` → пробел (это делает `text_layout`). Поэтому рисуется
  `run.text`, а не сырой срез `text[first..last)`: для `<s>`/`<k>` байты
  диапазона — это сам тег, и наивный срез нарисовал бы `<s>` литералами;
* `\r`/`\n` дополнительно пропускаются (защита от документа, собранного
  вручную), неизвестный символ — **пропуск без сдвига пера** и `+1` в
  `missing_glyphs` (как `0x47fa50`/`0x47faf0`, где такой код даёт advance 0);
* `glyphs` — число байт, для которых глиф нашёлся (пробелы входят: у них
  глиф с нулевой шириной);
* `ink_pixels` — пиксели холста, отличные от фона, посчитанные по готовому
  буферу (значит, уникальные: перекрытие глифов не завышает число).

`lines` — `SmlPager::lines().size()`, то есть число визуальных строк страницы;
`glyphs`/`missing_glyphs` в тесте сверяются с независимым обходом прогонов.

## 3. CLI: `--pager FILE --png OUT`

```sh
gunmetal -g "$GUNMETAL_DIR" --pager help.sme --page 2 --png out/help2.png
pager: page 2/3 pages=3 lines=42
  line  0 x=88  y=0   w=91  h=10 font=1  DEFAULT CONTROLS
  ...
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905
png: out/help2.png (256283 bytes)
```

* `--png` рисует **текущую** страницу, то есть после `--page N` (1-based) и
  после всех клавиш `--pager-keys next,prev,first,last,line,up`;
* без `--png` поведение прежнее (только дамп страниц);
* файл пишется существующим путём `encode_png_rgba` + `write_file`
  (`res/png`, stored-deflate), 320×200 RGBA — 256 283 байта;
* ошибки: страница вне диапазона — `[error] pager: no page 99 (1..5)`, код 2;
  нет атласов/файла/каталога — `[error] pager: ...`, код 2; `--all` вместе с
  `--png` — `[error] pager: --all prints every page; --png renders the current
  page only`, код 2 (у `--all` нет одной картинки);
* PNG — единственный вывод картинки: `--png` без `--pager`/`--dump` по-прежнему
  отвергается в конце `main()`.

CTest: `cli_pager_png` (`help.sme --page 2`, проверяет строку `render:` с
`missing=0 ink>0`) и `cli_pager_png_all` (отказ комбинации).

## 4. Числа теста

`ctest --test-dir build/sandbox -R pager_render` (`pager_render`, label `quick`),
тест печатает сам:

```
  empty document <d></d>: lines=0 glyphs=0 missing=0 ink=0
  help.sme page 1/3: lines=15 glyphs=229 missing=0 ink=2534
  help.sme page 2/3: lines=15 glyphs=322 missing=0 ink=3905
  help.sme page 3/3: lines=12 glyphs=183 missing=0 ink=2226
  help.sme total: glyphs=734 missing=0 ink=8665
  CREDITS.SML page 1/5: lines=12 glyphs=194 missing=0 ink=2640
  CREDITS.SML page 2/5: lines=13 glyphs=176 missing=0 ink=2558
  CREDITS.SML page 3/5: lines=14 glyphs=189 missing=0 ink=2817
  CREDITS.SML page 4/5: lines=15 glyphs=202 missing=0 ink=3104
  CREDITS.SML page 5/5: lines=10 glyphs=209 missing=0 ink=2719
  CREDITS.SML total: glyphs=970 missing=0 ink=13838
  page bounds: --page 99 refused (5 pages), rendered page 1, ink=2640
pager_render: 320x200 RGBA of the current page (atlas glyphs, deterministic,
boundaries); OK
```

Проверки теста:

* `ink_pixels > 0` и `< width*height` на каждой странице обоих документов
  (картинка не пустая и не залитая), плюс пересчёт ink по буферу;
* `lines == lines().size()`, `glyphs`/`missing_glyphs` — против независимого
  обхода прогонов страницы; `missing_glyphs == 0` на `help.sme` и
  `CREDITS.SML` (все 8 страниц);
* детерминизм: два рендера одной страницы побайтово равны (буфер и все числа),
  при этом страница 2 отличается от страницы 1;
* перегрузка с каталогом игры (`load_font` из `BMP.IDX`) рисует **те же байты**
  и те же числа, что и перегрузка с готовыми атласами;
* пустой документ `<d></d>` — одна страница, 0 строк, 0 глифов, 0 ink;
* границы: `show_page(98)` (= `--page 99`), `show_page(-1)` и
  `show_page(page_count)` возвращают `false`, курсор остаётся на странице 1;
* негативы: закрытый пагер, пустой `FontLookup`, провайдер без шрифта 1 —
  `false` + текст ошибки (см. §1), буфер при этом сброшен в фон.

`missing_glyphs == 0` означает: документы используют только коды 32..127, для
которых в атласе есть глифы (96 символов на шрифт). Символы вне диапазона
(например, кириллица или `\t`) в поставке не встречаются; если появятся —
будут пропущены и посчитаны, рендер не упадёт.

## 5. Ограничения (что срез 3 сознательно не делает)

* **Нет фона экрана.** `HELPSCRN`/`LOADPAGE`/`BRIEFBG2` (320×200 из
  `FRONTEND/BMP.IDX`, `0x6d8fc0`) не рисуются: фон — `#000`. Поэтому contrast
  текста ниже, чем в игре; сам текст, его координаты и палитра — те же.
* **Нет `<I>`-картинок.** `LayoutImage` даёт слот и выравнивание, но битмапы
  `SML/<f>.bmp` не декодируются (это `Screen`, [SCREEN_CPP.md](SCREEN_CPP.md)
  §3); float-отступы при этом уже учтены раскладкой, так что текст стоит на
  своих местах, а картинок нет.
* **Нет звука `updown`** (`0x4bc454`, [UI_SOUNDS.md](UI_SOUNDS.md)): рендер
  ничего не проигрывает.
* **Нет курсора/подсветки.** Подсветка пункта меню (шрифт 4, `0x485e19`) —
  экран меню, а не пагер; построчная каретка `SmlPager` — UI-удобство
  ([SML_PAGER.md](SML_PAGER.md) §3) и не рисуется.
* **Нет интерлейса/эффектов 320×200.** Оригинал выводит буфер через
  `0x6d8fc0` с fade-LUT (`0x491f2d`); здесь пишется чистый RGBA без
  затемнения и без чересстрочной развёртки.
* **Только текущая страница.** Один вызов — одна страница; листание — за
  вызывающим (`next_page`/`show_page`), `--all` + `--png` отвергается.
* **Origin страницы не доказан** (§2.1): `(0,0)` — рабочее допущение, а не
  факт из бинарника.
* **`missing_glyphs` не фатален** по замыслу (как в движке): число печатается,
  но кадр всё равно выдаётся.

## 6. Что осталось

1. Снять с кадра игры экранный origin пагера (и подтвердить/опровергнуть
   `(0,0)`) — тогда `kPagerPageX/Y` станут фактом.
2. Фон `HELPSCRN` + fade-LUT: компоновка 320×200 «фон → текст → рамка».
3. `<I>`-битмапы поверх текста (переиспользовать `Screen`/`render/sprite`).
4. `warning` при `missing_glyphs != 0` в CLI, если такие документы появятся.

## 7. Проверка (выполненные команды)

Сборка (портативный тулчейн, [BUILD.md](BUILD.md)):

```
$ export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
$ cmake --build build/sandbox -j 16
[224/224] Linking CXX executable engine/tests/gm_weapons_tests
```

Тест модуля и CLI-тесты пагера:

```
$ ctest --test-dir build/sandbox -R "pager" --output-on-failure
1/7 Test  #10: cli_pager_bad ....................   Passed
4/7 Test  #11: cli_pager_png ....................   Passed
5/7 Test  #12: cli_pager_png_all ................   Passed
6/7 Test #100: sml_pager ........................   Passed
7/7 Test #101: pager_render .....................   Passed
100% tests passed out of 7
```

Полный CTest и Python-оракул:

```
$ ctest --test-dir build/sandbox -j 4
100% tests passed out of 183
Label Time Summary:
full     = 797.62 sec*proc (14 tests)
quick    = 252.41 sec*proc (169 tests)
Total Test time (real) = 379.82 sec

$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 66.6, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```
