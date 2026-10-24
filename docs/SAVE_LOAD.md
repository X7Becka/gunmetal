# Сейв → живой рантайм (`save_load`, M8.7/M9, срез 2)

* **Статус:** срез 2 закрыт — оба поставочных сейва применяются к живому
  `GameRuntime` + `SimPlayer`; состояние из файла реально оказывается в
  рантайме (виталы, арсенал, поза, `VmState::sectors`), а не только
  печатается. Тест `save_load` — 151 проверка, зелёный; CLI `--save-load`.
* **Ветка:** `feat/saveloadrun`, база `master` `7c8414e`, дата 2026-10-24.
* **Игра:** только чтение (`/mnt/d/games/Gunmetal 3dfx`). Сейвы не
  записываются, `SAVEGAME` не меняется; «битые» варианты собираются в памяти.
* **Файлы:** `engine/src/game/save_load.h`, `engine/src/game/save_load.cpp`,
  `engine/tests/test_save_load.cpp`, `docs/SAVE_LOAD.md`; регистрация —
  `engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt`; CLI —
  `engine/src/main.cpp`, `engine/src/core/config.{h,cpp}`.
* **Источники:** `docs/SAVE.md` (контейнер), `docs/SAVE_FIELDS.md` §2/§9
  (заголовок, блок 9), `docs/SAVE_BLOCKS.md` §13 (блок 8),
  `docs/SIM_PLAYER.md`, `docs/SIM_DAMAGE.md`, `docs/RUNTIME_CPP.md`,
  `docs/RECORDS.md` §2 (`SECTOR_STUFF`), `docs/SAVE_SLOTS.md` (нумерация
  `saved<N>.sbt`).

## 1. API

```cpp
struct LoadedSave {                     // что применилось и что нет
    std::string title, level;
    bool player_applied;
    Vitals vitals; Vec3d feet; double yaw; std::array<double, 3> angles;
    Arsenal arsenal;
    std::size_t blocks, sector_stuff, sector_stuff_active;
    bool sector_stuff_applied;
    std::vector<std::string> unsupported; // по строке на непринятое поле/блок
};

std::optional<std::filesystem::path> resolve_save_path(game, slot_or_path, &error);
std::string save_level_name(const Save&);                    // "LEVEL0" -> "level0"
bool apply_save_sector_stuff(const Save&, VmState&, LoadedSave&, &error);
bool apply_save(const Save&, GameRuntime&, SimPlayer&, LoadedSave&, &error);
bool apply_save(const Save&, VmState&, GameRuntime&, SimPlayer&, LoadedSave&, &error);
```

`apply_save()` требует уже инициализированный `SimPlayer` (`init()`): сейв не
несёт шасси, поэтому капсула/глаз/retention берутся из его
`ControllerConfig`. Дальше плеер **переинициализируется** в сохранённых
feet/yaw (это единственный документированный способ поставить игрока;
`init()` заново собирает конфиг из шасси, поэтому шасси восстанавливается из
`config()` — по `eye_height` и `max_speed / 91`, с точным обратным
преобразованием, см. `chassis_from_player()` в `.cpp`). Переинициализация
обнуляет счётчики жизни — загрузка и есть чистый старт.

`apply_save_sector_stuff()` нужен отдельно, потому что блок 8 — это дамп
рантайм-массива VM (`0x800b60`), и его приёмник (`VmState::sectors`) должен
быть заполнен **до** `GameRuntime::build()`: сборка читает `VmState`.
Перегрузка с `VmState` делает оба шага подряд, но порядок вызовов остаётся
за вызывающим.

## 2. Что применяется: поле → приёмник

