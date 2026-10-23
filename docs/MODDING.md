# MODDING — практическое руководство моддера

Статус: **черновик по факту на 2026-10-23** (пункты `M11.8` — моддер,
часть `M12.6` — документация). Всё, что ниже, **запущено** на этой машине;
вывод команд приведён дословно (worktree `~/build/gm-sub-moddocs`, база
`master` `905a922`). Где чего-то нет — так и написано.

Документ отвечает на три вопроса: что можно менять **сегодня**, как это
сделать по шагам на реальном уровне и где лежат спецификации форматов.

---

## 1. Что можно менять сейчас

| объект | инструмент | что делает | статус |
|---|---|---|---|
| скрипт уровня `.sbt`, запись `SCRIPT` (11), **один подскрипт** | `tools/editor/sbtasm.py` + `tools/editor/sbtwrite.py` | байткод ↔ текст, замена блока с пересчётом `m` и смещений | ✅ работает (проверено ниже) |
| весь `.sbt` (пересборка как есть) | `python3 gm.py sbt FILE --rebuild DIR` | `parse → to_bytes`, побайтово | ✅ 36/36 (`README.md`, «статус форматов») |
| текстуры/спрайты `.sbm` | `gm.py sprites` → PNG → `gm.py sbm` → `gm.py set` | PNG ↔ `.sbm`, замена записи в BIGFILE | ✅ (пример в §7) |
| запись в BIGFILE (`.idx`/`.dat`) | `gm.py extract` / `set` / `pack` | распаковка, замена одной записи, сборка архива | ✅ (пример в §7) |
| геометрия `.sbw`, лайтмапы `.sbc`, анимации `.soa`/`.soo`, `.soe`, `.sog`, `.sml`, `.sbv` | флаг `--rebuild DIR` у соответствующих команд `gm.py` | пересборка побайтово | ✅ по `README.md`; команды здесь не проверялись |
| сейвы | `gunmetal --save-out OUT --set K=V` | чтение, запись, правка полей | ✅ `docs/SAVE_WRITER.md` |

**Чего нет** (не ищите — не заработает):

* редактора уровней, сцены и геометрии — `M11.2`, `M11.3` в `ROADMAP.md`
  помечены ⬜;
* графа/отладчика/трассировки скриптов — `M11.4` ⬜ (есть только текст ↔
  байткод и замена блока);
* компилятора нового уровня «с нуля»: ассемблер собирает **подскрипт**,
  писатель вставляет его в **существующий** `.sbt`; новых записей
  (текстуры, объекты, handle-таблицы) никто не создаёт;
* записи сцены (вещи, двери, переключатели, лифты, таблицы `THING_TYPES`)
  — `tools/editor/sceneedit.py` (`docs/SCENE_EDIT.md`): `--list`/`--get`/
  `--set`/`--check`; код скриптов правится `sbtwrite.py`;
* BIGFILE — **есть**: `docs/BIGFILE_WRITE.md` и `tools/editor/bigpack.py`
  (`--roundtrip` всех 5 архивов побайтово, `--replace` с пересчётом
  `.IDX`/`.DAT`, `--check`); пункт `M11.6` закрыт. Быстрая замена одной
  записи по-прежнему делается `gm.py set` (§7);
* автоматической проверки мода в **оригинальной** игре — `M11.7` ⬜
  (проверено чтением нашим движком и структурно, не запуском `gundx.exe`).
  Проверка сегодня: структурная (`sbtwrite --check`) и чтением нашим
  движком (`gunmetal --dump`, §6).

Ассетов игры в репозитории нет и не будет (`M12.7`): для мода нужна
**собственная легальная установка** Gunmetal 3dfx v1.36.

---

## 2. Подготовка

* Python 3.11+ (проверено на 3.14), только стандартная библиотека —
  `README.md`, «Требования».
