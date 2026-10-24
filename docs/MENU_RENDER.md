# Рендер экрана меню (`engine/src/ui/menu_render.h`)

Срез 2 пункта `M8.2`: [`FrontendMenu`](FRONTEND.md) знает текущий экран, его
пункты (`label`, `id`, `x`, `row`) и выделение, этот модуль **рисует экран** в
RGBA-буфер 320×200 — плату `PLATE`, заголовок, тексты пунктов и подсветку — и
отдаёт числа (`items`, `drawn_items`, `ink_pixels`, `background_pixels`,
`missing_glyphs`). Ничего не измеряется заново: шрифты и метрики — из
`render/text_layout` (`FONTS4` в `FRONTEND/BMP.IDX`), координаты — из `Item` и
таблицы §2 `docs/UI_SCREENS.md`, выделение — по `FrontendMenu::selected_id()`.

Срез 3 (`M8.2`) добавляет **элементы управления**: полосу `SLIDE` с бегунком
`KNOB`, чекбоксы `CHECK`/`NOCHECK` и стрелку `MENUARRW` как `MenuWidget`
(§1, §3.5). Координаты SOUND/GRAPHICS/PLAY OPTIONS сняты с сайтов вызова в
`gundx.exe`, а не подобраны; всё, что осталось моделью, помечено ниже.

Срез 8 (`M8.5`) добавляет **точный fade** (§7): `MenuRenderOptions` с
`fade_lut`/`fade_lut_level`/`fade_lut_dump`/`fade_lut_table`, таблица
`0x682f90` применяется к палитровым индексам кадра **до** перевода в RGB
(как в `pager_render`, `docs/PAGER_RENDER.md` §2.6), плюс модельный
`fade`/`fade_level` (`render/upscale.h::fade_rgba`). Без таблицы и без дампа
`fade_lut` — ошибка с префиксом `menu_render:`, а не «тихо без fade».

Код: `engine/src/ui/menu_render.h/.cpp`; тест — `engine/tests/test_menu_render.cpp`
(CTest `menu_render`, label `quick`). CLI подключён: `--menu --png OUT`
(см. §6; виджеты в CLI пока не передаются).

## 1. API

```cpp
#include "ui/frontend_menu.h"
#include "ui/menu_render.h"

gm::FrontendMenu menu;
menu.reset();                              // MAIN
menu.enter(gm::Screen::ControlOptions);    // или любой §2-экран

std::vector<std::uint32_t> rgba;           // 0xRRGGBBAA
int width = 0, height = 0;
gm::MenuRenderStats stats;
std::string error;
if (!gm::render_screen(menu, "/mnt/d/games/Gunmetal 3dfx", rgba, width, height,
                       &stats, &error)) {
    // error: текст причины (None/Exit, нет атласа, нет каталога игры, ...)
}
// width=320 height=200, rgba.size() == 64000
// stats: items=10 drawn_items=10 ink_pixels=1466 background_pixels=62534
//        missing_glyphs=0
```

Элементы управления (срез 3):

```cpp
gm::MenuWidgetAssets assets;               // SLIDE/KNOB/CHECK/NOCHECK/MENUARRW
auto loaded = gm::load_menu_widget_assets(game, error);
if (!loaded) { /* error: текст причины */ }
assets = *loaded;

std::vector<gm::MenuWidget> widgets = gm::menu_widgets(gm::Screen::SoundOptions);
// 2 слайдера (68,37)/(68,57), 0..20 + чекбокс (20,59); значения — нейтральные
widgets[0].value = 10;                     // бегунок в середине
widgets[2].checked = true;                 // Switch Left Right

gm::render_screen(menu, game, widgets, rgba, width, height, &stats, &error);
// stats.widgets=3 drawn_widgets=3 widget_ink>0, ink_pixels вырос на widget_ink
```

Fade (срез 8) — те же шрифты/панель/спрайты плюс `MenuRenderOptions`:

```cpp
gm::MenuRenderOptions options;
options.fade_lut = true;                    // точный путь: индекс -> индекс
options.fade_lut_level = 13;                // 0..15; 11 — тождество, 12..15 светлеют
options.fade_lut_table = &built;            // build_hud_lut(GAMEPAL), без файла
// или: options.fade_lut_dump = "~/.cache/gunmetal-re/hud_lut.bin";
gm::render_screen(menu, fonts, panel, &assets, widgets, options,
                  rgba, width, height, &stats, &error);

gm::MenuRenderOptions model;                // модельный путь (как CLI сегодня)
model.fade = true;
model.fade_level = 5;                       // 0..15, 12..15 клампятся к 11
gm::render_screen(menu, game, options, rgba, width, height, &stats, &error);
```

