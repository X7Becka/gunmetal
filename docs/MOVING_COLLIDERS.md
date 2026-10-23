# Gunmetal — кинематические коллайдеры дверей и лифтов

Пункт roadmap: **M6.4.1, срез 1**. Модуль
`engine/src/world/moving_collider.{h,cpp}` + тест
`engine/tests/test_moving_collider.cpp` (CTest `moving_collider`, label `quick`).
Интеграции в `CollisionWorld`/рантайм в этом срезе **нет** — её делает основной
агент (см. §6).

---

## 0. Коротко

| что | значение |
|---|---|
| вход | панели в мировом кадре в покое (`MovingPanel::rest`) + `open - closed` |
| движение | чистый перенос: `travel * open_fraction` (или компонента оси), лифт — `+Z` на `lift_offset` |
| числа движения | `Door::open - Door::closed` (запись `+0x18` минус `+0x00`), см. §2 |
| точность | `double`, как в `render/collide.h` |
| нормали | `normal` пересчитывается из сдвинутого обхода; `data_normal` переносится (не поворачивается) и нормируется |
| инвариант | `plane_offset == dot(normal, a)`, `centroid` = среднее вершин, `bounds` = AABB вершин |
| фильтры | тот же `CollideFilter`; класс — `surface_of(data_normal)` |
| стороны | по умолчанию двусторонние, нормаль против запроса — как в `CollisionWorld` |
| запросы | без аллокаций, линейный перебор (broad-phase секторов не нужен: панель — единицы–десятки треугольников) |
| отказы | NaN/inf `open_fraction`/`lift_offset` — поза не меняется; пустой набор и вращающаяся дверь — `ready() == false` |
| интеграция | нет (ни в `CollisionWorld`, ни в `runtime/main`) |

---

## 1. Зачем

`CollisionWorld` строится один раз из `.sbw` и неизменен: треугольники двери
стоят в закрытой позе. При этом движок каждый тик переписывает трансформ
объекта створки (`0x44dfc0` → `0x434887`, docs/DOOR_FIELDS.md §1.2), и в
оригинале дверь — препятствие: `0x4392de`/`0x43abb7` зовут `0x450590`, когда
сущность упирается в створку. Пока коллизия в C++ статична, игрок проходит
сквозь закрытые двери и лифты.

Модуль решает узкую задачу: по состоянию двери дать **текущие** треугольники и
уметь с ними сталкиваться лучом/сферой, не перестраивая мир. Геометрия створки
не меняется — меняется только её положение, поэтому достаточно хранить `rest` и
переносить его.

---

## 2. Модель движения

### 2.1. Поля двери → `travel` и ось

Движение створки задают два поля записи `DOOR` (`0xB8`, docs/DOOR_FIELDS.md §1):

| поле | смещение | смысл |
|---|---|---|
| `closed` (`Door::closed`, `D1..D3`) | `+0x00` | закрытый угол, **уже сложенный** с `D7..D9` (`+0x00 += +0x30` в загрузчике) |
| `open` (`Door::open`, `D4..D6`) | `+0x18` | открытый угол, так же сложенный |
| `axis` (`A1..A3`) | `+0x48` | радиан на единицу фазы; все три нуля — чистый перенос |

`0x44dfc0` считает `center = closed + (open - closed) * phase/65535` и для
нулевых `A1..A3` пишет `origin = center - basisᵀ·size` при единичном базисе,
т.е. `origin = center`. Значит полный ход скользящей створки — ровно

```
travel = open - closed            (MovingPanel::travel, door_travel(door))
```

а поза при доле `f` — `rest + travel * f`.

Проверено на поставке (чтение, скрипт — §7): **1779/1779** объектов, на которые
ссылаются двери, хранятся в `.sbw` в закрытой позе, `object.origin ==
Door::closed` (совпадение до `1e-9`). То есть `rest` из `CollisionWorld` — это
действительно `f = 0`, и `travel` применим напрямую.

Статистика корпуса (36 уровней, `gm.scene` + `gm.sbw`):

