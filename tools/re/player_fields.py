#!/usr/bin/env python3
"""Field map of the Gunmetal player runtime structure (``M2.3.1``).

The game keeps up to 8 players in a static array::

    0x819040 + i * 0x3510        i = 0..7      end = 0x8338c0

Every field below is addressed relative to that base.  ``0x81bd00`` (often
quoted as "the player base") is *not* the array: it is ``&player[0] + 0x2cc0``,
an address inside the yaw trail of slot 0 (``+0x25b0..+0x2d30``) that the
compiler happened to use as a displacement anchor for the chassis fields.

The table is grouped by subsystem and every row carries a status:

``proven``      instruction + data agree (the evidence column names the
                instruction that reads/writes the field)
``hypothesis``  one side only (code shape or save value), meaning still a guess
``unknown``     offset and type are known (referenced by code / non-zero in a
                save), meaning not established

Companion document: ``docs/PLAYER_FIELDS.md``.

Usage::

    python3 tools/re/player_fields.py                # field map
    python3 tools/re/player_fields.py --map          # same, grouped
    python3 tools/re/player_fields.py --census       # all 0x3510-strided refs
    python3 tools/re/player_fields.py --verify-saves # check installed saves
    python3 tools/re/player_fields.py --self-test    # synthetic + real saves

Only the standard library is used; the executable and the saves are opened
read-only (``--census`` needs the objdump listing, see ``tools/re/README.md``).

Exit status: 0 = ok, 1 = IO/parse error, 2 = verification failure.
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import re
import struct
import sys

BASE = 0x819040
STEP = 0x3510
SLOTS = 8
END = BASE + SLOTS * STEP              # 0x8338c0
# "anchor" used by the decompiler for the chassis block of player 0
CHASSIS_ANCHOR = 0x81BD00
# spawn template / trailing blocks written by 0x452020 after the 8 slots
TEMPLATE_BASE = 0x815440
TAIL_AFTER_SLOTS = 0x8338C0
# bytes between the end of a slot struct and the end of the file
TAIL_REST = 7 + 0x50 + 0x3510 + 0x50 + 0x3510 + 1   # 27336
SAVE_MARKER = b"\x09\x01"
GAME_DEFAULT = "/mnt/d/games/Gunmetal 3dfx"
ASM_DEFAULT = "/tmp/opencode/gundx.asm"

# ---------------------------------------------------------------------------
# field map
# ---------------------------------------------------------------------------
# (offset, size, type, name, subsystem, status, evidence)
FIELDS = [
    # --- pose ---------------------------------------------------------------
    (0x0000, 8, "double", "pos_x", "pose", "proven",
     "0x452b.. writes from spawn table; 0x443f1b distance test"),
    (0x0008, 8, "double", "pos_y", "pose", "proven", "0x452bb4; 0x4445xx"),
    (0x0010, 8, "double", "pos_z", "pose", "proven",
     "0x452bc1; camera 0x448d31 `fadd [0x819050]` (docs/CAMERA.md)"),
    (0x0018, 8, "double", "pitch", "pose", "proven",
     "spring 0x81bde8->0x81be08 (0x4449d5..0x444a55), turret pitch added "
     "at 0x44fc30, rotation matrix 0x41e025"),
    (0x0020, 8, "double", "roll", "pose", "proven",
     "spring 0x81bdf0->0x81be10 (0x444af8..0x444ba5), clamp +-0x81be20 (20 deg)"),
    (0x0028, 8, "double", "yaw", "pose", "proven",
     "0x444118 `fld`, `fcos`/`fsin` at 0x444120/0x44412b, `fmul [0x81bd78]` "
     "at 0x44412f/0x444137 -- the movement direction"),

    # --- position / orientation history -------------------------------------
    (0x0030, 240 * 24, "double[240][3]", "trail_pos", "history", "proven",
     "ring write 0x443784..0x4437b5 (6 dwords = x,y,z), index +0x3204, wrap 0xf0"),
    (0x16b0, 240 * 8, "double[240]", "trail_pitch", "history", "proven",
     "0x44379b/0x4437a8 copy of +0x18"),
    (0x1e30, 240 * 8, "double[240]", "trail_roll", "history", "proven",
     "0x4437bd..0x4437ca copy of +0x20"),
    (0x25b0, 240 * 8, "double[240]", "trail_yaw", "history", "proven",
     "0x4437df..0x4437ec copy of +0x28"),

    # --- chassis / suspension (instantiated by 0x47a980) --------------------
    (0x2d30, 8, "double", "speed_base", "chassis", "proven",
     "0x47aa58 <- chassis+0x20; 0x4525d5"),
    (0x2d38, 8, "double", "speed", "chassis", "proven",
     "0x4440b3 `fstp`; scaled by +0x2f48 (0x4440ad)"),
    (0x2d40, 8, "double", "speed_penalty_per_mass", "chassis", "proven",
     "0x4525fb/0x47aca7 <- chassis+0x18; excess mass -> speed loss (0x4524d0)"),
    (0x2d48, 8, "double", "speed_max", "chassis", "proven",
     "0x4440a7 `fld`; 0x4525db <- chassis+0x28; база +0x2d30"),
    (0x2d50, 8, "double", "vel_x", "dynamics", "proven",
     "integrator 0x444478..0x444489; передаётся как смещение по x в 0x450f50 "
     "(docs/PLAYER_CPP.md)"),
    (0x2d58, 8, "double", "vel_y", "dynamics", "proven",
     "integrator 0x444495..0x4444a7; смещение по y"),
    (0x2d60, 8, "double", "velocity_penetration", "dynamics", "proven",
     "0x4446f3..; consumed by collision 0x450f50"),
    (0x2d68, 8, "double", "collision_radius", "dynamics", "proven",
     "0x443e77; used by 0x443f1b/0x444ff4 object distance tests"),
    (0x2d70, 24, "float[6]", "bbox_minmax", "chassis", "proven",
     "float x6 = (0,3.0, 0,5.25, 0,2.75) in saved0; 6 args of 0x43c51f "
     "in 0x44fc30"),
    (0x2d88, 8, "double", "yaw_accel", "chassis", "proven",
     "0x4442a4-контекст: `v = v - input*[0x81bdc8]` (0x44392e/0x444225); "
     "<- chassis+0x00 (0x47abd5)"),
    (0x2d90, 8, "double", "yaw_accel_alt", "chassis", "proven",
     "второй источник углового ускорения, делится на массу (0x4442a4); "
     "<- chassis+0x08 (0x47abe9)"),
    (0x2d98, 8, "double", "yaw_rate_max", "chassis", "proven",
     "кламп +-`[0x81bdd8]` для +0x2da0 (0x443940..0x44397c); "
     "<- chassis+0x10 (0x47abfd)"),
    (0x2da0, 8, "double", "yaw_rate", "dynamics", "proven",
     "0x4439f1 `yaw += ticks * [0x81bde0]` (0x819068); затухание ×0.125 "
     "(0x4438ea) и кламп +-0x2d98"),
    (0x2da8, 8, "double", "pitch_current", "dynamics", "proven",
     "0x444992 `fld`; spring to +0x2dc8"),
    (0x2db0, 8, "double", "roll_current", "dynamics", "proven",
     "0x444a22 `fld`; spring to +0x2dd0"),
    (0x2db8, 8, "double", "pitch_step", "dynamics", "proven",
     "0x4449e3 writes low/high dword"),
    (0x2dc0, 8, "double", "roll_step", "dynamics", "proven", "0x444a5b"),
    (0x2dc8, 8, "double", "pitch_target", "dynamics", "proven",
     "0x444998 `fsub`; written by 0x451160"),
    (0x2dd0, 8, "double", "roll_target", "dynamics", "proven",
     "0x444a28 `fsub`; written by 0x451160"),
    (0x2dd8, 8, "double", "pitch_limit", "dynamics", "proven",
     "= pi/6 (30 deg), set at 0x4528xx; clamp 0x444b0a"),
    (0x2de0, 8, "double", "roll_limit", "dynamics", "proven",
     "= 0.34906585 (20 deg); clamp 0x444ab0"),
    (0x2de8, 8, "double", "vel_z", "dynamics", "proven",
     "integrator 0x4444ad..0x4444cd (`vz = 0.85*vz + 0.15*pitch`), 0x444b67"),
    (0x2df0, 8, "double", "flight_z", "dynamics", "unknown", "0x4513c4"),
    (0x2df8, 8, "double", "flight_z2", "dynamics", "unknown", "0x45161e"),
    (0x2e10, 8, "double", "aim_y", "turret", "unknown", "0x444648 `fld`"),
    (0x2e20, 8, "double", "aim_y2", "turret", "unknown", "0x44466d `fld`"),
    (0x2e30, 8, "double", "aim_z", "turret", "unknown", "0x444652"),
    (0x2e40, 8, "double", "aim_z2", "turret", "unknown", "0x4518f9 `fmul`"),
    (0x2e50, 8, "double", "gimbal_0", "turret", "unknown",
     "0x444648; cleared on reset 0x4528xx"),
    (0x2e58, 8, "double", "route_height", "dynamics", "hypothesis",
     "0x4446a8..0x4446e4: max из двух высот FUN_00451160, 1e38 = «нет данных»"),
    (0x2e60, 8, "double", "turret_pitch", "turret", "proven",
     "limits +-pi/2 (+0x2e68/+0x2e70); added to pose pitch at 0x44fc30; "
     "docs/CAMERA.md"),
    (0x2e68, 8, "double", "turret_pitch_min", "turret", "proven",
     "-pi/2 reset at 0x4528xx; clamp 0x443a78"),
    (0x2e70, 8, "double", "turret_pitch_max", "turret", "proven",
     "+pi/2; clamp 0x443aa3"),
    (0x2e78, 64, "double[8]", "hardpoint_x", "turret", "proven",
     "8 doubles copied from the chassis record (0x47ac..); transformed with the "
     "body matrix in the firing path (0x4458fb/0x445966)"),
    (0x2eb8, 64, "double[8]", "hardpoint_y", "turret", "proven", "0x44590b/0x445930"),
    (0x2ef8, 64, "double[8]", "hardpoint_z", "turret", "proven", "0x4458fb/0x445925"),
    (0x2f38, 8, "double", "turret_pitch_delta", "turret", "proven",
     "0x443a03 `fld`; прибавляется к +0x2e60 и обнуляется (0x4439c2..), "
     "сброс 0x4528c0"),
    (0x2f40, 8, "double", "yaw_delta", "dynamics", "proven",
     "0x4439c2: `yaw += [0x81bf80]` и обнуление; 0x4443ed/0x44452c"),
    (0x2f48, 8, "double", "engine_power", "dynamics", "proven",
     "0x4440ad `fmul`; clamped to 0..1 (0x44407a/0x4440e6)"),
    (0x2f50, 8, "double", "eye_height", "camera", "proven",
     "0x444dbb; camera 0x448dd1; 10.0 in saved0 (docs/PLAYER.md, CAMERA.md)"),
    (0x2f60, 8, "double", "damage_threshold", "damage", "hypothesis",
     "0x44481c `fcom`; <- chassis+0x48"),
    (0x2f68, 8, "double", "damage_factor", "damage", "hypothesis",
     "0x444839 `fld`; <- chassis+0x50"),
    (0x2f70, 8, "double", "damage_factor2", "damage", "hypothesis",
     "0x444833; <- chassis+0x58"),
    (0x2f78, 8, "double", "velocity_retention_k", "dynamics", "proven",
     "0x44447e/0x44448f in the integrator; <- chassis+0x60 "
     "(docs/PLAYER_CPP.md)"),
    (0x2f80, 32, "double[4]", "physics_params", "chassis", "unknown",
     "<- chassis+0x64..0x7c; no consumer found"),
    (0x2fa0, 8, "double", "charge", "weapon", "proven",
     "0x445afc latches accumulated, 0x46f0f0 reads it as the FIRE operand[9] "
     "multiplier (speed), reset 0x45278a (docs/WEAPONS_CPP.md 3.5)"),
    (0x2fa8, 8, "double", "charge_accumulated", "weapon", "proven",
     "0x445b21..0x445cf1 `fadd dt*weapon[+0x00]`, capped at 1.0"),
    (0x2fb0, 8, "double", "aim_offset_mount", "weapon", "unknown", "0x452d7e"),
    (0x2fc0, 8, "double", "climb_limit", "dynamics", "proven",
     "0x444bb3 `fcomp`; 0x452cf9 <- chassis"),
    (0x2fc8, 8, "double", "climb_factor", "dynamics", "proven", "0x444bcc `fmul`"),
    (0x2fd0, 8, "double", "collision_stiffness", "dynamics", "proven",
     "0x45103b..0x4510e1; 0x452d19"),
    (0x2fd8, 8, "double", "mass_scale", "chassis", "proven",
     "= 1.0 on reset (0x4527e0); 0x4519e8"),
    (0x2fe0, 8, "double", "mass", "dynamics", "proven",
     "`fdiv` divisor of the acceleration term (0x443fed, 0x4442b0, 0x44d73c)"),
    (0x2fe8, 8, "double", "inertia_x", "dynamics", "unknown", "0x452ce1"),
    (0x2ff0, 8, "double", "inertia_y", "dynamics", "unknown", "0x452ced"),

    # --- attached parts ------------------------------------------------------
    (0x2ff8, 128, "double[16]", "attach_local_x", "attach", "proven",
     "0x45683b writes slot index; world transform 0x456874 "
     "(FUN_00456710)"),
    (0x3078, 128, "double[16]", "attach_local_y", "attach", "proven", "0x456845"),
    (0x30f8, 128, "double[16]", "attach_local_z", "attach", "proven", "0x45684f"),
    (0x3178, 4, "u32", "body_object_id", "attach", "proven",
     "0x453b50 guard `== 0 return`; 0x4277cd resolves the object; 0x454a20"),
    (0x317c, 64, "u32[16]", "attach_object_id", "attach", "proven",
     "0x454a5b loop; serialised as 16 entries (0x4520b1)"),
    (0x31bc, 64, "u32[16]", "attach_flags", "attach", "proven",
     "0x456855 (param_11: bit0/1/2, bit2 = quarter size)"),

    # --- state, flags, timers ------------------------------------------------
    (0x31fc, 4, "u32", "state_word", "state", "unknown",
     "zeroed for an empty slot (0x451c69); 0x3C9F2F2D in saved0; "
     "copied into the 0x61ec70 info record"),
    (0x3200, 4, "u32", "flags", "state", "proven", "see the bit table"),
    (0x3204, 4, "i32", "trail_index", "history", "proven",
     "0x44376a; incremented and wrapped at 0xf0"),
    (0x3208, 4, "i32", "trail_tick", "history", "proven",
     "0x44372c; a trail point is recorded every 22 ticks"),
    (0x320c, 4, "i32", "opacity", "state", "hypothesis",
     "low byte copied to object+0x74 (0x4538d0); written as (a+b)>>1 at "
     "0x4528xx; -12 = \"no override\" in saved0"),
    (0x3210, 4, "i32", "action_state", "state", "proven",
     "0 = слот свободен/уничтожен (0x45347f, 0x452020), 1 = жив (0x453303), "
     "7 = цикл выстрела (0x453763 + case 7 в 0x444476); 2..6 ставятся "
     "скриптом/магазином (0x466bdf, 0x47b377), смысл каждого не установлен"),
    (0x3214, 4, "i32", "chassis_type_id", "chassis", "proven",
     "0x451c69 -> FUN_0047c990 (type lookup); 16 in saved0"),
    (0x3218, 4, "i32", "timer_flag2", "state", "proven",
     "0x4440f0..0x444104 countdown, clears flag bit 0x2"),
    (0x321c, 4, "i32", "timer_flag1", "state", "proven",
     "0x444721.. countdown, clears flag bit 0x1"),
    (0x3220, 4, "i32", "timer_flag10", "state", "proven",
     "0x4448e0.. countdown, clears flag bit 0x10"),
    (0x3224, 4, "i32", "timer_flag20", "state", "proven",
     "0x453b50 adds 0x5b*pct/100 and sets bit 0x20; 0x4449xx countdown"),
    (0x3228, 4, "i32", "model_variant", "render", "unknown",
     "= -1 in saved0; `mov [-1]` at 0x4459c5"),
    (0x322c, 4, "i32", "damage_flash_a", "render", "unknown", "0x44702e"),
    (0x3230, 4, "i32", "damage_flash_b", "render", "unknown", "0x447039"),
    (0x3234, 4, "u32", "render_param_a", "render", "hypothesis",
     "0x453b29; 96 in saved0"),
    (0x3238, 4, "u32", "render_param_b", "render", "hypothesis",
     "= 0x40 on reset (0x4527e0); passed to 0x428421 (0x4538d0)"),
    (0x323c, 4, "i32", "model_id", "render", "proven",
     "first event_post argument (0x44462e, 0x453b50); 5 in saved0"),
    (0x3240, 4, "i32", "model_id2", "render", "hypothesis",
     "read as WORD at 0x465817; 5 in saved0"),

    # --- vitals (hundredths of a point) --------------------------------------
    (0x3244, 4, "i32", "armor_max", "vitals", "proven",
     "0x444084 `fild`; = chassis armor * 100 (0x47a9xx)"),
    (0x3248, 4, "i32", "armor", "vitals", "proven",
     "0x44408a `fild`; decremented by 0x453b50"),
    (0x324c, 4, "i32", "shield_max", "vitals", "proven",
     "0x443c8f/0x4538d0 ratio; = chassis shield * 100"),
    (0x3250, 16, "i32[4]", "shields", "vitals", "proven",
     "0x443c7d DoT loop; directional choice in 0x453b50; order front, right, "
     "back, left (SYSTEM_PLAYER_*_SHIELD_GONE strings)"),
    (0x3260, 16, "i32[4]", "shields_spawn", "vitals", "proven",
     "snapshot of +0x3250 taken when the chassis is instantiated (0x47aac0)"),
    (0x3270, 8, "i32[2]", "shield_misc", "vitals", "unknown", "0 in saved0"),

    # --- weapons -------------------------------------------------------------
    (0x3294, 4, "i32", "weapon_slot", "weapon", "proven",
     "0x44531c, 0x4456xx (docs/FORMATS.md 16.15)"),
    (0x3298, 4, "u32", "weapon_mask", "weapon", "proven",
     "0x44532d; test `1 << slot`"),
    (0x329c, 48, "u32[12]", "ammo", "weapon", "proven",
     "0x4455ad `[slot*4+0x81c2dc]`; +0x32c4 is ammo[10], not a resource"),
    (0x3318, 4, "i32", "fire_cooldown", "weapon", "proven",
     "0x4453f8; set to 0x5b/0xb6 on reset"),
    (0x331c, 4, "i32", "fire_timer", "weapon", "hypothesis", "0x44535e"),
    (0x3320, 4, "i32", "fire_anim", "weapon", "hypothesis",
     "0x445fd6..0x44611x (recoil animation)"),
    (0x3324, 4, "u32", "equipment_mask", "equipment", "proven",
     "bits over the equipment table 0x814c8c (0x47ac..); 17 in saved0"),
    (0x3328, 4, "u32", "equipment_active_mask", "equipment", "proven",
     "0x47aad1 clears a bit when the item is removed"),
    (0x332c, 4, "i32", "equipment_cost_total", "equipment", "proven",
     "accumulated at 0x47b0xx; 2000 in saved0"),
    (0x3330, 4, "i32", "equipment_slot", "equipment", "unknown", "0x4422c7"),
    (0x3334, 4, "i32", "mass_total", "equipment", "proven",
     "0x4524d0 sums shields/100*4 + weapons + equipment + ammo*weight"),
    (0x3338, 4, "i32", "mass_limit", "equipment", "proven",
     "0x4524d0; <- chassis+0x148; 500 in saved0"),

    # --- input -----------------------------------------------------------------
    (0x3340, 4, "u32", "input_flags", "input", "hypothesis",
     "byte +0x3340 bit 0x20 tested at 0x445a4a/0x44d6a6"),
    (0x3344, 4, "u32", "flags2", "state", "proven", "see the bit table"),
    (0x3348, 4, "i32", "input_a", "input", "proven",
     "0x444393: разность с +0x334c идёт в угловое ускорение (ось — гипотеза)"),
    (0x334c, 4, "i32", "input_a_neg", "input", "proven", "0x444399"),
    (0x3350, 4, "i32", "throttle_input", "input", "proven", "0x4438b7/0x44418c"),
    (0x3354, 4, "i32", "brake_input", "input", "proven", "0x4438bd/0x444192"),
    (0x3358, 4, "i32", "airborne_lock", "input", "proven",
     "0x443a5e: non-zero -> +0x2da0 = 0"),
    (0x3360, 4, "i32", "turn_left_input", "input", "proven", "0x443a36/0x443fcf"),
    (0x3364, 4, "i32", "turn_right_input", "input", "proven", "0x443a3c/0x443fd5"),
    (0x3368, 4, "i32", "handbrake", "input", "proven",
     "0x444180/0x444326: zero -> no throttle integration"),
    (0x336c, 4, "i32", "input_b", "input", "proven",
     "0x44431a: разность с +0x3370 -> смещение (0x44431a..0x444476)"),
    (0x3370, 4, "i32", "input_b_neg", "input", "proven", "0x444320"),
    (0x3374, 4, "i32", "boost_input", "input", "proven",
     "0x4446ff/0x444775 -> engine power"),
    (0x3378, 4, "i32", "boost_input_alt", "input", "proven", "0x444705/0x44477b"),
    (0x33bc, 4, "i32", "input_c", "input", "proven",
     "0x444245: разность с +0x33c0 -> боковой импульс через массу"),
    (0x33c0, 4, "i32", "input_c_neg", "input", "proven", "0x44424b"),
    (0x33c4, 4, "i32", "pending_weapon_id", "weapon", "proven",
     "0x445747..0x4457f6; -1 = none (0x4527e0); -1 in saved0"),
    (0x33c8, 4, "i32", "fire_cooldown_2", "weapon", "proven",
     "0x443716: `-= ticks*0x4000`, floor 0; 0xc0000 on reset"),
    (0x33cc, 4, "i32", "damage_accum", "damage", "unknown", "0x45280f"),
    (0x33d0, 4, "i32", "damage_accum_2", "damage", "unknown", "0x452815"),
    (0x33d4, 4, "i32", "fire_cycle_timer", "weapon", "proven",
     "0x443849 `+= ticks`; `>= 0x88` allows the next shot "
     "(0x44485f)"),
    (0x33d8, 4, "u32", "event_enter_id", "state", "proven", "0x444fdb"),
    (0x33dc, 4, "u32", "event_leave_id", "state", "proven", "0x444f7a"),

    # --- chassis record copies, string buffers, ids --------------------------
    (0x33e0, 4, "u32", "chassis_icon_id", "chassis", "proven",
     "<- chassis+0x17c; 28 in saved0"),
    (0x33e4, 20, "char[20]", "chassis_name", "chassis", "proven",
     "string copy <- chassis+0x16c (0x47ac38); \"thuml\" in saved0"),
    (0x33f8, 4, "u32", "chassis_field_190", "chassis", "proven",
     "<- chassis+0x190; 8 in saved0"),
    (0x33fc, 16, "char[16]", "chassis_field_1a0", "chassis", "proven",
     "string copy <- chassis+0x1a0; \" \" in saved0"),
    (0x340c, 4, "u32", "chassis_field_194", "chassis", "proven",
     "<- chassis+0x194; -30062 in saved0"),
    (0x3410, 4, "u32", "chassis_field_198", "chassis", "proven",
     "<- chassis+0x198; 901010 in saved0"),
    (0x3414, 16, "char[16]", "part_name_1", "chassis", "proven",
     "string copy <- chassis+0x1ac; \"tick\" in saved0"),
    (0x3424, 4, "u32", "part_1_param", "chassis", "proven",
     "<- chassis+0x1b8; 100 in saved0"),
    (0x3428, 16, "char[16]", "part_name_2", "chassis", "proven",
     "string copy <- chassis+0x1c4?; \"tickx\" in saved0"),
    (0x3438, 4, "u32", "part_2_param", "chassis", "proven",
     "<- chassis+0x1c0; 162 in saved0"),
    (0x343c, 16, "char[16]", "part_name_3", "chassis", "hypothesis",
     "\"coargus\" in saved0; not set by 0x47a980"),
    (0x344c, 4, "u32", "object_handle", "script", "hypothesis",
     "0x52878e-style handle stored at 0x47bxx; 382 in saved0"),
    (0x3450, 16, "char[16]", "object_script_name", "script", "hypothesis",
     "\"K-9\" in saved0"),
    (0x3460, 4, "i32", "chassis_type_requested", "chassis", "hypothesis",
     "compared with the type count DAT_007941bc (0x464521/0x4671e6); 1 in saved0"),
    (0x3464, 4, "i32", "chassis_type_current", "chassis", "proven",
     "0x47a980 stores the new type; -1 when invalid"),
    (0x3468, 4, "i32", "chassis_price", "chassis", "proven",
     "<- chassis+0x168 (0x47aa..); 10000 in saved0"),
    (0x346c, 32, "i32[8]", "unknown_ids", "script", "unknown",
     "8 x -1 on reset (0x4527e0)"),
]

GROUP_TITLES = [
    ("pose", "Поза: позиция и углы"),
    ("history", "Трейлы и история (240 точек)"),
    ("chassis", "Шасси: параметры, копируемые из записи типа"),
    ("dynamics", "Динамика: скорости, пружины, столкновения"),
    ("turret", "Башня, точки выстрела, прицеливание"),
    ("camera", "Камера"),
    ("attach", "Навесные части (до 16)"),
    ("state", "Состояние, флаги, таймеры"),
    ("damage", "Урон"),
    ("vitals", "Виталы (сотые доли)"),
    ("weapon", "Оружие"),
    ("equipment", "Оборудование и масса"),
    ("input", "Ввод (счётчики действий)"),
    ("render", "Рендер"),
    ("script", "Скриптовые идентификаторы"),
]

# flags (0x3200)
FLAGS = [
    (0x00000001, "движение заблокировано",
     "0x4446ea/0x444b52: ветка обнуляет +0x2da0/+0x2db8/+0x2dc0 (0x44470d..)"),
    (0x00000002, "форсаж",
     "0x44406b/0x4440d7: engine_power (+0x2f48) = 1.0, таймер +0x3218"),
    (0x00000004, "флаг рендера/HUD",
     "единственный `test` в 0x447d7c"),
    (0x00000008, "бездорожье / в воздухе",
     "0x444122: движение x0.75; ветки 0x444807, 0x444f4a"),
    (0x00000010, "неуязвимость",
     "0x453b94: 0x453b50 выходит сразу; таймер +0x3220"),
    (0x00000020, "вспышка попадания в щит",
     "0x443c0b; ставится в 0x453b50 вместе с +0x3224"),
    (0x00000040, "обработка столкновений",
     "0x49321f/0x494a52; в физике ставится вместе с 0x80"),
    (0x00000080, "«таран» ( ram )",
     "0x4443c2 ставит 0xc0, 0x444? снимает 0x40 (0x443be8)"),
    (0x00000200, "флаг маршрута",
     "тест в 0x44d6xx"),
    (0x00000400, "проверять патроны при смене",
     "0x4455xx (Ghidra: `& 0x400` в условии выбора слота)"),
    (0x00002000, "кламп крена/тангажа активен",
     "ставится 0x444b8a, снимается 0x444b2x"),
    (0x00004000, "флаг перезарядки",
     "0x4547f0/0x4548a0"),
    (0x00010000, "автоперезарядка",
     "0x454917: 0x454900 перезаряжает выбранное оружие"),
    (0x00020000, "флаг стрельбы/эффектов",
     "0x446e8c, 0x483fb3"),
    (0x00100000, "отладочное движение",
     "0x44455e; работает только при DAT_0083d184 & 0x40"),
]

# flags2 (0x3344): только 0x2000/0x4000/0x8000 доказаны (физика/оружие),
# младшие биты встречаются лишь в меню-функциях 0x47b6xx..0x47c3xx.
FLAGS2 = [
    (0x00000001, "флаг 1 (меню)", "0x47b739, 0x47c1fb"),
    (0x00000002, "флаг 2 (меню)", "0x47b676, 0x47b752, 0x47bd67"),
    (0x00000004, "флаг 4 (меню)", "0x47b7a5, 0x47c26d"),
    (0x00000008, "флаг 8 (меню)", "0x47b88f, 0x47c336"),
    (0x00000020, "флаг 0x20", "0x4459e7 (абс. адрес слота 0), 0x445d74"),
    (0x00002000, "следующее оружие", "0x4455xx: переключение вперёд"),
    (0x00004000, "предыдущее оружие", "0x4456xx"),
    (0x00008000, "синхронизация вида/башни",
     "0x443bb1 `test ah,0x80` -> вызов 0x44fc30 и сброс (0x443bbe)"),
]

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def field_at(off: int):
    for f in FIELDS:
        if f[0] == off:
            return f
    return None


def fmt_size(size: int, typ: str) -> str:
    m = re.match(r"^(?:double|i32|u32|char)\[(\d+)\](?:\[(\d+)\])?$", typ)
    if m and typ.startswith("double"):
        n = int(m.group(1)) * (int(m.group(2)) if m.group(2) else 1)
        return "%d x %d" % (int(m.group(1)), size // int(m.group(1)))
    return ("0x%x" % size) if size >= 16 else str(size)


def cmd_map(args) -> int:
    print("player array: 0x%06x + i * 0x%x, i = 0..%d (end 0x%06x)"
          % (BASE, STEP, SLOTS - 1, END))
    print("0x%06x = &player[0] + 0x%x (inside the yaw trail, not the array base)"
          % (CHASSIS_ANCHOR, CHASSIS_ANCHOR - BASE))
    print()
    for key, title in GROUP_TITLES:
        rows = [f for f in FIELDS if f[4] == key]
        if not rows:
            continue
        print("## %s" % title)
        print("  %-8s %-6s %-24s %-13s %s" %
              ("offset", "size", "type", "name", "status"))
        for off, size, typ, name, _sub, status, _ev in rows:
            print("  +0x%04x %-6s %-24s %-13s %s"
                  % (off, fmt_size(size, typ), typ, name, status))
        print()
    print("## Биты +0x3200 (flags)")
    for bit, name, ev in FLAGS:
        print("  0x%06x  %-26s %s" % (bit, name, ev))
    print("## Биты +0x3344 (flags2)")
    for bit, name, ev in FLAGS2:
        print("  0x%06x  %-26s %s" % (bit, name, ev))
    return 0


CENSUS_RE = re.compile(r"^\s*([0-9a-f]{6,8}):\t(?:[0-9a-f]{2} )+\s*(.*)$")


def census(path: str):
    """All memory operands pointing into the player array, normalised."""
    hits = {}
    for line in open(path, errors="replace"):
        m = CENSUS_RE.match(line.rstrip("\n"))
        if not m:
            continue
        addr = int(m.group(1), 16)
        text = m.group(2)
        if "0x81" not in text and "0x82" not in text and "0x83" not in text:
            continue
        for op in re.findall(r"\[[^\]]*\]", text):
            for dm in re.finditer(r"0x([0-9a-f]{6,8})", op):
                d = int(dm.group(1), 16)
                if BASE <= d < END:
                    off = (d - BASE) % STEP
                    hits.setdefault(off, []).append((addr, text))
    return hits


def cmd_census(args) -> int:
    path = args.asm or os.environ.get("GUNDX_ASM") or ASM_DEFAULT
    if not os.path.exists(path):
        print("нет листинга %s\n"
              "сгенерируйте: objdump -D -M intel \"%s/gundx.exe\" > %s"
              % (path, GAME_DEFAULT, path), file=sys.stderr)
        return 1
    hits = census(path)
    for off in sorted(hits):
        f = field_at(off)
        rows = hits[off]
        print("+0x%04x  n=%3d  %-22s  %s"
              % (off, len(rows),
                 (f[3] if f else ""),
                 ",".join("%06x" % a for a, _ in rows[:4])))
    named = sum(1 for o in hits if field_at(o))
    print("\nсмещений: %d, обращений: %d, из них в таблице: %d, без имени: %d"
          % (len(hits), sum(len(v) for v in hits.values()), named,
             len(hits) - named))
    return 0


def game_dir() -> str:
    exe = os.environ.get("GUNMETAL_EXE")
    if exe:
        return os.path.dirname(exe)
    return GAME_DEFAULT


def find_saves(paths) -> list:
    if paths:
        return list(paths)
    pat = os.path.join(game_dir(), "SAVEGAME", "saved*.sbt")
    return sorted(glob.glob(pat))


def find_player(tail: bytes) -> int:
    """Offset of the marker ``09 01`` whose struct is followed by TAIL_REST."""
    last = -1
    start = 0
    while True:
        i = tail.find(SAVE_MARKER, start)
        if i < 0:
            break
        start = i + 1
        if i + 2 + STEP > len(tail):
            break
        if len(tail) - (i + 2 + STEP) != TAIL_REST:
            continue
        st = tail[i + 2:i + 2 + STEP]
        if check_struct(st)[0]:
            continue
        last = i
    return last


def check_struct(st: bytes, strict: bool = True):
    """Return (errors, warnings, info) for one 0x3510 block."""
    err, warn = [], []
    if len(st) != STEP:
        return ["struct: %d bytes, expected 0x%x" % (len(st), STEP)], [], {}
    i = lambda o: struct.unpack_from("<i", st, o)[0]
    u = lambda o: struct.unpack_from("<I", st, o)[0]
    d = lambda o: struct.unpack_from("<d", st, o)[0]
    info = {}

    # pose
    pos = (d(0x00), d(0x08), d(0x10))
    info["pos"] = pos
    if not all(math.isfinite(v) for v in pos):
        err.append("pos: not finite %r" % (pos,))
    elif abs(pos[0]) > 1e6 or abs(pos[1]) > 1e6 or abs(pos[2]) > 1e5:
        err.append("pos: out of range %r" % (pos,))
    pitch, roll, yaw = d(0x18), d(0x20), d(0x28)
    info["angles"] = (pitch, roll, yaw)
    if not all(math.isfinite(v) for v in (pitch, roll, yaw)):
        err.append("angles: not finite")
    else:
        if abs(pitch) > 0.5236 + 1e-3:
            err.append("pitch %.4f out of +-30 deg (0x81be18)" % pitch)
        if abs(roll) > 0.3491 + 1e-3:
            err.append("roll %.4f out of +-20 deg (0x81be20)" % roll)
        if abs(yaw) > 2 * math.pi + 1e-3:
            warn.append("yaw %.4f is not wrapped to +-2pi" % yaw)

    # trail ring
    idx, tick = i(0x3204), i(0x3208)
    info["trail"] = (idx, tick)
    if not 0 <= idx < 240:
        err.append("trail_index %d outside 0..239 (wrap at 0xf0)" % idx)
    if not 0 <= tick <= 22:
        err.append("trail_tick %d outside 0..22" % tick)

    # chassis / state
    state = i(0x3210)
    info["state"] = state
    if not 0 <= state <= 7:
        err.append("action_state %d outside 0..7" % state)
    ctype = i(0x3214)
    if not -1 <= ctype < 4096:
        err.append("chassis_type_id %d out of range" % ctype)

    # vitals
    amax, acur = i(0x3244), i(0x3248)
    smax = i(0x324C)
    shields = struct.unpack_from("<4i", st, 0x3250)
    info["vitals"] = (amax, acur, smax, shields)
    if not 0 < amax <= 1000000:
        err.append("armor_max %d outside (0, 1e6]" % amax)
    if not 0 <= acur <= amax:
        err.append("armor %d outside 0..%d" % (acur, amax))
    if not 0 <= smax <= 1000000:
        err.append("shield_max %d outside [0, 1e6]" % smax)
    elif not all(0 <= v <= smax for v in shields):
        err.append("shields %r outside 0..%d" % (shields, smax))

    # weapon
    slot = i(0x3294)
    info["weapon_slot"] = slot
    if not -1 <= slot < 32:
        err.append("weapon_slot %d out of range" % slot)
    ammo = struct.unpack_from("<12I", st, 0x329C)
    info["ammo"] = ammo
    if any(v > 0x7FFFFFFF for v in ammo):
        err.append("ammo: value above 2^31-1 %r" % (ammo,))

    # mass
    mass, mlimit = d(0x2FE0), i(0x3338)
    info["mass"] = (mass, mlimit)
    if not math.isfinite(mass) or mass <= 0.0 or mass > 1e4:
        err.append("mass %r outside (0, 1e4]" % (mass,))
    if not 0 < mlimit <= 1e7:
        err.append("mass_limit %d outside (0, 1e7]" % mlimit)

    # attached parts
    ids = struct.unpack_from("<16I", st, 0x317C)
    aflags = struct.unpack_from("<16I", st, 0x31BC)
    info["attach"] = sum(1 for v in ids if v)
    for k, (oid, fl) in enumerate(zip(ids, aflags)):
        if oid == 0 and fl != 0:
            err.append("attach[%d]: flags 0x%x without an object id" % (k, fl))
        if fl & ~0x7:
            warn.append("attach[%d]: flags 0x%x has unknown bits" % (k, fl))

    # flags
    flags, flags2 = u(0x3200), u(0x3344)
    info["flags"] = (flags, flags2)
    known = 0
    for bit, _n, _e in FLAGS:
        known |= bit
    if flags & ~known:
        warn.append("flags 0x%x: unknown bits 0x%x"
                    % (flags, flags & ~known))
    known2 = 0
    for bit, _n, _e in FLAGS2:
        known2 |= bit
    if flags2 & ~known2:
        warn.append("flags2 0x%x: unknown bits 0x%x"
                    % (flags2, flags2 & ~known2))

    # constants that the reset code (0x4527e0) proves
    for off, want, name in ((0x2DD8, 0.5235987755982988, "pitch_limit 30 deg"),
                            (0x2DE0, 0.3490658503988659, "roll_limit 20 deg")):
        v = d(off)
        if abs(v - want) > 1e-6:
            warn.append("%s: %.9f, reset writes %.9f" % (name, v, want))
    return err, warn, info


def verify_tail(tail: bytes, name: str, verbose: bool = True) -> int:
    """Check one save's tail. Returns the number of errors."""
    errors = 0
    off = find_player(tail)
    if off < 0:
        print("%s: блок игрока не найден (нет 09 01, за которым ровно %d байт)"
              % (name, TAIL_REST), file=sys.stderr)
        return 1
    st = tail[off + 2:off + 2 + STEP]
    err, warn, info = check_struct(st)
    end = off + 2 + STEP

    # 8 slots: the marker is followed by slot 0's present flag, the other
    # seven flags sit right after the struct (0x452020 writes 8 blocks).
    if tail[off + 1] != 1:
        err.append("slot0 presence = %d" % tail[off + 1])
    rest = tail[end:end + 7]
    if rest != b"\x00" * 7:
        err.append("slots 1..7 presence = %s (expected seven zero bytes)"
                   % rest.hex())
    # trailing blocks: 0x50 (level name), 0x3510 (spawn template), 0x50,
    # 0x3510 (unused), one 0xff terminator
    tail_len = len(tail) - end
    if tail_len != TAIL_REST:
        err.append("tail after struct = %d, expected %d" % (tail_len, TAIL_REST))
    else:
        tpl = tail[end + 7 + 0x50:end + 7 + 0x50 + STEP]
        terr = check_struct(tpl)[0]
        # the template keeps armor/shields but its position is the spawn point
        if terr:
            err.extend("template: " + e for e in terr)
        if tail[-1] != 0xFF:
            err.append("terminator 0x%02x, expected 0xff" % tail[-1])
        info["template_pos"] = struct.unpack_from("<3d", tpl, 0)

    errors = len(err)
    if verbose:
        amax, acur, smax, shields = info["vitals"]
        print("%s: блок на 0x%x, поза (%.2f, %.2f, %.2f), углы (%.3f, %.3f, "
              "%.3f)" % (name, off, info["pos"][0], info["pos"][1],
                         info["pos"][2], info["angles"][0], info["angles"][1],
                         info["angles"][2]))
        print("  шкалы: броня %d/%d (%.2f), щиты %d max, %r (%.2f/%.2f/%.2f/%.2f)"
              % (acur, amax, acur / 100.0, smax, shields, shields[0] / 100.0,
                 shields[1] / 100.0, shields[2] / 100.0, shields[3] / 100.0))
        print("  состояние %d, тип шасси %d, трейл %d/%d, оружие %d, "
              "навесных %d, масса %.3f/%d, флаги 0x%x/0x%x"
              % (info["state"], struct.unpack_from("<i", st, 0x3214)[0],
                 info["trail"][0], info["trail"][1], info["weapon_slot"],
                 info["attach"], info["mass"][0], info["mass"][1],
                 info["flags"][0], info["flags"][1]))
        if "template_pos" in info:
            tp = info["template_pos"]
            print("  шаблон спавна (0x815440): (%.2f, %.2f, %.2f)"
                  % (tp[0], tp[1], tp[2]))
        for e in err:
            print("  ERROR %s" % e)
        for w in warn:
            print("  warn  %s" % w)
        print("  -> %s" % ("ok" if not err else "%d ошибок" % len(err)))
    return errors


