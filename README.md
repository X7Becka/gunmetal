# Gunmetal asset toolkit

Инструментарий для разбора и конвертации ресурсов игры **Gunmetal** (1998,
Mad Genius Software) — DirectX-сборка `gundx.exe` (v1.36). Это не декомпиляция
движка: форматы восстановлены по дизассемблеру `gundx.exe` и проверены на
реальных данных (побайтовое совпадение до EOF, попадание индексов в диапазон,
сверка с эмулятором x86 для видеокодека). Цель — чтение/конвертация активов, а
дальше и сборка обратно (моддинг).

Игра **только читается**: ничего в каталоге установки не изменяется, результаты
пишутся в отдельный каталог.

## Требования

* Python 3.11+ (проверено на 3.14), **только стандартная библиотека** —
  внешних зависимостей нет (`pip`/`PIL`/`numpy` не нужны; PNG/BMP-кодеки
  написаны вручную поверх `zlib`).
* Каталог установки Gunmetal (например `D:\games\Gunmetal 3dfx`; в WSL —
  `/mnt/d/games/Gunmetal 3dfx`).

## Состав

```
gm.py                 единый CLI (все команды ниже)
regress.py            сквозная регрессия по установке
gm/                   модули по форматам:
  bigfile.py          архивы BIGFILE (.idx + .dat)
  sbm.py              текстуры/спрайты .sbm
  sbc.py              лайтмапы уровня .sbc
  sob.py              модели .sob (и таблицы .soe)
  soa.py              анимации .soa
  sog.py              песни .sog (текстовые плейлисты)
  sou.py              звук .sou (контейнер архива SOU.DAT)
  sbv.py              видео .sbv (контейнер + собственный кодек)
  sbw.py              мир/геометрия .sbw
  sbt.py              мир/уровень .sbt (скомпилированная «World Things file»)
  sbtvm.py            скриптовая ВМ SCRIPT (опкоды с именами, таблицы команд,
                      IF/ELSE/END, DEFINE_*-таблицы, декомпилятор)
  scene.py            сцена уровня из кода SCRIPT: свет, «места», предметы,
                      спрайты, звуки + таблица сущностей (статической
                      расстановки в .sbt нет; id дверей/переключателей на
                      геометрию в поставке не отображаются)
  soe.py              сущности .soe (текстовая схема «thing»)
  look.py             таблицы поиска LOOK16.TAB / LOOK162.TAB
  render.py           софт-рендер моделей/уровней в PNG (камера, z-буфер,
                      планарные UV, текстуры из PICS/sprites)
  palette.py, bmp.py, png.py   палитра и вывод изображений
tools/gen_sbtvm.py    генератор gm/sbtvm.py из exe (правь его, не вывод)
tools/re/             инструментарий реверса exe (PE-карта, дизасм, xref,
                      таблицы команд, эмулятор x86) — см. tools/re/README.md
docs/FORMATS.md       спецификации форматов (главный документ)
NOTES.md              рабочие заметки, карта адресов движка, CLI-справка
```

## Быстрый старт

```bash
# что внутри архива
python3 gm.py list "…/PICS/SBM.IDX"

# все текстуры и спрайты в PNG (палитра берётся из установки)
python3 gm.py textures "…/Gunmetal 3dfx" out/textures
python3 gm.py sprites  "…/Gunmetal 3dfx" out/sprites --transparent 0

# звуки в WAV (формат SOU0) и «сырьё» (SOU1/2 — кодек Miles, не PCM)
python3 gm.py sounds   "…/Gunmetal 3dfx" out/sounds

# repack архивов: собрать из каталога или заменить одну запись
python3 gm.py extract  "…/Gunmetal 3dfx/sprites/SBM.IDX" out/raw
python3 gm.py pack     out/SBM.IDX out/raw --ext .bin
python3 gm.py set      "…/Gunmetal 3dfx/sprites/SBM.IDX" HERO01 new.bin out/mod.IDX

# правка текстуры: .sbm -> PNG -> правка -> .sbm -> в архив
python3 gm.py sprites  "…/Gunmetal 3dfx" out/sprites --transparent 0
python3 gm.py sbm      out/sprites/HERO01.png out/HERO01.sbm --game "…/Gunmetal 3dfx" --flags 257
python3 gm.py set      "…/Gunmetal 3dfx/sprites/SBM.IDX" HERO01 out/HERO01.sbm out/SBM.IDX

# модели и геометрия уровня
python3 gm.py sob      "…/Gunmetal 3dfx/SOBS" out/models
python3 gm.py sbw      "…/Gunmetal 3dfx"/*.sbw --obj out/levels
python3 gm.py sbw      "…/Gunmetal 3dfx"/*.sbw --rebuild out/levels_rebuilt   # правка геометрии

# просмотр: софт-рендер модели/уровня в PNG (с текстурами из PICS)
python3 gm.py view     "…/Gunmetal 3dfx/SOBS/kingtest.sob" --out out/view --size 640 --sheet
python3 gm.py view     "…/Gunmetal 3dfx/SOBS" --out out/view --size 256 --game "…/Gunmetal 3dfx"
# вид изнутри уровня: ручная камера + отсечка по дальности + материалы
python3 gm.py view     "…/Gunmetal 3dfx/dm1.sbw" --out out/view --size 640 \
                       --pos 0,0,20 --look 60,60,20 --clip 200 --cull
# лайтмапы уровня берутся из <уровень>.sbc автоматически (--no-lightmap, --ambient N)
# инспекция одной комнаты: --sector N (--no-sectors выключает отсечку по пирамиде)
python3 gm.py view     "…/Gunmetal 3dfx/dm1.sbw" --out out/view --size 640 --sector 2
# сцена уровня из кода SCRIPT: маркеры света/мест/предметов/звуков + их освещение
python3 gm.py scene    "…/Gunmetal 3dfx/dm1.sbt"
python3 gm.py view     "…/Gunmetal 3dfx/dm1.sbw" --out out/view --size 640 \
                       --scene --scene-lights

# уровни: таблицы handle + сверка с соседним .sbw
python3 gm.py sbt      "…/Gunmetal 3dfx" --resolve -v
python3 gm.py sbt      "…/Gunmetal 3dfx" --rebuild out/levels_rebuilt   # запись обратно
python3 gm.py sbt      "…/Gunmetal 3dfx/dm1.sbt" --script                # декомпиляция SCRIPT
python3 gm.py sbt      "…/Gunmetal 3dfx" --script --script-out out/scripts

# прочее
python3 gm.py soa      "…/Gunmetal 3dfx"            # анимации
python3 gm.py soe      "…/Gunmetal 3dfx"            # «thing»-определения
python3 gm.py look     "…/Gunmetal 3dfx/LOOK16.TAB" --png look16.png --palette "…/FLAG/naUSA.BMP"
python3 gm.py sog      "…/Gunmetal 3dfx/MUSIC" --music "…/Gunmetal 3dfx/MUSIC/SOU.IDX"
python3 gm.py sbv      "…/Gunmetal 3dfx/Vid/Intro.sbv" --png out/intro --frames all
```

