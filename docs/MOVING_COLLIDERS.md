# Gunmetal — кинематические коллайдеры дверей и лифтов

Пункт roadmap: **M6.4.1, срезы 1–3**. Модуль
`engine/src/world/moving_collider.{h,cpp}` + тесты
`engine/tests/test_moving_collider.cpp` (CTest `moving_collider`, label `quick`)
и `engine/tests/test_swing_door.cpp` (CTest `swing_door`, label `quick`).
Интеграция в рантайм — `docs/DOOR_COLLISION.md`.

---

## 0. Коротко

| что | значение |
|---|---|
| вход | панели в мировом кадре в покое (`MovingPanel::rest`) + `open - closed`; для распашной — `DoorSwing` (`pivot`, `axis`, `max_angle`) |
| движение | скользящая: перенос `travel * open_fraction` (или компонента оси), лифт — `+Z` на `lift_offset` |
| вращение | распашная: все вершины и обе нормали поворачиваются на `fraction * max_angle` вокруг `pivot`, сам `pivot` едет на `travel * fraction` |
| числа движения | `Door::open - Door::closed` (запись `+0x18` минус `+0x00`), угол — `A_i` (`+0x48`), см. §2 |
| точность | `double`, как в `render/collide.h`; при `f = 0` поза побитово равна `rest` |
| нормали | скользящая: `normal` пересчитывается из сдвинутого обхода, `data_normal` переносится; распашная: обе поворачиваются тем же `R(angle)` (обход не пересчитывается) и нормируются |
| инвариант | `plane_offset == dot(normal, a)`, `centroid` = среднее вершин, `bounds` = AABB вершин |
| фильтры | тот же `CollideFilter`; класс — `surface_of(data_normal)` |
| стороны | по умолчанию двусторонние, нормаль против запроса — как в `CollisionWorld` |
| запросы | без аллокаций, линейный перебор (broad-phase секторов не нужен: панель — единицы–десятки треугольников) |
| отказы | NaN/inf `open_fraction`/`lift_offset` — поза не меняется; пустой набор, нулевой угол, не-единичная ось, не-finite `pivot` — `ready() == false` |
| интеграция | `GameRuntime::build_door_colliders`/`sync_door_colliders` → `CollisionWorld::set_dynamic` (§6 `docs/DOOR_COLLISION.md`) |

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
* `DoorAxis::Rotating` — распашная дверь: панель поворачивается вокруг
  `pivot` (§2.1a). `door_axis()` возвращает его, когда любой из `A1..A3`
  не нулевой (357 дверей поставки, 113 из них движутся).

`door_axis(door)` — единственный правильный способ выбрать режим: `X`/`Y`/`Z`
только при ровно одной ненулевой компоненте `travel`, `Travel` иначе,
`Rotating` при любом ненулевом `A`.

### 2.1a. Распашная дверь (`DoorAxis::Rotating`)

`0x44dfc0` выбирает ось по приоритету **z > x > y** и берёт единичный вектор
`-x`, `+y` или `-z`; угол на фазе `p` равен `(A_i / 65535) * p` (в записи
`+0x48` лежит уже `A_i / 65535`, а фаза идёт `0..0xffff`). `0x434887` строит
базис как `R(-angle)` и пишет `origin = center - basis^T * size`. Если
подставить позу покоя объекта (`origin = D1..D3`, базис единичный),
получается

```
world(f) = center(f) + R(angle(f)) * (rest - closed)
center(f) = closed + (open - closed) * f
angle(f)  = A_i * 65535 * f          (то есть A_i радиан при f = 1)
closed    = D1..D3 + D7..D9          (загрузчик складывает размер, `0x44e368`)
```

`door_swing(door)` отдаёт `pivot = Door::closed`, единичную ось и
`max_angle = A_i * 65535`; `MovingCollider` поворачивает каждую вершину
формулой Родрига и переносит `pivot` на `travel * f`. При `f = 0` это
тождественное преобразование, поэтому поза копируется из `rest` без
арифметики — бит в бит.

Проверено на реальных дверях (`swing_door`, §5.5): `level0` `sec=48 obj=6`
(ось `-x`, 120°), `dm1` `sec=17 obj=55` (ось `-z`, −90°); вершины при `f = 1`
совпадают с `door_transform_at(door, 0xffff)`, применённым к позе покоя, до
`3e-14`.

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

struct DoorSwing {
    Vec3d pivot{};        // Door::closed (+0x00, уже сложенный с size)
    Vec3d axis{};         // -x / +y / -z (0x44dfc0)
    double max_angle = 0; // A_i радиан при f = 1
};

DoorSwing door_swing(const Door&);   // Rotating -> ось/угол, иначе max_angle = 0

class MovingCollider {
public:
    void set_panels(std::vector<MovingPanel> panels, DoorAxis axis,
                    const DoorSwing& swing = {});
    void update(double open_fraction, double lift_offset = 0.0);

