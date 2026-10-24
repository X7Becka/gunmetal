# Рендер страницы SML-пагера (`engine/src/ui/pager_render.h`)

Срезы 3–4 пункта `M8.1` и срез 5 пункта `M8.5`: [`SmlPager`](SML_PAGER.md)
знает строки и координаты текущей страницы,
[`render/text_layout`](TEXT_LAYOUT.md) — метрики и атласы, этот модуль
**рисует одну страницу** в RGBA-буфер 320×200 и отдаёт числа (`lines`,
`glyphs`, `missing_glyphs`, `ink_pixels`, `background_pixels`,
`contrast_min/max`). Ничего не раскладывается заново: `x`/`y`/`font` берутся
из `LayoutRun`/`LayoutLine`, ширина пера — из `FontAtlas::advance_of`. Срез 4
добавляет **фон экрана** (`HELPSCRN` из `FRONTEND/BMP.IDX`) и **fade-модель**
(`0x682f90`), не меняя путь без опций (побайтово те же PNG, что у среза 3).
Срез 5 добавляет **точный** fade по дампу `0x682f90` на уровне палитровых
индексов (§2.6): при `fade_lut` индекс пикселя идёт через `HudLut::map` **до**
палитры, поэтому уровни 12..15 осветляют, а не клампятся к 11.

Код: `engine/src/ui/pager_render.h/.cpp`; CLI — `--pager FILE --png OUT
[--background] [--fade N]` (`engine/src/main.cpp`); тест —
`engine/tests/test_pager_render.cpp` (CTest `pager_render`, label `quick`).

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

// Срез 4: фон и fade — те же числа плюс background/contrast.
gm::PageRenderOptions options;
options.background = true;                  // HELPSCRN до текста
options.background_asset = "HELPSCRN";      // или LOADPAGE/BRIEFBG2 по имени
options.fade = true;
options.fade_level = 5;                     // 0..15, 0 = чёрный кадр
gm::render_page(pager, game, rgba, width, height, options, &stats, &error);
// stats: ink_pixels=2534 background_pixels=64000 contrast_min=16 contrast_max=75

