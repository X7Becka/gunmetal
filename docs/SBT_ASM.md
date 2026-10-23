# Ассемблер скриптов `.sbt` (текст → байткод `SCRIPT`)

Статус: **готово для блока `M11.1` (начало)**: текст ↔ байткод подскрипта,
побайтовый round-trip на всём корпусе. Запись собранного блока обратно в
`.sbt` — это `M11.4` (редактор скриптов), здесь её нет.

Файлы:

| файл | что |
|---|---|
| `tools/editor/sbtasm.py` | ассемблер + точный рендер + CLI + self-test |
| `docs/SBT_ASM.md` | этот документ |

Зависимости: только `gm.sbtvm` (таблицы `COMMANDS`/`CONDITIONS`, декодер) и
`gm.sbt` (чтение записи 11). Ничего в существующих файлах не менялось.

## 1. Что покрывает `gm.sbtvm.render`

`render` — это **эталонный текстовый вид**: `decode(msg) -> Insn` (тела `IF`
рекурсивно) и печать. Проверено на всех 36 поставочных уровнях:

| метрика | значение |
|---|---:|
| уровней / блоков (подскриптов) | 36 / **15 061** |
| суммарно байт кода | **3 542 023** |
| блоков, где `decode` доходит ровно до своего `END` | **15 061 / 15 061** |
| инструкций (линейный обход, каждый байт один раз) | 147 777 |
| максимальная вложенность `IF` | 15 |
| блоки с таблицами `DEFINE_*` | 38 |
| блоки с `ADD_LIFT` | 17 (45 инструкций) |
| блоки, где `%g` теряет точность `double` | 2 478 (16,5 %) |
| полей `double` всего / из них теряют точность в `%g` | 154 656 / 17 299 (11,2 %) |

То есть **текст покрывает все 15 061 блок без исключений**: каждый блок
начинается на границе инструкции из таблицы сущностей и заканчивается
`END` (0xFF); блоки укладываются в код без дыр (сумма длин блоков равна
длине кода — 3 542 023 байта). Размер самого большого блока — 57 103 байта.

Важная особенность декодера, из-за которой этот инструмент не может
доверять дереву `Insn`: `IF`-обработчик `gm.sbtvm.decode` может «перепрыгнуть»
границу родительского тела. Компилятор кладёт `ELSE` **последней инструкцией
then-ветки**, а else-код — сразу за ней: при `true` `ELSE` перепрыгивает
else-код, при `false` `IF` пропускает then-ветку (вместе с `ELSE`) и попадает
на else-код. Декодер же дополнительно раскладывает else-код как обычные
инструкции на уровне выше, то есть те же байты видны дважды. На корпусе это
42 блока (во всех 36 уровнях; больше всего в `level13` — 3). Поэтому:

* `render_exact` (и `census`) идут **линейно по байтам**, а не по дереву;
* в таких местах печатается «плоская» форма `ELSE -> 0x…` (см. §3).

## 2. Грамматика

Текст — построчный, регистр мнемоник не важен (канон — верхний). Смещения и
цели — **относительно начала блока** (текст всегда описывает один подскрипт;
`disasm` вырезает его из кода, поэтому `ELSE -> 0x…` совпадает со смещением
внутри блока, а не файла).

```
line       := label | insn | if | else | table | db | comment | blank
label      := '.' NAME ':'                      ; напр. .skip:
insn       := NAME [operand...]                 ; 192 именованные команды
if         := 'IF' COND [operand...] '{' line* '}' [' else' '{' line* '}']
              ; '}' и 'else' — на одной строке: "} else {"
else       := 'ELSE' '->' (NUMBER | label)
table      := NAME N 'entries' record*          ; 4 таблицы DEFINE_*
record     := STRING ('|' STRING)* value*
db         := 'DB' (NUMBER | STRING)+ | 'RAW' HEXSTRING
comment    := ';' ... | '#' ... | '//' ...
END        := '---'                             ; 0xFF, конец подскрипта
```