* Установка игры, например `/mnt/d/games/Gunmetal 3dfx` (в WSL) или
  `D:\games\Gunmetal 3dfx` (в Windows). Редактору нужны только `*.sbt`
  оттуда; движку — данные уровней (при их отсутствии он пишет
  `level '…': .sbw not found under …`, `docs/BUILD.md` §CLI).
* Инструменты редактора берут путь **только** из `--game`: переменных
  окружения они не читают (`grep environ tools/editor/*.py` — пусто), по
  умолчанию `/mnt/d/games/Gunmetal 3dfx` (см. `--help`).
* Игра — **только чтение**. Все результаты пишите в `/tmp/opencode`,
  `~/.cache` или `~/build`: в каталог установки ничего не пишется
  (`docs/SBT_WRITE.md` §7).

---

## 3. Инструменты и их CLI

Обе утилиты — с `--help`, `--self-test` (без файлов игры) и `--json`.

`python3 tools/editor/sbtasm.py --help`:

```
usage: sbtasm.py [-h] [--block BLOCK] [--asm FILE] [--out FILE] [--exact]
                 [--allow-lossy] [--roundtrip]
                 [--render {exact,reference,both}] [--self-test] [--game GAME]
                 [--limit LIMIT] [--verbose] [--json]
                 [path]
```

`python3 tools/editor/sbtwrite.py --help`:

```
usage: sbtwrite.py [-h] [--roundtrip] [--script LEVEL] [--block OFF]
                   [--asm FILE] [--text TEXT] [--out PATH] [--check FILE]
                   [--expect FILE] [--plan FILE] [--verify-replace]
                   [--self-test] [--game GAME] [--json] [--verbose]
```

Полные описания флагов — `docs/SBT_ASM.md` §6 и `docs/SBT_WRITE.md` §4.
Коды возврата `sbtwrite`: `0` успех, `1` проверка не прошла, `2` ошибка
входа/аргументов, `3` внутренняя ошибка (`docs/SBT_WRITE.md` §4).

Быстрая проверка, что инструменты живы (файлы игры не нужны):

```
$ python3 tools/editor/sbtasm.py --self-test
...
11/11 self-tests passed
$ python3 tools/editor/sbtwrite.py --self-test
...
18/18 self-tests passed
```

---

## 4. Язык скриптов

Текст — построчный: одна инструкция на строку, метки `.name:`, блоки
`IF cond { … } else { … }`, таблицы `DEFINE_*`, сырые байты `DB`/`RAW`,
конец подскрипта — `---` (0xFF). Полная грамматика, операнды по сигнатурам
(`D` = `double`, `Y` = байт, `S` = строка, остальные — `u32`) и правила
`IF`/`ELSE` — `docs/SBT_ASM.md` §2–§3.

Два диалекта текста:

| режим | что печатает | когда использовать |
|---|---|---|
| эталонный (`render`, по умолчанию в `disasm`) | `gm.sbtvm.render`, 6 значащих цифр у `double` | прочитать глазами |
| точный (`--exact`, `render_exact`) | `repr` у `double`, `head=` у `ADD_LIFT`, плоский `ELSE -> 0x…` | **любая правка**: только он round-trip'ится побайтово |

Эталонный текст теряет данные в 2 516 блоках из 15 061 (`docs/SBT_ASM.md`
§5.1: 2 457 блоков с `%g`, 42 с `ELSE` за границей родителя, 17 с
`ADD_LIFT`) — поэтому в примерах ниже всегда `--exact`.

---

## 5. Пошаговый пример: правка `SET_CAMERA_HAZE` в `level0`

Задача: на `level0` найти подскрипт с `SET_CAMERA_HAZE` и заменить первый
литерал `0.35` на `0.5`, ничего не сдвигая.

### 5.1 Найти блок

```
$ python3 tools/editor/sbtasm.py disasm level0 --block all --exact --out /tmp/opencode/mod/all
wrote 350 block(s) to /tmp/opencode/mod/all
$ grep -l SET_CAMERA_HAZE /tmp/opencode/mod/all/*.asm
/tmp/opencode/mod/all/00006f92.asm
/tmp/opencode/mod/all/00008df3.asm
/tmp/opencode/mod/all/0000cddf.asm
```

