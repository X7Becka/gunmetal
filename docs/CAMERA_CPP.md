# `camera/camera.h` — камера первого/третьего лица

Что здесь: `engine/src/camera/camera.{h,cpp}` — детерминированная камера
поверх `PlayerState` из `player/controller.h`. Математика без SDL, без
глобального состояния, только `double`. Тесты: `engine/tests/test_camera.cpp`,
CTest `camera_rig` (метка `quick`, ~0.01 с). Формулы и адреса — из
`docs/CAMERA.md` (базис `0x41e025`, точка глаза, клампы, проекция) и
`docs/SHAKE.md` (view pitch, треугольная тряска, отдача); Python-эталон —
`gm/scene.py:first_person`, `tools/re/shake.py`.

Статус: первое лицо, углы, клампы, тряска, отдача и фокусные масштабы
повторяют доказанные формулы; третье лицо, сглаживание и near plane —
явные гипотезы (§5, §7). `regress.py` не менялся и проходит.

---

## 0. Коротко

| что | значение | статус |
|---|---|---|
| мир | X вправо, Y вперёд, Z вверх | доказано (`docs/CAMERA.md` §2) |
| базис | `Rz(-yaw)·Rx(-pitch)·Ry(roll)`, столбцы `right, forward, up` | доказано (§3) |
| `yaw = 0` / `+90°` | взгляд в `+Y` / `+X` | доказано |
| `pitch > 0` | взгляд **вниз** | доказано |
| eye | `(pos.x, pos.y, pos.z + player.eye_height)` | доказано (§5) |
| высота глаза | `player[0x2F50]`: 10/11/12/14/16 по шасси | доказано |
| тангаж тела | ±30° (`π/6`) | доказано |
| крен тела | ±20° | доказано |
| view pitch | `player[0x2E60]`, ±90° | доказано |
| тряска | 3 канала, треугольник `4·period`, пик `amplitude` | доказано |
| отдача | горизонтальный импульс скорости, не угол | доказано |
| фокус | `fx = zoom`, `fy = zoom·4H/(3W)` | доказано |
| третье лицо | дистанция 60, высота 0, сфера 1 | гипотеза |
| сглаживание | экспонента с `smooth_time`, по умолчанию выкл. | гипотеза |
| near/far | 0.9 / 1e5 | гипотеза |

Однострочно (крен 0, без тряски):

```
forward = ( sin(yaw)·cos(pitch),  cos(yaw)·cos(pitch), -sin(pitch) )
right   = ( cos(yaw),            -sin(yaw),             0            )
up      = ( sin(yaw)·sin(pitch),  cos(yaw)·sin(pitch),  cos(pitch)  )
eye     = ( pos.x, pos.y, pos.z + eye_height )
target  = eye + forward · focus_distance            # 100, как gm/scene
```

---

## 1. Формулы

### 1.1. Базис

`camera_basis(roll, pitch, yaw)` — развёрнутые формулы `0x41e025` из
`docs/CAMERA.md` §3 (`A = roll`, `B = pitch`, `C = yaw`):

```
right   = ( cC·cA − sC·sB·sA,  −sC·cA − cC·sB·sA,  −cB·sA )
forward = ( sC·cB,               cC·cB,              −sB    )
up      = ( cC·sA + sC·sB·cA,  −sC·sA + cC·sB·cA,   cB·cA )
```

Порядок операций фиксирован; `det = +1`, оси ортонормальны (проверяется на
200 наборах углов, допуск `1e-12`). `roll = 0` даёт упрощённые формулы §0.

### 1.1a. Кадр координат

Камера работает в **мировом** кадре движка (том же, что `render/collide`).
Заголовок объекта `.sbw` — 12 `f64` (`origin[3] + basis[3][3]`, базис
единичный; `docs/WORLD_FIELDS.md` §3): загрузчик уровня применяет
`world = origin + basis · local`, поэтому `PlayerState::pos`, спавны `.sbt` и
`CollisionWorld` — это один и тот же мировой кадр, и никакого масштаба к
eye/target применять нельзя. Проверка — `level0` Golden `eye.z = 58`: спавн
`(24, −208, 48)` уже мировой, пол в комнате лежит на `z = 16`, а не на
`48.546875` (это было артефактом старого `f32`-кадра; см. `docs/COLLIDE.md`
§1 и `docs/WORLD_FIELDS.md` §3.1).

### 1.2. Углы кадра

