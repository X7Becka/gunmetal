# Запись таблиц убийств `KILLED.skb` / `.skk` (`M11.5`, срез 12)

* **Статус:** срез готов. Round-trip **2/2** побайтово (4 305 Б, 166 строк,
  119 принятых), оракул `gm/small.py` — **2/2** по байтам и по строкам;
  `--check` на обоих файлах — 0 структурных проблем; self-test **55/55**.
  Правка строки меняет ровно ожидаемые байты: `KILLED.skb`, строка 2,
  `%S MUTILATES %S.` → `%S MUTILATES %s.` — **1 байт** (смещение 49), длина и
  CRLF не меняются; C++ CLI видит `lines=69 subs=138`, оракул — те же 91 строку.
  Укорочение строки 1 с 33 до 29 Б переводит её через границу фильтра
  (**69 → 70** принятых, `%S` в принятых 138 → 140), инструмент об этом
  предупреждает; C++ CLI подтверждает `lines=70 subs=140`. C++-тест `killed` —
  **133 проверки, ok** (бинарь не пересобирался, C++ не менялся).
* **Ветка:** `feat/killedwrite`, база `master` `6463ee3`, дата 2026-10-24.
* **Файлы:** `tools/editor/killedwrite.py` (новый), `docs/KILLED_WRITE.md` (этот
  документ). Существующие файлы (`engine/src/ui/killed.*`, `engine/tests/*`,
  `gm/*`, `gm.py`, `tools/editor/*`, `regress.py`, `ROADMAP.md`, `docs/*.md`)
  **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/killedwrite`
  (в git не попадают).
* **Оракул:** `gm/small.py` (`parse_kill`, `KillFile.to_bytes`) и `gm.py misc`;
  формат/поведение — `docs/FORMATS.md` §8.2, `docs/KILLED.md` §1–§3,
  `docs/SUB.md` §7 (загрузчики `0x485a00`/`0x485b2f`), C++-читатель —
  `engine/src/ui/killed.{h,cpp}`.

---

## 1. Формат и модель строк

Оба файла — обычный DOS-текст: по одному сообщению на строку, CRLF, без
заголовка, смещений и контрольных сумм. Игра открывает их `fopen(..., "r")`
(текстовый режим), читает `fgets(buf, 0x400, f)` и оставляет строку только при
`0 < strlen(line) < 0x20` (`docs/KILLED.md` §1). В текстовом режиме CRLF уже
свёрнут в LF, а `fgets` оставляет `\n`, поэтому **видимый текст принятой
строки — 1..30 байт**; строки длиннее 30 байт отбрасываются.

Единственная «бинарная» часть — переводы строк. Отсюда точная модель (одна
операция, обратная разбору):

```
records = raw.decode("latin1").split("\r\n")     # последний элемент — фантом
if raw.endswith(b"\r\n"): records.pop()          # он не строка файла
raw     = "\r\n".join(records).encode("latin1") + (b"\r\n" if final_eol else b"")
```

`latin1` — ровно один байт на символ, поэтому модель биективна для любого
содержимого без одиночных `CR`/`LF`. Такие файлы инструмент **отвергает**, а не
«нормализует»: `split`/`join` перестали бы быть обратными. В поставке их нет
(2/2 — чистый CRLF).

Ключевая деталь раскладки: файл кончается **`CRLF CRLF`**, то есть последняя
строка файла пустая, а сразу за ней идёт финальный CRLF. У `.skb` это
92 строки (91 сообщение + 1 пустая), у `.skk` — 74 (73 + 1). Поэтому
`--list`/`--set` нумеруют **строки файла** (1..92 / 1..74), включая последнюю
пустую; это не индекс в массиве игры (69/50). `--list --accepted` и фильтр в
`--list` показывают, какие строки попадут в массив.

Пустой файл, NUL внутри строки (`strlen` обрезал бы сообщение), файл > 64 КиБ и
одиночные `CR`/`LF` — отказ (код 2). Запись — только ASCII: `--text` с байтом
`> 0x7E` или управляющим байтом отвергается (в поставке ASCII 2/2; файл с
не-ASCII читается и проверяется, но править его инструмент отказывается).

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/killedwrite.py --roundtrip
```

