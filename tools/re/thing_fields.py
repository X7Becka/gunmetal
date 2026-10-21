#!/usr/bin/env python3
"""Thing types (``0x776d60``) and thing instances (``0x7ac080``): field maps.

Two structures drive every "thing" in the level runtime:

* **thing type** - ``THING_TYPES`` (``.sbt`` record 4), 936 bytes, an array at
  ``0x776d60`` with stride ``0x3a8`` and the count at ``0x794160``;
* **thing instance** - the 260-slot runtime array at ``0x7ac080`` with stride
  ``0x528`` (``0x7ac080 + 260*0x528 = 0x7ffd20``).  Slot allocation
  ``0x46d5e0``, creation ``0x46d000``/``0x46e420`` -> copy ``0x46d8b0``,
  per-tick update ``0x470c70``.

The ``0x7ac3e0`` / ``0x7ac3ec`` addresses found in the decompiler are **the
same array**: they are the ``+0x360`` (flag word ``A``) and ``+0x36c`` (model
index) fields of slot 0, named by Ghidra at first use.  The base is
``0x7ac080`` because that is what the block-3 save writer dumps
(``0x46fb50``: ``0x528`` bytes from ``pbVar4-0x360``) and what the slot
allocator walks (``0x46d5e0``: ``&DAT_007ac3e0 + i*0x528 < 0x800080``).

Usage::

    python3 thing_fields.py --map                 # both maps (text)
    python3 thing_fields.py --map --json          # machine-readable
    python3 thing_fields.py --verify LEVEL.sbt ...  # type + save checks
    python3 thing_fields.py --extract spawn.c     # re-derive the copy map
    python3 thing_fields.py --self-test           # synthetic + shipped files

``--extract`` takes the text of ``python3 tools/ghidra/mcp.py decompile
0x46d8b0`` and re-derives "instance offset <- type offset" from the store
pairs, so the embedded :data:`TYPE_TO_INSTANCE` is reproducible from the exe.

Only the standard library is used; level files are scanned structurally
(record byte + ``u32 n`` + ``n * record``) instead of importing ``gm``.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# constants proven in docs/THING_FIELDS.md
# ---------------------------------------------------------------------------
TYPE_BASE = 0x776D60      # THING_TYPES array
TYPE_SIZE = 0x3A8         # 936 bytes
TYPE_COUNT = 0x794160     # u32 number of loaded types
INST_BASE = 0x7AC080      # thing instance array
INST_SIZE = 0x528         # 1320 bytes
INST_SLOTS = 260          # (0x800080 - 0x7ac3e0) / 0x528, 0x46d5e0
SECTOR_BASE = 0x800B60    # SECTOR_STUFF, 256 * 0x58, flags at +0x50 = 0x800bb0
SECTOR_SIZE = 0x58
SECTOR_SLOTS = 256

SPAWN = 0x46D8B0          # type -> instance copy
THING_ADD = 0x46D000      # absolute spawn
THING_ADD_REL = 0x46E420  # relative spawn
SLOT_ALLOC = 0x46D5E0
THING_TICK = 0x470C70     # per-tick update (movement switch)
THING_EXPIRE = 0x4703A0
THING_DAMAGE = 0x470420
THING_IMPACT = 0x470500
THING_FREE = 0x470130
SAVE_WRITE = 0x46FB50
SAVE_READ = 0x46FD90

#: ``THING_TYPES`` 936-byte record (``0x776d60``, stride ``0x3a8``).
#: ``(offset, size, name, status)``; the full evidence per field is in
#: ``docs/SOE.md`` §3 and reproducible with ``tools/re/soe_semantics.py --json``.
#: ``0x010`` is ``speed`` (not ``off_z``): ``0x46d0fc`` does
#: ``inst.speed(+0xa0) += arg`` (`FLD [EBP+0x38]; FADD [EBX+0x7ac120];
#: FSTP [EBX+0x7ac120]`) and the shipped data agrees (CRATE 0.0 -> 1.0,
#: BAGCRTHQ 0.1 -> 1.1 in saved0).
TYPE_MAP = [
    (0x000, 16, "name", "proven"),
    (0x010, 8, "speed", "proven"),
    (0x018, 8, "f18", "unknown"),
    (0x020, 8, "turn_accel", "hypothesis"),
    (0x028, 8, "turn_max", "hypothesis"),
    (0x030, 8, "radius", "proven"),
    (0x038, 8, "radius_x", "proven"),
    (0x040, 8, "radius_y", "proven"),
    (0x048, 8, "radius_z", "proven"),
    (0x050, 8, "f50", "unknown"),
    (0x058, 8, "search_radius", "proven"),
    (0x060, 8, "f60", "unknown"),
    (0x068, 8, "f68", "unknown"),
    (0x070, 8, "arrive_radius", "hypothesis"),
    (0x078, 8, "gravity", "proven"),
    (0x080, 8, "damping", "proven"),
    (0x088, 8, "f88", "unknown"),
    (0x090, 8, "col_f90", "hypothesis"),
    (0x098, 8, "col_f98", "hypothesis"),
    (0x0A0, 8, "col_fa0", "hypothesis"),
    (0x0A8, 8, "f_a8", "unknown"),
    (0x0B0, 8, "f_b0", "unknown"),
    (0x0B8, 8, "f_b8", "unknown"),
    (0x0C0, 8, "f_c0", "unknown"),
    (0x0C8, 8, "falling_fear", "proven"),
    (0x0D0, 8, "leash_distance", "hypothesis"),
    (0x0D8, 8, "child_x[4]", "proven"),
    (0x0E0, 8, "child_y[4]", "proven"),
    (0x0E8, 8, "child_z[4]", "proven"),
    (0x138, 8, "child_r1[4]", "proven"),
    (0x158, 8, "child_r2[4]", "proven"),
    (0x178, 8, "child_r3[4]", "proven"),
    (0x198, 4, "f198", "hypothesis"),
    (0x19C, 4, "f19c", "hypothesis"),
    (0x1A0, 4, "collision_kind", "proven"),
    (0x1A4, 4, "sob_index", "proven"),
    (0x1A8, 4, "flags_a", "hypothesis"),
    (0x1AC, 4, "flags_b", "hypothesis"),
    (0x1B0, 4, "flags_c", "hypothesis"),
    (0x1B4, 4, "movement_type", "proven"),
    (0x1B8, 4, "event_low_health", "hypothesis"),
    (0x1BC, 4, "event_morale_failure", "proven"),
    (0x1C0, 4, "event_timeout", "proven"),
    (0x1C4, 4, "event_death", "hypothesis"),
    (0x1C8, 4, "hits", "hypothesis"),
    (0x1CC, 4, "target_zone_threshold", "proven"),
    (0x1D0, 4, "target_history_offset", "proven"),
    (0x1D4, 4, "max_health", "proven"),
    (0x1D8, 4, "shield0", "proven"),
    (0x1DC, 4, "shield1", "proven"),
    (0x1E0, 4, "shield2", "proven"),
    (0x1E4, 4, "shield3", "proven"),
    (0x1E8, 4, "expire_ticks", "proven"),
    (0x1EC, 4, "event_expire", "proven"),
    (0x1F0, 4, "event_create", "proven"),
    (0x1F4, 4, "event_damaged", "proven"),
    (0x1F8, 4, "event_destroy", "hypothesis"),
    (0x1FC, 4, "sound_id", "proven"),
    (0x200, 4, "sound_volume", "hypothesis"),
    (0x204, 4, "leash_location", "proven"),
    (0x208, 4, "texture_a", "proven"),
    (0x20C, 4, "texture_b", "proven"),
    (0x210, 4, "timer_period", "hypothesis"),
    (0x214, 4, "timer_step", "hypothesis"),
    (0x218, 4, "weapon_count", "proven"),
    (0x21C, 4, "weapon_id[8]", "proven"),
    (0x23C, 4, "weapon_p0[8]", "hypothesis"),
    (0x25C, 4, "weapon_p1[8]", "hypothesis"),
    (0x27C, 4, "weapon_p2[8]", "hypothesis"),
    (0x29C, 4, "weapon_p3[8]", "unknown"),
    (0x2BC, 4, "weapon_p4[8]", "unknown"),
    (0x2DC, 4, "weapon_p5[8]", "unknown"),
    (0x2FC, 4, "children_count", "proven"),
    (0x300, 4, "child_type[4]", "proven"),
    (0x310, 4, "child_param[4]", "proven"),
    (0x320, 4, "path_count", "proven"),
    (0x324, 4, "path_point[32]", "proven"),
]

#: Runtime instance record (``0x7ac080``, stride ``0x528``, 260 slots).
#: ``(offset, size, name, source, evidence, status)``.
#: ``source`` is the ``THING_TYPES`` offset the field is initialised from
#: (``"-"`` = computed/zeroed at spawn), ``evidence`` the consumer instructions.
INSTANCE_MAP = [
    (0x000, 24, "saved_position", "-", "0x46fb50 writes object+0x00 here before the dump; 0x46fd90 restores", "proven"),
    (0x018, 24, "angles_cur", "ADD_THING A/D operands", "0x46d9xx (spawn), 0x41e025 (matrix), 0x470c70, 0x46ea40", "proven"),
    (0x030, 24, "angles_next", "-", "0x470c70 initialises from +0x18, movement 1 adds rate*dt", "hypothesis"),
    (0x048, 24, "angle_rate", "0x090/0x098/0x0a0", "0x46d9b6..0x46d9f2, 0x470c70 case 1", "proven"),
    (0x060, 8, "turn_accel", "0x020", "0x46dd41, 0x470c70 (+0x58 -= dt*+0x60)", "proven"),
    (0x068, 8, "turn_max", "0x028", "0x46dd59, 0x470c70", "proven"),
    (0x070, 8, "angle_limit", "ADD_THING operand", "0x46df80 (param_13), 0x470c70 clamps +0x40", "hypothesis"),
    (0x078, 8, "collision_radius", "0x030", "0x46dc99, 0x46d7b0 (0x43c51f), spawn warning 0x46e2a0", "proven"),
    (0x080, 24, "collision_box", "0x038/0x040/0x048", "0x46dcb1..0x46dce1, 0x46d7b0 args 0x7ac100..", "proven"),
    (0x098, 8, "f50", "0x050", "0x46dcf9, no reader found", "unknown"),
    (0x0A0, 8, "speed", "0x010", "0x46dd11, +=arg 0x46d0fc; 0x4752d0 speed*dt; 0x46ea40", "proven"),
    (0x0A8, 8, "f18", "0x018", "0x46dd29, no reader found", "unknown"),
    (0x0B0, 8, "search_radius", "0x058", "0x46dc03, 0x473a00/0x477f90 scans", "proven"),
    (0x0B8, 8, "f68", "0x068", "0x46daa6, 0x470c70 (range gate)", "hypothesis"),
    (0x0C0, 8, "arrive_radius", "0x070", "0x46dbe5, movement helpers 0x474ea0", "hypothesis"),
    (0x0C8, 8, "damping", "0x080", "0x46def9, 0x4752d0 (speed *= damping on a bounce)", "proven"),
    (0x0D0, 8, "f88", "0x088", "0x46df05, no reader found", "unknown"),
    (0x0D8, 8, "gravity", "0x078", "0x46da76, 0x4752d0 (+0xf0 -= +0xd8*g*dt*-0.2)", "proven"),
    (0x0E0, 8, "1.0", "-", "0x46daf6 (0x3ff00000 hi word)", "proven"),
    (0x0E8, 8, "1.0", "-", "0x46db05", "proven"),
    (0x0F0, 8, "vertical_speed", "-", "0x4752d0 (vz), 0x470c70", "proven"),
    (0x0F8, 64, "weapon_p3[8]", "0x29C+4i", "0x46ddxx loop, int -> double", "unknown"),
    (0x138, 64, "weapon_p4[8]", "0x2BC+4i", "same loop", "unknown"),
    (0x178, 64, "weapon_p5[8]", "0x2DC+4i", "same loop", "unknown"),
    (0x1B8, 24, "velocity", "-", "0x470c70 zeroes then accumulates matrix*dv*dt", "proven"),
    (0x1D0, 8, "falling_fear", "0x0C8", "0x46da5e", "proven"),
    (0x1D8, 8, "fall_accel", "-", "0x46daee zeroes it; 0x470c70 uses it as a rate", "hypothesis"),
    (0x1E0, 8, "fall_max", "-", "0x46daf8 zeroes it; 0x470c70 clamps to it", "hypothesis"),
    (0x1E8, 8, "fall_speed", "-", "0x470c70 +/- rate, clamped to +0x1e0", "proven"),
    (0x208, 8, "impulse_fwd", "-", "0x470c70 (+0x1b8/+0x1c0 += matrix*impulse*dt)", "hypothesis"),
    (0x210, 8, "impulse_z", "-", "0x470c70 (+0x1c8 += impulse*dt)", "hypothesis"),
    (0x218, 8, "0.05", "-", "0x46dfa8 (0x3fa999999999999a)", "proven"),
    (0x228, 32, "f_a8..f_c4", "0x0A8/0x0B0/0x0B8/0x0C0", "0x46d9fe..0x46da52, no reader found", "unknown"),
    (0x278, 24, "spawn_position", "ADD_THING LLL operands", "0x46dbe9..; overridden by 0x204 location", "hypothesis"),
    (0x290, 8, "leash_distance", "0x0D0", "0x46dbbf, zeroed when radius<=0", "hypothesis"),
    (0x2F8, 24, "direction", "object+0x30 matrix row", "0x46e1xx (6 dwords from the model), 0x4752d0", "proven"),
    (0x310, 4, "parent", "spawn arg", "0x46e073 (param_17), 0x470130, 0x470c70", "proven"),
    (0x314, 4, "child_count", "-", "0x46e07b, 0x46d000 append, 0x470130", "proven"),
    (0x318, 32, "child_slots[8]", "-", "0x46d000 (0x7ac398 + count*4)", "proven"),
    (0x328, 4, "event_death", "0x1C4", "0x46da8e, 0x4752d0 posts it", "proven"),
    (0x32C, 4, "hits", "0x1C8", "0x46da9a, 0x475615/0x47573a", "proven"),
    (0x330, 4, "collision_kind", "0x1A0", "0x46dc8d, 0x46e282 switch", "proven"),
    (0x334, 4, "target_zone_threshold", "0x1CC", "0x46db2c, 0x473fb4", "proven"),
    (0x338, 4, "target_history_offset", "0x1D0", "0x46db32, 0x476523", "proven"),
    (0x33C, 4, "object", "-", "0x46d000 (0x4277cd handle), read by 0x46ca40", "proven"),
    (0x340, 16, "zeroed", "-", "0x46e0xx, no reader found", "unknown"),
    (0x360, 4, "flags_a", "0x1A8", "0x46d992; slot_alloc 0x46d5e0; 0x470c70", "proven"),
    (0x364, 4, "flags_b", "0x1AC", "0x46d99e, 0x470c70 (0x20000 = inert)", "proven"),
    (0x368, 4, "flags_c", "0x1B0", "0x46d9aa, 0x470500/0x470c70", "proven"),
    (0x36C, 4, "model_index", "0x1A4", "0x46d8d0 (param_2), 0x4677b0; DAT_007ac3ec", "proven"),
    (0x370, 4, "expire_ticks", "0x1E8", "0x46d97a, 0x470c70 -> 0x4703a0", "proven"),
    (0x374, 4, "event_expire", "0x1EC", "0x46d986, 0x4703a0", "proven"),
    (0x378, 4, "timeout_timer", "-", "0x46daeb, 0x470c70 counts down", "proven"),
    (0x37C, 4, "event_timeout", "0x1C0", "0x46dadc, 0x470c70 posts it", "proven"),
    (0x380, 4, "render_byte", "spawn arg", "0x46e0xx -> 0x46ca40 writes object+0x74", "hypothesis"),
    (0x384, 4, "effect_id", "-", "0x46e219 (0x40), 0x46ca40 -> 0x428421", "hypothesis"),
    (0x388, 4, "effect_timer", "-", "0x46e223, 0x470c70/0x470500", "hypothesis"),
    (0x38C, 4, "anim_state", "0x20C?", "0x46e210 <- spawned .soo (0x4284bb), 0x46ca40", "hypothesis"),
    (0x390, 4, "movement_type", "0x1B4", "0x46deed, 0x470c70 switch", "proven"),
    (0x394, 4, "event_low_health", "0x1B8", "0x46dabe, 0x470c70/0x470420", "proven"),
    (0x398, 4, "health", "0x1D4", "0x46dc21, 0x470420/0x470500", "proven"),
    (0x39C, 4, "max_health", "0x1D4", "0x46dc1b, 0x470420 (50% event)", "proven"),
    (0x3A0, 16, "shields", "0x1D8..0x1E4", "0x46dc4b..0x46dc69, 0x470420", "proven"),
    (0x3B0, 16, "shield_max", "0x1D8..0x1E4", "0x46dc2d..0x46dc45, 0x470420 clamp", "proven"),
    (0x3C0, 4, "texture_a", "0x208", "0x46df2f, 0x46ca40 -> 0x4289df", "proven"),
    (0x3C4, 4, "texture_b", "0x20C", "0x46df35, 0x46ca40", "proven"),
    (0x3C8, 4, "sector", "-", "0x4360f1 at spawn; 0x470c70 sector move; 0x472790", "proven"),
    (0x3CC, 4, "sound_volume", "0x200", "0x46ded5", "hypothesis"),
    (0x3D0, 4, "sound_id", "0x1FC", "0x46dee1", "proven"),
    (0x3D4, 4, "event_create", "0x1F0", "0x46dc75, 0x46e275", "proven"),
    (0x3D8, 4, "event_destroy", "0x1F8", "0x46dc81, 0x4702e9", "proven"),
    (0x3DC, 4, "type_index", "spawn arg", "0x46d96e, 0x470c70 (>>9 flag unpack)", "proven"),
    (0x3E0, 4, "last_attacker", "-", "0x46df62 (-1), 0x470500", "proven"),
    (0x3E4, 4, "attack_cooldown", "-", "0x46df56, 0x470c70, 0x4752d0 (+0x458)", "proven"),
    (0x3E8, 4, "unused", "-", "0x46df68", "unknown"),
    (0x3EC, 4, "timer_period", "0x210", "0x46dd71", "hypothesis"),
    (0x3F0, 4, "timer_step", "0x214", "0x46dd77", "hypothesis"),
    (0x3F4, 4, "weapon_count", "0x218", "0x46dd87, 0x4752d0", "proven"),
    (0x3F8, 32, "weapon_id[8]", "0x21C+4i", "0x46de6e/0x46decf, 0x4752d0 event", "proven"),
    (0x418, 32, "weapon_p0[8]", "0x23C+4i", "0x46de76, 0x473def", "hypothesis"),
    (0x438, 32, "weapon_p1[8]", "0x25C+4i", "0x46de7c, 0x4752d0 range", "hypothesis"),
    (0x458, 32, "weapon_p2[8]", "0x27C+4i", "0x46de82, 0x4752d0 cooldown", "hypothesis"),
    (0x478, 4, "anim_id", "spawn arg", "0x46e0xx -> 0x43b639/0x43b1b6 (.soo); 0x46fb50 refreshes it from object+0x78", "hypothesis"),
    (0x47C, 4, "owner", "spawn arg", "0x46e09b, 0x4752d0 (skip owner), 0x470500", "proven"),
    (0x480, 4, "path_count", "0x320", "0x46e08d (0x777080)", "proven"),
    (0x484, 128, "path_point[32]", "0x324+4i", "0x46e0xx copy loop", "proven"),
    (0x504, 4, "attached", "-", "0x46df7a (-1)", "unknown"),
    (0x508, 4, "timer_a", "-", "0x46df8c, 0x470c70 (0xaaa on expiry)", "hypothesis"),
    (0x50C, 4, "timer_b", "-", "0x46df92, 0x470c70", "hypothesis"),
    (0x510, 4, "spawn_param", "spawn arg", "0x46e032 (param), 0x46ea40 (-1)", "unknown"),
    (0x514, 4, "event_morale", "0x1BC", "0x46daca", "proven"),
    (0x518, 4, "event_damaged", "0x1F4", "0x46db17", "proven"),
    (0x51C, 4, "move_timer", "-", "0x46dfbc, 0x470c70 (signed phase timer)", "hypothesis"),
    (0x520, 4, "unused", "-", "0x46dfc2", "unknown"),
    (0x524, 4, "unused", "-", "0x46db23", "unknown"),
]

#: Proved ``instance <- type`` copies (``0x46d8b0``), ``(inst_off, type_off)``.
#: Arrays are given once with their stride in the comment.
TYPE_TO_INSTANCE = [
    (0x048, 0x090), (0x050, 0x098), (0x058, 0x0A0),      # angle rates
    (0x060, 0x020), (0x068, 0x028),                      # turn accel / max
    (0x078, 0x030), (0x080, 0x038), (0x088, 0x040), (0x090, 0x048),
    (0x098, 0x050),
    (0x0A0, 0x010), (0x0A8, 0x018),
    (0x0B0, 0x058), (0x0B8, 0x068), (0x0C0, 0x070),
    (0x0C8, 0x080), (0x0D0, 0x088),
    (0x0D8, 0x078),
    (0x1D0, 0x0C8),
    (0x228, 0x0A8), (0x230, 0x0B0), (0x238, 0x0B8), (0x240, 0x0C0),
    (0x290, 0x0D0),
    (0x328, 0x1C4), (0x32C, 0x1C8), (0x330, 0x1A0), (0x334, 0x1CC),
    (0x338, 0x1D0), (0x360, 0x1A8), (0x364, 0x1AC), (0x368, 0x1B0),
    (0x370, 0x1E8), (0x374, 0x1EC), (0x37C, 0x1C0),
    (0x390, 0x1B4), (0x394, 0x1B8),
    (0x398, 0x1D4), (0x39C, 0x1D4),
    (0x3A0, 0x1D8), (0x3A4, 0x1DC), (0x3A8, 0x1E0), (0x3AC, 0x1E4),
    (0x3B0, 0x1D8), (0x3B4, 0x1DC), (0x3B8, 0x1E0), (0x3BC, 0x1E4),
    (0x3C0, 0x208), (0x3C4, 0x20C),
    (0x3CC, 0x200), (0x3D0, 0x1FC), (0x3D4, 0x1F0), (0x3D8, 0x1F8),
    (0x3EC, 0x210), (0x3F0, 0x214), (0x3F4, 0x218),
    (0x514, 0x1BC), (0x518, 0x1F4),
]

#: Weapon / path columns (type stride 4, instance stride 4 or 8).
TYPE_TO_INSTANCE_COLUMNS = [
    ("weapon_id[8]", 0x3F8, 4, 0x21C, 4, ""),
    ("weapon_p0[8]", 0x418, 4, 0x23C, 4, ""),
    ("weapon_p1[8]", 0x438, 4, 0x25C, 4, ""),
    ("weapon_p2[8]", 0x458, 4, 0x27C, 4, ""),
    ("weapon_p3[8]", 0x0F8, 8, 0x29C, 4, "int -> double"),
    ("weapon_p4[8]", 0x138, 8, 0x2BC, 4, "int -> double"),
    ("weapon_p5[8]", 0x178, 8, 0x2DC, 4, "int -> double"),
    ("path_point[32]", 0x484, 4, 0x324, 4, ""),
    ("child_type[4]", 0x300, 4, 0x300, 4, "type table only; spawn uses thing_add"),
]

#: ``(instance field, C++ surface, test)``.  ``Things``/``Ai``/``Projectiles``
#: live in ``engine/src``; the instance record is not modelled by C++ (the
#: engine creates it in ``0x46d000``), so this is the wiring table for the
#: *type* fields that feed the C++ runtime.
CPP_COVERAGE = [
    ("+0x010 speed", "Thing::speed, ProjectileSpawn::speed", "test_things rocket 18.0 / grenade 2.0"),
    ("+0x030 radius", "Thing::radius (swept sphere)", "test_things rocket/grenade 10.0"),
    ("+0x038/40/48 box", "Thing::box[3]", "test_things rocket 4/10/4"),
    ("+0x058 search_radius", "Thing::search_radius", "test_things 6000.0 (contract)"),
    ("+0x068 f68", "not read", "none"),
    ("+0x070 arrive_radius", "AiConfig::move_arrive_radius / Thing::arrive_radius", "test_ai arrived"),
    ("+0x078 f78", "Thing::damping? -> ProjectileSpawn::damping", "test_things damping 0.8"),
    ("+0x080 f80", "ProjectileSpawn::gravity (TT+0x80 per docs/PROJECTILE_CPP §2)", "test_things gravity 1.0/0.0"),
    ("+0x090..0a0 col_f*", "not read", "none"),
    ("+0x0C8 falling_fear", "not read", "none"),
    ("+0x0D0 leash_distance", "Thing::leash_distance", "test_ai leash_and_return"),
    ("+0x1A0 collision_kind", "Thing::collision_shape", "test_things 1/2"),
    ("+0x1A4 sob_index", "Thing::model / model_index", "test_things type_events, things_full"),
    ("+0x1A8/1AC/1B0 flags", "Thing::flags (entity flags only), status_flags", "none"),
    ("+0x1B4 movement_type", "Thing::movement_type, ai_path_mode", "test_things 6; test_ai path modes"),
    ("+0x1B8 event_low_health", "Thing::damaged_event? (not wired)", "none"),
    ("+0x1BC event_morale", "not read", "none"),
    ("+0x1C0 event_timeout", "Thing::timeout_event", "test_events 1004"),
    ("+0x1C4 event_death", "Thing::collide_event", "test_things -30103/-30094"),
    ("+0x1C8 hits", "Thing::hits_left, ProjectileSpawn::hits_left", "test_things 0/1"),
    ("+0x1CC target_zone", "Thing::target_zone_threshold", "test_things 200"),
    ("+0x1D0 history lag", "Thing::target_history_offset, ai_history_lag", "test_ai target_history"),
    ("+0x1D4 max_health", "Thing::max_health/health, Ai max_health", "test_things 2000/6000/10000"),
    ("+0x1D8..1E4 shields", "Thing::shields / shield_max", "test_things 0/500"),
    ("+0x1E8 expire_ticks", "Thing::expire_ticks, Things::tick", "test_things expire_lifetime"),
    ("+0x1EC event_expire", "Thing::expire_event", "test_things -30093/-30103"),
    ("+0x1F0 event_create", "Thing::create_event, Things::spawn", "test_things type_events"),
    ("+0x1F4 event_damaged", "Thing::damaged_event", "test_events 1001"),
    ("+0x1F8 event_destroy", "Thing::destroy_event, Things::remove", "test_events 1005"),
    ("+0x1FC/200 sound", "not read", "none"),
    ("+0x204 leash_location", "not read (locations table exists)", "none"),
    ("+0x208/20C textures", "not read (render)", "none"),
    ("+0x210/214 timers", "not read", "none"),
    ("+0x218 weapon_count", "Thing::proximity size source? (Ai weapons)", "test_things proximity"),
    ("+0x21C+4i weapon ids", "AiWeapon::event, Thing::proximity[i].event", "test_ai weapons, test_things proximity"),
    ("+0x23C/25C bands", "ProximityEntry::low/high, AiWeapon::low/high", "test_things high 20"),
    ("+0x27C cooldown", "ProximityEntry::cooldown", "test_things proximity"),
    ("+0x29C/2BC/2DC offsets", "ProximityEntry::offset", "test_things proximity"),
    ("+0x2FC..310 children", "not read (engine spawns via thing_add)", "none"),
    ("+0x320/324 path", "Thing::path_locations, Ai::set_path_from_thing", "test_ai path_locations"),
]

#: Pairs a naive "last load into this register" extraction reports although the
#: value is *not* copied from the type record (Ghidra reuses the register):
#: +0x38c comes from FUN_004284bb (the .soo player) and +0x480 (path count) from
#: ``&DAT_00777080`` = type+0x320, not from the still-loaded type+0x218.
EXTRACTION_EXCEPTIONS = {
    (0x38C, 0x20C): "computed: 0x46e210 stores FUN_004284bb() (.soo animation)",
    (0x480, 0x218): "real source is type+0x320 (0x777080); register reuse",
}

VALID_RECORD_IDS = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 255}


# ---------------------------------------------------------------------------
# minimal structural .sbt scanner (no dependency on gm/)
# ---------------------------------------------------------------------------
def _plausible_names(buf, off, count, size):
    for i in range(count):
        chunk = buf[off + i * size:off + i * size + 16]
        if not chunk or chunk[0] < 0x20:
            return False
        name = chunk.split(b"\0")[0]
        if any(c < 0x20 or c > 0x7E for c in name):
            return False
    return True


def _valid_type_record(rec):
    """Structural sanity of one 936-byte ``THING_TYPES`` record."""
    name = rec[:16]
    if b"\0" not in name or name[0] < 0x20:
        return False
    head = name.split(b"\0")[0]
    if not head or any(c < 0x20 or c > 0x7E for c in head):
        return False
    if not any(chr(c).isalnum() for c in head):
        return False
    sob = struct.unpack_from("<i", rec, 0x1A4)[0]
    move = struct.unpack_from("<i", rec, 0x1B4)[0]
    flags_a = struct.unpack_from("<I", rec, 0x1A8)[0]
    return 0 <= sob <= 0xFF and 0 <= move <= 0x40 and bool(flags_a & 1)


def scan_table(buf, record_id, size, max_count):
    """Find ``[record_id][u32 n][n * record]`` tables; return ``(offset, n)``.

    The record byte is not a unique value, so a candidate is only accepted when
    all ``n`` records start with a printable NUL-terminated name (the first
    field of every table this scanner knows) and the payload fits.
    """
    out = []
    for off in range(1, len(buf) - 4):
        if buf[off - 1] != record_id:
            continue
        n = struct.unpack_from("<I", buf, off)[0]
        if not 1 <= n <= max_count:
            continue
        payload = off + 4
        if payload + n * size > len(buf):
            continue
        if record_id == 4:
            if all(_valid_type_record(buf[payload + i * size:payload + (i + 1) * size])
                   for i in range(n)):
                out.append((off, n))
        elif _plausible_names(buf, payload, n, size):
            out.append((off, n))
    return out


def best_table(buf, record_id, size, max_count):
    """Pick the largest candidate; ``level14`` has two 1-record false hits."""
    hits = scan_table(buf, record_id, size, max_count)
    if not hits:
        return None, []
    hits.sort(key=lambda h: -h[1])
    return hits[0], hits


def make_level_tables(buf, types, sobs):
    """Parse one ``THING_TYPES`` + ``SOB_TABLE`` pair into plain dicts."""
    (toff, tn) = types
    p = toff + 4
    out = []
    for i in range(tn):
        rec = buf[p + i * TYPE_SIZE:p + (i + 1) * TYPE_SIZE]
        name = rec[:16].split(b"\0")[0].decode("latin-1")
        params = struct.unpack_from("<49d", rec, 0x10)
        tail = rec[0x198:]
        sob = struct.unpack_from("<i", rec, 0x1A4)[0]
        out.append({"name": name, "params": params, "tail": tail, "sob": sob})
    sob_names = []
    if sobs:
        (soff, sn) = sobs
        q = soff + 4
        for i in range(sn):
            sob_names.append(buf[q + i * 20:q + i * 20 + 16].split(b"\0")[0].decode("latin-1"))
    return out, sob_names


def tail_i32(tail, off):
    """``tail`` starts at +0x198; read a signed int at an absolute offset."""
    return struct.unpack_from("<i", tail, off - 0x198)[0]


def tail_u32(tail, off):
    return struct.unpack_from("<I", tail, off - 0x198)[0]


# ---------------------------------------------------------------------------
# save (THING block) scan
# ---------------------------------------------------------------------------
def walk_thing_block(buf, off, slots=INST_SLOTS):
    """Decode ``slots x (u8 present + optional 0x528)``; raise ``ValueError``."""
    p = off
    recs = []
    for i in range(slots):
        if p >= len(buf):
            raise ValueError("truncated at slot %d" % i)
        present = buf[p]
        if present not in (0, 1):
            raise ValueError("slot %d: present=%d" % (i, present))
        p += 1
        if present:
            if p + INST_SIZE > len(buf):
                raise ValueError("slot %d: truncated record" % i)
            recs.append((i, buf[p:p + INST_SIZE]))
            p += INST_SIZE
    return recs, p


def find_thing_block(buf):
    """Locate the save-chain THING block (chain id 3) without other modules."""
    hits = []
    for off in range(1, len(buf) - INST_SLOTS):
        if buf[off - 1] != 3:
            continue
        try:
            recs, end = walk_thing_block(buf, off)
        except ValueError:
            continue
        if not recs:
            continue
        if end < len(buf) and buf[end] not in VALID_RECORD_IDS:
            continue
        # every live slot must have flags_a bit 0 set (0x46d5e0 contract)
        if any(not (struct.unpack_from("<I", r, 0x360)[0] & 1) for _, r in recs):
            continue
        hits.append((off, recs, end))
    return hits


def inst_i32(rec, off):
    return struct.unpack_from("<i", rec, off)[0]


def inst_u32(rec, off):
    return struct.unpack_from("<I", rec, off)[0]


def inst_f64(rec, off):
    return struct.unpack_from("<d", rec, off)[0]


# ---------------------------------------------------------------------------
# reports
# ---------------------------------------------------------------------------
def describe_types(entries):
    """Aggregate the per-level type table into the numbers quoted in the doc."""
    n = len(entries)
    move = {}
    flags_a_bit0 = 0
    hp_zero = 0
    hp_max = 0
    exp_nonzero = 0
    exp_max = 0
    weap = {}
    path = {}
    rad_min = None
    rad_max = 0.0
    bad_models = 0
    for e in entries:
        m = tail_i32(e["tail"], 0x1B4)
        move[m] = move.get(m, 0) + 1
        if tail_u32(e["tail"], 0x1A8) & 1:
            flags_a_bit0 += 1
        hp = tail_i32(e["tail"], 0x1D4)
        if hp == 0:
            hp_zero += 1
        hp_max = max(hp_max, hp)
        ex = tail_i32(e["tail"], 0x1E8)
        if ex > 0:
            exp_nonzero += 1
            exp_max = max(exp_max, ex)
        w = tail_i32(e["tail"], 0x218)
        weap[w] = weap.get(w, 0) + 1
        pc = tail_i32(e["tail"], 0x320)
        path[pc] = path.get(pc, 0) + 1
        r = e["params"][4] if len(e["params"]) > 4 else 0.0
        rad_min = r if rad_min is None else min(rad_min, r)
        rad_max = max(rad_max, r)
        if e["sob"] >= 0 and not (0 <= e["sob"]):
            bad_models += 1
    return {
        "types": n,
        "movement_types": dict(sorted(move.items())),
        "flags_a_bit0": flags_a_bit0,
        "max_health_zero": hp_zero,
        "max_health_max": hp_max,
        "expire_nonzero": exp_nonzero,
        "expire_max": exp_max,
        "weapon_count": dict(sorted(weap.items())),
        "path_count": dict(sorted(path.items())),
        "radius_min": rad_min,
        "radius_max": rad_max,
        "bad_models": bad_models,
    }


def example_types(entries, sob_names, wanted):
    """Pretty lines for ``wanted`` indexes plus the first projectile/AI type."""
    picked = []
    seen = set()

    def add(i):
        if i in seen or not (0 <= i < len(entries)):
            return
        seen.add(i)
        e = entries[i]
        sob = sob_names[e["sob"]] if 0 <= e["sob"] < len(sob_names) else "?"
        picked.append(
            "  [%3d] %-12s model=%-10s move=%-2d hp=%-6d shields=%s hits=%d expire=%d "
            "flags=%08x/%08x/%08x speed=%.3f radius=%.1f search=%.1f weap=%d path=%d"
            % (i, e["name"], sob, tail_i32(e["tail"], 0x1B4), tail_i32(e["tail"], 0x1D4),
               [tail_i32(e["tail"], 0x1D8 + 4 * k) for k in range(4)],
               tail_i32(e["tail"], 0x1C8), tail_i32(e["tail"], 0x1E8),
               tail_u32(e["tail"], 0x1A8), tail_u32(e["tail"], 0x1AC), tail_u32(e["tail"], 0x1B0),
               e["params"][0], e["params"][4], e["params"][9],
               tail_i32(e["tail"], 0x218), tail_i32(e["tail"], 0x320)))

    for i in wanted:
        add(i)
    for i, e in enumerate(entries):
        mt = tail_i32(e["tail"], 0x1B4)
        if mt == 6 and tail_i32(e["tail"], 0x1E8) > 0:
            add(i)
            break
    for i, e in enumerate(entries):
        if tail_i32(e["tail"], 0x218) > 1 and tail_i32(e["tail"], 0x1B4) in (14, 15):
            add(i)
            break
    for i, e in enumerate(entries):
        if tail_i32(e["tail"], 0x1B4) == 18:
            add(i)
            break
    return picked


def describe_instances(recs, entries, sob_names):
    """Sanity-checks of the 260-slot array against the level's type table."""
    rows = []
    problems = []
    for slot, rec in recs:
        t = inst_i32(rec, 0x3DC)
        model = inst_i32(rec, 0x36C)
        name = entries[t]["name"] if 0 <= t < len(entries) else "?"
        sob = sob_names[model] if 0 <= model < len(sob_names) else "?"
        hp = inst_i32(rec, 0x398)
        hpmax = inst_i32(rec, 0x39C)
        rows.append(
            "  slot %3d type=%-3d %-12s model=%-3d %-10s move=%-2d sector=%-3d "
            "hp=%d/%d shields=%d/%d flags=%08x/%08x/%08x expire=%d parent=%d path=%d"
            % (slot, t, name, model, sob, inst_i32(rec, 0x390), inst_i32(rec, 0x3C8),
               hp, hpmax, inst_i32(rec, 0x3A0), inst_i32(rec, 0x3B0),
               inst_u32(rec, 0x360), inst_u32(rec, 0x364), inst_u32(rec, 0x368),
               inst_i32(rec, 0x370), inst_i32(rec, 0x310), inst_i32(rec, 0x480)))
        if not inst_u32(rec, 0x360) & 1:
            problems.append("slot %d: flags_a bit0 clear (slot would be free)" % slot)
        if entries and not (0 <= t < len(entries)):
            problems.append("slot %d: type %d outside 0..%d" % (slot, t, len(entries) - 1))
        if sob_names and not (0 <= model < len(sob_names)):
            problems.append("slot %d: model %d outside SOB_TABLE" % (slot, model))
        if hp > hpmax:
            problems.append("slot %d: health %d > max %d" % (slot, hp, hpmax))
        for k in range(4):
            if inst_i32(rec, 0x3A0 + 4 * k) > inst_i32(rec, 0x3B0 + 4 * k):
                problems.append("slot %d: shield %d above its max" % (slot, k))
        if not 0 <= inst_i32(rec, 0x390) <= 18:
            problems.append("slot %d: movement_type %d outside 0..18" % (slot, inst_i32(rec, 0x390)))
        a = [inst_f64(rec, 0x18), inst_f64(rec, 0x20), inst_f64(rec, 0x28)]
        if any(abs(x) > 7.0 for x in a):
            problems.append("slot %d: angles %s out of range" % (slot, ["%.3f" % x for x in a]))
    return rows, problems


