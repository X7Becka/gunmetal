# HUD-фильмы `.sof` / `.fdx` — формат, привязка, паритет

* **Дата:** 2026-10-20.
* **Область:** `Film/*.sof` (48 файлов, 95 330 304 Б = 11 637 тайлов) и
  `Film/*.fdx` (6 файлов) + устаревший `Film/ammo.idx`.
* **Код:** `gm/sof.py` (читатель/писатель), `tools/parity_film.py` (паритет и
  негативные тесты), `gm.py film` (CLI, не менялся в этой ветке).
* **Статус:** контейнер, кадры, тайминг, палитра, `.fdx`-индекс и привязка к
  экранам закрыты; побайтовый round-trip 48/48 `.sof`, 6/6 `.fdx`, 1/1 `.idx`;
  декод кадров подтверждён дампом самой игры (30/30 BMP). Открытыми остаются
  несколько второстепенных пунктов (§9).

Проверка:

```sh
python3 tools/parity_film.py                 # 11/11 проверок, ~10 c
python3 tools/parity_film.py --json          # машинный отчёт
python3 tools/parity_film.py --dump /tmp/opencode/film-dump   # примеры PNG
python3 gm.py film "/mnt/d/games/Gunmetal 3dfx/Film" --json    # CLI
```

---

## 1. `.sof` — контейнер

**Заголовка нет.** Файл — плоский поток тайлов по **0x2000 (8192) байт**:

    tiles = size // 0x2000        (у всех 48 поставленных файлов остаток 0)
    offset(frame) = frame << 13

Доказательства в `gundx.exe`:

| что | адрес |
|---|---|
| загрузчик HUD-фильма | `0x49fbc0` |
| обновление/вывод кадра | `0x49fd40` |
| `offset = frame << 13` | `0x49fe0a` (`shl eax,0xd`) |
| `fseek(f, offset, SEEK_SET)` | `0x49fe10` → `0x40e110` |
| `fread(buf, 1, 0x2000, f)` | `0x49fe2c` → `0x40dc70` |
| буфер тайла `malloc(0x2000)` | `0x49fcbc` → `0x402770` |
| имя `FILM\%s.SOF` | `0x49fc19`, формат `0x4ac1b4` |
| запасной путь `%sFILM\%s.SOF` | `0x49fc80`, формат `0x4ac1a4` (префикс пути `0x5a4678`) |
| второй (скриптовый) загрузчик | `0x44da80`, тот же формат имени |

Размер файла движок не проверяет: `fread` может вернуть меньше 0x2000, и
тогда в буфере остаётся предыдущий кадр (короткое чтение игнорируется).
Поэтому читатель по умолчанию **сохраняет хвост** (`Sof.tail`) и возвращает
его в `to_bytes()`, а `strict=True` требует кратности 8192.

### 1.1. Кадр: 128×64, палитровые индексы, транспонирование

Тайл — одна картинка **64 шириной × 128 высотой** (128 строк по 64 байта),
хранимая транспонированной. Ветка в `0x49fd40`:

* `mode == 0` (все реальные вызовы) — транспонирование, `0x49fe48`…`0x49fe75`:

      for y in 0..63:
          for x in 0..127:
              dst[y*128 + x] = tile[x*64 + y]

* иначе (`0x49fe7f`…`0x49fea2`) — копия как есть: 128 строк по 64 байта
  (шириной 64), шаг приёмника `arg2`. Эта ветка в поставке не используется.

**Порядок кадров — прямой**: тайл `i` и есть кадр `i`, без перестановок,
дельт и общего заголовка. HUD-загрузчик начинает с кадра 1 (`start_frame=1`)
и при `loop=1` зацикливается через 0; скриптовый (`start=0`) начинает с 0.
Отдельного «кадра-палитры» или служебного первого тайла нет: тайл 0 —
обычный кадр (у `Default.sof` — чёрный, у `ammo0` — иконка).

Эквивалентная быстрая форма (реализована в `gm/sof.py`):

    out = bytearray(0x2000)
    for x in range(128):  out[x::128] = tile[x*64:(x+1)*64]     # decode
    for y in range(64):   tile[y::64] = pixels[y*128:(y+1)*128] # encode (inverse)