```
roundtrip: /mnt/d/games/Gunmetal 3dfx/FRONTEND
  2 files (1 .skb + 1 .skk), 4305 bytes, 166 lines (164 non-empty, 119 accepted), 0.00s
  identical=2/2  differs=0  errors=0
  oracle gm/small.py: to_bytes identical=2/2, lines match=2/2
result: OK
```

| метрика | результат |
|---|---|
| файлов найдено (`.skb`/`.skk`, регистр расширения не важен) | 2 |
| **побайтово идентичны после split → join** | **2 / 2** (4 305 Б) |
| `gm/small.py:KillFile.to_bytes()` даёт тот же файл | 2 / 2 |
| наш список строк == `KillFile.lines` | 2 / 2 |
| ошибок разбора | 0 |
| время | 0,00–0,01 с (и на `/mnt/d`, и на зеркале) |

`--roundtrip FILE` проверяет один файл (`identical=1/1`), `--roundtrip DIR` —
все `.skb`/`.skk` рекурсивно; без аргумента берётся `<game>/FRONTEND`.
Несовпадение с оракулом — `problem` и код 1: наш разбор обязан дать те же
строки, что `gm/small.py`, а не «похожие». Оракул не правится.

`--check` печатает структуру (EOL, финальный пустой CRLF, кодировка) и числа:

```sh
python3 tools/editor/killedwrite.py --check "$M/KILLED.skb"
# 2338 bytes, 92 lines, final CRLF=yes, final empty line=yes, sha256=43453d2d200dae7b
# eol: CRLF only (crlf=92, lone=0); encoding=ascii
# lines=92 non-empty=91 blank=0 accepted=69 line-len=0..47 accepted-len=12..30
# placeholders=182 (%S=109, %s=73), in accepted=138, max per line=2
# note: 22 line(s) longer than 30 bytes are dropped by the engine (first: line 1, 33 B)
# note: 9 duplicate occurrence(s) in 3 distinct line(s) -- data, kept as-is
# result: OK
```

## 3. Числа корпуса (`--audit`)

```sh
python3 tools/editor/killedwrite.py --audit
```

```
audit: /mnt/d/games/Gunmetal 3dfx/FRONTEND
  KILLED.skb
    bytes=2338 crlf=92 lone=0 final-crlf=yes final-empty-line=yes encoding=ascii
    lines=92 non-empty=91 blank=0 accepted=69 (longer than 30 B: 22, stored 69/128)
    line-len=0..47 non-empty-len=12..47 accepted-len=12..30
    placeholders=182 (%S=109, %s=73), in accepted=138, max per line=2
    duplicates=9 extra occurrence(s) in 3 line(s) (fold: 14 in 7)
  KILLED.skk
    bytes=1967 crlf=74 lone=0 final-crlf=yes final-empty-line=yes encoding=ascii
    lines=74 non-empty=73 blank=0 accepted=50 (longer than 30 B: 23, stored 50/128)
    line-len=0..53 non-empty-len=11..53 accepted-len=11..30
    placeholders=73 (%S=44, %s=29), in accepted=50, max per line=1
    duplicates=7 extra occurrence(s) in 1 line(s) (fold: 9 in 3)
  totals: files=2 (1 .skb + 1 .skk) bytes=4305 lines=166 non-empty=164 accepted=119 placeholders=255 (in accepted 188) dups=16
  0.01s
result: OK
```

Числа сходятся с `docs/KILLED.md` §1/§3, посчитанными независимо:

