# BIGFILE_WRITE — запись архивов `.IDX`/`.DAT` (`M11.6`)

Статус: рабочий документ, 2026-10-23. Инструмент:
`tools/editor/bigpack.py`. Читатель-оракул — `gm/bigfile.py` (не изменялся).

Документ отвечает на три вопроса: **что** пересчитывается при замене записи,
**почему** пересборка побайтово точна, и **чем** это проверено. Факты формата —
`docs/FORMATS.md` §1 и `gm/bigfile.py`; здесь только то, что нужно писателю,
с адресами кода, где поведение проверено.

## 1. Формат и его границы

```
IDX:  +0  char  magic[7]   "BIGFILE"
      +7  u8      0x1A
      +8  u32     count
      +12 count * { char name[8]; u32 offset }      // 12 байт на запись
      ...  хвост после таблицы (в поставке отсутствует)

DAT:  сырая конкатенация блобов.
      size[i] = offset[i+1] - offset[i], последний — до EOF.
```

| правило | источник |
|---|---|
| заголовок 12 байт, `count` по `+8`, таблица `12 + count*12` | `gm/bigfile.py:72-79`; `docs/FORMATS.md` §1 |
| запись 12 байт: `name[8]` (NUL-дополнение) + `u32 offset` | `gm/bigfile.py:82-86`; `docs/FORMATS.md` §1 |
| размер = следующий offset − текущий, последний до EOF | `gm/bigfile.py:102-103` |
| порядок записей сохраняется, новая — в конец | `gm/bigfile.py:262-274` |
| смещения `u32`, предел 4 ГиБ | `gm/bigfile.py:206-207` |
| `MAX_ENTRIES = 1e6` — защита от мусорного `count` | `gm/bigfile.py:32`, `:75-76` |

**Оригинал** (проверено по листингу `~/.cache/gunmetal-re/gundx.asm`, Ghidra не
нужна):

* заголовок читается одним `fread(buf=0x4d29c8, size=0xc, count=1)` —
  `0x40d4db`; `count` лежит по `+8` (`0x4d29d0`) и используется при расчёте
  порции (`0x40d513`, `0x40d60f`);
* таблица читается порциями по `0x400` записей: `fseek(stream, 0xc + chunk*0x3000)`
  (`0x40d4fd`…`0x40d508`), затем `fread(0x400*12 байт)` в `0x4cf9c8`
  (`0x40d531`…`0x40d540`). `0x3000 = 0x400*12` — то есть запись ровно 12 байт;
* поиск имени: сравнение двух dword (8 байт), шаг `0xc` (`0x40d6b3`), поле
  `offset` — по `+8` (`0x4cf9d0`), из него `fseek` перед чтением блоба
  (`0x40d80e`…`0x40d822`);
* **ключ — 8 байт, NUL-дополнение**: буфер обнуляется (`0x40d1b2`,
  `0x40d1b8`), затем копируется имя; подробнее — `docs/SOUND_NAMES.md` §3.1–3.2;
* **регистр**: верхний регистр поднимается у *запроса* (`0x40d1e6` →
  `0x480180`), а таблица сравнивается сырыми байтами (`0x40d6b3`). Значит
  запись в нижнем регистре игра не найдёт никогда; писатель хранит имена как
  есть, но `--add` требует `[A-Z0-9_+.-']` (обход — `--allow-any-name`);
* **дубликаты**: поиск идёт с записи 0 и берёт первое совпадение
  (`0x40d6b3`…`0x40d6c6`), поэтому дубль имени делает вторую запись
  недостижимой;
* **магия не проверяется**: у буфера заголовка `0x4d29c8` всего четыре ссылки
  (`0x40d4e0` — сам `fread`, `0x40d2a8`, `0x40d513`, `0x40d60f` — чтение
  `count` по `+8`), сравнения `"BIGFILE"` в коде нет. Писатель всё равно
  пишет магию: её требуют `gm/bigfile.py:72` и `engine/src/res/bigfile.cpp`
  (`BigFile::open`), иначе архив не прочитает наш собственный тулкит;
