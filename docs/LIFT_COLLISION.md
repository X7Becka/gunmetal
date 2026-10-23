# Gunmetal — кабина лифта как движущийся коллайдер

Пункт roadmap: **M6.4.1, срез 4**. Модуль `engine/src/world/moving_collider.*`
(шов `update(fraction, lift_offset)` из срезов 1–3), интеграция —
`engine/src/game/runtime.cpp`, тест — `engine/tests/test_lift_collision.cpp`
(CTest `lift_collision`, label `quick`).
Смежные доки: `docs/MOVING_COLLIDERS.md` (§2.2 — шов), `docs/DOOR_COLLISION.md`
(§6 — интеграция лифта), `docs/DOOR_FIELDS.md` §3 (запись `LIFT`).

---

## 0. Коротко

| что | результат |
|---|---|
| привязка «лифт → объект `.sbw`» | **не найдена**: 0/45 лифтов на 36 уровнях |
| почему | кабина — **runtime-объект** из `SOB_TABLE[Lift::sob]`, а не объект уровня |
| что реализовано | шов `lift_offset` (уже был) + **caller-supplied** привязка `Lift::cabin_sector/cabin_object` и её интеграция в рантайм |
| движение кабины | панель из треугольников `(sector, object)`, перенос на `pos_delta * height/65535` (`0x44e6f0`), статика пары подавлена |
| обновление | только при смене `Lift::height`; направление и знак сохраняются |
| негатив | лифт без привязки не ломает тик: `lifts_unbound`, dynamic set не меняется |
| тест | `level14`, `lift0` KELLIFT, `pos_delta=(0,0,260)`: `bounds.z 722..760 → 982..1020`, луч `t 1240 → 980`, нормали `≤ 2.5e-16` |
| ограничения | кабина не «везёт» игрока (нет contact resolve); поворот базиса кабины (`basis[3..5] != 0`) не моделируется; геометрия из `SOBS/*.sob` не грузится |

---

## 1. Что делает оригинал

`Lift::sob` (`ADD_LIFT` operand 2 → запись `+0x38`) — это **индекс
`SOB_TABLE`**, а не `(sector, object)`. При создании лифта `0x44e800`
(`tools/ghidra/fncache.py get 0x44e800`) делает:

```
model_index_to_pointer(+0x38)      -> FUN_004277cd   // тело кабины
FUN_004360f1(тело, 0,0,0)
model_index_to_pointer(+0x38,...)  -> FUN_0043b639   // объект отрисовки
[record +0x30] = результат 0x43b639                  // runtime handle
```

То есть кабина **создаётся в рантайме** из модели `SOBS/<имя>.sob`
(`0x47c990`), как модель игрока в `0x452240`. `+0x30` — указатель на объект
(в сейве 0), `+0x38` — индекс модели (`docs/DOOR_FIELDS.md` §3,
`docs/RECORDS.md` §4). Никакого объекта `.sbw` у лифта нет by design.

Движение кабины — `0x450180` (тик) → `0x434e32` (вектор) → `0x44e6f0`:
`origin = pos + pos_delta * height/65535`, затем `0x41e025` строит базис из
трёх углов `basis[i] + basis[3+i] * fraction` (в поставе базовые углы у части
лифтов не нулевые, приращения — у поворотных кабин, например `RNDSWTCH` в
`level18`).

---

## 2. Поиск привязки (данные, без новой Ghidra)

Скрипт-проба (чтение `~/.cache/gunmetal-re/game`, `gm.scene` + `gm.sbw` +
`gm.sob`):

```python
# для каждого лифта l:
#  H1 origin(object) == lift_transform(0).origin          (точное ==, double)
#  H2 origin(object) == lift_transform(65535).origin
#  H3 origin(object) == lift_transform(height любой остановки).origin
#  H4 |origin(object) - origin(f=0)| < 1e-6
#  H5 origin.x/y == lift.x/y (любой z)
#  H6 объекты с f75 == 1
#  H7 геометрия SOBS/<sob>.sob == локальные вершины какого-либо .sbw-объекта
```