8 бит на пиксель, без RLE, без дельт: каждый тайл — полный кадр.
Проверка биективности кодека — все 11 637 тайлов.

**Доказательство транспонирования (не визуальная догадка).** В `Film/` лежат
30 дампов `Kflm0000.bmp` … `KFLM0029.BMP` (128×64, 8 бит). После декода
`Kelfilm.sof` кадры `0..29` **побайтово равны** этим BMP (30/30), и все BMP
несут одну и ту же палитру. См. §6.

### 1.2. Тайминг

Кадры листаются по тикам движка, не по миллисекундам:

* счётчик `[0x5a4a74]` (+1 за итерацию главного цикла, `0x44bdda`);
* HUD-вызов: `ms = 91 // fps` — константа `0x5b` в `0x49594a`/`0x499869`;
* шаг кадра в `0x49fd40`:

      frame = (tick - base) / ms
      if frame < 0:  frame = 0; base = tick
      if loop:       frame %= count
      if frame >= count: frame = count-1; clock = 0; return 1   # «фильм кончился»
      if frame == last and not force: return 0                  # кадр уже на экране

* `base = tick0 - ms * start_frame`, поэтому первый кадр = `start_frame`;
  HUD-загрузчик передаёт `start_frame = 1`, `loop = 1`, `clock = 1`;
* скриптовый фильм: часы `[0x5a46e4]` прибавляют `91 // savefilm` за цикл
  (`0x44d4f0`, ключ `savefilm` — строка `0x4abd60`, по умолчанию **15**,
  запись `0x44ad4d`), а длительность кадра берётся из `FILM_DEFINE`.

Итого формула: `кадр_на_экран = (tick - base) / (91 // fps)`.
91 — база тиков движка (константа), а не «fps=91»: при `fps=24` целочисленное
`91//24 = 3` тика на кадр. Реальный wall-clock зависит от частоты цикла
(см. §9, п. 3).

### 1.3. Куда рисуется

`0x4958d0` (`film_set(prefix, id, suffix, x, pitch)`), путь обновления
`0x495a08`…`0x495a32`:

    dest   = x + 125 * pitch + 14
    stride = pitch                       # шаг экранной поверхности
    update(dest, pitch, tick, force=1, mode=0)

То есть фильм — оверлей **128×64 в строке 125** экранной поверхности
(для 320×200 это низ экрана: строки 125…188), со сдвигом +14 по X.
`gm.py film --frame N --png` экспортирует сам кадр 128×64.

---

## 2. `.fdx` / `.idx` — индекс

Текст, по одной записи `"%ld,%ld"` на строку (`count, fps`), CRLF:

    131, 24
    ...

Парсер — `0x498e20`:

| что | адрес |
|---|---|
| `sprintf(buf, "FILM\\%s.fdx", name)` | `0x498e60`, формат `0x4beae4` |
| проверка длины имени (> 0x14 = 20 → отказ) | `0x498e3d`, строка `0x4beaf0` |
| `fopen(..., "r")` | `0x498e78` → `0x40dae0` |
| пропуск `index` строк (`fgetc` до `\n`) | `0x498e8a`…`0x498eb4` |
| `fscanf(f, "%ld,%ld", &count, &fps)` | `0x498ed3`, формат `0x4beac0` |
| «index wrong FILM\%s.fdx» | `0x498ef4`, строка `0x4beaa4` |
| «couldn't index FILM\%s.fdx» | `0x498ebc`, строка `0x4beac8` |
| `fps <= 0` → «invalid FPS for film %s» | `0x498f2b`, строка `0x4bea68` |
| сообщение `couldn't open FILM\%s%d.fdx` | `0x498f13`, строка `0x4bea84` |

Важно: `FILM\%s%d.fdx` встречается только в **тексте ошибки** (имя + номер);
открывается всегда `FILM\%s.fdx`, запасного `fopen` по второму имени нет.
Файлов `weapon8.fdx` и т. п. в поставке тоже нет.

