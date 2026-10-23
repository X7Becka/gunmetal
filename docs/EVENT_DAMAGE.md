# Событийный урон по вещам: `damaged_event` → `GENERATE` → blast

Статус: `M6.2.5`. Код: `engine/src/game/events.h/.cpp`
(`EventBlastRegistry`, `build_event_registry()`, `apply_event_blast()`,
`chase_damaged_events()`), мост в рантайме — `GameRuntime::apply_projectile_blasts()`
(`engine/src/game/runtime.h/.cpp`). Тест — `engine/tests/test_event_damage.cpp`
(CTest `event_damage`, label `quick`).

## 1. Цепочка оригинала

```
попадание 0x477280
  -> damage_thing 0x470500
     -> вещь жива: event_post(damaged_event)         0x470995
        (id: THING_TYPES+0x1f4, копия в инстансе +0x518)
  -> event_dispatch 0x4799b0 -> generate_event 0x479960
  -> generate_runner 0x45fa00: подскрипт события по id (поиск 0x45fa5c)
  -> ВМ 0x457390: опкод 75 CAUSE_BLAST_RADIUS_DAMAGE 0x45775b
  -> 0x477280: спад plate*(radius-d)/radius по игрокам и вещам
```

Смежные id того же блока `THING_TYPES`: `+0x1c4` collide, `+0x1ec` expire,
`+0x1f0` create, `+0x1f8` destroy, `+0x1c0` timeout. Бласт принадлежит **id
события**, а не `Weapon`; поэтому реестр ключуется id, а не слотом оружия.

Опкоды (по `gm/sbtvm.py`, дизасм `docs/VM_EXEC.md` §2):

| опкод | имя | примечание |
|---:|---|---|
| 2 | `GENERATE` | переход к подскрипту события; цепочка идёт только по нему |
| 3 | `GENERATE_FOR_CHILDREN` | не преследуется (в поставе 2 команды на уровень) |
| 4 | `GENERATE_FOR_PARENT` | не преследуется (8 команд на уровень) |
| 75 | `CAUSE_BLAST_RADIUS_DAMAGE` | операнды `LLLLLE`: flags, no_direction, plate, hull, radius, event |

## 2. Реестр из данных уровня

`build_event_registry(level, out, error, stats)` — детерминированный проход по
`SCRIPT` (запись 11):

* таблица handle-ов: `n` строк, колонки `id`/`offset`; id ищется линейно
  (`0x457c49`), при дубле id побеждает **первая** строка (так же читает
  `weapons.cpp::entity_block`);
* блок события = `[offset, следующий offset)` — граница по всем offset-ам
  уровня, ровно как в `tools/re/impact.py` и `Weapons::load()`;
* внутри блока рекурсивно (включая тела `IF`/`ELSE`) собираются опкоды 2 и 75;
  `GENERATE` (2) идёт в подскрипт с глубиной до `kEventBlastChainDepth = 4`,
  повтор id в стеке — цикл и обрезается;
* ветки `IF SOURCE_UNDERWATER` / `SOURCE_FROM_GROUND` / `DEATHMATCH_GAME`
  переносятся в поля `Blast` (`air/water/ground/not_ground/deathmatch/...`) —
  та же модель, что у `Weapons::load()`.

Реестр заменяется целиком; `stats` — это перепись данных, а не догадка:

| уровень | handle-строк | distinct id | декодировано | `GENERATE` (2) | опкод 75 | id с бластами | бластов в реестре | циклов | обрезано по глубине |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `dm1` | 363 | 355 | 355 | 241 | 83 | 61 | 121 | 9 | 0 |
| `level0` | 350 | 346 | 346 | 212 | 78 | 56 | 114 | 18 | 4 |

Числа — вывод `event_damage` (см. §5) и независимо `tools/re/impact.py`-модель
(скрипт переписи, 36 уровней): 14 812 id, 10 933 `GENERATE` (опкод 2),
3 098 опкодов 75. Реестр `level0` совпадает с путём оружия: ROCKET
(`thing_type` 31) collide/expire = `-30103`, у обоих по 3 бласта — ровно
`Weapon::impact_blasts`/`expire_blasts` от `Weapons::load()`.

## 3. Что доказано про `damaged_event` (отрицательный результат)

Перепись по всем 36 поставочным уровням (границы блоков — как в §2, цепочка
`GENERATE` до 4):

