# NEXT — очередь автономной работы

Обновляется в конце каждого хода. Если пользователь не написал `STOP`/`пауза`,
агент берёт верхний незаблокированный пункт и работает дальше без разрешения.

## Где что

- Журнал прошлых ходов (перенесён из этого файла 2026-10-22) — `docs/JOURNAL.md`.
- Аудит процесса, узкие места и очередь задач — `docs/RE_AUDIT.md`.
- Снимок состояния (HEAD, roadmap, тесты, кэш) — `python3 tools/qa/state.py`.
- Здесь — только очередь и следующий шаг; длинные отчёты сюда не писать.

## Сейчас

1. **Фог / альфа / фон кадра** — в работе у трёх агентов (`feat/fog`,
   `feat/alpha`, `feat/sky`, 2026-10-23). Как закончат — влить, прогнать
   self-test'ы и интегрировать подтверждённое в `gm/render.py` за флагами
   (`--fog`, `--bg`, альфа), затем пересчитать метрики
   `tools/re/oracle_render.py` на `win1..win3`.
   > Уже известно: 3dfx-путь использует Glide-фог (`grFogTable`,
   > `grFogMode`, `grFogColorValue` в импортах), SW-путь тянет цвет к белому
   > (`level = clamp(0x3f−z, 4, 0x3f)`, `lt[level*0x100+index]`,
   > `0x44194d`/`0x441b59`), фон у нас — выдуманный градиент.
2. **Эталонный кадр атомарно** — инструменты готовы, нужен один прогон на
   Windows. Найдена и исправлена причина рассинхрона: камера смотрит на
   `p[0x18] + p[0x2E60]` (наклон башни), а `state.ps1`/`capture.ps1` читали
   только `+0x18`. Подбор (`tools/re/oracle_fit.py`) дал по кадрам +0°/+8°/+14°
   и рост `corr` 0.05→0.29 (`win2`). Скрипты теперь читают `+0x2E60` и пишут
   `camera_pitch`; нужен `win4` уже с этим полем.
3. **Проекция `focal_y`** — измерено на трёх кадрах: `133.33` выигрывает по
   `edges` 3/3 и по `corr` 2/3 (согласуется с `+0x94 = zoom·4H/(3W)`), дефолт
   оставлен. Окончательная точка — на `win4` (см. `docs/ORACLE.md`).
4. **Материалы → альфа текстуры.** `docs/MATERIAL_BLEND.md` и `docs/ALPHA.md`
   (агент): per-material LUT в 3dfx-пути не подтверждены, кадр рисуется одним
   Glide-стейтом `ONE/ONE_MINUS_SRC_ALPHA`. Значит `face_blend` менять на
   альфа-блендинг текстуры, а не «уточнять веса».
5. **Разметка `M5`/`M6` в roadmap.** Код по игроку/бою/AI уже есть и покрыт
   CTest (`weapons`, `player_trigger`, `thing_ai`, `thing_motion`, `events`,
   `blast_damage`, `aim_switch`, `game_state`, `rng`), а в roadmap эти пункты
   стоят ⬜. Задача — сверить пункты с кодом и тестами и поставить 🟡/✅
   с пометкой «> …», не выдумывая готовность.
6. **`SECTOR_STUFF`:** маппинг «сущность → сектор» для `SET_SOURCE/HANDLE
   SECTOR_*` (27–31, 40–45) и потребители секторного света (`0x470c70`,
   `0x453b50`, `0x46c5c0`).
7. **Числа живого кадра** (`n`, `param_2` интегратора) — нужен отладчик
   оригинала; до этого `step()` синтетика (`docs/PLAYER_CONTROLLER.md`).

### Закрыто кодом (проверено 2026-10-23, не переоткрывать)

- заряд → `fire(...)`: `gm::update_player_weapon` получает `ChargeState`,
  `fire(..., charge.charge)` (`game/trigger.cpp:60-68`), `runtime.cpp:341`;
- `Ai::apply_aim` из игрового тика: `runtime.cpp:449` (opt-in `ai_apply_aim`);
- события в тике: `events_.pump(now)`, `install_switches`,
  `post_door_events` (`runtime.cpp:494-496`);
- AI: path-семейство, leash, swept-sphere, виталы (`world/ai.h`);
- ввод игрока → выстрел, снаряды, blast-урон (`player_trigger`,
  `blast_damage`).

### Сабагенты: статус (2026-10-23)

- Партия 2 влита: `feat/look16` (`9d47865`), `feat/sectorlight` (`67f3d3e`),
  `feat/matblend` (`6282101`). Бесплатные модели по-прежнему недоступны
  (`beam-free` — не в этой стране, `opencode/*-free` — лимит); запуски шли на
  `opencode-go/deepseek-v4.1-flash` по разрешению пользователя.
