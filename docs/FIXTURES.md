# FIXTURES — синтетические минимальные тест-векторы (`M3.2.7`)

Статус: рабочий документ, 2026-10-23. Инструмент:
`tools/qa/fixtures.py` (stdlib + `gm/*`, ничего не пишет в установку игры).

Документ отвечает на четыре вопроса: **что** генерируется, **какие байты**
лежат в каждом файле, **что из этого принял движок**, и **какие ограничения**
у синтетики (включая случаи, где читатель молчит, хотя обязан ругаться).

## 1. Зачем это рядом с `export_parity.py`

`M3.2.7` уже закрыт частично: `tools/re/export_parity.py` пишет **ожидаемые
числа** по поставочному корпусу, а `engine/tests/test_parity.cpp` сверяет их с
C++-читателями (288 проверок). Это ловит расхождения на реальных файлах, но:

* тест зависит от установки игры (без неё C++-часть пропускается);
* файлы большие (`level0.sbw` — мегабайты), поэтому «минимальный» случай
  (1 сектор / 1 объект / 1 полигон) на них не проверить;
* CLI (`gunmetal --dump`, `--info`) на поставочных данных проверяет только
  счастливый путь.

`fixtures.py` закрывает другую половину: генерирует **маленькие файлы** (в
git не попадают — это производные данные, `M12.7`), которые проверяются
**двумя независимыми читателями** — Python-оракулом (`gm.*`) и настоящим
движком (`gunmetal -g DIR --dump FILE`, `-l fixture --info`).

## 2. Что покрыто

Десять файлов, 9 из них проверяются оракулом (`.dat` проверяется через `.idx`):

| файл | байт | содержимое | oracle writer | движок (`--dump`) |
|---|---:|---|---|---|
| `fixture.sbw` | 365 | 1 текстура, 0 эффектов, 1 сектор, 1 объект, 4 вершины, 1 полигон (3 индекса) | `gm/sbw.py::to_bytes` | `kind: SBW level` |
| `fixture.sbt` | 4013 | `SCRIPT` (11), `SOUND_SAMPLE_TABLE` (12), `END_OF_FILE` (255) | `gm/sbt.py::to_bytes`/`rebuild` | `kind: SBT level` |
| `fixture-unknown.sbt` | 3987 | валидная запись 12 + **неизвестный тип 0x42** (негативный контроль) | — | отказ, rc=2 |
| `fixture.sbc` | 16 | `SBC0`, 1 запись 2×2, 4 текселя | `gm/sbc.py::to_bytes` | `kind: SBC lightmaps` |
| `fixture.sbm` | 76 | 8×8, 1 мип-уровень, 64 индекса (палитра внешняя) | `gm/sbm.py::to_bytes` | `kind: SBM texture` |
| `fixture.sob` | 173 | 3 вершины, 1 грань, текстура `FIXTURE` (канонический текст) | `gm/sob.py::to_bytes` | `kind: SOB model` |
| `fixture.soo` | 19 | mode 0, 1 кадр `fixture 100` тиков | `gm/soo.py::to_bytes` | `kind: SOO model animation` |
| `fixture.sml` | 30 | `<d><r><p>Fixture</p></r></d>` + CRLF | нет сериализатора (текст) | `kind: SML markup` |
| `fixture.idx` + `fixture.dat` | 24 + 76 | BIGFILE с одной записью `FIXTURE` = `fixture.sbm` | `gm/bigfile.py` (только чтение) | `kind: BIGFILE index` |

Минимальность упирается в сам формат: `SOUND_SAMPLE_TABLE` — фиксированные
300 слотов (1200 + 2700 байт), поэтому `fixture.sbt` не может быть короче
4013 байт; `SCRIPT` в нём — 27 байт (`count=0, n=1, m=1, code=0xFF`).

## 3. Раскладка по байтам

### 3.1 `fixture.sbw` — 365 байт

