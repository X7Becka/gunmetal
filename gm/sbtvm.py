"""Gunmetal ``.sbt`` SCRIPT (record type 11) virtual machine.

The ``SCRIPT`` record's ``code`` block is a byte stream of *instructions*::

    [u8 opcode][operands]      ...      0xFF = end of sub-script

The opcode is the numeric id of a command.  Two command tables exist in the
shipped ``gundx.exe`` (recovered at ``0x4b53e8`` and ``0x4b62ec``):

* :data:`COMMANDS` -- the 192 *named* world-editor commands (``ADD_DOOR``,
  ``SET_POLY_TEXTURE``, ``LOAD_MUSIC`` ...).  Their *signature* string gives the
  operand layout (one letter per operand).
* :data:`CONDITIONS` -- the 59 commands of the second VM (``ADD_THING``,
  ``FIRE_SOURCEPLAYER_LASER``, ``SOURCEPLAYER_AMMO_AT_LEAST``, ``RANDOM`` ...)
  used by the conditional/expression VM entered through opcode ``0xC9`` and the
  ``IF`` opcode 199.

Operand letters -> size (engine func ``0x457170`` walking the table at
``0x457204``): ``A``/``B``/``D`` = 8 bytes, ``Y`` = 1 byte, ``S`` = a
NUL-terminated string, every other letter = 4 bytes.

A handful of ids have **no entry** in the exe's :data:`COMMANDS` table because
they are *structural* rather than plain commands.  Their *names* and *operand
signatures* are recovered from the engine's text loader (``0x460eca``): every
structural keyword is matched against a pooled string and compiled by emitting
the opcode byte followed by a count and then the record operands walked with
the same signature table (the ``DEFINE_*`` / ``ADD_LIFT`` /
``SOURCEPLAYER_PATH`` / ``IF`` / ``ELSE`` / ``END`` tokens)::

    103  DEFINE_LEVEL_OBJECTIVES  u32 n(<=30) + n strings        ("Objective")
    158  SOURCEPLAYER_PATH        u32 n(<=32) + n * {1 dword}    ("Path points")
    170  DEFINE_WEAPONS           u32 n(<=31) + n * {3 strings + 0x54}
    171  DEFINE_AMMO              u32 n(<=31) + n * {1 string  + 0x28}
    172  DEFINE_PLAYER_TYPES      u32 n        + n * {3 strings + 0x17c}
    175  DEFINE_EQUIPMENT         u32 n(<=31) + n * {1 string  + 0x30}
    188  ADD_LIFT                 0x94 bytes + u32 n(<=31) + n * 0xc ("Lift stops")
    199  IF: u8 cond-id + cond operands + u32 size + body(size)
    200  ELSE: u32 relative offset (jump over the else-branch at run time)
    201  EVAL: u8 cond-id + cond operands (a bare condition statement)

The record sizes above are byte-exact against the pooled signature strings
(``0x4b8fe8`` ``SLEEEEEELLLLL`` = 0x30, ``0x4b900c`` ``SLEOLLLLLLL`` = 0x28,
``0x4b8fa8`` ``SSSLLLLEEEELLLLDBBDL`` = 0x54,
``0x4b9024`` ``SSSLDLLD…EE`` = 0x17c, ``0x4b8f7c`` ``LOLLLLLL…VNVNVN`` =
0x94, ``ELE`` = 0xc, ``E`` = 4).  Opcode ``0xFF`` terminates a sub-script.

``decode()`` turns a code block into a flat list of :class:`Insn` (with ``IF``
bodies decoded recursively) and ``render()`` prints it as readable text.  Every
one of the 36 shipped ``.sbt`` files decodes up to a byte-exact end.
"""

import struct
from collections import namedtuple


