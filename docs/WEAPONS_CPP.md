# `game/weapons.*` — runtime оружия

Статус: реализовано, тест `weapons` (label `quick`, ~0.11 с), `regress.py`
113/113. Игра только читается: загрузчик берёт `level0.sbt` и сейв, ничего не
пишет.

Модуль — чистая логика без SDL, рендера, звука и часов:

* таблицы уровня `DEFINE_WEAPONS` / `DEFINE_AMMO` → `Weapon` / `AmmoType`;
* блок игрока из сейва (`ammo[12]`, `weapon_mask`, `weapon_slot`) → `Arsenal`;
* фиксированный тик 91 Гц: кулдаун, перезарядка, переключение слота;
* выстрел `hitscan` или прямолинейный «снаряд» через
  `CollisionWorld::raycast`;
* детерминированный разброс, отдача, урон/броня.

Реализация: `engine/src/game/weapons.h`, `engine/src/game/weapons.cpp`.
Тест: `engine/tests/test_weapons.cpp`.

---

## 1. API

```cpp
#include "game/weapons.h"

// GameState — разобранный сейв (game/state.h). level_path — <game>/level0.sbt.
std::optional<gm::Weapons> table =
    gm::Weapons::load(state, game / "level0.sbt", &error);

gm::Arsenal arsenal = table->initial();   // ammo/mask/slot из сейва
gm::ShotResult shot = gm::fire(*table, arsenal, eye, dir, world, charge);
gm::tick(*table, arsenal, 1);              // один тик 1/91 с
gm::reload(*table, arsenal, arsenal.current);
gm::switch_slot(*table, arsenal, 1);
```

### Данные

```cpp
struct Weapon {
    std::string name, icon;
    int slot, id;
    int fire_mode, fire_period_ms, fire_ticks;
    int ammo_type, ammo_per_shot;
    uint32_t projectile_id, projectile_alt, state, price, weight, flags;
    int class_index;
    double charge_rate, spread, spread_y, range;
    int muzzle;

    // слой гипотез, в DEFINE_WEAPONS полей нет:
    double damage; int magazine, reload_ticks;
    bool hitscan; double projectile_speed;
    int thing_type; double projectile_charge; // THING air branch
    std::vector<Blast> impact_blasts, expire_blasts; // impact/expire
    std::vector<Blast> inline_blasts; // body of IF FIRE_* (EMP rings)
    uint32_t impact_event;            // hitscan impact event id
    double secondary_damage, secondary_range; // nested hitscan (DISINT)
    std::vector<ThingVariant> thing_variants; // PLASMA 36/37/38
    std::vector<Blast> select_blasts(bool expired, bool underwater,
                                     bool from_ground = true,
                                     bool deathmatch = true) const;
    std::vector<Blast> select_blasts_at_power(double power, ...) const;
};

struct ThingVariant {
    double min_power_up; int thing_type;
    std::vector<Blast> impact_blasts, expire_blasts;
};

// CAUSE_BLAST_RADIUS_DAMAGE (op 75): целые сотые, plate < 0 — статус.
struct Blast {
    int32_t plate, hull; int radius; uint32_t flags, event;
    bool no_direction;
    // Guards: neutral op has both flags of a pair true; a taken IF body sets
    // the opposite flag false.  select_blasts picks exactly one context.
    bool air, water;
    bool ground, not_ground;
    bool deathmatch, not_deathmatch;
};

struct ThingVitals {              // runtime THING +0x7ac418..+0x7ac430
    int32_t hp, max_hp;
    std::array<int32_t,4> shields, shield_max;
    bool alive; uint32_t flags, flags2, flags3;
};

struct AmmoType { std::string name; uint32_t max, pickup, flags, lot, price,
                                         weight, rounds_per_unit; };

struct Arsenal {
    std::array<uint32_t, 12> ammo;      // сейв +0x329c, как есть
    std::array<uint32_t, 12> magazine;  // гипотеза, у загруженных = 0
    uint32_t mask;                      // +0x3298
    int current;                        // +0x3294
    std::array<int, 12> cooldown, reload_left;
    std::array<bool, 12> reloading;
    uint64_t seed, shot_index;          // поток разброса
};
```

`Weapons` — неизменяемая таблица уровня плюс `initial()` (снимок загрузки).
`at(slot)` возвращает `nullptr` за границей таблицы; `ammo_max_for(type)` —
лимит из `DEFINE_AMMO`.

### Функции