| факт | `KILLED.md` | `--audit` |
|---|---|---|
| `.skb`: байт / строк / непустых / принятых | 2338 / 92 / 91 / **69** | то же |
| `.skb`: длиннее 30 Б | 22 | 22 |
| `.skb`: `%S` / `%s` / всего | 109 / 73 / 182 | то же |
| `.skb`: плейсхолдеров в принятых | **138** | 138 |
| `.skb`: дубликаты | — | 9 лишних в 3 строках (фолд 14 в 7) |
| `.skk`: байт / строк / непустых / принятых | 1967 / 74 / 73 / **50** | то же |
| `.skk`: длиннее 30 Б | 23 | 23 |
| `.skk`: `%S` / `%s` / всего | 44 / 29 / 73 | то же |
| `.skk`: плейсхолдеров в принятых | **50** | 50 |
| `.skk`: дубликаты | — | 7 лишних в 1 строке (фолд 9 в 3) |
| EOL | CRLF, конец `CRLF CRLF` | crlf=92/74, lone=0, final-empty-line=yes |

Дубликаты — **данные**, а не ошибка: `.skb` восемь раз повторяет
`%S KILLS %S.` и дважды `%S ERADICATES %S.`; `.skk` восемь раз `%S IS DEAD.`
Игра берёт их как есть, инструмент сохраняет и только предупреждает.

`%` в файлах ровно 182 и 73 — все до единого `%S`/`%s`, посторонних `%<c>` нет.
Поэтому движок ничего, кроме имени игрока, не подставляет, а наш счётчик
плейсхолдеров совпадает с `substitution_count()` C++.

## 4. CLI и коды возврата

```sh
python3 tools/editor/killedwrite.py --roundtrip [DIR|FILE] [--json] [--verbose]
python3 tools/editor/killedwrite.py --audit [DIR|FILE] [--json]
python3 tools/editor/killedwrite.py --list FILE [--accepted] [--json]
python3 tools/editor/killedwrite.py --check FILE [--against ORIG] [--json]
python3 tools/editor/killedwrite.py --set FILE --line N --text TEXT --out OUT [--json]
python3 tools/editor/killedwrite.py --prepend FILE --line N --text TEXT --out OUT [--json]
python3 tools/editor/killedwrite.py --append FILE --text TEXT --out OUT [--json]
python3 tools/editor/killedwrite.py --self-test [--json]
```

`--game DIR` меняет установку (влияет на умолчания `--roundtrip`/`--audit` и на
защиту от записи). `--json` — машиночитаемый отчёт для любого режима.

`--list` печатает номер строки, байтовое смещение, длину, число плейсхолдеров,
метку `+`/`.` (принята ли фильтром) и флаги: `long` (>30 Б), `blank`,
`ws-only`, `dupN`, `ph>2`. `--accepted` оставляет только принятые строки.

Коды: `0` успех, `1` проверка не прошла (`--roundtrip`/`--check`/`--self-test`),
`2` неверный ввод/отказ (сообщение в stderr), `3` внутренняя ошибка.

Отказы (2) — проверены и в self-test, и вручную:

```
$ … --check /etc/hostname
killedwrite: error: /etc/hostname: not a .skb/.skk kill table (extension '<none>')
$ … --set "$M/KILLED.skb" --line 2 --text X --out "$M/x.skb"
killedwrite: error: refusing to write …/x.skb: it is inside …/.cache/gunmetal-re/game
# то же для /mnt/d/games/Gunmetal 3dfx, worktree и главного репозитория
$ … --set "$M/KILLED.skb" --line 2 --text X --out "$M/KILLED.skb"
killedwrite: error: refusing to overwrite the input file …/KILLED.skb
$ … --set "$M/KILLED.skb" --line 2 --text '%S %S %S' --out /tmp/opencode/x.skb
killedwrite: error: --text has 3 placeholder(s) (%S/%s), at most 2 fit: …
$ … --set "$M/KILLED.skb" --line 99 --text X --out /tmp/opencode/x.skb
killedwrite: error: --set 99 is out of range: …/KILLED.skb has 92 record(s) (1..92)
$ … --set "$M/KILLED.skb" --line 1 --text 'café' --out /tmp/opencode/x.skb
killedwrite: error: --text must be ASCII: byte(s) 0xe9 are not …
```