Операнды — по сигнатурам из `gm.sbtvm` (одна буква = один операнд):

| буквы | размер | как писать |
|---|---|---|
| `A`, `B`, `D` | 8 | `double`: `0.5`, `-2.25`, `1e+06`, `0.034906585039886584`, либо биты `0x` + 16 hex |
| `Y` | 1 | целое 0…255 |
| `S` | NUL-terminated | строка в кавычках, с escape |
| остальные (`E G I L M N O P R T V Z`) | 4 | целое 0…2³²−1 |

Числа:

* целые: `123`, `0x7b`, `-1` (=`0xffffffff`), а также форма самого `render`:
  `4294967295(0xffffffff)` — аннотация в скобках игнорируется;
* `double`: любой литерал, который принимает `float()` (точность — короткая
  round-trip форма `repr`), либо `0x` + 16 hex-цифр — **сырые байты**
  little-endian (для `inf`/`nan` это единственный способ);
* строки: `"text"`, escape `\" \\ \n \r \t \0 \xNN`; кодировка — latin-1
  (байты 0…255), NUL внутри строки запрещён (он терминатор);
* таблицы: имена строк печатаются как есть; если имя пустое, содержит `|`,
  кавычки, обратный слэш или пробелы по краям — оно берётся в кавычки
  (так делает `render_exact`; `render` такие имена не экранирует и они
  неоднозначны).

`DEFINE_LEVEL_OBJECTIVES`, `SOURCEPLAYER_PATH`, `ADD_LIFT`, `IF`, `ELSE`,
`EVAL` — структурные опкоды (`sbtasm.STRUCT_OPS`),
их текст:

```
DEFINE_LEVEL_OBJECTIVES "Objective 1" "Objective 2"
DEFINE_WEAPONS 2 entries                      ; строки — сокращённо
    POWER CELLS | s0_1 | s0_2 1 1 … 0.5 0.5 …
    "" | s1_1 | s1_2         2 2 … 0.5 0.5 …
SOURCEPLAYER_PATH n=3 1 2 3
ADD_LIFT n=2 head=<296 hex-цифр, 0x94 байта>
    7 8 9
    10 11 12
EVAL ADD_THING_RELATIVE 2 0 20 0 20 0.0 0.0 0.0 4.0 0.0 5.0 0.0
IF SOURCEPLAYER_AMMO_AT_LEAST 10 1 {
    ...
}
```

`ADD_LIFT`: заголовок `0x94` байта и остановки `n * 12` байт — в
`gm.sbtvm.render` **не печатаются** (§5). Поэтому `head=<hex>` и строки
остановок обязательны; без `head=` ассемблер выдаёт ошибку, а
`--allow-lossy` забивает их нулями (только для измерения потерь).

`DB`/`RAW` — сырые байты: `DB 0xfe 0x00 "ab"`, `RAW 00ff10…`. Нужны для
неизвестных опкодов и любых нестандартных вставок.

## 3. Метки, `IF`/`ELSE`, размеры

Байтовая раскладка (подтверждена побайтовым round-trip на всём корпусе):

```
IF   : 199 | cond-id(u8) | операнды условия | size(u32) | тело(size-4 байт)
ELSE : 200 | rel(u32)                                   ; target = здесь+1+rel
```

* размер then-ветки, который печатает декодер, включает собственные 4 байта:
  `size = 4 + len(тело)`. Ассемблер считает его сам по строкам между `{` и
  `}` — вручную размеры писать нельзя (и не нужно).
* пустая then-ветка — `size = 4` (в корпусе 304 таких `IF`), текст:
  `IF NAME {` + `}`.
* `ELSE` — только вперёд (`u32`), `rel = target - (позиция поля)`; в
  сгруппированной форме `} else {` ассемблер сам ставит
  `rel = 4 + len(else-тело)`, что совпадает с декодером.
* метки `.name:` привязываются к текущему смещению; `ELSE -> .name` и
  `ELSE -> 0x…` равноправны. Назад `ELSE` не умеет (u32) — это ошибка.

