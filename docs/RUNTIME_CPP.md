# Gunmetal — C++-каркас фиксированного тика (`GameRuntime`)

Что здесь: `engine/src/game/runtime.{h,cpp}` — один владелец runtime-подсистем
уровня и один документированный порядок шага. Срез вертикальный и без
рендера/ввода: `main.cpp` его пока не вызывает, API закреплён тестами
`gm_game_state_tests`.

Статус: каркас и тесты готовы; CTest `game_state` (метка `quick`, ~0.2 с)
проходит. Глобального состояния нет, конфиг — value-объект.

---

## 0. Коротко

| что | значение |
|---|---|
| владеет | `WorldState`, `Things`, `Ai`, `Projectiles`, `EventRuntime`, `CollisionWorld` |
| конфиг | `GameRuntimeConfig{ AiConfig, ThingsOptions, build_ai, install_switches, projectiles_use_things, player_radius }` |
| тик | 91 Гц, `tick(n)` = ровно `n` проходов тела |
| порядок | вещи → AI → снаряды → двери/мир → события |
| события | очередь опустошается в том же тике, повторного dispatch нет |
| снаряды | один `Projectiles::tick_results(1, …)`; записи отдаются как есть |
| ввод | `press_switch()` (не `world().interact()` напрямую) |
| сброс | `reset()` (всё, кроме конфига), `clear_events()`, `clear_projectiles()` |
| лимиты | нет рендера/ввода/звука, `GameState`/`Weapons` не подключены |
| тесты | `gm_game_state_tests` → `ctest -R '^game_state$'` |

---

## 1. Владение и конфиг

`GameRuntime` — не копируемый и не перемещаемый объект: `Ai` хранит указатель
на принадлежащий ему `Things`, а спаны `ProjectileTargets` указывают на
scratch-массивы рантайма. Собирайте и используйте его на месте.

```cpp
#include "game/runtime.h"

gm::GameRuntimeConfig config;      // значение, копируется в рантайм
config.ai.use_move_step = false;   // например
gm::GameRuntime runtime(config);

std::string error;
if (!runtime.build(level, vm, geometry, &error)) {
    // error: "world: …" или "things: …"
}

runtime.set_player_eyes(eyes);     // 0..8 точек глаз
runtime.press_switch(0, 0);        // вход через шов, см. §4
runtime.tick(10);

for (const gm::ProjectileTickRecord& record : runtime.last_projectile_results()) {
    // …
}
```

`build()` пересобирает всё: `CollisionWorld` из `.sbw`, `WorldState` из
записей DOOR/SWITCH/LIFT (или SCRIPT), `Things::load` из `.sbt` + `VmState`,
затем `Ai::attach` и таблицу `ADD_SWITCH` в `EventRuntime`. При ошибке рантайм
остаётся в `reset()`-состоянии. Частичные сборки — `build_world()` (без вещей)
и `build_things()` (без мира); они не сбрасывают чужую половину.
`build_world()` заменяет таблицу свитчей целиком и поэтому очищает staging-
очередь: type-2 записи адресуют старую таблицу по индексу.

`has_world()`/`has_things()`/`has_ai()` показывают, что собрано; `clock()` —
число исполненных тиков (до первого тика `0`).

---

## 2. Порядок тика

`tick(n)` вызывает одно и то же тело `n` раз. Внутри ровно один проход по
каждой подсистеме:

1. **вещи** — `Things::tick(1, &queue)`: возраст, `+0x1e8` lifetime, событие
   `+0x1ec` один раз на вещь;
2. **AI** — `Ai::sync_vitals()` (вещь, убитая уроном, умирает здесь), затем
   `Ai::tick(collision, 1/91 s)`;
3. **снаряды** — `Projectiles::tick_results(1, targets, collision, &queue)`;
   события постит сам `tick_results` внутри своего единственного цикла, а
   возвращённые записи перемещаются в `last_projectile_results()`;
4. **двери/мир** — `WorldState::tick(1)`: фазы дверей, лифты, сброс `pressed`;
5. **события** — повторная попытка хвоста switch-записей, затем
   `EventRuntime::pump(now)`; `now` — номер текущего тика до инкремента
   (первый тик — `0`).

`tick(0)` не делает ничего. `tick(n)` не вызывает подсистемы «пачкой»: AI и
снаряды шагают по одному тику, поэтому таймеры и `life_left` совпадают с
побайтовым проходом `n` раз по `tick(1)`.

Детерминизм: все подсистемы обходят собственные векторы в порядке вставки, RNG
и чтения часов нет, `std::map` не итерируется. Два одинаковых билда + одинаковая
последовательность вызовов дают одинаковый поток состояний (тест §7).

---

## 3. API результатов

* `last_projectile_results()` — `std::span<const ProjectileTickRecord>`, одна
  запись на слот `Projectiles::list()` в порядке спавна, ровно из последнего
  `tick_results`. Записи не пересобираются: это те же объекты, что вернул
  `Projectiles`. Живут до следующего `tick()`/`reset()`.
* `last_events()` — `const EventDispatchResult&` последнего тика (`generated`,
  `sounds`, `text_posted`). Пустой, но валидный результат, если очередь была
  пуста. `ok == false` рантайм сам не производит: очередь он строит только
  через `post_*`.
* `events()` — сам `EventRuntime` (очередь, таблица свитчей, реестр текста) для
  чтения/настройки; `world()`, `things()`, `ai()`, `projectiles()` — владеемые
  объекты. Внутренние объекты не копируются: аксессоры отдают ссылки/спаны.
* `last_projectile_results()` и `last_events()` относятся только к последнему
  шагу; для `tick(n)` это шаг `n`.