# ---------------------------------------------------------------------------
# copy-map extraction from a Ghidra decompile
# ---------------------------------------------------------------------------
_LOAD = re.compile(r"(\w+)\s*=\s*\*\(\w[\w ]*\*\)\(&DAT_00776([0-9a-f]{3}) \+ \w+\)")
_STORE = [
    # *(T *)(&DAT_007acXXX + iVar13) = rhs
    (re.compile(r"\*\(\w[\w ]*\*\)\(&DAT_007ac([0-9a-f]{3}) \+ \w+\)\s*=\s*([^;]+);"), 0),
    # (&DAT_007acXXX)[param_3 * 0x14a] = rhs
    (re.compile(r"\(&DAT_007ac([0-9a-f]{3})\)\[[^\]]*\]\s*=\s*([^;]+);"), 0),
    # *(T *)(&DAT_007acXXX + param_3 * 0xa5) = rhs   (double* scaling of 0x528)
    (re.compile(r"\*\(\w[\w ]*\*\)\(&DAT_007ac([0-9a-f]{3}) \+ \w+ \* 0xa5\)\s*=\s*([^;]+);"), 0),
    # *(T *)((int)&DAT_007acXXX + iVar13 + 4) = rhs   (high half of a double)
    (re.compile(r"\*\(\w[\w ]*\*\)\(\(int\)&DAT_007ac([0-9a-f]{3}) \+ \w+ \+ 4\)\s*=\s*([^;]+);"), 4),
]