| величина | значение |
|---|---:|
| записей `THING_TYPES+0x1f4` с ненулевым id | 531 |
| различных пар (уровень, id) | 337 |
| из них id с декодируемым блоком | 272 |
| **из них id, у которых блок или `GENERATE`-цепочка содержит опкод 75** | **0** |
| destroy-событий (`+0x1f8`) с бластом в цепочке | 1 892 |

То есть в поставочных подскриптах `damaged_event` **не содержит**
`CAUSE_BLAST_RADIUS_DAMAGE` ни прямо, ни через `GENERATE`: цепных взрывов
«от попадания в вещь» в данных нет. `dm1`: 5 записей (типы 41…45 `SenTop`,
`H-Tick`, `H-Spart`, `H-Steal`, `H-Dev`), 5 id, 0 с бластом; самый частый id
`-30129` вообще не имеет блока в таблице handle-ов. `level0`: 8 записей
(+`BAGCRTHQ` `-30201`, `HQTICK`/`HQDEV` → `-30129`), 6 id, 0 с бластом.
Бласты у вещей живут на collide/expire/destroy — это путь `M6.2`/`M6.5.1`, не
этот.

Поэтому связка «урон по вещи → `damaged_event` → цепной blast» на поставе —
**нулевой урон**, и это зафиксировано тестом (`0/5`, `0/6`). Механизм при этом
рабочий: ниже он проверен на синтетическом реестре и на реальном id `dm1`,
подставленном в `damaged_event`.

## 4. Применение цепочки

`chase_damaged_events(registry, applied, damaged_events, positions, alive,
player_vitals, thing_vitals, context)`:

* идёт по `applied.hits` в порядке попаданий; игроков пропускает;
* берёт `damaged_events[i]` только у вещи, которая **выжила**
  (`ThingDamageResult::damaged_event`); погибшая вещь — это `destroy_event`
  (`+0x1f8`), он здесь не моделируется (`skipped_dead`);
* `0` → `skipped_no_event`; id нет в реестре → `skipped_missing`
  (no-op, не ошибка); id уже в стеке → `skipped_cycle`; глубина >
  `context.max_depth` (по умолчанию 4) → `skipped_depth`;
* бласт применяется `apply_event_blast()` в позиции вещи; результат
  рекурсивно преследуется с глубиной +1 и дополненным стеком id;
* `damage.hits[i]` соответствует `centers[i]` (центр шага), поэтому мост
  рантайма постит `event` каждого попадания в правильной точке.

Порядок — DFS в порядке `applied.hits`, реестр отсортирован по id, хеш-таблиц и
RNG нет: результат детерминирован. `GameRuntimeConfig::chain_damaged_events`
(по умолчанию `true`) включает цепочку в registry-перегрузках
`apply_projectile_blasts*`; `Weapon`-перегрузки её не делают (у них нет
реестра). Счётчики — `ProjectileBlastBatch::chain`.

## 5. Числа теста `event_damage`

```
event_damage: dm1 SCRIPT handles=363 ids=355 decoded=355 GENERATE=241 blast_ops=83 -> registry ids=61 blasts=121 (cycles=9 depth_cut=0)
event_damage: level0 SCRIPT handles=350 ids=346 decoded=346 GENERATE=212 blast_ops=78 -> registry ids=56 blasts=114 (cycles=18 depth_cut=4)
event_damage: level0 ROCKET collide=-30103 (3 blasts) expire=-30103 (3 blasts) match Weapons::load
event_damage: dm1 THING_TYPES damaged_event: 5 records, 5 distinct ids, 0 with a blast (sample type 'SenTop' id=-30129)
event_damage: level0 THING_TYPES damaged_event: 8 records, 6 distinct ids, 0 with a blast (sample type 'SenTop' id=-30129)
event_damage: real damaged_event dm1 type 'SenTop' id=-30129 (event 900000 primary): hp 1000000 -> 990000, chain events=0 -> 990000
event_damage: cycle graph events=4 blasts=4 hits=8 cycles=6 hp=(9524,9536)
event_damage: depth chain max=1 -> events=3 blasts=3; max=4 -> events=63 blasts=63 hits=255 depth_cut=122
event_damage: real chain dm1 type 'bgchunk1' event=-30071 blasts=2: hp 400000+400000 -> 399500 (primary hits=2) -> 398552 (chain events=2 blasts=2 hits=4 depth=1)
event_damage: runtime chain=1 hp=(7702,8702) events=2 hits=4; chain=0 hp=(8000,9000) events=0
```

