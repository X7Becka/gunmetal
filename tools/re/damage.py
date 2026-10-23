#!/usr/bin/env python3
"""Damage, armour and shields of Gunmetal 1.36 (``M6.5.2``).

``docs/DAMAGE.md`` carries the prose and the evidence; this module is the
runnable model plus the corpus numbers.  Everything was read from the shipped
``gundx.exe`` (reads only) and from the 36 ``.sbt`` scripts -- **no Ghidra**.

The model in one screen::

    player[0] = 0x819040, stride 0x3510
      +0x3200 flags        bit 0x10 = invincible -> no damage
      +0x3244 armor_max    hull maximum, hundredths (10000 = 100.00)
      +0x3248 armor        hull  *current* -- this IS the HP
      +0x324c shield_max   maximum of ONE plate
      +0x3250 i32[4]       shields front,right,back,left
      +0x2fb0/+0x2fb8 f64  damage multipliers (FORCE SHIELD, op 132)

    0x453b50(player, flags, x, y, z, plate, hull, attacker)  player damage
    0x470500(thing,  flags, x, y, z, plate, hull, attacker)  thing damage
    0x477280(...)      sphere blast; scales plate/hull by (radius-dist)/radius
    0x4698b0(slot,x,y,z, radius)  armed *items* within radius*0.5 -> their event

Order of absorption: shields -> hull.  ``plate`` hits plates, ``hull`` hits
the hull directly; whatever a plate cannot absorb spills into the hull.

Usage::

    python3 tools/re/damage.py --self-test          # no game files needed
    python3 tools/re/damage.py --fields             # field/address map
    python3 tools/re/damage.py --calc --plate 400   # one hit, LIGHT chassis
    python3 tools/re/damage.py --calc --plate 8000 --hull 0 --flags 2 \\
            --shields 8000,8000,8000,8000 --repeat 1
    python3 tools/re/damage.py --corpus             # dm1 + level0
    python3 tools/re/damage.py --corpus --all --json

Standard library only; ``gm/sbt.py``/``gm/sbtvm.py`` and
``tools/re/impact.py`` are reused as-is (never edited here).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

#: engine tick; ``SET_SOURCESECTOR_DAMAGE`` is multiplied by it and the virus
#: drain too (``docs/PLAYER_GHIDRA.md``, 0x44db80)
TICKS_PER_SECOND = 91

# ---------------------------------------------------------------------------
# addresses (full VA, ImageBase 0x400000)
# ---------------------------------------------------------------------------
PLAYER_BASE = 0x819040          # player[0]
PLAYER_STRIDE = 0x3510
THING_BASE = 0x7AC080           # thing instance[0]
THING_STRIDE = 0x528

PLAYER_DAMAGE_VA = 0x453B50     # damage_player
PLAYER_KILLED_VA = 0x4536E0     # player_killed(victim, killer)
FLASH_APPLY_VA = 0x4538D0       # damage flash -> model light
THING_DAMAGE_VA = 0x470500      # damage_thing
THING_DESTROY_VA = 0x470290     # destroy_thing
BLAST_VA = 0x477280             # blast_radius_damage (op 75 -> here)
ITEM_BLAST_VA = 0x4698B0        # trigger armed items inside radius
FTOL_VA = 0x40E0E0              # _ftol: trunc toward zero, 64-bit
VIRUS_TICK_VA = 0x443C7D        # shield drain (plate -= dt*[0x80d8b4])
SECTOR_DAMAGE_VA = 0x45799A     # SET_*SECTOR_DAMAGE writer

#: early-out when the frame selector is 1 (VM validation / debug frame)
DEBUG_FRAME_GLOBAL = 0x83D620
GAME_MODE_GLOBAL = 0x83D2C8
MP_FLAGS_GLOBAL = 0x83D184
GOD_MODE_GLOBAL = 0x62CAD4
VIRUS_RATE_GLOBAL = 0x80D8B4

# damage flags, ``arg2`` of 0x453b50 / 0x470500
FLAG_NO_DIRECTION = 0x1         # spread over all four plates
FLAG_FULL_PLATE = 0x2           # each plate takes the full plate value
FLAG_RAW = 0x4                  # skip the FORCE SHIELD multipliers

#: plate indices in ``+0x3250`` (front, right, back, left)
SIDES = ("front", "right", "back", "left")

#: ``event_post`` ids the damage path raises (docs/EVENTS.md §4.1)
EVENTS = {
    -12: "SYSTEM_PLAYER_FRONT_SHIELD_GONE",
    -13: "SYSTEM_PLAYER_BACK_SHIELD_GONE",
    -14: "SYSTEM_PLAYER_LEFT_SHIELD_GONE",
    -15: "SYSTEM_PLAYER_RIGHT_SHIELD_GONE",
    -17: "SYSTEM_PLAYER_LIFE_HALF_GONE",
    -18: "SYSTEM_PLAYER_ABOVE_HALF_LIFE",
}

#: wall-clock names per plate index, exactly as 0x453fcf/0x45401d/0x45406b/
#: 0x4540b9 push them ([0] -12, [1] -15, [2] -13, [3] -14)
PLATE_EVENT = (-12, -15, -13, -14)

# ---------------------------------------------------------------------------
# field maps
# ---------------------------------------------------------------------------
# (offset, type, name, status, evidence)
PLAYER_FIELDS = [
    (0x3178, "u32", "body_object_id", "код",
     "0x453bdb: 0 -> ранний выход (слот пуст)"),
    (0x3200, "u32", "flags", "код",
     "0x453b94: бит 0x10 (неуязвимость) -> ранний выход; 0x20 вспышка; "
     "0x40/0x80 заражение"),
    (0x3224, "i32", "damage_flash_timer (мс)", "код",
     "0x453f8a: += hull*91/100; НЕ щиты (в задаче названо неверно)"),
    (0x3244, "i32", "armor_max", "код",
     "0x453e3a (половина), 0x452dd8 (респавн: armor = armor_max)"),
    (0x3248, "i32", "armor (он же HP)", "код",
     "0x453e58: armor -= spill; 0x4536e0 при смерти пишет 0"),
    (0x324c, "i32", "shield_max (одной плиты)", "код",
     "0x453c47 (порог тряски), 0x452de4 (респавн: 4 плиты = shield_max)"),
    (0x3250, "i32[4]", "shields front/right/back/left", "код",
     "0x453d91/0x453dbf: направленное вычитание, 0x443c7d вирус"),
    (0x3260, "i32[4]", "shields_spawn", "код",
     "0x452e1a: снимок shield_max при спавне"),
    (0x2fb0, "f64", "damage_mult_plate", "код",
     "0x453c13: plate = ftol(plate * [0x81bff0]); пишет op 132"),
    (0x2fb8, "f64", "damage_mult_hull", "код",
     "0x453c24: hull = ftol(hull * [0x81bff8])"),
    (0x33c8, "i32", "hitstun / shake", "код",
     "0x453ca9: max(old, clamp(524288*(plate+hull)/max(1,(shield_max+armor)/64)))"
     "; 0x443716: -= ticks*0x4000, пол 0; читают 0x447140..0x448cd1"),
    (0x33d4, "i32", "respawn timer (в состоянии 7)", "код",
     "0x45372f = 0 при смерти, >= 0x88 -> 0x452670 (docs/DM_RULES.md §4.3)"),
    (0x3210, "i32", "action_state", "код",
     "0x453700: 0/7 -> player_killed не срабатывает; 0x453763 ставит 7"),
    (0x3510, "—", "stride записи игрока", "код", "0x819040 + i*0x3510"),
]

THING_FIELDS = [
    (0x360, "u32", "flags_a", "код",
     "бит 0 слот занят (0x46d5e0/0x470c70)"),
    (0x364, "u32", "flags_b", "код",
     "0x4705f7: бит 0x2 -> plate = 0, бит 0x4 -> hull = 0; "
     "0x47053b бит 0x800 -> атакующий берётся из +0x47c; "
     "0x470824 бит 0x1000 -> событие -17; 0x470888 бит 0x200 -> статус"),
    (0x368, "u32", "flags_c", "код",
     "0x4707c6: бит 0x20000 + game_mode 0 -> frags[attacker]++ при destroy"),
    (0x398, "i32", "health", "код", "0x4707ba: health -= spill"),
    (0x39c, "i32", "max_health (0 = неуязвима)", "код",
     "0x470633: <= 0 -> выход; 0x4707a1 половина"),
    (0x3a0, "i32[4]", "shields (текущие)", "код", "0x47072c: shields[i] -= plate"),
    (0x3b0, "i32[4]", "shield_max[4]", "код", "0x46dc2d: копия из THING_TYPES"),
    (0x0e0, "f64", "damage_mult_plate", "код", "0x470611: plate = ftol(plate*[+0xe0])"),
    (0x0e8, "f64", "damage_mult_hull", "код", "0x470626: hull = ftol(hull*[+0xe8])"),
    (0x47c, "u32", "owner (0..7 игрок / вещь+8)", "код",
     "0x4705bb / 0x453bce: атакующий = owner, если flags_b & 0x800"),
    (0x518, "u32", "damaged_event", "код", "0x470995: event_post после не смертельного попадания"),
    (0x528, "—", "stride записи вещи", "код", "0x7ac080 + i*0x528, 260 слотов"),
]

GLOBALS = [
    (DEBUG_FRAME_GLOBAL, "frame selector", "== 1 -> damage_player/blast выходят сразу",
     "0x453b53, 0x4698b0; 0x457412 (VM трассировка)"),
    (GAME_MODE_GLOBAL, "game_mode", "0 кампания .. 3 hunt", "docs/DM_RULES.md §2.1"),
    (MP_FLAGS_GLOBAL, "mp_flags", "0x20 гандикап — только деньги на респавне",
     "0x452e67 (docs/DM_RULES.md §5)"),
    (GOD_MODE_GLOBAL, "god_mode (/god)", "условие 26 GOD_MODE; в уроне НЕ читается",
     "0x44a9e8 запись, 0x45eb3f чтение"),
    (VIRUS_RATE_GLOBAL, "virus drain per tick", "DEFINE_VIRUS L0, в поставе 16",
     "0x443c8f imul edx,[0x80d8b4]"),
]

#: constants read out of .rdata; ``--self-test`` checks the doubles
CONSTANTS = [
    (0x4A2490, 0.0, "нулевой порог выбора стороны (front/back, right/left)"),
    (0x4A2498, 1.0, "нижняя граница масштаба тряски"),
    (0x4A2518, 1.0 / 64.0, "делитель суммы щит+броня для тряски"),
    (0x4A2520, 524288.0, "множитель тряски (0x80000)"),
    (0x4A2528, 0.1, "порог «точка попадания совпала с игроком»"),
    (0x4A2658, 0.0, "минимальная глубина конуса hitscan: строго depth > 0"
                    " (не 0.32, как в docs/WEAPONS_CPP.md §6)"),
    (0x4A2680, 0.1, "тот же порог 0.1 для вещей (0x470662)"),
    (0x4A2608, 0.5, "радиус цепного подрыва предметов = arg * 0.5 (0x4698c1)"),
]

#: byte anchors: (VA, expected bytes, what).  ``--anchors``/``--self-test``.
ANCHORS = [
    (0x453B50, "83ec44", "damage_player: sub esp,0x44"),
    (0x453B53, "a120d68300", "damage_player: mov eax,ds:0x83d620 (ранний выход)"),
    (0x453B94, "f68640c28100 10", "test byte [esi+0x81c240],0x10 (неуязвимость)"),
    (0x453C13, "dc8ef0bf8100", "fmul qword [esi+0x81bff0] (множитель плиты)"),
    (0x453C24, "dc8ef8bf8100", "fmul qword [esi+0x81bff8] (множитель корпуса)"),
    (0x453C8F, "3d00000100", "cmp eax,0x10000 (нижний кламп тряски)"),
    (0x453C9D, "3d00000800", "cmp eax,0x80000 (верхний кламп тряски)"),
    (0x453DB6, "c1ff02", "sar edi,0x2 (четверть на плиту без флага 2)"),
    (0x453DC4, "8b01", "mov eax,[ecx] (цикл по 4 плитам)"),
    (0x453E3A, "8b8684c28100", "mov eax,[esi+0x81c284] (armor_max)"),
    (0x453E58, "2bcd", "sub ecx,ebp (armor -= spill)"),
    (0x453E5B, "898e88c28100", "mov [esi+0x81c288],ecx (запись armor)"),
    (0x453EF4, "6aef", "push 0xffffffef (событие -17)"),
    (0x454013, "6af4", "push 0xfffffff4 (плита 0: -12)"),
    (0x454061, "6af1", "push 0xfffffff1 (плита 1: -15)"),
    (0x4540AF, "6af3", "push 0xfffffff3 (плита 2: -13)"),
    (0x4540FD, "6af2", "push 0xfffffff2 (плита 3: -14)"),
    (0x4536E0, "538b5c2408", "player_killed: push ebx / mov ebx,[esp+8]"),
    (0x453763, "c78650c2810007000000", "player_killed: action_state = 7"),
    (0x453718, "c78688c2810000000000", "player_killed: armor = 0"),
    (0x470500, "83ec34", "damage_thing: sub esp,0x34"),
    (0x4705F7, "8b86e4c37a00", "damage_thing: flags_b вещи"),
    (0x4707BA, "2bc5", "damage_thing: health -= spill"),
    (0x4707C6, "f786e8c37a0000000200", "test [esi+0x7ac3e8],0x20000 (счёт фрага)"),
    (0x477280, "558bec", "blast_radius_damage: push ebp / mov ebp,esp"),
    (0x4773E1, "f7ff", "idiv edi (falloff (radius-dist)/radius)"),
    (0x4698B0, "833d20d6830001", "item blast: cmp [0x83d620],1"),
    (0x4698C1, "dc0d08264a00", "item blast: fmul 0.5"),
    (0x443C8F, "0faf15b4d88000", "вирус: imul edx,[0x80d8b4]"),
    (0x45799A, "8990a00b8000", "SET_*SECTOR_DAMAGE: damage_a"),
    (0x4579A0, "8988a40b8000", "SET_*SECTOR_DAMAGE: damage_b"),
    (0x44A9E8, "c705d4ca62000100000 0", "/god -> [0x62cad4] = 1"),
    (0x45EB3F, "a1d4ca6200", "GOD_MODE (условие 26): mov eax,[0x62cad4]"),
]


# ---------------------------------------------------------------------------
# the model
# ---------------------------------------------------------------------------
def ftol(value):
    """x87 ``fistp`` after switching the control word to truncation (0x40e0e0)."""
    return int(value)


def half_of(value):
    """``cdq / sub / sar 1`` for a positive int (the engine halves with sar)."""
    return value // 2


def apply_player_damage(plate, hull, shields, shield_max, armor, armor_max,
                        flags=0, side=-1, mult_plate=1.0, mult_hull=1.0,
                        invincible=False, debug_frame=False):
    """Exact ``0x453b50`` model.  All gauges are integer hundredths.

    ``side`` is the plate the hit lands on (0..3) or ``-1`` for a hit whose
    point coincides with the player (``0x453cc2``..``0x453d71``: no direction
    only when ``|dx| <= 0.1`` *and* ``|dy| <= 0.1``, comparisons strict), which
    spreads the plate damage over all four shields.  ``flags`` is ``arg2``:
    ``1`` forces ``side = -1``, ``2`` makes each plate take the full value,
    ``4`` skips the FORCE SHIELD multipliers.
    """
    shields = ([int(s) for s in shields] + [0, 0, 0, 0])[:4]
    report = {
        "plate_in": plate, "hull_in": hull, "flags": flags, "side": side,
        "mult_plate": mult_plate, "mult_hull": mult_hull,
        "plate": plate, "hull": hull,
        "shields_before": shields[:], "shields_after": None,
        "plate_absorbed": [0, 0, 0, 0], "broken": [False] * 4,
        "spill": 0, "hull_absorbed": 0,
        "armor_before": armor, "armor_after": armor,
        "stun": 0, "dead": False, "events": [], "skipped": None,
    }

    if debug_frame:
        report["skipped"] = "debug_frame: ds:0x83d620 == 1"
        report["shields_after"] = shields
        return report
    if invincible:
        report["skipped"] = "invincible: player+0x3200 & 0x10"
        report["shields_after"] = shields
        return report

    if plate < 0:
        report["shields_after"] = shields
        if plate == -1:
            add = ftol(hull * 91.0 / 100.0) if hull >= 0 else -ftol((-hull) * 91.0 / 100.0)
            report["status"] = "burn"
            report["flash_timer_add"] = add
        elif plate == -2:
            report["status"] = "freeze/infect"
            report["status_flags_or"] = 0xC0
        else:
            report["status"] = "unknown(%d)" % plate
        return report

    if not (flags & FLAG_RAW):
        plate = ftol(plate * mult_plate)
        hull = ftol(hull * mult_hull)
        if plate == 0 and hull == 0:
            report["skipped"] = "both gauges truncated to 0 by the multipliers"
            report["shields_after"] = shields
            return report

    pool = shield_max + armor
    scale = pool * (1.0 / 64.0)
    if scale < 1.0:
        scale = 1.0
    stun = ftol(524288.0 * (plate + hull) / scale)
    stun = max(0x10000, min(0x80000, stun))
    report["stun"] = stun

    if flags & FLAG_NO_DIRECTION:
        side = -1

    spill = hull
    if side >= 0:
        if shields[side] > 0:
            shields[side] -= plate
        else:
            spill = plate + hull
    else:
        per = plate if (flags & FLAG_FULL_PLATE) else (plate >> 2)
        over = plate >> 2
        for i in range(4):
            if shields[i] > 0:
                shields[i] -= per
            else:
                spill += over

    for i in range(4):
        if shields[i] < 0:
            neg = (shields[i] >> 2) if (flags & FLAG_FULL_PLATE) else shields[i]
            spill -= neg
            shields[i] = 0
            report["broken"][i] = True

    for i in range(4):
        report["plate_absorbed"][i] = report["shields_before"][i] - shields[i]
    report["shields_after"] = shields
    report["spill"] = spill
    report["plate"] = plate
    report["hull"] = hull

    old = armor
    armor -= spill
    report["hull_absorbed"] = spill
    report["armor_after"] = armor
    report["dead"] = armor <= 0

    if not report["dead"] and armor_max > 0:
        half = half_of(armor_max)
        if old >= half and armor < half:
            report["events"].append(-17)
        elif old < half and armor >= half:
            # by code reachable only with negative damage (0x453f03 path)
            report["events"].append(-18)
    for i in range(4):
        if report["broken"][i]:
            report["events"].append(PLATE_EVENT[i])
    return report


def apply_thing_damage(plate, hull, shields, health, max_health,
                       flags=0, side=-1, mult_plate=1.0, mult_hull=1.0,
                       no_plate_damage=False, no_hull_damage=False,
                       flags_b=0):
    """Exact ``0x470500`` model (no ``0x3200``/``0x83d620`` guards for things)."""
    shields = ([int(s) for s in shields] + [0, 0, 0, 0])[:4]
    report = {
        "plate_in": plate, "hull_in": hull, "flags": flags, "side": side,
        "plate": plate, "hull": hull,
        "shields_before": shields[:], "shields_after": None,
        "plate_absorbed": [0, 0, 0, 0], "broken": [False] * 4,
        "spill": 0, "health_before": health, "health_after": health,
        "destroyed": False, "events": [], "skipped": None,
    }
    if max_health <= 0:
        # game/weapons.cpp rule; 0x470500 itself never reads +0x39c
        report["skipped"] = "max_health <= 0: immune (C++ rule, unproven)"
        report["shields_after"] = shields
        return report
    if health <= 0:
        report["skipped"] = "health <= 0: already dead (0x470633)"
        report["shields_after"] = shields
        return report
    if plate < 0:
        report["shields_after"] = shields
        if plate == -1:
            # 0x4709d4: [thing+0x388] (burn timer) += hull*91/100, then flags_a |= 0x40
            add = ftol(hull * 91.0 / 100.0)
            report["status"] = "burn"
            report["flash_timer_add"] = add
            report["flags_a_or"] = 0x40 if add > 0 else 0
        elif plate == -2:
            # 0x470a02: skipped when flags_b & 0x40, else flags_a |= 0x18
            if flags_b & 0x40:
                report["skipped"] = "already infected (flags_b & 0x40)"
            else:
                report["status"] = "freeze/infect"
                report["flags_a_or"] = 0x18
        else:
            report["status"] = "unknown(%d)" % plate
        return report
    if no_plate_damage:
        plate = 0
    if no_hull_damage:
        hull = 0
    plate = ftol(plate * mult_plate)
    hull = ftol(hull * mult_hull)
    report["plate"] = plate
    report["hull"] = hull
    if plate <= 0 and hull <= 0:
        report["skipped"] = "no damage after flags/multipliers"
        report["shields_after"] = shields
        return report

    if flags & FLAG_NO_DIRECTION:
        side = -1

    spill = hull
    if side >= 0:
        idx = side
        shields[idx] -= plate
        if shields[idx] < 0:
            spill -= shields[idx]
            shields[idx] = 0
            report["broken"][idx] = True
    else:
        per = plate if (flags & FLAG_FULL_PLATE) else (plate >> 2)
        for i in range(4):
            shields[i] -= per
            if shields[i] < 0:
                if flags & FLAG_FULL_PLATE:
                    # 0x470784: cdq/and 3/add/sar 2/neg -> |x| / 4 truncating
                    neg = -((-shields[i]) // 4)
                else:
                    neg = shields[i]
                spill -= neg
                shields[i] = 0
                report["broken"][i] = True
    for i in range(4):
        report["plate_absorbed"][i] = report["shields_before"][i] - shields[i]
    report["shields_after"] = shields
    report["spill"] = spill

    health -= spill
    report["health_after"] = health
    if health <= 0:
        report["destroyed"] = True
    else:
        half = half_of(max_health)
        if (flags_b & 0x1000) and report["health_before"] >= half and health < half:
            report["events"].append(-17)     # 0x47081a
    report["damaged_event"] = not report["destroyed"]
    return report


def blast_falloff(value, radius, distance):
    """``value * (radius - distance) / radius`` with integer idiv (0x4773a3)."""
    if radius <= 0:
        return 0
    if distance >= radius:
        return 0
    if value < 0:
        return 0
    return value * (radius - distance) // radius


def hits_until_dead(plate, hull, shields, shield_max, armor, armor_max,
                    flags=0, side=0, limit=10000):
    """Apply the same hit repeatedly; return (hits, final report)."""
    shields = list(shields)
    report = None
    for hit in range(1, limit + 1):
        report = apply_player_damage(plate, hull, shields, shield_max, armor,
                                     armor_max, flags=flags, side=side)
        shields = report["shields_after"]
        armor = report["armor_after"]
        if report["dead"]:
            return hit, report
        if report["skipped"]:
            return 0, report
    return limit, report


# ---------------------------------------------------------------------------
# exe access (anchors, constants)
# ---------------------------------------------------------------------------
def _exelib():
    sys.path.insert(0, os.path.join(ROOT, "tools", "re"))
    import exelib
    return exelib


def exe_bytes(va, size):
    """Raw bytes at ``va`` or ``None`` when the exe/cache is unavailable."""
    try:
        lib = _exelib()
        off = lib.va2off(va)
        if off is None:
            return None
        return lib.data()[off:off + size]
    except Exception:
        return None


def check_anchors():
    out = []
    for va, want, what in ANCHORS:
        want = want.replace(" ", "")
        got = exe_bytes(va, len(want) // 2)
        if got is None:
            out.append({"vma": va, "what": what, "want": want, "got": "",
                        "ok": None})
            continue
        ok = got.hex() == want
        out.append({"vma": va, "what": what, "want": want,
                    "got": got.hex(), "ok": ok})
    return out


def read_doubles():
    out = []
    for va, value, what in CONSTANTS:
        raw = exe_bytes(va, 8)
        got = struct.unpack("<d", raw)[0] if raw and len(raw) == 8 else None
        out.append({"vma": va, "value": value, "got": got, "what": what,
                    "ok": None if got is None else abs(got - value) < 1e-12})
    return out


# ---------------------------------------------------------------------------
# fields
# ---------------------------------------------------------------------------
def print_fields(as_json=False):
    if as_json:
        print(json.dumps({
            "player_base": PLAYER_BASE, "player_stride": PLAYER_STRIDE,
            "thing_base": THING_BASE, "thing_stride": THING_STRIDE,
            "player_fields": [
                {"offset": o, "type": t, "name": n, "status": s,
                 "va": "0x%06x" % (PLAYER_BASE + o), "evidence": e}
                for o, t, n, s, e in PLAYER_FIELDS],
            "thing_fields": [
                {"offset": o, "type": t, "name": n, "status": s,
                 "va": "0x%06x" % (THING_BASE + o), "evidence": e}
                for o, t, n, s, e in THING_FIELDS],
            "globals": [{"va": "0x%06x" % v, "name": n, "role": r,
                         "evidence": e} for v, n, r, e in GLOBALS],
            "constants": [{"va": "0x%06x" % v, "value": x, "what": w}
                          for v, x, w in CONSTANTS],
            "flags": {"1": "FLAG_NO_DIRECTION", "2": "FLAG_FULL_PLATE",
                      "4": "FLAG_RAW"},
            "events": {str(k): v for k, v in sorted(EVENTS.items())},
            "plate_event": {str(i): PLATE_EVENT[i] for i in range(4)},
        }, indent=2, sort_keys=True))
        return

    print("player record 0x%06x, stride 0x%x" % (PLAYER_BASE, PLAYER_STRIDE))
    print("%-9s %-8s %-30s %-6s %s" % ("offset", "type", "name", "status",
                                       "evidence"))
    for o, t, n, s, e in PLAYER_FIELDS:
        print("+0x%04x  %-8s %-30s %-6s %s" % (o, t, n, s, e))
    print()
    print("thing record 0x%06x, stride 0x%x" % (THING_BASE, THING_STRIDE))
    for o, t, n, s, e in THING_FIELDS:
        print("+0x%04x  %-8s %-30s %-6s %s" % (o, t, n, s, e))
    print()
    print("globals")
    for v, n, r, e in GLOBALS:
        print("0x%06x  %-24s %-52s %s" % (v, n, r, e))
    print()
    print("constants (.rdata, doubles)")
    for v, x, w in CONSTANTS:
        print("0x%06x  %-12g %s" % (v, x, w))
    print()
    print("arg2 flags: 0x1 no direction | 0x2 full plate per shield | "
          "0x4 raw (no FORCE SHIELD)")
    print("events: " + ", ".join("%d %s" % (k, v) for k, v in
                                 sorted(EVENTS.items())))
    print("plate i -> event: " + ", ".join(
        "%s=%d" % (SIDES[i], PLATE_EVENT[i]) for i in range(4)))


# ---------------------------------------------------------------------------
# calc
# ---------------------------------------------------------------------------
def parse_shields(text):
    if text is None:
        return [8000, 8000, 8000, 8000]
    parts = [p for p in text.replace(" ", "").split(",") if p]
    if len(parts) == 1:
        v = int(parts[0], 0)
        return [v, v, v, v]
    if len(parts) != 4:
        raise SystemExit("--shields wants one value or four (front,right,back,left)")
    return [int(p, 0) for p in parts]


def print_report(rep, as_json=False):
    if as_json:
        print(json.dumps(rep, indent=2, sort_keys=True))
        return
    if rep.get("skipped"):
        print("skipped: %s" % rep["skipped"])
        return
    if rep.get("status"):
        print("status hit: %s (plate=%d hull=%d)" % (rep["status"],
                                                     rep["plate_in"],
                                                     rep["hull_in"]))
        if "flash_timer_add" in rep:
            where = "thing+0x388" if "health_before" in rep else "player+0x3224"
            print("  %s (flash/burn, ms) += %d"
                  % (where, rep["flash_timer_add"]))
        if "status_flags_or" in rep:
            print("  player+0x3200 |= 0x%x" % rep["status_flags_or"])
        if "flags_a_or" in rep:
            print("  thing+0x360 |= 0x%x" % rep["flags_a_or"])
        return
    print("flags=0x%x side=%s  plate=%d hull=%d  (after mult %.3f/%.3f)"
          % (rep["flags"],
             "spread" if rep["side"] < 0 else SIDES[rep["side"]],
             rep["plate"], rep["hull"], rep["mult_plate"], rep["mult_hull"]))
    print("stun (player+0x33c8) = %d (0x%x)" % (rep["stun"], rep["stun"]))
    for i, name in enumerate(SIDES):
        print("  %-5s %6d -> %6d  (-%d)%s"
              % (name, rep["shields_before"][i], rep["shields_after"][i],
                 rep["plate_absorbed"][i],
                 "  BROKEN" if rep["broken"][i] else ""))
    print("  spill to hull: %d" % rep["spill"])
    if "armor_before" in rep:
        print("  armor/HP: %d -> %d%s"
              % (rep["armor_before"], rep["armor_after"],
                 "  DEAD (0x4536e0 sets 0)" if rep["dead"] else ""))
    else:
        print("  health: %d -> %d%s"
              % (rep["health_before"], rep["health_after"],
                 "  DESTROYED (0x470290)" if rep["destroyed"] else ""))
    if rep["events"]:
        print("  events: " + ", ".join("%d %s" % (e, EVENTS.get(e, "?"))
                                       for e in rep["events"]))


def run_calc(args):
    if args.side < -1 or args.side > 3:
        raise SystemExit("--side wants -1 (spread) or 0..3")
    shields = parse_shields(args.shields)
    armor = args.armor
    report = apply_player_damage(
        args.plate, args.hull, shields, args.shield_max, armor, args.armor_max,
        flags=args.flags, side=args.side, mult_plate=args.mult_plate,
        mult_hull=args.mult_hull, invincible=args.invincible,
        debug_frame=args.debug_frame)

    if args.repeat > 1 or args.repeat == 0:
        limit = args.repeat if args.repeat else 10000
        hits, last = hits_until_dead(
            args.plate, args.hull, shields, args.shield_max, armor,
            args.armor_max, flags=args.flags, side=-1 if args.side < 0 else args.side,
            limit=limit)
        data = {"first_hit": report, "hits": hits,
                "last": last, "repeat": args.repeat}
        if args.json:
            print(json.dumps(data, indent=2, sort_keys=True))
        else:
            print_report(report)
            print()
            if hits:
                print("%d hits of plate=%d hull=%d -> dead "
                      "(armor %d -> %d)" % (hits, args.plate, args.hull,
                                            armor, last["armor_after"]))
            else:
                print("no hit applies: %s" % last.get("skipped"))
        return 0

    if args.json:
        print(json.dumps({"first_hit": report}, indent=2, sort_keys=True))
    else:
        print_report(report)
    return 0


# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------
def level_paths(game, wanted, use_all):
    if use_all:
        return sorted(glob.glob(os.path.join(game, "*.sbt")))
    out = []
    for stem in wanted:
        path = os.path.join(game, stem if stem.endswith(".sbt") else stem + ".sbt")
        if os.path.isfile(path):
            out.append(path)
        else:
            alt = os.path.join(game, stem)
            if os.path.isfile(alt):
                out.append(alt)
    return out


def corpus_level(path):
    """Weapons, chassis and projectile thing types of one ``.sbt``."""
    from tools.re import impact

    level = impact.Level(path)
    weapons = []
    for record in level.weapons:
        info = impact.resolve_weapon(level, record)
        hitscan_damage = None
        hitscan_range = None
        if info["fire"] in ("FIRE_SOURCEPLAYER_BULLET", "FIRE_SOURCEPLAYER_LASER"):
            insns = level.entity_script(impact._s32(record[1][4]))
            branch = impact.fire_branch(insns) if insns else None
            if branch is not None:
                args = branch[1]
                index = 2 if info["fire"] == "FIRE_SOURCEPLAYER_BULLET" else 4
                if len(args) > index:
                    hitscan_damage = int(args[index])
                if len(args) > 0:
                    hitscan_range = int(args[0]) if index == 2 else int(args[1])
        thing = None
        if info["thing_type"] is not None:
            entry = level.things[info["thing_type"]] if \
                info["thing_type"] < len(level.things) else None
            if entry is not None:
                thing = {
                    "index": info["thing_type"], "name": entry.name,
                    "hp": _tail_u32(entry, 0x1D4),
                    "shields": [_tail_u32(entry, 0x1D8 + 4 * i) for i in range(4)],
                    "speed": info["speed"], "hits": info["hits"],
                    "expire": info["expire"],
                }
        blasts = []
        seen = set()
        for item in info["impacts"]:
            b = item["blast"]
            row = {
                "plate": _s32(b["plate"]), "hull": _s32(b["hull"]),
                "radius": b["radius"], "flags": b["flags"],
                "no_direction": b["no_direction"], "event": b["event"],
            }
            key = (row["plate"], row["hull"], row["radius"], row["flags"],
                   row["no_direction"])
            if key in seen:          # the same burst repeats per chain hop
                continue
            seen.add(key)
            blasts.append(row)
        weapons.append({
            "name": info["name"], "fire": info["fire"],
            "head": "hitscan" if info["fire"] in
                    ("FIRE_SOURCEPLAYER_BULLET", "FIRE_SOURCEPLAYER_LASER")
                    else "thing",
            "hitscan_damage": hitscan_damage,
            "hitscan_range": hitscan_range,
            "thing": thing, "blasts": blasts,
        })
    return {
        "stem": os.path.splitext(os.path.basename(path))[0],
        "weapons": weapons,
        "chassis": chassis_table(level),
    }


def _s32(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def _tail_u32(entry, offset):
    return struct.unpack_from("<I", entry.tail, offset - 0x198)[0]


def chassis_table(level):
    """DEFINE_PLAYER_TYPES vitals: ``+0x14c*100`` = shield_max, ``+0x150*100``
    = armor_max (0x47acce / 0x47ace0, respawn copies at 0x452dd8/0x452de4)."""
    try:
        from tools.re import weapon_fields as wf
        tables = wf.read_level(level.path)
        tab = wf.BY_NAME["DEFINE_PLAYER_TYPES"]
    except Exception:
        return []
    out = []
    for strings, dwords in tables.get("DEFINE_PLAYER_TYPES", []):
        try:
            view = wf.memory_view(tab, dwords)
        except Exception:
            continue
        name = strings[0] if strings else "?"
        if 0x14C not in view or 0x150 not in view:
            continue
        out.append({"name": name,
                    "shield_max": view[0x14C][1],
                    "armor_max": view[0x150][1]})
    return out


def corpus_damage_numbers(levels):
    """Which plate values occur in the shipped scripts."""
    seen = {}
    for lv in levels:
        for w in lv["weapons"]:
            if w["hitscan_damage"]:
                key = ("hitscan", w["hitscan_damage"])
                seen.setdefault(key, set()).add(w["name"])
            for b in w["blasts"]:
                key = ("blast", b["plate"], b["hull"], b["radius"], b["flags"],
                       b["no_direction"])
                seen.setdefault(key, set()).add(w["name"])
    return seen


def run_corpus(args):
    if not os.path.isdir(args.game):
        print("нет каталога игры: %s" % args.game, file=sys.stderr)
        return 1
    paths = level_paths(args.game, args.levels.split(",") if args.levels
                        else ["dm1", "level0"], args.all)
    if not paths:
        print("нет .sbt в %s" % args.game, file=sys.stderr)
        return 1

    levels = [corpus_level(p) for p in paths]

    if args.json:
        print(json.dumps({
            "game": args.game,
            "levels": levels,
            "constants": {"ticks_per_second": TICKS_PER_SECOND,
                          "blast": "value*(radius-dist)/radius (idiv)",
                          "plate_flags": {"1": "no direction",
                                          "2": "full plate per shield"}},
        }, indent=2, sort_keys=True))
        return 0

    for lv in levels:
        print("=== %s" % lv["stem"])
        print("  %-16s %-6s %-22s %-8s %s"
              % ("weapon", "kind", "projectile", "damage", "blasts"))
        for w in lv["weapons"]:
            dmg = "-"
            if w["hitscan_damage"] is not None:
                dmg = "%d (%.2f)" % (w["hitscan_damage"],
                                     w["hitscan_damage"] / 100.0)
            proj = "-"
            if w["thing"]:
                proj = "%s hp=%d sh=%s" % (
                    w["thing"]["name"], w["thing"]["hp"],
                    "/".join(str(x) for x in w["thing"]["shields"]))
            blasts = "; ".join(
                "p%d/h%d r%d f%d%s" % (b["plate"], b["hull"], b["radius"],
                                       b["flags"],
                                       " nd" if b["no_direction"] else "")
                for b in w["blasts"]) or "-"
            print("  %-16s %-6s %-22s %-8s %s"
                  % (w["name"], w["head"], proj, dmg, blasts))
        for ch in lv["chassis"]:
            print("  chassis %-16s shield_max=%d armor_max=%d" %
                  (ch["name"], ch["shield_max"], ch["armor_max"]))
        # a concrete model example on the lightest chassis
        if lv["chassis"]:
            ch = lv["chassis"][0]
            print("  example: %s, one hit on the front plate" % ch["name"])
            for w in lv["weapons"]:
                if not w["hitscan_damage"] or w["hitscan_damage"] < 10:
                    continue
                hits, _ = hits_until_dead(
                    w["hitscan_damage"], 0,
                    [ch["shield_max"]] * 4, ch["shield_max"],
                    ch["armor_max"], ch["armor_max"], side=0)
                print("    %-16s plate=%5d -> %3d hits to kill (front)"
                      % (w["name"], w["hitscan_damage"], hits))
        print()

    print("unique damage numbers in the scanned levels")
    for key in sorted(corpus_damage_numbers(levels), key=lambda k: (k[0],) + k[1:]):
        if key[0] == "hitscan":
            print("  hitscan plate=%5d   %s" % (key[1], ", ".join(sorted(corpus_damage_numbers(levels)[key]))))
        else:
            print("  blast  plate=%5d hull=%5d radius=%4d flags=%d nd=%d   %s"
                  % (key[1], key[2], key[3], key[4], key[5],
                     ", ".join(sorted(corpus_damage_numbers(levels)[key]))))
    return 0


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def self_test(game=None):
    game = game or DEFAULT_GAME
    checks = []

    def check(name, cond, extra=""):
        checks.append((name, bool(cond), extra))

    # --- basic directional hit -------------------------------------------
    sh = [8000, 8000, 8000, 8000]
    r = apply_player_damage(400, 0, sh, 8000, 10000, 10000, side=0)
    check("MG 4.00 front: plate 8000 -> 7600",
          r["shields_after"] == [7600, 8000, 8000, 8000], r["shields_after"])
    check("MG 4.00 front: hull untouched", r["armor_after"] == 10000)
    check("MG 4.00 front: no spill", r["spill"] == 0)
    check("MG 4.00 front: stun clamps to 0x80000", r["stun"] == 0x80000)
    check("MG 4.00 front: not dead", not r["dead"])

    # --- overflow of a plate goes into the hull ---------------------------
    r = apply_player_damage(6000, 0, [8000, 3000, 8000, 8000], 8000, 10000,
                            10000, side=1)
    check("overflow: plate 3000 - 6000 -> 0", r["shields_after"][1] == 0)
    check("overflow: 3000 spill to hull", r["spill"] == 3000, r["spill"])
    check("overflow: armor 10000 -> 7000", r["armor_after"] == 7000)
    check("overflow: event -15 (right plate)", r["events"] == [-15], r["events"])

    # --- spread (flag 1) --------------------------------------------------
    r = apply_player_damage(400, 0, sh, 8000, 10000, 10000, flags=1)
    check("spread flag1: 100 per plate",
          r["shields_after"] == [7900, 7900, 7900, 7900], r["shields_after"])
    r = apply_player_damage(402, 0, sh, 8000, 10000, 10000, flags=1)
    check("spread 402: 100 per plate, 2 lost to sar 2",
          r["shields_after"] == [7900, 7900, 7900, 7900], r["shields_after"])

    # --- blast (flag 2, full plate per shield) ----------------------------
    r = apply_player_damage(8000, 0, sh, 8000, 10000, 10000, flags=2)
    check("blast 8000 flag2: all four plates to 0",
          r["shields_after"] == [0, 0, 0, 0], r["shields_after"])
    check("blast 8000 flag2: no spill (plates exactly cover it)",
          r["spill"] == 0, r["spill"])
    check("blast 8000 flag2: exact 0 is not 'broken' (no events)",
          r["events"] == [], r["events"])
    r = apply_player_damage(8001, 0, sh, 8000, 10000, 10000, flags=2)
    check("blast 8001 flag2: four plate events -12/-15/-13/-14",
          r["events"] == [-12, -15, -13, -14], r["events"])
    # each plate goes to -1; the overkill is shifted right by 2 (sar -1,2 = -1)
    check("blast 8001 flag2: spill 4*|-1>>2| = 4", r["spill"] == 4, r["spill"])
    r = apply_player_damage(8000, 0, [8000, 8000, 0, 0], 8000, 10000, 10000,
                            flags=2)
    check("blast with two empty plates: spill 2*2000",
          r["spill"] == 4000, r["spill"])
    check("blast with two empty plates: armor 6000", r["armor_after"] == 6000)

    # --- hull damage directly (second gauge) ------------------------------
    r = apply_player_damage(0, 1500, sh, 8000, 10000, 10000, side=0)
    check("hull gauge: plates untouched",
          r["shields_after"] == sh, r["shields_after"])
    check("hull gauge: armor 10000 -> 8500", r["armor_after"] == 8500)

    # --- death ------------------------------------------------------------
    r = apply_player_damage(0, 10001, sh, 8000, 10000, 10000, side=0)
    check("death: armor goes negative then 0x4536e0 writes 0",
          r["dead"] and r["armor_after"] == -1, r["armor_after"])
    check("death: no -17 event", -17 not in r["events"], r["events"])

    # --- half-life event --------------------------------------------------
    r = apply_player_damage(0, 5000, sh, 8000, 10000, 10000, side=0)
    check("half-life: armor 10000 -> 5000 is not below half",
          r["events"] == [], r["events"])
    r = apply_player_damage(0, 5001, sh, 8000, 10000, 10000, side=0)
    check("half-life: 4999 < 5000 -> event -17", r["events"] == [-17],
          r["events"])

    # --- FORCE SHIELD multipliers (ftol trunc, flag 4 skips them) ---------
    r = apply_player_damage(400, 100, sh, 8000, 10000, 10000, side=0,
                            mult_plate=0.5, mult_hull=1.5)
    check("mult 0.5/1.5: plate 200 hull 150",
          (r["plate"], r["hull"]) == (200, 150), (r["plate"], r["hull"]))
    r = apply_player_damage(400, 100, sh, 8000, 10000, 10000, flags=4, side=0,
                            mult_plate=0.5, mult_hull=1.5)
    check("flag 4 (raw): multipliers ignored",
          (r["plate"], r["hull"]) == (400, 100), (r["plate"], r["hull"]))
    r = apply_player_damage(1, 0, sh, 8000, 10000, 10000, side=0,
                            mult_plate=0.5)
    check("ftol truncation: 1*0.5 -> 0 -> hit skipped",
          r["skipped"] is not None, r["skipped"])

    # --- guards -----------------------------------------------------------
    r = apply_player_damage(400, 0, sh, 8000, 10000, 10000, invincible=True)
    check("flags 0x10 -> no damage", r["skipped"] is not None and r["armor_after"] == 10000)
    r = apply_player_damage(400, 0, sh, 8000, 10000, 10000, debug_frame=True)
    check("0x83d620 == 1 -> no damage", r["skipped"] is not None)

    # --- status hits (-1 burn / -2 freeze) --------------------------------
    r = apply_player_damage(-1, 100, sh, 8000, 10000, 10000)
    check("plate -1: flash timer += 100*91/100 = 91",
          r["flash_timer_add"] == 91, r.get("flash_timer_add"))
    r = apply_player_damage(-2, 0, sh, 8000, 10000, 10000)
    check("plate -2: status flags |= 0xc0", r["status_flags_or"] == 0xC0)

    # --- stun formula -----------------------------------------------------
    # pool = 8000+10000 = 18000 -> scale 281.25; 1 dmg -> 1864 -> clamp 0x10000
    r = apply_player_damage(1, 0, sh, 8000, 10000, 10000, side=0)
    check("stun floor 0x10000 for 1 damage", r["stun"] == 0x10000, r["stun"])
    # 400 dmg -> 745653 -> clamp 0x80000
    r = apply_player_damage(400, 0, sh, 8000, 10000, 10000, side=0)
    check("stun ceiling 0x80000 for 400 damage", r["stun"] == 0x80000)
    # small pool: shield_max 500, armor 100 -> scale 9.375; 3 dmg -> 167772
    r = apply_player_damage(3, 0, [500] * 4, 500, 100, 100, side=0)
    check("stun 167772 for pool 600 and 3 damage", r["stun"] == 167772,
          r["stun"])

    # --- thing damage -----------------------------------------------------
    r = apply_thing_damage(400, 0, [500, 500, 500, 500], 800, 800, side=0)
    check("thing front: shield 500 -> 100", r["shields_after"] == [100, 500, 500, 500],
          r["shields_after"])
    check("thing front: hp untouched", r["health_after"] == 800)
    r = apply_thing_damage(600, 0, [500, 0, 0, 0], 800, 800, side=0)
    check("thing overflow: 100 to hp", r["spill"] == 100, r["spill"])
    check("thing overflow: hp 700", r["health_after"] == 700)
    r = apply_thing_damage(2000, 0, [0, 0, 0, 0], 800, 800, side=0)
    check("thing dies, hp negative", r["destroyed"] and r["health_after"] == -1200)
    r = apply_thing_damage(400, 0, [500] * 4, 800, 0)
    check("max_health 0 -> immune", r["skipped"] is not None)
    r = apply_thing_damage(400, 0, [500] * 4, 800, 800, no_plate_damage=True)
    check("flags_b 0x2 -> plate damage zeroed", r["skipped"] is not None)
    # spread: 400/4 = 100 per plate
    r = apply_thing_damage(400, 0, [500] * 4, 800, 800, flags=1)
    check("thing spread: 100 per plate",
          r["shields_after"] == [400, 400, 400, 400], r["shields_after"])
    # 1000/4 = 250; two empty plates -> hull 500
    r = apply_thing_damage(1000, 0, [500, 500, 0, 0], 800, 800, flags=1)
    check("thing spread with empty plates spills 2*250",
          r["spill"] == 500, r["spill"])

    r = apply_thing_damage(500, 0, [0] * 4, 800, 800, side=0, flags_b=0x1000)
    check("thing low-health event needs flags_b & 0x1000",
          r["events"] == [-17], r["events"])
    r = apply_thing_damage(500, 0, [0] * 4, 800, 800, side=0)
    check("thing without 0x1000 posts no -17", r["events"] == [], r["events"])
    r = apply_thing_damage(-1, 100, [0] * 4, 800, 800)
    check("thing burn: +0x388 += 91, flags_a |= 0x40",
          r["flash_timer_add"] == 91 and r["flags_a_or"] == 0x40)

    # --- blast falloff ----------------------------------------------------
    check("falloff full at distance 0", blast_falloff(8000, 500, 0) == 8000)
    check("falloff half at half radius", blast_falloff(8000, 500, 250) == 4000)
    check("falloff 0 at radius", blast_falloff(8000, 500, 500) == 0)
    check("falloff idiv truncation", blast_falloff(100, 3, 1) == 66,
          blast_falloff(100, 3, 1))
    r = apply_player_damage(blast_falloff(8000, 80, 40), 0, sh, 8000, 10000,
                            10000, flags=2)
    check("rocket at half radius (4000): plates 8000 -> 4000",
          r["shields_after"] == [4000] * 4, r["shields_after"])

    # --- hits to kill (LIGHT chassis 8000/10000, MG) ---------------------
    hits, _ = hits_until_dead(400, 0, [8000] * 4, 8000, 10000, 10000, side=0)
    check("MG front: 20 hits break 8000, 25 hull -> 45", hits == 45, hits)
    hits, _ = hits_until_dead(400, 0, [8000] * 4, 8000, 10000, 10000, flags=1)
    check("MG spread: 80 hits break all plates, 25 hull -> 105", hits == 105, hits)

    # --- exe anchors and constants ---------------------------------------
    anchors = check_anchors()
    bad = [a for a in anchors if a["ok"] is False]
    missing = [a for a in anchors if a["ok"] is None]
    if missing:
        checks.append(("exe anchors", True, "skipped (no gundx.exe/cache)"))
    else:
        checks.append(("exe anchors %d/%d" % (len(anchors) - len(bad), len(anchors)),
                       not bad, ",".join("0x%06x" % a["vma"] for a in bad)))
    doubles = read_doubles()
    bad_d = [d for d in doubles if d["ok"] is False]
    if any(d["ok"] is None for d in doubles):
        checks.append(("rdata constants", True, "skipped (no gundx.exe/cache)"))
    else:
        checks.append(("rdata constants %d/%d" % (len(doubles) - len(bad_d),
                                                  len(doubles)),
                       not bad_d,
                       ",".join("0x%06x" % d["vma"] for d in bad_d)))

    # --- shipped data (optional) -----------------------------------------
    dm1 = os.path.join(game, "dm1.sbt")
    if os.path.isfile(dm1):
        lv = corpus_level(dm1)
        by_name = {w["name"]: w for w in lv["weapons"]}
        check("dm1 MACHINE GUN hitscan plate = 400",
              by_name["MACHINE GUN"]["hitscan_damage"] == 400,
              by_name["MACHINE GUN"]["hitscan_damage"])
        check("dm1 CANNON hitscan plate = 1500",
              by_name["CANNON"]["hitscan_damage"] == 1500,
              by_name["CANNON"]["hitscan_damage"])
        check("dm1 RAILGUN hitscan plate = 6000",
              by_name["RAILGUN"]["hitscan_damage"] == 6000,
              by_name["RAILGUN"]["hitscan_damage"])
        check("dm1 ROCKET projectile hp = 800",
              by_name["ROCKET"]["thing"]["hp"] == 800)
        check("dm1 ROCKET projectile shields = 0/500/0/500",
              by_name["ROCKET"]["thing"]["shields"] == [0, 500, 0, 500])
        chassis = {c["name"]: c for c in lv["chassis"]}
        check("dm1 LIGHT: shield_max 8000, armor_max 10000",
              chassis["LIGHT CHASSIS"]["shield_max"] == 8000 and
              chassis["LIGHT CHASSIS"]["armor_max"] == 10000,
              chassis.get("LIGHT CHASSIS"))
    else:
        checks.append(("shipped dm1", True, "skipped (no game install)"))

    # whole corpus: every shipped .sbt must parse into a weapon table
    if os.path.isdir(game):
        paths = level_paths(game, None, True)
        bad = []
        for path in paths:
            try:
                if not corpus_level(path)["weapons"]:
                    bad.append(os.path.basename(path))
            except Exception as exc:                      # noqa: BLE001
                bad.append("%s: %s" % (os.path.basename(path), exc))
        check("corpus: %d levels parse with a weapon table" % len(paths),
              bool(paths) and not bad, bad[:3])
        check("corpus: 36 shipped .sbt", len(paths) == 36, len(paths))
    else:
        checks.append(("corpus --all", True, "skipped (no game install)"))

    width = max(len(n) for n, _, _ in checks)
    passed = 0
    for name, ok, extra in checks:
        print("%s %-*s %s" % ("OK  " if ok else "FAIL", width, name,
                              "" if ok else extra))
        passed += 1 if ok else 0
    print("%d/%d" % (passed, len(checks)))
    return 0 if passed == len(checks) else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--self-test", action="store_true", help="model tests")
    ap.add_argument("--fields", action="store_true", help="field/address map")
    ap.add_argument("--anchors", action="store_true", help="bytes in gundx.exe")
    ap.add_argument("--calc", action="store_true", help="one damage calculation")
    ap.add_argument("--corpus", action="store_true", help="numbers from .sbt")
    ap.add_argument("--json", action="store_true", help="machine-readable")
    ap.add_argument("--levels", default=None,
                    help="--corpus: comma list (default dm1,level0)")
    ap.add_argument("--all", action="store_true",
                    help="--corpus: every .sbt in the game directory")

    g = ap.add_argument_group("calc")
    g.add_argument("--shields", default=None,
                   help="current plates: one value or front,right,back,left")
    g.add_argument("--shield-max", type=int, default=8000)
    g.add_argument("--armor", type=int, default=10000)
    g.add_argument("--armor-max", type=int, default=10000)
    g.add_argument("--plate", type=int, default=400, help="shield gauge")
    g.add_argument("--hull", type=int, default=0, help="hull gauge")
    g.add_argument("--flags", type=lambda s: int(s, 0), default=0,
                   help="arg2: 1 no direction, 2 full plate, 4 raw")
    g.add_argument("--side", type=int, default=-1,
                   help="-1 spread, 0 front, 1 right, 2 back, 3 left")
    g.add_argument("--mult-plate", type=float, default=1.0)
    g.add_argument("--mult-hull", type=float, default=1.0)
    g.add_argument("--repeat", type=int, default=1,
                   help="apply the hit N times (0 = until death)")
    g.add_argument("--invincible", action="store_true")
    g.add_argument("--debug-frame", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test(args.game)
    if args.fields:
        print_fields(args.json)
        return 0
    if args.anchors:
        bad = 0
        for a in check_anchors():
            state = "?" if a["ok"] is None else ("OK " if a["ok"] else "BAD")
            print("%s 0x%06x  %-44s %s" % (state, a["vma"], a["what"], a["want"]))
            if a["ok"] is False:
                print("     got: %s" % a.get("got", ""))
                bad += 1
        return 1 if bad else 0
    if args.corpus:
        return run_corpus(args)
    if args.calc:
        return run_calc(args)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
