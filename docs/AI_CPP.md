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
(`standoff`-окно: seek / hold+fire / retreat, §7), opt-in A4
(`patrol` по списку точек, §8) и A6 (маршрут из `THING_TYPES +0x324[]`
через таблицу локаций, §8.2) и `pathrandom` (тип 11) с внедряемым
`MsvcRand`; реального оружия и решений по истории по-прежнему нет, см. §6.

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
| движение | default: только return к `home`; с `use_move_step=true` alert/attack идут к `aim_point`, patrol/return — к `home` через A2 (`world/ai_move.*`); внутри `standoff` стоят и стреляют, внутри `standoff-60` пятятся (A3); с `use_path=true` patrol идёт по `path_waypoints` (`loop`/`path`/`expire`, §8), приоритетнее шага домой; `load_path_from_types=true` сам заполняет маршрут из `THING_TYPES +0x324[]` и таблицы локаций (§8.2) |
| урон | `damage()` меняет только AI-агента, не `Things`; `sync_vitals()` читает виталы вещи (щиты, hits, HP) и убивает агента вместе с вещью |
| оружие | opt-in `use_weapon_table=true`: выбор записи `Thing::proximity` (`+0x218`/`+0x474`) по inclusive `[low, high]` и `cooldown`, отдельное событие `weapon`; opt-in `use_weapon_shots=true` кладёт чистый `AiWeaponShot` в очередь, снаряд внутри AI не спавнится, урона нет (§9) |
| история цели | кольцо **240** позиций; `tick` пишет один слот, лаг 0..239 |
| лаг из данных | `Thing::target_history_offset` (+0x1d0): 0 = текущая, иначе 1..239 |
| автоматы движения | `world/thing_motion.*` (§11): `movement_type` 0..18 — типы 0–6, 13–17 доказаны по байтам, 7/8 и 9–12 живут в `projectile.h`/`ai_move.h`, 18 — заглушка |

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
ai.set_path_from_thing(id);      // A6: маршрут из THING_TYPES +0x324[]
ai.sync_vitals();                // зеркалить здоровье/щиты/hits вещи в агента
// A5: таблица оружия уже загружена из Thing::proximity; выбор — только
// при AiConfig::use_weapon_table == true (событие weapon, без урона).
gm::AiWeapon weapon;
gm::ai_weapon_select(*things.by_id(id), distance, false, weapon);

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

    // A5-оружие; читается при AiConfig::use_weapon_table.
    AiWeapon weapon;           // +0x218/+0x474: count + последняя выбранная запись
    double weapon_cooldown;    // секунды из +0x27c (тики / 91)
    bool weapon_reverse;       // +0x468: следующее сканирование с конца
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
`pathexpire`, — один инкремент на агента), `weapon_events` (A5: выбранная
запись и отправленное событие `weapon`).

`events()` возвращает упорядоченный журнал `AiEvent { type, thing_id, from, to,
target, target_thing, aim, value, event_id, tick }`; `tick` 1-based, у `attach` — 0. Типы: `attach`, `sight`,
`lost_sight`, `state`, `attack`, `weapon`, `damage`, `death`, `leash`, `arrived`.
`event_id` заполняется только у `weapon` (id из `+0x21c`).
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

### 1.5. Прицел → автоматы движения (`apply_aim`)

`Ai::apply_aim(Things&)` — явный мост к `world/thing_motion` (вызывает
владелец тика, сам `Ai::tick` его не делает). Для каждого агента с
`thing_id` берётся слот (`Things::index_of`) и:

* `agent.has_aim` → `Things::aim_motion_at(index, agent.aim_point,
  agent.visible)`: позиция цели плюс три ошибки `0x473a00`, посчитанные в
  системе тела (см. `THINGS_CPP.md` §2.3);
* иначе → `Things::clear_motion_aim(index)`.