| функция | поведение |
|---|---|
| `Weapons::load(game, level_path)` | читает `.sbt`, декодирует top-level `DEFINE_WEAPONS`/`DEFINE_AMMO`, копирует `ammo/mask/slot` из `GameState`. Ошибка — `nullopt` + текст (нет файла, нет SCRIPT, нет таблицы, неверная арность/поле). |
| `Weapons::load(level_path)` | только таблицы; `initial()` пустой. |
| `can_fire(table, arsenal, slot)` | слот 0..11, запись в таблице, бит маски, не идёт перезарядка, кулдаун 0, есть патроны/магазин. Возвращает `FireError`. |
| `fire(table, arsenal, origin, dir, world, charge = 0)` | один выстрел. Сначала валидация (ничего не тратится при ошибке), затем списание, кулдаун, разброс, `raycast`, `ShotResult`; для THING скорость = `projectile_speed + charge*projectile_charge*91` (`0x46f0f0`), `charge < 0`/NaN — `bad_charge`. |
| `projectile_spawn_from_shot(weapon, shot, thing, owner_player, owner_thing, level_gravity)` | чистый мост `game/projectile.h`: успешный снарядный `ShotResult` → `ProjectileSpawn`, `speed = shot.projectile_speed/91` ед/тик, позиция `origin`, направление после разброса; из `Thing` — gravity/damping/shape/box/hits/lifetime/события; без `Thing` — безопасные defaults; `nullopt` для отказа, hitscan и невалидной скорости. |
| `blast_damage` / `blast_hull_damage` | целочисленный falloff `value*(radius-d)/radius`; `plate < 0` не масштабируется; `radius <= 0` → 0. |
| `apply_blast` | сферический запрос `0x477280` без LOS; цели-игроки/THING, флаги bit0/2/3, событие на попадание. |
| `apply_blast_damage` | `apply_blast` + `apply_damage_gauge` для игроков и `apply_thing_damage` для THING; возвращает hits с результатами. |
| `apply_weapon_blast` | выбирает `impact_blasts`/`expire_blasts` по контексту (`expired`, вода, земля, DM) и применяет их через `apply_blast_damage`; события остаются в `hits` для `event_post`. |
| `apply_weapon_inline` | применяет `inline_blasts` (EMP-кольца) в точке выстрела; интервалы `SLEEP 22` остаются вызывающему слою. |
| `post_weapon_impact` | постит hitscan `impact_event` в `ShotResult.point` (`event_post`) **только при `shot.ok && shot.hit && weapon.hitscan`**; для оружия без события или промаха — `false`. |
| `apply_thing_damage` | точная ветка `0x470500`: без множителей, дефицит щита в HP, `damaged_event`; `max_health == 0` — иммунитет; при `flags2 & 0x1000` — `low_health_event` (`-17`) на переходе через половину HP. |
| `thing_vitals` / `store_thing_vitals` | мост между runtime `Thing` (`+0x1d4/+0x1d8/+0x1c8`) и `ThingVitals`; позволяет blast’у обновить ту же вещь, которую читает AI/рендер. |
| `apply_thing_damage(Thing&, ...)` | та же ветка `0x470500`, сразу пишет HP/щиты/статусы обратно в runtime `Thing`; мёртвая (`alive == false`) вещь отклоняется; AI видит результат на следующем `sync_vitals`. |
| `apply_thing_damage(Things&, id, ..., EventQueue*)` | ищет живую вещь по id, наносит урон и при `EventQueue*` постит `damaged`/`destroy` из THING_TYPES; мёртвая/неизвестная вещь — `ok=false` без событий. |
| `Weapon::select_blasts(expired, underwater, from_ground, deathmatch)` | копия `expire_blasts` или `impact_blasts` только для выбранного контекста; нейтральные blast’ы проходят всегда. Рантайм вызывает после попадания/истечения. |
| `Weapon::select_blasts_at_power(power, ...)` | то же для PLASMA: `variant_for_power` выбирает TT36 (>=0.66), TT37 (>=0.33) или TT38. |
| `reload(table, arsenal, slot)` | требует `magazine > 0`, бит маски, неполный магазин и резерв. `reload_ticks == 0` — мгновенно. |
| `tick(table, arsenal, ticks)` | уменьшает кулдауны/таймеры, завершает перезарядку. `ticks == 0` — no-op. |
| `switch_slot(table, arsenal, slot)` | 0..11 + бит маски; текущий слот — успешный no-op. Перезарядку не отменяет. |
| `fire_ticks_from_ms(ms)` | вспомогательная: `ceil(ms*91/1000)`, минимум 1 для `ms > 0`; `0 → 0`. `Weapons::load` не использует — `+0x2c` уже тики. |

### `ShotResult`

`ok` — выстрел принят; `fired` — был реальный выстрел; `error` — причина
отказа (`FireError`). Успешный выстрел может промахнуться (`hit == false`).
`distance` на промахе равно `range`; `time` = 0 для hitscan и
`distance / projectile_speed` для снаряда. `spread_x/y` — сэмплы в
`[-1, 1]`, `ammo_left` — магазин при `magazine > 0`, иначе резерв.

---