COMMANDS = {
    1: ('EXIT_EVENT', None),
    2: ('GENERATE', 'E'),
    3: ('GENERATE_FOR_CHILDREN', 'E'),
    4: ('GENERATE_FOR_PARENT', 'E'),
    5: ('ENABLE', 'E'),
    6: ('DISABLE', 'E'),
    7: ('SLEEP', 'P'),
    8: ('SET_SECTOR_MOVEMENT', 'LLLLAAA'),
    9: ('SET_SECTOR_BRIGHTNESS', 'LL'),
    10: ('SET_SECTOR_TYPE', 'LL'),
    11: ('SET_SECTOR_DAMAGE', 'LL'),
    12: ('SET_SECTOR_SOUND', 'LVND'),
    13: ('SET_SECTOR_EVENTS', 'LEE'),
    14: ('SET_SECTOR_FLAGS', 'LL'),
    15: ('SET_POLY_TEXTURE', 'LLLT'),
    16: ('SET_POLY_BRIGHTNESS', 'LLLL'),
    17: ('SET_POLY_SCROLL', 'LLLLLLL'),
    18: ('LOG_MESSAGE', 'S'),
    19: ('EXIT_PROGRAM', None),
    20: ('PLAY_SAMPLE', 'NVLLL'),
    21: ('PLAY_SAMPLE_AT_LOCATION', 'NVLLLLL'),
    22: ('SPEAK_NUMBER', 'L'),
    23: ('SPEAK_SAMPLE', 'NVLL'),
    24: ('SET_SOURCEWORLDOBJECT_HIT_OFF', None),
    25: ('SET_SOURCEWORLDOBJECT_HIT_ON', None),
    26: ('SET_SOURCEWORLDOBJECT_TEXTURES', 'T'),
    27: ('SET_SOURCESECTOR_MOVEMENT', 'LLLAAA'),
    28: ('SET_SOURCESECTOR_BRIGHTNESS', 'L'),
    29: ('SET_SOURCESECTOR_FLAGS', 'L'),
    30: ('SET_SOURCESECTOR_TYPE', 'L'),
    31: ('SET_SOURCESECTOR_DAMAGE', 'L'),
    32: ('SET_SOURCEPOLY_TEXTURE', 'T'),
    33: ('SET_SOURCEPOLY_BRIGHTNESS', 'L'),
    34: ('SET_SOURCEPOLY_MATERIAL', 'M'),
    35: ('SET_SOURCEPOLY_SCROLL', 'LLLL'),
    36: ('SET_SOURCEPOLY_SWITCH_EVENT', 'E'),
    37: ('PLAY_SAMPLE_AT_SOURCE', 'NVLL'),
    38: ('PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE', 'LENVL'),
    39: ('REMOVE_SOURCE_ITEM', None),
    40: ('SET_HANDLESECTOR_MOVEMENT', 'ELLLAAA'),
    41: ('SET_HANDLESECTOR_BRIGHTNESS', 'EL'),
    42: ('SET_HANDLESECTOR_TYPE', 'EL'),
    43: ('SET_HANDLESECTOR_DAMAGE', 'EL'),
    44: ('SET_HANDLESECTOR_FLAGS', 'EL'),
    45: ('SET_HANDLESECTOR_SOUND', 'ENVD'),
    46: ('SET_HANDLEPOLY_TEXTURE', 'LT'),
    47: ('SET_HANDLEPOLY_BRIGHTNESS', 'LL'),
    48: ('SET_HANDLEPOLY_SCROLL', 'LLLLL'),
    49: ('SET_HANDLEPOLY_MATERIAL', 'LM'),
    50: ('ADD_SCREEN_TEXT', 'LS'),
    51: ('ADD_SOURCESCREEN_TEXT', 'LS'),
    52: ('ADD_ITEM_AT_SOURCE', 'OPPEELP'),
    53: ('ADD_ITEM', 'OPPEELLLLAAAP'),
    54: ('ADD_ITEM_AT_SOURCE_FIXED', 'OPPEELP'),
    55: ('ADD_ITEM_FIXED', 'OPPEELLLLAAAP'),
    56: ('SET_DOOR_DELTAS', 'LLLLAAA'),
    57: ('SET_DOOR_POSITION', 'LL'),
    58: ('SET_DOOR_SOUNDS', 'LNVNVNVD'),
    59: ('SET_DOOR_SPEED', 'LL'),
    60: ('NEW_GAME', 'S'),
    61: ('NEW_LEVEL', 'S'),
    62: ('NEXT_LEVEL', None),
    63: ('END_GAME', None),
    64: ('ITEMS_MOVE_CHECK', None),
    65: ('SET_SOURCEITEM_TRIGGER_EVENT', 'E'),
    66: ('ADD_SPRITE', 'RDDDLLLLMDDL'),
    67: ('ADD_SPRITE_AT_SOURCE', 'RLLLLLLLMDDL'),
    68: ('ADD_SPRITE_AT_SOURCE_WEAPON_POINT', 'LRLMDDL'),
    69: ('ADD_SPRITE_MOVING', 'RLLLLMDDLDDDP'),
    70: ('ADD_SPRITE_MOVING_AT_SOURCE', 'RLLLLLLLMDDLDDDP'),
    71: ('ADD_SPRITE_MOVING_AT_SOURCEPLAYER', 'RLLLLLLLMDDLDDDP'),
    72: ('ADD_SPRITE_MOVING_AT_SOURCE_WEAPON_POINT', 'LRLMDDLDDDP'),
    73: ('ADD_SOURCEPLAYER_SPRITE', 'LRLLLLLLLMDDLL'),
    74: ('REMOVE_SOURCEPLAYER_SPRITE', 'L'),
    75: ('CAUSE_BLAST_RADIUS_DAMAGE', 'LLLLLE'),
    76: ('SOURCE_CLEAR', None),
    77: ('SET_CAMERA_PITCH_SHAKE', 'APP'),
    78: ('SET_CAMERA_ROLL_SHAKE', 'APP'),
    79: ('SET_CAMERA_YAW_SHAKE', 'APP'),
    80: ('SET_CAMERA_HAZE', 'DD'),
    81: ('LOAD_MUSIC', 'LS'),
    82: ('PLAY_MUSIC', 'L'),
    83: ('STOP_MUSIC', None),
    84: ('UNLOAD_MUSIC', 'L'),
    85: ('MEMORIZE_CURRENT_MUSIC_BANK', None),
    86: ('PLAY_MEMORIZED_MUSIC_BANK', None),
    87: ('ADD_LIGHT_SOURCE', 'LLLLLLLLLLLLLLLL'),
    88: ('ADD_SKY_LIGHT_SOURCE', 'LLLLLLL'),
    89: ('SET_SECTOR_LINK', 'LL'),
    90: ('SET_LEVEL_NAME', 'S'),
    91: ('SHOW_LEVEL_END_SCREEN', None),
    92: ('SET_LEVEL_END_CAMERA_XYZ', 'LLL'),
    93: ('SET_LEVEL_BEST_TIME', 'L'),
    94: ('SET_LEVEL_SECRETS', 'L'),
    95: ('INCREMENT_SOURCEPLAYER_SECRET_TALLY', None),
    96: ('SET_LEVEL_GRAVITY', 'D'),
    97: ('SET_LEVEL_FOG_COLOUR', 'LLL'),
    98: ('SET_LEVEL_FOG_DISTANCE', 'L'),
    99: ('SET_LEVEL_COUNTDOWN_TIMER', 'P'),
    100: ('SET_LEVEL_FOCAL_ADJUST', 'D'),
    101: ('SET_LEVEL_ALIASING_VALUES', 'DD'),
    102: ('SET_LEVEL_VERSION', 'L'),
    104: ('OR_LEVEL_OBJECTIVES_BITS', 'L'),
    105: ('AND_SOURCEPLAYER_KEY_BITS', 'L'),
    106: ('OR_SOURCEPLAYER_KEY_BITS', 'L'),
    107: ('AND_SOURCEPLAYER_WEAPON_BITS', 'L'),
    108: ('OR_SOURCEPLAYER_WEAPON_BITS', 'L'),
    109: ('ADD_SOURCEPLAYER_AMMO', 'LL'),
    110: ('AND_SOURCEPLAYER_EQUIPMENT_BITS', 'L'),
    111: ('OR_SOURCEPLAYER_EQUIPMENT_BITS', 'L'),
    112: ('HEAL_SOURCEPLAYER', 'LL'),
    113: ('REPAIR_SOURCEPLAYER', 'LL'),
    114: ('ADD_SOURCEPLAYER_CASH', 'L'),
    115: ('ADD_SOURCEPLAYER_SALVAGE', 'L'),
    116: ('CHANGE_SOURCEPLAYER_SOB', 'O'),
    117: ('SOURCEPLAYER_TURBO_BOOST', 'P'),
    118: ('SOURCEPLAYER_RECOIL', 'D'),
    119: ('SOURCEPLAYER_COLLISION_TYPE', 'LLLL'),
    120: ('SOURCEPLAYER_HAZE', 'P'),
    121: ('SOURCEPLAYER_HAZE_BRIGHTNESS', 'L'),
    122: ('SOURCEPLAYER_INVINCIBLE', 'P'),
    123: ('SOURCEPLAYER_INFECT', None),
    124: ('SOURCEPLAYER_CURE', None),
    125: ('SOURCEPLAYER_HIGHEST_WEAPON_WITH_AMMO', 'L'),
    126: ('SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO', None),
    127: ('SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO', None),
    128: ('SOURCEPLAYER_FLY', 'L'),
    129: ('SOURCEPLAYER_HEADLIGHT', 'L'),
    130: ('SOURCEPLAYER_EQUIPMENT_ACTIVATE', 'LL'),
    131: ('SOURCEPLAYER_FOOT_OFFSETS', 'DDDD'),
    132: ('SOURCEPLAYER_DAMAGE_MODIFIER', 'DD'),
    133: ('SOURCEPLAYER_DETECTOR', 'L'),
    134: ('SOURCEPLAYER_TARGETING', 'L'),
    135: ('SOURCEPLAYER_TARGETED', 'L'),
    136: ('SOURCEPLAYER_EQUIPMENT_INDEX', 'L'),
    137: ('SOURCEPLAYER_WEAPON_INDEX', 'L'),
    138: ('SOURCEPLAYER_WAKE', 'L'),
    139: ('SOURCEPLAYER_KILL', None),
    140: ('SOURCEPLAYER_SELL_ALL', None),
    141: ('SOURCEPLAYER_DISABLE', 'E'),
    142: ('SOURCEPLAYER_ENABLE', 'E'),
    143: ('SOURCEPLAYER_FOCAL_ADJUST', 'D'),
    144: ('SOURCEPLAYER_STAND_GROUND', 'P'),
    145: ('SOURCEPLAYER_FALL_PITCH_DOWN', 'DD'),
    146: ('SOURCEPLAYER_RUB_SPEED_ADJUST', 'D'),
    147: ('SOURCEPLAYER_PLAY_LOOP', 'LNVLLD'),
    148: ('SOURCEPLAYER_STOP_LOOP', 'L'),
    149: ('SOURCEPLAYER_STRAFE', 'DPD'),
    150: ('SOURCEPLAYER_Z_STRAFE', 'DPD'),
    151: ('SOURCEPLAYER_TARGET_LAG', 'P'),
    152: ('SOURCEPLAYER_MOVEMENT_TYPE', 'Y'),
    153: ('SOURCEPLAYER_SLEEP', None),
    154: ('SOURCEPLAYER_DELETE', None),
    155: ('SOURCEPLAYER_MOVE_SPEED', 'D'),
    156: ('SOURCEPLAYER_STEALTH', 'L'),
    157: ('SOURCEPLAYER_TARGETED_MODIFIERS', 'DD'),
    159: ('SOURCEPLAYER_PATH_INDEX', 'L'),
    160: ('AND_SOURCEPLAYER_BEHAVIOUR_BITS', 'L'),
    161: ('OR_SOURCEPLAYER_BEHAVIOUR_BITS', 'L'),
    162: ('AND_SOURCEPLAYER_EXTRA_BITS', 'L'),
    163: ('OR_SOURCEPLAYER_EXTRA_BITS', 'L'),
    164: ('THING_ACCURACY_MODIFIERS', 'PDD'),
    165: ('THING_TARGET_PLAYERS_MODIFIERS', 'DD'),
    166: ('FREEZE', 'P'),
    167: ('REFRESH_SCREEN', None),
    168: ('SET_DEATHMATCH_START', 'LLLLAAA'),
    169: ('SET_PLAYER_START', 'LLLLAAA'),
    173: ('DEFINE_VIRUS', 'LTNVLD'),
    174: ('DEFINE_REGENERATION_MARKER', 'ODD'),
    176: ('DEFINE_SHIELDS', 'LL'),
    177: ('ADD_RANDOM_AMMO_ITEM_AT_SOURCE', 'P'),
    178: ('ADJUST_SOURCEPLAYER_MORALE', 'L'),
    179: ('FILM_DEFINE', 'LPLS'),
    180: ('FILM_PLAY', None),
    181: ('FILM_STOP', None),
    182: ('FILM_REWIND', None),
    183: ('ADD_SWITCH', 'LELLLLVN'),
    184: ('ADD_DOOR', 'LEEEEEAAALLLLLLLDDDDDDDDDVNVNVN'),
    185: ('ADD_NAMED_ITEM', 'ILLLAAA'),
    186: ('ADD_NAMED_ITEM_EX', 'LLLILLLAAA'),
    187: ('ADD_SOUND_SOURCE', 'LLLNVD'),
    189: ('SHOW_AMMO_BUY_SCREEN', None),
    190: ('SHOW_ITEM_BUY_SCREEN', None),
    191: ('SHOW_TANK_BUY_SCREEN', None),
    192: ('SHOW_TANK_SAVE_SCREEN', None),
    193: ('SHOW_WEAPON_BUY_SCREEN', None),
    194: ('SHOW_BRIEFING_SCREEN', None),
    195: ('SHOW_HELP_SCREEN', None),
    196: ('ADD_LOCATION', 'ELLLAAA'),
    197: ('DELETE_LOCATION', 'E'),
    198: ('STOP_SOURCEPLAYER_SLEEPING_SCRIPTS', None),
    202: ('SET_SOURCEPLAYER_TYPE', 'L'),
}

