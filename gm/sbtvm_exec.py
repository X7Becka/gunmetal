"""Proto-executor for the Gunmetal ``SCRIPT`` VM (``.sbt`` record type 11).

:mod:`gm.sbtvm` decodes the byte code; this module *runs* it against an explicit
state model.  The real VM lives in ``gundx.exe`` at ``0x457390`` (see
``docs/VM_EXEC.md`` for the annotated disassembly); its loop is::

    pc = 0
    for (;;) {
        op = code[pc++];
        if (op == 0xff) break;                     // 0x4573d1
        if (op == 0xc9) { eval_cond(); continue; } // 0x4573db
        if (op == 199) {                           // IF, 0x4574db
            r = eval_cond();                       //   0x45d120
            if (r) pc += 4;                        //   skip the sizing dword
            else   pc += *(u32*)&code[pc];         //   jump to ELSE / past IF
            continue;
        }
        if (op == 200) { pc += *(u32*)&code[pc]; continue; }  // ELSE
        handlers[op - 1]();
    }

There is **no operand stack and no arithmetic opcode class**: conditions are
individual engine handlers that compare their inline operands with engine state
and return 0/1 (``docs/VM_EXEC.md`` has the table).  The executor therefore
models an explicit state instead of a stack machine: PC, a sub-script call
stack (``GENERATE``), entity flags, per-player variables, level globals, the
random source and the scene the script builds.

Operand layouts are read from :mod:`gm.sbtvm`'s recovered tables, but the
byte-level reader is local so that a bad operand can be reported at the exact
PC (the decoder's own reader assumes a well-formed block).
"""

from __future__ import annotations

import copy
import struct
import time
from dataclasses import dataclass, field
from typing import Any

from . import sbtvm

#: engine code-buffer limit (``0x45739e``: ``pc < 0x30000``)
MAX_BLOCK = sbtvm.MAX_BLOCK

#: sub-script call depth (engine recursion is bounded by the C stack; a limit
#: here turns a runaway ``GENERATE`` chain into a clean diagnostic)
MAX_DEPTH = 32

_UNSET = object()


# --------------------------------------------------------------------------
# errors / results


class VMError(Exception):
    """Base class for a clean, reportable VM failure."""


class BadOperand(VMError):
    def __init__(self, pc: int, what: str):
        super().__init__("bad operand at 0x%x: %s" % (pc, what))
        self.pc = pc


class BadPC(VMError):
    def __init__(self, pc: int, length: int):
        super().__init__("pc 0x%x outside code [0, 0x%x)" % (pc, length))
        self.pc = pc


class Halt(VMError):
    """Internal control-flow exception carrying a stop reason."""

    def __init__(self, reason: str, pc: int, detail: str = ""):
        super().__init__("%s at 0x%x %s" % (reason, pc, detail))
        self.reason = reason
        self.pc = pc
        self.detail = detail


#: stop reasons (``PathResult.reason``)
END = "end"                       # reached 0xff at call depth 0
RETURN = "return"                 # EXIT_EVENT / 0xff inside a sub-script
UNKNOWN_OPCODE = "unknown-opcode"
UNSUPPORTED_COMMAND = "unsupported-command"
UNKNOWN_CONDITION = "unknown-condition"
STEP_LIMIT = "step-limit"
PATH_LIMIT = "path-limit"
DEADLINE = "deadline"
BAD_OPERAND = "bad-operand"
BAD_PC = "bad-pc"
STACK_OVERFLOW = "stack-overflow"
RECURSION = "recursion"
PROGRAM_EXIT = "program-exit"
LEVEL_TRANSITION = "level-transition"


# --------------------------------------------------------------------------
# instruction reader


def _need(code: bytes, i: int, n: int, pc: int = -1) -> None:
    if i < 0 or n < 0 or i + n > len(code):
        raise BadOperand(pc if pc >= 0 else i, "need %d bytes at 0x%x" % (n, i))


def _u32(code: bytes, i: int, pc: int) -> int:
    _need(code, i, 4, pc)
    return struct.unpack_from("<I", code, i)[0]


def _str(code: bytes, i: int, pc: int) -> tuple[str, int]:
    j = code.find(b"\0", i)
    if j < 0:
        raise BadOperand(pc, "unterminated string at 0x%x" % i)
    return code[i:j].decode("latin1"), j + 1


def _args(code: bytes, i: int, sig: str, pc: int) -> tuple[list, int]:
    out = []
    for ch in sig:
        size = sbtvm.LETTER_SIZE.get(ch)
        if size is None:
            raise BadOperand(pc, "bad signature letter %r" % ch)
        if size == "str":
            v, i = _str(code, i, pc)
            out.append(v)
        elif size == 8:
            _need(code, i, 8, pc)
            out.append(struct.unpack_from("<d", code, i)[0])
            i += 8
        elif size == 1:
            _need(code, i, 1, pc)
            out.append(code[i])
            i += 1
        else:
            out.append(_u32(code, i, pc))
            i += size
    return out, i


def _table(code: bytes, i: int, sig: str, limit: int, pc: int):
    nstr = len(sig) - len(sig.lstrip("S"))
    letters = sig[nstr:]
    n = _u32(code, i, pc)
    i += 4
    if n > limit:
        raise BadOperand(pc, "table count %d > %d" % (n, limit))
    recs = []
    for _ in range(n):
        strs = []
        for _ in range(nstr):
            s, i = _str(code, i, pc)
            strs.append(s)
        vals, i = _args(code, i, letters, pc)
        recs.append((strs, vals))
    return recs, i


@dataclass(frozen=True)
class Ins:
    """One instruction, decoded on demand at a byte PC."""

    pc: int
    op: int
    name: str
    args: tuple
    next: int                 # PC after the whole instruction (incl. bodies)
    kind: str = "cmd"         # cmd | end | if | else | eval | table | ...
    cond: str | None = None   # IF / EVAL: condition name
    cond_args: tuple = ()
    body_start: int = -1      # IF: first byte of the then-branch
    body_end: int = -1        # IF: first byte after it (ELSE opcode or next insn)
    target: int = -1          # ELSE: absolute jump target
    data: Any = None          # tables / lift payload
    error: str = ""           # kind == "unknown"


_TABLES = sbtvm.TABLE_SIGS
_TABLE_LIMIT = {170: 31, 171: 31, 172: 32, 175: 31}


def decode_at(code: bytes, pc: int) -> Ins:
    """Decode the instruction at *pc* (byte-exact; no recursion into bodies)."""
    if pc < 0 or pc >= len(code):
        raise BadPC(pc, len(code))
    op = code[pc]
    if op == 0xFF:
        return Ins(pc, op, "END", (), pc + 1, "end")
    if op in _TABLES:
        recs, end = _table(code, pc + 1, _TABLES[op], _TABLE_LIMIT[op], pc)
        return Ins(pc, op, sbtvm.TABLE_NAMES[op], (("n", len(recs)),), end,
                   "table", data=recs)
    if op == 103:                                    # DEFINE_LEVEL_OBJECTIVES
        n = _u32(code, pc + 1, pc)
        if n > 30:
            raise BadOperand(pc, "objective count %d > 30" % n)
        i = pc + 5
        vals = []
        for _ in range(n):
            s, i = _str(code, i, pc)
            vals.append(s)
        return Ins(pc, op, "DEFINE_LEVEL_OBJECTIVES", (("n", n),), i,
                   "objectives", data=vals)
    if op == 158:                                    # SOURCEPLAYER_PATH
        n = _u32(code, pc + 1, pc)
        if n > 32:
            raise BadOperand(pc, "path point count %d > 32" % n)
        _need(code, pc + 5, 4 * n, pc)
        vals = list(struct.unpack_from("<%dI" % n, code, pc + 5))
        return Ins(pc, op, "SOURCEPLAYER_PATH", (("n", n),), pc + 5 + 4 * n,
                   "path", data=vals)
    if op == 188:                                    # ADD_LIFT
        _need(code, pc + 1, 0x94 + 4, pc)
        head = code[pc + 1:pc + 1 + 0x94]
        j = pc + 1 + 0x94
        n = _u32(code, j, pc)
        if n > 31:
            raise BadOperand(pc, "lift stop count %d > 31" % n)
        _need(code, j + 4, 12 * n, pc)
        stops = [struct.unpack_from("<3I", code, j + 4 + 12 * k)
                 for k in range(n)]
        return Ins(pc, op, "ADD_LIFT", (("n", n),), j + 4 + 12 * n, "lift",
                   data=(head, stops))
    if op == 199:                                    # IF
        _need(code, pc + 1, 1, pc)
        cid = code[pc + 1]
        cname, sig = sbtvm.CONDITIONS.get(cid, ("OP_%d" % cid, ""))
        cargs, i = _args(code, pc + 2, sig or "", pc)
        size = _u32(code, i, pc)
        body = i + 4
        end = i + size
        if size < 4 or end > len(code):
            raise BadOperand(pc, "IF body size 0x%x at 0x%x" % (size, i))
        return Ins(pc, op, "IF", (), end, "if", cname, tuple(cargs), body, end)
    if op == 200:                                    # ELSE / relative jump
        rel = _u32(code, pc + 1, pc)
        return Ins(pc, op, "ELSE", (("rel", rel),), pc + 1, "else",
                   target=pc + 1 + rel)
    if op == 0xC9:                                   # EVAL <condition>
        _need(code, pc + 1, 1, pc)
        cid = code[pc + 1]
        cname, sig = sbtvm.CONDITIONS.get(cid, ("OP_%d" % cid, ""))
        cargs, i = _args(code, pc + 2, sig or "", pc)
        return Ins(pc, op, "EVAL", (), i, "eval", cname, tuple(cargs))
    cmd = sbtvm.COMMANDS.get(op)
    if cmd is None:
        return Ins(pc, op, "OP_%d" % op, (), pc + 1, "unknown",
                   error="no command table entry")
    name, sig = cmd
    args, i = _args(code, pc + 1, sig or "", pc)
    return Ins(pc, op, name, tuple(args), i, "cmd")