* путь к паре собирается строками `"%s\\%s.idx"` / `"%s\\%s.dat"`
  (`0x4a7670` / `0x4a76b4`), т.е. данные лежат рядом с индексом и называются
  так же, только `.dat` (в поставке — верхний регистр).

## 2. Что пересчитывается при замене

Только поля `offset` (и `count`, если запись добавляется): размеры в индексе
не хранятся, они выводятся из соседних смещений. Поэтому:

* замена **того же размера** не меняет `.IDX` ни на байт (проверено `cmp`);
* рост/усадка на `d` байт сдвигает все последующие `offset` на `d` и меняет
  размер `.DAT` на `d`;
* имена, магия, байт `0x1A`, порядок записей и любые «лишние» байты остаются
  как были.

Что писатель **сохраняет**, хотя оракул этого не моделирует:

| байты | где | как сохраняются |
|---|---|---|
| паддинг между блобами | внутри экстента предыдущего блоба (`size = next-off`) | входит в блоб, переносится целиком |
| хвост `.IDX` после таблицы | `idx[12+count*12:]` | копируется в конец нового индекса |
| префикс `.DAT` до `offset[0]` | `dat[:offset[0]]` | копируется в начало нового `.DAT` |

В поставке все пять архивов без префикса, без хвоста и без «пустых» записей
(`count*12+12 == размер .IDX`; смещения непрерывны, последний блоб кончается
ровно на EOF).

## 3. Инструмент

```
python3 tools/editor/bigpack.py --roundtrip [--game DIR] [--tmp-dir /tmp]
python3 tools/editor/bigpack.py --list PICS/SBM.IDX     # имя разрешается под --game

python3 tools/editor/bigpack.py --list ARCHIVE.IDX [--limit N]
python3 tools/editor/bigpack.py --replace ARCHIVE.IDX --name NAME \
        --file NEW.BIN --out PREFIX [--add] [--force]
python3 tools/editor/bigpack.py --check PREFIX.IDX [--against SRC.IDX \
        --name NAME --expect NEW.BIN]
python3 tools/editor/bigpack.py --self-test [--json]
```

* `--out PREFIX` даёт `PREFIX.IDX` + `PREFIX.DAT` (верхний регистр, как в
  поставке). Запись в каталог установки запрещена (обход —
  `BIGPACK_ALLOW_GAME_WRITE=1`, только для копии-зеркала), существующие файлы
  не перезаписываются без `--force`.
* `--replace` после записи сам проверяет: число записей, целевую запись
  побайтово, все остальные — побайтово, независимое чтение оракулом
  (`gm.bigfile`) и идемпотентность (перечитать и пересобрать = те же байты).
* `--check` без `--against` проверяет структуру (магия, `0x1A`, длина таблицы,
  монотонность и границы смещений, конец последнего блоба = размер `.DAT`,
  пересборка = файл) и читает архив оракулом; с `--against --name --expect` —
  дополнительно «все прочие записи те же, целевая равна новому файлу».
* `--json` — одна строка JSON в любом режиме.
* Коды возврата: `0` успех, `1` проверка не прошла (round-trip/check/replace/
  self-test), `2` неверный ввод/использование, `3` внутренняя ошибка.
  Ошибки печатаются одной строкой `bigpack: …`, без трейсбека.

`--list` печатает `#`, имя, смещение, размер и FNV-1a 64 блоба (тот же дайджест,
что в остальном тулките).

## 4. Результаты

### 4.1. Побайтовый round-trip всех поставочных архивов

```
$ python3 tools/editor/bigpack.py --roundtrip
round-trip: /mnt/d/games/Gunmetal 3dfx
  skipped (no .DAT): Film/ammo.idx
  ok   FRONTEND/BMP.IDX       143 entries     2845582 B  idx    1728 B  identical=True/True  0.04s
  ok   MUSIC/SOU.IDX         3092 entries    75628172 B  idx   37116 B  identical=True/True  0.97s
  ok   PICS/SBM.IDX          1648 entries    45442175 B  idx   19788 B  identical=True/True  0.44s
  ok   sounds/SOU.IDX         999 entries    26738998 B  idx   12000 B  identical=True/True  0.28s
  ok   sprites/SBM.IDX        179 entries     1083044 B  idx    2160 B  identical=True/True  0.02s
  totals: 5 archives, 6061 entries, 151737971 bytes, all identical=True
```

