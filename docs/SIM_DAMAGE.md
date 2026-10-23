# Урон игроку, смерть и респавн (`game/sim_player.*`, `M6.5.1`, срез 3)

Снимок 2026-10-23. Срез 2 (`docs/SIM_PROJECTILE.md`) доводил выстрел до
взрыва, но игрок был неуязвим: в мост взрыва передавался пустой
`player_vitals`, урон получали только вещи. Этот срез закрывает сторону
игрока **на готовых API**: `SimPlayer` держит свои шкалы (`Vitals`),
ставит их на рантайм (`GameRuntime::set_player_vitals`), взрывается через
`apply_projectile_blasts_to_players`, читает шкалы назад и считает урон,
смерть (`armor <= 0`) и респавн.

Новой физики и правок рантайма нет: изменены только
`engine/src/game/sim_player.h`, `engine/src/game/sim_player.cpp`, добавлен
тест `engine/tests/test_sim_damage.cpp` (CTest `sim_damage`, метка `quick`)
и строка в `engine/tests/CMakeLists.txt`. CLI-проводки нет:
`--sim-respawn` — следующий шаг (§6).

## 1. API

```cpp
class SimPlayer {
public:
    // стартовые шкалы текущей/следующей жизни; ставятся на рантайм
    // в начале следующего tick()
    bool set_vitals(const Vitals& vitals, std::string* error = nullptr);

    // вернуть позу init() и стартовые шкалы, +1 к deaths()
    bool respawn(std::string* error = nullptr);

    bool alive() const;        // armor > 0
    int  death_tick() const;   // tick_index() смертельного тика, -1 до смерти
    int  deaths() const;       // число успешных respawn()
    int  damage_taken() const; // сумма потерь armor + 4 щитов, сотые
    const Vitals& vitals() const;
};
```

Стартовые шкалы по умолчанию — LIGHT в единицах сейва
(`kLightPlayerVitals`): `armor = 10000`, `armor_max = 10000`,
`shield_max = 8000`, `shields = 8000/8000/8000/8000`, множители `1.0`.
Это числа `docs/DAMAGE.md` §7 (`shield_max 8000`, `armor_max 10000`, плиты
`8000` ×4) и `docs/PLAYER_FIELDS.md` §11 (в `saved0` броня
`10000/10000`, щиты `8000` ×4; шасси LIGHT — `op2 = 80` → `+0x324c`,
`op3 = 100` → `+0x3244`, `docs/CHASSIS.md` §4).

`set_vitals()` проверяет инварианты сейва (`docs/PLAYER_FIELDS.md` §11):
`armor_max > 0`, `0 <= armor <= armor_max`, `shield_max >= 0`,
`0 <= shields[i] <= shield_max`, конечные неотрицательные
`mult_plate`/`mult_hull`. Нарушение — `false` и текст в `error`, прежние
шкалы не меняются (не «зажимаем» молча). Вызывать можно до и после
`init()`; на рантайм функция не пишет — только помечает шкалы грязными.

`respawn()` возвращает `PlayerState` из `init()` (та же точка `feet`, тот же
`yaw`), стартовые шкалы и `alive() = (armor > 0)` (для шкал по умолчанию —
`true`), увеличивает `deaths()`.
`death_tick()` после респавна хранит **последнюю** смерть. Счётчик тиков
(`tick_index()`) не сбрасывается, шаблон снаряда и боезапас рантайма не
трогаются — поза и шкалы восстанавливаются на следующем тике, поэтому план
остаётся детерминированным. До `init()` — `false` и текст.

## 2. Порядок тика (режим снаряда)

```
install_projectile(runtime)
yaw += segment.turn * dt
step(...)                                  # player/controller.h
runtime.set_player_eyes({eye})
install_vitals(runtime)                    # set_player_vitals, если dirty/другой рантайм
если segment.fire: runtime.update_player_weapon(...)
runtime.tick(1)
apply_blasts(runtime)                      # apply_projectile_blasts_to_players
vitals_ = runtime.player_vitals()[0]       # обратное чтение
damage_taken_ += потери armor и 4 щитов
если alive_ и vitals_.armor <= 0: смерть (death_tick_ = ticks_done_)
```