Две формы одной и той же раскладки:

```
; сгруппированная (обычный случай: else-код внутри текущего тела)
IF SOURCEPLAYER_TYPE 0 {
    ADD_SOURCEPLAYER_SPRITE …
} else {
    LOG_MESSAGE "not a pilot"
}

; плоская (else-код физически вне родительского IF; см. §1)
IF SOURCE_UNDERWATER {
    IF FIRE_SOURCEPLAYER_LASER … {
    }
    ELSE -> 0x3e1
}
IF FIRE_SOURCEPLAYER_LASER … {
}
```

Обе формы дают побайтово одинаковый результат, если байты идут в том же
порядке; `render_exact` печатает сгруппированную, когда else-код укладывается
в текущую область, и плоскую — когда нет.

## 4. Покрытые опкоды и условия

Всего 202 ненулевых опкода: 192 именованных в `gm.sbtvm.COMMANDS`, 4 таблицы
`DEFINE_*` (`TABLE_SIGS`), 6 структурных, плюс `END` (0xFF).

| группа | есть в таблице | встречается в поставке |
|---|---:|---:|
| именованные команды | 192 | **160** |
| таблицы `DEFINE_WEAPONS/AMMO/PLAYER_TYPES/EQUIPMENT` | 4 | **4** (58 / 48 / 145 / 36 инструкций) |
| структурные (`DEFINE_LEVEL_OBJECTIVES`, `SOURCEPLAYER_PATH`, `ADD_LIFT`, `IF`, `ELSE`, `EVAL`) | 6 | **6** (35 / 11 / 45 / 16 830 / 11 144 / 11 017) |
| `END` | 1 | 15 061 |

32 именованные команды в поставочных уровнях **не встречаются** (ассемблер
их всё равно поддерживает — сигнатуры в таблице): `AND_SOURCEPLAYER_BEHAVIOUR_BITS`,
`AND_SOURCEPLAYER_KEY_BITS`, `DELETE_LOCATION`, `END_GAME`, `EXIT_PROGRAM`,
`FREEZE`, `LOG_MESSAGE`, `NEW_GAME`, `REFRESH_SCREEN`,
`SET_HANDLEPOLY_MATERIAL`, `SET_HANDLESECTOR_FLAGS`,
`SET_LEVEL_ALIASING_VALUES`, `SET_POLY_BRIGHTNESS`, `SET_POLY_SCROLL`,
`SET_POLY_TEXTURE`, `SET_SECTOR_DAMAGE`, `SET_SECTOR_LINK`, `SET_SECTOR_TYPE`,
`SET_SOURCEPOLY_BRIGHTNESS`, `SET_SOURCEPOLY_MATERIAL`,
`SET_SOURCEPOLY_SCROLL`, `SET_SOURCESECTOR_DAMAGE`, `SET_SOURCESECTOR_FLAGS`,
`SET_SOURCESECTOR_MOVEMENT`, `SET_SOURCEWORLDOBJECT_HIT_ON`,
`SHOW_HELP_SCREEN`, `SOURCEPLAYER_FALL_PITCH_DOWN`, `SOURCEPLAYER_INVINCIBLE`,
`SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO`, `SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO`,
`SOURCEPLAYER_RUB_SPEED_ADJUST`, `SPEAK_NUMBER`.

Условия (`CONDITIONS`, таблица второй ВМ, 59 штук): в поставке встречаются
**43**, чаще всего `RANDOM` (3 680), `ADD_THING_RELATIVE` (2 995),
`ADD_THING_EX` (2 663), `SOURCE_IS_PLAYER` (1 894), `SOURCE_SWAP` (1 621).
Не встречаются 16: `ADD_OTHER`, `CAPTURE_THE_FLAG_GAME`, `CUSTOM_COMMAND_1`,
`CUSTOM_COMMAND_3`, `CUSTOM_COMMAND_4`, `CUSTOM_COMMAND_5`, `CUSTOM_COMMAND_6`,
`DEATHMATCH_NORMAL_GAME`, `NEAREST_OBSTRUCTION`, `NEXT_LEVEL_NUMBER`,
`RANDOM_FRAME_BALANCED`, `SOURCEPLAYER_IS`, `SOURCEPLAYER_LIFE_AT_LEAST`,
`SOURCEPOLY_HANDLE`, `SOURCESECTOR_FLAGS`, `TEAM_GAME`.

