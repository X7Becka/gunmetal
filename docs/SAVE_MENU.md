# Экран сохранений: модель `save_menu` (`M8.4`, срез 2)

`engine/src/ui/save_menu.*` — **модель экрана SAVE GAME (14) / LOAD GAME (13)**
поверх таблицы слотов, которую уже разбирает `ui/save_slots.*`. Модуль владеет
тем, чего у списка нет: режимом save/load, выделенным слотом, окном из 6 строк,
данными строки (имя, уровень, секунды) и событием Enter/Esc. По-прежнему нет
ни пикселей, ни шрифтов, ни писателя/загрузчика: `activate()` возвращает
событие, а не пишет файл.

* **Статус:** срез 2 готов — `ctest -R save_menu` **131 проверка**, зелёный;
  реальные `saved0`/`saved1` читаются (`"A SAVED GAME"` / `"2"`, `LEVEL0`,
  47 и 89 секунд).
* **Ветка:** `feat/menusave`, база `master` `b61c2f2`, дата 2026-10-24.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`); синтетические
  сейвы собираются писателем движка во временный каталог, установка не
  меняется.
* **Новые файлы:** `engine/src/ui/save_menu.h`, `engine/src/ui/save_menu.cpp`,
  `engine/tests/test_save_menu.cpp`, этот документ.
* **Правки только в:** `engine/CMakeLists.txt` (`src/ui/save_menu.cpp` в
  `gm_engine`), `engine/tests/CMakeLists.txt` (CTest `save_menu`, label `quick`).
* **Опоры:** `docs/UI_SCREENS.md` §2/§3.6/§5 (источник истины),
  `docs/SAVE_SLOTS.md` (список слотов), `docs/SAVE.md` §1.1 (заголовок),
  `docs/SAVE_FIELDS.md` §2 (F1/F20), `docs/FRONTEND.md` (экраны 13/14).

## 1. Доказано / модель

| факт | адрес / вывод | статус |
|---|---|---|
| 20 слотов, шаг `0x13` | `0x6c2fe0`, конец `0x6c315c`; `(0x6c315c-0x6c2fe0)/0x13 = 20` (UI_SCREENS §5.1) | proven |
| имя слота | `read_name` `0x455d90` → `savegame\saved%d.sbt` (`0x4ab460`), имя из заголовка `name[0x50]` | proven |
| пустой слот | `::: empty slot :::` (`0x4bc3b0`) | proven |
| 6 видимых строк | экран 13 — пункты 2..7 (§3.6), экран 14 — `rows 6`/шаг 12 (§2) | proven |
| окно `0..13` | `0x68ccdc`, условия `scroll > 0` и `scroll + 6 < 20` в `0x48aa60`/`0x48abd0` | proven |
| слот 19 недостижим | следствие `scroll + 6 < 20` при `scroll <= 13` (SAVE_SLOTS §4) | следствие формул |
| Esc выходит с экрана | `ESC TO CANCEL` на обоих экранах (§3.6) | proven |
| нет подтверждения перезаписи | из 14 сразу в 15, строк overwrite/confirm в бинаре нет (§5.2) | вывод из отсутствия |
| уровень слота | заголовок `level[0x50]` (+0x54) | proven (поле), **показ строкой — модель** |
| секунды слота | `F1` (+0xa8) — часы уровня; `F20` = `F1 / 91` секунд (`0x45c057`/`0x46053a`, SAVE_FIELDS §2) | делитель proven, **применение к каждой строке — модель** |
| пустой слот не выбирается в load | требование задачи; в оригинале 13 показывает и пустые строки | **модель** |
| пустой/битый слот перезаписывается в save | следствие «подтверждения нет»: Enter на слоте = запись | **модель** поведения |
| начальное выделение — первый занятый слот (load) | в оригинале подсветка стоит на первой строке независимо от пустоты | **модель** |

Оригинал на экранах 13/14 рисует **только имя** слота
(`0x6c2fe0 + slot*0x13`). Уровень и время добавлены в модель, потому что они
лежат в том же заголовке и нужны будущему UI/CLI; в доке это помечено явно.
`seconds = F1 / 91` округляется вниз (целочисленное деление, как в оригинале:
`0x45c057` делит на 91).

## 2. API

```cpp
#include "ui/save_menu.h"

enum class SaveMenuMode { save, load };
enum class SaveMenuEvent { none, load, save, back };

struct SaveSlotView {
    int index;          // <N> файла saved<N>.sbt
    bool occupied;      // файл есть и разбирается
    std::string name;   // заголовок name[0x50], первые 12 байт
    std::string level;  // заголовок level[0x50] ("LEVEL0")
    int seconds;        // F1 / 91 (модель, см. §1)
    bool selectable;    // достижим (не 19) и, в load, занят
};