`MenuRenderOptions` (§7):

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool fade` | `false` | модельный fade `(v*k + max/2)/max` по готовому RGBA-кадру |
| `int fade_level` | `11` | уровень модели 0..15; 0 — чёрный, 11 — тождество, 12..15 клампятся к 11 |
| `bool fade_lut` | `false` | точные таблицы `0x682f90` по палитровым индексам; приоритетнее `fade` |
| `int fade_lut_level` | `11` | уровень точного пути 0..15; 12..15 — реальные таблицы |
| `std::string fade_lut_dump` | `""` | путь к дампу 4096 Б (`load_hud_lut`) |
| `const HudLut* fade_lut_table` | `nullptr` | уже загруженная таблица; приоритетнее дампа |

`MenuWidget`: `kind` (`Slider`/`Checkbox`/`Arrow`), `x`, `y` (координаты
платы), `value`/`min`/`max`, `checked`, `rotated` (стрелка `MENUARRW`,
повёрнутая на 180°). `MenuSprite` — спрайт BMP.IDX с ключевым цветом
(индекс палитры 0 → прозрачный), `MenuWidgetAssets` — пять нужных спрайтов.
`menu_widgets(screen)` отдаёт геометрию, **доказанную** по сайтам вызова
(§3.5), и пустой вектор для экранов без доказанной раскладки.

| Перегрузка | Источник шрифтов и панели |
|---|---|
| `render_screen(menu, FontLookup, rgba, w, h, stats, error)` | провайдер вызывающего, фон чёрный (синтетика в тестах) |
| `render_screen(menu, FontLookup, const MenuBackground*, ...)` | то же + слой панели, нарисованный первым (обрезается по холсту) |
| `render_screen(menu, FontLookup, const MenuWidgetAssets*, const std::vector<MenuWidget>&, ...)` | то же + спрайты и элементы управления, фон чёрный |
| `render_screen(menu, FontLookup, const MenuBackground*, const MenuWidgetAssets*, const std::vector<MenuWidget>&, ...)` | полный ручной набор: панель, спрайты, виджеты |
| `render_screen(menu, game, ...)` | `load_font` (0/4/5) и `load_menu_panel` из `<game>/FRONTEND/BMP.IDX` — путь CLI, только чтение |
| `render_screen(menu, game, const std::vector<MenuWidget>&, ...)` | то же + `load_menu_widget_assets` (только если виджеты непусты) |
| `render_screen(..., const MenuRenderOptions&, ...)` | любая из перегрузок выше плюс fade (срез 8, §7); `options` идёт перед `rgba` |

`width`/`height`/`rgba` выставляются всегда (при ошибке — сплошной фон, старых
пикселей вызывающий не увидит). `stats` и `error` могут быть `nullptr`.

| Ситуация | Текст ошибки |
|---|---|
| до `reset()` (`Screen::None`) | `menu_render: no screen (FrontendMenu::reset was not called)` |
| терминальное состояние | `menu_render: Screen::Exit has no picture (the front-end has left)` |
| провайдер не передан | `menu_render: no font source (pass a FontLookup)` |
| у провайдера нет шрифта | `menu_render: no atlas for font N` |
| атлас без битмапов / без палитры | `menu_render: font N is metrics-only` / `... has no palette` |
| пустой каталог игры | `menu_render: no game directory for the font atlases and the panel` |
| атлас не читается | `menu_render: font N: <ошибка load_font>` |
| панель не читается | `menu_render: panel PLATE: <ошибка load_menu_panel>` |
| у экрана нет панели (`load_menu_panel`) | `menu_render: screen N has no panel asset` |
| виджеты без спрайтов | `menu_render: widgets need the SLIDE/KNOB/CHECK/NOCHECK/MENUARRW sprites (pass MenuWidgetAssets)` |
| спрайт не читается (путь игры) | `menu_render: control sprites: <ошибка load_menu_widget_assets>` |
| спрайт не того размера | `menu_render: SLIDE is 8x8, expected 72x8` |
| спрайт вне палитры | `menu_render: SLIDE uses palette index N of 256` |
| `fade_lut` без таблицы и дампа | `menu_render: fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)` |
| дамп не читается | `menu_render: <текст load_hud_lut>`, напр. `menu_render: hud_lut: cannot read '...'` |
| индекс после LUT вне палитры | `menu_render: fade LUT level L maps index I to M, outside the K-entry palette` |

## 2. Привязка «экран → ассет»: что найдено

Инвентарь `FRONTEND/BMP.IDX` — 143 записи; размеры ниже прочитаны из самих BMP
(`gm/bigfile.py` + `gm/bmp.py`), совпадают с `docs/UI_SCREENS.md` §9:

| запись | размер | назначение (§9) |
|---|---|---|
| `PLATE` | 196×131 | плата меню, в её копию `0x486050` компонуется текст |
| `SCROLL` | 404×200 | фон карусели главного меню |
| `F_M_TITL` | 232×52 | арт-заголовок главного меню |
| `controls`/`sound`/`graphics`/`startgam`/`savegame`/`quitgame`/`play` | 112×25 | entry-art главного меню |
| `MENULIT` / `MENUARRW` | 16×16 / 16×8 | маркер выбранного / стрелка списка |
| `SLIDE` / `KNOB` / `CHECK` / `NOCHECK` / `FILESLOT` | 72×8 / 8×8 / 8×16 / 8×8 / 124×10 | слайдер, кноб, чекбоксы, полоса слота |
| `FONTS1`..`FONTS4` | 512×168 / 1024×80 / 520×56 / 512×168 | атласы шрифтов |
| `NUMFONTS` | 180×48 | цифровой атлас |

**Вывод (факт + отрицание).** Отдельного полноэкранного арта на каждый экран
меню в поставке **нет**. §2 (`ui_screens.py --json`, поле `assets`) даёт одну
общую панель `PLATE` 196×131 для **28 из 34** экранов §2:

* 5, 6, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
  30, 31, 32, 33, 34, 35, 36, 37 — `plate` (+ `slide`/`knob`, `check`/`nocheck`,
  `fileslot`, `menuarrw` по экранам);
* 0 (intro), 2 (SML-пагер), 3/4 (переходы брифинга) — другие рендереры;
* 1 (MAIN) — `scroll` 404×200 + `f_m_titl` + семь entry-art 112×25 (карусель);
* 9 (in-game menu) — `demotion`/`disband` 320×200 + `humpan`/`panel`/`panelmid`.

`SCROLL` шире холста (404 > 320) и раскладка карусели не документирована,
поэтому MAIN рисуется без своего арта (чёрный фон, подписи-имена артов
центрированы — модель).

**Чего не хватает для «настоящей» панели:** экранная позиция копии `PLATE`
(`0x6865f8`) в бинарнике не доказана — §3 говорит только, что текст пишется в
копию 196×131, а куда эта копия потом блитится, в доках нет. Поэтому панель
рисуется в **(0,0)** — та же конвенция и та же оговорка, что у страницы
SML-пагера (`PAGER_RENDER.md` §2.1): нулевое смещение — единственное, что не
добавляет гипотезу. Когда origin будет снят с кадра игры, он станет константой.

## 3. Что и как рисуется

### 3.1. Холст, панель, клип

* Холст — **320×200**, буфер фронтенда `0x6d8fc0` (`docs/UI.md` §6.3); пиксель
  `0xRRGGBBAA`, альфа 255, фон `#000` (`kMenuBackground`).
