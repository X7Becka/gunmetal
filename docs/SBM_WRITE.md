# Запись SBM-архивов: round-trip, аудит и замена записи (`M11.5`, срез 9)

* **Статус:** срез готов. Round-trip **2/2** архивов побайтово (`.IDX` и `.DAT`),
  1827 записей, 46 525 219 Б. `--audit` — те же числа, 0 пустых записей,
  0 ошибок разбора. Замена одной записи сохраняет **178/178** и **1647/1647**
  остальных блобов побайтово, `gm/sbm.py` + `gm/bigfile.py` читают новую
  картинку (размер/пиксели/флаги), `gm.py list` видит ту же таблицу.
  Self-test **37/37**. Регрессия `regress.py --mirror --json` — **115/115**.
* **Ветка:** `feat/sbmwrite`, база `master` `85a12d4`, дата 2026-10-24.
* **Файлы:** `tools/editor/sbmwrite.py` (новый), `docs/SBM_WRITE.md` (этот
  документ). Существующие файлы (`gm/bigfile.py`, `gm/sbm.py`, `gm.py`,
  `gm/*`, `engine/src/res/sbm.*`, `tools/editor/*`, `regress.py`,
  `ROADMAP.md`, `docs/*.md`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sbmwrite`
  (в git не попадают).
* **Оракулы:** `gm/bigfile.py` (`BigFile` — читатель BIGFILE) и `gm/sbm.py`
  (`parse`, `encode`, `expected_size`); формат — `docs/FORMATS.md` §1 и §2,
  палитра — §6, C++-читатель — `engine/src/res/sbm.h`.

---

## 1. Раскладка и что пересчитывается

SBM-архив — это обычная пара BIGFILE (`docs/FORMATS.md` §1):

```
IDX:  +0  "BIGFILE" (7 Б), +7 0x1A, +8 u32 count,
      +12 count * { char name[8]; u32 offset }          // 12 Б на запись
DAT:  конкатенация блобов; size[i] = offset[i+1] - offset[i],
      последний — до EOF.
```

Блоб — `.sbm` (`docs/FORMATS.md` §2): 12-байтная шапка
(`u32 zero; u8 log2w; u8 log2h; u8 levels-1; u8 zero; u32 flags`) и
mip-цепочка; размер пиксельных данных равен
`sum(max(1,1<<(log2w-k)) * max(1,1<<(log2h-k)))`. Палитра в блобе **не
хранится** — она общая VGA-таблица игры (`docs/FORMATS.md` §6).

При замене пересчитываются только `u32 offset` — размеры в индексе не
хранятся, они выводятся из соседних смещений. `count`, имена, магия, байт
`0x1A` и порядок записей не меняются. Как и в `bigpack.py`
(`docs/BIGFILE_WRITE.md` §2), писатель сохраняет то, чего нет в модели
оракула:

| байты | где | как сохраняются |
|---|---|---|
| паддинг между блобами | внутри экстента предыдущего блоба | едет вместе с блобом |
| хвост `.IDX` после таблицы | `idx[12+count*12:]` | копируется в конец индекса |
| префикс `.DAT` до `offset[0]` | `dat[:offset[0]]` | копируется в начало `.DAT` |

В поставке оба SBM-архива без префикса, хвоста и паддинга: `12+count*12`
равно размеру `.IDX`, смещения непрерывны, последний блоб кончается на EOF.
Поэтому пересборка совпадает побайтово.

**Какие архивы считаются SBM.** Инструмент рекурсивно обходит каталог, берёт
каждую пару `*.idx` + `*.DAT/.dat` и отбирает:

1. архив с именем `SBM` (стем файла) — всегда, даже если внутри мусор:
   тогда `--roundtrip`/`--check` сообщат об ошибке, а не «пропустят» его;
2. любой другой архив, если **каждый** непустой блоб удовлетворяет строгой
   проверке раскладки: первые два `u32`-нулевых поля, `log2w/log2h ≤ 12`,
   `levels-1 ≤ 12` и `len(blob) == 12 + expected_size(...)`.

Второй критерий нужен, потому что `gm.sbm.parse` намеренно терпим: он не
смотрит на `zero` и допускает «хвост» после цепочки, поэтому заголовок
Windows BMP случайно декодируется как mip-цепочка 1×1. На поставке
`FRONTEND/BMP.IDX` именно так и «проходил» до ужесточения (self-test
`detect: false positive rejected`).

Найденные архивы установки (2026-10-24):

| индекс | записей | `.DAT` | полезная нагрузка |
|---|---:|---:|---|
| `PICS/SBM.IDX` | 1648 | 45 442 175 | текстуры, mip 1..9 |
| `sprites/SBM.IDX` | 179 | 1 083 044 | спрайты, 1 уровень, `flags` 256/257 |

Пропускаются с причиной: `MUSIC/SOU.IDX` и `sounds/SOU.IDX` (payload —
`SOU1`/`SOU2`, не `.sbm`), `FRONTEND/BMP.IDX` (Windows BMP), `Film/ammo.idx`
(не BIGFILE: текст `%ld, %ld`, пары `.DAT` нет). Других `*.idx` в установке
нет — итого **два** SBM-архива.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/sbmwrite.py --roundtrip
```

```
round-trip: /mnt/d/games/Gunmetal 3dfx
  skipped: FRONTEND/BMP.IDX: entry 0 ('BLINK0') does not look like .sbm (...)
  skipped: Film/ammo.idx: ... not a BIGFILE index: magic is b'499, 24', ...
  skipped: MUSIC/SOU.IDX: entry 0 ('DM1A01') does not look like .sbm (...)
  skipped: sounds/SOU.IDX: entry 0 ('ADON') does not look like .sbm (...)
  ok   PICS/SBM.IDX          1648 entries    45442175 B  idx  19788 B  identical=True/True  0.45s
  ok   sprites/SBM.IDX        179 entries     1083044 B  idx   2160 B  identical=True/True  0.03s
  totals: 2 archives, 1827 entries, 46525219 bytes, identical=2/2, 0.48s
```

| метрика | результат |
|---|---|
| SBM-архивов | 2 |
| **`.IDX` и `.DAT` побайтово после parse → to_bytes** | **2/2** |
| записей | 1827 (1648 + 179) |
| суммарно `.DAT` | 46 525 219 Б |
| `.sbm`-блобов, разобранных `gm/sbm.py` | 1827 / 1827 |
| `gm.bigfile.BigFile` на пересобранной паре | те же имена, тот же размер `.DAT`, тот же поток блобов — 2/2 |
| время | 0,45–0,52 с на `/mnt/d`, 0,28 с на зеркале |

`identical=N/M` в итоговой строке — число архивов, у которых **оба** файла
совпали. Расхождение печатается как `FAIL <архив>: <причина>`
(`.IDX differs` / `.DAT differs` / `gm.bigfile disagrees` / ошибка `.sbm`),
код возврата — 1. Ошибка разбора/структуры — код 2.

## 3. Числа корпуса (`--audit`)

```sh
python3 tools/editor/sbmwrite.py --audit
```

```
audit: /mnt/d/games/Gunmetal 3dfx
  archive              entries       bytes             min/median/max  empty names  flags / levels
  PICS/SBM.IDX            1648    45442175             33/10934/87393      0     8  0:1481, 1:126, 2:5, 16:36 / 1..9
  sprites/SBM.IDX          179     1083044              76/4108/32780      0     8  256:1, 257:178 / 1..1
  totals: 2 archives, 1827 entries, 46525219 bytes, 0 empty entries, name field 8 bytes
```

| величина | `PICS` | `sprites` |
|---|---:|---:|
| записей | 1648 | 179 |
| `.DAT`, Б | 45 442 175 | 1 083 044 |
| размер записи min / median / max, Б | 33 / 10 934 / 87 393 | 76 / 4 108 / 32 780 |
| пустых записей | 0 | 0 |
| имя: поле / максимум | 8 / 8 Б | 8 / 8 Б |
| `flags` (число записей) | 0:1481, 1:126, 2:5, 16:36 | 256:1, 257:178 |
| mip-уровни | 1..9 | 1 |
| `.sbm` extra-байтов после цепочки | 0 | 0 |

Числа сходятся с `docs/FORMATS.md` §1 (1648/179 записей, 45 442 175 / 1 083 044 Б),
§2 (`flags` 0/1/2/16 у текстур, 256/257 у спрайтов, 1 уровень у спрайтов) и
`docs/BIGFILE_WRITE.md` §4.1 (там те же два архива в общем прогоне пяти).
`names` — максимум длины имени в байтах; поле записи всегда 8 Б, имена
NUL-дополнены. Медиана — обычная (`statistics.median`), при чётном числе
записей может быть полуцелой; в примере обе целые.

## 4. CLI

```sh
python3 tools/editor/sbmwrite.py --roundtrip [DIR] [--tmp-dir DIR] [--json]
python3 tools/editor/sbmwrite.py --audit [DIR] [--json]
python3 tools/editor/sbmwrite.py --list ARCHIVE.IDX [--limit N] [--json]
python3 tools/editor/sbmwrite.py --check ARCHIVE.IDX [--dat FILE.DAT] \
        [--against ORIG.IDX [--name NAME] [--expect FILE]] [--json]
python3 tools/editor/sbmwrite.py --replace ARCHIVE.IDX --name NAME \
        --image FILE.bmp --out-dir DIR [--palette FILE.bmp] [--levels N] \
        [--flags N] [--filter box|nearest] [--force] [--json]
python3 tools/editor/sbmwrite.py --self-test [--json]
```

`--roundtrip`/`--audit` без `DIR` берут `--game` (по умолчанию
`/mnt/d/games/Gunmetal 3dfx`); `DIR` заменяет каталог. Путь вида
`PICS/SBM.IDX` в `--list`/`--check`/`--replace`/`--against` разрешается под
`--game`, если такого файла нет в текущем каталоге (как в `bigpack.py`).

`--list` печатает по умолчанию **первые 5 и последние 5** записей (`--limit N`
меняет число с каждого конца, `0` — все): номер, имя, смещение, размер,
размеры уровня 0, `flags`, число mip-уровней. `--limit` не дублирует записи на
коротких архивах.

`--check` — структура: магия, `0x1A`, длина таблицы, монотонность смещений,
`offset ≤ размер .DAT`, конец последней записи = размер `.DAT`, имена
(непустые, верхний регистр, дубликаты), разбор каждого блоба `gm/sbm.py`,
пересборка = файл, независимое чтение `gm.bigfile`. `--dat FILE.DAT` задаёт
данные явно. **Код возврата `--check` определяется структурой**, а не
равенством с `--against`: правка обязана проходить `--check`, но не обязана
быть равна оригиналу.

`--against ORIG.IDX` добавляет побайтовое сравнение обоих файлов (число
изменившихся байт, первое расхождение, `delta`) и построчную сверку записей:
какие записи изменились и сколько остальных совпало побайтово. `--name`
указывает целевую запись, `--expect FILE` требует, чтобы её блоб совпал с
файлом (сырой `.sbm`).

`--replace` пишет `DIR/SBM.IDX` + `DIR/SBM.DAT` (стем исходного архива,
верхний регистр, как в поставке). После записи инструмент сам проверяет:
число записей, имена и блобы всех прочих записей, целевую запись, разбор
нового блоба (`размеры`, `пиксели`, `flags`), чтение оракулом
(`gm.bigfile`), идемпотентность пересборки. Если проверка не прошла — код 1
и список `PROBLEM`. Существующие файлы не перезаписываются без `--force`.

Коды возврата: `0` успех, `1` проверка не прошла (`--roundtrip`/`--audit`/
`--check`/`--replace`/`--self-test`), `2` неверный ввод или отказ (сообщение
`sbmwrite: …` в stderr), `3` внутренняя ошибка. `--json` — одна строка JSON
в любом режиме.

## 5. Замена записи: доказательство

Все выходы — в `/tmp/opencode/sbmwrite`; игра только читается.

### 5.1. Спрайт, тот же размер (флаги сохраняются)

64×64 BMP собран из записи `AFLM0001` (палитра `GAMEPAL` движка), пиксели
сдвинуты на +7 по индексу:

```sh
$ python3 tools/editor/sbmwrite.py --replace sprites/SBM.IDX \
      --name AFLM0001 --image /tmp/opencode/sbmwrite/sprite_new.bmp \
      --out-dir /tmp/opencode/sbmwrite/sprites
replace: …/sprites/SBM.IDX -> /tmp/opencode/sbmwrite/sprites/SBM.IDX
  entry 0 AFLM0001: 4108 -> 4108 bytes (delta +0), 64x64, 1 level(s), flags 257
  NEW: …/sprites/SBM.IDX (2160 bytes)
       …/sprites/SBM.DAT (1083044 bytes)
  entries: 179 (was 179)
  others byte-identical: 178/178
  target identical: True  pixels match: True  oracle agrees: True  idempotent: True
```

* `cmp` по `.IDX` — **идентичен** исходному (размер записи не изменился,
  смещения те же);
* `cmp -l` по `.DAT` — **4096** различающихся байт, все в диапазоне **13..4108**
  (1-based), т.е. строго внутри первой записи; остальные 178 блобов не
  тронуты;
* независимая проверка `gm/bigfile.py` + `gm/sbm.py`: 179 записей,
  1 083 044 Б, `AFLM0001` — 64×64, `flags=257`, 4096 пикселей **совпадают**
  с ожидаемым массивом, 178 остальных блобов побайтово те же.

### 5.2. Текстура с mip-цепочкой

256×256 BMP (шаблон `x ^ y`) на место `57CHEV` (256×256, 9 уровней):

```sh
$ … --replace PICS/SBM.IDX --name 57CHEV \
      --image /tmp/opencode/sbmwrite/texture_new.bmp \
      --out-dir /tmp/opencode/sbmwrite/pics
  entry 0 57CHEV: 87393 -> 87393 bytes (delta +0), 256x256, 9 level(s), flags 0
  entries: 1648 (was 1648)
  others byte-identical: 1647/1647
  target identical: True  pixels match: True  oracle agrees: True  idempotent: True
```

* `.IDX` идентичен, `.DAT` — 87 026 изменённых байт в **13..87 393**
  (внутри первой записи);
* `gm/sbm.py`: уровень 0 — 65 536 пикселей, ровно шаблон; mip-размеры
  `65536, 16384, 4096, 1024, 256, 64, 16, 4, 1`; `flags=0`;
* mip-уровни сохранены автоматически, потому что размеры картинки совпали с
  исходной записью (иначе по умолчанию был бы 1 уровень — см. §7).

### 5.3. Рост записи (сдвиг смещений)

Спрайт 128×128 вместо 64×64:

```sh
$ … --replace sprites/SBM.IDX --name AFLM0001 \
      --image /tmp/opencode/sbmwrite/sprite_grow.bmp \
      --out-dir /tmp/opencode/sbmwrite/sprites-grow
  entry 0 AFLM0001: 4108 -> 16396 bytes (delta +12288), 128x128, 1 level(s), flags 257
  others byte-identical: 178/178
```

Разбор индекса вручную: заголовок совпадает, `count` 179, **все 179 имён**
совпадают, множество сдвигов смещений — `{0, +12288}` (запись 0 не двигается,
все последующие — на +12 288), `.DAT` 1 083 044 → 1 095 332 Б (ровно +12 288),
`.IDX` 2160 Б (та же длина). `--check --against` показывает `IDX: +0 bytes,
204 positional byte(s) differ` и `1 changed entry, 178/178 others identical`.

`gm.py list` на результатах видит ту же таблицу:

```
$ python3 gm.py list /tmp/opencode/sbmwrite/sprites/SBM.IDX | head -3
/tmp/opencode/sbmwrite/sprites/SBM.IDX: 179 entries, 1083044 bytes of data
  AFLM0001     off=0          size=4108
  AFLM0002     off=4108       size=4108
```

## 6. Self-test и регрессия

```sh
python3 tools/editor/sbmwrite.py --self-test      # 37/37, без файлов игры
python3 regress.py --mirror --json                # 115/115, 61.4 с
```

Self-test **37/37** (временный каталог `/var/tmp`, после прогона удаляется):
round-trip синтетического архива, префикс `.DAT`, хвост `.IDX`, паддинг между
блобами, пустой архив, архив от `gm.bigfile.pack`, `extra`-байты `.sbm`,
детерминизм; обнаружение по имени и по содержимому, ложное срабатывание на
BMP-подобном payload; семь структурных отказов (магия, короткая шапка,
короткая таблица, `count=5e6`, смещения назад, смещение за EOF, нет `.DAT`);
замена — рост, усадка, тот же размер (индекс побайтово тот же), сохранение и
переопределение `flags`, mip-уровни по источнику, точные пиксели, неизвестное
имя, картинка не-степень-двойки, защита вывода (игра/зеркало/оба корня
репозитория), `--force`; `--check` (чистый, подделанное смещение через
`--against`, обрезанный `.DAT`, `--against`, `--expect`); `--list`;
`--audit`; JSON во всех режимах.

Регрессия:

```
{"passed": 115, "total": 115, "seconds": 61.4, "full": false,
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

`gm/bigfile.py`, `gm/sbm.py`, `gm.py` и C++ не менялись; новый инструмент —
отдельный файл, поэтому категории `archives/*`, `repack/*`, `inventory`,
`trust/*` остались зелёными.

## 7. Ограничения

* **Не создаёт и не удаляет записи.** Нет `--add`/`--delete`: `count`,
  порядок и имена фиксированы. Замена — только существующего имени (первое
  совпадение, как ищет игра: `0x40d6b3`).
* **Не сжимает.** Блобы хранятся как есть; новая картинка кодируется
  `gm.sbm.encode` (`mip` = `box`/`nearest`), без RLE/LZ и без ресайза:
  ширина и высота должны быть степенями двойки, число пикселей — ровно
  `width*height`.
* **Палитра не хранится в `.sbm`.** Для новой картинки порядок выбора:
  `--palette BMP` → палитра движка (`FRONTEND/GAMEPAL` из
  `FRONTEND/BMP.IDX`, как `gm.py sbm`) → палитра самой картинки. При
  совпадении палитры индексы сохраняются точно; иначе — ближайший цвет
  (дубликаты в общей палитре делают наивный nearest-match неоднозначным).
* **Mip-уровни по умолчанию:** если размеры новой картинки совпадают с
  исходной записью — сохраняется её число уровней; иначе 1. `--levels N`
  задаёт явно (зажимается до `bit_length` стороны). `--flags` по умолчанию
  берётся из исходной записи (важно для спрайтов: 256/257), `--flags N`
  переопределяет.
* **Структурная проверка не ловит порчу payload.** `.DAT` пересобирается из
  тех же блобов, а смещения пересчитываются в те же накопленные значения,
  поэтому ни побитовая порча пикселей, ни монотонный сдвиг границ не видны
  без `--against` (он показывает изменившиеся файлы, байты и записи).
  Обрезанный `.DAT` и немонотонные/выходящие за EOF смещения ловятся.
* **4 ГиБ не проверялись:** смещения `u32`; писатель отказывает при
  `offset > 0xFFFFFFFF` и размере `.DAT` больше 4 ГиБ.
* **В игре архив не запускался** (установка только читается). Доказано:
  результат открывается оракулом `gm.bigfile`, `gm/sbm` декодирует новую
  картинку, `gm.py list` видит ту же таблицу; оригинальный читатель берёт
  `offset` из того же поля `+8` (`0x40d819`) и ключ из 8 байт (`0x40d6b3`) —
  см. `docs/BIGFILE_WRITE.md` §1.
* **C++ не менялся и к CTest не подключён:** новые файлы — только
  `tools/editor/sbmwrite.py` и этот документ; `CMakeLists.txt`, `main.cpp`,
  `docs/INDEX.md` — вне разрешённого списка.
* **Имена-дубликаты не отвергаются:** в поставке их нет; вторая запись
  недостижима для игры, `--check` выдаёт предупреждение.

## 8. Ссылки

`docs/FORMATS.md` §1 (BIGFILE: шапка, записи, смещения), §2 (`.sbm`:
шапка, mip-цепочка, `flags`), §6 (общая палитра, `.bmp`);
`docs/BIGFILE_WRITE.md` §1–§2 (что пересчитывается, сохранение
префикса/хвоста/паддинга), §4 (числа архивов); `docs/SOG_WRITE.md`
(структура отчёта и стиль CLI); `gm/bigfile.py` (`BigFile`, `pack`);
`gm/sbm.py` (`parse`, `encode`, `expected_size`, `to_bytes`);
`gm/bmp.py` (§6: 8-битный BMP, палитра BGRX→RGBX); `gm/palette.py`
(`installed` — `FRONTEND/GAMEPAL`); `engine/src/res/sbm.h` (C++-читатель);
`ROADMAP.md` `M11.5`.
