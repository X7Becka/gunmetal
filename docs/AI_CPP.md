# Gunmetal — минимальный C++ AI вещей (`Ai`)

Что здесь: `engine/src/world/ai.{h,cpp}` — детерминированная заготовка
state machine для runtime-вещей `Things`. Модуль **не порт** оригинального AI:
нет pathfinding, анимации, звука, урона игроку и рендера/SDL. Он берёт уже
известную абсолютную позицию вещи, считает расстояние до единственного глаза
игрока, проверяет line-of-sight лучом через `render/collide.h` и переключает
состояния на фиксированном тике **91 Гц**. Opt-in `use_move_step` (§7)
подключает один A2-шаг к `aim_point`/`home` и A3-окно `standoff`
(подойти / стоять и стрелять / отойти), opt-in `use_path` (§8) ведёт
`patrol` по A4-маршруту (`path`/`pathloop`/`pathexpire`).

Статус: API, состояния, LOS, таймеры, leash/return, synthetic- и
поставочные golden'ы, 240-слотовая история позиции цели с лагом
`Thing::target_history_offset` и зеркало виталов вещи (`sync_vitals`: HP,
щиты, счётчик попаданий; мёртвая вещь убивает агента) готовы. CTest
`thing_ai` (quick, **0.39 с**) проходит, `python3 regress.py` — **113/113**
(скрипт не менялся). Игра только читается; `Things` AI не мутирует.
Навигация — A2 (один waypoint, столкновения, leash) плюс A3
(`standoff`-окно: seek / hold+fire / retreat, §7) и opt-in A4
(`patrol` по списку точек, §8); `pathrandom`, реального оружия и решений
по истории по-прежнему нет, см. §6.

---

## 0. Коротко

| что | значение |
|---|---|
| тик | `kAiTickHz = 91`, `kAiTickSeconds = 1/91` |
| состояния | `idle`, `patrol`, `alert`, `attack`, `dead` |
| вход | `Things` (только чтение), один eye игрока, `CollisionWorld` |
| LOS | `CollisionWorld::raycast` от eye агента к eye игрока |
| позиции | `Thing::position` (абсолютные варианты `ADD_THING*`) |
| порядок | порядок `Things::things()`, без rand/времени/map-обхода |
| `dead` | не тикает: ни таймеров, ни LOS, ни событий |
| dm1 | attached **5** из 141, первый id **115** `(-350, 1304, 40)` |
| level0 | attached **6** из 121, первый id **115** `(24, 2708, -92)` |
| движение | default: только return к `home`; с `use_move_step=true` alert/attack идут к `aim_point`, patrol/return — к `home` через A2 (`world/ai_move.*`); внутри `standoff` стоят и стреляют, внутри `standoff-60` пятятся (A3); с `use_path=true` patrol идёт по `path_waypoints` (`loop`/`path`/`expire`, §8), приоритетнее шага домой |
| урон | `damage()` меняет только AI-агента, не `Things`; `sync_vitals()` читает виталы вещи (щиты, hits, HP) и убивает агента вместе с вещью |
| история цели | кольцо **240** позиций; `tick` пишет один слот, лаг 0..239 |
| лаг из данных | `Thing::target_history_offset` (+0x1d0): 0 = текущая, иначе 1..239 |

---

## 1. API

```cpp
#include "world/ai.h"

gm::Ai ai;                       // AiConfig по умолчанию
const std::size_t n = ai.attach(things); // executed view, только absolute
ai.set_player(eye);              // один eye; старый API
ai.set_players(eyes);            // 1..8 eye, как скан 0x473a00
ai.tick(world);                  // ровно один тик 1/91 с
ai.tick(world, dt);              // dt зажимается до config().max_dt
ai.damage(id, 25.0);             // true, если агент жив и урон принят
ai.place(id, {x, y, z});         // только тест/отладка, без навигации
ai.set_path(id, waypoints);      // A4: маршрут patrol (до 32 точек)
ai.sync_vitals();                // зеркалить здоровье/щиты/hits вещи в агента

const auto& agents = ai.agents();
const gm::AiAgent* a = ai.agent(id);
const auto& stats = ai.stats();
const auto& events = ai.events(); // накопительный журнал

// История позиции цели (240 слотов, один write на принятый tick).
const gm::TargetHistory& history = ai.target_history();
gm::Vec3d p{};
ai.target_position(0, p);        // 0 = текущая позиция player 0
ai.target_position(5, p);        // 5 тиков назад; false, если истории меньше
ai.target_position(1, 5, p);     // player 1, 5 записей назад
ai.reset_target_history();       // ручной сброс
ai.record_target_position(p);    // ручная запись (тест/отладка), NaN/inf → false
```

### 1.1. `AiAgent`

