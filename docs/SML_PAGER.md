# SML-пагер: runtime-курсор страниц (`engine/src/ui/sml_pager.h`)

Модуль `SmlPager` — тонкая обёртка над [`render/text_layout`](TEXT_LAYOUT.md):
раскладчик уже режет документ на страницы 267×167, пагер только держит
**курсор** (текущая страница, текущая визуальная строка, строка статуса).
Сам он не раскладывает текст, не рисует и ничего не пишет; из установки
читается только `FRONTEND/BMP.IDX` для атласов шрифтов и сам `.sml/.sme`.

Это срез 2 пункта `M8.1` (экран 2 `SML PAGER`, обработчик `0x487b69`,
[UI.md](UI.md) §10, [UI_SCREENS.md](UI_SCREENS.md) §1/§2). Срез 1 —
состояние фронтенда `frontend_menu` ([FRONTEND.md](FRONTEND.md)); рендер
экрана — `Screen` ([SCREEN_CPP.md](SCREEN_CPP.md)); раскладка и golden —
[TEXT_LAYOUT.md](TEXT_LAYOUT.md).

## 1. API

```cpp
#include "ui/sml_pager.h"

gm::SmlPager pager("/mnt/d/games/Gunmetal 3dfx");   // атласы из BMP.IDX
std::string error;
if (!pager.open("CREDITS.SML", &error)) { /* error: текст ошибки */ }

pager.page_count();   // 5
pager.page();         // 0
pager.next_page();    // true, page() == 1
pager.status();       // "page 2/5"
pager.lines();        // строки текущей страницы (LayoutLine, пиксели страницы)
pager.images();       // <I>-float'ы текущей страницы (LayoutImage)
```

| Метод | Назначение |
|---|---|
| `SmlPager(game, options)` | читать атласы из `game/FRONTEND/BMP.IDX` при первом `open()` |
| `SmlPager(FontLookup, options)` | атласы даёт вызывающий (тесты, синтетика); живут дольше пагера |
| `open(const SmlDocument&, error)` | разложить готовый документ, курсор на первую страницу |
| `open(const path&, error)` | прочитать файл, `parse_sml`, разложить; путь ищется как есть, затем без учёта регистра в `game` и `game/SML` (`HELP.SME` → `help.sme`) |
| `reset()` | забыть документ/страницы/курсор; загруженные атласы остаются в кэше |
| `opened()`, `error()` | состояние и текст последней ошибки |
| `page_count()`, `page()` | число страниц и номер текущей (0-based) |
| `next_page()` / `prev_page()` | `0x4986b0` / `0x498750`: целые страницы |
| `first_page()` / `last_page()` | `0x498600(0)` / последняя; `true`, только если страница сменилась |
| `show_page(int)` | переход на страницу; вне диапазона — `false` без изменений |
| `lines()`, `images()`, `current_page()` | данные текущей страницы из `text_layout` |
| `line_count()`, `line()`, `scroll_next_line()`, `scroll_prev_line()` | курсор визуальной строки по всему документу (см. §3) |
| `page_width()`, `page_height()` | 267×167 по умолчанию |
| `document()`, `layout()` | разобранный документ и полный `LayoutResult` |
| `status()` | `"page 2/7"`, для пустого/закрытого — `"0/0"` |

Инварианты границ (проверены тестом):

* до `open()` / после `reset()` — 0 страниц, все переходы `false`, курсор не
  двигается, `status() == "0/0"`;
* `next_page()` на последней и `prev_page()` на первой — `false`, не меняются
  ни страница, ни строка;
* `first_page()`/`last_page()`/`show_page()` возвращают `true` только при
  реальном переходе;
* неудачный `open()` оставляет пагер закрытым (0 страниц) и заполняет
  `error()`; предыдущий документ не остаётся «наполовину» доступным.

## 2. Что берётся из `text_layout` (и какие числа)

