# Gunmetal — C++-модель дверей, переключателей и лифтов

Что здесь: `engine/src/world/doors.{h,cpp}` — headless-состояние дверей,
переключателей и лифтов уровня, восстановленное из `.sbt` (SCRIPT/записи
`DOOR`/`SWITCH`/`LIFT`) и привязанное к объектам `.sbw`. Модуль **не трогает
рендер, игрока и коллизии**: он считает `(sector, object)`, фазу, таймеры,
высоту лифта, **трансформ створки/кабины**, вектор движения и триггеры, а
`apply_to_objects()` отдаёт результат обычными данными. Writer записей
1/2/18 живёт рядом, в `engine/src/res/sbt.{h,cpp}`.

Статус: API, парсинг, состояния, геометрия, триггеры, звуковые поля, writer и
тесты готовы; CTest `doors` (быстрый, `dm1`) и `doors_full` (все 36 уровней)
проходят. Игра только читается; `regress.py` не менялся.

---

## 0. Коротко

| что | значение |
|---|---|
| единица времени | фиксированный тик **91 Гц** |
| фаза двери | счётчик `0..65536`, но движок клампит `+0x8c` в `0..0xffff` и делит на **65535**: `motion_fraction() = min(phase,0xffff)/65535` |
| `open_fraction` | `phase / 65536` — прежняя API-доля (golden'ы), для геометрии не используется |
| скорость | `step` фазы за тик (поле записи `+0x94`, скриптовый `L3`) |
| состояние | `-1` закрывается, `0` покой (в любом конце), `+1` открывается |
| привязка | `(sector, object)` → объект `.sbw`, проверено 1779/1779 |
| геометрия | origin + базис объекта, поворот `A[i]*phase` вокруг `-x/+y/-z` |
| точность | целые для фазы/высоты, `double` для долей и трансформа |
| детерминизм | порядок записей, без rand/времени; повторный прогон бит-в-бит |
| NaN/OOB | не-конечные `double`/углы отвергаются на сборке; все индексы проверяются |

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
| `0x00..0x30` | 9 × f64 | closed `D1..D3`, open `D4..D6`, size `D7..D9` (**уже сложенные** с size, см. ниже) |
| `0x48` | 3 × f64 | `A1..A3 / 65535` — ось вращения створки |
| `0x60` | f64 | константа `0.03` (аргумент звука) |
| `0x68` | flags | `kind = flags & 0xf`; `kind = 0` — инертная дверь |
| `0x6c..0x7c` | 5 × i32 | `ids[0..4]`; первые три — ключи `GENERATE`, `ids[3]`/`ids[4]` — id событий «открылась»/«закрылась» |
| `0x80` | state | `-1/0/+1` |
| `0x84/0x88` | sector/object | адрес створки в `.sbw` (`0x434419`) |
| `0x8c/0x90` | phase | `0..0xffff` и «фаза после тика» |
| `0x94` | step | приращение фазы за тик |
| `0x98/0x9c` | dwell/timer | авто-закрытие |
| `0xa0..0xb4` | V,N,V,N,V,N | три пары (громкость, sample) |

`id` двери — **первый ненулевой из `ids[0..2]`**: `GENERATE` (`0x44f690`)
сравнивает именно эти три поля. В `dm1` это, например, `-30231`, а не
нулевой `E1`.

`closed`/`open` в модели — **runtime-значения** (`+0x00` записи), т.е. угол
`D1..D3` плюс `size` `D7..D9`: загрузчик складывает их в `0x44e368..0x44e3f6`,
и именно в таком виде они лежат в сейве. Для SCRIPT-пути сложение делает
`door_from_scene`, поэтому `Door.closed` одинаков в обоих путях.
Восстановить исходный угол: `closed - size`.

### 1.3. Запись `SWITCH` (32 байта)

`kind, id, radius, x, y, z, aux, sound` — восемь dword'ов **дословно**
(`0x44e5f0`). В модели:

* `Switch.id` — **индекс записи** (в формате нет отдельного id переключателя);
* `Switch.target` — `id` (операнд 2), то есть тот самый `GENERATE`-id;
* `Switch.aux` (`+0x18`) и `Switch.sound` (`+0x1c`, `-1` = тишина) — звук
  переключателя (`0x44f960`);
* `Switch.inside` — runtime-флаг: последний ответ `poll_triggers`
  (`0x44fc30` защёлки не имеет).

`interact(switch_id)` принимает именно индекс переключателя.

### 1.4. `LIFT` (18) и `ADD_LIFT` (188)

Запись типа 18: `u32 n` + `n × 0xA8` + `u32 k` + три колонки по `k`:
`floor/button id`, `height`, `sound` (`docs/RECORDS.md` §4). Читаются
`kind +0x34`, `sob +0x38`, `x/y/z +0x3c..0x44`, `pos_delta +0x48..0x50`,
`basis +0x00..0x30` (три базовых угла + три приращения), `speed +0x54`,
`dwell +0x58`, `height +0x5c`, `height_tick +0x60`, `wait +0x64`,
`direction +0x68`, `stop_index +0x6c`, `call_ids +0x70..0x78`,
`call_event/radius +0x7c/+0x80`, звуки `+0x84..+0x98`, `stop_count +0x9c`,
`stop_start +0xa0`. Высота — целые единицы `0..0xffff`, `speed` — единицы за
тик; `frac = height/65535` даёт и позицию кабины, и углы.

У `ADD_LIFT` 148-байтный блок разобран по `docs/DOOR_FIELDS.md` §3.3: 37
dword'ов, где операнды 9..16 — **`double`**, поэтому `head[20] = speed`,
`head[21] = dwell`, `head[22] = height`, `head[26..28] = call ids`,
`head[31..36] = V,N,V,N,V,N`. Остановки читаются точно (`n × 12`).
Скриптовые лифты получают настоящую скорость и высоту и двигаются как записи
18; в поставочных 36 уровнях 45 `ADD_LIFT` (151 остановка), записей 18 нет
ни одной.

---

## 2. API

```cpp
#include "world/doors.h"

gm::WorldState state;
std::string error;
if (!state.build(sbt_level, sbw_level, &error)) { /* нет источников */ }

state.interact(51);            // нажать переключатель №51 (индекс)
state.interact(51, /*source=*/3); // то же, но source сразу в записи
state.poll_triggers({x,y,z}, 3); // 0x44fc30: proximity kind 0/1
state.trigger_at({x,y,z}, 3);    // 0x44fbd0: kind 0x104, точная точка
state.block_contact(sector, object); // 0x450590: кто-то в проёме
gm::EventQueue queue;
state.post_switch_events(queue, /*source=*/3); // SwitchPressed -> type-2
state.tick();              // один тик 91 Гц
state.tick(44);            // сразу 44 тика (O(1))

const gm::WorldStats& st = state.stats();
auto objects = state.apply_to_objects();   // + transform створки

gm::DoorTransform leaf = gm::door_transform(*state.find_door(-30231));
std::array<double,3> motion = gm::door_motion_delta(door, prev, next);
gm::LiftTransform cabin = gm::lift_transform(state.lifts().front());
```

`post_switch_events(queue, source)` постит по одному type-2 `Trigger Switch`
на каждый ещё не отзеркаленный `WorldEvent::SwitchPressed`; `source` — id
источника (игрок/сущность), `switch_index` — индекс переключателя. Внутренний
курсор (`switch_event_cursor_`) сдвигается по мере постинга; id `>= 0x200`
пропускается как заведомо непостижимый, а false от `post_switch` означает
только переполнение очереди. `tick()` сохраняет неотправленный хвост лога только после
неудачного `post` (очередь была полна); если post не вызывали, лог
очищается как frame-local. Полностью отправленный лог тоже очищается.

**Источник принадлежит записи, а не вызову.** Параллельно логу ведётся
`switch_event_sources_` (`std::vector<std::optional<std::int16_t>>`,
index-aligned с `events_`; слот заполняется при первом `post_switch_events`,
который видит запись). Поэтому хвост из нескольких прессов, застрявший в
полной очереди, уходит каждый со своим `source`: первое нажатие — source 2,
второе — source 7. Привязка одноразовая: retry из другого вызывающего
(`source=3`) не переписывает уже привязанный источник. Следствие: несколько
`interact()` без единого `post_switch_events()` между ними делят `source`
первого такого вызова — API `post_switch_events(queue, source)` знает только
один источник на вызов, а `WorldEvent`/`interact()` источника не несут (см.
§7, п. 8). Повторный вызов без нового `interact()` ничего не постит и не дублирует
события; при переполнении очереди курсор остаётся на неотправленной записи.
`clear_events()` и `build()` сбрасывают курсор и лог источников. Связка с
игровым циклом — `gm::post_switch_events(EventRuntime&, const WorldState&,
source)` из `game/events.h`: она кладёт type-2 в очередь рантайма, после чего
`runtime.pump(now)` даёт `GENERATED`-событие и (если у переключателя есть
звук) `SoundCommand`.

### 2.1. Типы

| тип | поля |
|---|---|
| `Door` | `id, sector, object, kind, state, timer, locked, open_fraction` + `flags, ids[5], closed/open/size[3], step, phase, phase_prev, phase_next, hold_ticks, sliding, linked, bound, axis[3], sound_level, start/move/stop_sound, event_open/event_close` |
| `DoorTransform` | `origin[3], basis[9], center[3], axis, angle` |
| `DoorSound` | `sample` (`SOUND_SAMPLE_TABLE` индекс, `-1`/`-2` = тишина), `volume`, `silent()` |
| `Switch` | `id, target, pressed, kind, radius, pos[3], aux, sound, inside` |
| `Lift` | `id, kind, sob, x, y, z, speed, height, target, stop_index, moving, call_ids[3], stops[], basis[6], pos_delta[3], dwell, height_tick, wait, direction, call_event, call_radius, call/arrive/move_sound, head[37], script` |
| `LiftTransform` | `origin[3], basis[9], angles[3]` |
| `LiftStop` | `floor_id, height, sound` |
| `ObjectDoorState` | `door_id, sector, object, state, open_fraction, sliding, linked, bound, transform` |
| `WorldStats` | счётчики дверей/переключателей/лифтов, `doors_bound`, `doors_distinct_objects`, `bad_records`, `bad_bindings`, `locked_hits`, `unmatched_targets`, `lift_calls`, `trigger_presses`, `blocked_hits`, `crush_hits`, `ticks` |
| `WorldEvent` | `SwitchPressed, DoorOpening, DoorClosing, DoorOpened, DoorClosed, DoorLocked, LiftCalled, NoTarget, DoorBlocked, DoorCrush` + `sample/volume` |

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
Двери с `kind == 0` инертны: `0x44f690` их пропускает, поэтому `interact`
считает такой `target` ненайденным.

`poll_triggers(pos, source)` — половина `0x44fc30` (см. `docs/DOOR_FIELDS.md`
§2.1):

* `kind == 0` — октагональное расстояние `max + (mid+min)/4` до `Switch.pos`
  меньше `radius` (строго);
* `kind == 1` — то же расстояние до `origin` створки, найденной по
  `ids[0..2] == target`;
* защёлки нет: пока условие выполнено, каждый вызов постит новое нажатие;
  `Switch::inside` даёт материал для фронта на стороне вызывающего;
* `kind 0x101` (прицел) и `0x102/0x103` (source-варианты скрипта) не
  позиционные и здесь не срабатывают; `kind 0x104` — это `trigger_at`
  (точное совпадение трёх `int`).

`block_contact(sector, object)` — `0x450590`: `kind 2` откатывает фазу тика
(`phase = phase_prev`) и переводит дверь в открывание (`DoorBlocked`);
`kind 3` делает то же и логирует `DoorCrush` (движковый `event_post(-22)`);
`kind 1` и неизвестная пара ничего не делают.

### 2.4. `tick(ticks)`

Один вызов — это `ticks` фиксированных шагов 91 Гц. Для двери:

```
phase_prev = min(phase, 0xffff)            // как +0x8c → +0x90 в 0x44f2e0
phase += sign(state) * step * ticks        // с зажимом в 0..65536
phase_next = min(phase, 0xffff)
```

`phase_prev` — это «зафиксированная» фаза, которую `0x450590` возвращает при
блокировке (`block_contact`); геометрия (`door_transform`) всегда считается
от текущей фазы. `door_motion_delta(door, phase_prev, phase_next)` — вектор
`obj+0x90` за тик.

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
    move_object(o.sector, o.object, o.transform); // origin + basis
}
```

`transform` — ровно то, что движок пишет в запись объекта `.sbw`
(`0x44dfc0` → `0x434887`). Кадр координат — операнды двери как есть: движок
использует их напрямую (никакого перевода в мировой кадр нет), поэтому
`origin` при `phase = 0` совпадает с `D1..D3` на всех 1779 дверях корпуса.
`apply_to_objects()` сам `SbwLevel` не мутирует: это data-only слой, а
применение — дело рендера/коллизии.

### 2.6. Runtime-управление

`set_locked(id, bool)`, `set_hold_ticks(id, ticks)`, `set_step(id, step)` —
для сценариев, которых нет в данных уровня (замок) или поле которых не
доказано (авто-закрытие). `find_door(id)`/`find_lift(id)` — поиск по
эффективному id (`Lift::id` — индекс записи, `Lift::sob` — модель).

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

Таймер считает **тики**, а не секунды: `91 тик = 1 с`. `hold_ticks` теперь
читается из записи двери `+0x98` (dwell), а `+0x9c` (timer) — из записи при
загрузке, как в `0x44f54d`/`0x44f550`; `L6 > 0` у **727** дверей корпуса
(`dm1` 22, `level0` 7), так что авто-закрытие в поставе есть. `L7 = 0`
у всех 1779 дверей; при `hold_ticks = 0` дверь остаётся открытой до
следующего `interact` — сознательный opt-in, а не забытая константа.

### 3.1. Скользящие, вращающиеся и «связанные» двери

* `sliding` — `D4..D6 ≠ D1..D3`; в поставочных данных 1534/1779.
* вращение — `A1..A3 ≠ 0`: 357 дверей (147 по `x`, 113 по `z`, 97 по `y`;
  при нескольких ненулевых работает приоритет `z > x > y`). Базис —
  поворот на `-angle`, `angle = A[i]*phase`.
* `linked` — прежний алиас `ids[3] != 0` (200/1779). По коду `ids[3]` —
  **id события «дверь открылась»**, `ids[4]` — «закрылась»
  (`0x44f3f0` → `event_post`); модель отдаёт их как
  `event_open`/`event_close` и пишет в `WorldEvent::target`, но не
  диспетчеризует (это `game/events.*` + VM).
* `kind` — младший ниббл `flags` (1..3). Различается только реакция на
  препятствие (`kind 2/3`, `0x450590`); геометрия у всех одна. `kind == 0`
  движок не анимирует и не переключает.
* Смещение створки **считается**: `door_transform()` — origin+basis,
  `door_motion_delta()` — вектор `obj+0x90`. Это не «мировой кадр»: движок
  работает в кадре операндов двери, и C++ повторяет его один в один.

### 3.2. Звук

У двери три пары (громкость, sample): `v0/n0` — запуск (`GENERATE` и
истечение dwell), `v1/n1` — цикл движения (ключ `i+1000000`), `v2/n2` —
остановка на любом конце. `n` — индекс `SOUND_SAMPLE_TABLE` (300 записей);
в поставе `n >= 0` у 523/864/626 дверей. Константа `+0x60 = 0.03` уходит в
вызов звука как `double`. У лифта: `+0x84/+0x90` вызов-отправление,
`+0x88/+0x94` движение (ключ `i+2000000`), `+0x8c/+0x98` прибытие;
samples есть у 30/33/28 из 45.

### 3.3. Writer записей 1/2/18

`engine/src/res/sbt.h`:

```cpp
std::vector<gm::SbtDoorRecord>   doors;
std::vector<gm::SbtSwitchRecord> switches;
gm::SbtLiftBlock lifts;          // {lifts, stops}
gm::parse_door_block(body, doors, error);      // body = n * 0xB8
gm::parse_switch_block(body, switches, error); // body = n * 0x20
gm::parse_lift_block(payload, lifts, error);   // u32 n + n*0xA8 + u32 k + 3*k
auto bytes = gm::encode_door_block(doors);     // обратно, байт-в-байт
auto payload = gm::encode_lift_block(lifts);
auto counted = gm::encode_counted(bytes, n);   // .sbt payload (u32 count + body)
```

Тела записей 1/2 — это одновременно тела блоков 1/2 сейва (без ведущего
`u32 count`), поэтому один парсер обслуживает оба контейнера. Каждый байт
двери/переключателя покрыт полем, у лифта недоказанные слова сохранены как
`unknown_8c`/`unknown_a4`, так что `encode(parse(x)) == x` точный.
Ограничения движка соблюдаются: 512/100/30 записей, 200 остановок,
`encode_*` возвращает пустой вектор на выходе за границы и на
`stop_start + stop_count > k`.

---

## 4. Доказано / гипотеза

**Доказано (код + данные):**

* запись `DOOR`: flags/ids/state/sector/object/phase/step/axis/dwell/timer/
  звуки по смещениям `0x48..0xb4` (`docs/DOOR_FIELDS.md` §1);
* `state = -1/0/+1`, `phase` — `0..0xffff` (геометрия делит на 65535),
  `step` — приращение за тик; `phase_prev`/`phase_next` — `+0x8c`/`+0x90`;
* `id` двери ищется по `ids[0..2]` (`0x44f690`), `id` переключателя —
  операнд 2; `ids[3]`/`ids[4]` — id событий открытия/закрытия;
* `(sector, object)` — адрес створки, `0x434419`; 1779/1779 валидны,
  1779 различных;
* геометрия створки: `center`, поворот, `origin = center - basisᵀ·size`
  (`0x44dfc0` → `0x434887`); `origin(0) == D1..D3` на всех 1779 дверях;
  вектор движения `obj+0x90` (`0x44f2e0` → `0x434f72`);
* триггеры: `0x44fc30` (kind 0/1, октагональная метрика), `0x479810`,
  `0x44f960`, `0x44fbd0` (kind 0x104); переключатель переключает **все**
  двери с этим id (группа);
* препятствие в проёме: `0x450590` (kind 2/3 откатывают фазу, kind 3 —
  `event_post(-22)`);
* `ADD_LIFT` остановки `n × 12` и все 37 dword'ов, включая `double`-операнды
  (speed `head[20]`, height `head[22]`);
* кабина лифта: `0x450180` (тик) → `0x434e32` (вектор) → `0x44e6f0` →
  `0x41e025` (базис из трёх углов);
* writer 1/2/18: байт-в-байт на реальных блоках сейва и на 45 лифтах; каждый
  байт записи покрыт полем.

**Гипотеза / приближение:**

* `kind` 1/2/3 не различает геометрию; доказана только реакция 2/3 на
  препятствие;
* `+0x60 = 0.03` и `v0..v2` — параметры звука (место вызова доказано, роль в
  микшере нет);
* порядок трёх углов кабины в `0x41e025` назван условно (`angles[0..2]`);
* `ADD_LIFT head[20] = speed` — доказано кодом и правдоподобно по данным
  (`100`, `150`, `4000`), но трёхлифтовое `99999` выглядит как «мгновенно»;
* `timer/hold_ticks` — читаются из уровня (`+0x98/+0x9c`), но `L7 = 0` у всех
  1779 дверей, поэтому авто-закрытие включает только `L6 > 0` (727 дверей);
* `locked` в `.sbt` не хранится (это runtime/редакторское состояние).

**Не проверено:** точная формула `0x44f326` при переменном `dt` (модель
фиксирует `dt = 1` тик, что совпадает с 91 Гц); диспетчеризация событий
двери и `-22` через VM; микшер `0x46b8d0`/`0x46b9f0`; прицельный триггер
`0x101` (нужен луч `0x43c51f`).

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
| `source` хвоста (`test_switch_source_tail`) | два пресса (source 2 и 7) в полной очереди, `tick` между ними; после drain retry даёт ровно две записи `switch_index` 0/1 с `source` 2/7 по порядку, третий вызов — 0 |
| `kind = 0` | дверь инертна: `interact` не находит target, `tick` не двигает фазу |
| геометрия (`test_geometry`) | скользящая дверь: базис единичный, `origin(0) = D1..D3`, `origin(0xffff) = D4..D6`, `delta = (open-closed)*1500/65535`; вращающаяся: `angle(0xffff) = A`, базис ортонормирован, `origin = center - Bᵀ·size`; приоритет осей `z > x > y`; кабина: `origin = pos + pos_delta*frac`, базис единичный при нулевых углах |
| звук (`test_door_sounds`) | `v0/n0` в `DoorOpening`, `v2/n2` в `DoorOpened`, `target = ids[3]`; `-1` и `>=300` — тишина |
| триггеры (`test_triggers`) | kind 0/1 по октагональной метрике; `(0.75r,0.75r,0.1r)` внутри, хотя евклидово `1.065r`; kind `0x101` по позиции не срабатывает; `0x104` — только точная точка; source переживает `post_switch_events(q, 99)`; повторов нет-защёлки: два `poll` → два пресса |
| блокировка (`test_block_contact`) | kind 1 — без реакции; kind 2 — `phase = phase_prev`, `Opening`, `DoorBlocked`; kind 3 — плюс `DoorCrush`, `crush_hits = 1` |
| writer (`test_sbt_writer`) | синтетика `encode(parse(x)) == x`; реальные `saved0/1.sbt`: 25 дверей и 24 переключателя байт-в-байт; 45 лифтов / 151 остановка: `encode(parse(x)) == x` и поля; битые размеры и `stop_count` за границей отвергаются |

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

Геометрия `dm1`: у всех 61 двери `door_transform(door).origin` при
`phase = 0` совпадает с `D1..D3` скрипта (до `1e-9`), `axis = A/65535`,
23 двери вращаются; базис ортонормирован на обоих концах хода.

### 5.3. Все 36 уровней (`--full`)

```
1779 doors, 3964 switches, 45 lifts
1779/1779 bound, 1779 distinct (sector, object)
1534 sliding, 200 linked, 357 rotating
```

Совпадает с `tools/re/doors.py` (36/36 уровней парсятся).

### 5.4. Writer (`test_sbt_writer`)

```
doors: saved0.sbt round-trip 25 doors, 24 switches
doors: saved1.sbt round-trip 25 doors, 24 switches
doors: type-18 writer round-trip 45 lifts, 151 stops
```

Реальных записей типа 18 в поставе нет (0 файлов), реальных блоков-лифтов в
сейвах тоже; поэтому байтовый якорь для 18 — `encode(parse(x)) == x` на
полезной нагрузке, собранной из 45 настоящих `ADD_LIFT` (поля, 151
остановка), плюс синтетическая запись из `make_lift_payload`. Для дверей и
переключателей якорь — настоящие блоки 1/2 сейва `saved0`/`saved1`.

---

## 6. Сборка и тесты

```bash
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGM_WITH_SDL3=OFF
cmake --build build/sandbox --target gm_doors_tests -j 32
ctest --test-dir build/sandbox -R doors --output-on-failure
```

* `doors` — метка `quick`, только `dm1` (~0.3 с);
* `doors_full` — метка `full`, все 36 уровней (~9 с);
* при отсутствии игры реальная часть печатает `skipped` и не падает;
* `python3 regress.py` к модулю отношения не имеет, но запускался на
  неизменённых `gm/**`.

---

## 7. Доверие и остаток

**Можно доверять:** привязке `(sector, object)` (1779/1779), числам
`dm1`/корпуса, фазе/`step`/`state`, развороту по повторному switch,
групповому переключению, лифту по записи 18 и по `ADD_LIFT`, трансформу
створки (origin+basis), вектору движения, октагональным триггерам,
реакции на препятствие и writer'у записей 1/2/18.

**Нельзя считать окончательным:**

1. **`kind` 1/2/3** — различается только реакция на препятствие; типа
   открывания в геометрии нет.
2. **Диспетчеризация событий двери** (`ids[3]/ids[4]`, `-22`) — модель
   логирует, но не исполняет: нужен `game/events.*` + VM.
3. **Прицельный триггер `0x101`** — требует луча `0x43c51f` и таблицы
   `sector/object/poly`; позицией не заменяется.
4. **Звук** — индексы сэмплов и места вызова доказаны; связка с
   микшером/`SOUND_SAMPLE_TABLE` вне модуля.
5. **Кабина лифта** — базис из трёх углов воспроизведён по asm `0x41e025`,
   но имена осей условны; в поставе базовые углы нулевые.
6. **Связь с коллизиями** — `CollisionWorld` по-прежнему статичен;
   `transform` и `motion_delta` отдаются, но в физику не подключены.
7. **Сейв-совместимость** — модель читает те же смещения, что блок 2;
   writer блока 2 уже есть в `res/save.*`, writer `.sbt` — здесь.
   `GameState::apply_to_doors` (`game/state.*`, только чтение) восстанавливает
   box/ids/phase/state, но не трогает новые `axis`/`phase_prev`/`phase_next`:
   угол створки остаётся из уровня, а `block_contact` до первого `tick`
   откатит фазу к значению из уровня. Для сетевого/сейв-восстановления это
   надо будет дополнить на стороне `game/state.*`.
8. **`source` на уровне `WorldEvent`** — источник живёт в параллельном логе
   `switch_event_sources_`, а не в самом событии. Добавлена перегрузка
   `interact(switch_id, source)` и `poll_triggers(pos, source)`: они
   привязывают source к своей записи сразу, поэтому `post_switch_events(q,
   99)` уже не подменит его. Старый `interact(switch_id)` источника не
   несёт — его сообщает только `post_switch_events(queue, source)`; несколько
   таких вызовов без `post` между ними делят source первого. В рантайме это
   не проявляется: `GameRuntime::press_switch` постит сразу после каждого
   `interact()`.
