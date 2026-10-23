# RELEASE — чек-лист релиза (`M12.8`)

Снимок 2026-10-23. Документ фиксирует **порядок** выпуска и фактический статус
каждого шага; тег `v1.0` пока не ставится — см. §5 «Блокеры».

## 1. Предусловия

| что | проверка |
|---|---|
| лицензионная установка Gunmetal v1.36 3dfx | `python3 tools/re/install_audit.py --game '<DIR>'` → `RESULT: ok` |
| чистое дерево на `master` | `git status --short` пусто |
| тулчейн | CMake ≥ 3.25, Ninja, C++20 (или портативный `~/tools` — `docs/BUILD.md`) |

## 2. Гейты (все обязаны быть зелёными)

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja
cmake --build build/sandbox -j 8
ctest --test-dir build/sandbox -j 4 --output-on-failure     # сейчас 158/158
python3 regress.py --full --mirror --json                   # сейчас 158/158
python3 tools/re/install_audit.py --game "$GUNMETAL_DIR"    # RESULT: ok
python3 tools/qa/perf.py --game "$GUNMETAL_DIR" --repeat 3  # снимок, не гейт
```

Санитайзеры — отдельная конфигурация (`docs/SANITIZERS.md`):

```sh
cmake --preset asan --fresh && cmake --build --preset asan -j 8
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/asan -j 4   # сейчас 101/101
```

Фаззинг: `python3 tools/fuzz_formats.py --game "$GUNMETAL_DIR"` (полная
матрица) и `python3 tools/fuzz_formats.py --game "$GUNMETAL_DIR" --quick`
(входит в `regress.py`).

## 3. Что входит в релиз

* бинарник `gunmetal` (`linux-x86_64`), `docs/` (или собранный индекс),
  `README.md`, `ROADMAP.md`, `CURRENT_STATE.md`, `NEXT.md`;
* **никаких ассетов игры** (`M12.7`): ни одного файла из установки, ни
  производных (декодированных `.wav`/`.png`/`.obj` из поставочных данных);
* скрипты разработчика (`tools/`, `gm/`, `regress.py`) — по желанию; для
  запуска игры не нужны.

Проверка «нет ассетов»:

```sh
git ls-files | grep -iE '\.(sbt|sbw|sbc|sbm|sbv|so[abefgho]|sog|sou|key|bmp|wav|png|obj)$'
# docs/avatar.png — единственный файл; это производный рендер модели Tick
# (README), к релизу его нужно убрать или заменить (M12.7), см. §5.
```

## 4. Версия и тег

Версия живёт в `engine/src/core/config.h` (`kVersion`), печатается
`gunmetal --version` (сейчас `0.1.0`). Порядок выпуска:

```sh
git tag -a v1.0 -m 'Gunmetal re-release v1.0'
git push origin v1.0        # только после §5
```

Тег **не ставится**, пока не закрыты блокеры: он обещает играбельный релиз,
а не «набор проверенных данных и рендера».

## 5. Блокеры (проверено по `ROADMAP.md`)

| блокер | статус |
|---|---|
| игровой цикл с фронтендом (меню, смерть/респавн, титры) | ⬜ `M8`/`M9.5` — только RE |
| автопрохождение кампании | ⬜ `M9.6` |
| 60 FPS в 320×200 и 640×400 | ⬜ `M12.3` — измерен только холодный прогон (`docs/PERFORMANCE.md`) |
| Windows-сборка и CI-матрица | ⬜ `M12.2` (Linux x86-64 уже 64-битный) |
| DM с ботами | ⬜ `M6.5.1` |
| редактор уровней/сцены/скриптов | 🟡 `M11.2`/`M11.3`/`M11.4` (данные пишутся: `sbtasm`/`sbtwrite`/`bigpack`) |
| `docs/avatar.png` — производный рендер модели из `SOBS/` | ⬜ `M12.7`: убрать/заменить до релиза (README ссылается на него) |

Когда блокеры закроются, §2–§4 выполняются дословно, а `ROADMAP.md` меняет
`M12.8` на ✅.
