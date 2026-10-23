# Игровой тик одного игрока (`game/sim_player.*`, `M6.5.1`, срез 1)

Снимок 2026-10-23. Модуль ставит **одного игрока** на готовый
`GameRuntime` и ведёт его по сценарию вождения (`DriveSegment`): движение —
проверенным контроллером, стрельба — проверенным швом оружия. Новой физики,
новых правил столкновений и правок рантайма здесь нет.

Файлы: `engine/src/game/sim_player.h`, `engine/src/game/sim_player.cpp`,
тест `engine/tests/test_sim_player.cpp` (CTest `sim_player`, метка `quick`).
CLI-проводки нет: `--sim-drive` — следующий срез (см. §6).

## 1. Что делает

Один `tick()` — ровно один фиксированный тик 91 Гц (`1/91` с):

```
yaw   += segment.turn * config.dt          # turn — радианы в секунду
input  = {forward, strafe, jump, crouch}   # из сегмента
step   = gm::step(old_state, input, runtime.collision(), config, config.dt)
         # step.ok == false -> tick() возвращает false + текст,
         # state() не меняется
eye    = eye_position(new_state)           # pos + (0,0,eye_height)
runtime.set_player_eyes({eye})             # игрок виден AI и blast-мосту
если segment.fire:
    PlayerWeaponInput{trigger_held = true,
                      trigger_edge = (tick_index == 0),
                      slot = -1,
                      aim_direction = (cos(yaw), sin(yaw), 0)}
    runtime.update_player_weapon(input, eye)   # 0x4453d0
```

Порядок фиксирован, всё в `double`, без часов, RNG и платформенного
состояния; движок собирается с `-ffp-contract=off`, поэтому два прогона одного
плана побитово совпадают (в тесте — равенство с допуском `1e-12`).

## 2. API

```cpp
struct DriveSegment {
    int ticks = 0;          // 1..kDriveMaxTicks (1'000'000)
    double forward = 0.0;   // ось «вперёд», клампится gm::step в [-1, 1]
    double strafe = 0.0;    // ось «вправо»
    double turn = 0.0;      // радианы/сек
    bool fire = false;      // держать курок
    bool jump = false;
    bool crouch = false;
};

std::optional<std::vector<DriveSegment>> parse_drive(std::string_view script,
                                                     std::string* error = nullptr);
```

Формат плана — сегменты через `;`. Пример:
`forward=1:900;turn=0.5:300;fire:200;jump=5` — четыре сегмента (900 тиков
вперёд, 300 тиков с поворотом, 200 тиков с курком, 5 тиков прыжка).
Допустимые написания:

| запись | смысл |
|---|---|
| `forward=1:900`, `turn=0.5:300`, `strafe=-0.25:7` | числовой ключ: значение и тики |
| `fire:200`, `crouch:30` | булев ключ, значение `true`, тики |
| `jump=5` | булев ключ в компактной записи: тики после `=` (форма из примера выше) |
| `fire=0:200` | булев ключ с явным значением: `0` — не стрелять |

Пробелы, табуляции и переводы строк вокруг полей игнорируются. `ticks` — целое
`1..1 000 000` (только цифры). Отказ (`nullopt` + текст, без исключений):
пустой скрипт, пустой сегмент, неизвестный ключ, числовой ключ без значения,
булев ключ без тиков, NaN/inf/`1e999`/`abc`, `ticks` вне диапазона, больше
одного `:`. Текст ошибки начинается с номера сегмента:
`drive segment 2: ...`.

```cpp
class SimPlayer {
public:
    bool init(const Vec3d& feet, double yaw, const ChassisSpec& chassis,
              std::string* error = nullptr);
    bool tick(GameRuntime& runtime, const DriveSegment& segment, int tick_index,
              std::string* error = nullptr);

    const PlayerState& state() const;
    double yaw() const;                 // == state().yaw
    int shots_fired() const;            // принятые выстрелы за жизнь игрока
    std::uint32_t ammo(int slot) const; // runtime.player_arsenal().ammo[slot]
    int tick_index() const;             // сколько тиков уже выполнено
    const ControllerConfig& config() const;
    const StepResult& last_step() const;
    const PlayerWeaponTick& last_weapon() const;
    bool initialized() const;
};
```

`init()` собирает `ControllerConfig` только из шасси:

| поле | источник |
|---|---|
| `eye_height` | `chassis.eye_height` — `op9`, `0x81bf90`/`player+0x2f50` |
| `max_speed` | `chassis.max_speed` (`op4`, юниты **за тик**) × `kPlayerTickHz` (91) |
| `retention` | `chassis_motion(chassis).retention` (`0x81bfb8`, у LIGHT `0.9`) |
| `radius`/`height`/`gravity`/… | значения по умолчанию `ControllerConfig` (капсула LIGHT) |