`lift_transform` — `origin[i] = pos[i] + pos_delta[i] * clamp(height,0,0xffff)/65535`.
`f75` — байт `0x75` заголовка объекта (в поставе 1 у 7 объектов).

### 2.1. Итог по корпусу

* **36** `.sbt`/`.sbw`, лифты есть на **17** уровнях, всего **45** лифтов.
* H1 (height 0): **0/45**.
* H2 (height 65535): **0/45**.
* H3 (любая остановка): **0/45**.
* H4 (допуск 1e-6): **0/45**.
* H5 (совпали x,y при любом z): **4/45** — и все четыре z далеки от кабины:
  `dm3 lift0` (0,0): ближайшие объекты на `z=-40, 28, 196, 576, 832, 1368`
  при кабине `-247`; `level14 lift0` (-128,5816): объекты `z=188, 736, 741,
  751` при кабине `620` (это полы/шахта, а не кабина); `level22 lift1`
  (-384,7424): объект `(13,32)` на `z=576` при кабине `-2142`; `level6
  lift0` (296,-1280): объект `(200,0)` на `z=-920` при кабине `-560`.
* H6: `f75 == 1` у **7** объектов на 36 уровнях; ближайший к любому лифту —
  **313.2** юнита (`level23`), т.е. ни один не кабина.
* H7: все **45/45** моделей лифтов есть в `SOBS/`; у **5/45** моделей
  локальный набор вершин совпал с каким-то `.sbw`-объектом, но это тривиальные
  квады/боксы (`RNDSWTCH` — 4 вершины, встречается в `dm1`, `level14`,
  `level18`; `RCLIFT` — 8 вершин в `level24`), и **ни один** не стоит в точке
  лифта. У 40/45 совпадений нет вовсе.
* Минимальное расстояние от `origin` объекта до `lift_transform(0).origin` по
  корпусу — **2.0** (`level13 lift2` и пластина `(20,29)`), но это
  плоскость-площадка, а не кабина: вершинного совпадения с моделью `HOLOLIFT`
  нет, и при `pos_delta=(0,0,122)` она не повторяет трансформ кабины.
  Расстояние <16 у **8/45**, <64 у **15/45** — всё это полы, шахты и детали
  декора.

### 2.2. По уровням

| уровень | лифтов | H1/H2/H3 (точных) | H5 (x,y) | min dist до `origin` |
|---|---:|---:|---:|---:|
| dm3 | 2 | 0 | 1 | 17.117 |
| level11 | 4 | 0 | 0 | 6.708 |
| level12 | 4 | 0 | 0 | 58.327 |
| level13 | 5 | 0 | 0 | 2.000 |
| level14 | 1 | 0 | 1 | 116.000 |
| level16 | 2 | 0 | 0 | 51.400 |
| level17 | 6 | 0 | 0 | 57.131 |
| level18 | 7 | 0 | 0 | 6.000 |
| level2 | 1 | 0 | 0 | 226.592 |
| level20 | 1 | 0 | 0 | 121.696 |
| level21 | 2 | 0 | 0 | 229.122 |
| level22 | 3 | 0 | 1 | 55.227 |
| level23 | 3 | 0 | 0 | 11.225 |
| level24 | 1 | 0 | 0 | 161.443 |
| level3 | 1 | 0 | 0 | 328.512 |
| level6 | 1 | 0 | 1 | 132.966 |
| level8 | 1 | 0 | 0 | 270.141 |
| **итого** | **45** | **0** | **4** | **2.000** |

Вывод: привязки в данных **нет**. Ни одна из гипотез H1–H7 не даёт
соответствия, которое можно принять без выдумывания. Поэтому в движке нет
автопривязки: `Lift::cabin_sector/cabin_object` по умолчанию `-1`, и все 45
поставочных лифтов попадают в `lifts_unbound`.

---

## 3. Что реализовано

### 3.1. Модель

`engine/src/world/doors.h`, `Lift`:

```cpp
int cabin_sector = -1;   // .sbw-объект кабины; -1 = нет привязки
int cabin_object = -1;
bool cabin_bound() const { return cabin_sector >= 0 && cabin_object >= 0; }
```

