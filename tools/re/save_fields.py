#!/usr/bin/env python3
"""Field map of a Gunmetal save (``SAVEGAME/saved<N>.sbt``).

The container, the block order and byte-exact rebuild are covered by
``tools/re/save_rebuild.py``; this tool answers a different question: what each
*field* of the blocks that are still copied verbatim means.  It carries a map
``field -> offset -> meaning -> evidence`` for

* the 22 dwords of the save header, the four name slots and the level name;
* doors (0xB8), switches (0x20), sprites (44), sound sources (28, stored as six
  parallel columns);
* the fixed tables ``ITEM`` (300 x 0x60) and ``SECTOR_STUFF`` (256 x 0x58);
* the player structure ``0x3510`` and its attachment array.

Every entry is tagged ``proven`` (instruction + data), ``hypothesis`` (code or
data only) or ``unknown`` (offset/type known, meaning not).  The tool only
reads saves and the executable is not needed; it is stdlib-only.

Usage::

    python3 tools/re/save_fields.py                    # field map + installed saves
    python3 tools/re/save_fields.py --map              # field map only
    python3 tools/re/save_fields.py SAVE --json
    python3 tools/re/save_fields.py --diff saved0.sbt saved1.sbt
    python3 tools/re/save_fields.py --check [SAVE ...]
    python3 tools/re/save_fields.py --self-test [SAVE]

Exit status: 0 = ok, 1 = IO/parse error, 2 = check failure.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

HEADER_SIZE = 0x13C
NAME_SLOTS = 4
NAME_SLOT = 16
FIELD_COUNT = 22

DOOR_STRIDE = 0xB8
SWITCH_STRIDE = 0x20
SPRITE_STRIDE = 0x2C
SOUND_STRIDE = 28
ITEM_STRIDE = 0x60
ITEM_SLOTS = 300
SECTOR_STRIDE = 0x58
SECTOR_SLOTS = 256
PLAYER_STRUCT = 0x3510
PLAYER_SLOTS = 8
THING_SLOTS = 260
THING_RECORD = 0x528

LIMITS = {
    1: 0x200, 2: 100, 3: THING_SLOTS, 4: 0x80, 5: 0x31, 6: 0x1F3,
    7: 0x7F, 15: 0x1F3, 16: 0x12B, 17: 0xFF, 18: 0x1E, 20: 0xC8,
}
REQUIRED = (12, 20, 15, 17, 5, 4, 11, 3, 9)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"


class SaveError(ValueError):
    """Corrupt or unsupported save."""


# --- map -------------------------------------------------------------------


class Field:
    __slots__ = ("offset", "name", "typ", "status", "meaning", "evidence")

    def __init__(self, offset, name, typ, status, meaning, evidence):
        self.offset, self.name, self.typ = offset, name, typ
        self.status, self.meaning, self.evidence = status, meaning, evidence

    def as_dict(self):
        return {"offset": "0x%03x" % self.offset, "name": self.name,
                "type": self.typ, "status": self.status,
                "meaning": self.meaning, "evidence": self.evidence}

    def label(self):
        return "%+5d  0x%03x" % (self.offset, self.offset) if self.offset else "    0  0x000"


def _f(offset, name, typ, status, meaning, evidence):
    return Field(offset, name, typ, status, meaning, evidence)


HEADER_FIELDS = [
    _f(0x00, "name[0x50]", "char[80]", "proven",
       "save-slot name; bytes 0..11 are the display title",
       "writer 0x4557b5 writes a local 0x50-byte name buffer; saved0 starts "
       "with 'A SAVED GAME', saved1 with '2'"),
    _f(0x50, "reserved", "u32", "proven",
       "always 0; the writer stores a zeroed local dword",
       "0x4557e0 (writer) / 0x454e74 (loader) read/write exactly 4 bytes, 0 in "
       "both saves"),
    _f(0x54, "level[0x50]", "char[80]", "proven",
       "level name used to build the .sbt/.sbw paths; NUL-terminated at +0",
       "writer 0x455816/loader 0x454ec1 copy 0x50 bytes from 0x5a4818; "
       "0x442e53..0x442e73 build 'LEVEL%ld' into the same buffer"),
    _f(0xa4, "F0 player_count", "u32", "proven",
       "number of player slots present (1 or 2)",
       "0x448466/0x448473 set 0/1 for the two player ids and 0x44847d adds the "
       "second; 0x44d196 writes the same value to F0 and F16; 1 in both saves"),
    _f(0xa8, "F1 level_clock", "u32", "proven",
       "level clock; the completion code divides it by 91 to get seconds",
       "0x44328e/0x4454c8 clear it at level start, 0x44b6d2 adds a delta, "
       "0x44c715 computes now-last; 0x45c057 divides it by 91 (magic "
       "0xb40b40b5) -> F20; saved0=4288 (47 s), saved1=8148 (89 s)"),
    _f(0xac, "F2 next_mission_number", "u32", "proven",
       "mission/level number used by NEXT_LEVEL_NUMBER and LEVEL%ld",
       "condition 0x45ea26 compares it; 0x442a0e/0x442cf7 set 1; "
       "docs/FORMATS.md 15; 1 in both saves"),
    _f(0xb0, "F3 spawn_counter", "u32", "hypothesis",
       "counter of runtime-spawned things (flag 0x20000)",
       "inc on spawn at 0x46d0f6/0x46d421/0x46fb3d, stored at the end of the "
       "things blob (0x4654b4) and restored at 0x466ddf; no gameplay reader; "
       "0 in both saves"),
    _f(0xb4, "F4 player_type_current", "i32", "hypothesis",
       "current player type/variant (0..3)",
       "copied from F5 at 0x442e1c; tested by 0x4421c0/0x4421d0 and the "
       "0x45c84f mask switch, used by the rpv model loader 0x47a720; 0"),
    _f(0xb8, "F5 player_type_config", "i32", "hypothesis",
       "configured player type/variant, source of F4",
       "set from a set_new_game_parameters argument at 0x442c02; 0"),
    _f(0xbc, "F6 game_flags_current", "u32", "proven",
       "live game option/state bitmask",
       "0x442e22 initialises it from F7; bits 0x1/0x8/0x10/0x20/0x40 are "
       "tested at 0x444555, 0x44ce14, 0x452e67, 0x45c0ae, 0x470b4f; "
       "0x40 in both saves (set during play)"),
    _f(0xc0, "F7 game_flags_initial", "u32", "proven",
       "game option bitmask as passed to set_new_game_parameters",
       "0x442c0d stores the argument; 0x442e17..0x442e22 copies it to F6; 0"),
    _f(0xc4, "F8 control_param_current", "i32", "hypothesis",
       "player look/turn step used every frame",
       "copied from F9 at 0x442e49; 0x452e55 adds it to player+0x3330 and the "
       "0x452e76 loop scales it by 4/2/1/1/2/1/4; 90000"),
    _f(0xc8, "F9 control_param_config", "i32", "hypothesis",
       "configured value of the look/turn step, source of F8",
       "set from a set_new_game_parameters argument at 0x442c07; 90000"),
    _f(0xcc, "F10 mission_param_a_current", "i32", "hypothesis",
       "current mission parameter A (0..4)",
       "copied from F11 at 0x442e06; compared with F11 in the level-transition "
       "code 0x442d34; 0"),
    _f(0xd0, "F11 mission_param_a_config", "i32", "hypothesis",
       "configured mission parameter A",
       "set by the mission switch 0x442ad5..0x442bb4 (observed 0/1/3); 0"),
    _f(0xd4, "F12 mission_param_b_current", "i32", "hypothesis",
       "current mission parameter B (0..4)",
       "copied from F13 at 0x442e39; 0x442d23/0x442d2c compare A/B pairs to "
       "decide whether to re-init the level data; 0"),
    _f(0xd8, "F13 mission_param_b_config", "i32", "hypothesis",
       "configured mission parameter B",
       "set by the mission switch 0x442ad5..0x442bb4 (observed 0..4); 0"),
    _f(0xdc, "F14 game_state", "i32", "proven",
       "game state/mode: 2/4 = playing, 5 = level complete",
       "0x4435c2..0x4435ca sets it from F15 and tests 4; 0x45c066 sets 5 on "
       "completion and 0x4436a2/0x443699 compare it; 4 in both saves"),
    _f(0xe0, "F15 game_mode", "i32", "proven",
       "selected game mode (2 or 4)",
       "0x442a56..0x442a63 picks 4 when bit 1 of the argument is set, else 2; "
       "F14 = F15 at 0x4435ca; 0x4435a2 prints 'ok %ld,%ld'; 2 in both saves"),
    _f(0xe4, "F16 player_count_current", "i32", "hypothesis",
       "live player count, initialised from F17",
       "0x442e99 initialises it from F17; 0x44d196 rewrites it together with "
       "F0; 0x443431 uses it as a count; 1"),
    _f(0xe8, "F17 game_type", "i32", "hypothesis",
       "game type/mode passed to set_new_game_parameters",
       "0x442bf2 stores the argument; 0x442e73 sets 0x83d940 = (F17 == 2); 1"),
    _f(0xec, "F18 rank_or_mission", "i32", "hypothesis",
       "progress/rank value tested against item and thing ranges",
       "0x442e01..0x442e12 copies F19; 0x468d64/0x468d6f compare NAMED_ITEMS "
       "min/max, 0x46d950 compares THING_TYPES bounds and 0x459ae8 checks an "
       "ADD_NAMED_ITEM operand range; 6"),
    _f(0xf0, "F19 rank_or_mission_config", "i32", "hypothesis",
       "configured value of F18",
       "set from a set_new_game_parameters argument at 0x442bec; 6"),
    _f(0xf4, "F20 elapsed_seconds", "i32", "proven",
       "level completion time in seconds = F1 / 91",
       "0x45c057/0x46053a divide 0x5a46e4 by 91 (magic 0xb40b40b5, shift 6) "
       "and store the quotient; 0x483f00 prints it as '%ld : %ld%ld' and "
       "0x483c7c shows it; 0 while playing"),
    _f(0xf8, "F21 best_time_result", "i32", "hypothesis",
       "result of the best-time lookup for F20",
       "0x45c880(0x80f860, seconds) result stored at 0x45c08e/0x460571 and "
       "printed at 0x483ce2; 0"),
]

HEADER_SLOTS = [
    _f(0x0fc, "slot0 player A name", "char[16]", "proven",
       "player A display name ('K-9')",
       "0x442bf8..0x442c13 copies an argument into 0x83d2e0; 0x443443 passes "
       "it to 0x453120 (player A)"),
    _f(0x10c, "slot1 player B name", "char[16]", "proven",
       "player B display name ('')",
       "0x442c26..0x442c2e copies an argument into 0x83d2f0; used for the "
       "second player in 0x44b528"),
    _f(0x11c, "slot2 player A model", "char[16]", "proven",
       "player A chassis/model name ('coargus')",
       "0x442c51..0x442c71 copy or default to 'none' at 0x4aa2d8; 0x44343e "
       "passes it to 0x453120 as the model argument"),
    _f(0x12c, "slot3 player B model", "char[16]", "proven",
       "player B chassis/model name ('none')",
       "0x442c87..0x442ca5 copy or default to 'none' at 0x4aa2e8; used for "
       "player B in 0x44b517/0x44b56a"),
]

DOOR_FIELDS = [
    _f(0x00, "box[0..8]", "9 x f64", "proven",
       "door collision box: closed corner (0x00), open corner (0x18), size "
       "(0x30); the loader folds corner += size",
       "ADD_DOOR 0x44e31c..0x44e41a stores the 9 D operands here and adds "
       "D7..D9 at 0x44e368..0x44e3f6; all 25 records of level0 match the "
       "script operands after folding"),
    _f(0x48, "delta[0..2]", "3 x f64", "proven",
       "per-tick movement delta of the box",
       "door update 0x450054..0x45005e writes the three doubles; 0 while the "
       "door is idle (all 25 records)"),
    _f(0x60, "const 0.03", "f64", "unknown",
       "constant 0.03 written by ADD_DOOR, no reader found",
       "0x44e460/0x44e46a store 0x3f9eb851eb851eb8; only write xref"),
    _f(0x68, "flags", "i32", "proven",
       "door type in the low nibble (1..3); bit 0x40 marks a usable door",
       "0x45068d/0x44f6b6 mask with 0xf, 0x4507c6 tests 0x40; level data has "
       "1/2/3 and 65/66/67"),
    _f(0x6c, "ids[0..4]", "5 x i32", "proven",
       "the five E operands: identity/trigger ids",
       "ADD_DOOR 0x44e289..0x44e2bd; GENERATE 0x44f6c0 matches ids[0] and "
       "ids[1], 0x44fc78 checks ids[0..2]"),
    _f(0x80, "state", "i32", "proven",
       "door motion state: -1 closing, 0 closed, +1 opening",
       "0x44f6e5/0x44f6fc write +-1 and the door update 0x44f31b uses it as a "
       "direction; all 25 saved doors are 0"),
    _f(0x84, "sector", "i32", "proven",
       "sector index of the door object in the .sbw level",
       "0x44ffdc compares it with the sector argument, 0x4507dd passes it with "
       "0x88 to 0x434419; all 25 saved records valid"),
    _f(0x88, "object", "i32", "proven",
       "object index inside the sector",
       "same 0x44ffdc/0x4507e5 pair; 25/25 valid in level0"),
    _f(0x8c, "phase_prev", "u32", "proven",
       "animation phase before the last tick, 0..0xffff = fraction/65536",
       "0x44f316 copies it to phase_next, 0x44f351..0x44f371 use it as the "
       "old fraction; 0 in both saves"),
    _f(0x90, "phase_next", "u32", "proven",
       "animation phase after the last tick, clamped to 0xffff",
       "0x44f337/0x44f349 store and clamp it; 0 in both saves"),
    _f(0x94, "step", "i32", "proven",
       "phase increment per tick (script L3: 700/800)",
       "0x44f326 multiplies it by dt and the state; level0 has 700/800"),
    _f(0x98, "param5", "i32", "unknown",
       "script L6 (300 in level0); no reader found",
       "ADD_DOOR 0x44e310 stores it; only write xref"),
    _f(0x9c, "param6", "i32", "unknown",
       "script L7 (0 in level0); no reader found",
       "ADD_DOOR 0x44e316 stores it; only write xref"),
    _f(0xa0, "v[0..2]", "3 x i32", "hypothesis",
       "the three V operands of the VNVNVN tail (64/64/64)",
       "ADD_DOOR 0x44e420..0x44e45a stores them; no reader found"),
    _f(0xac, "n[0..2]", "3 x i32", "hypothesis",
       "the three N operands of the VNVNVN tail (132/133/134)",
       "same store block 0x44e420..0x44e45a; no reader found"),
]

SWITCH_FIELDS = [
    _f(0x00, "kind", "i32", "proven",
       "trigger type: 0 ordinary, 0x101 transformed, 0x102/0x103/0x104 source",
       "0x44f97a tests < 0x101, 0x44faf4 matches 0x102, 0x44fb4d matches "
       "0x102/0x103, 0x44fbf1 matches 0x104; delivery 0x44e5f0 copies verbatim"),
    _f(0x04, "id", "i32", "proven",
       "entity id passed to GENERATE (0x479960/0x479810)",
       "0x44f96c loads it and returns if 0; 0x44f9ad calls GENERATE with it; "
       "all 24 saved ids match the level's switches"),
    _f(0x08, "radius", "i32", "proven",
       "trigger radius for kinds 0x102..0x104",
       "0x44fb9e fild's it and compares with the point distance from "
       "0x43e541; 160/120/128 in level0"),
    _f(0x0c, "pos.x", "i32", "proven",
       "trigger position, engine world frame",
       "0x44f997/0x44fa17 push x with y/z into GENERATE; 86.9% of all shipped "
       "positions fall inside the level box (docs/doors.py)"),
    _f(0x10, "pos.y", "i32", "proven", "trigger position Y", "same 0x44f990"),
    _f(0x14, "pos.z", "i32", "proven", "trigger position Z", "same 0x44f98a"),
    _f(0x18, "aux", "i32", "hypothesis",
       "auxiliary sound argument (0/12/19/25/38 in level0)",
       "0x44f9e4 pushes it as the second argument of 0x46b840; meaning not "
       "proven"),
    _f(0x1c, "sound", "i32", "proven",
       "sound sample index; -1 = silent",
       "0x44f9c2 returns when < 0, 0x44f9f6 calls 0x46b840 with it; values "
       "-1 and 121..135 in the shipped levels (< 300 samples)"),
]

SPRITE_FIELDS = [
    _f(0x00, "scale_x", "f32", "proven",
       "sprite X scale (script double arg 9, 1.5 in level0)",
       "0x456164 stores argument a6; 0x455fa0 copies it into object+0x28; "
       "all four saved sprites are 1.5"),
    _f(0x04, "scale_y", "f32", "proven",
       "sprite Y scale (script double arg 10)",
       "0x456173 stores argument a7; 0x455fb7 copies it into object+0x2c"),
    _f(0x08, "runtime_object", "u32", "proven",
       "runtime object handle/pointer, re-created on load",
       "0x456556..0x456562 overwrites it with 0x436685(); the saved values "
       "point into .data and are stale"),
    _f(0x0c, "pos.x", "i32", "proven",
       "world X after applying the +-arg11 scatter",
       "0x4592c2..0x4592e1 scatters and 0x456136 stores a1; values equal the "
       "script positions (scatter returned 0)"),
    _f(0x10, "pos.y", "i32", "proven", "world Y", "0x456140 stores a2"),
    _f(0x14, "pos.z", "i32", "proven", "world Z", "0x45614a stores a3"),
    _f(0x18, "param", "u8", "hypothesis",
       "byte parameter (alpha/brightness?); script arg 7 (0..250)",
       "0x45615e stores argument a4; 0x455fd2 copies it to object+0x24"),
    _f(0x1c, "flags", "u32", "hypothesis",
       "render flags; script arg 11 (0/2/4 in shipped levels)",
       "0x456136 stores a5; bits 0x1, 0x2 and 0x4 are tested at 0x456001, "
       "0x45603b and 0x456053 (model setup / half size)"),
    _f(0x20, "effect_id", "i32", "proven",
       "sprite/effect model id (arg 0; 363 in level0)",
       "0x456130 stores a0; 0x455fdb/0x456021 resolve it with 0x4277d5, "
       "0x42d8cc and 0x4360f1"),
    _f(0x24, "param16", "u16", "hypothesis",
       "16-bit render parameter; script arg 8 (4 in level0)",
       "0x456179 stores a8; 0x456016 copies it to object+0x22"),
    _f(0x28, "spawn_time", "u32", "proven",
       "level clock at which the sprite object was created",
       "0x456185 stores 0x5a46e4; 0 in both shipped saves (created at t=0)"),
]

SOUND_FIELDS = [
    _f(0x00, "x", "i32 column", "proven", "sound source X",
       "creator 0x46b23e stores arg0 in 0x800340, saver 0x46b41b writes it "
       "first; saved (x,y,z,volume,sample,pitch) matches all 6 ADD_SOUND_SOURCE"),
    _f(0x04, "y", "i32 column", "proven", "sound source Y",
       "0x46b250 stores arg1 in 0x800540; second saved column"),
    _f(0x08, "z", "i32 column", "proven", "sound source Z",
       "0x46b25b stores arg2 in 0x800140; third column"),
    _f(0x0c, "volume", "i32 column", "proven",
       "volume (arg 4, 12/19 in level0)",
       "0x46b26d stores arg4 in 0x7ffd40; saver 0x46b488 writes it fourth"),
    _f(0x10, "sample", "i32 column", "proven",
       "SOUND_SAMPLE_TABLE index (arg 3, 114..129 in level0)",
       "0x46b266 stores arg3 in 0x7fff40; 0x46b602 reads it as the sample id"),
    _f(0x14, "pitch", "f64 column", "proven",
       "playback pitch/period (0.05/0.1 in level0)",
       "0x46b249 fstp's the argument double in 0x800760; 0x46b616/0x46b630 "
       "move both halves to the event record"),
]

ITEM_FIELDS = [
    _f(0x00, "basis0", "3 x f64", "proven", "first basis vector",
       "0x4688ad copies 6 dwords into the object; 0x41e025 builds the matrix "
       "(docs/RECORDS.md 3)"),
    _f(0x18, "position", "3 x f64", "proven", "world position",
       "0x468507 copies it into object+0x00"),
    _f(0x30, "runtime_object", "u32", "proven",
       "runtime object/emitter pointer; always 0 in saves",
       "0x468619; re-created at load (0x4684b0/0x468840)"),
    _f(0x34, "kind", "u32", "proven",
       "1 = ADD_ITEM/AT_SOURCE, 2 = *_FIXED",
       "0x468710; saved slot 1 is 2 (ADD_ITEM_FIXED)"),
    _f(0x38, "state", "i32", "proven",
       "-2 free, -1 active, 0 respawn wait, -3/-4 delete, >0 DM timer",
       "0x468e41/0x468840; 298 free slots are -2, the two active are -1"),
    _f(0x3c, "op2", "u32", "hypothesis", "script operand a2",
       "0x4686a0 stores it; game meaning not recovered"),
    _f(0x40, "sob_index", "i32", "proven", "SOB_TABLE model index",
       "0x4684b0/0x468670 via 0x47c990 -> 0x4277cd; 41/36 in the saves"),
    _f(0x44, "op3", "u32", "hypothesis", "script operand a3",
       "0x4686a0; named items use f5"),
    _f(0x48, "op4", "u32", "hypothesis", "script operand a4",
       "0x4686a0; named items use f6"),
    _f(0x4c, "op5", "u32", "hypothesis",
       "script operand a5 (quantity for the fixed item)",
       "0x4686a0; slot 1 has 4000 from ADD_ITEM_FIXED"),
    _f(0x50, "op6", "u32", "hypothesis", "script operand a6",
       "0x4686a0; named items use f8"),
    _f(0x54, "op6_hi", "u32", "proven",
       "duplicate of op6 written by the same handler",
       "0x4687be/0x4687c4 store the high half"),
    _f(0x58, "flags", "u8", "proven", "object flag byte",
       "0x468516 copies it into object+0x74; +0x59..+0x5b padding"),
    _f(0x5c, "sentinel", "u32", "proven",
       "0xffff for a used slot, 0 for a free one",
       "0x4686a0 writes 0xffff; 300/300 match the state"),
]

SECTOR_FIELDS = [
    _f(0x00, "move", "3 x f64", "proven",
       "movement added to objects inside the sector (dx,dy,dz per tick)",
       "SET_SECTOR_MOVEMENT 0x457815; consumer 0x4713a5"),
    _f(0x18, "extra", "3 x f64", "hypothesis",
       "second movement/rotation triple; no reader in this build",
       "same 0x457846..0x4578ac stores it"),
    _f(0x30, "sound_param", "f64", "proven",
       "sound parameter (the D operand of SET_SECTOR_SOUND)",
       "0x46b740/0x46b790; 0.03/0.05 in the saves"),
    _f(0x38, "event_a", "u32", "proven", "sector event id A",
       "SET_SECTOR_EVENTS 0x4579ab; 0x445095 posts it via event_post 0x4795b0"),
    _f(0x3c, "event_b", "u32", "proven", "sector event id B",
       "0x4579ab, consumed by 0x44503f"),
    _f(0x40, "damage_lo", "u16", "proven", "damage, low 16 bits",
       "SET_SECTOR_DAMAGE 0x457961; 0x4450f6 multiplies it by dt"),
    _f(0x44, "damage_hi", "u16", "proven", "damage, high 16 bits",
       "0x457997 stores the high half"),
    _f(0x48, "sound", "i32", "proven", "SOUND_SAMPLE_TABLE index",
       "SET_SECTOR_SOUND 0x46b6e3; 0x46c5fd plays it; 129..131 saved"),
    _f(0x4c, "sound_count", "i32", "proven",
       "remaining sound repeats; >0 means active",
       "0x46c649; 10/25/29 in the saves"),
    _f(0x50, "flags", "u32", "proven", "sector gameplay flags",
       "SET_SECTOR_FLAGS 0x457929; bits 1/2/4/8/0x10 used at 0x444166, "
       "0x46bce5, 0x46c652, 0x444767, 0x4727cb; all 0 in the saves"),
    _f(0x54, "unused", "u32", "unknown",
       "no reader or writer; 0 everywhere",
       "0 in all 256 sectors of both saves; docs/RECORDS.md 2"),
]

PLAYER_FIELDS = [
    _f(0x0000, "position.x", "f64", "proven", "player world X",
       "0x451d04..0x451d2f copy the first three doubles into the object; "
       "saved0 1116.17 -> saved1 1156.36"),
    _f(0x0008, "position.y", "f64", "proven", "player world Y",
       "same 0x451d08; 798.21 -> 797.28"),
    _f(0x0010, "position.z", "f64", "proven", "player world Z",
       "same 0x451d0e; 48.0 in both"),
    _f(0x0018, "angle.pitch", "f64", "proven", "Euler angle 1 (pitch)",
       "0x451d3e..0x451d74 push the three doubles at 0x18/0x20/0x28 into "
       "0x41e025 (matrix from angles); 0"),
    _f(0x0020, "angle.yaw", "f64", "proven", "Euler angle 2 (yaw)",
       "same block; 5.0736 -> 4.7471 (turned while shooting)"),
    _f(0x0028, "angle.roll", "f64", "proven", "Euler angle 3 (roll)",
       "same block; 0"),
    _f(0x0030, "history[240][3]", "240 x 3 f64", "proven",
       "position history ring buffer",
       "0x44377b..0x44378b copies 24 bytes (3 doubles) to "
       "0x819070 + slot*24; loop limit 0xf0 at 0x443802; game position is "
       "pushed there every update"),
    _f(0x16b0, "unknown arrays", "0x1938 B", "unknown",
       "unmapped part of the player struct",
       "zeros or stale doubles; no field proof yet"),
    _f(0x2ff8, "history_x[16]", "16 x f64", "hypothesis",
       "secondary X history (docs/FORMATS.md 16)",
       "0x451ec3; all zero in the shipped saves"),
    _f(0x3078, "history_y[16]", "16 x f64", "hypothesis",
       "secondary Y history", "0x451ed0"),
    _f(0x30f8, "history_z[16]", "16 x f64", "hypothesis",
       "secondary Z history", "0x451edc"),
    _f(0x3178, "exists", "u32", "proven",
       "non-zero while the player slot is valid",
       "0x451c9b tests it; 7496160 (0x7261e0) in both saves"),
    _f(0x317c, "attachments[16]", "16 x u32", "proven",
       "ids of attached objects; 16 bytes per non-zero id follow the struct",
       "0x451c3f zeroes 16 dwords at +0x317c; 0x4520b1 walks them; none in "
       "the shipped saves"),
    _f(0x3204, "history_index", "i32", "proven",
       "next slot of the position history ring",
       "0x4437f9..0x443809 increments it and wraps at 0xf0; 194 -> 130"),
    _f(0x3210, "present", "i32", "proven",
       "1 = this player slot exists",
       "0x452045 tests it before writing the struct; 0x45217c clears it; 1"),
    _f(0x3214, "object_id", "i32", "proven",
       "id used to resolve the player object (0x47c990)",
       "0x451ca9..0x451cd5; 16 in both saves"),
    _f(0x3218, "unknown", "i32", "unknown", "0 in both saves; no xref", "-"),
    _f(0x321c, "unknown", "i32", "unknown", "9999 in both saves", "-"),
    _f(0x3220, "unknown", "i32", "unknown", "0", "-"),
    _f(0x3224, "unknown", "i32", "unknown", "0", "-"),
    _f(0x3228, "unknown", "i32", "unknown", "-1", "-"),
    _f(0x322c, "unknown", "i32", "unknown", "0", "-"),
    _f(0x3230, "unknown", "i32", "unknown", "0", "-"),
    _f(0x3234, "object_handle", "u32", "hypothesis",
       "0x4284bb(object) result", "0x452409 stores it; 96"),
    _f(0x3238, "render_param", "u32", "hypothesis",
       "render scale/param; set to 0x40 at spawn",
       "0x452762 writes 0x40; 0x453b0c uses it with 0x428421"),
    _f(0x323c, "unknown", "i32", "unknown", "5 in both saves", "-"),
    _f(0x3240, "unknown", "i32", "unknown", "5 in both saves", "-"),
    _f(0x3244, "armor_max", "i32", "proven",
       "armor maximum in hundredths (10000 = 100.00)",
       "0x444084 fild's it for the gauge; 10000"),
    _f(0x3248, "armor", "i32", "proven",
       "current armor in hundredths",
       "0x44408a; 10000"),
    _f(0x324c, "shield_max", "i32", "proven",
       "shield maximum in hundredths",
       "0x443c8f; 8000"),
    _f(0x3250, "shields[4]", "4 x i32", "proven",
       "front/back/left/right shields in hundredths",
       "0x443c7d loops 4 slots at 0x81c290; SYSTEM_PLAYER_*_SHIELD_GONE "
       "strings fix the order; 8000 each"),
    _f(0x3294, "weapon_slot", "i32", "proven",
       "selected weapon slot",
       "0x44531c; 0 -> 1"),
    _f(0x3298, "weapon_mask", "u32", "proven",
       "weapon slots available in the level (1 << slot)",
       "0x445328; 3"),
    _f(0x329c, "ammo[12]", "12 x u32", "proven",
       "per-slot ammo counters",
       "0x4455ad reads [slot*4 + 0x81c2dc]; [0] 2000 -> 1975, [1] 90 -> 87"),
]

BLOCK_MAP = {
    1: ("switches", SWITCH_STRIDE, "u32 n + n x 0x20; 8 verbatim ADD_SWITCH operands"),
    2: ("doors", DOOR_STRIDE, "u32 n + n x 0xB8 runtime door records"),
    3: ("things", THING_RECORD, "260 x {u8 present; 0x528 record}"),
    4: ("thing_types", 936, "u32 n + n x 936 (gm.sbt codec)"),
    5: ("named_items", 52, "u32 n + n x 52 (gm.sbt codec)"),
    6: ("sprites", SPRITE_STRIDE, "u32 n + n x 44 runtime sprite records"),
    7: ("sound_sources", SOUND_STRIDE, "u32 n + five u32 columns + one f64 column"),
    8: ("sector_stuff", SECTOR_STRIDE, "fixed 0x5800 = 256 x 0x58"),
    9: ("player", PLAYER_STRUCT, "8 slots + two 0x50+0x3510 pairs"),
    11: ("script", 0, "u32 c + c*40, u32 n + n*12, u32 m + m, u8 flag, [9*0x400]"),
    12: ("sound_table", 0, "300 u32 counts + 300 x 9-byte names"),
    15: ("poly_handles", 16, "u32 n + 4 columns"),
    16: ("object_handles", 12, "u32 n + 3 columns"),
    17: ("sector_handles", 8, "u32 n + 2 columns"),
    18: ("lifts", 168, "u32 n + n*168 + u32 k + k*12"),
    19: ("item", ITEM_STRIDE, "fixed 0x7080 = 300 x 0x60"),
    20: ("sob_table", 20, "u32 n + n x 20 (gm.sbt codec)"),
}

MAP = {
    "header": [f.as_dict() for f in HEADER_FIELDS],
    "header_slots": [f.as_dict() for f in HEADER_SLOTS],
    "door": [f.as_dict() for f in DOOR_FIELDS],
    "switch": [f.as_dict() for f in SWITCH_FIELDS],
    "sprite": [f.as_dict() for f in SPRITE_FIELDS],
    "sound": [f.as_dict() for f in SOUND_FIELDS],
    "item": [f.as_dict() for f in ITEM_FIELDS],
    "sector": [f.as_dict() for f in SECTOR_FIELDS],
    "player": [f.as_dict() for f in PLAYER_FIELDS],
    "blocks": {str(k): {"name": v[0], "stride": v[1], "layout": v[2]}
               for k, v in BLOCK_MAP.items()},
}


def map_status_counts():
    counts = {"proven": 0, "hypothesis": 0, "unknown": 0}
    for group in (HEADER_FIELDS, HEADER_SLOTS, DOOR_FIELDS, SWITCH_FIELDS,
                  SPRITE_FIELDS, SOUND_FIELDS, ITEM_FIELDS, SECTOR_FIELDS,
                  PLAYER_FIELDS):
        for f in group:
            counts[f.status] += 1
    return counts


# --- container -------------------------------------------------------------


class _R:
    __slots__ = ("d", "o")

    def __init__(self, data, off=0):
        self.d, self.o = data, off

    def take(self, n, what):
        if n < 0 or self.o + n > len(self.d):
            raise SaveError("truncated %s at 0x%x (need %d, have %d)"
                            % (what, self.o, n, len(self.d) - self.o))
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def u8(self, what):
        return self.take(1, what)[0]

    def u32(self, what):
        b = self.take(4, what)
        return struct.unpack("<I", b)[0]

    def count(self, what, limit):
        n = self.u32(what)
        if n > limit:
            raise SaveError("%s: count %d out of range [0, %d]" % (what, n, limit))
        return n


def parse_header(data):
    if len(data) < HEADER_SIZE:
        raise SaveError("too short for a header: %d < 0x%x"
                        % (len(data), HEADER_SIZE))
    title = data[:12].split(b"\0")[0]
    if not title or any(c < 0x20 or c > 0x7E for c in title):
        raise SaveError("save title is not printable ASCII: %r" % title)
    level = data[0x54:0xA4].split(b"\0")[0].decode("latin1")
    if not level:
        raise SaveError("empty level name at +0x54")
    fields = list(struct.unpack_from("<22I", data, 0xA4))
    slots = [data[0xFC + i * NAME_SLOT:0xFC + (i + 1) * NAME_SLOT]
             for i in range(NAME_SLOTS)]
    return {"title": title.decode("latin1"), "reserved": struct.unpack_from("<I", data, 0x50)[0],
            "level": level,
            "fields": fields,
            "field_map": {f.name.split()[0]: v for f, v in zip(HEADER_FIELDS[3:], fields)},
            "name_slots": [s.split(b"\0")[0].decode("latin1") for s in slots]}


def parse_doors(c, blk):
    out = []
    for i in range(c):
        r = blk.take(DOOR_STRIDE, "door record")
        flags = struct.unpack_from("<i", r, 0x68)[0]
        out.append({
            "index": i,
            "box": list(struct.unpack_from("<9d", r, 0x00)),
            "delta": list(struct.unpack_from("<3d", r, 0x48)),
            "const": struct.unpack_from("<d", r, 0x60)[0],
            "flags": flags, "kind": flags & 0xF,
            "ids": list(struct.unpack_from("<5i", r, 0x6C)),
            "state": struct.unpack_from("<i", r, 0x80)[0],
            "sector": struct.unpack_from("<i", r, 0x84)[0],
            "object": struct.unpack_from("<i", r, 0x88)[0],
            "phase": struct.unpack_from("<I", r, 0x8C)[0],
            "phase_next": struct.unpack_from("<I", r, 0x90)[0],
            "step": struct.unpack_from("<i", r, 0x94)[0],
            "params": list(struct.unpack_from("<2i", r, 0x98)),
            "v": list(struct.unpack_from("<3i", r, 0xA0)),
            "n": list(struct.unpack_from("<3i", r, 0xAC)),
        })
    return out


def parse_switches(c, blk):
    out = []
    for i in range(c):
        v = struct.unpack("<8i", blk.take(SWITCH_STRIDE, "switch record"))
        out.append({"index": i, "kind": v[0], "id": v[1], "radius": v[2],
                    "pos": list(v[3:6]), "aux": v[6], "sound": v[7]})
    return out


def parse_sprites(c, blk):
    out = []
    for i in range(c):
        r = blk.take(SPRITE_STRIDE, "sprite record")
        out.append({"index": i,
                    "scale": list(struct.unpack_from("<2f", r, 0x00)),
                    "runtime_object": struct.unpack_from("<I", r, 0x08)[0],
                    "pos": list(struct.unpack_from("<3i", r, 0x0C)),
                    "param": r[0x18],
                    "flags": struct.unpack_from("<I", r, 0x1C)[0],
                    "effect": struct.unpack_from("<i", r, 0x20)[0],
                    "param16": struct.unpack_from("<H", r, 0x24)[0],
                    "spawn_time": struct.unpack_from("<I", r, 0x28)[0]})
    return out


def parse_sounds(c, blk):
    cols = []
    for _ in range(5):
        cols.append(list(struct.unpack("<%di" % c,
                                       blk.take(4 * c, "sound column"))))
    cols.append(list(struct.unpack("<%dd" % c,
                                   blk.take(8 * c, "sound pitch column"))))
    return [{"index": i, "x": cols[0][i], "y": cols[1][i], "z": cols[2][i],
             "volume": cols[3][i], "sample": cols[4][i], "pitch": cols[5][i]}
            for i in range(c)]


def parse_items(body):
    used, free = [], 0
    for i in range(ITEM_SLOTS):
        r = body[i * ITEM_STRIDE:(i + 1) * ITEM_STRIDE]
        state = struct.unpack_from("<i", r, 0x38)[0]
        sentinel = struct.unpack_from("<I", r, 0x5C)[0]
        if state == -2 and sentinel == 0:
            free += 1
            continue
        used.append({
            "index": i,
            "basis0": list(struct.unpack_from("<3d", r, 0x00)),
            "position": list(struct.unpack_from("<3d", r, 0x18)),
            "runtime_object": struct.unpack_from("<I", r, 0x30)[0],
            "kind": struct.unpack_from("<I", r, 0x34)[0],
            "state": state,
            "op2": struct.unpack_from("<I", r, 0x3C)[0],
            "sob_index": struct.unpack_from("<i", r, 0x40)[0],
            "op3": struct.unpack_from("<I", r, 0x44)[0],
            "op4": struct.unpack_from("<I", r, 0x48)[0],
            "op5": struct.unpack_from("<I", r, 0x4C)[0],
            "op6": struct.unpack_from("<I", r, 0x50)[0],
            "flags": r[0x58],
            "sentinel": sentinel,
        })
    return {"slots": used, "free": free}


def parse_sectors(body):
    used = []
    for i in range(SECTOR_SLOTS):
        r = body[i * SECTOR_STRIDE:(i + 1) * SECTOR_STRIDE]
        f = {
            "index": i,
            "move": list(struct.unpack_from("<3d", r, 0x00)),
            "extra": list(struct.unpack_from("<3d", r, 0x18)),
            "sound_param": struct.unpack_from("<d", r, 0x30)[0],
            "event_a": struct.unpack_from("<I", r, 0x38)[0],
            "event_b": struct.unpack_from("<I", r, 0x3C)[0],
            "damage_lo": struct.unpack_from("<H", r, 0x40)[0],
            "damage_hi": struct.unpack_from("<H", r, 0x44)[0],
            "sound": struct.unpack_from("<i", r, 0x48)[0],
            "sound_count": struct.unpack_from("<i", r, 0x4C)[0],
            "flags": struct.unpack_from("<I", r, 0x50)[0],
            "unused": struct.unpack_from("<I", r, 0x54)[0],
        }
        if (any(v != 0 for v in f["move"]) or any(v != 0 for v in f["extra"])
                or f["sound_param"] or f["event_a"] or f["event_b"]
                or f["damage_lo"] or f["damage_hi"] or f["sound"]
                or f["sound_count"] or f["flags"]):
            used.append(f)
    return {"used": used, "free": SECTOR_SLOTS - len(used)}


def parse_player(r):
    # The saver (0x452020) writes 8 x {u8 flag; if flag: 0x3510 struct and
    # one 16-byte record per non-zero +0x317c id}.  Flag and struct are
    # interleaved, not "8 flags first".
    flags, structs, attachments = [], [], []
    for i in range(PLAYER_SLOTS):
        f = r.u8("player %d flag" % i)
        if f not in (0, 1):
            raise SaveError("player %d: presence flag %d is not 0/1" % (i, f))
        flags.append(f)
        if not f:
            structs.append(None)
            attachments.append([])
            continue
        st = r.take(PLAYER_STRUCT, "player %d struct" % i)
        att = []
        for k in range(16):
            if struct.unpack_from("<I", st, 0x317C + 4 * k)[0]:
                att.append(r.take(16, "player attachment"))
        structs.append(st)
        attachments.append(att)
    pairs = []
    for _ in range(2):
        pairs.append({"header": r.take(0x50, "player sub header"),
                      "struct": r.take(PLAYER_STRUCT, "player sub struct")})
    return {"present": flags, "structs": structs, "attachments": attachments,
            "pairs": pairs}


def parse_player_fields(st):
    if st is None:
        return None
    hist = struct.unpack_from("<i", st, 0x3204)[0]
    return {
        "position": list(struct.unpack_from("<3d", st, 0x0000)),
        "angles": list(struct.unpack_from("<3d", st, 0x0018)),
        "history_index": hist,
        "exists": struct.unpack_from("<I", st, 0x3178)[0],
        "present": struct.unpack_from("<i", st, 0x3210)[0],
        "object_id": struct.unpack_from("<i", st, 0x3214)[0],
        "armor_max": struct.unpack_from("<i", st, 0x3244)[0],
        "armor": struct.unpack_from("<i", st, 0x3248)[0],
        "shield_max": struct.unpack_from("<i", st, 0x324C)[0],
        "shields": list(struct.unpack_from("<4i", st, 0x3250)),
        "weapon_slot": struct.unpack_from("<i", st, 0x3294)[0],
        "weapon_mask": struct.unpack_from("<I", st, 0x3298)[0],
        "ammo": list(struct.unpack_from("<12I", st, 0x329C)),
        "attachments": [struct.unpack_from("<I", st, 0x317C + 4 * k)[0]
                        for k in range(16)
                        if struct.unpack_from("<I", st, 0x317C + 4 * k)[0]],
    }


def parse_things(r, offset):
    present, records = [], []
    for i in range(THING_SLOTS):
        f = r.u8("thing flag %d" % i)
        if f not in (0, 1):
            raise SaveError("things: slot %d presence flag %d is not 0/1" % (i, f))
        present.append(f)
        records.append(r.take(THING_RECORD, "thing %d" % i) if f else None)
    return {"present": present, "records": records}


def parse_block(r, t, offset):
    if t == 1:
        n = r.count("switches", LIMITS[1])
        return {"name": "switches", "count": n,
                "records": parse_switches(n, r)}
    if t == 2:
        n = r.count("doors", LIMITS[2])
        return {"name": "doors", "count": n, "records": parse_doors(n, r)}
    if t == 3:
        th = parse_things(r, offset)
        return {"name": "things", "slots": THING_SLOTS,
                "present": [i for i, p in enumerate(th["present"]) if p],
                "records": th["records"]}
    if t == 4:
        n = r.count("thing_types", LIMITS[4])
        r.take(n * 936, "thing types")
        return {"name": "thing_types", "count": n}
    if t == 5:
        n = r.count("named_items", LIMITS[5])
        r.take(n * 52, "named items")
        return {"name": "named_items", "count": n}
    if t == 6:
        n = r.count("sprites", LIMITS[6])
        return {"name": "sprites", "count": n, "records": parse_sprites(n, r)}
    if t == 7:
        n = r.count("sound_sources", LIMITS[7])
        return {"name": "sound_sources", "count": n,
                "records": parse_sounds(n, r)}
    if t == 8:
        body = r.take(0x5800, "sector stuff")
        out = parse_sectors(body)
        out["name"] = "sector_stuff"
        return out
    if t == 9:
        p = parse_player(r)
        return {"name": "player", "present": p["present"],
                "slots": [parse_player_fields(s) for s in p["structs"]],
                "attachments": [[len(a) for a in p["attachments"]]],
                "pairs": [{"header": pr["header"].split(b"\0")[0].decode("latin1"),
                           "struct": parse_player_fields(pr["struct"])}
                          for pr in p["pairs"]]}
    if t == 11:
        c = r.count("script things", 0xC8)
        r.take(c * 40, "script things")
        n = r.count("script named", 0x400)
        r.take(n * 12, "script handles")
        m = r.count("script code", 0x30000)
        r.take(m, "script code")
        flag = r.u8("script flag")
        if flag:
            r.take(9 * 0x400, "script tables")
        return {"name": "script", "things": c, "named": n, "code": m,
                "tables": bool(flag)}
    if t == 12:
        counts = struct.unpack("<300I", r.take(0x4B0, "sample counts"))
        raw = r.take(0xA8C, "sample names")
        names = [raw[i * 9:(i + 1) * 9].split(b"\0")[0].decode("latin1")
                 for i in range(300)]
        used = sum(1 for c, n in zip(counts, names) if c and n)
        return {"name": "sound_table", "used": used,
                "referenced": sum(1 for c in counts if c)}
    if t == 15:
        n = r.count("poly handles", LIMITS[15])
        r.take(n * 16, "poly handles")
        return {"name": "poly_handles", "count": n}
    if t == 16:
        n = r.count("object handles", LIMITS[16])
        r.take(n * 12, "object handles")
        return {"name": "object_handles", "count": n}
    if t == 17:
        n = r.count("sector handles", LIMITS[17])
        r.take(n * 8, "sector handles")
        return {"name": "sector_handles", "count": n}
    if t == 18:
        n = r.count("lifts", LIMITS[18])
        r.take(n * 168, "lifts")
        k = r.count("lift stops", 0xC8)
        r.take(k * 12, "lift stops")
        return {"name": "lifts", "count": n, "stops": k}
    if t == 19:
        body = r.take(0x7080, "item")
        out = parse_items(body)
        out["name"] = "item"
        return out
    if t == 20:
        n = r.count("sob table", LIMITS[20])
        r.take(n * 20, "sob table")
        return {"name": "sob_table", "count": n}
    raise SaveError("unknown record type %d at 0x%x" % (t, offset))


def parse_save(data, path=""):
    header = parse_header(data)
    r = _R(data, HEADER_SIZE)
    blocks = {}
    order = []
    while True:
        if r.o >= len(data):
            raise SaveError("record stream has no 0xff terminator")
        offset = r.o
        t = r.u8("record type")
        if t == 255:
            if r.o != len(data):
                raise SaveError("%d trailing bytes after 0xff"
                                % (len(data) - r.o))
            break
        if t in blocks:
            raise SaveError("duplicate block %d at 0x%x" % (t, offset))
        if len(blocks) >= 32:
            raise SaveError("too many blocks (> 32)")
        blk = parse_block(r, t, offset)
        if t in (4, 5, 11, 12, 15, 16, 17, 20):
            blk["raw"] = data[offset + 1:r.o]
        blk["id"] = t
        blk["offset"] = offset
        blk["size"] = r.o - offset
        blocks[t] = blk
        order.append(t)
    return {"path": path, "file": os.path.basename(path),
            "bytes": len(data), "header": header, "order": order,
            "blocks": blocks}


# --- checks ----------------------------------------------------------------


def check_save(s):
    """Return a list of human-readable problems (empty = ok)."""
    p = []
    h = s["header"]
    if len(h["fields"]) != FIELD_COUNT:
        p.append("header: %d fields, expected %d" % (len(h["fields"]), FIELD_COUNT))
    if h["reserved"] != 0:
        p.append("header: reserved dword is 0x%x, expected 0" % h["reserved"])
    missing = [t for t in REQUIRED if t not in s["blocks"]]
    if missing:
        p.append("missing block(s): %s" % ", ".join(map(str, missing)))

    doors = s["blocks"].get(2)
    if doors:
        for d in doors["records"]:
            if d["kind"] not in (1, 2, 3):
                p.append("door %d: type %d (low nibble) not in 1..3"
                         % (d["index"], d["kind"]))
            if d["flags"] & ~0x4F:
                p.append("door %d: unknown flag bits 0x%x"
                         % (d["index"], d["flags"] & ~0x4F))
            if d["state"] not in (-1, 0, 1):
                p.append("door %d: state %d not in -1/0/+1"
                         % (d["index"], d["state"]))
            if not 0 <= d["phase"] <= 0xFFFF or not 0 <= d["phase_next"] <= 0xFFFF:
                p.append("door %d: phase outside 0..0xffff" % d["index"])
            if d["sector"] < 0 or d["object"] < 0:
                p.append("door %d: negative sector/object" % d["index"])
            if d["step"] < 0:
                p.append("door %d: negative step" % d["index"])

    switches = s["blocks"].get(1)
    if switches:
        for w in switches["records"]:
            if w["kind"] not in (0, 0x101, 0x102, 0x103, 0x104):
                p.append("switch %d: unknown kind 0x%x" % (w["index"], w["kind"]))
            if w["radius"] < 0:
                p.append("switch %d: negative radius" % w["index"])
            if w["id"] == 0 and w["kind"] == 0:
                p.append("switch %d: ordinary trigger with id 0" % w["index"])
            if not -1 <= w["sound"] < 300:
                p.append("switch %d: sound %d outside -1..299"
                         % (w["index"], w["sound"]))

    sprites = s["blocks"].get(6)
    if sprites:
        for sp in sprites["records"]:
            if sp["effect"] == 0:
                p.append("sprite %d: effect id 0" % sp["index"])
            if sp["flags"] & ~0x7:
                p.append("sprite %d: unknown flag bits 0x%x"
                         % (sp["index"], sp["flags"] & ~0x7))

    sounds = s["blocks"].get(7)
    if sounds:
        for w in sounds["records"]:
            if not 0 <= w["sample"] < 300:
                p.append("sound %d: sample %d outside 0..299"
                         % (w["index"], w["sample"]))
            if not 0 <= w["volume"] <= 1000:
                p.append("sound %d: volume %d outside 0..1000"
                         % (w["index"], w["volume"]))
            if not 0.0 < w["pitch"] < 16.0:
                p.append("sound %d: pitch %g outside the sane range"
                         % (w["index"], w["pitch"]))

    items = s["blocks"].get(19)
    if items:
        for it in items["slots"]:
            if it["kind"] not in (1, 2):
                p.append("item %d: kind %d not in 1/2" % (it["index"], it["kind"]))
            if it["sentinel"] != 0xFFFF:
                p.append("item %d: sentinel 0x%x, expected 0xffff"
                         % (it["index"], it["sentinel"]))
            if it["sob_index"] < 0:
                p.append("item %d: negative sob index" % it["index"])

    sector = s["blocks"].get(8)
    if sector:
        for x in sector["used"]:
            if not 0 <= x["sound"] < 300:
                p.append("sector %d: sound %d outside 0..299"
                         % (x["index"], x["sound"]))
            if x["sound_count"] < 0:
                p.append("sector %d: negative sound_count" % x["index"])
            if x["flags"] & ~0x1F:
                p.append("sector %d: unknown flag bits 0x%x"
                         % (x["index"], x["flags"] & ~0x1F))
            if x["unused"] != 0:
                p.append("sector %d: +0x54 is not 0" % x["index"])

    player = s["blocks"].get(9)
    if player:
        for i, st in enumerate(player["slots"]):
            if st is None:
                continue
            if not 0 < st["armor_max"] <= 1000000:
                p.append("player %d: armor_max %d out of range"
                         % (i, st["armor_max"]))
            if not 0 <= st["armor"] <= st["armor_max"]:
                p.append("player %d: armor %d > max %d"
                         % (i, st["armor"], st["armor_max"]))
            if not 0 < st["shield_max"] <= 1000000:
                p.append("player %d: shield_max %d out of range"
                         % (i, st["shield_max"]))
            if any(not 0 <= v <= st["shield_max"] for v in st["shields"]):
                p.append("player %d: shield value > max" % i)
            if not 0 <= st["weapon_slot"] < 12:
                p.append("player %d: weapon slot %d outside 0..11"
                         % (i, st["weapon_slot"]))
    return p


# --- diff ------------------------------------------------------------------


def _runs(a, b, limit=8):
    n = min(len(a), len(b))
    out, start = [], None
    for i in range(n):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, n))
    return out[:limit], len(out)


def diff_saves(a, b):
    out = {"a": a["file"], "b": b["file"], "level": a["header"]["level"],
           "header": [], "blocks": []}
    if a["header"]["level"] != b["header"]["level"]:
        out["level_changed"] = True
        return out
    names = [f.name for f in HEADER_FIELDS[3:]]
    for i, (x, y) in enumerate(zip(a["header"]["fields"], b["header"]["fields"])):
        if x != y:
            out["header"].append({"field": names[i], "offset": "0x%03x" % (0xA4 + 4 * i),
                                  "a": x, "b": y, "delta": y - x})
    for i, (x, y) in enumerate(zip(a["header"]["name_slots"], b["header"]["name_slots"])):
        if x != y:
            out["header"].append({"field": "slot%d" % i,
                                  "offset": "0x%03x" % (0xFC + 16 * i),
                                  "a": x, "b": y})
    for t in sorted(set(a["blocks"]) | set(b["blocks"])):
        x, y = a["blocks"].get(t), b["blocks"].get(t)
        if x is None or y is None:
            out["blocks"].append({"id": t, "only": "A" if y is None else "B"})
            continue
        entry = {"id": t, "name": x["name"], "size_a": x["size"],
                 "size_b": y["size"]}
        if t == 2:
            recs = []
            for da, db in zip(x["records"], y["records"]):
                changed = {k: [da[k], db[k]] for k in da
                           if k not in ("index",) and da[k] != db[k]}
                if changed:
                    recs.append({"index": da["index"], "fields": changed})
            entry["records"] = recs
        elif t == 1:
            recs = []
            for da, db in zip(x["records"], y["records"]):
                changed = {k: [da[k], db[k]] for k in da
                           if k not in ("index",) and da[k] != db[k]}
                if changed:
                    recs.append({"index": da["index"], "fields": changed})
            entry["records"] = recs
        elif t == 6:
            recs = []
            for da, db in zip(x["records"], y["records"]):
                changed = {k: [da[k], db[k]] for k in da
                           if k not in ("index", "runtime_object")
                           and da[k] != db[k]}
                if changed:
                    recs.append({"index": da["index"], "fields": changed})
            entry["records"] = recs
        elif t == 7:
            entry["records"] = [{"index": i,
                                 "fields": {k: [da[k], db[k]] for k in da
                                            if da[k] != db[k]}}
                                for i, (da, db) in enumerate(zip(x["records"], y["records"]))
                                if any(da[k] != db[k] for k in da)]
        elif t == 19:
            ma = {it["index"]: it for it in x["slots"]}
            mb = {it["index"]: it for it in y["slots"]}
            entry["slots"] = [{"index": i,
                               "fields": {k: [ma[i][k], mb[i][k]] for k in ma[i]
                                          if k != "index" and ma[i][k] != mb[i][k]}}
                              for i in sorted(set(ma) & set(mb))
                              if any(ma[i][k] != mb[i][k] for k in ma[i] if k != "index")]
        elif t == 8:
            ma = {it["index"]: it for it in x["used"]}
            mb = {it["index"]: it for it in y["used"]}
            entry["sectors"] = [{"index": i,
                                 "fields": {k: [ma[i][k], mb[i][k]] for k in ma[i]
                                            if k != "index" and ma[i][k] != mb[i][k]}}
                                for i in sorted(set(ma) & set(mb))
                                if any(ma[i][k] != mb[i][k] for k in ma[i] if k != "index")]
        elif t == 9:
            sa = x["slots"][0] if x["slots"] else None
            sb = y["slots"][0] if y["slots"] else None
            if sa and sb:
                entry["player"] = {k: [sa[k], sb[k]] for k in sa
                                   if sa[k] != sb[k]}
                if sa["ammo"] != sb["ammo"]:
                    entry["ammo"] = [[i, va, vb] for i, (va, vb)
                                     in enumerate(zip(sa["ammo"], sb["ammo"]))
                                     if va != vb]
        elif t == 3:
            slots = [i for i in range(THING_SLOTS)
                     if x["records"][i] != y["records"][i]]
            entry["changed_slots"] = slots
            entry["changed_bytes"] = {
                str(i): sum(1 for k in range(THING_RECORD)
                            if x["records"][i][k] != y["records"][i][k])
                for i in slots}
        else:
            ra, rb = x.get("raw"), y.get("raw")
            if ra is not None and rb is not None:
                if ra != rb:
                    runs, total = _runs(ra, rb)
                    entry["raw_runs"] = total
                    entry["raw_first"] = ["+0x%x" % lo for lo, _hi in runs]
                else:
                    entry["same_counters"] = True
            else:
                entry["same_counters"] = all(x.get(k) == y.get(k) for k in
                                             ("count", "used", "things", "named",
                                              "code", "tables", "referenced", "stops"))
        out["blocks"].append(entry)
    return out


def print_diff(d):
    print("%s vs %s (%s)" % (d["a"], d["b"], d["level"]))
    if d.get("level_changed"):
        print("different levels: the embedded scripts differ, diff is meaningless")
        return
    if d["header"]:
        print("\nheader fields changed:")
        for e in d["header"]:
            print("  %-24s %s  %s -> %s" %
                  (e["field"], e["offset"], e["a"], e["b"]))
    else:
        print("\nheader: identical")
    print("\nblocks:")
    for e in d["blocks"]:
        if "only" in e:
            print("  %2d %-14s only in %s" % (e["id"], "", e["only"]))
            continue
        bits = []
        if e.get("records"):
            bits.append("records " + ", ".join(str(r["index"]) for r in e["records"]))
        if e.get("slots"):
            bits.append("item slots " + ", ".join(str(r["index"]) for r in e["slots"]))
        if e.get("sectors"):
            bits.append("sectors " + ", ".join(str(r["index"]) for r in e["sectors"]))
        if e.get("changed_slots"):
            bits.append("thing slots " + ", ".join(
                "%d (%d B)" % (i, e["changed_bytes"][str(i)])
                for i in e["changed_slots"]))
        if "player" in e:
            bits.append("player: " + ", ".join(e["player"]))
        if e.get("ammo"):
            bits.append("ammo " + ", ".join("slot%d %d->%d" % tuple(x)
                                            for x in e["ammo"]))
        if e.get("raw_runs"):
            bits.append("%d raw byte run(s) at %s"
                        % (e["raw_runs"], ", ".join(e["raw_first"])))
        if not bits:
            bits.append("no mapped field changed")
        print("  %2d %-14s %s" % (e["id"], e["name"], "; ".join(bits)))


# --- reporting -------------------------------------------------------------


def print_map(verbose=False):
    groups = [
        ("header (offsets relative to 0x000)", HEADER_FIELDS),
        ("header name slots", HEADER_SLOTS),
        ("door record 0xB8", DOOR_FIELDS),
        ("switch record 0x20", SWITCH_FIELDS),
        ("sprite record 0x2c", SPRITE_FIELDS),
        ("sound sources (six columns, record stride 28)", SOUND_FIELDS),
        ("ITEM slot 0x60", ITEM_FIELDS),
        ("SECTOR_STUFF slot 0x58", SECTOR_FIELDS),
        ("player struct 0x3510", PLAYER_FIELDS),
    ]
    for title, fields in groups:
        print("\n== %s ==" % title)
        for f in fields:
            print("  %-9s %-24s %-11s %-10s %s"
                  % ("+0x%03x" % f.offset if f.offset else "+0",
                     f.name, f.typ, f.status, f.meaning))
            if verbose:
                print("           %s" % f.evidence)
    counts = map_status_counts()
    print("\nfields: %d proven, %d hypothesis, %d unknown"
          % (counts["proven"], counts["hypothesis"], counts["unknown"]))


def summary(s):
    h = s["header"]
    b = s["blocks"]
    lines = ["%s: %d bytes, title %r, level %r"
             % (s["file"], s["bytes"], h["title"], h["level"])]
    lines.append("  header: F0=%d F1=%d (%.1f s at 91 Hz) F2=%d, slots %s"
                 % (h["fields"][0], h["fields"][1], h["fields"][1] / 91.0,
                    h["fields"][2],
                    "/".join(repr(x) for x in h["name_slots"])))
    for t in s["order"]:
        blk = b[t]
        extra = ""
        if t in (1, 2, 6, 7):
            extra = " n=%d" % blk["count"]
        elif t == 3:
            extra = " present=%s" % blk["present"]
        elif t == 8:
            extra = " used=%d" % len(blk["used"])
        elif t == 9:
            extra = " present=%s" % blk["present"]
        elif t == 12:
            extra = " referenced=%d" % blk["referenced"]
        elif t in (11,):
            extra = " things=%d named=%d code=%d" % (blk["things"], blk["named"], blk["code"])
        elif t == 19:
            extra = " used=%d free=%d" % (len(blk["slots"]), blk["free"])
        elif "count" in blk:
            extra = " n=%d" % blk["count"]
        lines.append("  %2d %-14s @0x%05x %7d B%s"
                     % (t, blk["name"], blk["offset"], blk["size"], extra))
    return "\n".join(lines)


# --- self-test -------------------------------------------------------------


def self_test(data):
    """Corrupt a valid save in memory; every case must be rejected."""
    cases = []

    def expect(name, mutate):
        d = bytearray(data)
        mutate(d)
        try:
            s = parse_save(bytes(d), name)
            problems = check_save(s)
        except SaveError as exc:
            cases.append((name, True, str(exc)))
        else:
            if problems:
                cases.append((name, True, problems[0]))
            else:
                cases.append((name, False, "accepted"))

    expect("truncated header", lambda d: d.__delitem__(slice(0x80, None)))
    expect("unknown record type", lambda d: d.__setitem__(HEADER_SIZE, 99))
    expect("trailing byte after eof", lambda d: d.append(0))
    expect("missing eof", lambda d: d.__delitem__(-1))

    def duplicate(d):
        # retype the SOB table (20) as the already-seen sound table (12)
        pos = _find_block(d, 20)
        d[pos] = 12
    expect("duplicate block", duplicate)

    def door_count(d):
        pos = _find_block(d, 2)
        struct.pack_into("<I", d, pos + 1, 101)
    expect("door count > 100", door_count)

    def switch_count(d):
        pos = _find_block(d, 1)
        struct.pack_into("<I", d, pos + 1, 0x201)
    expect("switch count > 0x200", switch_count)

    def sprite_count(d):
        pos = _find_block(d, 6)
        struct.pack_into("<I", d, pos + 1, 0x1F4)
    expect("sprite count > 0x1f3", sprite_count)

    def truncated(d):
        pos = _find_block(d, 2)
        del d[pos + 1 + 0xB8:]
    expect("truncated door record", truncated)

    def bad_flag(d):
        pos = _find_block(d, 9)
        d[pos + 1] = 2
    expect("player flag 2", bad_flag)

    def bad_state(d):
        pos = _find_block(d, 2)
        struct.pack_into("<i", d, pos + 5 + 0x80, 5)
    expect("door state 5", bad_state)

    def bad_armor(d):
        pos = _find_block(d, 9)
        struct.pack_into("<i", d, pos + 2 + 0x3248, 1000001)
    expect("armor > max", bad_armor)

    def bad_sentinel(d):
        pos = _find_block(d, 19)
        struct.pack_into("<I", d, pos + 1 + 0x5C, 0x1234)
    expect("item sentinel", bad_sentinel)

    ok = sum(1 for _n, good, _e in cases if good)
    for name, good, info in cases:
        print("  %-24s %s  %s" % (name, "ok  " if good else "FAIL", info))
    print("%d/%d negative cases rejected" % (ok, len(cases)))
    return ok == len(cases)


def _find_block(data, want):
    r = _R(data, HEADER_SIZE)
    while r.o < len(data):
        offset = r.o
        t = r.u8("type")
        if t == 255:
            break
        if t == want:
            return offset
        parse_block(r, t, offset)
    raise SaveError("block %d not found" % want)


# --- main ------------------------------------------------------------------


def load(path):
    try:
        with open(path, "rb") as fh:
            data = fh.read(64 << 20)
    except OSError as exc:
        raise SaveError("%s: %s" % (path, exc))
    return parse_save(data, path)


def installed_saves(game):
    return sorted(glob.glob(os.path.join(game, "SAVEGAME", "*.sbt")))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("saves", nargs="*", help="save files (default: installed)")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--map", action="store_true", help="print the field map only")
    ap.add_argument("--verbose", action="store_true", help="include evidence")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"),
                    help="compare two saves field by field")
    ap.add_argument("--check", action="store_true", help="validate the files")
    ap.add_argument("--self-test", action="store_true",
                    help="run the broken-block negative cases")
    args = ap.parse_args(argv)

    if args.map and not args.saves and not args.diff:
        print_map(args.verbose)
        return 0

    paths = args.saves or installed_saves(args.game)
    if not paths:
        print("no saves given and none in %s/SAVEGAME" % args.game,
              file=sys.stderr)
        return 1

    try:
        saves = [load(p) for p in paths]
    except SaveError as exc:
        print(exc, file=sys.stderr)
        return 1

    if args.self_test:
        ok = self_test(open(paths[0], "rb").read())
        return 0 if ok else 2

    if args.diff:
        try:
            a, b = load(args.diff[0]), load(args.diff[1])
        except SaveError as exc:
            print(exc, file=sys.stderr)
            return 1
        d = diff_saves(a, b)
        if args.json:
            print(json.dumps(d, indent=2))
        else:
            print_diff(d)
        return 0

    if args.check:
        rc = 0
        for s in saves:
            problems = check_save(s)
            if problems:
                rc = 2
                print("FAIL: %s" % s["file"])
                for x in problems:
                    print("  - " + x)
            else:
                print("ok  : %s, %d bytes, %d blocks, %d doors, %d switches, "
                      "%d sprites, %d sounds, %d items, %d sectors, "
                      "%d player slot(s)"
                      % (s["file"], s["bytes"], len(s["order"]),
                         s["blocks"].get(2, {}).get("count", 0),
                         s["blocks"].get(1, {}).get("count", 0),
                         s["blocks"].get(6, {}).get("count", 0),
                         s["blocks"].get(7, {}).get("count", 0),
                         len(s["blocks"].get(19, {}).get("slots", [])),
                         len(s["blocks"].get(8, {}).get("used", [])),
                         sum(s["blocks"].get(9, {}).get("present", []))))
        counts = map_status_counts()
        print("%d/%d files clean; map: %d proven, %d hypothesis, %d unknown"
              % (sum(1 for s in saves if not check_save(s)), len(saves),
                 counts["proven"], counts["hypothesis"], counts["unknown"]))
        return rc

    if args.json:
        doc = {"map": MAP, "saves": []}
        for s in saves:
            doc["saves"].append({
                "file": s["file"], "bytes": s["bytes"],
                "header": s["header"],
                "blocks": [{k: v for k, v in b.items()
                            if k not in ("records", "raw")}
                           if b["id"] in (3, 5, 11, 12, 15, 16, 17, 20, 4)
                           else b for b in
                           (s["blocks"][t] for t in s["order"])],
            })
        print(json.dumps(doc, indent=2, default=str))
        return 0

    for s in saves:
        print(summary(s))
    if args.map:
        print_map(args.verbose)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
