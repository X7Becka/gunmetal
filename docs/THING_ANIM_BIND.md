# `thing_anim` — AI-состояние → клип → кадр (M6.3.1, срез 2)

**Статус:** срез 2 закрыт (2026-10-24). Модуль `engine/src/world/thing_anim.*`,
CTest `thing_anim` (label `quick`, 0.25 с), тест `engine/tests/test_thing_anim.cpp`.
Это **связка данных**, а не рантайм: `Things`/`Ai` её пока не вызывают (для
этого нужен тик `M9`).

```
Thing (model) + AiState + tick  ->  ThingClip -> Sobanim/<clip>.soo -> кадр
```

---

## 1. Что доказано

| факт | доказательство |
|---|---|
| Клип модели — это `Sobanim/<model>.soo`, и он берётся **только** если нет `SOBS/<model>.sob` | загрузчик `0x42b4e1`: сначала `.sob` (`0x427a05`), при неудаче `.soo` (`0x40d1a0`), `docs/SOO.md` §2. Все 40 поставочных `.soo` не имеют одноимённого `.sob` — проверено тестом (`SOBS/<stem>.sob` отсутствует, 40/40) |
| `ThingClip` = файл `.soo` целиком: список поз + длительности | `0x42b5c2`/`0x42b657`: `mode`, `count`, затем `count` пар «поза длительность»; позы — отдельные `.sob` |
| Кадр на тике: mode 0 удерживает последний кадр, mode 1 зацикливается, отрицательное время прижимается к 0 | `0x433e00`: `0x433e8b`…`0x433eb2` (clamp в `total-1`), `0x433eb4`…`0x433ebb` (`t %= total`), `0x433e84` (`t<0 -> 0`); `docs/SOO.md` §3 |
| Длительности — тики 91 Гц, не миллисекунды | `0x44db8d` (`cmp ecx,0x5b` = 91), `0x4074e0` (Sleep 11 мс), `.soa`-загрузчик `0x42badf` (`period*91/100`), `docs/SOO.md` §3.3 |
| Клип принадлежит **модели**, а не состоянию | id анимации вешается только на первую позу (`0x42b7bd`: `sob0+0x7c = -1 - n_soo`), а имя анимации — это имя модели |
| В поставке 40 клипов, движок читает 261 кадр, 19 987 тиков; mode 0 — 26 файлов, mode 1 — 14 | `docs/SOO.md` §4; воспроизведено тестом на всём корпусе (`mode0=26 mode1=14 frames=261`) |

Кадр в модуле считает `soo_pose_at_tick()` из `res/anim.h` — то есть арифметика
та же, что у проверенного плеера; дублирования таймлайна нет. Поэтому
`ThingAnimBinding::frame/tick` наследуют её поведение один в один:

* `tick` — **нормализованная** фаза (`SooPose::phase`): для mode 1 это
  `tick % total`, для mode 0 — `min(tick, total-1)`;
* `frame` — индекс в кадрах, которые читает движок (`min(count, frames)`,
  `0x42b5f1` + `0x42b637`), т.е. лишние строки `nblast`/`nbloom`/`ruinbeam`
  сюда не попадают;
* `looping` — `mode == 1` после применения модели «мёртвый не циклится» (§2).

## 2. Что модель (не доказано данными или кодом)

### 2.1 Соответствие состояния клипу

Оригинал не хранит анимацию в состоянии AI: у инстанса есть только «модель» и
клип, привязанный к модели. Поэтому отображение `AiState → ThingClip` —
**модель**:

| `AiState` | `ThingClip` | обоснование |
|---|---|---|
| `idle` | `idle` | покой |
| `patrol` | `walk` | скеффолд AI различает `patrol` как «идёт по маршруту», отдельного клипа ходьбы нет |
| `alert` | `walk` | в скеффолде `alert` — уже сближение с целью (`docs/AI_CPP.md`) |
| `attack` | `attack` | атака |
| `dead` | `death` | смерть |

