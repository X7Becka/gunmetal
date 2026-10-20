# Gunmetal — минимальный C++ AI вещей (`Ai`)

Что здесь: `engine/src/world/ai.{h,cpp}` — детерминированная заготовка
state machine для runtime-вещей `Things`. Модуль **не порт** оригинального AI:
нет навигации, анимации, звука, урона игроку и рендера/SDL. Он берёт уже
известную абсолютную позицию вещи, считает расстояние до единственного глаза
игрока, проверяет line-of-sight лучом через `render/collide.h` и переключает
состояния на фиксированном тике **91 Гц**.

Статус: API, состояния, LOS, таймеры, leash/return, synthetic- и
поставочные golden'ы, 240-слотовая история позиции цели с лагом
`Thing::target_history_offset` и зеркало виталов вещи (`sync_vitals`: HP,
щиты, счётчик попаданий; мёртвая вещь убивает агента) готовы. CTest
`thing_ai` (quick, **0.35 с**) проходит, `python3 regress.py` — **113/113**
(скрипт не менялся). Игра только читается; `Things` AI не мутирует.
Навигации и принятия решений по истории (куда стрелять/идти) по-прежнему нет
— см. §6.

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
| движение | только return к `home`; alert/attack не преследуют |
| урон | `damage()` меняет только AI-агента, не `Things`; `sync_vitals()` читает виталы вещи (щиты, hits, HP) и убивает агента вместе с вещью |
| история цели | кольцо **240** позиций; `tick` пишет один слот, лаг 0..239 |
| лаг из данных | `Thing::target_history_offset` (+0x1d0): 0 = текущая, иначе 1..239 |

---

## 1. API