// Срез 5: точный fade по дампу 0x682f90 (индекс -> индекс, §2.6).
gm::HudLut lut;
if (!gm::load_hud_lut("~/.cache/gunmetal-re/hud_lut.bin", lut, &error)) { ... }
gm::PageRenderOptions exact;
exact.background = true;
exact.fade_lut = true;                      // включает точный путь, `fade` игнорится
exact.fade_lut_level = 12;                  // 0..15, 12..15 — реальные таблицы
exact.fade_lut_table = &lut;                // или exact.fade_lut_dump = "<путь>"
gm::render_page(pager, game, rgba, width, height, exact, &stats, &error);
```

| Перегрузка | Источник атласов/фона |
|---|---|
| `render_page(pager, FontLookup, rgba, width, height, stats, error)` | провайдер вызывающего; фон недоступен, fade работает (`PageRenderOptions{}`) |
| `render_page(pager, FontLookup, rgba, width, height, options, stats, error)` | то же; `options.background` — ошибка (нет каталога игры); `fade_lut` работает, таблица/дамп — от вызывающего |
| `render_page(pager, game, rgba, width, height, stats, error)` | `load_font` из `<game>/FRONTEND/BMP.IDX` (путь CLI), только чтение |
| `render_page(pager, game, rgba, width, height, options, stats, error)` | `load_font` + `options.background_asset` из того же архива |

`PageRenderOptions`:

| Поле | Значение по умолчанию | Смысл |
|---|---|---|
| `bool background` | `false` | нарисовать 320×200 экран до текста |
| `std::string background_asset` | `"HELPSCRN"` | имя записи `FRONTEND/BMP.IDX` |
| `bool fade` | `false` | применить **модель** кривой `0x682f90` к готовому кадру (RGB) |
| `int fade_level` | `11` | уровень модели 0..15 (клампится к 11), 0 = чёрный, 11 = тождество |
| `bool fade_lut` | `false` | применить **точные** таблицы `0x682f90` к палитровым индексам (§2.6); при `true` модель не применяется |
| `int fade_lut_level` | `11` | уровень точного пути 0..15; 12..15 — реальные таблицы, не алиасы 11 |
| `std::string fade_lut_dump` | `""` | путь к дампу 4096 Б; читается `load_hud_lut`, ошибка — текст |
| `const HudLut* fade_lut_table` | `nullptr` | уже загруженная таблица; приоритетнее `fade_lut_dump` |

`PageRenderStats` (все поля — целые):

| Поле | Смысл |
|---|---|
| `lines`, `glyphs`, `missing_glyphs` | как в срезе 3 |
| `ink_pixels` | пиксели, накрытые хотя бы одним пикселем глифа (уникальные; без фона совпадает со срезом 3) |
| `background_pixels` | нечёрные пиксели экранного BMP (0 без фона; у `HELPSCRN` 64000) |
| `contrast_min/max` | диапазон яркости (BT.601) **текстовых** пикселей; 0/0, если текста нет |

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
| фона нет в архиве | `pager_render: background "NAME" not in <...>/FRONTEND/BMP.IDX` |
| фон не 320×200/битый | `pager_render: background "NAME" is WxH, expected 320x200` |
| фон через `FontLookup`-перегрузку | `pager_render: background "NAME" needs a game directory` |
| `fade_lut` без таблицы и дампа | `pager_render: fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)` |
| дамп не читается/не 4096 Б | `pager_render: <текст load_hud_lut>`, напр. `pager_render: hud_lut: cannot read '...'` |
| индекс после LUT вне палитры | `pager_render: fade LUT level L maps index I to M, outside the K-entry palette` |

## 2. Что и как рисуется

### 2.1. Холст и координаты

* Холст — **320×200**, как буфер фронтенда `0x6d8fc0` ([UI.md](UI.md) §6.3);
  пиксель упакован `0xRRGGBBAA`, альфа 255. Без `background` фон — `#000`
  (`kPagerBackground = 0x000000FF`), с ним — экран `HELPSCRN` (§2.4).
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
пиксель. Глиф шрифта 1 — тонкий штрих: тело `(19,15,15)`, тень `(75,75,75)`,
яркость 16..75; на сером `HELPSCRN` он читается, на чёрном холсте выглядит
тускло — это свойство фона (§2.4), а не ошибки палитры.

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
* `ink_pixels` — пиксели холста, накрытые хотя бы одним пикселем глифа;
  считаются по маске, поэтому уникальные (перекрытие глифов не завышает
  число) и от цвета фона не зависят. Без фона это ровно прежнее число
  среза 3 («пиксели, отличные от `#000`»), потому что ни один используемый
  индекс атласа не чёрный.

`lines` — `SmlPager::lines().size()`, то есть число визуальных строк страницы;
`glyphs`/`missing_glyphs` в тесте сверяются с независимым обходом прогонов.

### 2.4. Фон экрана: `HELPSCRN`

**Что найдено** в поставочном `FRONTEND/BMP.IDX` (143 записи, 8 bpp,
`gm/bigfile.py` + `gm/bmp.py`):

| имя | индекс | размер | файл | палитра |
|---|---|---|---|---|
| `HELPSCRN` | 46 | 320×200 | 65 078 Б | `GAMEPAL`, кроме индекса 0 (там cyan-заглушка) |
| `BRIEFBG2` | 14 | 320×200 | 65 078 Б | то же |
| `LOADPAGE` | 56 | 320×200 | 65 078 Б | то же |

Индекс 0 у всех трёх в пикселях не встречается (в `HELPSCRN` все 64 000
индексов ненулевые), поэтому разница палитры на картинку не влияет; берём
палитру самого BMP, как `gm/hud.py::read_panel`.

Фон пагера в коде — `HELPSCRN`: `docs/UI.md` §6.3 — состояние 2 грузит
`SML\HELP.SML` в буфер `0x6d8fc0` 320×200 и рисует `HELPSCRN`;
`docs/UI_SCREENS.md` §9 — `helpscrn`/`briefbg2`/`loadpage` грузятся в тот же
буфер (у каждого свой экран). Поэтому `HELPSCRN` — умолчание
(`kPagerBackgroundAsset`), а `LOADPAGE`/`BRIEFBG2` выбираются по имени через
`PageRenderOptions::background_asset`; привязка *их* к пагеру не доказана и в
доке не утверждается.

