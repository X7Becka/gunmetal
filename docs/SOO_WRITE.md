# Запись `.soo`: побайтовый round-trip и правка `mode`/длительностей (`M11.5`, срез 6)

* **Статус:** срез готов. Round-trip **40/40** файлов поставки побайтово
  (3 416 Б, 264 кадра), свой читатель + сборка из строковых спанов
  **40/40**, семантический re-parse **40/40**, таймлайн движка после
  round-trip **40/40** загружаемых. Правка `Sobanim/Cultist.soo`
  (длительность кадра 2 = 70 тиков) меняет **1 байт** (52 → 52 Б), `--check`
  ок; правка с изменением ширины (`50` → `250`) даёт 53 Б (+1), `--check`
  ок. Self-test **50/50**. `python3 regress.py --mirror --json` —
  **115/115**, `failed: []` (61.7–81.0 с на двух прогонах).
* **Ветка:** `feat/soowrite`, база `master` `efa0ff6`, дата 2026-10-24.
* **Файлы:** `tools/editor/soowrite.py` (новый), `docs/SOO_WRITE.md` (этот
  документ). Существующие файлы (`gm/soo.py`, `gm/*.py`, `gm.py`,
  `tools/re/*`, `tools/editor/*`, `regress.py`, `ROADMAP.md`, `docs/*.md`,
  `engine/*`) **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx` (зеркало
  `~/.cache/gunmetal-re/game`). Результаты — в `/tmp/opencode/soowrite`
  (в git не попадают). C++-движок собран из этого worktree в
  `/var/tmp/gm-soowrite-build` (headless, `-DGM_WITH_SDL3=OFF`), в репозиторий
  ничего не писалось.
* **Оракул:** `gm/soo.py` (`parse`, `Soo.to_bytes`, `timeline`, `frame_at`),
  раскладка — `docs/SOO.md` §1, загрузчик/плеер — `docs/SOO.md` §2–§3,
  строка формата — `docs/FORMATS.md` §8.1.

> **Число файлов.** В поставке v1.36 ровно **40** `.soo`: 38 с расширением
> `.soo` и 2 с `.SOO` (`DISRUPT.SOO`, `NUKEring.SOO`), все в `Sobanim/`.
> Обход корня игры рекурсивный и регистронезависимый по расширению.

---

## 1. Раскладка и почему правка байтовая

`.soo` — обычный текст DOS (CRLF): два числа заголовка, затем по строке
`<имя позы> <длительность>` (`docs/SOO.md` §1, `docs/FORMATS.md` §8.1):

```
0
3
BigGate1 200
BigGate2 6000
BigGate3 10
```

Что реально читает загрузчик `0x42b4e1` (доказано, `docs/SOO.md` §2):

* `fscanf(f, "%ld %ld")` — ровно два числа `mode count` (`0x42b5c2`);
* `1 <= count <= 0x20` (`0x42b5f1`); записей `.soo` не больше `0x200`;
* цикл `i = 0..count-1`: `fscanf(f, "%s %ld")` (`0x42b657`), затем **файл
  закрывается** — строки после `count` не читаются вообще (`0x42b637`);
* все позы должны иметь одинаковые число вершин `+0x68` и граней `+0x6a`
  (`0x42b71b`), иначе ошибка всей анимации.

Плеер `0x433e00` (`docs/SOO.md` §3) знает только `mode` 0 и 1:

| `mode` | поведение | адрес |
|---|---|---|
| `0` | one-shot: `t >= total` прижимается к `total-1`, последний кадр удерживается | `0x433e8b` |
| `1` | loop: `t %= total`, с последнего кадра интерполируется в первый | `0x433eb4` |
| другое | нормализации нет, выбор кадра выходит за массив | — |

Длительности — **тики 91 Гц**, не миллисекунды (`0x44db8d`), и в плеере нет
ни одного деления на частоту.

### Почему splice, если `to_bytes()` уже даёт 40/40

`gm/soo.py` хранит раскладку флагами (`combined`, `eol`, `trailing`,
`blanks`), поэтому `parse -> to_bytes` побайтово обратен для **всех 40**
поставочных файлов. Но это работает только потому, что в корпусе нет
«неканонических» разделителей:

* `bamfin`, `bamfrays`, `icebamf` — `mode count` в одной строке (3 файла);
* `BigGate`, `keldorc`, `keldoro` — без перевода строки в конце (3 файла);
* пустые строки в конце — 14 штук в 13 файлах (у `kingarm` их две);
* `mode`/`count` всегда на отдельных строках, кроме трёх файлов выше.

`Soo.to_bytes()` собирает строку как `"%s %d"` с одним пробелом, поэтому
табы, несколько пробелов, хвостовые пробелы и пустые строки **между**
кадрами он бы нормализовал (это отдельная проверка self-test'а). Кроме того,
пересборка из полей не даёт точечной правки: нельзя доказать, что изменился
только один токен.

Поэтому инструмент правит **байты целевого токена**, а всё остальное
(CRLF, комбинированный заголовок, пустые строки, отсутствие финального
перевода строки, разделители) остаётся на месте. Длина файла меняется
только на разницу ширины числа (`50` → `70` — 0 байт, `50` → `250` — +1).

Редактируемые поля — только доказанные и однозначные:

| поле | токенов | смысл | диапазон |
|---|---:|---|---|
| `mode` | 1 | 0 — one-shot, 1 — loop (`0x433e8b`/`0x433eb4`) | 0..1 |
| `durN` | 1 | длительность кадра `N` (0-based) в тиках 91 Гц | 1..65535 (в поставке 5..6000) |

Не редактируются осознанно:

* `count` — это число поз, которые читает загрузчик (`0x42b5f1`/`0x42b637`);
  его изменение добавляет или убирает кадры, а не «поправляет число»;
* имена кадров — ссылки на отдельные файлы `SOBS/<name>.sob`; переименование
  меняет, какую модель грузить, и не является правкой числа;
* число кадров/строк и создание новой анимации — вне среза.

Верхняя граница `65535` на длительность держит сумму по 32 кадрам
(`32 × 65535 = 2 097 120`) внутри `int32`, который использует плеер.

## 2. Что доказано round-trip'ом

```sh
python3 tools/editor/soowrite.py --roundtrip
```

```
roundtrip: 40/40 identical, 3416 bytes, 0.295 s (/mnt/d/games/Gunmetal 3dfx)
  own reader: 40/40 parsed, line-span re-join 40/40
  read from disk after write: 40/40
  semantic re-parse (parse -> to_bytes -> parse): 40/40
  engine timeline agrees after the round-trip: 40/40 loadable file(s)