Неизвестный id условия (`OP_<n>`, как его печатает `render`) тоже
принимается: операндов у него нет, `IF OP_77 {` даёт `199 0x4d 4 0 0 0`.

## 5. Round-trip по корпусу

Команда (по умолчанию — точный рендер; 36 уровней, все блоки):

```
$ python3 tools/editor/sbtasm.py --roundtrip
roundtrip: 36 levels, 15061 blocks, 3542023 bytes, 2.74s
  decode reaches block end exactly: 15061/15061
  exact    : 15061/15061 byte-identical (100.00%), 0 mismatch, 0 error
  instructions: 147777, max IF nesting: 15
  blocks with tables: 38, with ADD_LIFT: 17, with lossy doubles: 2478
  opcodes seen: 171 names; conditions seen: 43
```

То есть **100,00 % побайтовых совпадений** на всех 15 061 блоках
(3 542 023 байта) для цепочки
`decode(bytes) → render_exact(text) → assemble(text) → bytes'`.

### 5.1 Где `gm.sbtvm.render` теряет информацию

Тот же прогон через **эталонный** `render` (`--render reference`, или
`--render both`, чтобы увидеть обе цифры):

```
$ python3 tools/editor/sbtasm.py --roundtrip --render both
roundtrip: 36 levels, 15061 blocks, 3542023 bytes, 5.42s
  exact    : 15061/15061 byte-identical (100.00%), 0 mismatch, 0 error
  reference: 12545/15061 byte-identical (83.29%), 2474 mismatch, 42 error
  ...
  failures by cause:
     2457  float-g                double printed with %g (6 significant digits)
       42  structural-nesting     ELSE body lies outside the enclosing IF's size; render nests it inside
       17  float-g+add-lift       both of the above
```

Три причины, все — свойства `render`, а не ассемблера:

1. **`%g` для `double` (2 457 + 17 блоков).** `gm.sbtvm._fmt` печатает
   `double` шестью значащими цифрами: `0.034906585039886584` → `0.0349066`,
   `0.33000000000000007` → `0.33`. Обратный `float()` даёт другой `double`
   (другие 8 байт). В корпусе это 17 299 из 154 656 полей `double`
   (11,2 %). `render_exact` печатает `repr` (кратчайшая форма с побайтовым
   round-trip) — текст читаемее, чем hex, и точен.
2. **`ADD_LIFT` (17 блоков).** `render` печатает только `ADD_LIFT n=…`;
   заголовок `0x94` байта и остановки не печатаются вовсе. `render_exact`
   добавляет `head=<hex>` и строки остановок.
3. **Вложенность `ELSE` через границу родителя (42 блока).** См. §1: в дереве
   декодера else-код оказывается внутри `then`-ветки внешнего `IF`, хотя по
   `size` он лежит вне её. Такой текст нельзя собрать ни одним сборщиком,
   который доверяет скобкам: цель `ELSE` оказывается позади текущей позиции
   (`asm-error: ELSE target 0x1d5 is not forward of the jump`). `render_exact`
   печатает плоскую форму `ELSE -> 0x…`, и байты восстанавливаются точно.

Других расхождений нет: строки, целые (включая `N(0xH)` и отрицательные),
таблицы, `DEFINE_LEVEL_OBJECTIVES`, `SOURCEPLAYER_PATH`, `EVAL`, пустые
`IF`-ветки — всё round-trip'ится через `render` без потерь.

## 6. CLI

