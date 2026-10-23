# Игрок стреляет снарядом: попадание и урон (`game/sim_player.*`, `M6.5.1`, срез 2)

Снимок 2026-10-23. Срез 1 (`docs/SIM_PLAYER.md`) доводил план вождения до
принятого выстрела, но у проекта снаряда не было: `PlayerWeaponTick::spawned`
оставался `-1`, ракета/граната не летела, урон не проверялся. Этот срез
закрывает дырку **на готовых API**: `SimPlayer` получает шаблон снаряда,
тикает рантайм, а попадание/взрыв идут через уже проверенные
`Projectiles` и `apply_projectile_blasts`.

Новой физики, новых правил столкновений и правок рантайма нет. Файлы:
`engine/src/game/sim_player.h`, `engine/src/game/sim_player.cpp`; тест
`engine/tests/test_sim_projectile.cpp` (CTest `sim_projectile`, метка
`quick`). CLI-проводки нет: `--sim-projectile` — следующий шаг (§6).

## 1. Что добавлено

```cpp
class SimPlayer {
public:
    void set_projectile(const Thing& projectile, int owner_player = 0);
    void clear_projectile();
    bool has_projectile() const;

    int projectiles_spawned() const; // выстрелы, реально породившие снаряд
    int blast_hits() const;          // сумма damage.hits.size() по взрывам
    int things_damaged() const;      // сколько разных вещей получило урон
};
```

`set_projectile()` — тонкая обёртка над
`GameRuntime::set_player_projectile(projectile, owner_player, 0.0)`
(`engine/src/game/runtime.h`): шаблон хранится в `SimPlayer` и ставится на
рантайм в начале следующего `tick()` (до шва оружия, поэтому уже первый
выстрел порождает снаряд). Вызывать можно сразу после `init()` или между
тиками; повторный вызов заменяет шаблон.

**Режим снаряда меняет ровно одно:** тик дополнительно двигает рантайм
(`GameRuntime::tick(1)`), потому что спавн и шаг снаряда должны попасть в
один тик. Пока шаблон не задан, `tick()` — прежний тик среза 1 (рантайм не
тикается), поэтому старые планы и тесты не меняются. Вызывающий не должен
сам крутить `runtime.tick()` у игрока в режиме снаряда.

Порядок одного тика в режиме снаряда:

```
install_projectile(runtime)                 # set_player_projectile, один раз
yaw += segment.turn * dt
step(...)                                   # player/controller.h
runtime.set_player_eyes({eye})
если segment.fire:
    runtime.update_player_weapon(input, eye)  # 0x4453d0 -> spawn_player_shot
    если spawned >= 0: ++projectiles_spawned_
runtime.tick(1)                             # things/AI/projectiles/world/events
apply_blasts(runtime)                       # apply_projectile_blasts + store
```

`apply_blasts()` берёт `GameRuntime::last_projectile_results()` последнего
тика, выбирает текущий слот (`player_arsenal().current`) и вызывает
`GameRuntime::apply_projectile_blasts(weapon, context, player_vitals,
thing_vitals)`. В `player_vitals` передаётся пустой span (урона игроку в
срезе нет), в `thing_vitals` — копии `thing_vitals(thing)` всех вещей.
Результат пишется назад через `store_thing_vitals()` (виталы вещей —
caller-owned, `docs/RUNTIME_CPP.md` §7). `blast_hits()` складывает
`batch.damage.hits.size()` (игрок тоже считается, если попал в радиус, но
урона не получает), `things_damaged()` — число **разных** слотов вещей с
`max_hp > 0`, по которым прошёл взрыв.

`clear_projectile()` снимает шаблон и у рантайма (`clear_player_projectile`),
поэтому последующий `fire` снова даёт `spawned == -1`.

## 2. Использованные API рантайма

