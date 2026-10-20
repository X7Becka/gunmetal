# Player speed and gravity: Ghidra notes

**Статус:** 2026-10-20, Ghidra 12.1.4. Это дополнение к
`docs/PLAYER_CONTROLLER.md`; здесь только то, что удалось доказать по коду.

## 1. Скорость из `DEFINE_PLAYER_TYPES`

`0x47a980` (spawn/player init) читает запись `DEFINE_PLAYER_TYPES` из таблицы
`0x7995e0` с шагом **`0x1cc`** и раскладывает её в runtime-поля игрока:

| runtime | источник | смысл |
|---|---|---|
| `0x81bd70` (double) | запись `+0x20/+0x24` (`0x799600`/`0x799604`) | базовая максимальная скорость |
| `0x81bd80` (double) | запись `+0x18` (`0x7995f8`) | штраф за единицу превышения веса |
| `0x81bd88` (double) | вычисляется `0x4524d0` | скорость после весового штрафа |
| `0x81bd78` (double) | вычисляется `0x443690` | эффективная скорость с health-фактором |

`0x4524d0`:

```text
speed = player[0x81bd70]
if weight > limit:
    speed -= (weight - limit) * player[0x81bd80]
speed = max(speed, 0.05)          // double 0.05
player[0x81bd88] = speed
```

`0x443690` берёт `0x81bd88`, умножает на `0x81bf88` (health factor) и получает
`0x81bd78`; затем:

```text
vx = cos(heading) * player[0x81bd78] * throttle
vy = sin(heading) * player[0x81bd78] * throttle
```

Интегратор в `0x443690` делает `param_2` шагов и прибавляет `local_48 / param_2`
к скорости, где `local_48` уже содержит `param_2` тиков. **Множителя `1/91` или
`dt` в этом месте нет**, поэтому `1.70` из `op4` — это **мировые единицы за
тик 91 Гц**, а не за секунду. C++-конверсия `1.70 * 91 = 154.7` ед/с корректна
для текущей модели контроллера.

`docs/FORMATS.md` §12.3 фиксирует те же поля и формулу
`weight-limited speed`; Ghidra подтверждает её и добавляет нижний clamp `0.05`.

## 2. `SET_LEVEL_GRAVITY` — это множитель, не `g`

VM-опкод 96 (`0x45b7db`) делает:

```asm
FLD double ptr [EBX+ESI]
ADD EBX, 8
FSTP double ptr [0x0080f840]
```

При загрузке уровня `0x456f50` сбрасывает `0x80f840` в **1.0**
(`low=0`, `high=0x3ff00000`). Значения `0.1…4.5` из `SET_LEVEL_GRAVITY` —
это множитель, а не ускорение.

Чтения `0x80f840`:

- `0x443690` (`0x44493e`) — `penetration -= value * 0.2 * dt`;
- `0x4690f0` — масштаб вертикального смещения/подвески;
- `0x472790`, `0x4752d0` — тот же член `value * -0.2` в подвеске/вертикальной
  динамике.

То есть оригинал не делает `vel.z -= g * dt` с одной константой: он считает
подвеску/проникновение, а `0x80f840` масштабирует этот член. **Базовая
скалярная гравитация в этом коде не найдена.** C++ `gravity = 700` —
синтетическая модель, а не перенос оригинала; её нельзя выдавать за
реконструкцию.

## 3. Что это меняет в C++/docs

- `ControllerConfig::max_speed = 1.70 * 91.0` — оставить; это доказанная
  конверсия «единицы/тик → единицы/с». В комментарии можно убрать «unit not
  proven».
- `ControllerConfig::gravity` — оставить только как гипотезу/параметр
  синтетического контроллера; в docs явно отделить от `SET_LEVEL_GRAVITY`.
- `0x81bd70`/`0x81bd80`/`0x81bd88`/`0x81bd78` добавлены в `tools/re/names.csv`
  как data-поля подсистемы `player`.
- `radius`, `height`, `step_height`, `jump_speed`, `accel`, `friction` пока
  **не доказаны**; их поиск — отдельная задача (капсула/коллизии).

## 4. Воспроизведение

```text
list_xrefs 0x81bd70       -> 0x47a980 (write), 0x4524d0 (read), ...
list_xrefs 0x80f840       -> 0x45b7db (write), 0x443690/0x4690f0/...
decompile 0x47a980        -> таблица 0x7995e0, stride 0x1cc
decompile 0x4524d0        -> weight-limited speed, clamp 0.05
decompile 0x443690        -> speed*health, cos/sin, tick integration
disassemble 0x45b7db      -> FSTP [0x80f840]
decompile 0x456f50        -> reset 0x80f840 = 1.0
```
