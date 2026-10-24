# Брифинги миссий: `SML/brief/*.SME` -> уровень, страницы, строки

Модуль `engine/src/ui/briefing.h/.cpp` — тонкий индекс над пагером:
`briefing_levels()` перечисляет уровни, у которых есть брифинг, а
`load_briefing()` находит файл, декодирует его тем же `parse_sml`
(`decode_best` + plain-фолбэк), раскладывает `SmlPager` на страницы 267×167 и
отдаёт страницы/строки/глифы/текст как данные. Это срез 1 пункта `M8.3`.

Смежные документы: формат `.sml/.sme` и корпус — [UI.md](UI.md) §1–§4;
C++-читатель разметки — [SML_CPP.md](SML_CPP.md); пагер — [SML_PAGER.md](SML_PAGER.md);
рендер страницы и `glyphs` — [PAGER_RENDER.md](PAGER_RENDER.md); раскладка —
[TEXT_LAYOUT.md](TEXT_LAYOUT.md).

## 1. Доказано (формат, файлы, страницы)

* **Файлы.** В `SML/brief/` поставки 49 файлов: **25 `.sme` и 24 `.sml`**
  (строка `brief/` в [UI.md](UI.md) §1; проверено `ls` и `dir.txt`). В
  задании среза стояло «149», но 149 — это число **всех** `.sml` в `SML/`
  (300 файлов: 151 `.sme` + 149 `.sml`), а не файлов брифингов.
* **Именование.** Загрузчик строит `SML\BRIEF\LEVEL%ld.SML` (`0x4bde48`),
  вариант с буквенным суффиксом — `LEVEL%ld%c.SML` (`0x4bde64`), и предпочитает
  `.SME`-соседа (`0x497310`, [UI.md](UI.md) §3). Поэтому **уровня 0 в виде
  `LEVEL0.SME` нет**: миссия 0 поставляется как `LEVEL0A.SME`
  (`SML/brief/dir.txt` перечисляет `level0a`, не `level0`). `load_briefing("level0")`
  — корректный отказ «file not found», а не ошибка индекса.
* **Набор уровней** (25, нижний регистр, сортировка):
  `level0a, level1, level2, level3, level4, level5, level6, level7, level8,
  level9, level10, level11, level12, level13, level14, level15, level16,
  level17, level18, level19, level20, level21, level22, level23, level24`.
  Пропусков «по номеру» нет только потому, что буква `A` у нуля и нет
  отдельных 25+; список — это ровно `dir.txt`.
* **Декодирование.** `.sme` — позиционный шифр Цезаря по латин-1
  ([UI.md](UI.md) §3). `load_briefing` использует `parse_sml` с `SmlSource::Auto`:
  `decode_best` (три варианта: `as-is`, `newline-repair`, `crlf-strip`) и
  фолбэк на plain-байты. Это лучше старого `decode_auto`, который не видел
  CRLF-аномалии и не портирован в C++ ([SML_CPP.md](SML_CPP.md) §4.7).
  Из 25 файлов **24 декодируются `as-is`**, `brief/level17.sme` —
  `newline-repair` (врождённый лишний CRLF, [UI.md](UI.md) §4.1).
* **Кодировка текста.** Все 25 брифингов после декодирования — чистый ASCII
  (байты `0x20..0x7e`; проверено по всем 36 978 декодированным символам).
  Поэтому «latin-1 или UTF-8» на этих данных неразличимы, и тест сравнивает
  **байты**, а не предполагаемый английский текст.
* **Страницы.** Пагинация — не модель: `SmlPager` берёт её из
  `render/text_layout` (`LayoutPage`, 267×167, `0x4972f0(0x10b, 0xa7)`,
  [SML_PAGER.md](SML_PAGER.md) §2). Число страниц/строк брифинга — это
  `LayoutResult::pages` и `LayoutPage::lines`; `page.index` — их индекс.

## 2. Модель (что именно считается)

