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
  sbc.py              уровневые «лайтмапы» .sbc
  sob.py              модели .sob (и таблицы .soe)
  soa.py              анимации .soa
  sog.py              песни .sog (текстовые плейлисты)
  sou.py              звук .sou (контейнер архива SOU.DAT)
  sbv.py              видео .sbv (контейнер + собственный кодек)
  sbw.py              мир/геометрия .sbw
  sbt.py              мир/уровень .sbt (скомпилированная «World Things file»)
  soe.py              сущности .soe (текстовая схема «thing»)
  look.py             таблицы поиска LOOK16.TAB / LOOK162.TAB
  palette.py, bmp.py, png.py   палитра и вывод изображений
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

# модели и геометрия уровня
python3 gm.py sob      "…/Gunmetal 3dfx/SOBS" out/models
python3 gm.py sbw      "…/Gunmetal 3dfx"/*.sbw --obj out/levels

# уровни: таблицы handle + сверка с соседним .sbw
python3 gm.py sbt      "…/Gunmetal 3dfx" --resolve -v

# прочее
python3 gm.py soa      "…/Gunmetal 3dfx"            # анимации
python3 gm.py soe      "…/Gunmetal 3dfx"            # «thing»-определения
python3 gm.py look     "…/Gunmetal 3dfx/LOOK16.TAB" --png look16.png --palette "…/FLAG/naUSA.BMP"
python3 gm.py sog      "…/Gunmetal 3dfx/MUSIC" --music "…/Gunmetal 3dfx/MUSIC/SOU.IDX"
python3 gm.py sbv      "…/Gunmetal 3dfx/Vid/Intro.sbv" --png out/intro --frames all
```

Список всех команд: `python3 gm.py --help`, справка по команде —
`python3 gm.py <команда> --help`.

## Статус форматов

| Формат | Что это | Статус |
|---|---|---|
| BIGFILE `.idx`/`.dat` | архивы (PICS, sprites, sounds, FRONTEND, MUSIC) | разобран; **repack** (чтение+запись, по-байтово точно) |
| `.sbm` | 8-битные текстуры/спрайты | разобран |
| `.bmp`, палитра | общая VGA-палитра из BMP; вывод PNG | разобран |
| `.sob` | модели (672 файла) | разобран |
| `.sbc` | уровневые градационные карты (51 файл) | разобран (побайтово) |
| `.soa` | анимации (77 файлов) | разобран |
| `.soe` | «thing»-определения (текст) | разобран (грамматика снята с загрузчика) |
| `.sog` | песни (651) | разобран |
| `.sou` | контейнер звука (999 + 3092 записи) | контейнер разобран; кодеки SOU1/2 — Miles, не воспроизведены |
| `.sbv` | видео (Intro/orient/winner) | разобран полностью, включая кодек (сверка с эмулятором) |
| `.sbw` | мир: геометрия + поля полигонов/объектов | разобран (36/36 до EOF) |
| `.sbt` | мир/уровень (World Things file) | разобран (36/36; таблицы handle сверены с `.sbw`) |
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