Имя файла — смещение блока в коде восемью hex-цифрами: `00008df3.asm` —
блок `0x8df3`. В `level0` таких блоков три; берём самый короткий (56 байт).

### 5.2 Выгрузить один блок и посмотреть текст

```
$ python3 tools/editor/sbtasm.py disasm level0 --block 0x8df3 --exact --out /tmp/opencode/mod/level0_haze.asm
wrote 1 block(s) to /tmp/opencode/mod/level0_haze.asm
$ cat /tmp/opencode/mod/level0_haze.asm
EVAL SOURCE_SWAP
IF SOURCE_IS_LOCAL_PLAYER_ONE {
    SET_CAMERA_HAZE                    0.35000000000000003 0.005
    ELSE -> 0x35
}
IF SOURCE_IS_LOCAL_PLAYER_TWO {
    SET_CAMERA_HAZE                    0.35000000000000003 0.005
}
EVAL SOURCE_SWAP
```

`SET_CAMERA_HAZE` — сигнатура `DD`, два `double` (17 байт:
`docs/FORMATS.md` §12.3, таблица команд `gm/sbtvm.py`). `ELSE -> 0x35` —
плоская форма: цель — смещение **внутри блока**, а не файла
(`docs/SBT_ASM.md` §3).

### 5.3 Поменять литерал

```
$ sed -i '0,/SET_CAMERA_HAZE                    0.35000000000000003 0.005/s//SET_CAMERA_HAZE                    0.5 0.005/' /tmp/opencode/mod/level0_haze.asm
$ diff <(python3 tools/editor/sbtasm.py disasm level0 --block 0x8df3 --exact) /tmp/opencode/mod/level0_haze.asm
3c3
<     SET_CAMERA_HAZE                    0.35000000000000003 0.005
---
>     SET_CAMERA_HAZE                    0.5 0.005
```

Заменён только первый из двух литералов; второй остался `0.35`.

### 5.4 Собрать и сверить байты

```
$ python3 tools/editor/sbtasm.py --asm /tmp/opencode/mod/level0_haze.asm -o /tmp/opencode/mod/level0_haze.bin
assembled /tmp/opencode/mod/level0_haze.asm -> /tmp/opencode/mod/level0_haze.bin (56 bytes)
```

Проверка, что изменились ровно байты литерала, а длина прежняя:

```
$ python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from gm import sbt as sbtmod
from tools.editor import sbtasm as A
sbt = sbtmod.parse_file("/mnt/d/games/Gunmetal 3dfx/level0.sbt")
sc = A.script_of(sbt); spans = dict(A.block_spans(sc.code, A.block_starts(sc)))
orig = sc.code[0x8df3:spans[0x8df3]]
new = open("/tmp/opencode/mod/level0_haze.bin","rb").read()
diff=[i for i in range(len(orig)) if orig[i]!=new[i]]
print("same length:", len(orig)==len(new), len(orig), "changed bytes:", len(diff),
      "first at", hex(0x8df3+diff[0]) if diff else None)
PY
same length: True 56 changed bytes: 7 first at 0x8dfc
```

### 5.5 Заменить блок в копии `.sbt`

```
$ python3 tools/editor/sbtwrite.py --script level0 --block 0x8df3 \
      --asm /tmp/opencode/mod/level0_haze.asm \
      --out /tmp/opencode/mod/level0_mod.sbt \
      --plan /tmp/opencode/mod/level0_mod.plan.json
wrote /tmp/opencode/mod/level0_mod.sbt (127338 bytes, sha256 9ffa3b8951ecdc87)
  block 0x8df3: 56 -> 56 bytes (+0), shifted 0 sub-script offset(s)
  other records byte-identical: yes; target block == assembled: yes; check: ok
  plan: /tmp/opencode/mod/level0_mod.plan.json
```