- Worktree: `~/build/gm-sub-{look16,sectorlight,matblend,spritegolden,
  texaudit,oraclerender,lmaudit}` — можно удалять после мержа.

## Инфраструктура (2026-10-22, ✅)

- **Постоянный кэш вместо tmpfs.** `tools/re/gmcache.py`: листинг objdump,
  бинарный индекс `gundx.asm.idx`, копия exe и зеркало установки в
  `~/.cache/gunmetal-re` (переживает `wsl --shutdown`). `exelib` собирает
  листинг сам; `disasm()` — bisect + seek (≈100 мс → ≈0,02 мс на запрос),
  `gmcache.py selftest` — 98 сэмплов, 0 расхождений на 283 626 строках.
- **Зеркало игры.** `regress.py --mirror` (сверка + автоСинхронизация):
  `113/113 за 45,7 с` против `113/113 за 78,8 с` на `/mnt/d`;
  `gm.py inventory` 7,1 с → 0,14 с, `sob --check` 1,5 с → 0,07 с.
- **Регистр имён.** `regress.py::gp()` и пары `.SME/.SML` больше не зависят
  от регистронезависимой drvfs (зеркало на ext4); `savegame.py` берёт exe из
  кэша, а не из `/tmp/opencode` (из-за этого прежний baseline давал 112/113).
- **Ещё параллель.** `gm.py sounds` 28,6 → 9,6 с (WAV-деревья совпали
  побайтово), `sou1_model.py --verify` ~10 мин → ~2 мин (`--jobs`, JSON
  совпал с `docs/manifests/sou1-fnv.json`, 0 расхождений).
- **Кэш результатов.** С `--mirror` четыре чистые тяжёлые проверки
  (`sob_writer`, `sou_container`, VM-корпус, film parity) кэшируются по хэшу
  исходников + digest зеркала + argv; повторный `--full` 106,9 → 79,8 → 60,2 с
  (`--json.cached`), падения не кэшируются.
- **Тесты по содержимому.** `tools/re/game_content.py` (+ `engine/tests/
  game_content.h`, `game_path.h`): `.sbt`/`.sbw` резолвятся по FNV-1a64
  канонического содержимого из манифеста, поэтому переставленные в установке
  `level1`/`level2` больше не дают ложных падений (`core`/`camera_rig`/
  `sbtvm`/`vm` зелёные). `--check` фиксирует аномалию, `--write` обновляет
  таблицу детерминированно.
- **DOS-сборка `gun.exe`.** `tools/re/lexe.py` — LE/LX-парсер (stdlib):
  7 объектов, 670 страниц, 33 055 fixup на 200 страницах, импортов 0,
  EIP `obj 2 + 0x8f614` → файл 0xD9E14 (`eb 76`, баннер WATCOM), 670 страниц
  покрывают объекты без пересечений; `--selftest` 23/23, 10 порч → ValueError.
  Осталось: iterated pages объекта 7, неопознанные поля заголовка (`docs/LEXE.md` §8).
- **Аудит установки.** `tools/re/install_audit.py`: 2161 игровой файл,
  2154 ok, 6 renamed (3 пары `.sbc`/`.sbt`/`.sbw`), 0 missing/modified/extra,
  `GUNMETAL.CFG` volatile; 4,3 с на 8 потоках, `--strict` → rc=2.
- **Фазз-корпус.** `tools/fuzz_formats.py --save/--replay/--seed` + инспектор
  `tools/qa/fuzz_corpus.py`: 552 кейса quick / 1224 полных, манифест с sha256
  входов, детерминизм проверен (`docs/FUZZ_CORPUS.md`).
- **Покрытие.** `tools/qa/coverage.py` (stdlib `trace`): быстрый набор — 41,8 %
  строк (`--full` — 47,7 %), тёмные места `render`/`sou1`/`sbv`/`png`/`sml`;
  `--json`, `--missing MODULE`, `--min` для гейта.
- **Контекст/токены.** `tools/ctx.py` (`doc`/`addr`/`find`/`outline`/`task`,
  бюджет 40 строк), `docs/INDEX.md`, правила в `AGENTS.md`, методика —
  `docs/RE_METHOD.md`.
- **Проверено:** `regress.py` 113/113 на установке и на зеркале, `--full`
  121/121 за 175,7 с (зеркало),
  `player_fields --self-test`, `names`/`table_layout`/`xref`,
  `gmcache selftest`, `ctx.py index`.
- **Следующий шаг:** пункт 1 — освещение `level0` (кривая `0..127 → RGB`
  через `LOOK16.TAB`). Блокеров нет.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.
