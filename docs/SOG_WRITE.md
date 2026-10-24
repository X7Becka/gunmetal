# Запись `.sog`: побайтовый round-trip и правка одной строки (`M11.5`, срез 5)

* **Статус:** срез готов. Round-trip **651/651** файлов побайтово (598 `.sog` +
  53 `.SOG`, 43 205 Б); собственный разбор + сборка из строк **651/651**,
  оракул `gm/sog.py` — **651/651** по байтам и по именам; `--check` на всём
  корпусе даёт **0** структурных проблем. Правка одной строки (`Dm1A.sog`,
  `Dm1A01` → `winner`) меняет **6 байт** (66 → 66 Б), `--check` ок,
  `gm.py sog` видит новое имя и резолвит его в запись **3091** (1 341 632 Б),
  C++ `--playlist` подтверждает: `entries=8 found=8 missing=0`. Выдуманное имя
  даёт `MISSING=1` / `found=7`. Self-test **41/41**.
* **Ветка:** `feat/sogwrite`, база `master` `b3d5189`, дата 2026-10-24.
* **Файлы:** `tools/editor/sogwrite.py` (новый), `docs/SOG_WRITE.md` (этот
  документ). Существующие файлы (`gm/sog.py`, `gm/*.py`, `gm.py`,
  `engine/src/audio/playlist.*`, `tools/editor/*`, `regress.py`,
  `ROADMAP.md`, `docs/*.md`, `engine/*`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/sogwrite`
  (в git не попадают).
* **Оракул:** `gm/sog.py` (`parse`, `to_bytes`, `from_names`), формат —
  `docs/FORMATS.md` §9, модель/числа резолва — `docs/PLAYLIST.md`,
  C++-читатель — `engine/src/audio/playlist.*` (`docs/PLAYLIST.md` §3).

---

## 1. Формат и почему правка строковая

`.sog` — обычный текст: по одному имени сэмпла на строку, без заголовка,
смещений и контрольных сумм (`docs/FORMATS.md` §9). Имена ссылаются не на
`sounds/SOU.DAT`, а на второй BIGFILE-архив `MUSIC/SOU.IDX` (3092 записи,
все `SOU2`). Единственная «бинарная» часть формата — переводы строк: во всём
корпусе это **CRLF**.

Отсюда точная модель (одна операция, обратная разбору):

```
lines = raw.decode("latin1").split("\r\n")
raw   = b"\r\n".join(line.encode("latin1") for line in lines)
```

`latin1` — ровно один байт на символ, поэтому модель биективна для любого
содержимого. `split`/`join` обратны тогда и только тогда, когда в файле нет
одиночных `CR`/`LF`; такие файлы инструмент **отвергает**, а не «нормализует»
(в поставке их нет: 651/651 — чистый CRLF). `--out` пишется только вне игры,
зеркала и репозитория.

Что даёт модель на поставочном корпусе (`--audit`, §3):

| признак | значение |
|---|---|
| файлов | 651: **598** `.sog` + **53** `.SOG` |
| байт | 43 205 |
| записей (элементов `split("\r\n")`) | 6 438 |
| пустых записей | 1 227 (из них 1 — только из пробелов) |
| строк с внешними пробелами (непустых) | 320 (+1 пробельная = 321, как в `docs/PLAYLIST.md` §1) |
| имён сэмплов | 5 211 |
| дубликатов | 998 (фолд) / 988 (точный регистр) в 131 плейлисте |
| разных имён (фолд) | 3 017 |
| длина имени | 6..8 Б (6 Б — 5 188, 7 Б — 2, 8 Б — 21) |
| длина строки | 0..9 Б |
| файлов без CRLF в конце | **7** (`L01A`..`L01G`) |

Запись после финального CRLF — пустая: у `Dm1A.sog`
(`…Dm1A08\r\n\r\n`) `split` даёт 10 элементов, 8 имён и 2 пустых записи.
Именно так считает и `gm/sog.py` (`lines = text.split("\r\n")`), и число
1 227 пустых строк в `docs/PLAYLIST.md` §1. `--list` печатает такие записи
явно, чтобы номер строки в правке не разошёлся с ожиданием.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/sogwrite.py --roundtrip
```

```
roundtrip: /mnt/d/games/Gunmetal 3dfx/MUSIC
  651 files (598 .sog + 53 .SOG), 43205 bytes, 2.0 s
  identical=651/651  differs=0  errors=0
  oracle gm/sog.py: to_bytes identical=651/651, names match=651/651
result: OK
```

| метрика | результат |
|---|---|
| найдено `.sog` (рекурсивно, регистр расширения не важен) | 651 |
| **побайтово идентичны после split → join** | **651 / 651** (43 205 Б) |
| `gm/sog.py:to_bytes()` даёт тот же файл | 651 / 651 |
| `gm/sog.py:names` == наши имена | 651 / 651 |
| ошибок разбора (lone CR/LF, пустой файл) | 0 |
| время | 2,0–2,4 с на `/mnt/d`, **0,03 с** на зеркале |
| `--check` на всём корпусе | 651/651 без проблем |

Сверка с оракулом — не доверие к нему, а независимая проверка: наш разбор
обязан дать те же строки и имена, что `gm/sog.py`, иначе `--roundtrip`
печатает `problem` и возвращает код 1. Сам оракул не правится.

```sh
python3 tools/editor/sogwrite.py --check "$MUSIC/L01C.sog"
# 70 bytes, 8 records, final CRLF=no, sha256=…
# names=8 blank=0 outer-ws=8 …
# note: 8 name(s) surrounded by spaces (trimmed by the reader, preserved here)
# note: last record has no CRLF (7 of 651 shipped files)
# result: OK
```

## 3. Числа корпуса (`--audit`)

```sh
python3 tools/editor/sogwrite.py --audit
```

```
files=651 (598 .sog + 53 .SOG)  bytes=43205  records=6438  names=5211
blank=1227 (whitespace-only 1)  outer-ws=320  dups=998 (exact 988) in 131 playlists, 454 duplicate names
distinct(fold)=3017  name-len=6..8  line-len=0..9  without final CRLF=7
name length histogram: 6B=5188, 7B=2, 8B=21
result: OK
```

Числа сходятся с `docs/PLAYLIST.md` §1/§4, посчитанными независимо:
651 файл, 5 211 имён, 998 лишних вхождений в 131 плейлисте, 3 017 разных имён
после фолда; 320 строк с внешними пробелами + 1 пробельная запись = 321
«строка с внешними пробелами» из `PLAYLIST.md`. Дубликаты — **данные**, а не
ошибка: `L26I.sog` (112 имён, 72 повтора) играет блоки повторно
(`PLAYLIST.md` §1). Инструмент их сохраняет и только предупреждает.

`--audit`/`--roundtrip` без аргумента берут `<game>/MUSIC`; явный `DIR`
заменяет каталог, `--game` — установку. Медленный `/mnt/d` (drvfs) даёт
2,0–2,4 с, зеркало — 0,03 с (651 файл).

## 4. CLI

```sh
python3 tools/editor/sogwrite.py --roundtrip [DIR] [--json] [--verbose]
python3 tools/editor/sogwrite.py --audit [DIR] [--json]
python3 tools/editor/sogwrite.py --list FILE [--json]
python3 tools/editor/sogwrite.py --check FILE [--against ORIG] [--json]
python3 tools/editor/sogwrite.py --set FILE --line N --name NAME --out OUT
                                 [--strict] [--json]
python3 tools/editor/sogwrite.py --self-test
```

`FILE` — путь к `.sog`/`.SOG`; `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx` и влияет только на умолчания `--roundtrip`/
`--audit` и на защиту от записи в установку.

`--list` печатает номер записи, байтовое смещение, длину, имя (с экранированием
непечатаемых байтов) и флаги: `blank`/`ws-only` (пустая запись), `outer-ws`,
`name>8B`, `dupN`, `odd-name` (пробелы/управляющие внутри имени).

`--check` — структура: расширение, непустой файл, только CRLF, нет NUL,
есть хотя бы одно имя, нет имён >8 Б (предупреждение), нет управляющих
байтов; отдельно печатает числа и «данные» (пустые строки, внешние пробелы,
дубликаты, отсутствие финального CRLF) как `note`. `--against ORIG` добавляет
побайтовое сравнение: `IDENTICAL`/`differs`, число изменённых байт (позиционно),
первое расхождение, `delta` и первую изменённую запись. **Код возврата
`--check` определяется структурой**, а не равенством с `--against`: правка
обязана проходить `--check`, но не обязана быть равна оригиналу.

`--set` заменяет имя одной записи, сохраняя раскладку:

* ведущие/хвостовые пробелы строки остаются (у `L01C.sog` это `L01C01 `);
* CRLF и финальный EOL не меняются; число записей и число CRLF — тоже;
* если запись была пустой, она получает имя: песня выигрывает трек, но
  **строка не добавляется** (число записей прежнее);
* пустая запись после финального CRLF — это тоже запись (`--list` её
  показывает); если дать имя ей, файл перестанет кончаться на CRLF — об этом
  печатается предупреждение (7 поставочных файлов и так без финального CRLF).

Пример реальной правки (игра только читается, вывод в `/tmp/opencode`):

```sh
M=~/.cache/gunmetal-re/game/MUSIC
python3 tools/editor/sogwrite.py --set "$M/Dm1A.sog" --line 1 \
        --name winner --out /tmp/opencode/sogwrite/Dm1A.sog
# edit: …/Dm1A.sog -> /tmp/opencode/sogwrite/Dm1A.sog (record 1/10)
#   name: 'Dm1A01' -> 'winner'
#   bytes: 66 -> 66 (+0); changed 6 byte(s) in the record …
#   gm/sog.py sees 8 name(s); --check: ok

python3 tools/editor/sogwrite.py --check /tmp/opencode/sogwrite/Dm1A.sog \
        --against "$M/Dm1A.sog"
# against …/Dm1A.sog (66 bytes): differs
#   changed 6 byte(s) (positional), first diff at offset 0, delta +0, records 10 -> 10
#   first changed record 1: 'Dm1A01' -> 'winner'
# result: OK
```

Проверка оракулом (тот же разбор/резолв, что у C++ `load_playlist`/
`resolve_playlist`), Python и C++:

```sh
python3 gm.py sog /tmp/opencode/sogwrite/Dm1A.sog --music "$M/SOU.IDX"
# sog: 1 songs, 0 with unresolved samples

# C++ audio/playlist через CLI движка (сборка build/sandbox, код не менялся):
build/sandbox/engine/gunmetal --playlist /tmp/opencode/sogwrite/Dm1A.sog
# playlist: Dm1A.sog entries=8 found=8 missing=0 bytes=1554688
#   winner     3091  1341632
#   Dm1A02        1    23716
#   …

python3 gm.py sog /tmp/opencode/sogwrite/neg/Dm1A.sog --music "$M/SOU.IDX"
# Dm1A.sog     8 samples  MISSING=1      (строка 1 = ZZZZZZZZ)
# sog: 1 songs, 1 with unresolved samples
build/sandbox/engine/gunmetal --playlist /tmp/opencode/sogwrite/neg/Dm1A.sog
# playlist: Dm1A.sog entries=8 found=7 missing=1 bytes=213056
#   ZZZZZZZZ     -1        0  (missing)
```

`winner` — запись **3091**, **1 341 632** Б (`docs/PLAYLIST.md` §1). Значит,
правка видна читателю и резолвится; выдуманное имя честно помечается как
`MISSING`. Пять проверок `--set` (все обязательные, иначе `internal error`,
код 3): число и соседи записей не изменились; число CRLF прежнее; повторный
разбор даёт ровно новые записи; `gm/sog.py` видит новое имя; `--check` не
находит структурных проблем. После записи sha256 файла на диске сверяется с
отчётом.

Коды возврата: `0` успех, `1` проверка не прошла (`--roundtrip`/`--check`/
`--self-test`), `2` неверный ввод/отказ (сообщение в stderr), `3` внутренняя
ошибка.

Отказы (self-test + вручную, код 2):

```
$ … --set "$M/Dm1A.sog" --line 1 --name X --out "$M/x.sog"
sogwrite: error: refusing to write …: it is inside /home/nooverload/.cache/gunmetal-re/game
# то же для /mnt/d/games/Gunmetal 3dfx и для репозитория; --out == вход — отказ
$ … --check /etc/hostname
sogwrite: error: /etc/hostname: not a .sog file (extension '<none>')
$ … --check empty.sog
sogwrite: error: empty playlist: empty.sog
$ … --set "$M/Dm1A.sog" --line 99 --name X --out /tmp/opencode/x.sog
sogwrite: error: --line 99 is out of range: …/Dm1A.sog has 10 record(s)
```

## 5. Имя: проверки и предупреждения

| случай | поведение | почему |
|---|---|---|
| пустое имя | отказ (2) | пустая строка — не сэмпл; строки не создаём |
| имя с ведущими/хвостовыми пробелами | отказ (2) | читатель делает `strip()`, эти байты не войдут в имя |
| CR/LF/NUL в имени | отказ (2) | запись — это одна строка; NUL обрезал бы имя |
| длина > 255 Б | отказ (2) | граница доверия читателя (`docs/PLAYLIST.md` §2) |
| длина 9..255 Б | **предупреждение**, запись выполняется | поле BIGFILE — `name[8]`, имя никогда не резолвится (`PLAYLIST.md` §2); `--strict` превращает это в отказ |
| пробелы/управляющие внутри имени | предупреждение | не совпадёт с записью архива |
| байты > 0x7E | предупреждение | latin1 легален и сравнивается побайтово, но в поставке таких имён нет |

`--strict` — для тех, кому нужно гарантированно резолвимое имя (≤8 Б, ASCII,
без пробелов); инструмент тогда отказывает до записи.

## 6. Self-test и регрессия

```sh
python3 tools/editor/sogwrite.py --self-test     # 41/41, без файлов игры
python3 regress.py --mirror --json               # 115/115
```

Self-test 41/41: идемпотентность split/join на CRLF, отсутствие финального
EOL, хвостовые пробелы, двойной финальный CRLF, lone CR/LF и пустой файл,
регистр расширения, latin1, `--check` (структура/`--against`/длинное имя/
пробельный файл), `--list`, `--set` (одна строка, пробелы, отсутствие EOL,
смена длины, пустая запись, выход за диапазон, пустое/CRLF/пробельное/
длинное имя, `--strict`, >255 Б), защита `--out` (игра/зеркало/репозиторий/
вход), синтетические `--audit`/`--roundtrip`, LF-файл в `--roundtrip`,
вспомогательные функции. Временный каталог — `/var/tmp` (не tmpfs), после
прогона удаляется.

## 7. Ограничения

* **Не создаёт и не удаляет строки.** Нет `--add`/`--delete`; число записей и
  CRLF фиксировано. Новая песня/новый трек — это другой инструмент (или
  `gm/sog.py:from_names` + свой writer).
* **Не резолвит имена в `MUSIC/SOU.IDX`.** Проверка резолва — `gm.py sog`
  и `docs/PLAYLIST.md` (`audio/playlist` в C++); здесь только текст.
  Соответственно `--strict` опирается на длину поля `name[8]`, а не на
  реальный индекс.
* **Не конвертирует переводы строк.** Файл с одиночными `CR`/`LF` отвергается
  (`--roundtrip` печатает причину и возвращает 1), а не переписывается в CRLF:
  молчаливая нормализация изменила бы чужой файл.
* **Не валидирует charset.** Любые байты, кроме CR/LF/NUL и внешних
  пробелов, допустимы (latin1); не-ASCII и управляющие — предупреждение.
* **Правка «фантомной» записи после финального CRLF** убирает финальный EOL
  (иначе нельзя: имя должно лечь в последнюю запись). Предупреждение
  печатается; число записей не меняется.
* **`--against` — справка, не ворота.** Позиционное число изменённых байт
  включает сдвинутый хвост при смене длины; для «размера правки» `--set`
  отдельно печатает изменение внутри строки (`changed_bytes`) и в файле
  (`file_changed_bytes`).
* **C++ `audio/playlist` остаётся только читателем**; `main.cpp`,
  `engine/src/audio/*`, `gm.py sog --rebuild`, `gm/sog.py` не менялись.
  Инструмент не подключён к CLI движка и к CTest (новые файлы — только
  `tools/editor/sogwrite.py` и этот документ).
* **Игра, зеркало и репозиторий защищены**: `--out` внутрь них — отказ (2),
  `--out`, равный входу, — тоже; всё остальное пишется атомарно
  (`path.tmp` + `os.replace`).

## 8. Ссылки

`docs/FORMATS.md` §9 (`.sog` — текстовый плейлист, `MUSIC/SOU.DAT`);
`docs/FORMATS.md` §1 (`char name[8]`); `docs/PLAYLIST.md` §1 (числа корпуса,
дубликаты, регистр), §2 (модель и отказы), §3 (C++ API), §5 (только чтение);
`docs/SOUND_NAMES.md` §3.1 (ключ в верхнем регистре); `gm/sog.py`;
`tools/editor/soawrite.py`/`docs/SOA_WRITE.md` (тот же приём для `.soa`);
`ROADMAP.md` `M11.5`.