`install_vitals()` вызывается **после** `set_player_eyes()`: рантайм хранит
ровно одну запись шкал на точку глаза (`docs/RUNTIME_CPP.md` §7), и до
первого тика списка игроков ещё нет. Флаг `vitals_dirty_` ставится в
`init()`, `set_vitals()` и `respawn()`, поэтому восстановленные шкалы
попадают на рантайм в первом тике новой жизни.

`apply_blasts()` теперь зовёт
`GameRuntime::apply_projectile_blasts_to_players(weapon, context,
thing_vitals)` — ту же weapon-keyed ветку, что и срез 2, но шкалы игрока
берутся из `player_vitals_` рантайма. Урон вещам по-прежнему пишется через
`store_thing_vitals()`, счётчики `blast_hits()`/`things_damaged()` не
изменились (попадание по игроку считается в `blast_hits()` и реально
наносит урон).

## 3. Правило смерти

Смерть — **`armor <= 0` после записи урона**. Это правило оригинала:
`0x453b50` (`apply_damage_gauge`) вычитает `spill` из `+0x3248` и при
`armor <= 0` вызывает `0x4536e0`, который пишет `armor = 0`, ставит
`action_state = 7` и постит `-7` (`docs/DAMAGE.md` §2.7, §3;
`docs/PLAYER_FIELDS.md` §11 — `+0x3248` `armor`).

Про `status_flags & 0x20`: в этом движке это **не смерть**, а «вспышка
попадания в щит»/горение — `0x453b50` ставит бит `0x20` вместе с таймером
`+0x3224` при `plate = -1` (`docs/DAMAGE.md` §2.2), а в
`docs/PLAYER_FIELDS.md` §7 бит `0x20` поля `+0x3200` описан как «вспышка
попадания в щит». Поэтому `alive()` не смотрит на `Vitals::status_flags`
(иначе одно попадание огнемёта «убивало» бы игрока); бит сохраняется в
шкалах как есть, а смерть определяется по броне. Отличие от формулировки
задания зафиксировано здесь явно.

Порядок внутри одного взрыва не менялся: сначала порог/плиты
(`apply_damage_gauge`: `flags & 2` → полный урон в плиту, иначе `plate >> 2`
на каждую; `side = -1` для взрыва), затем `spill` в броню, затем проверка
смерти.

## 4. Что происходит после смерти

Тик мёртвого игрока — **успешный no-op**: ни движения, ни выстрела, ни
`runtime.tick(1)`. `state()`, `tick_index()`, `vitals()` и `damage_taken()`
не меняются, пока не вызван `respawn()`. Это отличает срез от оригинала
(там мир продолжает тикать, а игрок лежит в `action_state = 7`), зато
детерминизм плана не зависит от того, сколько тиков вызывающий «прокрутил»
после смерти. Альтернатива (тикать рантайм и игнорировать ввод) описана в
§5 как возможное расширение.

## 5. Ограничения среза (с причинами)

* **Нет анимации смерти, трупа, эффекта и звука.** `0x4536e0` зовёт
  `0x4569d0` (эффект/взрыв) и меняет `action_state`; рендер/аудио в
  `SimPlayer` не подключены ни в одном срезе.
* **Нет события `-7` и событий плит.** Мост взрыва постит только события
  попаданий (`BlastHit::event`); `-17 LIFE_HALF_GONE`, `-12/-15/-13/-14`
  и `-7` (`docs/DAMAGE.md` §2.8, §3) в этом слое не формируются — очередь
  событий рантайма их не получает.
* **Респавн мгновенный и ручной.** `respawn()` — это поза + шкалы + счётчик.
  Оригинальный респавн идёт по правилам DM (`0x452e67`/`docs/DM_RULES.md`
  §3.2: задержка, выбор точки, деньги, фраги); здесь ни задержки, ни выбора
  точки, ни фрагов нет.
* **`hitstun`/`burn_timer` не тикают.** `apply_damage_gauge` пишет
  `hitstun` (`+0x33c8`) и `burn_timer` (`+0x3224`), но их спад
  (`0x443716`, `ticks*0x4000`) не моделируется: поля только сохраняются в
  `vitals()`.