`--out` обязателен: исходный файл игры не перезаписывается никогда
(`docs/SBT_WRITE.md` §7). `--plan` — структурный дайджест, его же можно
сразу скормить `--check --expect`.

### 5.6 Проверить результат

```
$ python3 tools/editor/sbtwrite.py --check /tmp/opencode/mod/level0_mod.sbt \
      --expect /tmp/opencode/mod/level0_mod.plan.json
check: /tmp/opencode/mod/level0_mod.sbt ok (8 records, 350 sub-scripts, code 62497 bytes, 127338 bytes)
$ echo $?
0
```

`--check` без `--expect` тоже работает (внутренняя согласованность +
round-trip). Если блок стал длиннее/короче, писатель сам пересчитывает
`SCRIPT.m` и сдвигает смещения подскриптов в третьей колонке таблицы
сущностей: `--verify-replace` показывает `+1` байт → сдвиг 257…364
смещений (`docs/SBT_WRITE.md` §5.3).

---

## 6. Проверка мода движком

Вся цепочка (disasm → правка → asm → replace → загрузка движком) уже
упакована в один скрипт и в CTest:

```sh
python3 tools/qa/mod_regress.py --level level0 --block 0x6f92
# mod_regress: level0 block 0x6f92 -> /tmp/opencode/mod-regress
#   engine: records=8 scripts=350/62497 sbw=254439 B (unchanged)
#   edit visible in the written .sbt: 1/7 SET_CAMERA_HAZE at 0.5
#   result: ok
```

Он копирует `level0.sbt/.sbw` во временный каталог, меняет один литерал
`SET_CAMERA_HAZE` и требует, чтобы движок загрузил результат с той же
структурой (CTest `mod_regress`). Ручная проверка того же — ниже.

Наш движок читает тот же контейнер, поэтому структурную проверку можно
сделать без запуска игры:

```
$ ./build/sandbox/engine/gunmetal --dump /tmp/opencode/mod/level0_mod.sbt
file: /tmp/opencode/mod/level0_mod.sbt
size: 127338 bytes
head: 42 49 4E 41 52 59 20 53 41 56 45 00 00 ...
kind: SBT level
version: 0
records: 8
   12 SOUND_SAMPLE_TABLE      3901
   20 SOB_TABLE               1345
   15 POLY_HANDLE             1669
   17 SECTOR_HANDLE            213
    5 NAMED_ITEMS               57
    4 THING_TYPES            53357
   11 SCRIPT                 66711
  255 END_OF_FILE                1
poly handles: 104
object handles: 0
sector handles: 26
sobs: 67
script: things=0 handles=350 code=62497 flag=0
```

`kind: SBT level` и `code=62497` (длина не изменилась) — файл разобран
движком как обычный уровень. Запуск **оригинальной** игры с модом
автоматически не проверяется (`M11.7` ⬜): это ручной шаг, и его
результат — единственное настоящее доказательство, что мод играется.

---

## 7. Соседние форматы: текстуры и BIGFILE (проверено)

Текстуры/спрайты — цикл «распаковать → поправить PNG → собрать → заменить
запись». На `sprites/SBM.IDX` (179 записей):

```
$ python3 gm.py sprites "/mnt/d/games/Gunmetal 3dfx" /tmp/opencode/mod/sprites --transparent 0
sprites: 179 PNG written, 0 failed -> /tmp/opencode/mod/sprites
$ python3 gm.py sbm /tmp/opencode/mod/sprites/AFLM0001.png \
      /tmp/opencode/mod/AFLM0001.sbm --game "/mnt/d/games/Gunmetal 3dfx" --flags 257
sbm: 64x64, 0 colours, 1 mip level(s), 4108 bytes -> /tmp/opencode/mod/AFLM0001.sbm
$ cmp /tmp/opencode/mod/AFLM0001.sbm /tmp/opencode/mod/big/raw/AFLM0001.bin && echo byte-identical
byte-identical
```