* 1779 дверей; у **1422** все три `A` равны нулю (docs/DOOR_FIELDS.md §1.2);
* из них **1421** реально смещается (`open != closed`), у **105** смещение
  диагональное (2–3 ненулевые компоненты) — их нельзя свести к одной оси;
* 357 дверей имеют угол: 113 из них движутся (вращение), остальные стоят;
* типичный ход: `(528, 0, 0)`, `(0, 0, 176)`, `(0, 0, -92)`, `(0, 0, -9999)`
  (створка уезжает вниз), `(63, 0, 40)` (диагональ).

Поэтому у `DoorAxis` два «неосевых» значения:

* `DoorAxis::Travel` — смещать на весь вектор `travel` (диагональные и
  статичные панели); `door_axis()` возвращает его, когда ненулевых компонент
  не одна;
* `DoorAxis::Rotating` — распашная дверь; `set_panels` её **отказывается**
  принимать, потому что перенос не описывает поворот вокруг `center` (§6).

`door_axis(door)` — единственный правильный способ выбрать режим: `X`/`Y`/`Z`
только при ровно одной ненулевой компоненте `travel`, `Travel` иначе,
`Rotating` при любом ненулевом `A`.

### 2.2. Лифт

Кабина лифта в оригинале — тоже трансформ объекта: `0x44e6f0` пишет
`pos + pos_delta * height/65535` (`docs/DOOR_FIELDS.md` §3). Модель `Lift`
хранит `pos_delta[3]`, но привязки «лифт → объект `.sbw`» в C++ нет:
`Lift::sob` — индекс SOB-модели, не объект уровня. Поэтому геометрический слой
принимает **готовый** `lift_offset` (мировые единицы по `+Z`) и не угадывает
кабину:

```cpp
collider.update(0.0, lift_offset);   // панель = треугольники кабины
```

`lift_offset` знаковый: кабина едет и вниз (`Lift::direction` = `-1`, поле
`pos_delta` — `i32`). В 45 поставочных `ADD_LIFT` `pos_delta = 0`, но
отрицательный перенос уже есть в данных дверей (`level0` сектор 34 объект 10:
`travel.z = -192`; `dm1`: `-9999`), а геометрический слой не должен запрещать
направление, которое модель выражает. Тест проверяет `lift_offset = -37/-60`.

---

## 3. API

```cpp
#include "world/moving_collider.h"

enum class DoorAxis : std::uint8_t { X, Y, Z, Travel, Rotating };

const char* door_axis_name(DoorAxis);
Vec3d       door_axis_unit(DoorAxis);   // 0 для Travel/Rotating
Vec3d       door_travel(const Door&);   // open - closed
DoorAxis    door_axis(const Door&);     // учитывает и угловые операнды

struct MovingPanel {
    std::vector<Triangle> rest;  // мировые треугольники в покое (f = 0)
    Vec3d travel{};              // open - closed; 0 для чистой платформы
};

class MovingCollider {
public:
    void set_panels(std::vector<MovingPanel> panels, DoorAxis axis);
    void update(double open_fraction, double lift_offset = 0.0);

    bool ready() const;                 // есть панели и хотя бы один треугольник
    bool last_update_ok() const;        // последний set_panels/update принят
    const std::string& error() const;

    DoorAxis axis() const;
    std::size_t panel_count() const;
    const std::vector<Triangle>& triangles() const;   // текущая поза
    std::size_t triangle_count() const;
    std::size_t panel_of(std::uint32_t triangle_index) const;
    const Aabb& bounds() const;
    double open_fraction() const;
    double lift_offset() const;

    std::optional<RayHit> raycast(const Vec3d& origin, const Vec3d& direction,
                                  double max_t = kCollideInfinity,
                                  const CollideFilter& filter = {}) const;
    std::optional<SphereHit> sphere_cast(const Vec3d& origin, const Vec3d& direction,
                                         double radius, double max_t = kCollideInfinity,
                                         const CollideFilter& filter = {}) const;
};
```

Как собрать панель (делает вызывающий; в этом срезе — тест, дальше — рантайм):