```
python3 tools/editor/sbtasm.py disasm <level.sbt|stem> [--block OFF|all]
                                    [--exact] [--out FILE|DIR] [--json]
python3 tools/editor/sbtasm.py asm <file.asm> [-o out.bin] [--allow-lossy] [--json]
python3 tools/editor/sbtasm.py roundtrip [--render exact|reference|both]
                                    [--game DIR] [--limit N] [--json] [--verbose]
python3 tools/editor/sbtasm.py self-test
```

`disasm` без `--block` печатает главный блок (offset 0), `--block 0x2b07` —
блок по точному смещению из таблицы сущностей, `--block all` — все блоки
(с `--out DIR` — по файлу `<offset>.asm` на блок). По умолчанию текст —
эталонный `render`; если он для этого блока не побайтовый, в stderr
печатается предупреждение с причиной. `--exact` — точный диалект (§5.1).

```
$ python3 tools/editor/sbtasm.py disasm dm1 --block 0x2b07
note: block 0x2b07: reference render is not byte-preserving
      (1 double field(s) lose digits with %g); use --exact
IF RANDOM 0.25 {
    EVAL ADD_THING_RELATIVE 2 0 20 0 20 0 0 0 4 0 5 0
    ...
```

Сборка и проверка на реальном блоке (994 байта):

```
$ python3 tools/editor/sbtasm.py disasm dm1 --block 0x2b07 --exact --out /var/tmp/blk.asm
wrote 1 block(s) to /var/tmp/blk.asm
$ python3 tools/editor/sbtasm.py --asm /var/tmp/blk.asm -o /var/tmp/blk.bin
assembled /var/tmp/blk.asm -> /var/tmp/blk.bin (994 bytes)
$ python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from gm import sbt as sbtmod
from tools.editor import sbtasm as A
sbt = sbtmod.parse_file("/mnt/d/games/Gunmetal 3dfx/dm1.sbt")
blk = A.script_of(sbt).code[0x2b07:0x2b07 + 994]
print("identical:", blk == open("/var/tmp/blk.bin", "rb").read())
PY
identical: True
```

Правка текста — обычная: меняем литерал и собираем заново. Пример на том же
блоке (`IF RANDOM 0.25` → `IF RANDOM 0.125`):

```
$ python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from gm import sbt as sbtmod, sbtvm
from tools.editor import sbtasm as A
sbt = sbtmod.parse_file("/mnt/d/games/Gunmetal 3dfx/dm1.sbt")
sc = A.script_of(sbt)
spans = dict(A.block_spans(sc.code, A.block_starts(sc)))
blk = sc.code[0x2b07:spans[0x2b07]]
out = A.assemble(A.render_exact(blk).replace("IF RANDOM 0.25",
                                             "IF RANDOM 0.125", 1))
ins, _ = sbtvm.decode(out)
diff = [i for i in range(len(blk)) if blk[i] != out[i]]
print("cond arg:", ins[0].args[0][1], "length:", len(out),
      "changed bytes:", len(diff), "at", hex(diff[0]))
PY
cond arg: [0.125] length: 994 changed bytes: 1 at 0x8
```

`asm` после сборки сам вызывает `gm.sbtvm.decode()`; если декодер не доходит
до конца (например, текст содержит `DB` с неизвестным опкодом), печатается
предупреждение — это не ошибка сборки.

`--json` отдаёт по одной структуре: для `disasm` — блоки и текст, для `asm` —
длину, hex и статус декодирования, для `roundtrip` — счётчики, гистограммы
опкодов/условий, причины расхождений (`causes`, `reasons`) и список
расхождений (`mismatches`, в `--verbose` — все).

## 7. Self-test (без файлов игры)

```
$ python3 tools/editor/sbtasm.py --self-test
11/11 self-tests passed
```

Группы проверок:

1. все 192 именованные команды: синтетический блок → `render`/`render_exact`
   → `assemble` → те же байты (обе ветки, эталонная и точная);
2. все 59 условий — в `IF` (пустое тело) и в `EVAL`;
3. `double`: `%g`-потеря обнаруживается, `repr` и битовая форма `0x…` точны,
   `-0.0` сохраняется;