## 2. Данные: что доказано

`DEFINE_WEAPONS` (потребитель `0x450be0`, запись `0x2a8`) и `DEFINE_AMMO`
(`0x450b10`, запись `0x38`) разобраны в `docs/FORMATS.md` §12.3 и
`docs/SHAKE.md` §7.1. Загрузчик читает те же 17 операндов сигнатуры
`SSSLLLLEEEELLLLDBBDL`:

| d | смещение | поле `Weapon` | статус |
|---|---|---|---|
| 0 | `+0x24` | `fire_mode` | код: switch `0x445ac0`, кейсы 0/1/4/5 |
| 1 | `+0x2c` | `fire_ticks` (сырое значение; `fire_period_ms` — производное) | код: `0x445f02..0x445f08`; `0x44db80` 91 тик = 1 с |
| 2 | `+0x3c` | `ammo_type` | код: индекс `DEFINE_AMMO` |
| 3 | `+0x40` | `ammo_per_shot` | код: `0x445e98` |
| 4 | `+0x30` | `projectile_id` | код: GENERATE entity/event id (`0x445eee`), не указатель на снаряд |
| 5 | `+0x34` | `projectile_alt` | код: loop/secondary event id (`0x445f61`), не второй снаряд |
| 6 | `+0x38` | `state` | код: stop-loop event id, копируется в `player[0x331c]` |
| 7 | `+0x4c` | `id` | значения |
| 8/9 | `+0x44/48` | `price/weight` | код |
| 10 | `+0x28` | `flags` | код (биты 1/0x40/0x80) |
| 11 | `+0x294` | `class_index` | значения (0..8) |
| 12/13 | `+0x00` | `charge_rate` | код (f64) |
| 14/15 | `+0x08` | `spread` | код: tan конуса, ось 1 |
| 16/17 | `+0x10` | `spread_y` | код: tan конуса, ось 2 |
| 18/19 | `+0x18` | `range` | код: верхняя граница `0x477f90` |
| 20 | `+0x20` | `muzzle` | код: 0..7 |

`DEFINE_AMMO`: `L0 → +0x04` — лимит запаса (в `level0.sbt`: BULLETS 5000,
CANNON SHELLS 180, FLECHETTES 100, …). `ammo_type` оружия указывает на эту
таблицу; `MACHINE GUN → BULLETS`, `CANNON/AUTOCANNON → CANNON SHELLS`.

### 2.1. Что реально создаёт снаряд

`weapon[+0x30]` — не объект, а id GENERATE-события/сущности. Для
`FIRE_SOURCEPLAYER_THING` (condition `cid=20`, обработчик `0x45e19d`) цепочка:

```text
0x45e19d (FIRE_SOURCEPLAYER_THING)
  -> 0x46ea40 (fire thing)
     -> 0x46d8b0 (create thing from THING_TYPES[type])
```

Скорость берётся не из `DEFINE_WEAPONS`:

```text
speed = THING_TYPES[type].+0x10 + charge * operand[9]   // 0x46f0f0
pos  += speed * dt * dir                                // 0x4752d0
```

`THING_TYPES +0x10` для `ROCKET` = 18, `MRL` = 25, `NUKE` = 10 (dm1).
`BULLET`/`LASER` — **hitscan**: `0x478520`/`0x478be0`, урон из операндов
`L2`/`L4`, impact event отдельно. `Weapons::load()` теперь выбирает hitscan
для BULLET/LASER и берёт урон из подскрипта; THING остаётся объектом.

Сейв (`docs/SAVE_FIELDS.md` §9.2, `docs/FORMATS.md` «Оружие и патроны»):

* `+0x3294` — выбранный слот (0..11 в сейве);
* `+0x3298` — маска `1 << slot`;
* `+0x329c[12]` — патроны по слотам; читаются `0x4455ad` как
  `[slot*4 + 0x81c2dc]`.

В таблице уровня 19 записей, а сейв адресует 12 слотов. Это не ошибка
загрузчика: `Weapons` хранит все 19 записей, но `Arsenal`/`can_fire`
ограничены 12 слотами сейва. Слоты 12..18 остаются доступными только через
`table.at()` (документированная граница формата сейва, не домысел о балансе).

### Таблица `level0.sbt`