def cmd_verify(args) -> int:
    saves = find_saves(args.saves)
    if not saves:
        print("сейвы не найдены (%s/SAVEGAME/saved*.sbt)"
              % game_dir(), file=sys.stderr)
        return 1
    bad = 0
    for path in saves:
        try:
            data = open(path, "rb").read()
        except OSError as exc:
            print("не читается %s: %s" % (path, exc), file=sys.stderr)
            return 1
        bad += verify_tail(data, os.path.basename(path))
    print("%d сейв(ов), ошибок: %d" % (len(saves), bad))
    return 2 if bad else 0


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def synth_struct(pos=(100.0, -200.0, 48.0), angles=(0.0, 0.0, 1.0),
                 armor=(10000, 10000), shield=(8000, (8000,) * 4)) -> bytearray:
    st = bytearray(STEP)
    struct.pack_into("<3d", st, 0x00, *pos)
    struct.pack_into("<3d", st, 0x18, *angles)
    struct.pack_into("<d", st, 0x2DD8, 0.5235987755982988)
    struct.pack_into("<d", st, 0x2DE0, 0.3490658503988659)
    struct.pack_into("<d", st, 0x2DB0, 0.0)      # turret pitch
    struct.pack_into("<d", st, 0x2FE0, 1.0)      # mass
    struct.pack_into("<6f", st, 0x2D70, *[0.0, 3.0, 0.0, 5.25, 0.0, 2.75])
    struct.pack_into("<i", st, 0x3204, 5)        # trail index
    struct.pack_into("<i", st, 0x3208, 0)        # trail tick
    struct.pack_into("<i", st, 0x3210, 1)        # driving
    struct.pack_into("<i", st, 0x3214, 0)        # chassis type
    struct.pack_into("<I", st, 0x3200, 0x00010000)
    struct.pack_into("<I", st, 0x3344, 0)
    struct.pack_into("<2i", st, 0x3244, *armor)
    struct.pack_into("<i", st, 0x324C, shield[0])
    struct.pack_into("<4i", st, 0x3250, *shield[1])
    struct.pack_into("<i", st, 0x3294, 0)
    struct.pack_into("<3i", st, 0x3334, 615, 500, 0)
    struct.pack_into("<i", st, 0x3464, 0)
    return st


