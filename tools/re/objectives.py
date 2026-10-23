#!/usr/bin/env python3
"""Level objectives, secret tally and the triggers that end a level (M6.4.5).

Read-only: the executable and the installation are never written.  Everything
works **without Ghidra** -- the numbers come from the bytes of ``gundx.exe``
(via :mod:`exelib`) and from the 36 shipped ``.sbt`` scripts, which are decoded
here by a *local* operand walker built from the engine's own tables:

* ``COMMANDS``  ``0x4b53e8`` -- 192 entries x 20 bytes
  ``{char *name; char *sig; u32 flag; u32 -1; u32 opcode}``;
* ``CONDITIONS`` ``0x4b62e8`` -- 59 entries of the same shape (the second VM,
  used by ``IF``/``EVAL``).

``gm.sbtvm`` is imported only as a **second opinion** in ``--self-test``: the
two decoders must agree instruction for instruction.  The counts below never
come from it.

Usage::

    python3 tools/re/objectives.py --self-test
    python3 tools/re/objectives.py --fields
    python3 tools/re/objectives.py --summary [--game DIR]
    python3 tools/re/objectives.py --anchors
    python3 tools/re/objectives.py --json

Status words: ``код`` = instruction bytes, ``данные`` = shipped files,
``гипотеза`` = not proven.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

import exelib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)          # gm.sbt is used for the raw record split

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")
MIRROR = os.path.join(os.path.expanduser("~"), ".cache", "gunmetal-re", "game")
CACHE_EXE = os.path.join(exelib.cache_dir(), "gundx.exe")

# --- objectives ----------------------------------------------------------
OBJ_COUNT = 0x80D7EC      # u32, DEFINE_LEVEL_OBJECTIVES count (0..30)
OBJ_TEXT = 0x80D800       # char *[30], one pointer per objective
OBJ_TEXT_STRIDE = 4
OBJ_MAX = 30
OBJ_BITS = 0x80F9D0       # u32, OR_LEVEL_OBJECTIVES_BITS accumulator
OBJ_VERSION = 0x80F9D4    # u16, SET_LEVEL_VERSION (opcode 102) -- NOT objectives
OBJ_NEIGHBOUR = 0x80D7F0  # f64 low half, SET_LEVEL_FOG_DISTANCE (98) -- NOT objectives

# --- secrets -------------------------------------------------------------
SECRETS_DECL = 0x80D8C0   # u32, SET_LEVEL_SECRETS (94)
PLAYER_BASE = 0x819040    # player slot 0 (block 9 of a save, 0x3510 bytes)
PLAYER_STRIDE = 0x3510
SECRET_TALLY = 0x33CC     # -> 0x81c40c for player 0
CURRENT_PLAYER = 0x83D308 # u32, index used by the "%ld / %ld" getters

# --- sectors / triggers --------------------------------------------------
SECTOR_STUFF = 0x800B60   # 256 x 0x58
SECTOR_STRIDE = 0x58
SECTOR_EVENT_A = 0x38     # -> 0x800b98
SECTOR_EVENT_B = 0x3C     # -> 0x800b9c
SECTOR_DAMAGE_A = 0x40    # -> 0x800ba0
SECTOR_DAMAGE_B = 0x44    # -> 0x800ba4
SECTOR_FLAGS = 0x50       # -> 0x800bb0
REGEN_SOB = 0x80D878      # DEFINE_REGENERATION_MARKER (174) sob index

# --- end-of-level screen -------------------------------------------------
FRAME_SUBMODE = 0x83CB7C  # == 5 -> level end screen
LEVEL_SECONDS = 0x83D638
YOUR_BEST = 0x83D2C0
PAR_TIME = 0x80D88C       # SET_LEVEL_BEST_TIME (93)

CMD_TABLE = 0x4B53E8
CMD_COUNT = 0x00C0
COND_TABLE = 0x4B62E8
COND_COUNT = 0x003B
TABLE_ENTRY = 20
DISPATCH = 0x45C500       # jmp table for opcodes 1..202
COND_DISPATCH = 0x45F2AC

#: fixed-size record tables: opcode -> operand signature of *one* record.
TABLE_SIGS = {
    170: "SSSLLLLEEEELLLLDBBDL",                                    # DEFINE_WEAPONS
    171: "SLEOLLLLLLL",                                            # DEFINE_AMMO
    172: "SSSLDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE",  # DEFINE_PLAYER_TYPES
    175: "SLEEEEEELLLLL",                                         # DEFINE_EQUIPMENT
}
TABLE_NAMES = {
    170: "DEFINE_WEAPONS",
    171: "DEFINE_AMMO",
    172: "DEFINE_PLAYER_TYPES",
    175: "DEFINE_EQUIPMENT",
}
END_OP = 0xFF
IF_OP = 199
ELSE_OP = 200
EVAL_OP = 0xC9
DEFINE_OBJECTIVES_OP = 103
OR_BITS_OP = 104
SET_SECRETS_OP = 94
INC_TALLY_OP = 95
LEVEL_END_OP = 91

# ---------------------------------------------------------------------------
# field map (printed by --fields, embedded in --json)
# ---------------------------------------------------------------------------
FIELDS = [
    dict(key="objectives_count", addr=OBJ_COUNT, width=4, status="код",
         what="число целей уровня, 0..30",
         writer="DEFINE_LEVEL_OBJECTIVES (103) 0x45be04 (a3 ec d7 80 00), "
                "сброс 0x45709d (в 0x456f50)",
         reader="условие 7 0x45e9f2; HUD 0x487338; сейв-писатель не читает"),
    dict(key="objectives_text", addr=OBJ_TEXT, width=OBJ_TEXT_STRIDE * OBJ_MAX,
         status="код",
         what="char*[30]: указатели на строки целей (текст = код уровня)",
         writer="0x45be11..0x45be21 (mov esi,0x80d800; mov [esi],ebx; шаг 4)",
         reader="HUD 0x487354 (строка 0x487350), рисует 0x47faf0"),
    dict(key="objectives_bits", addr=OBJ_BITS, width=4, status="код",
         what="битовая маска выполненных целей; бит i = 1<<i",
         writer="только OR_LEVEL_OBJECTIVES_BITS (104) 0x45be4e (or 0x45be49); "
                "сброс 0x4570a3; больше писателей нет (--anchors/--self-test)",
         reader="условие 7 0x45ea01; HUD 0x48735a; сейв-писатель 0x462f2b"),
    dict(key="level_version", addr=OBJ_VERSION, width=2, status="код",
         what="SET_LEVEL_VERSION (102), u16; НЕ цели (соседний адрес)",
         writer="0x45b858 (66 89 15 d4 f9 80 00)",
         reader="0x465580 (склейка имени файла/версии)"),
    dict(key="fog_distance_lo", addr=OBJ_NEIGHBOUR, width=8, status="код",
         what="SET_LEVEL_FOG_DISTANCE (98), младший dword double; НЕ цели",
         writer="0x45b7cf", reader="0x446790, 0x44a0e5"),
    dict(key="secrets_declared", addr=SECRETS_DECL, width=4, status="код",
         what="сколько секретов объявлено в уровне (SET_LEVEL_SECRETS)",
         writer="0x45b816..0x45b81e, сброс 0x45708a",
         reader="0x483c09 (конец уровня), 0x499536 (панель F6)"),
    dict(key="secret_tally", addr=PLAYER_BASE + SECRET_TALLY, width=4,
         status="код",
         what="найдено секретов текущим игроком; player+0x33cc, шаг 0x3510 "
              "(8 слотов: 0x81c40c + i*0x3510)",
         writer="0x4536dc (+1) -- единственный писатель, вызывается из "
                "0x45b978 (INCREMENT_SOURCEPLAYER_SECRET_TALLY, 95); "
                "сброс 0x45280f на старте уровня (0x452730 <- 0x452430 <- 0x442f5a)",
         reader="0x4422a7 (геттер 0x442290, индекс 0x83d308); 0x483c11; 0x49953c"),
    dict(key="current_player", addr=CURRENT_PLAYER, width=4, status="код",
         what="индекс игрока для геттеров 0x442290/0x4422b0",
         writer="0x819040-относительные пути игрока (docs/PLAYER_FIELDS.md)",
         reader="0x442290, 0x4422b0"),
    dict(key="frame_submode", addr=FRAME_SUBMODE, width=4, status="код",
         what="==5 экран конца уровня (ставит SHOW_LEVEL_END_SCREEN 0x45c066)",
         writer="0x45c066 (=5), 0x460549, 0x44b85a, 0x44c2ed",
         reader="0x445445 (Activate -> event -8), диспетчер кадра"),
    dict(key="level_seconds", addr=LEVEL_SECONDS, width=4, status="код",
         what="время уровня в секундах = [0x5a46e4] / 91",
         writer="0x45c080", reader="0x483a?? (TIME:)"),
    dict(key="sector_event_a", addr=SECTOR_STUFF + SECTOR_EVENT_A, width=4,
         status="код",
         what="event_a (SET_SECTOR_EVENTS 13); вход/выход — не разделено",
         writer="0x4579d8 (из 0x4579ab)",
         reader="0x445095 -> event_post 0x4795b0 (индекс [player+0x81c27c])"),
    dict(key="sector_event_b", addr=SECTOR_STUFF + SECTOR_EVENT_B, width=4,
         status="код",
         what="event_b (SET_SECTOR_EVENTS 13); вход/выход — не разделено",
         writer="0x4579e2",
         reader="0x44503f -> event_post 0x4795b0 (индекс eax из [esp+0xc])"),
    dict(key="sector_damage", addr=SECTOR_STUFF + SECTOR_DAMAGE_A, width=8,
         status="код",
         what="SET_SECTOR_DAMAGE (11): damage_a (+0x40), damage_b (+0x44)",
         writer="0x45799c/0x4579a2 (low16/high16 знаково)",
         reader="0x4450f6/0x4450fc, умножает на dt и зовёт 0x453b50"),
    dict(key="regen_marker_sob", addr=REGEN_SOB, width=4, status="код",
         what="DEFINE_REGENERATION_MARKER (174) sob index (у всех 36 = 19)",
         writer="0x45c37d", reader="0x468841/0x468937 (маркер вместо предмета)"),
]

STRINGS = [
    (0x4BCBB0, "SECRETS FOUND:"),
    (0x4BCB78, "%ld / %ld"),
    (0x4BEB10, "SECRETS FOUND"),
    (0x4BE7D8, "%ld/%ld"),
    (0x4BCB60, "TO CONTINUE"),
    (0x4BCB6C, "PRESS %s"),
]

#: byte anchors -- every row is verified by --anchors/--self-test.
ANCHORS = [
    (0x45BDF0, "8b043383c30483f81e895d08", "103 DEFINE_LEVEL_OBJECTIVES: count<=30"),
    (0x45BE04, "a3ecd78000", "count -> 0x80d7ec"),
    (0x45BE11, "be00d88000", "text array base 0x80d800"),
    (0x45BE40, "8b14338b0dd0f980000bca", "104: load mask, OR operand"),
    (0x45BE4E, "890dd0f98000", "104: store 0x80f9d0 (единственный писатель)"),
    (0x45E9F0, "8b15ecd7800033c985d2", "условие 7: count"),
    (0x45EA00, "a1d0f98000be01000000", "условие 7: mask, 1<<i"),
    (0x45EA19, "b801000000", "условие 7: true"),
    (0x4570A3, "891dd0f98000", "сброс 0x80f9d0 (0x456f50)"),
    (0x45709D, "891decd78000", "сброс 0x80d7ec"),
    (0x45B816, "8b143383c3048915c0d880", "94 SET_LEVEL_SECRETS -> 0x80d8c0"),
    (0x45708A, "891dc0d88000", "сброс 0x80d8c0"),
    (0x45B978, "25ffff00000f8c", "95: индекс игрока 0..7"),
    (0x4536B0, "8b44240485c07c26", "0x4536b0(player): bounds"),
    (0x4536CF, "8d810cc48100", "0x4536b0: lea [ecx+0x81c40c]"),
    (0x45280F, "89b30cc48100", "сброс tally на старте уровня"),
    (0x442290, "a108d383008d", "геттер tally: текущий игрок 0x83d308"),
    (0x4422A7, "8b810cc48100c3", "геттер tally: +0x33cc"),
    (0x483C07, "8b15c0d88000", "экран конца: declared"),
    (0x483C10, "52e87ae6fbff50", "экран конца: call 0x442290 (tally)"),
    (0x462F2A, "a1d0f9800085c07446a1", "сейв-писатель: if bits != 0"),
    (0x462F33, "a184674b006a68", "сейв-писатель: opcode 0x68 = 104"),
    (0x462E0C, "c7048580465e00fdffffff", "сейв-писатель: entity -3"),
    (0x4454A2, "6af8e807", "event_post(-8) с экрана конца"),
    (0x4537A5, "6af9e8", "event_post(-7) смерть игрока"),
    (0x453EF4, "6aefe8", "event_post(-17) жизнь ниже половины"),
    (0x453F51, "6aeee8", "event_post(-18) жизнь выше половины"),
    (0x4426A5, "6afde854d30100", "generate_runner(-3) INTERNAL_LEVEL_RESUME"),
    (0x44328C, "6afc891de4465a00", "generate_runner(-4) INTERNAL_LEVEL_STARTUP"),
    (0x487337, "a1ecd780008d", "HUD: count"),
    (0x487358, "8b15d0f98000", "HUD: bits"),
    (0x48737A, "e861c0ffff", "HUD: call 0x4833e0 (маркер)"),
    (0x4833F2, "b810fc6700ba10000000", "маркер 'check' (16 байт)"),
    (0x4833FE, "b8b83f68008d0c", "маркер 'nocheck' (8 байт)"),
    (0x445456, "f6c480", "Activate: бит 0x8000 в [+0x3344]"),
    (0x445095, "8b0cd5980b8000", "сектор: event_a"),
    (0x44503F, "8b0cd59c0b8000", "сектор: event_b"),
    (0x4450F6, "8b81a00b8000", "сектор: damage_a"),
    (0x4450FC, "8b89a40b8000", "сектор: damage_b"),
    (0x457961, "8b043383c304895d088b", "11 SET_SECTOR_DAMAGE"),
    (0x4579AB, "8b043383c30485c0895d", "13 SET_SECTOR_EVENTS"),
    (0x4697EB, "db4614", "радиус-триггер: fild [esi+0x14]"),
    (0x45C057, "8b0de4465a00b8b5400bb4f7", "91 SHOW_LEVEL_END_SCREEN"),
]

#: expected xref counts for the fields the doc cares about (dword references)
XREFS = {
    OBJ_BITS: 7,        # reset, 104 (load+store), cond 7, save writer (x2), HUD
    OBJ_COUNT: 6,       # reset, 103 (x2), cond 7, HUD (x2)
    SECRETS_DECL: 4,    # reset, 94, end screen, panel
    PLAYER_BASE + SECRET_TALLY: 5,  # getter (x2), reset, 0x4536b0 (x2), panel
    OBJ_TEXT: 2,        # 103 writer, HUD
}

#: corpus expectations, all reproduced by --summary
EXPECTED = {
    "levels": 36,
    "obj_levels": 25,        # levels that declare objectives
    "obj_sites": 35,         # DEFINE_LEVEL_OBJECTIVES instructions
    "obj_primary_texts": 51, # first declaration of each level
    "obj_all_texts": 67,     # every declaration, variants included
    "obj_union_texts": 58,   # distinct texts per level, summed
    "obj_multi_levels": 4,   # levels with more than one declaration
    "or_sites": 51,          # OR_LEVEL_OBJECTIVES_BITS instructions
    "or_levels": 19,         # levels that actually set bits
    "or_without_complete": 0,
    "if_complete": 48,       # IF LEVEL_OBJECTIVES_COMPLETE
    "eval_complete": 0,      # EVAL LEVEL_OBJECTIVES_COMPLETE
    "end_screens": 38,       # SHOW_LEVEL_END_SCREEN sites
    "end_levels": 31,
    "secret_sites": 25,      # SET_LEVEL_SECRETS instructions
    "secret_total": 101,     # sum of declared counts
    "secret_levels": 25,
    "inc_sites": 105,        # INCREMENT_SOURCEPLAYER_SECRET_TALLY
    "inc_levels": 27,
    "obj_no_bits_levels": 6, # declare text but never set a bit
    "complete_no_or_levels": 0,
}


# ---------------------------------------------------------------------------
# 1. local decoder (engine tables -> operand walker)
# ---------------------------------------------------------------------------
class Insn:
    __slots__ = ("off", "op", "name", "args", "size", "body")

    def __init__(self, off, op, name, args=(), size=None, body=None):
        self.off, self.op, self.name = off, op, name
        self.args, self.size, self.body = list(args), size, body

    def __repr__(self):
        return "Insn(0x%x, %d, %s)" % (self.off, self.op, self.name)


def _exe_ok():
    if os.path.exists(exelib.EXE):
        return True
    if os.path.exists(CACHE_EXE):
        exelib.EXE = CACHE_EXE
        return True
    return False


def _read(va, n):
    off = exelib.va2off(va)
    if off is None:
        return None
    return exelib.data()[off:off + n]


def _u32(buf, i):
    return struct.unpack_from("<I", buf, i)[0]


def _cstr(buf, i):
    j = buf.find(b"\0", i)
    if j < 0:
        raise ValueError("unterminated string at 0x%x" % i)
    return buf[i:j].decode("latin1"), j + 1


def read_engine_table(base, count):
    """``{opcode: (name, sig)}`` from a 20-byte-entry table in the exe."""
    out = {}
    for i in range(count):
        raw = _read(base + TABLE_ENTRY * i, TABLE_ENTRY)
        if raw is None or len(raw) < TABLE_ENTRY:
            raise ValueError("table 0x%x truncated at %d" % (base, i))
        name_va, sig_va, _flag, _neg, op = struct.unpack("<5I", raw)
        name = exelib.cstr(name_va) or ""
        sig = exelib.cstr(sig_va) or ""
        out[op] = (name, sig)
    return out


def _sig_ops(sig):
    out = []
    for ch in sig:
        if ch not in exelib.LETTER_SIZE:
            raise ValueError("bad signature letter %r" % ch)
        out.append(exelib.LETTER_SIZE[ch])
    return out


def _read_ops(buf, i, ops, want_values=True):
    vals = []
    for size in ops:
        if size == "str":
            s, i = _cstr(buf, i)
            vals.append(s)
        elif size == 8:
            vals.append(struct.unpack_from("<d", buf, i)[0])
            i += 8
        else:
            vals.append(int.from_bytes(buf[i:i + size], "little"))
            i += size
    return (vals if want_values else []), i


def decode(code, start=0, end=None, cmds=None, conds=None, exact=True):
    """Decode a SCRIPT code block with the exe-derived operand tables.

    Mirrors the engine's walker (``0x457170`` + ``0x45c500``): one byte opcode,
    signature-sized operands, ``0xFF`` terminates a sub-script.  ``IF``/``ELSE``
    bodies are decoded recursively; every sub-script must end exactly at *end*.
    """
    if cmds is None or conds is None:
        raise ValueError("decode() needs the command/condition tables")
    if end is None:
        end = len(code)
    out, i = [], start
    while i < end:
        off, op = i, code[i]
        if op == END_OP:
            out.append(Insn(off, op, "END", size=1))
            i += 1
            continue
        if op in TABLE_SIGS:
            sig_ops = _sig_ops(TABLE_SIGS[op])
            n = _u32(code, i + 1)
            if n > 0x40:
                raise ValueError("%s count too large (%d) at 0x%x"
                                 % (TABLE_NAMES[op], n, off))
            j = i + 5
            for _ in range(n):
                _, j = _read_ops(code, j, sig_ops)
            out.append(Insn(off, op, TABLE_NAMES[op], [("n", n)], size=j - i))
            i = j
            continue
        if op == DEFINE_OBJECTIVES_OP:
            n = _u32(code, i + 1)
            if n > OBJ_MAX:
                raise ValueError("DEFINE_LEVEL_OBJECTIVES count %d at 0x%x"
                                 % (n, off))
            j, texts = i + 5, []
            for _ in range(n):
                s, j = _cstr(code, j)
                texts.append(s)
            out.append(Insn(off, op, "DEFINE_LEVEL_OBJECTIVES", texts,
                            size=j - i))
            i = j
            continue
        if op == 158:                                     # SOURCEPLAYER_PATH
            n = _u32(code, i + 1)
            if n > 0x20:
                raise ValueError("SOURCEPLAYER_PATH count %d at 0x%x" % (n, off))
            out.append(Insn(off, op, "SOURCEPLAYER_PATH", [("n", n)],
                            size=5 + 4 * n))
            i += 5 + 4 * n
            continue
        if op == 188:                                     # ADD_LIFT
            n = _u32(code, i + 1 + 0x94)
            j = i + 5 + 0x94
            if n > 0x1F:
                raise ValueError("ADD_LIFT stop count %d at 0x%x" % (n, off))
            out.append(Insn(off, op, "ADD_LIFT", [("n", n)], size=j + 12 * n - i))
            i = j + 12 * n
            continue
        if op == IF_OP:
            cid = code[i + 1]
            name, sig = conds.get(cid, ("OP_%d" % cid, ""))
            args, j = _read_ops(code, i + 2, _sig_ops(sig))
            size = _u32(code, j)
            body, _ = decode(code, j + 4, j + size, cmds, conds,
                             exact=False)
            out.append(Insn(off, op, "IF", [(name, args)], size=j + size - i,
                            body=body))
            i = j + size
            if i < end and code[i] == ELSE_OP:            # else-branch follows
                rel = _u32(code, i + 1)
                tgt = i + 1 + rel
                ebody, _ = decode(code, i + 5, tgt, cmds, conds,
                                  exact=False)
                out.append(Insn(i, ELSE_OP, "ELSE", [("target", tgt)],
                                size=rel, body=ebody))
                i = tgt
            continue
        if op == ELSE_OP:
            rel = _u32(code, i + 1)
            out.append(Insn(off, op, "ELSE", [("target", i + 1 + rel)], size=rel))
            i = i + 1 + rel
            continue
        if op == EVAL_OP:
            cid = code[i + 1]
            name, sig = conds.get(cid, ("OP_%d" % cid, ""))
            args, j = _read_ops(code, i + 2, _sig_ops(sig))
            out.append(Insn(off, op, "EVAL", [(name, args)], size=j - i))
            i = j
            continue
        if op not in cmds:
            raise ValueError("unknown opcode %d at 0x%x" % (op, off))
        name, sig = cmds[op]
        args, j = _read_ops(code, i + 1, _sig_ops(sig))
        out.append(Insn(off, op, name, args, size=j - i))
        i = j
    if exact and i != end:
        raise ValueError("decode overshoot at 0x%x (block 0x%x..0x%x)"
                         % (i, start, end))
    return out, i


def walk(insns, conds=()):
    """Yield ``(insn, conditions)`` descending into IF/ELSE bodies."""
    for ins in insns:
        yield ins, conds
        if ins.name == "IF" and ins.body is not None:
            cond = ins.args[0][0] if ins.args else "?"
            yield from walk(ins.body, conds + (cond,))
        elif ins.name == "ELSE" and ins.body is not None:
            yield from walk(ins.body, conds + ("else",))


def val(arg):
    """Operand value: plain values pass through, ``(tag, v)`` pairs unwrap."""
    if isinstance(arg, (int, float, str)):
        return arg
    return arg[1]


def objectives_mask_complete(mask, count):
    """Same rule as ``LEVEL_OBJECTIVES_COMPLETE`` (0x45e9f0)."""
    if count <= 0:
        return False
    return all(mask & (1 << i) for i in range(count))


# ---------------------------------------------------------------------------
# 2. corpus scan
# ---------------------------------------------------------------------------
def _level_files(game):
    return sorted(glob.glob(os.path.join(game, "*.sbt")))


def _script(payload):
    """Record 11 payload -> its code block (the level's compiled script)."""
    return payload.code


def scan_level(stem, path, cmds, conds):
    """Decode one ``.sbt`` and collect the objective/secret facts."""
    from gm import sbt as sbtmod                       # local oracle parser

    sbt = sbtmod.parse_file(path)
    recs = sbt.records_of(11)
    row = dict(stem=stem, script_records=len(recs), code_bytes=0,
               obj_sites=[], obj_primary=None, obj_all_texts=0,
               obj_union_texts=0, or_sites=[], or_union=0, if_complete=0,
               eval_complete=0, end_screens=0, secret_sites=[],
               secrets_declared=[], inc_sites=[], errors=[])
    if not recs:
        row["errors"].append("no SCRIPT record (11)")
        return row
    code = _script(recs[0].payload)
    row["code_bytes"] = len(code)
    try:
        insns, used = decode(code, cmds=cmds, conds=conds)
        if used != len(code):
            row["errors"].append("decode ended at %d/%d" % (used, len(code)))
    except ValueError as exc:
        row["errors"].append(str(exc))
        return row

    texts_union = set()
    for ins, conds_path in walk(insns):
        if ins.name == "DEFINE_LEVEL_OBJECTIVES":
            texts = [str(val(a)) for a in ins.args]
            row["obj_sites"].append(dict(off=ins.off, texts=texts,
                                         conditions=list(conds_path)))
            row["obj_all_texts"] += len(texts)
            texts_union.update(texts)
        elif ins.name == "OR_LEVEL_OBJECTIVES_BITS":
            row["or_sites"].append(dict(off=ins.off,
                                        value=int(val(ins.args[0])) & 0xFFFFFFFF))
        elif ins.name == "IF" and ins.args and \
                ins.args[0][0] == "LEVEL_OBJECTIVES_COMPLETE":
            row["if_complete"] += 1
        elif ins.name == "EVAL" and ins.args and \
                ins.args[0][0] == "LEVEL_OBJECTIVES_COMPLETE":
            row["eval_complete"] += 1
        elif ins.name == "SET_LEVEL_SECRETS":
            row["secret_sites"].append(dict(off=ins.off,
                                            value=int(val(ins.args[0]))))
        elif ins.name == "INCREMENT_SOURCEPLAYER_SECRET_TALLY":
            row["inc_sites"].append(ins.off)
        elif ins.name == "SHOW_LEVEL_END_SCREEN":
            row["end_screens"] += 1
    row["obj_union_texts"] = len(texts_union)
    primary = next((s for s in row["obj_sites"] if not s["conditions"]),
                   row["obj_sites"][0] if row["obj_sites"] else None)
    row["obj_primary"] = primary
    bits = 0
    for s in row["or_sites"]:
        bits |= s["value"]
    row["or_union"] = bits
    row["secrets_declared"] = [s["value"] for s in row["secret_sites"]]
    return row


def scan_corpus(game, cmds, conds):
    rows = {}
    for path in _level_files(game):
        stem = os.path.splitext(os.path.basename(path))[0]
        rows[stem] = scan_level(stem, path, cmds, conds)
    return rows


def totals(rows):
    t = dict(levels=len(rows), levels_bad=0)
    for key in ("obj_levels", "obj_sites", "obj_primary_texts",
                "obj_all_texts", "obj_union_texts", "obj_multi_levels",
                "or_sites", "or_levels", "if_complete", "eval_complete",
                "end_screens", "end_levels", "secret_sites", "secret_total",
                "secret_levels", "inc_sites", "inc_levels",
                "obj_no_bits_levels", "complete_no_or_levels"):
        t[key] = 0
    for stem, r in rows.items():
        if r["errors"]:
            t["levels_bad"] += 1
        if r["obj_sites"]:
            t["obj_levels"] += 1
            t["obj_multi_levels"] += 1 if len(r["obj_sites"]) > 1 else 0
            t["obj_primary_texts"] += len(r["obj_primary"]["texts"])
        t["obj_sites"] += len(r["obj_sites"])
        t["obj_all_texts"] += r["obj_all_texts"]
        t["obj_union_texts"] += r["obj_union_texts"]
        t["or_sites"] += len(r["or_sites"])
        if r["or_sites"]:
            t["or_levels"] += 1
        t["if_complete"] += r["if_complete"]
        t["eval_complete"] += r["eval_complete"]
        t["end_screens"] += r["end_screens"]
        t["end_levels"] += 1 if r["end_screens"] else 0
        t["secret_sites"] += len(r["secret_sites"])
        t["secret_total"] += sum(r["secrets_declared"])
        if r["secret_sites"]:
            t["secret_levels"] += 1
        t["inc_sites"] += len(r["inc_sites"])
        if r["inc_sites"]:
            t["inc_levels"] += 1
        if r["obj_sites"] and not r["or_sites"]:
            t["obj_no_bits_levels"] += 1
        if r["or_sites"] and not (r["if_complete"] or r["eval_complete"]):
            t["complete_no_or_levels"] += 1
    t["or_without_complete"] = sum(
        1 for r in rows.values()
        if r["or_sites"] and not (r["if_complete"] or r["eval_complete"]))
    return t


def corpus_numbers(rows):
    """Per-level table + totals, the payload of --summary/--json."""
    t = totals(rows)
    order = sorted(rows, key=lambda s: (
        0 if s.startswith("level") and s[5:].isdigit() else
        1 if s in ("level0", "level00", "level000", "level0a", "level0b") else
        2 if s.startswith("secrt") or s == "temple" else
        3 if s.startswith("dm") else 4, s))
    levels = {}
    for stem in order:
        r = rows[stem]
        levels[stem] = dict(
            role=("hq" if stem in ("level0", "level00", "level000",
                                   "level0a", "level0b")
                  else "secret" if stem in ("secrt1", "secrt2", "temple")
                  else "dm" if stem.startswith("dm") else "mission"),
            code_bytes=r["code_bytes"],
            objectives=[dict(off="0x%06x" % s["off"], texts=s["texts"],
                             conditions=s["conditions"])
                        for s in r["obj_sites"]],
            objective_primary=[s["texts"] for s in [r["obj_primary"]]
                               if s],
            or_sites=[dict(off="0x%06x" % s["off"], value=s["value"])
                      for s in r["or_sites"]],
            or_union=r["or_union"],
            if_complete=r["if_complete"],
            eval_complete=r["eval_complete"],
            end_screens=r["end_screens"],
            secrets=[dict(off="0x%06x" % s["off"], value=s["value"])
                     for s in r["secret_sites"]],
            secret_increments=["0x%06x" % o for o in r["inc_sites"]],
            errors=r["errors"],
        )
    return dict(totals=t, levels=levels)


# ---------------------------------------------------------------------------
# 3. exe facts: xrefs, event posts, anchors
# ---------------------------------------------------------------------------
def dword_refs(value):
    """VAs of every little-endian dword equal to *value* in the image."""
    needle = struct.pack("<I", value & 0xFFFFFFFF)
    data = exelib.data()
    out, i = [], data.find(needle)
    while i >= 0:
        va = exelib.off2va(i)
        if va is not None:
            out.append(va)
        i = data.find(needle, i + 1)
    return out


def _calls_to(target):
    data = exelib.data()
    out = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        src = exelib.off2va(i)
        if src is None:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        if (src + 5 + rel) & 0xFFFFFFFF == target:
            out.append(src)
    return out


def event_posts():
    """``{negative id: [site]}}`` for the SYSTEM events this doc relies on.

    Scans the ``event_post`` call sites (``0x4795b0``) and the direct
    ``generate_runner`` calls (``0x45fa00``) for the ``push imm8`` that carries
    the event id in the preceding window.
    """
    data = exelib.data()
    out = {-3: [], -4: [], -7: [], -8: [], -17: [], -18: []}
    for target in (0x4795B0, 0x45FA00):
        for src in _calls_to(target):
            off = exelib.va2off(src)
            for j in range(off - 1, max(-1, off - 80), -1):
                if data[j] != 0x6A or data[j + 1] < 0x80:
                    continue                 # nearest *negative* push = event id
                eid = data[j + 1] - 0x100
                if eid in out:
                    out[eid].append(src)
                break
    return {k: sorted(set(v)) for k, v in out.items()}


def anchor_rows():
    rows = []
    for va, want, note in ANCHORS:
        raw = _read(va, len(want) // 2)
        got = raw.hex() if raw is not None else None
        rows.append(dict(va="0x%06x" % va, bytes=want, note=note,
                         ok=(got == want.lower()), got=got))
    return rows


def exe_facts():
    facts = dict(
        exe=exelib.EXE,
        sha256=exelib.exe_sha256() if _exe_ok() else None,
        xrefs={},
        events={},
        anchors=anchor_rows(),
    )
    for va, want in XREFS.items():
        hits = dword_refs(va)
        facts["xrefs"]["0x%06x" % va] = dict(
            expected=want, actual=len(hits),
            sites=["0x%06x" % h for h in hits])
    ev = event_posts()
    facts["events"] = {"%d" % k: ["0x%06x" % s for s in v]
                       for k, v in sorted(ev.items())}
    return facts


def triggers_json():
    return dict(
        sector_events=dict(
            table="0x800b60", stride=SECTOR_STRIDE,
            event_a="+0x38 (0x800b98)", event_b="+0x3c (0x800b9c)",
            damage_a="+0x40 (0x800ba0)", damage_b="+0x44 (0x800ba4)",
            flags="+0x50 (0x800bb0)",
            writer="SET_SECTOR_EVENTS 13 -> 0x4579d8/0x4579e2; "
                   "SET_SECTOR_DAMAGE 11 -> 0x45799c/0x4579a2",
            reader="0x44503f/0x445095: при смене сектора (сравнение "
                   "0x445026) два event_post по 11 аргументов: b для индекса "
                   "eax=[esp+0xc], a для [player+0x81c27c]; какой из них "
                   "вход/выход — не доказано (docs/RECORDS.md §2); "
                   "0x4450f6/0x4450fc: damage*dt -> 0x453b50",
            status="код"),
        radius=dict(
            site="0x469770", status="код",
            what="радиальный скан 300 записей: player/x,y,z/flags 3 каждый тик "
                 "из 0x445165; предметы с флагами 3 из 0x471e03; "
                 "условие distance < radius (+0x4c), fild в 0x4697eb; "
                 "попадание -> event_post(+0x44) (docs/PICKUP.md §4)",
            player_fields="+0x14 radius, +0x18..+0x20 position, +0x44 event"),
        regen_marker=dict(
            opcode=174, sob=REGEN_SOB, doubles=["0x80f9e0", "0x80f830"],
            status="код",
            what="sob-часть жива (маркер в DM вместо пропавшего предмета, "
                 "0x468841); double-поля никто не читает (docs/PICKUP.md §7)"),
        level_end=dict(
            opcode=LEVEL_END_OP, handler="0x45c057", status="код",
            steps="ticks/91 -> 0x83d638; frame_submode=5 (0x45c066); "
                  "0x45c880 обновляет GUNMETAL.HI",
            activate="0x445456 test ah,0x80; 0x4454a2 push -8 / call 0x4795b0",
            screen="0x483a70 (один игрок) / 0x483550 (несколько): KILLS, "
                   "SECRETS FOUND: tally/declared, SALVAGE, TIME, BEST TIME; "
                   "списка целей на экране нет"),
        objectives_hud=dict(
            entry="0x4869c0", site="0x487337", status="код",
            callers=["0x487aec", "0x488309"],
            marker="0x4833e0: 16 байт из 0x67fc10 ('check') если бит стоит, "
                   "8 байт из 0x683fb8 ('nocheck') если нет (загрузка 0x48fdb9/"
                   "0x48fdf8, имена 0x4be318/0x4be310)",
            line_break="'*' в тексте цели = перенос строки (0x487394..0x4873a3)"),
    )


# ---------------------------------------------------------------------------
# 4. self-test
# ---------------------------------------------------------------------------
class Checker:
    def __init__(self):
        self.passed = 0
        self.failed = []
        self.skipped = []

    def check(self, cond, name):
        if cond:
            self.passed += 1
        else:
            self.failed.append(name)

    def skip(self, name):
        self.skipped.append(name)


def _synthetic():
    """Decoder sanity on hand-built bytes (no game files, no exe)."""
    cmds = {1: ("EXIT_EVENT", ""), 91: ("SHOW_LEVEL_END_SCREEN", ""),
            94: ("SET_LEVEL_SECRETS", "L"), 95:
            ("INCREMENT_SOURCEPLAYER_SECRET_TALLY", ""),
            104: ("OR_LEVEL_OBJECTIVES_BITS", "L")}
    conds = {7: ("LEVEL_OBJECTIVES_COMPLETE", ""),
             9: ("SOURCE_IS_PLAYER", "")}
    out = {}
    # DEFINE_LEVEL_OBJECTIVES 2 "A" "B"
    code = b"\x67\x02\x00\x00\x00A\x00B\x00\xff"
    ins, used = decode(code, cmds=cmds, conds=conds)
    out["define"] = (ins[0].name, ins[0].args, used == len(code))
    # OR_LEVEL_OBJECTIVES_BITS 5 ; SET_LEVEL_SECRETS 3 ; inc
    code = b"\x68\x05\x00\x00\x00\x5e\x03\x00\x00\x00\x5f\xff"
    ins, used = decode(code, cmds=cmds, conds=conds)
    out["state"] = ([i.name for i in ins], [i.args for i in ins[:2]],
                    used == len(code))
    # IF <cond 7> { OR 1 } ELSE;  body size = 4 (u32 size) + 5
    body = b"\x68\x01\x00\x00\x00"
    then = struct.pack("<I", len(body) + 4) + body
    p = b"\xc7\x07" + then
    # else branch: OR 2 ; END ; then jump over
    else_body = b"\x68\x02\x00\x00\x00\xff"
    p += b"\xc8" + struct.pack("<I", 4 + len(else_body)) + else_body
    p += b"\xff"
    ins, used = decode(p, cmds=cmds, conds=conds)
    names = [i.name for i in ins]
    out["if"] = (names, used == len(p), ins[0].args[0][0],
                 [i.name for i in ins[0].body],
                 [i.name for i in ins[1].body])
    # EVAL <SOURCE_IS_PLAYER>
    ins, used = decode(b"\xc9\x09\xff", cmds=cmds, conds=conds)
    out["eval"] = (ins[0].name, ins[0].args[0][0], used == 3)
    return out


def self_test(game):
    c = Checker()
    syn = _synthetic()
    c.check(syn["define"] == ("DEFINE_LEVEL_OBJECTIVES", ["A", "B"], True),
            "synthetic: DEFINE_LEVEL_OBJECTIVES")
    c.check(syn["state"][0] == ["OR_LEVEL_OBJECTIVES_BITS",
                                "SET_LEVEL_SECRETS",
                                "INCREMENT_SOURCEPLAYER_SECRET_TALLY", "END"],
            "synthetic: state opcodes")
    c.check(syn["state"][1] == [[5], [3]], "synthetic: operand values")
    c.check(syn["state"][2], "synthetic: byte-exact end")
    c.check(syn["if"][0] == ["IF", "ELSE", "END"], "synthetic: IF/ELSE shape")
    c.check(syn["if"][2] == "LEVEL_OBJECTIVES_COMPLETE",
            "synthetic: IF condition 7")
    c.check(syn["if"][3] == ["OR_LEVEL_OBJECTIVES_BITS"],
            "synthetic: then-branch")
    c.check(syn["if"][4] == ["OR_LEVEL_OBJECTIVES_BITS", "END"],
            "synthetic: else-branch")
    c.check(syn["if"][1], "synthetic: IF byte-exact end")
    c.check(syn["eval"] == ("EVAL", "SOURCE_IS_PLAYER", True),
            "synthetic: EVAL")

    c.check(objectives_mask_complete(0b1111, 4), "bits: 4 of 4 -> complete")
    c.check(not objectives_mask_complete(0b0111, 4), "bits: 3 of 4 -> incomplete")
    c.check(not objectives_mask_complete(0, 0), "bits: count 0 -> false")
    c.check(objectives_mask_complete(0b11111, 4),
            "bits: extra high bit ignored")
    c.check(not objectives_mask_complete(0b1111, 5), "bits: one short -> false")

    # --- structural tables ------------------------------------------------
    c.check(len(FIELDS) >= 14, "fields: >=14 записей")
    c.check(len({f["key"] for f in FIELDS}) == len(FIELDS),
            "fields: ключи уникальны")
    c.check(len({f["addr"] for f in FIELDS}) == len(FIELDS),
            "fields: адреса уникальны")
    c.check(all(f["status"] in ("код", "данные", "код+данные", "гипотеза")
                for f in FIELDS), "fields: допустимые статусы")
    c.check(len({a[0] for a in ANCHORS}) == len(ANCHORS),
            "anchors: адреса уникальны")
    c.check(all(len(h) % 2 == 0 for _va, h, _n in ANCHORS),
            "anchors: чётная длина hex")
    a = json.dumps(triggers_json(), sort_keys=True)
    b = json.dumps(triggers_json(), sort_keys=True)
    c.check(a == b, "json: детерминизм")

    facts = dict(exe=None, corpus=None)
    if not _exe_ok():
        for name in ("tables", "anchors", "xrefs", "events"):
            c.skip("%s (нет gundx.exe)" % name)
        cmds = conds = None
    else:
        cmds = read_engine_table(CMD_TABLE, CMD_COUNT)
        conds = read_engine_table(COND_TABLE, COND_COUNT)
        facts["exe"] = exe_facts()
        c.check(len(cmds) == 192, "exe: 192 команды")
        c.check(len(conds) == 59, "exe: 59 условий")
        c.check(cmds.get(94) == ("SET_LEVEL_SECRETS", "L"),
                "exe: 94 SET_LEVEL_SECRETS (L)")
        c.check(cmds.get(95) == ("INCREMENT_SOURCEPLAYER_SECRET_TALLY", ""),
                "exe: 95 INCREMENT_SOURCEPLAYER_SECRET_TALLY")
        c.check(103 not in cmds, "exe: 103 структурный (нет в COMMANDS)")
        c.check(cmds.get(104) == ("OR_LEVEL_OBJECTIVES_BITS", "L"),
                "exe: 104 OR_LEVEL_OBJECTIVES_BITS (L)")
        c.check(cmds.get(91) == ("SHOW_LEVEL_END_SCREEN", ""),
                "exe: 91 SHOW_LEVEL_END_SCREEN")
        c.check(cmds.get(174) == ("DEFINE_REGENERATION_MARKER", "ODD"),
                "exe: 174 DEFINE_REGENERATION_MARKER")
        c.check(conds.get(7) == ("LEVEL_OBJECTIVES_COMPLETE", ""),
                "exe: условие 7")
        c.check(_read(0x4B53E8, 4) is not None, "exe: COMMANDS mapped")

        for row in anchor_rows():
            c.check(row["ok"], "anchor %s: %s" % (row["va"], row["note"]))
        for va, want in XREFS.items():
            hits = dword_refs(va)
            c.check(len(hits) == want,
                    "xref 0x%06x: %d (ожидалось %d)"
                    % (va, len(hits), want))
        ev = facts["exe"]["events"]
        c.check(len(ev["-8"]) == 1, "event -8: 1 сайт (%r)" % ev["-8"])
        c.check(len(ev["-7"]) >= 1, "event -7: есть сайт")
        c.check(len(ev["-17"]) >= 1, "event -17: есть сайт")
        c.check(len(ev["-18"]) >= 1, "event -18: есть сайт")
        c.check(len(ev["-3"]) >= 1, "event -3: есть сайт")
        c.check(len(ev["-4"]) >= 1, "event -4: есть сайт")
        # the level-end screen must not reference the objective arrays
        end_lo, end_hi = 0x483A70, 0x483E40
        bad = [h for h in dword_refs(OBJ_BITS) + dword_refs(OBJ_COUNT)
               if end_lo <= h < end_hi]
        c.check(not bad, "экран конца не читает цели (%r)" % bad)
        # only the reset, the OR handler and the save writer touch the mask
        writers = [h for h in dword_refs(OBJ_BITS)
                   if 0x4570A0 <= h < 0x4570B0 or 0x45BE40 <= h < 0x45BE58]
        c.check(len(writers) == 3,
                "0x80f9d0: reset + 104 load/store (%d)" % len(writers))
        c.check(facts["exe"]["sha256"] is not None, "exe: sha256")

    if cmds is None:
        for name in ("corpus", "decoder cross-check", "numbers"):
            c.skip("%s (нет gundx.exe)" % name)
        return c, facts

    files = _level_files(game)
    c.check(len(files) == 36, "корпус: 36 .sbt (найдено %d)" % len(files))
    if len(files) != 36:
        c.skip("corpus (нет 36 файлов)")
        return c, facts

    rows = scan_corpus(game, cmds, conds)
    numbers = corpus_numbers(rows)
    facts["corpus"] = numbers
    c.check(all(not r["errors"] for r in rows.values()),
            "корпус: нет ошибок декодирования %r"
            % {k: v["errors"] for k, v in rows.items() if v["errors"]})

    # --- second opinion: gm.sbtvm -----------------------------------------
    try:
        from gm import sbt as sbtmod
        from gm import sbtvm
    except Exception as exc:                          # pragma: no cover
        c.skip("gm.sbtvm (%s)" % exc)
    else:
        mismatch = []
        for path in files:
            stem = os.path.splitext(os.path.basename(path))[0]
            sbt = sbtmod.parse_file(path)
            ref, _ = sbtvm.decode(sbt.records_of(11)[0].payload.code)
            ours, _ = decode(sbt.records_of(11)[0].payload.code,
                             cmds=cmds, conds=conds)
            if len(ref) != len(ours) or any(
                    (a.off, a.op, a.name) != (b.off, b.op, b.name)
                    for a, b in zip(ref, ours)):
                mismatch.append(stem)
        c.check(not mismatch,
                "сверка с gm.sbtvm: %d/%d файлов" % (len(files) - len(mismatch),
                                                     len(files)))
        c.check(not any(r["errors"] for r in rows.values()),
                "локальный декодер: байт-в-байт до конца")

    t = numbers["totals"]
    for key, want in EXPECTED.items():
        c.check(t.get(key) == want,
                "число %s: %s (ожидалось %s)" % (key, t.get(key), want))
    # secrets arithmetic: increments - declared = alternative/no-declare sites
    c.check(t["secret_total"] + 2 - 1 + 3 == t["inc_sites"],
            "секреты: 101 + 2 (level3) - 1 (level15) + 3 (secrt2/temple) = %d"
            % t["inc_sites"])
    # levels that declare objectives but never set bits
    no_bits = sorted(k for k, r in rows.items()
                     if r["obj_sites"] and not r["or_sites"])
    c.check(no_bits == ["dm3", "level13", "level16", "level18", "level21",
                        "level24"], "без битов: %r" % no_bits)
    # level19 is the only level with many declarations
    many = sorted(k for k, r in rows.items() if len(r["obj_sites"]) > 1)
    c.check(many == ["level18", "level19", "level2", "level7"],
            "несколько объявлений: %r" % many)
    # "*" is the HUD line break, no objective text uses a bare CR/LF
    c.check(all("\n" not in tx for r in rows.values()
                for s in r["obj_sites"] for tx in s["texts"]),
            "тексты целей без переводов строки (разделитель '*')")
    # exactly the levels that set bits also test completion
    with_or = {k for k, r in rows.items() if r["or_sites"]}
    with_test = {k for k, r in rows.items()
                 if r["if_complete"] or r["eval_complete"]}
    c.check(with_or == with_test,
            "биты и проверка завершения — одни и те же 19 уровней")
    # the three exception levels of the tally arithmetic
    odd = {k: (sum(r["secrets_declared"]), len(r["inc_sites"]))
           for k, r in rows.items()
           if sum(r["secrets_declared"]) != len(r["inc_sites"])}
    c.check(odd == {"level3": (6, 8), "level15": (5, 4),
                    "secrt2": (0, 2), "temple": (0, 1)},
            "аномалии tally: %r" % odd)
    return c, facts


# ---------------------------------------------------------------------------
# 5. output
# ---------------------------------------------------------------------------
def exe_needed(name):
    if _exe_ok():
        return True
    print("%s: пропущено, gundx.exe не найден" % name, file=sys.stderr)
    return False


def resolve_game(game):
    if game:
        return game
    if os.path.isdir(MIRROR):
        return MIRROR
    return DEFAULT_GAME


def print_fields():
    print("%-22s %-9s %3s %-8s %s" % ("field", "addr", "w", "status", "what"))
    for f in FIELDS:
        print("%-22s 0x%06x %3d %-8s %s" % (f["key"], f["addr"], f["width"],
                                            f["status"], f["what"]))
        print("%22s writer: %s" % ("", f["writer"]))
        print("%22s reader: %s" % ("", f["reader"]))


def print_summary(game):
    cmds = read_engine_table(CMD_TABLE, CMD_COUNT)
    conds = read_engine_table(COND_TABLE, COND_COUNT)
    rows = scan_corpus(game, cmds, conds)
    numbers = corpus_numbers(rows)
    t = numbers["totals"]
    print("objectives/secrets per level (36 .sbt, local decoder)")
    print("%-8s %-8s %3s %3s %3s %5s %5s %3s %4s %8s %5s"
          % ("level", "role", "def", "txt", "or", "bits", "ifC", "end",
             "secr", "declared", "inc"))
    for stem, r in numbers["levels"].items():
        if r["role"] == "mission" or r["objectives"] or r["secrets"] or \
                r["secret_increments"]:
            print("%-8s %-8s %3d %3d %3d %5s %5d %3d %4s %8s %5d"
                  % (stem, r["role"], len(r["objectives"]),
                     sum(len(o["texts"]) for o in r["objectives"]),
                     len(r["or_sites"]),
                     ("0x%x" % r["or_union"]) if r["or_union"] else "-",
                     r["if_complete"], r["end_screens"],
                     len(r["secrets"]),
                     ",".join(str(s["value"]) for s in r["secrets"]) or "-",
                     len(r["secret_increments"])))
    print()
    print("totals:")
    print("  levels                  %d (decode errors: %d)"
          % (t["levels"], t["levels_bad"]))
    print("  objectives: levels %d, declarations %d, primary texts %d, "
          "all texts %d, distinct %d, multi-variant levels %d"
          % (t["obj_levels"], t["obj_sites"], t["obj_primary_texts"],
             t["obj_all_texts"], t["obj_union_texts"], t["obj_multi_levels"]))
    print("  OR_LEVEL_OBJECTIVES_BITS: sites %d, levels %d"
          % (t["or_sites"], t["or_levels"]))
    print("  LEVEL_OBJECTIVES_COMPLETE: IF %d, EVAL %d, levels with bits but "
          "no test %d" % (t["if_complete"], t["eval_complete"],
                          t["or_without_complete"]))
    print("  declare text but never set bits: %d levels"
          % t["obj_no_bits_levels"])
    print("  SHOW_LEVEL_END_SCREEN: sites %d, levels %d"
          % (t["end_screens"], t["end_levels"]))
    print("  SET_LEVEL_SECRETS: sites %d, levels %d, declared total %d"
          % (t["secret_sites"], t["secret_levels"], t["secret_total"]))
    print("  INCREMENT_SOURCEPLAYER_SECRET_TALLY: sites %d, levels %d"
          % (t["inc_sites"], t["inc_levels"]))
    print("  arithmetic: %d declared + 2 (level3) - 1 (level15) + 3 "
          "(secrt2/temple) = %d sites"
          % (t["secret_total"], t["inc_sites"]))
    if _exe_ok():
        ev = event_posts()
        print("  events: -3 %s; -4 %s; -7 %s; -8 %s; -17 %s; -18 %s"
              % tuple(",".join("0x%06x" % s for s in ev[k]) or "-"
                      for k in (-3, -4, -7, -8, -17, -18)))
    return numbers


def print_anchors():
    if not exe_needed("anchors"):
        return
    bad = 0
    for row in anchor_rows():
        mark = "ok  " if row["ok"] else "FAIL"
        bad += 0 if row["ok"] else 1
        print("%s %s %-24s %s" % (mark, row["va"], row["bytes"], row["note"]))
    print("%d anchors, %d failed" % (len(ANCHORS), bad))


def json_payload(game):
    facts = exe_facts() if _exe_ok() else {"exe": None}
    if not _exe_ok():
        return dict(fields=FIELDS, anchors=ANCHORS, triggers=triggers_json(),
                    exe=None, corpus=None)
    cmds = read_engine_table(CMD_TABLE, CMD_COUNT)
    conds = read_engine_table(COND_TABLE, COND_COUNT)
    rows = scan_corpus(game, cmds, conds)
    return dict(
        fields=[dict(f, addr="0x%06x" % f["addr"]) for f in FIELDS],
        anchors=[dict(va="0x%06x" % va, bytes=h, note=n)
                 for va, h, n in ANCHORS],
        triggers=triggers_json(),
        exe=dict(sha256=facts["sha256"], xrefs=facts["xrefs"],
                 events=facts["events"],
                 commands=len(cmds), conditions=len(conds)),
        corpus=corpus_numbers(rows),
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=None,
                    help="game directory (default: mirror, else installation)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--fields", action="store_true")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--anchors", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    game = resolve_game(args.game)
    if not any((args.self_test, args.fields, args.summary, args.anchors,
                args.json)):
        ap.print_help()
        return 1
    if args.self_test:
        c, _facts = self_test(game)
        print("self-test: %d/%d checks passed"
              % (c.passed, c.passed + len(c.failed)))
        for name in c.skipped:
            print("  SKIP %s" % name)
        for name in c.failed:
            print("  FAIL %s" % name)
        return 0 if not c.failed else 1
    if args.anchors:
        print_anchors()
    if args.fields:
        print_fields()
    if args.summary:
        print_summary(game)
    if args.json:
        print(json.dumps(json_payload(game), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