| сейв | смещение | приёмник | доказательство |
|---|---|---|---|
| заголовок `level[0x50]` | `+0x54` | `LoadedSave::level` (имя уровня для `load_level`) | `docs/SAVE.md` §1.1 |
| игрок `position` | `+0x00/08/10` | `SimPlayer::state().pos` (`init(feet, ...)`) | `0x451d04..0x451d2f` |
| игрок `angle.yaw` | `+0x28` | `SimPlayer` yaw (`angles[2]`) | `0x451d3e..0x451d74`, `docs/CAMERA.md` §3 |
| игрок `armor`/`armor_max` | `+0x3248/0x3244` | `SimPlayer::vitals()` + `GameRuntime::player_vitals()` | `0x444084/0x44408a` |
| игрок `shields[4]`/`shield_max` | `+0x3250/0x324c` | те же виталы | `0x443c7d/0x443c8f` |
| игрок `weapon_slot` | `+0x3294` | `Arsenal::current` (`player_arsenal()`) | `0x44531c` |
| игрок `weapon_mask` | `+0x3298` | `Arsenal::mask` | `0x445328` |
| игрок `ammo[12]` | `+0x329c` | `Arsenal::ammo[12]` | `0x4455ad` |
| блок 8 `SECTOR_STUFF` | блок в файле | `VmState::sectors` (`vm_load_sector_stuff()`) | `0x46b7b0`, `docs/RECORDS.md` §2 |

Виталы ставятся дважды и намеренно: `SimPlayer::set_vitals()` — это
валидатор (`docs/PLAYER_FIELDS.md` §11: `armor_max > 0`,
`0 <= armor <= armor_max`, `0 <= shields[i] <= shield_max`), а
`GameRuntime::set_player_vitals()` кладёт их в собственный span рантайма,
которым бьёт `apply_projectile_blasts_to_players()`. Точка глаза —
`feet + config().eye_height`, ставится `set_player_eyes()`, поэтому виталы
принимаются (размеры списков совпадают).

Арсенал пишется прямо в `runtime.player_arsenal()`: таблицы оружия в сейве
нет, её даёт уровень (`Weapons::load(level0.sbt)`), а сейв несёт только
загрузку (слот/маска/патроны). CLI ставит таблицу после `apply_save()` через
`set_player_weapons(table, loaded.arsenal)`.

## 3. Что не применяется и почему

`LoadedSave::unsupported` заполняется по факту содержимого файла: одна
строка на заголовок, на каждый блок без приёмника и на поля игрока без
приёмника. Для поставочных сейвов это **16** строк (без `VmState`) или
**15** (с применённым блоком 8).

| что | почему не применяется |
|---|---|
| заголовок `F0..F21` (часы уровня, миссия, режим игры, опции) | в `GameRuntime` нет ни одного поля-приёмника; часы рантайма — это счётчик тиков `clock()`, а не `F1` |
| блок 1 switches (24) | рантайм ставит таблицу `ADD_SWITCH` из `.sbt` (`WorldState::switches()`), сохранённое состояние переключателей не переигрывается |
| блок 2 doors (25) | двери строятся из `.sbt`; `state`/`phase`/`delta` сейва не восстанавливаются (это следующий срез) |
| блок 3 things (6 слотов) | запись `0x528` не картирована (`docs/SAVE_FIELDS.md` §10), переносить нечего |
| блоки 4/5/11/12/15/17/20 | таблицы/скрипт/ассеты: их заново строит `vm_state_from_level()` из `.sbt`, а ITEM/NAMED ITEMS — инвентарь, модели в рантайме нет |
| блок 6 sprites (4), блок 7 sound sources (6) | в `GameRuntime` нет приёмника (спрайты/источники звука не часть тика) |
| блок 19 ITEM | состояние предметов, модели нет |
| игрок `history[240]`, `history_index`, `attachments[16]`, `object_id`, `exists`, `present` | нет приёмника в `SimPlayer`/`GameRuntime`; кольцо истории — это прошлые позиции, а не текущее состояние |
| кадры `0x16b0`, `0x2ff8/0x3078/0x30f8` | неизвестные/гипотетические поля (`docs/SAVE_FIELDS.md` §9.2), не переносятся |
| блок 8 без `VmState` | у базовой формы `apply_save()` нет приёмника: строка попадает в `unsupported` (в CLI блок 8 применяется до сборки рантайма) |

Ничего не «додумывается»: поля без доказанного адреса-приёмника остаются в
списке, а не превращаются в значение по умолчанию.

## 4. CLI

