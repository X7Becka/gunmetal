# Безголовый список слотов сохранения (`M8.7`, срез 1)

`engine/src/ui/save_slots.*` — **модель списка сейвов** экранов 13/14/15
(START GAME / SAVE GAME / NAME ENTRY) без окна, рендера и записи. Числа и
строки взяты из `docs/UI_SCREENS.md` §5.1 (источник истины), поля заголовка —
из `docs/SAVE.md` §1.1 и `docs/SAVE_FIELDS.md` §2.

Что это **не** делает: не рисует строки и подсветку (`0x485d00`/`0x485cc0`),
не грузит атласы `FRONTEND/BMP.IDX`, не пишет и не читает игру как сейв
(писатель `0x4422d0`, загрузчик `0x442340`), не трогает парный мир
`saved<N>.sbw` и буфер ввода имени `0x661280`. См. «Ограничения».

* **Ветка:** `feat/slots`, база `master` `c2c4463`, дата 2026-10-24.
* **Файлы:** `engine/src/ui/save_slots.h`, `engine/src/ui/save_slots.cpp`,
  `engine/tests/test_save_slots.cpp`, `docs/SAVE_SLOTS.md`; регистрация —
  `engine/CMakeLists.txt` (`src/ui/save_slots.cpp`) и
  `engine/tests/CMakeLists.txt` (CTest `save_slots`, label `quick`).
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`).

## 1. Доказательства (из `UI_SCREENS.md` §5.1)

| факт | адрес / вывод | статус |
|---|---|---|
| загрузчик таблицы имён | `0x48f970`, цикл `slot < 20` | proven (§5.1) |
| таблица: 20 записей, шаг `0x13` | `0x6c2fe0`, конец `0x6c315c`; `(0x6c315c-0x6c2fe0)/0x13 = 20` | proven |
| имя слота | `read_name` `0x455d90` открывает `savegame\saved%d.sbt` (`0x4ab460`) и копирует имя из заголовка (`name[0x50]`) | proven |
| парный мир | `savegame\saved%d.sbw` (`0x4ab478`) — в модели не читается | proven |
| пустой слот | `strcpy(..., "::: empty slot :::")` `0x4bc3b0` | proven |
| 6 видимых строк | экран 13: пункты 2..7, строки `-44` шаг `-11` (§3.6); экран 14: `rows 6` / шаг 12 (§2) | proven |
| прокрутка | `0x68ccdc`, условия `scroll > 0` и `scroll + 6 < 20` в `0x48aa60`/`0x48abd0` | proven |
| перерисовка | `0x485d00`/`0x485cc0` — не портирована | — |
| подтверждение перезаписи | строк overwrite/confirm в бинаре нет; из 14 сразу идут в 15 (§5.2) | вывод из отсутствия |

`level`/`mission` — из заголовка сейва: `level` — числовой хвост
`level[0x50]` (`"LEVEL0"` → 0, иначе −1), `mission` — поле F2 (+0xac),
«номер следующей миссии/уровня» (proven, `SAVE_FIELDS.md` §2). `detail` —
`"<level_name>, mission <N>"`.

Имя берётся из `gm::parse_save()` (`SaveHeader::title`), то есть первые
**12** байт `name[0x50]` — отображаемый заголовок по `SAVE.md` §1.1. В
`saved0.sbt` за ними лежит `'4'` в `+0x0c` (остаток буфера, не версия);
поэтому `name` = `"A SAVED GAME"`, а не `"A SAVED GAME4"`.

## 2. API

```cpp
#include "ui/save_slots.h"