Возвращает число прицеленных агентов. Разделение как в оригинале: **какую**
точку выбрать, решает AI (скан игроков/вещей, история `target_history_offset`,
`aim_point`); **как** на неё смотреть — арифметика `0x473a00`, уже в
`thing_motion`. Тест `test_apply_aim` (`gm_ai_tests`): игрок на `+X` даёт
`yaw_error ≈ +π/2`, `apply_aim` возвращает 1; после `clear_players()` цель
становится `valid=false`.

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
| `use_weapon_table` | `false` | opt-in | включает A5-выбор оружия (§9); `false` не меняет A2/A3/A4 и исторический каркас |
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
| A6: режимы | `ai_path_mode(9/10/12/11)` → `path/loop/expire/random`; прочие → `false` |
| A6: default | таблица локаций (7 записей, дубликат id → первая) и `+0x324[]` (4 id) на месте, но `load_path_from_types=false` → маршрут пуст у всех 5 агентов |
| A6: load | `load_path_from_types=true` → loop-агент 3 точки (битый id пропущен), expire-агент 2 точки, `pathrandom`-агент 2 точки с `path_mode=random`, `NO_PATH`/«все id битые» пусты; неизвестный/мёртвый id → `false`; 40 id → 32 |
| A6: patrol | loop идёт `0,1,2,0`; expire доходит до последней и умирает (`path_expiries=1`); агенты без маршрута стоят |
| A6: level0 | `BAGCRTHQ`, 9 id → 9 точек, первая `(24,1128,-72)`, последняя `(-360,1000,-80)`, `path_mode=loop` |
| A4: random | 4 точки + `MsvcRand(12345)` → `rand()%4`, повторный advance без прибытия RNG не тратит, прибытие даёт следующее `%4`; пустой список не expire, без RNG (`nullptr`) выбора нет |
| A4: random opt-in | `use_path_random=true` + `set_path_rng` → patrol-агент типа 11 выбирает точку; `use_path_random=false` → `index=-1`, RNG не тронут |

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
   `path`/`loop`/`random`/`expire`, `expire` убивает агента. Автоматы самого движка
   (`movement_type` 0..18) перенесены отдельным модулем `world/thing_motion`
   (§11) — включая пол/потолок (`0x472120`), отскок и турель. **Мост
   `Ai` → `thing_motion` есть**: `Ai::apply_aim(Things&)` раздаёт
   `agent.aim_point` через `Things::aim_motion_at()` (ошибки `0x473a00` в
   системе тела), а потерянная цель очищает её (`valid=false`). **Интеграция в `Things::tick`
   сделана** (opt-in `set_motion_enabled(world)`, `motion_state/last`,
   `set_motion_target`, `set_push_targets`, тест `test_motion_tick`).
   Маршрут из
   `THING_TYPES +0x324[]` через таблицу локаций подключён третьим opt-in'ом
   (`load_path_from_types`, §8.2).
4. **Бой**: оружие из таблицы `+0x21c` уже выбирается и постит событие
   `weapon` (§9), но без снаряда и урона; остались спавн снаряда по
   `event`/`offset`, урон игроку, звук (`+0x1fc`), анимация.
5. **Оригинальный AI**: разобрать `0x473eed`/`0x473f07`/`0x475615`, leash,
   состояния и переходы движка; сверить с `.soe`-типами на dm1/level0.
   В частности, подтвердить, что `lag = N` — это ровно N тиков (§3).
   Автоматы движения (`0x474260`/`0x474ea0`/`0x474a10`/`0x475930`/`0x476070`)
   уже разобраны — см. §10 и `docs/THING_FIELDS.md` §4a; остались
   `0x473240`-флаги режимов 14/15/17 и таблица «следа» `0x819070`.
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

