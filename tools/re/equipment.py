#!/usr/bin/env python3
"""Equipment model: ``DEFINE_EQUIPMENT`` table + runtime state + effects.

The level script (``DEFINE_EQUIPMENT``, opcode 175) fills a fixed array of
``0x40``-byte records at ``0x814c60``.  The *record* is static data; the
per-player *state* lives in the player block (``0x819040 + i*0x3510``) and the
engine never changes the "active" bit from input directly -- input posts an
event, the level sub-script calls ``SOURCEPLAYER_EQUIPMENT_ACTIVATE`` (opcode
130), and that is what flips the bit.  This module carries the resulting model
and checks it against the shipped data + the executable bytes.

Layout (all proven by the store function ``0x450e60`` and its callers, see
``docs/EQUIPMENT.md``)::

    DEFINE_EQUIPMENT   175  "SLEEEEEELLLLL"  array 0x814c60  stride 0x40
    count              0x815420              max 31 records (0x450e65)
    name               +0x2c  char[16]       (copied to 0x814c8c + n*0x40)
    E0..E11            file dwords 0..11     -> +0x00..+0x28, +0x3c

Player state (``docs/PLAYER_FIELDS.md`` calls some of it "fire_anim"/unknown)::

    +0x3320  i32  equipment_index      current selection, -1 = none
    +0x3324  u32  equipment_owned      bit i = record i owned
    +0x3328  u32  equipment_active     bit i = record i active
    +0x332c  i32  equipment_cost_total cash paid for the current fit
    +0x3330  i32  equipment_refit_base respawn/refit accounting
    +0x3344  u32  action_flags         input action bits (see ACTION_BITS)

Usage::

    python3 tools/re/equipment.py --map
    python3 tools/re/equipment.py --stats
    python3 tools/re/equipment.py --json
    python3 tools/re/equipment.py --file level0.sbt --table
    python3 tools/re/equipment.py --file level0.sbt --effects
    python3 tools/re/equipment.py --check
    python3 tools/re/equipment.py --self-test

Only the standard library is used; the SBT/VM codecs come from the repo
(``gm/sbt.py``, ``gm/sbtvm.py``, ``gm/scene.py``).
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

REC_BASE = 0x814C60
REC_STRIDE = 0x40
COUNT_VA = 0x815420
NAME_OFF = 0x2C
MAX_RECORDS = 31          # 0x450e65: `cmp eax,0x1f; jl` -> index 31 rejected
OP_DEFINE = 175

PLAYER_BASE = 0x819040
PLAYER_STRIDE = 0x3510
PLAYER_COUNT = 8

# ---------------------------------------------------------------------------
# field map: (file dword, memory offset, type, name, status, readers, note)
# ---------------------------------------------------------------------------
FIELDS = [
    (0, 0x00, "u32", "flags", "код",
     "0x47b6f5 0x47b798 0x47b7d0 0x47b8ba 0x445ff1 0x4460e3 0x464deb 0x495cfb",
     "0x01 купить, 0x02 продать, 0x04 вне цикла выбора, 0x08 скрыть, 0x80 сеть"),
    (1, 0x04, "u32", "spawn_event", "код", "0x45427f 0x453370 0x47b940",
     "event_post при спавне/подборе предмета этого типа"),
    (2, 0x08, "u32", "activate_event", "код", "0x45a542 0x446099 0x446193",
     "постится при смене индекса (+0x3320); в поставочных данных везде 0"),
    (3, 0x0c, "u32", "event_on", "код", "0x446228 0x446574",
     "постится, когда предмет выключают (нет бита в active) -> скрипт включает"),
    (4, 0x10, "u32", "event_off", "код", "0x44621d 0x446521 0x47b83a 0x45bd96",
     "постится, когда предмет уже активен -> скрипт выключает"),
    (5, 0x14, "u32", "reserved_14", "—", "",
     "читателей в exe нет; во всех 36 уровнях 0"),
    (6, 0x18, "u32", "event_no_ammo", "код", "0x446445 ([edi-8])",
     "постится, когда запас активного предмета дошёл до 0; в данных = event_off"),
    (7, 0x1c, "u32", "ammo_per_tick", "код", "0x4463c0 ([edi-4]), imul 0x4463eb",
     "расход ammo[ammo_type] за тик, пока предмет активен (0 = не тратит)"),
    (8, 0x20, "i32", "ammo_type", "код",
     "0x4463cb 0x4463a4 0x4509e9 0x45e89c 0x492ba3 0x4945d9 0x499a2f",
     "индекс DEFINE_AMMO; -1 = без боеприпаса"),
    (9, 0x24, "u32", "price", "код",
     "0x47b879 0x47b8cd 0x479edc 0x47b0f1 0x464de6 0x495d30 0x47bbb3",
     "списывается/возвращается в cash 0x81c36c"),
    (10, 0x28, "u32", "weight", "код", "0x452576",
     "суммируется в mass_total +0x3334 (0x4524d0)"),
    (11, 0x3c, "u32", "object_id", "код", "0x446488 0x47b0e7",
     "ключ горячей клавиши и поиска: FUN_00446470(object_id, player)"),
]

FIELD_NAMES = [f[3] for f in FIELDS]

#: event ids are signed in the shipped data (e.g. -30043); so is ammo_type
SIGNED_FIELDS = {"spawn_event", "activate_event", "event_on", "event_off",
                 "event_no_ammo"}

# ---------------------------------------------------------------------------
# runtime state: (player offset, address, type, name, status, writers, readers)
# ---------------------------------------------------------------------------
STATE = [
    (0x3320, 0x81C360, "i32", "equipment_index", "доказано",
     "0x45a4ec (op136) 0x4460ab 0x4461a5 (next/prev) 0x45bdd9 (sell all) "
     "0x452da2 (сброс = -1) 0x46721a (загрузка)",
     "0x446053 0x4460cd 0x4461c3 0x492b32 0x492d26 0x4946df 0x494561"),
    (0x3324, 0x81C364, "u32", "equipment_owned", "доказано",
     "0x45bbc8 (op110) 0x45bc08 (op111) 0x45bdcc 0x47b7e1 0x47b8f8 "
     "0x4671a8 (загрузка)",
     "0x445fe6 0x4460d8 0x446119 0x4464aa 0x45e8be 0x464df8 0x47a0e3 "
     "0x495ce4 0x495f73 0x4998fd 0x499a60"),
    (0x3328, 0x81C368, "u32", "equipment_active", "доказано",
     "0x45a858/0x45a881 (op130) 0x45bd60 0x45bdd2 0x47b863 0x4671b4 (загрузка)",
     "0x4461da 0x4463a9 0x4464db 0x45bd48 0x491a13 0x491a53 0x491b8e "
     "0x492003 0x492d2e 0x4946ec 0x494b0c 0x499a96"),
    (0x332c, 0x81C36C, "i32", "equipment_cost_total", "доказано",
     "0x452e61 0x452eaa 0x452ec3 0x452ede 0x452f0f 0x452f2b 0x45bc77 "
     "0x45bcb0 0x45bce9 0x4671c0",
     "0x45e572 0x464d56 0x47b2a1 0x47b4fe 0x47b562 0x47b59d 0x47b881"),
    (0x3330, 0x81C370, "i32", "equipment_refit_base", "гипотеза",
     "0x452f85",
     "0x4422c7 0x452e4f 0x459e18 0x466dc9 0x4995ab"),
    (0x3344, 0x81C384, "u32", "action_flags", "доказано",
     "ввод игрока (per-frame action word; ср. 0x4459e7, бит 0x20 — выстрел)",
     "0x445fa0..0x446372 (FUN_00445fa0), 0x443ac8 0x443b9e 0x445450"),
]

#: input action bits consumed by ``FUN_00445fa0``; ``0x40000 << object_id``
ACTION_BITS = [
    (0x10000, "next", "следующий доступный предмет (owned & !(flags&4)), пост E2"),
    (0x20000, "prev", "предыдущий доступный предмет, пост E2"),
    (0x40000, "toggle", "переключить текущий индекс: E3 если не активен, иначе E4"),
]
for _oid, _name in enumerate([
        "HEADLIGHTS", "FORCE SHIELD", "MINE DETECTOR", "TARGETING",
        "WARP DEVICE", "RADAR", "MINE LAYER", "COMBAT BLOCK",
        "AUTO SENTRY", "DECOY"], start=1):
    ACTION_BITS.append((0x40000 << _oid, "hotkey %d" % _oid,
                        "%s: FUN_00446470(%d, player) -> E3/E4" % (_name, _oid)))

# ---------------------------------------------------------------------------
# VM opcodes that touch equipment
# ---------------------------------------------------------------------------
VM_OPS = [
    (110, "AND_SOURCEPLAYER_EQUIPMENT_BITS", "L", 0x45BAAA,
     "owned &= arg; пересчёт массы 0x4524d0"),
    (111, "OR_SOURCEPLAYER_EQUIPMENT_BITS", "L", 0x45BADB,
     "owned |= arg; пересчёт массы 0x4524d0"),
    (129, "SOURCEPLAYER_HEADLIGHT", "L", 0x45AC91,
     "player flags (0x81c240) |=/&=~ 0x4; не трогает маску экипировки"),
    (130, "SOURCEPLAYER_EQUIPMENT_ACTIVATE", "LL", 0x45A816,
     "active (0x81c368) |= / &=~ (1<<bit); bit 0..31, игрок 0..7"),
    (136, "SOURCEPLAYER_EQUIPMENT_INDEX", "L", 0x45A4BE,
     "+0x3320 = arg (0..count-1), затем event_post(record+0x08 = E2)"),
]
VM_OPS_BY_ID = {op[0]: op for op in VM_OPS}

#: where the effect opcodes actually land (proven by the handler bytes below)
EFFECT_ADDRS = [
    (0x81C240, "player flags", "0x4 HEADLIGHT (op129), 0x100 DETECTOR (op133), "
     "0x200 TARGETING (op134), 0x40000 TARGETED (op135); якорь потребителя "
     "есть только у 0x4 — рендер 0x447d7c"),
    (0x81BFF0, "damage modifier", "op132: две пары double; умножаются в 0x453c13/"
     "0x453c24 (урон 0x453b50)"),
    (0x68CD9C, "HUD equip #6 state", "байт 0/1/2; см. STATE и 0x4919f0"),
]

# ---------------------------------------------------------------------------
# byte anchors in gundx.exe: (vma, hex, what)
# ---------------------------------------------------------------------------
ANCHORS = [
    # store function: file dword -> memory offset (the whole field map)
    (0x450E65, "83 f8 1f", "DEFINE_EQUIPMENT: count >= 31 отвергается (31 запись)"),
    (0x450E87, "05 8c 4c 81 00", "имя: 0x814c8c = база + 0x2c"),
    (0x450EAD, "89 88 60 4c 81 00", "E0 -> +0x00"),
    (0x450EB7, "89 90 64 4c 81 00", "E1 -> +0x04"),
    (0x450EC1, "89 88 68 4c 81 00", "E2 -> +0x08"),
    (0x450ECB, "89 90 6c 4c 81 00", "E3 -> +0x0c"),
    (0x450ED5, "89 88 70 4c 81 00", "E4 -> +0x10"),
    (0x450EDF, "89 90 74 4c 81 00", "E5 -> +0x14"),
    (0x450EE9, "89 88 78 4c 81 00", "E6 -> +0x18"),
    (0x450EF3, "89 90 7c 4c 81 00", "E7 -> +0x1c"),
    (0x450F1F, "89 88 80 4c 81 00", "E8 -> +0x20"),
    (0x450F29, "89 88 84 4c 81 00", "E9 -> +0x24"),
    (0x450F33, "89 88 88 4c 81 00", "E10 -> +0x28"),
    (0x450F3D, "89 88 9c 4c 81 00", "E11 -> +0x3c"),
    # state writes/reads in the player block
    (0x45A4EC, "89 88 60 c3 81 00", "op136: +0x3320 = индекс"),
    (0x45A4F8, "3b 0d 20 54 81 00", "op136: индекс < count (0x815420)"),
    (0x45A542, "8b 89 68 4c 81 00", "op136: post E2 (+0x08) после выбора"),
    (0x45A852, "8d 81 68 c3 81 00", "op130: lea active (+0x3328), OR бита"),
    (0x45A87B, "8d 81 68 c3 81 00", "op130: lea active (+0x3328), AND ~бита"),
    (0x45BAD0, "8d 89 64 c3 81 00", "op110: owned (+0x3324) AND"),
    (0x45BB01, "8d 89 64 c3 81 00", "op111: owned (+0x3324) OR"),
    (0x45BD48, "8b 8e 68 c3 81 00", "sell_all: читает active"),
    (0x45BD60, "89 8e 68 c3 81 00", "sell_all: снимает бит active"),
    (0x45BDCC, "89 86 64 c3 81 00", "sell_all: обнуляет owned"),
    (0x45BDD2, "89 86 68 c3 81 00", "sell_all: обнуляет active"),
    (0x45BDD9, "c7 86 60 c3 81 00 ff ff ff ff", "sell_all: index = -1"),
    (0x47B863, "89 86 68 c3 81 00", "магазин: снимает бит active у проданного"),
    (0x47B879, "8b 88 84 4c 81 00", "магазин: цена E9 (+0x24)"),
    # event post paths
    (0x4461DA, "85 d0", "toggle: active & (1<<index) выбирает E3/E4"),
    (0x44621D, "8b 89 70 4c 81 00", "post E4 (+0x10) когда уже активно"),
    (0x446228, "8b 89 6c 4c 81 00", "post E3 (+0x0c) когда не активно"),
    (0x446488, "be 9c 4c 81 00", "FUN_00446470: поиск по object_id (+0x3c)"),
    (0x4464DB, "85 b0 68 c3 81 00", "FUN_00446470: test active (+0x3328)"),
    (0x4463C0, "8b 57 fc", "тик: E7 ammo_per_tick ([edi-4])"),
    (0x4463CB, "8b 0f", "тик: E8 ammo_type ([edi])"),
    (0x4463EE, "8b 14 85 dc c2 81 00", "тик: ammo[] = 0x81c2dc + type*4"),
    (0x446445, "8b 4f f8", "тик: E6 event_no_ammo ([edi-8])"),
    (0x45257B, "8b 9e 64 c3 81 00", "масса: читает owned (+0x3324)"),
    (0x452576, "b8 88 4c 81 00", "масса: E10 weight (+0x28)"),
    (0x45427F, "bb 64 4c 81 00", "спавн предмета: E1 (+0x04)"),
    # input action bits
    (0x445FC0, "a9 00 00 01 00", "action 0x10000 = next equipment"),
    (0x4460B7, "a9 00 00 02 00", "action 0x20000 = prev equipment"),
    (0x4461B1, "a9 00 00 04 00", "action 0x40000 = toggle current"),
    (0x44623D, "a9 00 00 08 00", "action 0x80000 = object_id 1 (HEADLIGHTS)"),
    (0x446355, "a9 00 00 00 08", "action 0x8000000 = object_id 9 (AUTO SENTRY)"),
    (0x446378, "a9 00 00 00 10", "action 0x10000000 = object_id 10 (DECOY)"),
    # HUD / frame consumers
    (0x491A13, "8b 8a 68 c3 81 00", "HUD: active & (1<<[0x6c1c40])"),
    (0x491A22, "88 0d 9c cd 68 00", "HUD: пишет байт 0x68cd9c (0/2)"),
    (0x49CB4B, "c7 05 40 1c 6c 00 06 00 00 00", "0x6c1c40 = 6 (индекс RADAR)"),
    (0x447D7C, "f6 86 40 c2 81 00 04", "рендер: читает флаг фар 0x4 (0x81c240)"),
    (0x45ACBD, "8d 81 40 c2 81 00", "op129: lea player flags 0x81c240"),
    (0x45ACC9, "83 c9 04", "op129: OR 0x4 в player flags"),
    (0x45A0DC, "8b 89 40 c2 81 00", "op133: lea flags, детектор"),
    (0x45A0E2, "80 cd 01", "op133: OR 0x100 (детектор)"),
    (0x45AF54, "8b 89 40 c2 81 00", "op134: lea flags, таргетинг"),
    (0x45AF5A, "80 cd 02", "op134: OR 0x200 (таргетинг)"),
    (0x45A1EC, "81 c9 00 00 04 00", "op135: OR 0x40000 (в меня целятся)"),
    (0x453C13, "dc 8e f0 bf 81 00", "урон умножается на damage modifier 0x81bff0"),
    (0x453C24, "dc 8e f8 bf 81 00", "урон умножается на damage modifier 0x81bff8"),
    (0x492B56, "05 8c 4c 81 00", "HUD: имя = 0x814c8c + index*0x40"),
    (0x492D26, "8b 88 60 c3 81 00", "HUD: индекс из +0x3320 для подсветки"),
    (0x492D2E, "85 90 68 c3 81 00", "HUD: active (+0x3328) -> маркер списка"),
    (0x4946EC, "85 83 68 c3 81 00", "HUD: active (+0x3328) -> иконка"),
]

#: opcodes of the on/off sub-scripts that we surface per equipment
EFFECT_OPS = {
    2: "GENERATE", 20: "PLAY_SAMPLE", 37: "PLAY_SAMPLE_AT_SOURCE",
    51: "ADD_SOURCESCREEN_TEXT", 52: "ADD_ITEM_AT_SOURCE",
    73: "ADD_SOURCEPLAYER_SPRITE", 109: "ADD_SOURCEPLAYER_AMMO",
    116: "CHANGE_SOURCEPLAYER_SOB", 117: "SOURCEPLAYER_TURBO_BOOST",
    120: "SOURCEPLAYER_HAZE", 121: "SOURCEPLAYER_HAZE_BRIGHTNESS",
    128: "SOURCEPLAYER_FLY", 129: "SOURCEPLAYER_HEADLIGHT",
    130: "SOURCEPLAYER_EQUIPMENT_ACTIVATE", 131: "SOURCEPLAYER_FOOT_OFFSETS",
    132: "SOURCEPLAYER_DAMAGE_MODIFIER", 133: "SOURCEPLAYER_DETECTOR",
    134: "SOURCEPLAYER_TARGETING", 135: "SOURCEPLAYER_TARGETED",
    139: "SOURCEPLAYER_KILL", 147: "SOURCEPLAYER_PLAY_LOOP",
    148: "SOURCEPLAYER_STOP_LOOP", 154: "SOURCEPLAYER_DELETE",
}


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
# decoding
# ---------------------------------------------------------------------------
def _sbtmod():
    from gm import sbt, sbtvm
    return sbt, sbtvm


def _scene():
    from gm import scene
    return scene


def record_from(strings, dwords):
    """One record: ``{name, dwords, view}``; raises ``Fail`` on a bad shape."""
    if len(dwords) != len(FIELDS):
        raise Fail("запись %r: %d dword'ов, ожидалось %d"
                   % (strings[0] if strings else "?", len(dwords), len(FIELDS)))
    view = {}
    for dw, off, kind, name, _status, _readers, _note in FIELDS:
        value = dwords[dw]
        if kind == "i32" or name in SIGNED_FIELDS:
            value = s32(value)
        view[name] = value
    return {"name": strings[0] if strings else "",
            "dwords": [v & 0xFFFFFFFF for v in dwords],
            "view": view}


def tables_from_code(code):
    """All ``DEFINE_EQUIPMENT`` instructions in a code block -> list of tables."""
    _sbt, sbtvm = _sbtmod()
    insns, _end = sbtvm.decode(code)
    out = []
    for ins in insns:
        if ins.op != OP_DEFINE:
            continue
        recs = [record_from(strs, vals) for strs, vals in ins.data]
        out.append({"offset": ins.off, "records": recs})
    return out


def read_level(path):
    """``[{offset, records}]`` for every DEFINE_EQUIPMENT in one ``.sbt``."""
    sbt, _sbtvm = _sbtmod()
    level = sbt.parse_file(path)
    scripts = level.records_of(11)
    out = []
    for rec in scripts:
        out.extend(tables_from_code(rec.payload.code))
    return out


def level_script(path):
    sbt, _ = _sbtmod()
    level = sbt.parse_file(path)
    scripts = level.records_of(11)
    return scripts[0].payload if scripts else None


def split_blocks(insns):
    """``{offset: [insns]}`` for every top-level sub-script in a code block."""
    blocks = {}
    cur = []
    for ins in insns:
        if ins.op == 0xFF:
            if cur:
                blocks[cur[0].off] = cur
            cur = []
        else:
            cur.append(ins)
    return blocks


def iter_insns(insns):
    """Every instruction, including ``IF``/``ELSE`` bodies."""
    for ins in insns:
        yield ins
        body = getattr(ins, "body", None)
        if body:
            yield from iter_insns(body)


def block_ops(insns):
    """Effect ops of one sub-script, including ``IF``/``ELSE`` bodies."""
    out = []
    seen = set()

    def emit(name, args):
        key = (name, tuple(args))
        if key in seen:                  # the same op in several IF branches
            return
        seen.add(key)
        out.append({"op": None, "name": name, "args": list(args)})

    def walk(items):
        for ins in items:
            if ins.op in EFFECT_OPS:
                emit(EFFECT_OPS[ins.op],
                     [s32(a) if isinstance(a, int) else a for a in ins.args])
            elif ins.op == 199 and ins.args:
                cond, cargs = ins.args[0]
                emit("IF:" + cond,
                     [s32(a) if isinstance(a, int) else a for a in cargs])
            body = getattr(ins, "body", None)
            if body:
                walk(body)

    walk(insns)
    return out


def level_effects(path, records):
    """Resolve E3/E4/E6/E1 of every record to the level's sub-scripts."""
    _sbt, sbtvm = _sbtmod()
    scene = _scene()
    script = level_script(path)
    if script is None:
        return {}
    insns, _ = sbtvm.decode(script.code)
    blocks = split_blocks(insns)
    ents = {}
    for eid, _flags, off in scene.entities(script):
        ents.setdefault(eid, off)

    def resolve(event):
        if event == 0:
            return None
        off = ents.get(event)
        if off is None:
            return {"event": event, "entity": None, "ops": []}
        block = blocks.get(off)
        if block is None:                        # entity points into a block
            block = []
            for ins in sbtvm.decode(script.code, off)[0]:
                if ins.op == 0xFF:
                    break
                block.append(ins)
        return {"event": event, "entity": off, "ops": block_ops(block)}

    out = {}
    for i, rec in enumerate(records):
        v = rec["view"]
        out[i] = {"on": resolve(v["event_on"]),
                  "off": resolve(v["event_off"]),
                  "no_ammo": resolve(v["event_no_ammo"]),
                  "spawn": resolve(v["spawn_event"])}
    return out