# --------------------------------------------------------------------------
# command classification


#: structural operators (no COMMANDS table entry; see docs/FORMATS.md 12.3)
STRUCTURAL = {103, 158, 170, 171, 172, 175, 188}

FLOW = {
    "EXIT_EVENT", "EXIT_PROGRAM", "SLEEP", "GENERATE",
    "NEW_GAME", "NEW_LEVEL", "NEXT_LEVEL", "END_GAME",
}

#: commands that place an object at an absolute world position
SCENE = {
    "ADD_SKY_LIGHT_SOURCE", "ADD_LIGHT_SOURCE", "ADD_LOCATION",
    "ADD_ITEM", "ADD_ITEM_FIXED", "ADD_SOUND_SOURCE", "ADD_SPRITE",
    "ADD_DOOR", "ADD_SWITCH", "SET_PLAYER_START", "SET_DEATHMATCH_START",
    "ADD_NAMED_ITEM", "ADD_NAMED_ITEM_EX", "DELETE_LOCATION",
}

#: commands that place an object relative to a runtime source (no position)
ATTACHED = {
    "ADD_ITEM_AT_SOURCE", "ADD_ITEM_AT_SOURCE_FIXED",
    "ADD_SPRITE_AT_SOURCE", "ADD_SPRITE_AT_SOURCE_WEAPON_POINT",
    "ADD_SPRITE_MOVING", "ADD_SPRITE_MOVING_AT_SOURCE",
    "ADD_SPRITE_MOVING_AT_SOURCEPLAYER",
    "ADD_SPRITE_MOVING_AT_SOURCE_WEAPON_POINT",
    "ADD_SOURCEPLAYER_SPRITE", "ADD_RANDOM_AMMO_ITEM_AT_SOURCE",
}

#: commands whose effect is implemented on the VM state
STATE = {
    "ENABLE", "DISABLE", "SOURCEPLAYER_ENABLE", "SOURCEPLAYER_DISABLE",
    "OR_LEVEL_OBJECTIVES_BITS",
    "AND_SOURCEPLAYER_KEY_BITS", "OR_SOURCEPLAYER_KEY_BITS",
    "AND_SOURCEPLAYER_WEAPON_BITS", "OR_SOURCEPLAYER_WEAPON_BITS",
    "AND_SOURCEPLAYER_EQUIPMENT_BITS", "OR_SOURCEPLAYER_EQUIPMENT_BITS",
    "AND_SOURCEPLAYER_BEHAVIOUR_BITS", "OR_SOURCEPLAYER_BEHAVIOUR_BITS",
    "AND_SOURCEPLAYER_EXTRA_BITS", "OR_SOURCEPLAYER_EXTRA_BITS",
    "ADD_SOURCEPLAYER_AMMO", "ADD_SOURCEPLAYER_CASH",
    "ADD_SOURCEPLAYER_SALVAGE", "ADJUST_SOURCEPLAYER_MORALE",
    "HEAL_SOURCEPLAYER", "REPAIR_SOURCEPLAYER",
    "INCREMENT_SOURCEPLAYER_SECRET_TALLY",
    "SOURCEPLAYER_EQUIPMENT_ACTIVATE", "SOURCEPLAYER_EQUIPMENT_INDEX",
    "SOURCEPLAYER_WEAPON_INDEX", "SOURCEPLAYER_TYPE", "SET_SOURCEPLAYER_TYPE",
    "SOURCEPLAYER_MOVEMENT_TYPE",
    "SET_LEVEL_NAME", "SET_LEVEL_GRAVITY", "SET_LEVEL_FOG_COLOUR",
    "SET_LEVEL_FOG_DISTANCE", "SET_LEVEL_COUNTDOWN_TIMER",
    "SET_LEVEL_FOCAL_ADJUST", "SET_LEVEL_ALIASING_VALUES",
    "SET_LEVEL_VERSION", "SET_LEVEL_BEST_TIME", "SET_LEVEL_SECRETS",
    "SET_LEVEL_END_CAMERA_XYZ",
    "DEFINE_SHIELDS", "DEFINE_VIRUS", "DEFINE_REGENERATION_MARKER",
    "SOURCE_CLEAR", "SOURCEPLAYER_KILL",
}

#: presentation / world-mutation handlers: recorded, not applied to the state
#: model (they touch audio, the screen, the camera, the film player, a sector or
#: polygon of the loaded level -- never the entity table / player variables the
#: conditions read; see docs/VM_EXEC.md for the spot checks)
EVENT = {
    "SET_SECTOR_MOVEMENT", "SET_SECTOR_BRIGHTNESS", "SET_SECTOR_TYPE",
    "SET_SECTOR_DAMAGE", "SET_SECTOR_SOUND", "SET_SECTOR_EVENTS",
    "SET_SECTOR_FLAGS", "SET_POLY_TEXTURE", "SET_POLY_BRIGHTNESS",
    "SET_POLY_SCROLL", "LOG_MESSAGE", "PLAY_SAMPLE",
    "PLAY_SAMPLE_AT_LOCATION", "SPEAK_NUMBER", "SPEAK_SAMPLE",
    "SET_SOURCEWORLDOBJECT_HIT_OFF", "SET_SOURCEWORLDOBJECT_HIT_ON",
    "SET_SOURCEWORLDOBJECT_TEXTURES", "SET_SOURCESECTOR_MOVEMENT",
    "SET_SOURCESECTOR_BRIGHTNESS", "SET_SOURCESECTOR_FLAGS",
    "SET_SOURCESECTOR_TYPE", "SET_SOURCESECTOR_DAMAGE",
    "SET_SOURCEPOLY_TEXTURE", "SET_SOURCEPOLY_BRIGHTNESS",
    "SET_SOURCEPOLY_MATERIAL", "SET_SOURCEPOLY_SCROLL",
    "SET_SOURCEPOLY_SWITCH_EVENT", "PLAY_SAMPLE_AT_SOURCE",
    "PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE", "REMOVE_SOURCE_ITEM",
    "SET_HANDLESECTOR_MOVEMENT", "SET_HANDLESECTOR_BRIGHTNESS",
    "SET_HANDLESECTOR_TYPE", "SET_HANDLESECTOR_DAMAGE",
    "SET_HANDLESECTOR_FLAGS", "SET_HANDLESECTOR_SOUND",
    "SET_HANDLEPOLY_TEXTURE", "SET_HANDLEPOLY_BRIGHTNESS",
    "SET_HANDLEPOLY_SCROLL", "SET_HANDLEPOLY_MATERIAL",
    "ADD_SCREEN_TEXT", "ADD_SOURCESCREEN_TEXT", "SET_DOOR_DELTAS",
    "SET_DOOR_POSITION", "SET_DOOR_SOUNDS", "SET_DOOR_SPEED",
    "ITEMS_MOVE_CHECK", "SET_SOURCEITEM_TRIGGER_EVENT", "SOURCE_CLEAR",
    "SET_CAMERA_PITCH_SHAKE", "SET_CAMERA_ROLL_SHAKE", "SET_CAMERA_YAW_SHAKE",
    "SET_CAMERA_HAZE", "LOAD_MUSIC", "PLAY_MUSIC", "STOP_MUSIC",
    "UNLOAD_MUSIC", "MEMORIZE_CURRENT_MUSIC_BANK",
    "PLAY_MEMORIZED_MUSIC_BANK", "SET_SECTOR_LINK", "SHOW_LEVEL_END_SCREEN",
    "INCREMENT_SOURCEPLAYER_SECRET_TALLY", "SET_LEVEL_COUNTDOWN_TIMER",
    "FREEZE", "REFRESH_SCREEN", "FILM_DEFINE", "FILM_PLAY", "FILM_STOP",
    "FILM_REWIND", "SHOW_AMMO_BUY_SCREEN", "SHOW_ITEM_BUY_SCREEN",
    "SHOW_TANK_BUY_SCREEN", "SHOW_TANK_SAVE_SCREEN",
    "SHOW_WEAPON_BUY_SCREEN", "SHOW_BRIEFING_SCREEN", "SHOW_HELP_SCREEN",
    "STOP_SOURCEPLAYER_SLEEPING_SCRIPTS",
    "SET_SOURCEPLAYER_TYPE",
    "GENERATE_FOR_CHILDREN", "GENERATE_FOR_PARENT",
    # gameplay effects on the source player: not modelled, no control flow
    "CHANGE_SOURCEPLAYER_SOB", "SOURCEPLAYER_TURBO_BOOST",
    "SOURCEPLAYER_RECOIL", "SOURCEPLAYER_COLLISION_TYPE", "SOURCEPLAYER_HAZE",
    "SOURCEPLAYER_HAZE_BRIGHTNESS", "SOURCEPLAYER_INVINCIBLE",
    "SOURCEPLAYER_INFECT", "SOURCEPLAYER_CURE",
    "SOURCEPLAYER_HIGHEST_WEAPON_WITH_AMMO",
    "SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO",
    "SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO", "SOURCEPLAYER_FLY",
    "SOURCEPLAYER_HEADLIGHT", "SOURCEPLAYER_FOOT_OFFSETS",
    "SOURCEPLAYER_DAMAGE_MODIFIER", "SOURCEPLAYER_DETECTOR",
    "SOURCEPLAYER_TARGETING", "SOURCEPLAYER_TARGETED",
    "SOURCEPLAYER_WAKE", "SOURCEPLAYER_KILL", "SOURCEPLAYER_SELL_ALL",
    "SOURCEPLAYER_FOCAL_ADJUST", "SOURCEPLAYER_STAND_GROUND",
    "SOURCEPLAYER_FALL_PITCH_DOWN", "SOURCEPLAYER_RUB_SPEED_ADJUST",
    "SOURCEPLAYER_PLAY_LOOP", "SOURCEPLAYER_STOP_LOOP",
    "SOURCEPLAYER_STRAFE", "SOURCEPLAYER_Z_STRAFE",
    "SOURCEPLAYER_TARGET_LAG", "SOURCEPLAYER_SLEEP",
    "SOURCEPLAYER_DELETE", "SOURCEPLAYER_MOVE_SPEED",
    "SOURCEPLAYER_STEALTH", "SOURCEPLAYER_TARGETED_MODIFIERS",
    "SOURCEPLAYER_PATH_INDEX", "SOURCEPLAYER_DISABLE", "SOURCEPLAYER_ENABLE",
    "THING_ACCURACY_MODIFIERS", "THING_TARGET_PLAYERS_MODIFIERS",
    "CAUSE_BLAST_RADIUS_DAMAGE", "REMOVE_SOURCEPLAYER_SPRITE",
    "SOURCEPLAYER_WEAPON_INDEX",
}