def extract_copy_map(text):
    """Return ``{(inst_off, type_off)}`` from a decompile of ``0x46d8b0``."""
    regs = {}
    out = set()
    for line in text.splitlines():
        m = _LOAD.search(line)
        if m:
            regs[m.group(1)] = 0x776000 + int(m.group(2), 16)
        for rx, bias in _STORE:
            m = rx.search(line)
            if not m:
                continue
            inst = 0x7AC000 + int(m.group(1), 16) + bias
            rhs = m.group(2).strip()
            src = None
            if re.fullmatch(r"\w+", rhs) and rhs in regs:
                src = regs[rhs]
            mm = re.search(r"&DAT_00776([0-9a-f]{3})", rhs)
            if mm:
                src = 0x776000 + int(mm.group(1), 16)
            if src is not None:
                out.add((inst - INST_BASE, src - TYPE_BASE))
    return out


# ---------------------------------------------------------------------------
# verification
# ---------------------------------------------------------------------------
def read_level(path):
    buf = open(path, "rb").read()
    types, all_types = best_table(buf, 4, TYPE_SIZE, 0x80)
    sobs, all_sobs = best_table(buf, 20, 20, 0xC8)
    return buf, types, all_types, sobs, all_sobs


def verify_file(path):
    """Return ``(ok, lines, problems)`` for a level ``.sbt`` or a save."""
    if not os.path.isfile(path):
        return False, ["  not a file"], ["not a file"]
    buf = open(path, "rb").read()
    lines = []
    problems = []
    if buf[:12] == b"A SAVED GAME" or b"A SAVED GAME" in buf[:64]:
        hits = find_thing_block(buf)
        if not hits:
            return False, ["  no THING block"], ["no THING block found"]
        off, recs, end = hits[0]
        level_name = buf[0x54:0x64].split(b"\0")[0].decode("latin-1")
        entries, sob_names = [], []
        cand = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(path))),
                            level_name.lower() + ".sbt")
        for p in (cand, os.path.join(os.path.dirname(os.path.abspath(path)),
                                     level_name.lower() + ".sbt")):
            if os.path.exists(p):
                _, types, _, sobs, _ = read_level(p)
                if types:
                    entries, sob_names = make_level_tables(open(p, "rb").read(), types, sobs)
                break
        rows, probs = describe_instances(recs, entries, sob_names)
        lines.append("  save '%s' level %s: THING block at %#x, %d/%d slots live, ends %#x"
                     % (os.path.basename(path), level_name or "?", off, len(recs), INST_SLOTS, end))
        lines.extend(rows[:3])
        problems.extend(probs)
        return not problems, lines, problems
    _buf, types, all_types, sobs, all_sobs = read_level(path)
    if not types:
        return False, ["  no THING_TYPES table (not a level .sbt?)"], ["THING_TYPES not found"]
    entries, sob_names = make_level_tables(buf, types, sobs)
    agg = describe_types(entries)
    bad = [e for e in entries if not (0 <= e["sob"] < len(sob_names))] if sob_names else []
    if bad:
        problems.append("%d type(s) point outside SOB_TABLE" % len(bad))
    if len(all_types) > 1:
        lines.append("  note: %d THING_TYPES candidates %s, using %s"
                     % (len(all_types), all_types, types))
    lines.append("  %s: %d types @ %#x, SOB_TABLE %d @ %s, model refs %d/%d valid"
                 % (os.path.basename(path), agg["types"], types[0],
                    len(sob_names), ("%#x" % sobs[0]) if sobs else "-",
                    agg["types"] - len(bad), agg["types"]))
    lines.append("    movement_type %s" % agg["movement_types"])
    lines.append("    flags_a bit0 set in %d/%d (instance slot contract)"
                 % (agg["flags_a_bit0"], agg["types"]))
    lines.append("    max_health 0 (immune) %d, max %d; expire>0 %d, max %d ticks"
                 % (agg["max_health_zero"], agg["max_health_max"],
                    agg["expire_nonzero"], agg["expire_max"]))
    lines.append("    weapon_count %s, path_count %s, radius %.1f..%.1f"
                 % (agg["weapon_count"], agg["path_count"],
                    agg["radius_min"] or 0.0, agg["radius_max"]))
    lines.extend(example_types(entries, sob_names, (0, 28, 31)))
    return not problems, lines, problems


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def _synthetic_level():
    """Two types + two SOB entries, laid out like a compiled ``.sbt``."""
    sob = bytearray(b"\x14" + struct.pack("<I", 2))
    for nm in (b"BIGCHUNK", b"GRENADE"):
        sob += nm + b"\0" * (16 - len(nm)) + b"\0" * 4
    tt = bytearray(b"\x04" + struct.pack("<I", 2))
    for i, (nm, sob_i, move, hp, exp, wpn) in enumerate((
            (b"bgchunk1", 0, 6, 2000, 145, 0),
            (b"Grenade", 1, 6, 2000, 364, 1))):
        rec = bytearray(TYPE_SIZE)
        rec[0:len(nm)] = nm
        struct.pack_into("<49d", rec, 0x10, *([0.0] * 49))
        struct.pack_into("<d", rec, 0x10, 2.0 if i else 0.0)
        struct.pack_into("<d", rec, 0x30, 10.0)
        struct.pack_into("<i", rec, 0x1A4, sob_i)
        struct.pack_into("<i", rec, 0x1A8, 5)
        struct.pack_into("<i", rec, 0x1B4, move)
        struct.pack_into("<i", rec, 0x1C8, 1)
        struct.pack_into("<i", rec, 0x1D4, hp)
        struct.pack_into("<i", rec, 0x1E8, exp)
        struct.pack_into("<i", rec, 0x218, wpn)
        tt += rec
    body = b"BINARY SAVE".ljust(80, b"\0") + struct.pack("<I", 0)
    return body + bytes(sob) + bytes(tt) + b"\xff"