**Декодирование** — как `gm/hud.py::read_panel`: индекс → палитра самого BMP
(она же `GAMEPAL`, отличается одна запись), альфа 255. Для полного экрана
индекс 0 — **непрозрачный** чёрный (в отличие от глифов, где 0 = прозрачный).

**Ориентация строк.** `parse_bmp` нормализует BMP к top-down, но загрузчик
движка (`0x47efe0`) копирует строки файла как есть, и экран выводит буфер в
этом же порядке; видимая картинка = `flipV(parse_bmp)`
(`gm/hud.py::engine_order` для `PANEL`). Проверено читаемыми заголовками
ассетов: после флипа `HELPSCRN` читается `HELP`, `LOADPAGE` — `LOADING`,
`BRIEFBG2` — `MISSION OBJECTIVES`; без флипа те же буквы перевёрнуты. Тест
`pager_render` закрепляет это побайтово: пустая страница с фоном **точно
равна** независимо декодированному `flipV(HELPSCRN)` (64 000 пикселей).

**Порядок.** Фон кладётся до текста; вне страницы 267×167 (правые 53 px и
нижние 33 px холста) байты фона не меняются — это тоже проверяется тестом.
Сам текст в игре тоже рисуется поверх готового экрана (`0x47faf0` в тот же
буфер `0x6d8fc0`).

### 2.5. Fade (`0x682f90`): модель и точный путь

В доке доказана **кривая**, но не формула пересчёта цвета обратно в индекс:

* 16 таблиц по 256 байт, строятся при старте `0x48f970` → `0x483290(dst, k/11, 1)`
  (`docs/HUD_AUDIT.md` §2.7): множитель `k/11`, уровень 11 —
  почти тождественный (254/256 совпадающих записей), уровень 0 — чёрный;
* сам блит полосы переводит индекс через `LUT[k]`
  (`FUN_00a1b510(..., &DAT_00682f90 + k*0x100)`), а формула `0x483290`
  «не разобрана — берём дамп» (`docs/HUD_AUDIT.md` §2.9). Дамп снят:
  `~/.cache/gunmetal-re/hud_lut.bin` (4096 Б, `tools/win/dump_lut.lua`,
  [HUD_LUT.md](HUD_LUT.md)), и срез 5 применяет **его** (§2.6).

Путь `options.fade` (срез 4) остаётся **моделью** (помечена как модель):

```
fade(v, k) = (v * min(k, 11) + 5) / 11      // целочисленно, v = 0..255
```

* `k = 0` — чёрный кадр, `k = 11` — побайтово исходный, `k = 12..15`
  клампятся к 11. **Уточнение (2026-10-23, `docs/UPSCALE.md`):** по дампу
  `hud_lut.bin` уровни 12..15 **не** «почти тождественны» — совпадений по
  цвету 132/73/48/45 из 256, то есть движок там осветляет кадр. Модель этого
  не воспроизводит и честно клампит; точный путь (§2.6) воспроизводит;
* применяется к **готовому** 320×200 кадру (фон + текст), как в движке, где
  через LUT идёт весь буфер;
* `contrast_min/max` считаются **до** затемнения (это свойство текста, а не
  кадра), `ink_pixels`/`background_pixels` от fade не зависят.

### 2.6. Точный fade по `0x682f90` (`fade_lut`, срез 5)

`HudLut` — таблица **индекс → индекс**, поэтому её нельзя честно приложить к
готовому RGBA-кадру. Точный путь делает то же, что движок: композитный кадр
остаётся в палитровых индексах, каждый индекс проходит через
`HudLut::map(level, index)`, и только потом берётся цвет из палитры своего
источника (экранный BMP или атлас шрифта). Точки, где индекс не из палитры
(чёрный холст без `background`), не трогаются — у них индекса нет.

Что **доказано** тестом `pager_render` на реальной поставке (`SML/help.sme`,
страница 1, фон `HELPSCRN`, 64 000 пикселей; дамп
`~/.cache/gunmetal-re/hud_lut.bin`, FNV-1a64 `0x6330406254771f3d`,
[HUD_LUT.md](HUD_LUT.md) §2):