**Смысл индекса строки доказан кодом вызова.** В `0x4958d0` один и тот же
номер `id` (аргумент `esi`/`arg3`) идёт и в `sprintf(buf, "%s%ld", prefix, id)`
(`0x49590f`…`0x49591b`, формат `0x4be744`), и в парсер индекса
(`0x49592d`: `push esi`). Значит `WEAPON8.SOF` читает **строку 8** из
`weapon.fdx`. Ошибка «index wrong» — это выход за число строк.

Поставка (все строки `131, 24`, кроме указанных):

| файл | строк | id-пространство | примечание |
|---|---|---|---|
| `weapon.fdx` | 24 | 0…23 (оружие) | строка 8 = `137, 24` |
| `ammo.fdx` | 16 | 0…15 (тип боеприпаса) | ровно 131 |
| `tank.fdx` | 8 | 0…7 (шасси) | ровно 131 |
| `equip.fdx` | 6 | 0…5 (оборудование) | файлов `EQUIP*.SOF` в поставке нет |
| `shield.fdx` | 1 | 0 (щит) | |
| `default.fdx` | 1 | 0 (запасной фильм) | `289, 24` |

`Film/ammo.idx` — **устаревшее имя того же формата**: 16 строк, но первая
`499, 24` (в `ammo.fdx` — `131, 24`). Это след старого экспортёра; движок
сначала ищет `.fdx`, затем `.idx` (`gm/sof.py:find_fdx`).

### 2.1. `count`: число кадров

`count` — **количество кадров**, верхняя граница (кадры `0 … count-1`);
это доказано скриптовыми фильмами, где `FILM_DEFINE` задаёт `count` явно и он
**всегда равен числу тайлов**:

| фильм | `FILM_DEFINE` | тайлов | скрипт |
|---|---|---|---|
| `comms` | 1755 | 1755 | `level3.sbt` |
| `cthu` | 391 | 391 | `level15.sbt` |
| `kelfilm` | 30 | 30 | `level8.sbt`, `level18.sbt` |
| `slasher` | 220 | 220 | `dm1.sbt`, `level22.sbt` |
| `level7` | 459 | 459 | `level7.sbt` |
| `level12` | 361 | 361 | `level12.sbt` |
| `level20` | 1744 | 1744 | `level19.sbt` |
| `level25` | 120 | 120 | `level23.sbt` |
| `tank0..3` | 131 | 132 | `level0/00/000/0a.sbt` |
| `default` | 289 | 290 | `level7.sbt` |

У HUD-семейств (`ammo`, `weapon`, `tank`, `shield`, `default`) в 47 из 48
файлов `count = tiles - 1`, то есть **один последний тайл не читается**;
`weapon8.sof` — единственный, где `count == tiles == 137`. Это данные
поставки, а не ошибка парсера: `parity_film.py` фиксирует оба случая.
Дополнительно HUD-загрузчик стартует с кадра 1 (`start_frame=1`), но при
`loop=1` кадр 0 достигается на втором круге (`frame %= count`).

---

## 3. Привязка: какой `.sof` к какому экрану/оружию

Имя собирается как `<PREFIX><id>` (`0x4958d0`), префиксы — литералы в коде:

| экран / семейство | префикс | адрес строки | вызов `film_set` |
|---|---|---|---|
| оружие (гараж, «CUSTOM CHASSIS») | `WEAPON` | `0x4be894` | `0x496d82` |
| боеприпасы (панель статуса) | `AMMO` | `0x4be724` | `0x495494` |
| шасси | `TANK` | `0x4be7e8` | `0x496566` |
| щит | `SHIELD` | `0x4be7e0` | `0x49657e` |
| оборудование | `EQUIP` | `0x4be778` | `0x495d78` |
| временный/шаблон (`TEMP0`) | `TEMP` | `0x4be7f0` | `0x49654f` |
| запасной фильм | `DEFAULT` | `0x4bb344` | `0x49597e`, `0x4959b4` |

