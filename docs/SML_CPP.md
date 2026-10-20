# SML/SME в C++: читатель разметки UI

Документ описывает `engine/src/res/sml.h` / `sml.cpp` — C++-порт
документного слоя `gm/sml.py` (`decode_best`, `elements`, `describe`).
Формат, корпус и адреса движка — в [UI.md](UI.md); здесь только API,
golden-таблица, отличия от Python и остаток.

Модуль только читает: `.sml`/`.sme` никогда не переписываются, игра и
корпус не изменяются.

## 1. API

```cpp
#include "res/sml.h"

std::string error;
auto doc = gm::parse_sml(bytes, error);        // .sml или .sme, авто
if (!doc) { /* error: пустой ввод, нет <d>/</d>, дисбаланс, 512+ тегов */ }

const gm::SmlSummary s = gm::describe_sml(*doc);
// s.tags, s.images, s.fonts, s.music, s.tag_counts, s.attributes,
// s.doc_ok, s.brackets_balanced, s.plain, s.body, s.preview, ...

for (const gm::SmlElement& e : doc->elements) {
    // e.kind, e.tag, e.value, e.attrs, e.line, e.pos,
    // e.x, e.y, e.font, e.track, e.offset, e.align, e.text
}
```

| Функция / тип | Python-аналог | Назначение |
|---|---|---|
| `parse_sml(bytes, error)` | `decode_best` + `describe` | декодировать, проверить и разобрать документ |
| `parse_sml(bytes, SmlSource, error)` | — | явно `Plain` / `Encoded` / `Auto` |
| `sml_decode_best(bytes)` | `decode_best` | `(text, note)`, без валидации оболочки |
| `sml_decode_candidates(bytes)` | `decode_candidates` | `[(note, text)]`: `as-is`, `newline-repair`, `crlf-strip` |
| `sml_decode_text(bytes, repair)` | `decode_text` | latin-1 + опциональная починка `[\r\n]+` → пробел |
| `sml_scan_tags(text)` | `tags` | плоский поток тегов с атрибутами и `pos` |
| `sml_elements(text)` | `elements` | упорядоченные элементы, включая текстовые прогоны |
| `describe_sml(doc)` | `describe` | сводка без раскладки |
| `SmlElement` | `Element` | `kind`/`tag`/`value`/`attrs`/`line`/`pos` + типизированные поля |
| `SmlDocument` | результат `decode_best` | текст, `note`, теги, элементы, флаги, `plain`/`body` |
| `SmlSummary` | dict `describe` | счётчики, карты тегов/атрибутов, оболочка, превью |

Шифр не дублируется: `sml_decode`/`sml_encode` остаются в `res/text.h`,
`text.h`/`text.cpp` не изменялись. `parse_sml` вызывает их и добавляет
транспортные варианты и модель документа.

### 1.1. Автоопределение

`SmlSource::Auto` сначала пробует `decode_best` (как загрузчик, который
предпочитает `.sme`), а если ни один вариант не даёт корректный
`<d>...</d>`, проверяет исходные байты как plain-разметку. Признак plain —
`<d` в начале (у закодированного файла первая буква `d` сдвинута). Для
файлов с известным расширением надёжнее явный `SmlSource::Plain` или
`SmlSource::Encoded`.

### 1.2. Валидация

`parse_sml` возвращает `nullopt` и текст ошибки для:

* пустого ввода (`sml: empty input`);
* документа без тегов (`sml: no tags`);
* первого тега не `<d>` (`sml: first tag is not <d>`);
* последнего тега не `</d>` (`sml: last tag is not </d>`);
* дисбаланса `<`/`>` (`sml: bracket imbalance`) — включая обрезанный хвост;
* 512 и более тегов (`sml: tag limit exceeded`); лимит движка — 511
  (`0x49779c`: `count >= 0x200` — ошибка).

Неизвестные буквы тегов не фатальны: они попадают в `unknown_tags`
(`describe_sml`), как предупреждение Python-инструмента.

## 2. Golden

`engine/tests/test_sml.cpp` читает `/mnt/d/games/Gunmetal 3dfx/SML`
(переопределяется `GUNMETAL_DIR`) и сверяет все 300 файлов с таблицей,
сгенерированной `gm/sml.py`. На каждый файл закреплены: размер, вариант
декодирования, число символов/строк/тегов, количество image/font/music/
text-run элементов, неизвестные теги, флаги оболочки, FNV-1a64 декодированной
разметки, `plain` и `body`. Дополнительно проверяется
`encode(decode(bytes)) == bytes` на всех 151 `.sme`.

| Проверка | Значение |
|---|---:|
| файлов | 300 |
| `.sme` | 151 |
| `.sml` | 149 |
| пар (stem, регистр не важен) | 147 |
| побайтово идентичных пар | 131 |
| редакций | 14 |
| пар с LF→CRLF (`crlf-strip`) | 2 |
| `.sme` с `newline-repair` | 1 (`brief/level17.sme`) |
| `.sme` без `.sml` | 4 |
| `.sml` без `.sme` | 2 |
| тегов всего | 2632 |
| `<I>` | 72 (15 уникальных имён, все `x=64 y=64`) |
| выравнивание картинок | 50 `l`, 22 `r` |
| `<f>` | 268 (только `s=2`: 174, `s=3`: 94) |
| `<M>` | 56 (`B` ∈ {0,1,60}, `S` ∈ {0,40,50,70,80,100,170}) |
| разобрано без ошибок | 299 / 300 |

Единственный «плохой» файл — `prelevel/dm4.sml` без `</d>`; тест ожидает
именно `sml: last tag is not </d>`, а его `.sme`-брат разбирается (4 тега,
закрыт). Остальные 299 файлов дают точное совпадение всех golden-полей.