```cpp
struct AiAgent {
    int thing_id;       // Thing::id из Things
    int thing_type;     // Thing::type, -1 = нет записи
    int movement_type;  // 0..18, данные; навигации пока нет
    double search_radius; // эффективный радиус обнаружения (строго <)
    int target_zone;    // эффективный радиус взаимодействия (целый)
    int history_lag;    // 0..239; 0 = текущая позиция цели
    double max_health;  // эффективный максимум HP
    bool immune;        // max_health == 0
    AiState state;      // idle / patrol / alert / attack / dead
    int target;         // 0 = единственный игрок, -1 = нет
    double alertness;   // 1.0 пока видит, убывает без LOS
    double timer;       // секунды до следующего перехода/атаки
    double leash;       // эффективный радиус (копия AiConfig::leash)
    Vec3d home;         // абсолютная Thing::position, не меняется
    Vec3d position;     // home, пока агент не ушёл в return
    double health;
    std::array<int, 4> shields;  // зеркало щитов вещи (сотые)
    int hits_left;               // зеркало счётчика попаданий вещи (+0x1c8)
    double distance;    // последняя дистанция eye-to-eye
    bool visible;       // последний LOS
    bool returning;     // idle-подсостояние «идёт домой»
    bool has_home;

    // A2-движение; используется только при AiConfig::use_move_step.
    double speed;          // +0x10, единиц за тик 91 Гц
    double radius;         // радиус swept-сферы (Thing::radius)
    double arrive_radius;  // +0x70, октогональная дистанция прибытия
    double standoff;       // +0x1cc: окно A3, 0 = выключено
    Vec3d leash_anchor;    // центр жёсткого leash, после attach = home
    double leash_distance; // +0xd0, 0 = кламп выключен
    double heading;        // atan2 последнего move_delta, рад
    double omega;          // зарезервировано под интегратор поворота (A6)
    Vec3d move_delta;      // перемещение, применённое за последний тик

    // A4-path — маршрут patrol; читается при AiConfig::use_path.
    std::vector<Vec3d> path_waypoints; // +0x324[], до 32 точек (set_path)
    AiPathState path;                  // +0x4ec index / +0x4f4 waiting
};
```

`target` — не id вещи, а слот игрока: в этом срезе игрок один (`0`).
`alertness` — не «очки подозрения» оригинала, а простой множитель 0..1:
видит → 1.0, не видит → линейно к 0 за `lose_sight_seconds`.

### 1.2. `AiStats` и `AiEvent`

`AiStats` — счётчики на текущий момент: `attached`, `skipped_without_position`,
`idle/patrol/alert/attack/dead`, `ticks`, `los_checks`, `los_blocked`,
`transitions`, `attacks`, `damage_events`, `deaths`, `ignored_ticks`,
`clamped_ticks`, `invalid_damage`, `typed_agents`, `immune_hits`,
`vitals_syncs` (вызовы `sync_vitals`), `thing_deaths` (агенты, убитые
виталями вещи), `move_steps` (A2-шаги, изменившие `position`),
`blocked_steps` (шаги, урезанные стеной), `leash_clamps` (шаги, отменённые
жёстким leash), `standoff_holds` (тики A3-стояния в окне),
`retreat_steps` (A3-шаги назад) и `path_expiries` (агенты, завершённые
`pathexpire`, — один инкремент на агента).

`events()` возвращает упорядоченный журнал `AiEvent { type, thing_id, from, to,
target, target_thing, aim, value, tick }`; `tick` 1-based, у `attach` — 0. Типы: `attach`, `sight`,
`lost_sight`, `state`, `attack`, `damage`, `death`, `leash`, `arrived`.
`clear_events()` очищает только журнал, не состояние.

### 1.3. История позиции цели
`TargetHistory` — фиксированное кольцо `slots[240]`, `next` (следующий слот,
аналог `history_index` +0x3204) и `count` (0..240). `Ai::tick` после проверки
`dt` пишет ровно одну точку — eye игрока; без игрока записи нет. `attach()` и
`clear_player()` сбрасывают кольцо.

* `TargetHistory::at(lag, out)` — `lag` зажимается в 0..239; 0 = самая свежая
  запись, 1 = предыдущий tick. `false`, если записей меньше `lag + 1`.
* `Ai::target_position(lag, out)` — player 0: для `lag == 0` возвращает
  **живой** eye (как оригинал, где 0 = текущая позиция), иначе читает кольцо.
  Перегрузка `target_position(player, lag, out)` делает то же для слота
  0..7; без живого игрока `lag 0` отдаёт свежую запись, пустое кольцо →
  `false`.
* `attach(const Things&)` хранит указатель на `Things` для `sync_vitals`;
  rvalue-перегрузка удалена, чтобы временный объект не повис на следующем
  тике. Владелец `Things` должен жить не меньше `Ai`.
* `set_players(span)` принимает 1..8 finite eye; пустой/9/NaN отклоняются
  без изменения прежнего набора. `tick` пишет по одной точке в кольцо
  каждого игрока; `AiAgent::target` — индекс ближайшего видимого игрока.
  Если видимого игрока нет, `tick` сканирует абсолютные живые вещи (кроме
  себя и уже истёкших `expired`) и выбирает ближайшую: `target_thing` = `Thing::id`,
  `target_is_thing=true` (оригинал `0x473a00` сканирует игроков 0..7,
  затем вещи 8..0x10b). У вещи нет кольца истории, поэтому её
  `aim_point` — текущая мировая позиция.
* `Ai::sync_vitals()` зеркалит не только HP/щиты/hits, но и
  `burn_timer`/`status_flags` (те же поля, что пишет
  `apply_thing_damage(Thing&)`); агент видит поджог/заморозку на следующем
  тике.
* `AiAgent::history_lag` — результат `ai_history_lag(+0x1d0)`: 0 остаётся 0,
  положительное значение зажимается в 1..239. `tick()` пишет `aim_point` =
  `target_position(history_lag)`; `0` — живой eye, положительный лаг — точка
  из кольца (при нехватке истории — живой eye). `distance`/LOS/зоны по-прежнему
  считаются по живому глазу; `aim_point` — то, куда должен стрелять игровой
  слой. См. §6.2.

### 1.4. Витали вещи → агент (`sync_vitals`)

`Ai::attach` запоминает `Things` (только на чтение). `Ai::sync_vitals()`
зеркалит в агента то, что пишет runtime-путь урона:

| поле агента | источник |
|---|---|
| `shields[4]` | `Thing::shields` (+0x1d8…+0x1e4) |
| `hits_left` | `Thing::hits_left` (+0x1c8) |
| `health` | `Thing::health` (+0x1d4, текущее значение) — только при `use_thing_fields` |

