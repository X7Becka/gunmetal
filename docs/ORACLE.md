# Эталонный оракул (M4.4.2)

«Оракул» — запущенная оригинальная игра, с которой снимаются эталонные кадры
для сравнения с новым рендером.

## Что уже есть в установке

Проверено в `D:\games\Gunmetal 3dfx`:

| компонент | файлы | статус |
|---|---|---|
| игра | `gundx.exe` (v1.36), `gun.exe`, `gunmetal.cfg`, `gunmetal.hi` | есть |
| dgVoodoo | `dgVoodooCpl.exe`, `dgVoodoo.conf` (Version `0x273` = 2.7.3), `DDraw.dll`, `D3DImm.dll` | есть |
| DXWnd | каталог `DXWnd\` (`dxwnd.exe`, `dxwnd.ini`, `tweaks.ini`, …) | есть |
| сохранения | `SAVEGAME\saved0.sbt/.sbw`, `saved1.sbt/.sbw` | есть |

То есть **оракул доступен**: игра запускается на стороне Windows через DXWnd
(окно 640×400), dgVoodoo подменяет DirectDraw/D3DImm.

## Как снять кадр

1. На Windows запустить игру через профиль DXWnd (windowed, без рамки,
   640×400 или 320×200 с целочисленным масштабом).
2. Загрузить нужное сохранение (`SAVEGAME\saved0.sbt`) и встать в
   фиксированную точку (например, старт `level0`, 3 секунды без ввода).
3. В PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File capture.ps1 -ProcessName gundx -Out level0.png
```

Скрипт `tools/win/capture.ps1` находит окно процесса, поднимает его,
захватывает прямоугольник окна и пишет PNG. Он **не работает из WSL** (нет
доступа к рабочему столу Windows); скрипт нужно скопировать на Windows-сторону,
например:

```sh
cp tools/win/capture.ps1 /mnt/c/Users/nooverload/capture.ps1
powershell.exe -ExecutionPolicy Bypass -File 'C:\Users\nooverload\capture.ps1' \
    -ProcessName gundx -Out 'C:\Users\nooverload\level0.png'
```

## Что фиксировать вместе с кадром

* `level` (имя `.sbt`), координаты/угол камеры (из сохранения или скрипта);
* хэш пары сохранений (`sha256sum saved0.sbt saved0.sbw`);
* разрешение и режим вывода (dgVoodoo: `OutputAPI`, `ImageScaleFactor`);
* время/кадр от старта уровня.

Без этого кадр нельзя сравнивать: HUD, оружие и анимации зависят от состояния.

## Ограничения

* dgVoodoo — это Glide/D3D-обёртка: цвета и фильтрация могут отличаться от
  оригинального 3dfx-рендера. Для сравнения «наш рендер ↔ оригинал» нужно
  либо отключить пост-обработку dgVoodoo, либо сравнивать структурно
  (геометрия, HUD-элементы), а не побитово.
* Полноэкранный захват зависит от DPI и темы Windows; скрипт берёт **прямоугольник
  окна**, поэтому масштаб 100 % обязателен.
* Запуск игры из WSL (`cmd.exe /c start …`) технически возможен, но забирает
  экран пользователя — для CI и регрессии это не автоматизируется; кадры
  снимаются вручную/по запросу и складываются в `data/oracle/` (без ассетов
  игры — только PNG-кадры, если пользователь разрешит их хранить).

## Сравнение кадра с нашим рендером

Инструмент: `tools/re/oracle_compare.py` (stdlib + `gm/png.py`). Побитовое
сравнение с dgVoodoo некорректно, поэтому считаются структурные метрики на
сетке 320×200 (кадр DXWnd в `DXWnd/dxwnd.ini` — 320×200, как и наш рендер):

| метрика | смысл | хорошо |
|---|---|---|
| `corr` | корреляция яркости (Пирсон) | ≥ 0,90 в полосе world |
| `mad` | средняя абсолютная разница, 0..1 | чем меньше, тем лучше |
| `edges` | Jaccard пересечения Sobel-краёв | ≥ 0,50 |
| `hist` | L1/2 гистограммы RGB 4×4×4 | чем меньше, тем лучше |

Метрики считаются отдельно для всего кадра, `world` (верхние 70 %) и `hud`
(нижние 30 %): HUD и оружие зависят от состояния и не должны маскировать
расхождение геометрии.

```sh
# 1. снять кадр (Windows, игра запущена через DXWnd)
powershell -ExecutionPolicy Bypass -File capture.ps1 -ProcessName gundx -Out level0.png

# 2. отрендерить тот же уровень/позу нашим движком (пример для level0)
./build/release/engine/gunmetal --level level0 --render out/level0.png --width 320 --height 200

# 3. сравнить (crop — если в кадр попала рамка окна)
python3 tools/re/oracle_compare.py --oracle level0.png --ours out/level0.png
python3 tools/re/oracle_compare.py --oracle level0.png --ours out/level0.png \
    --crop 0,0,320,200 --json
```

Проверка самого инструмента: `python3 tools/re/oracle_compare.py --selftest`
(6/6; одинаковые кадры дают `corr=1, mad=0, edges=1`).

`heuristic: match` — это **не доказательство**: сначала сверь состояние
(уровень, поза/угол, хэш сейва, разрешение), иначе совпадение может быть
случайным. Результат и метрики складывай рядом с кадром в `data/oracle/`
(см. `data/oracle/README.md`).
