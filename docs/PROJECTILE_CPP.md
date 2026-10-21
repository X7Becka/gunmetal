# Снаряды: `movement_type == 6`

Статус: ядро `game/projectile.*` реализовано и покрыто синтетическими тестами;
загрузка полей `Thing` из `THING_TYPES` подключена, и есть чистый мост
`projectile_spawn_from_shot()`: успешный `ShotResult` (включая заряд) →
`ProjectileSpawn` с полями `Thing`. Общий игровой цикл (`Projectiles::tick` в
`main.cpp`, blast/RGM) по-прежнему не подключён.

## 1. Что доказано

Снаряд — запись `Thing`, которую двигает `0x470c70` → `switch(+0x1b4)` →
`case 6` → `0x4752d0`. Ветка RGM (`movement_type == 8`, `0x475930`), дроны и
общая объектная система сюда не входят.

Алгоритм одного тика (`dt` — целое число тиков 91 Гц):

1. Проверка lifetime: `life -= dt`; при `< 1` — expire-event и выход.
2. Проверка timeout (для снарядов поставочных типов не задан).
3. Таблица близости: сначала игроки, затем вещи; сравнение — 3D-расстояние
   между центрами. `0x4752d0` читает **только запись 0** (`event/low/high/
   cooldown`); `low`/`offset`/остальные записи загружаются, но в этой ветке
   не используются. При попадании постится событие и **движение тика не
   выполняется**. Цель игрока кодируется `0..7`, вещи — `8+index`.
4. `vz += 0.2 * gravity * level_gravity * dt` (полунеявный Эйлер).
5. `desired = pos + speed*dt*dir`, затем `desired.z -= vz*dt`.
6. Swept-сфера (`CollisionWorld::sphere_cast`) вдоль `desired - pos`.
7. Контакт:
   * `hits > 0`: декремент; при нуле — expire, без collide;
   * иначе отражение `dir = normalize(inc - 2(inc·n)n)`, `vz = 0`,
     `speed *= damping`;
   * collide-event постится только при `dt <= |vz_old| + speed_old` и с
     **позицией до шага**.
8. `pos = resolved`.

`expire` не удаляет запись: `0x4703a0` только обнуляет lifetime и постит
событие; удаление — отдельный `0x470290` (destroy).

## 2. Единицы и данные

* `speed` — world units per tick (`THING_TYPES+0x10`); `×91` только на границе
  с `Weapon::projectile_speed`.
* `gravity` — `THING_TYPES+0x78`; `level_gravity` — `SET_LEVEL_GRAVITY`
  (`0x80f840`), по умолчанию **0**. В `level0`/`dm1` граната летит по прямой.
* `damping` — `+0x80`; `shape` — `+0x1a0` (1 сфера, 2 бокс); радиус —
  `+0x30` (`params[4]`), бокс — `+0x38/+0x40/+0x48`.
* `hits` — `+0x1c8` (0 = без лимита), lifetime — `+0x1e8`.
* события — `+0x1c4` collide, `+0x1ec` expire, `+0x1f8` destroy,
  `+0x1c0` timeout, `+0x1f0` create.

Значения `level0.sbt` (тип → speed/gravity/damping/shape/radius/hits/life):
Grenade 28 — 2/1/0.8/1/10/0/364, Rocket 31 — 18/0/0.8/2/box/1/728,
RGM 32 — отдельная ветка. Проверяется в `test_things`.

## 3. API

```cpp
#include "game/projectile.h"

gm::ProjectileSpawn spawn;
spawn.position = eye;
spawn.direction = dir;             // unit
spawn.speed = speed_per_tick;      // TT+0x10 + charge
spawn.gravity = thing.gravity;
spawn.damping = thing.damping;
spawn.radius = thing.radius;
spawn.hits_left = thing.hits_left;
spawn.life_ticks = thing.expire_ticks;
spawn.collide_event = thing.collide_event;
spawn.expire_event = thing.expire_event;

gm::Projectiles projectiles;
projectiles.spawn(spawn, &queue);
projectiles.tick(1, targets, world, &queue);
```

`projectile_step()` — чистая функция, возвращает `ProjectileTickResult` с
событиями в порядке оригинала. `dt` — накопленные тики: `Projectiles::tick(5)`
делает один шаг с `dt=5` (как `0x470c70`), а не пять шагов по одному. `post_projectile_events()` — тонкий слой в
`EventQueue`. `ProjectileTargets` — spans; отсутствующие записи считаются
отсутствующими. `ProjectileState::spent` означает, что expire уже постился;
`dead` — удаление из мира. `Projectiles::remove(id, &queue)` постит
`destroy_event` (expire сам запись не удаляет).