Вещь, которая мертва (`alive == false`) или у которой здоровье дошло до нуля
(и она не `immune`), переводит своего агента в `AiState::dead`. `tick()`
вызывает `sync_vitals()` до состоянийной машины, поэтому урон, нанесённый
оружием (`apply_thing_damage` → `Things`), виден агентам на следующем тике.
AI **не меняет** `Things` — только читает. Счётчики: `vitals_syncs`,
`thing_deaths`.

---

## 2. Состояния и переходы

```
attach (absolute, alive)
        │
        ▼
      idle ──(timer 2 s, нет цели)──► patrol
        │                              │
        │◄──────── lost_sight ─────────┤
        │                              │
        └──────── sight ──────────────►alert
                                       │  timer 0.5 s + dist ≤ 96
                                       ▼
                                     attack
                                       │  dist > 120 (1.25×96)
                                       └──────► alert

alert/attack + |position − home| > leash
        └──► idle (returning) ── дошёл ──► patrol
```

* **idle** — стоит в `home`, ждёт `patrol_after_seconds`; при LOS сразу
  переходит в `alert`.
* **patrol** — «маршрут не задан»: позиция та же, сканирует; при LOS → `alert`.
  Реального патрулирования/waypoint'ов нет. С `use_move_step=true` patrol
  шагает к `home` (то есть возвращается, если его оттуда сдвинули).
* **alert** — цель захвачена. `timer` 0.5 с; если цель видна и ближе
  `attack_range`, переход в `attack`. Без LOS `alertness` падает 3 с, затем
  `patrol` и цель сбрасывается. С `use_move_step=true` идёт к `aim_point`,
  а порог перехода в `attack` расширяется до `standoff` (§7, A3).
* **attack** — цель в радиусе; каждые `attack_period` 0.75 с пишется событие
  `attack`. Урона игроку нет — это заготовка. При дистанции > 1.25×range
  возврат в `alert` (гистерезис). С `use_move_step=true`: дальше `standoff`
  идёт к `aim_point`, в окне `standoff` стоит, ближе `standoff − 60` пятится.
  Событие `attack` при A3 пишется только внутри `engage_range`; в зоне
  гистерезиса (`engage_range … 1.25×engage_range`) агент ещё в `attack`,
  но не стреляет.
* **dead** — после `damage()` до `health <= 0`. Таймеры и LOS не считаются;
  `damage()` по мёртвому/неизвестному id возвращает `false`.
* **leash/return** — если агент (например, после `place()`) оказался дальше
  `leash` от `home`, он сбрасывает цель, идёт в `idle(returning)` и движется к
  `home` со скоростью `return_speed`; по прибытии — `patrol` и событие
  `arrived`. Пока идёт return, LOS игнорируется. С `use_move_step=true` тот же
  путь идёт через `ai_seek_step` (`speed`, `arrive_radius`), а прибытие
  притягивает позицию ровно в `home`.

Движение (A2/A3 — только `AiConfig::use_move_step`, см. §7; A4-маршрут —
`use_path`, §8) выполняется **после**
восприятия и смены состояния, поэтому тик, на котором цель захвачена, уже
делает шаг. Евклидов скан цели (`distance`, `search_radius`) не меняется;
октогональная метрика используется только внутри A2/A3-шага. При `false`
`position` по-прежнему меняется только на leash/return, побайтово как раньше.

LOS: луч из `position + (0,0,eye_height)` в eye игрока, `max_t = distance`;
промах или `hit.t >= distance − los_skin` — видно. Стены/пол/потолок
фильтруются настройками `CollisionWorld::raycast` по умолчанию (двусторонние).
Если игрока нет (`clear_player()`), LOS не считается: `distance = 0`,
`visible = false`.

Валидация: `dt` ≤ 0 / NaN / inf игнорируется (`ignored_ticks`); `dt >
max_dt` зажимается (`clamped_ticks`; оригинальный `0x470b20` при выходе за 1..181 тик сбрасывает dt в 1); `set_player`/`place` с NaN/inf
возвращают `false`; `damage` с NaN/0/отрицательным числом отклоняется. В
состоянии нет OOB-индексов: `agent(id)` — линейный поиск, массивы не
адресуются чужими id.

---

## 3. Константы: proven / hypothesis

`AiConfig` намеренно не читает `THING_TYPES`: привязка полей `.soe` к этому
state machine — следующий шаг. Tunables ниже — **гипотезы** для заготовки,
кроме тика и размера/шага кольца истории (они proven по коду и данным).

| константа | значение | статус | почему |
|---|---:|---|---|
| `kAiTickHz` | 91 | **proven** | тот же фиксированный тик, что `Things`/`Doors`/HUD (docs/THINGS_CPP.md, docs/PLAYER_CONTROLLER.md) |
| `sight_range` | 512 | hypothesis | fallback; при `use_thing_fields=true` берётся `Thing::search_radius` (`+0x58`) |
| `attack_range` | 96 | hypothesis | fallback; при `use_thing_fields=true` берётся `Thing::target_zone_threshold` (`+0x1cc`) |
| `leash` | 384 | hypothesis | 0.75 × sight; оригинальный leash не разобран |
| `eye_height` | 10 | hypothesis | LIGHT chassis = 10.0 (docs/PLAYER.md §5); у вещей высота глаза не доказана |
| `alert_seconds` | 0.5 | hypothesis | задержка «увидел → атака» |
| `lose_sight_seconds` | 3.0 | hypothesis | время забывания цели |
| `patrol_after_seconds` | 2.0 | hypothesis | idle → patrol без цели |

