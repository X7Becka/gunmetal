#!/usr/bin/env python3
"""Deathmatch rules of Gunmetal: modes, score, respawn, handicap, teams.

``docs/DM_RULES.md`` carries the prose and the evidence; this module is the
runnable model.  Everything was taken from the shipped ``gundx.exe`` (reads
only) and from the 36 ``.sbt`` level scripts -- **no Ghidra needed**.

The rules in one screen::

    0x83d2c8  game_mode            0 campaign | 1 DM normal | 2 DM classic | 3 hunt
    0x83d184  mp_flags (bitmask)   1 team, 2 CTF, 4 no monsters, 8 freeze,
                                   0x10 no ammo buying, 0x20 handicap
    player+0x3270  i32  frags      (0x81c2b0 for slot 0)
    player+0x3274  i32[8] kills_by_victim[victim]
    player+0x344c  i32  team id    (compared on a team kill)
    player+0x33d4  i32  dead/fire timer; respawn at >= 0x88
    player+0x3330  i32  salvage    -> added to cash on respawn, then cleared
    player+0x332c  i32  cash

Everything in ``SET_DEATHMATCH_START`` (op 168): 8 slots per map, 248 sites in
the 31 non-HQ maps, one DM chain ``dm1 -> dm2 -> dm3 -> dm4 -> level1``.

Usage::

    python3 tools/re/dm_rules.py --self-test          # no game files needed
    python3 tools/re/dm_rules.py --summary            # 36 levels
    python3 tools/re/dm_rules.py --summary --game ~/.cache/gunmetal-re/game
    python3 tools/re/dm_rules.py --json
    python3 tools/re/dm_rules.py --fields
    python3 tools/re/dm_rules.py --anchors            # bytes in gundx.exe

Standard library only; ``gm/sbt.py``/``gm/sbtvm.py`` and
``tools/re/campaign.py`` are reused as-is (never edited here).
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

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")

#: Headquarters maps: campaign-only, no DM starts (docs/FORMATS.md 15).
HQ = ("level0", "level00", "level000", "level0a", "level0b")
#: The DM chain and the two DM-capable secret maps.
DM_MAPS = ("dm1", "dm2", "dm3", "dm4", "temple")

# ---------------------------------------------------------------------------
# values proven in the executable
# ---------------------------------------------------------------------------
#: ``game_mode`` = ``ds:0x83d2c8``.  Proof: 0x4421c0 ``!=0`` (DEATHMATCH_GAME),
#: 0x45eb06 ``==2`` (CLASSIC), 0x45eb1c ``==1`` (NORMAL), 0x4421d0 ``==3``
#: (SCAVENGERHUNT_GAME).
MODES = [
    dict(value=0, key="campaign", title="campaign",
         proof="0x4421c0: test ecx,ecx / setne (DEATHMATCH_GAME == 0)"),
    dict(value=1, key="dm-normal", title="Normal Deathmatch",
         proof="0x45eb1c: cmp ecx,1 / sete (DEATHMATCH_NORMAL_GAME)"),
    dict(value=2, key="dm-classic", title="Deathmatch Classic",
         proof="0x45eb06: cmp ecx,2 / sete (DEATHMATCH_CLASSIC_GAME)"),
    dict(value=3, key="dm-hunt", title="Scavenger Hunt Deathmatch",
         proof="0x4421d0: cmp ecx,3 / sete (SCAVENGERHUNT_GAME), /hunt 0x44a72f"),
]

#: Frontend list index ``ds:0x68cd84`` -> ``game_mode``.
#: 0x4bc6e8 is the name table; 0x48b0c0 maps the index to ``ds:0x678148``
#: (0->1 at 0x48b0f8, 1->3 at 0x48b0d8, 2->2 at 0x48b0e4, 3->0, 4->1).
MENU_MODES = [
    dict(index=0, mode=1, name="Normal Deathmatch", str_va="0x4bc77c"),
    dict(index=1, mode=3, name="Scavenger Hunt Deathmatch", str_va="0x4bc760"),
    dict(index=2, mode=2, name="Deathmatch Classic", str_va="0x4bc74c"),
]

#: ``mp_flags`` = ``ds:0x83d184``, parsed from the command line at
#: 0x44a719..0x44a811 (``strcmp`` against ``0x4abe38``..) and from the menu
#: (``ds:0x68cd88``).
MP_FLAGS = [
    dict(bit=0x01, key="team", option="/team", site="0x44a752"),
    dict(bit=0x02, key="ctf", option="/flag", site="0x44a777"),
    dict(bit=0x04, key="no_monsters", option="/nomonsters", site="0x44a79c"),
    dict(bit=0x08, key="freeze", option="/freeze", site="0x44a7c1"),
    dict(bit=0x10, key="no_buy_ammo", option="/nobuyammo", site="0x44a80b"),
    dict(bit=0x20, key="handicap", option="/handicap", site="0x44a7e6"),
]

#: Mode mask of a thing: ``0x45c830(mask)`` returns the mask bit for the
#: current mode, 0 = "do not create".  Used by ``ADD_THING`` (0x459adc) and by
#: the ``ADD_THING`` condition (0x45d2eb).
MODE_MASK = [
    dict(bit=0x01, key="campaign", site="0x45c85f and eax,1"),
    dict(bit=0x02, key="dm-normal", site="0x45c863 and eax,2"),
    dict(bit=0x04, key="ctf", site="0x45c84c and eax,4"),
    dict(bit=0x08, key="dm-classic", site="0x45c874 and eax,8"),
    dict(bit=0x10, key="dm-hunt", site="0x45c86b and eax,0x10"),
    dict(bit=0x20, key="monster", site="0x45c83f test al,0x20 (скрывается /nomonsters)"),
]

#: Field map (base ``player[i] = 0x819040 + i*0x3510``, docs/PLAYER_FIELDS.md 1).
FIELDS = [
    dict(key="game_mode", what="active game mode / deathmatch kind", addr="0x83d2c8",
         status="proven",
         writer="0x442e1c (new game: from 0x83d624); 0x466bb6 (load save)",
         reader="0x4421c0 !=0; 0x45eb06 ==2; 0x45eb1c ==1; 0x4421d0 ==3; "
                "0x45c84f mode mask; 0x4707df campaign-only frag count",
         evidence="docs/VM_EXEC.md 4; docs/VM_SCENE.md 2"),
    dict(key="pending_game_mode", what="mode chosen in the frontend / cmdline", addr="0x83d624",
         status="proven",
         writer="new_game_parameters 0x442c02 (param_9); 0x48b0c0 -> 0x678148",
         reader="0x442e0c (copied to 0x83d2c8 at new game start)",
         evidence="0x442a90 decomp: DAT_0083d624 = param_9"),
    dict(key="mp_flags", what="multiplayer/deathmatch option bits", addr="0x83d184",
         status="proven",
         writer="0x442e22 (from 0x83d188); 0x4604e5/0x460513; 0x48efd9",
         reader="team 0x45eb71/0x4537c8, ctf 0x45eb81, no-monsters 0x45c83a, "
                "freeze 0x470b4f, no-buy 0x45c0ae, handicap 0x452e67",
         evidence="cmdline parser 0x44a719..0x44a811"),
    dict(key="frags", what="kill/frag score (the HUD 'KILL' number)", addr="player+0x3270 (0x81c2b0)",
         status="proven",
         writer="0x453812 +1 (kill), 0x4537fd -1 (team kill), 0x45387f -1 (suicide), "
                "0x4707fa +1 (campaign), 0x4532ee =0 (spawn)",
         reader="HUD 0x492fdd; end screen 0x48377f; sort 0x486b19/0x493763; "
                "handicap 0x452e94; accessor 0x442287; save 0x465832/0x466d9c",
         evidence="0x4536e0 player_killed"),
    dict(key="kills_by_victim", what="i32[8]: times this player killed slot v", addr="player+0x3274 (0x81c2b4)",
         status="proven",
         writer="0x453835..0x453840 (index = killer*0x3510 + victim*4)",
         reader="0x4532f8 cleared on spawn (8 dwords)",
         evidence="rc = 0x453835 lea ecx,[ebx+edx*4]; [ecx*4+0x81c2b4]++"),
    dict(key="team", what="team / colour id compared on a team kill", addr="player+0x344c (0x81c48c)",
         status="proven",
         writer="0x452b0c (=0 for SET_PLAYER_START), 0x453284 (= 0x42b81b(name,4))",
         reader="0x4537db team-kill check; 0x44ce2b camera cycle in team mode",
         evidence="0x4537d9..0x453803"),
    dict(key="name", what="player name char[16]", addr="player+0x3450 (0x81c490)",
         status="proven",
         writer="0x4531f4 (strncpy 0x403190 len 0x10), 0x453200 NUL",
         reader="0x4936a0 kill message; scoreboard 0x483607",
         evidence="0x4531ed..0x453207"),
    dict(key="last_attacker", what="player id of the last damager (= killer on death)", addr="player+0x3178 (0x81c1b8)",
         status="proven",
         writer="damage paths (0x448xxx, 0x450f94 ...); cleared 0x442a24",
         reader="0x45373e -> event_post(-7) source, then 0x435a93",
         evidence="0x4536e0"),
    dict(key="action_state", what="0 free, 1 alive, 7 dead/exploding", addr="player+0x3210 (0x81c250)",
         status="proven",
         writer="0x453763 =7 on death, 0x453303/0x4526d3 =1 on (re)spawn",
         reader="0x453700 (ignore already dead), jump table 0x4452d8",
         evidence="docs/PLAYER_FIELDS.md 10"),
    dict(key="dead_timer", what="timer that runs while dead; respawn at >= 0x88", addr="player+0x33d4 (0x81c414)",
         status="proven",
         writer="0x45372f =0 on death; 0x443849 += frame delta every tick",
         reader="0x443b0a cmp 0x88 -> 0x452670 respawn",
         evidence="0x443846 (case action_state==7) + 0x443b0a"),
    dict(key="salvage", what="credits carried into the respawn", addr="player+0x3330 (0x81c370)",
         status="proven",
         writer="ADD_SOURCEPLAYER_SALVAGE (op 115, 0x459de9)",
         reader="0x452e4f respawn: cash = salvage + 0x83d310 (+ handicap); then 0",
         evidence="docs/ECONOMY.md 8"),
    dict(key="cash", what="credits", addr="player+0x332c (0x81c36c)",
         status="proven",
         writer="0x452e61/0x452eaa/... respawn handicap ladder; shops",
         reader="shops 0x47b8c7; HUD 0x4927e0",
         evidence="docs/ECONOMY.md 2"),
    dict(key="starting_cash", what="credits granted by a new game / respawn base", addr="0x83d310",
         status="proven",
         writer="0x442e49 <- 0x83d198 (new game param_11); cmdline default 0x30d40",
         reader="0x452e4f/0x452e55 (respawn), 0x452e74..0x452f71 (handicap unit)",
         evidence="menu ladder 0x4bc6f8 {36000..5000000}, 0x4863c0"),
    dict(key="handicap", what="bonus cash for the trailing player", addr="0x83d184 & 0x20",
         status="proven",
         writer="/handicap 0x44a7e6",
         reader="0x452e67 test byte, then ladder 0x452e9a..0x452f71",
         evidence="docs/ECONOMY.md 8/11 (consumer was open there)"),
    dict(key="countdown_timer", what="level countdown in ticks; expiry posts event -16", addr="0x80d898",
         status="proven",
         writer="op 99 SET_LEVEL_COUNTDOWN_TIMER 0x45b843; reset 0x4570a9",
         reader="ticker 0x457230 (subtract delta, post -16 when <=0); HUD 0x462edb",
         evidence="docs/VM_EXEC.md 3 (op 99)"),
    dict(key="menu_mode_index", what="selected DM type in the frontend (0..2)", addr="0x68cd84",
         status="proven",
         writer="0x48c93a/0x48c0ab (cursor), 0x484fb7 (init from saved setup)",
         reader="0x48b0c0 -> 0x678148 = game_mode; display 0x4885f8",
         evidence="name table 0x4bc6e8"),
    dict(key="menu_flags", what="frontend option bits (team etc.)", addr="0x68cd88",
         status="proven",
         writer="0x48c961/0x48c969/0x48c971/0x48c97f/0x48c98d or-bits",
         reader="0x48dfa3 -> param_10 (mp_flags)",
         evidence="0x48dfaf..0x48dfb6"),
    dict(key="dm_starts", what="8 spawn points per map, slot = player index", addr="0x5a9500/0x5a9540/...",
         status="proven",
         writer="SET_DEATHMATCH_START (op 168) handler 0x45c2bc -> 0x452b1b",
         reader="level start 0x452af1 (slot = player), respawn 0x452969 (farthest)",
         evidence="docs/PLAYER.md 1/2; docs/CAMERA.md (248 sites)"),
]

#: Byte anchors in ``gundx.exe``: (vma, hex, what).
ANCHORS = [
    (0x4421C8, "85 c9 0f 95 c0", "DEATHMATCH_GAME: game_mode != 0"),
    (0x4421D8, "83 f9 03 0f 94 c0", "SCAVENGERHUNT_GAME: game_mode == 3"),
    (0x45EB0E, "83 f9 02 0f 94 c0", "DEATHMATCH_CLASSIC_GAME: game_mode == 2"),
    (0x45EB24, "83 f9 01 0f 94 c0", "DEATHMATCH_NORMAL_GAME: game_mode == 1"),
    (0x45EB76, "83 e0 01", "TEAM_GAME: mp_flags & 1"),
    (0x45EB86, "83 e0 02", "CAPTURE_THE_FLAG_GAME: mp_flags & 2"),
    (0x452E67, "f6 05 84 d1 83 00 20", "handicap: mp_flags & 0x20"),
    (0x452E76, "b8 b0 c2 81 00", "handicap: scan frags +0x3270 of the other players"),
    (0x453812, "8b 88 b0 c2 81 00 41", "kill: killer frags (+0x3270) += 1"),
    (0x45387F, "48", "suicide: frags -= 1"),
    (0x453763, "c7 86 50 c2 81 00 07", "death: action_state (+0x210) = 7"),
    (0x45372F, "c7 86 14 c4 81 00 00", "death: dead timer (+0x33d4) = 0"),
    (0x443B0A, "81 bb 14 c4 81 00 88", "respawn when +0x33d4 >= 0x88"),
    (0x492FDD, "8b 80 b0 c2 81 00", "HUD KILL number = player+0x3270"),
    (0x44A72F, "c7 44 24 1c 03 00 00", "/hunt selects game_mode 3"),
    (0x48B0D8, "c7 05 48 81 67 00 03", "menu index 1 (hunt) -> game_mode 3"),
    (0x48B0E4, "c7 05 48 81 67 00 02", "menu index 2 (classic) -> game_mode 2"),
]

#: Handicap ladder read at 0x452e9a..0x452f71: ``diff = max_other - mine``.
#: (comparison threshold, multiplier of the starting cash added to the respawn)
HANDICAP_LADDER = [
    (50, 5.0),    # my < max-50  -> base + 5*start   (0x452e9a..0x452eb0)
    (30, 4.0),    # my < max-30  -> base + 4*start   (0x452eb5..0x452ec9)
    (25, 3.0),    # my < max-25  -> base + 3*start   (0x452ece..0x452ee4)
    (20, 2.0),    # my < max-20  -> base + 2*start   (0x452ee9..0x452efd)
    (15, 1.0),    # my < max-15  -> base + 1*start   (0x452f02..0x452f15)
    (10, 0.5),    # my < max-10  -> base + start/2  (0x452f17..0x452f31)
    (7, 0.25),    # my < max-7   -> base + start/4  (0x452f33..0x452f51)
    (4, 0.1),     # my < max-4   -> base + start/10 (0x452f53..0x452f77)
]

#: Mode-gated script conditions that select DM content.
MODE_CONDITIONS = (
    "DEATHMATCH_GAME",
    "DEATHMATCH_CLASSIC_GAME",
    "DEATHMATCH_NORMAL_GAME",
    "TEAM_GAME",
    "SCAVENGERHUNT_GAME",
    "MULTIPLAYER_GAME",
    "CAPTURE_THE_FLAG_GAME",
)

#: Hunt mode grants every weapon bit: ``OR_SOURCEPLAYER_WEAPON_BITS 0x7fffff``.
HUNT_ALL_WEAPONS = 0x7FFFFF


# ---------------------------------------------------------------------------
# model helpers (pure, exercised by --self-test)
# ---------------------------------------------------------------------------
def mode_by_value(value):
    for m in MODES:
        if m["value"] == value:
            return m
    return None


def mode_from_menu_index(index):
    """Frontend list index -> game_mode (0x48b0c0 table)."""
    for row in MENU_MODES:
        if row["index"] == index:
            return row["mode"]
    return None


def mp_flag(bit):
    for f in MP_FLAGS:
        if f["bit"] == bit:
            return f
    return None


def mode_mask_bit(value):
    """Bit a thing carries to appear in *value* mode (0x45c830)."""
    for row in MODE_MASK:
        if row["key"] == (mode_by_value(value) or {}).get("key"):
            return row["bit"]
    return None


def handicap_multiplier(max_other, mine):
    """Cash multiplier of ``0x83d310`` the trailing player gets on respawn.

    ``0x452e9a``..``0x452f71`` compares ``mine`` with ``max_other - N`` using
    ``jge`` (i.e. the ladder step applies while ``diff > N``).
    """
    diff = max_other - mine
    for limit, mult in HANDICAP_LADDER:
        if diff > limit:
            return mult
    return 0.0


def handicap_cash(base, start, max_other, mine):
    """``cash`` after the handicap block: base + mult*start, truncated."""
    mult = handicap_multiplier(max_other, mine)
    if mult <= 0:
        return base
    if mult == 0.5:
        return base + int(start / 2)
    if mult == 0.25:
        return base + int(start / 4)
    if mult == 0.1:
        return base + int(start / 10)
    return base + mult * start


def fixed16(value):
    """Signed 16.16 -> float (DM start coordinates in SET_DEATHMATCH_START)."""
    if value >= 0x80000000:
        value -= 0x100000000
    return value / 65536.0


# ---------------------------------------------------------------------------
# per-level facts
# ---------------------------------------------------------------------------
def _args(ins):
    return camp._args(ins)


def level_dm(stem, path):
    """Collect the DM-relevant script facts of one level."""
    info = {
        "stem": stem,
        "name": None,
        "dm_starts": 0,
        "dm_slots": [],
        "dm_points": [],
        "player_starts": 0,
        "mode_conditions": {k: 0 for k in MODE_CONDITIONS},
        "hunt_weapon_bits": None,
        "other_weapon_bits": None,
        "player_types": [],
        "countdown": [],
        "player_kill_sites": 0,
        "end_screens": 0,
        "shop_screens": 0,
        "exits": [],
        "role": "dm" if stem in DM_MAPS else ("hq" if stem in HQ else "campaign"),
    }
    sbt = sbtmod.parse_file(path)
    recs = sbt.records_of(11)
    if not recs:
        return info
    insns = sbtvm.decode(recs[0].payload.code)[0]

    for ins, conds in camp._walk(insns):
        name = ins.name
        args = _args(ins)
        for cond in conds:
            if cond in info["mode_conditions"]:
                info["mode_conditions"][cond] += 1
        if name == "SET_LEVEL_NAME" and info["name"] is None:
            info["name"] = args[0]
        elif name == "SET_DEATHMATCH_START":
            info["dm_starts"] += 1
            if args:
                info["dm_slots"].append(args[0])
                if len(args) >= 4:
                    info["dm_points"].append(
                        [fixed16(args[1]), fixed16(args[2]), fixed16(args[3])])
        elif name == "SET_PLAYER_START":
            info["player_starts"] += 1
        elif name == "OR_SOURCEPLAYER_WEAPON_BITS" and args:
            if "SCAVENGERHUNT_GAME" in conds and not info["hunt_weapon_bits"]:
                info["hunt_weapon_bits"] = args[0]
            elif not conds and info["other_weapon_bits"] is None:
                info["other_weapon_bits"] = args[0]
        elif name == "DEFINE_PLAYER_TYPES" and args:
            info["player_types"].append({"n": args[0], "conds": list(conds)})
        elif name == "SET_LEVEL_COUNTDOWN_TIMER" and args:
            info["countdown"].append(args[0])
        elif name == "SOURCEPLAYER_KILL":
            info["player_kill_sites"] += 1
        elif name == "SHOW_LEVEL_END_SCREEN":
            info["end_screens"] += 1
        elif name and name.startswith("SHOW_") and name.endswith("BUY_SCREEN"):
            info["shop_screens"] += 1
        elif name in ("NEW_LEVEL", "NEXT_LEVEL"):
            info["exits"].append({
                "target": (args[0] if name == "NEW_LEVEL" and args else None),
                "conditions": list(conds),
            })

    info["dm_slots"] = sorted(set(info["dm_slots"]))
    return info


def all_levels(game):
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    return [level_dm(os.path.splitext(os.path.basename(p))[0], p) for p in files]


def totals(levels):
    return {
        "levels": len(levels),
        "dm_starts": sum(l["dm_starts"] for l in levels),
        "levels_with_dm_starts": sum(1 for l in levels if l["dm_starts"]),
        "player_starts": sum(l["player_starts"] for l in levels),
        "countdown_levels": [l["stem"] for l in levels if l["countdown"]],
        "hunt_levels": sum(1 for l in levels if l["hunt_weapon_bits"]),
        "classic_branches": sum(l["mode_conditions"]["DEATHMATCH_CLASSIC_GAME"] for l in levels),
        "deathmatch_branches": sum(l["mode_conditions"]["DEATHMATCH_GAME"] for l in levels),
    }


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------
def _order(stem):
    return (0 if stem.startswith("level") else 1 if stem.startswith("dm") else 2, stem)


def print_summary(levels, out=print):
    t = totals(levels)
    out("режимы: 0 campaign | 1 dm-normal | 2 dm-classic | 3 dm-hunt "
        "(game_mode = ds:0x83d2c8)")
    out("флаги: 0x83d184  bit0 team, bit1 ctf, bit2 no-monsters, bit3 freeze, "
        "bit4 no-buy-ammo, bit5 handicap")
    out("фраги: player+0x3270 (0x81c2b0 для слота 0); respawn: +0x33d4 >= 0x88")
    out("")
    out("%-8s %-9s %-3s %-3s %-4s %-4s %-5s %-6s %s" % (
        "level", "role", "dm", "pl", "dmG", "cls", "hunt", "timer", "выходы"))
    for l in sorted(levels, key=lambda x: _order(x["stem"])):
        exits = "; ".join(
            "%s%s" % (e["target"] or "NEXT_LEVEL",
                      " [" + ",".join(e["conditions"]) + "]" if e["conditions"] else "")
            for e in l["exits"]) or "-"
        out("%-8s %-9s %-3d %-3d %-4d %-4d %-5s %-6s %s" % (
            l["stem"], l["role"], l["dm_starts"], l["player_starts"],
            l["mode_conditions"]["DEATHMATCH_GAME"],
            l["mode_conditions"]["DEATHMATCH_CLASSIC_GAME"],
            ("0x%x" % l["hunt_weapon_bits"]) if l["hunt_weapon_bits"] else "-",
            ",".join(str(v) for v in l["countdown"]) or "-", exits))
    out("")
    out("итого: %d уровней, DM-стартов %d на %d картах, игрок-стартов %d, "
        "hunt-веток %d, classic-веток %d, DM-условий %d" % (
            t["levels"], t["dm_starts"], t["levels_with_dm_starts"],
            t["player_starts"], t["hunt_levels"], t["classic_branches"],
            t["deathmatch_branches"]))
    out("счётчик уровня (op 99) задан на: %s" % (", ".join(t["countdown_levels"]) or "-"))
    out("лимита фрагов в коде нет: +0x3270 только инкрементится/сортируется "
        "(см. --fields)")


def print_fields(out=print):
    out("%-22s %-28s %-8s %s" % ("поле", "адрес", "статус", "доказательство"))
    for f in FIELDS:
        out("%-22s %-28s %-8s %s" % (f["key"], f["addr"], f["status"], f["evidence"]))
    out("")
    out("режимы:")
    for m in MODES:
        out("  %d  %-11s %-27s %s" % (m["value"], m["key"], m["title"], m["proof"]))
    out("меню -> режим (0x48b0c0):")
    for r in MENU_MODES:
        out("  индекс %d -> режим %d  %-27s %s" % (
            r["index"], r["mode"], r["name"], r["str_va"]))
    out("флаги mp_flags (0x83d184):")
    for f in MP_FLAGS:
        out("  0x%02x  %-12s %-12s %s" % (f["bit"], f["key"], f["option"], f["site"]))
    out("маска режима вещи (0x45c830):")
    for r in MODE_MASK:
        out("  0x%02x  %-11s %s" % (r["bit"], r["key"], r["site"]))
    out("гандикап (0x452e9a..0x452f71), diff = max_other - mine:")
    for limit, mult in HANDICAP_LADDER:
        out("  diff > %-2d -> base + %-4s * starting_cash" % (limit, mult))


def check_anchors():
    """Compare code bytes with ``gundx.exe`` (``ok=None`` when exe missing)."""
    try:
        import exelib
        blob = exelib.data()
        va2off = exelib.va2off
    except Exception:
        return [dict(vma=v, want=w, what=t, ok=None) for v, w, t in ANCHORS]
    out = []
    for vma, want, what in ANCHORS:
        off = va2off(vma)
        got = blob[off:off + len(want.split())].hex(" ") if off is not None else ""
        out.append(dict(vma=vma, want=want, what=what, got=got,
                        ok=(off is not None and got == want)))
    return out


# ---------------------------------------------------------------------------
# self-test  (synthetic; byte anchors are optional)
# ---------------------------------------------------------------------------
def _insn(name, args=(), body=None, off=0):
    return sbtvm.Insn(off, 0, name, list(args), body, 0)


def _script_insns():
    """A synthetic SCRIPT block with DM starts, hunt/all-weapons and a timer."""
    def dm(slot, x):
        return _insn("SET_DEATHMATCH_START",
                     [slot, x, 0, -0x10000, 0.0, 0.0, 0.0])

    hunt_if = _insn("IF", [("SCAVENGERHUNT_GAME", [])],
                    [_insn("OR_SOURCEPLAYER_WEAPON_BITS", [HUNT_ALL_WEAPONS])])
    classic_if = _insn("IF", [("DEATHMATCH_CLASSIC_GAME", [])],
                       [_insn("DEFINE_PLAYER_TYPES", [4])])
    dm_if = _insn("IF", [("DEATHMATCH_GAME", [])],
                  [_insn("SOURCEPLAYER_KILL"), _insn("SET_PLAYER_START", [0, 0, 0, 0, 0, 0, 0])])
    return [
        _insn("SET_LEVEL_NAME", ["TEST MAP"]),
        _insn("OR_SOURCEPLAYER_WEAPON_BITS", [1]),
        hunt_if,
        classic_if,
        dm_if,
        _insn("SET_LEVEL_COUNTDOWN_TIMER", [1234]),
        _insn("NEW_LEVEL", ["level2"]),
    ]


def _fake_level(stem, insns, role=None):
    """Run the same collectors as :func:`level_dm` over a synthetic script."""
    info = {
        "stem": stem, "name": None, "dm_starts": 0, "dm_slots": [], "dm_points": [],
        "player_starts": 0, "mode_conditions": {k: 0 for k in MODE_CONDITIONS},
        "hunt_weapon_bits": None, "other_weapon_bits": None, "player_types": [],
        "countdown": [], "player_kill_sites": 0, "end_screens": 0,
        "shop_screens": 0, "exits": [],
        "role": role or ("dm" if stem in DM_MAPS else ("hq" if stem in HQ else "campaign")),
    }
    for ins, conds in camp._walk(insns):
        args = _args(ins)
        for c in conds:
            if c in info["mode_conditions"]:
                info["mode_conditions"][c] += 1
        if ins.name == "SET_LEVEL_NAME":
            info["name"] = args[0]
        elif ins.name == "SET_DEATHMATCH_START":
            info["dm_starts"] += 1
            info["dm_slots"].append(args[0])
            info["dm_points"].append([fixed16(a) for a in args[1:4]])
        elif ins.name == "SET_PLAYER_START":
            info["player_starts"] += 1
        elif ins.name == "OR_SOURCEPLAYER_WEAPON_BITS":
            if "SCAVENGERHUNT_GAME" in conds:
                info["hunt_weapon_bits"] = args[0]
            elif not conds and info["other_weapon_bits"] is None:
                info["other_weapon_bits"] = args[0]
        elif ins.name == "DEFINE_PLAYER_TYPES":
            info["player_types"].append({"n": args[0], "conds": list(conds)})
        elif ins.name == "SET_LEVEL_COUNTDOWN_TIMER":
            info["countdown"].append(args[0])
        elif ins.name == "SOURCEPLAYER_KILL":
            info["player_kill_sites"] += 1
        elif ins.name == "NEW_LEVEL":
            info["exits"].append({"target": args[0], "conditions": list(conds)})
    info["dm_slots"] = sorted(set(info["dm_slots"]))
    return info


def self_test():
    checks = []

    def check(name, cond):
        checks.append((name, bool(cond)))

    # 1. mode values and their proof sites
    check("режимы 0..3 без дыр", [m["value"] for m in MODES] == [0, 1, 2, 3])
    check("campaign != 0, classic == 2, normal == 1, hunt == 3",
          mode_by_value(0)["key"] == "campaign" and mode_by_value(2)["key"] == "dm-classic"
          and mode_by_value(1)["key"] == "dm-normal" and mode_by_value(3)["key"] == "dm-hunt")
    check("нет режима 4/5", mode_by_value(4) is None and mode_by_value(5) is None)

    # 2. frontend list index -> mode (0x48b0c0)
    check("меню: Normal->1, Hunt->3, Classic->2",
          [mode_from_menu_index(i) for i in (0, 1, 2)] == [1, 3, 2])
    check("меню: индекса 3 в списке режимов нет", mode_from_menu_index(3) is None)

    # 3. mp_flags
    check("/team -> 1, /flag -> 2, /handicap -> 0x20",
          mp_flag(1)["key"] == "team" and mp_flag(2)["key"] == "ctf"
          and mp_flag(0x20)["key"] == "handicap")
    bits = [f["bit"] for f in MP_FLAGS]
    check("биты mp_flags уникальны и в пределах байта",
          len(bits) == len(set(bits)) and all(0 < b < 0x100 for b in bits))

    # 4. thing mode mask
    check("маска режима: campaign 1, normal 2, ctf 4, classic 8, hunt 0x10",
          [mode_mask_bit(v) for v in (0, 1, 2, 3)] == [1, 2, 8, 0x10])
    check("маска ctf отдельная (0x45c84c)", MODE_MASK[2]["bit"] == 0x04)
    check("маска: бит 0x20 = монстр, гасится mp_flags & 4",
          mp_flag(0x04)["key"] == "no_monsters"
          and any(r["bit"] == 0x20 and r["key"] == "monster" for r in MODE_MASK))

    # 5. handicap ladder (0x452e9a..0x452f71)
    ladder = [(51, 5.0), (50, 4.0), (31, 4.0), (30, 3.0), (26, 3.0), (25, 2.0),
              (21, 2.0), (20, 1.0), (16, 1.0), (15, 0.5), (11, 0.5), (10, 0.25),
              (8, 0.25), (7, 0.1), (5, 0.1), (4, 0.0), (0, 0.0), (-3, 0.0)]
    check("гандикап: пороги 50/30/25/20/15/10/7/4",
          all(handicap_multiplier(d, 0) == m for d, m in ladder))
    check("гандикап: 4x = base + 4*start",
          handicap_cash(1000, 200000, 40, 0) == 1000 + 4 * 200000)
    check("гандикап: 1/10 = base + start/10",
          handicap_cash(1000, 200000, 5, 0) == 1000 + 20000)
    check("гандикап: вне ладдера cash не меняется",
          handicap_cash(1000, 200000, 4, 0) == 1000)

    # 6. fixed 16.16
    check("16.16: -0x10000 = -1.0", fixed16(0xFFFF0000) == -1.0)
    check("16.16: 0x18000 = 1.5", fixed16(0x18000) == 1.5)

    # 7. synthetic script collectors
    lvl = _fake_level("dm1", _script_insns())
    check("synthetic: 0 DM-стартов (их нет в блоке)", lvl["dm_starts"] == 0)
    check("synthetic: hunt даёт все 23 бита оружия",
          lvl["hunt_weapon_bits"] == HUNT_ALL_WEAPONS)
    check("synthetic: обычная ветка — оружие 0", lvl["other_weapon_bits"] == 1)
    check("synthetic: classic-ветка DEFINE_PLAYER_TYPES 4",
          any(p["n"] == 4 and "DEATHMATCH_CLASSIC_GAME" in p["conds"]
              for p in lvl["player_types"]))
    check("synthetic: 2 инструкции под DEATHMATCH_GAME",
          lvl["mode_conditions"]["DEATHMATCH_GAME"] == 2)
    check("synthetic: таймер 1234", lvl["countdown"] == [1234])
    check("synthetic: SOURCEPLAYER_KILL = 1", lvl["player_kill_sites"] == 1)
    check("synthetic: выход level2", lvl["exits"][0]["target"] == "level2")

    # 8. 8 starts per non-HQ map = 248 total (docs/CAMERA.md)
    synth = []
    for i in range(31):
        ins = [_insn("SET_DEATHMATCH_START", [s, 0, 0, 0, 0.0, 0.0, 0.0])
               for s in range(8)]
        synth.append(_fake_level("map%d" % i, ins))
    for i in range(5):
        synth.append(_fake_level(HQ[i], []))
    t = totals(synth)
    check("31 карта * 8 слотов = 248 DM-стартов", t["dm_starts"] == 248)
    check("248 = ровно на 31 карте", t["levels_with_dm_starts"] == 31)
    check("HQ-карты без DM-стартов", t["dm_starts"] - 248 == 0)

    # 9. fields/anchors sanity
    keys = [f["key"] for f in FIELDS]
    check("карта полей: ключи уникальны", len(keys) == len(set(keys)))
    check("фраги и HUD-чтение указывают на +0x3270",
          any("3270" in f["addr"] for f in FIELDS if f["key"] == "frags"))
    anchors = check_anchors()
    if anchors and anchors[0]["ok"] is None:
        check("байтовые якоря: exe недоступен -> пропуск", True)
    else:
        bad = [a for a in anchors if not a["ok"]]
        check("байтовые якоря: %d/%d" % (len(anchors) - len(bad), len(anchors)), not bad)

    passed = sum(1 for _n, ok in checks if ok)
    for name, ok in checks:
        print("%s  %s" % ("OK  " if ok else "FAIL", name))
    print("%d/%d self-tests passed" % (passed, len(checks)))
    return 0 if passed == len(checks) else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--self-test", action="store_true", help="run without game files")
    ap.add_argument("--summary", action="store_true", help="per-level DM table")
    ap.add_argument("--json", action="store_true", help="dump everything as JSON")
    ap.add_argument("--fields", action="store_true", help="field/address map")
    ap.add_argument("--anchors", action="store_true", help="check bytes in gundx.exe")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.fields:
        print_fields()
        return 0
    if args.anchors:
        bad = 0
        for a in check_anchors():
            state = "?" if a["ok"] is None else ("OK " if a["ok"] else "BAD")
            print("%s 0x%06x  %-40s %s" % (state, a["vma"], a["what"], a["want"]))
            if a["ok"] is False:
                print("     got: %s" % a.get("got", ""))
                bad += 1
        return 1 if bad else 0

    if not os.path.isdir(args.game):
        print("нет каталога игры: %s" % args.game, file=sys.stderr)
        return 1
    levels = all_levels(args.game)
    if not levels:
        print("нет .sbt в %s" % args.game, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps({
            "modes": MODES,
            "menu_modes": MENU_MODES,
            "mp_flags": MP_FLAGS,
            "mode_mask": MODE_MASK,
            "fields": FIELDS,
            "handicap_ladder": [{"diff_gt": l, "multiplier": m}
                                for l, m in HANDICAP_LADDER],
            "totals": totals(levels),
            "levels": sorted(levels, key=lambda x: _order(x["stem"])),
        }, indent=2, sort_keys=True))
        return 0

    print_summary(levels)
    return 0


if __name__ == "__main__":
    sys.exit(main())