| Что | Источник | Число |
|---|---|---|
| страницы | `LayoutResult::pages` | 267×167 = `0x4972f0(0x10b, 0xa7)` |
| границы страницы | `LayoutPage::start` / `end` | байтовые смещения в `SmlDocument::text`, `end == 0` — последняя |
| строки страницы | `LayoutPage::lines` (`x, y, width, height, font, first, last`) | page-relative пиксели |
| `<I>`-float'ы | `LayoutPage::images` | слот 64×64 (`SmlPagerOptions::image_width/height`) |
| высота строки | `LayoutOptions::line_height/big_line_height` | 10 / 18 (`0x4983c3`) |
| шрифт до первого `<f>` | `LayoutOptions` / `default_font` | 1 (`0x497a99`) |
| предел страниц | `LayoutOptions::max_pages` | 256 в `text_layout` (движок обрывает листание на 99, `0x4986b0`) |
| перенос | `LayoutOptions::wrap` | жадный, `0x497c20` |

Пагер не пересчитывает ни одного из этих чисел: тест сравнивает каждый
`page_count()`, `lines().size()`, `images().size()` и `line_count()` с
независимым вызовом `layout_text` на том же документе.

## 3. `<p>` — не разрыв страницы

По [TEXT_LAYOUT.md](TEXT_LAYOUT.md) §3.4 `<p>` (`0x497da0`) — это «полстроки
отступа + перевод строки»; если строка уже полная (`x == right`), добавка
пропускается. Страница кончается только тогда, когда следующая строка не
влезает в 167 px, либо на `</d>`. Поэтому у пагера нет своей семантики `<p>`:
он листает уже нарезанные `LayoutPage`.

Отличие маркера продолжения от буквального кода (`0x497c08` пишет маркер
только когда следующая строка ещё влезает, C++ пишет после тега) описано в
TEXT_LAYOUT.md §3.5 и здесь не дублируется.

Курсор строки — **UI-удобство, а не доказанное поведение оригинала**:
движок листает только страницы (`0x4bc43c` — номер страницы, `0x4bc3a8` —
флаг), построчной каретки у пагера нет. `scroll_next_line()` /
`scroll_prev_line()` двигают абсолютный индекс визуальной строки по всему
документу и переводят страницу вслед за ней; на границах возвращают `false`
без изменений. Та же модель, что у `Screen::scroll`, но без выделения рамкой.

## 4. Числа теста

`ctest -R sml_pager` (`engine/tests/test_sml_pager.cpp`, label `quick`).
Корпус — шесть документов с разным числом страниц (числа печатает сам тест):

```
$ ./build/sandbox/engine/tests/gm_sml_pager_tests
  prelevel/level21.sme: 1 pages, lines 13 (13), status page 1/1
  design/weapon20.sme: 1 pages, lines 2 (2), status page 1/1
  help.sme: 3 pages, lines 15/15/12 (42), status page 1/3
  brief/level10.sme: 3 pages, lines 16/16/3 (35), status page 1/3
  CREDITS.SML: 5 pages, lines 12/13/14/15/10 (64), status page 1/5
  brief/level22.sme: 5 pages, lines 16/16/15/16/11 (74), status page 1/5
sml_pager corpus: 6 documents, 18 pages, 230 lines
  game constructor: SML/help.sme -> page 1/3
  game constructor: SML/CREDITS.SML -> page 1/5
  game constructor: design/weapon20.sme -> page 1/1 (font 3 loaded after a font-1 document)
sml_pager: cursor over text_layout pages (267x167), line cursor, boundary
invariants and negatives; OK
```

Атласы кэшируются по шрифту, но набор «какие шрифты нужны» считается для
каждого документа заново: `level21.sme` (только шрифт 1) и следующий за ним
`weapon20.sme` (`<F S="3">`) открываются подряд одним и тем же пагером —
последняя строка вывода это и проверяет.

