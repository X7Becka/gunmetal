# `render/camera_model.h` — режимы камеры, focal adjust, детерминированная тряска

Что здесь: самостоятельная модель камеры (`engine/src/render/camera_model.{h,cpp}`)
поверх готовой точки глаза — режимы `cockpit/chase/orbit/fixed`, масштабы
проекции движка (`+0x90`/`+0x94`), треугольная волна `SET_CAMERA_*_SHAKE` и
детерминированный импульс тряски с явным seed. Математика без SDL, без
глобального состояния, только `double`; `rand()` не используется.

Источники: `docs/CAMERA.md` §2–§7 (базис `0x41e025`, точка глаза, клампы,
фокусы), `docs/SHAKE.md` §5 (формула и таблица тряски), `docs/CAMERA_CPP.md`
§0/§5.1/§7 (что уже реализовано и что остаётся гипотезой).

Тест: `engine/tests/test_camera_model.cpp`, CTest `camera_model` (метка
`quick`, ~0.00 с), зелёный; `python3 regress.py --mirror --json` —
`115/115 passed`.

---

## 0. Коротко

| что | значение | статус |
|---|---|---|
| мир | X вправо, Y вперёд, Z вверх; `yaw = 0` → +Y, `yaw > 0` → к +X | доказано (`CAMERA.md` §2) |
| `pitch > 0` | взгляд **вниз** | доказано (§3) |
| `cockpit` | `position = eye`, углы = `yaw/pitch/roll` | доказано (§4–§5) |
| eye | caller передаёт `player.pos + eye_height`; модель высоту не добавляет | доказано (§5) |
| `chase` | `eye − (sin yaw, cos yaw, 0)·60 + (0,0,0)`, углы как есть | MODEL (`CAMERA_CPP.md` §5.1) |
| `orbit` | `eye − forward(yaw,pitch)·60`, углы пересчитаны в look-at | MODEL |
| `fixed` | позиция из `CameraConfig::fixed_position`, target игнорируется | MODEL |
| фокусы | `fx = zoom`, `fy = zoom·4H/(3W)` → 320×200 `(1, 5/6)`, 640×480 `(1, 1)` | доказано (`CAMERA.md` §7) |
| FOV 320×200 | `2·atan(6/5) = 1.7521161011963868` рад (`kFov320x200`) | выведено из фокусов |
| тряска (волна) | треугольник `4·period`, пик `amplitude`, `angle += A/p·s` | доказано (`SHAKE.md` §5.2) |
| тряска (импульс модели) | xorshift32, seed из конфига, линейная огибающая | MODEL |
| затухание | `env -= shake_decay·dt`, ровно 0 (а не «почти») | MODEL |
| near/far | 0.9 / 1e5 | MODEL (`+0xe0 = 0.9`, роль не доказана) |
| fov по умолчанию | 90° | MODEL (`CAMERA_CPP.md` §4.2) |

Однострочно (крен 0, без тряски):

```
forward = ( sin(yaw)·cos(pitch),  cos(yaw)·cos(pitch), -sin(pitch) )
right   = ( cos(yaw),            -sin(yaw),             0            )
up      = ( sin(yaw)·sin(pitch),  cos(yaw)·sin(pitch),  cos(pitch)  )
```

---

## 1. Что доказано, что модель

**Доказано кодом `gundx.exe` v1.36 (адреса — в `CAMERA.md`/`SHAKE.md`):**

* базис `0x41e025 = Rz(−yaw)·Rx(−pitch)·Ry(roll)`, `forward = e2`, знаки
  `yaw`/`pitch` (`CAMERA.md` §2–§4);
* первое лицо: `eye = (p.x, p.y, p.z + player[0x2F50])`, углы
  `pitch = p[0x18] + p[0x2E60] + shake`, `roll = p[0x20] + shake`,
  `yaw = p[0x28] + shake` (`0x446590`, `CAMERA.md` §4–§6);
* фокусы `+0x90 = zoom`, `+0x94 = zoom·4H/(3W)` (`0x4466a6`…`0x446780`);
* `SET_CAMERA_{PITCH,ROLL,YAW}_SHAKE`: `p = max(1, period)`,
  `r = duration mod 4p`, `s = (r < 2p) ? r − p : 3p − r`,
  `angle += amplitude/p · s`, таймер в мс убывает до 0 (`SHAKE.md` §5).

