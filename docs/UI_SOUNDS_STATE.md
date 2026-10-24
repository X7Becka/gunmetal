# Звуки интерфейса в C++: состояние (`M8.6`, срез 2)

Срез добавляет связь «событие меню → UI-сэмпл».  Источник — `docs/UI_SOUNDS.md`
(RE по `gundx.exe` и поставочным данным), а не декомпилятор: реализация
повторяет **поведение** таблиц, псевдо-C в движок не переносился.

Файлы: `engine/src/ui/ui_sounds.{h,cpp}`, `engine/tests/test_ui_sounds.cpp`,
CTest `ui_sounds` (label `quick`).  Звук нигде не проигрывается: наружу отдаётся
событие с именем сэмпла и индексом записи; загрузка/микшер — дело
`SoundPolicy`/`Mixer`.

## 1. Что доказано (код и данные)

| факт | доказательство |
|---|---|
| загрузчик семи имён `0x48f770`, хендлы `0x4bc44c..0x4bc464` | `docs/UI_SOUNDS.md` §1.1, `tools/re/ui_sounds.py --self-test` |
| `UPDOWN` = запись 581, `SELECT` = 510, `SELECTM` = 511, `MENUON` = 425, `VLEFT` = 804, `VBACK` = 636 в `sounds/SOU.IDX` (0-based) | §1.2; тест сверяет порядок записей через `BigFile` (`skip=12+12*i`), имя и непустой срез `SOU.DAT` |
| имена в `SOUND_SAMPLE_TABLE` уровня: `updown` (слот 0), `select` (1), `vleft` (2), `vback` (4), `menuon` (5), `selectm` (6), `count > 0` | level0; тест берёт `--level` (по умолчанию `level0`); на всех 36 поставочных уровнях те же имена с `count > 0` (проверка ниже) |
| event 1 → `updown` (`0x48e60c`), event 2 → `select` (`0x48e639`), гейт `0x4aa384` | §2.1 |
| Up/Down → `updown` только при реальном перемещении; упор в предел — event 0 (`0x48ce70`) | §2.3 |
| Enter на экранах 1..37, кроме 16 → `select` сразу и `selectm` на следующем кадре (колбэк 27-мс анимации `0x486700`, четыре сайта `0x48ced5`, `0x48f3f9`, `0x48f65a`, `0x48f692`) | §2.3, §2.4 |
| экран 16 `KEY JAMMING` — клавиша как данные: ни Enter (`0x48cf08`), ни Up/Down (`0x48b9fa`, `0x48bb9f`) звука не дают | §2.3 |
| Left/Right по умолчанию → `select` (event 2 ставится до switch по пункту, `0x48bd79`); исключения: `Switch Left Right` (экран 11, пункт 4) — **только** `vleft`, pan = 0 (`0x48c6c0`); громкость музыки/эффектов (пункты 0/2) — `updown` при изменении значения, на пределе тишина (`0x48be88/0x48be93`, `0x48c717/0x48c722`); `Border` экрана 12 — `updown` (`0x48c7a8`), `Gamma/Reflections/Light Maps` — `select` | §2.3, §2.6 |
| Esc — тишина на всех экранах (`0x48b4e1`; в `0x48e4ca..0x48e5ad` записи event нет) | §2.3 |
| `vfront`/`vback` загружаются и никогда не читаются (полные наборы dword-ссылок) | §1.1, §5 |

Сверка «все 36 уровней» (не входит в CTest, выполнена на зеркале игры):

```sh
python3 - <<'PY'
import sys, glob, os; sys.path.insert(0,'.')
from gm import sbt
names=["updown","select","selectm","menuon","vleft","vback"]
bad=[]
for p in sorted(glob.glob('/home/nooverload/.cache/gunmetal-re/game/*.sbt')):
    st=sbt.parse_file(p).records_of(12)[0].payload
    miss=[n for n in names
          if not [i for i,x in enumerate(st.names) if x.lower()==n and st.counts[i]>0]]
    if miss: bad.append((os.path.basename(p), miss))
print(len(bad), bad)
PY
# 0 []
```

## 2. Что модель, а не факт

* **`menu_on` на Enter со сменой экрана.** В оригинале `menuon` — колбэк
  появления арта титула (`0x486720`, §2.5), ни одна ветка ввода не пишет его
  запрос. Событие добавлено, чтобы оболочка была слышна целиком; тест помечает
  его как модель (MAIN item 0/3 → `[select, selectm, menuon]`).
* **`back` (`vback`, 636) не выдаётся никогда** — по доку его хендл не читает
  никто. Значение оставлено в enum/таблице для полноты §1.1 и проверки
  «в плане его нет».
* **`selectm` на «переключателе» не отличается от обычного Enter.** Док
  доказывает `selectm` на **каждом** Enter (все четыре сайта — колбэк
  анимации), отдельного признака «пункт-переключатель» в §2 нет. Поэтому
  тест проверяет одинаковый план на обычном пункте и на toggle-строке
  (`PLAY OPTIONS` пункт 0), а не «SELECTM только на переключателе».