* pathfinding — A4 идёт строго по списку точек (§8), обхода препятствий
  нет (стена лишь урезает шаг); `pathrandom` (тип 11) подключён, но только
  с внедряемым `MsvcRand` (§8), детерминизм задаёт вызывающий;
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
                        AiPathMode, MsvcRand* rng = nullptr);
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
| `pathrandom` (11), не ждём | `index = rand() % count` (`0x480e30`), `selected=true`, `waiting=true`; при `rng == nullptr` выбора нет |
| `pathrandom`, пусто | ничего, без expire (как `path`/`loop`) |
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

`pathrandom` (тип 11) — та же точка входа, но индекс берётся как
`rand() % path_count` на **каждом** вызове, который не ждёт тело
(`0x475f10`: и ветка «индекс вне диапазона», и ветка «индекс в диапазоне»
зовут `0x480e30`). Поэтому режим недетерминирован по своей природе и включён
дважды opt-in: `AiConfig::use_path_random` и `Ai::set_path_rng(&MsvcRand)`.
Генератор принадлежит вызывающему (`game/rng.h`, MSVC LCG `0x343FD/0x269EC3`),
так что replay восстанавливает ту же последовательность; без ГПСЧ типа 11 не
входит в ветку маршрута и стоит. `path_advance` сам `rand()` не зовёт.

Чего здесь нет намеренно:

* обхода препятствий (pathfinding) — маршрут строго по списку точек;

Разрешение id в координаты живёт отдельно от примитива: `path_advance`
работает с готовыми `Vec3d`, а загрузчик `+0x324[]` через таблицу локаций —
это A6 (§8.2).

### 8.1. Подключение к `Ai::tick`

Второй opt-in: `AiConfig::use_path` (по умолчанию `false`) плюс маршрут
агента. Маршрут задаётся `Ai::set_path(thing_id, span<const Vec3d>)`:
обрезается до 32 точек, сбрасывает `path.index`/`waiting`/`expired`,
отклоняет неизвестный/мёртвый id и NaN/inf без изменения прежнего маршрута;
пустой span очищает маршрут. Второй источник — A6
`Ai::set_path_from_thing(thing_id)` / `AiConfig::load_path_from_types` (§8.2).

За тик, **после** восприятия и `switch` состояний:

1. Условие входа: `use_path=true`, `state == patrol`, (маршрут непуст или
   `path_mode == expire`), а для `random` ещё `use_path_random` и непустой
   `path_rng()`. Это независимо от `use_move_step`: маршрут — свой
   opt-in, A2-параметры (`speed`, `radius`, `arrive_radius`) уже заполнены в
   `AiAgent` на `attach`.
2. `path_advance(state, waypoints, path_mode, rng)` выбирает/возвращает
   текущую точку (`rng` передаётся только для `random`); `Ai::tick`
   сохраняет `step.state` в `AiAgent::path`.
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

Ограничения подключения: маршрут только у `patrol`, без pathfinding;
`pathrandom` требует внешнего `MsvcRand`; `expire` убивает **агента**, но не мутирует `Things` (вещь
удаляет слой `Things`/spawn); при пустом маршруте и `expire` агент умирает на
первом patrol-тике — это осознанно повторяет ветку оригинала
`path_count < 1`.

Тесты (`gm_ai_tests`, label `thing_ai`): `test_path_loop` (4 точки →
`0,1,2,3,0,1`), `test_path_expire` (expire после последней, терминальность),
`test_path_empty` (пустой список для всех режимов), `test_path_random`
(`rand()%4`, RNG только на выбор, повтор без прибытия RNG не тратит,
детерминизм по сиду, `nullptr`/пусто), `test_path_repeat_advance`
(повторный advance без прибытия, `path`-терминал, битый индекс, обрезка до 32,
дефолты `AiConfig`), а также подключение к тику: `test_tick_path_opt_in`
(`use_path=false` не входит в маршрут, `set_path`/NaN/обрезка),
`test_tick_path_loop` (4 точки по кругу без `use_move_step`),
`test_tick_path_stand` (`path` замирает на последней), `test_tick_path_expire`
(один `death`, `path_expiries=1`), `test_tick_path_wall` (стена урезает шаг),
`test_tick_path_alert_ignores_route` (alert идёт к цели, не по маршруту).

