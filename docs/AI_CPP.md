# Gunmetal — минимальный C++ AI вещей (`Ai`)

Что здесь: `engine/src/world/ai.{h,cpp}` — детерминированная заготовка
state machine для runtime-вещей `Things`. Модуль **не порт** оригинального AI:
нет навигации, анимации, звука, урона игроку и рендера/SDL. Он берёт уже
известную абсолютную позицию вещи, считает расстояние до единственного глаза
игрока, проверяет line-of-sight лучом через `render/collide.h` и переключает
состояния на фиксированном тике **91 Гц**.

Статус: API, состояния, LOS, таймеры, leash/return, synthetic- и
поставочные golden'ы готовы. CTest `thing_ai` (quick, **0.35 с**) проходит,
`python3 regress.py` — **113/113** (скрипт не менялся). Игра только читается;
`Things` AI не мутирует.

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
| урон | `damage()` меняет только AI-агента, не `Things` |

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

const auto& agents = ai.agents();
const gm::AiAgent* a = ai.agent(id);
const auto& stats = ai.stats();
const auto& events = ai.events(); // накопительный журнал
```

### 1.1. `AiAgent`

```cpp
struct AiAgent {
    int thing_id;       // Thing::id из Things
    AiState state;      // idle / patrol / alert / attack / dead
    int target;         // 0 = единственный игрок, -1 = нет
    double alertness;   // 1.0 пока видит, убывает без LOS
    double timer;       // секунды до следующего перехода/атаки
    double leash;       // эффективный радиус (копия AiConfig::leash)
    Vec3d home;         // абсолютная Thing::position, не меняется
    Vec3d position;     // home, пока агент не ушёл в return
    double health;
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
`clamped_ticks`, `invalid_damage`.

`events()` возвращает упорядоченный журнал `AiEvent { type, thing_id, from, to,
value, tick }`; `tick` 1-based, у `attach` — 0. Типы: `attach`, `sight`,
`lost_sight`, `state`, `attack`, `damage`, `death`, `leash`, `arrived`.
`clear_events()` очищает только журнал, не состояние.

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
max_dt` зажимается (`clamped_ticks`); `set_player`/`place` с NaN/inf
возвращают `false`; `damage` с NaN/0/отрицательным числом отклоняется. В
состоянии нет OOB-индексов: `agent(id)` — линейный поиск, массивы не
адресуются чужими id.

---

## 3. Константы: proven / hypothesis

`AiConfig` намеренно не читает `THING_TYPES`: привязка полей `.soe` к этому
state machine — следующий шаг. Все числа ниже — **гипотезы** для заготовки,
кроме самого тика.

| константа | значение | статус | почему |
|---|---:|---|---|
| `kAiTickHz` | 91 | **proven** | тот же фиксированный тик, что `Things`/`Doors`/HUD (docs/THINGS_CPP.md, docs/PLAYER_CONTROLLER.md) |
| `sight_range` | 512 | hypothesis | удобный радиус уровня; `Thing::search_radius` уже заполняется из `+0x58`, но AI его пока не использует |
| `attack_range` | 96 | hypothesis | масштаб уровня; `+0x1cc` — порог «цель в зоне» (default 200), а не дальность атаки; пока не подключён |
| `leash` | 384 | hypothesis | 0.75 × sight; оригинальный leash не разобран |
| `eye_height` | 10 | hypothesis | LIGHT chassis = 10.0 (docs/PLAYER.md §5); у вещей высота глаза не доказана |
| `alert_seconds` | 0.5 | hypothesis | задержка «увидел → атака» |
| `lose_sight_seconds` | 3.0 | hypothesis | время забывания цели |
| `patrol_after_seconds` | 2.0 | hypothesis | idle → patrol без цели |
| `attack_period` | 0.75 | hypothesis | период события атаки; `+0x1d0` — не период, а лаг истории цели, поэтому он не подходит |
| `return_speed` | 200 | hypothesis | скорость возврата, не ориентация движения `+0x1b4` |
| `health` | 100 | hypothesis | HP вещи; `+0x1d4` — max HP, 0 означает неуязвимость; пока не подключён |
| `max_dt` | 0.25 | hypothesis | защита от «огромного dt» |
| `los_skin` | 1e-3 | hypothesis | допуск на конечную точку луча |

`Things` теперь отдаёт `movement_type`, `search_radius`, `target_zone_threshold`,
`target_history_offset` и `max_health` (`docs/THINGS_CPP.md`), но `Ai` пока
использует fallback-константы: привязка состояния/переходов к этим полям —
следующий этап. Значения в `.sbt` уже масштабированы, повторно их умножать
нельзя.

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
| два прогона | `same_agent` для всех полей и совпадение журнала на 300 тиках |

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
* LOS использует проверенный `CollisionWorld::raycast` (docs/COLLIDE.md), а не
  собственную геометрию.
* CTest `thing_ai` — **0.35 с**, label quick. `regress.py` — **113/113**.

Ограничения доверия: golden'ы AI (радиусы, таймеры, скорости) — это golden'ы
**этой заготовки**, а не оригинала. Они не доказывают поведение `gundx.exe`.

---

## 6. Что осталось

1. **Типы вещей**: подключить `THING_TYPES +0x1b4` (movement), `+0x1cc`
   (attack range), `+0x1d0` (period), `+0x1d4` (health), `+0x1c8`/`+0x1c4`
   (счётчик попаданий/событие смерти), `+0x1e8` (expire). Для этого `Things`
   должен отдавать разрешённые поля типа, а не только имя/модель.
2. **Навигация**: patrol waypoint'ы (`+0x320`), path/pathloop/pathrandom,
   collision-aware движение, высота/пол через `floor_at`, гравитация.
3. **Бой**: урон игроку, оружие (`+0x21c`), снаряды, звук (`+0x1fc`),
   анимация; сейчас `attack` — только событие.
4. **Оригинальный AI**: разобрать `0x473eed`/`0x473f07`/`0x475615`, leash,
   состояния и переходы движка; сверить с `.soe`-типами на dm1/level0.
5. **Несколько игроков/целей**: `target` сейчас слот 0; нужен выбор цели,
   приоритеты и DM-респавн.
6. **Сохранение**: AI-состояние не входит в `saved*.sbt`; решить, какие поля
   писать (docs/SAVE_BLOCKS.md).
