# Рендер экрана 23 `SET DIFFICULTY LEVEL` (`engine/src/ui/difficulty_render.h`)

Срез 2 пункта `M8.8`: модуль **рисует экран 23 — `SET DIFFICULTY LEVEL`** — в
RGBA-буфер 320×200: заголовок, шесть строк (`Private` … `Cancel`) с подсветкой
курсора шрифтом 4 и опциональную плату `PLATE`.  Состояния у экрана нет — это
плоский список из шести рангов, поэтому кадр строится целиком из опций.
Кирпичи те же, что у соседей: атласы `FONTS4` из `FRONTEND/BMP.IDX`
(`render/text_layout`), плата 196×131 `0x486050`, точные таблицы fade `0x682f90`
(`docs/START_GAME_RENDER.md` §6, `docs/PAGER_RENDER.md` §2.6).

Код: `engine/src/ui/difficulty_render.h/.cpp`; тест —
`engine/tests/test_difficulty_render.cpp` (CTest `difficulty_render`, label
`quick`).  Канон поведения — `docs/UI_SCREENS.md` §2/§3, образцы кадра —
`docs/START_GAME_RENDER.md` (13), `docs/LEVEL_SELECT_RENDER.md` (17),
`docs/KEYCFG_RENDER.md` (30/34/35).

Проверка:

```sh
cmake --build build/sandbox --target gm_difficulty_render_tests -j 4
./build/sandbox/engine/tests/gm_difficulty_render_tests
ctest --test-dir build/sandbox -R '^difficulty_render$' --output-on-failure
python3 regress.py --mirror --json
```

## 1. Что доказано (`0x48a9a0`, docs/UI_SCREENS.md §2)

Обработчик — девять инструкций и шесть вызовов текста, до `ret` в `0x48aa5a`
(листинг: `python3 tools/re/disrange.py 48a9a0 48aa60`).  Всё в таблице — его
сайты, не догадка по картинке.

| факт | значение | источник |
|---|---|---|
| число строк | **6** (`[0x67f10c] = 6`) | `0x48a9a0` |
| шаг строки | **15** (`[0x662d58] = 0xf`) | `0x48a9aa` |
| плата | `0x48a9b4` вызывает `0x486050` — копию 196×131 `PLATE` в `0x6865f8`; текст пишется в неё (клип 196×131) | `0x48a9b4`; §3 |
| заголовок | `SET DIFFICULTY LEVEL` (`0x4bdbd8`), шрифт **5**, по центру, `x 38`, `row 14`, `item -1` → **y 14** | `0x48a9b9`..`0x48a9c8` |
| строки | `x 40`, `row 27`, `item 0..5`, шрифт **0** → `y = 27 + 15·item` | `0x48a9d0`..`0x48aa57` |
| item 0 | `Private` (`0x4bdbd0`) → **y 27** | `0x48a9d0`..`0x48a9e4` |
| item 1 | `Corporal` (`0x4bdbc4`) → **y 42** | `0x48a9e7`..`0x48a9fb` |
| item 2 | `Lieutenant` (`0x4bdbb8`) → **y 57** | `0x48a9fe`..`0x48aa12` |
| item 3 | `Captain` (`0x4bdbb0`) → **y 72** | `0x48aa15`..`0x48aa29` |
| item 4 | `Major` (`0x4bdba8`) → **y 87** | `0x48aa2c`..`0x48aa40` |
| item 5 | `Cancel` (`0x4bda0c`) → **y 102** | `0x48aa43`..`0x48aa57` |
| подсветка | `item == [0x68ccf4]` и текст не пуст → шрифт принудительно **4** (`0x485e0d`..`0x485e19`) | §3 |
| курсор при входе | **2** (`Lieutenant`): `[0x68ccf4] = 2` пишут оба входа в состояние 23 — из `START GAME` (item 0) и из списка уровней | `0x48daa0`, `0x48e28b` |
| `ESC` | **отдельной строки и ветки нет**: в обработчике нет `ESC TO CANCEL`, а в клавиатурном диспетчере состояние 23 не сравнивается с `0x1b`; назад — item 5 `Cancel` | §4 (нет строки 23), `0x48e29f` — единственное `cmp ebp,0x17` |
| стрелки/слайдеры/чекбоксы | **нет** ни одного вызова: только `0x486050` и шесть `0x485dd0` | весь листинг `0x48a9a0`..`0x48aa5a` |
| порядок | плата → заголовок → item 0 … item 5 | порядок вызовов |

Модель `0x485dd0` (общая для §3): `centered` даёт `x = (196 − width)/2`
(`0x485de2`..`0x485dfa`) и игнорирует переданный `x 38`; `item == -1` делает
строку абсолютной (`y = row`), `item >= 0` — `y = row + item·step`.  Поэтому
заголовок стоит на y 14, а `Cancel` — на 102, хотя все шесть вызовов передают
один и тот же `row 27`.