| API | файл | роль |
|---|---|---|
| `GameRuntime::set_player_projectile` | `game/runtime.h` | шаблон `Thing` для шва `0x46ea40` |
| `GameRuntime::update_player_weapon` | `game/runtime.h` / `runtime.cpp` | тик `0x4453d0`: курок -> `fire()` -> `spawn_player_shot()` |
| `GameRuntime::spawn_player_shot` | `runtime.cpp` | `projectile_spawn_from_shot` -> `Projectiles::spawn`, create-событие в очередь |
| `GameRuntime::tick` / `tick_once` | `runtime.cpp` | фаза 3: `projectiles_.tick_results(1, targets, collision, queue)` |
| `Projectiles::tick_results` / `ProjectileState` | `game/projectile.h` | шаг `0x4752d0`, статусы `flying/bounced/expired/destroyed` |
| `GameRuntime::last_projectile_results` | `runtime.h` | записи последнего шага (одна генерация) |
| `GameRuntime::apply_projectile_blasts` | `runtime.h` / `runtime.cpp` | мост «запись снаряда -> `apply_weapon_blast` -> `apply_blast_damage`», идемпотентно на генерацию |
| `Weapon::impact_blasts` / `expire_blasts` | `game/weapons.h` | `select_blasts` выбирает ветку по статусу (`bounced`/`expired`) |
| `thing_vitals` / `store_thing_vitals` | `game/weapons.h` | виталы вещей: HP, 4 щита, `alive` |
| `Things::things()` / `by_id` | `world/things.h` | цель и запись урона обратно |

Выбор моста: это **weapon-keyed** путь (`apply_projectile_blast`), то есть
тот же, которым идёт игровой выстрел: `fire()` уже разложил impact/expire
события в `Weapon::impact_blasts`/`expire_blasts`. Событийный мост
(`EventBlastRegistry` + `apply_event_blast`, `game/events.h`) — правильный для
AI-снарядов и generic-вещей; здесь он не используется, потому что шаблон
задаёт вызывающий, а не FIRE-подскрипт.

## 3. Сценарий теста

Тест не «прицеливается на глаз»:

1. Загружается реальный `dm1` (`load_level` -> `GameRuntime::build`) и LIGHT
   шасси (`chassis_from_level`).
2. Из реальной таблицы `level0.sbt` берётся `ROCKET` (слот 7, `fire_mode = 1`,
   период 80 тиков) и ставится синтетический лоадаут: маска `1 << 7`,
   `current = 7`, 20 патронов (`Weapons::load` без сейва даёт пустую маску и
   нулевые патроны — так же, как в `test_sim_player.cpp`).
3. Шаблон снаряда — `Thing` с `movement_type = 6`, `id = -1` (рантайм выдаёт
   `0x10000+` на выстрел), `gravity = 0`, `damping = 0.8`, сфера радиуса 10,
   `hits_left = 1`, `expire_ticks = 728`, уникальное create-событие `64201`.
4. Цель выбирается из `Things`: живые вещи с `position` и `max_hp > 0`,
   сортировка по `max_hp` (слабейшая первой). Для каждой — 72 направления и
   стойки 30/40/60/90 юнитов; линия проверяется **настоящим**
   `GameRuntime::update_player_weapon()` (без шаблона, `spawned == -1`) и
   `CollisionWorld::raycast` по реальному направлению выстрела. Это важно:
   у ROCKET `spread = 0.105104`, `spread_y = 0.466308` (`+0x14/+0x18`), и
   детерминированный разброс уводит снаряд до ~26° по вертикали — простая
   линия по yaw попала бы мимо.
5. Выбирается первая пара «цель + линия», где первая же поверхность лежит не
   дальше 200 юнитов от вещи (самый широкий воздушный взрыв ROCKET —
   `12000@256`), затем лоадаут сбрасывается (`shot_index = 0`, тот же seed),
   и настоящий первый выстрел имеет ровно ту же траекторию, что и проба.