def synth_tail() -> bytes:
    st = bytes(synth_struct())
    tpl = bytes(synth_struct(pos=(24.0, -208.0, 48.0)))
    tail = (SAVE_MARKER + st + b"\x00" * 7 + b"LEVEL0".ljust(0x50, b"\x00")
            + tpl + b"\x00" * 0x50 + bytes(STEP) + b"\xff")
    return tail


def expect(errors: list, want: str, what: str, fails: list, verbose: bool):
    hit = any(want in e for e in errors)
    if not hit:
        fails.append("%s: ожидалась ошибка %r, получено %r" % (what, want, errors))
    elif verbose:
        print("    ok  %s -> %r" % (what, [e for e in errors if want in e][0]))


def self_test(saves, verbose: bool) -> int:
    fails = []
    print("[1] синтетическая структура")
    st = bytes(synth_struct())
    err, warn, info = check_struct(st)
    if err:
        fails.append("эталон: %r" % err)
    if verbose:
        print("    эталон: ошибок нет, поза %r, шкалы %r" % (info["pos"],
                                                            info["vitals"]))

    def mutate(off, fmt, value):
        b = bytearray(st)
        struct.pack_into(fmt, b, off, value)
        return check_struct(bytes(b))[0]

    cases = [
        (0x3248, "<i", 20000, "armor"),
        (0x3250, "<i", -1, "shields"),
        (0x3204, "<i", 240, "trail_index"),
        (0x3210, "<i", 9, "action_state"),
        (0x0018, "<d", 1.0, "pitch"),
        (0x0020, "<d", -0.9, "roll"),
        (0x0000, "<d", float("nan"), "pos"),
        (0x2FE0, "<d", 0.0, "mass"),
        (0x31BC, "<I", 0x40, "attach[0]"),
        (0x31D0, "<I", 0x1, "attach[5]"),
    ]
    for off, fmt, value, what in cases:
        errs = mutate(off, fmt, value)
        expect(errs, what, "+0x%04x=%r" % (off, value), fails, verbose)

    # an unknown flag bit must be a warning, not an error
    b = bytearray(st)
    struct.pack_into("<I", b, 0x3200, 0x80000000)
    e2, w2, _ = check_struct(bytes(b))
    if e2:
        fails.append("неизвестный бит флагов дал ошибку: %r" % e2)
    elif not any("unknown bits" in w for w in w2):
        fails.append("неизвестный бит флагов не попал в предупреждения")
    elif verbose:
        print("    ok  неизвестный бит флагов -> предупреждение")

    print("[2] синтетический хвост сейва")
    tail = synth_tail()
    if len(tail) != 2 + STEP + TAIL_REST:
        fails.append("синтетический хвост %d байт" % len(tail))
    off = find_player(tail)
    if off != 0:
        fails.append("find_player на синтетике вернул %d" % off)
    else:
        err, _w, _i = check_struct(tail[2:2 + STEP])
        if err:
            fails.append("синтетический сейв: %r" % err)
        elif verbose:
            print("    ok  структура и хвост (%d байт) разобраны" % len(tail))

    print("[3] реальные сейвы")
    if not saves:
        print("    нет сейвов (%s/SAVEGAME) -- пропущено" % game_dir())
    else:
        for path in saves:
            data = open(path, "rb").read()
            n = verify_tail(data, os.path.basename(path), verbose)
            if n:
                fails.append("%s: %d ошибок" % (os.path.basename(path), n))
            elif verbose:
                print("    ok  %s" % os.path.basename(path))

    print()
    if fails:
        for f in fails:
            print("FAIL %s" % f)
        print("self-test: ПРОВАЛ (%d)" % len(fails))
        return 2
    print("self-test: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--map", action="store_true", help="печать карты полей")
    ap.add_argument("--census", action="store_true",
                    help="все обращения к полям по листингу objdump")
    ap.add_argument("--asm", help="путь к листингу (по умолчанию %s)" % ASM_DEFAULT)
    ap.add_argument("--verify-saves", action="store_true",
                    help="проверить инварианты в установленных сейвах")
    ap.add_argument("--self-test", action="store_true",
                    help="синтетика + реальные сейвы, exit != 0 при провале")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("saves", nargs="*", help="сейвы .sbt (по умолчанию SAVEGAME)")
    args = ap.parse_args()

    if args.self_test:
        return self_test(find_saves(args.saves), not args.quiet)
    if args.verify_saves:
        return cmd_verify(args)
    if args.census:
        return cmd_census(args)
    return cmd_map(args)


if __name__ == "__main__":
    sys.exit(main())