| Поле | Определение | Источник |
|---|---|---|
| `Briefing::source` | прочитанный файл, написание как на диске | результат поиска |
| `Briefing::level` | ствол в нижнем регистре (`level0a`) | нормализация имени |
| `BriefingPage::index` | 0-based номер страницы | `LayoutPage::index` |
| `BriefingPage::lines` | визуальные строки страницы | `LayoutPage::lines` |
| `BriefingPage::glyphs` | символы всех `LayoutRun::text` страницы (пробелы входят, `<k>` уже `Soldier`, `<s>` — пробел) | `LayoutRun::text` |
| `BriefingPage::text` | текст каждой визуальной строки, обрезанный по краям | `LayoutRun::text`, склейка прогонов одной базовой линии |
| `page_count()` | число страниц | `pages.size()` |
| `line_count()` | сумма `lines` | — |
| `plain_text()` | все строки через `\n` | — |

Решения, которые пришлось принять (и почему):

1. **Поиск файла — сканирование `SML/brief`, а не подстановка точного имени.**
   Поставка лежит на регистронезависимой Windows-ФС и на Linux-ФС как есть
   (`LEVEL0A.SME`, `level10.sme`, `Level24.sme`). `resolve_ci` находит сам
   каталог, затем сравниваются стволы в нижнем регистре. Если бы встретились
   два ствола, отличающиеся регистром, берётся первый по сортировке — в
   поставке таких нет.
2. **`level` — имя, не путь.** `/`, `\`, `.` и `..` отвергаются: модуль
   читает только `SML/brief/<level>.SME`, а не произвольный файл.
   Допускается суффикс `.sme` в любом регистре; `.sml` отвергается, потому что
   индекс описывает основную (закодированную) форму, а `.sml` — резервную
   редакцию ([UI.md](UI.md) §3).
3. **Глифы — разложенные символы, а не запрос к атласу.** Иначе модуль зависел
   бы от `FRONTEND/BMP.IDX` и не работал бы на metrics-only установке. Для
   поставки это ровно `PageRenderStats::glyphs`: на трёх брифингах тест
   рендерит каждую страницу через `ui/pager_render` и сверяет
   `stats.glyphs == page.glyphs` и `missing_glyphs == 0`.
4. **`text` обрезан по краям, `glyphs` — нет.** Хвостовой пробел переноса
   остаётся в `LayoutRun::text` (и в счётчике глифов, как у рендера), но в
   отображаемой строке он не нужен. Поэтому `line_count`/`glyphs` сходятся с
   пагером/рендером, а `text` читается как есть.
5. **Сравнение с Python — по символам без пробелов.** Оракул —
   `gm/sml.py:describe()["body"]` (все теги сняты, `<k>`/`<s>` подставлены,
   пробелы схлопнуты). Раскладка может разорвать слово на границе строки
   (`level9`: «cra» + «te»), и разрыв не является частью документа. Поэтому
   тест сравнивает FNV-1a64 последовательности **без пробелов** — любая
   потерянная/лишняя буква или перестановка всё равно меняет хэш. На 25/25
   значения совпали.

## 3. API

```cpp
#include "ui/briefing.h"

gm::Briefing briefing;
std::string error;
if (!gm::load_briefing("/mnt/d/games/Gunmetal 3dfx", "level1", briefing, &error)) {
    // error: "briefing: ..." (см. таблицу)
}
briefing.level;         // "level1"
briefing.source;        // ".../SML/brief/LEVEL1.SME"
briefing.page_count();  // 2
briefing.line_count();  // 20
briefing.plain_text();  // строки через '\n'

for (const gm::BriefingPage& page : briefing.pages) {
    page.index;  // 0-based
    page.lines;  // визуальных строк
    page.glyphs; // символов на странице
    page.text;   // по строке на элемент (первые строки см. §4)
}