Счётчики подсистем остаются их собственными: `world().stats()`,
`things().stats()`, `ai().stats()`, `clock()`.

---

## 4. Свитчи и «хвост», который нельзя терять

`WorldState::tick()` стирает свою frame-local лог-очередь `events_`, если
предыдущий `post_switch_events()` не упёрся в полную очередь. Поэтому ввод
идёт через `press_switch(switch_id, source)`:

1. `WorldState::interact()` нажимает свитч и пишет `SwitchPressed` в лог мира;
2. сразу же `post_switch_events()` переносит запись в staging-очередь
   (`EventQueue`); при переполнении `WorldState` запоминает позицию хвоста;
3. в конце тика фаза событий ещё раз пробует `post_switch_events()` и делает
   `pump()`. Очередь опустошается, поэтому на следующем тике остаток
   доедет ровно один раз.

`source` первой попытки сохраняется в `WorldState` и не перезаписывается
ретраем. `clear_events()` сбрасывает и курсор, и блокировку — незапощенный
хвост при этом теряется осознанно (это «сброс событий»).

Прямой `world().interact()` допустим только если вызывающий сам сразу
постит запись; иначе ближайший `tick()` сотрёт её.

---

## 5. `ProjectileTargets` и вещи

Спаны `ProjectileTargets` строятся из собственных данных рантайма в
`projectile_targets()` (плоские scratch-векторы, capacity сохраняется между
тиками — записи вещей не копируются):

* игроки: точки `set_player_eyes()` + `config.player_radius` (по умолчанию 0,
  то есть точки); `player_kind`/`player_has_object` не задаются — отсутствующие
  спаны `projectile_step` трактует как «нет данных»;
* вещи: позиция (только absolute, source-relative получают `alive = 0` и
  нулевую точку, чтобы индексы совпадали с `Things::things()`), радиус,
  `alive`, `movement_type`, `owner`, `parent`; `flags2 = 0` (у `Thing` нет
  runtime-слова `0x20000`, мёртвость уже в `alive`), `has_object` — наличие
  модели (`Thing::model`);
* `config.projectiles_use_things = false` оставляет только игроков.

`Ai` снимает снимок списка вещей в `attach()`; после `spawn_thing()` /
`remove_thing()` нужно вызвать `attach_ai()` (агенты пересоздаются, их
состояние сбрасывается).

---

## 6. Сброс

* `reset()` — пустые мир/вещи/AI/снаряды/события/коллизия, нет игроков,
  `clock() == 0`; `config()` не меняется. Используется тестом детерминизма
  как «второй прогон с нуля».
* `clear_events()` — очередь, лог свитчей мира, лог AI и last-результаты;
  реестр текста (у него свои дедлайны по кадрам) остаётся. Часы не сбрасываются.
* `clear_projectiles()` — флот снарядов и `last_projectile_results()`.

---

## 7. Тесты

`gm_game_state_tests` (тот же файл `engine/tests/test_game_state.cpp`,
`engine/tests/CMakeLists.txt` не менялся):

| тест | что закрепляет |
|---|---|
| `test_runtime_events_not_duplicated` | вещь с `+0x1e8 = 1` постит `+0x1ec` один раз; второй тик не даёт `generated`, очередь пуста, `age`/`expired` корректны; `tick(0)` — no-op |
| `test_runtime_projectile_results` | `life_ticks = 1` → `expired` + одно событие в записи и в dispatch; второй тик — `already_spent` без событий; `remove_projectile` → `destroyed`/`dead`, слот остаётся |
| `test_runtime_pending_switch_tail` | 4300 нажатий переполняют очередь (0x7526 / 7 байт); тик 1 отдаёт часть, тик 2 — остаток, сумма ровно 4300, третий тик пуст |
| `test_runtime_determinism` | 12 тиков с дверью, вещами, AI и снарядом: хеши состояний двух рантаймов совпадают, `reset()` + повторная сборка дают тот же поток; дверь за 12 тиков уходит на `phase = 18000` |

Фикстуры синтетические (SWITCH/DOOR-записи + `.sbw` из одного объекта), поэтому
тесты не зависят от установленной игры. Реальные level0/dm1 по-прежнему
покрываются `doors`, `things`, `thing_ai`, `game_state` (сейвы).

Сборка:

```sh
cmake --build build/dev --target gm_game_state_tests
ctest --test-dir build/dev -R '^game_state$' --output-on-failure
```

---

## 8. Границы и что не доказано

* `main.cpp` не подключён: срез не рендерит, не читает ввод, не играет звук.
* `GameState` и `Weapons` не входят во владение. Сейв-мост и blast-слой
  подключаются отдельно: `GameState::apply_to_player/apply_to_doors` и
  `apply_projectile_blast(last_projectile_results(), …)`.
* AI — существующий scaffold (`docs/AI_CPP.md`), а не порт оригинального AI;
  `sync_vitals()` — единственная связь с вещами.
* `CollisionWorld` строится из `.sbw` как value; геометрия не перестраивается
  при движении дверей (как и в остальном C++-слое).
* События дверей (`DoorOpening/…`) остаются frame-local логом `WorldState` и в
  `EventRuntime` не попадают — как и раньше; в очередь идут только
  `SwitchPressed`.
* Позиции source-relative вещей в этом слое не разрешаются (нет transform),
  поэтому они не цели proximity-таблицы снарядов.
* `reset()` не трогает `config()`, но стирает текст-реестр и таблицу свитчей.
* Точный порядок фаз — контракт этого слоя, а не доказанный порядок оригинала;
  он выбран так, чтобы события каждого тика ссылались на состояние того же
  тика, а хвост свитчей не терялся (см. §4).
