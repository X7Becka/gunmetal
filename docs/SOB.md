# `.sob` — модели объектов: формат, писатель, round-trip

Модели `SOBS/*.sob` (672 файла в поставке) — плоский текстовый поток токенов.
Читатель и писатель живут в `gm/sob.py`, инструмент проверки/пересборки —
`tools/re/sob_writer.py`. Формат открыт в M1.1 (чтение), здесь закрывается
**M1.4 — запись**: `Sob.to_bytes()` / `Sob.encode()`, `write()` / `rebuild()`,
побайтовый round-trip и правка вершин.

> Переводы строк не значимы для парсера: файл — поток токенов, разделённых
> пробелами/CRLF. Однако **раскладка строк значима для побайтового
> round-trip**: в большинстве файлов имя первой текстуры грани стоит в конце
> «строки плоскости» (см. §2.2).

## 1. Структура

```
<vertex_count> <face_count>

vertex_count * ( x y z )                    // float, мировые единицы

face_count * {
    8 токенов-имён                          // [0] — диффузная текстура,
                                            // [1..7] — флаги материала
                                            //   ("none", "translucent", "add")
    flags nverts i0 .. i(nverts-1)          // целые; 3 <= nverts <= 32
    15 чисел                                // origin(3) U(3) V(3) N(3) spare(3)
    [ -1 ]                                  // необязательный одиночный -1
                                            // (в поставке не встречается)
}
```

* `origin/u/v/normal/spare` — 15 чисел на грань. `U/V` — единичные оси,
  умноженные на 65536 (16.16-проекция); `UV = dot(p - origin, axis)/65536`.
  В `spare` обычно лежит маркер `(0, <константа файла>, 0)`: чаще всего
  `12345678` (10 753 грани) или `16539982` (10 961), встречаются `12345934`,
  `16540494` и др. Писатель переносит его как есть.
* Целые `flags/nverts/индексы` и 15 чисел грани — отдельные токены; имя
  текстуры — тоже токен и может быть числом (`0`, `none`, `TICK`, `H-Dev`…).
* В поставке **нет** ни одного файла, где срабатывает ветка необязательного
  `-1` *после* 15 чисел (`extra=1`, 0/672). Тот `-1`, который в варианте B
  стоит в конце целочисленной строки, парсер — как и движок (`res/sob.cpp`) —
  читает как **первое из 15 чисел** грани, поэтому у B-моделей `origin.x == -1`
  (сдвиг на единицу по X, на UV/рендер не влияет). Ветка `extra=1` сохранена
  для совместимости и проверена `--self-test`.

### 1.1. Числа

Писатель повторяет орфографию исходного генератора:

* неотрицательное число печатается с пробелом на месте знака, затем пробел:
  ` 0 `, ` 65536 `, ` 12345678 `; отрицательное — без ведущего пробела:
  `-65536 `, `-1.5 ` (аналог `printf("% d ")` в C);
* целые значения (включая `12.0`) печатаются без дробной части: `12`;
* дробные — в кратчайшей форме, которая точно читается обратно
  (Python `repr`): `26214.400391`, `-55245.5`, `1.22124532708767e-15`.

Поэтому `parse → to_bytes → parse` не теряет ни одного бита значения.

### 1.2. Лимиты (граница доверия)

| Константа | Значение | Зачем |
|---|---|---|
| `MAX_VERTS` | 200 000 | крупнейшая модель — 957 вершин |
| `MAX_FACES` | 5 000 | крупнейшая модель — 1 130 граней |
| `nverts` на грань | 3..32 | как в движке (`res/sob.cpp`) |
| индексы | `0 <= i < vertex_count` | иначе парсер/писатель отказывают |

Счётчики проверяются **до** выделения памяти: `"999999999999 0"`,
`"0 999999999999"` и обрезанный поток отвергаются (`ValueError`), а не
приводят к огромному циклу/рекурсии. Рекурсия по граням мемоизирована по
позиции токена, поэтому разбор остаётся линейным.

## 2. Раскладка

### 2.1. Каноническая (`Sob.to_bytes`)

```
 957 \r\n 1130 \r\n              счётчики, каждый на своей строке
 0  37  61 \r\n                 по 3 вершины на строку
 0  16  61 \r\n
 …
TICK\r\nnone\r\n…\r\n           8 имён, каждое на своей строке
 96  6  10  8  6  4  2  0 \r\n  flags, nverts, индексы
 12928  6400 … 12345678  0 \r\n 15 чисел грани
```

Это «чистый» вариант A: 8 имён на своих строках, 15 чисел на своей, CRLF.
Он парсится обратно в ту же модель и совпадает с грамматикой из
`docs/FORMATS.md` §3.

### 2.2. Как это лежит в поставке

Исходный генератор писал несколькими вариантами; все они читаются одним
парсером:

| вариант | признак | файлов |
|---|---|---|
| A | `-1` нет, после 15 чисел — CRLF | 77 |
| B | сигнальный `-1` в конце целочисленной строки, после 15 чисел **нет** CRLF → имя первой текстуры следующей грани оказывается в конце «строки плоскости» | 436 |
| B + финальный CRLF | то же + перевод строки в конце файла | 50 |
| «длинные» float | числа в `%.15G`-подобной форме (`-24.0000000000001`, `1.22124532708767E-15`) | 96 |
| прочее | MOCLASS.SOB — вообще однопробельная раскладка (`957\r\n1130\r\n0 37 61\r\n…`); ещё 12 файлов с иной орфографией `E`-чисел | 13 |

Канонический писатель **не** воспроизводит эти варианты: он всегда пишет
вариант A. Побайтовое совпадение для неизменённой модели даёт сохранённый
`Sob.raw` (§3.2), а не канонический текст.

## 3. API `gm/sob.py`

### 3.1. Чтение

```python
from gm import sob

m = sob.parse("SOBS/tick.sob")          # читает байты, кладёт их в m.raw
m = sob.parse_data(data, name="tick")   # из bytes
m = sob.parse_tokens(tokens, name="")   # без raw (для тестов/сборки)
```

`Sob(vertices, faces, name, raw)` и `SobFace(textures, flags, indices,
origin, u, v, normal, spare)` — обычные dataclass'ы; `raw` имеет значение по
умолчанию, поэтому старый код (`Sob(verts, faces, "x")`) не меняется.

### 3.2. Запись

| метод | что делает |
|---|---|
| `m.to_bytes(newline="\r\n")` | канонический текст (§2.1), `bytes` в latin-1 |
| `m.encode()` | алиас `to_bytes()` для собранной/отредактированной модели |
| `m.write(path, force=False, exact=True)` | атомарная запись; при `exact` и неизменённой модели пишет исходные `raw` байты. Возвращает записанные байты |
| `m.rebuild(path, force=False)` | атомарная запись **канонического** текста (сбрасывает исходную раскладку) |
| `m.is_unmodified()` | `True`, если модель всё ещё равна разобранной из `raw` (сравнение вершин/граней/текстур) |
| `m.content_equal(other)` | структурное равенство моделей |
| `m.move_vertex(i, dx, dy, dz)` | сдвиг вершины на месте, возвращает новую позицию |
| `sob.atomic_write(path, data)` | `mkstemp` в том же каталоге + `fsync` + `os.replace` |

Политика перезаписи: существующий файл заменяется только с `force=True`,
иначе `FileExistsError`. Временный файл (`.sob-*.tmp`) удаляется при любой
ошибке.

`to_bytes()` проверяет представимость модели и бросает `ValueError`:
не 8 текстур, пустой/пробельный токен имени, `nverts` вне 3..32, индекс вне
диапазона, `NaN`/`inf`, превышение `MAX_VERTS`/`MAX_FACES`. Это защищает от
записи файла, который потом не прочитается.

## 4. Статус round-trip (поставка, 672 модели)

Проверено `python3 tools/re/sob_writer.py --check`:

| метрика | результат |
|---|---|
| разбор | 672 / 672 |
| vertices / faces | 31 090 / 24 036 |
| **семантический** round-trip (`parse → encode → parse`, вершины/грани/плоскости/текстуры) | **672 / 672** |
| **побайтовый** round-trip неизменённой модели (`raw`) | **672 / 672** |
| канонический текст совпал с файлом поставки | 77 / 672 |
| FNV-1a64 канонических байтов всех моделей | `0x8f43045cd57a1222` |

Важно: **канонический writer не является побайтовым**. Для 595 файлов его
вывод отличается от поставки только раскладкой/орфографией чисел (значения
те же); побайтовое совпадение даёт `raw`. Единственный файл с другой
грамматикой раскладки — `MOCLASS.SOB` (957/1130, однопробельные строки); он
читается, редактируется и пишется канонически, но не «как в поставке».

## 5. Инструмент `tools/re/sob_writer.py`

```bash
# проверка всех моделей поставки: parse -> encode -> parse, FNV, лимиты
python3 tools/re/sob_writer.py --check

# подробно: какие файлы не совпали с каноном побайтово
python3 tools/re/sob_writer.py --check --verbose

# пересборка в каноническом виде (атомарно, без перезаписи без --force)
python3 tools/re/sob_writer.py --rebuild /tmp/opencode/sob-out

# побайтовая пересборка: неизменённые модели пишутся из raw
python3 tools/re/sob_writer.py --rebuild /tmp/opencode/sob-exact --exact

# правка вершины 0: сдвиг по Z, затем просмотр
python3 tools/re/sob_writer.py --edit 0,0,20 --out /tmp/opencode/tick-shift.sob \
    "/mnt/d/games/Gunmetal 3dfx/SOBS/tick.sob"
python3 gm.py view /tmp/opencode/tick-shift.sob \
    --game "/mnt/d/games/Gunmetal 3dfx" --out /tmp/opencode/tick-shift.png --ascii

# негативные тесты без игровых данных (битые счётчики, огромные n,
# обрезанный поток, nverts/индексы, NaN, политика --force, атомарность)
python3 tools/re/sob_writer.py --self-test
```