| `attack_period` | 0.75 | hypothesis | период события атаки; `+0x1d0` — не период, а лаг истории цели, поэтому он не подходит |
| `return_speed` | 200 | hypothesis | скорость возврата, не ориентация движения `+0x1b4` |
| `health` | 100 | hypothesis | fallback; при `use_thing_fields=true` берётся `Thing::max_health` (`+0x1d4`), 0 = иммунитет |
| `use_move_step` | `false` | opt-in | включает A2/A3-движение (§7); `false` сохраняет прежнее поведение байт-в-байт |
| `use_path` | `false` | opt-in | включает A4-маршрут `patrol` (§8); не зависит от `use_move_step`, `false` сохраняет A2/A3 байт-в-байт |
| `path_mode` | `loop` | opt-in | `+0x1b4` типы 9/10/12: `path` (стоять на последней), `loop` (по кругу), `expire` (умереть на последней); читается только при `use_path=true` |
| `move_speed` | 1.0 | fallback | ед./тик; при `use_thing_fields=true` и `Thing::speed > 0` берётся `+0x10` |
| `move_radius` | 0.0 | fallback | радиус swept-сферы; при `use_thing_fields=true` берётся `Thing::radius`; **0 отключает** проверку стены, поэтому для столкновений задайте радиус или включите `use_thing_fields` |
| `move_arrive_radius` | 50 | fallback | `+0x70`; при `use_thing_fields=true` берётся `Thing::arrive_radius` |
| `move_standoff` | 200.0 | fallback (**proven** дефолт данных) | окно A3 `+0x1cc` (поставка: 200); при `use_thing_fields=true` берётся `Thing::target_zone_threshold` |
| `move_standoff_backoff` | 60.0 | **proven** (код `0x473a00`) | полоса `v + 60 <= standoff` → шаг назад |
| `move_leash` | 0.0 | fallback | жёсткий leash `+0xd0`; `0` = выключен. При `use_thing_fields=true` и `Thing::leash_distance > 0` берётся поле |
| `max_dt` | `181/91 ≈ 1.989` | guard | `0x470b20` при выходе суммы за 1..181 тик **сбрасывает её в 1**, а не зажимает; C++-`Ai` использует верхний предел как защиту от огромного dt (`clamped_ticks`), это осознанное отличие |
| `los_skin` | 1e-3 | hypothesis | допуск на конечную точку луча |
| `kAiHistorySlots` | 240 | **proven** (данные+код) | `history[240][3]` в структуре объекта (`+0x30`), `history_index` (`+0x3204`) wrap на `0xf0` — docs/SAVE_FIELDS.md §9.2 |
| `TargetHistory` шаг | 1 запись / tick | **proven** (код) | «позиция пишется каждый апдейт», `0x44377b..0x44378b`, docs/SAVE_FIELDS.md §9.2 |
| clamp `history_lag` | 0 или 1..239 | **proven** (код) | `0x473eed`/`0x476523`, поле +0x1d0, docs/SOE.md; 0 — «текущая позиция». Данные: 0..10 во всём корпусе, у dm1/level0 только 0 (например level1 — 1..2, level3 — 1..10) |
| смысл `lag = N` | N записей/тиков назад | **proven** (код) | `FUN_00473a00` (`0x473a00`): `i = history_index; i = (i < lag) ? i - lag + 0xf0 : i - lag; p = &DAT_00819070 + (i + player*0x236)*0x18`; stride `0x18` = 3 double, `0xf0` = 240. `+0x1d0` — ровно число записей назад, C++ хранит 1 запись/тик. |

`Thing`/`ThingTypeEvents` уже несут `turn_accel` (`+0x20`), `turn_max`
(`+0x28`), `arrive_radius` (`+0x70`), `leash_distance` (`+0xd0`),
`target_zone_threshold` (`+0x1cc`), `radius`, `speed` (`+0x10`).
`octagonal_distance()` повторяет метрику `0x4344df` (`max + 0.25*(mid+min)`,
тест `test_octagonal_distance`); `world/ai_move.*` (A2/A3) даёт чистые
`ai_seek_step()` (один waypoint, `arrive_radius` по октогональной метрике, шаг
`speed_per_tick*91*dt`, swept-сфера радиуса `Thing::radius`) и
`ai_retreat_step()` (шаг строго от цели с той же swept-сферой, направление
планарное). С opt-in `use_move_step` (§7) их вызывает `Ai::tick`; евклидов
скан цели и LOS при этом не меняются.

`AiConfig::use_thing_fields` (по умолчанию `false`) включает реальные поля:
`search_radius → sight`, `target_zone_threshold → attack zone`,
`max_health → health/immune`, `movement_type` и `target_history_offset →
history_lag` (через `ai_history_lag`, clamp 0/1..239) — данные агента. `false`
сохраняет прежние fallback-константы, `history_lag = 0` и старые goldens;
синтетический тест с нулевой записью типа поэтому не ломается. Значения в
`.sbt` уже масштабированы, повторно их умножать нельзя. `search_radius`
используется строгим `<`, как `FCOMP`; `max_health == 0` отклоняет урон
(`immune_hits`).

---

## 4. Golden'ы

### 4.1. Синтетика (`engine/tests/test_ai.cpp`)

SBT/SCRIPT: два абсолютных `ADD_THING` в `(0,0,0)` и `(1000,0,0)` и один
`ADD_THING_AT_SOURCE` (без позиции). `ThingsOptions::contexts = {{0,0}}`.

