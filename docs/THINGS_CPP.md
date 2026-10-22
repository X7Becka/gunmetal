# Gunmetal — C++-runtime вещей (`Thing`)

Что здесь: `engine/src/world/things.{h,cpp}` — headless-runtime вещей уровня,
построенных условиями `ADD_THING*` (cid 1..5) из `SCRIPT` (11) поставочного
`.sbt`. Модуль использует уже проверенные `parse_sbt`, `decode_sbt_script`,
`extract_scene`, `vm_state_from_level`, `VmExecutor` и `vm_merge_scenes`; он не
дублирует ни парсер `.sbt`, ни VM и **не трогает** рендер, игрока, коллизии,
аудио и UI.

Статус: API, статический и исполняемый обходы, минимальный тик, synthetic и
поставочные golden'ы готовы. CTest `things` (quick, 0.36 с) и `things_full`
(корпус, 47 с в Debug) проходят. Игра только читается; `regress.py` не
менялся (113/113).

---

## 0. Коротко

| что | значение |
|---|---|
| тик | фиксированные **91 Гц** (`kThingTickHz`) |
| top-level | **4 008** вещей (историческое поведение `scene.things`) |
| recursive | **7 563** статических сайта (then-ветки `IF`) |
| executed | **7 617** экземпляров (матрица 8 контекстов + `RANDOM`) |
| starts | **391** (`SET_PLAYER_START` + `SET_DEATHMATCH_START`) |
| типы | **2 896** записей `THING_TYPES`, **2 896/2 896** валидных model ref |
| «нет типа» | `0xFFFFFFFF` → `Thing.type == -1` |
| null-типы | 3 в top-level, 13 в recursive/executed (36 уровней) |
| порядок | контекст → entity-блок → инструкция; без rand/времени |
| пределы | глубина `IF` 64, число типов ≤ `0x80`, все индексы проверяются |
| tick | без AI: `age += ticks` у живых; при достижении `+0x1e8` — `expired=true` и один `expire`-event в `EventQueue`; `alive` меняет только API |

---

## 1. Источники и модель

### 1.1. Где лежат вещи

`SCRIPT` не хранит список объектов: каждая строка таблицы сущностей
`(flags, id, offset)` задаёт подсценарий, а `ADD_THING*` — это **условие**
(`EVAL`, опкод `0xC9`) или условие `IF`/`ELSE`, которое заодно строит вещь.
`GENERATE` запускает подсценарий сущности по `offset` (в сравнении с
Python-эталоном он по умолчанию не преследуется: вещи уже находятся
обходом всех блоков таблицы).

Компилятор раскладывает ветвление так (docs/NESTED_THINGS.md §1):

```
IF <cond> <size> { then-код ... ELSE <rel> } else-код ... join
```

* `size` покрывает then-ветку вместе с `ELSE`;
* else-код физически лежит **после** `IF` и в декодере является соседним
  top-level кодом;
* `0xFF` завершает подсценарий целиком.

Отсюда три множества:

| множество | как получается | число |
|---|---|---:|
| top-level | только `EVAL ADD_THING*` на верхнем уровне каждого блока | 4 008 |
| recursive | плюс `EVAL ADD_THING*` в then-ветках `IF`/`ELSE`, дедуп по `site` | 7 563 |
| executed | `VmExecutor` по всем блокам в матрице контекстов, дедуп по `(site, occ)` | 7 617 |

Разница `executed - recursive = +54` — сайты в петлях, исполненные повторно.
`executed` может содержать `ADD_THING*` в **условии `IF`**: статический
обход смотрит только `EVAL` (как Python), а VM создаёт вещь и из `IF`.
Именно поэтому у `dm1` top-level 38, а executed top-level 41.

### 1.2. Тип и модель

`Thing.type` — операнд `G`, индекс в записи `THING_TYPES` (4). Модель типа
лежит в `THING_TYPES[i] + 0x1A4` — это индекс в `SOB_TABLE` (20), доказано
загрузчиком `0x47cb6c` и проверено на 2 896/2 896 типах
(docs/SBT_RECORDS.md §2.4). `Things` читает обе таблицы напрямую из
`SbtLevel` (`records` типа 4, `level.sobs`), не парся `.soe`/`.sob`.

