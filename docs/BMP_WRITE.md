# Запись архива картинок `FRONTEND/BMP.IDX` (`M11.5`, срез 10)

* **Статус:** срез готов. Round-trip **1/1** архива побайтово (`.IDX` и `.DAT`),
  143 записи, 2 845 582 Б. `--audit` — те же числа: 143/143 payload'ов валидны
  по заголовку BMP, `gm/bmp.py` декодирует 142/143 (одна запись — 4-битная
  `ICON2`), 41 уникальный размер, 0 пустых записей, 0 «хвостовых» байт.
  Замена записи тем же файлом — **0 изменённых байт**, `.IDX`/`.DAT` совпадают
  побайтово (md5 совпал). Замена записью другого размера — **142/142** прочих
  записей байт в байт, `--check --against` показывает **ровно 1** изменённую
  запись. Self-test **40/40**. Регрессия `regress.py --mirror --json` —
  **115/115**, 63.1 с (холодный прогон 79.7 с).
* **Ветка:** `feat/bmpwrite`, база `master` `c1889ba`, дата 2026-10-24.
* **Файлы:** `tools/editor/bmpwrite.py` (новый), `docs/BMP_WRITE.md` (этот
  документ). Существующие файлы (`gm/*`, `gm.py`, `tools/editor/*`,
  `engine/*`, `regress.py`, `ROADMAP.md`, `docs/*.md`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/bmpwrite`
  (в git не попадают).
* **Оракулы:** `gm/bigfile.py` (`BigFile` — читатель BIGFILE) и `gm/bmp.py`
  (`parse` — 8-битный BMP); формат — `docs/FORMATS.md` §1 и §6, назначение
  записей — `docs/UI_SCREENS.md` §9, C++-читатели — `engine/src/res/bigfile.*`
  и `engine/src/res/bmp.*`.

---

## 1. Раскладка и что пересчитывается

`FRONTEND/BMP.IDX` — обычная пара BIGFILE (`docs/FORMATS.md` §1):

```
IDX:  +0  "BIGFILE" (7 Б), +7 0x1A, +8 u32 count,
      +12 count * { char name[8]; u32 offset }          // 12 Б на запись
DAT:  конкатенация блобов; size[i] = offset[i+1] - offset[i],
      последний — до EOF.
```

Блоб — Windows BMP (`docs/FORMATS.md` §6). В поставке это классический
`BITMAPFILEHEADER` (14 Б) + `BITMAPINFOHEADER` (40 Б) + палитра + пиксели:

```
+0   char  bfType[2]      "BM"
+2   u32   bfSize         размер файла (совпадает с длиной блоба: 143/143)
+6   u16   bfReserved1    (0)
+8   u16   bfReserved2    (0)
+10  u32   bfOffBits      смещение пикселей
+14  u32   biSize         40
+18  i32   biWidth        > 0
+22  i32   biHeight       отрицательный = top-down (в поставке нет: 0/143)
+26  u16   biPlanes       1 (143/143)
+28  u16   biBitCount     8 у 142 записей, 4 у одной (`ICON2`)
+30  u32   biCompression  0 = BI_RGB (143/143)
+34  u32   biSizeImage    0 у 71 записи, иначе stride*abs(height)
+38  i32   biXPelsPerMeter
+42  i32   biYPelsPerMeter
+46  u32   biClrUsed      0 (=1<<bpp) у 72, 256 у 70, 16 у одной
+50  u32   biClrImportant
палитра: (biClrUsed or 1<<bpp) * 4 Б, порядок на диске B,G,R,X
пиксели: строка ((width*bpp + 31)/32)*4 Б, снизу вверх (biHeight > 0)
```

Заголовок проверяется полностью до записи: magic, размеры DIB, `planes`,
`bpp`, `compression`, границы палитры, `bfOffBits`, длина пиксельных данных.
Поставка сходится по всем полям: `bfSize` = длина блоба, `bfOffBits` =
`14 + 40 + ncol*4`, пиксельные данные кончаются ровно на конце блоба
(`trailing = 0` у 143/143).

При замене пересчитываются только `u32 offset` — размеры в индексе не
хранятся, они выводятся из соседних смещений. `count`, имена, магия, байт
`0x1A` и порядок записей не меняются. Как и в `sbmwrite.py`
(`docs/SBM_WRITE.md` §1) и `bigpack.py` (`docs/BIGFILE_WRITE.md` §2), писатель
сохраняет то, чего нет в модели оракула:

| байты | где | как сохраняются |
|---|---|---|
| паддинг между блобами | внутри экстента предыдущего блоба | едет вместе с блобом |
| хвост `.IDX` после таблицы | `idx[12+count*12:]` | копируется в конец индекса |
| префикс `.DAT` до `offset[0]` | `dat[:offset[0]]` | копируется в начало `.DAT` |

В поставке архив без префикса, хвоста и паддинга: `12 + 143*12 = 1728` равно
размеру `.IDX`, смещения непрерывны, последний блоб кончается на EOF. Поэтому
пересборка совпадает побайтово.

**Какие архивы считаются BMP.** Инструмент рекурсивно обходит каталог, берёт
каждую пару `*.idx` + `*.DAT/.dat` и отбирает:

1. архив со стемом `BMP` — всегда (если внутри мусор, `--audit`/`--check`
   сообщат об этом, а не «пропустят»);
2. любой другой архив, если **каждый** непустой блоб проходит проверку
   заголовка BMP из §1.

Найденные архивы установки (2026-10-24):

| индекс | записей | `.DAT` | полезная нагрузка |
|---|---:|---:|---|
| `FRONTEND/BMP.IDX` | 143 | 2 845 582 | 142 × 8-bit BI_RGB + 1 × 4-bit (`ICON2`) |

Пропускаются с причиной: `PICS/SBM.IDX`, `sprites/SBM.IDX` (payload — `.sbm`,
магия `00 00`), `sounds/SOU.IDX`, `MUSIC/SOU.IDX` (`SOU1`/`SOU2`, магия `SO`),
`Film/ammo.idx` (не BIGFILE: текст `499, 24`). Других `*.idx` в установке нет —
итого **один** BMP-архив.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/bmpwrite.py --roundtrip
```

```
round-trip: /mnt/d/games/Gunmetal 3dfx
  skipped: Film/ammo.idx: ... not a BIGFILE index: magic is b'499, 24' ...
  skipped: MUSIC/SOU.IDX: entry 0 ('DM1A01') does not look like a BMP: magic is b'SO' ...
  skipped: PICS/SBM.IDX: entry 0 ('57CHEV') does not look like a BMP: magic is b'\x00\x00' ...
  skipped: sounds/SOU.IDX: entry 0 ('ADON') does not look like a BMP: magic is b'SO' ...
  skipped: sprites/SBM.IDX: entry 0 ('AFLM0001') does not look like a BMP: magic is b'\x00\x00' ...
  ok   FRONTEND/BMP.IDX       143 entries   2845582 B  idx   1728 B  identical=True/True  0.034s
  totals: 1 archive(s), 143 entries, 2845582 bytes, identical=1/1, 0.034s
```

| метрика | результат |
|---|---|
| BMP-архивов | 1 |
| **`.IDX` и `.DAT` побайтово после parse → to_bytes** | **1/1** |
| записей | 143 |
| суммарно `.DAT` | 2 845 582 Б |
| `.IDX` | 1 728 Б (`12 + 143*12`) |
| `gm.bigfile.BigFile` на пересобранной паре | те же имена, тот же размер `.DAT`, тот же поток блобов |
| время | 0,034 с на `/mnt/d`, 0,005 с на зеркале |

`identical=N/M` в итоговой строке — число архивов, у которых **оба** файла
совпали; расхождение печатается как `FAIL <архив>: <причина>`
(`.IDX differs` / `.DAT differs` / `gm.bigfile disagrees`), код возврата — 1.
Ошибка разбора/структуры — код 2.

## 3. Числа корпуса (`--audit`)

```sh
python3 tools/editor/bmpwrite.py --audit
```

```
audit: /mnt/d/games/Gunmetal 3dfx
  archive              entries     bytes         min/median/max empty name            bpp dims     bmp  gm.bmp
  FRONTEND/BMP.IDX         143   2845582        630/1654/308278     0    8     4:1, 8:142   41  143/143  142/143
       bpp 8:142, 24:0, 32:0, 1:0, 4:1, 16:0; RLE 0
       compression 0:143; biClrUsed 0:72, 16:1, 256:70; top-down 0; biSizeImage 0 in 71; trailing 0 B; bfSize mismatch 0
       gm.bmp cannot decode ICON2 (630 B): unsupported bpp 4
  totals: 1 archive(s), 143 entries, 2845582 bytes, 0 empty entries, header-valid BMP 143/143, gm.bmp decodes 142/143, 41 unique sizes
```

| величина | значение |
|---|---|
| записей | 143 |
| `.DAT`, Б | 2 845 582 |
| размер записи min / median / max, Б | 630 / 1 654 / 308 278 |
| пустых записей | 0 |
| имя: поле / максимум | 8 / 8 Б (дубликатов нет) |
| `bpp` (число записей) | 8:142, 4:1; 1/16/24/32 bpp — 0 |
| `biCompression` | 0 (BI_RGB): 143; RLE (`BI_RLE8`/`BI_RLE4`) — **0** |
| `biClrUsed` | 0:72, 256:70, 16:1 |
| top-down (`biHeight < 0`) | 0 |
| `biSizeImage` = 0 | 71 (остальные равны `stride*height`) |
| «хвост» после пикселей | 0 Б во всех 143 |
| уникальных размеров (w×h) | 41 |
| заголовок BMP валиден | 143/143 |
| декодирует `gm/bmp.py` | 142/143 (`ICON2`: 4 bpp, `unsupported bpp 4`) |

Самые частые размеры: `24x24` (57 записей — иконки `WCON*/WICON*` и `EQUIP*`),
`320x200` (14 — фоны SML/загрузки), `80x125` (10 — `BRAIN01..10`), `112x25`
(7 — арт пунктов меню), `64x64` (7 — `ZOOMA..G`), `8x8` (4 — `KNOB`,
`NOCHECK`, `WATERPAL`, `GAMEPAL`). Документ `docs/UI_SCREENS.md` §9 называет
эти же записи и размеры: `f_m_titl` 232×52, `scroll` 404×200, `plate` 196×131,
`fonts1..4` 512×168 / 1024×80 / 520×56 / 512×168, `numfonts` 180×48 и т.д.

**Про «flags».** У BMP нет поля флагов (в отличие от `.sbm`, где `flags`
значим — `docs/FORMATS.md` §2). Ближайшие аналоги — `biCompression` (у всех
143 = 0) и `biClrUsed`; они и печатаются в `--audit` вместо `flags`.

## 4. CLI

```sh
python3 tools/editor/bmpwrite.py --roundtrip [DIR] [--tmp-dir DIR] [--json]
python3 tools/editor/bmpwrite.py --audit [DIR] [--json]
python3 tools/editor/bmpwrite.py --list [ARCHIVE.IDX] [--limit N] [--json]
python3 tools/editor/bmpwrite.py --extract NAME [--archive IDX] --out DIR \
        [--force] [--json]
python3 tools/editor/bmpwrite.py --replace NAME [--archive IDX] \
        --image FILE.bmp --out-dir DIR [--force] [--json]
python3 tools/editor/bmpwrite.py --check ARCHIVE.IDX [--dat FILE.DAT] \
        [--against DIR|IDX] [--json]
python3 tools/editor/bmpwrite.py --self-test [--json]
```

`--roundtrip`/`--audit` без `DIR` берут `--game` (по умолчанию
`/mnt/d/games/Gunmetal 3dfx`). `--extract`/`--replace`/`--list` без архива
берут `FRONTEND/BMP.IDX` под `--game`; `--archive` переопределяет. Путь вида
`FRONTEND/BMP.IDX` в `--check`/`--against` разрешается под `--game`, если такого
файла нет в текущем каталоге.

`--list` печатает по умолчанию **первые 5 и последние 5** записей
(`--limit N` меняет число с каждого конца, `0` — все): номер, имя, смещение,
размер, размеры, `bpp`, `biCompression`.

`--extract NAME --out DIR` пишет `DIR/<NAME>.bmp` **байт в байт** (без
перекодирования и без палитры-посредника) и проверяет, что записанный файл
совпал с блобом. Имя ищется без учёта регистра (первое совпадение, как в игре).
Существующий файл не перезаписывается без `--force`.

`--replace NAME --image FILE.bmp --out-dir DIR` пишет `DIR/BMP.IDX` +
`DIR/BMP.DAT` (стем исходного архива, верхний регистр, как в поставке).
Картинка **не кодируется заново**: её байты становятся payload'ом, поэтому файл
обязан быть BMP (проверяется заголовок из §1). После записи инструмент сам
проверяет число записей, имена и блобы всех прочих записей, целевую запись,
чтение оракулом `gm.bigfile`, идемпотентность пересборки, и печатает
`changed bytes vs source` / `files identical to source`. Существующие файлы не
перезаписываются без `--force`.

`--check IDX` — структура: магия, `0x1A`, длина таблицы, монотонность и
границы смещений (нарушения — отказ, код 2), покрытие `.DAT` без остатка,
непустые имена, дубликаты, регистр, валидность заголовка каждого BMP (для
BMP-архива это проблема, иначе предупреждение), отсутствие «хвоста» после
пикселей, пересборка = файл, независимое чтение `gm.bigfile`. `--dat` задаёт
данные явно.

`--against DIR` (или `.IDX`) добавляет побайтовое сравнение обоих файлов
(число изменившихся байт, первое расхождение, `delta`) и построчную сверку
записей: какие записи изменились и сколько остальных совпало побайтово. Код
возврата `--check` определяется структурой, а не равенством с `--against`.

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод или отказ
(сообщение `bmpwrite: …` в stderr), `3` внутренняя ошибка. `--json` — одна
строка JSON в любом режиме.

## 5. Извлечение и замена: доказательство

Все выходы — в `/tmp/opencode/bmpwrite`; игра только читается.

### 5.1. Extract + replace тем же файлом = 0 изменённых байт

```sh
$ python3 tools/editor/bmpwrite.py --extract PLATE --out /tmp/opencode/bmpwrite/ex
extract: …/FRONTEND/BMP.IDX -> /tmp/opencode/bmpwrite/ex/PLATE.bmp
  entry 72 PLATE: 26754 bytes, 196x131, bpp 8, BI_RGB
  bytes written: 26754 (verbatim, no re-encoding)

$ python3 tools/editor/bmpwrite.py --replace PLATE \
      --image /tmp/opencode/bmpwrite/ex/PLATE.bmp --out-dir /tmp/opencode/bmpwrite/same
replace: …/FRONTEND/BMP.IDX -> /tmp/opencode/bmpwrite/same/BMP.IDX
  entry 72 PLATE: 26754 -> 26754 bytes (delta +0), 196x131, bpp 8, BI_RGB
  NEW: …/same/BMP.IDX (1728 bytes)
       …/same/BMP.DAT (2845582 bytes)
  entries: 143 (was 143)
  others byte-identical: 142/142
  target identical: True  oracle agrees: True  idempotent: True
  changed bytes vs source: IDX 0 (+0), DAT 0 (+0)
  files identical to source: IDX True, DAT True
```

`cmp` подтверждает: `.IDX` и `.DAT` **идентичны** исходным; md5 совпадают
(`8d615046d44ec4d23b74b3aa256324f5` для `.IDX` и
`0ed07eac643c4d5bf2acf62068373f7d` для `.DAT`). `--check --against` на этой
паре: `idx identical=True, DAT identical=True, other entries identical:
143/143`.

### 5.2. Замена записью другого размера

В слот `PLATE` (196×131, 26 754 Б) положен payload записи `ICON2` (32×32,
4 bpp, 630 Б):

```sh
$ … --extract ICON2 --out /tmp/opencode/bmpwrite/ex
$ … --replace PLATE --image /tmp/opencode/bmpwrite/ex/ICON2.bmp \
      --out-dir /tmp/opencode/bmpwrite/other
  entry 72 PLATE: 26754 -> 630 bytes (delta -26124), 32x32, bpp 4, BI_RGB
  entries: 143 (was 143)
  others byte-identical: 142/142
  target identical: True  oracle agrees: True  idempotent: True
  changed bytes vs source: IDX 169 (+0), DAT 534270 (-26124)
```

Проверка независимо:

* `--check --against "/mnt/d/games/Gunmetal 3dfx/FRONTEND"`:
  `idx identical=False, DAT identical=False`, `DAT: -26124 bytes, 534270
  positional byte(s) differ, first at 2239222`, `entry 72 PLATE changed:
  26754 -> 630 bytes`, `other entries identical: 142/142`, `result: ok`;
* `.IDX` той же длины (1728 Б): 169 различающихся байт — это поля `offset`
  записей 73..142, сдвинутые на −26 124 (первое расхождение — 0-based 896,
  т.е. `offset` записи 73; `offset` самой записи 72 не меняется);
* первое расхождение `.DAT` — байт 2 239 222 (0-based), то есть `+2` внутри
  блоба `PLATE` (поле `bfSize` нового файла);
- `gm/bigfile.py`: 143 записи, 2 819 458 Б, имена совпадают, изменён ровно
  один payload (`PLATE`), его sha256 совпадает с payload'ом `ICON2`
  исходного архива; `gm.py list` видит ту же таблицу.

Разбор индекса вручную: заголовок `BIGFILE\x1a`, `count = 143`, все 143 имени
совпадают, множество сдвигов `offset` — `{0, −26124}`.

### 5.3. Та же длина, другое содержимое: пиксельная проверка через C++

`HELPSCRN` (320×200, 65 078 Б) заменён на `DEMOTION` (320×200, тот же размер):

```sh
$ … --replace HELPSCRN --image /tmp/opencode/bmpwrite/ex/DEMOTION.bmp \
      --out-dir /tmp/opencode/bmpwrite/cppbg2
  entry 46 HELPSCRN: 65078 -> 65078 bytes (delta +0), 320x200, bpp 8, BI_RGB
  others byte-identical: 142/142
  changed bytes vs source: IDX 0 (+0), DAT 63726 (+0)
  files identical to source: IDX True, DAT False
```

`.IDX` побайтово тот же (смещения не изменились), `.DAT` — 63 726
различающихся байт (пиксели). Движок C++ (`engine/src/res/bigfile.cpp` +
`res/bmp.cpp`) читает пересобранную пару: `--menu --png` на копии установки
даёт `missing=0`, а `--pager SML/credits.sml --png --background` рисует
`HELPSCRN` как фон (`background=64000`) и выдаёт **другой** PNG:

| сборка | md5 PNG |
|---|---|
| исходный `HELPSCRN` | `1cd6f160238defebccf95977ef2fc688` |
| `HELPSCRN` ← `DEMOTION` | `ddd82db318edfe0748c00b3b650de8c6` |

Дополнительно C++-путь проверен двумя способами: `--dump` на извлечённом
`PLATE.bmp` печатает `kind: BMP image, dimensions: 196x131, bpp: 8, palette
entries: 256` (ровно то, что даёт `gm/bmp.py`), а попытка подставить 8×8
`KNOB` в `HELPSCRN` завершается сообщением движка
`pager_render: background "HELPSCRN" is 8x8, expected 320x200` — то есть C++
прочитал именно пересобранную пару. Как и `gm/bmp.py`, C++ `parse_bmp`
отказывает `ICON2` (4 bpp) — обе реализации сходятся на 142/143.

## 6. Self-test и регрессия

```sh
python3 tools/editor/bmpwrite.py --self-test      # 40/40, без файлов игры
python3 regress.py --mirror --json                # 115/115, 63.1 с
```

Self-test **40/40** (временный каталог `/var/tmp`, после прогона удаляется):

* round-trip синтетического архива (8-bit, 4-bit, top-down), префикс `.DAT`,
  хвост `.IDX`, паддинг между блобами, пустой архив, архив от
  `gm.bigfile.pack`, детерминизм;
* обнаружение по имени и по содержимому, пропуск не-BMP архива с причиной,
  пропуск не-BIGFILE `*.idx`;
* семь структурных отказов: магия, короткая шапка, короткая таблица,
  `count=5e6`, смещения назад, смещение за EOF, нет `.DAT`;
* модель заголовка BMP: 8/4-bit и top-down, обрезанные пиксели, `bpp=7`;
* extract: байт в байт, поиск без учёта регистра, неизвестное имя, защита
  вывода (игра/зеркало/оба корня репозитория), `--force`;
* replace: тот же payload → 0 изменённых байт и оба файла идентичны; рост;
  усадка; точные пиксели; неизвестное имя; не-BMP картинка; `--force`;
* `--check`: чистая пара, `--against` с ровно одной изменённой записью,
  обрезанный `.DAT`, «хвост» после пикселей, подделанное смещение (ломает
  экстенты BMP и видно в `--against`), переименованная запись (структура
  цела, `--against` называет её);
* `--list`, `--audit` (min/median/max, `bpp`, счётчики декодера, не-BMP
  payload в архиве с именем `BMP`), `--json` во всех режимах.

Регрессия:

```
{"passed": 115, "total": 115, "seconds": 63.1, "full": false,
 "game": "/home/nooverload/.cache/gunmetal-re/game", "failed": [], "cached": 0}
```

`gm/*`, `gm.py`, C++ и `regress.py` не менялись; новый инструмент — отдельный
файл, поэтому проверки `archives/frontend`, `archives/*`, `repack/*`,
`inventory`, `trust/*` остались зелёными.

## 7. Ограничения

* **Не создаёт и не удаляет записи.** Нет `--add`/`--delete`: `count`, порядок
  и имена фиксированы. Замена — только существующего имени (первое
  совпадение, как ищет игра).
* **Не кодирует BMP.** Payload новой записи — это ровно байты `--image`.
  Поэтому нельзя дать PNG/JPEG, нельзя сменить палитру, `bpp` или сжать
  пиксели: новый файл должен быть готовым BMP. Палитра хранится в самом BMP,
  общей VGA-таблицы здесь нет (в отличие от `.sbm`, `docs/FORMATS.md` §2/§6).
* **Декодера нет.** Инструмент проверяет заголовок и длину пиксельных данных,
  но не разворачивает 1/4/16/24/32 bpp и RLE; `gm/bmp.py` декодирует только
  8-битный BI_RGB. В поставке 4/16/24/32 bpp и RLE **отсутствуют** (0
  записей), поэтому эти ветки заголовка на реальных данных не проверены —
  только self-test'ом.
* **Структурная проверка не ловит порчу пикселей.** Изменённый байт внутри
  пиксельного массива не меняет заголовок, а `.DAT` пересобирается из тех же
  блобов, поэтому такую порчу видит только `--against`. Обрезанный блоб,
  «хвост» после пикселей, немонотонные/выходящие за EOF смещения ловятся.
* **4 ГиБ не проверялись:** смещения `u32`; писатель отказывает при
  `offset > 0xFFFFFFFF` и размере `.DAT` больше 4 ГиБ.
* **В игре архив не запускался** (установка только читается). Доказано:
  результат открывается оракулами `gm.bigfile` + `gm.bmp`, `gm.py list` видит
  ту же таблицу, а C++-движок читает пересобранную пару из копии установки
  (`--menu`, `--pager --background`, `--dump`).
* **C++ не менялся и к CTest не подключён:** новые файлы — только
  `tools/editor/bmpwrite.py` и этот документ.
* **Имена-дубликаты не отвергаются:** в поставке их нет; вторая запись
  недостижима для игры, `--check` выдаёт предупреждение.
* **`--check` строг к BMP.** Если в архиве с именем `BMP` (или с хотя бы одним
  BMP-payload'ом) встретится не-BMP блоб, это проблема, а не предупреждение.

## 8. Ссылки

`docs/FORMATS.md` §1 (BIGFILE: шапка, записи, смещения), §6 (`.bmp`, общая
палитра); `docs/UI_SCREENS.md` §9 (что фронтенд грузит из `FRONTEND/BMP.IDX`);
`docs/SBM_WRITE.md` §1–§2 (структура отчёта, сохранение префикса/хвоста/
паддинга, тот же класс задачи); `docs/BIGFILE_WRITE.md` §2 (что
пересчитывается при замене); `gm/bigfile.py` (`BigFile`, `pack`);
`gm/bmp.py` (`parse`); `gm.py list`; `engine/src/res/bigfile.h`,
`engine/src/res/bmp.h` (C++-читатели); `ROADMAP.md` `M11.5`.
