# NEXT — очередь автономной работы

Обновляется в конце каждого хода. Если пользователь не написал `STOP`/`пауза`,
агент берёт верхний незаблокированный пункт и работает дальше без разрешения.

## Сейчас

1. **Impact/THING → runtime.** `Weapons::load` уже разворачивает collide/expire
   события `THING_TYPES` в `impact_blasts`/`expire_blasts` (GRENADE/ROCKET,
   тесты `weapons`/`things`). Осталось: применить нужную ветку в момент
   попадания/истечения, затем PLASMA/RGM/MRL/NUKE/EMP/`level10`.
   Проверка: `weapons` + новые golden’ы из `/tmp/opencode/gm-impact-matrix.md`.
2. **AI wiring.** Навигация/240-кадровая история цели по `target_history_offset`;
   использовать `Thing::health/shields/hits/events` в runtime.
3. **Event effects.** Реестр текста `0x619860`, побочные эффекты switch,
   сетевой event-блоб (`u32 len + bytes`).
4. **Player integrator.** Проверить `0x81bfb8` (`v = v*k + step`) в отладчике:
   согласовать `op4` как ускорение/максимум и `ControllerConfig`.
5. **Полный регресс.** `ctest` (52), `regress.py --full`, обновить baseline.

## Стоп-условия

- Пользователь написал `STOP`/`пауза`.
- Все пункты выше закрыты или помечены `⛔` с причиной.
- Нужно необратимое действие/секрет, которого нет.

## Последний ход

- Impact-отчёт прочитан; добавлены `Blast`, `apply_blast`, `apply_thing_damage`,
  исправлен `ShieldSide`, предпочтение воздушной ветки `FIRE_*`.
- `weapons` (1/1), `things` (1/1), `thing_ai` (1/1), полный CTest 52/52
  (до последней правки `thing_type`; targeted после неё — 1/1).
- Добавлен CLI `--third-person`: `TICK` (LIGHT) ставится низом на
  `SET_PLAYER_START`, поворачивается на `spawn.yaw`, камера — `CameraRig`
  `Third` с окклюзией `.sbw`. Флаги `--third-distance/height`,
  `--player-model`, фото-оверрайды `--third-pos/--third-yaw`.
  Новый тест `cli_third_person` — 1/1. Кадр:
  `out/third_person/level0_tick_third_person.png` (1280×720).
- ⚠️ Установка игры изменена по просьбе пользователя: содержимое `level1.*`
  и `level2.*` переставлено. Поэтому corpus-голдены `core`, `core_full`,
  `camera_rig`, `sbtvm`, `vm`, `vm_full`, `things_full` падают на реальных
  данных; `controls` падает независимо (изменённые `KEYCFG/*.key`). Это не
  регрессия кода; после обратного swap голдены снова сойдутся.
- Следующий шаг — пункт 1 (не начинать без явного запроса: текущая
  side-задача завершена).