## 4. Фактические числа (`dm1` + `level0` v1.36, 2026-10-23)

```
target [139] id=139 type=OILDRUM pos=(5176.00, 600.00, 264.00) hp=1000/1000
feet=(5154.79, 621.21, 264.00) yaw=5.497787 rad (315.0 deg)
shot_dir=(0.6808, -0.6489, 0.3398) spread=(0.2287, 0.7750)
hit=161.88 blast_from_target=138.47; fire=20 ticks
shots=1 projectiles_spawned=1 in_flight=1 live_rounds_max=1
expiry=1 round_spent=1 thing_id=65536 blast_hits=2 things_damaged=1
target after hp=0/1000 shields=0/0/0/0 alive=0
replay spawned=1 blast_hits=2 things_damaged=1 hp=0 alive=0
```

Что это значит:

* `projectiles_spawned() == 1`: выстрел действительно породил снаряд
  (`spawned >= 0`), а не остался hitscan/пустым.
* `in_flight == 1`, `live_rounds_max == 1`: в полёте снаряд виден в
  `runtime.projectiles().list()`; запись одна на весь срез (второй выстрел
  ROCKET — только через 80 тиков, тест держит курок 20).
* `expiry == 1`, `round_spent == 1`: первое касание геометрии израсходовало
  `hits_left = 1`, снаряд перешёл в `expired` и больше не шагает. Запись
  остаётся в `Projectiles::list()` (`spent = true`), рантайм её не удаляет —
  «исчез» здесь означает «взорвался и не летит».
* `blast_hits == 2`: сработали две воздушные ветки ROCKET (`10000@80` и
  `12000@256`), обе достали цель (расстояние от взрыва до вещи 138.47).
* `things_damaged() == 1`, у OILDRUM `hp 1000 -> 0`, все четыре щита
  `1000 -> 0`, `alive == false`: урон реально записан в `Things` через
  `store_thing_vitals`. Это самая слабая вещь уровня с линией огня; у
  `POPMACH` (id 138, 100000 HP и 100000 на щит) одиночный выстрел ROCKET
  снимает только щиты (`100000 -> 95790`, HP не меняется) — поэтому сортировка
  по `max_hp` и выбирает бочку.
* Повторный прогон на свежем рантайме даёт те же числа (детерминизм,
  `sim_projectile: replay ...`).

Негативы (печатаются тестом):

```
no template -> shots=1 spawned=-1 projectiles_spawned=0 blast_hits=0
               things_damaged=0 rounds 1->1
movement_type=0 template -> spawned=1 (last tick -1) projectiles_spawned=1
                            rounds=2 (accepted by the runtime: no movement_type guard)
clear_projectile -> has=0 spawned=-1 projectiles_spawned=1
```

То есть без шаблона шов оружия честно отказывает (`spawned == -1`), а шаблон
не с типом движения 6 рантайм **принимает** — см. §5.

## 5. Ограничения среза (с причинами)

* **Тип движения не валидируется.** Ни `SimPlayer::set_projectile`, ни
  `GameRuntime::set_player_projectile`, ни `projectile_spawn_from_shot` не
  проверяют `movement_type`: тип 8 только включает homing-ветку
  (`0x475930`), остальные летят как обычный снаряд. Тест фиксирует это
  фактическое поведение (`movement_type = 0` -> `spawned >= 0`), а не
  выдумывает отказ. Валидация — вопрос игрового слоя/CLI.
* **Нет урона игроку.** `player_vitals` в мосте пустой: попадания по игроку
  считаются в `blast_hits()`, но `apply_blast_damage()` не наносит урон
  (документированное правило «короткий span = только счёт»). Смерть/респавн
  игрока — не этот срез.
* **Контекст взрыва фиксирован.** `ProjectileBlastContext` берётся по
  умолчанию: `underwater = false`, `from_ground = true`,
  `deathmatch = true`, `source_player = owner_player`, `source_thing = -1`.
  Рантайм не знает среду игрока/уровня, поэтому водные ветки и
  `DEATHMATCH_GAME`-ветки не выбираются по фактическому состоянию.