**Модель (здесь придумано, в бинарнике не доказано):**

* режимы `chase`/`orbit`/`fixed` и их числа: движок строит только первое лицо
  и cutscene (`CAMERA.md` §9), дистанция/высота третьего лица не снята
  (`CAMERA_CPP.md` §5.1, §11.3);
* импульс `CameraModel::add_shake()` (шум + линейная огибающая): в игре
  тряска приходит из трёх скриптовых каналов, а в поставочных `.sbt` нет ни
  одного вызова `SET_CAMERA_*_SHAKE` (`SHAKE.md` §7.3), поэтому амплитуду и
  скорость затухания нельзя взять из данных; доказанная волна доступна
  отдельно как `triangle_shake()`;
* `near_plane = 0.9`: `+0xe0 = 0.9` в структуре камеры есть, но что это
  near plane — не доказано (`CAMERA.md` §7, §11.5);
* `far_plane = 1e5`, `fov = 90°`, `focus_distance = 100`;
* `fixed_position` и веса каналов тряски (pitch 1.0, roll 0.5, yaw 0.5).

**Не реализовано намеренно:** cutscene-путь (`yaw + π`, `zoom·0.25`,
`CAMERA.md` §9), клампы тела ±π/6/±20° и view-pitch ±π/2 (они уже в
`camera/camera.h`), отдача `SOURCEPLAYER_RECOIL` (это импульс скорости, не
угол, `SHAKE.md` §6), per-level `k(level)` из `0x81bf98`.

---

## 2. Режимы

`enum class CameraMode { cockpit, chase, orbit, fixed }`; `target` — это
**точка глаза** (caller сам прибавляет `eye_height`), углы в радианах.
Тряска (текущие `shake_yaw_/shake_pitch_/shake_roll_`) прибавляется к углам
во всех режимах.

### 2.1. `cockpit` — доказано

```
position = target
yaw/pitch/roll = переданные
```

`pose({24,−208,58}, 0, 0)` → позиция `(24, −208, 58)`, углы `0/0/0`
(level0: спавн `(24,−208,48)` + глаз LIGHT 10). При `yaw = 90°, pitch = 0.3`
`forward = (0.955336489125606, 5.85e−17, −0.29552020666133955)` — взгляд
вдоль +X и вниз.

### 2.2. `chase` — MODEL

```
position = target − (sin yaw, cos yaw, 0)·chase_distance + (0, 0, chase_height)
углы    = переданные (over-the-shoulder, не look-at)
```

Смещение берётся **только по yaw**: pitch не «поднимает» камеру и не
опускает её, поэтому при `pitch ≠ 0` позиция та же, а ориентация следует
pitch. Это сознательное отличие от гипотезы `CAMERA_CPP.md` §5.1
(`eye − forward·distance + up·height`): при `pitch = 0` формулы совпадают, а
при ненулевом тангаже полная версия уводит камеру под пол/в потолок без
коллизии. Числа `60`/`0` — гипотеза из `CAMERA_CPP.md` §0/§5.1.

`pose({100,200,30}, 0, 0)` при `(60, 12)` → `(100, 140, 42)`;
`yaw = 90°` → `(40, 200, 42)`; `pitch = 0.5` позицию не меняет.

### 2.3. `orbit` — MODEL

```
position = target − forward(yaw, pitch)·chase_distance
yaw'   = atan2(target.x − position.x, target.y − position.y)
pitch' = atan2(−(target.z − position.z), hypot(dx, dy))
roll'  = 0
```

`pose({0,0,0}, 0, 0.2)` при дистанции 60 → `(0, −58.803994670474495,
11.920159847703673)`, обратно `yaw = 0`, `pitch = 0.2`; базис из этих углов
возвращает ровно в target (`|ошибка| ≤ 1e−12`). Угол `yaw` нормализуется в
`(−π, π]` (`atan2`), исходное значение за пределами диапазона не сохраняется.

### 2.4. `fixed` — MODEL

```
position = CameraConfig::fixed_position
углы     = переданные, target игнорируется
```

Штатив/фото-режим: `pose({9,9,9}, 1, −1, 0.3)` при `fixed_position =
(1,2,3)` → `(1,2,3)`, углы `1/−1/0.3`.

---

## 3. Тряска