gm::SaveMenu menu;
menu.open(gm::SaveMenuMode::load, "/mnt/d/games/Gunmetal 3dfx");
menu.move(+1);                       // пропускает невыбираемые слоты
menu.visible_first();                // 0..13, окно 6 строк
menu.slots();                        // всегда 20 записей
menu.at(0).seconds;                  // 47
menu.label(0);                       // "A SAVED GAME" или "::: empty slot :::"
std::string error;
if (menu.activate(&error)) {         // Enter
    // menu.event() == load | save
}
menu.escape();                       // Esc -> SaveMenuEvent::back
```

* `open(mode, game)` — читает `saved0..19.sbt` из `game/SAVEGAME` (или
  `game/savegame`), сбрасывает выделение/окно/событие. Не бросает.
* `selected()` ∈ `[0, 18]` (после открытия — на выбираемом слоте),
  `visible_first()` ∈ `[0, 13]`, `selected()` всегда внутри окна
  `[visible_first, visible_first + 5]`.
* `move(delta)` — `delta > 0` вниз, `< 0` вверх, `0` — ничего. Пропускает
  невыбираемые слоты (19 всегда, пустые/битые — в load); на краю диапазона
  **клампится**, а не заворачивается.
* `activate(&error)` — `true` ⇔ `event() != none`. `load` только для занятого
  слота; пустой — отказ с текстом `save slot N is empty`, битый —
  `save slot N cannot be loaded: <ошибка парсера>`. В `save` занятый слот даёт
  `save` (перезапись, подтверждения нет — §1), пустой тоже `save`, а
  отсутствие каталога — отказ `cannot save: no SAVEGAME directory under …`.
* `escape()` — `back`; состояние не меняется, экран закрывает вызывающий.
* `event()`, `clear_event()`, `error()`, `has_save_directory()`, `is_open()`,
  `mode()`, `visible_rows()` (всегда 6).

Инварианты проверены тестом: 20 записей до и после `open()`, выделение только
на выбираемом слоте, окно 6 строк и не выходит за `0..13`, повторный `open()`
сбрасывает состояние, два прогона по одному каталогу совпадают.

## 3. Ошибки (без крэшей)

| ситуация | что видно |
|---|---|
| нет каталога `SAVEGAME`/`savegame` | 20 пустых записей, ни один слот не выбирается, `error()` пуст (листинг не ошибка); `activate()` в load — `save slot 0 is empty`, в save — `cannot save: no SAVEGAME directory under <game>` |
| файл не читается / не разбирается | слот пустой (`occupied == false`), первый текст в `error()`, в save — всё ещё выбираем (перезапись), в load — отказ `cannot be loaded: <текст>` |
| файл > 64 МиБ | как «не читается» (тот же предел `kMaxSaveBytes`, что в `res/save.cpp`) |
| `activate()` до `open()` | `false`, `event() == none`, текст `save menu is not open` |
| слот 19 | `selectable == false` в обоих режимах; `move()` на него не попадает |

## 4. Числа: реальные сейвы

| слот | файл | имя | уровень | F1 (+0xa8) | секунды (F1/91) |
|---:|---|---|---|---:|---:|
| 0 | `saved0.sbt` (243368 Б) | `A SAVED GAME` | `LEVEL0` | 4288 | 47 |
| 1 | `saved1.sbt` (243368 Б) | `2` | `LEVEL0` | 8148 | 89 |
| 2..19 | — | пусто | — | — | 0 |

`--save-slots` даёт те же имена и уровни (`present=2/20`, `saved0="A SAVED
GAME"`, `saved1="2"`, `level=0`):

```
$ ./build/sandbox/engine/gunmetal --save-slots --game "/mnt/d/games/Gunmetal 3dfx"
save-slots: present=2/20 scroll=0..5 selected=0
   0 * A SAVED GAME        level=0 mission=1
   1   2                   level=0 mission=1
   2   ::: empty slot :::  level=-1 mission=0
```

(Секунды CLI не печатает — это новое поле модели; F1 сверен с заголовками
файлов: 4288 и 8148, то есть 47 и 89 целых секунд.)

## 5. Тест: числа

```sh
$ ctest --test-dir build/sandbox -R '^save_menu$' --output-on-failure
1/1 Test #124: save_menu .......................   Passed    0.13 sec
100% tests passed out of 1

$ ./build/sandbox/engine/tests/gm_save_menu_tests
save_menu: window 20 slots, visible=6, first(0,5,6,19)=(0,0,1,13), slot19 selectable=0
save_menu: load occupied -> load (0,2,5, gaps skipped); empty -> none "save slot 0 is empty"
save_menu: save empty/occupied/corrupt -> save (no overwrite prompt), missing dir refused
save_menu: header -> name="LONG RUN" level=LEVEL3 F1=11238 seconds=123
save_menu: corrupt -> error text + none "save slot 0 cannot be loaded: ..."; esc -> back
save_menu: deterministic (same dir + same moves -> same views/cursor)
save_menu: shipped saved0 "A SAVED GAME" level=LEVEL0 seconds=47; saved1 "2" level=LEVEL0 seconds=89
save_menu: 131 checks, ok (20 slots, 6 visible, slot 19 unreachable)
```