Поля не читаются из `.sbt`/сейва и не заполняются эвристикой: их ставит
только вызывающий (тест или отредактированный уровень). `WorldState` получил
`lifts_mutable()` для этой привязки и восстановления состояния.

### 3.2. Рантайм

`GameRuntime::build_door_panels` после панелей дверей обходит `world.lifts()`:

* `!cabin_bound()` → `lifts_unbound++`, панели нет;
* `basis[3..5] != 0` (кабина поворачивается за ход) →
  `lifts_skipped_rotating++`: шов умеет только перенос, поза не выдумывается;
* пара уже занята дверью → `lifts_skipped_duplicate++`;
* `pos_delta == 0` → `lifts_skipped_static++`;
* иначе `MovingPanel{rest = все треугольники (sector, object),
  travel = pos_delta}`, `MovingCollider::set_panels(..., DoorAxis::Travel)`,
  `update(lift_fraction(lift), 0)`; пара уходит в `suppressed`, статика
  вырезается из запросов `CollisionWorld`.

`sync_door_colliders` хранит `applied_lift_height` и перекладывает панель
только когда `Lift::height` изменился (сравнение целых), публикуя dynamic set
через тот же `publish_door_dynamic`. Полная дельта — `pos_delta *
height/65535`, знак сохраняется (кабина едет и вниз; `direction` учтён в
`height`). `lift_offset` остаётся отдельным швом для панели, у которой есть
только вертикальная составляющая.

Новые счётчики `DoorCollisionStats`: `lifts`, `lift_panels`, `lifts_unbound`,
`lifts_skipped_static`, `lifts_skipped_rotating`, `lifts_skipped_duplicate`,
`lifts_skipped_refused`.

### 3.3. Исправление нулевой позы

`MovingCollider::pose` копировал `rest` бит-в-бит только для `Rotating` при
`f = 0`. Скользящая панель/кабина при `f = 0` шла через `moved_triangle`,
которая пересчитывает `centroid`, `plane_offset` и нормаль — совпадение
«побитово» не гарантировалось. Теперь `identity = (fraction == 0 &&
lift_offset == 0)` для всех режимов; при `f = 0` панель — точная копия
статики. Это же требование проверяет `door_collision`/`lift_collision`.

---

## 4. Тест `lift_collision`

Реальный уровень: **level14**, лифт **0** (`KELLIFT`), `pos=(-128,5816,620)`,
`pos_delta=(0,0,260)`, `speed=300`, остановки `[0, 65535]`, `height=0`.
Привязка в тесте — реальная площадка **`sec=35 obj=1`**
(origin `(-128,5816,741)`, AABB `-243..-13 × 5701..5931 × 722..760`,
52 треугольника, верхняя грань `+Z` на `z=760`).
Это **стенд-ин**, а не найденная привязка: по §2 кабины в `.sbw` нет.

Прогон:

```
$ ./build/sandbox/engine/tests/gm_lift_collision_tests
lift_collision seam: level14 lift0 sob=79 pos=(-128,5816,620) pos_delta=(0,0,260) speed=300 stops=2 full_fraction=1
  plate sec=35 obj=1: 52 triangles, bounds (-243.000, 5701.000, 722.000)..(-13.000, 5931.000, 760.000)
  f=0: ray from z=2000.0 down -> t=1240.000000 point.z=760.000000
  f=1: delta_z=260 bounds.z 982.000..1020.000 (rest 722.000..760.000) drift_lo=0 drift_hi=0
  f=1: max |normal drift| = 2.48e-16
  f=1: ray t=980.000000 (rest 1240.000000, delta 260.000000) point.z=1020.000000
  lift_offset=-500: bounds.z 222.000..260.000
  negative: NaN lift_offset refused ("lift_offset is not finite"), pose kept
runtime unbound: lifts=1 unbound=1 bound=0 dynamic=626 suppressed=34
runtime unbound tick: sync 0 -> 1, rebuilds 1 -> 1, dynamic 626 -> 626
runtime bound: lifts=1 panels=1 unbound=0 dynamic=678 suppressed=35 applied_height=0
runtime rest: static pair 52 triangles, dynamic pair 52, bit-identical=1
runtime floor at rest: z=760.000000
runtime raised: rebuilds 1 -> 2, height=65535 fraction=1
runtime raised: expected dz=260 max vertex drift=0 max normal drift=2.48e-16
runtime floor raised: z=1020.000000 (rest 760.000000, dz=260.000000)
runtime second tick: rebuilds 2 -> 2 (height unchanged)
player on rest cabin: (-128.000000, 5816.000000, 760.000001) floor=760.000000
player floor after raise: z=1020.000000 (cabin top 1020.000000)
player after raise: (-128.000000, 5816.000000, 720.693154) dz=-39.306847 (not carried; falls)
lift_collision: ok
```