### 3.1. Доказанная волна: `triangle_shake()`

Свободная функция повторяет `SET_CAMERA_*_SHAKE` (`SHAKE.md` §5.2):

```
p = period_ms (> 0), r = duration_ms mod 4p
s = (r < 2p) ? r − p : 3p − r
return amplitude/p · s
```

Таблица `A = 0.4`, `P = 100` воспроизводится точно (те же числа, что в
`SHAKE.md` §5.2 и `CAMERA_CPP.md` §3.1):

| duration, мс | результат, рад |
|---:|---:|
| 400 | −0.4 |
| 300 | 0 |
| 200 | +0.4 |
| 100 | 0 |
| 1 | −0.396 |

В `pose()` она не применяется: в игре эти каналы двигают скрипты, а
поставочные `.sbt` их не вызывают (`SHAKE.md` §7.3). Функция — эталон
формулы и готовая точка подключения для VM-команд.

### 3.2. Импульс модели: `add_shake()` / `tick()` / `shake()`

Генератор — свой xorshift32 (`x ^= x<<13; x ^= x>>17; x ^= x<<5`), seed —
`CameraConfig::seed` (0 заменяется на `0x9E3779B9`, чтобы состояние не
залипло). На импульс:

```
env = |amplitude|; seed сбрасывается; рисуются 3 отсчёта u ∈ [−1,1]
v = (u0, 0.5·u1, 0.5·u2) / |(u0, 0.5·u1, 0.5·u2)|
shake_pitch/roll/yaw = env · v          # |offset| = env
```

`tick(dt)`:

```
env -= shake_decay·dt;  если env <= 0 → env = 0 (ровно), offset = 0
иначе — новый отсчёт и elapsed += dt
```

`shake()` возвращает `env` (магнитуда углового смещения, рад). Скорость
`shake_decay` — MODEL (у движка таймер линейный в мс, `SHAKE.md` §5.4);
линейная огибающая в амплитуде даёт ту же «линейно до нуля» картину.
`add_shake(0)` и `clear_shake()` выключают тряску. Знак амплитуды
игнорируется.

Золотые числа (seed `0x9E3779B9`, `add_shake(1.0)`): `pitch =
−0.6982337496373869`, `roll = 0.7151637082602379`,
`yaw = −0.03178838238697359`, `|offset| = 1.0`.

---

## 4. Focal adjust

`engine_focal(W, H, zoom)` — доказанные масштабы `CAMERA.md` §7:

| W×H | `fx` | `fy = zoom·4H/(3W)` |
|---|---:|---:|
| 320×200 | 1 | 0.8333333333333334 (5/6) |
| 640×480 | 1 | 1 |

`focal_adjust(camera_space, W, H, zoom)` применяет их к точке в
координатах камеры `(e1·p, e3·p, e2·p)` (экран X, экран Y, глубина):
`(fx·x, fy·y, z)`. `(1,1,1)` → `(1, 5/6, 1)`; `(2,−3,4)` →
`(2, −2.5, 4)`; при `zoom = 0.25` (cutscene) → `(0.25, 0.20833333333333334, 1)`.
Отрицательные/нулевые/NaN `W`, `H`, `zoom` — отказ с текстом, вход
возвращается без изменений.

`focal_to_fov(f) = 2·atan(1/f)`, `fov_to_focal` — обратная; при
`f = 5/6` → `1.7521161011963868` (`kFov320x200`), при `f = 1` → 90°
(`kFov640x480`). Переход «фокус → пиксели» не доказан (`CAMERA.md` §11.6):
угол верен при нормировке NDC в ±1.

---

## 5. API

```cpp
#include "render/camera_model.h"
using namespace gm::camera_model;

CameraModel model;                        // дефолты, ошибок нет
CameraConfig cfg = model.config();
cfg.chase_distance = 60.0;
cfg.chase_height = 12.0;
if (!model.configure(cfg)) { /* model.error() — текст, конфиг старый */ }

CameraPose p = model.pose({24, -208, 58}, 0.0, 0.0, CameraMode::cockpit);
CameraPose c = model.pose(eye, yaw, pitch, 0.1, CameraMode::chase); // с креном
Vec3d s = model.focal_adjust({1, 1, 1}, 320, 200);                  // (1, 5/6, 1)

model.add_shake(0.3);                     // seed из конфига
model.tick(1.0 / 91.0);                   // env -= decay·dt, отсчёт обновлён
double amp = model.shake();               // рад, ровно 0 после затухания
```