Что это значит:

* **циклы**: две вещи, чьи `damaged_event` (300/301) ссылаются на два
  взаимных бласта; без разрезания рекурсия бесконечна. Стек id даёт ровно
  4 применения, 6 обрезанных циклов, глубину 2 — и конечные hp `(9524, 9536)`;
* **глубина**: 6 вещей в линию, каждый бласт достаёт соседей; при
  `max_depth=1` — 3 события, при 4 — 63 события, `depth_max == 4`,
  122 обрезания по глубине;
* **реальный `damaged_event`**: `dm1`, тип `SenTop`, id `-30129`; прямое
  попадание реальным бластом `900000` даёт `1000000 -> 990000`, цепочка —
  `chain events=0` и те же `990000` (id нет в реестре);
* **реальные данные**: `dm1`, тип `bgchunk1`, реальный id `-30071` (2 бласта),
  подставлен в `damaged_event` двух вещей по 400 000 hp: прямое попадание
  `400000 -> 399500`, цепочка `-> 398552` (2 события, 4 попадания);
* **рантайм**: снаряд с `expire_event=700` рядом с двумя ящиками
  (`damaged_event=701`, hp 10 000): с цепочкой hp `(7702, 8702)`,
  без (`chain_damaged_events=false`) — `(8000, 9000)`; `chain.events=2`,
  `hits=4`, `skipped_cycle=4`.

## 6. Доказано / гипотеза / ограничения

**Доказано (данные + код + тест):**

* адреса цепочки: `0x470995` (`event_post` после несмертельного урона
  `0x470500`), `0x4799b0`, `0x45fa00`, опкод 75 `0x45775b` → `0x477280`
  (`docs/DAMAGE.md` §2.3, §5.1; `docs/PROJECTILE_CPP.md` §7);
* границы блоков и первые строки handle-таблицы совпадают с
  `tools/re/impact.py`; перепись dm1/level0 совпадает с реестром;
* в поставочных `damaged_event` опкода 75 нет: 0 из 272 блоков (531 запись,
  337 id, 36 уровней); у destroy — 1 892, поэтому именно destroy, а не
  damaged, несёт цепные взрывы в данных;
* механизм цепочки конечен при циклах и ограничен глубиной (числа §5).

**Гипотеза:**

* что оригинал постит `damaged_event` на **каждое** несмертельное попадание
  площадного урона, а не только на «первое» (в коде `0x470995` виден вызов
  `event_post`, но счётчик/дедуп не проверялся);
* порядок обхода попаданий (в тесте — порядок `BlastResult::hits`, т.е.
  игроки, затем вещи по индексу). В оригинале порядок задаёт цикл `0x477280`;
* что `GENERATE`-подскрипт выполняется без `SLEEP`-задержек: отложенные
  события не моделируются.

**Ограничения:**

* не моделируются опкоды 3/4 (`GENERATE_FOR_CHILDREN`/`PARENT`), `SLEEP`,
  звук, анимации/эффекты, `FILM_*`, `SET_*`, спавн вещей из подскрипта: реестр
  собирает только статические бласты (с тремя guard-условиями), остальные
  условия ВМ не вычисляются;
* цепочка идёт только по `damaged_event`; `destroy_event` (`+0x1f8`) и его
  бласты не применяются, `destroy`-анимаций нет — это отдельный пункт;
* у вещи без world-позиции (source-relative) цепочка не запускается: попадание
  в такую вещь в `apply_blast` всё равно отбрасывается по `thing_alive`;
* координаты `event`-операнда бласта для каждого попадания не доказаны: в
  мосте они постятся в центр шага (позиция вещи), как и раньше для первичного
  бласта — в центр результата снаряда;
* урон в цепочке считается по тем же `Vitals`/`ThingVitals`-span-ам, что и
  первичный бласт; `Things` не обновляется, пока вызывающий не сделает
  `store_thing_vitals()`;
* числа `GENERATE` в §2 — это опкод 2 в distinct-блоках; `impact.py`-модель с
  тем же правилом даёт те же 241/212. Сумма всех `GENERATE*` на 2/8 больше.
