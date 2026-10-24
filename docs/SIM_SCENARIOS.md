# Детерминированные сценарии `--sim` (`M9.5`, срез 1)

Снимок 2026-10-24. Срез добавляет **golden-сценарии игрового тика**: пять
сценариев повторяют цикл `main.cpp::simulate()` шаг за шагом на реальном уровне
`dm1`, печатают компактную сводку и проверяют каждое число. Задача среза — ловить
регрессии в тике и в числах, а не заново доказывать физику, правила оружия или
модель урона (это делают `sim_player`, `sim_projectile`, `sim_damage` и модульные
тесты).

Файлы среза: новый `engine/tests/test_sim_scenario.cpp` (CTest `sim_scenario`,
метка `quick`), этот документ и одна строка в `engine/tests/CMakeLists.txt`.
`engine/src/*`, `main.cpp`, `config.*` не менялись: сценарии — внешний
пользователь готовых API.

## 1. Что именно повторяется

Загрузка — та же, что у CLI (`main.cpp::simulate()`):

```
load_level(game, "dm1")                 # .sbt + .sbw, parse_sbt внутри
vm_state_from_level(entities, VmContext{game_mode 0, players 1, difficulty 1, god})
GameRuntime::build(...)                 # GameRuntimeConfig{ai_apply_aim = true}
extract_scene -> кампанийный спавн слота 0 (feet, yaw)
chassis_from_level("dm1.sbt") -> LIGHT   # eye_height, max_speed, retention
Weapons::load("dm1.sbt")                 # таблица оружия УРОВНЯ (не level0)
```

Лоадаут — «полный пикап» CLI: все слоты таблицы в маске, патроны по максимуму
типа боеприпаса, `current = 0`; с `--sim-projectile` выбирается предпочитаемый
снарядный слот (ROCKET первым, иначе ранжирование CLI). Перед первым тиком на
рантайм ставится точка глаза `feet.z + 10` — как в `main.cpp`.

Один тик цикла (порядок CLI):

```
++drive_ticks
player.tick(runtime, segment, index)     # контроллер -> глаза -> шов оружия
if (!player.has_projectile()) runtime.tick(1)   # в режиме снаряда рантайм тикает сам игрок
if (player.last_step().hit_wall) ++wall_ticks
if (!player.alive() && --sim-respawn N && drive_ticks - death_tick >= N) player.respawn()
```

## 2. Сценарии и числа (`dm1` v1.36, 2026-10-24)

Все числа ниже — вывод теста; каждое проверяется (`CHECK*`), а не только
печатается. Формат `позиция (feet)`, `vel` — юниты/с.

### S1 «проезд» — `forward=1:900`

CLI: `gunmetal --sim dm1 --sim-drive 'forward=1:900'`

```
ticks=900 player_ticks=900 pos=(-160.000000, 1684.750000, 0.000001)
vel=(0.000000, 154.700000, 0.000000) yaw=0.000000000 on_ground=1 walls=0
travelled=1508.750000 shots=0
```

Проверено: `pos.x = -160`, `pos.y = 1684.75`, `pos.z = contact_skin = 1e-6`,
`travelled = 1508.75`, `vel.y = 154.7` (1.7 юнита/тик × 91 Гц), `vel.x = vel.z = 0`,
`on_ground`, `walls = 0`, `shots = 0`, yaw не изменился.

### S2 «поворот» — `turn=0.5:300`

CLI: `gunmetal --sim dm1 --sim-drive 'turn=0.5:300'`

```
ticks=300 pos=(-160.000000, 176.000000, 0.000001) vel=(0, 0, 0)
yaw=1.648351648352 (ожидание 300*0.5/91 = 1.648351648352) on_ground=1 walls=0
```

Проверено: `turn` — радианы **в секунду**, поэтому yaw растёт ровно на
`300 * 0.5 / 91` рад (1e-12); без оси `forward` игрок не смещается
(`x/y` те же, что на спавне), `on_ground` держится.

### S3a «стрельба hitscan» — `fire:200`

CLI: `gunmetal --sim dm1 --sim-drive 'fire:200'`

```
slot=0 weapon="MACHINE GUN" mode=1 period=17 hitscan=1
shots=12 spawned=0 shot_index=12 ammo=5000->4988 damage_taken=0 alive=1
```

Проверено: 12 выстрелов за 200 тиков при периоде 17 (`200/17 -> 12`), 12
патронов из максимума 5000, `Arsenal::shot_index = 12`, снарядов нет
(`spawned = 0`), урона/попаданий нет — hitscan-ветка в этом слое только
считает выстрел и тратит патрон (цели/урон разрешает вызывающий,
`docs/SIM_PLAYER.md` §5; `main.cpp` тоже не наносит hitscan-урон).