| тип | поля / методы |
|---|---|
| `CameraMode` | `cockpit`, `chase`, `orbit`, `fixed` |
| `CameraConfig` | `fov`, `near_plane`, `far_plane`, `chase_distance`, `chase_height`, `shake_amplitude`, `shake_decay`, `shake_period_ms`, `shake_duration_ms`, `focus_distance`, `fixed_position`, `seed` |
| `CameraPose` | `position`, `yaw`, `pitch`, `roll` |
| `CameraModel` | `configure`, `config`, `pose` (2 перегрузки), `focal_adjust`, `add_shake` (2), `tick`, `shake`, `shake_envelope`, `shake_pitch/roll/yaw`, `shake_elapsed`, `clear_shake`, `clear_error`, `ok`, `error` |
| свободные | `engine_focal`, `focal_to_fov`, `fov_to_focal`, `forward`, `right`, `up`, `triangle_shake`, `kFov320x200`, `kFov640x480` |

Ошибки не бросают исключений: `configure`/`tick` возвращают `false` и
пишут текст, `add_shake`/`pose`/`focal_adjust` возвращают `void`/значение и
пишут `error()`; состояние при отказе не меняется. Успешный
`configure`/`add_shake`/`tick` очищает ошибку.

**Namespace `gm::camera_model`, а не `gm`:** в `engine/src/camera/camera.h`
уже объявлен `gm::CameraMode` (`First/Third`), и этот модуль не имеет права
править тот заголовок. Вложенное пространство имён исключает конфликт, если
оба заголовка когда-нибудь окажутся в одной единице трансляции.

---

## 6. Тест и числа

```
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake --build build/sandbox --target gm_camera_model_tests -j 4
ctest --test-dir build/sandbox -R '^camera_model$' --output-on-failure
```

Фактический вывод (`gm_camera_model_tests`, Debug, `zigcxx`, `1e−15`):

```
defaults: fov=1.5707963267948966 near=0.9 far=100000 chase=(60, 0) shake=(0.05, 1) seed=0x9e3779b9
cockpit:  pos=(24, -208, 58) yaw=0 pitch=0 roll=0
cockpit:  forward=(0.95533648912560598, 5.8497488675817182e-17, -0.29552020666133955)
chase:    target=(100, 200, 30) yaw=0 -> pos=(100, 140, 42)
chase:    yaw=90° -> pos=(40, 200, 42)
orbit:    pos=(0, -58.803994670474495, 11.920159847703673) yaw=0 pitch=0.2
focal:    320x200=(1, 0.83333333333333337) 640x480=(1, 1)
focal:    unit zoom 0.25=(0.25, 0.20833333333333334, 1)
wave:     duration=400 ms -> -0.4 | 300 -> 0 | 200 -> +0.4 | 100 -> 0 | 1 -> -0.396
shake:    add(1.0) env=1 pitch=-0.69823374963738694 roll=0.7151637082602379 yaw=-0.031788382386973589
shake:    after 63 x 1/64 s env=0.015625
shake:    after 64 ticks env=0 (exactly 0)
shake:    91 Hz: env=0 after 92 ticks (1.0109890109890109 s)
determ:   same seed -> yaw=0.48728464704521057 pitch=-0.27929349985495477 roll=0.38606548330409518
determ:   other seed -> yaw=0.60653943188441162 pitch=0.37882569658736265 roll=0.02830313075980033
camera_model: ok
```

Что проверяется:

* `cockpit`: позиция = target, углы проходят как есть; формулы
  `forward/right/up` при `yaw = 90°, pitch = 0.3` (числа выше); `roll`
  передаётся; `|forward| = 1`;
* `chase`: `(100,140,42)` при yaw 0 и `(40,200,42)` при yaw 90°; pitch не
  сдвигает позицию; дистанция 0 → точка глаза;
* `orbit`: позиция `(0, −58.803994670474495, 11.920159847703673)`,
  восстановленные `yaw/pitch` возвращают в target; `fixed` берёт позицию из
  конфига;
* `focal`: `(1, 5/6)` / `(1, 1)`, zoom 0.25, `(2,−3,4) → (2,−2.5,4)`,
  `focal_to_fov/fov_to_focal` на `5/6` и `1`;