```
00000000: 0100 0000 4649 5854 5552 4500 0100 0300  ....FIXTURE.....
00000010: 0000 0000 0000 0100 0000 0100 0000 0000  ................
00000020: 0001 0000 0000 0000 0000 0060 4000 0000  ...........`@...
00000030: 0000 0050 4000 0000 0000 0000 0000 0000  ...P@...........
00000040: 0000 00f0 3f00 0000 0000 0000 0000 0000  ....?...........
00000050: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000060: 0000 00f0 3f00 0000 0000 0000 0000 0000  ....?...........
00000070: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000080: 0000 00f0 3f04 0001 0000 0000 0000 0000  ....?...........
00000090: 0000 0000 0000 0080 4200 0000 0000 0000  ........B.......
000000a0: 0000 0080 4200 0080 4200 0000 0000 0000  ....B...B.......
000000b0: 0000 0080 4200 0000 00f3 0435 4200 0000  ....B......5B...
000000c0: 0000 0000 0000 0000 0000 4200 0000 4200  ..........B...B.
000000d0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000000e0: 0000 0000 0000 0000 0000 0001 0000 0000  ................
000000f0: 0000 005a 7a00 0300 0001 0002 0000 0000  ...Zz...........
00000100: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000110: 0000 0000 4200 0000 4200 0000 0000 0000  ....B...B.......
00000120: 0000 0000 0000 0080 3f00 0080 3f00 0000  ........?...?...
00000130: 0000 0000 0000 0000 0000 0080 3f00 0000  ............?...
00000140: 0000 0000 0000 0000 0000 0000 0000 0080  ................
00000150: 4200 0080 4200 0000 0034 1278 5600 0000  B...B....4.xV...
00000160: 0000 0000 8000 0000 0000 0000 00         .............
```

| смещение | поле |
|---|---|
| `0..3` | `u32 count1 = 1` |
| `4..11` | `"FIXTURE\0"` |
| `12..13` | `u16 flag = 1` |
| `14..17` | `u32 value = 3` (вес текстуры уровня) |
| `18..21` | `u32 count2 = 0` (эффектов нет) |
| `22..25` | `u32 count3 = 1` (сектор) |
| `26..29` | `u32 objects = 1` |
| `30..31` / `32` | `u16 kind = 0` / `u8 sub = 0` |
| `33..36` | `u32 id = 1` |
| `37..132` | 96 байт заголовка: 12 × `f64` = origin `(128,64,0)` + единичный базис (3×3) |
| `133..134` / `135..136` | `u16 n_verts = 4` / `u16 n_polys = 1` |
| `137..184` | 4 вершины `(0,0,0) (64,0,0) (64,64,0) (0,64,0)` |
| `185..234` | 50 байт хвоста: `radius=45.254834`, `f75=0`, `f74=0`, `f7c=0`, `f78=0`, `bounds=(32,32,0)`, 24 нуля |
| `235..244` | `5H`: texture=1, lightmap=0, material=0, overlay=0, f7a=`0x7A5A` |
| `245..246` | brightness=0, n_idx=3 |
| `247..252` | индексы `0 1 2` |
| `253..364` | 112-байтный хвост полигона |

Хвост полигона (смещения относительно `253`): `scroll_u/v` 0/2, `u_scale` 4,
`v_scale` 6, `uv_u/v` 8/10, `uv_u_fp/v_fp` 12/16 (signed), `centre` 20
`(32,32,0)`, `normal` 32 `(0,0,1)`, `u_axis` 44 `(1,0,0)`, `v_axis` 56
`(0,1,0)`, `corner_a` 68 `(0,0,0)`, `corner_b` 80 `(64,64,0)`,
`f74` 92 = `0x1234`, `f76` 94 = `0x5678`, `handle` 96 = 0, `reserved` 98 = 0,
`flags` 102 = `0x8000` («UV/frame computed»), `anim0/1/2` 106/108/110 = 0.

`f74`/`f76`/`f7a` и «сырой» 112-байтный хвост намеренно ненулевые: это
**проба сохранения неизвестных полей** — писатель оракула переносит хвост
verbatim, и `--check` требует, чтобы значения вернулись после `parse`.

### 3.2 `fixture.sbt` — 4013 байт

| смещение | поле |
|---|---|
| `0..79` | `"BINARY SAVE\0"` + нули (80 байт) |
| `80..83` | `u32 version = 0` |
| `84` | тип `11` (`SCRIPT`) |
| `85..88` | `u32 count = 0` (things) |
| `89..92` | `u32 n = 1` |
| `93..104` | три колонки по одному `u32`: `1, 0, 0` |
| `105..108` / `109` / `110` | `u32 m = 1` / код `0xFF` (`END`) / `u8 flag = 0` |
| `111` | тип `12` (`SOUND_SAMPLE_TABLE`) |
| `112..1311` | 300 × `u32` refcount; `[0] = 1`, остальные 0 |
| `1312..4011` | 300 × 9 байт имён; `[0] = "FIXTURE\0\0"`, `[299] = AA×9` |
| `4012` | тип `255` (`END_OF_FILE`) |

Слот 299 забит `0xAA`: имя пустое, но байты обязаны сохраниться — это вторая
проба «неизвестные поля не теряются» (оракул хранит точные `name_slots`).
Движок печатает записи так:

```
kind: SBT level
version: 0
records: 3
   11 SCRIPT                    27
   12 SOUND_SAMPLE_TABLE      3901
  255 END_OF_FILE                1