CONDITIONS = {
    1: ('ADD_THING_AT_SOURCE', 'GLLLAAA'),
    2: ('ADD_THING_RELATIVE', 'LLLGLLLAAAL'),
    3: ('ADD_THING_AT_SOURCE_WEAPON_POINT', 'LGAAA'),
    4: ('ADD_THING_EX', 'LLGDDDD'),
    5: ('CAPTURE_THE_FLAG_GAME', 'LGLLLAAADDDD'),
    6: ('GOD_MODE', 'L'),
    7: ('MULTIPLAYER_GAME', None),
    8: ('PLAYERS_AT_LEAST', 'L'),
    9: ('SOURCEPLAYER_AMMO_FULL', None),
    10: ('SOURCE_IS_LOCAL_PLAYER_TWO', None),
    11: ('SOURCE_IS_PLAYER', None),
    12: ('DEATHMATCH_CLASSIC_GAME', None),
    13: ('DEATHMATCH_NORMAL_GAME', None),
    14: ('FIRE_SOURCEPLAYER_BULLET', None),
    15: ('NEAREST_OBSTRUCTION', None),
    16: ('CUSTOM_COMMAND_0', None),
    17: (None, None),
    18: ('FIRE_SOURCEPLAYER_LASER', 'LELBBLAAL'),
    19: ('FIRE_SOURCEPLAYER_THING', 'TMLELBBLAAD'),
    20: ('GAME_DIFFICULTY_AT_LEAST', 'LLGLLLAAADAAAAD'),
    21: ('NEAREST_OBSTRUCTION_FROM_SOURCE', 'LLLL'),
    22: ('NEXT_LEVEL_NUMBER', 'L'),
    23: ('RANDOM', 'L'),
    24: ('RANDOM_FRAME_BALANCED', 'D'),
    25: ('SCAVENGERHUNT_GAME', 'D'),
    26: ('LEVEL_OBJECTIVES_COMPLETE', None),
    27: ('SOURCE_UNDERWATER', None),
    28: ('SOURCE_FROM_GROUND', None),
    29: ('SOURCE_IS_LOCAL_PLAYER_ONE', 'D'),
    30: ('SOURCEPLAYER_LIFE_AT_LEAST', 'L'),
    31: ('SOURCEPLAYER_WEAPON_POWER_UP', 'L'),
    32: ('SOURCEPLAYER_AMMO_AT_LEAST', 'L'),
    33: ('SOURCEPLAYER_CASH_AT_LEAST', 'LL'),
    34: ('SOURCEPLAYER_EXTRA_BITS_SET', 'L'),
    35: ('SOURCEPLAYER_HAS_WEAPON_FOR_AMMO', 'L'),
    36: ('SOURCEPLAYER_KEY_BITS_SET', 'L'),
    37: ('SOURCEPLAYER_TELEPORT', None),
    38: ('SOURCEPLAYER_LIFE_FULL', 'D'),
    39: ('SOURCEPLAYER_SECTOR_FLAGS', None),
    40: ('SOURCEPLAYER_IS', 'L'),
    41: ('SOURCEPLAYER_TYPE', 'E'),
    42: ('SOURCEPLAYER_TELEPORT_HANDLE', 'DDDAAA'),
    43: ('SOURCEPLAYER_THING_TYPE', 'L'),
    44: ('SOURCEPLAYER_UNDERWATER', 'S'),
    45: ('SOURCEPLAYER_WEAPON_BITS_SET', 'P'),
    46: ('SOURCEPOLY_HANDLE', 'D'),
    47: ('SOURCEPLAYER_WARP', None),
    48: ('TEAM_GAME', None),
    49: ('SOURCEPLAYER_SHIELD_FULL', 'L'),
    50: ('SOURCESECTOR_FLAGS', 'E'),
    51: ('SOURCE_SWAP', 'L'),
    52: ('CUSTOM_COMMAND_1', None),
    53: ('CUSTOM_COMMAND_2', 'L'),
    54: ('CUSTOM_COMMAND_3', 'LL'),
    55: ('CUSTOM_COMMAND_4', 'LLL'),
    56: ('CUSTOM_COMMAND_5', 'LLLL'),
    57: ('CUSTOM_COMMAND_6', 'LLLLL'),
    58: ('DEATHMATCH_GAME', 'LLLLLL'),
    59: ('ADD_THING', 'ZLLLAAA'),
}