result: OK
```

Каждый файл: читается своим разборщиком (строки + байтовые спаны), собирается
из спанов обратно, пишется оракулом (`Soo.to_bytes()`) в scratch-каталог,
читается с диска и сравнивается с оригиналом побайтово. Затем результат
разбирается оракулом заново и сравнивается таймлайн.

| метрика | результат |
|---|---|
| найдено `.soo` (рекурсивно от корня игры, регистр расширения) | 40 |
| **побайтово идентичны после read → write → read** | **40 / 40** (3 416 Б) |
| свой разборщик доходит до конца | 40 / 40 |
| сборка файла из строковых спанов (`rejoin`) | 40 / 40 |
| семантический round-trip (`parse → to_bytes → parse`) | 40 / 40 |
| таймлайн: свой расчёт == `Soo.timeline()` | 40 / 40 грамматически загружаемых |
| время | 0,2–0,3 с на `/mnt/d`, 0,006 с на зеркале (`--mirror`) |

`rejoin` — отдельное доказательство, что строковые спаны полны и не
пересекаются: файл собирается из `data[..line.start] + line.text + line.eol`
и обязан совпасть побайтово. На этом стоит байтовая правка. Строки режутся по
CRLF/LF/CR с сохранением терминатора, поэтому «битая» раскладка не
«нормализуется» молча.

«40/40 загружаемых» здесь означает грамматику загрузчика (count в 1..32,
строк не меньше count, mode 0/1). Отсутствие `.sob` у 8 кадров и
несовместимость топологии поз — отдельные проверки, см. §6; они не мешают
round-trip'у текста.

## 3. Как инструмент правит файл

1. `split_lines` + `tokenize_line` дают **байтовые спаны**: токены заголовка
   (mode, count) и оба токена каждой строки кадра (имя, длительность).
   Разбиение — по `str.isspace()`, как `str.split()` оракула; latin-1 = один
   байт на символ.
2. `walk` проверяет грамматику: заголовок — либо две строки по одному числу,
   либо одна строка `mode count`; каждая непустая строка после заголовка —
   ровно два токена; длительности — целые; пустые строки допускаются только
   в конце (иначе `ScanError`, потому что оракул их потеряет).
3. `resolve_field` превращает `mode|count|durN` в конкретный токен
   (`dur[N]`, `durationN`, `frameN` — синонимы).
4. `validate` сверяет с диапазоном: `mode` 0/1, `duration` 1..65535, `count`
   отвергается всегда.
5. `splice` собирает файл из исходных байтов, подменяя только диапазон
   целевого токена.
6. `verify_edit` (13–14 проверок в зависимости от поля, все обязательны):
   * свой разборщик и оракул читают новый файл;
   * изменение размера равно разнице ширины токена;
   * число строк не изменилось, раскладка заголовка (combined/split) та же;
   * второй токен заголовка и все имена кадров идентичны;
   * все остальные длительности идентичны;
   * **все байты вне целевого токена идентичны** (включая разделители и
     сдвинутый хвост при изменении ширины);
   * свой разборщик и оракул видят записанное значение;
   * для `mode` — флаг `loop` оракула следует за значением, а свой таймлайн
     равен оракульному;
   * для длительности — суммарная длительность движка меняется ровно на
     дельту (для кадра после `count` — на 0, он не читается), а свой таймлайн
     равен оракульному;
   * число изменённых байт посчитано и напечатано.
7. Файл пишется только после успешной проверки. Запись атомарная
   (`mkstemp` в каталоге цели + `fsync` + `os.replace`).

`changed_bytes` заново разбирает результат и считает разницу по тексту
токена, а не по значению, которое «хотели» записать. При изменении ширины
числа хвост файла сдвигается; `--check` отдельно печатает число байт в
токене (`'50' -> '250'`, 3 байта) и сдвиг файла (`+1`), а не выдаёт сдвиг
за «17 изменённых байт».

## 4. CLI

```sh
python3 tools/editor/soowrite.py --list FILE [--limit N] [--json]
python3 tools/editor/soowrite.py --get FILE --field NAME [--json]
python3 tools/editor/soowrite.py --set FILE --field NAME=VALUE --out OUT [--json]
python3 tools/editor/soowrite.py --check FILE [--against ORIG] [--field NAME=VALUE]
                                     [--strict] [--json]