| проверка | golden |
|---|---|
| `attach` | 2 агента, 1 skipped (source-relative) |
| idle → alert | 1 тик, игрок `(0,60,10)`, `distance=60`, `timer=0.5` |
| alert → attack | ещё 46 тиков (0.5 с × 91), `timer=0.75` |
| attack events | ещё 69 тиков → ≥1 событие `attack` |
| LOS-стена | стена `y=100`, игрок `(0,200,10)` → idle, `los_blocked=1` |
| потеря LOS | 274 тика за стеной → `patrol`, `target=-1`, `alertness=0` |
| leash | `place(0, (1000,0,0))`, leash 384 → `idle(returning)`, шаг к дому |
| return | < 1000 тиков → `position == home`, `patrol`, событие `arrived` |
| смерть | `damage(0,1000)` → `dead`, `health=0`; 300 тиков без событий |
| негативы | unknown id, −5, 0, NaN → `false`, `invalid_damage=5` |
| dt | `1e9` → 1 тик, `clamped=1`; `0/-1/NaN` → `ignored=3` |
| история: пусто | `attach` → 0 записей; `target_position(0/1)` → `false`; tick без игрока не пишет |
| история: 3 точки | eye A/B/C на трёх тиках → `size=3`; lag 0 = C, 1 = B, 2 = A, 3 → `false` |
| история: clamp | `ai_history_lag(-5/0/1/10/240/1e6)` → `0/0/1/10/239/239`; `target_position(-5)` = текущая, `(240)` = лаг 239; NaN-запись отклонена |
| история: кольцо | 240 записей → `size=240`, lag 239 = самая старая, `attach`/`clear_player` очищают |
| история: поля типа | `use_thing_fields` + `+0x1d0` = 3/500/−4 → `history_lag` 3/239/0; без opt-in → 0 |
| история: прицел | лаг 3 после 5 тиков (x=1..5) → `aim_point.x=2`, `aim_distance=2`; без игрока `has_aim=false` |
| событие attack | несёт `target`, `target_thing` и `aim_point`; проверено на игроке `(0,60,10)` |
| несколько игроков | `set_players` (2 eye, dist 60/30) → `target=1`, `distance=30`, у player 1 своё кольцо; пустой набор отклонён |
| вещь как цель | после `clear_player` агент видит вторую вещь → `target_thing=1`, `distance≈1000.05` (eye z=10), `aim_point=(1000,0,0)` |
| статусы вещи | `burn_timer=91`, `status_flags=0x40` в `Thing` → те же поля у агента после `sync_vitals` |
| два прогона | `same_agent` для всех полей и совпадение журнала на 300 тиках; отдельный прогон сравнивает кольцо истории |
| A2: opt-in | `use_move_step=false` на 200 тиках: `position == home`, `move_steps=blocked_steps=leash_clamps=0` |
| A2: seek | `use_move_step=true`, speed 2/тик, игрок `(0,300,0)`: 1-й тик `position.y=2`, `heading=π/2`; 200 тиков → `y=296` (`arrive_radius=5`), второй прогон идентичен |
| A2: стена | низкая стена `y=100`, radius 10 → `position.y=90`, `blocked_steps>0` (тело упирается, eye-луч видит цель) |
| A2: leash | `move_leash=100`, цель `(0,500,0)` → позиция замирает на `y=100`, `move_steps=50`, `leash_clamps>0` |
| A2: return | `place(0,(1000,0,0))` → `returning`, затем `position==home`, `patrol`, `arrived`; шаги через `ai_seek_step` |
| A2: standoff | `move_standoff=100`, цель `(0,150,0)` → стоп на `y=50`, `move_steps=25`, 100 лишних тиков не двигают |
| A3: далеко | `move_standoff=200`, цель `(0,500,0)`, speed 2 → стоп на `y=300`, `move_steps=150`, `state=attack`, `attacks>0` |
| A3: окно | цель `(0,150,0)` → `position=(0,0,0)`, `standoff_holds=200`, `move_steps=retreat_steps=0`, есть событие `attack` |
| A3: отход | цель `(0,100,0)` → пятится до `y=-42` (`d=142`), `retreat_steps=21`, затем стоит |
| A3: отход+стена | стена `y=-50`, radius 10 → `y=-40`, `blocked_steps>0`, сквозь стену не проходит |
| A3: opt-in | `AiConfig{}`: `use_move_step=false`, `move_standoff=200`, `backoff=60`; игрок в окне 200 тиков — позиция не меняется |
| A4: opt-in | маршрут `(40,0)/(40,40)` при `use_path=false` → `path.index=-1`, `position==home`, `move_steps=0`; при `true` patrol идёт по точкам; `set_path(99)`/NaN → `false`, 40 точек → 32, курсор сброшен |
| A4: loop | 4 точки `(40,0)→(40,40)→(0,40)→(0,0)`, `use_move_step=false` → прибытия `0,1,2,3,0`, `move_steps>0` |
| A4: path | 3 точки `10/20/30` → стоп на `(≈30,0)`, `index=2`, 200 лишних тиков не двигают |
| A4: expire | 2 точки `10/20` → на следующем advance после `(20,0)` `state=dead`, `path.expired`, `path_expiries=1`, один `death`, дальше тишина |
| A4: стена | маршрут `(0,182,0)`, низкая стена `y=100`, radius 10 → `position.y=90`, `path.waiting`, `blocked_steps>0` |
| A4: alert | `use_path=true`, маршрут в `-y`, игрок `(0,300,0)` → `alert`, 1-й тик `y=2`, `path.index=-1` (маршрут не выбран) |

### 4.2. Поставочные уровни

`dm1.sbt` / `level0.sbt`, один контекст `{{0,0}}` (те же 141/121 executed, что
в CTest `things`).

| уровень | executed | attached | skipped | первый агент `id` | `home` |
|---|---:|---:|---:|---:|---|
| dm1 | 141 | **5** | 136 | **115** | `(-350, 1304, 40)` `telrad` |
| level0 | 121 | **6** | 115 | **115** | `(24, 2708, -92)` |