Именованные кейсы:

* `prelevel/level17.sme` — обычный `as-is`, 3 тега (не путать с
  `brief/level17.sme`);
* `prelevel/level20.sme` — `crlf-strip`, 2 тега;
* `brief/level17.sme` — `newline-repair`, 18 тегов, после починки нет `\r`;
* `prelevel/dm4.sml` — ошибка оболочки, `.sme` корректен;
* `help.sme` — 90 тегов, `Toggle Player ID`, `P - Pause`, без опечатки
  `PLayer`; `help.sml` — 63 тега и опечатка на месте.

Негативные кейсы теста: пустой ввод, отсутствие тегов/`<d>`/`</d>`,
дисбаланс `<`, обрезанный хвост, ровно 511 тегов (успех) и 512 (отказ),
неизвестный тег, атрибуты в одинарных кавычках (не разбираются — паритет с
регуляркой Python), испорченная буква закрывающего тега, вставка байта в
реальный `brief/LEVEL1.SME` (сдвиг позиционного ключа), синтетические
CRLF-транспорт и вставка одного CRLF.

## 3. Сборка и прогон

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja
cmake --build build/sandbox --target gm_sml_tests -j 32
ctest --test-dir build/sandbox -R sml --output-on-failure
python3 regress.py
```

Ожидаемый вывод теста:

```
sml: 300 files (151 .sme, 149 .sml), 147 pairs: identical 131, revisions 14,
line-endings 2; images 72 (15 unique), music 56; OK
```

Если игра не установлена, тест печатает `sml: game not installed, skipped`
и завершается успешно (как остальные корпусные тесты).

## 4. Отличия от Python

1. **Строгий разбор.** `parse_sml` — валидатор: без `<d>`/`</d>`,
   дисбаланса или лимита документа нет. Python `describe` разбирает что
   угодно и только выставляет флаги, а `decode_best` возвращает
   `"unrepaired"`. Для best-effort сценария в C++ есть `sml_decode_best` и
   `sml_decode_candidates` без проверки оболочки.
2. **Нет расширения файла.** Python вызывает `decode_best` только для
   `.sme`, а `.sml` читает как есть. C++ `Auto` определяет форму по
   содержимому; для точного паритета используйте `SmlSource::Plain` /
   `Encoded`.
3. **`value` — строка.** В Python `Element.value` бывает `int` (шрифт,
   музыка). В C++ числовые поля лежат в `font`/`track`/`offset`/`x`/`y`, а
   `value` остаётся текстовым (для `font`/`music` — десятичная запись).
4. **`attrs` — упорядоченный `vector`**, а не dict; поиск
   `sml_attribute()` регистронезависимый. Повторный ключ сохраняет первую
   позицию и последнее значение — как dict в Python.
5. **Пробельные классы — ASCII.** Python `str` для latin-1 дополнительно
   считает пробелами `\x1c..\x1f`, `\x85`, `\xa0`; в поставке таких байтов
   нет, на golden это не влияет.
6. **Нет JSON.** `SmlSummary` — типизированная структура; сериализация
   остаётся задачей вызывающего кода.
7. **`decode_auto` не портирован.** Он вытеснен `decode_best`
   (`docs/UI.md` §4.2): эвристика по буквам тегов не видит LEVEL20/LEVEL4.
8. **`plain()` отдельной функцией не экспортируется** (имя `sml_plain`
   занято в `text.h`); результат доступен как `doc.plain` /
   `summary.plain`, `body` — уже без тегов.

## 5. Доказано и гипотезы

Доказано (совпадение с Python на всех 300 файлах и golden-хэшами):

* позиционный шифр и его обратимость на 151 `.sme`;
* три варианта декодирования и выбор `as-is` / `newline-repair` /
  `crlf-strip`, включая приоритет по числу `\r\n`;
* оболочка `<d>...</d>`, лимит 511 тегов, дисбаланс скобок;
* грамматика тегов и атрибутов, подстановки `<k>`→`Soldier`, `<s>`→пробел,
  `plain`/`body` до байта;
* элементы: `doc/row/paragraph/column/image/font/music/placeholder/text/
  close/unknown`, `pos`/`line`, `x`/`y`/`align`, `B`/`S`;
* корпусные числа: 147 пар, 131 identical, 14 редакций, 2 line-endings,
  72 картинки (15 имён, 50 `l`/22 `r`), 268 шрифтов (2/3), 56 `<M>`.

Гипотезы / не добито (унаследовано от UI.md, C++ ничего не меняет):

* смысл атрибута `M S` (смещение/громкость/стиль) — в коде только запись;
* вклад `I x` при заданном `a` (в поставке всегда `x=64`);
* сопоставление номера шрифта и атласа `FONTS*`;
* алгоритм пагинации брифингов и точные экранные координаты SML-пагера;
* полная карта «экран → пункты меню» (меню не на SML).

## 6. Остаток

* Раскладка/метрики шрифтов и пагинация: нужен порт `0x47fa50`/`0x47faf0`
  и `0x498600/0x4986b0/0x498750`; текущий модуль layout-free, как Python.
* Проверка наличия `SML/<image>.bmp` и размеров 64×64 — в C++ тесте не
  дублируется (это делает `tools/re/ui_semantics.py --check`); при переносе
  HUD добавить загрузку loose-BMP.
* Рендер элементов (шрифт, цвет, origin) и HUD из `FRONTEND/BMP.IDX` —
  отдельная задача; см. `docs/UI.md` §10.
* `parse_sml` не кэширует и не мемоизирует; при массовой загрузке экранов
  документ стоит держать рядом с исходными байтами.
