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
| владеет | `WorldState`, `Things`, `Ai`, `Projectiles`, `EventRuntime`, `CollisionWorld`, точки глаз и виталы игроков |
| конфиг | `GameRuntimeConfig{ AiConfig, ThingsOptions, build_ai, install_switches, projectiles_use_things, player_radius, ai_shots_to_projectiles }` |
| шаблон снаряда | `set_ai_projectile_template(Thing)` / `clear_ai_projectile_template()` |
| игроки | `set_player_eyes(span)` + `set_player_vitals(span)` (размеры совпадают), `player_vitals()` |
| тик | 91 Гц, `tick(n)` = ровно `n` проходов тела |
| порядок | вещи → AI → AI-выстрелы (opt-in) → снаряды → двери/мир → события |
| blast | `apply_projectile_blasts(weapon, …)` — явный мост, не фаза тика; идемпотентен на генерацию `tick_results`; `apply_projectile_blasts_to_players()` — то же по собственным виталам |
| события | очередь опустошается в том же тике, повторного dispatch нет |
| снаряды | один `Projectiles::tick_results(1, …)`; записи отдаются как есть |
| ввод | `press_switch()` (не `world().interact()` напрямую) |
| сброс | `reset()` (всё, кроме конфига и шаблона), `clear_events()`, `clear_projectiles()` |
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
runtime.set_player_vitals(gauges); // те же 0..8 записей Vitals, см. §7
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

Шаблон снаряда для AI-моста — не поле конфига, а значение рантайма:
`set_ai_projectile_template(Thing)`, см. §6.

Виталы игроков (`Vitals`, единицы сейва — сотые) рантайм хранит рядом с точками
глаз: `set_player_vitals(span)` принимает ровно столько записей, сколько точек в
`set_player_eyes()`, иначе возвращает `false`, ничего не записав. Смена числа
точек сбрасывает виталы (они привязаны к индексам списка), повторная установка
того же числа их сохраняет; `clear_players()` и `reset()` очищают и точки, и
виталы. Пустой `player_vitals()` — «гейджи неизвестны»: blast считает попадание,
но урон не применяет (правило короткого спана в `apply_blast_damage()`).
Аксессор `player_vitals()` отдаёт спаны (const и mutable): менять содержимое
можно, размер при этом остаётся привязан к списку игроков.

---

## 2. Порядок тика

`tick(n)` вызывает одно и то же тело `n` раз. Внутри ровно один проход по
каждой подсистеме:

1. **вещи** — `Things::tick(1, &queue)`: возраст, `+0x1e8` lifetime, событие
   `+0x1ec` один раз на вещь;
2. **AI** — `Ai::sync_vitals()` (вещь, убитая уроном, умирает здесь), затем
   `Ai::tick(collision, 1/91 s)`;
2b. **AI-выстрелы** (opt-in, §6) — при `config.ai_shots_to_projectiles` и
   заданном шаблоне очередь `Ai::take_weapon_shots()` дренируется и каждый
   принятый выстрел спавнится в `Projectiles`; события создания идут в ту же
   очередь;
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

Blast/урон в тело тика не входит: оружие снаряда выбирает вызывающий, поэтому
это отдельный явный вызов `apply_projectile_blasts()` (§7).

Детерминизм: все подсистемы обходят собственные векторы в порядке вставки, RNG
и чтения часов нет, `std::map` не итерируется. Два одинаковых билда + одинаковая
последовательность вызовов дают одинаковый поток состояний (тест §9).

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
* `apply_projectile_blasts()` — не аксессор, а явный мост снаряды → blast; его
  результат `ProjectileBlastBatch` действителен сразу после вызова, отметки
  «уже взорвано» живут до следующего `tick()`/сброса (§7).
* `player_vitals()` — принадлежащие рантайму гейджи игроков, индекс в индекс с
  `set_player_eyes()`; спаны живут до следующей установки игроков/виталов,
  `clear_players()` или `reset()`.

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
  спаны `projectile_step` трактует как «нет данных»; виталы игроков в
  `ProjectileTargets` не входят (это цель blast-моста, §7);
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

## 6. AI → снаряды (opt-in)

По умолчанию мост выключен: `GameRuntimeConfig::ai_shots_to_projectiles=false`
не спавнит ничего и **не трогает** очередь `Ai::weapon_shots()` — она остаётся
данными для вызывающего (как и в `Ai` без рантайма). Очередь наполняется только
при `config.ai.use_weapon_shots` (а он требует `ai.use_weapon_table`).