`WEAPON` и `AMMO` берут номер из одного и того же глобала `0x81c4a0`
(в коде — `[base+0x81c4a0]`, `0x496d73`, `0x495485`; `base` — запись
игрока/шасси); `TANK`/`SHIELD` передают номер из того же поля/0. Что это
именно **индекс элемента**, видно по `0x47b423`: значение умножается на 56 и
индексирует таблицу боеприпасов `0x818960` (размер записи 56 Б, см.
`docs/FORMATS.md`). Рядом лежат документированные поля записи шасси
`0x81c44c`/`0x81c450` («id объекта выбранного/прежнего шасси», см.
`docs/FORMATS.md` §13), а экран подписан «CUSTOM CHASSIS» (`0x4be78c`) и
грузит `SML\DESIGN\TEMP0.SML` (`0x4be7bc`). Отсюда привязка:

* фильм — **анимированная иконка выбранного элемента** в гараже/панели
  снаряжения (128×64 в нижней части экрана);
* номер элемента = **id в пространстве `.fdx`** (строка индекса) и
  одновременно числовой суффикс имени файла;
* `0x81c4a0` — «текущий выбранный id» для панели; точное имя поля (слот
  оружия vs. тип боеприпаса) остаётся **гипотезой** (§9, п. 1).

Фактическое покрытие поставки:

| семейство | id с файлами | id без файлов |
|---|---|---|
| weapon | 1–8, 10–19 | 0, 9, 20–23 |
| ammo | 0–7, 9–11 | 8, 12–15 |
| tank | 0–3 | 4–7 |
| shield | 0 | — |
| equip | — | 0–5 (только `equip.fdx`) |
| default | 0 (`Default.sof`) | — |

Если файла нет, `fopen` в `0x49fbc0` возвращает ошибку, и HUD остаётся без
нового фильма (код возврата `-1`/`-2`), поэтому «дыры» в id-пространстве —
норма.

### 3.1. Скриптовые (не-HUD) фильмы

14 `.sof` не имеют `.fdx`: их проигрывает второй загрузчик `0x44da80` по
команде скрипта **`FILM_DEFINE`** (opcode `0xb3`, сигнатура `LPLS`:
`count, ms, flag, name`; обработчик `0x45c000`, соседние `FILM_PLAY` `0xb4`,
`FILM_STOP` `0xb5`, `FILM_REWIND` `0xb6`). Восемь из них реально определены в
поставке (§2.1), пять — **нигде не упомянуты** (ни в `.sbt`, ни в exe, ни в
`.sml`): `BIOFORT.SOF` (562), `JFILM3.SOF` (279), `Jtest.sof` (314),
`jupiter.sof` (120), `soyboy.sof` (499) — вероятно, вырезанный/тестовый
контент.

Тайминг скриптовых фильмов задаётся вторым числом `FILM_DEFINE` (3…7 тиков
на кадр), а не `fps`: `3` у `tank*`, `4` у `default`, `5` у `comms`/`cthu`/
`level20`/`level25`, `6` у `kelfilm`/`level7`, `7` у `slasher`/`level12`.

Третье число `FILM_DEFINE` — флаг цикла (`1` во всех поставленных скриптах).
`FILM_PLAY` (`0x44da30`) включает часы: `base = clock - frame*ms`, глобал
`0x5a4ac4 = 1`; `FILM_STOP` (`0x44da20`) сбрасывает его в 0; `FILM_REWIND`
(`0x44da60`) ставит `frame = 0`, `base = clock`. Обновление/вывод —
`0x44de30` (те же правила `frame = (clock-base)/ms`, `%= count`, clamp).

---

## 4. Палитра

Кадры — индексы общей VGA-палитры игры (256 цветов). Палитру для фильмов
можно взять из дампа самой игры: `Film/Kflm0000.bmp`; ту же таблицу несут
30 `Kflm*.bmp` и ещё 17 BMP (всего 47 файлов, в `Film/` и `PICS/` — 11).
Она совпадает с палитрой из
`FLAG/*.bmp` (`gm.palette.installed`) во всех 236 «обычных» слотах и
отличается ровно в 20 слотах `NEONS.PIN` (`173, 174, 175, 191, 240…255`) —
это неоновые цвета. Для рендера фильмов правильнее брать вариант из
`Film/Kflm0000.bmp`:

```python
from gm import sof
pal = sof.load_palette("/mnt/d/games/Gunmetal 3dfx")   # R,G,B,X
blob = sof.to_png(path, pal, frame_index)
```

`gm.py film --palette BMP` работает как раньше; без `--palette` CLI пишет
grayscale (не менялось).

---

## 5. Доказательства (сводка)

1. **Контейнер.** 48/48 размеров кратны 8192; `offset = frame << 13`,
   `fread(0x2000)` — код `0x49fbc0`/`0x49fd40`.
2. **Транспонирование.** 30/30 `Film/Kflm*.bmp` побайтово равны кадрам
   `Kelfilm.sof` после декода (и палитры совпадают). Без транспонирования
   иконки `ammo0` выглядят повёрнутыми (визуальная проверка).
3. **Кадры — полные, без RLE.** Каждый тайл — ровно 8192 индекса; дамп BMP
   совпал с кадром целиком, включая фон.
4. **`count` = кадры.** `FILM_DEFINE` в 8 скриптах даёт ровно число тайлов;
   HUD-семейства дают `tiles-1` (лишний тайл не читается).
5. **Индекс строки.** Один номер идёт и в `%s%ld` имени, и в парсер `.fdx`
   (`0x49590f`/`0x49592d`); `weapon.fdx:8 = 137` совпадает с 137 тайлами
   `weapon8.sof` — единственная строка, отличная от 131.
6. **Round-trip.** `tools/parity_film.py`: 48/48 `.sof` (FNV набора
   `08238cbb`, 95 330 304 Б), 6/6 `.fdx` (504 Б, FNV `ccf43961`), 1/1
   `ammo.idx` — байт в байт; 11 637/11 637 тайлов проходят
   `encode(decode(t)) == t`.
7. **Негативные тесты.** 23 кейса: пустой/обрезанный тайл, хвост, лимиты
   счётчиков, битые строки `.fdx`, `fps=0/-1`, BOM, CRLF/LF/без EOL, пустые
   строки, выход за границы кадра/строки — всё либо отвергается
   `ValueError`/`IndexError`, либо сохраняется дословно.

---

## 6. Безопасность и границы (в стиле `docs/SECURITY.md`)

`gm/sof.py` следует общему принципу «сначала границы, потом выделение»:

* `Sof.parse`: `size // 0x2000` → число тайлов; `MAX_TILES = 1<<20`
  (8 ГиБ); хвост < 8192 сохраняется, а не отбрасывается;
  `strict=True` отвергает некратность;
* `frame()`/`tile()`: `0 <= index < tiles`, иначе `ValueError` (раньше
  `frame()` проверял только `< 0` и мог вернуть короткое чтение);
* `decode_tile`/`encode_tile`: ровно 8192 Б, иначе `ValueError`;
* `.fdx`: регулярка `^\s*(\d+)\s*,\s*(\d+)\s*$`; `MAX_FDX_LINES = 4096`,
  `MAX_COUNT = 1<<20`, `1 <= fps <= 1000` (движок сам отвергает `fps <= 0`);
  пустые строки допустимы и сохраняются; длина строки не ограничена
  (файл читается целиком — как и остальные парсеры тулкита);
* `to_bytes()` никогда не теряет байты: `.sof` = тайлы + хвост,
  `.fdx` = исходные строки с их EOL;
* `load_palette()` только читает BMP; `to_png()` не пишет файлы.

`docs/SECURITY.md` пока не упоминает `.sof/.fdx` в таблице §1 и в
фазз-матрице §4 — это задача основного агента (файл в этой ветке не
редактировался).

---

## 7. Инструменты

```sh
# паритет + негативные тесты на установке (только чтение)
python3 tools/parity_film.py --game "/mnt/d/games/Gunmetal 3dfx"

# машинный отчёт
python3 tools/parity_film.py --json

# примеры кадров (пишет только в указанный каталог вне игры)
python3 tools/parity_film.py --dump /tmp/opencode/film-dump

# CLI (без изменений в gm.py)
python3 gm.py film "/mnt/d/games/Gunmetal 3dfx/Film" --json
python3 gm.py film "/mnt/d/games/Gunmetal 3dfx/Film/AMMO4.SOF" \
        --frame 0 --png /tmp/opencode/ammo4.png \
        --palette "/mnt/d/games/Gunmetal 3dfx/Film/Kflm0000.bmp"
```