* **Ввод `moved` снаружи.** Автомат `FrontendMenu` не владеет значениями
  (громкость, border/gamma), поэтому `ui_sounds_for(menu, key)` выводит
  `moved` из `feed()` на копии: изменение выделения/экрана. Хост, который знает
  про изменение значения, передаёт третий аргумент
  `std::optional<bool> moved`; без него Left/Right на строке громкости —
  тишина.
* **Left/Right по умолчанию — `select`, а не `vleft`.** В задании среза было
  «Left/Right → VLEFT», но §2.3 доказывает `vleft` только для пункта
  `Switch Left Right`; на остальных экранах горизонтальная клавиша даёт
  event 2 = `select`. Реализовано по доку (см. `ui_sound_for` док-комментарий).

## 3. API

```cpp
enum class UiSound { up_down, select, select_m, menu_on, v_left, back };

struct UiSoundEvent { UiSound sound; std::string_view name; int table_index; };

std::span<const UiSoundSample> ui_sound_samples();      // 6 строк §1.1/§1.2
const UiSoundSample* ui_sound_sample(UiSound);          // nullptr вне enum

std::optional<UiSoundEvent> ui_sound_for(const KeyEvent&, Screen from,
                                         Screen to, bool moved);

struct UiSoundPlan { std::vector<UiSoundEvent> events; bool empty() const; };
UiSoundPlan ui_sounds_for(const FrontendMenu&, const KeyEvent&,
                          std::optional<bool> moved = std::nullopt);

int  table_slot(const SoundSampleTable&, std::string_view name); // слот или -1
bool table_has(const SoundSampleTable&, int index);              // запись §1.2
```

`ui_sounds_for` **не меняет** переданное меню: `feed` выполняется на копии,
оттуда берутся `from`/`to`/«переместилось ли», а id выделенного пункта — из
`menu.selected_id()` (нужен для `Switch Left Right`, строк громкости и
`Border`).  `table_has(index)` склеивает документированный **индекс записи
архива** с реальной таблицей уровня по имени: слоты таблицы 0..6, записи
архива 425..804 — это разные пространства, поэтому проверка идёт по имени, а
сам индекс записи тест подтверждает через `BigFile`.

## 4. Числа теста

```sh
cmake --build ~/build/gm-sub-uisounds-build --target gm_ui_sounds_tests -j 4
~/build/gm-sub-uisounds-build/engine/tests/gm_ui_sounds_tests
# ui_sounds: 306 plan(s) over 34 screens, one key at a time
# ui_sounds: level level0, 1230 checks, 6 sample rows
#   updown   idx 581  slot  0  count 1  archive UPDOWN    rec 3153 B
#   select   idx 510  slot  1  count 1  archive SELECT    rec 3153 B
#   selectm  idx 511  slot  6  count 1  archive SELECTM   rec 3321 B
#   menuon   idx 425  slot  5  count 1  archive MENUON    rec 14267 B
#   vleft    idx 804  slot  2  count 3  archive VLEFT     rec 2321 B
#   vback    idx 636  slot  4  count 3  archive VBACK     rec 2100 B
# ui_sounds: enter plan [select selectm menuon], down plan [updown]
# ui_sounds: ok

ctest --test-dir ~/build/gm-sub-uisounds-build -R '^ui_sounds$'
# 1/1 Test #88: ui_sounds ... Passed  0.07 sec
```

`rec` — весь срез записи `SOU.IDX` (16-байтовый заголовок + payload, который
видит загрузчик `0x40af30`); полезная нагрузка и FNV — в §1.2 `UI_SOUNDS.md`.

Проверки покрывают: таблицу сэмплов и границы enum; реальную таблицу уровня и
порядок архива; Up/Down с `moved` и без; Enter (обычный, toggle, смена экрана);
Left/Right (по умолчанию, `vleft`, громкость, `Border`); Esc на всех 34
экранах; неизвестные клавиши; 306 планов «экран × клавиша» без `vback` и с
непустыми именами; пустую/нулевую таблицу, неизвестные и отрицательные
индексы; состояния `None`/`Exit`; отсутствие установки (тест печатает, что
проверена только модель, и не падает).

## 5. Ограничения

* Звук **не играется**: событие — это имя + индекс; громкость/панорама/
  приоритет/rate (`vol 0x30`, `pan 0x80`/`0x00` у `vleft`, `prio 0x3e8`,
  rate `0x2b11`) и микшер не подключены.
* Нет приоритетов, очередей, дедупликации и тайминга «`select` сейчас,
  `selectm` через кадр» — события идут в порядке плана, задержку обеспечивает
  вызывающий слой (если вообще будет).
* `menuon` — модель (см. §2); Esc и экран 16 — тишина по доку, а не «звук
  выхода».
* Мышиная ветка меню (синтезирует ли клик код `0xd`) и `vfront`/`vback` не
  разобраны в `UI_SOUNDS.md` §5 — здесь тоже не моделируются.
* Тест читает установку (`/mnt/d/games/Gunmetal 3dfx`, переопределяется
  `GUNMETAL_DIR`, `--game`, уровень — `--level`); без неё остаются 1178 из
  1230 проверок (модель), реальные строки пропускаются.
