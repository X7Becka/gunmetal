# Фазз-корпус: сохранение и повтор (save / replay)

`tools/fuzz_formats.py` гоняет матрицу враждебных входов (пустые, обрезанные,
мутированные, extreme-header) через парсеры `gm/`. До сих пор матрица
пересобиралась заново при каждом запуске: найденный «интересный» вход нельзя
было ни сохранить, ни воспроизвести, а `--seed` не влиял на данные вовсе
(генератор сидел на собственных константах `random.Random(0xC0FFEE ^ n)`).

Теперь у харнесса есть **persistent-корпус**: `--save DIR` кладёт на диск
каждый сгенерированный вход и манифест, `--replay DIR` прогоняет сохранённые
байты через те же парсеры, ничего не генерируя.

## 1. Команды

```bash
# обычный прогон (как раньше): матрица в памяти, на диск ничего не пишется
python3 tools/fuzz_formats.py --game "$HOME/.cache/gunmetal-re/game" --quick

# сохранить корпус (seed задаёт данные)
python3 tools/fuzz_formats.py --game "$HOME/.cache/gunmetal-re/game" \
        --quick --seed 7 --save /var/tmp/fuzz-corpus

# тот же каталог через --corpus (эквивалентно)
python3 tools/fuzz_formats.py --game "$HOME/.cache/gunmetal-re/game" \
        --quick --seed 7 --corpus /var/tmp/fuzz-corpus --save

# повторить сохранённые входы (каталог игры не нужен)
python3 tools/fuzz_formats.py --replay /var/tmp/fuzz-corpus

# машинно-читаемые отчёты
python3 tools/fuzz_formats.py --replay /var/tmp/fuzz-corpus --json
python3 tools/qa/fuzz_corpus.py --dir /var/tmp/fuzz-corpus [--replay] [--json]
```

| флаг | смысл |
|---|---|
| `--seed N` | seed генератора; реально меняет данные, воспроизводим |
| `--save [DIR]` | сохранить сгенерированные входы в `DIR` (или в `--corpus`) |
| `--corpus DIR` | каталог корпуса (в паре с `--save`) |
| `--replay DIR` | прогнать сохранённый корпус вместо генерации |
| `--json` | одна строка JSON вместо текстового отчёта |
| `--quick` / `--timeout` / `--game` | как раньше |

Коды возврата: `0` — чисто, `1` — провал кейса или битый корпус, `2` — ошибка
аргументов (нет каталога игры, `--replay` вместе с `--save`).

## 2. Раскладка корпуса

```
/var/tmp/fuzz-corpus/
├── manifest.json
└── cases/
    ├── 7_bigfile_trunc_00000.bin      # seed=7, target=bigfile, kind=trunc, #0
    ├── 7_bigfile_mutation_00000.bin
    ├── 7_sbv_extreme_00000.bin
    └── …                              # 552 файла на --quick
```

Имя файла детерминировано: `<seed>_<target>_<kind>_<index>.bin`, где `index` —
счётчик внутри пары (target, kind). Запись атомарная (временный файл +
`os.replace`), так что прерванный прогон не оставляет half-записанных файлов.
Повторный `--save` в тот же каталог перезаписывает файлы с теми же именами;
файлы от других seed остаются лежать, но в манифест не попадают и при replay
не используются.

`manifest.json`:

```json
{
  "cases": [
    {
      "extra": {"cut": 0},
      "file": "cases/7_bigfile_trunc_00000.bin",
      "kind": "trunc",
      "name": "bigfile trunc[0]",
      "seed": 7,
      "seq": 0,
      "sha256": "e3b0c442…52b855",
      "size": 0,
      "target": "bigfile"
    }
  ],
  "digest": "e049b94273d28e29…",
  "format": "gunmetal-fuzz-corpus",
  "meta": {
    "case_count": 552, "game": "/…/gunmetal-re/game", "limit": 12,
    "mode": "quick", "seed": 7, "timeout": 2.0,
    "tool": "tools/fuzz_formats.py", "tool_version": 2
  },
  "version": 1
}
```

* `target` — что именно парсится (`bigfile`, `sbv`, `sbw`, `sbt`, `sbm`,
  `sbc`, `sof`, `fdx`, `soo`, `soe`, `soa`, `sou1`);
* `kind` — `trunc` / `mutation` / `extreme` / `payload` / `dat_size`;
* `name` — метка кейса, ровно та же, что печатается при провале;
* `sha256`/`size` — контроль целостности входа;
* `seed` — seed, которым получен этот вход;
* `extra` — параметры кейса (`cut`, `width`/`height`, `count`, `dat_size`…);
* `digest` — sha256 канонического JSON из `format`/`version`/`cases`
  (не зависит от пути к корпусу).

В манифесте **нет времени и других «плавающих» полей** — иначе два прогона с
одинаковым seed давали бы разные файлы манифеста.

## 3. Детерминизм (проверено)

* Каждой паре (target, kind) соответствует свой поток RNG, seed которого —
  `sha256("<seed>|<target>|<kind>")`: потоки не зависят от порядка обхода и
  одинаковы на любой машине и в любой версии Python.