`clip_for_state` возвращает `nullopt` для значения вне этих пяти
(`static_cast<AiState>(200)`) — `bind_thing_anim` такое отвергает, а не
подставляет клип.

### 2.2 Почему суффикса нет: данные

Модуль пробует вариант `<model><suffix>.soo` (`_idle`, `_walk`, `_atk`,
`_dead`), затем базовый `<model>.soo`. В поставке **нет ни одного** файла с
суффиксом: `variant_hits = 0` на 800 привязках (40 клипов × 4 состояния × 5
тиков). То есть все состояния всегда падают на базовый клип модели, а суффикс —
только расширяемая конвенция (если появятся данные, пробел закроется без
изменения API).

Имена, которые *выглядят* состоянием, — это отдельные **модели отдельных
типов**, а не варианты одной модели. Полный разбор поставки (36 уровней,
2896 типов, 330 различных имён `SOB_TABLE`):

* 28 имён `SOB_TABLE` — клипы (`BAMFRAYS`, `CLOUD`, `CULTIST`, `DISRUPT`,
  `FMBLOK`, `FMBLOKTO`, `ICEBAMF`, `KELMOR`, `KINGARM`, `KINGBODY`,
  `KINGMOUF`, `KINGYELL`, `KNDEATH`, `KNEEL`, `KNEELTWO`, `KNTBOX`,
  `NATAKAG`, `NATAKAS`, `NBLAST`, `NBLOOM`, `NUKERING`, `OCTAPOD`,
  `QUEENLAY`, `RUINBEAM`, `SHOCK`, `SLUMP`, `TELEPIN`, `VPARM`);
* 115 строк «уровень × тип» указывают на клип, различных пар «тип → модель»
  14: `CULTIST→CULTIST`, `CULTKNL→KNEEL`, `CULTKNL2→KNEELTWO`,
  `CultKC→SLUMP`, `CultnKC→KNDEATH`, `FMblock→FMBLOK`, `KELMORPA→KELMOR`,
  `KELMORPB→KELMOR`, `KING→KINGBODY`, `TelepIn→TELEPIN`, `telepin→TELEPIN`,
  `bamfrays→BAMFRAYS`, `icebamf→ICEBAMF`, `octapod→OCTAPOD`.

То есть `kndeath`/`kneel`/`slump` — это разные типы культиста (лежащий,
коленопреклонённый, сгорбленный), у каждого **свой** клип на все состояния.
Отсюда же следует, что в оригинале труп, скорее всего, просто другой
тип/модель (гипотеза: `CultnKC` = «cultist, kneel, corpse», `kndeath` — mode 0,
4×15, заканчивается позой трупа), а не «состояние смерти» того же объекта.
Гипотезу не закрепляем: доказан только формат/плеер, не решение геймдизайна.

### 2.3 Смерть удерживает кадр

`ThingAnimConfig::hold_on_death = true` (по умолчанию): для `ThingClip::death`
клип с `mode 1` проигрывается как mode 0, то есть встаёт на последнем кадре
(`Cultist.soo`: t=401 → кадр 3, `tick=399`, `looping=false`, вместо кадра 0).
Обоснование — «труп не должен циклиться»; в движке для этого есть только режим
0. `hold_on_death=false` возвращает поведение файла (`looping=true`, кадр 0).
Это единственное место, где модуль переопределяет `mode` из файла.

### 2.4 Прочее, чего нет в данных

* Отдельного «walk»/«attack» клипа нет ни у одной модели — значит, ходьба и
  стрельба в оригинале на анимацию модели не влияли (или влияли через скорости
  движения, но не через подмену клипа).
* `.soa` (`0x42b81b`, спрайт/флаг-анимация) не является клипом модели и здесь
  не привязывается; `.sob` — статическая геометрия без таймлайна.

## 3. API