| # | имя | mode | ticks | ms | ammo | /выстр | range | spread1 | spread2 | proj id |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | MACHINE GUN | 1 | 17 | 187 | 0 | 1 | 7000 | 0.140541 | 0.466308 | 1000000 |
| 1 | CANNON | 1 | 80 | 879 | 1 | 1 | 8000 | 0.140541 | 0.466308 | 1000001 |
| 2 | AUTOCANNON | 1 | 20 | 220 | 1 | 1 | 8000 | 0.140541 | 0.466308 | 1000002 |
| 3 | RAILGUN | 1 | 50 | 549 | 2 | 1 | 9999 | 0.0874887 | 0.466308 | 1000003 |
| 4 | GRENADE | 2 | 40 | 440 | 3 | 1 | 800 | 0 | 0 | -30028 |
| 5 | VIRAL AGENT | 2 | 40 | 440 | 4 | 1 | 700 | 0 | 0 | -30029 |
| 6 | FLAMETHROWER | 1 | 7 | 77 | 5 | 1 | 700 | 0.249328 | 0 | 1000005 |
| 7 | ROCKET | 1 | 80 | 879 | 6 | 1 | 9999 | 0.105104 | 0.466308 | 1000006 |
| 8 | RGM | 1 | 80 | 879 | 7 | 1 | 9999 | 0.57735 | 1.19175 | -30032 |
| 9 | TAC NUKE | 1 | 80 | 879 | 8 | 1 | 9999 | 0.105104 | 0.36397 | 1000010 |
| 10 | MRL | 1 | 40 | 440 | 9 | 3 | 9999 | 0.140541 | 0.700208 | -30033 |
| 11 | LASER | 1 | 2 | 22 | 10 | 10 | 9000 | 0.249328 | 0.57735 | 1000011 |
| 12 | FUSIONBEAM | 1 | 1 | 11 | 10 | 60 | 6000 | 0.140541 | 0.466308 | 1000012 |
| 13 | PLASMA CANNON | 4 | 30 | 330 | 10 | 20 | 6000 | 0.0874887 | 0.466308 | -30038 |
| 14 | MICROWAVE GUN | 1 | 15 | 165 | 10 | 100 | 3000 | 0.36397 | 1.19175 | 1000015 |
| 15 | EMP GUN | 1 | 70 | 769 | 10 | 300 | 500 | 0.57735 | 1.73205 | 1000016 |
| 16 | DISRUPTORFIELD | 1 | 100 | 1099 | 10 | 600 | 6000 | 0.286745 | 0.57735 | 1000017 |
| 17 | DISINTEGRATOR | 1 | 90 | 989 | 10 | 3000 | 9999 | 0.140541 | 0.466308 | 1000020 |
| 18 | MESON GUN | 1 | 400 | 4396 | 10 | 4500 | 4000 | 0.286745 | 1.19175 | 1000014 |

`-30xxx` — те же `0xffff8axx` в знаковом виде; `Weapon::id` хранит u32.

### Почему `fire_ticks` — это тики, а не миллисекунды

Кулдаун `player[0x3318]` = `weapon[+0x2c]`, а в апдейте игрока
(`0x4453f8..0x44540c`) он уменьшается на тот же фиксированный `dt`, что и
остальная физика:

```
eax = [player+0x81c358]
if (eax > 0) { eax -= dt; [player+0x81c358] = eax; if (eax < 0) = 0; }
```

`0x445f02..0x445f08` кладёт `weapon[+0x2c]` в `player[0x3318]` **без
масштабирования**, а `dt` — тики (91 тик = 1 с, `0x44db80`). Поэтому `17` у
пулемёта — это **17 тиков ≈ 187 мс**, а не 17 мс. Раньше в этом месте была
ошибка: `fire_ticks = ceil(ms*91/1000)` давал 2 тика и ускорял оружие в
8.5 раза.

Теперь `Weapons::load()` читает `fire_ticks` напрямую из `+0x2c`;
`fire_period_ms` — только отображаемое `round(ticks*1000/91)`:

* 17 тиков → 187 мс;
* 80 тиков → 879 мс;
* 2 тика → 22 мс;
* 0 тиков → 0 (без кулдауна).

`fire_ticks_from_ms()` оставлен как вспомогательная функция для **других**
источников, где время действительно задано в миллисекундах; загрузчик
`DEFINE_WEAPONS` её больше не использует.

---

## 3. Гипотезы и консервативные константы

В `DEFINE_WEAPONS` **нет** полей магазина, перезарядки, урона, скорости
снаряда и hitscan. Всё это — слой рантайма; значения по умолчанию подобраны
так, чтобы поведение было ближе всего к доказанному оригиналу.