`--edit` принимает `X,Y,Z` (можно `0;0;20`), требует `--out FILE` или
`--out-dir DIR`; пишет канонический текст и печатает старую/новую позицию
вершины 0 и команду для `gm.py view`.

Игра только читается; `--rebuild`/`--edit` пишут в указанный каталог.

## 6. Интеграция в `gm.py` (предлагаемые строки, файл не менялся)

`gm.py` сейчас умеет только `--check` и OBJ/MTL. Глобальный `--force` уже
добавляется всем подкомандам в конце `main()` (`for _sp in sub.choices.values()`),
поэтому его достаточно использовать как `opts.force`, а не объявлять заново.
Проверенный (временно применённый к копии `gm.py`) вариант:

```python
def _sob_files(src: str) -> list[str]:
    if not os.path.isdir(src):
        return [src]
    return sorted(os.path.join(src, f) for f in os.listdir(src)
                  if f.lower().endswith(".sob"))


def cmd_sob(opts) -> int:
    if not opts.check and not opts.out:
        print("sob: OUT is required unless --check", file=sys.stderr)
        return 2
    ok = errs = 0
    for p in _sob_files(opts.src):
        try:
            if opts.check:
                sob.parse(p)
            elif opts.rebuild:
                sob.parse(p).rebuild(
                    os.path.join(opts.out, os.path.basename(p)), force=opts.force)
            else:
                sob.to_obj_file(p, opts.out)
            ok += 1
        except Exception as exc:  # noqa: BLE001 - report and continue
            errs += 1
            print(f"  FAIL {os.path.basename(p)}: {exc}", file=sys.stderr)
    what = "parsed only" if opts.check else opts.out
    print(f"sob: {ok} written, {errs} failed -> {what}")
    return 0 if errs == 0 else 1


# argparse, вместо существующего sub.add_parser("sob", ...)
p = sub.add_parser("sob", help="convert .sob models to OBJ/MTL, or rebuild them")
p.add_argument("src", help=".sob file or a directory of them")
p.add_argument("out", nargs="?")
p.add_argument("--check", action="store_true", help="parse only, write nothing")
p.add_argument("--rebuild", action="store_true",
               help="write the models back to OUT instead of OBJ/MTL")
p.set_defaults(func=cmd_sob)
```

Тогда работают:

```bash
python3 gm.py sob "…/SOBS" /tmp/opencode/sob-out --rebuild            # 672 written
python3 gm.py sob "…/SOBS/tick.sob" /tmp/opencode/one --rebuild      # один файл
python3 gm.py sob "…/SOBS" --check                                   # 672, 0 failed
python3 gm.py sob "…/SOBS" /tmp/opencode/obj                         # OBJ+MTL, как раньше
```

Для побайтовой пересборки в ветке `--rebuild` вместо `model.rebuild(...)`
нужно `model.write(target, force=opts.force, exact=True)`. Правка вершины —
это инструмент (`tools/re/sob_writer.py --edit`), а не `gm.py`: сдвигать одну
вершину у всех 672 моделей смысла нет.

Проверка в `regress.py` (после `sob / models`) может выглядеть так:

```python
rb = os.path.join(out, "sob")
run(["sob", f"{G}/SOBS", rb, "--rebuild"])
results.append(check("sob / writer (672 rebuilds)", ["sob", ...],
                     ["sob: 672 written, 0 failed"]))
```

## 7. Ограничения

* Canonical ≠ byte-exact: исходная раскладка (варианты A/B, финальный CRLF,
  орфография `E`-чисел, MOCLASS) сохраняется только через `raw`/`write(exact=True)`;
  `rebuild()` намеренно её сбрасывает.
* `face_count` из заголовка не сверяется с числом реально разобранных граней
  (парсер идёт по потоку); писатель всегда печатает фактическое
  `len(faces)`. В поставке расхождений нет (672/672).
* `encode()` не синтезирует UV/материалы: 15 чисел грани переносятся как
  есть. Правка геометрии не пересчитывает плоскости — для этого нужен
  отдельный генератор UV.
* Токены текстур пишутся как есть; имена с пробелами форматом не
  поддерживаются (в поставке таких нет).
* Числа хранятся как `float64` (после `float(token)`); исходные значения
  поставки — `float32`-совместимые, повторный `to_bytes` даёт кратчайшую
  запись, которая читается в то же значение.
* Рекурсивный обход граней повышает `sys.setrecursionlimit`; на модели
  `MOCLASS.SOB` (1 130 граней) это штатно.

## 8. Проверка

```bash
python3 tools/re/sob_writer.py --check          # 672/672 semantic, 672/672 raw
python3 tools/re/sob_writer.py --self-test      # 36 негативных/позитивных тестов
python3 gm.py sob "/mnt/d/games/Gunmetal 3dfx/SOBS" --check   # 672 converted, 0 failed
python3 gm.py sob "…/SOBS" /tmp/opencode/sob-obj             # 1344 OBJ+MTL
python3 regress.py                              # 101/101 checks
```