### S3b «снаряд из спавна» — `fire:200 --sim-projectile`

CLI: `gunmetal --sim dm1 --sim-drive 'fire:200' --sim-projectile`

```
slot=7 weapon="ROCKET" mode=1 period=80 hitscan=0
shots=3 spawned=3 shot_index=3 ammo=50->47 blasts=0 things_damaged=0 alive=1
```

Проверено: ROCKET стреляет раз в 80 тиков (3 выстрела за 200), каждый выстрел
реально породил снаряд (`spawned == projectiles_spawned == 3`), 3 патрона из 50,
`blast_hits = 0` — на линии от спавна в 200 тиках ничего не взрывается
(снаряд ещё летит). Шаблон — синтетический, ровно тот, что ставит
`main.cpp --sim-projectile` (тип 31, движение 6, `gravity 1.0`, `expire 364`,
create-событие 901).

### S3c «снаряд в цель» — прицельная линия, `fire:20`

Цель и стойка берутся из уровня тем же поиском, что в `sim_projectile`/
`sim_damage`: слабейшая damageable-вещь с линией огня по **реальному**
направлению `update_player_weapon()` (разброс ROCKET — не прямая по yaw).

```
target [139] id=139 type=OILDRUM pos=(5176.00, 600.00, 264.00)
feet=(5154.79, 621.21, 264.00) yaw=5.497787 rad stand_off=30 blast_from_eye=161.88
shots=1 spawned=1 ammo=50->49 shot_index=1
blasts=2 things_damaged=1 target_hp=1000->0 target_alive=0
damage_taken=4216 armor=10000 shields=6946/6946/6946/6946 alive=1
```

Проверено: выстрел породил снаряд, взрыв дал две воздушные ветки ROCKET
(`blast_hits = 2`), вещь получила урон и умерла (`hp 1000 -> 0`, `alive = 0`),
патрон списан (49), самоурон того же взрыва снял 4216 со щитов (плиты), броня
не тронута — игрок жив.

`damage_taken = 4216`, а не `5060` из `docs/SIM_DAMAGE.md` §6, потому что
источник таблицы другой: CLI (и этот тест) читает таблицу **уровня**
`dm1.sbt`, а `sim_projectile`/`sim_damage` — `level0.sbt`. Проверено
экспериментом: с таблицей `level0.sbt` тот же сценарий даёт ровно `5060` и
плиты `6735` (числа `SIM_DAMAGE.md`).

### S4 «смерть и респавн» — `--sim-vitals 500,0`, контроль и `--sim-respawn 20`

`--sim-vitals 500,0` в CLI: `armor = armor_max = 500`, четыре щита `0`,
`shield_max` остаётся LIGHT (8000).

S4a, без респавна (`fire:40`, `--sim-vitals 500,0`):

```
ticks=40 player_ticks=9 pos=(5154.786797, 621.213203, 260.196112) alive=0
armor=0/500 shields=0/0/0/0 damage_taken=500 death_tick=9 deaths=0
shots=1 spawned=1 ammo=49 blasts=2 things_damaged=1
```

Проверено: смерть на тике **9** (первый же взрыв), `armor = 0`, `damage_taken =
500` (ровно ёмкость), счётчик тиков игрока замирает на 9 (мёртвый тик — no-op),
`drive_ticks` продолжает идти (40), `deaths = 0`.

S4b, `--sim-respawn 20` (тот же план):

```
ticks=40 player_ticks=20 death_tick=9 first_respawn_tick=29 deaths=1 alive=1
armor=500/500 shields=0/0/0/0 damage_taken=500 shot_index=1 ammo=49
```

Проверено: респавн на **drive-тике 29** (9 + 20), `deaths = 1`, шкалы вернулись
(`armor 500/500`, щиты 0), `damage_taken` остался суммой за все жизни (500),
`death_tick` хранит последнюю смерть (9), после респавна дожито 11 тиков
(`player_ticks = 20`), новых выстрелов нет — кулдаун ROCKET (80) не дотёк.

### S5 «GOD_MODE» — `--god`

`--god` в `main.cpp` кладёт `VmContext::god_mode = true`; это поле читает только
VM-условие `GOD_MODE` (`res/vm_exec.cpp`) и подскрипты вещей. Мост урона игрока
(`SimPlayer::set_vitals` -> `GameRuntime::set_player_vitals` ->
`apply_projectile_blasts_to_players`) его не читает, поэтому `--god` **не**
защищает шкалы на этом слое.

S5a, `--god` + `--sim-vitals 500,0` + тот же прицельный `fire:40`:

```
armor=0/500 alive=0 damage_taken=500 death_tick=9 blasts=2 things_damaged=1
target_hp=1000->0
```

Проверено: числа **совпадают с S4a до последнего** (тик смерти 9, урон 500,
броня 0) — флаг `god` меняет только VM-контекст, а не урон игроку. Наблюдение
CLI: `--sim ... --god` печатает `god=1` и тот же digest, что без флага.

S5b «урон не убивает» по правилу шкал (LIGHT по умолчанию, `fire:30`):

```
armor=10000/10000 shields=6946/6946/6946/6946 alive=1 damage_taken=4216
blasts=2 things_damaged=1 target_hp=1000->0
```

То есть «один взрыв не убивает» обеспечивают именно шкалы LIGHT (плиты
съедают 4216, броня цела), а не `--god`.

## 3. Детерминизм

Каждый сценарий прогоняется дважды на **двух свежих рантаймах** (второй
`load_level` + `GameRuntime::build`), сравнение — точное по счётчикам и `1e-12`
по `pos/vel/yaw`. Вывод: `determinism <id> match=1` для всех семи прогонов
(S1, S2, S3a, S3b, S3c, S4a, S4b, S5a, S5b). Побитовое совпадение
обеспечено тем же свойством, что у контроллера: только `double`, без часов, RNG
и платформенного состояния (`-ffp-contract=off`).

## 4. Доказано / модель / ограничения

| | |
|---|---|
| доказано | числа S1/S2/S3a/S3b совпадают с выводом CLI `--sim ... --sim-drive` (прогон на главном чекауте, `build/sandbox`); S3c/S4/S5 — те же модули и тот же тик, но поза задаётся вызывающим |
| доказано | ROCKET из `dm1.sbt` даёт 4216/6946, из `level0.sbt` — 5060/6735: разница только в таблице уровня (проверено переключением таблицы в тесте) |
| доказано | `--god` (VmContext) не меняет урон/смерть игрока: S5a идентичен S4a |
| модель | `--sim-respawn N` — задержка в **drive-тиках**, не DM-правило (`docs/DM_RULES.md` §3.2 не моделируется); респавн мгновенно возвращает позу/шкалы |
| модель | шаблон снаряда синтетический (`main.cpp` берёт его же); вещь уровня как источник шаблона не выбирается |
| ограничение | **нет фронтенда/сети/SDL**: только headless-рантайм, ни рендера, ни звука, ни ввода с клавиатуры; план — заранее заданные числа |
| ограничение | **прицельные сценарии (S3c/S4/S5) недостижимы текущими флагами CLI**: `--sim` ставит игрока только на кампанийный спавн слота 0. Тест задаёт позу через `SimPlayer::init()`; тик, оружие и урон — те же |
| ограничение | hitscan-урон в этом слое не наносится (нет целей/`resolve_hitscan_impact` у вызывающего), поэтому S3a проверяет только выстрелы/патроны/`shot_index` |
| ограничение | один игрок; мультиплеер (8 точек глаз) не покрыт |
| ограничение | мёртвый игрок не двигает мир (тик мира не зовётся), поэтому после смерти прогон «замирает» до `--sim-respawn` |
| ограничение | числа привязаны к поставочным данным v1.36 (`dm1.sbt/.sbw`); подмена файлов установки требует нового baseline (`docs/BASELINE.md`) |
| цена | тест ~31 с (14 загрузок `dm1` + 900-тиковый проезд ×2); метка `quick`, но это самый дорогой из sim-тестов |

## 5. Проверка

```sh
cmake --build build --target gm_sim_scenario_tests -j 4
ctest --test-dir build -R '^sim_scenario$' --output-on-failure
python3 regress.py --mirror --json
```

## 6. Доверие

| утверждение | доказательство |
|---|---|
| 900 тиков `forward=1` = 1508.75 юнита | `pos.y 176 -> 1684.75`, `travelled=1508.75`, S1 |
| `turn` — рад/с | `yaw = 300*0.5/91` с точностью 1e-12, S2 |
| hitscan: 12 выстрелов / 12 патронов / `shot_index 12` | S3a |
| `--sim-projectile`: 3 выстрела = 3 снаряда, 3 патрона | S3b |
| прицельный ROCKET: 2 взрыва, цель 1000 -> 0 hp, самоурон 4216 | S3c |
| смерть при `armor <= 0` на тике 9, дальше no-op | S4a |
| респавн на тике 29, шкалы 500/500, `deaths=1` | S4b |
| `--god` не защищает шкалы | S5a идентичен S4a |
| один взрыв LIGHT не убивает | S5b: броня 10000, плиты 6946, жив |
| детерминизм | 9 прогонов `match=1`, сравнение 1e-12 |