Кроме перечисленного отвергаются: пустой файл, NUL в файле, одиночный `CR`/`LF`,
файл > 64 КиБ, `--text` с CR/LF, NUL, управляющим байтом или длиной > 1022 Б,
правка файла с не-ASCII байтами, `--append` в файл без финального CRLF.

## 5. Правка строки (`--set`)

`--set` заменяет текст одной строки и **не трогает больше ничего**: ни CRLF, ни
финальную пустую строку, ни соседей. Внутри — пять доказательств, иначе
`internal error` (код 2, файл не пишется): число строк и соседи не изменились;
число CRLF и состояние финального EOL прежние; повторный разбор даёт ровно новые
строки; `gm/small.py` видит те же строки; `--check` не находит проблем. После
записи sha256 файла на диске сверяется с отчётом.

Минимальная правка (игра только читается, вывод в `/tmp/opencode`):

```sh
M=~/.cache/gunmetal-re/game/FRONTEND
python3 tools/editor/killedwrite.py --set "$M/KILLED.skb" --line 2 \
        --text "%S MUTILATES %s." --out /tmp/opencode/killedwrite/KILLED.skb
# edit: …/KILLED.skb -> /tmp/opencode/killedwrite/KILLED.skb (line 2/92)
#   text: '%S MUTILATES %S.' -> '%S MUTILATES %s.'
#   length: 16 -> 16 B, 2 placeholder(s); changed 1 byte(s) in the line (first at offset 14), file +0
#   accepted: 69 -> 69 line(s); placeholders in accepted: 138 -> 138
#   bytes: 2338 -> 2338, CRLF/final empty line preserved; sha256: 43453d2d200dae7b -> 9c22bb214cd96da7
#   gm/small.py re-reads the output; --check: ok

python3 tools/editor/killedwrite.py --check /tmp/opencode/killedwrite/KILLED.skb \
        --against "$M/KILLED.skb"
# against …/KILLED.skb (2338 bytes): differs
#   changed 1 byte(s) (positional), first diff at offset 49, delta +0, lines 92 -> 92,
#   accepted 69 -> 69, placeholders in accepted 138 -> 138
#   first changed line 2: '%S MUTILATES %S.' -> '%S MUTILATES %s.'
# result: OK
```

**Изменён ровно 1 байт** — смещение 49 (строка 2 начинается с 35, внутри строки
позиция 14), `'S'` → `'s'`; длина файла и число CRLF те же. Оракул и C++:

```sh
python3 gm.py misc /tmp/opencode/killedwrite/KILLED.skb
# KILLED.skb              2338 B  91 messages
GUNMETAL_DIR=/tmp/opencode/killedwrite/game \
  build/sandbox/engine/gunmetal --killed PLAYER --killed-seed 999
# killed: FRONTEND/KILLED.skb lines=69 subs=138 seed=999
#   PLAYER SENDS PLAYER TO THE BONEYARD.
```

Второй пример — переход через границу фильтра (33 → 29 Б, строка 1; игра
загрузит её впервые):

```sh
python3 tools/editor/killedwrite.py --set "$M/KILLED.skb" --line 1 \
        --text "%S PUTS %S TO FIRE AND SWORD." --out /tmp/opencode/killedwrite/wide.skb
#   length: 33 -> 29 B, 2 placeholder(s); changed 10 byte(s) in the line (first at offset 23), file -4
#   accepted: 69 -> 70 line(s); placeholders in accepted: 138 -> 140
#   warning: the accepted set changes: 69 -> 70 line(s) (the engine keeps 1..30 visible bytes); …
GUNMETAL_DIR=/tmp/opencode/killedwrite/game \
  build/sandbox/engine/gunmetal --killed PLAYER --killed-seed 999
# killed: FRONTEND/KILLED.skb lines=70 subs=140 seed=999
#   PLAYER BUSTS PLAYER UP.
build/sandbox/engine/gunmetal --killed PLAYER --killed-all | sed -n 2p
#   PLAYER PUTS PLAYER TO FIRE AND SWORD.
```