### 8.2. A6: маршрут из `THING_TYPES +0x324[]`

Формат доказан по загрузчику и потребителю (все адреса — `gundx.exe`,
`docs/SOE.md` §3):

| что | адрес | смысл |
|---|---|---|
| `+0x320` | `0x47daea` | `path_count`; `cmp eax,0x20; jg error`, т.е. **0..32** |
| `+0x324 + 4*i` | `0x47db3d` | `path_point[i]`; загрузчик резолвит токен через `0x460000` и пишет **числовой id локации** |
| таблица локаций | `0x80d8e0`, счётчик `0x80d87c`, шаг `0x28` | запись `{int id, int x, int y, int z, double orient[3]}` |
| `ADD_LOCATION` | handler `0x45bea6` | добавляет операнды `ELLLAAA` дословно, максимум `0xc8` = 200 |
| разрешение | `0x476008`..`0x476052` | линейный поиск `entry.id == path_point[i]`; `fild +0x04/+0x08/+0x0c` → x/y/z, затем `+0x4f4`/таймер |

Ключевые следствия, заложенные в C++:

* координаты точки — **целые** операнды `ADD_LOCATION`, а не double;
* id ищется с начала таблицы, поэтому при дубликате id побеждает **первая**
  запись (`0x476012`), а отсутствующий id не даёт точки (`0x476023` —
  ранний `ret`, позиция и таймер не обновляются);
* `+0x324[]` — это id, а не координаты и не индекс в массиве локаций;
  `pathrandom` (11) к этому массиву тоже обращается и на каждом прибытии
  выбирает индекс через `rand() % path_count`.

`Things` собирает таблицу локаций из **выполненной** сцены VM
(`VmScene::locations`, `h_location`/`0x45bea6`) в порядке выполнения,
дедуп по `(site, occ)` — так же, как список вещей. Публичный доступ:

```cpp
const std::vector<VmLocation>& Things::locations() const;
const VmLocation* Things::location(int id) const; // first match, 0x476012
```

Загрузчик в `Ai`:

```cpp
bool Ai::set_path_from_thing(int thing_id); // resolves +0x324[] via Things
// AiConfig::load_path_from_types = false по умолчанию
```

`set_path_from_thing` обрезает до `kAiPathMaxPoints` (32), пропускает id,
которых нет в таблице (как miss-ветка `0x476023`), отклоняет неизвестный/
мёртвый id, пустой список и маршрут, где не разрешилась ни одна точка, не
трогая прежний маршрут. При `load_path_from_types=true` `attach` сам вызывает
загрузчик для типов движения 9/10/11/12 и выставляет **per-agent**
`AiAgent::path_mode` (9 `path`, 10 `loop`, 11 `random`, 12 `expire`); при `false`
поведение A2/A3/A4 прежнее, а `path_mode` остаётся глобальным
`AiConfig::path_mode`. Прочие типы движения route не получают; тип 11
получает маршрут, но остаётся инертным без `use_path_random` и `MsvcRand`.

Ограничения A6: маршрут по-прежнему проходят только `patrol`; `expire`
убивает агента, но не мутирует `Things`; отсутствующие id **выбрасываются**
из маршрута (индексы последующих точек сдвигаются — оригинал в этом случае
оставляет точку невыбранной и ждёт, что для детерминированного каркаса
равносильно застреванию); случайный старт `pathrandom` воспроизводится
только через внешний ГПСЧ, `DELETE_LOCATION` во время игры не моделируется.