def _synthetic_save():
    """One live slot whose fields match the embedded map."""
    rec = bytearray(INST_SIZE)
    struct.pack_into("<3d", rec, 0x18, 0.0, 0.0, 1.5707963267948966)
    struct.pack_into("<d", rec, 0xA0, 2.0)
    struct.pack_into("<i", rec, 0x310, -1)
    struct.pack_into("<i", rec, 0x360, 0x25)
    struct.pack_into("<i", rec, 0x36C, 1)
    struct.pack_into("<i", rec, 0x370, 364)
    struct.pack_into("<i", rec, 0x390, 6)
    struct.pack_into("<i", rec, 0x398, 2000)
    struct.pack_into("<i", rec, 0x39C, 2000)
    struct.pack_into("<i", rec, 0x3DC, 1)
    body = bytearray(b"A SAVED GAME" + b"\0" * 0x40)
    body[0x54:0x64] = b"LEVELX".ljust(16, b"\0")
    body += b"\0" * 64
    body += b"\x03"                       # chain id
    body += b"\x01" + bytes(rec)          # slot 0 live
    body += b"\x00" * (INST_SLOTS - 1)    # remaining slots empty
    body += b"\xff"
    return bytes(body)


_SYNTHETIC_SPAWN = """
  uVar4 = *(undefined4 *)(&DAT_00776f48 + iVar8);
  *(undefined4 *)(&DAT_007ac3f0 + iVar13) = uVar4;
  uVar2 = *(undefined4 *)(&DAT_00776f4c + iVar8);
  *(undefined4 *)(&DAT_007ac3f4 + iVar13) = uVar2;
  uVar4 = *(undefined4 *)(&DAT_00776f08 + iVar8);
  (&DAT_007ac3e0)[param_3 * 0x14a] = uVar4;
  uVar4 = *(undefined4 *)(&DAT_00776df0 + iVar8);
  *(undefined4 *)(&DAT_007ac0c8 + iVar13) = uVar4;
  uVar2 = *(undefined4 *)(&DAT_00776df4 + iVar8);
  *(undefined4 *)(&DAT_007ac0cc + iVar13) = uVar2;
"""