```

### 3.3 `fixture-unknown.sbt` — 3987 байт

`header(84) + 0x0C + запись 12 (3900) + 0x42 + 0xFF`. Тип `0x42 = 66` не
входит в таблицу загрузчика. Оба читателя обязаны отказать, и отказывают:

```
$ gunmetal -g /tmp/opencode/fixtures --dump \
      /tmp/opencode/fixtures/fixture-unknown.sbt ; echo rc=$?
[error] sbt: unsupported .sbt record type 66
rc=2

>>> gm.sbt.parse(open('fixture-unknown.sbt','rb').read())
ValueError: unknown record type 66
```

### 3.4 `fixture.sbc` — 16 байт

```
00000000: 5342 4330 0100 0000 0100 0100 0020 4060  SBC0......... @`
```

`"SBC0"`, `u32 count = 1`, `u32 desc = 0x00010001` (`w_log=1`, `h_log=1` →
2×2), пиксели `00 20 40 60`.

### 3.5 `fixture.sbm` — 76 байт

`u32 zero = 0`, `log2w = 3`, `log2h = 3`, `levels-1 = 0`, `zero = 0`,
`u32 flags = 0`, затем 64 индекса `(x*9 + y*17) & 0xFF`. **Палитра в блобе не
хранится** (общая VGA-палитра игры), поэтому «палитровый» аспект здесь — только
индексы; PNG-экспорт требует `-g` с установкой.

### 3.6 `fixture.sob` — 173 байта (канонический текст)

```
 3 \r\n
 1 \r\n
 0  0  0 \r\n
 64  0  0 \r\n
 0  64  0 \r\n
FIXTURE\r\nnone ×7
 96  3  0  1  2 \r\n
 0  0  0  65536  0  0  0  65536  0  0  0  65536  0  0  0 \r\n
```

`to_bytes()` пишет канонический вариант; `parse → to_bytes` идемпотентен
(второй проход даёт те же байты).

### 3.7 `fixture.soo` / `fixture.sml`

```
$ xxd fixture.soo
00000000: 300d 0a31 0d0a 6669 7874 7572 6520 3130  0..1..fixture 10
00000010: 300d 0a                                  0..

$ xxd fixture.sml
00000000: 3c64 3e3c 723e 3c70 3e46 6978 7475 7265  <d><r><p>Fixture
00000010: 3c2f 703e 3c2f 723e 3c2f 643e 0d0a       </p></r></d>..
```

У `.sml` нет писателя: документ — это и есть байты. Round-trip проверяется как
`text == bytes.decode()` + `sml.encode(sml.decode(x)) == x` (шифр `.sme`
обратим) + структурная валидность `<d>…</d>`.

### 3.8 `fixture.idx` / `fixture.dat`

```
00000000: 4249 4746 494c 451a 0100 0000 4649 5854  BIGFILE.....FIXT
00000010: 5552 4500 0000 0000                      URE.....
```

`"BIGFILE"`, `0x1A`, `u32 count = 1`, запись `name[8] = "FIXTURE\0"`,
`u32 offset = 0`; DAT — ровно байты `fixture.sbm`.

## 4. Команды и вывод

Все команды — из корня worktree; фикстуры пишутся только в `--write DIR`
(в отчёте — `/tmp/opencode/fixtures`), движок читает игру не трогает.

### 4.1 `--write`