* `type == -1` (`0xFFFFFFFF`) — «нет»: `type_name` и `model` пустые;
* индекс вне таблицы — `invalid_type_things`;
* валидный тип с битым/отсутствующим `SOB_TABLE` — `missing_model_things`
  и `missing_model_refs` (в поставке таких нет).

### 1.3. Позиция и углы

Как в `gm/scene.py`:

| вариант | подпись | позиция |
|---|---|---|
| `ADD_THING` | `GLLLAAA` | абсолютная `(x,y,z)` |
| `ADD_THING_EX` | `LLLGLLLAAAL` | абсолютная |
| `ADD_THING_AT_SOURCE` | `LGAAA` | нет (от источника) |
| `ADD_THING_AT_SOURCE_WEAPON_POINT` | `LLGDDDD` | нет |
| `ADD_THING_RELATIVE` | `LGLLLAAADDDD` | нет |

`A/B/D`-операнды сохраняются в `angles` как `double` в порядке операндов
(у `ADD_THING_RELATIVE` их семь). Целые перед типом перезаписывают `owner`
ровно как `_thing_args`; целые после типа — позиция у абсолютных вариантов.

### 1.4. Parent, script, flags

* `parent` — id сущности, чей подсценарий создал вещь (`source_entity` VM).
* `script` — байтовое смещение этого подсценария в `SCRIPT` (у `GENERATE`
  берётся offset сущности-родителя, а не вызвавшего блока).
* `flags` — **flags строки таблицы сущностей** (0x3FF в поставке). Это
  единственный доказанный флаг, который есть у создателя; внутренние
  runtime-флаги записи `0x528` не разобраны (см. §5).
* `site` — смещение инструкции `EVAL`/`IF`; `occ` — счётчик её исполнения
  в рамках пути (петли); `depth`/`path` — синтаксическая вложенность `IF`.
* `id` — последовательный runtime-id в порядке обнаружения. Он уникален
  внутри `Things`; `stats().duplicate_ids` всегда 0. Это **не** слот
  движкового массива `0x528`.

---

## 2. API

```cpp
#include "world/things.h"

gm::SbtLevel level = gm::parse_sbt(bytes, error).value();
gm::VmState vm = gm::vm_state_from_level(level); // seed, players, entities

gm::Things things = gm::Things::load(level, vm); // матрица 8 контекстов
if (!things.ok()) { /* error() */ }

const auto& list = things.things();          // 7 617 исполняемых экземпляров
const auto& st   = things.stats();           // top/recursive/executed/spawns/...
const gm::Thing* t = things.by_id(0);
auto kids = things.children(-30070);         // parent == -30070
things.tick();                               // один тик 91 Гц
things.tick(91);                             // сразу 91 тик
things.tick(1, &events);                     // + expire-событие в очередь
things.kill(0); things.revive(0);

auto kids = things.spawn_children(0, &events); // дети типа (0x46d000), opt-in
```

### 2.1. `Thing`

```cpp
struct Thing {
    int id; std::string type_name; int type; std::string model; int model_index;
    std::optional<std::array<int,3>> position; std::vector<double> angles;
    std::uint32_t flags; int parent; bool alive; std::uint32_t script;
    std::string kind; int owner; std::uint32_t site; int occ; int depth;
    std::vector<std::string> path; bool executed; std::uint64_t age;
    bool expired; int expire_ticks; int movement_type; double speed; double search_radius; int target_zone_threshold;
    double gravity, damping, radius, turn_accel, turn_max, arrive_radius, leash_distance;
    int collision_shape; std::array<double,3> box; std::vector<ProximityEntry> proximity;
    int target_history_offset; int max_health; int health;
    std::array<int,4> shields; std::array<int,4> shield_max;
    int hits_left; std::uint32_t timeout_event, collide_event, expire_event;
    std::uint32_t create_event, damaged_event, destroy_event;
};
```

`absolute()` — `position.has_value()`. Поля `type_name/model/model_index` —
разрешённые ссылки; `position` пуст у source-relative вариантов.