Тот же файл, заменённый в архиве (`--flags 257` — те флаги, при которых
запись воспроизводится побайтово; для `PICS` значения другие, см.
`gm.py sbm --help`):

```
$ python3 gm.py set "/mnt/d/games/Gunmetal 3dfx/sprites/SBM.IDX" AFLM0001 \
      /tmp/opencode/mod/AFLM0001.sbm /tmp/opencode/mod/big/SBM.IDX
set: AFLM0001 (4108 bytes) -> /tmp/opencode/mod/big/SBM.IDX + /tmp/opencode/mod/big/SBM.DAT
```

Упаковка целого каталога — `gm.py pack OUT.IDX DIR [--ext .bin]`. Всё это
описано в `README.md` («Быстрый старт»), а правила контейнера и замена
одной записи с пересчётом — в `docs/BIGFILE_WRITE.md`
(`tools/editor/bigpack.py`).

Повторный запуск в тот же каталог упадёт с `gm.py: refusing to overwrite …
(use --force)`: `extract`/`set`/`pack`/`sbm` по умолчанию не перезаписывают
существующие файлы — добавляйте `--force` осознанно.

---

## 8. Лимиты, которые проверяет инструмент

| что | предел | кто проверяет |
|---|---|---|
| размер **всего кода** `SCRIPT.m` | `0x30000` = 196 608 байт (`MAX_BLOCK`) | `sbtwrite` перед записью |
| `things` в записи `SCRIPT` | `count <= 0xC8` | `sbtwrite` |
| число подскриптов | `n <= 0x400` | `sbtwrite` |
| таблицы `DEFINE_*` | только при `flag != 0`, `9*0x400` байт | формат (`gm/sbt.py`) |
| строки `DEFINE_LEVEL_OBJECTIVES` | **≤ 30** | `sbtasm` |
| точки `SOURCEPLAYER_PATH` | **≤ 32** | `sbtasm` |
| записей в таблице `DEFINE_*` | ≤ `0x1000` | `sbtasm` |
| остановок `ADD_LIFT` | ≤ `0x1000` | `sbtasm` |

Пределы контейнера — `docs/SBT_WRITE.md` §3; пределы структур — проверки в
`tools/editor/sbtasm.py`. Лимиты целей и точек видны на ошибке (код 1,
номер строки); файлы-примеры собраны так:

```
$ python3 - <<'PY' > /tmp/opencode/mod/limits.asm
print("DEFINE_LEVEL_OBJECTIVES " + " ".join('"obj%d"' % i for i in range(31)))
print("---")
PY
$ python3 - <<'PY' > /tmp/opencode/mod/path.asm
print("SOURCEPLAYER_PATH n=33 " + " ".join(str(i) for i in range(33)))
print("---")
PY
$ python3 tools/editor/sbtasm.py --asm /tmp/opencode/mod/limits.asm -o /tmp/x.bin
/tmp/opencode/mod/limits.asm: line 1: DEFINE_LEVEL_OBJECTIVES accepts at most 30 strings
$ python3 tools/editor/sbtasm.py --asm /tmp/opencode/mod/path.asm -o /tmp/x.bin
/tmp/opencode/mod/path.asm: line 1: SOURCEPLAYER_PATH accepts at most 32 points
```

Запас по `MAX_BLOCK` в поставке небольшой: самый большой код — 134 686
байт (`0x20e1e`), то есть ~62 КБ до отказа (`docs/SBT_WRITE.md` §5.5).

Счётчики `things`/`n` при замене кода **не пересчитываются** — замена блока
не добавляет сущностей (`docs/SBT_WRITE.md` §7).

---

## 9. Что нельзя и где легко ошибиться