Отказ `init`: нефинитные `feet`/`yaw`, `eye_height` не конечный или `<= 0`,
`max_speed` нефинитный или `< 0`. `tick()` отказывает до `init()`, при
`tick_index < 0`, нефинитных `forward/strafe/turn`, при `step.ok == false` и
если рантайм не принял точку глаза. Во всех отказах `state()` и счётчик тиков
не меняются.

`tick_index` — индекс тика **внутри серии**: вызывающий крутит
`0..segment.ticks-1`. `tick_index == 0` превращает `fire` в фронт курка
(`PlayerWeaponInput::trigger_edge`), остальные тики только держат его — так
`fire:300` для оружия режима 0 даёт один выстрел, а для режима 1 — выстрел
каждый период `+0x2c`.

`ammo(slot)` читает арсенал рантайма, с которым игрок **последний раз**
тикал; до первого тика (или после `slot` вне `0..11`) возвращает `0`.

## 3. Что переиспользуется (без изменений)

| шаг | функция | где |
|---|---|---|
| состояние игрока | `make_state`, `PlayerState` | `player/controller.h` |
| тик движения | `step`, `StepResult` | `player/controller.h` / `controller.cpp` |
| точка глаза | `eye_position` (`pos + eye_height`) | `player/controller.h` |
| глаза игрока в рантайме | `GameRuntime::set_player_eyes` | `game/runtime.h` |
| тик оружия `0x4453d0` | `GameRuntime::update_player_weapon` | `game/runtime.h` |
| вход оружия | `PlayerWeaponInput` (hold/edge/slot/aim) | `game/trigger.h` |
| арсенал | `Arsenal` (`ammo`/`mask`/`current`/`shot_index`) | `game/weapons.h` |
| шасси | `ChassisSpec`, `chassis_motion` | `game/chassis_loadout.h` |

`GameRuntime::tick()` модуль не вызывает: он не двигает AI и не крутит
снаряды/двери — это делает вызывающий (в будущем `--sim-drive`), чтобы тик
игрока и тик мира оставались явными.

## 4. Тест и фактические числа

`gm_sim_player_tests` собран как `engine/src/main.cpp::simulate`:
`load_level` (`.sbw` + `.sbt`; `parse_sbt` внутри) → `decode_sbt_script` →
`vm_state_from_level` → `GameRuntime::build` (`ai_apply_aim = true`) →
`extract_scene` (спавн слота 0) → `chassis_from_level` (LIGHT).

### 4.1. `dm1`, `forward=1:900`

Спавн слота 0 (кампания): `(-160, 176, 64)`, yaw `0`, скрипт 2 317 инструкций.

```
sim_player: dm1 forward=1:900 -> (-160, 1684.7500000000298, 1.0000000000823792e-06)
            travelled=1508.7500000000298 on_ground=1 wall_ticks=0 shots=0
```

Смещение 1508.75 юнита (> 100), все координаты конечны, NaN нет, yaw не
изменился. Пол `dm1` под спавном лежит на `z = 0`; его обход противоположен
сохранённой нормали (`data_normal.z = +1`, обход `−1`), поэтому зонд пола
классифицирует его по сохранённой нормали как пол (`surface_of`,
`docs/COLLIDE.md` §1.2) и `on_ground` держится всё время движения. Капсула
стоит на полу: `z = contact_skin = 1e-6`, `vel.z = 0`. Второй случай, на
площадке с наклоном, — §4.4.

Детерминизм: два независимых прогона `forward=1:900` дают одинаковые
`x/y/z/yaw` (проверка с допуском `1e-12`, фактически побитово):

```
determinism first =(-160, 1684.7500000000298, 1.0000000000821971e-06, yaw 0)
            second=(-160, 1684.7500000000298, 1.0000000000821971e-06, yaw 0)
```

### 4.2. Стены: направление берётся из геометрии

Тест не «подбирает на глаз»: из точки глаза спавна
(`feet.z + eye_height`) кастуются лучи `CollisionWorld::raycast` в восьми
направлениях (шаг 45°), направления сортируются по расстоянию, и в каждое
идёт `forward=1` на `dist / (max_speed·dt) + 200` тиков.