```
--save-load SLOT|PATH      загрузить сейв в рантайм (SLOT = 0..19, путь — файл)
--save-load-ticks N        прогнать N фиксированных тиков 91 Гц после загрузки
--level NAME               переопределить уровень из заголовка
```

`SLOT` резолвится как `SAVEGAME/saved<N>.sbt` без учёта регистра
(`resolve_ci`), путь — как есть или относительно `--game`.
Уровень — `--level`, иначе имя из заголовка (тоже без учёта регистра;
`LEVEL0` → `level0`). Дальше тот же путь, что у `--sim`:
`load_level` → `vm_state_from_level` → `apply_save_sector_stuff` →
`GameRuntime::build` → `SimPlayer::init` (LIGHT-шасси уровня) →
`apply_save` → `set_player_weapons(table, arsenal)` → `runtime.tick(N)`.

```
$ ./build/sandbox/engine/gunmetal --save-load 0
save-load: slot=0 level=level0 pos=(1116.174, 798.208, 48.000) armor=10000 shields=8000/8000/8000/8000 ammo=2000,90 sector_stuff=13 unsupported=15 ticks=0
save-load: level=level0 sectors=58 objects=192 things=121 doors=25 switches=24 sector_stuff_slots=256 applied=1 blocks=15
save-load: unsupported: header F0..F21 (level clock, mission, game mode, options): no runtime receiver
...
save-load: unsupported: player history ring/history_index, attachments[16], object_id/exists/present: no runtime receiver

$ ./build/sandbox/engine/gunmetal --save-load 1 --save-load-ticks 60 | tail -1
save-load: after ticks pos=(1156.355, 797.276, 48.000) armor=10000

$ ./build/sandbox/engine/gunmetal --save-load 99; echo $?
[error] save-load: save slot 99 out of range [0, 19]
2
```

`unsupported=N` — число строк в `LoadedSave::unsupported` (они печатаются
следом). Ошибки (нет файла, нет `.sbt`/`.sbw` уровня, битый сейв, слот вне
0..19, нет блока 9, виталы вне инвариантов, слот оружия вне 0..11, блок 8 не
на 256 слотов) — `[error] save-load: …` и код 2.

## 5. Тест: числа

`ctest -R save_load` (и `gm_save_load_tests`): **151 проверка**, зелёный.
Оба сейва читаются из установки; «битые» варианты — копии в памяти.

```
save_load: saved0 pos=(1116.173752, 798.207621, 48.000000) yaw=5.073615 armor=10000/10000 shields=8000/8000/8000/8000 ammo=2000,90 mask=0x3 slot=0 blocks=15 sector_stuff=13/256 unsupported=16
save_load: saved1 pos=(1156.355385, 797.275830, 48.000000) yaw=4.747052 armor=10000/10000 shields=8000/8000/8000/8000 ammo=1975,87 mask=0x3 slot=1 blocks=15 sector_stuff=13/256 unsupported=16
save_load: level=level0 sectors=58 objects=192 things=121 doors=25 switches=24 sector_stuff=256 slots, 13 active, vm=13, applied=1
save_load: ticks=60 clock=60 digest0=26726fa104161c45 digest1=2bba3655dedd9d51 deterministic=1 motion_stepped=6876 motion_moved=5572
save_load: armor=0 -> alive=0 runtime_armor=0 clock=3
save_load: broken save -> "truncated sample counts at 0x13d (need 1200, have 683)"
save_load: missing level -> "level 'nolevel': nolevel.sbw not found under /mnt/d/games/Gunmetal 3dfx (no nolevel.sbt either -- wrong level name?)"
save_load: resolve slot19=missing slot99=refused slot1=ok
save_load: no block 9 -> "save has no player block (9)"
save_load: armor>max -> "set_vitals: armor must be in 0..armor_max"
save_load: weapon slot 12 -> "player weapon slot 12 out of range [0, 11]"
save_load: sector_stuff size -> "SECTOR_STUFF: expected 256 slots, got 255"
save_load: 151 checks, ok
```

Что именно проверено:

1. **Поза**: `state().pos` и `yaw` равны полям блока 9 бит в бит (saved0 —
   `(1116.173751898651, 798.2076214059642, 48)`, yaw `5.0736149946795805`;
   saved1 — `(1156.355385156283, 797.2758300679517, 48)`, yaw
   `4.747052494679579`); `init()` сохранил `max_speed`/`eye_height` конфига
   без дрейфа.
2. **Виталы в рантайме**: `player.vitals()` и *собственный*
   `runtime.player_vitals()[0]` — `armor 10000/10000`, `shields 8000 ×4`,
   `shield_max 8000`.
3. **Арсенал в рантайме**: `runtime.player_arsenal()` — saved0 `ammo 2000/90`,
   saved1 `1975/87`, `mask 0x3`, `current` 0/1; после
   `set_player_weapons(level0.sbt, ...)` значения те же.
4. **Уровень из заголовка**: `level0` — 58 секторов, 192 объекта, 25 дверей,
   24 переключателя, 121 thing в рантайме.
5. **Блок 8**: 256 слотов, 13 активных (5, 9, 11, 13, 14, 19, 34, 35, 48, 50,
   51, 54, 57), в `VmState` тоже 13; слоты 5 и 57 совпадают с сейвом
   поле-в-поле; пустой слот 0 остаётся нулевым.
6. **Тики**: два свежих прогона по 60 тиков дают одинаковый digest
   (`2bba3655dedd9d51`), часы 60, мост AI→движение действительно двигает
   things (`motion_stepped=6876`, `motion_moved=5572`), состояние отличается
   от загруженного (`digest0 ≠ digest1`).
7. **Смерть из сейва**: `armor = 0` → `alive() == false`, в рантайме
   `armor = 0`, три тика не падают.
8. **Негативы**: обрезанный файл, отсутствующий уровень, отсутствующий
   `saved19.sbt`, слот 99, сейв без блока 9, блок 9 без present-слота,
   неинициализированный `SimPlayer`, `armor > armor_max`, слот оружия 12,
   блок 8 на 255 слотов — все с текстом (`apply_save()` возвращает `false`).

CLI-тесты: `cli_save_load` (строка сводки saved0), `cli_save_load_ticks`
(`after ticks` для saved1), `cli_save_load_bad` (слот 99, `[error]`, код 2).

## 6. Ограничения (следующий срез)

1. **Вирд/геометрия уровня берётся из установки**, а не из пары
   `saved<N>.sbw`: имя из заголовка резолвится как `<name>.sbw/.sbt/.sbc`
   (`load_level`). Сейв самодостаточен для `.sbt`, но `.sbw` в нём нет.
2. **Состояние мира не восстанавливается**: двери (блок 2), переключатели
   (блок 1), things (блок 3), спрайты/звуки — рантайм строит их из уровня.
   Сохранённые `state`/`phase`/`delta` дверей и вещи — отдельная задача
   (`docs/SAVE_LOAD.md` §3 — полный список).
3. **Скриптовое состояние не переигрывается**: блок 11 не исполняется,
   `VmContext` (режим/сложность/god) берётся из `--sim-mode`/`--sim-difficulty`
   /`--god`, а не из `F14..F17` (их потребитель — гипотеза).
4. **Блок 8 доходит до `VmState::sectors`**, но `GameRuntime` не исполняет VM,
   поэтому живых `SET_SECTOR_*` эффектов в этом срезе нет; приёмник —
   ровно документированный загрузчик `0x46b7b0`.
5. **Инвентарь/предметы/топливо**: в модели рантайма нет ни `ITEM` (блок 19),
   ни `NAMED ITEMS` (блок 5), ни счётчиков топлива — переносить некуда,
   поэтому они в `unsupported`.
6. **Запись сейва не выполняется**: модуль только читает. Сохранение —
   `res/save_write.*` (`--dump --save-out`), оно сюда не подключено.

## 7. Проверка

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_save_load_tests gunmetal -j 16
./build/sandbox/engine/tests/gm_save_load_tests
ctest --test-dir build/sandbox -R 'save_load|save-load' --output-on-failure
ctest --test-dir build/sandbox -j 4
python3 regress.py --mirror --json
```