python3 tools/editor/soowrite.py --roundtrip [DIR] [--mirror] [--verbose] [--json]
python3 tools/editor/soowrite.py --audit [DIR] [--mirror] [--json]
python3 tools/editor/soowrite.py --self-test [--json]
```

`FILE` — путь или имя (`Cultist`, `Cultist.soo`, `DISRUPT.SOO` — ищется в
`Sobanim/`; регистр расширения не важен). `--game` по умолчанию
`/mnt/d/games/Gunmetal 3dfx` (или `$GUNMETAL`). `--roundtrip`/`--audit` без
аргумента обходят корень игры рекурсивно; явный `DIR` заменяет его,
`--mirror` подставляет зеркало.

`--check` без `--against` сам ищет поставочный файл с тем же именем
(`Sobanim/<name>.soo`): для поставочного файла сравнение пропускается с
пометкой, для копии в `/tmp` — сравнивается (поэтому для проверки правки
нужен `--field VALUE`, иначе файл законно отличается). Относительный путь
`Sobanim/x.soo` тоже подхватывается.

Коды возврата: `0` успех, `1` проверка не прошла, `2` неверный ввод/отказ
(сообщение в stderr), `3` внутренняя ошибка. Трассировка наружу не выходит.

Отказы (проверены self-test'ом и вручную):

```
$ python3 tools/editor/soowrite.py --set Cultist --field dur0=10 \
      --out "/mnt/d/games/Gunmetal 3dfx/Sobanim/x.soo"
soowrite: refusing to write inside the game installation: … (write to /tmp/opencode or /var/tmp)
# exit 2 — то же для зеркала (~/.cache/gunmetal-re/game) и репозитория
# (worktree и ~/gunmetal-re)

$ … --set Cultist --field count=3 --out /tmp/opencode/soowrite/x.soo
soowrite: count is not editable: it is the number of frames the loader reads
(0x42b5f1/0x42b637), so changing it adds or removes poses
# exit 2