```cpp
MovingPanel panel;
panel.travel = gm::door_travel(door);
for (const gm::Triangle& t : world.triangles())
    if (t.sector == door.sector && t.object == door.object)
        panel.rest.push_back(t);
collider.set_panels({panel}, gm::door_axis(door));
collider.update(door.motion_fraction());   // phase/65535, как 0x44dfc0
```

`rest` берётся из уже собранного `CollisionWorld` — это те же треугольники
(тот же fan, обход, `data_normal`, `sector/object/texture`), ничего не
пересчитывается заново.

---

## 4. Семантика

### 4.1. Поза

`update(f, lift)` для каждой панели:

```
slide = (axis == Travel) ? travel * f : axis_unit * dot(travel, axis_unit) * f
delta = slide + (0, 0, lift)
triangle' = triangle + delta
```

* `f` зажимается в `[0, 1]` — как фаза движка (`0x44f335..0x44f349`); `f = 2`
  даёт позу `f = 1`;
* `lift` не зажимается и может быть отрицательным;
* каждый треугольник переносится независимо, порядок панелей/треугольников
  сохраняется, `panel_of(i)` даёт индекс панели для `RayHit::triangle_index`.

### 4.2. Нормали и производные поля

Перенос не меняет ориентацию, поэтому:

* `normal` **пересчитывается** из сдвинутых вершин (`cross(b-a, c-a)`,
  нормировка); вырожденный rest-треугольник сохраняет свою нормаль;
* `data_normal` переносится без поворота и нормируется (для unit-вектора это
  меняет только последний бит); нулевой остаётся нулевым — `surface_normal()`
  тогда берёт обход, как и в `CollisionWorld`;
* `plane_offset = dot(normal, a)` — пересчитывается, а не сдвигается;
* `centroid = (a+b+c)/3`, `bounds` — AABB трёх вершин;
* `sector/object/texture/material/lightmap/source_poly` копируются из `rest`.

Тест проверяет это на каждом треугольнике после сдвига: единичность нормалей,
`plane_offset == dot(normal, a)` (точное равенство), `centroid`/`bounds` от
вершин, `triangle' == rest + delta`, `plane_offset' = plane_offset +
dot(normal_rest, delta)`.

### 4.3. Запросы и фильтры

`raycast`/`sphere_cast` повторяют `CollisionWorld`:

* те же проверки входа: не-finite origin/direction, нулевое направление,
  `max_t <= 0`, `radius < 0`/NaN;
* `t >= kCollideRayEps`, ближайшее попадание, при точном равенстве побеждает
  первый треугольник;
* `CollideFilter::allows` — класс по `surface_of(data_normal)` (`floors`,
  `ceilings`, `walls`), плюс `sectors` по полю `Triangle::sector`;
* `backfaces = true` по умолчанию: двусторонне, сторона задаётся
  `surface_normal()`, а не обходом; при `backfaces = false` отбрасывается
  авторская задняя сторона;
* `RayHit::normal`/`SphereHit::normal` всегда против запроса/выталкивает сферу;
  `RayHit::front_face` — про авторскую сторону, как в `CollisionWorld`;
* `triangle` указывает внутрь `triangles()`, `triangle_index` — индекс в нём.

Сферный sweep — порт `sphere_vs_triangle` из `render/collide.cpp` (грань, три
ребра, три вершины; начальное перекрытие — `t = 0` с выталкивающей нормалью).
Оригинальная функция file-local, поэтому код продублирован; тест §5.4 сверяет
оба sweep'а на одних и тех же треугольниках, чтобы копии не разъехались.

### 4.4. Отказы

| вход | поведение |
|---|---|
| пустой набор панелей | `ready() == false`, `error() == "no panels"`, запросы → `nullopt` |
| панели без треугольников | `ready() == false`, `error()` |
| NaN/inf в `travel`/вершинах `rest` | `set_panels` отказывает целиком, `ready() == false` |
| NaN/inf `open_fraction`/`lift_offset` | `update` отказывает: поза и `triangles()` **не меняются**, `last_update_ok() == false`, `error()` называет аргумент |
| `DoorAxis::Rotating` | `ready() == false`, `error()` объясняет, что поворот не моделируется |
| `f` вне `[0, 1]` | зажимается (движковая семантика фазы) |