* **Weapon-keyed, а не event-driven взрыв.** Урон идёт по
  `Weapon::impact_blasts/expire_blasts` (путь игрока). События шаблона
  (`collide_event`/`expire_event`) рантайм постит в очередь, но они не
  выбирают blast: для этого нужен `EventBlastRegistry`, а он в этом срезе не
  строится (нужен разбор `.sbt`-подскриптов; см. `docs/PROJECTILE_CPP.md` §7).
* **Звука взрыва нет.** Аудио-слой не подключён ни в рантайме, ни в
  `SimPlayer`; событие взрыва только постится в очередь.
* **Запись снаряда не удаляется.** После `expired` она остаётся в
  `Projectiles::list()` со `spent = true` до `remove_projectile`/`reset`;
  тест проверяет `spent`, а не размер списка.
* **Вещь не удаляется из `Things`.** `store_thing_vitals` ставит
  `alive = false` (при `max_hp > 0`), но запись остаётся на месте — стабильные
  индексы, как у `Things::remove`.
* **Режим снаряда тикает рантайм целиком.** Вместе со снарядом шагают
  things/AI/двери/события. Это осознанно (`GameRuntime::tick_once` — тот же
  порядок, что у игры), но вызывающий не должен тикать рантайм сам, иначе
  снаряд шагнёт дважды.
* **Один шаблон на игрока.** `set_projectile` задаёт один `Thing`; разные
  боеприпасы (граната/ракета) — это разные вызовы/игроки, выбора по слоту
  оружия нет.

## 6. Что осталось для CLI (`--sim-projectile`)

Проводка `main.cpp`/`config.*` — отдельная задача (её делает основной агент):

1. Флаг `--sim-projectile NAME` (или `--sim-drive` + `--projectile`): шаблон
   брать не синтетический, а из `Things` уровня по `Weapon::projectile_id`
   (THING_TYPES с `movement_type = 6`), иначе — ошибка с текстом.
2. Лоадаут: реальный сейв (`GameState`) или явные `--weapon/--ammo`, как у
   остальных `--sim*`; без маски/патронов ROCKET не выстрелит.
3. Печать счётчиков `SimPlayer`: `projectiles_spawned`, `blast_hits`,
   `things_damaged`, финальные виталы цели и статус снаряда (`expired`),
   чтобы прогон был проверяемым без CTest.
4. Контекст взрыва из CLI: вода/земля/deathmatch и `source_player` —
   сейчас зашиты значения по умолчанию.
5. `ROADMAP.md` (M6.5.1) и `CURRENT_STATE.md` — статус ставит основной агент
   после merge; здесь только код, тест и этот документ.

## 7. Доверие

| утверждение | доказательство |
|---|---|
| выстрел порождает снаряд | `spawned == 1`, `projectiles_spawned() == 1` на `dm1` |
| снаряд летит и виден | `in_flight == 1`, `live_rounds_max == 1` |
| снаряд взрывается о геометрию | `expiry == 1`, `round_spent == 1` (статус `expired`) |
| взрыв наносит урон вещи | `blast_hits == 2`, `things_damaged == 1`, `hp 1000 -> 0` |
| вещь умирает | `alive == false` после `store_thing_vitals` |
| детерминизм | повтор на свежем рантайме: те же spawned/blast/hp/alive |
| без шаблона снаряда нет | `spawned == -1`, `projectiles_spawned() == 0` |
| тип движения не проверяется | `movement_type = 0` -> `spawned >= 0` (факт, не гипотеза) |

Проверка:

```sh
ctest --test-dir build/sandbox -R 'sim_projectile|sim_player|player_trigger' --output-on-failure
python3 regress.py --mirror --json
```