for (const std::string& level : gm::briefing_levels(game)) {
    // "level0a", "level1", ... — все *.sme, нижний регистр, сортировка
}
```

| Функция / поле | Назначение |
|---|---|
| `load_briefing(game, level, out, error)` | найти, декодировать, разложить и разложить по страницам; `false` + текст при отказе |
| `briefing_levels(game)` | стволы всех `SML/brief/*.sme` (наличие файла, не проверка декодируемости); пусто, если установки/каталога нет |
| `Briefing::page_count/line_count/plain_text` | счётчики и склейка строк |
| `BriefingPage` | `index`, `lines`, `glyphs`, `text` |
| `Briefing` | `source`, `level`, `pages` |

Инварианты (проверены тестом):

* на любом отказе `out` очищен: `page_count() == 0`, `line_count() == 0`,
  `plain_text()` пуст, `source` пуст; `*error` начинается с `briefing:`;
* на успехе `*error` пуст, `level` — нормализованный ствол, `source` —
  реально прочитанный путь;
* `pages[i].index == i`, `pages[i].lines == pages[i].text.size()`, у каждой
  страницы есть хотя бы одна строка, у каждой строки текст непуст;
* повторная загрузка даёт те же страницы/строки/глифы/текст;
* `load_briefing(game, "LEVEL1")`, `"Level15.SmE"` и `"  level1  "` работают.

Отказы (текст):

| Ситуация | Текст |
|---|---|
| `game` пуст или не каталог | `briefing: game directory not found: <path>` |
| нет `SML/brief` | `briefing: no SML/brief directory under <game>` |
| пустое имя | `briefing: empty level name` |
| имя — путь | `briefing: level must be a file name, not a path: "<level>"` |
| расширение не `.sme` | `briefing: not a .sme briefing: "<level>"` |
| файла нет (в т.ч. `.sml`-сосед не подставляется) | `briefing: file not found: <stem>.SME under <dir>` |
| файл не читается / ошибка чтения | `briefing: cannot read <path>` / `briefing: read error on <path>` |
| файл пустой (0 байт) | `briefing: empty file: <path>` |
| байты не разбираются | `briefing: <path>: sml: ...` (например `sml: no tags`) |
| нет атласов/пагер отказал | `briefing: <path>: sml_pager: ...` |
| документ без текста | `briefing: no text: <path>` |
| ошибка обхода каталога | `briefing: cannot scan <dir>: <code>` |

## 4. Числа теста

CTest `briefing` (`engine/tests/test_briefing.cpp`, label `quick`), реальный
`SML/brief/`. Печатает сам тест:

```
$ ./build/sandbox/engine/tests/gm_briefing_tests
briefing levels: 25 (level0a .. level9)
  level0a: 5 pages, 37 lines, 601 glyphs, body 0x1babe20f4260875e
  level1: 2 pages, 20 lines, 910 glyphs, body 0x45239da6047fcad3
  ...
  level9: 2 pages, 28 lines, 1262 glyphs, body 0xa4673e8cb261368b
briefing corpus: 25 levels, 58 pages, 727 lines, 33284 glyphs;
  most pages: level0a (5)
```

Корпус целиком (25/25; `body` — FNV-1a64 оракула `gm/sml.py`, пробелы
убраны):

| уровень | страниц | строк | глифов | `body` |
|---|---:|---:|---:|---|
| level0a | 5 | 37 | 601 | `1babe20f4260875e` |
| level1 | 2 | 20 | 910 | `45239da6047fcad3` |
| level2 | 2 | 17 | 781 | `17034ab8a39a5ded` |
| level3 | 2 | 23 | 1042 | `b898b085c2aff17c` |
| level4 | 3 | 37 | 1643 | `86ba1fa7739bf8eb` |
| level5 | 2 | 28 | 1307 | `ae3da1fc7527429e` |
| level6 | 2 | 29 | 1379 | `61f8a40ad0156629` |
| level7 | 2 | 21 | 999 | `0941e12abe68d283` |
| level8 | 2 | 31 | 1562 | `98823206d5029854` |
| level9 | 2 | 28 | 1262 | `a4673e8cb261368b` |
| level10 | 3 | 35 | 1665 | `a902c546459b6a85` |
| level11 | 2 | 30 | 1443 | `baab87e3e31cf305` |
| level12 | 3 | 47 | 2224 | `b9754706891c4d3f` |
| level13 | 1 | 12 | 474 | `4fdbada1eb159347` |
| level14 | 2 | 21 | 970 | `4c4c31b0cdb8e3c4` |
| level15 | 2 | 29 | 1423 | `d46a942a3f039717` |
| level16 | 3 | 36 | 1752 | `3fb9f68a173210fb` |
| level17 | 2 | 31 | 1471 | `81ef726a7fe65012` |
| level18 | 2 | 21 | 989 | `9fcd393a5dfab809` |
| level19 | 1 | 9 | 343 | `00e9765d187124d3` |
| level20 | 2 | 31 | 1509 | `3e3581fb35943c5b` |
| level21 | 2 | 30 | 1487 | `1a1fac56ae9b2356` |
| level22 | 5 | 74 | 3699 | `33c00e1abf0a8920` |
| level23 | 2 | 26 | 1180 | `402b2972229e95a2` |
| level24 | 2 | 24 | 1169 | `695c4f8d343b22e4` |
| **итого** | **58** | **727** | **33284** | 25/25 совпало |

Максимум страниц — 5; делят `level0a` и `level22`. Тест печатает и
закрепляет `level0a` (первый в порядке таблицы, сравнение строгое `>`).

Три брифинга разобраны постранично (числа закреплены; печатаются первые две
строки каждой страницы):

```
level0a: 5 pages
  page 1/5: 10 lines, 180 glyphs   "CONGRATULATIONS!" / "You have been named"
  page 2/5:  6 lines,  87 glyphs   "Maximum Ammo" / "MADMECHASTREISAND"
  page 3/5:  6 lines,  91 glyphs   "Turbo Boost On" / "MADBUCKETOWEASELS"
  page 4/5:  4 lines,  48 glyphs   "Show Map" / "MADCONNECTTHEDOTS"
  page 5/5: 11 lines, 195 glyphs   "Other cheat codes" / "include Flight Pack,"
level1: 2 pages
  page 1/2: 15 lines, 719 glyphs   "dmills=niacdispatch" /
                                   "Morning, Soldier. Lieutenant Mills here, Dispatch. Welcome"
  page 2/2:  5 lines, 191 glyphs   "equipment outside of the field. But get back as soon as" /
                                   "you can. Nataka doesn't pay you to slack off."
level15: 2 pages
  page 1/2: 16 lines, 768 glyphs   "dmills=niacdispatch" /
                                   "Howdy, Soldier! Lieutenant Mills here. Nataka brass"
  page 2/2: 13 lines, 655 glyphs   "Divine Logic transmitter. Bad news is a file fragment got" /
                                   "out of the R & D quarantine and into guess where?"
```

Первая строка `dmills=niacdispatch` — не опечатка теста: так выглядит первая
строка `.SME` (адрес отправителя/канал), она остаётся в тексте брифинга.
`level0a` — не «миссия 0», а экран чит-кодов/поздравления, который в поставке
лежит в `brief/`.

Что проверяет тест:

* `briefing_levels`: ровно 25, первое `level0a`, последнее `level9`, строгая
  сортировка и уникальность, каждое имя есть в закреплённой таблице;
* по каждому уровню: числа страниц/строк/глифов, `index == i`,
  `lines == text.size()`, непустые строки, хэш текста против Python, ASCII;
* повторная загрузка: те же страницы, строки, текст и `source`;
* детали трёх уровней: постраничные числа, первые две строки, и
  `pager_render` на каждой странице (`stats.lines`, `stats.glyphs`,
  `missing_glyphs == 0`) — то есть `glyphs` действительно символы с глифами;
* негативы (реальная установка): `level0` (нет файла), `help` (не брифинг),
  `level1.sml`, полный путь, пустой `game`, `game` — файл;
* негативы (синтетика, без установки): нет `game`/`SML/brief`, пустое/пробельное
  имя, `.sml`, путь, `..`, отсутствующий уровень, пустой файл, битые байты
  (`sml: no tags`), каталог с именем `level97.sme`, отсутствие атласов
  (`sml_pager: ...`). Каждый отказ — `false`, текст `briefing: ...`, очищенный
  `out`, без крэша.

Прогон:

```
$ cmake --build build/sandbox --target gm_briefing_tests -j 4
$ ctest --test-dir build/sandbox -R '^briefing$' --output-on-failure
1/1 Test #90: briefing .........................   Passed    2.92 sec
100% tests passed out of 1
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 65.7, "full": false,
 "categories": [], "game": "/home/nooverload/.cache/gunmetal-re/game",
 "failed": [], "cached": 0}
```

## 5. Ограничения (что срез 1 сознательно не делает)

* **Не подключено к меню, рантайму и экрану конца уровня.** Модуль только
  отдаёт данные; состояния фронтенда (`frontend_menu`), переходы и клавиши —
  не его зона.
* **CLI — есть (срез 2):** `--briefing LEVEL [--briefing-text]` печатает
  `pages/lines/glyphs` каждой страницы и, с `--briefing-text`, сам текст
  (`level1`: 2 страницы / 20 строк / 910 глифов; CTest `cli_briefing`,
  `cli_briefing_text`, `cli_briefing_missing`). PNG-рендер — срез 3 (`ui/briefing_render.*`, `--briefing LEVEL --png OUT`).
* **Par/лучшее время не читаются.** `par` — это состояние VM
  (`SET_LEVEL_BEST_TIME`, опкод 93, `0x80d88c`), рекорд игрока — файл
  `GUNMETAL.HI` (`0x45c880`, [CAMPAIGN_FLOW.md](CAMPAIGN_FLOW.md) §6,
  [GAME_OVER.md](GAME_OVER.md) §5). Ни того, ни другого в `.SME` нет, в
  `Briefing` они не входят и не рисуются.
* **Шрифты и вёрстка — пагерские.** 267×167, высоты строк 10/18, жадный
  перенос, шрифты 1/2/3 из `FRONTEND/BMP.IDX` — всё из `text_layout`; модуль
  не пересчитывает ни одного числа. Экранный origin страницы по-прежнему не
  доказан ([PAGER_RENDER.md](PAGER_RENDER.md) §2.1).
* **Только `.sme`.** `.sml`-редакции (24 файла, у части другой текст) не
  читаются — как и загрузчик, который предпочитает `.sme`. Для `.sml` нужен
  явный обход с `SmlSource::Plain`.
* **Наличие, не валидность.** `briefing_levels` смотрит на файлы, а не
  декодирует их; битый `.sme` попадёт в список, но `load_briefing` откажет
  текстом.
* **Глифы не проверяются по атласу при загрузке.** Это число разложенных
  символов; на поставке оно совпадает с рендером (проверено на трёх
  брифингах), но для установки без `BMP.IDX` `load_briefing` откажет на
  пагере, потому что без метрик нет и строк.
* **Нет `<I>`-битмапов и рендера.** `BriefingPage` не содержит
  `LayoutImage`; картинки брифинга (`SML/<f>.bmp`) не декодируются. Рендер
  страницы — `ui/pager_render` (отдельный слой), здесь его нет.
* **Нет кэша.** Каждый `load_briefing` заново читает файл, декодирует и
  раскладывает; пагер кэширует только атласы в пределах своего объекта,
  который создаётся на вызов.
* **Нет максимума страниц сверх `text_layout`.** Предохранитель — 256 страниц
  (`LayoutOptions::max_pages`); в поставке максимум 5.
* **Один и тот же ствол в разном регистре** (теоретически) разрешается
  сортировкой; в поставке дублей нет.

## 6. Проверка (выполненные команды)

Сборка только своей цели (портативный тулчейн, [BUILD.md](BUILD.md)):

```
$ export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
$ cmake --build build/sandbox --target gm_briefing_tests -j 4
[1/3] Linking CXX static library engine/libgm_engine.a
[2/3] Building CXX object engine/tests/CMakeFiles/gm_briefing_tests.dir/test_briefing.cpp.o
[3/3] Linking CXX executable engine/tests/gm_briefing_tests
```

Тест и Python-оракул — вывод выше (§4). Полный `ctest` не запускался
(по условию среза), `regress.py --mirror --json` — 115/115.