| Проверка | Число |
|---|---|
| уровень 11 + LUT против кадра без fade | **0** отличий из 64 000 |
| уровень 11 + LUT против независимого индексного эталона | 0 |
| уровень 0 + LUT: чёрных пикселей | **63 992** (модель: 64 000) |
| уровень 0: отличий LUT от модели | **8** пикселей |
| уровень 0: исходных индексов, оставшихся нечёрными | 2 |
| 12/13, 13/14, 14/15: отличий между соседними | 40 747 / 56 593 / 45 025 |
| 12/13/14/15 против уровня 11 | 38 332 / 61 195 / 61 323 / 63 846 |
| модель при `fade_level=12` против кадра без fade | 0 (кламп к 11) |
| то же в «однопалитровом» чтении (`FRONTEND/GAMEPAL` на весь кадр) | 0 отличий на **всех 16** уровнях |

То есть на 11 путь побайтово совпадает со старым (тождество таблицы), на 0 он
оставляет 8 пикселей (2 индекса вне рамп — те самые слоты, которые таблица не
трогает), а 12..15 дают четыре разных кадра там, где модель отдаёт один и тот
же. Числа печатает сам тест (§4).

Эталон в тесте независим: он заново собирает индексный холст (индексы
`HELPSCRN` в порядке строк движка + индексы глифов из атласов), применяет
`map` и палитру. Поэтому «fade после RGB» или «пересчёт цвета обратно в
индекс» такой эталон не пройдут.

Ограничения точного пути:

* **Палитра обязательна.** Цвет берётся из палитры источника пикселя
  (`BmpImage::palette` / `FontAtlas::palette`). Движок читает весь 8-bpp буфер
  через одну `FRONTEND/GAMEPAL`; на проверенном кадре (`help.sme` стр. 1 +
  `HELPSCRN`) палитры совпадают на всех пикселях и всех 16 уровнях (0
  отличий), но вообще атласы `FONTS2/FONTS4` расходятся с `GAMEPAL` в 20
  слотах вне рамп (173..175, 191, 240..255), так что на других кадрах
  единичные глифовые пиксели могут отличаться от однопалитрового чтения.
* **Дамп — внешний артефакт.** Без `hud_lut.bin` (или `GM_HUD_LUT`) точный
  путь отказывает текстом; `SKIP`-путь теста оставляет сборку зелёной.
* **CLI подключён (срез 5):** `--pager FILE --fade-lut N` (0..15) —
  точные таблицы, дамп из `$GM_HUD_LUT` или
  `~/.cache/gunmetal-re/hud_lut.bin`; `--fade N` остаётся моделью (флаг
  `fade_lut` приоритетнее `fade`). CTest `cli_fade_lut` проверяет через
  `cmake -E compare_files`, что уровень 11 даёт кадр, **побайтово равный**
  кадру без fade, а 13 — другой.
* **Чёрный холст без `background`** не имеет палитровых индексов и остаётся
  чёрным на всех уровнях (в движке буфер всегда 8 bpp и всегда через LUT).
* **Индекс вне палитры — отказ.** Дамп пишет 0..255, поэтому с 256-записной
  палитрой это недостижимо; тест проверяет отказ на укороченной палитре
  (64 записи): `pager_render: fade LUT level 1 maps index 6 to 64, outside the
  64-entry palette`.
* **`ink_pixels`/`contrast_min/max` считаются до таблицы**, как и в модельном
  пути: это свойство текста, а не кадра. `background_pixels` тоже не зависит
  от fade.
* **Порядок с моделью.** Если выставлены и `fade`, и `fade_lut`, применяется
  только точный путь (`fade_lut`); это закреплено тестом (`fade_level=0` +
  `fade_lut_level=11` дают кадр без fade).

## 3. CLI: `--pager FILE --png OUT`

