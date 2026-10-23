#!/usr/bin/env python3
"""Предметы, маркеры регенерации, вирус и щиты (M6.2.6).

Модель снята с ``gundx.exe`` (только чтение): таблица предметов, её
состояния, кто и когда постит триггер предмета, как работает респавн
(маркер ``DEFINE_REGENERATION_MARKER``), а также ``DEFINE_VIRUS`` и
``DEFINE_SHIELDS``.

Таблица (создатель ``0x4686a0``, апдейтер ``0x4690f0``)::

    база 0x806360, 300 слотов по 0x60 (0x7080 байт, конец 0x80d418)
    state      +0x38   (0x806398 для слота 0)   -2 = свободен
    объект     +0x30   runtime-объект 3D (0 = объекта нет)
    kind       +0x34   1 = ADD_ITEM[_AT_SOURCE], 2 = *_FIXED
    радиус     +0x4c   i32, единицы мира, 0 = не подбирается
    триггер    +0x44   id события, которое постится при подборе
    handle     (0x10c + slot) << 16   (0x457bc8: диапазон 0x10c..0x237)

Состояния (``state``) и что с ними делает код::

    -2  свободный слот           0x4686d7, 0x469050, 0x469099
    -1  «фантом»: объекта нет,   0x4687e8..0x468805 (не 0/-3/-4 -> отказ),
        но радиус-скан его       слот занят, в сейв не пишется (0x466340)
        всё равно триггерит
     0  живой предмет            0x4687e8 -> 0x46882b (объект), 0x46929c
    -3  «взведён» (мина):       0x4687ef -> 0x46882b (объект), 0x4692be
        триггер по контакту
    -4  «взведён + цепочка»:    0x4687ef? нет: 0x4687ea -> 0x46882b,
        триггер по контакту,    0x4695d4 (переход -3 -> -4),
        взрыву (op75) и цеп-    цапочному 0x469670/0x4698b0
        реакции соседей
    >0  таймер появления:       0x4694dc (state -= dt), 0x468840 (маркер),
        предмет материализуется 0x46950c (объект), +0x48 (0x46952b)

Триггер предмета (``+0x44``) постит не сам апдейтер, а радиус-скан
``0x469770``: его вызывает физика игрока ``0x445165`` (внутри ``0x443690``,
каждый тик) с флагами 3 и код вещей ``0x471e03``. Скан берёт состояния
``0``/``-1`` (бит 0) и ``-4`` (бит 1) и сравнивает расстояние с радиусом
``+0x4c`` самого предмета. Награду выдаёт скрипт-сущность триггера
(``ADD_SOURCEPLAYER_CASH``/``_SALVAGE``/``HEAL_SOURCEPLAYER``/...), он же
обычно делает ``REMOVE_SOURCE_ITEM`` (op 39 -> ``0x469020``).

Использование::

    python3 tools/re/pickup.py --fields        # карта записи + состояния
    python3 tools/re/pickup.py --summary       # корпус 36 уровней
    python3 tools/re/pickup.py --json          # то же машинно
    python3 tools/re/pickup.py --file level0.sbt --items
    python3 tools/re/pickup.py --anchors       # байтовые якоря в gundx.exe
    python3 tools/re/pickup.py --self-test

Только стандартная библиотека; разбор ``.sbt`` — из репозитория
(``gm/sbt.py``, ``gm/sbtvm.py``, ``gm/scene.py``), блок ``ITEM`` —
``tools/re/records.py``.
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# --- таблица предметов ------------------------------------------------------
ITEM_BASE = 0x806360
ITEM_STATE = 0x806398          # == ITEM_BASE + 0x38
ITEM_SLOTS = 300               # 0x4686e7: cmp edi,0x12c
ITEM_SIZE = 0x60
ITEM_BLOCK = ITEM_SLOTS * ITEM_SIZE            # 0x7080
TABLE_END = ITEM_BASE + ITEM_BLOCK             # 0x80d3e0
SCAN_END = ITEM_STATE + ITEM_BLOCK             # 0x80d418 — предел цикла
HANDLE_FIRST = 0x10C           # 0x457bc8: cmp eax,0x10c
HANDLE_LAST = 0x237            # 0x10c + 299
HANDLE_END = 0x238             # 0x4694a3: cmp eax,0x2380000

# --- состояния --------------------------------------------------------------
ST_FREE = -2
ST_PHANTOM = -1
ST_LIVE = 0
ST_ARMED = -3
ST_CHAIN = -4

#: какие состояния создают 3D-объект (0x4687e8/0x4687ed/0x4687f2 -> 0x46882b)
CREATE_STATES = (ST_LIVE, ST_ARMED, ST_CHAIN)

#: радиус-скан 0x469770: бит 0 (флаги & 1) берёт 0/-1, бит 1 (флаги & 2) — -4
SCAN_BIT0 = (ST_LIVE, ST_PHANTOM)
SCAN_BIT1 = (ST_CHAIN,)

#: диапазоны состояний, которые апдейтер 0x4690f0 разбирает по-разному
STATE_REJECTED = "отказ: объекта нет (0x468819)"

# --- игрок ------------------------------------------------------------------
PLAYER_BASE = 0x819040         # docs/PLAYER_FIELDS.md
PLAYER_STRIDE = 0x3510
PLAYER_COUNT = 8
SHIELD_PLATE = 0x3250          # 0x81c290 - 0x819040: 4 плиты по 4 байта
SHIELD_PLATES = 4
PLAYER_FLAGS = 0x3200          # 0x81c240: 0x40/0x80 — вирус
PLATE_ADDRS = [PLAYER_BASE + SHIELD_PLATE + 4 * i
               for i in range(SHIELD_PLATES)]

# --- глобалы DEFINE_* -------------------------------------------------------
V_VIRUS_DMG = 0x80D8B4         # L0: урон щиту за тик
V_VIRUS_TEX = 0x80D880         # T: текстура (0x453923)
V_VIRUS_SND = 0x80D884         # N: сэмпл
V_VIRUS_VOL = 0x80D890         # V: громкость
V_VIRUS_PRI = 0x80D8B8         # P: приоритет
V_VIRUS_ATT = 0x80F9C0         # D: затухание (double)
V_REGEN_SOB = 0x80D878         # индекс SOB маркера регенерации
V_REGEN_A = 0x80F9E0           # double, читателей нет
V_REGEN_B = 0x80F830           # double, читателей нет
V_SHIELD_PRICE = 0x818954      # цена 100 щитов
V_SHIELD_WEIGHT = 0x818950     # вес 100 щитов

# --- опкоды -----------------------------------------------------------------
OP_REMOVE = 39
OP_AT_SOURCE = 52
OP_ADD = 53
OP_AT_SOURCE_FIXED = 54
OP_ADD_FIXED = 55
OP_MOVE_CHECK = 64
OP_SET_TRIGGER = 65
OP_BLAST = 75
OP_DEFINE_VIRUS = 173
OP_DEFINE_REGEN = 174
OP_DEFINE_SHIELDS = 176
OP_RANDOM_AMMO = 177
OP_NAMED = 185
OP_NAMED_EX = 186

#: опкоды, создающие запись предмета: (поле state, поле trigger, поле radius,
#: поле timer, kind, обработчик).  Позиция операндов доказана обработчиками
#: 0x4598e7/0x45994c/0x4599c5/0x459a14 (push -> аргументы 0x4686a0).
ITEM_OPS = {
    OP_AT_SOURCE: dict(kind=1, state=1, delay=2, trigger=3, event2=4,
                       radius=5, timer=6, handler=0x4599C5, pos=None),
    OP_ADD: dict(kind=1, state=1, delay=2, trigger=3, event2=4,
                 radius=5, timer=12, handler=0x4598E7, pos=(6, 7, 8)),
    OP_AT_SOURCE_FIXED: dict(kind=2, state=1, delay=2, trigger=3, event2=4,
                             radius=5, timer=6, handler=0x459A14, pos=None),
    OP_ADD_FIXED: dict(kind=2, state=1, delay=2, trigger=3, event2=4,
                       radius=5, timer=12, handler=0x45994C, pos=(6, 7, 8)),
    # op 177: один операнд = state, sob/trigger/radius берутся из DEFINE_AMMO
    # (+0x14/+0x18/+0x1c) в 0x450aeb..0x450af5
    OP_RANDOM_AMMO: dict(kind=1, state=0, delay=None, trigger=None,
                         event2=None, radius=None, timer=None,
                         handler=0x45BE77, pos=None),
    OP_NAMED: dict(kind=1, state=None, delay=None, trigger=None, event2=None,
                   radius=None, timer=None, handler=0x459A78, pos=None),
    OP_NAMED_EX: dict(kind=2, state=None, delay=None, trigger=None,
                      event2=None, radius=None, timer=None,
                      handler=0x459AC0, pos=None),
}

#: имя опкода и его сигнатура (gm/sbtvm.py) для печати
OP_NAMES = {
    OP_REMOVE: ("REMOVE_SOURCE_ITEM", ""),
    OP_AT_SOURCE: ("ADD_ITEM_AT_SOURCE", "OPPEELP"),
    OP_ADD: ("ADD_ITEM", "OPPEELLLLAAAP"),
    OP_AT_SOURCE_FIXED: ("ADD_ITEM_AT_SOURCE_FIXED", "OPPEELP"),
    OP_ADD_FIXED: ("ADD_ITEM_FIXED", "OPPEELLLLAAAP"),
    OP_MOVE_CHECK: ("ITEMS_MOVE_CHECK", ""),
    OP_SET_TRIGGER: ("SET_SOURCEITEM_TRIGGER_EVENT", "E"),
    OP_BLAST: ("CAUSE_BLAST_RADIUS_DAMAGE", "LLLLLE"),
    OP_DEFINE_VIRUS: ("DEFINE_VIRUS", "LLLLLOD"),
    OP_DEFINE_REGEN: ("DEFINE_REGENERATION_MARKER", "ODD"),
    OP_DEFINE_SHIELDS: ("DEFINE_SHIELDS", "LL"),
    OP_RANDOM_AMMO: ("ADD_RANDOM_AMMO_ITEM_AT_SOURCE", "P"),
    OP_NAMED: ("ADD_NAMED_ITEM", "ILLLAAA"),
    OP_NAMED_EX: ("ADD_NAMED_ITEM_EX", "LLLILLLAAA"),
}

# ---------------------------------------------------------------------------
# карта записи предмета: (offset, kind, имя, статус, кто читает, примечание)
# ---------------------------------------------------------------------------
FIELDS = (
    (0x00, "3d", "basis", "код",
     "0x4684b0 (rep movsd 6 dword), 0x41e025",
     "3 вектора-строки (3x f64), из double-операндов d9..d11"),
    (0x18, "3d", "position", "код",
     "0x4684b0, 0x469430, 0x469670, 0x469770, 0x4698b0",
     "x/y/z (3x f64); в 0x469430 z правится на константу"),
    (0x30, "u32", "runtime_object", "код",
     "0x4684b0 (запись), 0x46906a/0x469501 (0x435a93 destroy)",
     "0 = объекта нет; так у state -1 и у отложенных (state > 0)"),
    (0x34, "u32", "kind", "код",
     "0x468715 (1/2), 0x46917c, 0x469443, 0x4694eb, 0x46988a",
     "1 = ADD_ITEM[_AT_SOURCE], 2 = *_FIXED; 0 = «снят с обновления»"),
    (0x38, "i32", "state", "код",
     "0x4686d7, 0x4687e2, 0x469038, 0x4694d0, 0x46952b",
     "см. STATES; -2 свободен, -1 фантом, 0/-3/-4 живые, >0 таймер"),
    (0x3c, "i32", "respawn", "код",
     "0x46907c..0x4690c8",
     "задержка респавна: <0 -> только DM, 0 -> удалить, >0 -> тики"),
    (0x40, "i32", "sob_index", "код",
     "0x4684c1 (0x47c990 -> 0x4277cd), 0x46867a, 0x468d80",
     "индекс SOB_TABLE (модель предмета)"),
    (0x44, "u32", "trigger", "код",
     "0x4692f7/0x469303, 0x46981b, 0x469949, 0x46972f",
     "id события, которое постится при подборе/взрыве/цепочке; 0 = нет"),
    (0x48, "u32", "appear_event", "код",
     "0x46952b, 0x4695da",
     "постится, когда предмет появился (state > 0 -> 0) или взведён"),
    (0x4c, "i32", "radius", "код",
     "0x4695b1 (fild), 0x4697eb (fild), 0x4695bb",
     "радиус срабатывания в единицах мира; 0 = не подбирается"),
    (0x50, "i32", "timer", "код",
     "0x469619 (timer -= dt -> 0x469020), 0x4690d8 (сброс из +0x54)",
     "ticks жизни после появления; 0 = бессрочно"),
    (0x54, "i32", "timer_reload", "код",
     "0x4690d2 -> 0x4690d8",
     "копия +0x50 при создании; восстанавливается при респавне"),
    (0x58, "u8", "flags", "код",
     "0x468516 (object+0x74), 0x469239",
     "байт, уходящий в object+0x74 (видимость/альфа?) — гипотеза"),
    (0x5c, "u32", "source_low", "код",
     "0x4687da, 0x4692d8, 0x4697ff, 0x46992b",
     "слагаемое к handle предмета при посте события (низ 16 бит "
     "контекста VM 0x4573ae); op 177 пишет 0xffffffff"),
)

#: состояния: (значение, имя, что делает код, адреса)
STATES = (
    (ST_FREE, "free", "слот свободен; поиск слота 0x4686d7, освобождение "
     "0x469050/0x469099", "0x4686d7 0x469050 0x469099"),
    (ST_PHANTOM, "phantom", "объект не создан, слот занят; в сейв не "
     "попадает (0x466340), радиус-скан его триггерит (бит 0), op 39 "
     "освобождает слот (0x469050)",
     "0x4687fb 0x469047 0x4697a2 0x4695cf"),
    (ST_LIVE, "live", "обычный предмет: объект есть, триггер по "
     "радиусу (бит 0), снятие -> свободен либо таймер респавна",
     "0x4687e8 0x46929c 0x469564"),
    (ST_ARMED, "armed", "мина: объект есть, триггер по контакту "
     "(0x4692c7), при отсутствии игрока в радиусе переходит в -4 "
     "(0x4695d4); радиус-скан player-битом НЕ берёт",
     "0x4687ef 0x4692be 0x4695d4"),
    (ST_CHAIN, "chain", "взведённая мина: триггер по контакту, взрыву "
     "(0x4698b0) и цепочке соседей (0x469670, радиус 40.0)",
     "0x4687ea 0x4692b9 0x4696e5 0x4698dd"),
)

#: >0 — таймер появления (отдельная строка, т.к. это не «состояние», а тики)
TIMER_STATE = (
    "> 0", "timer", "state -= dt каждый тик (0x4694dc); при 0 — маркер "
    "меняется на предмет (0x4684b0) и постится +0x48 (0x46952b); "
    "маркер создаёт 0x468840 моделью из 0x80d878",
    "0x4694dc 0x468840 0x46950c 0x46952b")

#: кто постит +0x44 (пути срабатывания)
PATHS = (
    ("радиус игрока", "0x469770",
     "вызов из физики игрока 0x445165 (0x443690 <- 0x44d923, каждый тик), "
     "флаги 3; берёт state 0/-1 (бит 0) и -4 (бит 1); расстояние < +0x4c",
     "0x469770 0x445165"),
    ("радиус вещи", "0x469770",
     "код вещей 0x471e03 при флагах вещи (0x7ac3e4 & 0x80 или "
     "0x7ac3e8 & 0x800000)", "0x471e03"),
    ("взрыв", "0x4698b0",
     "op 75 CAUSE_BLAST_RADIUS_DAMAGE -> 0x4577f9; берёт только state -4; "
     "радиус = аргумент * 0.5 (0x4a2608)", "0x4577f9 0x4698c1 0x4698dd"),
    ("цепочка", "0x469670",
     "из апдейтера при state --4 (0x4692af/0x46938b) и при взведении "
     "(0x469612); соседи state -4 в радиусе 40.0 (0x4a2600)",
     "0x469670 0x4696e5"),
    ("снятие с обновления", "0x4690f0",
     "если трансформ предмета не пересчитался (0x43c51f/0x434c11, дельта "
     "0.0), kind = 0 (0x46929c) и для state -3/-4 постится +0x44",
     "0x46929c 0x4692c7 0x469378"),
)

#: опкоды наград внутри скриптов-триггеров (счётчики в --summary)
REWARD_OPS = (
    "ADD_SOURCEPLAYER_CASH", "ADD_SOURCEPLAYER_SALVAGE", "HEAL_SOURCEPLAYER",
    "ADD_SOURCEPLAYER_AMMO", "SOURCEPLAYER_INFECT", "REMOVE_SOURCE_ITEM",
    "CAUSE_BLAST_RADIUS_DAMAGE", "SET_SOURCEITEM_TRIGGER_EVENT",
    "ADD_SOURCESCREEN_TEXT", "PLAY_SAMPLE_AT_SOURCE", "GENERATE",
    "ADD_SPRITE_AT_SOURCE", "SLEEP", "SOURCEPLAYER_EQUIPMENT_INDEX",
)

#: опкоды, которые --summary считает по корпусу
TRACK_OPS = set(ITEM_OPS) | {
    OP_REMOVE, OP_MOVE_CHECK, OP_SET_TRIGGER, OP_BLAST,
    OP_DEFINE_VIRUS, OP_DEFINE_REGEN, OP_DEFINE_SHIELDS,
}

# ---------------------------------------------------------------------------
# байтовые якоря: (vma, hex, что)
# ---------------------------------------------------------------------------
ANCHORS = (
    (0x4686D2, "b8 98 63 80 00 83 38 fe",
     "поиск слота: база 0x806398, свободен = -2"),
    (0x4686E7, "81 ff 2c 01 00 00", "предел 300 предметов (0x12c)"),
    (0x4687EA, "83 f8 fc 74 3c 83 f8 fd 74 37",
     "state -4 и -3 создают объект (0x46882b)"),
    (0x4687E8, "74 41", "state 0 тоже создаёт объект"),
    (0x46882B, "8b 4c 24 4c 51 57 e8", "создание объекта 0x4684b0(slot, arg16)"),
    (0x4684C1, "8b 83 a0 63 80 00 50", "модель = SOB_TABLE[+0x40]"),
    (0x468715, "89 96 94 63 80 00", "kind (+0x34) = 1"),
    (0x468720, "c7 86 94 63 80 00 02", "kind (+0x34) = 2 (FIXED)"),
    (0x468823, "b8 fe ff ff ff", "отказ создания возвращает -2"),
    (0x469038, "8b 86 98 63 80 00", "op 39: читает state"),
    (0x469050, "c7 86 98 63 80 00 fe ff ff ff",
     "op 39: state -1 -> -2 (слот свободен)"),
    (0x4690CC, "89 86 98 63 80 00", "op 39: state = |+0x3c| (респавн)"),
    (0x4690D2, "8b 8e b4 63 80 00", "op 39: +0x54 -> таймер"),
    (0x4690D8, "89 8e b0 63 80 00", "op 39: ... в +0x50"),
    (0x46929C, "c7 46 fc 00 00 00 00", "апдейтер: kind = 0 (снят)"),
    (0x4692A5, "83 f8 fc 75 0d", "апдейтер: state == -4 -> цепочка"),
    (0x4692B9, "83 f8 fc 74 09", "апдейтер: -4 или -3 -> пост триггера"),
    (0x4692D8, "8b 46 24 8b 54 24 10", "пост: source = handle + [+0x5c]"),
    (0x4694D4, "3b c3", "апдейтер: state <= 0 -> вторая ветка"),
    (0x4694F2, "c7 46 fc 01 00 00 00", "появление: kind 0 -> 1"),
    (0x4695D4, "c7 06 fc ff ff ff", "нет игрока в радиусе -> state = -4"),
    (0x4695B1, "db 46 14", "радиус = +0x4c (fild)"),
    (0x4696E5, "dc 1d 00 26 4a 00", "цепочка: радиус 40.0 (0x4a2600)"),
    (0x469784, "bf 00 00 0c 01", "радиус-скан: handle = 0x10c0000 + slot"),
    (0x469799, "83 f8 fc 0f 85", "радиус-скан: state -4 только по биту 1"),
    (0x4698C1, "dc 0d 08 26 4a 00", "взрыв: радиус * 0.5 (0x4a2608)"),
    (0x4698DD, "39 5e 10 75", "взрыв: только state -4"),
    (0x445165, "e8 06 46 02 00", "физика игрока -> радиус-скан 0x469770"),
    (0x471E03, "e8 68 79 ff ff", "код вещей -> радиус-скан 0x469770"),
    (0x4577F9, "e8 b2 20 01 00", "op 75 -> 0x4698b0"),
    (0x457BE4, "e8 37 14 01 00", "op 39 -> 0x469020"),
    (0x457C26, "e8 55 1c 01 00", "op 64 -> 0x469880 (kind 0 -> 1)"),
    (0x457BC8, "3d 0c 01 00 00", "op 39: handle >= 0x10c"),
    (0x4599AF, "e8 ec ec 00 00", "op 52 -> создатель 0x4686a0"),
    (0x459A62, "e8 39 ec 00 00", "op 54 -> создатель 0x4686a0"),
    (0x45BE77, "e8 c4 4a ff ff", "op 177 -> 0x450940"),
    (0x468DE1, "e8 ba f8 ff ff", "op 185/186 -> создатель 0x4686a0"),
    (0x45C353, "8b 04 33 83 c3 04", "DEFINE_SHIELDS: arg0"),
    (0x45C359, "a3 54 89 81 00", "DEFINE_SHIELDS arg0 -> 0x818954 (цена/100)"),
    (0x45C364, "89 0d 50 89 81 00", "DEFINE_SHIELDS arg1 -> 0x818950 (вес/100)"),
    (0x45C375, "8b 14 33 83 c3 04", "DEFINE_REGENERATION_MARKER: arg0"),
    (0x45C37B, "89 15 78 d8 80 00", "маркер: SOB -> 0x80d878"),
    (0x45C3AA, "a3 b4 d8 80 00", "DEFINE_VIRUS L0 -> 0x80d8b4"),
    (0x45C3B8, "89 0d 80 d8 80 00", "DEFINE_VIRUS T -> 0x80d880"),
    (0x45C3F0, "dd 1d c0 f9 80 00", "DEFINE_VIRUS D -> 0x80f9c0 (double)"),
    (0x443C7D, "8b 04 8d 90 c2 81 00", "щиты игрока: 4 плиты 0x81c290"),
    (0x443C8F, "0f af 15 b4 d8 80 00", "вирус: плита -= dt * L0"),
    (0x443E10, "8b 83 40 c2 81 00", "конец вируса: все 4 плиты 0 -> flags"),
    (0x443E16, "24 bf", "снимает бит 0x40 (вирус) у 0x81c240"),
    (0x453923, "a1 80 d8 80 00", "текстура вируса = T (0x80d880)"),
    (0x45A6D9, "80 ca c0", "заражение игрока: flags |= 0xc0"),
    (0x45A71A, "83 ca 18", "заражение вещи: 0x7ac3e0 |= 0x18"),
    (0x446C1D, "8b 87 40 c2 81 00", "детектор (0x100) -> 0x469970 (мины)"),
)


class Fail(Exception):
    pass


_COUNT = [0]


def expect(cond, msg):
    _COUNT[0] += 1
    if not cond:
        raise Fail(msg)


def s32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


# ---------------------------------------------------------------------------
# модель состояния
# ---------------------------------------------------------------------------
def classify(state):
    """Класс состояния по коду (см. STATES/TIMER_STATE)."""
    if state == ST_FREE:
        return "free"
    if state == ST_PHANTOM:
        return "phantom"
    if state == ST_LIVE:
        return "live"
    if state == ST_CHAIN:
        return "chain"
    if state == ST_ARMED or state < ST_CHAIN:
        return "armed"
    return "timer"


def creates_object(state):
    """Создаётся ли 3D-объект сразу при создании записи (0x4687e8..0x468805)."""
    return state in CREATE_STATES


def triggerable(state, flags):
    """Взял бы радиус-скан 0x469770 этот state при данных флагах."""
    if flags & 1 and state in SCAN_BIT0:
        return True
    if flags & 2 and state in SCAN_BIT1:
        return True
    return False


def chainable(state):
    """Триггерится ли предмет взрывом/цепочкой (0x4698b0/0x469670)."""
    return state == ST_CHAIN


def handle(slot):
    return (HANDLE_FIRST + slot) << 16


def state_va(slot):
    return ITEM_STATE + slot * ITEM_SIZE


# ---------------------------------------------------------------------------
# разбор .sbt
# ---------------------------------------------------------------------------
def _sbtmod():
    from gm import sbt, sbtvm
    return sbt, sbtvm


def _scenemod():
    from gm import scene
    return scene


def walk(insns, path=()):
    """Все инструкции, включая тела IF/ELSE, с путём вложенности."""
    for ins in insns:
        yield ins, path
        body = getattr(ins, "body", None)
        if body:
            yield from walk(body, path + (ins,))


def dm_gated(path):
    """Лежит ли инструкция внутри IF DEATHMATCH_GAME (опкод 199)."""
    for ins in path:
        if ins.op == 199 and ins.args:
            name = ins.args[0][0]
            if name == "DEATHMATCH_GAME":
                return True
    return False


def level(path):
    sbt, sbtvm = _sbtmod()
    lvl = sbt.parse_file(path)
    scripts = lvl.records_of(11)
    script = scripts[0].payload if scripts else None
    code = script.code if script is not None else b""
    insns, _end = sbtvm.decode(code) if code else ([], 0)
    return lvl, script, insns


def named_table(lvl):
    """``[(name, params)]`` из записи NAMED_ITEMS (тип 5)."""
    out = []
    for rec in lvl.records:
        if rec.type == 5:
            for entry in rec.payload.entries:
                out.append((entry.name, tuple(entry.params)))
    return out


def named_state(params):
    return s32(params[2]) if len(params) > 2 else None


def item_insns(insns):
    """Все инструкции, создающие/трогающие предметы, с разобранными полями."""
    out = []
    for ins, path in walk(insns):
        if ins.op not in ITEM_OPS:
            continue
        spec = ITEM_OPS[ins.op]
        args = [s32(a) if isinstance(a, int) else a for a in ins.args]
        rec = {"op": ins.op, "offset": ins.off, "dm": dm_gated(path),
               "kind": spec["kind"], "args": args}
        if ins.op == OP_RANDOM_AMMO:
            rec["state"] = args[0] if args else None
        elif ins.op in (OP_NAMED, OP_NAMED_EX):
            idx = args[0] if ins.op == OP_NAMED else (
                args[3] if len(args) > 3 else None)
            rec["named_index"] = idx
            rec["state"] = None
        else:
            for key in ("state", "delay", "trigger", "event2", "radius",
                        "timer"):
                pos = spec.get(key)
                rec[key] = args[pos] if pos is not None and \
                    pos < len(args) else None
        out.append(rec)
    return out


def defs(insns):
    """Значения DEFINE_VIRUS/SHIELDS/REGENERATION_MARKER одного уровня."""
    out = {"virus": [], "shields": [], "regen": []}
    for ins, _path in walk(insns):
        args = [s32(a) if isinstance(a, int) else a for a in ins.args]
        if ins.op == OP_DEFINE_VIRUS:
            out["virus"].append(args)
        elif ins.op == OP_DEFINE_SHIELDS:
            out["shields"].append(args)
        elif ins.op == OP_DEFINE_REGEN:
            out["regen"].append(args)
    return out


def blocks(insns):
    """``{offset: [insns]}`` по под-скриптам верхнего уровня."""
    out, cur = {}, []
    for ins in insns:
        if ins.op == 0xFF:
            if cur:
                out[cur[0].off] = cur
            cur = []
        else:
            cur.append(ins)
    return out


def trigger_ops(insns, script, event):
    """Эффекты под-скрипта-сущности, на который указывает триггер."""
    if not event:
        return None
    scene = _scenemod()
    ents = {eid: off for eid, _f, off in scene.entities(script)}
    off = ents.get(event)
    if off is None:
        return {"event": event, "entity": None, "ops": []}
    blk = blocks(insns).get(off)
    if blk is None:
        sbt, sbtvm = _sbtmod()
        blk = [i for i in sbtvm.decode(script.code, off)[0] if i.op != 0xFF]
    return {"event": event, "entity": off,
            "ops": [ins.name for ins, _p in walk(blk)]}


def corpus_paths(game):
    return sorted(glob.glob(os.path.join(game, "*.sbt")))


# ---------------------------------------------------------------------------
# статистика по корпусу
# ---------------------------------------------------------------------------
def build_summary(paths):
    stats = {
        "files": 0, "files_with_script": 0, "levels": {},
        "opcodes": collections.Counter(), "states": collections.Counter(),
        "radius": collections.Counter(), "kind": collections.Counter(),
        "dm": collections.Counter(), "triggers": collections.Counter(),
        "trigger_miss": [], "named": collections.Counter(),
        "named_states": collections.Counter(), "named_bad": [],
        "defs": {"virus": collections.Counter(), "shields": collections.Counter(),
                 "regen": collections.Counter()},
        "def_files": collections.Counter(), "reward": collections.Counter(),
        "problems": [],
    }
    for path in paths:
        base = os.path.basename(path)
        stats["files"] += 1
        try:
            lvl, script, insns = level(path)
        except Exception as exc:                        # noqa: BLE001
            stats["problems"].append("%s: %s" % (base, exc))
            continue
        if script is not None:
            stats["files_with_script"] += 1
        table = named_table(lvl)
        recs = item_insns(insns)
        for ins, _path in walk(insns):
            if ins.op in TRACK_OPS:
                stats["opcodes"][ins.op] += 1
        stats["levels"][base] = {
            "items": collections.Counter(r["op"] for r in recs),
            "states": collections.Counter(r["state"] for r in recs
                                          if r["state"] is not None),
        }
        for rec in recs:
            stats["kind"][(rec["op"], rec["kind"])] += 1
            state = rec["state"]
            named = rec["op"] in (OP_NAMED, OP_NAMED_EX)
            if named:
                idx = rec.get("named_index")
                if idx is None or not 0 <= idx < len(table):
                    stats["named_bad"].append((base, rec["offset"], idx,
                                               len(table)))
                    state = None
                else:
                    name, params = table[idx]
                    state = named_state(params)
                    stats["named"][name] += 1
                    stats["named_states"][state] += 1
            if state is not None:
                if not named:
                    stats["states"][state] += 1
                stats["dm"][(state > 0, rec["dm"], named)] += 1
            if "radius" in rec and rec.get("radius") is not None:
                stats["radius"][rec["radius"]] += 1
            trig = rec.get("trigger")
            if trig is None:
                continue
            if trig == 0:
                stats["triggers"]["zero"] += 1
                continue
            info = trigger_ops(insns, script, trig)
            if not info or info["entity"] is None:
                stats["triggers"]["miss"] += 1
                stats["trigger_miss"].append((base, rec["offset"], trig))
                continue
            stats["triggers"]["hit"] += 1
            for name in info["ops"]:
                if name in REWARD_OPS:
                    stats["reward"][name] += 1
        for key, value in defs(insns).items():
            if value:
                stats["def_files"][key] += 1
            for args in value:
                stats["defs"][key][tuple(args)] += 1
    return stats


# ---------------------------------------------------------------------------
# печать
# ---------------------------------------------------------------------------
def print_fields():
    print("таблица предметов: база 0x%06x, %d слотов x 0x%x = 0x%x, "
          "конец 0x%06x" % (ITEM_BASE, ITEM_SLOTS, ITEM_SIZE, ITEM_BLOCK,
                            TABLE_END))
    print("state слота i: 0x%06x + i*0x%x;  handle: (0x%x + i) << 16 "
          "(0x%x..0x%x)" % (ITEM_STATE, ITEM_SIZE, HANDLE_FIRST,
                            HANDLE_FIRST, HANDLE_LAST))
    print()
    print("поля записи:")
    print("  %-6s %-5s %-17s %-5s %s" %
          ("offset", "тип", "имя", "статус", "кто читает/пишет"))
    for off, kind, name, status, readers, note in FIELDS:
        print("  +0x%02x  %-5s %-17s %-5s %s" % (off, kind, name, status,
                                                 readers))
        if note:
            print("         %s" % note)
    print()
    print("состояния:")
    for value, name, what, where in STATES + (TIMER_STATE,):
        print("  %-4s %-8s %s" % (value, name, what))
        print("        %s" % where)
    print()
    print("создание объекта (0x4687e8..0x468805): state 0, -3, -4 -> "
          "0x4684b0; state -1 -> отказ (объекта нет); state > 0 -> "
          "0x468840 (маркер) только в DM, иначе отказ")
    print("радиус-скан 0x469770: флаги & 1 -> state 0/-1, флаги & 2 -> -4; "
          "флаги 3 у обоих вызывающих")
    print()
    print("пути срабатывания +0x44:")
    for name, entry, what, where in PATHS:
        print("  %-22s %-9s %s" % (name, entry, what))
        print("        %s" % where)
    print()
    print("глобалы DEFINE_*:")
    for addr, name, what in (
            (V_VIRUS_DMG, "L0", "урон щиту за тик (0x443c8f)"),
            (V_VIRUS_TEX, "T", "текстура заражения (0x453923)"),
            (V_VIRUS_SND, "N", "сэмпл петли (0x46bb2c)"),
            (V_VIRUS_VOL, "V", "громкость петли"),
            (V_VIRUS_PRI, "P", "приоритет петли"),
            (V_VIRUS_ATT, "D", "затухание, double"),
            (V_REGEN_SOB, "regen sob", "модель маркера (0x468840)"),
            (V_REGEN_A, "regen a", "double, читателей нет"),
            (V_REGEN_B, "regen b", "double, читателей нет"),
            (V_SHIELD_PRICE, "shield price", "цена 100 щитов (0x452fb1)"),
            (V_SHIELD_WEIGHT, "shield weight", "вес 100 щитов (0x4524fe)")):
        print("  0x%06x  %-12s %s" % (addr, name, what))
    print()
    print("опкоды:")
    for op in sorted(OP_NAMES):
        name, sig = OP_NAMES[op]
        spec = ITEM_OPS.get(op)
        handler = spec["handler"] if spec else {
            OP_REMOVE: 0x457BC0, OP_MOVE_CHECK: 0x457C26,
            OP_SET_TRIGGER: 0x457BF1, OP_BLAST: 0x45775B,
            OP_DEFINE_VIRUS: 0x45C3A4, OP_DEFINE_REGEN: 0x45C375,
            OP_DEFINE_SHIELDS: 0x45C353}[op]
        print("  %3d 0x%06x %-34s %-12s %s"
              % (op, handler, name, sig or "-",
                 "создаёт запись" if spec else "прочее"))
    print()
    print("награды в под-скриптах триггеров считаются по: %s"
          % ", ".join(REWARD_OPS))


def _hist(counter, label, signed=True):
    items = sorted(counter.items(),
                   key=lambda kv: (kv[0] is None, kv[0]))
    return "%s: %s" % (label, ", ".join("%s×%d" % (k, v) for k, v in items))


def print_summary(paths, game=None):
    stats = build_summary(paths)
    if not stats["files"]:
        print("нет .sbt в %s" % (game or "?"))
        return False
    print("== корпус: %d файлов (со SCRIPT %d)"
          % (stats["files"], stats["files_with_script"]))
    print("  %-14s %-28s %s" % ("файл", "создание предметов (op×n)", "state"))
    for base in sorted(stats["levels"]):
        lvl = stats["levels"][base]
        ops = ", ".join("%d×%d" % (op, n) for op, n in sorted(lvl["items"].items()))
        sts = ", ".join("%d×%d" % (s, n)
                        for s, n in sorted(lvl["states"].items()))
        print("  %-14s %-28s %s" % (base, ops or "-", sts or "-"))
    print()
    print("== создание записей")
    total = sum(stats["opcodes"].values())
    for op in sorted(stats["opcodes"]):
        name = OP_NAMES[op][0]
        kinds = {k: n for (o, k), n in stats["kind"].items() if o == op}
        print("  op %-3d %-34s %4d  kind %s"
              % (op, name, stats["opcodes"][op],
                 ", ".join("%d×%d" % (k, n) for k, n in sorted(kinds.items()))
                 or "-"))
    print("  всего %d инструкций" % total)
    print()
    print("== состояния (state)")
    lines = []
    for st in sorted(stats["states"]):
        lines.append("%d:%-7s×%d" % (st, classify(st), stats["states"][st]))
    print("  " + ", ".join(lines))
    for named in (False, True):
        part = [(dm, n) for (v, dm, nm), n in stats["dm"].items()
                if v and nm == named]
        if not part:
            continue
        total = sum(n for _dm, n in part)
        inside = sum(n for dm, n in part if dm)
        print("  state > 0 (%s): %d, внутри IF DEATHMATCH_GAME %d, вне %d"
              % ("именованные" if named else "op 52-55/177", total,
                 inside, total - inside))
    rare = [(v, n) for v, n in stats["states"].items()
            if v not in (ST_FREE, ST_PHANTOM, ST_LIVE, ST_ARMED, ST_CHAIN)
            and v <= 0]
    if rare:
        print("  прочие отрицательные: %s"
              % ", ".join("%d×%d" % kv for kv in sorted(rare)))
    print()
    print("== радиусы (+0x4c)")
    print("  " + ", ".join("%d×%d" % (r, n) for r, n in
                           sorted(stats["radius"].items(), key=lambda kv: -kv[1])))
    print()
    print("== триггеры (+0x44)")
    print("  ноль: %d, в сущность: %d, промахов: %d"
          % (stats["triggers"]["zero"], stats["triggers"]["hit"],
             stats["triggers"]["miss"]))
    if stats["trigger_miss"]:
        for miss in stats["trigger_miss"][:5]:
            print("    %s @0x%x -> %d" % miss)
    print()
    print("== именованные предметы (op 185/186 -> NAMED_ITEMS)")
    print("  инструкций: %d, разрешено в записи: %d, state: %s"
          % (stats["opcodes"][OP_NAMED] + stats["opcodes"][OP_NAMED_EX],
             sum(stats["named"].values()),
             ", ".join("%d×%d" % (s, n) for s, n in
                       sorted(stats["named_states"].items()))))
    print("  индекс -1 (запись не создаётся, 0x468d52): %d"
          % len(stats["named_bad"]))
    print()
    print("== DEFINE_VIRUS / DEFINE_SHIELDS / DEFINE_REGENERATION_MARKER")
    for key, title in (("virus", "DEFINE_VIRUS (L0,T,N,V,P,D)"),
                       ("shields", "DEFINE_SHIELDS (цена/100, вес/100)"),
                       ("regen", "REGEN MARKER (sob, a, b)")):
        vals = stats["defs"][key]
        print("  %s: файлов %d" % (title, stats["def_files"][key]))
        if len(vals) <= 4:
            for value, n in sorted(vals.items()):
                print("     %s ×%d" % (value, n))
        else:
            common, n = collections.Counter(vals).most_common(1)[0]
            print("     частое %s ×%d, всего вариантов %d"
                  % (common, n, len(vals)))
            others = [v for v in vals if v != common]
            if others:
                print("     прочие: %s"
                      % ", ".join("%s×%d" % (v, vals[v])
                                  for v in sorted(others)[:6]))
    print()
    print("== награды в под-скриптах триггеров")
    reward = ", ".join("%s×%d" % (n, c) for n, c in
                       stats["reward"].most_common())
    print("  " + (reward or "-"))
    print()
    print("== проверки")
    problems = list(stats["problems"])
    if stats["files_with_script"] != stats["files"]:
        problems.append("не во всех файлах есть SCRIPT")
    if stats["trigger_miss"]:
        problems.append("триггеры без сущности: %d" % len(stats["trigger_miss"]))
    weird = sorted(v for v in stats["states"]
                   if v not in (ST_PHANTOM, ST_LIVE, ST_ARMED, ST_CHAIN)
                   and v <= 0)
    if weird:
        problems.append("незнакомые отрицательные state: %s" % weird)
    if stats["def_files"]["virus"] != stats["files"]:
        problems.append("DEFINE_VIRUS не во всех файлах")
    if stats["def_files"]["shields"] != stats["files"]:
        problems.append("DEFINE_SHIELDS не во всех файлах")
    if stats["def_files"]["regen"] != stats["files"]:
        problems.append("DEFINE_REGENERATION_MARKER не во всех файлах")
    for p in problems:
        print("  ОШИБКА: %s" % p)
    if not problems:
        print("  OK: предметы создаются, все триггеры разрешаются, "
              "DEFINE_* есть в каждом уровне")
    return not problems


def _num(value):
    return "-" if value is None else str(value)


def print_items(path):
    lvl, script, insns = level(path)
    table = named_table(lvl)
    recs = item_insns(insns)
    print("== %s: инструкций с предметами %d, NAMED_ITEMS %d"
          % (os.path.basename(path), len(recs), len(table)))
    print("  %-7s %-30s %-6s %-6s %-7s %-6s %-7s %-6s %s"
          % ("offset", "опкод", "state", "delay", "trigger", "radius",
             "timer", "DM", "куда ведёт триггер"))
    for rec in recs:
        state = rec["state"]
        note = ""
        if rec["op"] in (OP_NAMED, OP_NAMED_EX):
            idx = rec.get("named_index")
            if idx is not None and 0 <= idx < len(table):
                name, params = table[idx]
                state = named_state(params)
                note = "%s (f2=%s f4=%s f5=%s f7=%s)" % (
                    name, state, _num(s32(params[4]) if len(params) > 4 else None),
                    _num(s32(params[5]) if len(params) > 5 else None),
                    _num(s32(params[7]) if len(params) > 7 else None))
            else:
                note = "индекс %s вне таблицы" % idx
        elif rec["op"] == OP_RANDOM_AMMO:
            note = "sob/trigger/radius из DEFINE_AMMO"
        trig = rec.get("trigger")
        if trig:
            info = trigger_ops(insns, script, trig)
            ops = info["ops"] if info else []
            note = "-> " + (", ".join(ops[:6]) or "нет сущности")
        print("  0x%05x %-30s %-6s %-6s %-7s %-6s %-7s %-6s %s"
              % (rec["offset"], OP_NAMES[rec["op"]][0], _num(state),
                 _num(rec.get("delay")), _num(trig), _num(rec.get("radius")),
                 _num(rec.get("timer")), "да" if rec["dm"] else "-", note))


def json_model(paths, with_items=False):
    out = {"table": {
        "base": ITEM_BASE, "state": ITEM_STATE, "slots": ITEM_SLOTS,
        "size": ITEM_SIZE, "block": ITEM_BLOCK, "end": TABLE_END,
        "handle_first": HANDLE_FIRST, "handle_last": HANDLE_LAST,
        "fields": [{"offset": off, "type": kind, "name": name,
                    "status": status, "readers": readers, "note": note}
                   for off, kind, name, status, readers, note in FIELDS],
        "states": [{"value": value, "name": name, "what": what,
                    "where": where} for value, name, what, where in STATES],
        "timer": {"value": TIMER_STATE[0], "name": TIMER_STATE[1],
                  "what": TIMER_STATE[2], "where": TIMER_STATE[3]},
        "create_states": list(CREATE_STATES),
        "scan_bit0": list(SCAN_BIT0), "scan_bit1": list(SCAN_BIT1),
        "paths": [{"name": n, "entry": e, "what": w, "where": a}
                  for n, e, w, a in PATHS],
        "globals": {
            "virus": {"L0": V_VIRUS_DMG, "T": V_VIRUS_TEX, "N": V_VIRUS_SND,
                      "V": V_VIRUS_VOL, "P": V_VIRUS_PRI, "D": V_VIRUS_ATT},
            "regen": {"sob": V_REGEN_SOB, "a": V_REGEN_A, "b": V_REGEN_B},
            "shields": {"price": V_SHIELD_PRICE, "weight": V_SHIELD_WEIGHT},
            "player_plates": PLATE_ADDRS,
            "player_flags": PLAYER_BASE + PLAYER_FLAGS},
        "opcodes": {str(op): {"name": OP_NAMES[op][0], "sig": OP_NAMES[op][1]}
                    for op in OP_NAMES},
    }, "stats": {}, "anchors": check_anchors()}
    stats = build_summary(paths)
    out["stats"] = {
        "files": stats["files"],
        "opcodes": {str(k): v for k, v in stats["opcodes"].items()},
        "states": {str(k): v for k, v in stats["states"].items()},
        "radius": {str(k): v for k, v in stats["radius"].items()},
        "triggers": dict(stats["triggers"]),
        "trigger_miss": stats["trigger_miss"],
        "named": dict(stats["named"]),
        "named_states": {str(k): v for k, v in stats["named_states"].items()},
        "named_bad": stats["named_bad"],
        "defs": {k: {"%s" % (v,): n for v, n in stats["defs"][k].items()}
                 for k in stats["defs"]},
        "def_files": dict(stats["def_files"]),
        "reward": dict(stats["reward"]),
        "problems": stats["problems"],
    }
    if with_items:
        out["levels"] = {}
        for path in paths:
            lvl, _script, insns = level(path)
            out["levels"][os.path.basename(path)] = {
                "items": item_insns(insns),
                "named": [{"name": n, "params": list(p)}
                          for n, p in named_table(lvl)],
                "defs": defs(insns),
            }
    return out


# ---------------------------------------------------------------------------
# якоря
# ---------------------------------------------------------------------------
def check_anchors():
    """``[{vma, bytes, expect, ok, what}]``; ``ok`` = None, если нет exe."""
    try:
        import exelib
        blob = exelib.data()
        va2off = exelib.va2off
    except Exception:                                    # noqa: BLE001
        blob = None
    out = []
    for vma, want, what in ANCHORS:
        ok, got = None, ""
        if blob is not None:
            off = va2off(vma)
            if off is not None:
                got = blob[off:off + len(want) // 3 + 1].hex(" ")
                ok = got == want
        out.append({"vma": vma, "expect": want, "bytes": got, "ok": ok,
                    "what": what})
    return out


def print_anchors():
    anchors = check_anchors()
    for a in anchors:
        state = "-" if a["ok"] is None else ("ok" if a["ok"] else "FAIL")
        print("%-4s 0x%06x  %-32s %s"
              % (state, a["vma"], a["expect"][:32], a["what"]))
    bad = [a for a in anchors if a["ok"] is False]
    return not bad


def self_test_anchors():
    anchors = check_anchors()
    if anchors and anchors[0]["ok"] is None:
        print("SKIP байтовые якоря: gundx.exe недоступен")
        return
    bad = [a for a in anchors if not a["ok"]]
    expect(not bad, "якоря не совпали: %s"
           % ", ".join("0x%06x (%s != %s)" % (a["vma"], a["bytes"], a["expect"])
                       for a in bad[:3]))
    print("OK   байтовые якоря: %d/%d" % (len(anchors), len(anchors)))


# ---------------------------------------------------------------------------
# синтетический код (self-test)
# ---------------------------------------------------------------------------
def insn(op, operands):
    """``operands`` = список ``("i", value)`` / ``("d", value)`` по порядку."""
    out = bytearray([op])
    for kind, value in operands:
        out += struct.pack("<i" if kind == "i" else "<d", value)
    return bytes(out)


def if_block(cond_id, body, cond_args=b""):
    """IF <cond> <size> { body }: size считается от поля size (0x457412)."""
    size = 4 + len(body)
    return bytes([199, cond_id]) + cond_args + struct.pack("<I", size) + body


def item_code():
    """Код с одной инструкцией каждого вида + DM-гейт для op 177."""
    i = lambda *values: [("i", v) for v in values]          # noqa: E731
    d = lambda *values: [("d", v) for v in values]          # noqa: E731
    at_source = insn(OP_AT_SOURCE, i(25, 0, 0, -30083, 0, 200, 0))
    add = insn(OP_ADD, i(40, -3, 0, -30084, 0, 100, 1, 2, 3) +
               d(1.0, 0.0, 0.0) + i(55))
    fixed = insn(OP_ADD_FIXED, i(41, -1, -5460, 900000, 0, 1600, 7, 8, 9) +
                 d(1.0, 0.0, 0.0) + i(0))
    gated = if_block(12, insn(OP_RANDOM_AMMO, i(5460)))     # DEATHMATCH_GAME
    named = insn(OP_NAMED, i(0, 0, 10, 0) + d(1.0, 2.0, 3.0))
    return at_source + add + fixed + gated + named + b"\xff"


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def do_selftest():
    _COUNT[0] = 0

    # 1. карта записи
    expect([f[0] for f in FIELDS] == sorted(f[0] for f in FIELDS),
           "смещения полей не по возрастанию")
    expect(len({f[0] for f in FIELDS}) == len(FIELDS), "смещения повторяются")
    expect(all(0 <= f[0] < ITEM_SIZE for f in FIELDS),
           "поле за пределами 0x60")
    expect(FIELDS[4][2] == "state" and FIELDS[4][0] == 0x38,
           "state не на +0x38")
    expect([f[2] for f in FIELDS][-1] == "source_low", "последнее поле")
    print("OK   карта: %d полей, state +0x38, запись 0x%x"
          % (len(FIELDS), ITEM_SIZE))

    # 2. геометрия таблицы и handle
    expect(ITEM_SLOTS == 300, "слотов %d" % ITEM_SLOTS)
    expect(ITEM_BASE + ITEM_BLOCK == TABLE_END == 0x80D3E0,
           "конец таблицы 0x%06x" % TABLE_END)
    expect(state_va(0) == ITEM_STATE == 0x806398, "state слота 0")
    expect(state_va(299) == 0x80D3B8, "state слота 299 = 0x%06x" % state_va(299))
    expect(SCAN_END == 0x80D418, "предел цикла 0x%06x" % SCAN_END)
    expect(handle(0) == 0x10C0000, "handle слота 0")
    expect(handle(299) >> 16 == HANDLE_LAST == 0x237, "handle слота 299")
    expect((handle(299) >> 16) < HANDLE_END, "0x2380000 — предел цикла")
    print("OK   таблица: 300 x 0x60, конец 0x80d3e0, handle 0x10c..0x237")

    # 3. состояния
    expect(classify(-2) == "free" and classify(-1) == "phantom", "free/phantom")
    expect(classify(0) == "live", "0 не live")
    expect(classify(-3) == "armed" and classify(-4) == "chain", "-3/-4")
    expect(classify(-5460) == "armed", "state < -4 не armed")
    expect(classify(1) == "timer" and classify(5460) == "timer", ">0 не timer")
    expect(CREATE_STATES == (0, -3, -4), "создающие состояния")
    expect(not creates_object(-1) and not creates_object(-2), "фантом/свободен")
    expect(not creates_object(36) and not creates_object(5460),
           "state > 0 создаёт объект в кампании")
    expect(SCAN_BIT0 == (0, -1) and SCAN_BIT1 == (-4,), "наборы скана")
    expect(triggerable(0, 3) and triggerable(-1, 3), "бит 0")
    expect(not triggerable(-3, 3), "мина не берётся радиус-сканом")
    expect(triggerable(-4, 2) and not triggerable(-4, 1), "бит 1")
    expect(chainable(-4) and not chainable(-3), "цепочка только -4")
    print("OK   состояния: create {0,-3,-4}, скан {0,-1}+{-4}, цепочка -4")

    # 4. разбор синтетического кода реальным декодером
    code = item_code()
    recs = item_insns(_sbtmod()[1].decode(code)[0])
    expect(len(recs) == 5, "синтетика: инструкций %d" % len(recs))
    by_op = {r["op"]: r for r in recs}
    expect(by_op[OP_AT_SOURCE]["state"] == 0, "op52 state")
    expect(by_op[OP_AT_SOURCE]["trigger"] == -30083, "op52 trigger знаковый")
    expect(by_op[OP_AT_SOURCE]["radius"] == 200, "op52 radius")
    expect(by_op[OP_AT_SOURCE]["kind"] == 1, "op52 kind")
    expect(by_op[OP_ADD]["state"] == -3 and by_op[OP_ADD]["kind"] == 1,
           "op53 state/kind")
    expect(by_op[OP_ADD]["timer"] == 55, "op53 timer (операнд 12)")
    expect(by_op[OP_ADD_FIXED]["kind"] == 2, "op55 kind 2")
    expect(by_op[OP_ADD_FIXED]["delay"] == -5460, "op55 delay")
    expect(by_op[OP_ADD_FIXED]["radius"] == 1600, "op55 radius")
    expect(by_op[OP_RANDOM_AMMO]["state"] == 5460, "op177 state (операнд 0)")
    expect(by_op[OP_RANDOM_AMMO]["dm"], "IF DEATHMATCH_GAME не распознан")
    expect(not by_op[OP_AT_SOURCE]["dm"], "ложный DM-гейт")
    expect(by_op[OP_NAMED]["kind"] == 1, "kind op185")
    print("OK   синтетика: 5 инструкций, поля и DM-гейт разобраны")

    # 5. блок ITEM 300 x 0x60 через records.py (round-trip)
    import records
    samples = [(ST_PHANTOM, 0), (ST_LIVE, 0), (ST_ARMED, 200),
               (ST_CHAIN, 200), (36, 200), (5460, 0)]
    payload = bytearray(ITEM_BLOCK)
    for slot in range(ITEM_SLOTS):
        state, radius = samples[slot % len(samples)]
        off = slot * ITEM_SIZE
        struct.pack_into("<3d", payload, off + 0x00, 1.0, 0.0, 0.0)
        struct.pack_into("<3d", payload, off + 0x18, slot, slot * 2, 8)
        struct.pack_into("<i", payload, off + 0x34, 1 + (slot & 1))
        struct.pack_into("<i", payload, off + 0x38, state)
        struct.pack_into("<i", payload, off + 0x3C, -slot)
        struct.pack_into("<i", payload, off + 0x40, slot % 8)
        struct.pack_into("<i", payload, off + 0x44, -30083)
        struct.pack_into("<i", payload, off + 0x48, 0)
        struct.pack_into("<i", payload, off + 0x4C, radius)
        struct.pack_into("<i", payload, off + 0x50, 100)
        struct.pack_into("<i", payload, off + 0x54, 100)
        payload[off + 0x58] = slot & 0xFF
        struct.pack_into("<I", payload, off + 0x5C, 0xFFFF)
    decoded = records.decode_items(bytes(payload), include_empty=True)
    expect(decoded["slots"] == ITEM_SLOTS, "слотов в блоке")
    expect(not decoded["checks"], "проверки блока: %s" % decoded["checks"][:3])
    expect(records.encode_items(decoded) == bytes(payload),
           "блок ITEM не пересобрался побайтово")
    expect(decoded["active"] == ITEM_SLOTS, "активных слотов %d"
           % decoded["active"])
    states = [it["state"] for it in decoded["items"]]
    expect(states[0] == ST_PHANTOM and states[1] == ST_LIVE, "состояния 0/1")
    expect(states[4] == 36 and states[5] == 5460, "таймеры в блоке")
    expect(decoded["items"][3]["ops"][1] == -30083, "триггер из блока")
    expect(decoded["items"][3]["ops"][3] == 200, "радиус из блока")
    free = bytearray(ITEM_BLOCK)
    for slot in range(ITEM_SLOTS):
        struct.pack_into("<i", free, slot * ITEM_SIZE + 0x38, ST_FREE)
    empty = records.decode_items(bytes(free))
    expect(empty["active"] == 0 and empty["items"] == [], "маркер -2 не работает")
    print("OK   блок ITEM: 300 x 0x60, decode/encode побайтово, маркер -2")

    # 6. якоря
    self_test_anchors()

    # 8. корпус (пропускается без игры)
    paths = corpus_paths(DEFAULT_GAME)
    if not paths:
        print("SKIP корпус: %s не найден" % DEFAULT_GAME)
        print("\nOK: %d проверок" % _COUNT[0])
        return
    stats = build_summary(paths)
    expect(stats["files"] == 36, "файлов %d, ожидалось 36" % stats["files"])
    expect(stats["files_with_script"] == 36, "SCRIPT не во всех файлах")
    expect(stats["opcodes"][OP_AT_SOURCE] == 401, "op52 %d"
           % stats["opcodes"][OP_AT_SOURCE])
    expect(stats["opcodes"][OP_ADD] == 17, "op53 %d" % stats["opcodes"][OP_ADD])
    expect(stats["opcodes"][OP_AT_SOURCE_FIXED] == 636, "op54 %d"
           % stats["opcodes"][OP_AT_SOURCE_FIXED])
    expect(stats["opcodes"][OP_ADD_FIXED] == 84, "op55 %d"
           % stats["opcodes"][OP_ADD_FIXED])
    expect(stats["opcodes"][OP_RANDOM_AMMO] == 440, "op177 %d"
           % stats["opcodes"][OP_RANDOM_AMMO])
    expect(stats["opcodes"][OP_REMOVE] == 2342, "op39 %d"
           % stats["opcodes"][OP_REMOVE])
    expect(stats["opcodes"][OP_MOVE_CHECK] == 45, "op64 %d"
           % stats["opcodes"][OP_MOVE_CHECK])
    expect(stats["opcodes"][OP_SET_TRIGGER] == 37, "op65 %d"
           % stats["opcodes"][OP_SET_TRIGGER])
    expect(stats["triggers"]["miss"] == 0, "триггеры без сущности: %d"
           % stats["triggers"]["miss"])
    expect(stats["triggers"]["hit"] == 690, "триггеров в сущность %d"
           % stats["triggers"]["hit"])
    expect(stats["triggers"]["zero"] == 448, "нулевых триггеров %d"
           % stats["triggers"]["zero"])
    expect(stats["states"][ST_ARMED] == 40, "-3: %d" % stats["states"][ST_ARMED])
    expect(stats["states"][ST_PHANTOM] == 236, "-1: %d"
           % stats["states"][ST_PHANTOM])
    expect(stats["radius"][0] == 448, "радиус 0: %d" % stats["radius"][0])
    expect(stats["radius"][200] == 492, "радиус 200: %d" % stats["radius"][200])
    expect(stats["named_states"][0] > 500, "именованные state 0")
    expect(sum(stats["named"].values()) > 3000, "именованных предметов %d"
           % sum(stats["named"].values()))
    for key in ("virus", "shields", "regen"):
        expect(stats["def_files"][key] == 36, "%s не во всех файлах" % key)
    shield = list(stats["defs"]["shields"])
    expect(shield == [(50, 0)], "DEFINE_SHIELDS: %s" % shield)
    regen = list(stats["defs"]["regen"])
    expect(regen == [(19, 3000.0, 50.0)], "REGEN: %s" % regen)
    expect(stats["reward"]["REMOVE_SOURCE_ITEM"] > 300,
           "REMOVE_SOURCE_ITEM в триггерах: %d"
           % stats["reward"]["REMOVE_SOURCE_ITEM"])
    expect(stats["reward"]["ADD_SOURCEPLAYER_CASH"] > 300,
           "CASH в триггерах: %d" % stats["reward"]["ADD_SOURCEPLAYER_CASH"])
    print("OK   корпус: %d файлов, %d созданий, триггеры %d/%d, "
          "награды: cash×%d remove×%d"
          % (stats["files"], sum(stats["opcodes"][op] for op in ITEM_OPS),
             stats["triggers"]["hit"], stats["triggers"]["hit"] +
             stats["triggers"]["zero"],
             stats["reward"]["ADD_SOURCEPLAYER_CASH"],
             stats["reward"]["REMOVE_SOURCE_ITEM"]))

    print("\nOK: %d проверок" % _COUNT[0])


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def resolve(value, game):
    if os.path.isabs(value) or os.path.exists(value):
        return value
    cand = os.path.join(game, value)
    return cand if cand.endswith(".sbt") else cand + ".sbt"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Gunmetal: предметы, подбор, маркеры регенерации, "
                    "вирус и щиты")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME",
                                                     DEFAULT_GAME))
    ap.add_argument("--fields", action="store_true",
                    help="карта записи, состояния, пути триггера, глобалы")
    ap.add_argument("--summary", action="store_true",
                    help="сводка по корпусу 36 уровней")
    ap.add_argument("--json", action="store_true", help="модель в JSON")
    ap.add_argument("--file", help="один .sbt (для --items / --json)")
    ap.add_argument("--items", action="store_true",
                    help="инструкции предметов уровня (нужен --file)")
    ap.add_argument("--anchors", action="store_true", help="байтовые якоря")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    paths = corpus_paths(args.game)
    if args.file:
        paths = [resolve(args.file, args.game)]

    if args.self_test:
        do_selftest()
        return 0
    if args.anchors:
        return 0 if print_anchors() else 1
    if args.fields:
        print_fields()
        return 0
    if args.json:
        print(json.dumps(json_model(paths, with_items=bool(args.file)),
                         ensure_ascii=False, indent=2, sort_keys=True,
                         default=str))
        return 0
    if args.items:
        if not args.file:
            ap.error("--items требует --file LEVEL.sbt")
        print_items(paths[0])
        return 0
    if args.summary:
        return 0 if print_summary(paths, args.game) else 1
    print_fields()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
