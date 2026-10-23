# Sanitizers (ASan/UBSan)

Этот репозиторий на WSL использует Zig (`~/tools/bin/zigcc|zigcxx`), а не
системный GCC/Clang. У Zig есть UBSan, но **нет ASan-runtime**.

## Ловушка Zig

```text
-fsanitize=address,undefined   # ASan молча выброшен, остаётся только UBSan
-fsanitize=address -fsanitize=undefined   # работают оба
```

С запятой Zig не выдаёт ошибку и не линкует `__asan_*`: сборка зелёная,
память не проверяется. Поэтому `CMakeLists.txt` падает на такой записи для
компилятора Zig, а `asan`-пресет использует раздельные флаги.

## Установка runtime без sudo

```sh
tools/qa/install-asan-runtime.sh
```

Скрипт делает `apt-get download libasan8 libubsan1` и распаковывает их в
`~/tools/sanitizers` через `dpkg-deb -x`.

## Сборка и прогон

```sh
cmake --preset asan --fresh
cmake --build --preset asan --target gm_ai_tests gm_weapons_tests
ASAN_OPTIONS=detect_leaks=1 ./build/asan/engine/tests/gm_ai_tests
ASAN_OPTIONS=detect_leaks=1 ./build/asan/engine/tests/gm_weapons_tests
```

Проверка, что инструментация настоящая:

```sh
nm build/asan/engine/tests/gm_ai_tests | grep -c __asan_report   # > 0
ldd build/asan/engine/tests/gm_ai_tests | grep libasan
```

`ctest --test-dir build/asan -R '^sanitizer_selfcheck$'` — негативный гейт:
тест обязан аварийно завершиться из-за намеренного OOB и помечен `WILL_FAIL`.
Чистый exit 0 означает, что ASan снова не инструментирует код.

`ASAN_OPTIONS`/`UBSAN_OPTIONS` можно взять из
`/tmp/opencode/san/sanitizer-env.sh` (временный QA-каталог) или задать
`detect_leaks=1:halt_on_error=1`, `print_stacktrace=1`.

## Результат 2026-10-23 (весь набор)

```
cmake --preset asan --fresh
cmake --build --preset asan -j 8
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/asan -j 4
# 100% tests passed out of 101, real 416 s (quick 87 + full 13)
```

Инструментация настоящая: `nm … | grep -c __asan_report` = 5,
`ldd … | grep libasan` = 1. Отдельно проверены новые тесты `engine_3d`,
`vm_screens`, `parity`, `sound_catalog`, `postfx`, `damage_model` и
существующие `vm`, `weapons`, `ai`.

**Найдено и исправлено:** `engine/tests/test_ai.cpp::test_tick_path_random`
делал `ai->attach(things)` и **потом** перемещал `things` в возвращаемую
пару — `Ai` держал ссылку на перемещённый объект. ASan:
`stack-use-after-return` в `Ai::sync_vitals` (`things.cpp:917` через
`Ai::tick`). Теперь `Things` живёт в `unique_ptr` и не перемещается после
`attach` (у `Ai::attach(Things&&)` и так стоит `= delete`).