* Панель (`MenuBackground`, из `PLATE`) кладётся первой в `(0,0)`, лишнее
  обрезается по холсту; чёрный фон остаётся справа (124 px) и снизу (69 px).
* **Клип текста — 196×131**: `0x485dd0` пишет именно в копию `PLATE`
  (§3), поэтому строка ниже 131 на экран не попадает — у списков наборов
  клавиш (31 строка, шаг 8) видны 13 строк списка и подсказка (14 из 32
  пунктов), как и в оригинале, который их прокручивает (состояния прокрутки в
  снимке `items()` нет).

### 3.2. Координаты пункта (§3, `0x485dd0`)

| случай | y |
|---|---|
| `row < 0` (любой пункт, в том числе текст) | `y = -row` — так заданы `MULTIPLAYER GAME TYPE`, `START GAME` (включая `ESC TO CANCEL` = `-110`), `SAVE GAME`, `LEVEL SELECT`, `START GAME!` экрана 27 |
| `row >= 0`, `item == -1` (невыделяемый текст) | `y = row` — абсолютная координата (ERROR 91, SHAREWARE 27/36/45, подсказки 20) |
| `row >= 0`, пункт выделяемый | `y = row + id * step`, `step` — immediate §2 (`0x662d58`, `menu_row_step`) |
| `step == 0`, пункт выделяемый (только MAIN) | `y = 20 + index*10`, подпись центрируется — **модель** |

`x` — левое смещение внутри полосы 196 px из `Item::x`. При `x == 0`
(у MAIN) подпись центрируется в полосе: `x = (196 - width) / 2`.

Единственное отступление от буквы §3 — **31-строчные списки наборов клавиш**
(30/34/35): все строки приходят с одним `row = -29`, и буквальное `y = -row`
схлопнуло бы их в одну линию. Они раскладываются от `-row` с шагом §2 (8);
это отмечено в коде (`shared_negative_base`) и здесь как модель.

Заголовок: `y = kMenuTitleY = 10`, по центру полосы 196 px. §2 даёт строку
заголовка, но не её строку-координату, а над первым пунктом плотных экранов
(27) остаётся 27 px — 10 это **модель**. Заголовок рисуется только там, где §2
даёт `title VA` (24 экрана); у 30/34/35 заголовок — имя набора (в снимке его
нет), у MAIN — арт `F_M_TITL`, у 9/15 заголовка нет.

### 3.3. Шрифты и цвета — доказано атласом

§3: «шрифт заголовка — 5, пунктов — 0, подсветки — 4». Все три — грани одного
атласа `FONTS4` (`font_table_spec`: `origin_y` 84 / 0 / 42, ячейка 8×8).
Проверка по пикселям самого BMP (не по докам): у всех четырёх граней **ровно
1174** чернильных пикселя (одинаковые глифы), но разные палитры:

| шрифт | грань | доминирующий цвет | роль |
|---|---|---|---|
| 0 | `FONTS4` y=0 | (19, 15, 15) тёмно-серый | обычный пункт |
| 4 | `FONTS4` y=42 | (239, 239, 239) белый | подсветка |
| 5 | `FONTS4` y=84 | (155, 15, 15) тёмно-красный | заголовок |
| 8 | `FONTS4` y=126 | (75, 75, 75) серый | не используется |

То есть подсветка — **те же глифы в белой грани**; полосы/рамки/маркер
`MENULIT` не рисуются (их геометрия не документирована). Подсветка выбирается
по id пункта (`selected_id()`), поэтому двухстрочные пункты (`NUMBER OF LOCAL
PLAYERS`, `PLAY OPTIONS`) выделяются обеими строками — как в §3.

Порядок строк глифа: `FontGlyph::pixels` строка 0 — **верхняя** ячейка
(`0x47faf0` пишет её в строку назначения и идёт вниз; `gm/hud.py::draw_text`
сверен с `gm_screen.bin` на 100% глифовых пикселей, `docs/HUD_AUDIT.md` §2.9).

### 3.4. Числа статистики

* `items` — `menu.items().size()`;
* `drawn_items` — пункты, покрывшие хотя бы один пиксель (клип 196×131
  учитывается: у 31-строчных списков это 14, а не 32);
* `ink_pixels` — пиксели холста, покрытые глифом **или виджетом** (уникальные,
  пересечения не считаются дважды);
* `background_pixels` — `w*h - ink_pixels`;
* `missing_glyphs` — байты подписи без глифа в атласе;
* `widgets` / `drawn_widgets` / `widget_ink` — передано / нарисовано (хотя бы
  один пиксель) / покрыто виджетами; `widget_ink` входит в `ink_pixels`.

### 3.5. Элементы управления — доказанная геометрия (`M8.2`, срез 3)

**Спрайты.** Размеры прочитаны из BMP-заголовков `FRONTEND/BMP.IDX`
(`gm/bigfile.py` + `gm/bmp.py`), совпадают с §9 `UI_SCREENS.md`:

| запись | размер | ключевых пикселей (индекс 0) | роль |
|---|---|---|---|
| `SLIDE` | 72×8 | 71 | дорожка слайдера |
| `KNOB` | 8×8 | 0 (непрозрачный) | бегунок |
| `CHECK` | 8×16 | 37 | чекбокс включён |
| `NOCHECK` | 8×8 | 0 (непрозрачный) | чекбокс выключен |
| `MENUARRW` | 16×8 | 87 | стрелка списка |

Загрузчик проверяет размер и отказывает текстом при расхождении, поэтому
константы `kMenuSlide*`/`kMenuKnob*`/… не могут молча разойтись с данными.

**Как рисует оригинал.** Это не «8 полос» и не «8 пар координат»: `0x486070`
(`SLIDE`), `0x4860b0` (`KNOB`), `0x4833e0` (`CHECK`/`NOCHECK`) и `0x485cc0`
(`MENUARRW`) — **маскированные блиты одного спрайта** в копию платы `0x6865f8`
(шаг строки `0xc4` = 196). У `SLIDE` шаг источника `0x48` = 72 = одна строка
спрайта; `0x67f0c8` — буфер самой картинки `KNOB`, а не таблица координат.
(Формулировку §3 `UI_SCREENS.md` про «8 полос/8 пар» стоит поправить
основному агенту — файл вне среза.) Все четыре помощника **пропускают байт 0**
(индекс палитры 0 = прозрачный), `0x4833e0` рисует `NOCHECK` **на 8 строк
ниже** якоря (8×16 `CHECK` выровнен по верху, 8×8 `NOCHECK` — по низу), а
`0x485d00` читает `MENUARRW` задом наперёд — это тот же спрайт, повёрнутый на
180° (стрелка вверх → вниз), второго ассета нет.

**Доказанные раскладки** (`menu_widgets`), координаты — `(x, y)` платы,
получены из констант-указателей сайтов вызова (`адрес - 0x6865f8`, `y` —
частное, `x` — остаток по модулю 196):

| экран | элемент | x, y | значение | доказательство |
|---|---|---|---|---|
| 11 SOUND | слайдер Music | 68, 37 | 0..20 | трек `0x688290` (`0x488a45`), кноб `0x688291` (`0x488a74`), кламп `0x48c711` |
| 11 SOUND | слайдер Effects | 68, 57 | 0..20 | трек `0x6891e0` (`0x488a93`), кноб `0x6891e1` (`0x488ac2`), кламп `0x48c6da` |
| 11 SOUND | чекбокс Switch Left Right | 20, 59 | — | `0x689338` (`0x488af2`), подпись x 30, строка 67 |
| 12 GRAPHICS | слайдер Border | 88, 39 | 0..9 | трек `0x68842c` (`0x485f67`), кноб `0x68842d` (`0x485f84`), кламп `0x48c773` |
| 12 GRAPHICS | чекбокс Reflections | 40, 61 | — | `0x6894d4` (`0x485fd4`), подпись x 50, строка 69 |
| 32 PLAY | 6 чекбоксов | 16, 13/24/35/52/70/87 | — | `0x686ffc`, `0x687868`, `0x6880d4`, `0x688dd8`, `0x689ba0`, `0x68a8a4` |
| 22 MOUSE | 3 слайдера | 105, 40/56/72 | float | `0x688501`/`0x689141`/`0x689d81` (`0x489ace`, `0x489b52`, `0x489bd6`) — **не** в `menu_widgets`, см. ниже |