Отказ `update` не портит последнюю хорошую позу: кадр с NaN не «телепортирует»
дверь и не оставляет её без коллизии.

---

## 5. Числа теста (`gm_moving_collider_tests`)

### 5.1. `level0`, дверь сектор 4 объект 1 (скользящая, +Z)

| величина | значение |
|---|---|
| `closed` / `open` | `(664, 928, -4)` / `(664, 928, 172)` |
| `travel` | `(0, 0, 176)`, `door_axis == Z` |
| треугольников / классы | 12: floors 2, walls 8, ceilings 2 |
| rest bounds | `(660, 856, -144)..(668, 1000, 136)` |
| луч `+X` из `(624, 928, -4)` (центр − 40) | закрыто: `t = 36`, точка `(660, 928, -4)`, нормаль `(-1, 0, 0)`, класс `wall` |
| сфера `r = 8`, тот же луч | закрыто: `t = 28`, центр `(652, 928, -4)`, нормаль `(-1, 0, 0)` |
| `f = 0.5` | попадание `t = 36`, `bounds.lo.z = -56` (створка ещё перекрывает линию) |
| `f = 1` | `bounds.z` сдвинут на `+176` (`32..312`), луч и сфера — **промах** |
| `f = 2` | зажато до 1, промах |
| `lift_offset = -37` | `bounds.z = -181..99`, сдвиг `centroid` на `-37` |
| NaN `f` / inf `lift` | отказ, поза прежняя (`bounds.lo.z` не изменился) |
| пустой набор | `ready = false`, запросы — промах, `error = "no panels"` |
| вращающаяся (сектор 48 объект 6) | `set_panels` отказ, `ready = false` |
| диагональная (сектор 23 объект 1) | `travel = (63, 0, 40)`, `door_axis == Travel`, 6 треугольников, `bounds` сдвинут на `(63, 0, 40)` |

### 5.2. `dm1`, дверь сектор 17 объект 37 (скользящая, +Y)

| величина | значение |
|---|---|
| `travel` | `(0, 130, 0)`, `door_axis == Y`, 8 треугольников |
| rest bounds | `(8002, 1648, 132)..(8007, 1787, 225)` |
| луч `+X` из `(7964.5, 1717.5, 178.5)` | закрыто: `t = 37.5`, точка `x = 8002`, нормаль `(-1, 0, 0)` |
| сфера `r = 5` | закрыто: `t = 32.5` |
| `f = 1` | сдвиг по `Y` на `130`, луч и сфера — промах |

### 5.3. Синтетика: платформа и фильтры

Пол `z = 0` (класс floor, `sector 7`) + стена `x = 10` (класс wall,
`sector 8`), `travel = 0`:

* `+Z`-лифт: `update(0, +120)` → пол на `z = 120`, луч сверху даёт
  `t = 10`, `point.z = 120`; `bounds.z` сдвинут на `+120`;
* `update(0, -60)` → `bounds.z = -60` (отрицательный сдвиг принят);
* `floors` пропускает пол и не пропускает стену, `walls` — наоборот;
  `sectors = {7}`/`{8}` выбирают по `Triangle::sector`;
* NaN в rest-вершине → `set_panels` отказ;
* NaN-радиус, нулевое направление, `max_t = 0`, `radius = -1` → `nullopt`.

### 5.4. Паритет с `CollisionWorld`

Один объект `level0` (сектор 4 объект 1) собран в отдельный `SbwLevel`
(тот же 96-байтный header и геометрия), `CollisionWorld::build` и
`MovingCollider` с теми же треугольниками при `f = 0`:

```
rays    128 случаев / 43 попадания, max |dt| = 0, max |dn| = 0
spheres 128 случаев / 63 попадания, max |dt| = 0, max |dn| = 0
```