```sh
gunmetal -g "$GUNMETAL_DIR" --pager help.sme --page 2 --png out/help2.png
pager: page 2/3 pages=3 lines=42
  line  0 x=88  y=0   w=91  h=10 font=1  DEFAULT CONTROLS
  ...
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=0 contrast=16..75
png: out/help2.png (256283 bytes)

gunmetal -g "$GUNMETAL_DIR" --pager help.sme --page 2 --background --fade 5 \
         --png out/help2-bg.png
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=64000 contrast=16..75
png: out/help2-bg.png (256283 bytes)
```

* `--png` рисует **текущую** страницу, то есть после `--page N` (1-based) и
  после всех клавиш `--pager-keys next,prev,first,last,line,up`;
* `--background` кладёт `HELPSCRN` под текст, `--fade N` (0..15) применяет
  fade-модель §2.5; без них кадр — прежний срез 3, **побайтово тот же PNG**
  (проверено `cmp` против бинарника master `b559781`: SHA-256 совпал);
  точный путь `fade_lut` (§2.6) в CLI пока не выведен — он только в API,
  `main.cpp` в срезе 5 не менялся;
* без `--png` поведение прежнее (только дамп страниц);
* файл пишется существующим путём `encode_png_rgba` + `write_file`
  (`res/png`, stored-deflate), 320×200 RGBA — 256 283 байта;
* ошибки: страница вне диапазона — `[error] pager: no page 99 (1..5)`, код 2;
  нет атласов/файла/каталога — `[error] pager: ...`, код 2; `--all` вместе с
  `--png` — `[error] pager: --all prints every page; --png renders the current
  page only`, код 2 (у `--all` нет одной картинки);
* PNG — единственный вывод картинки: `--png` без `--pager`/`--dump` по-прежнему
  отвергается в конце `main()`.

CTest: `cli_pager_png` (`help.sme --page 2`, строка `render:` с
`missing=0 ink>0`), `cli_pager_png_all` (отказ комбинации),
`cli_pager_png_background` (`background=64000`) и `cli_pager_png_fade`
(`--fade 5` вместе с фоном, `background=64000 contrast=...`).

## 4. Числа теста

`ctest --test-dir build/sandbox -R pager_render` (`pager_render`, label `quick`),
тест печатает сам:

```
  empty document <d></d>: lines=0 glyphs=0 missing=0 ink=0
  help.sme page 1/3: lines=15 glyphs=229 missing=0 ink=2534 contrast=16..75
  help.sme page 2/3: lines=15 glyphs=322 missing=0 ink=3905 contrast=16..75
  help.sme page 3/3: lines=12 glyphs=183 missing=0 ink=2226 contrast=16..75
  help.sme total: glyphs=734 missing=0 ink=8665
  CREDITS.SML page 1/5: lines=12 glyphs=194 missing=0 ink=2640 contrast=16..75
  CREDITS.SML page 2/5: lines=13 glyphs=176 missing=0 ink=2558 contrast=16..75
  CREDITS.SML page 3/5: lines=14 glyphs=189 missing=0 ink=2817 contrast=16..75
  CREDITS.SML page 4/5: lines=15 glyphs=202 missing=0 ink=3104 contrast=16..75
  CREDITS.SML page 5/5: lines=10 glyphs=209 missing=0 ink=2719 contrast=16..75
  CREDITS.SML total: glyphs=970 missing=0 ink=13838
  HELPSCRN 320x200: non-black=64000, empty page reproduces it byte for byte
  help.sme page 1 over HELPSCRN: ink=2534 background=64000 contrast=16..75 changed-inside=2534
  fade: 0 = black, 5 = darkened (brightest channel 75->34), 11/15 = identity
  fade LUT: dump=.../hud_lut.bin, 2 palettes (1 screen + 1 font atlases)
  fade LUT level  0: black 63992/64000 (model 64000), 8 px differ from the model, 2 source indices left non-black
  fade LUT level 12..15: adjacent diffs 12/13 40747, 13/14 56593, 14/15 45025; vs level 11: 38332/61195/61323/63846 px
  fade LUT single-palette GAMEPAL (per level, px): 0:0 1:0 ... 15:0
  fade LUT out-of-palette: level 1 index 6 -> 64 refused with: pager_render: fade LUT level 1 maps index 6 to 64, outside the 64-entry palette
  fade LUT: level 11 = no fade (0/64000 px), 12..15 differ; exact tables from hud_lut.bin
  page bounds: --page 99 refused (5 pages), rendered page 1, ink=2640
pager_render: 320x200 RGBA of the current page (atlas glyphs, HELPSCRN
background, fade LUT, deterministic, boundaries); OK
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
  `false` + текст ошибки (см. §1), буфер при этом сброшен в фон;
* **срез 4, фон:** пустая страница с `--background` **побайтово равна**
  независимо декодированному `flipV(HELPSCRN)` (64 000 пикселей);
  `background_pixels == 64000`; текст добавляет ровно те же 2534 ink-пикселя,
  что и на чёрном холсте; вне 267×167 ни один байт не изменился; `LOADPAGE`
  по имени даёт свои 64 000 пикселей и точное совпадение с декодом; неизвестное
  имя и `FontLookup`-перегрузка с фоном — `false` + текст (§1);
* **срез 4, fade:** уровень 0 — все 64 000 пикселей чёрные; 11 и 15 —
  побайтово равны кадру без fade; 5 — детерминированное монотонное затемнение
  (ни один канал не ярче, хотя бы один строго темнее: максимум 75→34),
  `ink_pixels` не меняется.
* **срез 5, точный fade:** уровень 11 + LUT — **0** отличий от кадра без fade
  (и 0 от независимого индексного эталона); 0 + LUT — 63 992 чёрных из 64 000,
  8 пикселей отличаются от модели, 2 исходных индекса остаются нечёрными;
  12..15 — четыре разных кадра (соседние диффы 40 747/56 593/45 025, против
  уровня 11 — 38 332/61 195/61 323/63 846), тогда как модель при 12 побайтово
  равна кадру без fade; загрузка из `fade_lut_dump` даёт те же байты, что
  предзагруженная таблица; перегрузка с `FontLookup` (без каталога игры) на
  уровне 11 тоже тождественна; отказы: нет таблицы/дампа, нечитаемый дамп,
  индекс вне укороченной палитры — `false` + текст, буфер сброшен; при
  `fade` + `fade_lut` побеждает LUT; однопалитровое чтение `GAMEPAL`
  совпадает с покадровым на всех 16 уровнях (0 отличий). Без дампа тест
  печатает `fade LUT: SKIP real dump` и остаётся зелёным.

Числа `ink`/`glyphs` на каждой странице — golden среза 3 (2534/3905/2226 и
2640/2558/2817/3104/2719): они закреплены в тесте, поэтому включение
фона/fade не может незаметно сдвинуть текст.

`missing_glyphs == 0` означает: документы используют только коды 32..127, для
которых в атласе есть глифы (96 символов на шрифт). Символы вне диапазона
(например, кириллица или `\t`) в поставке не встречаются; если появятся —
будут пропущены и посчитаны, рендер не упадёт.

## 5. Ограничения (что срезы 3–4 сознательно не делают)

* ~~**Глифы перевёрнуты по вертикали**~~ — **исправлено 2026-10-23** (§8).
  В срезах 3–4 строка глифа `row` писалась в `y + cell_height - 1 - row`,
  хотя движок (`0x47faf0`) и сверенный с кадром `gm/hud.py::draw_text`
  пишут её в `top + row` (`docs/HUD_AUDIT.md` §2.9). Дефект нашёл сабагент
  среза 4 независимо (по буквам `F`/`L`/`T`) и не стал править молча,
  потому что срез 3 требовал побайтово тот же PNG. Теперь порядок
  исправлен, старые PNG отличаются — это и есть исправление; числа
  `ink`/`glyphs` от переворота не зависели, golden среза 3 остались
  валидными.
* **Фон — плоское наложение, без рамки.** `HELPSCRN` рисуется целиком, но
  экранный origin пагера не доказан (§2.1): текст кладётся в `(0,0)` и
  заходит на арт заголовка `HELP` и на клетки прокрутки. В игре origin,
  вероятно, сдвинут внутрь рамки (левая красная полоса и поле заголовка
  `HELPSCRN` это подсказывают), но координата не снята с кадра — выдумывать
  её нельзя.
* **Нет `<I>`-картинок.** `LayoutImage` даёт слот и выравнивание, но битмапы
  `SML/<f>.bmp` не декодируются (это `Screen`, [SCREEN_CPP.md](SCREEN_CPP.md)
  §3); float-отступы при этом уже учтены раскладкой, так что текст стоит на
  своих местах, а картинок нет.
* **Нет звука `updown`** (`0x4bc454`, [UI_SOUNDS.md](UI_SOUNDS.md)): рендер
  ничего не проигрывает.
* **Нет курсора/подсветки.** Подсветка пункта меню (шрифт 4, `0x485e19`) —
  экран меню, а не пагер; построчная каретка `SmlPager` — UI-удобство
  ([SML_PAGER.md](SML_PAGER.md) §3) и не рисуется.
* **Fade: два пути.** `fade_lut` — точные таблицы `0x682f90` по дампу (§2.6),
  побайтовое тождество на 11 и реальные 12..15 доказаны тестом. `fade`/
  `--fade N` остаются **моделью**: кривая `k/11` доказана
  (`0x483290(dst, k/11, 1)`), а пересчёт «цвет → ближайший индекс палитры»
  в `0x483290` не разобран; модель умножает канал и округляет (§2.5), 12..15
  клампит к 11. CLI к точному пути не подключён (main.cpp не менялся, §6).
* **Нет интерлейса 320×200.** Оригинал выводит буфер через `0x6d8fc0`
  чересстрочно; здесь пишется чистый RGBA (fade уже есть, §2.5).
* **Только текущая страница.** Один вызов — одна страница; листание — за
  вызывающим (`next_page`/`show_page`), `--all` + `--png` отвергается.
* **Origin страницы не доказан** (§2.1): `(0,0)` — рабочее допущение, а не
  факт из бинарника.
* **`missing_glyphs` не фатален** по замыслу (как в движке): число печатается,
  но кадр всё равно выдаётся.

## 6. Что осталось

1. ~~Исправить перевёрнутые глифы~~ — **исправлено 2026-10-23** (§8):
   `y + row`, проверка `test_glyph_orientation` сверяет профиль чернил строки
   с растром атласа; golden-числа `ink` не изменились (переворот их не
   менял), поэтому закреплены как были. Старые PNG отличаются — это и есть
   исправление.
2. Снять с кадра игры экранный origin пагера (и подтвердить/опровергнуть
   `(0,0)`) — тогда `kPagerPageX/Y` станут фактом, а текст перестанет
   наезжать на арт `HELPSCRN` (§5).
3. ~~Снять `gm_lut.bin` (`tools/win/dump_lut.lua`) и заменить fade-модель
   таблицей движка~~ — дамп снят (`hud_lut.bin`), точный путь в API готов
   (§2.6, 0 отличий на 11, реальные 12..15). Осталось **подключить CLI**:
   флаг вида `--fade-lut LEVEL` (и путь к дампу) в `engine/src/main.cpp` —
   тогда `--fade` станет побайтовым и для PNG; сейчас `--fade N` — модель.
4. `<I>`-битмапы поверх текста (переиспользовать `Screen`/`render/sprite`).
5. `warning` при `missing_glyphs != 0` в CLI, если такие документы появятся.
6. Привязка `LOADPAGE`/`BRIEFBG2` к пагеру: сейчас они выбираются по имени,
   но доки доказывают только `HELPSCRN` для `HELP.SML` (§2.4).

## 7. Проверка (выполненные команды)

Сборка (портативный тулчейн, [BUILD.md](BUILD.md)):

```
$ export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
$ cmake --build build/sandbox -j 8
[1/2] Building CXX object engine/tests/CMakeFiles/gm_pager_render_tests.dir/test_pager_render.cpp.o
[2/2] Linking CXX executable engine/tests/gm_pager_render_tests
```

Старый путь побайтово (бинарник master `b559781` в
`/home/nooverload/gunmetal-re/build/sandbox/engine/gunmetal`):

```
$ ./build/sandbox/engine/gunmetal --pager help.sme --page 2 --png /var/tmp/new_help2.png
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=0 contrast=16..75
png: /var/tmp/new_help2.png (256283 bytes)
$ /home/nooverload/gunmetal-re/build/sandbox/engine/gunmetal --pager help.sme --page 2 \
      --png /var/tmp/old_help2.png
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905
png: /var/tmp/old_help2.png (256283 bytes)
$ sha256sum /var/tmp/new_help2.png /var/tmp/old_help2.png
06ee653d297e8948c4567dedeb5efef257bd55164b949e5a452e1537ae536274  /var/tmp/new_help2.png
06ee653d297e8948c4567dedeb5efef257bd55164b949e5a452e1537ae536274  /var/tmp/old_help2.png
$ cmp /var/tmp/new_help2.png /var/tmp/old_help2.png && echo BYTE-IDENTICAL
BYTE-IDENTICAL
```

Новый путь с фоном и fade (PNG те же 256 283 байта — RGBA 320×200):

```
$ ./build/sandbox/engine/gunmetal --pager help.sme --page 2 --background \
      --png /var/tmp/bg_help2.png
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=64000 contrast=16..75
png: /var/tmp/bg_help2.png (256283 bytes)
$ ./build/sandbox/engine/gunmetal --pager help.sme --page 2 --background --fade 5 \
      --png /var/tmp/bg_help2_f5.png