# ---------------------------------------------------------------------------
# model / statistics
# ---------------------------------------------------------------------------
def corpus_paths(game):
    return sorted(glob.glob(os.path.join(game, "*.sbt")))


def build_stats(paths):
    _sbt, sbtvm = _sbtmod()
    scene = _scene()
    stats = {
        "files": 0, "with_table": 0, "without_table": [],
        "records": 0, "per_level": {}, "names": collections.Counter(),
        "flags": collections.Counter(), "ammo_type": collections.Counter(),
        "ammo_per_tick": collections.Counter(), "object_id": collections.Counter(),
        "opcodes": collections.Counter(), "activate_pairs": collections.Counter(),
        "events": collections.Counter(), "event_misses": [],
        "activate_bits_out_of_range": [],
    }
    for path in paths:
        stats["files"] += 1
        base = os.path.basename(path)
        try:
            tables = read_level(path)
        except Fail as exc:
            stats["without_table"].append("%s (%s)" % (base, exc))
            continue
        records = [r for t in tables for r in t["records"]]
        if not records:
            stats["without_table"].append(base)
            continue
        stats["with_table"] += 1
        stats["records"] += len(records)
        stats["per_level"][base] = len(records)
        for rec in records:
            v = rec["view"]
            stats["names"][rec["name"]] += 1
            stats["flags"][v["flags"]] += 1
            stats["ammo_type"][v["ammo_type"]] += 1
            stats["ammo_per_tick"][v["ammo_per_tick"]] += 1
            stats["object_id"][v["object_id"]] += 1

        script = level_script(path)
        if script is None:
            continue
        insns, _ = sbtvm.decode(script.code)
        ents = {eid for eid, _f, _o in scene.entities(script)}
        count = len(records)
        for ins in iter_insns(insns):
            if ins.op in VM_OPS_BY_ID:
                stats["opcodes"][ins.op] += 1
            if ins.op == 130:
                args = [s32(a) for a in ins.args if isinstance(a, int)]
                if len(args) >= 2:
                    stats["activate_pairs"][(args[0], args[1])] += 1
                    if not 0 <= args[0] < count:
                        stats["activate_bits_out_of_range"].append(
                            (base, args[0], count))
        for rec in records:
            v = rec["view"]
            for key, name in (("spawn_event", "E1"), ("activate_event", "E2"),
                              ("event_on", "E3"), ("event_off", "E4"),
                              ("event_no_ammo", "E6")):
                ev = v[key]
                if ev == 0:
                    continue
                stats["events"][name] += 1
                if ev not in ents:
                    stats["event_misses"].append((base, rec["name"], name, ev))
    return stats