```cpp
namespace gm::thinganim {

enum class ThingClip : std::uint8_t { none, idle, walk, attack, death };

const char*      clip_name(ThingClip);        // "none"/"idle"/...
std::string_view clip_suffix(ThingClip);      // "", "_idle", "_walk", "_atk", "_dead"
std::optional<ThingClip> clip_for_state(AiState);

struct ThingAnimBinding {
    ThingClip   clip;       // запрошенный клип состояния
    std::string clip_name;  // фактически загруженный клип (стем файла, напр. "Cultist")
    int         frame;      // индекс кадра, который читает движок
    int         tick;       // нормализованная фаза, тики 91 Гц
    bool        looping;
};

struct ThingAnimConfig {
    std::filesystem::path root;   // пусто -> $GUNMETAL_DIR, зеркало, поставка
    bool hold_on_death = true;
};

std::filesystem::path resolve_anim_root(const ThingAnimConfig& = {});
void clear_clip_cache();

std::optional<ThingAnimBinding> bind_thing_anim(const Thing&, AiState, int tick,
                                                std::string* error = nullptr);
std::optional<ThingAnimBinding> bind_thing_anim(const Thing&, AiState, int tick,
                                                const ThingAnimConfig&, std::string* error = nullptr);

struct ThingAnimStats { int bound, unbound, animated, still; };
ThingAnimStats collect(std::span<const Thing>, std::span<const AiState>, int tick,
                       std::string* first_error = nullptr);
ThingAnimStats collect(std::span<const Thing>, std::span<const AiState>, int tick,
                       const ThingAnimConfig&, std::string* first_error = nullptr);
}
```

Правила:

* имя клипа берётся из `Thing::model`; пустая модель откатывается на
  `Thing::type_name` (та же запись `THING_TYPES`, т.е. это имя, а не догадка);
* поиск регистронезависимый по ASCII (`Cultist.soo` и `DISRUPT.SOO` в одной
  папке), внутри `<root>/Sobanim/`, один скан каталога на root и один разбор
  на файл (кэш; `clear_clip_cache()` — сброс);
* `animated` — поза ещё движется: клип циклится, либо (mode 0) фаза не дошла до
  последнего тика; и в клипе больше одного кадра. `still` — привязан, но замер
  на последнем кадре. `bound + unbound == things.size()`,
  `animated + still == bound`;
* `collect` без состояния для вещи (`states.size() < things.size()`) считает её
  `unbound`, а не «idle по умолчанию».

Отказы (текст с префиксом `thing_anim:`, `*error` очищается при успехе):

| причина | пример текста |
|---|---|
| `tick < 0` | `thing_anim: negative tick -1 (the engine clamps to 0 at 0x433e84, the caller may not)` |
| состояние вне пяти | `thing_anim: unknown AI state 200` |
| нет модели и типа | `thing_anim: thing 0 has no model and no type name (type -1)` |
| имя модели — не один компонент пути | `thing_anim: model name '../loop' is not a plain Sobanim/ stem` |
| нет установки/`Sobanim` | `thing_anim: no installation with a Sobanim/ directory (...)` |
| нет клипа у модели | `thing_anim: no clip for model '20MMCLP' (probed '20MMCLP_idle.soo' and '20MMCLP.soo')` |
| клип битый | `thing_anim: clip 'empty': animation has no frames`, `... unsupported .soo mode 7`, `... duration is not positive` |

## 4. Числа (как воспроизвести)

```sh
cmake --build build/sandbox --target gm_thing_anim_tests -j 4
./build/sandbox/engine/tests/gm_thing_anim_tests
ctest --test-dir build/sandbox -R thing_anim --output-on-failure
```

Вывод на поставочных данных (`~/.cache/gunmetal-re/game`):

