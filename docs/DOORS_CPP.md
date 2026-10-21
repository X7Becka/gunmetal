# Gunmetal — C++-модель дверей, переключателей и лифтов

Что здесь: `engine/src/world/doors.{h,cpp}` — headless-состояние дверей,
переключателей и лифтов уровня, восстановленное из `.sbt` (SCRIPT/записи
`DOOR`/`SWITCH`/`LIFT`) и привязанное к объектам `.sbw`. Модуль **не трогает
рендер, игрока и коллизии**: он только считает `(sector, object)`,
`open_fraction`, таймеры и высоту лифта, а `apply_to_objects()` отдаёт
результат обычными данными.

Статус: API, парсинг, состояния, тесты и golden'ы готовы; CTest `doors`
(быстрый, `dm1`) и `doors_full` (все 36 уровней) проходят. Игра только
читается; `regress.py` не менялся.

---

## 0. Коротко

| что | значение |
|---|---|
| единица времени | фиксированный тик **91 Гц** |
| фаза двери | `0..65536`, `open_fraction = phase / 65536` |
| скорость | `step` фазы за тик (поле записи `+0x94`, скриптовый `L3`) |
| состояние | `-1` закрывается, `0` покой (в любом конце), `+1` открывается |
| привязка | `(sector, object)` → объект `.sbw`, проверено 1779/1779 |
| точность | целые для фазы/высоты, `double` только для `open_fraction` |
| детерминизм | порядок записей, без rand/времени; повторный прогон бит-в-бит |
| NaN/OOB | не-конечные `double` отвергаются на сборке; все индексы проверяются |

---

## 1. Источники данных

### 1.1. Поставочные уровни — SCRIPT

В 36 поставочных `.sbt` нет записей `DOOR` (2), `SWITCH` (1), `LIFT` (18):
двери и переключатели создаются командами `ADD_DOOR` (184) и `ADD_SWITCH`
(183), лифты — структурной командой `ADD_LIFT` (188) внутри `SCRIPT` (11).
Поэтому `WorldState::build` идёт двумя путями:

1. если запись `DOOR`/`SWITCH`/`LIFT` есть (сейвоподобный `.sbt`), поля
   читаются прямо из неё;
2. иначе `engine/src/res/scene.cpp::extract_scene` декодирует `ADD_DOOR` и
   `ADD_SWITCH` (без `ADD_THING*`, `with_things=false`), а `ADD_LIFT`
   локализуется уже декодированным `SbtProgram` и его 148-байтный блок
   читается из `script_code` (декодер `sbtvm` эти байты не сохраняет).

Парсинг `.sbt`/`.sbw` не дублируется: используются `parse_sbt`, `parse_sbw`,
`extract_scene` и `decode_sbt_script`.

### 1.2. Запись `DOOR` (184 байта)

Поля взяты из `docs/SAVE_FIELDS.md` (все — proven по коду):

| смещ. | поле | как используется |
|---|---|---|
| `0x00..0x30` | 9 × f64 | closed `D1..D3`, open `D4..D6`, size `D7..D9` |
| `0x68` | flags | `kind = flags & 0xf`; бит `0x40` — «используемая» дверь |
| `0x6c..0x7c` | 5 × i32 | `ids[0..4]`; первые три — ключи `GENERATE`, `ids[3]` — связь створок |
| `0x80` | state | `-1/0/+1` |
| `0x84/0x88` | sector/object | адрес створки в `.sbw` (`0x434419`) |
| `0x8c` | phase | `0..0xffff` (`65536` в скрипте = полностью открыта) |
| `0x94` | step | приращение фазы за тик |

`id` двери — **первый ненулевой из `ids[0..2]`**: `GENERATE` (`0x44f690`)
сравнивает именно эти три поля. В `dm1` это, например, `-30231`, а не
нулевой `E1`.

### 1.3. Запись `SWITCH` (32 байта)

`kind, id, radius, x, y, z, aux, sound` — восемь dword'ов **дословно**
(`0x44e5f0`). В модели:

* `Switch.id` — **индекс записи** (в формате нет отдельного id переключателя);
* `Switch.target` — `id` (операнд 2), то есть тот самый `GENERATE`-id;
* `kind`, `radius`, `pos` сохраняются как данные (поведение от них пока не
  зависит).

`interact(switch_id)` принимает именно индекс переключателя.

### 1.4. `LIFT` (18) и `ADD_LIFT` (188)

Запись типа 18: `u32 n` + `n × 0xA8` + `u32 k` + три колонки по `k`:
`floor/button id`, `height`, `sound` (`docs/RECORDS.md` §4). Из неё читаются
`kind +0x34`, `x/y/z +0x3c..0x44`, `speed +0x54`, `height +0x5c`,
`target +0x60`, `moving +0x68`, `stop_index +0x6c`, `call_ids +0x70..0x78`,
`stop_count +0x9c`, `stop_start +0xa0`. Высота — целые единицы, `speed` —
единицы за тик.