Что именно закреплено:

1. **Покой.** `f = 0`: панель побитово равна статике (52/52 треугольника,
   все поля, включая `normal`, `data_normal`, `plane_offset`, `centroid`,
   `bounds`), `bounds` совпадает.
2. **Ход.** `height = 65535` → `f = 1`: `delta_z = 260`,
   `bounds.z 722..760 → 982..1020`, сдвиг `bounds.lo/hi` **точно** равен
   `rest + 260` (`drift_lo = drift_hi = 0`); x/y не меняются; максимальный
   дрейф нормалей `2.48e-16` (пересчёт нормали из сдвинутого обхода); луч
   сверху видит верхнюю грань на `z=1020` вместо `760` (`t 1240 → 980`).
3. **Шов `lift_offset`.** `update(0, -500)` → `bounds.z 222..260`, луч
   смещается на `-500`; NaN отклонён, поза и `bounds` не меняются.
4. **Рантайм без привязки.** `lifts_unbound = 1` (весь уровень), `lifts = 0`;
   `tick()` увеличивает `sync_ticks`, но не `rebuilds` и не размер dynamic
   set (626 → 626): тик не ломается.
5. **Привязка вызывающего.** `lifts = 1`, `unbound = 0`, пара `(35,1)` в
   `suppressed`, dynamic set `626 → 678`; панель при `height=0` — статика
   бит-в-бит; `height=65535` + один тик → `rebuilds + 1`, вершины сдвинуты
   ровно на `260`, нормали `≤ 2.48e-16`; второй тик при той же высоте не
   публикует ничего (`rebuilds` не меняется).
6. **Пол/игрок.** `floor_at` с фильтром сектора `35`: `760 → 1020`
   (`dz = 260`). Игрок, стоящий на кабине
   (`(-128, 5816, 760.000001)` после 30 тиков), при подъёме кабины **не
   везётся**: его `z` не меняется, а на следующем тике он падает
   (`dz = -39.306847`). Это фактическое поведение модели: contact resolve
   «кабина толкает игрока» не реализован.

---

## 5. Ограничения и следующий шаг

* **Геометрии кабины в коллизии нет.** Чтобы кабина была препятствием на
  поставочных уровнях, нужен не `.sbw`-объект, а инстанс модели
  `SOBS/<имя>.sob` в `lift_transform()`: парсер `res/sob.*` уже есть, но
  загрузка ресурсов/`SOB_TABLE` в рантайм и построение панели из `Sob` — это
  отдельный срез. Привязку к `.sbw` выдумывать нельзя (§2).
* **Поворот кабины не моделируется.** `MovingCollider` умеет перенос +
  распашное вращение вокруг одной оси; кабина задаёт три угла
  (`0x41e025`). Лифты с `basis[3..5] != 0` (например `RNDSWTCH` в `level18`)
  сознательно пропускаются (`lifts_skipped_rotating`), а не ставятся
  приблизительно.
* **Игрока кабина не везёт.** Нет object-vs-object/contact resolve
  (`docs/MOVING_COLLIDERS.md` §6); сейчас подъём кабины оставляет стоящего
  на ней игрока в воздухе. Это зафиксировано тестом.
* **`lifts_unbound` — норма для поставки**, а не ошибка: 45/45. Счётчик нужен,
  чтобы отличить «нет привязки» от «панель отказала» (`lifts_skipped_refused`).