```
wall probes (yaw deg / ray distance):
  270/399.000 180/400.000 225/552.958 315/564.271
  135/565.685   0/826.679  45/1182.283 90/1964.061
forward yaw=270.00 deg for  435 ticks: wall_ticks=188 moved=397.000 (ray 399.000)
forward yaw=180.00 deg for  436 ticks: wall_ticks=191 moved=396.000 (ray 400.000)
forward yaw=225.00 deg for  526 ticks: wall_ticks=209 moved=555.115 (ray 552.958)
forward yaw=315.00 deg for  532 ticks: wall_ticks=163 moved=831.200 (ray 564.271)
forward yaw=135.00 deg for  533 ticks: wall_ticks=192 moved=605.459 (ray 565.685)
forward yaw=  0.00 deg for  687 ticks: wall_ticks=  0 moved=1146.650 (ray 826.679)
forward yaw= 45.00 deg for  896 ticks: wall_ticks= 76 moved=1449.261 (ray 1182.283)
forward yaw= 90.00 deg for 1356 ticks: wall_ticks=1046 moved=506.000 (ray 1964.061)
blocked directions=7 (first yaw=4.7123889803846897, ray=399.000, wall_ticks=188)
```

Ключевые факты:

* 7 из 8 направлений дают `hit_wall` в тиках шага: капсула упирается в
  геометрию, а не проходит мимо;
* смещение может быть **больше** длины луча, когда капсула скользит вдоль
  наклонной поверхности (225°: 555.115 > 552.958; 315°: 831.200 > 564.271;
  45°: 1449.261 > 1182.283) — это слайд контроллера, а не проход сквозь;
* yaw `0°`: луч попадает в поверхность на 826.679, а игрок уходит на
  1146.65 без `hit_wall` — луч задел не стену на высоте капсулы (порог/наклон),
  поэтому «луч попал» ≠ «стена»; это видно в таблице и не выдается за стену;
* повторный прогон выбранного направления (270°, лоб в лоб) даёт ровно те же
  188 тиков с `hit_wall`, а смещение 397.000 не превышает расстояния луча
  399.000 — капсула останавливается перед стеной.

### 4.3. Огонь: `fire:300`

Оружие — реальная таблица `level0.sbt` (`Weapons::load`), лоадаут
синтетический: таблица без сейва даёт пустую маску и нулевой запас, поэтому
тест ставит `mask = 1`, `ammo[0] = 50`, `current = 0` и
`runtime.set_player_weapons(table, arsenal)`.

```
fire:300 slot=0 weapon="MACHINE GUN" mode=1 period=17 ticks ->
          shots=18 ammo 50->32 shot_index=18 spawned=-1 fired=0
```

18 выстрелов за 300 тиков при периоде 17 тиков (300/17 = 17.6 → 18),
`ammo[0]` 50 → 32, `Arsenal::shot_index` 18. `spawned = -1`: у MACHINE GUN
нет шаблона снаряда в этом срезе (`set_player_projectile` не вызывается) —
выстрел всё равно засчитан, патрон списан, но летящего снаряда нет.
`fired = 0` в последней строке — последний тик серии был на перезарядке.

### 4.4. `on_ground == true` на реальной геометрии `dm1`

Тест ищет в триангуляции уровня треугольники, которые
`surface_of(t) == Surface::floor` — теперь по сохранённой нормали
(`surface_normal().z >= 0.9`) — достаточно широкие для капсулы (`>= 8`
юнитов по X и Y), где `floor_at(центроид)` попадает в тот же треугольник,
сортирует их по расстоянию до спавна и ставит капсулу на первый, который
держит `on_ground` 10 тиков без дрейфа > 1 юнита:

```
authored flat floors: 1688 candidate(s), tried=1, resting=1
    at (-30.666667, 386.666667, 26.000000) sector=82 feet_z=26.000001000000001
```

Площадка плоская (`surface_normal().z >= 0.9`), капсула стоит точно на
`contact_skin` над ней (`on_ground == 1`), а не висит. Это второе место в
срезе (после §4.1), где проверяется флаг `on_ground` на реальной геометрии.

### 4.5. `level1` (дымовой кейс)

```
level1 spawn (53, -527, 3) -> (53, -451.00000091107108, -39.999999000000003)
        travelled=76.000 yaw 0.000000 -> 1.098901 on_ground=1
```

300 тиков (`forward=1:200;turn=1:100`): состояние конечно, yaw вырос ровно на
`100/91` рад (`turn = 1` рад/с), глаз = `pos.z + 10`. Установка поставляет
`level1`/`level2` перепутанными по имени (см. `engine/tests/game_path.h`),
а тест идёт тем же именем, что и `--sim level1`, поэтому числа описывают файл
`level1.sbt`.

### 4.6. Негативы