* Один и тот же seed + та же установка ⇒ **побайтово одинаковые** и файлы
  входа, и `manifest.json`.

Проверка (см. §6): два прогона `--save` с `--seed 7` в разные каталоги дали
одинаковый sha256 `manifest.json`
(`7e17213f9483c4a9a1509bc0b6b6d16d7b276f02616725e7b12eb6e993fc0955`) и
одинаковый digest дерева файлов. Другой seed (`8`) дал другой манифест и
другие данные: из 552 кейсов 263 payload’а отличаются (зависящие от seed
`trunc`/`mutation`/`payload`), 289 совпадают (детерминированные extreme-шаги:
лестница обрезки, header-мутации, extreme-заголовки, `dat_size`).

## 4. Повтор (`--replay`)

1. читается `manifest.json`, проверяется `format`/`version`;
2. пересчитывается `digest` манифеста — правка манифеста ловится сразу
   (`manifest digest mismatch`);
3. для каждого кейса файл читается, сверяются `size` и `sha256`;
4. оставшиеся кейсы прогоняются через **те же** функции парсеров, что и
   генератор (`gm.sbv.parse`, `gm.bigfile.BigFile`, …), с теми же метками.

Битый вход не «лечится» и не подменяется: кейс пропускается, в отчёт
попадает строка

```
corpus: cases/7_sbw_mutation_00003.bin: sha256 mismatch: expected 82c2c592…, got f133fcd0…
```

а код возврата — `1`. Повторный `--replay` того же корпуса даёт тот же
результат (число кейсов и провалов).

Каталог игры для повтора **не нужен**: все байты лежат в корпусе.

## 5. Отчёт `--json`

```json
{"cases": 552, "failures": 0, "fuzz_failures": 0, "corpus_errors": 0,
 "corpus": "/var/tmp/fuzz-corpus", "seed": 7, "replay": true, "mode": "replay",
 "manifest_digest": "e049b942…", "manifest_sha256": "7e17213f…",
 "inputs": [{"target": "bigfile", "kind": "trunc", "name": "bigfile trunc[0]",
             "file": "cases/7_bigfile_trunc_00000.bin", "sha256": "e3b0c442…",
             "size": 0}, …],
 "errors": []}
```

* `inputs` — sha256 **каждого** входа (в обычном прогоне — тоже, по памяти);
* `manifest_digest` — путе-независимый digest корпуса, `manifest_sha256` —
  sha256 самого файла манифеста;
* `seed` в режиме replay берётся из манифеста (seed, которым получен корпус);
* `failures` = `fuzz_failures` + `corpus_errors` (это же число стоит в строке
  итога).

## 6. Совместимость

* Строка итога осталась прежней и остаётся **первой строкой stdout**:
  `fuzz: N cases, M failure(s)` — её разбирает `regress.py` (проверка
  «trust / malformed-input fuzz matrix» показывает её в деталях).
* Обычный прогон (без `--save`/`--replay`) ничего не пишет на диск, кроме
  временного файла для BIGFILE-кейсов (как и раньше); каталог корпуса не
  создаётся.
* В обычном (не `--json`) режиме отчёт не изменился: строка итога, затем
  строки провалов, затем «… N more».

## 7. Что изменилось в самой матрице

* Лестница обрезки теперь добирает `limit` **различных** случайных точек
  (раньше — с повторами, поэтому число кейсов зависело от случайных
  совпадений RNG и менялось от seed к seed). Теперь число кейсов
  **не зависит от seed**: 552 на `--quick` и 1224 без `--quick`
  (вместо 545/1184 — на 7 больше из-за трёх маленьких файлов, где старый
  генератор ловил совпадения). Документация со старыми числами
  (`docs/SECURITY.md`, `docs/REGRESSION.md`) требует обновления.
* `--seed` теперь действительно влияет на данные (см. §3).

## 8. Размер и хранение

Корпус на `--quick` — около 207 МБ (552 файла); больше половины — группа
`sbv` (`Vid/Intro.sbv` ≈ 4 МБ на каждый из 77 кейсов). Корпус **не должен
лежать в репозитории**: путь передаётся снаружи (`/var/tmp/…`, `~/.cache/…`).
Если всё-таки кладут рядом с исходниками — добавьте каталог в `.gitignore`
сами.

## 9. Ночной прогон

```bash
#!/bin/sh
set -e
seed=$(( $(od -An -N4 -tu4 /dev/urandom) % 100000 ))
dir=/var/tmp/fuzz-corpus-$seed
python3 tools/fuzz_formats.py --game "$HOME/.cache/gunmetal-re/game" \
        --seed "$seed" --save "$dir"
python3 tools/fuzz_formats.py --replay "$dir"        # повтор должен дать то же
python3 tools/qa/fuzz_corpus.py --dir "$dir"         # целостность корпуса
```

`tools/qa/fuzz_corpus.py` — отдельный инспектор корпуса: печатает digest,
гистограмму по target/kind, проверяет sha256 всех файлов (`--json` — одна
строка JSON) и по `--replay` прогоняет корпус через парсеры.