У `ADD_LIFT` 148-байтный блок **не разобран** в C++: он сохраняется в
`Lift::head[37]`, а `id/kind/x/y/z/height` берутся по гипотезе
`head[1], head[0], head[2..4], head[7]`. Остановки читаются точно
(`n × 12`, поля `floor_id/height/sound`). Из-за неразобранного `speed`
скриптовые лифты в модели **не двигаются**, пока скорость не задана вручную;
это осознанно, а не ошибка парсинга.

---

## 2. API

```cpp
#include "world/doors.h"

gm::WorldState state;
std::string error;
if (!state.build(sbt_level, sbw_level, &error)) { /* нет источников */ }

state.interact(51);        // нажать переключатель №51 (индекс)
gm::EventQueue queue;
state.post_switch_events(queue, /*source=*/3); // SwitchPressed -> type-2
state.tick();              // один тик 91 Гц
state.tick(44);            // сразу 44 тика (O(1))

const gm::WorldStats& st = state.stats();
auto objects = state.apply_to_objects();   // door_id, sector, object, fraction
```

`post_switch_events(queue, source)` постит по одному type-2 `Trigger Switch`
на каждый ещё не отзеркаленный `WorldEvent::SwitchPressed`; `source` — id
источника (игрок/сущность), `switch_index` — индекс переключателя. Внутренний
курсор (`switch_event_cursor_`) сдвигается по мере постинга; id `>= 0x200`
пропускается как заведомо непостижимый, а false от `post_switch` означает
только переполнение очереди. `tick()` сохраняет неотправленный хвост лога только после
неудачного `post` (очередь была полна); если post не вызывали, лог
очищается как frame-local. Полностью отправленный лог тоже очищается. Поэтому повторный
вызов без нового `interact()` ничего не постит и не дублирует события; при
переполнении очереди курсор остаётся на неотправленной записи.
`clear_events()` и `build()` сбрасывают курсор. Связка с игровым циклом —
`gm::post_switch_events(EventRuntime&, const WorldState&, source)` из
`game/events.h`: она кладёт type-2 в очередь рантайма, после чего
`runtime.pump(now)` даёт `GENERATED`-событие и (если у переключателя есть
звук) `SoundCommand`.

### 2.1. Типы

| тип | поля |
|---|---|
| `Door` | `id, sector, object, kind, state, timer, locked, open_fraction` + `flags, ids[5], closed/open/size[3], step, phase, hold_ticks, sliding, linked, bound` |
| `Switch` | `id, target, pressed, kind, radius, pos[3]` |
| `Lift` | `id, kind, x, y, z, speed, height, target, stop_index, moving, call_ids[3], stops[], head[37], script` |
| `LiftStop` | `floor_id, height, sound` |
| `ObjectDoorState` | `door_id, sector, object, state, open_fraction, sliding, linked, bound` |
| `WorldStats` | счётчики дверей/переключателей/лифтов, `doors_bound`, `doors_distinct_objects`, `bad_records`, `bad_bindings`, `locked_hits`, `unmatched_targets`, `lift_calls`, `ticks` |
| `WorldEvent` | `SwitchPressed, DoorOpening, DoorClosing, DoorOpened, DoorClosed, DoorLocked, LiftCalled, NoTarget` |

`DoorState` — `Closing = -1`, `Closed = 0`, `Opening = +1`; значения совпадают
с полем `+0x80` сейва. `door_state_name()` даёт `"closing"/"closed"/"opening"`.

### 2.2. `build`