| поле | значение `load()` | почему |
|---|---|---|
| `magazine` | `0` | 0 = «прямая подача»: оригинал вычитает `ammo[slot]` напрямую (`0x445e98`), магазина в сейве нет. Магазин включается только вручную/синтетикой. |
| `reload_ticks` | `0` | механики перезарядки в оригинале не найдено; 0 = мгновенно, но `reload()` для `magazine == 0` возвращает `no_magazine`. |
| `damage` | резолвится для BULLET/LASER, иначе `1.0` | `load()` ищет `projectile_id` в entity-таблице, декодирует подскрипт и берёт `L2` у BULLET / `L4` у LASER: MG 4.00, CANNON/AUTOCANNON 15.00, RAILGUN 60.00. Для THING остаётся placeholder 1.0, но урон снаряда теперь считается через `apply_blast`/`apply_thing_damage` по таблице §7 Ghidra, а не по этому полю. |
| `hitscan` | резолвится | первый `FIRE_SOURCEPLAYER_BULLET`/`_LASER` → true, `_THING` → false (`0x478520`/`0x478be0`/`0x45e19d`). |
| `projectile_speed` | `THING_TYPES[type].+0x10 × 91` ед/с; `12000` — fallback | `0x46f0f0` читает double `+0x10` (ед/тик), `0x4752d0` делает `pos += speed·dt`; 91 тик = 1 с, поэтому ROCKET 18→1638, MRL 25→2275, NUKE 10→910. `charge*operand[9]` добавляет `fire(..., charge)`. |

`hitscan` для поставочных BULLET/LASER теперь `true`; THING (GRENADE/ROCKET/
PLASMA и т.п.) остаётся `false`. Для `level10.sbt` цепочка `GENERATE` не
развёрнута — там возможен fallback.

Разброс: `spread`/`spread_y` — тангенсы конуса из данных. Рантайм берёт
точку равномерно в квадрате `[-1,1]²` (не в круге), масштабирует на тангенсы
и нормирует. Поток — SplitMix64 от `seed ^ ((slot+1)<<32) ^ (shot_index*φ)`;
один успешный выстрел = один шаг. При `spread == spread_y == 0` направление
не трогается вообще (точное равенство входа после нормировки).

Отдача: поля отдачи в таблицах нет (`docs/SHAKE.md` §7.1). `recoil_velocity_delta`
реализует доказанную формулу `SOURCEPLAYER_RECOIL` (`0x459f4d`):

```
dv.x = -sin(yaw) * |cos(view_pitch)| * D
dv.y = -cos(yaw) * |cos(view_pitch)| * D
dv.z = 0
```

`apply_recoil(player, view_pitch, D)` добавляет это к `player.vel`; величина
`D` — на усмотрение вызывающего (в скриптах кампании `SOURCEPLAYER_RECOIL`
не используется, 0 совпадений по 36 `.sbt`).

Урон/броня: точная модель `0x453b50` реализована в
`apply_damage_gauge(Vitals&, plate, hull, flags, side)`. Значения — целые
**сотые** (`400 = 4.00`), как в скриптах; `plate` идёт в выбранную плиту
(`side` 0..3), `hull` — сразу в корпус. Квадранты: `0=+Y front`, `1=+X right`,
`2=-Y back`, `3=-X left` (`ShieldSide` исправлен; прежний порядок
front/back/left/right был неверен). Если плита уже 0, в корпус уходит
`plate + hull`; при `flags & 1` бьются все четыре плиты, при `flags & 2` —
без деления на 4, при `flags & 4` множители `mult_plate/mult_hull` не
применяются. Переполнение плиты добавляется к корпусу; ровно 0 не считается
разрушением. `plate < 0` — статус: `-1` поджог (`burn_timer += hull*91/100`),
`-2` заморозка/электро. `apply_damage(points, side)` оставлен как
синтетическая обёртка старого API (очки → сотые, один щит → броня); это **не**
оригинальная модель.

Для THING реализован отдельный `apply_thing_damage(ThingVitals&, plate, hull,
flags, side)` — точная ветка `0x470500` без множителей: недостача щита уходит
в HP, есть `damaged_event`. Статусы восстановлены по `0x470500`: `-1`
добавляет `hull*91/100` в `burn_timer` и ставит `0x40` при переходе через
1; `-2` ставит `0x18` (frozen), если `0x40` ещё нет. `ThingVitals` и
runtim’овый `Thing` переносят `burn_timer/status_flags` через
`thing_vitals`/`store_thing_vitals`. `blast_damage`/`blast_hull_damage`
считают `value*(radius-d)/radius` целочисленно; `plate < 0` не масштабируется;
`radius <= 0` возвращает 0 (в оригинале деление на ноль).
`apply_blast` повторяет сферическую проверку `0x477280` **без LOS**: флаги
bit0/2/3, границы игроков 8 / THING 260, событие на каждую цель и
`chain_reaction` при `plate > 0`.

> **Важно:** `Weapons::load` теперь предпочитает **воздушную** ветку при
> `SOURCE_UNDERWATER` (LASER = 3.00, а не 1.00). THING-ветки дополнительно
> разворачиваются по `THING_TYPES` collide/expire; ещё не разобраны
> `SOURCEPLAYER_WEAPON_POWER_UP` (PLASMA TT36/37/38), inline-impact’ы
> hitscan (EMP-кольца, CANNON/FUSION/MICROWAVE/DISRUPTOR) и
> взаимоисключающие ground/DM-ветки TAC NUKE. Таблица §7 отчёта Ghidra
> содержит точные числа для следующего шага.