$ … --set Cultist --field mode=2 --out /tmp/opencode/soowrite/x.soo
soowrite: mode=2: the player normalises only 0 (one-shot, 0x433e8b) and
1 (loop, 0x433eb4); any other value runs past the frame array
# exit 2

$ … --set Cultist --field dur0=0 --out …
soowrite: duration=0: the player divides by the frame duration (0x433f24)
and its selection loop never leaves the frame; use 1..65535 ticks
# exit 2

$ … --set Cultist --field dur9=100 --out …
soowrite: frame 9: the file lists 4 frame(s) (0..3)
# exit 2

$ … --set Cultist --field mode=1 --out "…/Sobanim/Cultist.soo"
soowrite: --out is the input file; pick a different path
# exit 2

$ … --set Cultist --field dur0=10 --out /tmp/opencode/soowrite/x.txt
soowrite: --out must be a .soo path: /tmp/opencode/soowrite/x.txt
# exit 2
```

Дополнительно отвергаются: не `.soo` на вход/выход, пустой файл, заголовок
не из двух чисел, строка кадра не из двух токенов, нецелая длительность,
`mode`/`count`/длительность вне 32-битного поля, индекс кадра за списком,
неизвестное поле. Битая раскладка (пустая строка между кадрами) читается, но
`--check` предупреждает, что оракул её потеряет.

## 5. Правка на реальных данных: `Sobanim/Cultist.soo`

Файл (52 Б): `mode=1`, `count=4`, кадры `cult1a 50`, `cult1a 150`,
`cult1b 50`, `cult1b 150`; сумма 400 тиков ≈ 4.40 с. Меняем длительность
кадра 2 (0-based) с 50 на 70 тиков:

```sh
python3 tools/editor/soowrite.py --set Cultist --field dur2=70 \
    --out /tmp/opencode/soowrite/Cultist-dur70.soo
```

```
/mnt/d/games/Gunmetal 3dfx/Sobanim/Cultist.soo -> /tmp/opencode/soowrite/Cultist-dur70.soo
dur2: 50 -> 70 (52 -> 52 bytes, delta +0)
  [ok  ] own reader parses the edited file (6 line(s), 4 frame(s))
  [ok  ] oracle parses the edited file to EOF
  [ok  ] size change equals the token width change (52 -> 52 bytes (+0))
  [ok  ] line count is unchanged (6 line(s))
  [ok  ] the header layout is unchanged (split)
  [ok  ] every other header token is identical (mode/count)
  [ok  ] the frame name tokens are identical (4 name(s))
  [ok  ] every other frame duration is identical (4 duration(s))
  [ok  ] bytes outside the target token are identical
  [ok  ] own reader sees the new value (70)
  [ok  ] oracle sees the new value (70)
  [ok  ] the engine total changes by exactly the duration delta (400 -> 420 ticks (+20))
  [ok  ] own timeline equals the oracle timeline after the edit (4 interval(s))
  [ok  ] changed-byte count is reported (1 byte(s))
wrote /tmp/opencode/soowrite/Cultist-dur70.soo
result: OK
```

`--check` против оригинала:

```sh
python3 tools/editor/soowrite.py --check /tmp/opencode/soowrite/Cultist-dur70.soo \
    --against "/mnt/d/games/Gunmetal 3dfx/Sobanim/Cultist.soo" --field dur2=70
```

```
check: /tmp/opencode/soowrite/Cultist-dur70.soo (52 bytes)
  [ok  ] line scanner re-joins the file byte-exactly (6 line(s), final EOL b'\r\n')
  [ok  ] the header is two decimal integers (mode 1, count 4 (split header))
  [ok  ] the declared count is within the loader limit (4 (loader: 1..32, 0x42b5f1))
  [ok  ] own header equals the oracle header (1 4)
  [ok  ] own frames equal the oracle frames (4 frame(s))
  [ok  ] own engine timeline equals the oracle timeline (4 interval(s), 420 ticks)
  [ok  ] the file is CRLF-only (0 bare LF, 0 bare CR)
  [ok  ] every frame line holds one 'name duration' pair (4 frame line(s))
