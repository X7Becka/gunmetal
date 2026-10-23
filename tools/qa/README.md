# `tools/qa` — проверки, аудит и регресс

Здесь лежат проверки **поверх** готовых компонентов: они не разбирают новые
форматы, а сверяют уже существующие данные, доки и движок. У каждого
инструмента есть `--self-test` (без установки игры) и понятные коды возврата
`0` — успех, `1` — расхождение, `2` — не удалось прочитать вход.

Большинство инструментов зарегистрированы в CTest (`engine/tests/CMakeLists.txt`)
и попадают в `ctest`; те, что требуют долгого прогона или пишут в дерево,
запускаются вручную.

| инструмент | что проверяет | CTest | заметки |
|---|---|---|---|
| `doc_numbers.py` | числа, процитированные в RE-доках, пересчитаны заново | `doc_numbers` | **52/52 по 18 докам**; `--check --strict`; см. `docs/AUDIT_NUMBERS.md` |
| `schema_check.py` | схема форматов: C++ структуры ↔ Python-парсеры ↔ данные | `schema_check_selftest`, `schema_check` | `M3.2.1`; ловит дрейф полей; `docs/SCHEMA.md` |
| `fixtures.py` | минимальные синтетические фикстуры всех форматов и их чтение движком | `fixtures_selftest`, `fixtures_write`, `fixtures_check` | `M3.2.7`; побайтовый round-trip; `docs/FIXTURES.md` |
| `sim_check.py` | безголовый тик: `--sim`, детерминизм дайджеста, план `--sim-drive` | `sim_check`, `sim_drive_check`, `sim_corpus` | корпус 36 уровней; см. `docs/SIM.md` |
| `mod_regress.py` | правка мода → загрузка движком | `mod_regress_*` | `M11.7`; по одному тесту на мод |
| `perf.py` | снимок производительности (загрузка/рендер/декод) | `perf_selftest` | `M12.3`; числа зависят от машины, отчёт, а не гейт |
| `state.py` | снимок `CURRENT_STATE.md` сходится с HEAD и `ROADMAP.md` | вручную (`--check`) | счёт коммитов пишется **после** коммита самого файла |
| `coverage.py` | покрытие Python-оракула (stdlib `trace`) | вручную | нужен прогретый прогон `regress.py` |
| `fuzz_corpus.py` | инспекция/целостность накопленного фазз-корпуса | вручную | корпус создаёт `tools/fuzz_formats.py --save` |

## Как запускать

```sh
python3 tools/qa/<tool>.py --self-test          # без игры
python3 tools/qa/<tool>.py --check              # на установке игры
python3 tools/qa/<tool>.py --json               # машинный вывод
ctest --test-dir build/sandbox -R '^(schema_check|fixtures)_' -V
```

Игра по умолчанию — `/mnt/d/games/Gunmetal 3dfx`, зеркало —
`~/.cache/gunmetal-re/game`; оба только читаются. Инструменты **не пишут**
в каталог игры: результаты идут в `build/` или `/tmp/opencode`.