Правила, подтверждённые таблицей: дорожка SOUND стоит на 10 px ниже строки
подписи (27 → 37, 47 → 57), дорожка GRAPHICS — в строке подписи (24 + 15 = 39),
якорь чекбокса — на 8 px выше строки подписи (у `PLAY` 21 → 13 и т. д.), `x`
чекбокса — левее подписи (`SLIDE`/`CHECK` не перекрывают текст).

**Что здесь модель.** Позиция бегунка считается по формуле среза
`x + value*(SLIDE.width-KNOB.width)/(max-min)` = `x + 0..64`; оригинал же
двигает кноб на **54 px** от `x+1` (`GRAPHICS`: `89 + 6*border`;
`SOUND`: `69 + 27*volume/10`). Разница — до 10 px у правого края; формула
среза оставлена как заявленная модель, точная арифметика сайтов вызова
записана здесь и в комментариях `menu_widgets`. `MOUSE SETUP` в раскладку не
попал сознательно: его кноб привязан к `x+3` и едет **справа налево**
(`0x489af2` — `sub`), а значения — `double` (`0x4aac38/0x40/0x48`); для этого
нужен флаг «реверс» и диапазон из конфига, а не выдуманное значение.

## 4. Тест (`menu_render`, label `quick`)

```sh
$ ctest --test-dir build/sandbox -R '^menu_render$' -V | tail -22
  MAIN MENU (id 1): items=7 drawn=7 ink=638 background=63362 missing=0 highlight_diff=157
  CONTROL OPTIONS (id 10): items=10 drawn=10 ink=1466 background=62534 missing=0 highlight_diff=250
  QUIT GAME? (id 33): items=2 drawn=2 ink=228 background=63772 missing=0 highlight_diff=78
  panel: PLATE 196x131, pixels under text=24210, ink with/without=1466/1466
  widget assets: SLIDE 72x8 key=71, KNOB 8x8 key=0, CHECK 8x16 key=37, NOCHECK 8x8 key=0, MENUARRW 16x8 key=87
  CONTROL OPTIONS: widgets=9 drawn=9 ink=5752 (text 1466 + widget 4286) diff=4313
  slider values: 0/mid/max diffs=121/121/126, clamp identical (-1000==0: 1, 10000==100: 1), degenerate ink=1991
  checkbox: checked/unchecked diff=128, widget ink 57 -> 78
  arrow: 16x8, rotated diff=56, ink=41
  GRAPHICS OPTIONS: widgets=2 drawn=2 ink=1696 (text 1087 + widget 609) diff=609
  GRAPHICS OPTIONS toggle: diff=128, widget ink 609 -> 636
  SOUND OPTIONS: widgets=3 drawn=3 ink=1976 (text 795 + widget 1181) diff=1181
  SOUND OPTIONS volume: 0 vs 20 diff=252
  PLAY OPTIONS: widgets=6 drawn=6 ink=2898 (text 2376 + widget 522) diff=522
  PLAY OPTIONS checkboxes: off/on diff=680
  sweep: 34/34 screens render, 26 with items, 3 clipped (key set lists), ink total 29916
fade: SOUND OPTIONS + 3 widgets (PLATE + sprite palettes)
fade: LUT level 11 vs no fade: 0/64000 px differ
fade: LUT level 0: black 63963/64000 (model 64000), 37 px differ from the model
fade: levels 12..15 adjacent diffs 16246/17736/23108/20284, vs level 11 16246/24811/25165/25522
fade: model level 5 == fade_rgba on the unfaded frame, model level 12 == level 11, fade_lut wins over fade
fade: built table FNV-1a64 0x6330406254771f3d (dump 0x6330406254771f3d), no dump file needed
fade: game overload level 13 vs 11 diff=24811
menu_render: 320x200 RGBA of the menu screen (plate + §3 fonts, highlight, deterministic); OK
1/1 Test #83: menu_render ......................   Passed    0.48 sec
100% tests passed out of 1
```

Что проверяется (срез 2):

* **Числа** — для 1 / 10 / 33: `items == items().size()`,
  `drawn_items == items`, `ink > 0`, `background > 0`,
  `ink + background == 320*200`, `missing_glyphs == 0`; на чёрном фоне
  независимый пересчёт «не-чёрных» пикселей совпадает с `ink_pixels`;
* **детерминизм** — два рендера одного состояния побайтово равны, статистика
  совпадает;
* **подсветка** — `feed(Down)` меняет картинку: 157 / 250 / 78 отличающихся
  пикселя, число пунктов не меняется;