`Vitals` строится из `GameState` (`vitals_from_game`), так что сейвовые
`armor=10000`, `shields=8000` можно использовать без пересчёта.

#### Impact/THING: числа `level0` (Ghidra §7)

| оружие | прямой | снаряд/событие | blast (воздух) |
|---|---|---|---|
| MACHINE GUN | 4.00 | TT23/24/25, hits 1 | 0@50 (f1, вспышка) |
| CANNON/AUTOCANNON | 15.00 | TT26 | 1500@200; вода 1500@400 |
| GRENADE | — | TT28, speed 2, expire 364 | 8000@350 (f1); вода 8000@500 |
| ROCKET | 0.03 dummy | TT31, speed 18, hits 1, expire 728 | 10000@80 + 12000@256; вода 10000@512 |
| RGM | 0.03 dummy | TT32, speed 18, hits 0, expire 910 | 15000@256 |
| TAC NUKE | 0.03 dummy | TT33, speed 10, hits 1 | ветки DM/вода/земля, до 80000@2250 |
| MRL | 0.00 dummy | TT46 ×3, speed 25, expire 546 | 1000@80 + 2000@300 |
| LASER | 3.00 (вода 1.00) | hitscan | 0@50 (f1) |
| FUSIONBEAM | 3.00 | hitscan | 500@128; вода 500@256 |
| PLASMA | — | TT36/37/38 по заряду | 10800@200 / 5000@150 / 2200@150 |
| EMP | 0 | hitscan 900016 | кольца 0@4500 + 1500@76 + 2000@256 + 2000@512 + 1500@720 |
| DISINTEGRATOR | 0 | TT34, speed 16, hits 1 | 0@200 + hitscan 400.00 в r200 |

Это **не** единая таблица для всех уровней: 33 из 36 используют базовые
ROCKET 8000/10000, а `level0` — 10000/12000; `level20` меняет TAC NUKE.
`Weapons::load` сворачивает collide/expire-блоки `THING_TYPES` и один
`GENERATE` в `impact_blasts`/`expire_blasts`; `Blast` хранит флаги среды
(`air`/`water`), а `select_blasts` выбирает ветку. Для hitscan загрузчик
читает impact-событие из операнда `FIRE` (BULLET `args[1]`, LASER `args[3]`)
и кладёт его blast’ы в `impact_blasts`: CANNON `1500@200`/вода `1500@400`,
FUSION `500@128`, MICROWAVE `hull 500@128`, DISRUPTOR `12000@128`
(`nonpos=1`), MESON `0@512 + 0@4500`. `inline_blasts` — тело выбранного
`IF FIRE_*`; у EMP это 5 колец (`0@4500`, `hull 1500@76`, `2000@256`,
`2000@512`, `1500@720`). `damage=0` теперь настоящий ноль, а не fallback
1.00. DISINTEGRATOR дополнительно получает `secondary_damage=400.00`,
`secondary_range=200` из `EVAL FIRE_SOURCEPLAYER_BULLET` в событии
`900013 → -30112`. `RGM` получает тип 32 из
`ADD_THING_AT_SOURCE_WEAPON_POINT` (не из `FIRE_THING`), поэтому больше не
считается hitscan с уроном 0.03; его fallback-событие `-30105` не попадает
в `impact_blasts`.

TAC NUKE содержит несколько взаимоисключающих top-level IF
(`SOURCE_UNDERWATER`, `SOURCE_FROM_GROUND`, `DEATHMATCH_GAME`). Загрузчик
сохраняет все 15 blast’ов, но помечает их guards’ами; `select_blasts`
возвращает 3 blast’а для каждого проверенного контекста (вода/воздух ×
земля/не-земля × DM/не-DM). Вложенный `IF DEATHMATCH_GAME` даёт 2
DM-only blast’а плюс общий статус, поэтому DM и non-DM дают по 3, а не 5.

`apply_weapon_blast` — runtime-мост: на попадание берёт collide-ветку
(`expired=false`), на истечение — expire (`expired=true`), фильтрует контекст
и применяет каждый blast к игрокам/THING. `projectile_speed` теперь
`THING_TYPES+0x10 × 91` (ед/тик, доказано `0x4752d0`); 12000 остаётся только
fallback’ом для типа без скорости. `fire(..., charge)` уже добавляет
`charge*operand[9]` к скорости; игровому слою остаётся передать заряд.

---

## 4. Golden

Сейвы (`docs/SAVE_FIELDS.md` §11) и `level0.sbt` из установленной игры:

| сейв | slot | ammo[0] | ammo[1] | mask | первый выстрел: ammo_left | cooldown | distance | time | damage |
|---|---|---|---|---|---|---|---|---|---|
| `saved0.sbt` | 0 (MACHINE GUN) | 2000 | 90 | 3 | 1999 | 2 | 7000 | 0 | 4.0 |
| `saved1.sbt` | 1 (CANNON) | 1975 | 87 | 3 | 86 | 8 | 8000 | 0 | 15.0 |

Первый выстрел сделан из `origin = (0,0,0)`, `dir = (1,0,0)` в пустой
`CollisionWorld` (промах, `hit == false`). Оба оружия — hitscan, поэтому
`projectile=false`, `time=0`; урон 400/1500 сотых из подскрипта.

Сэмплы разброса первого выстрела (SplitMix64, seed по умолчанию
`0x243f6a8885a308d3`, `shot_index = 0`):

| сейв | slot | spread_x | spread_y |
|---|---|---|---|
| `saved0.sbt` | 0 | `-0.2421790113617004` | `-0.646618239477736` |
| `saved1.sbt` | 1 | `0.26468357609715865` | `0.61488239516758347` |

Слот входит в seed, поэтому потоки разные. Тест также проверяет
детерминизм (два одинаковых `Arsenal` дают бит-в-бит одинаковые
`direction/spread`), смену потока на втором выстреле и другой seed.

Синтетические голдены:

* helper `fire_ticks_from_ms`: `17 → 2`, `80 → 8`, `400 → 37`, `1000 → 91`,
  `1 → 1`, `0 → 0`; реальные `DEFINE_WEAPONS` значения читаются как тики
  (`MG 17`, `CANNON 80`) и дают кулдаун ровно 17/80 тиков;
* hitscan: стена `x=10`, луч `+X` → `t=10`, `time=0`, `damage=25`;
  луч `-X` (цель позади) → промах, `distance=100`;
* снаряд: скорость 100, стена `x=50` → `distance=50`, `time=0.5`;
* перезарядка: магазин 5, резерв 3, `reload_ticks=7` → после 7 тиков
  `magazine=3`, `ammo=0`;
* отдача: `yaw=0, pitch=0, D=100` → `(0,-100,0)`;
* урон (старый synthetic API): 10 очков во front при щите 8000 → щит 7000,
  броня не тронута; 100 очков в back → щит 0, броня `-2000` сотых; overkill
  1000 очков → `overflow=84000`, броня 0;
* gauge (`apply_damage_gauge`): 400 в плиту 8000 → 7600, ровно 0 не broken;
  следующий 400 → hull 9600; 400 в плиту 300 → broken[0], hull 9900;
  flags 1/3 и перелив; `-1,150` → `burn_timer=136`, урона нет.
* blast: `10000@100`, d=0/50/99/100 → 10000/5000/100/0; `radius=0` → 0;
  `plate=-1,hull=150` не масштабируется; сферический запрос без LOS,
  bit0 исключает источник, bit2/3 — игроков/THING.
* THING: щит 500, HP 1000, plate 1000 → щит 0, HP 500, `broken[0]`;
  летальный blast 10000 → HP 0, `dead`;
* реальный `level0`: LASER (slot 11) — воздушная ветка 3.00, не подводная
  1.00; `Weapons::load` предпочитает воздух при `SOURCE_UNDERWATER`;
  GRENADE (slot 4) — `thing_type=28`, `projectile_charge=16`, не водные 12.
* impact-события из `THING_TYPES` (CTest `weapons`): GRENADE collide `-30094`
  содержит только звук, expire `-30093` → GENERATE `-30096` даёт два
  `CAUSE_BLAST_RADIUS_DAMAGE`: `8000@500` (под водой) и `8000@350` (воздух).
  ROCKET collide=expire `-30103`: прямой `10000@80` плюс GENERATE `-30105` с
  `10000@512` (под водой) и `12000@256` (воздух). Это данные
  `Weapon::impact_blasts`/`expire_blasts`; ветки `SOURCE_UNDERWATER`
  помечены `Blast::air`/`Blast::water`; нейтральные blast’ы имеют оба флага,
  поэтому исполняются в обеих средах. `select_blasts(false, true)` для
  ROCKET даёт прямой `10000@80` + `10000@512`, для воздуха — `10000@80` +
  `12000@256` (раньше терялся нейтральный blast).
* RGM: `projectile_id=-30032` использует
  `ADD_THING_AT_SOURCE_WEAPON_POINT` с типом 32, а не `FIRE_THING`; после
  исправления `hitscan=false`, `thing_type=32`, `expire_blasts` =
  `0@50` + `15000@256` (вода/воздух), как в таблице §7.
* CTest `weapons` дополнительно проходит **все 36** `.sbt`: RGM всегда
  не-hitscan (TT32, пустой impact, 3 expire), ROCKET — 2 blast’а на среду,
  NUKE — 15/13/16 записей (группы A/C/D) и 3/3, 3/2, 4/4 после guards,
  PLASMA — три варианта 36/37/38, CANNON — 15.00 и 3 impact-блоба.