Тесты: `test_path_mode_map` (9/10/11/12), `test_path_from_thing_default_off`
(таблица локаций и `+0x324[]` есть, но `attach` не заполняет маршрут;
first-match дубликата id), `test_path_from_thing_load` (loop/expire, пропуск
неизвестного id, пустой тип, `pathrandom` (2 точки, mode random), «все id битые», мёртвый агент,
обрезка 40 → 32), `test_path_from_thing_patrol` (loop 0,1,2,0; expire →
`dead`/`path_expiries=1`; агенты без маршрута стоят) и
`test_path_from_thing_real` (level0 `BAGCRTHQ`: 9 точек, первая
`(24,1128,-72)`, последняя `(-360,1000,-80)`).

---

## 9. A5: оружейная таблица (`AiConfig::use_weapon_table`)

Таблица оружия вещи — те же 8 слотов, что `Thing::proximity`:
`THING_TYPES +0x218` (count), `+0x21c[i]` event, `+0x23c[i]` low,
`+0x25c[i]` high, `+0x27c[i]` cooldown, `+0x29c/+0x2bc/+0x2dc[i]` offset.
Загрузчик уже копирует её в `Thing`, поля загрузчика/рантайма описаны в
`docs/SOE.md`. `ProximityEntry` — общий тип с `game/projectile.h`, поэтому
`AiWeapon` не дублирует поля в `Thing` и не заводит вторую таблицу.

`AiWeapon` — **одна** провалидированная запись плюс размер таблицы:

```cpp
struct AiWeapon {
    int count;              // 0..8, clamp 0x47dc2c
    std::uint32_t event;    // +0x21c, 0 = inactive
    int low, high;          // +0x23c/+0x25c, inclusive band
    int cooldown;           // +0x27c, тики 91 Гц (0.91*token)
    Vec3d offset;           // +0x29c.., данные для будущего спавна
    bool valid() const;
};
int ai_weapon_count(const Thing&);
bool ai_weapon_at(const Thing&, int index, AiWeapon&);
bool ai_weapon_select(const Thing&, double distance, bool reverse, AiWeapon&);
```

Валидация: `event != 0`, `high >= low`, `cooldown >= 0`, конечный `offset`;
индекс вне `0..min(8, proximity.size())-1` отклоняется. `ai_weapon_select`
берёт **первую** запись, у которой `low <= distance <= high`; `reverse=true`
сканирует с конца, иначе с начала.

Оригинал (`0x473a00`, после выбора цели): per-agent блок `+0x474` (count),
`+0x478` event[8], `+0x498` low[8], `+0x4b8` high[8], `+0x4d8` cooldown[8],
`+0x4f8/+0x518/+0x538` offset x/y/z[8]. Условие записи —
`event != 0 && low <= d && d <= high`; при `+0x468 == 0` скан идёт с конца,
иначе с начала, а после выстрела направление переключается. `event_post`
получает id записи, `+0x464 += +0x4d8[i]`; сам `0x473a00` снаряд не спавнит и
урон не наносит. C++ повторяет выбор, inclusive-полосу, направление
(`AiAgent::weapon_reverse`, стартует с конца) и кулдаун; `d` — октогональная
дистанция `agent.position → aim_point`, как `local_60` оригинала.

Подключение в `Ai::tick` (только при `use_weapon_table=true`):

1. кулдаун `AiAgent::weapon_cooldown` уменьшается на `dt` каждый живой тик;
2. событие `weapon` разрешено только в `state == attack`, при `visible` и
   `has_aim`; при активном A3 дополнительно требуется
   `engage_distance <= engage_range`, то есть **внутри окна** `standoff`;
3. при `weapon_cooldown <= 0` вызывается `ai_weapon_select`; успешный выбор
   пишет `AiAgent::weapon`, переключает `weapon_reverse`, ставит кулдаун
   `+0x27c / 91` секунд, увеличивает `stats().weapon_events` и пишет
   `AiEvent{type=weapon, event_id=<+0x21c>, value=<distance>}`;
4. события `attack`/`state` не меняются: `attack` по-прежнему живёт в своей
   ветке и в A3-окне;