### 1.1. Что делает выбор ранга (модель, вне рендера)

Ввод состояния 23 — единственная ветка `cmp ebp,0x17` (`0x48e29f`); выбор
пункта копирует 16-байтовое имя ранга в буфер позывного `0x4bc2b8` (таблица
переходов `0x48e960`: `0x48e2d6` `Private` … `0x48e35e` `Major`; `0x48e378` —
fallback `First Seaman`, `0x4bdd14`, достижимый только при курсоре вне 0..5) и
продолжает на экран имени игрока 24 (`0x48e3c7`).  `Cancel` — ветка
`cursor >= rows − 1` (`0x48e2b5`..`0x48e2b7` → `0x48e473`), не ранг.
Это поведение **не** входит в модуль: рендер не читает и не пишет ни одного из
этих глобалов.

## 2. Модель кадра (не доказано кодом)

* **Курсор.** `DifficultyRenderOptions::cursor` — это `[0x68ccf4]`, индекс
  0..5; вне диапазона зажимается.  В оригинале при входе стоит 2; значение
  задаёт вызывающий (API-дефолт структуры — нейтральный 0,
  `kDifficultyDefaultCursor = 2` — доказанный первый кадр).
* **Цвета** — палитра атласа `FONTS4`: заголовок (шрифт 5) → `9b0f0f`
  (тёмно-красный), строки (0) → `130f0f` (тёмно-серый), подсветка (4) →
  `efefef` (белый), тени `6b473b`/`4b4b4b`/`afafaf`.  Привязка «номер шрифта →
  грань атласа» — из `text_layout::font_table_spec` (`UI_SCREENS.md` §11),
  проверена пикселями.
* **Плата.** `PLATE` рисуется в (0,0) в порядке `parse_bmp`, как в соседних
  рендерах; экранный origin копии `0x6865f8` не документирован (`UI.md` §8).
  Модуль **не** рисует плату неявно: её включает `options.background =
  "PLATE"`; пустая строка — чёрный холст.
* **Клип.** Глифы обрезаются полосой 196×131 (буфер `0x485dd0`).  Самый
  широкий текст — заголовок (103 px в тесте), так что клип — страховка.
* **`rows`/`arrows` в статистике.** `rows` = 6 (все строки всегда рисуются);
  `arrows` = 0 всегда — у экрана нет прокрутки и вызова `MENUARRW`; поле
  оставлено ради общей формы статистики соседних рендеров и закреплено тестом.
* **Fade.** `fade_lut` — точный путь `0x682f90`: таблицы индекс → индекс,
  поэтому уровень применяется к индексу палитры каждого нарисованного пикселя
  **до** поиска цвета (`render/hud_lut.h::map`).  Уровни 0..15 берутся как
  есть (12..15 — реальные таблицы, они brighten).  Таблица — из памяти
  (`fade_lut_table`) или из дампа 4096 Б (`fade_lut_dump`, `docs/HUD_LUT.md`
  §2); без обоих — текстовый отказ, не «молча без fade».

## 3. API

```cpp
struct DifficultyRenderOptions {
    std::string background;          // "" = чёрный холст; "PLATE" — плата 13/23
    int cursor = 0;                  // [0x68ccf4], 0..5; вне — кламп
    bool fade_lut = false;
    int fade_lut_level = kHudLutIdentityLevel;  // 11 — тождество, 0..15
    std::string fade_lut_dump;       // 4096-байтовый hud_lut.bin
    const HudLut* fade_lut_table = nullptr;     // приоритетнее дампа
};

struct DifficultyRenderStats {
    int rows = 0;            // 6
    int ink_pixels = 0;      // уникальные пиксели глифов
    int missing_glyphs = 0;  // символы без глифа (пропускаются)
    int arrows = 0;          // всегда 0: на экране нет MENUARRW
};

bool render_difficulty(const std::filesystem::path& game,
                       const DifficultyRenderOptions& options,
                       std::vector<std::uint32_t>& rgba,
                       int& width, int& height,
                       DifficultyRenderStats* stats = nullptr,
                       std::string* error = nullptr);
```

Константы: `kDifficultyTitle`, `kDifficultyItems[6]`, `kDifficultyRows = 6`,
`kDifficultyItemStep = 15`, `kDifficultyTitleY = 14`,
`kDifficultyItemX = 40`, `kDifficultyItemFirstY = 27`,
`kDifficultyDefaultCursor = 2`, `kDifficultyPanelName = "PLATE"`,
`kDifficultyPanelWidth/Height = 196/131`,
`kDifficultyTitleFont/RowFont/SelectedFont = 5/0/4`.

