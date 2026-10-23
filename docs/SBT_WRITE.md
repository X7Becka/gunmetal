# Писатель контейнера `.sbt` (`M11.4`, часть 1)

* **Статус:** готово: побайтовый round-trip контейнера (36/36), замена одного
  подскрипта `SCRIPT` собранным текстом с пересчётом `m` и смещений
  подскриптов, проверка `--check` и `--verify-replace` на `level0`/`dm1`/
  `level24`.
* **Ветка:** `feat/sbtwrite`, база `master` `da2fac3`, дата 2026-10-23.
* **Файлы:** `tools/editor/sbtwrite.py` (новый), `docs/SBT_WRITE.md` (этот
  документ). `gm/sbt.py`, `gm/sbtvm.py`, `tools/editor/sbtasm.py`, тесты и
  прочие существующие файлы **не менялись**.
* **Игра:** только чтение, `/mnt/d/games/Gunmetal 3dfx`. Результаты — в
  `/tmp/opencode/sbtwrite` (не в git).
* **Эталон (оракул):** `gm/sbt.py` (`parse_file`), ассемблер/рендер —
  `tools/editor/sbtasm.py` (`render_exact`/`assemble`), формат — `docs/SBT_ASM.md`,
  `docs/FORMATS.md` §12/§12.3/§12.4.

## 1. Контейнер по фактам парсера

Заголовок — 84 байта (`gm/sbt.py:113-115`, `:758-767`):

```
0x00  char magic[80]   b"BINARY SAVE\0" + нули      (MAGIC, gm/sbt.py:113)
0x50  u32  version     должен быть 0                 (parse, :763-767)
```

Дальше — **поток записей без таблицы смещений и без длин**: `u8 type` +
нагрузка, которую обработчик этого типа вычитывает целиком; `255`
(`END_OF_FILE`) нагрузки не имеет и является последним байтом
(`gm/sbt.py:22-25`, `:768-786`, таблица типов `:127-145`).

| id | имя | нагрузка (из `gm/sbt.py:647-741`) |
|---|---|---|
| 4 | `THING_TYPES` | `u32 n<=0x80`; `n*936` (`name[16]` + 49 `double` + 528 байт) |
| 5 | `NAMED_ITEMS` | `u32 n<=0x31`; `n*52` (`name[16]` + 9 `u32`) |
| 11 | `SCRIPT` | `u32 c<=0xC8` (`c*40`) + `u32 n<=0x400` (3 колонки `u32[n]`) + `u32 m<=0x30000` (`m` байт) + `u8 flag` (+`9*0x400`) |
| 12 | `SOUND_SAMPLE_TABLE` | `300*u32` + `300*9` байт имён (`0x4b0+0xa8c`) |
| 15/16/17 | `POLY_/OBJECT_/SECTOR_HANDLE` | `u32 n` + 4/3/2 параллельные колонки `u32[n]` |
| 20 | `SOB_TABLE` | `u32 n<=0xC8`; `n*20` (`name[12]` + `u32 amount` + `u32 ptr`) |
| 255 | `END_OF_FILE` | — |

Остальные типы (`1,2,3,6,7,8,9,18,19`) в поставочных `.sbt` не встречаются
(`docs/FORMATS.md` §12), но писатель их поддерживает наравне: он не
интерпретирует нагрузку, а переносит её байты.

Фактическая последовательность в поставке (проверено здесь по всем 36 файлам):

| вариантов | последовательность |
|---:|---|
| 27 | `12, 20, 15, 16, 17, 5, 4, 11, 255` |
| 5 | `12, 20, 15, 17, 5, 4, 11, 255` |
| 2 | `12, 20, 15, 5, 4, 11, 255` |
| 1 | `12, 20, 16, 17, 5, 4, 11, 255` |
| 1 | `12, 20, 15, 16, 5, 4, 11, 255` |

То есть записей `15/16/17` может не быть вовсе — писатель ни на какой порядок
и состав не опирается, кроме «есть ровно одна запись 11».