```cpp
gm::GameRuntimeConfig config;
config.ai.use_weapon_table = true;
config.ai.use_weapon_shots = true;
config.ai.weapon_shot_speed = 910.0;    // units/second, /91 в тик
config.ai_shots_to_projectiles = true;

gm::GameRuntime runtime(config);
runtime.set_ai_projectile_template(projectile_thing); // THING_TYPES, movement_type 6
```

Шаблон — значение внутри рантайма (`std::optional<Thing>`), глобального
состояния нет. В тике он применяется так:

1. после AI-фазы `Ai::take_weapon_shots()` опустошается;
2. на каждый выстрел `ai_shot_projectile_spawn(shot, &copy, -1, spawn)`
   строит `ProjectileSpawn` из копии шаблона с уже назначенным runtime-`id`
   (скорость — `weapon_shot_speed`, иначе `Thing::speed * 91`;
   гравитация/дамп/форма/радиус/hits/lifetime/события — из шаблона);
3. `Projectiles::spawn(spawn, &queue)` постит create-событие шаблона один раз;
4. шаг снарядов того же тика уже двигает новый снаряд, поэтому он сразу виден
   в `last_projectile_results()`.

Выстрелы без пригодной скорости (0 и у выстрела, и у шаблона) отбрасываются —
ровно как в `spawn_ai_shots()`. Если флаг включён, а шаблон не задан, очередь
**не** дренируется: выстрел не теряется и уедет на следующем тике после
`set_ai_projectile_template()`.

Мост передаёт `owner_player = -1` и `level_gravity = 0`: у рантайма нет ни
владельца-игрока для AI-выстрела, ни `SET_LEVEL_GRAVITY` (в срезе гравитация
уровня не хранится); вертикаль задаёт `Thing::gravity` шаблона. `owner_thing`
снаряда — `AiWeaponShot::thing_id` стрелка.

### `thing_id` шаблона

`spawn_ai_shots()` подставляет один и тот же `Thing::id` во все снаряды, а
`Projectiles::spawn()` отвергает дубликат живого `thing_id` — с фиксированным
шаблоном живым мог быть только один снаряд. Рантайм поэтому вызывает
`ai_shot_projectile_spawn()` сам и выдаёт каждому снаряду **свой runtime-id**:

* значения начинаются с `kAiProjectileRuntimeIdBase = 0x10000` и растут,
  поэтому не пересекаются с малыми id вещей (0..n-1);
* `Thing::id` шаблона не используется (он остаётся в шаблоне как есть);
* семантика `Projectiles::spawn()` не менялась: уникальные id → несколько
  живых снарядов; мёртвый слот по-прежнему переиспользуется по тому же id;
* `reset()` возвращает счётчик к базе вместе с опустошением флота; шаблон,
  как и `config()`, при `reset()` сохраняется.

Ограничение: id живут в отдельном пространстве от `Things` — это не вещи,
`things().by_id()` их не найдёт; `Projectiles::by_thing_id()` работает.

---

## 7. Снаряды → blast (урон)

`GameRuntime` не выбирает оружие и не вызывает blast из `tick()`: какой
`Weapon` соответствует снаряду, знает вызывающий. Мост поэтому явный и
идемпотентный на генерацию `tick_results`:

```cpp
std::vector<gm::Vitals> players(1);   // индекс = порядок set_player_eyes()
players[0].armor = players[0].armor_max = 10000;

std::vector<gm::ThingVitals> things;  // индекс = things().things()
for (const gm::Thing& thing : runtime.things().things()) {
    things.push_back(gm::thing_vitals(thing));
}

gm::ProjectileBlastContext context;   // underwater/from_ground/deathmatch,
                                      // source ids, power_up, allow_proximity
const gm::ProjectileBlastBatch batch = runtime.apply_projectile_blasts(
    weapon, context, players, things); // only_new = true по умолчанию

for (std::size_t i = 0; i < things.size(); ++i) {
    gm::store_thing_vitals(runtime.things().things()[i], things[i]);
}
```

Если виталы игроков принадлежат рантайму (§1), хватает короткой формы: она
берёт `player_vitals()` и пишет урон прямо в них.

```cpp
runtime.set_player_eyes(eyes);
gm::Vitals gauges;                 // индекс 0 = первая точка глаз
gauges.armor = gauges.armor_max = 10000;
runtime.set_player_vitals(std::span<const gm::Vitals>(&gauges, 1));

const gm::ProjectileBlastBatch batch =
    runtime.apply_projectile_blasts_to_players(weapon, context /*, thing_vitals */);
// новые значения — в runtime.player_vitals()
```

Виталы вещей по-прежнему не принадлежат рантайму: `thing_vitals` остаётся
необязательным параметром (пустой спан — попадания считаются, урон по вещам
пропускается), и после вызова нужен `store_thing_vitals()`. Оба входа
(`apply_projectile_blasts()` и `apply_projectile_blasts_to_players()`) делят
одни и те же отметки генерации, поэтому смешивать их безопасно: запись
взрывается не более одного раза до следующего `tick()`.