5. A5b (opt-in `use_weapon_shots=true`, требует `use_weapon_table=true`): на
   том же успешном выборе в очередь добавляется **ровно один** `AiWeaponShot`,
   `stats().weapon_shots` увеличивается на 1.  Состояние, кулдаун, A3-окно и
   журнал событий не меняются; при `use_weapon_shots=false` очередь всегда
   пуста.

`AiWeaponShot` — чистые данные, не снаряд:

```cpp
struct AiWeaponShot {
    int thing_id;               // стрелок, Thing::id
    std::uint32_t event_id;     // +0x21c записи (GENERATE/event id)
    Vec3d origin;               // position + AiWeapon::offset (дуло)
    Vec3d direction;            // единичный вектор origin -> aim_point
    double speed;               // мировых единиц/секунду; 0 = не разрешена
    int target;                 // 0..7, -1 = вещь/нет
    int target_thing;           // Thing::id, -1 = игрок/нет
    Vec3d aim;                  // точка прицела на момент выстрела
    double distance;            // октогональная дистанция выбора
    std::uint64_t tick;         // 1-based тик AI
};

const std::vector<AiWeaponShot>& ai.weapon_shots();
std::vector<AiWeaponShot> ai.take_weapon_shots(); // move + clear
ai.drain_weapon_shots();                          // discard
```

`origin` — `agent.position + offset` (смещение применяется как дуло;
гипотеза, исходное поле `+0x29c` — данные).  `direction` нормализуется, при
нулевом/неконечном векторе берётся `(0,0,1)`.  В таблице оружия нет скорости:
`AiConfig::weapon_shot_speed` (единицы/секунду) — opt-in подсказка, по
умолчанию `0` = «пусть решит слой снаряда».  `attach()` очищает очередь.

Мост к `Projectiles` (`game/projectile.h`) переиспользует существующий
`projectile_spawn_from_shot`:

```cpp
gm::Projectiles projectiles;
std::size_t n = gm::spawn_ai_shots(ai, projectiles, events);
// по одному:
for (const gm::AiWeaponShot& shot : ai.take_weapon_shots()) {
    gm::ProjectileSpawn spawn;
    if (gm::ai_shot_projectile_spawn(shot, projectile_thing, owner_player, spawn)) {
        projectiles.spawn(std::move(spawn), events);
    }
}
```

`ai_shot_projectile_spawn` переводит `speed` из единиц/секунду в единицы/тик
(`/91`) через тот же фабричный код, что и выстрел игрока; при `speed == 0`
берётся проверенная скорость типа снаряда `Thing::speed` (`+0x10`,
единиц/тик) `* 91`.  `projectile_thing` также поставляет gravity, damping,
shape/radius, hits, lifetime и event ids, а `shot.thing_id` становится
`owner_thing`.  Для `movement_type == 8` заполняются `target_index`
(игрок 0..7) и `target_point` (aim).  `spawn_ai_shots` дренирует очередь,
дропает выстрелы без скорости и возвращает число реально заспавненных
снарядов; `Projectiles::spawn` сам постит create-event.

Границы: ни `Ai`, ни мост не расходуют патроны, не наносят урон и не
резолвят `event_id` в конкретный `THING_TYPES`-тип — это делает игровой слой.
Один переданный `projectile_thing` имеет один `Thing::id`, поэтому
`Projectiles` примет с ним только один живой снаряд; для очереди используйте
`nullptr` и заполняйте id/скорость на стороне игры либо по одному шаблону на
выстрел.

Чего нет намеренно: спавна снаряда **внутри** `Ai`, урона игроку,
`extra`-кодирования `target<<16 | 8+thing`, расхода патронов и фронтального
гейта оригинала (`ABS(local_20)*4 < local_18`) — в каркасе нет
ориентации/камеры.  `offset` теперь применяется только к `AiWeaponShot.origin`.
`count` заполняется уже на `attach` и при `use_weapon_table=false` остаётся
данными, поведение байт-в-байт прежнее.