---

## 5. Trust

| утверждение | статус |
|---|---|
| `DEFINE_WEAPONS`/`DEFINE_AMMO` поля и смещения | **доказано** (код + 684 записи, `docs/FORMATS.md` §12.3) |
| период — тики (`+0x2c`), уменьшается на `dt` | **доказано** (`0x4453f8..0x44540c`, `0x44db80`: 91 тик = 1 с) |
| кулдаун = `weapon[+0x2c]`, `>0` блокирует выстрел | **доказано** (`0x445aa8`, `0x445f08`) |
| `ammo[12]`, `mask`, `slot` в сейве | **доказано** (`docs/SAVE_FIELDS.md` §9.2) |
| выстрел ставит `GENERATE`-событие `event_post` (`0x445eee`, `0x445f61`) | **доказано**; фактический spawn — в подскрипте сущности, см. `docs/EVENTS.md` |
| hitscan-путь | **доказан и подключён**: BULLET/LASER (`0x478520`/`0x478be0`), THING — объект |
| магазин/перезарядка | **гипотеза** (в данных и коде не найдено) |
| урон | BULLET `L2`, LASER `L4` резолвятся (MG 4.00, CANNON 15.00, RAILGUN 60.00); THING/impact — отдельные `apply_blast`/`apply_thing_damage`; загрузчик разворачивает collide/expire-блоки `THING_TYPES` (плюс один GENERATE) в `impact_blasts`/`expire_blasts`, `apply_weapon_blast` применяет выбранную ветку |
| скорость снаряда | **доказано**: `THING_TYPES +0x10` — ед/тик (`0x4752d0: pos += speed·dt`), C++ `×91`; `charge*operand[9]` подключён в `fire(..., charge)` |
| разброс — равномерный квадрат, SplitMix64 | **гипотеза рантайма** |
| отдача — горизонтальный импульс | **доказано** (`0x459f4d`, `docs/SHAKE.md` §6); поля в оружии нет |
| урон: плита/корпус, перелив, flags, статусы | **доказано** `0x453b50`; `0x443c7d` — DoT-деградация, а не попадание |

---

## 6. Что осталось

1. **Скорость и тип снаряда.** Цепочка `FIRE_SOURCEPLAYER_THING` →
   `0x46ea40` → `0x46d8b0` найдена; базовая скорость
   `THING_TYPES[type].+0x10 × 91` уже в `projectile_speed`, а
   `fire(..., charge)` применяет `charge*operand[9]`. Мост
   `projectile_spawn_from_shot` переводит `ShotResult` в `ProjectileSpawn`
   (`/91`, поля `Thing`). Осталось вызвать его из игрового слоя и завести
   `Projectiles::tick` в общий цикл.
2. **Hitscan.** Режим выбирается по первому условию подскрипта:
   BULLET/LASER → hitscan, THING → объект. Для BULLET/LASER найдены
   `0x478520`/`0x478be0`, дистанция, impact event, урон `L2`/`L4` и конус
   через `0x477f90`.
3. **Урон и броня.** `0x453b50` (`apply_damage_gauge`), `0x470500`
   (`apply_thing_damage`) и `0x477280` (`apply_blast`) реализованы с
   целочисленным falloff. `apply_weapon_blast` применяет collide/expire
   ветки к игрокам/THING; загрузчик разобрал GRENADE/ROCKET/RGM/MRL/
   NUKE/PLASMA 36/37/38/EMP/DISINT и hitscan impact-события. Осталось
   подключить игровой слой (позиция, цели, `event_post`) и проверить
   level10/20/24 golden’ы на реальных данных.
4. **Магазин/перезарядка.** Механики в оригинале нет; слой оставлен
   выключенным (`magazine=0`, `reload_ticks=0`). Включать только если
   появится доказательство из другой версии/режима.
5. **19 оружий против 12 слотов сейва.** Нужно доказать, как игра адресует
   слоты 12..18 (HUD/магазин/DM). Сейчас они доступны только как записи
   таблицы.
6. **Интеграция.** `fire/tick/reload` не подключены к `main.cpp`/`PlayerState`
   (по условию задачи); отдача — отдельный хук, вызывающий сам решает, когда
   её применить. Нет звука (`0x454900`), вспышки, альтернативного снаряда
   (`projectile_alt`) и состояния `player[0x331c]` в рантайме — поля
   сохраняются в `Weapon`/`ShotResult`, но не проигрываются.
7. **`fire_mode` 0/1/4/5** сохранён как число; ветки 2/3 и семантика
   «одиночный/автоматический» в рантайме не моделируются отдельно —
   скорострельность задаёт только `fire_ticks`.