* **панель** — `menu_panel_asset(10) == "PLATE"`, у MAIN панели нет; `PLATE`
  грузится как 196×131, `(0,0)` остаётся пикселем платы, картинка с панелью
  отличается от чёрной (24210 пикселей под текстом), `ink` не меняется; путь
  «каталог игры» рисует те же байты, что провайдер + загруженная панель; MAIN
  через путь игры рендерится на чёрном;
* **все 34 экрана** — каждый рендерится без ошибки и без `missing_glyphs`,
  суммы: `34/34 screens render, 26 with items, 3 clipped (key set lists), ink
  total 29916`; клип ожидаем только у списков наборов клавиш;
* **§2-стиль** — шаги 9/16/15/0 и правило заголовка (10/33 — есть, MAIN и 30 —
  нет);
* **негативы** — `Screen::None` и `Screen::Exit` отклоняются с текстом (и через
  путь игры, до чтения файлов), пустой `FontLookup` и пустой каталог игры —
  тоже; во всех случаях буфер сброшен в 320×200 фона, статистика нулевая.

Что добавляет срез 3:

* **спрайты** — размеры и ключевые пиксели каждого из пяти ассетов:
  `SLIDE 72x8 key=71, KNOB 8x8 key=0, CHECK 8x16 key=37, NOCHECK 8x8 key=0,
  MENUARRW 16x8 key=87`; пустой каталог игры отклоняется текстом;
* **раскладки** — `menu_widgets`: 3 виджета у SOUND (2 слайдера 0..20 + чекбокс
  в (20,59)), 2 у GRAPHICS (слайдер 88,39 0..9 + чекбокс 40,61), 6 чекбоксов у
  PLAY (x 16, y 13/24/35/52/70/87); CONTROL OPTIONS и MAIN — пусто (текст /
  карусель);
* **CONTROL OPTIONS + 8 слайдеров + 1 чекбокс** (заявленный синтетический
  набор): `widgets=9 drawn=9 ink=5752` против `1466` без виджетов
  (`+4286`), отличается `4313` пикселя, повтор побайтово равен;
* **слайдер** — min/mid/max дают разные кадры (121 / 121 / 126 пикселей),
  `value=-1000` совпадает с `min`, `value=10000` — с `max`, `max == min` не
  делит на ноль;
* **чекбокс** — `checked`/нет: 128 пикселей разницы, `CHECK` (78) рисует больше
  `NOCHECK` (57) при общем маскированном покрытии;
* **стрелка** — `MENUARRW` и она же повёрнутая на 180°: 56 пикселей разницы;
* **графика/звук/игра** — `GRAPHICS OPTIONS toggle: diff=128`,
  `SOUND OPTIONS volume: 0 vs 20 diff=252`, `PLAY OPTIONS checkboxes:
  off/on diff=680`; чекбоксы и слайдеры реально меняют кадр;
* **клип** — виджеты ниже 131-й строки не рисуются (`drawn_widgets=0`,
  `widget_ink=0`) и не крэшат;
* **негатив** — виджеты без спрайтов отклоняются текстом
  (`... (pass MenuWidgetAssets)`), буфер сброшен, статистика нулевая; путь
  «каталог игры» с виджетами рисует те же байты, что ручной набор
  (шрифты + панель + спрайты).

Тест читает установку только на чтение; без неё шрифтовые/панельные прогоны
пропускаются (`menu_render: game not installed, font/panel runs skipped`),
негативы выполняются всегда.

Что добавляет срез 8 (fade, §7) — экран SOUND OPTIONS (id 11) с
панелью `PLATE` и тремя виджетами, чекбокс *Switch Left Right* включён:

* **уровень 11** (тождество) — `0/64000` отличающихся пикселей от кадра без
  fade, через ручной путь и через путь «каталог игры»;
* **уровень 0** — `63963` чёрных из 64000 (модель — `64000`), `37` пикселей
  отличаются от модельного пути: это пиксели `CHECK` с индексами 243/244,
  которые входят в skip-список `0x483250` и таблица их не трогает;
* **12..15** — четыре разных кадра: соседние отличия
  `16246/17736/23108/20284`, против уровня 11 — `16246/24811/25165/25522`;
  модельный путь при `fade_level=12` совпадает с уровнем 11 (кламп);
* **модель не изменилась** — `fade_level=5` побайтово равен
  `render/upscale.h::fade_rgba(frame, 5, 11)` на несфейженном кадре (то, что
  CLI делает сейчас), `fade_lut` приоритетнее `fade` (уровень 0 модели +
  тождественная таблица = кадр без fade);
* **таблица без файла** — `build_hud_lut` из `FRONTEND/GAMEPAL` даёт
  FNV-1a64 `0x6330406254771f3d` (тот же, что у дампа), дамп не читается;
* **отказы** — `fade_lut` без таблицы и дампа: текст с `fade_lut_table` и
  `fade_lut_dump`, буфер сброшен, статистика нулевая; несуществующий дамп —
  текст `hud_lut`, оба с префиксом `menu_render:`;
* **спрайты тоже с индексами** — уровень 13 через путь «каталог игры»
  совпадает с ручным кадром и отличается от 11 на `24811` пикселей.

