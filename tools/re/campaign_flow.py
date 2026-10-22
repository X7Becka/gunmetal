#!/usr/bin/env python3
"""Campaign flow: mission counter, objectives, secrets, best time, finale.

Complements ``tools/re/campaign.py`` (which only prints the transition map) with
the *state* the engine keeps between levels:

* the mission counter ``ds:0x80f9cc`` -- who writes it, what it means
  (``docs/FORMATS.md`` 15, ``docs/SAVE_FIELDS.md`` F2);
* the per-level objectives (``DEFINE_LEVEL_OBJECTIVES`` / ``OR_LEVEL_OBJECTIVES_BITS``,
  ``docs/VM_EXEC.md`` 3) and the completion test ``LEVEL_OBJECTIVES_COMPLETE``;
* the secret tally (``INCREMENT_SOURCEPLAYER_SECRET_TALLY``) and the declared
  secret count (``SET_LEVEL_SECRETS``);
* best times: the script's par value (``SET_LEVEL_BEST_TIME``) and the
  ``GUNMETAL.HI`` record file (insert/scan ``0x45c880``);
* the end-of-level event chain: entity ``-8`` (``SYSTEM_LEVEL_FINISHED``) and
  entity ``305`` (mission exit / HQ launch), ``SHOW_LEVEL_END_SCREEN``
  (``0x45c057``) and the engine post at ``0x4454a2``;
* the finale/credits path (``Vid/winner.sbv`` + ``SML/CREDITS.SML``).

Everything is read-only.  The level map is taken from ``tools/re/campaign.py``
(imported as a module) so both tools agree on names/objectives/exits.

Usage::

    python3 tools/re/campaign_flow.py --self-test
    python3 tools/re/campaign_flow.py --summary
    python3 tools/re/campaign_flow.py --summary --game ~/.cache/gunmetal-re/game
    python3 tools/re/campaign_flow.py --json
    python3 tools/re/campaign_flow.py --fields
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import campaign as camp                      # noqa: E402  (same directory)
from gm import sbt as sbtmod                 # noqa: E402
from gm import sbtvm                         # noqa: E402
from gm import scene as scenemod             # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")

#: Headquarters stems; every mission returns to one of these (docs/FORMATS.md 15).
HQ = ("level0", "level00", "level000", "level0a", "level0b")
#: Secret maps, entered from a normal mission (docs/FORMATS.md 15).
SECRET = ("secrt1", "secrt2", "temple")
DEATHMATCH = ("dm1", "dm2", "dm3", "dm4")

MISSION_COUNT = 24        # level1..level24 (docs/FORMATS.md 15)
COUNTER = 0x80F9CC        # ds:0x80f9cc -- next mission number

#: ``GUNMETAL.HI`` record: i32 seconds + char[60] display name (0x45c880).
HI_RECORD = 0x40
HI_NAME = 0x3C

# ---------------------------------------------------------------------------
# field map (addresses are documented in the doc; each row is printed by
# --fields and embedded in --json).  status: proven | hypothesis | refuted.
# ---------------------------------------------------------------------------
FIELDS = [
    # --- mission counter / level selection --------------------------------
    dict(key="mission_counter", what="next mission number", addr="0x80f9cc",
         status="proven",
         writer="0x442a0e/0x442a4c/0x442cf7 =1 (new game), 0x457b2e += 1 (NEXT_LEVEL), "
                "0x44295e -= 1 (restart from HQ) + clamp 0x442973",
         reader="0x457b17 sprintf LEVEL%ld; 0x45ea26 NEXT_LEVEL_NUMBER; "
                "0x454f19 save / 0x45586e load; 0x48f0f3/0x48f116 briefing counter-1",
         evidence="docs/FORMATS.md 15; docs/SAVE_FIELDS.md F2; docs/LEVEL_LOAD.md 2"),
    dict(key="pending_level_stem", what="next level stem buffer char[0x50]", addr="0x5a47c8",
         status="proven",
         writer="0x457afd strncpy (NEW_LEVEL), 0x457b21 sprintf (NEXT_LEVEL), "
                "0x44298d/0x4429be copy from 0x5a48b8/0x5a4868",
         reader="0x442ef0 copies to current stem 0x5a4728 and loads the level",
         evidence="docs/LEVEL_LOAD.md 1-3"),
    dict(key="current_level_stem", what="current level stem", addr="0x5a4728",
         status="proven", writer="0x442ef0 (load_level S0)",
         reader="0x442ef0 unload_world / error text; 0x466c47 save-name compare",
         evidence="docs/LEVEL_LOAD.md 3 (S0)"),
    dict(key="frame_submode", what="frame submode; 5 = level finished/end screen",
         addr="0x83cb7c", status="proven",
         writer="0x45c066 = 5 (SHOW_LEVEL_END_SCREEN), 0x442ebc = 0x83d300 on load",
         reader="0x445445 (posts SYSTEM_LEVEL_FINISHED); 0x44c2** frame switch",
         evidence="docs/MAINLOOP.md 4.2"),
    dict(key="load_status", what="submode to enter after load (2 or 4)", addr="0x83d300",
         status="proven", writer="0x4428f0: (-(bit1) & -2) + 4",
         reader="0x442ebc -> 0x83cb7c; 0x4435c2",
         evidence="docs/LEVEL_LOAD.md 3 (S16)"),
    # --- objectives -------------------------------------------------------
    dict(key="objectives_count", what="DEFINE_LEVEL_OBJECTIVES count (<= 30)", addr="0x80d7ec",
         status="proven", writer="0x45be04 (cap 0x45bdf6 cmp 0x1e), reset 0x45709d",
         reader="0x45e9f0 (complete test), 0x4869c0 (objective list), 0x487338",
         evidence="docs/VM_EXEC.md 3; docs/FORMATS.md 12.3"),
    dict(key="objectives_text", what="array of objective string pointers", addr="0x80d800",
         status="proven", writer="0x45be11/0x45be21 (slot = code cursor), step +4 at 0x45be34",
         reader="0x4869c0 via 0x4833e0 (draw text)", evidence="docs/VM_EXEC.md 3"),
    dict(key="objectives_bits", what="completed-objective bit mask", addr="0x80f9d0",
         status="proven", writer="0x45be4e OR (opcode 104), reset 0x4570a3",
         reader="0x45ea00 (LEVEL_OBJECTIVES_COMPLETE), 0x487358 (draw done bits), "
                "0x462f4e (save writer emits OR_LEVEL_OBJECTIVES_BITS)",
         evidence="docs/VM_EXEC.md 3; save block 11 (see docs/CAMPAIGN_FLOW.md)"),
    dict(key="objectives_complete", what="condition LEVEL_OBJECTIVES_COMPLETE (cid 7)",
         addr="0x45e9f0", status="proven",
         writer="-", reader="all bits 1<<i for i<count must be set; count<=0 -> false",
         evidence="0x45e9f0..0x45ea25 (docs/VM_EXEC.md 4)"),
    # --- secrets ----------------------------------------------------------
    dict(key="secrets_declared", what="SET_LEVEL_SECRETS value (secrets in level)",
         addr="0x80d8c0", status="proven", writer="0x45b81e (opcode 94), reset 0x45708a",
         reader="0x483c07 end screen 'SECRETS FOUND: found / declared'; 0x499534",
         evidence="docs/FORMATS.md 15; docs/SAVE_FIELDS.md (end screen)"),
    dict(key="secret_tally", what="secrets found in this level (per player)",
         addr="0x81c40c (player+0x33cc)", status="proven",
         writer="0x4536b0 via 0x45b978 (INCREMENT_SOURCEPLAYER_SECRET_TALLY), "
                "reset to 0 at level load 0x45280f (inside 0x452730)",
         reader="0x483c07, 0x499534 (labels 'SECRETS FOUND')",
         evidence="0x4536b0; docs/PLAYER_FIELDS.md +0x33cc named damage_accum (wrong)"),
    # --- best time --------------------------------------------------------
    dict(key="best_time_par", what="SET_LEVEL_BEST_TIME (script par, seconds)",
         addr="0x80d88c", status="proven", writer="0x45b831 (opcode 93), reset 0x457080",
         reader="0x483caf end screen 'BEST TIME:' (fmt 0x483f00)",
         evidence="docs/FORMATS.md 15; docs/VM_SCENE.md 5.5"),
    dict(key="level_clock", what="level clock, 91 units per second", addr="0x5a46e4",
         status="proven", writer="0x44328e (reset), tick 0x44b6d2 add",
         reader="0x45c057 divides by 91 -> 0x83d638; 0x454efa save",
         evidence="docs/SAVE_FIELDS.md F1/F20"),
    dict(key="last_time", what="this run's time in seconds", addr="0x83d638",
         status="proven", writer="0x45c080/0x460565 (clock/91)", reader="0x483c7c 'TIME:'",
         evidence="0x45c057..0x45c08e"),
    dict(key="your_best", what="best recorded time for this level name", addr="0x83d2c0",
         status="proven", writer="0x45c08e/0x460571 = return of 0x45c880",
         reader="0x483ce2 end screen 'YOUR BEST:'", evidence="0x45c880"),
    dict(key="hi_file", what="GUNMETAL.HI: records of i32 seconds + char[60] name",
         addr="0x493630", status="proven",
         writer="0x45c880: scan 0x40-byte records (0x40dc70), strcasecmp 0x416850, "
                "append in 'ab' if better (0x40ddf0)",
         reader="0x45c880 only (called 0x45c086 end screen, 0x460569 cheat)",
         evidence="0x45c880 + file layout; docs/CAMPAIGN_FLOW.md 4"),
    # --- level finish / transition ---------------------------------------
    dict(key="system_level_finished", what="entity id -8 in every mission script",
         addr="entity table id -8", status="proven",
         writer="engine event_post at 0x4454a2 (id 0xfffffff8) when 0x83cb7c==5 and "
                "player+0x3344 bit 0x8000 (0x445450, cleared 0x44545f)",
         reader="script sub-script of entity -8: NEW_LEVEL / NEXT_LEVEL",
         evidence="docs/EVENTS.md 4.1 (-8); 0x445445..0x4454a4"),
    dict(key="level_exit_entity", what="entity id 305: mission exit / HQ launch",
         addr="entity table id 305", status="proven",
         writer="mission: SHOW_LEVEL_END_SCREEN under IF SOURCE_IS_PLAYER; "
                "HQ: NEXT_LEVEL under IF SOURCE_IS_PLAYER",
         reader="0x45c057 (end screen) / 0x457b11 (NEXT_LEVEL)",
         evidence="level0 0xc86e, level1 0xd15c, level19 0xf584, level24 0xf891"),
    dict(key="end_screen", what="SHOW_LEVEL_END_SCREEN (opcode 91)", addr="0x45c057",
         status="proven",
         writer="script", reader="sets 0x83cb7c=5, 0x83d638=clock/91, 0x83d2c0=best",
         evidence="0x45c057..0x45c08e; docs/FORMATS.md 15"),
    dict(key="new_level", what="NEW_LEVEL \"stem\" (opcode 61)", addr="0x457af3",
         status="proven", writer="script",
         reader="strncpy stem to 0x5a47c8 (<=0x4f), buffer cleared at 0x5a4817",
         evidence="docs/FORMATS.md 15"),
    dict(key="next_level", what="NEXT_LEVEL, no args (opcode 62)", addr="0x457b11",
         status="proven", writer="script",
         reader="sprintf(0x5a47c8, 'LEVEL%ld', 0x80f9cc) then 0x80f9cc += 1",
         evidence="0x457b11..0x457b34; docs/FORMATS.md 15"),
    dict(key="next_level_number", what="condition NEXT_LEVEL_NUMBER n (cid 8)",
         addr="0x45ea26", status="proven", writer="-",
         reader="returns 0x80f9cc == n", evidence="0x45ea26..0x45ea4a"),
    dict(key="end_game", what="END_GAME (opcode 63)", addr="0x45758f", status="proven",
         writer="script (0 sites in the shipped 36 levels)", reader="0x44b830 + exit flag",
         evidence="docs/FORMATS.md 15; docs/VM_SCENE.md 4"),
    # --- HQ / shop --------------------------------------------------------
    dict(key="shop_ammo", what="SHOW_AMMO_BUY_SCREEN (189)", addr="0x45c0ae",
         status="proven", writer="HQ script", reader="buy screen", evidence="docs/EQUIPMENT.md 5"),
    dict(key="shop_item", what="SHOW_ITEM_BUY_SCREEN (190)", addr="0x45c109",
         status="proven", writer="HQ script", reader="buy screen", evidence="docs/EQUIPMENT.md 5"),
    dict(key="shop_tank_buy", what="SHOW_TANK_BUY_SCREEN (191)", addr="0x45c157",
         status="proven", writer="HQ script", reader="buy screen", evidence="docs/EQUIPMENT.md 5"),
    dict(key="shop_tank_save", what="SHOW_TANK_SAVE_SCREEN (192)", addr="0x45c1a5",
         status="proven", writer="HQ script", reader="buy screen", evidence="docs/EQUIPMENT.md 5"),
    dict(key="shop_weapon", what="SHOW_WEAPON_BUY_SCREEN (193)", addr="0x45c1f0",
         status="proven", writer="HQ script", reader="buy screen", evidence="docs/EQUIPMENT.md 5"),
    dict(key="define_equipment", what="DEFINE_EQUIPMENT (175, 'SLEEEEEELLLLL')",
         addr="0x45c321", status="proven", writer="HQ script",
         reader="armory/equipment tick", evidence="docs/EQUIPMENT.md 2"),
    dict(key="hq_secrets_screen", what="'SECRETS FOUND' + '%ld/%ld' text",
         addr="0x499534", status="proven",
         writer="draw code", reader="-",
         evidence="0x499534/0x49954f; strings 0x4beb10, 0x4be7d8"),
    # --- briefing / finale ------------------------------------------------
    dict(key="briefing_path", what="SML\\BRIEF\\LEVEL%ld.SML", addr="0x48f103/0x48f128",
         status="proven", writer="0x48f0f3 uses counter, 0x48f116/0x48f13e use counter-1",
         reader="SML pager 0x497310", evidence="docs/UI.md 6.3"),
    dict(key="credits_entry", what="frontend action index 3 -> credits", addr="0x4971e0",
         status="proven", writer="SML action dispatcher",
         reader="case 3 at 0x4972c1 calls 0x483470; guarded by 0x83d940 == 0 (0x497264)",
         evidence="0x4971e0..0x4972cc jump table"),
    dict(key="credits", what="finale: winner video + credits scroll", addr="0x483470",
         status="proven", writer="-",
         reader="0x4913a0('winner') -> Vid/winner.sbv (0x49e1e8, ext 0x4bee28); "
                "0x4bc440=1; 0x44b830; 0x497310('SML\\CREDITS.SML'); 0x402770(0x22600)",
         evidence="strings 0x4bcb08, 0x4bd228, 0x4bcaac; 0x4913a0..0x4914xx"),
    dict(key="game_type_flag", what="0x83d940 = (game type arg 0x83d63c == 2)",
         addr="0x83d940", status="proven", writer="0x442e73..0x442e9e",
         reader="0x497264 gates the credits menu item",
         evidence="docs/SAVE_FIELDS.md F17"),
    dict(key="finale_music", what="FINALE/OVERTURE/WINNER are SOU2 samples, not scripts",
         addr="sounds/SOU.IDX", status="proven",
         writer="-", reader="no script/code reference to FINALE/OVERTURE found",
         evidence="docs/SOUND_FIELDS.md (SOU2 list); docs/SOU_CONTAINER.md"),
]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def insn_args(ins):
    return [a[1] if isinstance(a, tuple) else a for a in (ins.args or [])]


def subscript(insns, start):
    """Top-level instructions of the sub-script that begins at ``start``.

    Entity rows point at the first instruction of an ``END``-terminated block
    (docs/FORMATS.md 12.3).  Nested ``IF`` bodies are not in the top-level list
    but ``campaign._walk`` recurses into them, so the slice is enough.
    """
    idx = None
    for i, ins in enumerate(insns):
        if ins.off == start:
            idx = i
            break
    if idx is None:
        for i, ins in enumerate(insns):
            if ins.off >= start:
                idx = i
                break
    if idx is None:
        return []
    out = []
    for ins in insns[idx:]:
        out.append(ins)
        if ins.name == "END":
            break
    return out


def entity_rows(payload):
    """``[(id, flags, offset)]`` sorted by code offset."""
    return sorted(scenemod.entities(payload), key=lambda e: e[2])


def owner_of(rows, off):
    """Entity whose sub-script contains ``off`` (last row starting at/before it)."""
    best = None
    for row in rows:
        if row[2] <= off:
            best = row
        else:
            break
    return best


def classified_exits(insns, rows):
    """Every NEW_LEVEL/NEXT_LEVEL site with its owner entity and conditions."""
    out = []
    for ins, conds in camp._walk(insns):
        if ins.name not in ("NEW_LEVEL", "NEXT_LEVEL"):
            continue
        args = insn_args(ins)
        target = None
        if ins.name == "NEW_LEVEL":
            target = str(args[0]).lower() if args else None
        owner = owner_of(rows, ins.off)
        out.append({
            "kind": ins.name,
            "target": target,
            "conditions": list(conds),
            "offset": ins.off,
            "owner_id": owner[0] if owner else None,
            "deathmatch": any(c.upper().startswith("DEATHMATCH") for c in conds),
        })
    return out


def s32(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def generated_ids(body):
    """Signed entity ids of GENERATE commands (operands are raw u32)."""
    return [s32(int(insn_args(i)[0])) for i in body
            if i.name in ("GENERATE", "GENERATE_FOR_PARENT") and insn_args(i)]


def reachable_exits(insns, rows, start, depth=3, seen=None):
    """Exits of a sub-script plus those of the entity sub-scripts it GENERATEs.

    ``level4``'s ``-8`` block is just ``GENERATE -30246; GENERATE -30247; END``
    (compiled from two named events), so the transition lives one level down.
    ``GENERATE`` runs every row that carries the id (docs/VM_EXEC.md 3), hence
    the loop over all matching rows.
    """
    seen = set() if seen is None else seen
    body = subscript(insns, start)
    out = classified_exits(body, rows)
    if depth <= 0:
        return out
    by_id = {}
    for row in rows:
        by_id.setdefault(row[0], []).append(row)
    for gid in generated_ids(body):
        for row in by_id.get(gid, []):
            if row[2] in seen:
                continue
            seen.add(row[2])
            out.extend(reachable_exits(insns, rows, row[2], depth - 1, seen))
    return out


def _primary_objectives(insns):
    """First unconditional DEFINE_LEVEL_OBJECTIVES list + all variants."""
    variants = []
    for ins, conds in camp._walk(insns):
        if ins.name == "DEFINE_LEVEL_OBJECTIVES":
            variants.append({"texts": [str(t) for t in insn_args(ins)],
                             "conditions": list(conds)})
    primary = next((v for v in variants if not v["conditions"]),
                   variants[0] if variants else None)
    return primary, variants


def _or_bits(insns):
    bits = []
    for ins, _conds in camp._walk(insns):
        if ins.name == "OR_LEVEL_OBJECTIVES_BITS":
            args = insn_args(ins)
            if args:
                bits.append(int(args[0]) & 0xFFFFFFFF)
    return bits


def bits_to_list(mask, count):
    """``[bool]`` of the ``count`` objective bits -- mirrors 0x45e9f0."""
    return [bool(mask & (1 << i)) for i in range(max(0, count))]


def objectives_mask_complete(mask, count):
    """Same rule as ``LEVEL_OBJECTIVES_COMPLETE`` (0x45e9f0): count<=0 -> false."""
    if count <= 0:
        return False
    return all(mask & (1 << i) for i in range(count))


def next_level_stem(counter):
    """Name NEXT_LEVEL builds at 0x457b17: ``sprintf(buf, "LEVEL%ld", counter)``."""
    return "LEVEL%d" % counter


def role_of(stem):
    if stem in HQ:
        return "hq"
    if stem in DEATHMATCH:
        return "deathmatch"
    if stem in SECRET:
        return "secret"
    if stem.startswith("level") and stem[5:].isdigit():
        return "mission"
    return "other"


def mission_number(stem):
    if stem.startswith("level") and stem[5:].isdigit():
        n = int(stem[5:])
        return n if 1 <= n <= MISSION_COUNT else None
    return None


def command_counts(insns, names):
    want = set(names)
    counts = {n: 0 for n in names}
    for ins, _conds in camp._walk(insns):
        if ins.name in want:
            counts[ins.name] += 1
    return counts


# ---------------------------------------------------------------------------
# GUNMETAL.HI  (docs/CAMPAIGN_FLOW.md 4)
# ---------------------------------------------------------------------------
def parse_hi(data):
    """Decode ``i32 seconds + char[60]`` records; reject a partial tail."""
    if len(data) % HI_RECORD:
        raise ValueError("GUNMETAL.HI size %d is not a multiple of %d"
                         % (len(data), HI_RECORD))
    out = []
    for off in range(0, len(data), HI_RECORD):
        secs = struct.unpack_from("<i", data, off)[0]
        raw = data[off + 4:off + HI_RECORD].split(b"\0", 1)[0]
        out.append({"seconds": secs, "name": raw.decode("latin1")})
    return out


def encode_hi(records):
    out = bytearray()
    for rec in records:
        name = rec["name"].encode("latin1")
        if len(name) > HI_NAME:
            raise ValueError("HI name longer than %d bytes" % HI_NAME)
        out += struct.pack("<i", int(rec["seconds"]))
        out += name + b"\0" * (HI_NAME - len(name))
    return bytes(out)


def best_by_name(records):
    """Minimum recorded time per level display name (scan in 0x45c880)."""
    out = {}
    for rec in records:
        key = rec["name"]
        if key not in out or rec["seconds"] < out[key]:
            out[key] = rec["seconds"]
    return out


# ---------------------------------------------------------------------------
# per-level facts
# ---------------------------------------------------------------------------
def level_flow(stem, path, stems):
    info = camp.level(stem, path, stems)          # name/secrets/best/exits (campaign.py)
    sbt = sbtmod.parse_file(path)
    recs = sbt.records_of(11)
    flow = {
        "stem": stem,
        "role": role_of(stem),
        "mission": mission_number(stem),
        "name": info["name"],
        "secrets_declared": info["secrets"],
        "best_time_par": info["best_time"],
        "objectives": None,
        "objective_variants": [],
        "objectives_bits": [],
        "objectives_union": 0,
        "objectives_complete_sites": 0,
        "secret_tally_sites": 0,
        "end_screens": 0,
        "system_finished": None,
        "exit_entity": None,
        "exits": [],
        "shop": {},
    }
    if not recs:
        return flow
    payload = recs[0].payload
    code = payload.code
    insns = sbtvm.decode(code)[0]
    rows = entity_rows(payload)

    primary, variants = _primary_objectives(insns)
    flow["objectives"] = primary["texts"] if primary else None
    flow["objective_variants"] = variants
    bits = _or_bits(insns)
    flow["objectives_bits"] = bits
    union = 0
    for b in bits:
        union |= b
    flow["objectives_union"] = union
    flow["objectives_complete_sites"] = sum(
        1 for ins, _c in camp._walk(insns)
        if ins.name == "IF" and ins.args and
        getattr(ins.args[0], "__getitem__", None) and ins.args[0][0] == "LEVEL_OBJECTIVES_COMPLETE")
    flow["secret_tally_sites"] = sum(
        1 for ins, _c in camp._walk(insns)
        if ins.name == "INCREMENT_SOURCEPLAYER_SECRET_TALLY")
    flow["end_screens"] = sum(
        1 for ins, _c in camp._walk(insns) if ins.name == "SHOW_LEVEL_END_SCREEN")

    # entity -8 = SYSTEM_LEVEL_FINISHED and entity 305 = mission exit / HQ launch
    fin_rows = [row for row in rows if row[0] == -8]
    if fin_rows:
        exits, gens = [], []
        for row in fin_rows:
            exits.extend(reachable_exits(insns, rows, row[2]))
            gens.extend(generated_ids(subscript(insns, row[2])))
        flow["system_finished"] = {
            "offsets": [row[2] for row in fin_rows],
            "offset": fin_rows[0][2],
            "generates": gens,
            "exits": exits,
        }
    for row in rows:
        if row[0] == 305:
            body = subscript(insns, row[2])
            ex = classified_exits(body, rows)
            flow["exit_entity"] = {
                "offset": row[2],
                "commands": sorted({i.name for i in body
                                    if i.name in ("NEXT_LEVEL", "NEW_LEVEL",
                                                  "SHOW_LEVEL_END_SCREEN")}),
                "exits": ex,
            }

    flow["exits"] = classified_exits(insns, rows)

    if flow["role"] == "hq":
        flow["shop"] = command_counts(insns, (
            "SHOW_AMMO_BUY_SCREEN", "SHOW_ITEM_BUY_SCREEN", "SHOW_TANK_BUY_SCREEN",
            "SHOW_TANK_SAVE_SCREEN", "SHOW_WEAPON_BUY_SCREEN", "SHOW_BRIEFING_SCREEN",
            "ADD_SWITCH", "ADD_DOOR", "ADD_ITEM_FIXED",
            "DEFINE_WEAPONS", "DEFINE_AMMO", "DEFINE_EQUIPMENT", "DEFINE_SHIELDS"))
    return flow


def completion_exits(flow):
    """Exits that run when the mission is finished: prefer the -8 sub-script."""
    fin = flow.get("system_finished")
    if fin and fin["exits"]:
        return fin["exits"]
    return [e for e in flow["exits"] if e["owner_id"] in (None, -8)]


def completion_campaign_exits(flow):
    """Non-deathmatch exits of the mission-complete sub-script."""
    return [e for e in completion_exits(flow) if not e["deathmatch"]]


def hq_exits(exits):
    return [e for e in exits if e["target"] in HQ]


def secret_exits(exits):
    return [e for e in exits if e["target"] in SECRET]


# ---------------------------------------------------------------------------
# campaign chain: the counter is the only driver
# ---------------------------------------------------------------------------
def build_chain(mapinfo):
    """Derive the mission order from the counter semantics.

    ``mapinfo`` is ``{stem: flow-dict}``.  For mission *N* (file ``levelN``)
    the counter equals *N* when the HQ runs ``NEXT_LEVEL`` (it was incremented
    when the mission was loaded) and *N+1* while the mission is played.  So:

    * HQ ``NEXT_LEVEL`` loads ``LEVEL<counter>`` and increments it;
    * a mission returns to an HQ by name (counter untouched) or, like
      ``level19``, calls ``NEXT_LEVEL`` itself (loads the next mission).

    Returns ``{"steps": [...], "problems": [...], "missions": N}``.
    """
    problems = []
    steps = [{"kind": "start", "stem": "level0", "counter": 1,
              "note": "frontend new game: name LEVEL0, counter 1 (0x442a0e)"}]
    seen = []
    for m in range(1, MISSION_COUNT + 1):
        stem = "level%d" % m
        flow = mapinfo.get(stem)
        if flow is None:
            problems.append("mission %d: %s.sbt not found" % (m, stem))
            continue
        seen.append(stem)
        exits = completion_campaign_exits(flow)
        hqs = hq_exits(exits)
        nexts = [e for e in exits if e["kind"] == "NEXT_LEVEL"]
        secr = secret_exits(exits)
        step = {
            "kind": "mission",
            "mission": m,
            "stem": stem,
            "counter": m + 1,
            "name": flow["name"],
            "returns_to": [e["target"] for e in hqs],
            "next_level": bool(nexts),
            "secret_branches": [e["target"] for e in secr],
        }
        steps.append(step)
        if m == MISSION_COUNT:
            if hqs or nexts:
                problems.append("level24 has a campaign exit (expected none)")
            continue
        if nexts and not hqs:
            # the mission itself advances the counter (level19)
            nxt = "level%d" % (m + 1)
            steps.append({"kind": "direct", "stem": nxt, "counter": m + 2,
                          "note": "mission called NEXT_LEVEL itself"})
        elif hqs:
            for e in hqs:
                steps.append({"kind": "hq", "stem": e["target"], "counter": m + 1,
                              "loads": next_level_stem(m + 1).lower()})
            if len({e["target"] for e in hqs}) > 1:
                step["note"] = "several HQs accepted; the counter still picks LEVEL%d" % (m + 1)
        else:
            problems.append("mission %d: no campaign exit to an HQ or NEXT_LEVEL" % m)
    missing = [n for n in range(1, MISSION_COUNT + 1) if "level%d" % n not in seen]
    if missing:
        problems.append("missing missions: %s" % missing)
    if len(seen) != len(set(seen)):
        problems.append("duplicate mission rows")
    return {"steps": steps, "problems": problems, "missions": len(seen)}


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------
def flow_map(game):
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        raise SystemExit("no .sbt files in %s" % game)
    stems = {os.path.splitext(os.path.basename(p))[0].lower():
             os.path.splitext(os.path.basename(p))[0] for p in files}
    out = {}
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        out[stem] = level_flow(stem, path, stems)
    return out


def read_hi(game, path=None):
    path = path or os.path.join(game, "GUNMETAL.HI")
    if not os.path.exists(path):
        return {"path": path, "exists": False, "records": [], "best": {}}
    with open(path, "rb") as fh:
        records = parse_hi(fh.read())
    return {"path": path, "exists": True, "records": records,
            "best": best_by_name(records)}


def summary_rows(mapinfo, hi):
    rows = []
    best = hi.get("best", {})
    for stem in sorted(mapinfo, key=lambda s: (0 if s.startswith("level") else
                                               1 if s.startswith("dm") else 2, s)):
        f = mapinfo[stem]
        exits = []
        for e in f["exits"]:
            label = e["target"] or "NEXT_LEVEL"
            if e["conditions"]:
                label += " [%s]" % ",".join(e["conditions"])
            exits.append(label)
        rows.append({
            "stem": stem,
            "role": f["role"],
            "name": f["name"],
            "objectives": len(f["objectives"] or []),
            "objective_variants": len(f["objective_variants"]),
            "bits": f["objectives_union"],
            "secret_tally": f["secret_tally_sites"],
            "secrets": f["secrets_declared"],
            "best": f["best_time_par"],
            "your_best": best.get(f["name"]) if f["name"] else None,
            "end_screens": f["end_screens"],
            "fin": "-" if f["system_finished"] is None else
                   "%#x%s%s" % (f["system_finished"]["offset"],
                                "x%d" % len(f["system_finished"]["offsets"])
                                if len(f["system_finished"]["offsets"]) > 1 else "",
                                " +%dgen" % len(f["system_finished"]["generates"])
                                if f["system_finished"]["generates"] else ""),
            "exit_entity": "-" if f["exit_entity"] is None else
                           "%#x" % f["exit_entity"]["offset"],
            "exits": "; ".join(exits) or "-",
        })
    return rows


def print_summary(mapinfo, hi, chain):
    rows = summary_rows(mapinfo, hi)
    print("%-9s %-6s %-27s %4s %6s %5s %5s %5s %7s %9s  %s" % (
        "level", "role", "name", "obj", "bits", "tly", "secr", "par",
        "yourbest", "fin/exit", "exits"))
    for r in rows:
        obj = str(r["objectives"])
        if r["objective_variants"] > 1:
            obj += "(%d)" % r["objective_variants"]
        print("%-9s %-6s %-27s %4s %6s %5d %5s %5s %7s %9s  %s" % (
            r["stem"], r["role"], (r["name"] or "?")[:27], obj,
            "%#x" % r["bits"] if r["bits"] else "-", r["secret_tally"],
            r["secrets"] if r["secrets"] is not None else "-",
            r["best"] if r["best"] is not None else "-",
            r["your_best"] if r["your_best"] is not None else "-",
            "%s/%s" % (r["fin"], r["exit_entity"]), r["exits"]))

    hq = [r for r in rows if r["role"] == "hq"]
    if hq:
        print("\nheadquarters (shop screens: ammo/item/tank buy/tank save/weapon):")
        for r in hq:
            f = mapinfo[r["stem"]]
            shop = f["shop"]
            print("  %-8s %s = %d/%d/%d/%d/%d  DEFINE weapons/ammo/equipment = %d/%d/%d"
                  "  switches/doors = %d/%d  briefing = %d"
                  % (r["stem"], (r["name"] or "")[:20],
                     shop.get("SHOW_AMMO_BUY_SCREEN", 0),
                     shop.get("SHOW_ITEM_BUY_SCREEN", 0),
                     shop.get("SHOW_TANK_BUY_SCREEN", 0),
                     shop.get("SHOW_TANK_SAVE_SCREEN", 0),
                     shop.get("SHOW_WEAPON_BUY_SCREEN", 0),
                     shop.get("DEFINE_WEAPONS", 0),
                     shop.get("DEFINE_AMMO", 0),
                     shop.get("DEFINE_EQUIPMENT", 0),
                     shop.get("ADD_SWITCH", 0),
                     shop.get("ADD_DOOR", 0),
                     shop.get("SHOW_BRIEFING_SCREEN", 0)))

    print("\nmission chain (counter ds:0x80f9cc drives it; every HQ NEXT_LEVEL "
          "loads LEVEL<counter>):")
    for st in chain["steps"]:
        if st["kind"] == "start":
            print("  start %-8s counter %d  (%s)" % (st["stem"], st["counter"], st["note"]))
        elif st["kind"] == "mission":
            ret = ",".join(st["returns_to"]) or ("NEXT_LEVEL" if st["next_level"] else "-")
            print("  #%-2d %-8s counter %-2d  -> %s" % (
                st["mission"], st["stem"], st["counter"], ret))
        elif st["kind"] == "hq":
            print("       HQ %-8s counter %-2d  NEXT_LEVEL -> %s" % (
                st["stem"], st["counter"], st["loads"]))
        else:
            print("       %-8s counter %-2d  (%s)" % (st["stem"], st["counter"],
                                                      st.get("note", "")))
    if chain["problems"]:
        print("  PROBLEMS: %s" % "; ".join(chain["problems"]))
    else:
        print("  ok: %d missions, one counter step each, no gaps" % chain["missions"])

    print("\nGUNMETAL.HI: %s" % ("missing" if not hi.get("exists") else
                                 "%d records" % len(hi["records"])))
    if hi.get("records"):
        for rec in hi["records"]:
            print("  %5ds  %s" % (rec["seconds"], rec["name"]))


def print_fields():
    print("%-22s %-34s %-9s %s" % ("field", "address", "status", "what"))
    for f in FIELDS:
        print("%-22s %-34s %-9s %s" % (f["key"], f["addr"], f["status"], f["what"]))
    print("\nwriters / readers:")
    for f in FIELDS:
        print("\n%s (%s)\n  writer: %s\n  reader: %s\n  evidence: %s"
              % (f["key"], f["addr"], f["writer"], f["reader"], f["evidence"]))


# ---------------------------------------------------------------------------
# self-test (no game files needed)
# ---------------------------------------------------------------------------
class FakeInsn:
    def __init__(self, name, args=(), off=0, body=None):
        self.name = name
        self.args = list(args)
        self.off = off
        self.body = body


def _synthetic_level(stem, *, name, objectives, exits, hq=False):
    """Minimal flow dict with the same keys as ``level_flow`` (for self-test)."""
    return {
        "stem": stem,
        "role": "hq" if hq else ("secret" if stem in SECRET else "mission"),
        "mission": mission_number(stem),
        "name": name,
        "secrets_declared": 3,
        "best_time_par": 100,
        "objectives": objectives or None,
        "objective_variants": [],
        "objectives_bits": [1] if objectives else [],
        "objectives_union": 1 if objectives else 0,
        "objectives_complete_sites": 0,
        "secret_tally_sites": 0,
        "end_screens": 0 if hq else 1,
        "system_finished": None if hq else {
            "offset": 0x100,
            "offsets": [0x100],
            "generates": [],
            "exits": [{"kind": e["kind"], "target": e["target"],
                       "conditions": e.get("conditions", []),
                       "offset": 0x100, "owner_id": -8,
                       "deathmatch": e.get("deathmatch", False)} for e in exits],
        },
        "exit_entity": {"offset": 0x50, "commands": ["SHOW_LEVEL_END_SCREEN"],
                        "exits": []},
        "exits": [],
        "shop": {},
    }


def self_test():
    checks = []

    def check(name, cond):
        checks.append((name, bool(cond)))

    # 1. campaign._walk recurses into IF bodies and keeps the condition chain
    root = [FakeInsn("IF", [("DEATHMATCH_GAME", [])], 0,
                     [FakeInsn("NEW_LEVEL", [("s", "dm1")], 4)]),
            FakeInsn("NEW_LEVEL", [("s", "level3")], 10)]
    got = [(i.name, c) for i, c in camp._walk(root)]
    check("walk: nested IF keeps condition", ("NEW_LEVEL", ("DEATHMATCH_GAME",)) in got)
    check("walk: unconditional site has no condition", ("NEW_LEVEL", ()) in got)

    # 2. subscript() slices an END-terminated block
    insns = [FakeInsn("NEW_LEVEL", [("s", "a")], 0),
             FakeInsn("END", [("arg", None)], 5),
             FakeInsn("SHOW_LEVEL_END_SCREEN", [("arg", None)], 6),
             FakeInsn("END", [("arg", None)], 7)]
    sub = subscript(insns, 6)
    check("subscript: starts at offset and stops at END",
          [i.name for i in sub] == ["SHOW_LEVEL_END_SCREEN", "END"])
    check("subscript: unknown offset -> empty", subscript(insns, 999) == [])

    # 3. objective completion rule == 0x45e9f0
    check("bits: all four set -> complete", objectives_mask_complete(0b1111, 4))
    check("bits: subset -> incomplete", not objectives_mask_complete(0b0111, 4))
    check("bits: no objectives -> false (0x45e9f2 jle)", not objectives_mask_complete(0, 0))
    check("bits: extra high bits ignored", objectives_mask_complete(0b11111, 4))
    check("bits: to_list", bits_to_list(0b101, 3) == [True, False, True])

    # 4. counter semantics (0x457b11)
    check("counter: LEVEL name", next_level_stem(1) == "LEVEL1" and
          next_level_stem(24) == "LEVEL24")
    check("counter: role", role_of("level0") == "hq" and role_of("level7") == "mission"
          and role_of("dm2") == "deathmatch" and role_of("secrt1") == "secret")
    check("counter: mission number", mission_number("level19") == 19 and
          mission_number("level0") is None and mission_number("temple") is None)

    # 5. exit classification
    ex = [
        {"kind": "NEW_LEVEL", "target": "level2", "conditions": ["DEATHMATCH_GAME"],
         "offset": 0, "owner_id": -8, "deathmatch": True},
        {"kind": "NEW_LEVEL", "target": "level00", "conditions": [],
         "offset": 4, "owner_id": -8, "deathmatch": False},
        {"kind": "NEXT_LEVEL", "target": None, "conditions": [],
         "offset": 8, "owner_id": -8, "deathmatch": False},
    ]
    check("exits: DM filtered", [e["target"] for e in completion_campaign_exits(
        {"system_finished": {"exits": ex}})] == ["level00", None])
    check("exits: HQ filter", [e["target"] for e in hq_exits(ex)] == ["level00"])
    check("exits: only -8 counts",
          completion_exits({"system_finished": {"exits": ex}}) == ex)

    # 6. chain builder on a synthetic 4-mission map
    smap = {"level0": _synthetic_level("level0", name="HQ", objectives=None, exits=[], hq=True)}
    for m in range(1, 5):
        if m == 3:
            exits = [{"kind": "NEXT_LEVEL", "target": None}]
        else:
            exits = [{"kind": "NEW_LEVEL", "target": "level00"},
                     {"kind": "NEW_LEVEL", "target": "level2",
                      "conditions": ["DEATHMATCH_GAME"], "deathmatch": True}]
        smap["level%d" % m] = _synthetic_level(
            "level%d" % m, name="M%d" % m, objectives=["obj"], exits=exits)
    # the builder always walks MISSION_COUNT missions; give it 24 rows
    for m in range(5, MISSION_COUNT + 1):
        smap["level%d" % m] = _synthetic_level(
            "level%d" % m, name="M%d" % m, objectives=["obj"],
            exits=[] if m == MISSION_COUNT else
            [{"kind": "NEW_LEVEL", "target": "level0b"}])
    chain = build_chain(smap)
    check("chain: 24 missions", chain["missions"] == 24)
    check("chain: no problems", not chain["problems"])
    hqs = [s for s in chain["steps"] if s["kind"] == "hq"]
    check("chain: HQ step loads LEVEL<m+1>", hqs[0]["loads"] == "level2")
    direct = [s for s in chain["steps"] if s["kind"] == "direct"]
    check("chain: level3 NEXT_LEVEL goes straight to level4",
          direct and direct[0]["stem"] == "level4")
    check("chain: last mission has no exit",
          smap["level24"]["system_finished"]["exits"] == [])
    bad = build_chain({k: v for k, v in smap.items() if k != "level7"})
    check("chain: detects a missing mission",
          any("level7" in p for p in bad["problems"]))

    # 7. GUNMETAL.HI records
    recs = [{"seconds": 146, "name": "TESTING RANGE"},
            {"seconds": 18, "name": "TESTING RANGE"},
            {"seconds": 260, "name": "KELLER CORE"}]
    blob = encode_hi(recs)
    check("hi: 64-byte records", len(blob) == 3 * HI_RECORD)
    check("hi: round-trip", parse_hi(blob) == recs)
    check("hi: min per name", best_by_name(recs) ==
          {"TESTING RANGE": 18, "KELLER CORE": 260})
    check("hi: 60-byte name cap", HI_NAME == 60 and HI_RECORD == 64)
    try:
        parse_hi(blob + b"\0")
        truncated = False
    except ValueError:
        truncated = True
    check("hi: partial tail rejected", truncated)
    try:
        encode_hi([{"seconds": 1, "name": "X" * 61}])
        long_name = False
    except ValueError:
        long_name = True
    check("hi: over-long name rejected", long_name)

    # 8. fields table
    keys = [f["key"] for f in FIELDS]
    check("fields: unique keys", len(keys) == len(set(keys)))
    check("fields: valid status", all(f["status"] in ("proven", "hypothesis", "refuted")
                                      for f in FIELDS))
    check("fields: evidence present", all(f["evidence"] for f in FIELDS))

    # 9. deterministic JSON
    payload = {"levels": smap, "chain": chain, "fields": FIELDS}
    a = json.dumps(payload, sort_keys=True)
    b = json.dumps(payload, sort_keys=True)
    check("json: deterministic", a == b and json.loads(a)["chain"]["missions"] == 24)

    # 10. summary rows on synthetic data
    rows = summary_rows(smap, {"best": {"M1": 10}})
    check("summary: one row per level", len(rows) == len(smap))
    check("summary: best is joined by display name",
          any(r["your_best"] == 10 for r in rows))
    multi = _synthetic_level("level8", name="M8", objectives=["obj"],
                             exits=[{"kind": "NEW_LEVEL", "target": "level00"}])
    multi["system_finished"]["offsets"] = [0x100, 0x200]
    multi["system_finished"]["exits"].append(
        {"kind": "NEW_LEVEL", "target": "level0a", "conditions": [],
         "offset": 0x200, "owner_id": -8, "deathmatch": False})
    multi["exits"] = multi["system_finished"]["exits"]
    mrows = summary_rows({"level8": multi}, {})
    check("summary: several -8 rows marked xN", mrows[0]["fin"] == "0x100x2")
    check("summary: exits of all -8 rows listed",
          mrows[0]["exits"] == "level00; level0a")

    passed = sum(1 for _n, ok in checks if ok)
    for name, ok in checks:
        print("%s  %s" % ("ok  " if ok else "FAIL", name))
    print("%d/%d self-tests passed" % (passed, len(checks)))
    return 0 if passed == len(checks) else 1


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--summary", action="store_true", help="table over all levels")
    ap.add_argument("--json", action="store_true", help="machine-readable dump")
    ap.add_argument("--fields", action="store_true", help="field/address map")
    ap.add_argument("--hi", default=None, help="path to GUNMETAL.HI")
    ap.add_argument("--self-test", action="store_true", help="run without game files")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.fields:
        print_fields()
        return 0
    if not args.summary and not args.json:
        ap.print_help()
        return 2
    mapinfo = flow_map(args.game)
    hi = read_hi(args.game, args.hi)
    chain = build_chain(mapinfo)
    if args.json:
        print(json.dumps({"levels": mapinfo, "chain": chain, "hi": hi,
                          "fields": FIELDS}, indent=2, sort_keys=True))
        return 0
    print_summary(mapinfo, hi, chain)
    return 1 if chain["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