Новые поля — `THING_TYPES +0x1b4/+0x10/+0x58/+0x1cc/+0x1d0/+0x1d4` плюс боевой
блок `+0x1c4` (**collide event**, второй string десятиполевого блока; 0x4752d0),
`+0x1c8` (hits), `+0x1d8..+0x1e4` (shields), `+0x1ec` (expire),
`+0x1f4` (damaged), `+0x1f8` (destroy), `+0x1f0` (create), `+0x1c0`
(time-out), `+0x1c4` (collide), `+0x1ec` (expire) и `+0x1e8` (expire ticks),
копируемые в runtime; `gm::post_thing_event` перекладывает их в
`EventQueue` с координатами и `extra = id − 0xfff8` (`docs/SOE.md`, отчёт impact §4.1).
`speed` (`+0x10`, double) — мировые единицы за тик; `0x4752d0` двигает
снаряд как `pos += speed·dt`, поэтому оружие переводит её в ед/с (`×91`).
`gravity` (`+0x78`), `damping` (`+0x80`), `collision_shape` (`+0x1a0`),
`radius` (`+0x30`), `box` (`+0x38/+0x40/+0x48`), таблица близости (`+0x218`), `turn_accel` (`+0x20`),
`turn_max` (`+0x28`), `arrive_radius` (`+0x70`), `leash_distance` (`+0xd0`)
читаются в том же проходе; `standoff` — уже существующее
`target_zone_threshold` (`+0x1cc`).
`Things::load` заполняет их для статических и executed-экземпляров;
`health` и `shield_max` — копии на спавне. AI пока их не использует.
Для `dm1`/`level0` первый `bgchunk1`: `movement_type=6`,
`search_radius=6000`, `target_zone=200`, `history=0`,
`max_health=health=2000`, `shields=0`, `timeout_event=collide_event=0`,
`create_event=-30069`, `expire_event=destroy_event=-30071` (CTest `things`).

`gm::thing_type_events(level, type)` даёт те же event-id без построения
`Things`; загрузчик оружия использует его для `THING_TYPES[type]`
(`docs/WEAPONS_CPP.md`). Golden `level0`: type 28 GRENADE collide `-30094`,
expire `-30093`; type 31 ROCKET collide=expire `-30103`, create `-30104`
(CTest `things::type_events`).

### 2.2. `Things::load`

```cpp
Things Things::load(const SbtLevel&, const VmState&,
                    const ThingsOptions& = {});
```

* `build_static` — top-level и recursive списки;
* `execute` — прогон VM;
* `contexts` — пусто → `default_thing_contexts()`:
  `game_mode 0/1 × playertype 0..3`;
* `random_unknown = true` — форкать `RANDOM`; именно это даёт 7 617;
* `follow_generate = false` — как в эталонном сравнении; `true` нужен для
  проверки дочерних `GENERATE` и цикла;
* `strict = true` — битый/слишком глубокий блок попадает в `error()`,
  `stats().decode_error`/`nesting_limit`, а не молча теряется;
* `max_steps/max_paths/max_time` передаются в `VmOptions`.

Состояние каждого контекста строится заново через `vm_state_from_level` из
конфигурации переданного `vm` (difficulty, players, seed, source_player,
god_mode); переопределяются только `game_mode` и `playertype`. Поэтому
результат детерминирован и не зависит от того, исполнял ли вызывающий
главный скрипт.

### 2.2a. Локации и AI-маршруты

`Things` собирает выполненные `ADD_LOCATION` (`0x45bea6`) в таблицу
локаций; таблица детей типа — это `child_types`/`child_params` в каждой
`Thing` (см. §2.3a): id типа и параметр плюс локальное смещение и углы
ребёнка.

`locations()`: `{id, x, y, z, orient[3]}`. `location(id)` — первый матч,
как линейный поиск `0x476012`. `Thing::path_locations` хранит сырые id из
`THING_TYPES+0x324[]` (0..32); `Ai::set_path_from_thing` резолвит их через
эту таблицу. Дубликаты `(site, occ)` детерминированно дедуплицируются; при
`execute=false` таблица пуста. `pathrandom` (тип 11) выбирает точки через
внедряемый `MsvcRand` (см. `AI_CPP.md` §8); `DELETE_LOCATION` в рантайме не
моделируется.

### 2.3. `tick`, `alive`, `children`

`spawn(thing, events)` добавляет runtime-экземпляр, назначает свободный id
при `thing.id < 0` (явный занятый id отклоняется), ставит `executed/alive`,
сбрасывает `age/expired` и опционально постит `+0x1f0` create-event.
Явный id, уже занятый **живой** записью, отклоняется; id удалённой (`alive=false`)
записи можно переиспользовать — это нужно для респавна снарядов/вещей.
Автоматический аллокатор (`id < 0`) тоже берёт самый младший мёртвый слот,
а не только увеличивает счётчик.
`remove(id, events)` оставляет запись в `things_` (индексы стабильны), ставит
`alive=false` и опционально постит `+0x1f8` destroy-event.