* **Правка перед целью `ELSE -> 0x…`.** Цель в плоской форме — литерал;
  если вставить байты *перед* ней, цель нужно поправить в тексте вручную.
  Вставка в конец блока (перед `---`) безопасна: ни один `ELSE` в поставке
  не выходит за конец своего блока (`docs/SBT_WRITE.md` §7).
* **Неизвестный опкод.** `gm.sbtvm.decode` бросает `ValueError`, такой блок
  нельзя отрендерить; после правки его можно собрать через `DB`/`RAW`
  (`docs/SBT_ASM.md` §8).
* **Семантика не проверяется.** Гарантируется структура контейнера и
  байткода, а не поведение: вставленная команда может сломать уровень —
  это и есть предмет мода (`docs/SBT_WRITE.md` §7).
* **Записи типов 10/13/14** парсер оракула отвергает, длину нагрузки
  определить нельзя — писатель отказывается, а не угадывает. В поставке
  таких записей нет (`docs/SBT_WRITE.md` §7).
* **Эталонный рендер нельзя собирать.** `--roundtrip --render reference`
  всегда возвращает 1: текст с `%g` не восстанавливается побайтово
  (`docs/SBT_ASM.md` §8).

---

## 10. Форматы данных: где читать

| что | документ |
|---|---|
| контейнер `.sbt`: поток записей, `SCRIPT` (11), handle-таблицы | `docs/FORMATS.md` §12 |
| скриптовая ВМ: раскладка `SCRIPT`, команды, условия | `docs/FORMATS.md` §12.3 |
| таблица сущностей, колонка 2 = смещения подскриптов, сцена | `docs/FORMATS.md` §12.4 |
| все типы записей `.sbt` с адресами обработчиков и пределами | `docs/SBT_RECORDS.md` |
| текст ↔ байткод: грамматика, потери `render`, лимиты | `docs/SBT_ASM.md` |
| писатель контейнера: что пересчитывается, `--check` | `docs/SBT_WRITE.md` |
| исполнение байткода (что команда делает в игре) | `docs/VM_EXEC.md` |
| писатель BIGFILE | `docs/BIGFILE_WRITE.md` (`tools/editor/bigpack.py`); код оракула — `gm/bigfile.py` |

---

## 11. Воспроизведение целиком

```sh
cd /home/nooverload/build/gm-sub-moddocs      # или /home/nooverload/gunmetal-re
mkdir -p /tmp/opencode/mod

python3 tools/editor/sbtasm.py --self-test                    # 11/11
python3 tools/editor/sbtwrite.py --self-test                  # 18/18
python3 tools/editor/sbtasm.py --roundtrip                    # 15061/15061
python3 tools/editor/sbtwrite.py --roundtrip                  # 36/36

python3 tools/editor/sbtasm.py disasm level0 --block all --exact --out /tmp/opencode/mod/all
python3 tools/editor/sbtasm.py disasm level0 --block 0x8df3 --exact --out /tmp/opencode/mod/level0_haze.asm
# …правка литерала (§5.3)…
python3 tools/editor/sbtasm.py --asm /tmp/opencode/mod/level0_haze.asm -o /tmp/opencode/mod/level0_haze.bin
python3 tools/editor/sbtwrite.py --script level0 --block 0x8df3 \
      --asm /tmp/opencode/mod/level0_haze.asm --out /tmp/opencode/mod/level0_mod.sbt \
      --plan /tmp/opencode/mod/level0_mod.plan.json
python3 tools/editor/sbtwrite.py --check /tmp/opencode/mod/level0_mod.sbt \
      --expect /tmp/opencode/mod/level0_mod.plan.json
./build/sandbox/engine/gunmetal --dump /tmp/opencode/mod/level0_mod.sbt
```

Замеры на этой машине (36 уровней, `/mnt/d`): `sbtasm --roundtrip` — 2,67 с,
`sbtwrite --roundtrip` — 0,29 с, `--verify-replace` — доли секунды
(`docs/SBT_ASM.md` §9, `docs/SBT_WRITE.md` §8).

Файлы игры — только на чтение.
