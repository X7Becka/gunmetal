# Снаряды: `movement_type == 6` и RGM (`8`)

Статус: ядро `game/projectile.*` реализовано и покрыто синтетическими тестами;
загрузка полей `Thing` из `THING_TYPES` подключена, есть чистый мост
`projectile_spawn_from_shot()`: успешный `ShotResult` (включая заряд) →
`ProjectileSpawn` с полями `Thing`, blast-мост
`apply_projectile_blast()` (`bounced`/`expired` → `apply_weapon_blast`) и
отдельный режим RGM/homing (`movement_type == 8`, флаг `ProjectileState::homing`).
Общий игровой цикл (`Projectiles::tick` в `main.cpp`, хранение per-projectile
результатов, remove) по-прежнему не подключён.

## 1. Что доказано

Снаряд типа 6 — запись `Thing`, которую двигает `0x470c70` → `switch(+0x1b4)` →
`case 6` → `0x4752d0`. Ветка RGM (`movement_type == 8`, `0x475930`) — отдельный
шаг, описанный в §6; дроны и общая объектная система сюда не входят.

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
7. «Отдыхающий» контакт — **clean-room добавка** к шагу 8: если нормаль
   смотрит вверх (`n.z > 0`, пол) и снаряд входит в поверхность не глубже
   одного шага гравитации (`0 < -dot(desired - pos, n) <=
   0.2·gravity·level_gravity·dt²`), это не удар: центр выталкивается по
   нормали, `vz = 0`, `hits`/`damping`/события не меняются, статус `flying`.
   Причина: отражённая скорость такого контакта меньше, чем гравитация
   добавит за следующий тик, поэтому снаряд не может оторваться от пола.
   Оригинал в этой ситуации каждый тик декрементил `hits`, делил `damping`
   и (из-за условия collide) не постил ничего — «залипание» QA PRJ-1.
   Стены/потолки правило не затрагивает.
8. Контакт:
   * `hits > 0`: декремент; при нуле — expire, без collide;
   * иначе отражение `dir = normalize(inc - 2(inc·n)n)`, `vz = 0`,
     `speed *= damping`;
   * collide-event постится только при `dt <= |vz_old| + speed_old` и с
     **позицией до шага**;
   * если после отражения `|speed·dir.x| <= 0.04` и `|speed·dir.y| <= 0.04`,
     то `damping *= 0.5`, затем `speed *= damping` (`0x4752d0`; делится
     ровно на контактах, не каждый тик).
9. `pos = resolved + normal * 1e-4` (contact skin, чтобы касание не считалось
   новым попаданием на следующем тике).

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
const int index = projectiles.spawn(spawn, &queue);
if (index < 0) { /* duplicate thing_id: spawn a fresh runtime Thing */ }
projectiles.tick(1, targets, world, &queue);
```

`projectile_step()` — чистая функция, возвращает `ProjectileTickResult` с
событиями в порядке оригинала. `dt` — накопленные тики: `Projectiles::tick(5)`
делает один шаг с `dt=5` (как `0x470c70`), а не пять шагов по одному; поэтому
`tick(5)` и `projectile_step(dt=5)` дают побитово одинаковое состояние и
одинаковый список событий. `post_projectile_events()` — тонкий слой в
`EventQueue`: постит **все** события результата, поэтому вызывается ровно один
раз на результат (повторный вызов с тем же `ProjectileTickResult` запостит их
снова; `Projectiles::tick` вызывает его один раз). `ProjectileTargets` — spans;
отсутствующие записи считаются отсутствующими. `Projectiles::spawn` отклоняет
повторный `thing_id` (`-1`): каждый снаряд должен иметь уникальный runtime-id
из `Things::spawn`, а не id шаблона типа.
`ProjectileState::spent` означает, что expire уже постился; `tick` такие записи
пропускает и событий больше не даёт. `dead` — удаление из мира;
`Projectiles::remove(id, &queue)` постит `destroy_event` ровно один раз
(повторный `remove` возвращает `false` и ничего не постит), а expire сам запись
не удаляет. `ProjectileTickResult::already_spent` отличает первый expire
(`false`) от повторного шага уже `spent` записи (`true`, статус `expired`,
событий нет) — по нему blast-мост не взрывает снаряд второй раз.

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
    int index = projectiles.spawn(*spawn, &queue);
    if (index < 0) { /* unique runtime id required */ }
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

Мост смотрит только на `movement_type`: для 8 он выставляет `spawn.homing`, но
не ищет цель. Цель (`target_index` или `target_point`), `homing_turn` и
`destroy_on_contact` задаёт вызывающий слой; без цели RGM летит прямо.

### RGM / homing

```cpp
gm::ProjectileSpawn spawn = /* ... из projectile_spawn_from_shot ... */;
spawn.homing = true;             // или movement_type == 8 в Thing
spawn.target_index = 0;          // игрок 0; вещь = 8 + index
// либо: spawn.target_point = aim; spawn.has_target_point = true;
spawn.homing_turn = 0.2;         // радиан за тик; 0 = мгновенный разворот
spawn.destroy_on_contact = true; // контакт с миром = destroy, не expire
int index = projectiles.spawn(spawn, &queue);
```

`target_index` разрешается из `ProjectileTargets` на каждом шаге (игроки
`0..7`, вещи `8+index`), поэтому движущаяся цель подхватывается, если вызывающий
слой обновляет spans. `target_point` — снимок, его можно обновлять между
тиками через `Projectiles::by_thing_id(...)`. Если заданы оба, побеждает
индекс; если индекс отсутствует в `targets`, используется точка.

### Мост в blast-слой

```cpp
#include "game/projectile.h"