`tick(n, events)` увеличивает `clock()` и `age` **только живых** вещей. Если
у типа `+0x1e8 > 0` и `age` дошёл до него, вещь получает `expired=true` и в
`events` (если передан) уходит один `+0x1ec` expire-event через
`post_thing_event`; `alive` при этом **не** меняется — так же, как
`0x4703a0`, который только постит событие. AI, респауна и удаления в модуле
нет: `kill/revive/set_alive` — явные операции. `children(parent)` возвращает
указатели на экземпляры с данным `parent` в детерминированном порядке.

**Движение (opt-in).** `set_motion_enabled(enabled, world)` включает автоматы
`0x470c70` (`world/thing_motion.*`): каждый живой экземпляр с
`movement_type 0..18` и `!expired` делает один шаг за тик. Состояние создаётся
при первом шаге через `thing_motion_spawn(thing_motion_config(thing))`; для
абсолютных вариантов (`position` есть) в него кладётся позиция из скрипта,
для source-relative — начало координат (как спавн `0x46dfe9`). `Thing::position`
не переписывается: int-операнды не несут double-состояния, а у
source-relative вещей абсолютной позиции нет — авторитетно
`motion_state(index)`. Цель для seek/турели/патруля — `set_motion_target`;
тела для push-режима — `set_push_targets` (общий список, как таблица игроков
в `0x476830`). `motion_last(index)` отдаёт последний `ThingMotionStep`
(включая `fired_event` — это id `+0x21c`, постит его игровой слой).
Выключение не стирает состояния, только перестаёт шагать; respawn слота
сбрасывает состояние и цель. Счётчики — `stats().motion_steps/motion_moved/
motion_pushed`. Тест — `test_motion_tick` (`gm_things_tests`).

**Прицел из AI.** `aim_motion_at(index, point, visible)` заполняет вход
автоматов так, как это делает `0x473a00`: `position = point`, `yaw_error` —
пеленг точки в системе тела (`0x437909` — проекции на строки базиса,
`0x4226e9` — нормировка, `0x41e2b8` — угол; forward = `(sin yaw, cos yaw)`,
поэтому точка на `+X` при `yaw = 0` даёт `+π/2`), `pitch_error = 0` (эта
сборка его обнуляет), `vertical_error = point.z − position.z` (гейт
`|e| ≥ 20` и добавка к `velocity.z` — в `thing_motion.cpp`). Нужно уже
созданное состояние (включённые автоматы + хотя бы один `tick`), иначе
`false`: углы тела живут в состоянии. `clear_motion_aim(index)` ставит
`valid = false`, `motion_target(index)` отдаёт текущую цель (`nullptr`, если
её ещё не задавали), `index_of(id)` — стабильный индекс слота для моста из
AI. Тест — `test_motion_aim`.

### 2.3a. Спавн детей типа (`0x46d000`)

Запись `THING_TYPES` умеет описать до четырёх **детей**: `+0x2fc` — их
количество (clamp 4), `+0x300+4i` — `THING_TYPES`-id ребёнка, `+0x310+4i` —
параметр ребёнка, `+0xd8+0x18i` — три `double` локального смещения ребёнка и
`+0x138/+0x158/+0x178` (`+8i` каждое) — его собственные pitch/roll/yaw.
Оригинальный `0x46d000` (создание вещи) после создания родителя создаёт
**один уровень** детей:

```
child_pos     = parent_pos + parent_matrix · child_offset
child_matrix  = 0x41e141(child_matrix, parent_matrix)   // композиция
child_angles  = 0x41e2f9(child_matrix)                   // углы из матрицы
child_flags_a = type_flags_a | child_param | parent_flags  // +0x360
```

* `parent_matrix` — строки (right, forward, up) объекта родителя; `0x41e025`
  строит их из (roll, pitch, yaw), а `0x46d8b0` раскладывает операнды
  `ADD_THING*` как pitch/roll/yaw (в записи `+0x18/+0x20/+0x28`).
* `0x41e141` считает `result[i] = parent[0]·child[i][0] + parent[1]·child[i][1]
  + parent[2]·child[i][2]` — вектор ребёнка задан в системе родителя.