```
pitch = body_pitch + view_pitch + shake_pitch      # player[0x18] + [0x2E60] + shake
roll  = body_roll  + shake_roll                    # player[0x20] + shake
yaw   = player.yaw + yaw_offset + shake_yaw        # player[0x28] + look + shake
```

`player.yaw` приходит из контроллера, `yaw_offset` накапливает дельты
`LookInput::yaw` (движок: `player[0x28] += pending`, `0x4439c8`). Клампы:

| угол | предел | источник |
|---|---|---|
| `body_pitch` | ±π/6 | `[0x81be18]`, `0x444af8`…`0x444b48` |
| `body_roll` | ±20° | `[0x81be20]`, `0x444aaa`…`0x444af6` |
| `view_pitch` | ±π/2 | `player[0x2E68/0x2E70]`, `0x443a72`…`0x443ac2` |
| `yaw` | нет клампа | лимит скорости `[0x2D98]` в билде мёртв |

Порядок тика: yaw (pending → применение) → view pitch (импульс, иначе
затухание, lock, кламп) → крен → eye/target → уменьшение shake-таймеров.
Последнее совпадает с `tools/re/shake.py --trace`: значение канала берётся
**до** вычитания `dt`, поэтому первый кадр с `duration = 4·period` даёт
`−amplitude`, а не 0.

### 1.3. Точка глаза и view

```
player_eye = pos + (0, 0, eye_height)
eye        = player_eye                         # первое лицо
target     = eye + forward · focus_distance
```

`view()` — матрица «мир → камера» (x вправо, y вверх, z вперёд), как
`gm.render.Camera.view`:

```
[ right.x  right.y  right.z  −dot(right, eye)   ]
[ up.x     up.y     up.z     −dot(up, eye)      ]
[ forward.x forward.y forward.z −dot(forward, eye) ]
[ 0        0        0        1                  ]
```

Базис `(right, up, forward)` левосторонний (`det = −1`), потому что z —
глубина, а не «назад», как в OpenGL. Это та же ориентация, что в
`gm/render.py`; `camera_basis` (right, forward, up) — правосторонний,
`det = +1`.

---

## 2. API

```cpp
#include "camera/camera.h"

gm::CameraRig rig;                       // первое лицо, дефолты
gm::LookInput look;                     // дельты за кадр, радианы
look.yaw = 0.01;
look.pitch = 0.005;                     // > 0 — вниз
look.roll = 0.0;

gm::CameraUpdate r = rig.update(player, look, 1.0 / 91.0);
if (!r.ok) { /* r.error, состояние не изменилось */ }

gm::Vec3d eye = rig.eye();
gm::Vec3d target = rig.target();
gm::Mat4 view = rig.view();
gm::Mat4 proj = rig.projection();       // config: fov/aspect/near/far
gm::Mat4 engine = rig.engine_projection(320, 200, 1.0, 0.9, 1e5);
```

| тип | поля / методы |
|---|---|
| `LookInput` | `yaw`, `pitch`, `roll` — дельты |
| `CameraConfig` | лимиты, `focus_distance`, `smooth_time`, `third_distance/height/radius`, `fov`, `aspect`, `near_plane`, `far_plane`, `max_dt`, `yaw_rate_max`, дефолты тряски |
| `CameraState` | `mode`, `eye`, `target`, `right`, `forward`, `up`, `player_eye`, `yaw/pitch/roll`, `aim_yaw`, `view_pitch`, `body_pitch/roll`, `shake_*`, `eye_height`, `distance`, `occluded`, `fov/aspect/near/far` |
| `CameraUpdate` | `ok`, `error` |
| `CameraRig` | `update`, `view`, `projection`, `engine_projection`, `eye/target/up/forward/right`, `state`, `set_mode`, `set_third_person`, `set_collision_world`, `set_body_angles`, `add_shake`, `add_look_impulse`, `recoil_velocity`, `set_view_pitch_decay`, `set_pitch_locked`, `clear_shake`, `snap_smoothing`, `reset` |
| свободные | `camera_basis`, `look_at`, `decay_multiplier`, `recoil_velocity`, `engine_focal`, `focal_to_fov`, `fov_to_focal` |

Ошибки не бросают исключений: `update()` проверяет конфиг, `player.pos`,
`player.yaw`, `player.eye_height`, все поля `LookInput` и `dt` (конечность,
`0 <= dt <= max_dt`) и возвращает `ok = false` с текстом, **не меняя**
состояние. `add_shake`/`add_look_impulse`/`set_body_angles` возвращают
`false` на NaN/Inf (и на `period < 1` / `duration < 0` для тряски).