## 5. Ограничения

* **Нет клипов, звуков и анимаций** — `updown`/`select`/`menuon`
  (`UI_SOUNDS.md` §2), переходы/карусель MAIN, мигание маркера не
  моделируются; кадр всегда один.
* **Значения элементов управления не читаются** — `menu_widgets` отдаёт
  доказанную геометрию с нейтральными значениями (`value = min`,
  `checked = false`): громкости живут в `GUNMETAL.CFG` (`0x190`/`0x194`),
  биты PLAY OPTIONS — в `0x5a46f8`, Border — в `0x4bc1c8`. Их чтение и запись
  — отдельный срез (`config.*`, `KEYCFG`), здесь его нет.
* **Позиция бегунка — модель** — `x + value*(72-8)/(max-min)`; оригинал двигает
  кноб на 54 px от `x+1` (точные формулы — §3.5). Расхождение до 10 px у
  правого края, помечено в коде.
* **MOUSE SETUP не в раскладке** — три слайдера (105, 40/56/72) доказаны, но
  едут справа налево и принимают `double`; нужен флаг «реверс».
* **Нет полос слотов и маркера** — `FILESLOT` (124×10, `0x48ab90`) и
  `MENULIT` (16×16) не рисуются: у первого не снята раскладка шести полос, у
  второго не доказано, как он мигает/позиционируется. `MENUARRW` поддержан как
  `MenuWidget::Arrow`, но в `menu_widgets` не входит: стрелки списков
  (уровни/сейвы) зависят от прокрутки, которой в снимке нет.
* **Нет списков с прокруткой** — `items()` — статический снимок §3: 31 строка
  наборов клавиш, один слот сейва вместо шести, один уровень вместо списка,
  счётчик игроков, shareware-текст. Заголовки 30/34/35 (имя набора) не
  рисуются.
* **Текст — литералы** — `%s`/`%ld`/`%.25s` остаются в подписи как есть
  (`Player 1: %s`, `Mouse: %s`), значения из `KEYCFG`/глобусов не читаются.
* **Панель** — только `PLATE`, в `(0,0)`; экранная позиция копии не доказана
  (§2 выше). MAIN своего арта не получает.
* **Модели, а не байты** — `y` заголовка (10), раскладка MAIN (центр + шаг 10),
  раскладка 31-строчных списков от `-row`, формула бегунка; всё помечено в
  коде и здесь.
* **CLI виджеты не передаёт** — `--menu --png` рисует только текст и панель
  (§6); `main.cpp` в этом срезе не менялся.
* **Точный fade — и в CLI** — `--menu --menu-screen N --fade-lut K`; уровень
  11 = кадр без fade (0 отличий), 12..15 различаются (`cli_fade_lut`).
  (`main.cpp` в срезе 8 запрещён к правке): CLI по-прежнему применяет
  модельный `--fade` после `render_screen`, байты этого пути не изменились
  (§7).
* **Наблюдение вне среза:** `ui/pager_render.cpp` рисует строку 0 глифа как
  нижнюю (`y + cell_height - 1 - row`), из-за чего текст страницы пагера
  выходит вертикально перевёрнутым (проверено дампом `help.sme` в PNG). Тест
  `pager_render` считает только `ink`, поэтому этого не ловит. `menu_render`
  использует правильный порядок (`y + row`, §3.3). Файл `pager_render.*` в
  этом срезе не менялся — правка за основным агентом.

## 6. CLI (`--menu --png`) — сделано

`--menu --png OUT` рендерит **текущий** экран после `--menu-screen`/
`--menu-keys`, панель берётся из `FRONTEND/BMP.IDX`, шрифты 0/4/5 — из
атласов. Игра нужна только для атласов; без неё — `[error]` и код 2.

```sh
gunmetal -g "$GUNMETAL_DIR" --menu-screen 10 --menu-keys down,down \
         --png /tmp/control-options.png
render: CONTROL OPTIONS 320x200 items=10 drawn=10 ink=1466 missing=0
png: /tmp/control-options.png (256283 bytes)
```

Проверено визуально: панель `PLATE` 196×131 в `(0,0)`, красный заголовок
`CONTROL OPTIONS`, 10 пунктов (`Player 1: %s` … `Done`), один подсвечен
белым (font 4). MAIN рендерится на чёрном (своего арта нет). CTest
`cli_menu_png`.

Элементы управления (срез 3) живут только в API: у CLI нет ни флага «показать
виджеты», ни источника значений, а `main.cpp` в этом срезе запрещён к правке.
Подключение — следующим срезом: `menu_widgets(screen)` + значения из
`config.*`/KEYCFG, затем тот же `render_screen(menu, game, widgets, ...)`.

Осталось: origin копии `PLATE` с кадра игры, значения виджетов из
`GUNMETAL.CFG`/KEYCFG, `MOUSE SETUP` (реверс + `double`), `FILESLOT`,
`MENULIT`, прокрутка списков, динамические подписи, звуки, карусель MAIN
(`SCROLL` + entry-art), CLI-флаг `--menu --fade-lut N` (§7; API готов,
`main.cpp` — следующий срез).