LETTER_SIZE = {
    "A": 8, "B": 8, "D": 8, "Y": 1,
    "E": 4, "G": 4, "I": 4, "L": 4, "M": 4, "N": 4,
    "O": 4, "P": 4, "R": 4, "T": 4, "V": 4, "Z": 4,
    "S": "str",
}

# id -> operand signature of one record for the fixed-size record tables.
# The letters are the very strings the engine's text loader feeds to the
# operand walker (``0x461bc0``), so the derived byte size is authoritative.
TABLE_SIGS = {
    170: "SSSLLLLEEEELLLLDBBDL",  # DEFINE_WEAPONS
    171: "SLEOLLLLLLL",  # DEFINE_AMMO
    172: "SSSLDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE",  # DEFINE_PLAYER_TYPES
    175: "SLEEEEEELLLLL",  # DEFINE_EQUIPMENT
}
TABLE_NAMES = {
    170: "DEFINE_WEAPONS",
    171: "DEFINE_AMMO",
    172: "DEFINE_PLAYER_TYPES",
    175: "DEFINE_EQUIPMENT",
}
_TABLES = set(TABLE_NAMES.values())

MAX_BLOCK = 0x30000  # engine's code buffer limit (0x4b6784 / [ebp+8] < 0x30000)

Insn = namedtuple("Insn", "off op name args body data")