**Почему round-trip точен.** `Sbt.header` хранит 84 байта заголовка, а
`Record.raw` — нагрузку записи дословно; `to_bytes()` печатает
`header + type + raw` (`gm/sbt.py:490-515`). Никакой нормализации полей при
записи не происходит, поэтому сохраняются и «хвосты» имён в `THING_TYPES`,
и устаревшие указатели `SOB_TABLE`, и теги handle-таблиц, и любые паддинги.
`--roundtrip` дополнительно печатает «structural-encoder drift» — число
записей, у которых структурный кодировщик `encode_payload`
(`gm/sbt.py:792-903`) не совпал бы с сырыми байтами. На поставке таких **0**
(277/277 записей с нагрузкой; ещё 36 — `END_OF_FILE`), но даже при ненулевом drift писатель не потеряет байты:
он всегда пишет `raw`.

## 2. Запись `SCRIPT` и что пересчитывается при замене блока

Раскладка (`gm/sbt.py:333-371`, парсер `:692-701`, `docs/FORMATS.md` §12.3):

```
u32 count;  count*40 things;          ; "things" записи (в поставке count=0)
u32 n;      n*4 col0; n*4 col1; n*4 col2;
u32 m;      m code;                   ; байткод, gm/sbtvm.py
u8  flag;   flag ? 9*0x400 : 0;
```

Колонка 2 — **смещения подскриптов внутри буфера кода** (`docs/FORMATS.md`
§12.4, «Таблица сущностей», колонка 2 → глобал `0x616658`); так же её читает
`sbtasm.block_starts` (`tools/editor/sbtasm.py:996-1004`). Подскрипт
заканчивается на своём `END` (0xFF), поэтому границы блоков вычисляются
линейным проходом (`sbtasm.block_spans`, `:1006-1030`).

При замене блока `[off, end)` на байты `new` длины `L` писатель делает ровно
следующее (`replace_block`, `sbtwrite.py`):

1. `code' = code[:off] + new + code[end:]`;
2. `SCRIPT.m = len(code')` — загрузчик читает `m` и ровно `m` байт
   (`0x462a80`; `gm/sbt.py:695-700`);
3. каждая запись колонки 2 со значением `>= end` сдвигается на
   `delta = L - (end - off)`; запись, попавшая **внутрь** заменяемого
   диапазона, считается пересечением блоков и отвергается;
4. `raw`/`size` записи 11 пересобираются `Sbt.set_payload`
   (`gm/sbt.py:554-558`); размер файла меняется на `delta`, потому что
   смещений в контейнере нет;
5. всё остальное **не трогается и проверяется**: `count`, `n`, `flag`,
   `things`, `tables`, колонки 0/1, сырые байты всех прочих записей, байты
   кода вне заменённого диапазона.

Проверка нового блока до записи (`validate_block`): непустой, последний байт
`0xFF`, `gm.sbtvm.decode` доходит до конца, ровно один `END` и только
последний. Это исключает «полуинструкции» и хвосты после `END`.

## 3. Ограничения и инварианты

| ограничение | значение | источник |
|---|---|---|
| длина всего кода | `m <= 0x30000` | `gm/sbt.py:697`, `gm/sbtvm.py:338` (`MAX_BLOCK`) |
| число things | `count <= 0xC8` | `gm/sbt.py:693` |
| число подскриптов | `n <= 0x400` | `gm/sbt.py:695` |
| версия контейнера | `0` | `gm/sbt.py:763-767` |
| таблицы `DEFINE_*` | `9*0x400` только при `flag != 0` | `gm/sbt.py:699-700` |

Проверки, которые `--check`/запись считают обязательными: `m == len(code)`,
`len(things) == 40*count`, `len(handles) == 12*n`, колонка 2 отсортирована,
уникальна, начинается с 0, все подскрипты идут встык и покрывают код ровно
(0…`len(code)`), `to_bytes()` совпадает с файлом побайтово, `size` каждой
записи = `1 + len(raw)`, поток заканчивается `END` на последнем байте.

Проверено на всём корпусе: **15 061** подскрипт, и **каждый** декодируется
отдельно (0 ошибок), а **ни один** `ELSE` не целится за конец своего блока
(0 из 11 194 вхождений). Поэтому замена блока на самодостаточный байткод не
ломает `IF`/`ELSE`-структуру соседей.