gm::SaveSlotList list;
std::string error;
list.refresh("/mnt/d/games/Gunmetal 3dfx", &error); // savegame/saved0..19.sbt
```

* `SaveSlot { int index; bool present; std::string name; int level; int mission;
  std::string detail; }` — `index` = `<N>` файла `saved<N>.sbt`.
* `SaveSlotList::kSlots = 20`, `kVisible = 6`, `kMaxScroll = 13`,
  `kEmptyLabel = "::: empty slot :::"`.
* `refresh(game, error)` — читает `saved<N>.sbt` через `gm::parse_save()`;
  нет каталога `SAVEGAME`/`savegame` — пустой список **без ошибки**; нет
  файла — пустой слот; битый файл — слот пустой, в `*error` первая проблема,
  остальные слоты читаются. `error` может быть `nullptr`.
* `count()` — всегда 20 (таблица глобальная, а не список существующих файлов);
  `present_count()` — сколько слотов занято; `at(i)` — запись, индекс
  зажимается в `0..19`.
* `scroll()`/`scroll_down()`/`scroll_up()` — окно `0..13`, на границах
  `false` без изменения состояния.
* `selected()`/`select_down()`/`select_up()`/`select(slot)` — выделение
  (абсолютный номер слота) в `[scroll, scroll + 5]`; при выходе за окно окно
  едет за выделением.
* `visible_rows()` = 6, `item_label(row)` — подпись строки `0..5`: имя или
  `::: empty slot :::`; вне `0..5` — пустая строка.
* `needs_confirm(slot)`, `request_overwrite(slot)`, `overwrite_pending()`,
  `pending_slot()`, `confirm()`, `cancel()` — отдельное состояние
  подтверждения; `cancel()` возвращает выделение, которое было до запроса.

Инварианты (проверены тестом): `scroll ∈ [0,13]`,
`selected ∈ [scroll, scroll+5]`; до `refresh()` все `present == false`, имена
пустые; на границах операции возвращают `false`, ничего не меняя; пока
подтверждение активно, навигация запрещена (модальное состояние); все методы
детерминированы и не бросают исключений.

## 3. Какие файлы и заголовки читаются

`refresh` перебирает `saved0.sbt … saved19.sbt` в `game/SAVEGAME` (верхний
регистр, как в поставочной установке) или `game/savegame` (как в строке
`0x4ab460`). Файл больше 64 МиБ (тот же предел, что `kMaxSaveBytes` в
`res/save.cpp`) считается нечитаемым и даёт ошибку.

## 4. Границы и недостижимый слот 19

Оригинальное условие `scroll + 6 < 20` даёт максимум `scroll = 13`, а окно —
`[scroll, scroll + 5]`. Значит на последнем шаге видны слоты **13..18**, а
слот **19 не виден никогда**: `scroll = 14` условие уже запрещает. Модель
воспроизводит это как есть (`kMaxScroll = 13`), а не «чинит»: `select(19)`
возвращает `false`, `select_down()` на `selected == 18` — `false`. Это
следствие формул §5.1; намерение оригинала (off-by-one или сознательное
«последняя строка — подсказка») не доказано.

## 5. Подтверждение перезаписи — модель, не оригинал

В `gundx.exe` отдельного диалога перезаписи нет: экран 14 по Enter сразу
переходит в ввод имени (15), а единственная строка `%s has been used twice`
(`0x4bd790`) относится к наборам клавиш (`UI_SCREENS.md` §5.2). Поэтому
`needs_confirm`/`request_overwrite`/`confirm`/`cancel` — **наша модель** для
будущего UI/CLI, а не перенос поведения. Она детерминирована и изолирована:
`confirm()` ничего не пишет на диск.

## 6. Проверка и числа

```sh
$ ctest --test-dir build/sandbox -j 4
100% tests passed out of 146
```

```sh
$ ./build/sandbox/engine/tests/gm_save_slots_tests
save_slots: empty dir -> slots=20 present=0 labels=6 scroll=0..13
save_slots: 1 save -> present=1 name="SLOT 0" level=0 mission=1
save_slots: 6 saves -> present=6 window=0..5 labels=6
save_slots: 20 saves -> present=20 scroll=0..13 window=13..18 (slot 19 unreachable)
save_slots: selection 0..5 -> scroll 0; row 6 -> scroll 1; back up -> scroll 0
save_slots: overwrite needs_confirm=1, cancel restores selection, confirm keeps it
save_slots: corrupt slot reported (error set), slot 1 still loaded, two runs equal
save_slots: shipped install -> present=2 saved0="A SAVED GAME" saved1="2"
save_slots: 103 checks, 20 slots, 6 visible, scroll 0..13
```

Что покрыто (103 проверки):

* пустой каталог, отсутствующий каталог и `refresh(nullptr)` — без ошибок и
  крэшей; все 6 подписей `::: empty slot :::`;
* 1 / 6 / 20 синтетических сейвов (собраны `gm::Save::to_bytes` во временный
  `SAVEGAME`): имя, `level`, `mission`, `detail`;
* границы прокрутки: 13 шагов вниз, 14-й `false`; окно `13..18`; слот 19
  недостижим; 13 шагов обратно в 0;
* переход выделения через границу окна (`5 → 6` со сдвигом окна в 1) и
  обратно;
* перезапись: занятый слот → `needs_confirm`, пустой → отказ, `cancel`
  возвращает выделение (в том числе после второго запроса), `confirm`
  оставляет выделение на слоте, навигация заблокирована при pending;
* битый и обрезанный файл: ошибка в `*error`, слот пустой, соседний слот
  читается; два прогона по одному каталогу совпадают;
* нижний регистр `savegame`; реальные `saved0/saved1` при наличии установки
  (`present=2`, `saved0="A SAVED GAME"`, `saved1="2"`, `level 0`, `mission 1`).

```sh
$ python3 regress.py --mirror --json
{"passed": 115, "total": 115, "seconds": 51.0, "full": false, "categories": [], "game": "...", "failed": [], "cached": 0}
```

## 7. Ограничения

* Нет рендера: ни строк, ни шрифтов, ни подсветки, ни стрелок прокрутки
  (`0x485d00`/`0x485cc0`, атласы `FRONTEND/BMP.IDX`).
* Нет сохранения/загрузки: `confirm()` — только снятие состояния; писатель
  `0x4422d0` и загрузчик `0x442340` не вызываются.
* Нет `saved<N>.sbw`: парный мир не проверяется, поэтому «слот занят» значит
  «есть разбираемый `.sbt`», а не «пара полная».
* Нет UI-звуков `updown`/`select`/`selectm` и ввода имени (экран 15).
* Строки экранов 13/14 в `FrontendMenu::items()` остаются статическим
  снимком (`(save slot name)`); список с ним пока не связан.
* Подтверждение перезаписи — модель (§5), а не доказанное поведение.
* Имя — 12-байтовый отображаемый заголовок парсера (см. §1), а не все
  `name[0x50]`.

## 8. Что осталось (следующий срез)

* CLI `gunmetal --save-slots [--game DIR]` (печать 6 видимых строк, `scroll`,
  `selected`, `:empty`), регистрация CTest с `PASS_REGULAR_EXPRESSION` — это
  общий `main.cpp`, его правит основной агент.
* Связка со списком пунктов `FrontendMenu` (динамические 6 строк вместо
  `(save slot name)`) — `frontend_menu.*` в этом срезе не менялся.
* Реальное сохранение/загрузка (`0x4422d0`/`0x442340`) и `saved<N>.sbw`.
* Проверка на живом оригинале: показывает ли игра слот 19 при прокрутке
  (см. §4) и как выглядит имя длиннее 12 байт.