```
$ python3 tools/qa/fixtures.py --write /tmp/opencode/fixtures
[ok  ] wrote fixture.sbw            365 bytes  sha256=993e6cbac16dc76e
[ok  ] wrote fixture.sbt           4013 bytes  sha256=b071029772ac8155
[ok  ] wrote fixture-unknown.sbt   3987 bytes  sha256=d0fc35e4cc4d6682
[ok  ] wrote fixture.sbc             16 bytes  sha256=a136f52572ab145a
[ok  ] wrote fixture.sbm             76 bytes  sha256=c1cf6a50818580cb
[ok  ] wrote fixture.sob            173 bytes  sha256=5b9672e9b718fb8f
[ok  ] wrote fixture.soo             19 bytes  sha256=7794fa1b3a71654e
[ok  ] wrote fixture.sml             30 bytes  sha256=3eff11bbbeab8cd6
[ok  ] wrote fixture.idx             24 bytes  sha256=44a79153b26d5957
[ok  ] wrote fixture.dat             76 bytes  sha256=c1cf6a50818580cb
status: ok (rc=0)
```

Полные хэши (для сверки чужими руками):

```
993e6cbac16dc76e785d02de09f6df83279c9b275cf2fd6bf489bf6e98bb94ca  fixture.sbw
b071029772ac8155bf44e5879290436d11ab9aeeea9233bb4da9ebcb287a6d32  fixture.sbt
d0fc35e4cc4d668236a08439fa9a52a7302d61ca1dac148f3532ecc77507bb19  fixture-unknown.sbt
a136f52572ab145adc7a604cf79f5619cf6121fc0d2e49b0657f5227bdae4e95  fixture.sbc
c1cf6a50818580cb4a4df0a32a288463b2194769b58dbb222ca103c4d08254e9  fixture.sbm
5b9672e9b718fb8faadc6f6534f3e3cbf6c57184fd7b08d08c7499b6582c8b09  fixture.sob
7794fa1b3a71654e8f1c34fc2d22e218e594f495c493f748c4315c12b3c655ff  fixture.soo
3eff11bbbeab8cd6c38d45f8e78561ddcd7e87cf641b203d722bfc83b9be2f21  fixture.sml
44a79153b26d5957bb5672bd83b6e87d3fa4f48625a6d92e3844cd8ed20bd791  fixture.idx
c1cf6a50818580cb4a4df0a32a288463b2194769b58dbb222ca103c4d08254e9  fixture.dat
```

Повторный `--write` даёт те же sha256 (проверяется в `--self-test`).

### 4.2 `--check` (оракул)

```
$ python3 tools/qa/fixtures.py --check /tmp/opencode/fixtures
[ok  ] fixture.sbw            365 bytes  textures=1 effects=0 sectors=1 objects=1 vertices=4 polygons=1 tables_end=22 data_end=365
[ok  ] fixture.sbt           4013 bytes  records=3 version=0 script_things=0 script_handles=1 script_code=1 sample_count=1
[ok  ] fixture-unknown.sbt   3987 bytes  rejected=True error=unknown record type 66
[ok  ] fixture.sbc             16 bytes  magic=SBC0 records=1 texels=4 first=2x2
[ok  ] fixture.sbm             76 bytes  dimensions=8x8 levels=1 flags=0 mip0=8x8
[ok  ] fixture.sob            173 bytes  vertices=3 faces=1 texture=FIXTURE
[ok  ] fixture.soo             19 bytes  mode=0 declared=1 frames=1 first=fixture 100 ticks
[ok  ] fixture.sml             30 bytes  chars=30 lines=2 tags=6
[ok  ] fixture.idx             24 bytes  entries=1 data=76 first=FIXTURE (76 bytes)
status: ok (rc=0)
```

`--check` для каждого файла требует: размер, перечисленные поля, `parse →
to_bytes` побайтово, сохранность неизвестных полей (см. §3.1/§3.2), а для
`.sbt` ещё `rebuild(verify=True)` (структурные кодировщики записей 11 и 12
дают те же байты, что лежат в файле).

### 4.3 `--engine` (настоящий движок)