* `0x41e2f9` восстанавливает углы из матрицы: `yaw = atan2(forward.x,
  forward.y)`, `pitch = -asin(forward.z)`, `roll = atan2(-right.z, up.z)`.
  Если у ребёнка стоит флаг `0x2000` (тип `+0x1ac`), углы **не**
  пересчитываются — остаются собственные.

В C++ это `Things::spawn_children(parent_id, events)`:

* берёт у родителя `child_types`/`child_params`/`child_offsets`/`child_angles`
  (они уже разобраны в `Thing` при `load`);
* позиция ребёнка — `origin + базис·offset`: точный `double`-тройник лежит в
  `Thing::world_position`, целочисленный вид — в `Thing::position` (его же
  использует `post_thing_event` для координат события);
* ориентация — композиция выше; в `Thing::angles` кладётся
  (pitch, roll, yaw) — как и у скриптовых вещей;
* ребёнок получает `parent = parent_id`, `alive/executed`, поля своего типа
  (`apply_type_fields`) и `flags_a = type_flags_a | child_param |
  parent_flags` (в оригинале `param_12` — это флаги, которые вызывающий код
  передаёт в `0x46d000`; VM передаёт 0, здесь аналог — `flags` родителя);
* в `events` (если передан) уходит `+0x1f0` create-event ребёнка — так же,
  как у `Things::spawn`;
* возвращает id созданных детей.

Спавн **одного уровня**: дети ребёнка не создаются. Повторный вызов для того
же родителя не дублирует детей (оригинал создаёт их один раз, при создании
родителя); список лежит в `Thing::type_children` (в оригинале — счётчик
`+0x314` и ids `+0x318` в записи родителя), доступен как
`Things::type_children(parent_id)`.

**Opt-in.** По умолчанию спавн выключен
(`ThingsOptions::spawn_type_children = false`): движок создаёт детей всегда,
но C++-модуль считает вещи и AI по экземплярам, поэтому включение меняет
golden-счётчики корпуса (7 617 исполняемых экземпляров и 591 экземпляр с
детьми). Это осознанное отклонение до пересчёта голденов; включается флагом
(`Things::load` тогда создаёт детей каждого экземпляра).

**Порядок.** В оригинале ребёнок создаётся сразу за родителем, поэтому его id
идёт следом. Здесь `Things::load` сначала строит полный список скриптовых
экземпляров и только потом создаёт детей — id детей идут в конце списка.
Собственный вызов `spawn_children` в середине жизни списка, наоборот, даёт
id сразу после текущего максимума.

**Что не доказано.** Порядок сложений в x87 воспроизведён по дизассемблеру
(`0x46d000`: сначала `right`/`up`, потом `forward`; для `y` — сначала
`right`/`forward`, потом `up`), но реальные углы родителя в поставке всегда
нулевые, поэтому численно это различие не проверено. Позиция source-relative
родителя в C++-виде отсутствует (операны `ADD_THING_RELATIVE` не несут
мировых координат), поэтому начало координат для его детей — ноль, а не
мировая позиция сущности; мировая позиция ребёнка в оригинале берётся из
объекта родителя.

### 2.4. `stats()`
Счётчики: `top_level`, `nested`, `recursive`, `executed`,
`executed_top_level/nested`, `spawns_static/executed`, `thing_types`,
`valid_model_refs`, `missing_model_refs`, `null_type_static`,
`null_type_things`, `invalid_type_things`, `missing_model_things`,
`static_site_duplicates`, `duplicate_ids`, `blocks`, `paths`, `max_depth`,
`ticks`, `motion_steps`, `motion_moved`, `motion_pushed`, `nesting_limit`,
`decode_error`, `reasons` (стоп-причины VM).
`model_refs()` = `valid_model_refs + missing_model_refs`.

---

## 3. Поставочные counts и golden

36 `.sbt`, seed `0x12345678`, матрица `--contexts all --random-unknown`:

| метрика | значение |
|---|---:|
| уровней | 36/36 |
| top-level | **4 008** |
| recursive | **7 563** |
| nested (recursive − top) | 3 555 |
| executed | **7 617** |
| loop-повторы (executed − recursive) | +54 |
| starts static / executed | 391 / **391** |
| `THING_TYPES` | **2 896** |
| валидных model ref (`+0x1a4`) | **2 896/2 896** |
| null-типы top / recursive / executed | 3 / 13 / 13 |
| дубли статических сайтов | 0 |
| duplicate runtime id | 0 |