Вывод паритета (сокращённо):

```
ok   48 .sof films discovered                    48 files
ok   all .sof round-trip byte-exactly            48 films, 11637 tiles, 95330304 B
ok   tile codec is a bijection (all tiles)       48 films checked
ok   6 .fdx + 1 legacy .idx round-trip           canonical '%d, %d' CRLF
ok   Kflm*.bmp == Kelfilm.sof frames             30/30 BMPs, palette identical
ok   HUD films resolve to their .fdx line        35 HUD, 8 scripted, 5 unreferenced
ok   negative matrix                             23 cases
film parity: 11/11 checks passed
```

---

## 8. Строки для интеграции в `gm.py` (сам файл не менялся)

`gm.py` уже импортирует `gm.sof`; чтобы получить новую функциональность,
достаточно заменить тело `cmd_film` (и добавить флаги). Предлагаемые строки:

```python
from gm import sof as sofmod

# 1) расширенная карточка вместо tile_count/find_fdx:
info = sofmod.binding(path)
# info = {file, prefix, id, tiles, bytes, aligned, fdx, line, count, fps,
#         extra_tiles}

# 2) палитра по умолчанию — из дампа фильма, а не grayscale:
pal = palette_arg or sofmod.load_palette(opts.game)

# 3) кадр в PNG (path | Sof | готовые пиксели):
blob = sofmod.to_png(path, pal, opts.frame)

# 4) все кадры:
for i, pixels in enumerate(sofmod.frames(path)):
    ...

# 5) побайтовый round-trip / --rebuild:
blob = sofmod.parse_file(path).to_bytes()
fdx_blob = sofmod.load_fdx(idx).to_bytes()
```

CLI-флаги, которых сейчас нет: `--tiles` (список кадров и bbox изменений),
`--fdx` (дамп строк индекса), `--rebuild DIR` (запись `Sof.to_bytes()` и
`Fdx.to_bytes()`; писать через `_wopen`, как остальные команды),
`--all-png DIR` (все кадры). `film` должен остаться read-only по умолчанию.

---

## 9. Что осталось

1. **Точное имя `0x81c4a0`.** Это «текущий выбранный id» панели (одно поле и
   для `WEAPON`, и для `AMMO`), оно индексирует таблицу боеприпасов
   (`0x47b423`). Не доказано, слот это оружия, тип боеприпаса или общий курсор
   списка; в частности, не проверено, что id совпадает с номером строки
   `DEFINE_WEAPONS`. Проверяется трассировкой записи `0x81c4a0` в экране
   гаража.
2. **Лишний тайл HUD-фильмов.** У 47 файлов `count = tiles-1`; зачем
   экспортёр оставил неиспользуемый последний кадр (и почему `weapon8` —
   исключение) — не доказано. На чтение это не влияет.
3. **Wall-clock скорость.** Механизм тиков восстановлен (`91`, `savefilm`,
   `start=1/loop=1`), но фактическая частота зависит от главного цикла;
   нужен runtime-трейс (или запуск под отладчиком), чтобы сказать «24 fps
   в `.fdx` играет как N кадров/с».
4. **`equip.fdx` без `EQUIP*.SOF`.** Индекс на 6 строк есть, файлов нет;
   возможно, патч/другая поставка. Префикс `TEMP` вызывается с номером 0
   (`0x49654f`), но `TEMP0.SOF` в поставке нет — вызов, видимо, закрыт
   условием/экраном, которого нет в этой версии.
5. **5 «сиротских» фильмов** (`BIOFORT`, `JFILM3`, `Jtest`, `jupiter`,
   `soyboy`) не упомянуты ни в скриптах, ни в exe — вырезанный контент.
   Если найдутся ссылки в другой версии игры, добавить в таблицу §3.1.
6. **C++-читателя `.sof` нет**; `engine/**` в этой задаче не трогался.
7. **`docs/SECURITY.md`** — добавить `.sof/.fdx` в таблицу §1 и в
   `tools/fuzz_formats.py` (сейчас `MAX_TILES`/`MAX_FDX_LINES` покрыты только
   `tools/parity_film.py`).

