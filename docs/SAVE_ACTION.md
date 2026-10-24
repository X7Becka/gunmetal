# Действие «сохранить/загрузить»: `save_action` (`M8.7`/`M9.3`, срез 2)

`engine/src/game/save_action.*` — **связка события экрана с записью и
загрузкой сейва**. `ui/save_menu.*` решает, какое событие дал Enter
(`save`/`load`/`back`), но файл не трогает; `res/save_write.*` умеет
собрать контейнер; `game/save_load.*` умеет применить его к рантайму. Этот
модуль принимает событие и слот и делает ровно одно из трёх: пишет копию
слота в отдельный каталог, читает слот в одноразовый рантайм или
отказывает с текстом. В игру он не пишет никогда.

* **Статус:** срез 2 готов — `ctest -R save_action` **76 проверок**,
  зелёный. Поставочный `saved0.sbt` пишется побайтово (243368 Б, SHA-256
  `e39c0c35…b623ff`), читается в рантайм (`level0`, поза/броня/щиты/арсенал
  из `docs/SAVE_LOAD.md` §5), запись в установку отвергается, `saved0.sbt` до
  и после совпадает по SHA-256.
* **Ветка:** `feat/saveaction`, база `master` `b3d5189`, дата 2026-10-24.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx/SAVEGAME`); тест
  пишет в `/tmp/opencode/gm_save_action`, установка не меняется.
* **Новые файлы:** `engine/src/game/save_action.h`,
  `engine/src/game/save_action.cpp`, `engine/tests/test_save_action.cpp`,
  этот документ.
* **Правки только в:** `engine/CMakeLists.txt` (`src/game/save_action.cpp` в
  `gm_engine`), `engine/tests/CMakeLists.txt` (CTest `save_action`, label
  `quick`).
* **Опоры:** `docs/SAVE_MENU.md` (события/слоты), `docs/SAVE_WRITE_CPP.md`
  (писатель, round-trip), `docs/SAVE_LOAD.md` (§2 приёмники, §5 числа),
  `docs/SAVE.md` §1 (контейнер), `docs/SAVE_SLOTS.md` §1
  (`SAVEGAME/saved<N>.sbt`).

## 1. Доказано / модель

| факт | откуда | статус |
|---|---|---|
| источник записи — `SAVEGAME/saved<N>.sbt` | `ui/save_slots.*`, `docs/SAVE_SLOTS.md` §1; `resolve_ci` как в `save_load.cpp` | proven |
| `write_save()` воспроизводит поставочный сейв байт в байт | `docs/SAVE_WRITE_CPP.md` §5; тест `save_action`: 243368 Б, `differing_bytes=0`, SHA-256 равен | proven (повторно, на копии) |
| opaque-заголовок `0x10..0x50` переносится из оригинала | `write_save(..., original)`, `docs/SAVE_WRITE_CPP.md` §3 | proven |
| `load` применяет уровень/позу/виталы/арсенал | `apply_save()` (`docs/SAVE_LOAD.md` §2), тест сверяет числа §5 | proven на saved0 |
| пустой слот не грузится | `SaveMenu::activate` + отсутствие файла; отказ `refused_empty` | proven (поведение нашего слоя) |
| Esc (`back`) и `none` ничего не делают | требование события; счётчики не растут | модель |
| пустой слот при `save` **не** создаёт файл | нет доказанного шаблона «пустого сейва»; писатель требует полный контейнер (`docs/SAVE_WRITE_CPP.md` §8.7) | модель (осознанный отказ) |
| отсутствующий `out_dir` создаётся | `create_directories`; оригинал тоже создаёт `savegame` при записи (`docs/SAVE_WRITE_CPP.md` §1) | модель |
| запись внутрь установки запрещена | `weakly_canonical` + покомпонентное сравнение путей; тест: 2 отказа, SHA-256 до/после равны | модель (защита), проверена тестом |
| каркас для `load` — LIGHT-значения (`eye_height 10`, `max_speed 1.7`) | в сейве нет шасси, `apply_save()` берёт капсулу из `SimPlayer` (`docs/SAVE_LOAD.md` §1) | модель (на применённые числа не влияет) |

Оригинальный экран 14 после Enter уходит в ввод имени (15) и **не**
спрашивает подтверждения перезаписи (`docs/SAVE_MENU.md` §1,
`docs/UI_SCREENS.md` §5.2). `save_action` это сохраняет: `save` на занятом
слоте — сразу запись копии, без диалога.

## 2. API

```cpp
#include "game/save_action.h"

enum class SaveActionResult {
    none, saved, loaded, refused_empty, refused_write, refused_read,
};