def self_test(level_dir=None, quiet=False):
    """Return ``(ok, report_lines)``; every failed check is listed."""
    out = []
    fails = []

    def check(name, cond, extra=""):
        out.append("  %-52s %s%s" % (name, "ok" if cond else "FAIL", (" " + extra) if extra and not cond else ""))
        if not cond:
            fails.append(name)

    # 1. table scanner on a synthetic level
    buf = _synthetic_level()
    types, cands = best_table(buf, 4, TYPE_SIZE, 0x80)
    sobs, _ = best_table(buf, 20, 20, 0xC8)
    check("synthetic level: 1 THING_TYPES candidate", types is not None and len(cands) == 1)
    check("synthetic level: 2 types", types == (len(buf) - 2 * TYPE_SIZE - 1, 2) or (types and types[1] == 2))
    check("synthetic level: SOB_TABLE found", sobs is not None and sobs[1] == 2)
    if types and sobs:
        entries, sob_names = make_level_tables(buf, types, sobs)
        check("synthetic level: type names", [e["name"] for e in entries] == ["bgchunk1", "Grenade"])
        check("synthetic level: model refs", [e["sob"] for e in entries] == [0, 1])
        check("synthetic level: speed/expire copy", entries[1]["params"][0] == 2.0
              and tail_i32(entries[1]["tail"], 0x1E8) == 364)
        agg = describe_types(entries)
        check("synthetic level: aggregate", agg["movement_types"] == {6: 2} and agg["flags_a_bit0"] == 2)
    else:
        check("synthetic level: parse", False)

    # 2. save walker
    save = _synthetic_save()
    hits = find_thing_block(save)
    check("synthetic save: one THING block", len(hits) == 1)
    if hits:
        _off, recs, _end = hits[0]
        check("synthetic save: one live slot", len(recs) == 1 and recs[0][0] == 0)
        rec = recs[0][1]
        check("synthetic save: type/model/health", (inst_i32(rec, 0x3DC), inst_i32(rec, 0x36C),
                                                    inst_i32(rec, 0x398)) == (1, 1, 2000))
        check("synthetic save: flags bit0 + angles", bool(inst_u32(rec, 0x360) & 1)
              and abs(inst_f64(rec, 0x28) - 1.5707963267948966) < 1e-12)
        broken = bytearray(save)
        broken[hits[0][0]] = 7            # present = 7 -> not a valid slot
        check("synthetic save: bad present byte rejected", not find_thing_block(bytes(broken)))

    # 3. copy-map extraction from a decompile fragment
    got = extract_copy_map(_SYNTHETIC_SPAWN)
    want = {(0x370, 0x1E8), (0x374, 0x1EC), (0x360, 0x1A8), (0x048, 0x090), (0x04C, 0x094)}
    check("extract: type->instance pairs", got == want, "got %s" % sorted(got))
    check("extract: known register-reuse artefacts listed",
          set(EXTRACTION_EXCEPTIONS) == {(0x38C, 0x20C), (0x480, 0x218)})

    # 4. embedded map consistency
    def covered(off, table):
        return any(base <= off < base + size for base, size, *_ in table)
    bad = [p for p in TYPE_TO_INSTANCE if not covered(p[0], INSTANCE_MAP)]
    check("map: every copy target is a documented field", not bad, str(bad))
    bad = [p for p in TYPE_TO_INSTANCE if not covered(p[1], TYPE_MAP)]
    check("map: every copy source is a documented type field", not bad, str(bad))
    check("map: instance fields inside 0x528",
          all(0 <= off < INST_SIZE and off + size <= INST_SIZE for off, size, *_ in INSTANCE_MAP))
    check("map: type fields inside 0x3a8",
          all(0 <= off < TYPE_SIZE and off + size <= TYPE_SIZE for off, size, *_ in TYPE_MAP))
    check("layout: 260 * 0x528 ends at 0x7ffd20", INST_BASE + INST_SLOTS * INST_SIZE == 0x7FFD20)
    check("layout: sector flags 0x800b60 + 0x50 = 0x800bb0",
          SECTOR_BASE + 0x50 == 0x800BB0 and SECTOR_BASE + SECTOR_SLOTS * SECTOR_SIZE == 0x806360)

    # 5. shipped files
    game = level_dir or default_game_dir()
    if game and os.path.isdir(game):
        for stem, want_types in (("level0", 57), ("dm1", 59)):
            path = os.path.join(game, stem + ".sbt")
            if not os.path.exists(path):
                out.append("  %-52s skip (no %s)" % ("shipped " + stem, path))
                continue
            ok, lines, probs = verify_file(path)
            agg = None
            b = open(path, "rb").read()
            _b, t, _at, s, _as = read_level(path)
            if t:
                ent, _sob = make_level_tables(b, t, s)
                agg = describe_types(ent)
            check("shipped %s: table + model refs" % stem, ok and not probs, "; ".join(probs))
            check("shipped %s: %d types" % (stem, want_types),
                  agg is not None and agg["types"] == want_types,
                  str(agg and agg["types"]))
            check("shipped %s: flags_a bit0 everywhere" % stem,
                  agg is not None and agg["flags_a_bit0"] == agg["types"])
            if stem == "level0" and agg:
                check("shipped level0: no type outside SOB_TABLE", agg["bad_models"] == 0)
        sav = os.path.join(game, "SAVEGAME", "saved0.sbt")
        if os.path.exists(sav):
            ok, lines, probs = verify_file(sav)
            check("shipped saved0: THING block + invariants", ok and not probs, "; ".join(probs))
            b = open(sav, "rb").read()
            hits = find_thing_block(b)
            if hits:
                recs = hits[0][1]
                tpath = os.path.join(game, "level0.sbt")
                tb = open(tpath, "rb").read()
                _tb, ttypes, _tat, tsobs, _tas = read_level(tpath)
                ent, sob = make_level_tables(tb, ttypes, tsobs)
                _rows, probs = describe_instances(recs, ent, sob)
                check("shipped saved0: 6 live slots, types < 57",
                      len(recs) == 6 and all(0 <= inst_i32(r[1], 0x3DC) < 57 for r in recs))
                check("shipped saved0: instance invariants", not probs, "; ".join(probs))
    else:
        out.append("  %-52s skip (game dir not found)" % "shipped files")
    return not fails, out