```cpp
#include "world/ai.h"

gm::Ai ai;                       // AiConfig по умолчанию
const std::size_t n = ai.attach(things); // executed view, только absolute
ai.set_player(eye);              // eye = (x, y, z) в мировом кадре
ai.tick(world);                  // ровно один тик 1/91 с
ai.tick(world, dt);              // dt зажимается до config().max_dt
ai.damage(id, 25.0);             // true, если агент жив и урон принят
ai.place(id, {x, y, z});         // только тест/отладка, без навигации
ai.sync_vitals();                // зеркалить здоровье/щиты/hits вещи в агента

const auto& agents = ai.agents();
const gm::AiAgent* a = ai.agent(id);
const auto& stats = ai.stats();
const auto& events = ai.events(); // накопительный журнал

// История позиции цели (240 слотов, один write на принятый tick).
const gm::TargetHistory& history = ai.target_history();
gm::Vec3d p{};
ai.target_position(0, p);        // 0 = текущая позиция игрока; false без цели
ai.target_position(5, p);        // 5 тиков назад; false, если истории меньше
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
виталями вещи).

`events()` возвращает упорядоченный журнал `AiEvent { type, thing_id, from, to,
value, tick }`; `tick` 1-based, у `attach` — 0. Типы: `attach`, `sight`,
`lost_sight`, `state`, `attack`, `damage`, `death`, `leash`, `arrived`.
`clear_events()` очищает только журнал, не состояние.

### 1.3. История позиции цели
`TargetHistory` — фиксированное кольцо `slots[240]`, `next` (следующий слот,
аналог `history_index` +0x3204) и `count` (0..240). `Ai::tick` после проверки
`dt` пишет ровно одну точку — eye игрока; без игрока записи нет. `attach()` и
`clear_player()` сбрасывают кольцо.

* `TargetHistory::at(lag, out)` — `lag` зажимается в 0..239; 0 = самая свежая
  запись, 1 = предыдущий tick. `false`, если записей меньше `lag + 1`.
* `Ai::target_position(lag, out)` — для `lag == 0` возвращает **живой** eye
  игрока (как оригинал, где 0 = текущая позиция), иначе читает кольцо. Без
  игрока `lag 0` отдаёт самую свежую запись; пустое кольцо → `false`.
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
  Реального патрулирования/waypoint'ов нет.
* **alert** — цель захвачена. `timer` 0.5 с; если цель видна и ближе
  `attack_range`, переход в `attack`. Без LOS `alertness` падает 3 с, затем
  `patrol` и цель сбрасывается.
* **attack** — цель в радиусе; каждые `attack_period` 0.75 с пишется событие
  `attack`. Урона игроку нет — это заготовка. При дистанции > 1.25×range
  возврат в `alert` (гистерезис).
* **dead** — после `damage()` до `health <= 0`. Таймеры и LOS не считаются;
  `damage()` по мёртвому/неизвестному id возвращает `false`.
* **leash/return** — если агент (например, после `place()`) оказался дальше
  `leash` от `home`, он сбрасывает цель, идёт в `idle(returning)` и движется к
  `home` со скоростью `return_speed`; по прибытии — `patrol` и событие
  `arrived`. Пока идёт return, LOS игнорируется.

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
| `max_dt` | `181/91 ≈ 1.989` | guard | `0x470b20` при выходе суммы за 1..181 тик **сбрасывает её в 1**, а не зажимает; C++-`Ai` использует верхний предел как защиту от огромного dt (`clamped_ticks`), это осознанное отличие |
| `los_skin` | 1e-3 | hypothesis | допуск на конечную точку луча |
| `kAiHistorySlots` | 240 | **proven** (данные+код) | `history[240][3]` в структуре объекта (`+0x30`), `history_index` (`+0x3204`) wrap на `0xf0` — docs/SAVE_FIELDS.md §9.2 |
| `TargetHistory` шаг | 1 запись / tick | **proven** (код) | «позиция пишется каждый апдейт», `0x44377b..0x44378b`, docs/SAVE_FIELDS.md §9.2 |
| clamp `history_lag` | 0 или 1..239 | **proven** (код) | `0x473eed`/`0x476523`, поле +0x1d0, docs/SOE.md; 0 — «текущая позиция». Данные: 0..10 во всём корпусе, у dm1/level0 только 0 (например level1 — 1..2, level3 — 1..10) |
| смысл `lag = N` | N тиков назад | hypothesis | доказан сдвиг по 240-кадровой истории, но не единица счёта; C++ выбрал 1 tick = 1 запись |

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
| два прогона | `same_agent` для всех полей и совпадение журнала на 300 тиках; отдельный прогон сравнивает кольцо истории |

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
* CTest `thing_ai` — **0.35 с**, label quick. `regress.py` — **113/113**.

Ограничения доверия: golden'ы AI (радиусы, таймеры, скорости) — это golden'ы
**этой заготовки**, а не оригинала. Они не доказывают поведение `gundx.exe`.

---

## 6. Что осталось

Реализовано: кольцевая история позиции цели на 240 тиков и лаг из
`Thing::target_history_offset` (§1.3) — **код**; сама 240-слотовая раскладка и
clamp 1..239 — **данные+код** (`docs/SAVE_FIELDS.md`, `docs/SOE.md`). Остаётся:

1. **Типы вещей**: `+0x1b4` (movement), `+0x1cc` (attack range), `+0x1c8`
   (hits) уже в `AiAgent`; `+0x1d4` (health) и `+0x1d8` (щиты) — зеркало
   виталов (§1.4). Осталось подключить `+0x1c4` (collide event) и `+0x1e8`
   (expire ticks), а также отдавать эти поля через `Things` для остальных
   потребителей.
2. **Принятие решений**: `history_lag` уже даёт `AiAgent::aim_point`
   (с упреждением на N тиков) и `aim_distance`; state machine пока считает
   LOS/зоны по живому глазу. Осталось привязать историю к конкретной цели
   и использовать `aim_point` в реальном оружии/сглаживании; сейчас игрок
   один (`target=0`).
3. **Навигация**: patrol waypoint'ы (`+0x320`), path/pathloop/pathrandom,
   collision-aware движение, высота/пол через `floor_at`, гравитация.
4. **Бой**: урон игроку, оружие (`+0x21c`), снаряды, звук (`+0x1fc`),
   анимация; сейчас `attack` — только событие.
5. **Оригинальный AI**: разобрать `0x473eed`/`0x473f07`/`0x475615`, leash,
   состояния и переходы движка; сверить с `.soe`-типами на dm1/level0.
   В частности, подтвердить, что `lag = N` — это ровно N тиков (§3).
6. **Несколько игроков/целей**: `target` сейчас слот 0; нужен выбор цели,
   приоритеты и DM-респавн; тогда история станет per-target.
7. **Сохранение**: AI-состояние (включая кольцо) не входит в `saved*.sbt`;
   решить, какие поля писать (docs/SAVE_BLOCKS.md).