def _rdstr(code, i):
    j = code.find(b"\0", i)
    if j < 0:
        raise ValueError("unterminated string at 0x%x" % i)
    return code[i:j].decode("latin1"), j + 1


def record_size(sig):
    """Total byte size of one record described by an operand signature."""
    return sum(LETTER_SIZE[c] for c in sig if c != "S")


def _letters(sig):
    if not sig:
        return []
    try:
        return [LETTER_SIZE[c] for c in sig]
    except KeyError as exc:
        raise ValueError("bad signature %r (letter %s)" % (sig, exc.args[0]))


def _rdargs(code, i, letters):
    out = []
    for s in letters:
        if s == "str":
            v, i = _rdstr(code, i)
            out.append(v)
        elif s == 8:
            out.append(struct.unpack_from("<d", code, i)[0])
            i += 8
        else:
            out.append(int.from_bytes(code[i:i + s], "little"))
            i += s
    return out, i


def _table(code, i, sig):
    nstr = len(sig) - len(sig.lstrip("S"))
    letters = _letters(sig[nstr:])
    n = struct.unpack_from("<I", code, i)[0]
    i += 4
    recs = []
    for _ in range(n):
        strs = []
        for _ in range(nstr):
            s, i = _rdstr(code, i)
            strs.append(s)
        vals, i = _rdargs(code, i, letters)
        recs.append((strs, vals))
    return recs, i