def default_game_dir():
    for cand in ("/mnt/d/games/Gunmetal 3dfx", "/mnt/c/Games/Gunmetal 3dfx"):
        if os.path.isdir(cand):
            return cand
    return os.environ.get("GM_GAME_DIR", "")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def print_map(as_json):
    if as_json:
        doc = {
            "type_base": TYPE_BASE, "type_size": TYPE_SIZE, "type_count_global": TYPE_COUNT,
            "types": [{"offset": o, "size": s, "name": n, "status": st}
                      for o, s, n, st in TYPE_MAP],
            "instance_base": INST_BASE, "instance_size": INST_SIZE,
            "instance_slots": INST_SLOTS,
            "instances": [{"offset": o, "size": s, "name": n, "source": src,
                           "evidence": ev, "status": st}
                          for o, s, n, src, ev, st in INSTANCE_MAP],
            "copies": [{"instance": i, "type": t} for i, t in TYPE_TO_INSTANCE],
            "columns": [{"name": n, "instance": i, "istride": istr, "type": t,
                         "tstride": tstride, "note": note}
                        for n, i, istr, t, tstride, note in TYPE_TO_INSTANCE_COLUMNS],
            "cpp": [{"field": f, "cpp": c, "test": t} for f, c, t in CPP_COVERAGE],
        }
        print(json.dumps(doc, indent=1))
        return
    print("THING_TYPES  base %#x  record %d (0x%x)  count @ %#x  (.sbt record 4)"
          % (TYPE_BASE, TYPE_SIZE, TYPE_SIZE, TYPE_COUNT))
    for o, s, n, st in TYPE_MAP:
        print("  +%#05x %3d  %-22s %s" % (o, s, n, st))
    print()
    print("THING INSTANCES  base %#x  record %d (0x%x)  %d slots -> %#x"
          % (INST_BASE, INST_SIZE, INST_SIZE, INST_SLOTS, INST_BASE + INST_SLOTS * INST_SIZE))
    for o, s, n, src, ev, st in INSTANCE_MAP:
        print("  +%.3x %4d  %-22s <- %-24s %-10s %s" % (o, s, n, src, st, ev))
    print()
    print("TYPE -> INSTANCE copies (0x46d8b0):")
    for i, t in TYPE_TO_INSTANCE:
        print("  %#05x <- type %#05x" % (i, t))
    for n, i, istr, t, tstride, note in TYPE_TO_INSTANCE_COLUMNS:
        print("  %s: %#05x +%d*i <- type %#05x +%d*i %s" % (n, i, istr, t, tstride, note))