Построчно (эти числа закреплены в `test_things.cpp`):

| уровень | top | recursive | executed | starts |
|---|---:|---:|---:|---:|
| dm1 | 38 | 138 | 141 | 13 |
| dm2 | 33 | 123 | 125 | 9 |
| dm3 | 41 | 131 | 133 | 9 |
| dm4 | 40 | 130 | 132 | 10 |
| level0 | 29 | 119 | 121 | 8 |
| level00 | 28 | 118 | 120 | 8 |
| level000 | 28 | 118 | 120 | 8 |
| level0a | 28 | 118 | 120 | 8 |
| level0b | 27 | 117 | 119 | 1 |
| level1 | 59 | 155 | 157 | 16 |
| level10 | 64 | 169 | 171 | 10 |
| level11 | 188 | 292 | 294 | 9 |
| level12 | 141 | 244 | 248 | 9 |
| level13 | 171 | 267 | 268 | 9 |
| level14 | 149 | 249 | 253 | 9 |
| level15 | 96 | 190 | 191 | 9 |
| level16 | 239 | 335 | 337 | 16 |
| level17 | 189 | 299 | 292 | 10 |
| level18 | 278 | 410 | 397 | 10 |
| level19 | 127 | 237 | 242 | 16 |
| level2 | 55 | 148 | 152 | 16 |
| level20 | 102 | 197 | 199 | 11 |
| level21 | 159 | 258 | 260 | 16 |
| level22 | 212 | 320 | 324 | 9 |
| level23 | 236 | 337 | 339 | 16 |
| level24 | 165 | 265 | 267 | 16 |
| level3 | 139 | 248 | 247 | 16 |
| level4 | 88 | 181 | 183 | 10 |
| level5 | 109 | 204 | 206 | 17 |
| level6 | 98 | 195 | 199 | 10 |
| level7 | 134 | 240 | 244 | 9 |
| level8 | 126 | 226 | 230 | 9 |
| level9 | 192 | 287 | 289 | 9 |
| secrt1 | 74 | 175 | 176 | 12 |
| secrt2 | 90 | 197 | 193 | 9 |
| temple | 36 | 126 | 128 | 9 |
| **TOTAL** | **4 008** | **7 563** | **7 617** | **391** |

`executed < recursive` на level3/17/18/secrt2 — статические сайты, которые
не достигаются ни одним из 8 контекстов (в сумме 38); `executed > recursive`
на остальных — повторные исполнения сайта в петле. Это ровно поведение
`tools/re/nested_things.py --contexts all --random-unknown`.

`dm1` (golden первого и последнего экземпляра):

* `executed[0]`: `kind=ADD_THING_RELATIVE`, `type=0`, `bgchunk1`,
  `model=BIGCHUNK`, `parent=-30070`, `site=11029`, `depth=1`, позиции нет,
  7 углов;
* `executed[-1]`: `type=58`, `OILDRUM`, `model=OILDRUM`, `parent=-4`,
  `site=67357`, позиция `(5688, 216, 264)`.

`level0` (golden первого): `ADD_THING_RELATIVE`, `type=0`, `bgchunk1`,
`BIGCHUNK`, `site=13943`, `depth=1`.

---

## 4. Доказано / гипотеза

### 4.1. Доказано

* Подписи и семантика `ADD_THING*` (cid 1..5), `G` = тип, `A/B/D` = double,
  целые до типа = owner, после — позиция (docs/FORMATS.md §12.4,
  gm/scene.py, 4 005 реальных операндов + 3 `0xFFFFFFFF`).
* Связка `THING_TYPES + 0x1A4 → SOB_TABLE` (2 896/2 896; docs/SBT_RECORDS.md
  §2.4) и имена типов из `THING_TYPES[0..16]`.
* Три множества 4 008 / 7 563 / 7 617, 391 старт, 38 недостигнутых сайтов —
  совпадают с `tools/re/nested_things.py --check --contexts all
  --random-unknown`; C++ VM уже сверен с Python в `test_vm_exec.cpp`.
* `IF`-условие `ADD_THING*` тоже создаёт вещь (VM), а статический `EVAL`-
  обход её не видит; отсюда разница top-level 38 vs 41 в `dm1`.
* `RANDOM` форкается только с `random_unknown`; `GENERATE` по умолчанию не
  преследуется; цикл `GENERATE` останавливается причиной `recursion`.