Тест проверяет: совпадение страниц/строк/картинок с `layout_text`; ровно
`page_count()-1` успешных `next_page()`, затем `false` и неизменный курсор;
симметричный проход назад; `first_page`/`last_page`/`show_page`; обход всех
230 строк с переходом страницы; все строки внутри 267×167; негативы — пагер
до `open()`, пустой документ, `<d></d>` (одна страница без строк), битый файл
(сам исходник теста как «не-SML»), шрифт 6 (нет атласа), пагер без источника
шрифтов; `reset()` и повторный `open()`.

## 5. Ограничения (что срез 2 сознательно не делает)

* **Нет рендера 320×200.** Пагер не собирает кадр `HELPSCRN`/`LOADPAGE`/
  `BRIEFBG2`, не кладёт текст на фон и не отдаёт RGB. Это делает `Screen`
  (`docs/SCREEN_CPP.md`); пагер отдаёт только `LayoutLine`/`LayoutImage`.
* **Нет атласов, кроме шрифтовых.** Читается `FRONTEND/BMP.IDX` через
  `load_font`; `<I>`-битмапы (`SML/<f>.bmp`) не декодируются — от `text_layout`
  берутся только метрики и слоты float.
* **Нет звука `updown`** (`0x4bc454`, [UI_SOUNDS.md](UI_SOUNDS.md)); навигация
  не проигрывает event 1.
* **Нет пунктов меню `0x485dd0`** и заголовка экрана: пагер не является
  экраном фронтенда, `frontend_menu` его не вызывает.
* **Нет клавиатуры/переходов.** `Escape`/`Enter`/`Left`/`Right` не
  обрабатываются: методы вызываются напрямую; переходы состояний 2/3/4 —
  зона `frontend_menu`.
* **Нет предпочтения `.sme`-соседа.** `open(path)` читает ровно указанный
  файл; загрузчик движка сам подставляет `…SML` → `…SME` (`0x497310`).
  Выбор файла — за вызывающим.
* **Нет поведения «99 страниц → `...` и сброс на первую».** Предел
  `max_pages` взят из `text_layout`: превышение — ошибка раскладки
  (`truncated`), а не тихое листание.
* **Одна модель `<p>`** — та, что в `text_layout`; расхождение маркера
  продолжения с буквальным `0x497c08` остаётся задокументированным
  исключением.

## 6. Что осталось для CLI (`--pager`)

В `engine/src/main.cpp` (вне списка файлов среза) команды нет. Чтобы получить
`gunmetal --pager SML/CREDITS.SML [--page N]` нужно:

1. создать `SmlPager` с путём игры из `--game`/`GM_GAME`;
2. `open(path)`, при ошибке — `[error] pager: <текст>` и `rc=2`;
3. напечатать `status()` и строки текущей страницы (номер, `x/y/w/h`, шрифт,
   диапазон байт `first..last`) — либо заголовки страниц по `--page`/`--all`;
4. для картинки — подключить `Screen` (там уже есть атласы и RGB), пагер
   остаётся источником страниц;
5. опционально: `--pager-keys next,next,prev` для проверки переходов в CLI.

Сам модуль для этого готов: `open`, переходы, `lines()`, `status()`
не требуют ни окна, ни SDL, ни установки сверх `FRONTEND/BMP.IDX`.

## 7. Проверка (выполненные команды)

Сборка дерева (портативный тулчейн, `docs/BUILD.md`):

```
$ export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
$ cmake --build build/sandbox -j 4
[66/66] Linking CXX executable engine/tests/gm_save_sector_stuff_tests
```

Полный CTest, вместе с новым `sml_pager`:

```
$ ctest --test-dir build/sandbox -j 4
        Start  55: sml_pager
 72/133 Test  #55: sml_pager ........................   Passed    0.19 sec
...
100% tests passed out of 133
Label Time Summary:
full     = 626.34 sec*proc (13 tests)
quick    = 175.34 sec*proc (120 tests)
Total Test time (real) = 226.21 sec
```

Python-оракул (изменений в Python нет, прогон для ворот):

```
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 60.1, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```

`ctest -R 'sml_pager|text_layout|screen'`: 6/6 passed (новый тест, раскладчик
и экранный runtime не разошлись по числам страниц).