def decode(code, start=0, end=None):
    """Decode a SCRIPT code block into a list of :class:`Insn`.

    Returns ``(insns, offset_of_end)``; the end offset must equal ``end`` for a
    faithful decode.  ``IF`` bodies are decoded recursively into ``insn.body``.
    """
    if end is None:
        end = len(code)
    out = []
    i = start
    while i < end:
        off = i
        b = code[i]
        if b == 0xFF:                                   # end of sub-script
            out.append(Insn(off, b, "END", [], None, None))
            i += 1
            continue
        if b in TABLE_SIGS:                             # DEFINE_WEAPONS/AMMO/… tables
            recs, i = _table(code, i + 1, TABLE_SIGS[b])
            out.append(Insn(off, b, TABLE_NAMES[b], [("n", len(recs))], None, recs))
            continue
        if b == 103:                                    # DEFINE_LEVEL_OBJECTIVES
            n = struct.unpack_from("<I", code, i + 1)[0]
            if n > 30:
                raise ValueError("DEFINE_LEVEL_OBJECTIVES count too large (%d)" % n)
            i += 5
            vals = []
            for _ in range(n):
                s, i = _rdstr(code, i)
                vals.append(s)
            out.append(Insn(off, b, "DEFINE_LEVEL_OBJECTIVES", vals, None, None))
            continue
        if b == 158:                                    # SOURCEPLAYER_PATH
            n = struct.unpack_from("<I", code, i + 1)[0]
            if n > 32:
                raise ValueError("SOURCEPLAYER_PATH point count too large (%d)" % n)
            i += 5
            vals = list(struct.unpack_from("<%dI" % n, code, i))
            i += 4 * n
            out.append(Insn(off, b, "SOURCEPLAYER_PATH", [("n", n)], None, vals))
            continue
        if b == 188:                                    # ADD_LIFT
            j = i + 1 + 0x94
            head = code[i + 1:j]
            n = struct.unpack_from("<I", code, j)[0]
            j += 4
            recs = [tuple(struct.unpack_from("<3I", code, j + 12 * k)) for k in range(n)]
            i = j + 12 * n
            out.append(Insn(off, b, "ADD_LIFT", [("n", n)], None, (head, recs)))
            continue
        if b == 199:                                    # IF <cond> <size> { body }
            cid = code[i + 1]
            name, sig = CONDITIONS.get(cid, ("OP_%d" % cid, None))
            letters = _letters(sig)
            args, j = _rdargs(code, i + 2, letters)
            size = struct.unpack_from("<I", code, j)[0]
            body, j2 = decode(code, j + 4, j + size)
            out.append(Insn(off, b, "IF", [(name, args)], body, size))
            i = j + size
            # an ELSE clause (opcode 200) always immediately follows the
            # then-branch; decode the else-branch up to the jump target.
            if i < end and code[i] == 200:
                rel = struct.unpack_from("<I", code, i + 1)[0]
                tgt = i + 1 + rel
                ebody, _ = decode(code, i + 5, tgt)
                out.append(Insn(i, 200, "ELSE", [("target", tgt)], ebody, rel))
                i = tgt
            continue
        if b == 200:                                    # ELSE / relative jump
            rel = struct.unpack_from("<I", code, i + 1)[0]
            out.append(Insn(off, b, "ELSE", [("target", i + 1 + rel)], None, rel))
            i = i + 1 + rel
            continue
        if b == 0xC9:                                   # evaluate a condition op
            cid = code[i + 1]
            name, sig = CONDITIONS.get(cid, ("OP_%d" % cid, None))
            args, i = _rdargs(code, i + 2, _letters(sig))
            out.append(Insn(off, b, "EVAL", [(name, args)], None, None))
            continue
        if b not in COMMANDS:
            raise ValueError("unknown opcode %d at 0x%x" % (b, off))
        name, sig = COMMANDS[b]
        args, i = _rdargs(code, i + 1, _letters(sig))
        out.append(Insn(off, b, name, args, None, None))
    return out, i