Тесты (`gm_ai_tests`, label `thing_ai`): `test_weapon_table_load` (clamp 8,
inactive/inverted/negative/NaN), `test_weapon_selection` (inclusive band,
направление, нет совпадения), `test_weapon_default_off` (таблица загружена,
но `weapon_events=0` и `attack` работает), `test_weapon_opt_in_window_and_cooldown`
(alert не стреляет, первый `weapon` в `attack`, octagonal 152.5, id 41,
кулдаун 91 тик), `test_weapon_high_gate` (`high` режет событие, `attack`
остаётся), `test_weapon_shots_default_off` (событие есть, очередь пуста),
`test_weapon_shots_opt_in` (ровно один shot на событие, origin/direction/speed,
drain и кулдаун), `test_weapon_shots_a3_window` (alert не копит, attack даёт
один shot, позиция в окне) и `test_weapon_shot_spawn_bridge`
(`ai_shot_projectile_spawn`/`spawn_ai_shots`, единицы/тик, homing target,
отказ без скорости).

---

## 10. Движение оригинала: seek + collide, без pathfinder

Разобрано по байтам (`docs/THING_FIELDS.md` §4a): `0x470c70` — тик одной
вещи, `switch` по `inst+0x390 movement_type`, по обработчику на режим.

* **`0x474260` (тип 2)** — движение без цели: три зонда `0x43c51f`
  (вперёд `object+0x30`, влево `−right`, вправо `+right`; `right` — строка
  `object+0x18`), порог `2·radius` и «впереди уже, чем сбоку»; поворот
  `+0x58 ±= dt·+0x60` с клампом `±+0x68` и `*0.75`, крен `±π/8` в `+0x38`
  при `flags_b & 0x40000000`; шаг `speed·dt` по трём компонентам `forward`
  только если `d_fwd > speed·dt + radius`.
* **`0x474ea0` (тип 5)** — не преследование, а **турель**: скан цели
  `0x473a00`, наведение только по yaw (ошибка pitch в этой сборке всегда 0),
  клампы `+0x228/+0x230` (pitch) и `+0x238/+0x240` (yaw), `+0x0c8` работает
  как скорость pitch. Тело не двигается.
* **`0x474a10` (тип 13)**, **`0x4746b0` (3/4)**, **`0x473240` (14/15/17)** —
  та же семья: зонд `1.25·radius`, поворот, шаг по `forward`; 14/15/17 при
  наличии цели набирают скорость поворота к её знаку (при
  `flags_b & 0x00800000` — `0.25·turn_max`) и умеют вертикальный seek
  (`+0x0a8`) при `gravity == 0` и `|error| >= 20`.
* **`0x476070` (тип 16)** — патрульная пушка: не двигается, стреляет по
  полосам `[weapon_p0, weapon_p1]` с кулдауном `weapon_p2` и конусом
  `|lateral|·4 < forward`.

Навигация в оригинале — это seek к точке + collide, а не A*/сетка. Маршруты
(патрули, waypoint’ы) приходят из скрипта: `THING_TYPES +0x324[]` и
`ADD_LOCATION` (`docs/THING_FIELDS.md`), что уже читает
`load_path_from_types`. Поэтому «pathfinding отсутствует» — не пробел C++,
а свойство оригинала.

Отдельная поправка к прежней версии этого раздела: `+0x0d8` — **не** drag и
не «скорость поворота». Это переключатель физики: `== 0` → контактная
коллизия `0x46d7b0`, `!= 0` → снарядная `0x472790`; у типа 6 это множитель
гравитации. Скорость поворота — `+0x58`, цель — `+0x40`, демпфер `*0.75`.

---

## 11. Автоматы движения вещей (`world/thing_motion`)

Модуль `engine/src/world/thing_motion.{h,cpp}` — недостающий средний слой:
`Ai` владеет решением («куда идти»), `thing_motion` — исполнением
(`movement_type`), `game/projectile` — снарядами. Источник чисел —
`docs/THING_FIELDS.md` §4a (адреса, поля, переходы).