def command_status(op: int) -> str:
    """``structural|flow|scene|attached|state|event|unsupported|unknown``."""
    if op in STRUCTURAL:
        return "structural"
    cmd = sbtvm.COMMANDS.get(op)
    if cmd is None:
        return "unknown"
    name = cmd[0]
    if name in FLOW:
        return "flow"
    if name in SCENE:
        return "scene"
    if name in ATTACHED:
        return "attached"
    if name in STATE:
        return "state"
    if name in EVENT:
        return "event"
    return "unsupported"


def command_table() -> list[dict]:
    """Every command id with its handler address and prototype status.

    The handler address is entry ``op-1`` of the dispatcher table at
    ``0x45c500`` (``jmp [edx*4+0x45c500]``, ``0x4574d4``).
    """
    out = []
    for op in sorted(set(sbtvm.COMMANDS) | STRUCTURAL):
        if op in STRUCTURAL:
            name, sig = (sbtvm.TABLE_NAMES.get(op) or
                         ("SOURCEPLAYER_PATH" if op == 158 else
                          "ADD_LIFT" if op == 188 else
                          "DEFINE_LEVEL_OBJECTIVES"), "")
        else:
            name, sig = sbtvm.COMMANDS[op]
        out.append({"op": op, "name": name, "sig": sig or "",
                    "status": command_status(op),
                    "handler": _handler(op)})
    return out


#: dispatcher jump table at ``0x45c500`` (202 dwords, indexed by ``op - 1``);
#: dumped with ``tools/re/exelib.py``.  Opcode 201 (0xc9 / EVAL) never reaches
#: the table -- the loop handles it before the dispatch (``0x4573db``) -- and its
#: slot points at the "unknown opcode" error path.
HANDLERS = {
    1: 0x45c4f9, 2: 0x4575a3, 3: 0x4575d3, 4: 0x4576b1, 5: 0x457c67, 6: 0x457c30,
    7: 0x457729, 8: 0x457815, 9: 0x4578d6, 10: 0x457a1c, 11: 0x457961, 12: 0x4579ee,
    13: 0x4579ab, 14: 0x457929, 15: 0x457a40, 16: 0x457a79, 17: 0x457e7e, 18: 0x457ae3,
    19: 0x457585, 20: 0x457d3e, 21: 0x457d7f, 22: 0x457f13, 23: 0x457f2e, 24: 0x457f77,
    25: 0x457f89, 26: 0x457f9b, 27: 0x457fc6, 28: 0x458063, 29: 0x458111, 30: 0x4580a9,
    31: 0x4580cf, 32: 0x458178, 33: 0x4581ab, 34: 0x458140, 35: 0x45821e, 36: 0x4581fb,
    37: 0x457dbb, 38: 0x457df4, 39: 0x457bc0, 40: 0x45829b, 41: 0x4583a6, 42: 0x458429,
    43: 0x458486, 44: 0x4584e7, 45: 0x45833a, 46: 0x45853a, 47: 0x4586d5, 48: 0x4587a1,
    49: 0x458607, 50: 0x457b54, 51: 0x457b82, 52: 0x4599c5, 53: 0x4598e7, 54: 0x459a14,
    55: 0x45994c, 56: 0x45890a, 57: 0x4589a5, 58: 0x458a1c, 59: 0x458ab6, 60: 0x457b39,
    61: 0x457af3, 62: 0x457b11, 63: 0x45758f, 64: 0x457c26, 65: 0x457bf1, 66: 0x4592c2,
    67: 0x459382, 68: 0x45943e, 69: 0x458b2d, 70: 0x458b9e, 71: 0x458c76, 72: 0x458fe5,
    73: 0x4596b4, 74: 0x4598a5, 75: 0x45775b, 76: 0x457749, 77: 0x45b411, 78: 0x45b462,
    79: 0x45b4b3, 80: 0x45b3bf, 81: 0x45b5a0, 82: 0x45b56a, 83: 0x45b612, 84: 0x45b5f4,
    85: 0x45b629, 86: 0x45b63a, 87: 0x45b662, 88: 0x45b6c7, 89: 0x45889b, 90: 0x45b73c,
    91: 0x45c057, 92: 0x45b7ef, 93: 0x45b82a, 94: 0x45b816, 95: 0x45b978, 96: 0x45b7db,
    97: 0x45b7a0, 98: 0x45b7c7, 99: 0x45b83d, 100: 0x45b78c, 101: 0x45b769, 102: 0x45b851,
    103: 0x45bdf0, 104: 0x45be40, 105: 0x45bb0c, 106: 0x45bb50, 107: 0x45bb94, 108: 0x45bbd7,
    109: 0x459ca6, 110: 0x45baaa, 111: 0x45badb, 112: 0x45af95, 113: 0x45b13d, 114: 0x459d99,
    115: 0x459de9, 116: 0x459efe, 117: 0x45a050, 118: 0x459f4d, 119: 0x45a961, 120: 0x45a281,
    121: 0x45a455, 122: 0x45ad04, 123: 0x45a6aa, 124: 0x45a730, 125: 0x45b2bb, 126: 0x45b2e1,
    127: 0x45b303, 128: 0x45a8ee, 129: 0x45ac91, 130: 0x45a816, 131: 0x45a11d, 132: 0x45a775,
    133: 0x45a0aa, 134: 0x45af22, 135: 0x45a195, 136: 0x45a4be, 137: 0x45a670, 138: 0x45abee,
    139: 0x45a557, 140: 0x45bc1a, 141: 0x457c9e, 142: 0x457cef, 143: 0x45a62d, 144: 0x45a015,
    145: 0x45a896, 146: 0x459fd2, 147: 0x45a5bc, 148: 0x45a607, 149: 0x45ad5e, 150: 0x45ae09,
    151: 0x45aeb4, 152: 0x45a32e, 153: 0x45a422, 154: 0x45a3ce, 155: 0x45a387, 156: 0x459e2d,
    157: 0x459ea6, 158: 0x45b325, 159: 0x45b384, 160: 0x45b99a, 161: 0x45b9de, 162: 0x45ba22,
    163: 0x45ba66, 164: 0x45b504, 165: 0x45b547, 166: 0x45b867, 167: 0x45b893, 168: 0x45c2bc,
    169: 0x45c270, 170: 0x45c3fe, 171: 0x45c308, 172: 0x45c33a, 173: 0x45c3a4, 174: 0x45c375,
    175: 0x45c321, 176: 0x45c353, 177: 0x45be5c, 178: 0x45b89d, 179: 0x45c000, 180: 0x45c039,
    181: 0x45c043, 182: 0x45c04d, 183: 0x45b6ff, 184: 0x459b5b, 185: 0x459a78, 186: 0x459ac0,
    187: 0x459c6e, 188: 0x45be8d, 189: 0x45c0ae, 190: 0x45c109, 191: 0x45c157, 192: 0x45c1a5,
    193: 0x45c1f0, 194: 0x45c23e, 195: 0x45c257, 196: 0x45bea6, 197: 0x45bf4f, 198: 0x45b93e,
    199: 0x4574db, 200: 0x45757a, 201: 0x45c4d8, 202: 0x45bfbf,
}


def _handler(op: int) -> str:
    """Dispatcher target for command *op* (empty when not available)."""
    a = HANDLERS.get(op)
    return "0x%06x" % a if a else ""