22 плохих плана отвергнуты с текстом (`parse_drive negatives 22/22`):
`forward=abc:10`, пустой скрипт, `ticks=0`, `ticks=-5`, `ticks=1000001`,
`ticks=2.5`, `forward=1` (нет тиков и значения), `forward:10` (нет значения),
`fire=abc:10`, `fire` (нет тиков), `nope=1:10`, `=1:10`, хвостовой `;`,
ведущий `;`, `nan`, `inf`, `-inf`, `1e999`, `,5`, два `:`.

Плюс отказы `init` (NaN `feet`/`yaw`, `eye_height` 0/NaN, `max_speed`
`-1`/inf), `tick()` до `init()`, отрицательный `tick_index` и нефинитный
`turn` (состояние и счётчик не меняются).

## 5. Ограничения среза

* **Один игрок**; мультиплеер (8 точек глаз) не моделируется.
* Нет урона игроку, смерти, респавна: `Vitals`/`apply_projectile_blasts*` не
  вызываются, `set_player_vitals` не используется.
* Нет ввода с клавиатуры/мыши и `input/controls.*`: план — заранее заданные
  числа, `slot` оружия не переключается (`slot = -1` всегда).
* Нет отдачи (`apply_recoil`), прицела по вертикали (только yaw) и
  `slot_edge`.
* Нет шаблона снаряда игрока: `spawn_player_shot` не вызывается, летящих
  снарядов от игрока в этом срезе нет (`spawned == -1`).
* `SimPlayer::tick` не крутит `GameRuntime::tick()`: AI, снаряды и двери
  стоят, пока их не тикнет вызывающий.
* `on_ground` держится на авторских полах обоих проверенных уровней (`dm1`
  §4.1, `level0` в `PLAYER_CONTROLLER.md` §5.2) — с тех пор как
  `surface_of()` классифицирует по сохранённой нормали полигона
  (`docs/COLLIDE.md` §1.2). Исключение — уровни/точки без `.sbw`-пола под
  ногами: контроллер не ищет `.sob`/двери (см. `docs/COLLIDE.md` §9).
* Нет проверки, что игрок «влез» в дверь/на лифт: `WorldState` не
  опрашивается.

## 6. Что осталось для CLI (`--sim-drive`)

Модуль готов к проводке, но `engine/src/main.cpp`, `core/config.*`,
`docs/SIM.md` и `gm.py` в этом срезе не менялись. Основному агенту нужно:

1. опция `--sim-drive PLAN` (например
   `--sim-drive "forward=1:900;fire:300"`) + `--sim-drive-digest`/`--ticks`
   как ограничитель; разбор — `gm::parse_drive`, ошибка — `[error]` и rc=2;
2. цикл `for (tick=0; tick<segment.ticks; ++tick) player.tick(runtime,
   segment, tick, &error)`, с `runtime.tick(1)` в том же цикле, если нужен
   живой мир (порядок: игрок → мир или мир → игрок — решение вызывающего);
3. печать итогов: `travelled`, `on_ground`, `wall_ticks`, `shots_fired`,
   `ammo(slot)`, `shot_index`, `tick_index`, плюс дайджест по образцу
   `--sim` (позиция/yaw игрока в FNV-1a), чтобы CTest ловил регресс;
4. CTest-проверку на `dm1` (по образцу `tools/qa/sim_check.py`) и строку в
   `docs/SIM.md` §«Чего здесь нет» — игрок больше не «отсутствует целиком»,
   отсутствует только CLI-проводка;
5. при желании — `--sim-drive-dump` с трассой `pos/yaw` по тикам для
   сравнения с Python-оракулом.

## 7. Доверие

Проверено:

* `parse_drive` — 22 негатива с текстом, 2 позитивных плана, границы
  `1..1 000 000`;
* `dm1`: 900 тиков `forward=1` — 1508.75 юнита, конечные координаты,
  детерминизм `1e-12`;
* `dm1`: 7/8 направлений из геометрии дают `hit_wall`, капсула не проходит
  сквозь стену;
* `dm1`: `on_ground == 1` на полу арены (§4.1) и на отдельной плоской
  площадке уровня (§4.4);
* `dm1`: `fire:300` — 18 выстрелов, `ammo 50→32`, `shot_index 18`;
* `level1`: 300 тиков с поворотом — yaw `+100/91`, конечное состояние.

Не доказано (гипотеза/следующий срез): поведение при уроне и смерти игрока,
многопользовательский тик, снаряды игрока и переключение слотов. Пол под
ногами теперь берётся из сохранённой нормали полигона; `.sob`-модели, двери и
лифты в `CollisionWorld` по-прежнему не входят.