## 4. CLI

```
python3 tools/editor/sbtwrite.py --roundtrip [--game DIR] [--out DIR] [--json] [--verbose]
python3 tools/editor/sbtwrite.py --script LEVEL --block OFF (--asm FILE | --text TEXT)
                                 --out OUT.sbt [--plan FILE] [--expect FILE] [--json]
python3 tools/editor/sbtwrite.py --check FILE [--expect FILE] [--json]
python3 tools/editor/sbtwrite.py --verify-replace [--game DIR] [--out DIR] [--json]
python3 tools/editor/sbtwrite.py --self-test
```

* `--script` принимает путь или имя уровня (`dm1`, `level0`, …) — резолв как в
  `sbtasm.level_path`.
* `--block` — смещение подскрипта (`0x2b07` или десятичное). Смещение обязано
  быть входом из колонки 2 (главный блок — 0).
* `--asm FILE` — текст в диалекте `render_exact`/`assemble`
  (`docs/SBT_ASM.md` §2). `--text TEXT` — тот же текст строкой; `--text @FILE`
  читает файл.
* `--plan FILE` — сохранить структурный дайджест результата; тот же JSON
  печатается в `--json`, поэтому его можно сразу передать в
  `--check --expect`.
* `--check` без `--expect` проверяет внутреннюю согласованность и
  round-trip; с `--expect` сравнивает типы/размеры/sha256 всех записей и все
  поля `SCRIPT` (включая sha256 каждого блока).

Коды возврата: `0` — успех, `1` — проверка не прошла, `2` — ошибка
входа/аргументов (текст в stderr), `3` — внутренняя ошибка. Исключения наружу
не выходят.

### 4.1 Python API

```python
from tools.editor import sbtwrite as W
from gm import sbt as sbtmod

sbt = sbtmod.parse_file("dm1.sbt")

# 1. Правка без I/O: исходный объект не меняется (deep copy внутри).
out, rep = W.replace_block(sbt, 0x2b07, assembled_bytes)
print(rep["delta"], rep["shifted_offsets"])

# 2. Правка + запись + обратное чтение оракулом + проверка.
rep = W.write_replacement("dm1.sbt", 0x2b07, assembled_bytes, "/tmp/dm1.sbt")
assert rep["ok"], rep["problems"]

# 3. Проверка файла и сравнение со структурным планом.
plan = W.plan_digest(sbtmod.parse_file("/tmp/dm1.sbt"))
print(W.check_file("/tmp/dm1.sbt", expect=plan)["ok"])

# 4. Вспомогательное: валидация блока и drift структурного кодировщика.
W.validate_block(assembled_bytes)
print(W.encoding_drift(sbt))
```

`replace_block` возвращает `(новый Sbt, отчёт)` и бросает `WriteError` с
готовым текстом; `write_replacement`/`check_file` возвращают отчёт с полем
`ok` и списком `problems`, ничего не бросая на «проверочных» расхождениях.

## 5. Результаты

### 5.1 Self-test (без файлов игры)

```
$ python3 tools/editor/sbtwrite.py --self-test
  synthetic round-trip (parse -> to_bytes)             ok
  header kept verbatim (padding bytes)                 ok
  replace block: sizes, shifted offsets, others' raw   ok
  write + oracle re-read: longer block                 ok
  write + oracle re-read: shorter block                ok
  error: no SCRIPT record                              ok
  error: offset is not a sub-script                    ok
  error: empty block                                   ok
  error: block does not end with END                   ok
  error: trailing bytes after END                      ok
  error: unknown opcode                                ok
  error: code over MAX_BLOCK                           ok
  error: bad magic                                     ok
  opaque record bytes preserved (encoder drift)        ok
  structure: m != code length                          ok
  --check: clean ok, tampered code fails               ok
  --check --expect plan match/mismatch                 ok
  write --out: file equals reported sha256             ok
18/18 self-tests passed
```