# --------------------------------------------------------------------------
# state model


@dataclass
class Entity:
    id: int
    flags: int = 0x3FF
    offset: int | None = None

    @property
    def enabled(self) -> bool:
        return (self.flags & 0x1FF) != 0


@dataclass
class Player:
    index: int = 0
    playertype: int = 0
    behaviour: int = 0        # +0x410
    key_bits: int = 0
    weapon_bits: int = 0      # +0x2d8
    equipment_bits: int = 0
    extra_bits: int = 0
    cash: int = 0             # +0x36c
    health: float = 100.0
    shield: float = 0.0
    max_health: float = 100.0
    max_shield: float = 100.0
    ammo: dict = field(default_factory=dict)      # +0x2dc + 4*type
    ammo_max: dict = field(default_factory=dict)
    equipment_active: int = 0  # +0x368
    weapon_index: int = 0      # +0x2d4
    equipment_index: int = 0
    movement_type: int = 0
    salvage: int = 0
    morale: int = 0
    secrets: int = 0


@dataclass
class Light:
    kind: str
    pos: tuple
    rgb: tuple = (255, 255, 255)
    param: int = 0
    extra: tuple = ()


@dataclass
class Location:
    thing_id: int
    pos: tuple
    orientation: tuple = ()


@dataclass
class Item:
    item_id: int
    pos: tuple
    fixed: bool = False
    named: bool = False
    extra: tuple = ()


@dataclass
class Attached:
    """A sprite/item that is positioned by a runtime source."""

    kind: str
    args: tuple = ()
    source: int | None = None


@dataclass
class SoundSource:
    pos: tuple
    sample: int = 0
    volume: int = 0
    pitch: float = 0.0


@dataclass
class Sprite:
    pos: tuple
    jitter: int = 0
    id: int = 0


@dataclass
class Thing:
    entity: int | None
    kind: str
    type_index: int
    pos: tuple | None = None
    angles: tuple = ()
    owner: int = 0


@dataclass
class Door:
    flags: int
    ids: tuple = ()
    angles: tuple = ()
    params: tuple = ()
    values: tuple = ()
    tail: tuple = ()
    toggles: int = 0


@dataclass
class Switch:
    kind: int
    id: int
    radius: int = 0
    pos: tuple = ()
    extra: tuple = ()
    triggers: int = 0


@dataclass
class Lift:
    head: bytes = b""
    stops: tuple = ()


@dataclass
class Spawn:
    index: int
    pos: tuple
    angles: tuple = ()
    kind: str = "campaign"


@dataclass
class Scene:
    lights: list = field(default_factory=list)
    locations: list = field(default_factory=list)
    items: list = field(default_factory=list)
    named_items: list = field(default_factory=list)
    attached: list = field(default_factory=list)
    sounds: list = field(default_factory=list)
    sprites: list = field(default_factory=list)
    things: list = field(default_factory=list)
    doors: list = field(default_factory=list)
    switches: list = field(default_factory=list)
    lifts: list = field(default_factory=list)
    spawns: list = field(default_factory=list)

    def counts(self) -> dict:
        return {k: len(getattr(self, k)) for k in
                ("lights", "locations", "items", "named_items", "attached",
                 "sounds", "sprites", "things", "doors", "switches", "lifts",
                 "spawns")}

    def extend(self, other: "Scene") -> None:
        for k in self.counts():
            getattr(self, k).extend(getattr(other, k))


@dataclass
class State:
    code: bytes
    entities: dict = field(default_factory=dict)       # id -> Entity
    order: list = field(default_factory=list)          # ids in table order
    players: dict = field(default_factory=dict)        # index -> Player
    source_player: int | None = None
    source_entity: int | None = None
    source_alt: int | None = None      # second source id (SOURCE_SWAP)
    game_mode: int = 0            # 0 campaign, 1 dm-normal, 2 dm-classic
    mp_flags: int = 0             # bit0 team, bit1 CTF
    player_count: int = 1
    difficulty: int = 1
    level_number: int = 0
    god_mode: bool = False
    level_name: str = ""
    level: dict = field(default_factory=dict)          # SET_LEVEL_* values
    objectives: list = field(default_factory=list)     # names (DEFINE_*)
    objectives_bits: int = 0
    defs: dict = field(default_factory=dict)           # DEFINE_* record lists
    path: list = field(default_factory=list)
    scene: Scene = field(default_factory=Scene)
    events: list = field(default_factory=list)         # (op, name, args, pc)
    stack: list = field(default_factory=list)          # (return_pc, source)
    active: list = field(default_factory=list)         # active sub-script offsets
    pc: int = 0
    clock: int = 0
    rng: int = 0x12345678
    occ: dict = field(default_factory=dict)            # site -> occurrence
    steps: int = 0
    trace: list = field(default_factory=list)

    def player(self, index: int | None = None) -> Player | None:
        """The source player (or *index*) as a :class:`Player`."""
        i = self.source_player if index is None else index
        if i is None:
            return None
        p = self.players.get(i)
        if p is None:
            p = self.players[i] = Player(index=i)
        return p

    def rnd(self) -> float:
        """``rand() & 0xfff`` scaled by ``1/4095`` (constant ``0x4a2580``).

        ``0x45e965`` does ``rand() & 0xfff``, multiplies by 1/4095 and
        compares ``operand - x > 0``, i.e. ``x < operand``.
        """
        self.rng = (self.rng * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.rng & 0xFFF) / 4095.0


@dataclass
class TraceEvent:
    pc: int
    depth: int
    name: str
    args: tuple = ()
    note: str = ""


@dataclass
class PathResult:
    reason: str
    pc: int
    detail: str = ""
    steps: int = 0
    state: State | None = None
    trace: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.reason in (END, RETURN, PROGRAM_EXIT, LEVEL_TRANSITION)


# --------------------------------------------------------------------------
# helpers for operands


def s32(v) -> int:
    return v - 0x100000000 if isinstance(v, int) and v >= 0x80000000 else v


def _bits_op(kind: str, cur: int, operand: int) -> int:
    return (cur & operand) if kind == "and" else (cur | operand)


#: conditions that mutate the world while they compute a value
ACTION_CONDITIONS = {1, 2, 3, 4, 5, 18, 19, 20, 27, 41, 42, 45, 46,
                     52, 53, 54, 55, 56, 57, 58, 59}

#: conditions whose result is proven from the handler (docs/VM_EXEC.md)
KNOWN_CONDITIONS = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                    17, 23, 24, 25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38,
                    39, 43}

_COND_ID = {name: cid for cid, (name, _sig) in sbtvm.CONDITIONS.items()}


def cond_id(name: str) -> int:
    return _COND_ID.get(name, -1)


# --------------------------------------------------------------------------
# executor