against: /mnt/d/games/Gunmetal 3dfx/Sobanim/Cultist.soo (52 bytes)
  [ok  ] name token counts match the source (4 vs 4)
  [ok  ] line count and line endings match the source (6 vs 6 line(s))
  [ok  ] the file is exactly the source with the field patched (size differs by +0 byte(s), first diff at offset 36; aligned byte diff 1 (a width change shifts the tail))
  [ok  ] the field carries the expected value (dur2 = 70)
  [ok  ] only the target token differs (dur2 only, layout split -> split)
field: dur2 = 70, token '50' -> '70' (1 byte(s) in the token, file +0 byte(s))
result: OK
```

Изменён ровно один байт — `cmp -l` (позиции 1-based):

```
$ cmp -l "…/Sobanim/Cultist.soo" /tmp/opencode/soowrite/Cultist-dur70.soo
37  65  67      # '5' -> '7'
```

Токен `dur2` занимает байты `36..38` (0-based) — это и есть `50` после
`cult1b `; имена, CRLF и размер не тронуты.

**Правка с изменением ширины** (`50` → `250`, +1 байт):

```sh
python3 tools/editor/soowrite.py --set Cultist --field dur2=250 \
    --out /tmp/opencode/soowrite/Cultist-dur250.soo
# dur2: 50 -> 250 (52 -> 53 bytes, delta +1)
#   token 36..38: '50' -> '250' (3 byte(s) changed)
#   [ok  ] bytes outside the target token are identical
# result: OK
```

`cmp -l` показывает сдвиг хвоста (все байты после вставки сместились на 1),
но `--check` доказывает, что хвост побайтово тот же:

```
  [ok  ] the file is exactly the source with the field patched (size differs by +1 byte(s), first diff at offset 36; aligned byte diff 17 (a width change shifts the tail))
  [ok  ] only the target token differs (dur2 only, layout split -> split)
field: dur2 = 250, token '50' -> '250' (3 byte(s) in the token, file +1 byte(s))
result: OK
```

**Правка `mode`** (`1` → `0`, 1 байт): `--set Cultist --field mode=0` даёт
52 Б, `0x433e8b`-поведение (удержание последнего кадра); проверка печатает
`the engine loop flag follows the mode (loop=False)`.

**Таймлайн до/после** (`--list` правленого файла):

```
mode: 1 (loop), declared count: 4, frames listed: 4
engine: reads 4 frame(s), 420 ticks = 4615.4 ms; layout split header, blank lines 0
  #  name                  ticks      ms   start ms  engine  sob
  0  cult1a                   50   549.5       0.0    yes  ok
  1  cult1a                  150  1648.4     549.5    yes  ok
  2  cult1b                   70   769.2    2197.8    yes  ok
  3  cult1b                  150  1648.4    2967.0    yes  ok
```

У оригинала кадр 2 — `50` тиков (549.5 мс), кадр 3 начинается на 2747.3 мс;
у правленого — 70 тиков, кадр 3 начинается на 2967.0 мс. Python-оракул
согласен: `python3 gm.py soo …/Cultist.soo` → `total=400 ticks`,
`gm.py soo /tmp/.../Cultist-dur70.soo` → `total=420 ticks`.

## 6. `--audit`: что в корпусе (40 файлов)

```
audit: 40 files, 3416 bytes, 264 frames (261 read by the engine), 0.097 s (/mnt/d/games/Gunmetal 3dfx)
  modes: 0=26, 1=14
  declared count: min 2, median 4.5, max 29; 12 distinct; top 4=8, 2=6, 3=6, 5=6, 10=4, 12=3
  frames/file: min 2, median 4.5, max 29; distribution 2=6, 3=6, 4=8, 5=5, 6=2, 9=3, 10=4, 12=3, 16=1, 17=1, 29=1
  durations (ticks): min 5, max 6000, 32 distinct; top 10=44, 20=33, 6=20, 30=19, 40=18, 50=16
  total ticks: 20107 listed, 19987 read (219.6 s)
  size: min 28, median 57.5, max 354 bytes; total 3416
  layout: combined header 3, split 37; final EOL 37, without 3; blank lines 14 in 13 file(s) (interior 0); padded lines 0; bare LF/CR 0/0
  count vs listed: equal 37, short 0, dead lines 3 file(s)/3 frame(s)
    dead: Sobanim/nblast.soo
    dead: Sobanim/nbloom.soo
    dead: Sobanim/ruinbeam.soo
  pose models missing (no SOBS/<name>.sob): 4 file(s), 6 name(s) in 8 frame slot(s): BigGate3, bamfa, bamfb, bamfc, dev1a, dev1b
    Sobanim/BigGate.soo, Sobanim/Dev1.soo, Sobanim/bamfin.soo, Sobanim/tester.soo
  loadable: 36/40 (grammar + pose models resolved)
  names: 264 tokens, 175 distinct; repeated within a file: Sobanim/Cloud.soo, … (13 file(s))