```
$ python3 tools/qa/fixtures.py --check /tmp/opencode/fixtures --engine \
      --binary /home/nooverload/gunmetal-re/build/sandbox/engine/gunmetal
...
[ok  ] engine fixture.sbw          kind=SBW level textures=1 effects=0 sectors=1 objects=1 vertices=4 polygons=1 header=133 bytes data end=365 bytes
[ok  ] engine fixture.sbt          kind=SBT level version=0 records=3 poly handles=0 object handles=0 sector handles=0 sobs=0 script=things=0 handles=1 code=1 flag=0
[ok  ] engine fixture.sbc          kind=SBC lightmaps magic=SBC0 records=1 texels=4 first=2x2
[ok  ] engine fixture.sbm          kind=SBM texture dimensions=8x8 levels=1 flags=0 mip0=8x8
[ok  ] engine fixture.sob          kind=SOB model vertices=3 faces=1 first texture=FIXTURE
[ok  ] engine fixture.soo          kind=SOO model animation mode=0 declared frames=1 frames=1 first=fixture 100 ticks
[ok  ] engine fixture.sml          kind=SML markup note=plain chars=30 lines=2 tags=6 images=0 fonts=0 music=0 text runs=1 document=ok
[ok  ] engine fixture.idx          kind=BIGFILE index entries=1 data=76 bytes first=FIXTURE (76 bytes)
[ok  ] engine level --info     textures=1 effects=0 sectors=1 objects=1 vertices=4 polygons=1 bytes=365 | records=3 poly=0 object=0 sector=0 sobs=0 script=1/1 | 11:1 12:1 255:1 | magic=SBC0 records=1 texels=4
```

Ожидаемые числа `--engine` берёт **из оракула** (`gm.*`), а не из констант в
скрипте: сверяется именно паритет «Python == движок» на одних и тех же байтах.
Уровень грузится целиком (`-l fixture --info`): `.sbw` + `.sbt` + `.sbc`
собираются в один `LevelBundle`, поэтому проверены и `load_level`, и все три
читателя одновременно. Версия движка: `gunmetal 0.1.0`.

### 4.4 `--self-test` (без игры)

```
$ python3 tools/qa/fixtures.py --self-test
[ok  ] write                              10 files
[ok  ] write is deterministic             same sha256 on a second write
[ok  ] check generated set                9 checked file(s), 0 error(s)
[ok  ] corrupt fixture.sbw                vertex count high byte 0 -> 1 (truncated): ValueError
[ok  ] corrupt fixture.sbt                magic 'BINARY SAVE' -> 'CINARY SAVE': ValueError
[ok  ] corrupt fixture-unknown.sbt        magic byte: CheckFailed
[ok  ] corrupt fixture.sbc                magic 'SBC0' -> 'XBC0': ValueError
[ok  ] corrupt fixture.sbm                log2(width) 3 -> 13 (implausible): ValueError
[ok  ] corrupt fixture.sob                vertex count '3' -> '9' (truncated list): ValueError
[ok  ] corrupt fixture.soo                mode '0' -> 'x' (header not integers): ValueError
[ok  ] corrupt fixture.sml                first '<' of <d> -> 'x' (no tags): CheckFailed
[ok  ] corrupt fixture.idx                entry 0 offset 0 -> 255 (beyond DAT): ValueError
[ok  ] payload flip: parse ok, content mismatch parser accepts any pixel; --check compares the expected bytes
[ok  ] unknown record rejected            unknown record type 66
status: ok (rc=0)
```

Каталог создаётся в `/tmp/opencode` (или `/var/tmp`, если первый недоступен) и
удаляется; игра не нужна.

### 4.5 Коды возврата и `--json`

| код | значение |
|---|---|
| 0 | всё проверено (для `--engine` — все числа совпали, все порчи пойманы либо отмечены как ограничение) |
| 1 | расхождение значения (файл прочитан, но поле/размер/round-trip не те) |
| 2 | жёсткая ошибка: нет файла/бинарника, формат отвергнут, испорченный вход принят |

`--json` печатает **одну** строку (`ok`, `rc`, `steps.{write,check,engine,
engine-corrupt,self-test}`), человеческие строки подавляются; `--quiet`
оставляет только ошибки и итог.

## 5. Порча байта

Один структурный байт на формат; порча пишется в `DIR/corrupt/` (копия
`fixture.dat` кладётся рядом, чтобы BIGFILE падал на смещении, а не на
отсутствующем DAT):