## 7. Точный fade (`fade_lut`, срез 8)

У `ui/pager_render` два пути fade (`docs/PAGER_RENDER.md` §2.5/§2.6); кадр меню
получает их же, через `MenuRenderOptions`:

* **`fade`/`fade_level` — модель.** `render/upscale.h::fade_rgba` умножает
  каналы готового RGBA-кадра на `k/11` (уровень 11 — тождество, 0 — чёрный,
  12..15 клампятся к 11). Это ровно то, что CLI делает сегодня после
  `render_screen`, поэтому байты модельного пути не изменились.
* **`fade_lut`/`fade_lut_level` — точный путь.** `0x682f90` — таблица
  **индекс → индекс**, поэтому кадр собирается в палитровых индексах
  (`MenuBackground::indices`, `MenuSprite::indices`, индексы глифов атласа),
  индекс проходит через `render/hud_lut.h::map(level, index)` **до** выбора
  цвета, и цвет берётся из палитры **своего** источника (плата/спрайт/атлас).
  Уровни 12..15 берутся из таблицы как есть и светлеют, а не повторяют 11.
  `fade_lut` приоритетнее `fade`.

Таблица берётся из `fade_lut_table` (уже загруженная, в том числе
`build_hud_lut` из `FRONTEND/GAMEPAL` — файл не нужен) или из
`fade_lut_dump` (4096 байт, `load_hud_lut`). Нет ни того, ни другого — отказ
`menu_render: fade LUT needs a table (fade_lut_table) or a dump
(fade_lut_dump)`, а не тихо несфейженный кадр. Ошибки чтения дампа идут с
префиксом `menu_render: ` + текст `hud_lut`.

**Доказано тестом** (`menu_render`, SOUND OPTIONS id 11: `PLATE` 196×131,
два слайдера и включённый чекбокс `CHECK`; 64000 пикселей):

| Проверка | Число |
|---|---|
| уровень 11 против кадра без fade | **0** отличий из 64 000 |
| уровень 0: чёрных пикселей | **63 963** (модель: 64 000) |
| уровень 0: отличий от модели | **37** (пиксели `CHECK` индексов 243/244) |
| 11/12, 12/13, 13/14, 14/15: отличий между соседними | 16 246 / 17 736 / 23 108 / 20 284 |
| 12/13/14/15 против уровня 11 | 16 246 / 24 811 / 25 165 / 25 522 |
| модель при `fade_level=12` против кадра без fade | 0 (кламп к 11) |
| модель `fade_level=5` против `fade_rgba(frame, 5, 11)` | 0 (байты CLI не сдвинулись) |
| путь «каталог игры», уровень 13 против ручного | 0 (опции доходят) |
| `build_hud_lut` из `GAMEPAL`, FNV-1a64 | `0x6330406254771f3d` (= дамп) |

Почему уровень 0 отличается от модели **на этом** кадре: `PLATE` и `FONTS4` не
используют ни одного из 20 слотов skip-списка `0x483250`
(`{173,174,175,191} ∪ {240..255}`), поэтому в чисто текстовом кадре
(CONTROL OPTIONS) таблица на уровне 0 тоже даёт 64 000 чёрных. Единственный
ассет меню с такими индексами — `CHECK` (243/244, тёмно-красные), 37 пикселей;
`MENUARRW` использует 241/242/247/250, но в `menu_widgets` стрелки не входят.

Ограничения:

* **Палитра — на источник.** Цвет берётся из палитры пикселя, а не из общей
  `FRONTEND/GAMEPAL`: у `PLATE`, у каждого спрайта и у каждого лица `FONTS4`
  своя палитра, и слоты вне рампы (`0..2`, 240..255) в них разные. Движок
  читает весь 8-bpp буфер через `GAMEPAL`, поэтому на других кадрах единичные
  пиксели спрайтов могут отличаться от однопалитрового чтения (та же оговорка,
  что в `docs/PAGER_RENDER.md` §2.6).
* **Слой без индексов не фейдится.** `MenuBackground`/`MenuSprite`, собранные
  вручную без `palette`/`indices`, своих индексов не имеют — точный путь
  оставляет такие пиксели как есть. `load_menu_panel`/
  `load_menu_widget_assets` заполняют оба поля из BMP.
* **Чёрный холст** вне панели индекса не имеет и остаётся чёрным на всех
  уровнях (в движке буфер всегда 8 bpp и всегда проходит через LUT).
* **Индекс вне палитры — отказ** (`menu_render: fade LUT level L maps index I to
  M, outside the K-entry palette`); с 256-записной палитрой недостижим.
* **Статистика считается до таблицы**: `ink_pixels`, `widget_ink`,
  `background_pixels` — свойство композиции, а не цвета.
* **CLI не подключён** — `--menu --fade-lut N` остаётся следующим срезом
  (`main.cpp` здесь не менялся); модельный `--fade` работает как раньше и
  побайтово равен `MenuRenderOptions::fade`.