Запись обратно (repack) поддерживают команды `pack`/`set` (архивы), `sbm`
(PNG/BMP → текстура) и флаг `--rebuild DIR` у `sbw`, `sbt`, `sbc`, `soa`, `soe`,
`sog` — файлы собираются побайтово точно (проверяется в `regress.py`).

Список всех команд: `python3 gm.py --help`, справка по команде —
`python3 gm.py <команда> --help`.

## Статус форматов

| Формат | Что это | Статус |
|---|---|---|
| BIGFILE `.idx`/`.dat` | архивы (PICS, sprites, sounds, FRONTEND, MUSIC) | разобран; **repack** (чтение+запись, по-байтово точно) |
| `.sbm` | 8-битные текстуры/спрайты | разобран; **кодировщик** (blob round-trip побайтово; PNG/BMP→.sbm) |
| `.bmp`, палитра | общая VGA-палитра из BMP; вывод PNG | разобран |
| `.sob` | модели (672 файла) | разобран; **рендер в PNG** (`gm.py view`, текстуры из PICS) |
| `.sbc` | уровневые градационные карты (51 файл) | разобран (побайтово); **писатель** |
| `.soa` | анимации (104 файла) | разобран; **писатель** |
| `.soe` | «thing»-определения (текст) | разобран; **писатель** |
| `.sog` | песни (651) | разобран; **писатель** |
| `.sou` | контейнер звука (999 + 3092 записи) | контейнер разобран; кодеки SOU1/2 — Miles, не воспроизведены |
| `.sbv` | видео (Intro/orient/winner) | разобран полностью, включая кодек (сверка с эмулятором) |
| `.sbw` | мир: геометрия + поля полигонов/объектов | разобран (36/36 до EOF); **писатель** (rebuild побайтово); **рендер в PNG** (`gm.py view`) |
| `.sbt` | мир/уровень (World Things file) | разобран (36/36; handle сверены с `.sbw`); **писатель** (rebuild побайтово, структурные записи 205/205); **скриптовая ВМ SCRIPT** разобрана — имена опкодов (в т.ч. `DEFINE_WEAPONS`/`DEFINE_AMMO`/`DEFINE_EQUIPMENT`/`DEFINE_PLAYER_TYPES`/`DEFINE_LEVEL_OBJECTIVES`/`ADD_LIFT`/`SOURCEPLAYER_PATH`, `IF`/`ELSE`/`END`), декомпилятор, 36/36 в байтовый конец |
| `LOOK16.TAB`/`LOOK162.TAB` | кубы 64³ «6-6-6 RGB → индекс палитры» | разобран |

Детали и точные структуры — в `docs/FORMATS.md`; карта адресов движка — в
`NOTES.md`.

## Регрессия

```bash
python3 regress.py            # быстрые проверки (парсинг всех уровней и т.д.)
python3 regress.py --full     # плюс распаковка текстур/спрайтов/звуков/frontend
python3 regress.py --game "…/Gunmetal 3dfx" --out /tmp/opencode/regress
```

Скрипт прогоняет `gm.py` по всем семействам данных и сверяет итоговые строки
(количества и «0 failed»). Ненулевой код возврата — регрессия. Всё пишется в
`--out` (по умолчанию `/tmp/opencode/regress`), каталог игры не трогается.

## Заметки

* Тяжёлые операции (распаковка, конвертация) лучше выполнять в Linux-доме или
  `/tmp`, а не на медленных монтированиях `/mnt/c`/`/mnt/d`.
* `gun.exe` — DOS/4GW-сборка (нужен отсутствующий `GLIDE2X.OVL`); играбельная —
  `gundx.exe`, она и служит источником для дизассемблирования.
* Символы/структуры модулей менять свободно — это внутренний инструмент, обратная
  совместимость не требуется.