После **100 тиков без игрока** первый агент остаётся `idle`, его
`position`/`home` не меняются, `timer = 2 − 100/91 ≈ 0.9010989010989011`.
Последний absolute на dm1 — id **140** `OILDRUM` в `(5688, 216, 264)`.

Почему attached меньше executed: `Thing::position` есть только у абсолютных
`ADD_THING`/`ADD_THING_EX`; source-relative варианты не имеют мировой точки, и
AI не пытается её вывести (docs/THINGS_CPP.md §1.3).

---

## 5. Доверие (trust)

* Модуль не меняет `Things`, `Doors`, игрока, рендер, аудио, ввод, камеру,
  `gm.py`/`gm/**`, `tools/**`, `regress.py` и чужие docs.
* В `engine/CMakeLists.txt` добавлена одна строка `src/world/ai.cpp`; в
  `engine/tests/CMakeLists.txt` — только цель/тест `thing_ai` (quick).
* Порядок агентов и событий детерминирован: вектор `Things::things()` и
  последовательный обход; нет `unordered_map`, rand, времени и платформенных
  вызовов.
* Кольцо истории — `std::array<Vec3d, 240>` без аллокаций в `tick`; запись и
  чтение зависят только от числа принятых тиков и eye игрока. `clear_player`
  и `attach` сбрасывают кольцо, поэтому прогон детерминирован с нуля.
* LOS использует проверенный `CollisionWorld::raycast` (docs/COLLIDE.md), а не
  собственную геометрию.
* A2/A3-шаг использует проверенный `CollisionWorld::sphere_cast` (и вперёд, и
  назад); при `use_move_step=false` новый код не исполняется и прежние
  golden'ы не меняются (это проверяют `test_move_step_opt_in` и поставочные
  golden'ы).
* CTest `thing_ai` — **0.37 с**, label quick. `regress.py` — **113/113**.

Ограничения доверия: golden'ы AI (радиусы, таймеры, скорости) — это golden'ы
**этой заготовки**, а не оригинала. Они не доказывают поведение `gundx.exe`.

---

## 6. Что осталось

Реализовано: кольцевая история позиции цели на 240 тиков и лаг из
`Thing::target_history_offset` (§1.3) — **код**; сама 240-слотовая раскладка и
clamp 1..239 — **данные+код** (`docs/SAVE_FIELDS.md`, `docs/SOE.md`). Остаётся:

1. **Типы вещей**: `+0x1b4` (movement), `+0x1cc` (attack range), `+0x1c8`
   (hits), `+0x1c4` (collide event), `+0x1ec` (expire event) и `+0x1e8`
   (expire ticks) уже в `AiAgent`; `+0x1d4` (health) и `+0x1d8` (щиты) —
   зеркало виталов (§1.4). Expire-поля уже используются `Things::tick`
   (`expired` + один expire-event); осталось spawn/despawn и удаление.
2. **Принятие решений**: `history_lag` даёт `AiAgent::aim_point`
   (упреждение на N записей), `target` — индекс ближайшего видимого игрока,
   у каждого игрока своё кольцо; вещи сканируются как fallback-цели
   (`target_thing`). Осталось использовать `aim_point` в реальном
   оружии/сглаживании и добавить навигацию к выбранной цели.
3. **Навигация**: A2-шаг (`ai_seek_step`, leash-кламп) и A3 `standoff`-окно
   (`+0x1cc`: подойти / стоять+стрелять / отойти на `standoff-60`) уже
   подключены opt-in'ом (§7); A4 `path_advance` (§8) подключён вторым
   opt-in'ом: `patrol` идёт по `path_waypoints` в режимах
   `path`/`loop`/`expire`, `expire` убивает агента. Остались `pathrandom`,
   waypoint'ы из таблицы локаций, высота/пол через `floor_at` и гравитация.
4. **Бой**: урон игроку, оружие (`+0x21c`), снаряды, звук (`+0x1fc`),
   анимация; сейчас `attack` — только событие.
5. **Оригинальный AI**: разобрать `0x473eed`/`0x473f07`/`0x475615`, leash,
   состояния и переходы движка; сверить с `.soe`-типами на dm1/level0.
   В частности, подтвердить, что `lag = N` — это ровно N тиков (§3).
6. **Несколько игроков/целей**: `target` сейчас слот 0; нужен выбор цели,
   приоритеты и DM-респавн; тогда история станет per-target.
7. **Сохранение**: AI-состояние (включая кольцо) не входит в `saved*.sbt`;
   решить, какие поля писать (docs/SAVE_BLOCKS.md).

---

## 7. A2/A3: движение и `standoff` (`AiConfig::use_move_step`)

Opt-in слой между state machine и `world/ai_move.h`. Включается только
`use_move_step=true`; по умолчанию `false`, и тогда `Ai::tick` ведёт себя как
раньше (позиция меняется лишь на leash/return). A4-маршрут — отдельный
opt-in `use_path` (§8): он двигает только `patrol` и не требует
`use_move_step`.

Что происходит за тик при `true` (после LOS, выбора цели и `switch`):

| состояние | цель шага | остановка |
|---|---|---|
| `alert`, `attack`, `dist > standoff` | `AiAgent::aim_point` (игрок/вещь или лаг истории `+0x1d0`) | `standoff` (`+0x1cc`) |
| `alert`, `attack`, `dist + 60 <= standoff` | шаг **от** `aim_point` (`ai_retreat_step`) | выход из полосы `standoff-60` |
| `alert`, `attack`, `standoff-60 < dist <= standoff` | не двигается | стоит и стреляет |
| `alert`, `attack`, `standoff == 0` | `aim_point` | `arrive_radius` |
| `patrol` | `home` (или текущая точка A4-маршрута при `use_path`, §8) | `arrive_radius` |
| `idle(returning)` | `home` | `arrive_radius`, затем snap в `home`, `patrol`, событие `arrived` |
| `idle` | не двигается | — |
| `dead` | не тикает | — |