MODEL = {
    "record": {"base": REC_BASE, "stride": REC_STRIDE, "count_va": COUNT_VA,
               "name_offset": NAME_OFF, "max_records": MAX_RECORDS,
               "opcode": OP_DEFINE},
    "player": {"base": PLAYER_BASE, "stride": PLAYER_STRIDE,
               "count": PLAYER_COUNT},
}


def json_model(paths=None, with_effects=False):
    out = {
        "model": MODEL,
        "fields": [
            {"dword": f[0], "offset": f[1], "kind": f[2], "name": f[3],
             "status": f[4], "readers": f[5].split(), "note": f[6]}
            for f in FIELDS],
        "state": [
            {"offset": s[0], "address": s[1], "kind": s[2], "name": s[3],
             "status": s[4], "writers": s[5].split(), "readers": s[6].split()}
            for s in STATE],
        "action_bits": [{"bit": b[0], "role": b[1], "note": b[2]}
                        for b in ACTION_BITS],
        "effect_addrs": [{"address": a[0], "name": a[1], "note": a[2]}
                         for a in EFFECT_ADDRS],
        "vm": [{"opcode": v[0], "name": v[1], "signature": v[2],
                "handler": v[3], "effect": v[4]} for v in VM_OPS],
        "anchors": check_anchors(),
    }
    if paths:
        levels = []
        for path in paths:
            tables = read_level(path)
            records = [r for t in tables for r in t["records"]]
            entry = {"file": os.path.basename(path), "count": len(records),
                     "records": [{"index": i, "name": r["name"],
                                  "fields": r["view"],
                                  "dwords": ["0x%08x" % d for d in r["dwords"]]}
                                 for i, r in enumerate(records)]}
            if with_effects:
                eff = level_effects(path, records)
                for i, item in enumerate(entry["records"]):
                    item["effects"] = eff.get(i, {})
            levels.append(entry)
        out["levels"] = levels
        stats = build_stats(paths)
        out["stats"] = {
            "files": stats["files"], "with_table": stats["with_table"],
            "without_table": stats["without_table"],
            "records": stats["records"], "per_level": stats["per_level"],
            "names": dict(stats["names"]),
            "flags": {"0x%02x" % k: v for k, v in stats["flags"].items()},
            "ammo_type": {str(k): v for k, v in stats["ammo_type"].items()},
            "ammo_per_tick": {str(k): v for k, v in stats["ammo_per_tick"].items()},
            "object_id": {str(k): v for k, v in stats["object_id"].items()},
            "opcodes": {str(k): v for k, v in stats["opcodes"].items()},
            "activate_pairs": {"%d/%d" % k: v
                               for k, v in stats["activate_pairs"].items()},
            "events": dict(stats["events"]),
            "event_misses": stats["event_misses"],
            "activate_bits_out_of_range": stats["activate_bits_out_of_range"],
        }
    return out