Синтетика покрывает: точный round-trip, сохранение паддинга заголовка,
замену блока с ростом/уменьшением и сдвигом колонки 2, сохранение «сырых»
байтов записи при drift'е структурного кодировщика, семь классов ошибок
(нет `SCRIPT`, чужое смещение, пустой блок, нет `END`, лишний `END`,
неизвестный опкод, превышение `MAX_BLOCK`, битая магия), `m != len(code)` и
работу `--check`/`--expect` (включая обнаружение подмены кода).

### 5.2 Round-trip всех 36 поставочных `.sbt`

```
$ python3 tools/editor/sbtwrite.py --roundtrip
roundtrip: 36 files, 6793572 bytes, 0.28s
  identical: 36/36 (100.00%), 0 mismatch, 0 error
  structural checks: 36/36 (raw round-trip, sizes, SCRIPT m and sub-script coverage)
  structural-encoder drift: 0 record(s)
```

36/36 файлов, 6 793 572 байта, 313 записи (включая 36 `END_OF_FILE`),
15 061 подскрипт. Записанные копии лежат в
`/tmp/opencode/sbtwrite/roundtrip/`; сравнение — и `sha256`, и побайтовое.

### 5.3 Замена блока на реальных уровнях

`--verify-replace` для каждого из трёх уровней берёт первый подскрипт с
`SET_CAMERA_HAZE` (фиксированная форма `DD` = 17 байт) и делает два варианта:

1. **same-length** — первый `double` заменён на `+0.5` (текст
   `render_exact` → правка строки → `assemble`), длина блока не меняется;
2. **longer** — перед финальным `---` вставлена валидная команда `EXIT_EVENT`
   (1 байт), блок и весь код растут на 1.

```
$ python3 tools/editor/sbtwrite.py --verify-replace
verify-replace: 3 levels, 6 variants
  level0   block 0x6f92   1871 bytes; render->asm exact: yes
    same-length  1871 -> 1871 bytes (+0), shifted 0 offsets, others identical: yes, block==asm: yes, check: ok
    longer       1871 -> 1872 bytes (+1), shifted 257 offsets, others identical: yes, block==asm: yes, check: ok
  dm1      block 0x666e   1871 bytes; render->asm exact: yes
    same-length  1871 -> 1871 bytes (+0), shifted 0 offsets, others identical: yes, block==asm: yes, check: ok
    longer       1871 -> 1872 bytes (+1), shifted 269 offsets, others identical: yes, block==asm: yes, check: ok
  level24  block 0x6684   1871 bytes; render->asm exact: yes
    same-length  1871 -> 1871 bytes (+0), shifted 0 offsets, others identical: yes, block==asm: yes, check: ok
    longer       1871 -> 1872 bytes (+1), shifted 364 offsets, others identical: yes, block==asm: yes, check: ok
```

«others identical: yes» — это проверка внутри записи: все прочие записи
сравнены побайтово (по `raw`) после обратного чтения файла оракулом, а код вне
заменённого диапазона — по сплайсу. «block==asm» — целевой подскрипт в
записанном файле равен собранным байтам. Три блока различаются (sha256 разные),
но совпали по размеру случайно.

`render->asm exact: yes` — цепочка
`decode(bytes) → render_exact(text) → assemble(text) → bytes` даёт те же байты
(`docs/SBT_ASM.md` §5: 15 061/15 061 на корпусе).

### 5.4 Сквозной пример через CLI

```
$ python3 tools/editor/sbtasm.py disasm dm1 --block 0x2b07 --exact --out /tmp/opencode/sbtwrite/e2e/blk.asm
wrote 1 block(s) to /tmp/opencode/sbtwrite/e2e/blk.asm
$ python3 tools/editor/sbtwrite.py --script dm1 --block 0x2b07 \
      --asm /tmp/opencode/sbtwrite/e2e/blk.asm \
      --out /tmp/opencode/sbtwrite/e2e/dm1.sbt \
      --plan /tmp/opencode/sbtwrite/e2e/dm1.plan.json
wrote /tmp/opencode/sbtwrite/e2e/dm1.sbt (151516 bytes, sha256 cce4a9749ac18d08)
  block 0x2b07: 994 -> 994 bytes (+0), shifted 0 sub-script offset(s)
  other records byte-identical: yes; target block == assembled: yes; check: ok
$ python3 tools/editor/sbtwrite.py --check /tmp/opencode/sbtwrite/e2e/dm1.sbt \
      --expect /tmp/opencode/sbtwrite/e2e/dm1.plan.json
check: /tmp/opencode/sbtwrite/e2e/dm1.sbt ok (9 records, 363 sub-scripts, code 83990 bytes, 151516 bytes)
```