`changed 10` — позиционное число внутри строки: общий префикс 23 Б, дальше
`THE SWORD.` против `SWORD.` (6 несовпавших + 4 лишних). В файле правка
сдвигает хвост на −4 Б, поэтому позиционный дифф файла большой; «размер правки»
— это `changed` строки и `delta` файла.

C++-читатель не менялся и не пересобирался: готовый бинарь
`/home/nooverload/gunmetal-re/build/sandbox/engine/tests/gm_killed_tests` на
поставочных файлах даёт **133 проверки, ok** (`skb raw=92 kept=69 subs=138,
skk raw=74 kept=50 subs=50`). Правку он читает через тот же `load_killed` (CLI
выше) — числа совпадают с нашими.

## 6. Добавление строки (`--prepend` / `--append`)

Новая строка вставляется так, чтобы **не сломать хвост**: если последняя строка
пустая (а в поставке она такая), `--append` вставляет новую строку **перед**
ней, поэтому файл по-прежнему кончается `CRLF CRLF`. `--prepend N` вставляет
перед строкой N (1..число строк). Файл без финального CRLF для `--append`
отвергается: новая строка осталась бы без терминатора, то есть изменила бы
структуру хвоста.

```sh
python3 tools/editor/killedwrite.py --append "$M/KILLED.skk" \
        --text "%S SLIPPED ON A BANANA." --out /tmp/opencode/killedwrite/app.skk
# append: …/KILLED.skk -> …/app.skk (before the trailing empty line (line 74 of 75))
#   text: '%S SLIPPED ON A BANANA.' (23 B, 1 placeholder(s))
#   lines: 74 -> 75, bytes: 1967 -> 1992 (+25), inserted 25 byte(s) (text + CRLF)
#   accepted: 50 -> 51 line(s); placeholders in accepted: 50 -> 51
#   warning: the empty CRLF line at the end stays last (the file still ends with CRLF CRLF)

python3 tools/editor/killedwrite.py --prepend "$M/KILLED.skk" --line 1 \
        --text "%S MET THE FLAT SIDE." --out /tmp/opencode/killedwrite/pre.skk
# prepend: … (before line 1 of 75); lines: 74 -> 75, bytes: 1967 -> 1990 (+23)
#   file diff (positional, an insertion shifts the tail): 1851 byte(s), first at offset 3
```

Проверки те же, что у `--set`, плюс: добавлена ровно одна строка и ровно один
CRLF, старые строки в том же порядке, финальный пустой CRLF остался последним,
`--check` без проблем. Пустой `--text` для добавления отвергается: пустая строка
ничего не добавляет (обнулить существующую — это `--set`).

## 7. Self-test и регрессия

```sh
python3 tools/editor/killedwrite.py --self-test          # 55/55, без файлов игры
python3 tools/editor/killedwrite.py --self-test --json   # то же в JSON
python3 regress.py --mirror --json                       # 115/115, 64.7 с
```

Пятьдесят пять проверок: идемпотентность split/join, хвост `CRLF CRLF`,
отсутствие финального CRLF, одиночные CR/LF, пустой файл, NUL, >64 КиБ,
регистр расширения, latin1, счёт плейсхолдеров; `--check` (структура, «нет
принятых», длинная строка, `--against` одинаковый/разный, не-ASCII, хвосты);
`--list`/`--accepted`; `--set` (1 байт, смена длины, смена accepted, пустой
текст, диапазон, CR/LF, NUL, не-ASCII, управляющий байт, 3 плейсхолдера,
2 плейсхолдера, >1022 Б, не-ASCII файл, `--out` в игру); `--prepend`/
`--append` (хвост, без пустой строки, без финального CRLF, диапазон, длинная
строка); защита `--out` (игра/зеркало/worktree/главный репозиторий/вход);
синтетические `--audit`/`--roundtrip`, LF-файл в `--roundtrip`, одиночный файл,
вспомогательные функции. Временный каталог — `/var/tmp` (не tmpfs), после
прогона удаляется.