`dist` здесь — октогональная дистанция `position → aim_point`. Сравнение с
`+0x1cc` взято из `0x473a00` (отчёт §6.2): `v <= standoff` → `engage = 0`
(стоять), `v + 60 <= standoff` → `engage = -1` (пятиться). Оригинал считает
там планарную 2D-метрику `0x436774` с поправкой на высоту; в каркасе взята уже
готовая октогональная `octagonal_distance` (у земли разница ≤ 8 %, поправки на
высоту нет — это осознанное упрощение A3). Стрельба в оригинале не зависит от
`engage` и живёт в том же поиске цели (§8 отчёта); в каркасе её роль играет
событие `attack` состояния `attack`.

Чтобы «стоять и стрелять» работало на границе окна, при активном A3
(`use_move_step=true` и `standoff > 0`) пороги `alert → attack` и удержания
`attack` считаются по **планарной** дистанции `position → aim_point` и
расширяются до `max(target_zone, standoff)`. Иначе 3D eye-to-eye `distance`
(на 10 единиц `eye_height` больше планарной) не пускал бы агента в `attack`
ровно на сфере `standoff`, и события `attack` не было бы. При
`use_move_step=false` оба порога остаются прежними (`target_zone` /
`1.25×target_zone`).

Шаг считает `ai_seek_step()`: `speed_per_tick * 91 * dt` (то есть `+0x10` из
`THING_TYPES`), октогональное прибытие `max + 0.25*(mid+min)` и
swept-сфера `sphere_cast` радиуса `Thing::radius`. Стена урезает шаг до
контакта (`blocked_steps`), сквозь геометрию агент не проходит. Шаг назад
считает `ai_retreat_step()`: то же `speed_per_tick * 91 * dt`, то же
swept-сфера-ограничение, направление — планарное «от цели» (пола/гравитации в
каркасе нет, поэтому вертикальная разница тело не двигает). В `AiAgent`
пишутся `move_delta` (фактическое перемещение), `heading` (`atan2` этого
вектора); `omega` пока 0 — интегратор поворота это A6.

**Leash-кламп** (отчёт `ai-nav-report.md` §7). Якорь — `leash_anchor`, после
`attach` равный `home`; радиус — `leash_distance` (fallback `move_leash`,
`0` = выключен). Шаг (и вперёд, и назад) отменяется, если он *увеличивает*
октогональное расстояние до якоря и новое расстояние больше радиуса. Шаг
внутрь сферы разрешён всегда. Каждая отмена — `stats().leash_clamps += 1`;
состояние и цель при этом не сбрасываются (в отличие от state-машины leash в
§2, которая срабатывает по `AiConfig::leash` и уводит агента домой).

Чего здесь нет намеренно:

* pathfinding и `pathrandom` — A4 идёт строго по списку точек (§8), обхода
  препятствий нет (стена лишь урезает шаг); `pathrandom` (тип 11) не
  подключён, потому что вызывает `rand()`;
* оружия, окна `[w0,w1]`, кулдауна `w2` и `rand()%period` — `attack` остаётся
  событием, а не баллистикой (§8 отчёта — следующий шаг);
* памяти «последней увиденной точки»: во время `lose_sight_seconds` агент
  продолжает идти к `aim_point`, который обновляется по живой позиции цели
  (как и в прежнем каркасе), а не к точке потери LOS;
* изменения евклидова скана цели, `distance`, `search_radius`, LOS — они
  остаются прежними; октогональная метрика живёт только внутри шага;
* пола/гравитации/`floor_at` и интегратора `ω` — A6;
* `Things` не мутируется: движение живёт в `AiAgent::position` (как и
  return в прежнем каркасе).

Тесты: `test_move_step_opt_in`, `test_move_step_seek`, `test_move_step_wall`,
`test_move_step_leash`, `test_move_step_return`, `test_move_step_standoff`,
`test_move_step_standoff_far`, `test_move_step_standoff_window`,
`test_move_step_standoff_retreat`, `test_move_step_standoff_retreat_wall`
(все в `gm_ai_tests`, label `thing_ai`).

---

## 8. A4: path-примитив (`0x475f10`)

Чистый автомат точек из `FUN_00475f10` (отчёт `ai-nav-report.md` §6.8),
без RNG, без `Things`, без `CollisionWorld`. Живёт в `world/ai_move.h`:

```cpp
enum class AiPathMode : std::uint8_t { path = 9, loop = 10, expire = 12 };

struct AiPathState {
    int index = -1;       // +0x4ec
    bool waiting = false; // +0x4f4 > 0: точка выбрана, тело ещё не дошло
    bool expired = false; // pathexpire уже сработал
};

AiPathStep path_advance(const AiPathState&, std::span<const Vec3d> waypoints,
                        AiPathMode);
void path_mark_arrived(AiPathState&); // очистить `waiting`
```

`AiPathStep` возвращает `index` и саму `point`, флаги `selected` (выбрана
**новая** точка), `waiting` (точка ещё в работе), `arrived` (`path` стоит на
последней), `expired` (`pathexpire` дошёл до последней) и `state` — состояние
для следующего вызова. Ходьбу по-прежнему делает `ai_seek_step()`; когда тело
дошло, вызывающий зовёт `path_mark_arrived()`, и следующий `path_advance()`
берёт следующую точку. Это ровно разделение оригинала: `0x475f10` выбирает
точку и ставит `+0x4f4`, `0x476a50` идёт и сбрасывает флаг на прибытии.