Попадание/промах, `t`, точка, нормаль и `front_face` совпадают бит в бит;
радиусы сфер 2/7/20. Это же — проверка, что дублированный sweep не разошёлся
с `render/collide.cpp`.

---

## 6. Ограничения (что осталось)

1. **Нет интеграции.** `CollisionWorld`, `runtime.*`, `main.cpp`, `doors.*` не
   тронуты. Рантайм должен: собрать панели из статического мира, исключить эти
   треугольники из статической коллизии (или проверять движущиеся первыми),
   звать `update(door.motion_fraction())` каждый тик после `WorldState::tick`
   и `lift_offset` из `lift_transform()` для кабины.
2. **Нет вращения распашных дверей.** 357 дверей имеют угол; `door_axis`
   возвращает `Rotating`, `set_panels` отказывает. Нужен поворот вокруг
   `center` с базисом `0x434887` (данные уже есть в `door_transform_at`).
3. **Нет лифт → объект.** `Lift::sob` — индекс SOB-модели; привязку кабины к
   объекту `.sbw` ещё нужно доказать/сделать в рантайме.
4. **Нет столкновения объект-объект.** Модуль отвечает только на луч/сферу;
   дверь против игрока — это `sphere_cast`/капсула со стороны контроллера.
5. **Нет выталкивания/разрешения проникновения.** `sphere_cast` возвращает
   нормаль и `t`, но не двигает тело и не разрешает начальное перекрытие
   (кроме `t = 0` + нормаль).
6. **Нет broad-phase.** Линейный перебор всех треугольников коллайдера.
   Держите один коллайдер на дверь (или несколько ближних), а не один на
   уровень: 1779 дверей × ~10 треугольников — это ~20k тестов на запрос.
7. **Нет `.sbw`-записи.** Модуль ничего не пишет и не меняет `CollisionWorld`.

---

## 7. Доказательства и воспроизведение

* Поля `closed/open` и формула `0x44dfc0` — `docs/DOOR_FIELDS.md` §1.2
  (адреса `0x44dfc0`, `0x434887`, `0x44f2e0`).
* `object.origin == Door::closed` на всём корпусе — проверка чтением
  (`gm.sbt` + `gm.scene` + `gm.sbw`):

  ```sh
  python3 - <<'EOF'
  import glob, os, gm.sbt as sbt, gm.scene as scene, gm.sbw as sbw
  ok = bad = 0
  for p in sorted(glob.glob("/mnt/d/games/Gunmetal 3dfx/*.sbt")):
      sc = scene.extract(sbt.parse_file(p).records_of(11)[0].payload)
      lvl = sbw.parse_file(os.path.splitext(p)[0] + ".sbw")
      for d in sc.doors:
          org = lvl.sectors[d.sector].objects[d.object].points[0]
          ok += all(abs(org[k] - d.values[k]) < 1e-9 for k in range(3))
          bad += not all(abs(org[k] - d.values[k]) < 1e-9 for k in range(3))
  print(ok, "/", ok + bad, "bad", bad)
  EOF
  # 1779 / 1779 bad 0
  ```

* Числа `travel`/статистика — тот же проход по `gm.scene` (105 диагональных из
  1421 движущихся скользящих, 113 движущихся вращающихся).
* Тест: `ctest --test-dir build/sandbox -R moving_collider --output-on-failure`;
  полный прогон — `ctest --test-dir build/sandbox -j 4` и
  `python3 regress.py --mirror --json`.

---

## 8. Доверие

**Доказано кодом/данными:** формула переноса (`0x44dfc0` при единичном
базисе), `travel = open - closed`, закрытая поза объекта в `.sbw`
(1779/1779), классы поверхностей и нормали — те же функции, что у
`CollisionWorld` (`surface_of`, `surface_normal`), паритет sweep'ов на реальной
панели (0 расхождений).

**Гипотеза/не проверено:** что рантайм оригинала обновляет именно эти
треугольники для коллизии игрока (в оригинале препятствие обрабатывается через
`0x450590`/`0x4392de`, а не через перестроение `CollisionWorld`); момент
вызова `update` относительно `WorldState::tick`; привязка лифта к объекту.