Уменьшение блока (замена 994-байтного подскрипта на голый `END`, т.е. `---`):

```
$ python3 tools/editor/sbtwrite.py --script dm1 --block 0x2b07 --text=--- --out /tmp/opencode/sbtwrite/e2e/dm1_endonly.sbt
wrote /tmp/opencode/sbtwrite/e2e/dm1_endonly.sbt (150523 bytes, sha256 02c6fd1b8dd69a13)
  block 0x2b07: 994 -> 1 bytes (-993), shifted 357 sub-script offset(s)
  other records byte-identical: yes; target block == assembled: yes; check: ok
```

`--check` на подменённом файле и на чужом плане даёт код 1 и перечисляет
расхождения:

```
$ python3 tools/editor/sbtwrite.py --check "/mnt/d/games/Gunmetal 3dfx/level0.sbt" \
      --expect /tmp/opencode/sbtwrite/e2e/dm1.plan.json
check: /mnt/d/games/Gunmetal 3dfx/level0.sbt FAILED (8 records, 350 sub-scripts, code 62497 bytes)
  - record count: expected 9, got 8
  - record #0 sha256 differs
  - record #1 size: expected 1885, got 1345
  ...
$ echo $?
1
```

### 5.5 Проверенные уровни

| уровень | файл, байт | записей | подскриптов | код, байт |
|---|---:|---:|---:|---:|
| `level0` | 127 338 | 8 | 350 | 62 497 |
| `dm1` | 151 516 | 9 | 363 | 83 990 |
| `level24` | 233 770 | 9 | 459 | 120 924 |

Максимальный код в поставке — 134 686 байт (`0x20e1e`), то есть запас до
`MAX_BLOCK = 0x30000` (196 608) есть, но небольшой: замена блока проверяет
лимит и отказывает понятным текстом.

## 6. Ошибки (примеры)

Все сообщения — одна строка без traceback, код 2:

```
$ python3 tools/editor/sbtwrite.py --script nope --block 0 --text=--- --out /tmp/x.sbt
sbtwrite: error: level 'nope' not found (tried as a path and in /mnt/d/games/Gunmetal 3dfx)
$ python3 tools/editor/sbtwrite.py --script dm1 --block 1 --text=--- --out /tmp/x.sbt
sbtwrite: error: block 0x1 is not a sub-script entry (main block is 0, table has 363 entries; nearest: 0x0, 0x2a5a, 0x2a60, 0x2a6b)
$ python3 tools/editor/sbtwrite.py --script dm1 --block 0x2b07 --text 'NOT_A_COMMAND 1
---' --out /tmp/x.sbt
sbtwrite: error: assembling <text>: line 1: unknown command 'NOT_A_COMMAND' (use DB/RAW for raw bytes)
$ python3 tools/editor/sbtwrite.py --check /nope.sbt
sbtwrite: error: cannot read /nope.sbt: No such file or directory
```

## 7. Ограничения (что этот писатель не делает)

* **Неизвестные id записей — отказ оракула, не потеря байтов.** `gm.sbt.parse`
  бросает `ValueError` на типах 10/13 (заглушка) и 14 (`corrupt`) и на любом
  неизвестном байте (`gm/sbt.py:739-741`). Поскольку длину такой нагрузки
  определить нельзя, писатель не угадывает: `--check`/запись возвращают
  ошибку разбора. В поставочных файлах таких типов нет (проверено по всем 36).
  Для известных типов сырые байты сохраняются всегда (см. §1).
* **Семантика не проверяется.** Гарантируется структура контейнера и
  байткода, а не поведение уровня: вставленная команда может изменить игру
  (это и есть цель редактора). ВМ не исполняется.