* Дедупликация: статика по `site`, исполнение по `(site, occ)`, блоки
  сущностей — по offset (первая строка выигрывает), как в Python.
* `SET_PLAYER_START`/`SET_DEATHMATCH_START` дают 391 старт в обоих видах.
* Таблица детей типа: `+0x2fc` (clamp 4), `+0x300+4i` (id типа),
  `+0x310+4i` (параметр), `+0xd8+0x18i` (локальное смещение) и
  `+0x138/+0x158/+0x178` (pitch/roll/yaw ребёнка) — разобрана и используется
  спавном `0x46d000` (§2.3a): позиция `origin + базис·offset`, композиция
  ориентации `0x41e141`, обратное преобразование `0x41e2f9`
  (`yaw = atan2(forward.x, forward.y)`, `pitch = -asin(forward.z)`,
  `roll = atan2(-right.z, up.z)`), флаг `0x2000` оставляет собственные углы.

### 4.2. Гипотеза / модель, а не реверс

* `Thing.id` — наш последовательный handle, а не слот массива `0x528`.
  Реальный runtime-массив (260 слотов, шаг `0x528`) в карту не входит.
* `flags` — flags строки сущности; внутренние runtime-флаги `0x528`
  (render/effect/spawn time) не разобраны.
* `alive`, `age`, `tick`, `kill/revive` — минимальная модель. В движке
  удаление/респаун делают команды и AI; здесь их нет.
* `parent` — `source_entity` VM (id сущности). Для вещи из `IF`-условия это
  тот же блок; для `GENERATE` — дочерняя сущность. Иерархия вещей внутри
  одного блока не восстанавливается.
* `script` для `GENERATE` — offset родительской сущности; при совпадении id
  в таблице берётся последняя строка (`std::map`).
* Порядок `things()` — контекст → блок → путь → инструкция. Он
  детерминирован, но не является порядком движкового массива.
* `parent` у ребёнка типа — runtime-id родителя (в оригинале связь односторонняя:
  родитель хранит ids детей в `+0x314/+0x318`), у скриптовой вещи — id сущности;
  пространства имён разные, поэтому `children()` по одному и тому же числу
  может вернуть и то и другое.

---

## 5. Trust / безопасность

* Глубина `IF` ограничена `kThingMaxDepth = 64` (в корпусе максимум 5).
  Глубже — `nesting_limit`, ошибка, блок пропускается; рекурсия не растёт.
* `THING_TYPES` принимается только при `count ≤ 0x80` и полном размере
  `4 + count*936`; иначе таблица считается отсутствующей.
* Индексы `type`, `model_index`, `site`, `offset` проверяются; `0xFFFFFFFF`
  не превращается в огромный индекс.
* Битый блок (`truncated`, неизвестный опкод) даёт `decode_error` и
  `error()`; исполняемая часть всё равно не выходит за `code`.
* Никаких записей в игру: только чтение файлов; `regress.py` не менялся.
* Память: 7 617 экземпляров с вектором углов/пути — единицы мегабайт;
  статические и исполняемые списки хранятся отдельно.

---

## 6. Остаток

1. **Runtime-запись `0x528`** (260 слотов): id/флаги/effect/spawn time,
   привязка к слотам сейва (блок 3). Сейчас `Thing.flags` — флаги сущности.
2. **AI и жизненный цикл**: движение, респаун, `REMOVE_*`/`DELETE_*`,
   реакции на урон. `spawn`/`remove`/`kill`/`revive` уже дают явный lifecycle
   и create/destroy-события; `tick` делает age/expire.
3. **`GENERATE`-граф по умолчанию**: сейчас, как в эталонном сравнении,
   дочерние подсценарии не преследуются; `follow_generate=true` есть, но
   полноценная модель спавна (лимиты, очереди, id) не построена.
4. **Иерархия**: `parent` — сущность, а не родительская вещь; вложенные
   `ADD_THING` внутри одного блока не образуют дерево. Дети типа (§2.3a)
   образуют один уровень (родитель → его `THING_TYPES`-дети), но их дети
   не создаются.
5. **Модели**: строки `SOB_TABLE` разрешены, но сами `.sob`/`.soo` не
   загружаются и не валидируются; `model_index` — только ссылка.
6. **Сейв/сцена**: интеграция в `main.cpp`/`scene` не делалась (вне scope).