4. все 4 таблицы, включая пустые имена и имена с пробелами;
5. структурные: objectives (в т.ч. кавычки/слэш в строке), `SOURCEPLAYER_PATH`
   с `0xffffffff`, `ADD_LIFT` с заголовком, `END`; отсутствие `head=`
   у `ADD_LIFT` — ошибка;
6. `IF`/`ELSE`: вложенные, пустая then-ветка, метки, `ELSE` только вперёд,
   `ELSE -> .label`;
7. `DB`/`RAW`;
8. числа: десятичные, `0x`, аннотация `N(0xH)`, отрицательные, биты `double`;
9. комментарии/метки;
10. ошибки: 14 кейсов с проверкой **номера строки** и текста (неизвестная
    команда/условие, число операндов, незакрытая строка, отсутствие `}`,
    `ELSE` назад, неизвестная/дублирующая метка, переполнение, `RAW`);
11. стабильность `render_exact → assemble → render_exact → assemble`.

## 8. Ограничения и что осталось

* **Запись в `.sbt` не делается.** `sbtasm` собирает байты блока; вставка в
  запись `11`, пересчёт `SCRIPT.m` и пересборка файла — `M11.4`. Пока
  проверка — побайтовое сравнение с исходным блоком (этот документ, §5).
* **`gm.sbtvm.render` не является lossless** (три причины, §5.1). Для правок
  используйте `--exact`: точный диалект — надмножество эталонного по
  литералам (`repr`, биты `double`, `head=` у `ADD_LIFT`, плоский `ELSE`).
* **`render`-текст с пустым именем таблицы** (или именем, содержащим `|`)
  неоднозначен; в поставочных уровнях таких имён нет (проверено: 0 пустых,
  0 с кавычками/`|`), точный рендер экранирует их кавычками.
* **Неизвестные опкоды/условия.** `decode` на неизвестном опкоде бросает
  `ValueError` — такой блок нельзя отрендерить; после правки в тексте его
  можно собрать через `DB`/`RAW`. Неизвестные id условий печатаются как
  `OP_<n>` и собираются обратно.
* **Лимиты движка не проверяются**: `MAX_BLOCK = 0x30000` для всего кода и
  `c <= 0xc8`/`n <= 0x400` записи `SCRIPT` — это забота писателя `M11.4`.
  Локальные пределы структур (30 целей, 32 точки пути, счётчики таблиц/
  остановок ≤ 0x1000) ассемблер проверяет.
* **`EVAL` не является выражением.** Как и в движке, это «голое» условие:
  вложенных арифметических выражений и стека операндов в байткоде нет
  (`docs/VM_EXEC.md` §1, §4), поэтому в тексте их тоже нет.
* **Дубликаты в дереве декодера.** `census`/`render_exact` линейны, но
  сторонний код, использующий `gm.sbtvm.decode` как дерево, увидит
  «двойные» инструкции в 42 блоках с `ELSE` за границей родителя (§1).
* `--roundtrip --render reference` **всегда** возвращает код 1: эталонный
  рендер не может быть побайтовым по построению. Критерий приёмки — режим
  `exact` (по умолчанию), он зелёный.

## 9. Воспроизведение

```
cd /home/nooverload/gunmetal-re        # или worktree
python3 tools/editor/sbtasm.py --self-test
python3 tools/editor/sbtasm.py --roundtrip                 # exact, 100 %
python3 tools/editor/sbtasm.py --roundtrip --render both   # + потери render
python3 tools/editor/sbtasm.py disasm dm1 --block all --exact --out /var/tmp/dm1
```

Замеры (36 уровней, `/mnt/d`, без зеркала; время зависит от нагрузки):
`--self-test` ≈ 0,05 с, `--roundtrip` (exact) ≈ 2,6–2,8 с,
`--roundtrip --render both` ≈ 4,5–5,5 с.
Файлы игры — только на чтение.