`regress.py` не изменился и новых проверок не содержит: срез добавляет только
два новых файла, Python-оракул (`gm.py`, `gm/*`) и C++ не тронуты.

## 8. Ограничения

* **Число принятых строк может измениться.** Фильтр игры — длина 1..30 Б;
  правка, меняющая длину через границу 30, меняет и то, что загрузится
  (пример: 33 → 29 Б даёт 69 → 70). Инструмент считает это и **предупреждает**
  (`the accepted set changes: N -> M`), но не запрещает: это законная правка.
* **Предел 128 сообщений** (`0x80`). Если принятых строк станет больше 128,
  хвост массив не увидит; предупреждение печатается, но запись не блокируется
  (в поставке 69 и 50).
* **Только ASCII на запись.** `--text` с байтом `> 0x7E` — отказ; файл с
  не-ASCII байтами читается (`--check` покажет `encoding=latin-1` и байты), но
  `--set`/`--prepend`/`--append` его отвергают. Кодировка не конвертируется.
* **NUL и одиночные CR/LF — отказ при чтении.** `strlen` обрезал бы строку по
  NUL, а split/join не обратен при одиночном CR/LF. Инструмент не «чинит» такие
  файлы молча. LF-only файл C++-читатель принимает, но этот писатель его не
  правит — это сознательно.
* **Нет удаления и нет усечения.** Строку нельзя удалить, финальную пустую
  строку — убрать; `--set` с пустым текстом обнуляет строку, но оставляет её в
  файле (число строк и CRLF прежние).
* **`--text` длиннее 1022 Б — отказ.** `fgets(buf, 0x400)` читает максимум
  1023 байта с терминатором; более длинная строка пришла бы несколькими
  кусками, то есть лишними сообщениями.
* **Больше двух плейсхолдеров на строку — отказ.** Шаблоны называют убийцу и
  жертву (`%S KILLS %S.`); в поставке максимум 2 (`.skb`) и 1 (`.skk`), так что
  на реальных данных правило не срабатывает.
* **`--set LINE` — номер строки файла, а не индекс сообщения.** У `.skb` это
  1..92 (строка 92 — пустая), у `.skk` 1..74; принятых из них 69 и 50. Номер в
  массиве игры (`pick`) — другой; `--list --accepted` показывает соответствие.
* **`--against` — справка, не ворота.** Код возврата `--check` определяется
  структурой; позиционное число изменённых байт включает сдвинутый хвост (при
  вставке/смене длины). Для «размера правки» есть `changed` строки и `delta`.
* **Семантика не проверяется.** Дубликаты, орфография и смысл сообщений —
  данные; инструмент сохраняет их как есть и не «улучшает».
* **C++ остаётся только читателем.** `engine/src/ui/killed.*`, `main.cpp`,
  CTest и `gm/*` не менялись; инструмент не подключён к CLI движка и к
  регрессии. Новые файлы — только `tools/editor/killedwrite.py` и этот документ.
* **Игра, зеркало и оба корня репозитория защищены**: `--out` внутрь них —
  отказ (2), `--out`, равный входу, — тоже; всё остальное пишется атомарно
  (`path.tmp` + `os.replace`). Главный репозиторий можно переопределить
  переменной `GUNMETAL_REPO`.

## 9. Ссылки

`docs/FORMATS.md` §8.2 (`.skb`/`.skk` — текстовые таблицы сообщений);
`docs/KILLED.md` §1 (фильтр, числа поставки), §2 (модель и отказы), §3 (API и
числа C++), §4 (ограничения); `docs/SUB.md` §7 (загрузчики `0x485a00`/
`0x485b2f`, `0x485a7d`, `0x68ddb8`/`0x68ddb4`); `gm/small.py::parse_kill`;
`engine/src/ui/killed.{h,cpp}`; `engine/tests/test_killed.cpp`;
`tools/editor/sogwrite.py`/`docs/SOG_WRITE.md` и
`tools/editor/soawrite.py`/`docs/SOA_WRITE.md` (тот же приём);
`ROADMAP.md` `M11.5`.