`first(0,5,6,19)`: при выделении 0 и 5 окно начинается с 0, при 6 — с 1,
а «выбор 19» невозможен: длинный `move` останавливается на 18, окно — 13
(строки 13..18), `move(+1)` дальше не двигает. Слот 19 существует
(`occupied == true` на поставочном каталоге), но `selectable == false`.

Что покрыто (131 проверка):

1. **До `open()`**: 20 пустых записей, `move()`/`activate()` — no-op/отказ с
   текстом, `::: empty slot :::` в строке, пустые подписи вне `0..5`.
2. **Окно**: 20 занятых слотов, `visible_first` при 0/5/6/19 (0/0/1/13),
   подписи строк 13..18, клампы `move(±1)` и длинных `move(±100)` на 0 и 18.
3. **load**: занятые 0/2/5 — переходы 0→2→5 с пропуском пустых, кламп на 5,
   `activate` → `load`; пустой каталог — `none` + `"save slot 0 is empty"`.
4. **save**: пустой слот → `save`, занятый → `save` (перезапись, без
   подтверждения), битый → тоже `save`; нет каталога → `none` +
   `cannot save: no SAVEGAME directory under …`; `move` тоже клампится на 18.
5. **Заголовок**: имя/уровень/`seconds = F1 / 91` (11238 тиков → 123 с),
   F1 = 0 → 0 с, у пустого слота уровня нет.
6. **Негативы**: битый `saved0` при живом `saved1` (слот 0 пропущен, 1
   грузится), только битый файл (текст с именем файла), обрезанный
   (header-only) файл, Esc → `back`, `clear_event()`.
7. **Детерминизм**: два экземпляра + повторный `open()` дают одинаковые
   записи/выделение/окно.
8. **Поставочные сейвы**: имена, `LEVEL0`, 47/89 с, строка 2 — плейсхолдер,
   `activate` → `load` для saved0 и `save` для перезаписи.
9. **`python3 regress.py --mirror --json`**: `{"passed": 115, "total": 115,
   "seconds": 66.5, "failed": []}` (ветка `feat/menusave`).

## 6. Ограничения

1. **Не подключено к фронтенду**: `FrontendMenu::items()` для экранов 13/14
   остаётся статическим снимком (`(save slot name)`) — `ui/frontend_menu.*` в
   этом срезе не менялся. Связка — следующий срез.
2. **Нет рендера**: ни строк/подсветки (`0x485d00`/`0x485cc0`), ни стрелок
   прокрутки, ни атласов `FRONTEND/BMP.IDX`, ни звуков `updown`/`select`.
   `label()` отдаёт только текст.
3. **Нет миниатюр и подтверждения перезаписи**: `save` на занятом слоте — это
   событие, а не диалог; вызывающий сам решает, спрашивать ли (в оригинале
   такого диалога нет, §1).
4. **Реальной записи нет**: `activate()` не вызывает писатель `0x4422d0` и не
   создаёт каталог; `saved<N>.sbw` не читается и не пишется. Модуль только
   читает `.sbt` и возвращает событие.
5. **Окно 6 строк фиксировано**: 20 слотов и `first <= 13` всегда дают 6
   строк; отдельного «сколько видно» у оригинала нет.
6. **Имя — 12-байтовый отображаемый заголовок** парсера (`docs/SAVE.md` §1.1),
   а не все `name[0x50]`; хвост `'4'` в `saved0` (байт `+0x0c`) не выводится.
7. **Секунды — модель** (§1): оригинальный экран их не показывает; значение
   зависит от того, что `F1` — живой счётчик тиков, и обнуляется на старте
   уровня.
8. **Пустой слот в load невыбираем** — требование задачи, а не наблюдение:
   намерение оригинала (реагирует ли Enter на `::: empty slot :::`) не
   проверялось (нужен запуск игры).

## 7. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DGM_BUILD_TESTS=ON -DGM_WITH_SDL3=ON \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox --target gm_save_menu_tests -j 4
./build/sandbox/engine/tests/gm_save_menu_tests
ctest --test-dir build/sandbox -R '^save_menu$' --output-on-failure
python3 regress.py --mirror --json
```

Тест `save_menu` (label `quick`) не требует установки: без неё гоняется только
синтетическая половина и печатается `shipped saves not installed, skipping the
real check`.

## 8. Что осталось

* Подключить модель к `FrontendMenu::items()` (динамические 6 строк экранов
  13/14) и к рендеру `ui/menu_render.*` — общие файлы, правит основной агент.
* CLI `--save-menu save|load [--slot N]`, печатающий окно и событие, —
  `main.cpp`.
* Реальная загрузка (`0x442340`, `game/save_load.*` уже есть) и запись
  (`0x4422d0`, `res/save_write.*`) по событию `activate()`.
* Поведение оригинала на пустом слоте и слот 19 при прокрутке — проверка на
  живой игре.