`Film/ammo.idx` — не BIGFILE (текст `%ld, %ld`, см. `docs/FORMATS.md`), пары
`.DAT` у него нет, поэтому он пропускается с явной строкой. `--json` даёт
`{"totals": {"archives": 5, "identical": 5, "entries": 6061,
"bytes": 151737971}, "ok": true}`.

### 4.2. Self-test (без файлов игры, 15 проверок)

```
$ python3 tools/editor/bigpack.py --self-test
...
self-test: 15/15 checks passed
```

Покрыто: round-trip (обычный, с префиксом `.DAT`, паддингом, пустой записью,
хвостом `.IDX`, пустой архив, вывод `gm.bigfile.pack`); замена с ростом,
усадкой и тем же размером (индекс побайтово равен исходному); пустой payload;
запись нулевого размера; дубликаты (меняется только первая); `--add`;
регистр/длина имён; семь битых индексов (магия, короткий заголовок, короткая
таблица, `count=5e6`, смещения назад, смещение за EOF, отсутствующий `.DAT`);
`--check` (чистый архив проходит, lower-case и дубли — предупреждения); порча
одного байта в `.DAT` ловится верификатором; детерминизм пересборки.

### 4.3. Замена на реальном архиве

Источник — `PICS/SBM.IDX` (1648 записей, 45 442 175 Б), цель — запись 0
`57CHEV` (87 393 Б). Новые файлы: тот же размер с одним инвертированным байтом,
+1360 Б и −7393 Б. Всё писано в `/tmp/opencode/bigpack-m11`, игра не тронута.

```
$ python3 tools/editor/bigpack.py --replace "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX" \
      --name 57CHEV --file same0.bin --out /tmp/opencode/bigpack-m11/same
  entry 0 '57CHEV': 87393 -> 87393 bytes (delta +0)
  entries: 1648 (was 1648)
  others byte-identical: 1647/1647
  target identical: True  oracle agrees: True  idempotent: True

$ cmp /tmp/opencode/bigpack-m11/same.IDX "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX"
$ cmp -l /tmp/opencode/bigpack-m11/same.DAT "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.DAT"
     43697 305  72            # ровно один байт, внутри целевой записи
```

Рост (+1360, 87 393 → 88 753):

```
$ python3 tools/editor/bigpack.py --replace ... --file grow0.bin --out .../grow --json
{"...", "new_size": 88753, "delta": 1360, "count": 1648,
 "others_identical": 1647, "target_identical": true, "oracle_agrees": true,
 "idempotent": true, "problems": [], "ok": true}
```

Независимая проверка результата (не инструментом):

```
$ python3 gm.py list /tmp/opencode/bigpack-m11/grow.IDX | head -3
/tmp/opencode/bigpack-m11/grow.IDX: 1648 entries, 45443535 bytes of data
  57CHEV       off=0          size=88753
  57CHEVX      off=88753      size=87393

# разбор индекса вручную
idx sizes 19788 19788; header equal True
count 1648 1648
all 1648 names identical; first (0, 0) second (87393, 88753) last (45436702, 45438062)
shift 1360 uniform for entries 1..1647: True
oracle: 1648 45443535 57CHEV 88753 88753
target == grow0.bin: True
```

`--check` с эталоном:

```
$ python3 tools/editor/bigpack.py --check /tmp/opencode/bigpack-m11/grow.IDX \
      --against "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX" \
      --name 57CHEV --expect /tmp/opencode/bigpack-m11/grow0.bin
check: /tmp/opencode/bigpack-m11/grow.IDX
  1648 entries, 45443535 bytes of data, index 19788 bytes
  compared with .../PICS/SBM.IDX: 1647 other entries identical
  target equals .../grow0.bin: True
  result: ok
```

Усадка (−7393, 87 393 → 80 000): `others_identical=1647`, `.DAT` =
45 434 782 Б, `ok: true`.

Ошибки — понятным текстом и кодом:

```
$ ... --replace ... --name NOSUCH --file ... --out ...
bigpack: entry 'NOSUCH' not found in .../PICS/SBM.IDX (1648 entries); use --add to append it
exit=2
$ ... --replace ... --out .../x          # файл уже есть
bigpack: refusing to overwrite /tmp/opencode/bigpack-m11/x.IDX (use --force)
exit=2
$ ... --out "/mnt/d/games/Gunmetal 3dfx/PICS/SBM_TEST"
bigpack: refusing to write into the game installation (.../SBM_TEST.IDX); ...
exit=2
$ ... --check /tmp/opencode/bigpack-m11/bad.IDX   # инвертирована магия
  PROBLEM: ...: not a BIGFILE index: magic is b'\xbdIGFILE', expected b'BIGFILE' (gm/bigfile.py:72)
exit=1
```

### 4.4. Регрессия

```
$ python3 regress.py --mirror --json
{"passed": 114, "total": 114, "seconds": 44.8, "full": false, ..., "failed": []}
```

Категории `archives/*`, `repack/*`, `inventory` и `trust/*` остались зелёными:
`gm/bigfile.py` не менялся, новый инструмент — отдельный файл.

## 5. Ограничения

* **В игре замена не проверялась** (игра только читается, запуск не входил в
  задачу). Доказано: архив открывается оракулом, `gm.py list` видит ту же
  таблицу, оригинальный читатель берёт смещения из того же поля (`0x40d819`).
* **4 ГиБ не проверялись**: смещения `u32`; писатель отказывает при
  `offset > 0xFFFFFFFF` и при размере `.DAT` больше 4 ГиБ.
* Имя — 1…8 байт; более длинное игра всё равно обрежет ключом из 8 байт
  (`docs/SOUND_NAMES.md` §3.2). `--add` по умолчанию принимает только
  `[A-Z0-9_+.-']`, потому что иначе запись недостижима для игры.
* Дубликаты имён не отвергаются (в поставке их нет): вторая запись
  недостижима для игры, `--check` выдаёт предупреждение.
* «Хвост» `.IDX` и префикс `.DAT` сохраняются, но не интерпретируются; `--check`
  сообщает об их наличии предупреждением.
* `--roundtrip` пишет копии во временный каталог (`--tmp-dir`, по умолчанию
  `/tmp`): для `MUSIC/SOU` это ~150 МБ на архив, при тесном tmpfs используйте
  `--tmp-dir /var/tmp`.

## 6. Доказано / гипотеза / осталось

**Доказано (данные, 100 %):**

* пересборка всех 5 поставочных архивов (6061 запись, 151 737 971 Б) даёт
  побайтово идентичные `.IDX` и `.DAT` — `--roundtrip`;
* замена одной записи на тот же размер оставляет `.IDX` побайтово равным
  исходному и меняет ровно 1 байт в `.DAT` (`cmp`, `cmp -l`);
* рост/усадка меняет только смещения: 1648/1648 имён совпадают, сдвиг
  одинаков для всех последующих записей, 1647 прочих блобов идентичны;
* `gm.bigfile` читает результат (число записей и размер `.DAT` совпадают).

**Доказано (код, VA):** 12-байтный заголовок и `count+8` (`0x40d4db`,
`0x40d513`); запись 12 байт и порционное чтение (`0x40d4fd`, `0x40d531`,
`0x40d6b3`); `offset` по `+8` → `fseek` (`0x40d819`); ключ 8 байт и верхний
регистр запроса (`0x40d1b2`, `0x40d1e6` → `0x480180`); первое совпадение при
дубликате (`0x40d6b3`). Отсутствие проверки магии — по исчерпывающему списку
ссылок на буфер заголовка (`0x40d4e0`, `0x40d2a8`, `0x40d513`, `0x40d60f`).

**Гипотеза:** ничего нового о формате не вводилось: писатель воспроизводит
раскладку из `gm/bigfile.py` и `docs/FORMATS.md` §1.

**Осталось:** прогон в оригинале с подменённым архивом (копия установки, не
сама установка) и CTest-обёртка `bigpack_selftest`/`bigpack_roundtrip` —
это правит основной агент (`CMakeLists.txt` и `docs/INDEX.md` сабагенту
запрещены).