Ограничение огромного, но конечного pitch — не ошибка: `view_pitch`
зажимается в ±π/2, как в движке. `look_at` при вырожденном `up` не падает:
берёт `+Y`, затем `+X` (как `gm.render.Camera`), а на нулевом направлении
или NaN возвращает `false` с ошибкой.

`state()` — снимок последнего `update`; до первого вызова это состояние
покоя (eye = 0, target = `(0, focus_distance, 0)`).

---

## 3. Тряска, view pitch, отдача

### 3.1. Треугольная волна

`ShakeChannel` повторяет `SET_CAMERA_{PITCH,ROLL,YAW}_SHAKE`
(`docs/SHAKE.md` §5):

```
set(amplitude, period, duration):  p = max(1, period); duration = max(0, duration)
                                   amp_scaled = amplitude / p
value():  если duration <= 0: 0
          r = duration mod 4p
          s = (r < 2p) ? r − p : 3p − r
          return amp_scaled · s
tick:     duration = max(0, duration − dt_ms)
```

`amplitude` — пик в радианах, `period` — **четверть** периода, всё в мс.
Таблица из `docs/SHAKE.md` §5.2 (`A = 0.4`, `P = 100`) воспроизводится
точно:

| duration | r | s | добавка |
|---:|---:|---:|---:|
| 400 | 0 | −100 | **−0.400** |
| 300 | 300 | 0 | 0 |
| 200 | 200 | +100 | **+0.400** |
| 100 | 100 | 0 | 0 |
| 1 | 1 | −99 | −0.396 |

`add_shake(ShakeImpulse{pitch, roll, yaw, period_ms, duration_ms})` ставит
только ненулевые каналы (нулевые не сбрасывают чужие); `add_shake(double)`
использует `default_shake_period_ms` / `default_shake_duration_ms`
(200 / 800). Тряска добавляется **после** клампов игрока, поэтому крен
shake не ограничен ±20° — как в движке.

### 3.2. View pitch и затухание

```
pitch_pending += look.pitch                  # player[0x2F38]
если pending != 0: view_pitch += pending; pending = 0
иначе если timer_a != timer_b:
    view_pitch -= (timer_a − timer_b)·0.016·k(scale)/divisor
если locked: view_pitch = 0
view_pitch = clamp(view_pitch, −π/2, +π/2)
k(scale) = 1.0 при scale < 0.5, иначе 0.25 + 1.5·scale
```

В поставочном билде `player[0x3360/0x3364/0x2F48/0x2FE0]` никто не пишет,
поэтому затухание выключено (`timer_a = timer_b = 0`). Сеттеры
`set_view_pitch_decay` / `set_pitch_locked` нужны для сверки с
`tools/re/shake.py` и будущих снимков сети/демо.

### 3.3. Отдача

`SOURCEPLAYER_RECOIL(D)` — импульс скорости, а не добавка к углу
(`docs/SHAKE.md` §6):

```
recoil_velocity(yaw, view_pitch, D) =
    ( −sin(yaw)·|cos(view_pitch)|·D,  −cos(yaw)·|cos(view_pitch)|·D,  0 )
```

`CameraRig::recoil_velocity(D)` берёт `aim_yaw` (player yaw + look) и
`view_pitch`; тело (`body_pitch`) не участвует. Результат прибавляет
контроллер игрока, риг не меняется.

---

## 4. Проекция и FOV

### 4.1. Фокусные масштабы движка

`engine_focal(width, height, zoom)` (`docs/CAMERA.md` §7):

| поле | формула | 320×200 | 640×480 |
|---|---|---:|---:|
| `+0x90` | `zoom` | 1.0 | 1.0 |
| `+0x94` | `zoom·4H/(3W)` | 5/6 | 1.0 |

`engine_projection(width, height, zoom, near, far)` строит матрицу ровно с
этими `fx`/`fy`; при `zoom = 0.25` (cutscene) обе шкалы умножаются на 0.25.

### 4.2. `projection(fov, aspect, near, far)`

`fov` — **вертикальный** угол; `aspect` — отношение NDC:

```
fy = 1/tan(fov/2),   fx = fy/aspect
x' = fx·x, y' = fy·y, z' = A·z + B, w' = z
A = (far + near)/(far − near),  B = −2·far·near/(far − near)
```