```
synthetic: loop/hold 20 ticks, probe order, 9 refusals, collect ok
timeline: kndeath(mode 0,4x15): t0 f0 t0, t60 f3 t59, t61 f3; kntbox(mode 1,10x20): t0 f0, t199 f9, t200 f0 t0, t201 t1, t2020 f1; dead t250 f9 held
level13: types=85 clips=5 [FMblock->FMBLOK TelepIn->TELEPIN CultKC->SLUMP bamfrays->BAMFRAYS CULTIST->CULTIST] things=267 bound=10 unbound=257 t0(animated=10 still=0) t2000(animated=2 still=8); CULTIST/Cultist.soo t210 frame=2, dead t401 frame=3 tick=399
corpus: files=40 bound=800 unbound=0 variant_hits=0 mode0=26 mode1=14 frames=261 no_model=40; static .sob models=672 bound_as_clip=0; dm1 sobs=94 clip_models=10 static_refusal=20MMCLP
thing_anim: all checks passed
```

Что именно запинено:

* **mode 0** — `kndeath.soo` (4 кадра × 15 = 60 тиков): t=0 → кадр 0/tick 0;
  t=59 → кадр 3/tick 59; t=60 → кадр 3/tick 59 (прижат к `total-1`);
  t=61 → кадр 3/tick 59 (стоит навсегда);
* **mode 1** — `kntbox.soo` (10 × 20 = 200): t=0 → кадр 0; t=199 → кадр 9;
  t=200 → кадр 0/tick 0 (переход последней позы в первую); t=201 → tick 1;
  t=2020 → кадр 1/tick 20;
* **смерть на том же клипе**: t=250 при `hold_on_death` → кадр 9/tick 199/
  `looping=false`; при `hold_on_death=false` → кадр 2/tick 50/`looping=true`;
* **реальный тип**: `level13.sbt` (85 типов, 122 модели `SOB_TABLE`), тип 68
  `CULTIST`, `+0x1a4` = 82 → `SOB_TABLE` «CULTIST» → `Sobanim/Cultist.soo`
  (mode 1, кадры 50/150/50/150 = 400 тиков): `idle` t=210 → кадр 2/tick 210;
  `alert` и `attack` → тот же клип (вариантов нет); `dead` t=401 → кадр 3/
  tick 399 (hold); `dead` t=0 → кадр 0;
* **корпус**: 40 файлов `Sobanim/*.soo`, 800 привязок, 0 отказов,
  0 попаданий в суффиксные варианты, 26 mode 0 / 14 mode 1, 261 кадр движка;
  у всех 40 нет одноимённого `.sob` (иначе загрузчик не дошёл бы до `.soo`);
* **модели без клипа**: 672 статических `SOBS/*.sob` — ни одна не привязалась
  как клип (пересечения имён `.sob`/`.soo` нет, 0/672); 40 запросов с пустым
  `Thing` дают 40 отказов «no model»;
* **`level13` в сборе**: 267 инстансов, 10 привязаны (5 типов с клипами:
  `FMblock→FMBLOK`, `TelepIn→TELEPIN`, `CultKC→SLUMP`, `bamfrays→BAMFRAYS`,
  `CULTIST→CULTIST`), 257 без клипа (статическая геометрия);
  на t=0 все 10 `animated`, на t=2000 — 2 `animated` (mode 1) и 8 `still`
  (mode 0 досмотрели);
* **статическая модель**: первый `SOB_TABLE`-модель `dm1.sbt` без клипа
  (`20MMCLP`) даёт `no clip`; в `dm1` 94 модели, из них 10 — клипы;
* **детерминизм**: повторный проход по 800 привязкам совпадает поле в поле;
  кэш не влияет на результат (`clear_clip_cache()` между проходами).

Разбор корпуса (независимо, Python-оракулом):