    bool ready() const;                 // есть панели и хотя бы один треугольник
    bool last_update_ok() const;        // последний set_panels/update принят
    const std::string& error() const;

    DoorAxis axis() const;
    const DoorSwing& swing() const;
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

Как собрать панель (делает вызывающий; в рантайме —
`GameRuntime::build_door_panels`):

```cpp
MovingPanel panel;
panel.travel = gm::door_travel(door);
for (const gm::Triangle& t : world.triangles())
    if (t.sector == door.sector && t.object == door.object)
        panel.rest.push_back(t);
collider.set_panels({panel}, gm::door_axis(door), gm::door_swing(door));
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
скользящая:
  slide = (axis == Travel) ? travel * f : axis_unit * dot(travel, axis_unit) * f
  delta = slide + (0, 0, lift)
  triangle' = triangle + delta

распашная (axis == Rotating):
  center_delta = travel * f
  angle = max_angle * f
  vertex' = pivot + center_delta + R(angle) * (vertex - pivot) + (0, 0, lift)
```

* `R(angle)` — формула Родрига вокруг `DoorSwing::axis` (единичный вектор,
  проверяется в `set_panels`); это `basis^T` из `0x434887`;
* `f` зажимается в `[0, 1]` — как фаза движка (`0x44f335..0x44f349`); `f = 2`
  даёт позу `f = 1`;
* `lift` не зажимается и может быть отрицательным;
* при `f = 0` и `lift = 0` распашная панель копируется из `rest` без
  арифметики (тождественный трансформ движка), поэтому закрытая поза
  побитово равна статической геометрии;
* каждый треугольник переносится/поворачивается независимо, порядок
  панелей/треугольников сохраняется, `panel_of(i)` даёт индекс панели для
  `RayHit::triangle_index`.

### 4.2. Нормали и производные поля

* скользящая: `normal` **пересчитывается** из сдвинутых вершин
  (`cross(b-a, c-a)`, нормировка); вырожденный rest-треугольник сохраняет
  свою нормаль; `data_normal` переносится без поворота и нормируется;
* распашная: `normal` и `data_normal` поворачиваются тем же `R(angle)` и
  нормируются. Обход **не** пересчитывается: происхождение нормали
  сохраняется (геометрическая остаётся геометрической, авторская —
  авторской). Нулевая `data_normal` остаётся нулевой — `surface_normal()`
  тогда берёт обход, как и в `CollisionWorld`;
* `plane_offset = dot(normal, a)` — пересчитывается, а не сдвигается;
* `centroid = (a+b+c)/3`, `bounds` — AABB трёх вершин;
* `sector/object/texture/material/lightmap/source_poly` копируются из `rest`.

Тест проверяет это на каждом треугольнике после сдвига: единичность нормалей,
`plane_offset == dot(normal, a)` (точное равенство), `centroid`/`bounds` от
вершин, `triangle' == rest + delta`, `plane_offset' = plane_offset +
dot(normal_rest, delta)`. Для распашной (`swing_door`) — единичность обеих
нормалей, `plane_offset == dot(normal, a)`, сохранение длин рёбер, поворот
вершин на половину угла и сохранение расстояния плоскости до `pivot`
(плоскости, проходившие через `pivot`, проходят через него и в полуоткрытом
положении).

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
| `Rotating` с `max_angle == 0` | `set_panels` отказывает: «swing angle is zero» |
| `Rotating` с не-единичной/не-finite осью, не-finite `pivot` | `set_panels` отказывает |
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
| вращающаяся (сектор 48 объект 6) | `door_swing`: ось `-x`, 120°; при `f = 0` поза = `rest` бит в бит, при `f = 1` вершины сдвинуты (макс. 127.585), нулевой угол — отказ |
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

### 5.5. Распашные двери (`gm_swing_door_tests`, CTest `swing_door`)

Две реальные двери в изолированном мире из одного объекта (все треугольники
створки — динамический набор с подавлением статики).

| величина | `level0` `sec=48 obj=6` | `dm1` `sec=17 obj=55` |
|---|---|---|
| `A1..A3` | `(2.094395102, 0, 0)` -> ось `-x` | `(0, 0, -1.570796327)` -> ось `-z` |
| угол при `f = 1` | `+2.094395102` рад (120°) | `-1.570796327` рад (−90°) |
| `pivot = Door::closed` | `(-400, -295, -7)` | `(7034, 240, 182)` |
| `travel` | `(0, 0, 0)` (чистый поворот) | `(0, 0, 0)` |
| треугольников / bounds | 24, `(-432,-344,-16)..(-368,-286,48)` | 10, `(7034,240,146)..(7042,414,218)` |
| закрыто, `f = 0` | 24 треугольника = `rest` побитово, `bounds` равны | 10 треугольников = `rest` побитово, `bounds` равны |
| луч в закрытую | `t = 17.2`, `index 4` = статика бит в бит | `t = 22`, `index 8` = статика бит в бит |
| сфера `r = 5` | `t = 12.2` | `t = 17` |
| `f = 1`: луч/сфера | промах (закрытая поза попадала) | промах |
| `f = 1`: `|vertex − door_transform_at(0xffff)|` | `2.85e-14` | `0` |
| `f = 1`: макс. сдвиг вершины | `127.585` | `246.333` |
| `f = 0.5`: `max |dL|` на 3 рёбрах | `1.4e-14` | `8.5e-14` |
| `f = 0.5`: дрейф расстояния плоскость↔`pivot` | `2.1e-14` | `1.1e-13` |
| `f = 0.5`: `|angle − half|` | `1.8e-15` рад | `5.1e-15` рад |
| плоскости через `pivot` | 0 (панель из двух клиньев) | 4 из 10 (грани `y = 240`, `x = 7034`), `max |offset| = 9.6e-14` |
| игрок в закрытую | `x: −500 -> −435.000001`, `wall_ticks = 150` | `x: 6960 -> 7030.999999`, `wall_ticks = 96` |
| игрок в открытую | `x -> −181.25`, `wall_ticks = 0` | `x -> 7193.75`, `wall_ticks = 0` |
| рантайм `level0` | `doors=25 panels=25 rotating=1`, dynamic `446`, suppressed `25` | — |
| рантайм `dm1` | — | `doors=61 panels=61 rotating=23`, dynamic `1038`, suppressed `61` |

Рантайм обоих уровней: у распашной пары статика подавлена, `rest`-поза
динамики бит в бит равна статике, после `phase = 0xffff` и одного тика
`rebuilds 1 -> 2`, поза пары изменилась. Негативы: нулевой угол, не-единичная
ось, NaN `pivot` и NaN `fraction` отвергнуты, поза/счётчики не порваны.

---

## 6. Ограничения (что осталось)

1. **Нет интеграции в `CollisionWorld` из самого модуля.** Модуль остаётся
   чистым: рантайм собирает панели и публикует их через `set_dynamic`
   (`docs/DOOR_COLLISION.md`). `main.cpp` и CLI динамику пока не используют.
2. **Нет лифт → объект.** `Lift::sob` — индекс SOB-модели; привязку кабины к
   объекту `.sbw` ещё нужно доказать/сделать в рантайме.
3. **Нет столкновения объект-объект.** Модуль отвечает только на луч/сферу;
   дверь против игрока — это `sphere_cast`/капсула со стороны контроллера.
4. **Нет выталкивания/разрешения проникновения.** `sphere_cast` возвращает
   нормаль и `t`, но не двигает тело и не разрешает начальное перекрытие
   (кроме `t = 0` + нормаль).
5. **Нет broad-phase.** Линейный перебор всех треугольников коллайдера.
   Держите один коллайдер на дверь (или несколько ближних), а не один на
   уровень: 1779 дверей × ~10 треугольников — это ~20k тестов на запрос.
6. **Нет `.sbw`-записи.** Модуль ничего не пишет и не меняет `CollisionWorld`.
7. **Нет непрерывной коллизии между тиками.** Поза берётся на тике: при
   быстрой створке игрок, шагнувший до публикации новой позы, отстаёт на один
   тик (docs/DOOR_COLLISION.md §6.7).

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
* Тест: `ctest --test-dir build/sandbox -R moving_collider --output-on-failure`
  и `ctest --test-dir build/sandbox -R swing_door --output-on-failure`;
  полный прогон — `ctest --test-dir build/sandbox -j 4` и
  `python3 regress.py --mirror --json`.
* Распашная геометрия — `docs/DOOR_FIELDS.md` §1.2 (порядок осей `z > x > y`,
  единичные `-x/+y/-z`, `angle = A_i * phase`, `origin = center - basis^T *
  size`); сверка `f = 1` с `door_transform_at(door, 0xffff)` — `swing_door`,
  числа в §5.5.

---

## 8. Доверие

**Доказано кодом/данными:** формула переноса (`0x44dfc0` при единичном
базисе), `travel = open - closed`, закрытая поза объекта в `.sbw`
(1779/1779), классы поверхностей и нормали — те же функции, что у
`CollisionWorld` (`surface_of`, `surface_normal`), паритет sweep'ов на реальной
панели (0 расхождений). Поворот: формула `center + R(angle)(p - closed)`
выведена из `0x44dfc0` + `0x434887` и совпала с `door_transform_at(0xffff)` на
двух реальных дверях (`level0` 120°/-x, `dm1` −90°/−z) до `3e-14`; длины рёбер
и расстояния плоскостей до `pivot` сохраняются (`swing_door`).

**Гипотеза/не проверено:** что рантайм оригинала обновляет именно эти
треугольники для коллизии игрока (в оригинале препятствие обрабатывается через
`0x450590`/`0x4392de`, а не через перестроение `CollisionWorld`); момент
вызова `update` относительно `WorldState::tick`; привязка лифта к объекту.