`projectile_spawn_from_thing()` копирует только proven-поля; таблица близости
пока не заполняется в `Things::load` (см. §4).

### Мост из оружия

```cpp
#include "game/projectile.h"
#include "game/weapons.h"

gm::ShotResult shot = gm::fire(table, arsenal, eye, dir, world, charge);
if (auto spawn = gm::projectile_spawn_from_shot(weapon, shot, &thing_type,
                                                 /*owner_player=*/0,
                                                 /*owner_thing=*/thing_id,
                                                 level_gravity)) {
    projectiles.spawn(*spawn, &queue);
}
```

`projectile_spawn_from_shot()` — чистая функция, ничего не тратит и не постит:

* `speed = shot.projectile_speed / 91` (ед/тик): в `ShotResult` скорость уже
  включает `charge*operand[9]*91`; если там 0/NaN, берётся
  `weapon.projectile_speed`; если и он невалиден — `nullopt`;
* `position = shot.origin` (дуло), а не `shot.point` (точка попадания луча);
  `direction` — направление после разброса, нормализуется, нулевое заменяется
  на `(0,0,1)`;
* из `Thing` копируются `gravity`, `damping`, `collision_shape` (1/2),
  `radius`, `box`, `hits_left`, `expire_ticks` и события `collide/expire/
  destroy/timeout/create`;
* без `Thing` действуют безопасные defaults: `thing_id = -1`, события 0,
  сфера радиуса 0, `hits_left = 0` (без лимита), `life_ticks = 0` (без
  lifetime), `gravity = damping = 0` — снаряд просто летит прямо;
* `nullopt` для отклонённого выстрела, hitscan и отсутствия скорости.

Мост не проверяет `movement_type`: вызывающий слой обязан брать снарядный тип
(6), а RGM/homing (8) в него не входит.

## 4. Ограничения / что осталось

1. **Таблица близости** (`THING_TYPES+0x218`) загружается в `Thing` /
   `ThingTypeEvents`: count 0..8, event (`+0x21c`), low (`+0x23c`), high
   (`+0x25c`), cooldown (`+0x27c`), offset (`+0x29c/+0x2bc/+0x2dc`). Для
   гранаты 28 тест фиксирует `event=-30095, high=20`; у ракеты таблица пуста.
2. **Shape 2**: `sphere_cast` с радиусом `max(box)` — консервативное
   приближение; точного box-sweep в `CollisionWorld` нет.
3. **RGM / homing** (`movement_type == 8`) не реализован: другой финал
   (destroy), угол поворота и `+0x130`.
4. **Игровой слой**: `Weapons::fire` → `projectile_spawn_from_shot` →
   `Projectiles::spawn/tick` собрано и покрыто тестом, но в `main.cpp` цикл
   ещё не вызывает `Projectiles::tick`, а blast по `ProjectileStatus::bounced/
   expired` и удаление (`Projectiles::remove`) остаются за вызывающим слоем.
   `EventDispatcher` для событий снаряда тоже не подключён.
5. `collide` при контакте и `expire` при hits/lifetime разделены; не постить
   collide на последнем попадании.

## 5. Тесты

`test_weapons.cpp::test_projectile_core`:

* прямая скорость, отсутствие гравитации при `G=0`;
* полунеявный Эйлер: `z=-0.2·n(n+1)/2`, `vz=2` после 10 тиков;
* отскок от пола при `dt=1` (старая позиция в collide, `damping*0.5`);
* `hits=1` → expire без collide;
* точная граница lifetime и повторный тик без события;
* proximity: фильтр игрока, `extra=(target<<16)|(8+thing_id)`, движение
  отменяется;
* `Projectiles::spawn/tick` и create/expire-события;
* `projectile_spawn_from_thing` копирует speed/gravity/damping/shape/box.

`test_projectile_shot_bridge` (там же):

* `fire(..., charge)` → `projectile_spawn_from_shot`: `speed = 637/91 = 7`
  ед/тик, позиция дула, нормализованное направление, `life_ticks/hits/events`
  из `Thing`, `shape`/`box`, `level_gravity`;
* без `Thing` — безопасные defaults (`thing_id=-1`, события 0, сфера r=0,
  без hits/lifetime);
* fallback скорости на `weapon.projectile_speed`, фолбэк направления на
  `(0,0,1)`; `nullopt` для `!ok`, hitscan и невалидной скорости;
* сценарий `Projectiles` + `EventQueue`: spawn → отскок (один collide) →
  expire (один expire), повторные тики после `spent` событий не дают;
* ручной путь `projectile_step` + `post_projectile_events`: expire постится
  ровно один раз, повторный шаг возвращает пустой список событий.