Контракт ошибок: любой отказ — текст с префиксом `difficulty_render:` (пустой
каталог игры, нет/битый `FRONTEND/BMP.IDX`, нет грани 5/0/4, нет/битый/не-8bpp
фон, fade без таблицы и дампа или с нечитаемым дампом).  При ошибке `rgba`
очищается, `width`/`height` = 0, `*stats` обнуляется.  Успех: `width == 320`,
`height == 200`, `rgba.size() == 64000`; вызов только читает установку,
детерминирован и не зависит от текущего каталога.

## 4. Числа (прогон на поставочных ассетах)

`./build/sandbox/engine/tests/gm_difficulty_render_tests` — **206 проверок**,
все зелёные.  Печатаемые числа (v1.36, `/mnt/d/games/Gunmetal 3dfx`):

```
install: rows=6 ink=768 arrows=0 missing=0;
  title y14 (46,14)-(148,20) 103x7, 250 px;
  Private (40,27)-(70,33) 31x7, 81 px;
  Cancel  (40,102)-(66,108) 27x7, 76 px
cursor:  c0: 0 px vs c0, c1: 175, c2: 195, c3: 167, c4: 148, c5: 157;
  min pairwise diff 143 px; clamps -5->0, 99->5
plate:   PLATE 196x131, panel 25676 px, 768 under glyphs, 38324 outside black;
  plate-vs-black diff 24908 px
fade:    level 11 diffs 0/64000; level 0 black 64000 (plain 38324), diffs 25676;
  adjacent 12/13 17923, 13/14 23637, 14/15 20812;
  vs 11 16831/24994/25233/25560
```

Что именно закреплено:

* `rows == 6`, `arrows == 0`, `missing_glyphs == 0`, `ink == 768` = сумма
  независимых blit-полос семи текстов (полосы не пересекаются);
* каждая из семи полос (заголовок + шесть строк) побайтово равна независимой
  отрисовке `compose_band` в тесте; bbox заголовка/первой/последней строки
  печатается и проверяется (`y0` = 14 / 27 / 102, расстояние 5·15 = 75,
  `x0` заголовка = центрирование `(196 − 103)/2 = 46`);
* курсор 0..5: кадр равен независимому композиту с шрифтом 4 на этой строке,
  отличия от кадра `cursor 0` лежат только в двух затронутых полосах
  (175/195/167/148/157 px), все шесть кадров попарно различаются
  (минимум 143 px), `-5 → 0`, `99 → 5`;
* `PLATE`: все 25676 пикселей панели равны независимо декодированной `PLATE`
  (с учётом файлового порядка строк BMP), 768 пикселей под глифами совпадают с
  чёрным кадром, 38324 пикселя вне панели остаются чёрными;
  `menu_panel_asset(Screen::Difficulty) == "PLATE"`;
* fade: уровень 11 — 0 отличий из 64000 от кадра без fade; уровень 0 —
  64000 чёрных (в чёрном кадре 38324) и 25676 отличий; 12..15 попарно
  различаются (17923/23637/20812) и все отличаются от 11
  (16831/24994/25233/25560); 99 → 15; негативы — тексты `difficulty_render:`;
* негативы без установки: пустой путь, каталог без `BMP.IDX`, синтетический
  `BMP.IDX` только с `FONTS1` (→ «FONTS4»), неизвестный фон `NOSUCH`;
* детерминизм: два одинаковых вызова дают 0 отличающихся пикселей.

## 5. Ограничения

* **Нет ввода и перехода.** Модуль только рисует кадр: не двигает курсор, не
  обрабатывает Enter/Esc, не пишет `[0x4bc2b8]`/`[0x4bc440]`.  Переходы
  13→23, 17→23 и 23→24 — в `ui/frontend_menu` (`docs/UI_SCREENS.md` §4).
* **Нет звука.** `updown`/`select` и прочие UI-звуки — `ui/ui_sounds`,
  `docs/UI_SCREENS.md` §8.
* **CLI — есть (срез 2), связи с состоянием сложности нет.**
  `--difficulty [--difficulty-cursor N] [--no-plate] [--png OUT]
  [--fade-lut N]`: `difficulty: rows=6 cursor=2 ink=768 missing=0 arrows=0`;
  курсор 5 — `cursor=5`. CTest `cli_difficulty`, `_cursor`;
  `cli_fade_lut` (11 = без fade). Модуль по-прежнему не читает и не пишет
  игровое состояние сложности.
* **Плата не рисуется неявно.** Без `background = "PLATE"` кадр — текст на
  чёрном; это осознанный контракт соседних рендеров, а не расхождение с
  оригиналом.
* **Шрифты.** Номера 5/0/4 — из таблицы `text_layout`; если в установке нет
  `FONTS4`, кадр не рисуется (отказ), а не подменяется другим атласом.