* **`damage_taken()` — потеря шкал, не «нанесённый урон».** Считается
  `armor` + 4 щита до/после тика; перелив за обе ёмкости (`GaugeDamageResult
  ::overflow`) не учитывается, лечение/восстановление шкал сумму не
  уменьшает.
* **Один игрок.** Рантайм хранит шкалы в размер списка глаз, `SimPlayer`
  ставит ровно одну точку; мультиплеер (8 слотов) — не этот срез.
* **Контекст взрыва прежний.** `underwater = false`, `from_ground = true`,
  `deathmatch = true`, `source_player = owner_player`. Самоурон работает,
  пока у blast’а нет бита `flags & 1`: `apply_blast()` пропускает
  игрока-источника именно по этому биту (`docs/DAMAGE.md` §5.1). У ROCKET
  `level0` бит не выставлен, поэтому выстрел вплотную бьёт и стрелка; у
  оружия с `flags & 1` стрелок был бы неуязвим — это факт кода, а не
  решение `SimPlayer`.
* **Мёртвый игрок не двигает мир.** См. §4: `runtime.tick()` не зовётся,
  поэтому снаряды/двери/AI стоят. Для CLI это значит, что после смерти
  прогон «замирает» до `--sim-respawn`.
* **Респавн не сбрасывает оружие/патроны/снаряды.** Кулдаун оружия живёт в
  рантайме (`Arsenal`), шаблон снаряда — в `SimPlayer`; после респавна
  первый выстрел возможен только когда кулдаун дотёк.

## 6. Фактические числа (`dm1` + `level0` v1.36, 2026-10-23)

Сценарий тот же, что в срезе 2 (слабейшая damageable вещь с линией огня,
реальное направление `update_player_weapon()` — разброс ROCKET не прямая
линия):

```
start gauges (LIGHT, saved0): armor=10000/10000 shields=8000/8000/8000/8000
                             shield_max=8000
target [139] id=139 type=OILDRUM pos=(5176.00, 600.00, 264.00)
feet=(5154.79, 621.21, 264.00) yaw=5.497787 rad (315.0 deg)
stand_off=30 blast_from_eye=161.88
```

Один выстрел (30 тиков: ROCKET стреляет раз в 80):

```
shots=1 spawned=1 blast_hits=2 damage_taken=5060 alive=1
gauges: armor=10000/10000 shields=6735/6735/6735/6735
```

`blast_hits = 2` — два попадания одного взрыва (игрок и бочка). В игрока
достала только воздушная ветка `12000@256`: на дистанции 148
`12000*(256-148)/256 = 5062`, затем `5062 >> 2 = 1265` в каждую из четырёх
плит (`side = -1`), итого `4 * 1265 = 5060`; броня не тронута — сначала
плиты. Ветка `10000@80` до игрока не достала (148 > 80). Это и есть
«самоурон взрывом при малом уроне»: игрок ещё жив.

Серия до смерти:

```
fired 328 ticks: shots=5 spawned=5 blast_hits=7 damage_taken=42000
                 alive=0 deaths=0 death_tick=328
                 armor=0 shields=0/0/0/0
```

`damage_taken = 42000` — ровно полная ёмкость LIGHT (`10000 + 4*8000`),
смерть на тике 328 (пятый выстрел, снаряд летел 8 тиков). После смерти
5 тиков:

```
pos=(5154.786797, 621.213203, 238.000001) -> то же
tick 328 -> 328 damage 42000 -> 42000 alive=0
```

(Игрок за время серии успел упасть с `z = 264` до `z = 238` — в сценарии
нет пола под точкой `feet`; на смерть это не влияет, поза после смерти
заморожена.)

Респавн и урон после него:

```
respawn -> pos=(5154.786797, 621.213203, 264.000000) yaw=5.497787143782138
           alive=1 deaths=1 damage_taken=42000 death_tick=328
           gauges: armor=10000/10000 shields=8000/8000/8000/8000
250 ticks after respawn: damage 42000 -> 84000 shots 5 -> 8 alive=0
                         armor=0 shields=0/0/0/0
```

