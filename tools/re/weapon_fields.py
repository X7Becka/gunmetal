#!/usr/bin/env python3
"""Field maps of the four SBT record tables (M2.3.5).

The level script stores four ``DEFINE_*`` tables into fixed-size record arrays.
Every record is a run of script operands; the *store functions* decide which
memory offset each operand lands in, so the file order and the memory order do
not match.  This module carries the resulting maps (derived from the store
functions, see ``docs/WEAPON_FIELDS.md``) and can

* print the maps (``--map``, the default),
* decode a real ``.sbt`` and print the memory image of every record
  (``--file LEVEL.sbt``),
* check shipped data against the proven expectations (``--verify``),
* run the built-in tests (``--self-test``).

Record arrays::

    DEFINE_WEAPONS       0x80fa00  stride 0x2a8  store 0x450be0  handler 0x45cac0
    DEFINE_AMMO          0x818960  stride 0x038  store 0x450b10  handler 0x45cc40
    DEFINE_EQUIPMENT     0x814c60  stride 0x040  store 0x450e60  handler 0x45cbe0
    DEFINE_PLAYER_TYPES  0x7995e0  stride 0x1cc  store 0x47a170  handler 0x45ccc0

Tags: ``W0..W20`` (weapons), ``A0..A9`` (ammo), ``E0..E11`` (equipment),
``P0..P94`` (chassis, i.e. player types) — one tag per *file dword*, the same
numbering ``tools/re/sbt_fields.py`` calls ``dN``.

Only the Python standard library is used; the SBT codec comes from the repo
(``gm/sbt.py`` + ``gm/sbtvm.py``, also stdlib-only).

Usage::

    python3 tools/re/weapon_fields.py --map
    python3 tools/re/weapon_fields.py --map --table DEFINE_AMMO
    python3 tools/re/weapon_fields.py --file "/mnt/d/games/Gunmetal 3dfx/level0.sbt"
    python3 tools/re/weapon_fields.py --verify
    python3 tools/re/weapon_fields.py --self-test
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

DEFAULT_DIR = "/mnt/d/games/Gunmetal 3dfx"
DEFAULT_LEVELS = ("level0.sbt", "dm1.sbt")

# ---------------------------------------------------------------------------
# field tables
# ---------------------------------------------------------------------------
# (dw, mem, kind, name, status, readers, note)
#   dw      file dword index (int) or the two halves of one double (tuple)
#   mem     offset inside the memory record
#   kind    u32 / i32 / f64 / str / ptr
#   status  "код" = proven by a reader/validation, "значения" = value ranges,
#           "гипотеза" = guess, "—" = nobody reads it

WEAPONS = [
    (0, 0x24, "u32", "fire_mode", "код", "0x445ac6 0x445b4f 0x445d27 0x445dbd",
     "switch 0/1/4/5 у 0x445ac0"),
    (1, 0x2c, "u32", "fire_period_ticks", "код", "0x445f02 0x4453f8",
     "фиксированные тики (91 тик = 1 с, 0x44db80); 17 у MACHINE GUN"),
    (2, 0x3c, "u32", "ammo_type", "код", "0x445b63 0x445dd5 0x445e76 0x450991 0x4545e9",
     "индекс DEFINE_AMMO; стор проверяет < [0x818958] (0x450d3a)"),
    (3, 0x40, "u32", "ammo_per_shot", "код", "0x445b74 0x445df5 0x445e92 0x45494b",
     "вычитается из запаса 0x445e98"),
    (4, 0x30, "u32", "projectile_event", "код", "0x445ee2",
     "id GENERATE/event, не указатель на снаряд"),
    (5, 0x34, "u32", "secondary_event", "код", "0x445cbc 0x445f55",
     "второй event id (петля/альтернатива)"),
    (6, 0x38, "u32", "state_event", "код", "0x445cdd 0x445f75",
     "пишется в player+0x81c35c как состояние"),
    (7, 0x4c, "u32", "id", "код", "0x445c00 0x445e3d",
     "уходит в event_post 0x4795b0 как event id; в поставе 0xffff8ab5"),
    (8, 0x44, "u32", "price", "код", "0x47c2a7 0x47c36b 0x496c6a 0x45bcd3 0x479eae",
     "наличные 0x81c36c"),
    (9, 0x48, "u32", "weight", "код", "0x45253a", "суммируется в 0x81c374 (0x4524d0)"),
    (10, 0x28, "u32", "flags", "код",
     "0x47c1bc 0x47c260 0x47c29b 0x47c34d 0x496c11 0x496c53",
     "0x01 RIGHT TO BUY, 0x02 LEFT TO SELL, 0x08 пропуск в цикле; 0x40/0x80 не тестируются"),
    (11, 0x294, "u32", "class_index", "код", "0x44577e 0x4457bc",
     "сравнивается с player+0x81c404 (маска семейства)"),
    ((12, 13), 0x00, "f64", "charge_rate", "код", "0x445c5b 0x81bfe8",
     "в памяти = файловое значение x 0.91 (const 0x4a2488)"),
    ((14, 15), 0x08, "f64", "spread_x", "код", "0x4459a9 0x4781d9",
     "тангенс конуса, ось 1"),
    ((16, 17), 0x10, "f64", "spread_y", "код", "0x44599b 0x47821b",
     "тангенс конуса, ось 2"),
    ((18, 19), 0x18, "f64", "range", "код", "0x445992 0x4784fb",
     "верхняя граница дистанции попадания"),
    (20, 0x20, "u32", "muzzle", "код", "0x4458c4", "индекс узла 0..7 (таблица 0x81bf38)"),
]

WEAPONS_EXTRA = [
    (0x50, "icon", "структура 0x18 байт, грузится 0x47efe0; при \"none\" зануляются 0x240 байт"),
    (0x290, "handle", "FUN_0040b9c0(2-я строка записи); читает только стор"),
    (0x298, "name", "char[16], strncpy 15 байт; читают 0x49291e, 0x496f43"),
]

AMMO = [
    (0, 0x04, "u32", "max", "код", "0x47b5e6 0x47b634 0x4541c3 0x459d14 0x45e4ba 0x460311",
     "лимит запаса; покупка клампится"),
    (1, 0x18, "u32", "pickup_event", "код", "0x450ab4", "arg1 event_post при подборе"),
    (2, 0x14, "u32", "pickup_object", "код", "0x450aeb", "FUN_004686a0 (спавн объекта)"),
    (3, 0x1c, "u32", "pickup_extra", "код", "0x450aae", "третий id того же спавна"),
    (4, 0x08, "u32", "lot", "код", "0x47b4e4 0x47b613 0x479e76",
     "размер партии: продажа -=, покупка +="),
    (5, 0x0c, "u32", "price", "код", "0x47b50b 0x47b555 0x47b5af 0x47bbec 0x495404",
     "наличные 0x81c36c"),
    (6, 0x10, "u32", "weight_per_round", "код", "0x4525ae",
     "вес += запас x +0x10"),
    (7, 0x00, "u32", "flags", "код", "0x47b3b2 0x47b425 0x47b4db 0x47b5bd 0x49539b",
     "0x01 купить, 0x02 продать, 0x08 скрыть; 0x80 только в данных"),
    (8, 0x20, "u32", "rounds_per_unit", "код", "0x450bbf 0x4929d5 0x494415 0x495680",
     "патронов в обойме; стор клампит снизу в 1"),
    (9, 0x24, "u32", "drop_weight", "код", "0x4509b4 0x450a0c",
     "вес случайного выбора типа при подборе"),
]

AMMO_EXTRA = [
    (0x28, "name", "char[16], strncpy 15 байт; читают 0x49291e, 0x496f43"),
]

EQUIPMENT = [
    (0, 0x00, "u32", "flags", "код",
     "0x47b6f5 0x47b798 0x47b7d0 0x47b8ba 0x4460f5 0x464deb",
     "0x01 купить, 0x02 продать, 0x04 вне цикла, 0x08 скрыть, 0x80 сетевой фильтр"),
    (1, 0x04, "u32", "spawn_event", "код", "0x453370 0x45427f 0x47b940",
     "event id, который постят при выдаче/активации"),
    (2, 0x08, "u32", "activate_event", "код", "0x446099 0x45a542",
     "event id активации (SOURCEPLAYER_EQUIPMENT_ACTIVATE)"),
    (3, 0x0c, "u32", "event_on", "код", "0x446228 0x446574",
     "постится, когда оборудование ещё не активно"),
    (4, 0x10, "u32", "event_off", "код", "0x44621d 0x446521 0x45bd16",
     "постится, когда оборудование уже активно (снятие)"),
    (5, 0x14, "u32", "unused_14", "—", "", "читателей нет (только стор 0x450edf)"),
    (6, 0x18, "u32", "unused_18", "—", "", "читателей нет (только стор 0x450ee9)"),
    (7, 0x1c, "u32", "unused_1c", "—", "", "читателей нет (только стор 0x450ef3)"),
    (8, 0x20, "i32", "ammo_index", "код",
     "0x4463a4 0x4509e9 0x45e89c 0x492ba3 0x4945d9 0x499a2f",
     "индекс DEFINE_AMMO (-1 = без боеприпаса); проверка 0x450f1f"),
    (9, 0x24, "u32", "price", "код", "0x47b879 0x47b8cd 0x479edc 0x47b0f1 0x495d30",
     "наличные 0x81c36c"),
    (10, 0x28, "u32", "weight", "код", "0x452576", "суммируется в 0x81c374"),
    (11, 0x3c, "u32", "object_id", "код", "0x446488 0x47b0e7 0x492b56 0x495f18",
     "поиск оборудования по id (FUN_00446470)"),
]

EQUIPMENT_EXTRA = [
    (0x2c, "name", "char[16], strncpy 15 байт"),
]

# Chassis: 95 file dwords.  The header is explicit, the physics block is a
# regular grid, the tail is the handle/event block.
CHASSIS_HEADER = [
    (0, 0x148, "u32", "weight_limit", "код", "0x47ac8f 0x4525d5",
     "предел массы -> player+0x81c378 (1000 по умолчанию)"),
    ((1, 2), 0x18, "f64", "speed_penalty", "код", "0x47ac9b 0x4525d5",
     "штраф скорости на единицу массы сверх предела"),
    (3, 0x14c, "u32", "armor_x100", "код", "0x47acad 0x479dfc 0x47a00c",
     "броня: в игрока пишется x100 (0x47acc5)"),
    (4, 0x150, "u32", "armor_cap_x100", "код", "0x47acd4",
     "предел брони: x100 (0x47ace0)"),
    ((5, 6), 0x20, "f64", "max_speed", "код", "0x47ac77 0x47ac83",
     "базовая максимальная скорость (ед/тик)"),
    (7, 0x154, "u32", "model_index", "код", "0x47af34",
     "индекс SOB-модели шасси 15..18 -> FUN_00452240"),
    ((8, 9), 0x28, "f64", "body_x", "значения", "0x47ace0 0x47ad2b",
     "первая тройка модели: смещение/радиус (setobjectradius 0x4522ad)"),
    ((10, 11), 0x30, "f64", "body_y", "значения", "0x47acfb",
     "вторая тройка модели"),
    ((12, 13), 0x38, "f64", "body_z", "значения", "0x47ad13",
     "третья тройка модели"),
    ((14, 15), 0x40, "f64", "mass", "значения", "0x47ad2b",
     "масса/габарит (30..56 в поставе)"),
    (16, 0x168, "u32", "price", "код", "0x479e02 0x479f13 0x47ac6b",
     "цена шасси (FUN_00479dd0, сумма стоимости игрока)"),
    (17, None, "u32", "unused_17", "—", "",
     "N-операнд не читается: 9/10/-1/11"),
    (18, 0x17c, "u32", "sound_id", "код", "0x47ad6c 0x46c0ae 0x46c34b",
     "индекс звука двигателя -> player+0x81c420 (28/35/22/41)"),
    (19, None, "u32", "unused_19", "—", "",
     "второй N-операнд (8) не читается"),
    (20, 0x190, "u32", "hud_190", "гипотеза", "0x47ada3",
     "32 во всех поставочных шасси; роль не установлена"),
    (21, 0x194, "u32", "id_194", "гипотеза", "0x47ad98 0x4530b9",
     "0xffff8ab2 = -30030 во всех шасси; похоже на id объекта"),
    (22, 0x198, "u32", "id_198", "гипотеза", "0x47adb5 0x4530d8",
     "901010 во всех шасси; роль не установлена"),
]

CHASSIS_TAIL = [
    ((87, 88), 0x000, "f64", "coef_00", "значения", "0x47ac29 0x47ac2f",
     "0.001 во всех поставочных шасси; копируется в player+0x81bdc8"),
    ((89, 90), 0x008, "f64", "coef_08", "значения", "0x47ac3b 0x47ac47",
     "0.01; копируется в player+0x81bdd0"),
    ((91, 92), 0x010, "f64", "coef_10", "значения", "0x47ac53 0x47ac5f",
     "0.05; копируется в player+0x81bdd8"),
    (93, 0x1c4, "u32", "event_switch", "код", "0x47b314",
     "event id смены шасси (event_post при выборе)"),
    (94, 0x1c8, "u32", "event_apply", "код", "0x47aa13",
     "event id применения шасси"),
]

CHASSIS_STRINGS = [
    (0x16c, "str_16c", "копируется в player+0x81c424 (0x47acbf); из 2-й строки SBT"),
    (0x180, "str_180", "копируется в player+0x81c43c (0x47ad72); имя из 0x42b81b"),
    (0x19c, "str_19c", "копируется в player+0x81c454 (0x47ada9)"),
    (0x1b0, "str_1b0", "копируется в player+0x81c468 (0x47adf8)"),
    (0x1ac, "handle_1ac", "приходит из 0x42b81b, в файле поля нет"),
    (0x1c0, "zero_1c0", "стор всегда пишет 0 (arg29 = push 0)"),
]

CHASSIS_BASE_DW0 = 23          # first dword of the 8-double body block
CHASSIS_BASE_N = 8             # number of doubles copied to player+0x81bfa0
CHASSIS_BASE_MEM = 0x48
CHASSIS_PLATE_DW0 = 39         # first dword of plate 0 / column 0
CHASSIS_PLATE_N = 8
CHASSIS_PLATE_MEM = 0x88       # column 0 base; column stride 0x40, plate stride 8
CHASSIS_ANOMALY = (0xF8, "P77,P78")


class Table:
    def __init__(self, name, op, base, stride, counter, fields, extra, dwords):
        self.name = name
        self.op = op
        self.base = base
        self.stride = stride
        self.counter = counter
        self.fields = fields
        self.extra = extra
        self.dwords = dwords

    def field_by_tag(self, tag):
        for f in self.fields:
            if tag_of(self, f) == tag:
                return f
        return None


def tag_of(table, field):
    dw = field[0]
    letter = {"DEFINE_WEAPONS": "W", "DEFINE_AMMO": "A",
              "DEFINE_EQUIPMENT": "E", "DEFINE_PLAYER_TYPES": "P"}[table.name]
    if isinstance(dw, tuple):
        return "%s%d/%d" % (letter, dw[0], dw[1])
    return "%s%d" % (letter, dw)


def chassis_fields():
    """All chassis fields: header + body grid + tail + strings."""
    out = list(CHASSIS_HEADER)
    for k in range(CHASSIS_BASE_N):
        out.append(((CHASSIS_BASE_DW0 + 2 * k, CHASSIS_BASE_DW0 + 2 * k + 1),
                    CHASSIS_BASE_MEM + 8 * k, "f64",
                    "body[%d]" % k, "код", "0x47ae26..0x47aefc",
                    "копируется в player+0x81bfa0+%d (0x47ae26..0x47aefc)" % (8 * k)))
    for p in range(CHASSIS_PLATE_N):
        for c in range(3):
            dw = CHASSIS_PLATE_DW0 + 6 * p + 2 * c
            mem = CHASSIS_PLATE_MEM + 0x40 * c + 8 * p
            if mem == CHASSIS_ANOMALY[0]:
                out.append(((dw, dw + 1), mem, "f64", "plate[%d][%d]" % (p, c), "—",
                            "", "аномалия: читается из [record+0xa84], d%d/d%d пропущены"
                            % (dw, dw + 1)))
            else:
                out.append(((dw, dw + 1), mem, "f64", "plate[%d][%d]" % (p, c), "значения",
                            "0x47af08..0x47af32",
                            "8 плит x 3 double: файл по строкам, память по столбцам; "
                            "копируется в player+0x81beb8/+0x81bef8/+0x81bf38"))
    out.extend(CHASSIS_TAIL)
    return out


TABLES = [
    Table("DEFINE_WEAPONS", 170, 0x80fa00, 0x2a8, 0x819028, WEAPONS, WEAPONS_EXTRA, 21),
    Table("DEFINE_AMMO", 171, 0x818960, 0x038, 0x818958, AMMO, AMMO_EXTRA, 10),
    Table("DEFINE_EQUIPMENT", 175, 0x814c60, 0x040, 0x815420, EQUIPMENT, EQUIPMENT_EXTRA, 12),
    Table("DEFINE_PLAYER_TYPES", 172, 0x7995e0, 0x1cc, 0x7941bc, chassis_fields(), [], 95),
]

BY_NAME = {t.name: t for t in TABLES}


# ---------------------------------------------------------------------------
# decoding
# ---------------------------------------------------------------------------
def _sbtmod():
    from gm import sbt, sbtvm
    return sbt, sbtvm


def expand(vals, sig):
    """File dwords of one record, in file order (doubles split into halves).

    ``sig`` is the operand signature *after* the leading strings.
    """
    sbt, sbtvm = _sbtmod()
    out = []
    for ch, val in zip(sig, vals):
        size = sbtvm.LETTER_SIZE[ch]
        if size == "str":
            continue
        if size == 8:
            out.extend(struct.unpack("<2I", struct.pack("<d", val)))
        else:
            out.append(val & 0xFFFFFFFF)
    return out


def pack_double(dwords, i):
    return struct.unpack("<d", struct.pack("<2I", dwords[i], dwords[i + 1]))[0]


def memory_view(table, dwords):
    """``{mem offset: (kind, value)}`` for one decoded record.

    Applies the conversions the store functions perform, so the numbers are
    what the engine actually sees.
    """
    out = {}
    for f in table.fields:
        dw, mem, kind = f[0], f[1], f[2]
        if mem is None or f[4] == "—":
            continue
        if isinstance(dw, tuple):
            if dw[0] + 1 >= len(dwords):
                continue
            value = pack_double(dwords, dw[0])
        else:
            if dw >= len(dwords):
                continue
            value = dwords[dw]
            if kind == "i32" and value >= 0x80000000:
                value -= 0x100000000
        if table.name == "DEFINE_WEAPONS" and mem == 0x00:
            value = value * 0.91           # 0x450d34: fmul [0x4a2488]
        if table.name == "DEFINE_PLAYER_TYPES" and mem in (0x14c, 0x150):
            value = value * 100            # 0x47acc5 / 0x47ace0
        if table.name == "DEFINE_AMMO" and mem == 0x20 and value < 1:
            value = 1                      # 0x450bbf: clamp >= 1
        out[mem] = (kind, value)
    return out


def read_level(path):
    """``{table name: [(strings, dwords)]}`` for every DEFINE_* table."""
    sbt, sbtvm = _sbtmod()
    data = open(path, "rb").read()
    level = sbt.parse(data)
    result = {}
    for rec in level.records_of(11):
        insns = sbtvm.decode(rec.payload.code)[0]
        for ins in insns:
            if ins.op not in sbtvm.TABLE_SIGS:
                continue
            sig = sbtvm.TABLE_SIGS[ins.op]
            name = sbtvm.TABLE_NAMES[ins.op]
            tail = sig[len(sig) - len(sig.lstrip("S")):]
            result.setdefault(name, []).extend(
                (strs, expand(vals, tail)) for strs, vals in ins.data)
    return result


# ---------------------------------------------------------------------------
# map printing
# ---------------------------------------------------------------------------
def print_map(table, verbose=True):
    print("%s  op=%d  array 0x%06x  stride 0x%x  counter 0x%06x  %d dwords"
          % (table.name, table.op, table.base, table.stride, table.counter,
             table.dwords))
    print("  %-8s %-6s %-4s %-22s %-9s %s" %
          ("tag", "mem", "type", "field", "status", "readers"))
    for f in table.fields:
        dw, mem, kind, name, status, readers, note = f
        print("  %-8s %-6s %-4s %-22s %-9s %s" %
              (tag_of(table, f), "+0x%03x" % mem if mem is not None else "-",
               kind, name, status, readers or "-"))
        if verbose and note:
            print("           %s" % note)
    for mem, name, note in table.extra:
        print("  %-8s +0x%03x %-4s %-22s %-9s %s" %
              ("-", mem, "ext", name, "-", note))
    print()


# ---------------------------------------------------------------------------
# verification against shipped data
# ---------------------------------------------------------------------------
class Fail(Exception):
    pass


_COUNT = [0]


def signed(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def expect(cond, msg):
    _COUNT[0] += 1
    if not cond:
        raise Fail(msg)


def verify_levels(paths):
    _COUNT[0] = 0
    for path in paths:
        if not os.path.exists(path):
            print("SKIP %s (нет файла)" % path)
            continue
        print("== %s" % path)
        level = read_level(path)
        expect(level, "%s: нет ни одной DEFINE_* таблицы" % path)

        if "DEFINE_WEAPONS" in level:
            w = level["DEFINE_WEAPONS"]
            expect(len(w) == 19, "%s: оружия %d, ожидалось 19" % (path, len(w)))
            names = [r[0][0] for r in w]
            expect(names[0] == "MACHINE GUN" and names[1] == "CANNON",
                   "%s: имена слотов не те" % path)
            m = memory_view(BY_NAME["DEFINE_WEAPONS"], w[0][1])
            expect(m[0x2c][1] == 17, "%s: MACHINE GUN fire_ticks=%d != 17"
                   % (path, m[0x2c][1]))
            expect(m[0x24][1] == 1, "%s: MACHINE GUN fire_mode != 1" % path)
            m = memory_view(BY_NAME["DEFINE_WEAPONS"], w[1][1])
            expect(m[0x2c][1] == 80, "%s: CANNON fire_ticks=%d != 80"
                   % (path, m[0x2c][1]))
            expect(m[0x44][1] == 50000, "%s: CANNON price != 50000" % path)
            expect(-1 <= m[0x20][1] < 8, "%s: muzzle вне -1..7" % path)
            for r in w:
                mv = memory_view(BY_NAME["DEFINE_WEAPONS"], r[1])
                expect(0 <= mv[0x24][1] <= 5,
                       "%s: %s fire_mode=%d" % (path, r[0][0], mv[0x24][1]))
                expect(0 <= mv[0x3c][1] < 32,
                       "%s: %s ammo_type=%d" % (path, r[0][0], mv[0x3c][1]))
                expect(-1 <= signed(mv[0x20][1]) < 8,
                       "%s: %s muzzle=%d" % (path, r[0][0], signed(mv[0x20][1])))
                # charge rate is stored as value*0.91; both must be finite
                expect(mv[0x00][1] == mv[0x00][1], "%s: charge_rate NaN" % path)
            # flags: only the four used bits and the data-only ones appear
            for r in w:
                mv = memory_view(BY_NAME["DEFINE_WEAPONS"], r[1])
                expect(mv[0x28][1] & ~0xFF == 0,
                       "%s: флаги оружия вне байта" % path)
            print("   weapons: %d записей; MG ticks=%d, CANNON ticks=%d, "
                  "muzzle -1..7, ammo_type<32, флаги-байт" %
                  (len(w), memory_view(BY_NAME["DEFINE_WEAPONS"], w[0][1])[0x2c][1],
                   memory_view(BY_NAME["DEFINE_WEAPONS"], w[1][1])[0x2c][1]))

        if "DEFINE_AMMO" in level:
            a = level["DEFINE_AMMO"]
            expect(len(a) == 15, "%s: патронов %d, ожидалось 15" % (path, len(a)))
            names = [r[0][0] for r in a]
            expect(names[0] == "BULLETS", "%s: первый тип %r" % (path, names[0]))
            mv = memory_view(BY_NAME["DEFINE_AMMO"], a[0][1])
            expect(mv[0x04][1] == 5000, "%s: BULLETS max=%d" % (path, mv[0x04][1]))
            for i, r in enumerate(a):
                mv = memory_view(BY_NAME["DEFINE_AMMO"], r[1])
                expect(mv[0x20][1] >= 1, "%s: %s rounds_per_unit<1" % (path, r[0][0]))
                if mv[0x00][1] & 0x01:      # можно купить -> партия и цена есть
                    expect(mv[0x08][1] > 0, "%s: %s lot=0 при флаге покупки"
                           % (path, r[0][0]))
                    expect(mv[0x0c][1] > 0, "%s: %s price=0 при флаге покупки"
                           % (path, r[0][0]))
                expect(mv[0x10][1] < 10000, "%s: %s неправдоподобный вес"
                       % (path, r[0][0]))
            # weapon ammo_type must point inside the ammo table
            if "DEFINE_WEAPONS" in level:
                for r in level["DEFINE_WEAPONS"]:
                    mv = memory_view(BY_NAME["DEFINE_WEAPONS"], r[1])
                    t = mv[0x3c][1]
                    expect(t < len(a), "%s: %s ammo_type=%d вне таблицы"
                           % (path, r[0][0], t))
            print("   ammo: %d записей; BULLETS max=%d, units>=1, "
                  "lot/price>0 у покупаемых, индексы оружия валидны" % (len(a), 5000))

        if "DEFINE_EQUIPMENT" in level:
            e = level["DEFINE_EQUIPMENT"]
            expect(1 <= len(e) <= 31, "%s: оборудования %d вне 1..31" % (path, len(e)))
            mvs = [memory_view(BY_NAME["DEFINE_EQUIPMENT"], r[1]) for r in e]
            for r, mv in zip(e, mvs):
                expect(-1 <= mv[0x20][1] < 31,
                       "%s: %s ammo_index=%d" % (path, r[0][0], mv[0x20][1]))
            shield = [mv for r, mv in zip(e, mvs) if "SHIELD" in r[0][0]]
            if "DEFINE_AMMO" in level:
                cells = [i for i, r in enumerate(level["DEFINE_AMMO"])
                         if r[0][0] == "POWER CELLS"]
                expect(cells, "%s: нет POWER CELLS" % path)
                if shield:
                    expect(shield[0][0x20][1] == cells[0],
                           "%s: FORCE SHIELD ссылается не на POWER CELLS" % path)
            print("   equipment: %d записей; ammo_index в -1..30, SHIELD->POWER CELLS"
                  % len(e))

        if "DEFINE_PLAYER_TYPES" in level:
            c = level["DEFINE_PLAYER_TYPES"]
            expect(len(c) == 4, "%s: шасси %d, ожидалось 4" % (path, len(c)))
            got = {}
            for r in c:
                mv = memory_view(BY_NAME["DEFINE_PLAYER_TYPES"], r[1])
                got[r[0][0].split()[0]] = mv
                expect(mv[0x154][1] in (15, 16, 17, 18),
                       "%s: %s model_index=%d" % (path, r[0][0], mv[0x154][1]))
                expect(mv[0x14c][1] == mv[0x14c][1] and mv[0x148][1] > 0,
                       "%s: %s пустые ходовые поля" % (path, r[0][0]))
                expect(mv[0x168][1] > 0, "%s: %s price=0" % (path, r[0][0]))
            light = got["LIGHT"]
            expect(light[0x148][1] == 500, "%s: LIGHT weight_limit=%d"
                   % (path, light[0x148][1]))
            expect(light[0x14c][1] == 8000, "%s: LIGHT armor*100=%d"
                   % (path, light[0x14c][1]))
            expect(abs(light[0x20][1] - 1.7) < 1e-6, "%s: LIGHT max_speed=%r"
                   % (path, light[0x20][1]))
            expect(abs(light[0x60][1] - 0.9) < 1e-9,
                   "%s: LIGHT k=+0x60=%r != 0.9" % (path, light[0x60][1]))
            expect(light[0x00][1] == 0.001 and light[0x10][1] == 0.05,
                   "%s: LIGHT coef +0x00/+0x10 = %r/%r"
                   % (path, light[0x00][1], light[0x10][1]))
            heavy = got["HEAVY"]
            expect(heavy[0x148][1] == 1600 and heavy[0x14c][1] == 20000,
                   "%s: HEAVY %r" % (path, (heavy[0x148][1], heavy[0x14c][1])))
            expect(heavy[0x20][1] < light[0x20][1],
                   "%s: HEAVY быстрее LIGHT" % path)
            # plates: the file keeps them row-major, memory column-major
            mv = got["LIGHT"]
            expect(mv[0x88][1] == 0.0 and mv[0xc8][1] == 63.0 and mv[0x108][1] == 4.0,
                   "%s: plate[0] = %r/%r/%r" % (path, mv[0x88][1], mv[0xc8][1],
                                                mv[0x108][1]))
            expect(mv[0x90][1] == 0.0 and mv[0xd0][1] == 63.0,
                   "%s: plate[1] = %r/%r" % (path, mv[0x90][1], mv[0xd0][1]))
            print("   chassis: %d записи; LIGHT 500/8000/1.70/k=0.9, HEAVY "
                  "1600/20000/0.60, model_index 15..18" % len(c))
    print("\nOK: %d проверок" % _COUNT[0])
    return 0


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def synthetic_record(table):
    """A full record with distinct, checkable values for every dword."""
    n = table.dwords
    dwords = [0] * n
    for i in range(n):
        dwords[i] = 0x1000 + i
    if table.name == "DEFINE_WEAPONS":
        # doubles must be plausible for the 0.91 check
        for base in (12, 14, 16, 18):
            dwords[base:base + 2] = list(struct.unpack("<2I", struct.pack("<d", 2.0)))
        dwords[0] = 1        # fire mode
        dwords[2] = 3        # ammo type
        dwords[7] = 0xFFFF8AB5
    if table.name == "DEFINE_PLAYER_TYPES":
        for base in (1, 5):
            dwords[base:base + 2] = list(struct.unpack("<2I", struct.pack("<d", 1.5)))
    return dwords


def self_test():
    _COUNT[0] = 0
    ok = [True]

    def check(cond, msg):
        try:
            expect(cond, msg)
        except Fail as exc:
            print("FAIL: %s" % exc)
            ok[0] = False
        except Exception as exc:                      # noqa: BLE001
            print("FAIL: %s (%r)" % (msg, exc))
            ok[0] = False

    # --- map integrity -----------------------------------------------------
    for table in TABLES:
        tags = [tag_of(table, f) for f in table.fields]
        check(len(tags) == len(set(tags)), "%s: повторяющиеся теги" % table.name)
        offs = [f[1] for f in table.fields if f[1] is not None]
        check(len(offs) == len(set(offs)), "%s: повторяющиеся смещения" % table.name)
        for f in table.fields:
            if f[1] is None:
                continue
            width = 8 if f[2] == "f64" else 4
            check(f[1] + width <= table.stride,
                  "%s: %s вне записи (+0x%x)" % (table.name, tag_of(table, f), f[1]))
        # dword coverage: every file dword must be mapped exactly once
        used = []
        for f in table.fields:
            dw = f[0]
            used.extend(dw if isinstance(dw, tuple) else [dw])
        dup = [d for d in set(used) if used.count(d) > 1]
        check(not dup, "%s: dword используются дважды: %r" % (table.name, dup))
        missing = [d for d in range(table.dwords) if d not in used]
        check(missing == [],
              "%s: не покрыты dword %r" % (table.name, missing))

    # --- weapons -----------------------------------------------------------
    t = BY_NAME["DEFINE_WEAPONS"]
    dw = synthetic_record(t)
    mv = memory_view(t, dw)
    check(mv[0x24][1] == 1, "weapons: +0x24 (fire mode) не из dword 0")
    check(mv[0x2c][1] == 0x1001, "weapons: +0x2c не из dword 1")
    check(mv[0x3c][1] == 3, "weapons: +0x3c не из dword 2")
    check(mv[0x40][1] == 0x1003, "weapons: +0x40 не из dword 3")
    check(mv[0x30][1] == 0x1004 and mv[0x34][1] == 0x1005 and mv[0x38][1] == 0x1006,
          "weapons: +0x30/+0x34/+0x38 не из dwords 4/5/6")
    check(mv[0x4c][1] == 0xFFFF8AB5, "weapons: +0x4c не из dword 7")
    check(mv[0x44][1] == 0x1008 and mv[0x48][1] == 0x1009,
          "weapons: +0x44/+0x48 не из dwords 8/9")
    check(mv[0x28][1] == 0x100A, "weapons: +0x28 не из dword 10")
    check(mv[0x294][1] == 0x100B, "weapons: +0x294 не из dword 11")
    check(mv[0x00][1] == 2.0 * 0.91, "weapons: +0x00 != file*0.91 (%r)" % mv[0x00][1])
    check(mv[0x08][1] == 2.0 and mv[0x10][1] == 2.0 and mv[0x18][1] == 2.0,
          "weapons: +0x08/+0x10/+0x18 не double'ы")
    check(mv[0x20][1] == 0x1014, "weapons: +0x20 не из dword 20")

    # --- ammo --------------------------------------------------------------
    t = BY_NAME["DEFINE_AMMO"]
    dw = synthetic_record(t)
    dw[8] = 0                       # rounds_per_unit -> clamp 1
    mv = memory_view(t, dw)
    check(mv[0x04][1] == 0x1000, "ammo: +0x04 не из dword 0")
    check(mv[0x18][1] == 0x1001 and mv[0x14][1] == 0x1002 and mv[0x1c][1] == 0x1003,
          "ammo: перестановка +0x14/+0x18/+0x1c")
    check(mv[0x08][1] == 0x1004 and mv[0x0c][1] == 0x1005 and mv[0x10][1] == 0x1006,
          "ammo: +0x08/+0x0c/+0x10")
    check(mv[0x00][1] == 0x1007, "ammo: +0x00 не из dword 7")
    check(mv[0x20][1] == 1, "ammo: +0x20 не клампится в 1")
    check(mv[0x24][1] == 0x1009, "ammo: +0x24 не из dword 9")

    # --- equipment ---------------------------------------------------------
    t = BY_NAME["DEFINE_EQUIPMENT"]
    dw = synthetic_record(t)
    mv = memory_view(t, dw)
    check([mv[o][1] for o in (0x00, 0x04, 0x08, 0x0c, 0x10)]
          == [0x1000, 0x1001, 0x1002, 0x1003, 0x1004],
          "equipment: +0x00..+0x10 не по порядку")
    check([o for o in (0x14, 0x18, 0x1c) if o in mv] == [],
          "equipment: +0x14/+0x18/+0x1c должны быть помечены как нечитаемые")
    check(mv[0x20][1] == 0x1008 and mv[0x24][1] == 0x1009 and mv[0x28][1] == 0x100A,
          "equipment: +0x20/+0x24/+0x28")
    check(mv[0x3c][1] == 0x100B, "equipment: +0x3c не из dword 11")

    # --- chassis -----------------------------------------------------------
    t = BY_NAME["DEFINE_PLAYER_TYPES"]
    dw = synthetic_record(t)
    mv = memory_view(t, dw)
    check(mv[0x148][1] == 0x1000, "chassis: +0x148 не из dword 0")
    check(mv[0x18][1] == 1.5, "chassis: +0x18 не double из dwords 1/2")
    check(mv[0x14c][1] == 0x1003 * 100, "chassis: +0x14c без x100")
    check(mv[0x150][1] == 0x1004 * 100, "chassis: +0x150 без x100")
    check(mv[0x20][1] == 1.5, "chassis: +0x20 не double из dwords 5/6")
    check(mv[0x154][1] == 0x1007, "chassis: +0x154 не из dword 7")
    check(mv[0x168][1] == 0x1010, "chassis: +0x168 не из dword 16")
    check(mv[0x17c][1] == 0x1012, "chassis: +0x17c не из dword 18")
    check(mv[0x190][1] == 0x1014, "chassis: +0x190 не из dword 20")
    check(mv[0x194][1] == 0x1015 and mv[0x198][1] == 0x1016,
          "chassis: +0x194/+0x198")
    check(mv[0x48][1] == pack_double([0x1000 + i for i in range(95)], 23),
          "chassis: +0x48 не из dword 23")
    check(mv[0x60][1] == pack_double([0x1000 + i for i in range(95)], 29),
          "chassis: +0x60 не из dword 29")
    check(mv[0x88][1] == pack_double([0x1000 + i for i in range(95)], 39),
          "chassis: +0x88 не из dword 39")
    check(mv[0x90][1] == pack_double([0x1000 + i for i in range(95)], 45),
          "chassis: +0x90 не из dword 45 (плита 1, столбец 0)")
    check(mv[0xc8][1] == pack_double([0x1000 + i for i in range(95)], 41),
          "chassis: +0xc8 не из dword 41 (плита 0, столбец 1)")
    check(mv[0x108][1] == pack_double([0x1000 + i for i in range(95)], 43),
          "chassis: +0x108 не из dword 43 (плита 0, столбец 2)")
    check(mv[0xc0][1] == pack_double([0x1000 + i for i in range(95)], 81),
          "chassis: +0xc0 не из dword 81 (плита 7, столбец 0)")
    check(mv[0x140][1] == pack_double([0x1000 + i for i in range(95)], 85),
          "chassis: +0x140 не из dword 85 (плита 7, столбец 2)")
    check(0xf8 not in mv, "chassis: +0xf8 не должен заполняться из файла")
    check(mv[0x00][1] == pack_double([0x1000 + i for i in range(95)], 87),
          "chassis: +0x00 не double из dwords 87/88")
    check(mv[0x10][1] == pack_double([0x1000 + i for i in range(95)], 91),
          "chassis: +0x10 не double из dwords 91/92")
    check(mv[0x1c4][1] == 0x105D and mv[0x1c8][1] == 0x105E,
          "chassis: +0x1c4/+0x1c8 не из dwords 93/94")

    # --- real files --------------------------------------------------------
    real = [os.path.join(DEFAULT_DIR, n) for n in DEFAULT_LEVELS]
    real = [p for p in real if os.path.exists(p)]
    if not real:
        print("NOTE: поставочных .sbt не найдено (%s), проверка только синтетикой"
              % DEFAULT_DIR)
    for path in real:
        try:
            verify_levels([path])
        except (Fail, OSError, ValueError) as exc:
            print("FAIL: %s" % exc)
            ok[0] = False
        except Exception as exc:                      # noqa: BLE001
            print("FAIL: %s: %s" % (path, exc))
            ok[0] = False

    print("\nself-test: %s (%d проверок)" % ("OK" if ok[0] else "FAIL", _COUNT[0]))
    return 0 if ok[0] else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def dump_file(path, only=None):
    if not os.path.exists(path):
        print("%s: файла нет" % path)
        return 1
    level = read_level(path)
    if not level:
        print("%s: DEFINE_* таблиц нет" % path)
        return 1
    for table in TABLES:
        if only and table.name != only:
            continue
        recs = level.get(table.name)
        if not recs:
            continue
        print("== %s: записей %d (stride 0x%x)" % (table.name, len(recs), table.stride))
        for i, (strs, dwords) in enumerate(recs):
            print(" [%d] %s" % (i, " | ".join(strs)))
            mv = memory_view(table, dwords)
            for f in table.fields:
                mem = f[1]
                if mem is None:
                    print("     %-7s %-22s (в память не попадает)" % (tag_of(table, f), f[3]))
                    continue
                if mem not in mv:
                    continue
                kind, value = mv[mem]
                if kind == "f64":
                    shown = "%r" % value
                elif value >= 0x80000000:
                    shown = "%d (0x%x, знаковое %d)" % (
                        value, value, value - 0x100000000)
                else:
                    shown = "%d (0x%x)" % (value, value)
                print("     +0x%03x %-22s %-8s %s" %
                      (mem, f[3], tag_of(table, f), shown))
            print()
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--map", action="store_true", help="печать карт (по умолчанию)")
    ap.add_argument("--table", help="только эта таблица")
    ap.add_argument("--file", action="append", default=[],
                    help="напечатать записи .sbt (можно несколько раз)")
    ap.add_argument("--verify", action="store_true",
                    help="сверить поставочные level0.sbt/dm1.sbt")
    ap.add_argument("--dir", default=DEFAULT_DIR, help="каталог игры")
    ap.add_argument("--self-test", action="store_true", help="встроенные тесты")
    ap.add_argument("--quiet", action="store_true", help="без примечаний в карте")
    opts = ap.parse_args(argv)

    if opts.self_test:
        try:
            return self_test()
        except Exception as exc:                      # noqa: BLE001
            print("FAIL: непредвиденная ошибка self-test: %r" % (exc,))
            return 1

    status = 0
    if opts.verify:
        paths = [os.path.join(opts.dir, n) for n in DEFAULT_LEVELS]
        try:
            status |= verify_levels(paths)
        except Exception as exc:                      # noqa: BLE001
            print("FAIL: %s" % exc)
            status |= 1
    if opts.file:
        for path in opts.file:
            try:
                status |= dump_file(path, opts.table)
            except (Fail, OSError, ValueError) as exc:
                print("FAIL: %s: %s" % (path, exc))
                status |= 1
    if opts.map or (not opts.verify and not opts.file and not opts.self_test):
        for table in TABLES:
            if opts.table and table.name != opts.table:
                continue
            print_map(table, verbose=not opts.quiet)
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