result: OK
```

Расшифровка чисел:

* **3 416 Б** — сумма размеров 40 файлов; самый маленький 28 Б (`Dev1`,
  `Fire`, `TelepIn`, `tester`), самый большой 354 Б (`vparm`, 29 кадров),
  медиана 57.5 Б.
* **264 кадра** в тексте, **261** читает движок: `nblast`/`nbloom` (8 при 9
  строках) и `ruinbeam` (5 при 6) объявляют меньше, чем перечислено; лишние
  строки — мёртвые (`0x42b637`). Случаев `count > строк` в поставке нет.
* **mode**: 26 one-shot / 14 loop, других значений нет.
* **Длительности** — 32 различных значения, от 5 до 6000 тиков; чаще всего
  10 (44 кадра), 20 (33), 6 (20), 30 (19), 40 (18), 50 (16). Сумма
  объявленных длительностей 20 107 тиков, движок играет 19 987 (219.6 с).
* **Раскладка**: 3 файла с `mode count` в одной строке, 3 без финального
  CRLF, 13 файлов с пустыми строками в конце (14 строк), хвостовых пробелов
  и bare LF/CR нет; пустых строк между кадрами нет.
* **Позы**: 175 различных имён на 264 слота (в 13 файлах имя повторяется —
  это нормально для flipbook'а); 6 имён в 8 слотах не находят
  `SOBS/<name>.sob`, поэтому 4 анимации не загрузятся: `BigGate`, `Dev1`,
  `bamfin`, `tester`.
* **loadable 36/40** здесь = грамматика загрузчика + наличие всех `.sob` у
  кадров. `docs/SOO.md` §4 даёт 33/40, потому что дополнительно проверяет
  совместимость топологии поз (`+0x68`/`+0x6a`, `0x42b71b`): `Fire`,
  `keldorc`, `keldoro` падают именно на ней. Топологию `.sob` этот
  инструмент не разбирает (см. §8).

## 7. Читает ли движок

C++-читатель `.soo` в этой базе уже есть: `gm::parse_soo`
(`engine/src/res/anim.cpp:66`), `--dump` (`engine/src/main.cpp:583`) и плеер
`gm::soo_pose_at` с морфингом (`0x433e00`-паритет, `--anim NAME --at MS`).
C++-код **не менялся**; проверка — собранный из этого worktree headless
бинарь (`/var/tmp/gm-soowrite-build/engine/gunmetal`, SDL3 OFF).

`--dump` (печатает заголовок и первый кадр):

```sh
B=/var/tmp/gm-soowrite-build/engine/gunmetal
$B --dump "…/Sobanim/Cultist.soo"
$B --dump /tmp/opencode/soowrite/Cultist-dur70.soo
```

```
file: /mnt/d/games/Gunmetal 3dfx/Sobanim/Cultist.soo
size: 52 bytes
head: 31 0D 0A 34 0D 0A 63 75 6C 74 31 61 20 35 30 0D 0A …
ascii: 1..4..cult1a 50..cult1a 150..cul
kind: SOO model animation
mode: 1
declared frames: 4
frames: 4
first: cult1a 50 ticks
```

У правленого файла тот же вывод, кроме `file:` (и `size: 52 bytes` для
`dur70`, `53 bytes` для `dur250`) — `--dump` печатает только первый кадр.

`--dump` показывает только первый кадр, поэтому правку кадра 2 проверяем
плеером на `--at 3200` мс (в этой точке кадр 3 морфится `cult1b → cult1a`):

```sh
# оригинал (настоящий каталог игры)
$B -v --game "/mnt/d/games/Gunmetal 3dfx" \
   --anim Cultist --at 3200 --render /tmp/opencode/soowrite/o32.png