* **`ELSE` — локальные смещения блока.** `render_exact` печатает плоскую форму
  `ELSE -> 0x…`; ассемблер трактует цель как литерал
  (`docs/SBT_ASM.md` §3). Если править текст *перед* целью такого `ELSE`,
  цель нужно поправить в тексте вручную; вставка в конец блока (перед `END`)
  безопасна — проверено, что ни один `ELSE` в поставке не выходит за конец
  своего блока.
* **Счётчики `things`/`n` не пересчитываются**: замена кода не добавляет
  сущностей. Массив `things` (`count*40`) и таблица подскриптов остаются как
  были; лимиты `0xC8`/`0x400` только проверяются.
* **Контрольных сумм в формате нет** — ни заголовок, ни записи их не хранят,
  поэтому «подпись» файла не пересчитывается (её и не существует).
* **Файлы игры не изменяются**: `--out` обязателен, запись атомарная
  (`*.tmp` + `os.replace`).

## 8. Воспроизведение

```sh
cd /home/nooverload/build/gm-sub-sbtwrite     # или /home/nooverload/gunmetal-re
python3 tools/editor/sbtwrite.py --self-test      # 18/18
python3 tools/editor/sbtwrite.py --roundtrip      # 36/36 identical
python3 tools/editor/sbtwrite.py --verify-replace # 3 уровня, 6 вариантов
python3 tools/editor/sbtwrite.py --check "/mnt/d/games/Gunmetal 3dfx/dm1.sbt"
```

Замеры (36 уровней, `/mnt/d`): `--self-test` ≈ 0,08 с, `--roundtrip` ≈ 0,3 с,
`--verify-replace` ≈ 0,2 с.

## 9. Связь с `M11.4`

Ассемблер блоков (`M11.1`, `tools/editor/sbtasm.py`) и писатель контейнера
(этот документ) вместе закрывают «правку скрипта в `.sbt`». Осталось по
`M11.4`: выбор блока по сущности/id (а не по смещению), пакетная правка
нескольких блоков за один проход и проверка результата в оригинальной игре —
это отдельные пункты, здесь их нет.

## 10. Сквозная проверка движком (не только оракулом)

Проверено 2026-10-23: правка реального блока и загрузка результата **C++-движком**
(оракул `gm.sbt` — не единственная сторона).

```sh
cp '/mnt/d/games/Gunmetal 3dfx/level0.sbt' '/mnt/d/games/Gunmetal 3dfx/level0.sbw' /tmp/opencode/sbtmod/
python3 tools/editor/sbtasm.py disasm level0 --block 0x6f92 --exact --out /tmp/opencode/sbtmod/block.txt
sed -i 's/SET_CAMERA_HAZE                    1.0 0.005/SET_CAMERA_HAZE                    0.5 0.005/' \
    /tmp/opencode/sbtmod/block.txt
python3 tools/editor/sbtwrite.py --script /tmp/opencode/sbtmod/level0.sbt --block 0x6f92 \
    --asm /tmp/opencode/sbtmod/block.txt --out /tmp/opencode/sbtmod/level0_mod.sbt
# wrote … (127338 bytes, sha256 64e2b15c51172f40)
#   block 0x6f92: 1871 -> 1871 bytes (+0), shifted 0 sub-script offset(s)
#   other records byte-identical: yes; target block == assembled: yes; check: ok
python3 tools/editor/sbtwrite.py --check /tmp/opencode/sbtmod/level0_mod.sbt
# check: … ok (8 records, 350 sub-scripts, code 62497 bytes, 127338 bytes)
cp /tmp/opencode/sbtmod/level0_mod.sbt /tmp/opencode/sbtmod/level0.sbt
./build/sandbox/engine/gunmetal -g /tmp/opencode/sbtmod -l level0 --info
# level: level0
# world: textures=381 effects=37 sectors=58 objects=192 vertices=2173 polygons=1449 bytes=254439
# entities: records=8 poly=104 object=0 sector=26 sobs=67 script=350/62497
```

Оракул видит в записанном файле три `SET_CAMERA_HAZE 0.5` (правка попала в
байты), движок загружает файл с той же структурой (8 записей, 350
подскриптов, 62 497 байт кода) и тем же размером контейнера, что и оригинал.