То есть шкалы действительно восстановлены и снова получают урон (вторая
жизнь потеряла те же 42000 и умерла снова); `damage_taken()` — сумма за
обе жизни, `death_tick()` хранит последнюю смерть.

Детерминизм (тот же сценарий на свежем рантайме):

```
replay ticks=328 shots=5 blast_hits=7 damage=42000 death_tick=328 armor=0
first  ticks=328 shots=5 blast_hits=7 damage=42000 death_tick=328 armor=0
```

Свои стартовые шкалы (`set_vitals armor=500/1000, shields=0`):

```
ticks=9 death_tick=9 damage_taken=500 armor=0 alive=0 shots=1
```

Негативы (печатаются тестом):

```
respawn before init -> ok=0 error="respawn: SimPlayer::init() has not been called"
set_vitals armor=-1 -> ok=0 error="set_vitals: armor must be in 0..armor_max"
set_vitals shields[2]=-5 -> ok=0 error="set_vitals: shields[2] must be in 0..shield_max"
set_vitals armor=armor_max+1 -> ok=0 error="set_vitals: armor must be in 0..armor_max"
set_vitals shield_max=-1 -> ok=0 error="set_vitals: shield_max must be >= 0"
```

## 7. Что осталось для CLI (`--sim-respawn`)

Проводка `main.cpp`/`config.*` — отдельная задача (её делает основной агент):

1. Флаг `--sim-respawn N` (или `--sim-respawn-at TICK`): вызывать
   `SimPlayer::respawn()` по счётчику/тику и продолжать план; печатать
   `deaths`, `death_tick`, `damage_taken`.
2. `--sim-vitals armor,shield` (или `--sim-god`/`--sim-hp`): проброс в
   `set_vitals()`; без флага — LIGHT из `saved0`.
3. Печать шкал: `armor/armor_max`, `shields[4]`, `alive`, `damage_taken`,
   `death_tick` рядом с существующими `shots`/`projectiles_spawned`.
4. События смерти: `-7`, `LIFE_HALF_GONE -17`, `-12/-15/-13/-14` — когда
   событийный слой будет подключён к CLI (сейчас мост их не постит).
5. Настоящий респавн по DM-правилам (`docs/DM_RULES.md` §3.2): задержка,
   точка спавна, деньги/гандикап, фраги; сейчас `respawn()` — поза+шкалы.
6. `ROADMAP.md` (M6.5.1) и `CURRENT_STATE.md` — статус ставит основной агент
   после merge; здесь только код, тест и этот документ.

## 8. Доверие

| утверждение | доказательство |
|---|---|
| стартовые шкалы = LIGHT/saved0 | `armor=10000/10000`, `shields=8000` ×4 в тесте; `docs/PLAYER_FIELDS.md` §11, `docs/DAMAGE.md` §7 |
| попадание по игроку наносит урон | `damage_taken=5060`, плиты `8000 -> 6735`, после одного выстрела |
| сначала плиты, потом броня | первый выстрел: плиты `-1265`, броня без изменений |
| смерть при `armor <= 0` | серия: `armor=0`, `shields=0`, `alive=0`, `death_tick=328` |
| смертельный тик = фактический | `death_tick == tick_index()`, `328 == ticks` |
| после смерти тик — no-op | 5 тиков: поза/тик/урон/шкалы не изменились |
| `respawn()` возвращает позу/шкалы | `pos` и `yaw` совпали с `init()` побитово, `armor 10000`, `shields 8000` ×4 |
| урон после респавна идёт снова | `damage_taken 42000 -> 84000`, `shots 5 -> 8` |
| `deaths()` считает респавны | `deaths=1` после одного `respawn()` |
| детерминизм | replay: те же ticks/shots/blast_hits/damage/death_tick |
| `set_vitals` доходит до рантайма | `armor=500` → смерть на тике 9 от одного взрыва |
| негативы отклоняются с текстом | пять строк выше; шкалы не изменились |

Проверка:

```sh
ctest --test-dir build/sandbox -R 'sim_damage|sim_projectile|sim_player|player_trigger' \
      --output-on-failure
python3 regress.py --mirror --json
```
