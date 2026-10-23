# tools/editor — правка данных Gunmetal

Три инструмента, которые читают поставочные файлы и пишут **новые** (игра
только читается; вывод по умолчанию — вне установки). Все проверки
воспроизводимы и входят в CTest.

| файл | что делает | доказательство |
|---|---|---|
| `sbtasm.py` | дизассемблер/ассемблер скриптов `.sbt`: текст ↔ байткод подскрипта, `IF`/`ELSE` с метками, строки/double/`EVAL`/`DEFINE_*`/`ADD_LIFT`/`DB`/`RAW` | round-trip **15 061/15 061 блока побайтово** (3 542 023 Б); `--self-test` 11/11; CTest `sbtasm_selftest`, `sbtasm_roundtrip` |
| `sbtwrite.py` | писатель контейнера `.sbt`: `parse → write` побайтово и замена одного подскрипта с пересчётом `SCRIPT.m`/смещений колонки 2 | **36/36 файлов identical**; `--verify-replace` (+1 байт сдвигает 257…364 смещения, прочие записи идентичны); `--self-test` 18/18; CTest `sbtwrite_*` |
| `sbwmesh.py` | вершины `.sbw`: `--list`, `--set-vertex`, `--translate`, пересчёт `radius`/`bounds` по правилу `0x43529f` | round-trip **36/36**; правило воспроизводит **28 192/28 192** объекта побитово; сдвиг вершины на +8 — 517 пикселей из 64 000 в рендере; self-test 81/81; CTest `sbwmesh_*` |
| `sbwobj.py` | объекты мира `.sbw`: `--list`, `origin=x,y,z`, `radius`, `bounds`, `basis=identity`, флаги `f74..f7c`; `--check`, `--audit` | round-trip **36/36** (28 192 объекта, 357 818 вершин); сдвиг `origin` на +64 двигает только объект (1 байт), AABB мира растёт на 64, рендер отличается на 750 пикселей из 64 000; self-test 82/82; CTest `sbwobj_*` |
| `lightmapedit.py` | лайтмапы `.sbc`: `--list/--export/--import/--fill/--scale/--check`, `--roundtrip` | round-trip **51/51** (67 200 записей, 24 214 080 Б); `--fill` записи 0 у `level0` меняет 128 байт, движок грузит те же `records/texels`, рендер отличается на 88 пикселей из 64 000; self-test 70/70; CTest `lightmapedit_*` |
| `sbtdbg.py` | отладка скриптов `.sbt`: `--run` (трасса с эффектами), `--break-offset/--break-op/--break-event`, `--watch player.cash|vm.ticks|…`, `--graph --dot/--mermaid` (CFG блока), `--list-blocks` | self-test 26/26; `level0` init — 24 шага, `0x6f92` — 84 узла/92 ребра; `ADD_SOURCEPLAYER_CASH` виден в watch; CTest `sbtdbg_*` |
| `sbwedit.py` | поля полигонов мира `.sbw`: `--list`/`--get`/`--set` (`texture/material/lightmap/brightness/overlay/flags`), `--check`, `--audit`, `--fields` (дисковые смещения ≠ движковым!) | round-trip **36/36** (235 069 полигонов, 40 037 031 Б); правка `texture` грузится и рендерится движком (75 пикселей из 64 000); self-test 59/59; CTest `sbwedit_*` |
| `sceneedit.py` | записи сцены в `.sbt` (всё, кроме `SCRIPT`): `--list`/`--get`/`--set`/`--replace-record`/`--check`, `--fields` с provenance | round-trip **36/36** (241 запись сцены), self-test 38/38; правка `THING_TYPES.speed` загружается движком; CTest `sceneedit_*` |
| `bigpack.py` | упаковка/замена записей BIGFILE (`*.IDX`+`*.DAT`): round-trip, `--list`, `--replace`, `--check`, `--add` | **5 архивов, 6061 запись, 151 737 971 Б — identical**; замена `57CHEV` (тот же размер/+1360/−7393) с проверкой остальных; `--self-test` 15/15; CTest `bigpack_*` |

## Типичный цикл

```sh
# 1) блок -> текст (точный вариант, без потерь %g)
python3 tools/editor/sbtasm.py disasm level0 --block 0x6f92 --exact --out block.txt
# 2) правка block.txt (например, литерал SET_CAMERA_HAZE)
# 3) текст -> байты в существующий контейнер
python3 tools/editor/sbtwrite.py --script level0 --block 0x6f92 \
    --asm block.txt --out /tmp/opencode/level0_mod.sbt
# 4) проверка контейнера
python3 tools/editor/sbtwrite.py --check /tmp/opencode/level0_mod.sbt

# ассеты: заменить запись в архиве
python3 tools/editor/bigpack.py --replace PICS/SBM.IDX --name 57CHEV \
    --file new.bin --out /tmp/opencode/pics
python3 tools/editor/bigpack.py --check /tmp/opencode/pics/SBM.IDX \
    --against PICS/SBM.IDX --name 57CHEV --expect new.bin
```

`--game` по умолчанию `/mnt/d/games/Gunmetal 3dfx` (или `GUNMETAL_DIR`).
Подробности и ограничения — `docs/SBT_ASM.md`, `docs/SBT_WRITE.md`,
`docs/BIGFILE_WRITE.md`, `docs/MODDING.md`.