struct SaveActionStats { int saves, loads, refusals; };

class SaveAction {
public:
    SaveActionStats& stats();
    const SaveActionStats& stats() const;
    std::string error() const;             // текст последнего отказа
    const LoadedSave& last_load() const;   // что применил последний `loaded`

    SaveActionResult apply(const SaveMenu& menu, SaveMenuEvent event,
                           const std::filesystem::path& game,
                           const std::filesystem::path& out_dir,
                           std::string* error = nullptr);
};
```

`menu.selected()` — целевой слот (`<N>` файла `saved<N>.sbt`); `event` —
`SaveMenuEvent` из `activate()`/`escape()`. *error* (если не null) получает
тот же текст, что `error()`; при успехе очищается.

| событие / ситуация | результат | что происходит |
|---|---|---|
| `none`, `back` | `none` | ничего; счётчики и `error()` не меняются |
| `save`, слот занят | `saved` | `parse_save` → `write_save(..., original)` → `<out_dir>/saved<N>.sbt` |
| `save`, слота нет | `refused_empty` | файл **не** создаётся, шаблон не выдумывается |
| `load`, слот занят | `loaded` | `parse_save` → `apply_save()` в одноразовые `GameRuntime` + `SimPlayer`; числа в `last_load()` |
| `load`, слота нет | `refused_empty` | текст с путём |
| файл есть, но не читается/не разбирается | `refused_read` | текст парсера; на `save` тоже (мусор не копируется) |
| `out_dir` пуст/не создаётся/`out_dir` внутри игры; `write_save` отказал | `refused_write` | ничего не записано (частичный `.part` удаляется) |
| меню не открыто | `refused_write` (`save`) / `refused_read` (`load`) | `selected()` не адресует слот |
| слот вне `0..19` | `refused_write`/`refused_read` | защита от чужих индексов |

`refusals` увеличивается на каждом `refused_*`; `saves`/`loads` — только на
успех. Запись идёт во временный `saved<N>.sbt.part` и переименовывается,
поэтому отказ не оставляет полуфайла.

## 3. Числа: поставочный `saved0.sbt`

```sh
$ ./build/sandbox/engine/tests/gm_save_action_tests
save_action: empty slot 5 save + slot 0 load -> refused_empty, no file
save_action: corrupt saved0 load -> refused_read "save slot 0 (.../saved0.sbt) cannot be parsed: unknown record type 65 at 0x13c"
save_action: save slot0 -> 243368 bytes sha256 e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff identical=1 (saves=1)
save_action: load slot0 -> level=level0 pos=(1116.173752, 798.207621, 48.000000) yaw=5.073615 armor=10000/10000 shields=8000/8000/8000/8000 ammo=2000,90 mask=0x3 loads=1
save_action: install guard refused 2 writes, saved0 sha256 e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff unchanged=1
save_action: 76 checks, ok (saves/loads/refusals counted per instance)
```

| проверка | число |
|---|---|
| размер записанной копии | 243368 Б (= исходник) |
| SHA-256 записанной копии = исходника | `e39c0c35…b623ff` |
| отличающихся байт | 0 |
| `stats().saves` после записи | 1 |
| `last_load().level` | `level0` |
| `last_load().feet` | `(1116.173751898651, 798.2076214059642, 48.0)` |
| `last_load().yaw` | `5.0736149946795805` |
| броня / максимум | `10000 / 10000` |
| щиты (4) / максимум | `8000×4 / 8000` |
| арсенал | слот 0, маска `0x3`, патроны `2000, 90` |
| блоков / `SECTOR_STUFF` | 15 / 256 (13 активных) |
| `stats().loads` после загрузки | 1 |
| запись в игру: отказов / сохранённый SHA-256 | 2 / без изменений |
| `ctest -R '^save_action$'` | `1/1 Passed 0.47 sec` (label `quick`) |
| `python3 regress.py --mirror --json` | `{"passed": 115, "total": 115, "seconds": 79.2, "failed": []}` |

Пути установки до и после (внешняя проверка): `saved0.sbt`
`e39c0c35…b623ff`, `saved1.sbt` `93f4edd3…34bd13` — те же, что в
`docs/SAVE_WRITE_CPP.md` §5.

## 4. Что покрыто тестом (76 проверок)

1. **События**: `none`/`back` → `none`, счётчики и `error()` не меняются;
   закрытое меню → отказ с текстом `not open`; `out_dir` при отказе не
   создаётся.
2. **Пустой слот**: `save` слота 5 в пустом `SAVEGAME` → `refused_empty`,
   `saved5.sbt` нет, каталог вывода не создан, текст содержит
   `saved5.sbt`/`does not exist`/`no empty template`; `load` пустого слота 0
   → `refused_empty`.
3. **Запись**: `save` слота 0 поставочного каталога в несуществующий
   вложенный `out_dir` → `saved`, каталог создан, файл 243368 Б,
   побайтово равен исходнику, SHA-256 совпадает с golden, `saves=1`.
4. **Чтение**: `load` слота 0 → `loaded`, `level0`, поза/yaw/броня/щиты/
   арсенал/15 блоков/13 активных `SECTOR_STUFF` совпадают с
   `docs/SAVE_LOAD.md` §5, `loads=1`.
5. **Негативы**: битый `saved0.sbt` (0x140 байт `'A'`) — `load` и `save`
   дают `refused_read` с путём и текстом парсера, файл не создаётся.
6. **Защита установки**: `out_dir = game` и `out_dir = game/SAVEGAME` —
   два `refused_write`, `saved0.sbt` SHA-256 до/после одинаков, `.part` не
   осталось.
7. **Детерминизм**: второй экземпляр в другой каталог пишет те же байты
   (равенство векторов и SHA-256).

## 5. Ограничения

1. **Нет реального снимка рантайма.** `save` не снимает текущую игру: он
   переписывает **существующий** `saved<N>.sbt` (источник — файл слота).
   Модели «мир/игрок → новый сейв» в этом срезе нет; для пустого слота
   отказ, а не шаблон. Полноценное сохранение требует писателя `.sbw`
   (`docs/SAVE_WRITE_CPP.md` §8.1) и снимка состояния (`M9.4`).
2. **Нет подтверждения перезаписи и миниатюр.** `save` на занятом слоте
   сразу пишет копию; диалог/превью — уровень UI, в оригинале диалога нет.
3. **CLI — есть (срез 3).** `--save-action save|load --save-menu-slot N
   [--save-action-out DIR]`: `load` слота 0 печатает
   `result=loaded saves=0 loads=1 refusals=0`, `level=level0 title="A SAVED
   GAME" feet=(1116.173752, 798.207621, 48.000000) yaw=5.073615`,
   `blocks=15 sector_stuff=13 unsupported=16`; `save` пишет в `DIR`
   (по умолчанию `/tmp/opencode/save-action`) 243 368 Б, sha256
   `e39c0c35…`; пустой слот 5 — `refused_empty` с текстом. CTest
   `cli_save_action_load`, `_load_stats`, `_save`, `_empty`. Вызова из
   `FrontendMenu` нет; `ui/save_menu.*` не менялся.