`z = near` → `z_ndc = −1`, `z = far` → `+1`; `z = 0` даёт `w = 0` и NaN
(точки позади камеры отбрасываются). Неверные аргументы (`fov` вне
`(0, π)`, `aspect <= 0`, `near <= 0`, `far <= near`, NaN) дают единичную
матрицу.

Связь с движком точная: `engine_projection(W, H, zoom, ...)` равен
`projection(fov, aspect, ...)` при

```
fy = 1/tan(fov/2) = zoom·4H/(3W),   aspect = fy/fx = 4H/(3W)
```

Проверяется сравнением всех 16 элементов (допуск `1e-12`). Для 320×200 это
`fov = 2·atan(6/5) = 1.7521161011963868` рад, `aspect = 5/6`; для 640×480 —
`fov = 90°`, `aspect = 1`.

Дефолт `CameraConfig` — `fov = 90°`, `aspect = 4/3`: это удобная современная
рамка, а не доказанный угол игры (в движке угла нет, есть только масштабы).

`focal_to_fov(f) = 2·atan(1/f)`, `fov_to_focal` — обратная; `focal_to_fov(1)
= 90°`, `focal_to_fov(5/6) = 1.7521…`.

---

## 5. Третье лицо и сглаживание (гипотезы)

### 5.1. Третье лицо

Движок строит только первое лицо и cutscene (тот же eye, углы + π и
`zoom·0.25`, `docs/CAMERA.md` §9); полноценного третьего лица в
`gundx.exe` не доказано. Реализация — простая гипотеза:

```
eye3    = player_eye − forward·distance + up·height
target3 = eye3 + forward·focus_distance
```

`set_third_person(distance, height, radius)` задаёт параметры; при
`set_collision_world(&world)` и `radius > 0` сфера радиуса `radius`
сметается из `player_eye` назад (`−forward`, `CollisionWorld::sphere_cast`)
и `distance` укорачивается до первого контакта (`occluded = true`).
`height` добавляется после сметания. `radius = 0` или `world = nullptr`
отключают коллизию. Ориентация остаётся угловой (`yaw/pitch`), а не
`look_at` на игрока: это over-the-shoulder вариант, а не орбита.

### 5.2. Сглаживание

```
если smooth_time > 0:
    первый кадр / после snap_smoothing: smoothed = player_eye
    иначе: alpha = 1 − exp(−dt/smooth_time)
           smoothed += (player_eye − smoothed)·alpha
```

Сглаживается только точка глаза; углы не сглаживаются. `smooth_time = 0`
(дефолт) — прямой путь движка. `snap_smoothing()` нужен при телепорте или
респавне.

### 5.3. CLI-режим `--third-person`

`gunmetal --level level0 --third-person --render out.png` ставит модель
игрока в `SET_PLAYER_START` (низ модели на `spawn.z`), поворачивает её на
`spawn.yaw` и включает `CameraMode::Third` с окклюзией по `.sbw`.

```
--third-distance X   вынос назад (по умолчанию 220)
--third-height X     подъём над глазом игрока (по умолчанию 40)
--player-model NAME  модель шасси из SOBS/ (по умолчанию TICK = LIGHT)
--third-pos X,Y,Z    фото-режим: поставить шасси в точку, не в спавн
--third-yaw DEG      фото-режим: поворот шасси, не из спавна
```

Спавн `level0` — тесная ниша: сфера окклюзии укорачивает вынос до ~18
единиц, и камера оказывается внутри корпуса. Для постановочного кадра
используется проверенный открытый ангар `(0, -550, 11.5)`, например:

```sh
gunmetal --level level0 --third-person --third-pos 0,-550,11.5 \
         --third-yaw 90 --third-distance 140 --third-height 45 --fov 60 \
         --render out/third_person/level0_tick_third_person.png --width 1280 --height 720
```

`level1` (Testing Range после swap) устроен иначе: кампанийный старт
`(53, -527, 3)` — вертикальная шахта, до стены ~18 единиц, а пол/платформа
глубоко внизу. Вынос 220 (и даже 100) туда не помещается. Для постановки
шасси ставится в открытый зал базы на полу `z = 724` (точка `(200, 0, 724)`),
а камера — фото-оверрайд `--pos/--look`, потому что `CameraMode::Third`
смотрит горизонтально (`target = eye + forward * focus_distance`) и не
наклоняется на корпус:

```sh
gunmetal --level level1 --third-person --third-pos 200,0,724 --third-yaw 90 \
         --third-distance 100 --third-height 40 --fov 55 \
         --pos 100,-120,790 --look 210,0,760 \
         --render out/third_person/level1_tick_third_person.png --width 1280 --height 720
```

Второй кадр — крупный план спереди с пушкой:

```sh
gunmetal --level level1 --third-person --third-pos 200,0,724 --third-yaw 90 \
         --third-distance 100 --third-height 40 --fov 50 \
         --pos 320,80,775 --look 200,0,744 \
         --render out/third_person/level1_tick_third_person_front.png --width 1280 --height 720
```

---

## 6. Тесты и голдены

```
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_camera_tests -j 32
ctest --test-dir build/sandbox -R camera --output-on-failure
```

Синтетика (`engine/tests/test_camera.cpp`):

* базис: единичный, `yaw=+90°`, `pitch=+30°`, `roll=+20°`, Python-голден
  `roll=5°, pitch=15°, yaw=30°`, 200 наборов на ортонормальность и `det=+1`;
* первое лицо: eye/target/`view()` (eye → 0, target → `(0,0,100)`),
  высота глаза 10/14, тело-углы;
* клампы ±π/6, ±20°, ±π/2; накопление view pitch; lock; огромный pitch;
* тряска: точная таблица §3.1, порядок «значение → tick», три канала,
  невалидные импульсы;
* затухание `−6·0.016` и `−6·0.016·1.75/2`, `k(scale)` на границах;
* отдача: `(−98.48077530122079, −170.57370639048867, 0)` при
  `yaw=30°, view_pitch=10°, D=200`;
* проекция: `fx=0.75` при `fov=90°, aspect=4/3`, near/far → ∓1, NaN за
  камерой, фокусы 320×200 = `(1, 5/6)`, 640×480 = `(1, 1)`, zoom 0.25,
  эквивалентность `engine_projection` и `projection`;
* третье лицо: дистанция/высота, стена в 50 units → `distance = 49` при
  радиусе 1, `radius = 0`, возврат в первое лицо;
* сглаживание: `alpha = 1−exp(−0.5) = 0.3934693402873666`, сходимость,
  `snap_smoothing`;
* два прогона 120 кадров (yaw/pitch/roll, тряска, смена режима) —
  побитовое совпадение всех полей;
* негативы: NaN в pos/yaw/eye_height/look, `dt < 0`, `dt > max_dt`, NaN dt,
  невалидный конфиг (fov/aspect/near/far/smooth/third/max_dt), NaN в
  `add_shake`/`add_look_impulse`/`set_body_angles`, вырожденный `up`,
  `look_at` на нулевом направлении.

Реальные спавны (кампания, слот 0; `gm.scene.first_person(spawn,
eye=10.0, distance=100.0)`, LIGHT CHASSIS):

| уровень | spawn | eye | target |
|---|---|---|---|
| `dm1` | `(-160, 176, 64)` | `(-160, 176, 74)` | `(-160, 276, 74)` |
| `level0` | `(24, -208, 48)` | `(24, -208, 58)` | `(24, -108, 58)` |
| `level1` | `(84, 4180, -3064)`, yaw `-1.9198621771937623` | `(84, 4180, -3054)` | `(-9.969262078590845, 4145.797985667433, -3054)` |

`level1.forward = (-0.9396926207859084, -0.3420201433256685, 0)`; допуск
`1e-9` для target и `1e-12` для eye/forward (libm в C++ и Python может
разойтись на 1 ulp). Игра только читается; при отсутствии установки
реальная часть печатает `skipped` и не падает.

---

## 7. Trust: proven, data, hypothesis

**Доказано кодом (`gundx.exe` v1.36):**

* базис `0x41e025 = Rz(-yaw)·Rx(-pitch)·Ry(roll)` и `forward = e2`
  (`docs/CAMERA.md` §2–§4);
* eye = `player[+0x00/+0x08/+0x10 + 0x2F50]` (`0x448d2b`…`0x448d3a`);
* углы `pitch = p[0x18] + p[0x2E60] + shake`, `roll = p[0x20] + shake`,
  `yaw = p[0x28] + shake` (`0x4468e3`…`0x446910`);
* клампы ±π/6 (`0x444af8`…), ±20° (`0x444aaa`…), ±π/2
  (`0x443a72`…`0x443ac2`);