Семантика (proven по дизассемблеру `0x475f10`):

| ситуация | результат |
|---|---|
| `state.waiting` | ничего нового; `selected=false`, `waiting=true`, та же точка |
| `index + 1 < count` | `selected=true`, `index+1`, `waiting=true` |
| конец, `path` (9) | `index` остаётся последним, `arrived=true`, без события |
| конец, `loop` (10) | `index = 0`, `selected=true` (цикл `0,1,2,3,0…`) |
| конец, `expire` (12) | `index` последний, `expired=true` — аналог `0x4703a0` |
| пустой список, `path`/`loop` | ничего (`index=-1`), тик просто ничего не делает |
| пустой список, `expire` | `expired=true` сразу (ветка `path_count < 1`) |
| `index` вне `[-1, count-1]` | перезапуск с `-1` (защита оригинала от битого `+0x4ec`) |

Важно: `expire` срабатывает **не** в момент выбора последней точки, а на
следующем вызове после её достижения — как в оригинале, где последняя точка
сначала отрабатывается ходьбой. `state.expired` терминален: повторные вызовы
не перезапускают путь, но `step.expired` выставляется только в вызове, который
собственно выполнил expire (иначе будущий цикл постил бы `0x4703a0` каждый тик). Список длиннее 32 точек
обрезается: оригинал отбраковывает `0x1f < i` (`kAiPathMaxPoints = 32`).

Чего здесь нет намеренно:

* `pathrandom` (тип 11) — вызывает `rand()` (`0x480e30`), а детерминизм для
  каркаса обязателен; для него нужен `AiConfig::rng_seed` и собственный ГПСЧ;
* разрешения id точки через таблицу локаций `0x80d8e0` — примитив работает с
  уже разрешёнными `Vec3d`; загрузка `+0x324[]` остаётся слою спавна/типов.

### 8.1. Подключение к `Ai::tick`

Второй opt-in: `AiConfig::use_path` (по умолчанию `false`) плюс маршрут
агента. Маршрут задаётся `Ai::set_path(thing_id, span<const Vec3d>)`:
обрезается до 32 точек, сбрасывает `path.index`/`waiting`/`expired`,
отклоняет неизвестный/мёртвый id и NaN/inf без изменения прежнего маршрута;
пустой span очищает маршрут. Это единственный способ заполнить
`path_waypoints` — слой спавна/типов (`+0x324[]`, таблица локаций) ещё не
подключён.

За тик, **после** восприятия и `switch` состояний:

1. Условие входа: `use_path=true`, `state == patrol` и (маршрут непуст или
   `path_mode == expire`). Это независимо от `use_move_step`: маршрут — свой
   opt-in, A2-параметры (`speed`, `radius`, `arrive_radius`) уже заполнены в
   `AiAgent` на `attach`.
2. `path_advance(state, waypoints, path_mode)` выбирает/возвращает текущую
   точку; `Ai::tick` сохраняет `step.state` в `AiAgent::path`.
3. `step.expired` → агент переходит в `dead` **один раз**: цель/alertness/
   timer сбрасываются, пишутся события `state`/`death`,
   `stats().path_expiries += 1`. Дальше `dead` не тикает, второй expire
   невозможен (терминальный `state.expired`).
4. `step.waiting` → `ai_seek_step` к `step.point` через тот же `move_step`,
   что A2/A3: октогональное прибытие `arrive_radius`, swept-сфера
   `radius` (`blocked_steps`) и жёсткий leash (`leash_clamps`). На прибытии
   вызывается `path_mark_arrived(path)`, и **следующий** тик выбирает
   следующую точку — за тик не более одного шага.
5. `!step.waiting` → `path` стоит на последней точке, `move_delta = 0`.
   Пустой маршрут при `path`/`loop` в ветку не входит: patrol продолжает
   A2-шаг домой (или стоит, если `use_move_step=false`).

Что не затронуто: `alert`/`attack` продолжают A2-поиск `aim_point` (при
`use_move_step`), `idle(returning)` идёт домой — маршрут там намеренно не
применяется, иначе возврат по leash мог бы не дойти до `home`. Евклидов скан
цели, LOS, `standoff` и оба leash-правила не меняются. При
`use_path=false` ветка не выполняется вообще: A2/A3 и исторический каркас
байт-в-байт прежние.

Ограничения подключения: маршрут только у `patrol`, без pathfinding и
`pathrandom`; `expire` убивает **агента**, но не мутирует `Things` (вещь
удаляет слой `Things`/spawn); при пустом маршруте и `expire` агент умирает на
первом patrol-тике — это осознанно повторяет ветку оригинала
`path_count < 1`.

Тесты (`gm_ai_tests`, label `thing_ai`): `test_path_loop` (4 точки →
`0,1,2,3,0,1`), `test_path_expire` (expire после последней, терминальность),
`test_path_empty` (пустой список для всех режимов), `test_path_repeat_advance`
(повторный advance без прибытия, `path`-терминал, битый индекс, обрезка до 32,
дефолты `AiConfig`), а также подключение к тику: `test_tick_path_opt_in`
(`use_path=false` не входит в маршрут, `set_path`/NaN/обрезка),
`test_tick_path_loop` (4 точки по кругу без `use_move_step`),
`test_tick_path_stand` (`path` замирает на последней), `test_tick_path_expire`
(один `death`, `path_expiries=1`), `test_tick_path_wall` (стена урезает шаг),
`test_tick_path_alert_ignores_route` (alert идёт к цели, не по маршруту).