| файл | смещение | было → стало | оракул | движок |
|---|---:|---|---|---|
| `fixture.sbw` | 134 | `n_verts` hi `0x00→0x01` (4→260) | `ValueError: truncated .sbw …` | rc=2 `[error] sbw: truncated vertices in sector 0 object 0` |
| `fixture.sbt` | 0 | `'B'→'C'` | `ValueError: bad .sbt magic` | rc=2 `[error] sbt: bad .sbt magic` |
| `fixture-unknown.sbt` | 0 | `'B'→'C'` | `CheckFailed` (отказ не по той причине) | rc=2 `[error] sbt: bad .sbt magic` |
| `fixture.sbc` | 0 | `'S'→'X'` | `ValueError: bad .sbc magic` | rc=2 `[error] sbc: unknown .sbc magic 'XBC0'` |
| `fixture.sbm` | 4 | `log2w 3→13` | `ValueError: implausible dimensions` | rc=2 `[error] sbm: implausible dimensions 2^13 x 2^3` |
| `fixture.sob` | 1 | `'3'→'9'` (вершин) | `ValueError: truncated vertex list` | rc=2 `[error] sob: bad vertex 3` |
| `fixture.soo` | 0 | `'0'→'x'` (mode) | `ValueError: header is not two integers` | rc=2 `[error] soo: not a .soo: header is not two integers` |
| `fixture.sml` | 0 | `'<'→'x'` | `CheckFailed` (нет `<d>`) | **rc=2**, `[error] dump: not SML markup`, см. §6.2 |
| `fixture.idx` | 20 | offset `0→255` | `ValueError: entry 0 offset 255 beyond DAT` | rc=2 `[error] dump: entry 'FIXTURE' has a bad offset` |

Проверка движка (фрагмент вывода `--engine`):

```
[ok  ] engine corrupt fixture.sbw          rc=2 [error] sbw: truncated vertices in sector 0 object 0
[ok  ] engine corrupt fixture.sbt          rc=2 [error] sbt: bad .sbt magic
[ok  ] engine corrupt fixture.sbc          rc=2 [error] sbc: unknown .sbc magic 'XBC0'
[ok  ] engine corrupt fixture.sbm          rc=2 [error] sbm: implausible dimensions 2^13 x 2^3
[ok  ] engine corrupt fixture.sob          rc=2 [error] sob: bad vertex 3
[ok  ] engine corrupt fixture.soo          rc=2 [error] soo: not a .soo: header is not two integers
[note] fixture.sml: engine accepted corruption ... with rc=0; the --dump path has no error branch for this format (oracle rejects it)
[ok  ] engine corrupt fixture.idx          rc=2 [error] dump: entry 'FIXTURE' has a bad offset
```

Байты полезной нагрузки (пиксели `.sbm`/`.sbc`, координаты `.sob`, код после
`0xFF`) **не** probes: их изменение даёт такой же валидный файл, и называть это
«поймано форматом» было бы ложью. Строка self-test `payload flip` показывает
разницу явно: после порчи пикселя `sbm.parse` по-прежнему успешен, а
`--check` падает на сверке содержимого — это проверка *фикстуры*, а не
валидация формата. Порча ловится на структурном байте — это и есть критерий
«не молчаливый успех».

## 6. Ограничения и что осталось

### 6.1 Неизвестные **записи** не сохраняются (by design)

Ни `gm/sbt.py`, ни `engine/src/res/sbt.cpp` не умеют пропускать неизвестный
тип: оба останавливают разбор (`unknown record type 66` /
`unsupported .sbt record type 66`). Поэтому «запись с неизвестным типом» живёт
в отдельном негативном файле `fixture-unknown.sbt`, а **сохранение неизвестных
полей** проверяется внутри известных записей: `record.raw` (payload as read),
`SOUND_SAMPLE_TABLE.name_slots[299]`, `Poly.raw` и `f74/f76/f7a`, слоты имён
`sbt`. Побайтовый round-trip это и доказывает.

Если понадобится *пропускать* неизвестные записи (расширяемый формат), это
изменение поведения обоих читателей — отдельная задача, здесь не делалось
(общие файлы `gm/*`, `engine/src/*` в этой задаче не правятся).

### 6.2 `.sml`: битая разметка теперь ошибка (исправлено 2026-10-23)

`gunmetal --dump x.sml` при провале `parse_sml` уходит в резервную ветку
(скан тегов + `sml_plain`). Раньше она возвращала 0 на любом входе; теперь
файл, который не разбирается **и** не обрамлён разметкой (`<...>`), даёт
`[error] dump: not SML markup: the document does not parse and has no <...>
frame` и `rc=2` (DoD: «тихие пропуски» запрещены). Поставочный
`SML/prelevel/dm4.sml` — несбалансированная, но настоящая разметка, поэтому
он остаётся на резервной ветке с `[warn]` и `rc=0` (проверено: 148 из 149
файлов разбираются нормально, `dm4.sml` — единственное исключение).
Порча первого `<` у `fixture.sml` теперь ловится движком (`rc=2`), оракул
`gm.sml` тоже её отвергает.