---

## 10. Приложение: инвентарь 48 `.sof`

`tiles` — число тайлов; `ref` — строка `.fdx` или `FILM_DEFINE`.

| файл | семейство | id | tiles | ref |
|---|---|---|---|---|
| `ammo0,1,2,3,5,7,9,10,11.sof`, `AMMO4,6.SOF` | ammo | 0–7, 9–11 | 132 | `ammo.fdx:<id>`, 131@24 |
| `WEAPON1,3,4,6,7.SOF`, `Weapon10…19.sof`, `weapon2,5.sof` | weapon | 1–7, 10–19 | 132 | `weapon.fdx:<id>`, 131@24 |
| `weapon8.sof` | weapon | 8 | 137 | `weapon.fdx:8`, **137**@24 |
| `tank0,3.sof`, `TANK1,2.SOF` | tank | 0–3 | 132 | `tank.fdx:<id>`, 131@24 (и `FILM_DEFINE` 131/3) |
| `Shield0.SOF` | shield | 0 | 132 | `shield.fdx:0`, 131@24 |
| `Default.sof` | default | 0 | 290 | `default.fdx:0`, 289@24 (и `FILM_DEFINE` 289/4) |
| `COMMS.SOF` | — | — | 1755 | `FILM_DEFINE(1755, ms=5)` |
| `CTHU.SOF` | — | — | 391 | `FILM_DEFINE(391, ms=5)` |
| `Kelfilm.sof` | — | — | 30 | `FILM_DEFINE(30, ms=6)` |
| `SLASHER.SOF` | — | — | 220 | `FILM_DEFINE(220, ms=7)` |
| `LEVEL7.SOF` | — | — | 459 | `FILM_DEFINE(459, ms=6)` |
| `level12.sof` | — | — | 361 | `FILM_DEFINE(361, ms=7)` |
| `LEVEL20.SOF` | — | — | 1744 | `FILM_DEFINE(1744, ms=5)` |
| `level25.sof` | — | — | 120 | `FILM_DEFINE(120, ms=5)` |
| `BIOFORT.SOF` | — | — | 562 | нет ссылок |
| `JFILM3.SOF` | — | — | 279 | нет ссылок |
| `Jtest.sof` | — | — | 314 | нет ссылок |
| `jupiter.sof` | — | — | 120 | нет ссылок |
| `soyboy.sof` | — | — | 499 | нет ссылок |

Итого 48 файлов / 11 637 тайлов / 95 330 304 Б.

## C++ reader

`engine/src/res/sof.{h,cpp}` mirrors the Python tile codec:

```sh
gunmetal --film AMMO4 --frame 0
# film: AMMO4.sof
# bytes: 1081344 tiles: 132 tail: 0
# frame 0: 8192 bytes fnv1a64=06531011db474c67
# fdx: ammo.fdx entries: 16 line 4: count=131 fps=24

gunmetal --film AMMO4 --frame 0 --png out.png   # 128x64 RGBA
```

Animated preview (colour type 3, shared palette, `loops=0`):

```sh
python3 gm.py film Film/AMMO4.SOF --apng ammo4.apng
python3 gm.py film Film/AMMO4.SOF --apng ammo4.apng --limit-frames 4  # test
```

`gm/png.apng_indexed` writes `acTL`/`fcTL`/`IDAT`/`fdAT`; `fps` comes from the
sibling `.fdx` line (24) and the frame count is the binding `count` (131 for
AMMO4, one trailing tile is never read).  The rebuilt `.sof`/`.fdx` remain
byte-exact (`--rebuild`).

`gm_sof_tests` (CTest `film_sof`) checks all 48 films / 11 637 frames against
the Python aggregate FNV `8e1f0f6d9e5c88bd`, the encode/decode round-trip of
every stored tile, and the six indexes (entry counts, positive count/fps,
byte-exact `to_bytes`/`build_fdx`).  The reader keeps the whole file, so a
partial trailing tile (if one ever appears) is preserved.