* волна: таблица `SHAKE.md` §5.2, в том числе `400 → −0.4`, `1 → −0.396`;
* импульс: золотые числа PRNG при seed `0x9E3779B9`; `|offset| = env`;
  `63` тика по `1/64 с` дают `0.015625`, `64`-й — ровно `0`; на 91 Гц — 92
  тика (накопление `1/91`); после нуля `pose` побитово равна позе без
  тряски; `add_shake(0)`/отрицательная амплитуда;
* детерминизм: 200 тиков (тряска каждые 50, cockpit/chase вперемешку) — два
  прогона с одним seed побитово равны, другой seed даёт другие углы;
* негативы (все с текстом): `fov = 0`, `fov > 180°`, NaN `fov`, `near = 0`,
  `near >= far` (включая `2/1`), `chase_distance < 0`, `shake_decay = 0`,
  `shake_period_ms = 0/NaN`, `shake_amplitude < 0`, `focus_distance = 0`,
  NaN `fixed_position`; `dt = 0`, `dt < 0`, NaN/Inf `dt` (состояние не
  меняется); NaN/Inf `add_shake`; нулевые/отрицательные/NaN `W/H/zoom` и
  NaN-точка в `focal_adjust`; NaN в `pose`.

---

## 7. Ограничения

* **Не подключено к `render/`**: `render_scene` по-прежнему принимает
  `RenderOptions::camera_override` (`eye`/`target`); модель ничего не
  рисует и не меняет существующие кадры (паритет не затронут).
* **Нет коллизии камеры**: `chase`/`orbit` проходят сквозь стены и пол —
  окклюзия/сфера из `camera/camera.h` здесь не реализованы.
* **Нет интерполяции/сглаживания кадров**: поза считается на текущий тик,
  временного сглаживания eye/углов и «bob» нет; в движке сглаживание не
  найдено (`CAMERA.md` §11.4).
* **Нет клампов тела и view-pitch**: модель принимает уже сложенные углы;
  ±π/6, ±20°, ±π/2 живут в `camera/camera.h`/контроллере.
* **Тряска — не скриптовые каналы**: `add_shake` не привязан к VM
  `SET_CAMERA_*_SHAKE`; амплитуда/скорость затухания — MODEL, реальных
  значений в поставочных данных нет (`SHAKE.md` §7.3).
* **near/far и FOV в градусах** — гипотезы; `engine_focal` доказан, переход
  к пикселям/углу зависит от ступени растеризатора (`CAMERA.md` §11.5–11.6).
* **`fixed`/`orbit` — заглушки режимов**, а не восстановленное поведение:
  в `gundx.exe` их нет (кроме cutscene, который не реализован).
* **`pose` — const, но пишет текст ошибки** в `mutable` поле; на корректных
  входах это не наблюдаемо, но потокобезопасным класс не является (как и
  остальной движок).

---

## 8. Xref

| что | где |
|---|---|
| `0x41e025` — базис `Rz(−c)·Rx(−b)·Ry(a)` | `CAMERA.md` §3 |
| `0x446590` — кадр первого лица, углы `+0x20/+0x18/+0x28` | `CAMERA.md` §4 |
| `0x448d2b`… — `eye_z = player[0x10] + player[0x2F50]` | `CAMERA.md` §5 |
| `0x4466a6`…`0x446780` — `+0x90 = zoom`, `+0x94 = zoom·4H/(3W)` | `CAMERA.md` §7 |
| `0x4467ef`…`0x4468b1` — три канала shake, треугольник `4p` | `SHAKE.md` §5.2 |
| `0x457271` — тик shake-таймеров, мс | `SHAKE.md` §5.4 |
| `0x45b411/0x45b462/0x45b4b3` — `SET_CAMERA_{PITCH,ROLL,YAW}_SHAKE` | `SHAKE.md` §5.1 |
| `0x81be18`/`0x81be20` — ±π/6 / ±20° | `CAMERA.md` §6 |
| `0x4aa390` — `zoom` (cutscene `·0.25`) | `CAMERA.md` §7, §9 |
| реализация | `engine/src/render/camera_model.h`, `.cpp` |
| тест | `engine/tests/test_camera_model.cpp` (CTest `camera_model`) |