* треугольная тряска, `4·period`, пик `amplitude` (`0x4467ef`…`0x4468b1`),
  тик в мс (`0x457271`);
* `SOURCEPLAYER_RECOIL` — горизонтальный импульс (`0x459f4d`…);
* фокусы `+0x90 = zoom`, `+0x94 = zoom·4H/(3W)` (`0x4466a6`…`0x446780`).

**Доказано данными:** все 143 кампанийных и 248 DM-спавнов имеют
`pitch = roll = 0`; слот 0 `dm1`/`level0`/`level1` совпадает с
`gm/scene.first_person`; высота глаза LIGHT = 10.0; 320×200 → `fy = 5/6`.

**Гипотеза (не доказано):**

* третье лицо: дистанция, высота, over-the-shoulder ориентация, сфера
  коллизии — параметры выбраны, а не сняты с бинарника;
* сглаживание `1 − exp(−dt/τ)` — в движке интерполяция камеры не найдена;
* near/far `0.9` / `1e5`: `+0xe0 = 0.9` в структуре камеры есть, но его
  роль (near или вспомогательный коэффициент) не доказана;
* дефолтный `fov = 90°`, `aspect = 4/3` — удобная рамка, не угол игры;
* лимит скорости yaw (`yaw_rate_max`) и затухание view pitch — мёртвые
  ветки, включены только сеттерами.

**Не реализовано намеренно:** cutscene-переходы (`yaw + π`, `zoom·0.25`),
интерполяция между первым/третьим лицом, крен от поворотов/урона
(`docs/CAMERA.md` §11), «сползание» состояния 7 (`0.95·x + 0.75°/1.125°`),
покачивание/боб, per-level `k(level)` из `0x81bf98`.

---

## 8. Остаток

1. **Реальный FOV уровня.** `k(level)` в `+0xa0` не расшифрован; сейчас
   `engine_projection` использует только `zoom`, `W`, `H`.
2. **Третье лицо.** Нет доказанной дистанции/высоты, нет плавного перехода
   и окклюзии по нескольким лучам; коллизия — одна сфера назад.
3. **Сглаживание.** Экспонента — выбор реализации; в движке камера в
   первом лице ставится напрямую.
4. **Крен.** Источник `player[0x20]` (повороты, повреждения) не найден;
   риг принимает его через `set_body_angles`/`LookInput::roll`.
5. **Ввод.** `LookInput` — уже готовые дельты; чувствительность
   `1/1024` рад на отсчёт (`0x44c8f2`) и мёртвая зона — дело слоя ввода
   (`input/controls.*`), риг их не читает.
6. **Состояние 7** (смерть/конец уровня) и cutscene не перенесены.
7. **Near/far и клиппинг.** Значения — гипотеза; точная ступень
   растеризатора не проверялась.

---

## 9. Xref

| адрес | что |
|---|---|
| `0x41e025` | базис `Rz(-c)·Rx(-b)·Ry(a)` |
| `0x446590` | сборка кадра первого лица |
| `0x4467ef`…`0x4468b1` | три канала `SET_CAMERA_*_SHAKE` |
| `0x4468e3`…`0x446910` | упаковка углов и вызов базиса |
| `0x4439af`…`0x443a03` | yaw: pending/скорость |
| `0x443a03`…`0x443ac8` | view pitch: импульс, затухание, lock, кламп |
| `0x443f6d`…`0x44406b` | `k = 0.25 + 1.5·scale`, деление на zoom |
| `0x444aaa`…`0x444ba5` | кламп крена ±20° |
| `0x444af8`…`0x444b87` | кламп тангажа ±π/6 |
| `0x448d2b`…`0x448d3a` | `eye_z = player[0x10] + player[0x2F50]` |
| `0x459f4d`…`0x459fc7` | `SOURCEPLAYER_RECOIL(D)` |
| `0x4466a6`…`0x446780` | масштабы `+0x90`, `+0x94`, `+0xa0` |
| `0x440c1d`…`0x440c8f` | использование масштабов при выводе |
| `0x457230`/`0x457271` | тик shake-таймеров (мс) |
| `0x81be18`/`0x81be20` | π/6 (тангаж), 20° (крен) |
| `0x81bea8`/`0x81beb0` | −π/2 / +π/2 (view pitch) |
| `0x81bf90` | `player[0x2F50]` — высота глаза |