render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=64000 contrast=16..75
png: /var/tmp/bg_help2_f5.png (256283 bytes)
$ ./build/sandbox/engine/gunmetal --pager help.sme --page 2 --background --fade 0 \
      --png /var/tmp/bg_help2_f0.png   # кадр целиком чёрный (проверено глазами)
```

Тест модуля и CLI-тесты пагера:

```
$ ctest --test-dir build/sandbox -R "pager" --output-on-failure
1/9 Test  #10: cli_pager_bad ....................   Passed
4/9 Test  #11: cli_pager_png ....................   Passed
5/9 Test  #12: cli_pager_png_all ................   Passed
6/9 Test  #13: cli_pager_png_background .........   Passed
7/9 Test  #14: cli_pager_png_fade ...............   Passed
8/9 Test #103: sml_pager ........................   Passed
9/9 Test #104: pager_render .....................   Passed
100% tests passed out of 9
```

Полный CTest и Python-оракул:

```
$ ctest --test-dir build/sandbox -j 4
100% tests passed out of 186
Label Time Summary:
full     = 775.36 sec*proc (14 tests)
quick    = 250.18 sec*proc (172 tests)
Total Test time (real) = 256.41 sec

$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 53.7, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```

Срез 5 (точный fade, ветка `feat/fadelut`, база master `0097703`):

```
$ cmake --build build/sandbox --target gm_pager_render_tests gm_hud_lut_tests -j 4
[1/2] Building CXX object engine/tests/CMakeFiles/gm_pager_render_tests.dir/test_pager_render.cpp.o
[2/2] Linking CXX executable engine/tests/gm_pager_render_tests