* очищает состояние и счётчики;
* читает записи `DOOR`/`SWITCH`/`LIFT`, иначе `SCRIPT`;
* проверяет `sector`/`object` по `sbw.header.sectors` и ставит `bound`;
* считает `doors_distinct_objects` (пары `(sector, object)`;
* возвращает `false` только если **нет ни одного источника** (или SCRIPT не
  декодируется, а записей нет). Битые отдельные записи не валят мир:
  они попадают в `stats().bad_records`, невалидная привязка — в
  `bad_bindings`.

### 2.3. `interact(switch_id)`

* `switch_id` — индекс в `switches()`, не `target`;
* ставит `pressed = true` (сбрасывается в конце следующего `tick`);
* для **всех** дверей, у которых `target` совпал с одним из `ids[0..2]`,
  переключает направление:
  * `Opening → Closing`, `Closing → Opening`;
  * покой: `phase < 65536` → открывается, иначе закрывается;
* `locked` дверь пропускается (`DoorLocked`, `locked_hits++`);
* если `target` — call-id лифта, лифт едет к следующей остановке;
* если не нашлось ни двери, ни лифта — `NoTarget`, `unmatched_targets++`;
* возвращает `false` только для несуществующего индекса.

Повторное нажатие во время движения **разворачивает** дверь, а не игнорируется.

### 2.4. `tick(ticks)`

Один вызов — это `ticks` фиксированных шагов 91 Гц. Для двери:

```
phase += sign(state) * step * ticks        // с зажимом в 0..65536
```

Переходы считаются в закрытой форме: сначала `ceil((target − phase)/step)`,
остаток тиков уходит в следующий участок. Поэтому `tick(1e9)` не висит и
даёт конечное состояние. После полного открытия, если задан `hold_ticks`,
включается `timer`; по его истечении дверь начинает закрываться. `step = 0`
не двигает фазу (защита от деления на ноль).

Лифт: `height` движется к `target` по `speed` единиц за тик, с тем же
закрытым переходом; по прибытии `moving = false`.

### 2.5. `apply_to_objects`

Возвращает по элементу на каждую дверь:

```cpp
for (const auto& o : state.apply_to_objects()) {
    if (!o.bound) continue;              // (sector, object) нет в .sbw
    use(o.sector, o.object, o.open_fraction);
}
```

Это data-only слой: ни `SbwLevel`, ни `SbwGeometry` не мутируются (иначе
пришлось бы двигать вершины в непроверенном кадре `D`-операндов, см. §4).
`bound == false` означает, что дверь осталась в списке, но объекта для неё нет.

### 2.6. Runtime-управление

`set_locked(id, bool)`, `set_hold_ticks(id, ticks)`, `set_step(id, step)` —
для сценариев, которых нет в данных уровня (замок) или поле которых не
доказано (авто-закрытие). `find_door(id)`/`find_lift(id)` — поиск по
эффективному id.

---

## 3. Поведение и таймеры

| ситуация | результат |
|---|---|
| `interact` при `phase = 0`, покой | `Opening`, фаза растёт |
| `interact` при `phase = 65536`, покой | `Closing`, фаза падает |
| `interact` при `Opening` | `Closing` (разворот) |
| `interact` при `Closing` | `Opening` (разворот) |
| `phase` дошла до конца | `state = Closed` (покой), событие `DoorOpened`/`DoorClosed` |
| `hold_ticks > 0` после открытия | `timer = hold_ticks`, затем `Closing` |
| `hold_ticks = 0` | дверь стоит открытой до следующего `interact` |
| `locked` | дверь не меняет состояние, событие `DoorLocked` |
| один `target` у нескольких дверей | переключаются все (связанная группа) |
| `target` = call-id лифта | лифт едет к следующей остановке |
| `tick(0)` | ничего; `tick(1e9)` — сразу конечное состояние |

Таймер считает **тики**, а не секунды: `91 тик = 1 с`. Например,
`hold_ticks = 91` — секунда. В данных уровня поля авто-закрытия нет
(`L7 = 0` у всех 1779 дверей, `L6` читателя не имеет), поэтому по умолчанию
`hold_ticks = 0`; это не «забытая» константа, а сознательный opt-in.

### 3.1. Скользящие и связанные двери

* `sliding` — `D4..D6 ≠ D1..D3`; в поставочных данных 1534/1779.
* `linked` — `ids[3] != 0`; 200/1779 (в `dm1` — 4).
* `kind` — сырой младший ниббл `flags` (1..3). Семантика 1/2/3 (тип
  открывания) кодом не доказана, поэтому значение сохраняется, но не
  интерпретируется.
* Смещение створки в мировом кадре **не считается**: `D`-операнды не лежат
  в мировом кадре (docs/FORMATS.md §12.4), а `apply_to_objects` отдаёт только
  `open_fraction`.

---

## 4. Доказано / гипотеза

**Доказано (код + данные):**

* запись `DOOR`: flags/ids/state/sector/object/phase/step по смещениям
  `0x68..0x94` (`docs/SAVE_FIELDS.md`);
* `state = -1/0/+1`, `phase` — фаза `0..0xffff`, `step` — приращение за тик;
* `id` двери ищется по `ids[0..2]` (`0x44f690`), `id` переключателя —
  операнд 2;
* `(sector, object)` — адрес створки, `0x434419`; 1779/1779 валидны,
  1779 различных;
* `sliding`/`linked` по `D`-операндам и `E4`;
* переключатель переключает **все** двери с этим id (группа), лифт
  вызывает `GENERATE` по call-id;
* `ADD_LIFT` остановки: `n × 12`, поля `floor/height/sound`.

**Гипотеза / приближение:**

* `id = ids[0..2]` — доказано для `GENERATE`, но в данных `E1` часто `0`;
  берём первый ненулевой (совпадает с `tools/re/doors.py`);
* `kind` 1/2/3 не интерпретируется;
* `ADD_LIFT`: `head[1]` = id, `head[2..4]` = x/y/z, `head[7]` = начальная
  высота; `speed` в 148-байтном блоке не найден, поэтому скриптовые лифты
  статичны до `set_step`-аналога (в API — только синтетические записи 18);
* `timer/hold_ticks` — opt-in: в уровне поля авто-закрытия нет;
* `locked` в `.sbt` не хранится (это runtime/редакторское состояние);
* `apply_to_objects` не двигает геометрию: кадр `D`-операндов не мировой.

**Не проверено:** точная формула `0x44f326` (умножение `step` на `dt`) для
переменного `dt`; мы фиксируем `dt = 1 тик`, что совпадает с 91 Гц и
«приращение за тик» из сейва.

---

## 5. Golden'ы

### 5.1. Синтетика (`test_doors.cpp`)

`DOOR`-записи + `SWITCH`-записи + `LIFT` + `.sbw` из одного сектора с тремя
объектами:

| шаг | ожидание |
|---|---|
| `interact(0)` | дверь 100 → `Opening` |
| `tick(10)` | `phase = 15000`, `fraction = 15000/65536 = 0.2288818359375` |
| повторный `interact` + `tick(5)` | `phase = 7500`, `Closing` |
| третий `interact` + `tick(100)` | `phase = 65536`, `Closed`, `fraction = 1` |
| `hold_ticks = 12` | после открытия `timer = 12`, через 12 тиков — `Closing` |
| `locked` | дверь 300 не двигается, `locked_hits = 1` |
| switch на `999` | `NoTarget`, `unmatched_targets = 1` |
| лифт (speed 10) | `interact` → target 100; `tick(5)` height 50; ещё 5 → 100 |
| `tick(1e9)` | конечное состояние, без NaN/зависания |
| битые записи | `bad_records`, `bad_bindings`, никаких OOB |

### 5.2. `dm1` (поставка)

`python3 tools/re/doors.py --level dm1`:

```
doors/switches/lifts: 61 doors (46 sliding, 4 linked), 129 switches, 0 lifts
doors -> level objects: 61/61 valid (sector, object), 61 distinct
```

Модель даёт те же числа. Golden двери `-30231` (индекс 1, `step = 1500`,
переключатель №51):

| тики | `phase` | `open_fraction` |
|---|---|---|
| 10 | 15000 | `15000/65536` |
| 44 | 65536 | `1.0` |
| затем `interact` + 44 | 0 | `0.0` |

### 5.3. Все 36 уровней (`--full`)

```
1779 doors, 3964 switches, 45 lifts
1779/1779 bound, 1779 distinct (sector, object)
1534 sliding, 200 linked
```

Совпадает с `tools/re/doors.py` (36/36 уровней парсятся).

---

## 6. Сборка и тесты

```bash
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGM_WITH_SDL3=OFF
cmake --build build/sandbox --target gm_doors_tests -j 32
ctest --test-dir build/sandbox -R doors --output-on-failure
```

* `doors` — метка `quick`, только `dm1` (~0.06 с);
* `doors_full` — метка `full`, все 36 уровней (~2 с);
* при отсутствии игры реальная часть печатает `skipped` и не падает;
* `python3 regress.py` к модулю отношения не имеет, но запускался на
  неизменённых `gm/**`.

---

## 7. Доверие и остаток

**Можно доверять:** привязке `(sector, object)` (1779/1779), числам
`dm1`/корпуса, фазе/`step`/`state`, развороту по повторному switch,
групповому переключению, лифту по записи 18, отсутствию NaN/OOB на битых
входах.

**Нельзя считать окончательным:**

1. **Семантика `kind` 1/2/3** — только сохраняется.
2. **Авто-закрытие** — таймер есть в API, но поле уровня не найдено; в
   игре двери закрываются «по таймеру уровня» (docs/FORMATS.md §16),
   конкретный источник (L6? глобальный таймер?) не доказан.
3. **`ADD_LIFT`** — 148-байтный блок и `speed` не разобраны; скриптовые
   лифты статичны, запись 18 — полностью.
4. **Движение геометрии** — `apply_to_objects` отдаёт долю, а не вершины:
   кадр `D`-операндов не мировой, поэтому сдвиг створки в `SbwLevel` был бы
   необоснован.
5. **`locked`** — в уровне/сейве поля нет; модель принимает его извне.
6. **Связь с коллизиями** — `CollisionWorld` по-прежнему статичен; дверной
   бокс `corner .. corner + size` и `event_post` `0x4795b0` в физику не
   подключены.
7. **Сейв-совместимость** — модуль читает те же смещения, что save-блок 2,
   но writer/round-trip не реализованы.