Что делает вызов:

1. идёт по `last_projectile_results()` (только последний шаг `tick()`);
2. для `bounced` и `expired` (кроме `already_spent`) вызывает
   `apply_projectile_blast(weapon, projectile_result_of(record), expired, …)`:
   bounce → `Weapon::impact_blasts`, expiry → `Weapon::expire_blasts`;
   `proximity` — только при `context.allow_proximity`; `flying` и `destroyed`
   не бьют никогда;
3. цели берёт из рантайма: `player_positions` — точки `set_player_eyes()`,
   `thing_positions`/`thing_alive` — `things().things()` по индексу
   (source-relative вещь без мировой точки помечается «не живой»);
   `player_vitals[i]`/`thing_vitals[i]` должны описывать те же индексы (у
   короткой формы `player_vitals` — собственные виталы рантайма). Если
   спана vitals короче спана позиций, попадание считается, но урон по этому
   индексу пропускается (правило `apply_blast_damage()`);
4. каждое попадание с ненулевым `BlastHit::event` постит type-3 запись в
   очередь `events()` в центре blast (`extra = 0`); записи уедут в
   `last_events()` на следующем тике, как и любая другая запись очереди;
5. возвращает `ProjectileBlastBatch`: `damage` — объединённые
   `BlastDamageResult` в порядке записей, `blasts` — сколько blast реально
   применено (trigger, даже если в радиусе никого), `already_applied` —
   сколько записей пропущено из-за отметки, `posted_events` — сколько
   type-3 записей приняла очередь.

Blast пишет только в переданные спаны vitals: `Things`/`Ai` увидят новый
`health` после `store_thing_vitals()` (AI читает вещи в `sync_vitals()`).

Идемпотентность: у каждой записи текущей генерации есть отметка
`blasted_results_`. Она сбрасывается на новом `tick_results()` (то есть на
каждом шаге `tick()`), в `reset()`, `clear_events()` и `clear_projectiles()`.
Поэтому:

* повторный вызов без нового тика урон не дублирует (`blasts == 0`,
  `already_applied == числу trigger-записей`);
* новый bounce/expire на новом тике — новая генерация и снова ровно одно
  попадание;
* `already_spent` (повтор уже отработанного expiry) не бьёт никогда;
* `only_new = false` заново применяет текущую генерацию (например, если
  вызывающий поменял спаны vitals), но `already_spent` по-прежнему пропускает.

Ограничения моста: это чистая сфера `apply_blast` без проверки геометрии и
линии видимости; события blast'а постятся в центре взрыва (координаты цели в
оригинальном пути не доказаны); оружие и `Thing→Weapon`-связь остаются за
вызывающим.

---

## 8. Сброс

* `reset()` — пустые мир/вещи/AI/снаряды/события/коллизия, нет игроков
  (точки глаз и виталы), `clock() == 0`; `config()` не меняется. Используется
  тестом детерминизма как «второй прогон с нуля».
* `clear_events()` — очередь, лог свитчей мира, лог AI и last-результаты;
  реестр текста (у него свои дедлайны по кадрам) остаётся. Часы не сбрасываются.
* `clear_projectiles()` — флот снарядов и `last_projectile_results()`.
  Обе очистки (и `reset()`) сбрасывают и отметки «уже взорвано» из §7.

---

## 9. Тесты

`gm_game_state_tests` (тот же файл `engine/tests/test_game_state.cpp`,
`engine/tests/CMakeLists.txt` не менялся):