def _fmt(v):
    if isinstance(v, str):
        return '"%s"' % v
    if isinstance(v, float):
        return "%g" % v
    if isinstance(v, int) and v >= 0x10000:
        return "%d(0x%x)" % (v, v)
    return str(v)


def render(code, start=0, end=None, indent=0):
    """Render a code block as readable text (a mission decompiler)."""
    insns, _ = decode(code, start, end)
    return "\n".join(_render(insns, indent))


def _render(insns, indent):
    """Return the rendered lines as a list of strings."""
    pad = "    " * indent
    lines = []
    k = 0
    n = len(insns)
    while k < n:
        ins = insns[k]
        if ins.name == "END":
            lines.append(pad + "---")
        elif ins.name in _TABLES:
            lines.append("%s%s %d entries" % (pad, ins.name, len(ins.data)))
            for strs, vals in ins.data:
                lines.append("%s    %-24s %s" % (
                    pad, " | ".join(strs), " ".join(_fmt(v) for v in vals)))
        elif ins.name == "DEFINE_LEVEL_OBJECTIVES":
            lines.append(pad + "DEFINE_LEVEL_OBJECTIVES " +
                         " ".join('"%s"' % s for s in ins.args))
        elif ins.name == "SOURCEPLAYER_PATH":
            lines.append("%sSOURCEPLAYER_PATH n=%d %s" % (
                pad, ins.args[0][1], " ".join(_fmt(x) for x in ins.data)))
        elif ins.name == "ADD_LIFT":
            lines.append("%sADD_LIFT n=%d" % (pad, ins.args[0][1]))
        elif ins.name == "IF":
            (cname, cargs), = ins.args
            head = "%sIF %s" % (pad, cname)
            if cargs:
                head += " " + " ".join(_fmt(x) for x in cargs)
            nxt = insns[k + 1] if k + 1 < n else None
            has_else = nxt is not None and nxt.name == "ELSE" and nxt.body is not None
            lines.append(head + " {")
            lines.extend(_render(ins.body, indent + 1))
            if has_else:
                lines.append(pad + "} else {")
                lines.extend(_render(nxt.body, indent + 1))
                k += 1
            lines.append(pad + "}")
        elif ins.name == "ELSE":
            lines.append("%sELSE -> 0x%x" % (pad, ins.args[0][1]))
        elif ins.name == "EVAL":
            (cname, cargs), = ins.args
            lines.append("%sEVAL %s %s" % (pad, cname, " ".join(_fmt(x) for x in cargs)))
        else:
            args = " ".join(_fmt(x) for x in ins.args)
            lines.append(("%s%-34s %s" % (pad, ins.name, args)).rstrip())
        k += 1
    return lines