# правленый файл: scratch-каталог, где Sobanim/Cultist.soo — копия правки,
# остальные каталоги игры — симлинки (писать в игру нельзя)
$B -v --game /tmp/opencode/soowrite/game-tmp \
   --anim Cultist --at 3200 --render /tmp/opencode/soowrite/e32.png
```

```
[info] anim: mode=1 frame=3->0 alpha=0.273333 at=3200ms     # оригинал, 400 тиков
[info] anim: mode=1 frame=3->0 alpha=0.14 at=3200ms         # правка, 420 тиков
sha256  39f5512b…  o32.png
sha256  486fb78e…  e32.png            # картинки различаются
```

То есть движок читает новый файл и его плеер видит новую длительность
(кадр 3 сдвинулся, alpha на том же времени изменилась с 0.273 на 0.14).
Для контроля: на времени, где правка не должна ничего менять (например
`--at 2000`, кадр 1), alpha у обоих `0.88`, а PNG совпадают побайтово
(не приводится; проверено).

## 8. Ограничения

* **Правятся только `mode` и длительности кадров.** `count`, имена поз,
  число строк и создание новой анимации — нет: первые два меняют смысл
  загрузки, последние — новый файл, а не правку.
* **Топология поз не проверяется.** Загрузчик требует одинаковые
  `+0x68`/`+0x6a` у всех кадров (`0x42b71b`); для этого нужен разбор
  `SOBS/*.sob` (`gm/sob.py`), он вне среза. `--audit` проверяет только
  наличие файла позы по имени, поэтому его `loadable` (36/40) выше, чем в
  `docs/SOO.md` §4 (33/40).
* **Проверка наличия `.sob` требует каталога игры.** Без `--game` (или если
  его нет) `--audit`/`--list` работают в режиме «только грамматика» и
  печатают это явно (`sob: ?`, `loadable_note`).
* **Round-trip доказывается через `Soo.to_bytes()`** — собственного
  сериализатора у инструмента нет; его вклад — точные спаны (`rejoin`) и
  байтовый splice. `to_bytes()` уже обратен для всех 40 поставочных файлов,
  но нормализует табы, лишние пробелы, хвостовые пробелы и пустые строки
  между кадрами (self-test); поэтому правка идёт мимо него.
* **`mode` вне 0/1** читается и показывается, `--check` выдаёт warning (и
  `--strict` — код 1); `--set mode=0|1` может такой файл починить. В
  поставке таких нет.
* **Мёртвые строки после `count`** сохраняются как есть: правка длительности
  такого кадра меняет файл, но не поведение (движок его не читает), и
  проверка честно ждёт дельту 0.
* **В игре поведение не проверялось** (игра только на чтение). Проверены
  Python-оракул и C++-парсер/плеер на реальном файле; рендер-уровень `.soo`
  использует `--anim`, отдельного «уровня с анимацией» в срезе нет.
* **JSON** — один объект на режим (`--json`), без схемы-версии; поля
  перечислены в коде (`roundtrip`, `audit`, `check`, `list`, `get`, `set`,
  `self-test`).

## 9. Воспроизведение

```sh
cd /home/nooverload/build/gm-sub-soowrite
python3 tools/editor/soowrite.py --self-test            # 50/50, без игры
python3 tools/editor/soowrite.py --roundtrip           # 40/40, /mnt/d
python3 tools/editor/soowrite.py --roundtrip --mirror  # 40/40, зеркало
python3 tools/editor/soowrite.py --audit               # числа §6
python3 tools/editor/soowrite.py --set Cultist --field dur2=70 \
    --out /tmp/opencode/soowrite/Cultist-dur70.soo
python3 tools/editor/soowrite.py --check /tmp/opencode/soowrite/Cultist-dur70.soo \
    --against "/mnt/d/games/Gunmetal 3dfx/Sobanim/Cultist.soo" --field dur2=70
python3 regress.py --mirror --json                     # 115/115, 61.7–81.0 с, failed: []
```

`--self-test` не обращается к игре: все раскладки (CRLF, combined, без
финального EOL, пустые строки, LF-only, CR-only, табы, несколько пробелов),
патчи, отказы и end-to-end `--set`/`--check` строятся из синтетических байтов
во временном каталоге.