gm::ProjectileTickResult result =
    gm::projectile_step(state, targets, world, ticks);
gm::ProjectileBlastContext blast_context; // вода/земля/DM, source ids
gm::ProjectileBlastTargets blast_targets; // spans игроков и вещей
const std::size_t hits = gm::apply_projectile_blast(
    weapon, result, result.status == gm::ProjectileStatus::expired,
    blast_context, blast_targets);
```

`apply_projectile_blast(weapon, result, expired, context, targets, out)` —
тонкий мост к `apply_weapon_blast`; API самого `apply_weapon_blast` не менялся:

* `bounced` → blast в `result.position`; ветку задаёт `expired`
  (`false` — `impact_blasts`/collide);
* `expired` → то же, но обычно `expired=true` (`expire_blasts`); если
  `result.already_spent`, возвращается 0 и blast не применяется — иначе
  `spent`-снаряд взрывался бы на каждом следующем тике;
* `proximity` → **по умолчанию ничего**: таблица близости постит собственное
  событие (`THING_TYPES+0x21c`), проверенной связи этого события с
  `impact_blasts`/`expire_blasts` нет. `context.allow_proximity = true` —
  явное синтетическое решение вызывающего;
* `flying`/`destroyed` → ничего.

Возвращается число применённых hits (`BlastDamageResult::hits.size()`); `out`
(необязательный) получает полный `BlastDamageResult` с `chain_reaction`.
Контекст (`underwater`, `from_ground`, `deathmatch`, `source_player`,
`source_thing`, `power_up`) и target-spans собраны в
`ProjectileBlastContext`/`ProjectileBlastTargets`. Функция не хранит состояния:
её вызывают один раз на `ProjectileTickResult`, а не повторно с тем же
bounce-результатом (каждый отскок — отдельный контакт).

## 4. Ограничения / что осталось

1. **Таблица близости** (`THING_TYPES+0x218`) загружается в `Thing` /
   `ThingTypeEvents`: count 0..8, event (`+0x21c`), low (`+0x23c`), high
   (`+0x25c`), cooldown (`+0x27c`), offset (`+0x29c/+0x2bc/+0x2dc`). Для
   гранаты 28 тест фиксирует `event=-30095, high=20`; у ракеты таблица пуста.
2. **Shape 2**: `sphere_cast` с радиусом `max(box)` — консервативное
   приближение; точного box-sweep в `CollisionWorld` нет.
3. **RGM / homing** (`movement_type == 8`) реализован минимально (§6):
   наведение на точку/индекс, плавный или мгновенный поворот, destroy на
   контакте/достижении. Точная x87-математика `0x475930` (yaw/pitch-клэмп,
   `+0x130`, проверка видимости цели, финальный `FUN_00456de0`) не
   воспроизведена; `homing_turn` задаёт вызывающий слой.
4. **Игровой слой**: `Weapons::fire` → `projectile_spawn_from_shot` →
   `Projectiles::spawn/tick` и blast-мост `apply_projectile_blast` собраны и
   покрыты тестами, но в `main.cpp` цикл ещё не вызывает `Projectiles::tick`,
   не хранит per-projectile результаты (`tick` возвращает `void`) и не
   применяет blast/remove. Для RGM не подключены поиск реальной цели и blast
   по `ProjectileStatus::destroyed`; `EventDispatcher` для событий снаряда
   тоже не подключён.
5. `collide` при контакте и `expire` при hits/lifetime разделены; не постить
   collide на последнем попадании.
6. **Условие collide** в C++ — `dt <= |vz_old| + speed_old` (тики слева,
   ед/тик справа). Дизассемблер `0x4752d0` сравнивает `|vz| + speed` с полем
   записи `+0x150` (`FCOMP`, `0x47551b`), а оно копируется из
   `THING_TYPES+0x68` (`0x46df11`) и в `level0.sbt` равно 1500/2000/6000,
   т.е. оригинал сравнивает не `dt`. Семантика `+0x68` не доказана
   (QA COL-1), поэтому порт пока оставлен как есть; тесты фиксируют именно
   текущее правило.
7. **Триггер blast — статус, а не событие.** В оригинале blast живёт в
   обработчике collide/expire-события. `apply_projectile_blast` реагирует на
   `ProjectileStatus::bounced`, в том числе когда collide-событие не постилось
   (гейт из п. 6 или `collide_event == 0`). «Отдых» на полу сюда не попадает
   (статус остаётся `flying`), но это clean-room упрощение; точная привязка
   потребует причины события в `ProjectileTickResult`.

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

`test_projectile_floor_bounces` (там же) — регресс на «залипание» PRJ-1:

* пол z=0, радиус 10, `speed=40`, `gravity=1`, `damping=0.8`, `hits=5`:
  минимум три настоящих отскока, после каждого следующего тика `z` растёт,
  центр остаётся на contact skin (`z > 10`, `z ≈ 10`);
* `damping *= 0.5` и `hits -= 1` происходят только на тиках отскока
  (`0.8 → 0.4 → 0.2 → 0.1`), на остальных тиках не меняются;
* collide-событий не больше, чем отскоков, и ни одного на «тихих» тиках;
  событий на тик не больше одного;
* последний отскок оставляет скорость меньше шага гравитации → «отдых»:
  следующие 20 тиков не тратят hit, не меняют `damping`/`z` и ничего не
  постят (до правила это был hit каждые 1–2 тика);
* `Projectiles::tick` даёт ровно те же collide-события и то же состояние;
* медленный контакт со стеной (нормаль не пол) всё ещё отскок, а не «отдых».

`test_projectile_accumulated_dt` (там же):

* `Projectiles::tick(5)` и `projectile_step(dt=5)` совпадают по позиции,
  направлению, speed, `vz`, hits, lifetime и событиям; одна 5-тиковая ступень
  даёт `z=-5` против `z=-3` у пяти однотиковых;
* отскок внутри `dt=5` одинаков в обоих путях (один collide, `speed=7.2`);
* lifetime 5 при `tick(5)` — ровно один expire, запись `spent`, но не `dead`;
  повторные `tick` событий не дают;
* ручной путь: expire-событие в результате одно, `post_projectile_events()`
  постит один record, повторный шаг `spent` даёт пустой список.

`test_projectile_remove_events` (там же):

* create/expire/destroy: expire постится один раз, после `spent` тики молчат;
* `remove(…, &queue)` постит destroy ровно один раз, второй `remove` —
  `false` и без событий, `tick` после `dead` ничего не постит;
* удаление до истечения lifetime — тоже ровно один destroy.

`test_projectile_rgm_homing` (там же):

* цель сбоку (`target_point = (40,20,0)`), `speed=2`, `homing_turn=0.2`:
  уже на первом тике `direction` уходит от `+x` к `+y`, расстояние до цели
  не растёт, за 100 тиков наступает `destroyed`;
* при достижении позиция равна цели, `dead && spent`, ровно одно событие
  `destroy_event` (не `expire_event`), повторный шаг молчит;
* `target_index = 8` берёт позицию вещи из `ProjectileTargets` и игнорирует
  ложный `target_point` позади;
* без цели — прямое движение (`x += speed*dt`), без событий;
* при `homing == false` поля наведения инертны: тип 6 летит по-старому;
* контакт со стеной через `Projectiles::tick`: destroy в точке контакта
  (`x ≈ 19` при стене `x=20`, радиусе 1), один destroy, `remove()` — `false`;
* `projectile_spawn_from_thing` включает `homing` для `movement_type == 8`,
  но не выдумывает цель (`target_index == -1`, `has_target_point == false`).

## 6. RGM / homing (`movement_type == 8`, `0x475930`)

Это отдельная функция, а не `case 6`; всё ниже — из декомпиляции `0x475930` и
её вызовов. Цель хранится в runtime-поле `+0xa4` (`DAT_007ac460`), её позиция
берётся через `FUN_004277cd`; индекс игрока — `0..7`, вещи — `8+index`
(допустимый диапазон оригинала `0..0x10b`). При `param_4 != 0` сначала
выполняется проверка видимости (`FUN_0043c51f`): если цель перекрыта,
направление на этом тике не обновляется.

Наведение: `dir = normalize(target - pos)`, затем два угла (yaw/pitch)
плавно подтягиваются к углам этого вектора с шагом `dt * turn_rate`; разница
нормализуется через π, после чего направление пересобирается
(`FUN_0041e360`). Шаг: `desired = pos + speed*dt*dir + accumulated_offset`
(накопленные смещения обнуляются), затем swept-запрос `FUN_0043c51f`. Если
параметр контакта меньше `speed*dt`, оригинал делает `pos += dir*t`,
вызывает `FUN_00470290` (**destroy**) и возвращает 0; иначе синхронизирует
ориентацию/позицию и возвращает 1. Гравитации, `damping`, `hits` и таблицы
близости в этой ветке нет; финал — destroy (`0x470290`), а не expire
(`0x4703a0`).

Минимальная C++-реализация:

* поля `homing`, `target_index`, `target_point`, `has_target_point`,
  `destroy_on_contact`, `homing_turn` есть в `ProjectileSpawn` и
  `ProjectileState`; `Projectiles::spawn` копирует их, адаптеры
  `projectile_spawn_from_thing`/`projectile_spawn_from_shot` включают
  `homing` при `Thing::movement_type == 8`;
* `homing_step` вызывается после lifetime/timeout и до proximity; таблица
  близости, `hits_left` и `collide_event` не используются;
* `target_index` разрешается из `ProjectileTargets` каждый тик (игроки
  `0..7`, вещи `8+index`), `target_point` — запасная цель/снимок; если
  индекс задан, но отсутствует в `targets`, берётся точка;
* `homing_turn > 0` — максимум `homing_turn*dt` радиан за шаг (Родриг,
  кратчайшая дуга, детерминированная ось для антипараллельных векторов);
  `homing_turn <= 0` — мгновенный разворот на цель;
* шаг `speed*dt`; при `distance <= speed*dt` и совпадении направления
  (или `distance == 0`) — destroy в целевой точке; без цели — прямое
  движение;
* при `destroy_on_contact` контакт `CollisionWorld::sphere_cast` даёт destroy
  в `hit->centre` (радиус сферы/`max(box)`); `false` означает «пролетать
  сквозь уровень», достижение цели всё равно уничтожает;
* destroy ставит `spent` и `dead`, постит только `+0x1f8`; `Projectiles::tick`
  дальше запись пропускает, `Projectiles::remove` возвращает `false`.

Отличия от оригинала (осознанные упрощения): нет проверки видимости цели,
точной x87-математики yaw/pitch (включая поле `+0x130`), `FUN_00456de0` и
накопленных смещений; поворот считается по вектору, а не по двум углам, поэтому
на больших `dt` дуга может отличаться. Цель и `homing_turn` в игре пока никто
не заполняет — это работа следующего слоя.

Blast-мост:

`test_projectile_blast_bridge` (там же):

* синтетическая граната: `bounced` → ровно один blast в `result.position`,
  выбран `impact_blasts` (event 11), HP вещи уменьшился; `flying`/`destroyed`
  — 0 hits;
* `expired` (последний hit) → ровно один blast из `expire_blasts` (event 22);
  повторный шаг уже `spent` записи даёт `already_spent`, пустые события и
  0 hits (HP не меняется); lifetime-expire ведёт себя так же;
* `proximity` по умолчанию 0 hits и не трогает HP; с
  `allow_proximity = true` — один blast;
* реальный `ROCKET` из `level0.sbt`: bounce и expire дают по 2 hits, повторный
  expire после `spent` — 0.