### 6.3 Читатели не сверяют «хвост» `.sbw`/`.sbc`/`.sbm`/`.sbt` с размером

Ни Python, ни C++ не требуют `data_end == len(file)` (у `.sbt` — требуют,
у остальных — нет). Проверено: если обнулить `count3` в `fixture.sbw`
(смещение 22), оба читателя рапортуют успех с `sectors=0`, `data end: 26`
при размере 365:

```
$ python3 - <<'EOF'
import sys; sys.path.insert(0,".")
from gm import sbw
d = bytearray(open("/tmp/opencode/fixtures/fixture.sbw","rb").read()); d[22] = 0
open("/tmp/opencode/fixtures/sbw-nosectors.sbw","wb").write(bytes(d))
w = sbw.parse(bytes(d)); print(len(w.sectors), w.data_end, len(d))
EOF
0 26 365

$ gunmetal -g /tmp/opencode/fixtures --dump \
      /tmp/opencode/fixtures/sbw-nosectors.sbw | tail -7
sectors: 0
objects: 0
vertices: 0
polygons: 0
header: 26 bytes
data end: 26 bytes
first texture: FIXTURE
```

Это не «тихий пропуск CLI» (команда отработала, числа честные), но и не
валидация: хвост просто не читается. Поэтому probes выбирают байты, которые
обязаны ломать разбор (счётчики, магии), а не «любой байт файла». Исправление
(требовать EOF) меняет поведение `gm/sbw.py`, `gm/sbc.py`, `gm/sbm.py` и
`engine/src/res/*` — вне списка файлов этой задачи.

### 6.4 `SOUND_SAMPLE_TABLE` и `SOO`: объявленные счётчики не строгие

`.sbt`-запись 12 — фиксированные 300 слотов, поэтому файл не может быть
короче; «count=1» здесь — значение `counts[0]`, а не длина. У `.soo`
объявленный `count` не обязан совпадать с числом строк: движок и оракул
одинаково играют только первые `count` кадров (так в поставочных
`nblast`/`nboom`/`ruinbeam`), поэтому фикстура имеет `count == len(frames) == 1`
и `dropped_frames() == []`.

### 6.5 Чего нет

* `.sbv`, `.sou`, `.soa`, `.soe`, `.sog`, `.sof`, `.bmp`, `.sub`, `.key`,
  `.hi`, `.cfg` — вне списка `M3.2.7`; для `sbv`/`soe`/`sog`/`soa` уже есть
  C++-голдены (`core_full`/`re_*`), `sou` требует энтропийного кодека и
  звукового каталога, `.soe` — грамматика токенов ещё частично неизвестна;
* `fixture.dat` сам по себе не проверяется: любой набор байт — валидный блоб
  BIGFILE, структурен только индекс;
* PNG-экспорт `.sbm`/`.sbc` не проверяется: нужна палитра из установки.

## 7. Интеграция (делает основной агент)

Файлы этой задачи — только `tools/qa/fixtures.py` и `docs/FIXTURES.md`;
CTest/`regress.py`/`ROADMAP.md` добавляет основной агент. Предлагаемые точки:

* CTest (без игры): `add_test(NAME fixtures COMMAND python3
  ${CMAKE_SOURCE_DIR}/tools/qa/fixtures.py --self-test)` — 0.05 с, метка
  `quick`;
* CTest (с движком): `fixtures_engine` с `--write ${CMAKE_BINARY_DIR}/fixtures
  --engine --binary $<TARGET_FILE:gunmetal>` — числа сверяются с оракулом,
  порча обязана давать rc=2;
* `regress.py`: проверка `python3 tools/qa/fixtures.py --self-test` в
  категории `formats` (или `save`, где уже живут писатели) — ожидаемая строка
  `status: ok (rc=0)`;
* `ROADMAP.md` `M3.2.7`: дописать, что синтетический минимальный набор
  добавлен (движок принял 8/8 форматов, `.sml` — с оговоркой §6.2).

Команда для перепроверки всего пакета:

```sh
python3 tools/qa/fixtures.py --write /tmp/opencode/fixtures \
  && python3 tools/qa/fixtures.py --check /tmp/opencode/fixtures \
  && python3 tools/qa/fixtures.py --check /tmp/opencode/fixtures --engine \
       --binary /home/nooverload/gunmetal-re/build/sandbox/engine/gunmetal \
  && python3 tools/qa/fixtures.py --self-test
```