# ---------------------------------------------------------------------------
# anchors against gundx.exe
# ---------------------------------------------------------------------------
def check_anchors():
    """``[{vma, bytes, expect, ok, what}]``; ``ok`` is None if exe is missing."""
    try:
        import exelib
        blob = exelib.data()
        va2off = exelib.va2off
    except Exception:                                    # noqa: BLE001
        blob = None
    out = []
    for vma, want, what in ANCHORS:
        ok = None
        got = ""
        if blob is not None:
            off = va2off(vma)
            if off is not None:
                got = blob[off:off + len(want) // 3 + 1].hex(" ")
            ok = got == want
        out.append({"vma": vma, "expect": want, "bytes": got,
                    "ok": ok, "what": what})
    return out


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
# synthetic code (self-test / boundaries)
# ---------------------------------------------------------------------------
def build_code(records, count=None):
    """Assemble a code block with one ``DEFINE_EQUIPMENT`` instruction."""
    out = bytearray([OP_DEFINE])
    out += struct.pack("<I", len(records) if count is None else count)
    for name, dwords in records:
        out += name.encode("ascii") + b"\0"
        for value in dwords:
            out += struct.pack("<I", value & 0xFFFFFFFF)
    out += b"\xff"
    return bytes(out)


def limit_violation(count):
    """The store function rejects a call when the array already holds >= 31."""
    return count >= MAX_RECORDS


# ---------------------------------------------------------------------------
# printing
# ---------------------------------------------------------------------------
def print_map():
    print("DEFINE_EQUIPMENT  op=%d  array 0x%06x  stride 0x%x  count 0x%06x  "
          "max %d" % (OP_DEFINE, REC_BASE, REC_STRIDE, COUNT_VA, MAX_RECORDS))
    print("  %-5s %-7s %-4s %-16s %-9s %s" %
          ("tag", "mem", "type", "field", "status", "readers"))
    for dw, off, kind, name, status, readers, note in FIELDS:
        print("  E%-4d +0x%03x  %-4s %-16s %-9s %s" %
              (dw, off, kind, name, status, readers or "-"))
        if note:
            print("        %s" % note)
    print()
    print("player state (base 0x%06x, stride 0x%x):" %
          (PLAYER_BASE, PLAYER_STRIDE))
    for off, addr, kind, name, status, writers, readers in STATE:
        print("  +0x%04x 0x%06x %-4s %-22s %-9s" %
              (off, addr, kind, name, status))
        print("        W: %s" % (writers or "-"))
        print("        R: %s" % (readers or "-"))
    print()
    print("input action bits (player+0x3344, FUN_00445fa0):")
    for bit, role, note in ACTION_BITS:
        print("  0x%08x  %-10s %s" % (bit, role, note))
    print()
    print("куда попадают эффекты:")
    for addr, name, note in EFFECT_ADDRS:
        print("  0x%06x  %-18s %s" % (addr, name, note))
    print()
    print("VM opcodes:")
    for op, name, sig, handler, effect in VM_OPS:
        print("  %3d 0x%06x %-34s %-3s %s" % (op, handler, name, sig, effect))


def print_stats(paths, game=None):
    stats = build_stats(paths)
    if not stats["files"]:
        print("нет .sbt в %s" % (game or "?"))
        return
    print("== корпус: %d файлов" % stats["files"])
    counts = collections.Counter(stats["per_level"].values())
    spread = ", ".join("%d записей: %d файл(ов)" % (k, v)
                       for k, v in sorted(counts.items()))
    print("   с DEFINE_EQUIPMENT: %d, без: %d%s"
          % (stats["with_table"], len(stats["without_table"]),
             (" (" + ", ".join(stats["without_table"][:4]) + ")")
             if stats["without_table"] else ""))
    print("   %s; всего записей %d" % (spread, stats["records"]))
    repeated = {n: c for n, c in stats["names"].items() if c > 1}
    single = {n: c for n, c in stats["names"].items() if c == 1}
    print("   уникальных имён: %d; повторяются: %d" %
          (len(stats["names"]), len(repeated)))
    for name, cnt in stats["names"].most_common():
        if cnt != stats["with_table"]:
            print("     %-14s %d (не во всех уровнях)" % (name, cnt))
    if single:
        print("     однократные: %s" % ", ".join(sorted(single)))
    print()
    print("== поля (значения по корпусу)")
    print("   flags:          %s" % ", ".join(
        "0x%02x×%d" % (k, v) for k, v in sorted(stats["flags"].items())))
    print("   ammo_type:      %s" % ", ".join(
        "%d×%d" % (k, v) for k, v in sorted(stats["ammo_type"].items())))
    print("   ammo_per_tick:  %s" % ", ".join(
        "%d×%d" % (k, v) for k, v in sorted(stats["ammo_per_tick"].items())))
    print("   object_id:      %s" % ", ".join(
        "%d×%d" % (k, v) for k, v in sorted(stats["object_id"].items())))
    print()
    print("== активация (ВМ)")
    print("   опкоды: %s" % ", ".join(
        "%s×%d" % (VM_OPS_BY_ID[op][1], stats["opcodes"][op])
        for op in sorted(stats["opcodes"])))
    pairs = ", ".join("%d=%d×%d" % (bit, on, cnt)
                      for (bit, on), cnt in sorted(stats["activate_pairs"].items()))
    print("   op130 (бит, вкл): %s" % (pairs or "-"))
    print("   биты вне 0..count-1: %d" % len(stats["activate_bits_out_of_range"]))
    print()
    print("== события записей")
    print("   %s" % ", ".join("%s×%d" % (k, stats["events"][k])
                              for k in ("E1", "E2", "E3", "E4", "E6")))
    print("   не найдено сущности-скрипта: %d" % len(stats["event_misses"]))
    for miss in stats["event_misses"][:5]:
        print("     %s %s %s=%d" % miss)
    print()
    print("ИТОГ: %d файлов, %d записей, %d уникальных имён, "
          "%d вызовов op130, %d промахов событий"
          % (stats["files"], stats["records"], len(stats["names"]),
             stats["opcodes"][130], len(stats["event_misses"])))


def print_table(path):
    """«индекс -> имя -> поля» для одного уровня."""
    tables = read_level(path)
    records = [r for t in tables for r in t["records"]]
    if not records:
        print("%s: нет DEFINE_EQUIPMENT" % path)
        return
    print("== %s: DEFINE_EQUIPMENT, записей %d (stride 0x%x, база 0x%06x)"
          % (os.path.basename(path), len(records), REC_STRIDE, REC_BASE))
    print("  %-3s %-14s %-6s %-6s %-7s %-8s %-7s %-7s %-8s %-7s %-4s"
          % ("idx", "имя", "flags", "obj", "ammo", "ammo/t", "price",
             "weight", "on", "off", "E6"))
    for i, rec in enumerate(records):
        v = rec["view"]
        print("  %-3d %-14s 0x%02x   %-6d %-7d %-8d %-7d %-7d %-8d %-7d %-4d"
              % (i, rec["name"], v["flags"], v["object_id"], v["ammo_type"],
                 v["ammo_per_tick"], v["price"], v["weight"],
                 v["event_on"], v["event_off"], v["event_no_ammo"]))
    print()
    for i, rec in enumerate(records[:1]):
        print("  поля записи (пример [%d] %s):" % (i, rec["name"]))
        for dw, off, kind, name, status, _readers, _note in FIELDS:
            value = rec["view"][name]
            print("    %-3s +0x%03x %-4s %-16s %-10s %s"
                  % ("E%d" % dw, off, kind, name, status, value))


def print_effects(path):
    tables = read_level(path)
    records = [r for t in tables for r in t["records"]]
    if not records:
        print("%s: нет DEFINE_EQUIPMENT" % path)
        return
    eff = level_effects(path, records)
    print("== %s: %d записей" % (os.path.basename(path), len(records)))
    for i, rec in enumerate(records):
        v = rec["view"]
        print(" [%2d] %-14s obj=%-2d ammo=%-2d cost=%-2d price=%-7d weight=%d"
              % (i, rec["name"], v["object_id"], v["ammo_type"],
                 v["ammo_per_tick"], v["price"], v["weight"]))
        for key in ("on", "off", "no_ammo", "spawn"):
            item = eff[i].get(key)
            if not item or item["event"] == 0:
                continue
            ops = " | ".join(
                ("%s %s" % (op["name"].replace("SOURCEPLAYER_", ""),
                            ",".join(str(a) for a in op["args"]))).strip()
                for op in item["ops"])
            print("      %-8s %7d : %s" % (key, item["event"], ops or "—"))


def print_check(paths):
    """Data invariants over the corpus (also used by --self-test)."""
    stats = build_stats(paths)
    problems = []
    if not stats["files"]:
        problems.append("не найдено ни одного .sbt (--game)")
    if stats["without_table"]:
        problems.append("уровни без таблицы: %s" % stats["without_table"])
    bad_counts = {k: v for k, v in stats["per_level"].items() if v not in (12, 13)}
    if bad_counts:
        problems.append("нестандартное число записей: %s" % bad_counts)
    if stats["records"] and stats["names"].get("HEADLIGHTS") != stats["with_table"]:
        problems.append("HEADLIGHTS не во всех уровнях")
    if stats["event_misses"]:
        problems.append("события без сущности: %s" % stats["event_misses"][:3])
    if stats["activate_bits_out_of_range"]:
        problems.append("op130 с битом вне таблицы: %s"
                        % stats["activate_bits_out_of_range"][:3])
    zero_e2 = stats["events"]["E2"]
    print("== проверки данных")
    print("   файлов %d, записей %d, событий E1/E2/E3/E4/E6 = %d/%d/%d/%d/%d"
          % (stats["files"], stats["records"], stats["events"]["E1"],
             zero_e2, stats["events"]["E3"], stats["events"]["E4"],
             stats["events"]["E6"]))
    print("   ammo_per_tick > 0: %d записей; ammo_type >= 0: %d"
          % (sum(v for k, v in stats["ammo_per_tick"].items() if k > 0),
             sum(v for k, v in stats["ammo_type"].items() if k >= 0)))
    if problems:
        for p in problems:
            print("   ОШИБКА: %s" % p)
    else:
        print("   OK: инварианты выполнены")
    return not problems


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def _sample_record(index, name, **kw):
    dwords = [0] * len(FIELDS)
    dwords[0] = kw.get("flags", 3)                     # E0
    dwords[3] = kw.get("event_on", 0) & 0xFFFFFFFF     # E3
    dwords[4] = kw.get("event_off", 0) & 0xFFFFFFFF    # E4
    dwords[6] = kw.get("event_no_ammo", 0) & 0xFFFFFFFF
    dwords[7] = kw.get("ammo_per_tick", 0)             # E7
    dwords[8] = kw.get("ammo_type", 0) & 0xFFFFFFFF    # E8
    dwords[9] = kw.get("price", 0)                     # E9
    dwords[11] = kw.get("object_id", index + 1)        # E11
    return (name, dwords)


def do_selftest():
    _COUNT[0] = 0

    # 1. the map itself
    expect(len(FIELDS) == 12, "полей %d, ожидалось 12" % len(FIELDS))
    expect([f[0] for f in FIELDS] == list(range(12)), "dword'ы не 0..11")
    expect(len({f[1] for f in FIELDS}) == 12, "смещения полей пересекаются")
    expect(NAME_OFF not in {f[1] for f in FIELDS}, "имя пересекает поле")
    expect(not [f for f in FIELDS if NAME_OFF <= f[1] < NAME_OFF + 16],
           "поле внутри char[16] имени")
    expect(FIELDS[-1][1] == 0x3C, "object_id не на +0x3c")
    print("OK   карта: 12 dword'ов -> 12 смещений, имя +0x2c")

    # 2. synthetic parse through the real decoder
    a = _sample_record(0, "ALPHA", flags=1, event_on=-30043,
                       event_off=-30044, ammo_per_tick=1, ammo_type=10,
                       price=25000, object_id=6)
    b = _sample_record(1, "BETA", flags=0x89, event_on=1000018,
                       ammo_type=-1, object_id=7)
    code = build_code([a, b])
    tables = tables_from_code(code)
    expect(len(tables) == 1, "синтетика: таблиц %d" % len(tables))
    recs = tables[0]["records"]
    expect(len(recs) == 2, "синтетика: записей %d" % len(recs))
    expect(recs[0]["name"] == "ALPHA" and recs[1]["name"] == "BETA",
           "синтетика: имена")
    expect(recs[0]["view"]["event_on"] == -30043, "E3 не знаковый")
    expect(recs[0]["view"]["ammo_type"] == 10, "E8 не разобран")
    expect(recs[0]["view"]["ammo_per_tick"] == 1, "E7 не разобран")
    expect(recs[0]["view"]["price"] == 25000, "E9 не разобран")
    expect(recs[1]["view"]["ammo_type"] == -1, "E8=-1 не знаковый")
    expect(recs[1]["view"]["event_on"] == 1000018, "положительный E3")
    expect(recs[1]["dwords"][9] == 0, "E9 BETA")
    print("OK   синтетика: 2 записи, знаки E3/E8, смещения")

    # 3. empty table and 31-record boundary
    empty = tables_from_code(build_code([]))
    expect(empty and empty[0]["records"] == [], "пустая таблица")
    many = [(("E%02d" % i), _sample_record(i, "E%02d" % i)[1]) for i in range(31)]
    tables = tables_from_code(build_code(many))
    expect(len(tables[0]["records"]) == 31, "31 запись не разобрана")
    expect(not limit_violation(30), "предел сработал на 30 (31-я ещё влезает)")
    expect(limit_violation(31), "31 записей: 32-я должна отвергаться")
    expect(limit_violation(32), "предел не сработал на 32")
    print("OK   границы: 0, 31 запись; 32-я отвергается стором")

    # 4. a truncated record must be reported, not silently accepted
    broken = build_code([a, b], count=3)
    try:
        tables_from_code(broken)
    except Exception as exc:                            # noqa: BLE001
        expect(True, "битая таблица: %s" % exc)
    else:
        raise Fail("битая таблица (count=3, записей 2) не отвергнута")
    print("OK   битая таблица отвергается")

    # 5. determinism: same bytes -> same JSON
    j1 = json.dumps([t["records"] for t in tables_from_code(build_code([a, b]))],
                    sort_keys=True)
    j2 = json.dumps([t["records"] for t in tables_from_code(build_code([a, b]))],
                    sort_keys=True)
    expect(j1 == j2, "не детерминировано")
    expect(len(j1) > 100, "пустой JSON")
    print("OK   детерминизм разбора")

    # 6. state / action-bit arithmetic
    expect(PLAYER_BASE + 0x3328 == 0x81C368, "active адрес")
    expect(PLAYER_BASE + 0x3320 == 0x81C360, "index адрес")
    expect(PLAYER_BASE + 0x3324 == 0x81C364, "owned адрес")
    expect(0x40000 << 1 == 0x80000, "hotkey 1")
    expect(0x40000 << 10 == 0x10000000, "hotkey 10")
    expect(len(ACTION_BITS) == 13, "action bits %d" % len(ACTION_BITS))
    expect(VM_OPS_BY_ID[130][2] == "LL" and VM_OPS_BY_ID[136][2] == "L",
           "сигнатуры опкодов")
    print("OK   состояние и биты действий")

    # 7. byte anchors in the executable
    self_test_anchors()

    # 8. cross-check against tools/re/weapon_fields.py (no import side effects)
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import weapon_fields as wf
    except Exception:                                    # noqa: BLE001
        wf = None
    if wf is not None:
        theirs = {f[0]: (f[1], f[2]) for f in wf.BY_NAME["DEFINE_EQUIPMENT"].fields}
        mine = {f[0]: (f[1], f[2]) for f in FIELDS}
        expect(mine == theirs, "карта расходится с weapon_fields: %s"
               % [(k, mine.get(k), theirs.get(k))
                  for k in set(mine) | set(theirs) if mine.get(k) != theirs.get(k)])
        print("OK   карта совпадает с weapon_fields.py (12/12)")

    # 9. corpus (skipped when the game is not installed)
    paths = corpus_paths(DEFAULT_GAME)
    if paths:
        stats = build_stats(paths)
        expect(stats["files"] == 36, "файлов %d, ожидалось 36" % stats["files"])
        expect(stats["with_table"] == 36, "уровней с таблицей %d" % stats["with_table"])
        expect(stats["records"] == 36 * 12 + 2,
               "записей %d, ожидалось 434" % stats["records"])
        expect(stats["names"]["HEADLIGHTS"] == 36, "HEADLIGHTS не 36")
        expect(not stats["event_misses"], "события без скрипта: %s"
               % stats["event_misses"][:3])
        expect(not stats["activate_bits_out_of_range"], "op130 вне таблицы")
        expect(stats["opcodes"][130] > 400, "мало вызовов op130: %d"
               % stats["opcodes"][130])
        expect(stats["opcodes"][129] == 72, "op129 %d, ожидалось 72 (2×36)"
               % stats["opcodes"][129])
        bits = {bit for (bit, _on) in stats["activate_pairs"]}
        expect(bits == set(range(8)), "op130 трогает биты %s" % sorted(bits))
        expect(stats["activate_pairs"][(0, 1)] == 36, "HEADLIGHTS on не 36")
        expect(stats["activate_pairs"][(0, 0)] == 36, "HEADLIGHTS off не 36")
        expect(stats["ammo_per_tick"][0] > 0 and stats["ammo_per_tick"][1] > 0,
               "нет записей с ammo_per_tick")
        print("OK   корпус: %d файлов, %d записей, op130×%d, событий E3×%d"
              % (stats["files"], stats["records"], stats["opcodes"][130],
                 stats["events"]["E3"]))
    else:
        print("SKIP корпус: %s не найден" % DEFAULT_GAME)

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
        description="Модель экипировки Gunmetal: DEFINE_EQUIPMENT, состояние, эффекты")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--map", action="store_true", help="карта записи и состояния")
    ap.add_argument("--stats", action="store_true", help="статистика по корпусу")
    ap.add_argument("--json", action="store_true", help="полная модель в JSON")
    ap.add_argument("--file", help="один .sbt (для --table / --effects / --json)")
    ap.add_argument("--table", action="store_true",
                    help="таблица «индекс -> имя -> поля» уровня (нужен --file)")
    ap.add_argument("--effects", action="store_true",
                    help="что делают on/off-скрипты уровня (нужен --file)")
    ap.add_argument("--check", action="store_true", help="инварианты данных")
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
        anchors = check_anchors()
        for a in anchors:
            state = "-" if a["ok"] is None else ("ok" if a["ok"] else "FAIL")
            print("%s 0x%06x  %-29s %s" %
                  (state, a["vma"], a["expect"], a["what"]))
        return 0
    if args.json:
        print(json.dumps(json_model(paths, with_effects=args.effects),
                         ensure_ascii=False, indent=2, sort_keys=True,
                         default=str))
        return 0
    if args.table:
        if not args.file:
            ap.error("--table требует --file LEVEL.sbt")
        print_table(paths[0])
        return 0
    if args.effects:
        if not args.file:
            ap.error("--effects требует --file LEVEL.sbt")
        print_effects(paths[0])
        return 0
    if args.stats:
        print_stats(paths, args.game)
        return 0
    if args.check:
        return 0 if print_check(paths) else 1
    print_map()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