def _copy_spans():
    """``(inst, type, size)`` for every embedded copy pair."""
    size_of = {off: size for off, size, *_ in INSTANCE_MAP}
    out = []
    for i, t in TYPE_TO_INSTANCE:
        out.append((i, t, size_of.get(i, 4)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="thing type / thing instance field maps")
    ap.add_argument("--map", action="store_true", help="print both field maps")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--verify", nargs="+", metavar="FILE",
                    help="check level .sbt files and/or saves")
    ap.add_argument("--extract", metavar="DECOMPILE",
                    help="re-derive the copy map from 'mcp.py decompile 0x46d8b0'")
    ap.add_argument("--self-test", action="store_true", help="synthetic + shipped checks")
    ap.add_argument("--game", help="game directory (default: /mnt/d/games/Gunmetal 3dfx)")
    args = ap.parse_args(argv)

    rc = 0
    if args.map or not (args.verify or args.extract or args.self_test):
        print_map(args.json)
    if args.extract:
        text = sys.stdin.read() if args.extract == "-" else open(args.extract).read()
        got = extract_copy_map(text)
        want = list(TYPE_TO_INSTANCE)
        print("extracted %d dword pairs, embedded %d field pairs" % (len(got), len(want)))

        def explained(pair):
            """Is ``(inst, type)`` part of an embedded field (same delta)?"""
            i, t = pair
            return any(ei <= i < ei + size and (t - et) == (i - ei)
                       for ei, et, size in _copy_spans())

        extra = sorted(p for p in got if not explained(p) and p not in EXTRACTION_EXCEPTIONS)

        def seen(ei, et, size):
            return any(ei <= i < ei + size and (t - et) == (i - ei) for i, t in got)

        missing = [(ei, et) for ei, et, size in _copy_spans() if not seen(ei, et, size)]
        if missing:
            print("  not found in the decompile: %s" % ["%#x<-%#x" % p for p in missing])
        known = sorted(p for p in got if p in EXTRACTION_EXCEPTIONS)
        for p in known:
            print("  known artefact %#x<-%#x: %s" % (p[0], p[1], EXTRACTION_EXCEPTIONS[p]))
        if extra:
            print("  pairs outside the map: %s" % ["%#x<-%#x" % p for p in extra])
        print("  copy map matches" if not missing and not extra else "  copy map differs")
        rc = 0 if not missing and not extra else 1
    if args.verify:
        ok_all = True
        for path in args.verify:
            b = open(path, "rb").read()
            ok, lines, probs = verify_file(path)
            print("%s%s" % (path, "" if ok else "  <-- FAIL"))
            for ln in lines:
                print(ln)
            for p in probs:
                print("    PROBLEM: %s" % p)
            ok_all = ok_all and ok
        rc = rc or (0 if ok_all else 1)
    if args.self_test:
        ok, lines = self_test(args.game)
        print("thing_fields self-test")
        for ln in lines:
            print(ln)
        print("RESULT: %s" % ("PASS" if ok else "FAIL"))
        rc = rc or (0 if ok else 1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