class Executor:
    """Run SCRIPT byte code over a :class:`State`.

    ``on_unknown`` controls a condition whose value the model cannot prove:
    ``stop`` (default) halts the path, ``false``/``true`` assume a branch and
    ``both`` forks the state and explores both.
    """

    def __init__(self, state: State, *, max_steps: int = 200_000,
                 max_time: float = 5.0, max_paths: int = 64,
                 on_unknown: str = "both", follow_generate: bool = True,
                 random_unknown: bool = False,
                 trace: bool = False, trace_limit: int = 5000,
                 deadline=None):
        if on_unknown not in ("stop", "false", "true", "both"):
            raise ValueError("on_unknown must be stop|false|true|both")
        self.st = state
        self.max_steps = max_steps
        self.max_time = max_time
        self.max_paths = max_paths
        self.on_unknown = on_unknown
        self.follow = follow_generate
        self.random_unknown = random_unknown
        self.trace_on = trace
        self.trace_limit = trace_limit
        self.deadline = deadline
        self.notes: list[str] = []
        self.condition_counts: dict = {}
        self.command_counts: dict = {}

    # -- public -----------------------------------------------------------

    def run(self, start: int = 0, source_entity=_UNSET,
            source_player=_UNSET) -> list[PathResult]:
        """Execute from *start*, returning one result per explored path."""
        if self.max_time:
            self.deadline = time.monotonic() + self.max_time
        st = copy.deepcopy(self.st)
        st.pc = start
        if source_entity is not _UNSET:
            st.source_entity = source_entity
        if source_player is not _UNSET:
            st.source_player = source_player
        frontier = [(st, start)]
        results = []
        while frontier:
            st, pc = frontier.pop(0)
            if self._expired():
                results.append(PathResult(DEADLINE, st.pc, "wall clock",
                                          st.steps, st, st.trace))
                continue
            try:
                reason, detail = self._run_path(st, pc, frontier)
                results.append(PathResult(reason, st.pc, detail, st.steps,
                                          st, st.trace))
            except Halt as h:
                st.pc = h.pc
                results.append(PathResult(h.reason, h.pc, h.detail, st.steps,
                                          st, st.trace))
            except BadOperand as e:
                results.append(PathResult(BAD_OPERAND, e.pc, str(e), st.steps,
                                          st, st.trace))
            except BadPC as e:
                results.append(PathResult(BAD_PC, e.pc, str(e), st.steps,
                                          st, st.trace))
        return results

    # -- internals --------------------------------------------------------

    def _expired(self) -> bool:
        return self.deadline is not None and time.monotonic() > self.deadline

    def _trace(self, st: State, ins: Ins, note: str = "") -> None:
        if not self.trace_on or len(st.trace) >= self.trace_limit:
            return
        st.trace.append(TraceEvent(ins.pc, len(st.stack), ins.name,
                                   ins.args, note))

    def _note(self, msg: str) -> None:
        if msg not in self.notes:
            self.notes.append(msg)

    def _run_path(self, st: State, pc: int, frontier: list):
        code = st.code
        while True:
            st.steps += 1
            if st.steps > self.max_steps:
                raise Halt(STEP_LIMIT, pc, "%d steps" % st.steps)
            if self._expired():
                raise Halt(DEADLINE, pc, "wall clock")
            ins = decode_at(code, pc)
            st.pc = pc
            self._trace(st, ins)
            if ins.kind == "end":
                if st.stack:
                    ret, src = st.stack.pop()
                    st.active.pop()
                    st.source_entity = src
                    pc = ret
                    continue
                return END, ""
            if ins.kind == "if":
                pc = self._do_if(st, ins, pc, frontier)
                continue
            if ins.kind == "else":
                if ins.target < 0 or ins.target > len(code):
                    raise Halt(BAD_PC, ins.pc,
                               "ELSE target 0x%x" % ins.target)
                pc = ins.target
                continue
            if ins.kind == "eval":
                self._eval(st, ins, as_if=False)
                pc = ins.next
                continue
            if ins.kind == "unknown":
                raise Halt(UNKNOWN_OPCODE, pc,
                           "opcode 0x%02x at 0x%x" % (ins.op, ins.pc))
            if ins.kind in ("table", "objectives", "path", "lift"):
                self._structural(st, ins)
                pc = ins.next
                continue
            # plain command
            self.command_counts[ins.op] = self.command_counts.get(ins.op, 0) + 1
            status = command_status(ins.op)
            if status == "unsupported":
                raise Halt(UNSUPPORTED_COMMAND, pc,
                           "%s (op %d)" % (ins.name, ins.op))
            handler = self._cmd_handlers().get(ins.op)
            if handler is None:
                raise Halt(UNSUPPORTED_COMMAND, pc,
                           "%s (op %d) has no handler" % (ins.name, ins.op))
            nxt = handler(st, ins)
            if nxt is None:
                pc = ins.next
            else:
                pc = nxt
            if pc < 0 or pc > len(code):
                raise Halt(BAD_PC, pc, "")

    # -- IF / conditions ---------------------------------------------------

    def _do_if(self, st: State, ins: Ins, pc: int, frontier: list) -> int:
        val = self._eval(st, ins, as_if=True)
        if val is not None:
            return ins.body_start if val else ins.body_end
        if self.on_unknown == "stop":
            raise Halt(UNKNOWN_CONDITION, pc, ins.cond or "?")
        if self.on_unknown == "false":
            self._note("IF %s assumed false at 0x%x" % (ins.cond, pc))
            return ins.body_end
        if self.on_unknown == "true":
            self._note("IF %s assumed true at 0x%x" % (ins.cond, pc))
            return ins.body_start
        # both: explore the else path from a clone, continue with the then path
        if len(frontier) + 1 >= self.max_paths:
            self._note("IF %s fork capped at 0x%x (took false)" % (ins.cond, pc))
            return ins.body_end
        alt = copy.deepcopy(st)
        alt.pc = ins.body_end
        self._trace(alt, ins, "fork: else")
        frontier.append((alt, ins.body_end))
        self._trace(st, ins, "fork: then")
        return ins.body_start

    def _eval(self, st: State, ins: Ins, as_if: bool):
        cid = cond_id(ins.cond or "")
        cname = ins.cond or "?"
        self.condition_counts[cname] = self.condition_counts.get(cname, 0) + 1
        args = ins.cond_args
        if cid in ACTION_CONDITIONS:
            self._action(st, cid, cname, args, ins)
            if cid in (1, 2, 3, 4, 5, 27):
                return True
            return None
        if cid not in KNOWN_CONDITIONS:
            self._note("unknown condition %s at 0x%x" % (cname, ins.pc))
            if not as_if:
                raise Halt(UNKNOWN_CONDITION, ins.pc, cname)
            return None
        try:
            return self._check(st, cid, args)
        except (IndexError, KeyError, TypeError) as exc:
            self._note("cannot evaluate %s at 0x%x: %s" % (cname, ins.pc, exc))
            if as_if:
                return None
            raise Halt(UNKNOWN_CONDITION, ins.pc, cname)

    def _check(self, st: State, cid: int, a) -> bool | None:
        p = st.player()
        if cid == 6:                                   # GAME_DIFFICULTY_AT_LEAST
            return st.difficulty >= s32(a[0])
        if cid == 7:                                   # LEVEL_OBJECTIVES_COMPLETE
            if not st.objectives:
                return False
            return (st.objectives_bits & ((1 << len(st.objectives)) - 1)) == \
                ((1 << len(st.objectives)) - 1)
        if cid == 8:                                   # NEXT_LEVEL_NUMBER
            return st.level_number == s32(a[0])
        if cid == 9:                                   # SOURCE_IS_PLAYER
            return st.source_player is not None
        if cid == 10:                                  # SOURCE_IS_LOCAL_PLAYER_ONE
            return st.source_player == 0
        if cid == 11:                                  # SOURCE_IS_LOCAL_PLAYER_TWO
            return st.source_player == 1
        if cid == 12:                                  # DEATHMATCH_GAME
            return st.game_mode in (1, 2)
        if cid == 13:                                  # DEATHMATCH_CLASSIC_GAME
            return st.game_mode == 2
        if cid == 14:                                  # DEATHMATCH_NORMAL_GAME
            return st.game_mode == 1
        if cid == 15:                                  # MULTIPLAYER_GAME
            return st.game_mode != 0 or st.player_count > 1
        if cid == 16:                                  # CAPTURE_THE_FLAG_GAME
            return bool(st.mp_flags & 2)
        if cid == 17:                                  # TEAM_GAME
            return bool(st.mp_flags & 1)
        if cid == 23:                                  # PLAYERS_AT_LEAST
            return st.player_count >= s32(a[0])
        if cid == 24:                                  # RANDOM
            if self.random_unknown:
                return None
            return st.rnd() < float(a[0])
        if cid == 25:                                  # RANDOM_FRAME_BALANCED
            if self.random_unknown:
                return None
            return st.rnd() < float(a[0])
        if cid == 26:                                  # GOD_MODE
            return st.god_mode
        if cid == 28:                                  # SCAVENGERHUNT_GAME
            return st.game_mode == 5
        # every SOURCEPLAYER_* check below reads the current source player;
        # with no source (a level-level script) the value is genuinely open
        if p is None:
            return None
        if cid == 30:                                  # SOURCEPLAYER_KEY_BITS_SET
            return bool(p.key_bits & s32(a[0]))
        if cid == 31:                                  # SOURCEPLAYER_WEAPON_BITS_SET
            return bool(p.weapon_bits & s32(a[0]))
        if cid == 33:                                  # SOURCEPLAYER_AMMO_AT_LEAST
            return p.ammo.get(s32(a[0]), 0) >= s32(a[1])
        if cid == 34:                                  # SOURCEPLAYER_CASH_AT_LEAST
            return p.cash >= s32(a[0])
        if cid == 36:                                  # SOURCEPLAYER_IS
            return st.source_player == s32(a[0])
        if cid == 37:                                  # SOURCEPLAYER_SHIELD_FULL
            return p.shield >= p.max_shield
        if cid == 38:                                  # SOURCEPLAYER_LIFE_AT_LEAST
            return p.health >= float(a[0])
        if cid == 39:                                  # SOURCEPLAYER_LIFE_FULL
            return p.health >= p.max_health
        if cid == 43:                                  # SOURCEPLAYER_TYPE
            return p.playertype == s32(a[0])
        return None

    # -- action conditions -------------------------------------------------

    def _action(self, st: State, cid: int, name: str, a, ins: Ins) -> None:
        if cid in (1, 2, 3, 4, 5):
            self._thing(st, name, a, ins)
            self._event(st, ins, name)
            return
        if cid == 27:                                  # SOURCE_SWAP
            # 0x45d1b3 rotates the two 16-bit halves of one source descriptor
            # and returns whether the new low half is a valid object; modelled
            # as swapping the current and alternate source entity
            st.source_entity, st.source_alt = (st.source_alt,
                                               st.source_entity)
            self._event(st, ins, name, "source swapped")
            return
        if cid in (18, 19, 20):                        # FIRE_*
            self._event(st, ins, name, "projectile spawn not simulated")
            return
        if cid == 41:                                  # TELEPORT_HANDLE
            self._event(st, ins, name, "handle teleport")
            return
        if cid == 42:                                  # SOURCEPLAYER_TELEPORT
            self._event(st, ins, name, "player teleport")
            return
        if cid == 45:                                  # SOURCEPLAYER_WARP
            self._event(st, ins, name, "player warp")
            return
        if cid in (46, 52, 53, 54, 55, 56, 57, 58, 59):
            self._event(st, ins, name, "custom/engine action")
            return
        self._event(st, ins, name)

    # -- scene creation ----------------------------------------------------

    def _site(self, st: State, ins: Ins) -> tuple[int, int]:
        site = ins.pc
        occ = st.occ.get(site, 0)
        st.occ[site] = occ + 1
        return site, occ

    def _add(self, st: State, ins: Ins, category: str, obj):
        """Append a created object, tagging it with its creation site."""
        site, occ = self._site(st, ins)
        try:
            obj.site = site
            obj.occ = occ
        except AttributeError:               # pragma: no cover - dataclasses
            pass
        getattr(st.scene, category).append(obj)

    def _event(self, st: State, ins: Ins, name: str | None = None,
               note: str = "") -> None:
        if len(st.events) < 200_000:
            st.events.append((ins.pc, name or ins.name, ins.args, note))

    def _thing(self, st: State, name: str, a, ins: Ins) -> None:
        sig = sbtvm.CONDITIONS.get(cond_id(name), (None, ""))[1] or ""
        ti = None
        owner = 0
        ints: list = []
        dbls: list = []
        for ch, v in zip(sig, a):
            if ch == "G":
                ti = s32(v)
            elif ch in "ABD":
                dbls.append(float(v))
            elif isinstance(v, int):
                if ti is None and not ints:
                    owner = s32(v)
                else:
                    ints.append(s32(v))
        if ti is None:
            self._event(st, ins, name, "thing without a type operand")
            return
        absolute = name in ("ADD_THING", "ADD_THING_EX")
        pos = tuple(ints[:3]) if absolute and len(ints) >= 3 else None
        self._add(st, ins, "things",
                  Thing(st.source_entity, name, ti, pos, tuple(dbls), owner))

    def _structural(self, st: State, ins: Ins) -> None:
        name = ins.name
        if name in sbtvm.TABLE_NAMES.values():
            st.defs[name] = ins.data
            if name == "DEFINE_AMMO":
                # handler 0x459d14: ammo[type] is clamped to the record's
                # second value (array 0x818960, stride 0x38, +4)
                for n, (_strs, vals) in enumerate(ins.data):
                    if len(vals) > 1:
                        for pl in st.players.values():
                            pl.ammo_max[n] = s32(vals[1])
            return
        if name == "DEFINE_LEVEL_OBJECTIVES":
            st.objectives = list(ins.data)
            return
        if name == "SOURCEPLAYER_PATH":
            st.path = list(ins.data)
            return
        if name == "ADD_LIFT":
            head, stops = ins.data
            self._add(st, ins, "lifts",
                      Lift(bytes(head), tuple(tuple(r) for r in stops)))
            return

    # -- command handlers --------------------------------------------------

    def _cmd_handlers(self):
        if not hasattr(self, "_handlers"):
            self._handlers = {
                _op("EXIT_EVENT"): self._h_exit_event,
                _op("EXIT_PROGRAM"): self._h_exit_program,
                _op("SLEEP"): self._h_sleep,
                _op("GENERATE"): self._h_generate,
                _op("NEW_GAME"): self._h_transition,
                _op("NEW_LEVEL"): self._h_transition,
                _op("NEXT_LEVEL"): self._h_transition,
                _op("END_GAME"): self._h_transition,
                _op("ENABLE"): self._h_enable,
                _op("DISABLE"): self._h_disable,
                _op("SOURCEPLAYER_ENABLE"): self._h_sp_enable,
                _op("SOURCEPLAYER_DISABLE"): self._h_sp_disable,
                _op("OR_LEVEL_OBJECTIVES_BITS"): self._h_or_objectives,
                _op("ADD_SKY_LIGHT_SOURCE"): self._h_sky,
                _op("ADD_LIGHT_SOURCE"): self._h_light,
                _op("ADD_LOCATION"): self._h_location,
                _op("DELETE_LOCATION"): self._h_delete_location,
                _op("ADD_ITEM"): self._h_item,
                _op("ADD_ITEM_FIXED"): self._h_item,
                _op("ADD_NAMED_ITEM"): self._h_named_item,
                _op("ADD_NAMED_ITEM_EX"): self._h_named_item_ex,
                _op("ADD_SOUND_SOURCE"): self._h_sound,
                _op("ADD_SPRITE"): self._h_sprite,
                _op("ADD_DOOR"): self._h_door,
                _op("ADD_SWITCH"): self._h_switch,
                _op("SET_PLAYER_START"): self._h_spawn,
                _op("SET_DEATHMATCH_START"): self._h_spawn,
                _op("SET_LEVEL_NAME"): self._h_level,
                _op("SET_LEVEL_GRAVITY"): self._h_level,
                _op("SET_LEVEL_FOG_COLOUR"): self._h_level,
                _op("SET_LEVEL_FOG_DISTANCE"): self._h_level,
                _op("SET_LEVEL_COUNTDOWN_TIMER"): self._h_level,
                _op("SET_LEVEL_FOCAL_ADJUST"): self._h_level,
                _op("SET_LEVEL_ALIASING_VALUES"): self._h_level,
                _op("SET_LEVEL_VERSION"): self._h_level,
                _op("SET_LEVEL_BEST_TIME"): self._h_level,
                _op("SET_LEVEL_SECRETS"): self._h_level,
                _op("SET_LEVEL_END_CAMERA_XYZ"): self._h_level,
                _op("ADD_SOURCEPLAYER_AMMO"): self._h_add_ammo,
                _op("ADD_SOURCEPLAYER_CASH"): self._h_cash,
                _op("HEAL_SOURCEPLAYER"): self._h_heal,
                _op("REPAIR_SOURCEPLAYER"): self._h_heal,
                _op("SOURCEPLAYER_EQUIPMENT_ACTIVATE"): self._h_equip,
                _op("SOURCEPLAYER_EQUIPMENT_INDEX"): self._h_index,
                _op("SOURCEPLAYER_WEAPON_INDEX"): self._h_index,
                _op("SET_SOURCEPLAYER_TYPE"): self._h_playertype,
                _op("SOURCEPLAYER_MOVEMENT_TYPE"): self._h_movement,
                _op("ADD_SOURCEPLAYER_SALVAGE"): self._h_salvage,
                _op("ADJUST_SOURCEPLAYER_MORALE"): self._h_morale,
                _op("INCREMENT_SOURCEPLAYER_SECRET_TALLY"): self._h_secret,
                _op("DEFINE_SHIELDS"): self._h_def,
                _op("DEFINE_VIRUS"): self._h_def,
                _op("DEFINE_REGENERATION_MARKER"): self._h_def,
                _op("SOURCE_CLEAR"): self._h_source_clear,
                _op("SOURCEPLAYER_KILL"): self._h_kill,
            }
            for op, name in self._bit_commands().items():
                self._handlers[op] = self._h_bits
            for op in list(sbtvm.COMMANDS):
                st = command_status(op)
                if st == "attached":
                    self._handlers.setdefault(op, self._h_attached)
                elif st == "event":
                    self._handlers.setdefault(op, self._h_event)
            # every op still marked "state" must have an explicit handler
            for op in list(sbtvm.COMMANDS):
                if (command_status(op) == "state"
                        and op not in self._handlers):
                    raise AssertionError("state op %d has no handler" % op)
        return self._handlers

    def _bit_commands(self) -> dict:
        out = {}
        for op, (name, _sig) in sbtvm.COMMANDS.items():
            if name in STATE and ("BITS" in name):
                out[op] = name
        return out

    # flow
    def _h_exit_event(self, st, ins):
        if st.stack:
            ret, src = st.stack.pop()
            st.active.pop()
            st.source_entity = src
            return ret
        raise Halt(END, ins.pc, "EXIT_EVENT at depth 0")

    def _h_exit_program(self, st, ins):
        raise Halt(PROGRAM_EXIT, ins.pc, "EXIT_PROGRAM")

    def _h_sleep(self, st, ins):
        st.clock += s32(ins.args[0])
        self._event(st, ins, note="sleep %d" % s32(ins.args[0]))
        return None

    def _h_transition(self, st, ins):
        raise Halt(LEVEL_TRANSITION, ins.pc,
                   "%s %r" % (ins.name, ins.args))

    def _h_unsupported(self, st, ins):
        raise Halt(UNSUPPORTED_COMMAND, ins.pc, ins.name)

    def _h_generate(self, st, ins):
        gid = s32(ins.args[0])
        for d in st.scene.doors:
            if gid in [s32(v) for v in d.ids]:
                d.toggles += 1
        ent = st.entities.get(gid)
        self._event(st, ins, note="generate id=%d%s" % (
            gid, "" if ent else " (no entity)"))
        if not self.follow or ent is None or ent.offset is None:
            return None
        off = ent.offset
        if off < 0 or off >= len(st.code):
            raise Halt(BAD_PC, ins.pc, "entity offset 0x%x" % off)
        if off in st.active:
            raise Halt(RECURSION, ins.pc,
                       "GENERATE %d re-enters offset 0x%x" % (gid, off))
        if len(st.stack) >= MAX_DEPTH:
            raise Halt(STACK_OVERFLOW, ins.pc, "depth %d" % len(st.stack))
        st.stack.append((ins.next, st.source_entity))
        st.active.append(off)
        st.source_entity = gid
        return off

    # entity flags
    def _h_enable(self, st, ins):
        e = st.entities.get(s32(ins.args[0]))
        if e:
            e.flags |= 0x1FF
        self._event(st, ins)
        return None

    def _h_disable(self, st, ins):
        e = st.entities.get(s32(ins.args[0]))
        if e:
            e.flags &= ~0x1FF
        self._event(st, ins)
        return None

    def _sp_bit(self, st):
        if st.source_player is None or not 0 <= st.source_player <= 7:
            return 0x100
        return 1 << (9 + st.source_player)

    def _h_sp_enable(self, st, ins):
        e = st.entities.get(s32(ins.args[0]))
        if e:
            e.flags |= self._sp_bit(st)
        self._event(st, ins)
        return None

    def _h_sp_disable(self, st, ins):
        e = st.entities.get(s32(ins.args[0]))
        if e:
            e.flags &= ~self._sp_bit(st)
        self._event(st, ins)
        return None

    def _h_or_objectives(self, st, ins):
        st.objectives_bits |= s32(ins.args[0])
        return None

    # scene
    def _h_sky(self, st, ins):
        a = ins.args
        rgb = tuple(int(a[k]) & 0xFF for k in (4, 5, 6)) if len(a) >= 7 \
            else (255, 255, 255)
        self._add(st, ins, "lights",
                  Light("sky", tuple(s32(a[k]) for k in range(3)), rgb,
                        s32(a[3]) if len(a) > 3 else 0))

    def _h_light(self, st, ins):
        a = ins.args
        rgb = tuple(int(a[k]) & 0xFF for k in (4, 5, 6)) if len(a) >= 7 \
            else (255, 255, 255)
        self._add(st, ins, "lights",
                  Light("local", tuple(s32(a[k]) for k in range(3)), rgb,
                        s32(a[3]) if len(a) > 3 else 0,
                        tuple(s32(v) for v in a[9:12])))

    def _h_location(self, st, ins):
        a = ins.args
        self._add(st, ins, "locations",
                  Location(s32(a[0]), tuple(s32(a[k]) for k in (1, 2, 3)),
                           tuple(float(v) for v in a[4:7])))

    def _h_delete_location(self, st, ins):
        tid = s32(ins.args[0])
        st.scene.locations = [x for x in st.scene.locations
                              if x.thing_id != tid]
        self._event(st, ins)

    def _h_item(self, st, ins):
        a = ins.args
        self._add(st, ins, "items",
                  Item(s32(a[1]), tuple(s32(a[k]) for k in (5, 6, 7)),
                       ins.name == "ADD_ITEM_FIXED", False,
                       tuple(s32(v) for v in a[:5])))

    def _h_named_item(self, st, ins):
        a = ins.args
        self._add(st, ins, "named_items",
                  Item(s32(a[0]), tuple(s32(a[k]) for k in (1, 2, 3)),
                       False, True, tuple(a[4:7])))

    def _h_named_item_ex(self, st, ins):
        a = ins.args
        if len(a) < 10:
            raise Halt(BAD_OPERAND, ins.pc, "ADD_NAMED_ITEM_EX")
        self._add(st, ins, "named_items",
                  Item(s32(a[3]), tuple(s32(a[k]) for k in (4, 5, 6)),
                       False, True, tuple(a[7:10])))

    def _h_sound(self, st, ins):
        a = ins.args
        self._add(st, ins, "sounds",
                  SoundSource(tuple(s32(a[k]) for k in range(3)),
                              s32(a[3]) if len(a) > 3 else 0,
                              s32(a[4]) if len(a) > 4 else 0,
                              float(a[5]) if len(a) > 5 else 0.0))

    def _h_sprite(self, st, ins):
        a = ins.args
        self._add(st, ins, "sprites",
                  Sprite((float(a[1]), float(a[2]), float(a[3])),
                         s32(a[8]), s32(a[0])))

    def _h_door(self, st, ins):
        a = ins.args
        self._add(st, ins, "doors", Door(
            s32(a[0]), tuple(s32(v) for v in a[1:6]),
            tuple(float(v) for v in a[6:9]), tuple(s32(v) for v in a[9:16]),
            tuple(float(v) for v in a[16:25]),
            tuple(s32(v) for v in a[25:31])))

    def _h_switch(self, st, ins):
        a = ins.args
        self._add(st, ins, "switches", Switch(
            s32(a[0]), s32(a[1]), s32(a[2]),
            tuple(s32(v) for v in a[3:6]), tuple(s32(v) for v in a[6:8])))

    def _h_spawn(self, st, ins):
        a = ins.args
        if len(a) < 7:
            raise Halt(BAD_OPERAND, ins.pc, ins.name)
        self._add(st, ins, "spawns", Spawn(
            s32(a[0]), tuple(s32(a[k]) for k in (1, 2, 3)),
            tuple(float(a[k]) for k in (4, 5, 6)),
            "campaign" if ins.name == "SET_PLAYER_START" else "deathmatch"))

    def _h_attached(self, st, ins):
        self._add(st, ins, "attached",
                  Attached(ins.name, ins.args, st.source_entity))

    def _h_event(self, st, ins):
        self._event(st, ins)

    # state
    def _h_level(self, st, ins):
        st.level[ins.name] = ins.args
        if ins.name == "SET_LEVEL_NAME" and ins.args:
            st.level_name = str(ins.args[0])
        if ins.name == "SET_LEVEL_COUNTDOWN_TIMER" and ins.args:
            st.level["countdown"] = ins.args[0]
        return None

    def _h_add_ammo(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        atype, delta = s32(ins.args[0]), s32(ins.args[1])
        val = p.ammo.get(atype, 0) + delta
        if val < 0:
            val = 0
        cap = p.ammo_max.get(atype)
        if cap is not None and val > cap:
            val = cap
        p.ammo[atype] = val
        return None

    def _h_cash(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.cash += s32(ins.args[0])
        return None

    def _h_heal(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        a = ins.args
        amount = s32(a[1]) if len(a) > 1 else s32(a[0])
        if ins.name == "REPAIR_SOURCEPLAYER":
            p.shield = min(p.max_shield, p.shield + amount)
        else:
            p.health = min(p.max_health, p.health + amount)
        return None

    def _h_equip(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        bit, on = s32(ins.args[0]), s32(ins.args[1])
        if bit < 0 or bit > 31:
            return None
        if on:
            p.equipment_active |= 1 << bit
        else:
            p.equipment_active &= ~(1 << bit)
        return None

    def _h_index(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        if ins.name == "SOURCEPLAYER_WEAPON_INDEX":
            p.weapon_index = s32(ins.args[0])
        else:
            p.equipment_index = s32(ins.args[0])
        return None

    def _h_playertype(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.playertype = s32(ins.args[0])
        return None

    def _h_movement(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.movement_type = int(ins.args[0])
        return None

    def _h_salvage(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.salvage += s32(ins.args[0])
        return None

    def _h_morale(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.morale += s32(ins.args[0])
        return None

    def _h_source_clear(self, st, ins):
        # 0x457749: source descriptor = (value & 0xffff0000) | 0xffff
        st.source_entity = None
        st.source_player = None
        self._event(st, ins, note="source cleared")
        return None

    def _h_kill(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.health = 0.0
        return None

    def _h_def(self, st, ins):
        st.defs[ins.name] = ins.args
        if ins.name == "DEFINE_SHIELDS" and ins.args:
            for pl in st.players.values():
                pl.max_shield = float(s32(ins.args[0]))
        return None

    def _h_secret(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        p.secrets += 1
        return None

    def _h_bits(self, st, ins):
        p = st.player()
        if p is None:
            self._event(st, ins, note="no source player")
            return None
        operand = s32(ins.args[0])
        name = ins.name
        if "KEY_BITS" in name:
            p.key_bits = _bits_op("and" if name.startswith("AND") else "or",
                                  p.key_bits, operand)
        elif "WEAPON_BITS" in name:
            p.weapon_bits = _bits_op("and" if name.startswith("AND") else "or",
                                     p.weapon_bits, operand)
        elif "EQUIPMENT_BITS" in name:
            p.equipment_bits = _bits_op(
                "and" if name.startswith("AND") else "or",
                p.equipment_bits, operand)
        elif "BEHAVIOUR_BITS" in name:
            p.behaviour = _bits_op("and" if name.startswith("AND") else "or",
                                   p.behaviour, operand)
        else:
            p.extra_bits = _bits_op("and" if name.startswith("AND") else "or",
                                    p.extra_bits, operand)
        return None


def _op(name: str) -> int:
    for k, v in sbtvm.COMMANDS.items():
        if v[0] == name:
            return k
    raise KeyError(name)


# --------------------------------------------------------------------------
# convenience: build a State from a parsed script


def state_from_script(script, *, game_mode: int = 0, player_count: int = 1,
                      difficulty: int = 1, level_number: int = 0,
                      god_mode: bool = False, players: int = 1,
                      seed: int = 0x12345678, mp_flags: int = 0,
                      playertype: int | None = None,
                      source_player: int | None = None) -> State:
    """A :class:`State` for a ``gm.sbt.Script`` record.

    The entity table is the same three-column block ``gm.scene.entities``
    reads: *flags*, *id*, *offset*.
    """
    st = State(code=bytes(script.code), game_mode=game_mode,
               mp_flags=mp_flags, player_count=player_count,
               difficulty=difficulty, level_number=level_number,
               god_mode=god_mode, rng=seed & 0x7FFFFFFF,
               source_player=source_player)
    for i in range(players):
        st.players[i] = Player(index=i)
        if playertype is not None:
            st.players[i].playertype = playertype
    n = script.n
    if n > 0 and len(script.handles) >= 12 * n:
        cols = struct.unpack_from("<%dI" % (3 * n), script.handles, 0)
        for i in range(n):
            eid = s32(cols[n + i])
            st.entities[eid] = Entity(eid, cols[i], cols[2 * n + i])
            st.order.append(eid)
    return st


def merge_scenes(results: list[PathResult]) -> Scene:
    """Union of every path's scene, deduplicated by ``(site, occurrence)``.

    ``occ`` is the per-path execution count of an instruction offset, so two
    paths that both execute the same site do not double-count it, while a loop
    that executes it twice is preserved.
    """
    seen: dict = {}
    out = Scene()
    for r in results:
        if r.state is None:
            continue
        for cat in out.counts():
            for obj in getattr(r.state.scene, cat):
                key = (cat, getattr(obj, "site", None),
                       getattr(obj, "occ", None))
                if key in seen:
                    continue
                seen[key] = True
                getattr(out, cat).append(obj)
    return out


# --------------------------------------------------------------------------
# selftest


def _enc_cmd(name: str, *args) -> bytes:
    op = _op(name)
    sig = sbtvm.COMMANDS[op][1] or ""
    out = bytearray([op])
    for ch, v in zip(sig, args):
        size = sbtvm.LETTER_SIZE[ch]
        if size == "str":
            out += str(v).encode("latin1") + b"\0"
        elif size == 8:
            out += struct.pack("<d", float(v))
        elif size == 1:
            out += bytes([int(v) & 0xFF])
        else:
            out += struct.pack("<I", int(v) & 0xFFFFFFFF)
    return bytes(out)


def _enc_cond(cid: int, *args) -> bytes:
    sig = sbtvm.CONDITIONS[cid][1] or ""
    out = bytearray([cid])
    for ch, v in zip(sig, args):
        size = sbtvm.LETTER_SIZE[ch]
        if size == "str":
            out += str(v).encode("latin1") + b"\0"
        elif size == 8:
            out += struct.pack("<d", float(v))
        elif size == 1:
            out += bytes([int(v) & 0xFF])
        else:
            out += struct.pack("<I", int(v) & 0xFFFFFFFF)
    return bytes(out)


def enc_if(cid: int, args, then: bytes, otherwise: bytes | None = None) -> bytes:
    """Encode an IF exactly as the compiler does.

    The engine's ``ELSE`` (opcode 200) is an unconditional jump placed at the
    **end of the then-branch** (``0x2900``-``0x2a4a`` in ``dm1``), not a
    separator after it: on a false condition ``pc += size`` lands right after
    that jump, i.e. on the else code.
    """
    head = bytes([199]) + _enc_cond(cid, *args)
    if otherwise is None:
        size = 4 + len(then)
        return head + struct.pack("<I", size) + then
    size = 4 + len(then) + 5
    rel = 4 + len(otherwise)
    return (head + struct.pack("<I", size) + then + bytes([200]) +
            struct.pack("<I", rel) + otherwise)


def _run_bytes(code: bytes, rows=(), **kw) -> list[PathResult]:
    state_keys = {k: kw.pop(k) for k in list(kw)
                  if k in ("difficulty", "game_mode", "player_count",
                           "level_number", "god_mode", "mp_flags")}
    st = State(code=code, rng=kw.pop("seed", 1), **state_keys)
    st.players[0] = Player(index=0)
    for eid, off in rows:
        st.entities[eid] = Entity(eid, 0x3FF, off)
    ex = Executor(st, **kw)
    return ex.run(0)


def selftest() -> list[tuple[str, bool, str]]:
    """Synthetic tests for the executor (no game files needed)."""
    out = []

    def check(name, cond, detail=""):
        out.append((name, bool(cond), detail))

    # 1. IF/ELSE: GAME_DIFFICULTY_AT_LEAST 2, then else
    loc = _enc_cmd("ADD_LOCATION", 1, 10, 20, 30, 0.0, 0.0, 0.0)
    loc2 = _enc_cmd("ADD_LOCATION", 2, 40, 50, 60, 0.0, 0.0, 0.0)
    prog = enc_if(6, [2], loc, loc2) + b"\xff"
    r = _run_bytes(prog, difficulty=1)
    check("if/else: false branch", r[0].reason == END and
          len(r[0].state.scene.locations) == 1 and
          r[0].state.scene.locations[0].thing_id == 2, r[0].reason)
    r = _run_bytes(prog, difficulty=2)
    check("if/else: true branch", r[0].reason == END and
          r[0].state.scene.locations[0].thing_id == 1, r[0].reason)

    # 2. nested IF
    nested = enc_if(6, [2], enc_if(6, [3], loc)) + b"\xff"
    r = _run_bytes(nested, difficulty=1)
    check("nested IF skipped", r[0].reason == END and
          not r[0].state.scene.locations, r[0].reason)
    r = _run_bytes(nested, difficulty=3)
    check("nested IF entered", r[0].reason == END and
          len(r[0].state.scene.locations) == 1, r[0].reason)

    # 3. comparison: SOURCEPLAYER_CASH_AT_LEAST 100
    cash = _enc_cmd("ADD_SOURCEPLAYER_CASH", 100)
    prog = cash + enc_if(34, [50], loc) + cash + enc_if(34, [250], loc2) + b"\xff"
    st = State(code=prog, rng=1)
    st.players[0] = Player(index=0)
    st.source_player = 0
    r = Executor(st).run(0)
    check("comparison: cash condition",
          r[0].reason == END and [x.thing_id for x in r[0].state.scene.locations]
          == [1], str([x.thing_id for x in r[0].state.scene.locations]))

    # 4. unknown condition -> policy stop / both
    r = _run_bytes(enc_if(46, [1.0], loc) + b"\xff", on_unknown="stop")
    check("unknown condition stops", r[0].reason == UNKNOWN_CONDITION,
          r[0].reason)
    r = _run_bytes(enc_if(46, [1.0], loc, loc2) + b"\xff", on_unknown="both")
    check("unknown condition forks", len(r) == 2 and
          all(x.reason == END for x in r), str(len(r)))

    # 5. unknown opcode
    r = _run_bytes(b"\xfe\xff")
    check("unknown opcode stops", r[0].reason == UNKNOWN_OPCODE, r[0].reason)

    # 6. OOB PC (ELSE far beyond the block)
    r = _run_bytes(bytes([200]) + struct.pack("<I", 0x1000) + b"\xff")
    check("OOB pc", r[0].reason == BAD_PC, r[0].reason)

    # 7. bad operand: truncated command operand / unterminated string
    r = _run_bytes(_enc_cmd("LOG_MESSAGE", "x")[:-1])
    check("bad operand: string", r[0].reason == BAD_OPERAND, r[0].reason)
    r = _run_bytes(bytes([_op("GENERATE")]) + b"\x01")
    check("bad operand: dword", r[0].reason == BAD_OPERAND, r[0].reason)

    # 8. recursion via GENERATE self
    gen = _enc_cmd("GENERATE", 7)
    r = _run_bytes(gen + b"\xff", rows=[(7, 0)])
    check("recursion detected", r[0].reason == RECURSION, r[0].reason)

    # 9. stack overflow: a chain of 40 entities, each GENERATEs the next
    code = bytearray()
    offs = {}
    for k in range(40):
        offs[k] = len(code)
        code += _enc_cmd("GENERATE", 101 + k) + b"\xff"
    code += b"\xff"
    rows = [(100 + k, offs[k]) for k in range(40)]
    r = _run_bytes(bytes(code), rows=rows)
    check("stack overflow", r[0].reason == STACK_OVERFLOW, r[0].reason)

    # 10. step limit
    many = _enc_cmd("SLEEP", 1) * 50 + b"\xff"
    r = _run_bytes(many, max_steps=10)
    check("step limit", r[0].reason == STEP_LIMIT, r[0].reason)

    # 11. scene command operands
    prog = _enc_cmd("SET_PLAYER_START", 0, 1, 2, 3, 0.0, 0.0, 0.0) + b"\xff"
    r = _run_bytes(prog)
    sp = r[0].state.scene.spawns[0]
    check("SET_PLAYER_START", sp.index == 0 and sp.pos == (1, 2, 3) and
          sp.kind == "campaign", str(sp))

    # 12. ADD_THING_EX inside EVAL
    prog = (bytes([0xC9]) + _enc_cond(2, 0, 0, 0, 4200, 11, 22, 33,
                                      0.0, 0.0, 0.0, 0) + b"\xff")
    r = _run_bytes(prog)
    th = r[0].state.scene.things[0]
    check("ADD_THING_EX", th.type_index == 4200 and th.pos == (11, 22, 33),
          str(th))

    # 13. entity flags: DISABLE
    prog = _enc_cmd("DISABLE", 9) + b"\xff"
    r = _run_bytes(prog, rows=[(9, 0)])
    # the entity is the running block itself; check the table after the run
    check("DISABLE clears flags", (r[0].state.entities[9].flags & 0x1FF) == 0,
          hex(r[0].state.entities[9].flags))

    return out


__all__ = [
    "Executor", "State", "Scene", "Player", "Entity", "PathResult",
    "decode_at", "command_table", "command_status", "state_from_script",
    "selftest", "VMError", "BadOperand", "BadPC",
]