$ ctest --test-dir build/sandbox -R '^(pager_render|hud_lut)$' --output-on-failure
    Start 125: pager_render
1/2 Test #125: pager_render .....................   Passed    1.20 sec
    Start 131: hud_lut
2/2 Test #131: hud_lut ..........................   Passed    0.01 sec

100% tests passed out of 2

$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 85.7, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```


## 8. Исправлено 2026-10-23: порядок строк глифа

Первый вариант рендера (и он же в срезе 4) писал строку 0 глифа **последней** в ячейке
(`y + cell_height - 1 - row`), то есть зеркалил каждый глиф по вертикали.
`ink_pixels` при этом не менялся, поэтому тест этого не ловил (картинка
выглядела «похоже на текст», но буквы вроде `F`, `L`, `p` были
перевёрнуты).

Источник истины: `0x47faf0` пишет строку 0 в целевую строку и идёт вниз
(`gm/hud.py::draw_text`, `docs/HUD_AUDIT.md` §2.9). Теперь
`y = page_y + line.y + row`.

Добавлена проверка `test_glyph_orientation`: рендерится документ `<d>L</d>`,
и профиль чернил по строкам сравнивается с растром глифа из атласа.
С перевёрнутым порядком она падает (`4` расхождения), с правильным — `0`;
проверено обоими прогонами. Такая проверка ловит именно переворот, который
`ink` пропускает.