```cpp
#include "world/thing_motion.h"

const gm::ThingMoveInfo& info = gm::thing_move_info(thing.movement_type);
gm::ThingMotionConfig cfg = gm::thing_motion_config(thing);   // + доказанные поля типа
gm::ThingMotionState  st  = gm::thing_motion_spawn(cfg);     // +0x0a0, +0x48/50/58

gm::ThingMotionInput in;
in.dt = 1.0;                 // тики (в оригинале целое param_2)
in.world = &collision;       // nullptr = пустой мир: зонды «свободно»
in.target.position = ...;    // 0x473a00 / 0x476070
in.target.yaw_error = ...;   // текущая ошибка по yaw (пересчитывается каждый тик)
in.target.valid = true;

const gm::ThingMotionStep step = gm::thing_motion_step(st, cfg, in);
// step.turn (0/2/3), step.moved, step.blocked, step.grounded, step.bounced,
// step.fired_weapon / fired_event (тип 16), step.displacement
```

| режим | `ThingMove` | что делает | статус |
|---|---|---|---|
| 0 | `idle` | только общая часть тика | proven |
| 1 | `spin` | `углы += rate·dt` | proven |
| 2 | `avoid` | 3 зонда, `2r`, поворот, шаг | proven |
| 3/4 | `avoid_left/right` | `1.25r`, фиксированный поворот | proven |
| 5 | `turret` | наведение по yaw, лимиты, без движения | proven |
| 6 | `projectile` | гравитация `0.2·g·level_g·dt`, отскок, демпфер | proven (дубль `game/projectile.h` для оружия) |
| 7/8 | `seek`/`seek_los` | поворот к цели и шаг | external: `ProjectileState::homing` |
| 9–12 | `path*` | path/loop/random/expire | external: `ai_move.h path_advance` (`random` — с `MsvcRand`) |
| 13 | `ram` | `1.25r` + выбор стороны по цели | proven |
| 14/15/17 | `seek_far/near/17` | seek по yaw + зонды + вертикальный seek | proven |
| 16 | `patrol` | полосы `p0..p1`, кулдаун `p2`, конус 14° | proven (наведение — модель) |
| 18 | `push` | — | stub |

Что в модуле **не** моделируется и почему:

* `rand()`: в `0x472120` (знак `±2.5` и шанс отскока) и `0x476070`
  (`+0x3ec/+0x3f0`). C++ берёт детерминированный вариант, иначе рушится
  требование детерминизма тестов (`docs/AI_CPP.md` §5).
* «След» `0x819070` (0xf0 точек на игрока) для типа 16: заменён переданной
  целью; стрельба/полосы/кулдаун — как в оригинале.
* Мёртвые поля `+0x1e8/+0x200` (+0x1d8/+0x1e0, +0x1f0/+0x1f8, +0x51c/+0x520):
  арифметика сохранена (`thing_thrust_step`), но аккумуляторы никто не
  пишет, поэтому в поставочной игре дожим не срабатывает.

Тест — CTest `thing_motion` (quick, `engine/tests/test_thing_motion.cpp`):
`thing_basis` сверен с `camera_basis` (уже доказанный `0x41e025`), по
сценарию на режим (пол/потолок/стены из синтетического `.sbw`), гейты шага,
крен, лимиты, отскок, «прилипание» к полу, негативы (нулевой/NaN `dt`,
`expired`, тип вне 0..18) и два прогона по 200 тиков побитово. Реальная часть:
`level0.sbt` (57 типов, гистограмма `movement {0:10, 1:1, 5:1, 6:38, 8:1,
10:1, 15:4, 18:1}`) и `dm1.sbt` (59 типов), флаги `Grenade`/`ROCKET` из
`docs/THING_FIELDS.md` §8 и 91 тик по каждому заспавненному экземпляру
(121 и 141) с проверкой конечности, диапазона yaw и детерминизма.