```sh
python3 - <<'PY'
import pathlib, os, sys
sys.path.insert(0, '.')
from gm import sbt
game = pathlib.Path(os.path.expanduser("~/.cache/gunmetal-re/game"))
clips = {p.stem.lower() for p in (game / "Sobanim").iterdir() if p.suffix.lower() == ".soo"}
sobs, pairs, types, levels, rows = set(), set(), 0, 0, 0
for p in sorted(game.glob("*.sbt")):
    lv = sbt.parse(p.read_bytes()); sob = tt = None
    for r in lv.records:
        if r.type == 20: sob = r.payload
        if r.type == 4:  tt = r.payload
    levels += 1; types += len(tt.entries)
    for e in sob.entries: sobs.add(e.name.upper())
    models = [e.name for e in sob.entries]
    for e in tt.entries:
        mi = int.from_bytes(e.tail[0xc:0x10], 'little')
        if 0 <= mi < len(models) and models[mi].lower() in clips:
            pairs.add((e.name, models[mi])); rows += 1
print(levels, types, len(sobs), len(pairs), rows)
PY
# -> 36 2896 330 14 115
```

## 5. Ограничения

* **AI-состояние приходит извне.** Модуль не знает, почему вещь в `attack`:
  `AiState` — параметр, а не поле `Thing`. `collect` берёт состояние по индексу
  из параллельного списка; связывать со `Ai::agents()`/`Things` — задача тика
  `M9` (как и `apply_aim`/`sync_vitals`, вызов будет явным).
* **Интерполяции и смешивания клипов нет.** Внутри клипа движок морфит вершины
  (`0x433f6d`), но перехода «один клип → другой» с плавным блендом в оригинале
  не найдено; здесь его тем более нет. Смена клипа — мгновенная смена
  `frame`/`clip_name`.
* **Только таймлайн, не геометрия.** Кадр — индекс; `.sob`-позы не грузятся,
  число вершин/граней и радиус не проверяются (это `SooPlayer`,
  `anim/soo_player.*`, `docs/SOO_PLAYER.md`). Поэтому все 40 клипов «связались»,
  хотя движок полностью загрузил бы 33 из 40 (`docs/SOO.md` §4: 4 из-за
  отсутствующих `.sob`, 3 из-за разной топологии) — для привязки это
  несущественно, для рендера существенно.
* **45-тиковый кламп уровня не применён.** `0x44d4c0` прибавляет
  `min(delta, 45)` к часам уровня; здесь тик передаёт вызывающий.
* **Один клип на вещь.** Модель — одна на инстанс (`THING_TYPES +0x1a4`),
  поэтому и клип один; «двухслойных» анимаций (тело + оружие) в формате нет.
* **Кэш не потокобезопасен** и не ограничен по размеру (максимум — 40 клипов
  поставки).
* К `ROADMAP.md`/`Things`/`Ai` не подключено сознательно: правки этих файлов
  вне зоны среза. Готовый вызов для `M9` —
  `collect(things.things(), states, level_clock)`.

## 8. CLI (`--things-anim`, срез 3)

`--things-anim [--level NAME] [--things-anim-tick N] [--things-anim-limit N]`
строит живой рантайм (`Things::load` + `Ai::attach`), берёт состояние каждого
thing у AI (нет агента → `idle`), печатает сводку `collect` и первые
`--things-anim-limit` привязок:

```
things_anim: level=level13 things=268 agents=140 bound=10 unbound=258 animated=2 still=8 tick=210
  first error: thing_anim: no clip for model 'BIGCHUNK' (probed 'BIGCHUNK_idle.soo' and 'BIGCHUNK.soo')
  id=119 type=CultKC model=SLUMP clip=slump frame=8 tick=210 looping=1
```

`level0` — `things=121 agents=6 bound=0 unbound=121`. Число things здесь 268,
а в модульном тесте 267: CLI не сужает `ThingsOptions::contexts` (тест
использует `{{0,0}}`), поэтому в набор попадает один лишний инстанс; форма
счётчиков та же. CTest `cli_things_anim`, `cli_things_anim_clip`.