4. **`load` использует одноразовый рантайм без мира.** Через
   `apply_save()` подтверждаются уровень/поза/виталы/арсенал, но уровень не
   загружается (`load_level`), двери/переключатели/things не
   восстанавливаются — как и в `game/save_load.*`
   (`docs/SAVE_LOAD.md` §6). Капсула берётся из LIGHT-значений, потому что
   сейв не несёт шасси; на применённые поля это не влияет.
5. **Пишется только `.sbt`.** Парный `saved<N>.sbw` не создаётся и не
   копируется — то же ограничение `write_save` (`docs/SAVE_WRITE_CPP.md`
   §8.1).
6. **Защита установки — по путям.** Сравниваются канонизированные пути
   (`weakly_canonical` + компоненты); символическая ссылка внутрь установки
   разрешается канонизацией, но экзотические bind-mount'ы не проверяются.
7. **`out_dir` создаётся** (модель §1); поведение «отказать, если каталога
   нет» не выбрано. Тест это фиксирует.
8. **Оригинальный `gundx.exe` не запускался** с записанной копией: доказан
   побайтовый round-trip и наш разбор, не приём игрой.

## 6. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DGM_BUILD_TESTS=ON -DGM_WITH_SDL3=ON \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox --target gm_save_action_tests -j 4
./build/sandbox/engine/tests/gm_save_action_tests
ctest --test-dir build/sandbox -R '^save_action$' --output-on-failure
python3 regress.py --mirror --json
```

Тест `save_action` (label `quick`) не требует установки: без неё пустые и
битые каталоги проверяются синтетически, а поставочная половина печатает
`shipped saves not installed ... skipping`.

## 7. Что осталось

* CLI `--save-action save|load --slot N --out DIR` — `main.cpp`/`config.*`,
  правит основной агент.
* Снимок живого рантайма в `saved<N>.sbt` (писатель `.sbw` + состояние
  мира) — `M9.4`.
* Подключение к `FrontendMenu`/рендеру экранов 13/14 — общие файлы.
* Проверка на живой игре: принимает ли `gundx.exe` записанную копию и
  какой `.sbw` он ожидает рядом.