| тест | что закрепляет |
|---|---|
| `test_runtime_events_not_duplicated` | вещь с `+0x1e8 = 1` постит `+0x1ec` один раз; второй тик не даёт `generated`, очередь пуста, `age`/`expired` корректны; `tick(0)` — no-op |
| `test_runtime_projectile_results` | `life_ticks = 1` → `expired` + одно событие в записи и в dispatch; второй тик — `already_spent` без событий; `remove_projectile` → `destroyed`/`dead`, слот остаётся |
| `test_runtime_ai_shots_default_off` | `ai_shots_to_projectiles=false`: AI ставит выстрел в очередь, но снарядов и событий нет, очередь не выедена |
| `test_runtime_ai_shots_to_projectiles` | opt-in: ровно один снаряд на weapon-event (910 ед/с → 10 ед/тик), виден в `last_projectile_results()` с runtime-id ≥ `0x10000`; повторный тик без события ничего не плодит и не дублирует create-событие; второй выстрел через cooldown 91 даёт второй живой снаряд с другим id |
| `test_runtime_ai_shots_without_template` | флаг включён, шаблона нет: очередь не дренируется и не теряется — после `set_ai_projectile_template()` следующий тик спавнит ровно один снаряд; `clear_ai_projectile_template()` снова выключает мост |
| `test_runtime_pending_switch_tail` | 4300 нажатий переполняют очередь (0x7526 / 7 байт); тик 1 отдаёт часть, тик 2 — остаток, сумма ровно 4300, третий тик пуст |
| `test_runtime_projectile_blasts_expire` | expire-снаряд в центре игрока и вещи: один blast (`expire_blasts`, hull 2000 → armor/hp 8000), 2 попадания, 2 type-3 записи в очереди; повторный вызов без тика — `blasts == 0`, `already_applied == 1`, урон не меняется; на следующем тике события уезжают в `generated` ровно один раз, а `already_spent`-повтор expiry не бьёт |
| `test_runtime_projectile_blasts_bounce` | пол из двух треугольников: bounce бьёт по `impact_blasts` (hull 1000 у игрока, 400 у вещи на d=30 при радиусе 50), `flying` между bounce'ами не бьёт, второй bounce нового тика бьёт ещё раз, повтор вызова — 0 |
| `test_runtime_player_vitals` | собственные виталы: размер не равен списку глаз → `false` без записи; blast через `apply_projectile_blasts_to_players()` снимает 100 щита (400 plate >> 2) и 1300 брони (1000 hull + 300 пролитого plate), повтор без тика — 0, смешанный вызов с явным спаном тоже 0; смена числа глаз сбрасывает виталы, `clear_players()`/`reset()` очищают; без виталов попадание считается, урон не применяется |
| `test_runtime_determinism` | 12 тиков с дверью, вещами, AI и снарядом: хеши состояний двух рантаймов совпадают, `reset()` + повторная сборка дают тот же поток; дверь за 12 тиков уходит на `phase = 18000` |

Фикстуры синтетические (SWITCH/DOOR-записи + `.sbw` из одного объекта или пола
из двух треугольников), поэтому тесты не зависят от установленной игры. Реальные
level0/dm1 по-прежнему покрываются `doors`, `things`, `thing_ai`, `game_state`
(сейвы).

Сборка:

```sh
cmake --build build/dev --target gm_game_state_tests
ctest --test-dir build/dev -R '^game_state$' --output-on-failure
```

---

## 10. Границы и что не доказано

* `main.cpp` не подключён: срез не рендерит, не читает ввод, не играет звук.
* `GameState` и `Weapons` не входят во владение. Сейв-мост и blast-слой
  подключаются отдельно: `GameState::apply_to_player/apply_to_doors` и
  `apply_projectile_blasts(weapon, …)` (§7).
* AI — существующий scaffold (`docs/AI_CPP.md`), а не порт оригинального AI;
  `sync_vitals()` — единственная связь с вещами.
* Мост AI→снаряды — механическое соединение очереди `AiWeaponShot` с
  `Projectiles` (§6), а не доказанный оригинальный путь `0x46ea40`; сам он
  оружие не выбирает, поэтому урон появится, только если вызывающий применит
  `apply_projectile_blasts()` с нужным `Weapon`.
* `CollisionWorld` строится из `.sbw` как value; геометрия не перестраивается
  при движении дверей (как и в остальном C++-слое).
* События дверей (`DoorOpening/…`) остаются frame-local логом `WorldState` и в
  `EventRuntime` не попадают — как и раньше; в очередь идут только
  `SwitchPressed`.
* Позиции source-relative вещей в этом слое не разрешаются (нет transform),
  поэтому они не цели proximity-таблицы снарядов.
* Blast-мост (§7) — чистая сферическая проверка `apply_blast` без LOS и без
  геометрии; он пишет только в переданные спаны vitals (вещи — через
  `store_thing_vitals()`, игроки — либо в собственные `player_vitals_`, либо в
  спан вызывающего), а события попаданий постятся в центре взрыва,
  потому что координаты цели в оригинальном пути не доказаны.
* `player_vitals_` — только цель blast-моста: `Ai` их не читает (у scaffold нет
  гейджей игроков), в сейв/`GameState` они не переносятся (этот слой не
  владеет `GameState`), а смена числа точек глаз сбрасывает их, чтобы индексы
  не разъехались.
* `reset()` не трогает `config()` и шаблон снаряда, но стирает текст-реестр,
  таблицу свитчей и счётчик runtime-id снарядов.
* Точный порядок фаз — контракт этого слоя, а не доказанный порядок оригинала;
  он выбран так, чтобы события каждого тика ссылались на состояние того же
  тика, а хвост свитчей не терялся (см. §4).
