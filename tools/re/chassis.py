#!/usr/bin/env python3
"""Chassis selection, the 57 CHEVY bonus and chassis assets (`M6.1.4`/`M6.5.3`).

RE only: reads the level scripts (``.sbt`` record 11 = ``SCRIPT``, 12 = sound
samples, 20 = ``SOB_TABLE``) and the shipped exe bytes through
``tools/re/exelib.py``.  Ghidra is **not** used and the game is opened
read-only.

What this tool proves (the full argument is in ``docs/CHASSIS.md``):

* the chassis table of a level is one of **four mutually exclusive** blocks
  (an if/else-if/else chain), 36/36 levels:

  ==============================  ======  ==========================
  guard                            rows    notes
  ==============================  ======  ==========================
  ``GOD_MODE`` (condition 26)          5   four campaign + ``57 CHEVY``
  ``DEATHMATCH_CLASSIC_GAME``          4   ``mass_limit`` lifted
  ``SCAVENGERHUNT_GAME``               4   ``mass_limit`` lifted
  ``else`` (campaign)                  4   ``mass_limit`` 500..1600
  ==============================  ======  ==========================

* ``57 CHEVY`` is therefore available **only** with ``/god`` (the cheat sets
  ``[0x62cad4]`` = 1 at ``0x44a9e8``; condition 26 reads it at ``0x45eb3f``);
* the chassis is *bought* in a headquarters shop (opcode 191
  ``SHOW_TANK_BUY_SCREEN`` -> ``0x45c157`` -> ``action_state = 4`` ->
  ``0x443b43`` -> ``0x47b990``) and *applied* by ``0x47a980`` (chosen type ->
  ``player+0x3464`` at ``0x47aa34``, price -> ``+0x3468`` at ``0x47ac7d``,
  eye height -> ``+0x2F50`` at ``0x47ad3d/0x47ad43``, model index -> the
  ``0x452240`` call at ``0x47bca5``).

Usage::

    python3 tools/re/chassis.py --self-test
    python3 tools/re/chassis.py --fields      # карта полей записи (VA)
    python3 tools/re/chassis.py --levels      # значения по 36 уровням
    python3 tools/re/chassis.py --assets      # модели, звуки, HUD-экран
    python3 tools/re/chassis.py --select      # сайты записи выбора шасси

All commands accept ``--json``; ``--levels`` accepts ``--level NAME``.
Exit code 0 = invariants hold (``--self-test`` fails otherwise).
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from gm import sbt as sbtmod       # noqa: E402
from gm import sbtvm               # noqa: E402

try:                                # exe bytes/listing: optional, RE-only
    import exelib                   # noqa: E402
except Exception:                   # pragma: no cover - no listing in a bare tree
    exelib = None

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
CACHE_GAME = os.path.expanduser("~/.cache/gunmetal-re/game")

#: The five chassis names that occur in any level script (order = HUD order).
CHASSIS = ("LIGHT CHASSIS", "MEDIUM CHASSIS", "STEALTH CHASSIS",
           "HEAVY CHASSIS", "57 CHEVY")
CAMPAIGN = CHASSIS[:4]
CHEVY = CHASSIS[4]

#: The eight chassis *models* every level preloads in ``SOB_TABLE``: four live
#: hulls and their four wrecks (`docs/FORMATS.md` §4).
SOB_CHASSIS = {15: "DEVSTTR", 16: "TICK", 17: "SPARTAN", 18: "STEALTH",
               21: "TICKX", 22: "SPARTANX", 23: "STEALTHX", 24: "DEVSTTRX"}
SOB_LIVE = (15, 16, 17, 18)
SOB_WRECK = (21, 22, 23, 24)
#: live hull index -> wreck index (`devsttr` is the odd one: +9, the rest +5).
WRECK_OF = {15: 24, 16: 21, 17: 22, 18: 23}

#: Guard -> (mode label).  Everything else in the if/else chain is "else".
MODES = ("GOD_MODE", "DEATHMATCH_CLASSIC_GAME", "SCAVENGERHUNT_GAME", "else")

#: `gm/sbtvm.TABLE_SIGS[172]` / `engine/src/game/player_types.h`: three
#: strings (``S``) followed by 54 numeric fields = 0x17c bytes.
SIGNATURE = sbtvm.TABLE_SIGS[172]
STRING_FIELDS = 3

#: Player block base (`docs/PLAYER_FIELDS.md` §1): every target below is an
#: absolute VA; the comment gives the offset from this base.
PLAYER_BASE = 0x819040

#: Record fields with a *proven consumer*: (operand index in the decoded
#: record, name, file dword, memory offset in the 0x1cc record, target VA in
#: the player block, reading VA, status, note).
#: Record offsets are taken from the store function `0x47a170`; targets and
#: VAs from the apply branch `0x47ac1a..0x47af3e`.
FIELDS = (
    (0, "mass_limit", 0, 0x148, 0x81C378, 0x47AC8F, "code",
     "предел массы -> 0x81c378; дальше лимит веса 0x4524d0"),
    (1, "speed_penalty", 1, 0x018, 0x81BD80, 0x47AC9B, "code",
     "штраф скорости на единицу массы сверх предела"),
    (2, "armor", 3, 0x14C, 0x81C28C, 0x47ACC5, "code",
     "броня: ×100 (lea ×5 дважды, shl 2); база цены щитов 0x479dfc"),
    (3, "armor_max", 4, 0x150, 0x81C284, 0x47ACE0, "code",
     "предел брони: ×100"),
    (4, "max_speed", 5, 0x020, 0x81BD70, 0x47AC77, "code",
     "базовая макс. скорость (double)"),
    (5, "model_index", 7, 0x154, 0x81C254, 0x47AF34, "code",
     "индекс SOB-модели: 0x47af34 -> arg 0x452240 -> 0x45240f"),
    (9, "eye_height", 14, 0x040, 0x81BF90, 0x47AD3D, "code",
     "высота глаза (double): +0x40/+0x44 -> 0x81bf90/+0x81bf94"),
    (10, "price", 16, 0x168, 0x81C4A8, 0x47AC7D, "code",
     "цена шасси; база формулы 0x479dd0"),
    (11, "sound_index", 17, 0x17C, 0x81C420, 0x47AD6C, "code",
     "индекс сэмпла в SOUND_SAMPLE_TABLE; имя -> 0x81c424 (0x47ad58)"),
)

#: Sites that write/read the *selection*.  `kind` is one of
#: shop/cursor/apply/model/persist.  `anchor` is the byte pattern at `va`.
SELECT_SITES = (
    (0x45C157, "shop", "опкод 191 SHOW_TANK_BUY_SCREEN: обработчик; берёт "
     "индекс игрока 0..7", "25ffff0000"),
    (0x45C19A, "shop", "обработчик 191: action_state = 4 (кадр -> витрина "
     "шасси)", "c70104000000"),
    (0x443B47, "shop", "кадровый switch 0x44382d (action_state-2, таблица "
     "0x4452d8): запись 4 -> call 0x47b990; результат -> player+0x3210",
     "e8447e0300"),
    (0x47B9EF, "cursor", "витрина шасси: зажим курсора (sel > count+3 -> 0)",
     "3987a0c48100"),
    (0x47B9F7, "cursor", "витрина шасси: курсор = 0", "89afa0c48100"),
    (0x47B9FD, "cursor", "витрина шасси: курсор < 0 -> count+3", "39afa0c48100"),
    (0x47BD61, "cursor", "бит 0x0001 (prev): курсор--", "ff8fa0c48100"),
    (0x47BD70, "cursor", "бит 0x0002 (next): курсор++", "ff87a0c48100"),
    (0x47B2C1, "cursor", "выход из магазина: курсор сравнивается с числом "
     "записей 0x7941bc", "3b05bc417900"),
    (0x47BC19, "shop", "цена: call 0x479dd0(курсор)", "e8b2e1ffff"),
    (0x47BC34, "shop", "цена ещё раз перед списанием: call 0x479dd0(курсор)",
     "e897e1ffff"),
    (0x47BCA5, "apply", "покупка принята: call 0x47a980(player, курсор, "
     "маски щитов/стволов/экипировки)", "e8d6ecffff"),
    (0x47AA34, "apply", "0x47a980: выбранный тип -> player+0x3464",
     "89bba4c48100"),
    (0x47AC7D, "apply", "0x47a980: цена -> player+0x3468", "8983a8c48100"),
    (0x47AD3D, "apply", "0x47a980: младшее слово eye height -> player+0x2F50",
     "898390bf8100"),
    (0x47AF34, "apply", "0x47a980: индекс модели записи (+0x154) в аргумент "
     "0x452240", "8b9234977900"),
    (0x45240F, "model", "0x452240: индекс модели -> player+0x3214",
     "899e54c28100"),
    (0x45228E, "model", "0x452240: старый объект сброшен, индекс = -1",
     "c78654c28100ffffffff"),
    (0x452FE6, "model", "старт уровня/join: имя крафта из кэша 0x5a95e8 -> "
     "0x47c940 -> 0x452240", "c1e204"),
    (0x452DC2, "apply", "респавн: тип -> player+0x3464 до call 0x47a980",
     "8983a4c48100"),
    (0x4530B9, "apply", "после 0x452240: id объекта записи (+0x194) -> "
     "player+0x44c", "8b0c8574977900"),
    (0x467258, "persist", "загрузка сейва: курсор (+0x3460) из блока",
     "8988a0c48100"),
    (0x468233, "persist", "сейв/сеть: индекс модели (+0x3214)", "898654c28100"),
    (0x465866, "persist", "сеть: байт курсора (+0x3460) в пакет 0x61ec9d",
     "8a86a0c48100"),
)

#: GOD_MODE plumbing: condition 26, the `/god` cheat and the chassis count.
GOD_SITES = (
    (0x45EB3F, "условие 26 GOD_MODE: mov eax, ds:0x62cad4", "a1d4ca6200"),
    (0x44A9E8, "чит /god: [0x62cad4] = 1 (единственная запись)",
     "c705d4ca620001000000"),
    (0x47B9BB, "витрина шасси: строк = проекты R.P.V. + 4 щита + "
     "DAT_007941bc (счётчик записей)", "8b0dbc417900"),
)

#: Runtime tables used by the tool (absolute addresses, player 0 base 0x819040).
TABLES = (
    (0x7995E0, "записи DEFINE_PLAYER_TYPES в памяти, шаг 0x1cc, до 32 шт."),
    (0x7941BC, "счётчик записей DEFINE_PLAYER_TYPES (<= 32)"),
    (0x81C4A0, "player+0x3460 — курсор магазина/выбранный индекс"),
    (0x81C4A4, "player+0x3464 — текущий тип шасси (запись)"),
    (0x81C4A8, "player+0x3468 — цена текущего шасси"),
    (0x81BF90, "player+0x2F50 — высота глаза (double, из поля 9)"),
    (0x81C254, "player+0x3214 — индекс модели в SOB_TABLE"),
    (0x5A95E8, "имя крафта по игрокам, 8 слотов × 0x10 байт"),
)

REF_RANGES = ((0x47A170, 0x47A730), (0x47AC1A, 0x47AF3E),
              (0x452F00, 0x453200), (0x47B2A0, 0x47B340),
              (0x479DD0, 0x47A000))


# --------------------------------------------------------------------------
# level data
# --------------------------------------------------------------------------
def _collect(insns, code, ifs, blocks, elses):
    """Collect every ``IF`` range, ``ELSE`` target and chassis block by offset.

    The script decoder expands ``ELSE`` bodies as copies of the following
    siblings, so everything is de-duplicated by offset and each block is later
    attributed to the *innermost* ``IF`` whose byte range contains it.
    """
    for ins in insns:
        if ins.name == "IF" and ins.off not in ifs:
            cid = struct.unpack_from("<B", code, ins.off + 1)[0]
            _name, sig = sbtvm.CONDITIONS.get(cid, ("OP_%d" % cid, None))
            _args, j = sbtvm._rdargs(code, ins.off + 2, sbtvm._letters(sig))
            size = struct.unpack_from("<I", code, j)[0]
            ifs[ins.off] = (ins.args[0][0], j + 4, j + size)
        if ins.name == "ELSE" and ins.args:
            elses.setdefault(ins.off, ins.args[0][1])
        if ins.name == "DEFINE_PLAYER_TYPES":
            blocks.setdefault(ins.off, ins)
        if isinstance(ins.body, list):
            _collect(ins.body, code, ifs, blocks, elses)


def walk_blocks(code, insns):
    """``(guard, insn)`` for every chassis block, in script order.

    Runtime layout (proven by the jump targets, ``docs/CHASSIS.md`` §1): an
    ``if / else if / else`` chain, so **at most one** block runs.  The guard is
    the innermost enclosing ``IF`` condition, ``else`` for the trailing block.
    """
    ifs, blocks, elses = {}, {}, {}
    _collect(insns, code, ifs, blocks, elses)
    out = []
    for off in sorted(blocks):
        inner = [(start, end, name) for name, start, end in ifs.values()
                 if start <= off < end]
        if inner:
            guard = max(inner)[2]
        else:
            guard = "else" if ifs else "unconditional"
        out.append((guard, blocks[off]))
    return out


def chain_structure(code, insns, chain):
    """Jump targets of the chassis ``IF``\\ s: all must land on the same join.

    That is what makes the four blocks mutually exclusive: the then-branch
    jumps to the join, a false condition lands right after the ``ELSE`` insn
    (i.e. on the next branch), so exactly one branch runs.
    """
    ifs, blocks, elses = {}, {}, {}
    _collect(insns, code, ifs, blocks, elses)
    guard_offs = []
    for off in sorted(blocks):
        inner = [(ioff, v[1], v[2]) for ioff, v in ifs.items()
                 if v[1] <= off < v[2]]
        guard_offs.append(max(inner)[0] if inner else None)
    targets = []
    for start in guard_offs:
        if start is None:
            continue
        body_end = ifs[start][2]
        # the compiled layout puts the `jmp join` (opcode 200) in the last
        # five bytes of the then-branch: IF cond { body } -> ELSE at end-5
        hit = [tgt for off, tgt in elses.items() if off == body_end - 5]
        targets.append(hit[0] if hit else None)
    last = max(blocks) if blocks else 0
    join = targets[0] if targets and len(set(targets)) == 1 else None
    return {"else_targets": targets, "join": join,
            "join_after_last_block": join is not None and join > last}


def record_row(strings, vals, sounds, shield_price):
    """One chassis record in a readable form (all numbers kept raw too)."""
    snd = vals[11]
    snd_name = None
    if snd in (-1, 0xFFFFFFFF):
        snd_name = None
    elif isinstance(snd, int) and 0 <= snd < len(sounds):
        snd_name = sounds[snd]
    elif isinstance(snd, int):
        snd_name = "?%d" % snd
    price = vals[10]
    return {
        "name": strings[0], "hull": strings[1], "icon": strings[2],
        "mass_limit": vals[0], "speed_penalty": vals[1], "armor": vals[2],
        "armor_max": vals[3], "max_speed": vals[4], "model_index": vals[5],
        "eye_height": vals[9], "price": price, "price_shop":
            (price + 4 * vals[2] * shield_price
             if isinstance(shield_price, int) else None),
        "sound_index": snd, "sound": snd_name,
        "values": [v for v in vals],
    }


def level_info(path):
    """Parse one ``.sbt``: chassis chain, sound names, SOB names, shields."""
    sbt = sbtmod.parse_file(path)
    script = sbt.records_of(11)[0].payload
    insns = sbtvm.decode(script.code)[0]
    sounds = list(sbt.records_of(12)[0].payload.names)
    sob = [e.name.upper() for e in sbt.records_of(20)[0].payload.entries]
    shield_price = None
    for ins in insns:
        if ins.name == "DEFINE_SHIELDS" and ins.args:
            shield_price = ins.args[0]
            break
    chain = []
    for guard, block in walk_blocks(script.code, insns):
        rows = [record_row(strings, vals, sounds, shield_price)
                for strings, vals in block.data]
        chain.append({"guard": guard, "mode": guard,
                      "offset": block.off, "count": len(rows), "rows": rows})
    structure = chain_structure(script.code, insns, chain)
    if structure["join"] is not None:
        structure["blocks_after_join"] = [
            b["offset"] for b in chain if b["offset"] > structure["join"]]
    return {"name": os.path.splitext(os.path.basename(path))[0],
            "chain": chain, "sounds": sounds, "sob": sob,
            "shield_price": shield_price, "structure": structure}


def all_levels(game):
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    return [level_info(p) for p in files]


def pick_game(explicit):
    if explicit:
        return explicit
    if glob.glob(os.path.join(CACHE_GAME, "*.sbt")):
        return CACHE_GAME
    return DEFAULT_GAME


def _collapse(levels, mode, name):
    """``(set of value-tuples, block count)`` for one (mode, chassis)."""
    seen = {}
    n = 0
    for lv in levels:
        for blk in lv["chain"]:
            if blk["mode"] != mode:
                continue
            for row in blk["rows"]:
                if row["name"] != name:
                    continue
                key = (row["mass_limit"], row["speed_penalty"], row["armor"],
                       row["armor_max"], row["max_speed"], row["model_index"],
                       row["eye_height"], row["price"], row["sound"])
                seen[key] = seen.get(key, 0) + 1
                n += 1
    return seen, n


# --------------------------------------------------------------------------
# --fields
# --------------------------------------------------------------------------
def _record_refs():
    """``[(record offset, [VAs])]`` scanned from the consumers (needs exelib)."""
    if exelib is None:
        return []
    base = 0x7995E0
    hits = {}
    pattern = re.compile(r"0x(799[0-9a-f]{3})")
    for lo, hi in REF_RANGES:
        for line in exelib.disasm(lo, hi):
            if "0x799" not in line:
                continue
            head, _, rest = line.partition(":")
            try:
                va = int(head.strip(), 16)
            except ValueError:
                continue
            for token in pattern.findall(rest):
                off = int(token, 16) - base
                if 0 <= off < 0x1CC:
                    hits.setdefault(off, []).append(va)
    return sorted(hits.items())


def cmd_fields(args):
    named = {}
    for idx, name, dword, mem, player, va, status, note in FIELDS:
        named[mem] = (idx, name, dword, player, va, status, note)
    refs = _record_refs()
    out = {
        "signature": SIGNATURE,
        "fields": [
            {"operand": idx, "name": name, "dword": dword,
             "record_offset": mem, "player_va": player,
             "player_offset": player - PLAYER_BASE, "va": va,
             "status": status, "note": note}
            for idx, name, dword, mem, player, va, status, note in FIELDS],
        "record_refs": [{"offset": off, "vas": vas} for off, vas in refs],
    }
    if args.json:
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0

    print("сигнатура записи: %d полей = %d строки + %d чисел, 95 dword, "
          "0x17c байт:" % (len(SIGNATURE), STRING_FIELDS,
                           len(SIGNATURE) - STRING_FIELDS))
    print("  " + SIGNATURE)
    print("\nполя с доказанным потребителем (запись -> игрок):")
    print("  %-3s %-14s %-6s %-8s %-18s %-9s %s" % (
        "op", "поле", "dword", "запись", "игрок (VA)", "чтение", "что делает"))
    for idx, name, dword, mem, player, va, status, note in FIELDS:
        print("  %-3d %-14s %-6d +0x%03x   0x%06x (+0x%04x) 0x%06x %s" % (
            idx, name, dword, mem, player, player - PLAYER_BASE, va, note))
    print("\nвсе считываемые смещения записи (скан потребителей, exelib):")
    if not refs:
        print("  нет листинга/копии exe — скан пропущен")
    else:
        line = []
        for off, vas in refs:
            mark = "*" if off in named else " "
            line.append("%s+0x%03x<-0x%06x" % (mark, off, vas[0]))
            if len(line) == 3:
                print("  " + "  ".join(line))
                line = []
        if line:
            print("  " + "  ".join(line))
        print("  (* — поле из таблицы выше; остальные копируются блоками "
              "0x47acef..0x47af34 как «физика» шасси)")
    print("\nтаблицы в памяти:")
    for va, what in TABLES:
        print("  0x%06x  %s" % (va, what))
    return 0


# --------------------------------------------------------------------------
# --levels
# --------------------------------------------------------------------------
def chain_text(level):
    return " ".join("%s:%d" % (b["mode"], b["count"]) for b in level["chain"])


def cmd_levels(args):
    game = pick_game(args.game)
    t0 = time.time()
    levels = all_levels(game)
    if not levels:
        print("нет .sbt в %s" % game, file=sys.stderr)
        return 1
    if args.level:
        want = {n.lower() for n in args.level}
        levels = [lv for lv in levels if lv["name"].lower() in want]
        if not levels:
            print("уровень не найден: %s" % ", ".join(args.level),
                  file=sys.stderr)
            return 1

    if args.json:
        payload = {"game": game, "levels": levels,
                   "values": _values_summary(levels),
                   "seconds": round(time.time() - t0, 3)}
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        return 0

    print("игра: %s  (%d уровней, %.2f с)" % (game, len(levels),
                                             time.time() - t0))
    print("блоки DEFINE_PLAYER_TYPES: %d" % sum(len(lv["chain"])
                                                for lv in levels))
    print("\n%s" % ("-" * 96))
    for lv in levels:
        chevy = any(r["name"] == CHEVY for b in lv["chain"] for r in b["rows"])
        print("%-9s %-48s CHEVY=%s" % (lv["name"], chain_text(lv),
                                       "да" if chevy else "НЕТ"))
        print("          %s" % _level_summary(lv))
    print("%s" % ("-" * 96))
    print("\nзначения по (режим, шасси): сколько блоков и сколько вариантов")
    values = _values_summary(levels)
    for item in values:
        variants = "; ".join(
            "%s ×%d" % ("/".join(_fmt(v) for v in var["values"]),
                        var["blocks"])
            for var in item["variants"])
        print("  %-22s %-14s %3d блоков  %s" % (
            item["mode"], item["name"], item["blocks"], variants))
    print("\nэталон (кампания, else-блок):")
    ref = next((lv for lv in levels if lv["name"] == "level0"), levels[0])
    campaign = [b for b in ref["chain"] if b["mode"] == "else"]
    if campaign:
        _print_rows(campaign[0])
    return 0


def _level_summary(level):
    """Campaign block + CHEVY: eye heights and shop prices, short."""
    camp = next((b for b in level["chain"] if b["mode"] == "else"), None)
    god = next((b for b in level["chain"] if b["mode"] == "GOD_MODE"), None)
    rows = camp["rows"] if camp else []
    chevy = next((r for r in (god["rows"] if god else [])
                  if r["name"] == CHEVY), None)
    eyes = "/".join("%.0f" % r["eye_height"] for r in rows)
    prices = "/".join(str(r["price_shop"]) for r in rows)
    if chevy:
        eyes += "/%.0f" % chevy["eye_height"]
        prices += "/%d" % chevy["price_shop"]
    return "eye %s | магазин %s (кампания%s)" % (
        eyes, prices, " + 57 CHEVY" if chevy else "")


def _print_rows(block):
    print("  %-18s %-8s %-8s %5s %7s %5s %4s %3s %6s %7s %-7s" % (
        "шасси", "hull", "icon", "масса", "штраф", "броня", "arm", "мод",
        "eye", "цена", "звук"))
    for row in block["rows"]:
        print("  %-18s %-8s %-8s %5d %7.4f %5d %4d %3d %6.1f %7d %-7s" % (
            row["name"], row["hull"], row["icon"], row["mass_limit"],
            row["speed_penalty"], row["armor"], row["armor_max"],
            row["model_index"], row["eye_height"], row["price"],
            row["sound"] or "-"))


def _fmt(value):
    if isinstance(value, float):
        return ("%.4f" % value).rstrip("0").rstrip(".")
    return str(value)


def _values_summary(levels):
    out = []
    modes = [m for m in MODES if any(b["mode"] == m for lv in levels
                                      for b in lv["chain"])]
    for mode in modes:
        for name in CHASSIS:
            seen, blocks = _collapse(levels, mode, name)
            if not blocks:
                continue
            out.append({"mode": mode, "name": name, "blocks": blocks,
                        "variants": [{"values": list(key), "blocks": n}
                                     for key, n in sorted(seen.items())]})
    return out


# --------------------------------------------------------------------------
# --assets
# --------------------------------------------------------------------------
def campaign_reference(levels):
    """The level-0 campaign block (``else``), else the first campaign block."""
    for lv in levels:
        if lv["name"] == "level0":
            for blk in lv["chain"]:
                if blk["mode"] == "else":
                    return lv, blk
    for lv in levels:
        for blk in lv["chain"]:
            if blk["mode"] == "else":
                return lv, blk
    return levels[0], levels[0]["chain"][0]


def _find(dirname, name):
    """Case-insensitive lookup (the shipped tree mixes ``TANK1.SOF``/``tank0.sof``)."""
    want = name.lower()
    try:
        for entry in os.listdir(dirname):
            if entry.lower() == want:
                return os.path.join(dirname, entry)
    except OSError:
        pass
    return None


def _files_like(dirname, prefix, suffix):
    try:
        return [e for e in os.listdir(dirname)
                if e.lower().startswith(prefix) and e.lower().endswith(suffix)]
    except OSError:
        return []


def cmd_assets(args):
    game = pick_game(args.game)
    levels = all_levels(game)
    if not levels:
        print("нет .sbt в %s" % game, file=sys.stderr)
        return 1
    ref_level, ref_block = campaign_reference(levels)
    god_block = None
    for blk in ref_level["chain"]:
        if blk["mode"] == "GOD_MODE":
            god_block = blk
    rows = list(ref_block["rows"])
    if god_block:
        rows += [r for r in god_block["rows"] if r["name"] == CHEVY]

    sob_sets = {tuple(lv["sob"]) for lv in levels}
    index_sets = {}
    for lv in levels:
        for blk in lv["chain"]:
            for r in blk["rows"]:
                if r["name"] == CHEVY and blk["mode"] != "GOD_MODE":
                    continue
                key = (r["name"], r["model_index"], r["sound_index"],
                       r["sound"])
                index_sets[key] = index_sets.get(key, 0) + 1

    files = {}
    for name in ("Chevy", "Devsttr", "Devsttrx", "Tick", "Tickx", "Spartan",
                 "Spartanx", "Stealth", "Stealthx"):
        path = _find(os.path.join(game, "SOBS"), name + ".sob")
        files[name] = os.path.getsize(path) if path else None
    fdx = _find(os.path.join(game, "Film"), "tank.fdx")
    fdx_rows = 0
    if fdx:
        with open(fdx, "rb") as fh:
            fdx_rows = sum(1 for line in fh if line.strip())
    tank_sof = _files_like(os.path.join(game, "Film"), "tank", ".sof")
    sml = _files_like(os.path.join(game, "SML", "design"), "tank", ".sml")

    out = {
        "level": ref_level["name"],
        "rows": rows,
        "sob_chassis": SOB_CHASSIS,
        "sob_index_variants": [{"chassis": k[0], "model": k[1],
                                "sound_index": k[2], "sound": k[3],
                                "blocks": v}
                               for k, v in index_sets.items()],
        "sob_tables_identical": len(sob_sets) == 1,
        "files": files,
        "hud": {"tank_fdx_rows": fdx_rows, "tank_sof": len(tank_sof),
                "design": len(sml)},
    }
    if args.json:
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0

    print("игра: %s (эталон — %s, блок %s)" % (game, ref_level["name"],
                                               ref_block["mode"]))
    print("\nассеты шасси:")
    print("  %-18s %-6s %-8s %-9s %-8s %-8s %-7s %s" % (
        "шасси", "мод.", "hull", "обломок", "звук", "индекс", "файл .sob",
        "сэмплов"))
    for row in rows:
        hull = row["hull"].upper()
        wreck = SOB_CHASSIS.get(WRECK_OF.get(row["model_index"], -1), "-")
        name = hull.capitalize() if hull != "DEVSTTR" else "Devsttr"
        size = files.get(name)
        counts = {k[2] for k in index_sets if k[0] == row["name"] and k[3] == row["sound"]}
        print("  %-18s %-6d %-8s %-9s %-8s %-8s %-7s %s" % (
            row["name"], row["model_index"], hull, wreck,
            row["sound"] or "-",
            "-1" if row["sound_index"] in (-1, 0xFFFFFFFF)
            else row["sound_index"],
            ("%d Б" % size) if size else "нет", sorted(counts)))
    print("\n8 моделей шасси в SOB_TABLE уровня (одинаковы во всех %d):" %
          len(levels))
    for idx in sorted(SOB_CHASSIS):
        kind = "живой" if idx in SOB_LIVE else "обломок"
        ok = all(lv["sob"][idx] == SOB_CHASSIS[idx]
                 for lv in levels if idx < len(lv["sob"]))
        print("  %2d  %-9s %-8s %s" % (idx, SOB_CHASSIS[idx], kind,
                                       "36/36" if ok else "РАСХОЖДЕНИЕ"))
    chevy_in_sob = sum(1 for lv in levels if "CHEVY" in lv["sob"])
    print("  CHEVY в SOB_TABLE: %d/%d (модель не адресуется по имени)" %
          (chevy_in_sob, len(levels)))
    print("\nHUD-экран магазина шасси: Film/tank.fdx — %d строк; "
          "TANK*.SOF — %d; SML/design/TANK* — %d" %
          (fdx_rows, len(tank_sof), len(sml)))
    print("слоты имён крафта: 0x5a95e8, 8 × 0x10 байт (по числу игроков, "
          "не шасси)")
    return 0


# --------------------------------------------------------------------------
# --select
# --------------------------------------------------------------------------
def _bytes(va, size):
    if exelib is None:
        return None
    off = exelib.va2off(va)
    if off is None:
        return None
    return exelib.data()[off:off + size]


def cmd_select(args):
    groups = (("магазин/вход", "shop", SELECT_SITES),
              ("курсор выбора", "cursor", SELECT_SITES),
              ("применение записи", "apply", SELECT_SITES),
              ("модель", "model", SELECT_SITES),
              ("сохранение/сеть", "persist", SELECT_SITES))
    out = {"sites": [{"va": va, "kind": kind, "what": what,
                      "anchor": anchor}
                     for va, kind, what, anchor in SELECT_SITES],
           "god": [{"va": va, "what": what, "anchor": anchor}
                   for va, what, anchor in GOD_SITES],
           "tables": [{"va": va, "what": what} for va, what in TABLES]}
    if args.json:
        for item in out["sites"] + out["god"]:
            got = _bytes(item["va"], len(item["anchor"]) // 2)
            item["bytes"] = got.hex(" ") if got else None
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0

    print("поток выбора шасси (штаб -> магазин -> применение -> модель):")
    print("  опкод 191 -> action_state=4 -> 0x443b43 -> 0x47b990 "
          "(курсор +0x3460)\n"
          "  бит 0x0008 «купить»: 0x479dd0(курсор) -> cash -= цена -> "
          "0x47a980(player, курсор)\n"
          "  0x47a980: тип -> +0x3464 (0x47aa34), поля записи -> игрок, "
          "цена -> +0x3468\n"
          "  модель: +0x154 -> 0x452240 -> player+0x3214 (0x45240f)")
    for title, kind, sites in groups:
        print("\n%s:" % title)
        for va, s_kind, what, anchor in sites:
            if s_kind != kind:
                continue
            got = _bytes(va, len(anchor) // 2)
            if got is None:
                print("  0x%06x  %-46s (нет exe)" % (va, what))
                continue
            state = "OK" if got.hex() == anchor else "!! %s" % got.hex(" ")
            print("  0x%06x  %-46s [%s]" % (va, what, state))
    print("\n57 CHEVY / GOD_MODE:")
    for va, what, anchor in GOD_SITES:
        got = _bytes(va, len(anchor) // 2)
        state = ("OK" if got is not None and got.hex() == anchor
                 else "нет exe" if got is None else "!! %s" % got.hex(" "))
        print("  0x%06x  %-58s [%s]" % (va, what, state))
    print("\nтаблицы (адреса абсолютные, база игрока 0 — 0x819040):")
    for va, what in TABLES:
        print("  0x%06x  %s" % (va, what))
    return 0


# --------------------------------------------------------------------------
# --self-test
# --------------------------------------------------------------------------
_QUIET = False


def _check(results, name, ok, detail=""):
    results.append((name, bool(ok), detail))
    if not _QUIET:
        print("  %-4s %-52s %s" % ("ok" if ok else "FAIL", name, detail))
    return bool(ok)


def cmd_self_test(args):
    global _QUIET
    _QUIET = bool(args.json)
    game = pick_game(args.game)
    results = []
    if not _QUIET:
        print("self-test: %s" % game)
    t0 = time.time()
    levels = all_levels(game)
    _check(results, "уровни найдены (36)", len(levels) == 36,
           "%d" % len(levels))
    if len(levels) != 36:
        return 1

    blocks = sum(len(lv["chain"]) for lv in levels)
    rows_total = sum(b["count"] for lv in levels for b in lv["chain"])
    _check(results, "блоки/записи DEFINE_PLAYER_TYPES = 145/616",
           blocks == 145 and rows_total == 616,
           "%d блоков, %d записей" % (blocks, rows_total))

    per_mode = {}
    for lv in levels:
        for blk in lv["chain"]:
            per_mode.setdefault((blk["mode"], blk["count"]), 0)
            per_mode[(blk["mode"], blk["count"])] += 1
    expect = {("GOD_MODE", 5): 36, ("DEATHMATCH_CLASSIC_GAME", 4): 36,
              ("SCAVENGERHUNT_GAME", 4): 36, ("else", 4): 37}
    _check(results, "цепочка GOD:5 DM:4 SCAV:4 else:4 (36/36)",
           per_mode == expect, str(sorted(per_mode.items())))

    bad = [lv["name"] for lv in levels
           if [b["mode"] for b in lv["chain"]][:3] !=
           ["GOD_MODE", "DEATHMATCH_CLASSIC_GAME", "SCAVENGERHUNT_GAME"]]
    _check(results, "порядок ветвей одинаков во всех уровнях", not bad,
           ",".join(bad[:4]))

    # if/else-if: every branch's ELSE jumps to the same join, past the last
    # block -> exactly one of the four blocks runs (no overwrite).
    bad = []
    exceptions = []
    for lv in levels:
        st = lv["structure"]
        if st.get("blocks_after_join"):
            exceptions.append(lv["name"])
            continue
        if not st["join_after_last_block"] or len(set(st["else_targets"])) != 1:
            bad.append("%s:%s" % (lv["name"], st["else_targets"]))
    ref = next((lv for lv in levels if lv["name"] == "level0"), levels[0])
    _check(results, "ветви взаимоисключающие (ELSE -> общий join за блоками)",
           not bad and exceptions == ["level20"],
           "35/36, level0 join 0x%x; level20 — записи в следующих подскриптах" %
           ref["structure"]["join"] if not bad else "; ".join(bad[:3]))

    chevy_modes = sorted({blk["mode"] for lv in levels for blk in lv["chain"]
                          for r in blk["rows"] if r["name"] == CHEVY})
    chevy_blocks = sum(1 for lv in levels for blk in lv["chain"]
                       for r in blk["rows"] if r["name"] == CHEVY)
    _check(results, "57 CHEVY только под GOD_MODE, 36 блоков",
           chevy_modes == ["GOD_MODE"] and chevy_blocks == 36,
           "%s ×%d" % (chevy_modes, chevy_blocks))

    # values: one variant per (mode, chassis) everywhere
    problems = []
    for item in _values_summary(levels):
        if len(item["variants"]) != 1:
            problems.append("%s/%s: %d вариантов" % (
                item["mode"], item["name"], len(item["variants"])))
    _check(results, "значения записи без вариантов (36/36 на режим)",
           not problems, "; ".join(problems[:3]))

    reference = {
        "LIGHT CHASSIS": (500, 0.003, 80, 1.7, 16, 10.0, 10000, "thuml"),
        "MEDIUM CHASSIS": (1000, 0.0006, 130, 1.0, 17, 11.0, 30000, "thumm"),
        "STEALTH CHASSIS": (500, 0.001, 90, 1.0, 18, 11.0, 350000, None),
        "HEAVY CHASSIS": (1600, 0.0002, 200, 0.6, 15, 12.0, 80000, "thumh"),
        "57 CHEVY": (9999, 0.0001, 300, 1.7, 15, 14.0, 500000, "thumoil"),
    }
    got = {}
    for item in _values_summary(levels):
        if item["mode"] not in ("else", "GOD_MODE"):
            continue
        key = (item["name"])
        if key in got:
            continue
        v = item["variants"][0]["values"]
        got[key] = (v[0], round(v[1], 6), v[2], round(v[4], 6), v[5],
                    v[6], v[7], v[8])
    _check(results, "эталонные числа (масса/штраф/броня/скорость/модель/eye/"
           "цена/звук)", got == reference,
           "; ".join("%s:%s" % (k, v) for k, v in sorted(got.items())
                     if reference.get(k) != v)[:200] or "5/5")

    eye = {n: got[n][5] for n in got}
    _check(results, "eye height 10/11/11/12/14 (поле 9)",
           eye == {"LIGHT CHASSIS": 10.0, "MEDIUM CHASSIS": 11.0,
                   "STEALTH CHASSIS": 11.0, "HEAVY CHASSIS": 12.0,
                   "57 CHEVY": 14.0}, str(eye))

    # mass limit is the only difference between campaign and DM blocks
    dm = {}
    for item in _values_summary(levels):
        if item["mode"] == "DEATHMATCH_CLASSIC_GAME":
            dm[item["name"]] = item["variants"][0]["values"][0]
    _check(results, "DM/SCAV: снят предел массы (500000 у 4 шасси)",
           set(dm.values()) == {500000}, str(dm))

    # shop price = base + 4*armor*price_per_100_shields, 36/36
    bad = []
    for lv in levels:
        sp = lv["shield_price"]
        if sp is None:
            continue
        for blk in lv["chain"]:
            for r in blk["rows"]:
                want = r["price"] + 4 * r["armor"] * sp
                if r["price_shop"] != want:
                    bad.append((lv["name"], r["name"]))
    _check(results, "цена магазина = база + 4*броня*цена_щитов", not bad,
           "%d проверок, ошибок %d" % (len(levels) * 4, len(bad)))
    shop_prices = {}
    for lv in levels:
        if lv["name"] != "level0":
            continue
        for blk in lv["chain"]:
            if blk["mode"] != "else":
                continue
            for r in blk["rows"]:
                shop_prices[r["name"]] = r["price_shop"]
    _check(results, "цены магазина level0: 26000/56000/368000/120000",
           [shop_prices.get(n) for n in CAMPAIGN] ==
           [26000, 56000, 368000, 120000], str(shop_prices))

    # SOB_TABLE: 8 chassis names, fixed indices, no CHEVY
    sob_bad = []
    for lv in levels:
        for idx, name in SOB_CHASSIS.items():
            if idx >= len(lv["sob"]) or lv["sob"][idx] != name:
                sob_bad.append((lv["name"], idx))
        if "CHEVY" in lv["sob"]:
            sob_bad.append((lv["name"], "CHEVY"))
    _check(results, "8 моделей шасси по индексам 15-18/21-24, CHEVY нет",
           not sob_bad, "%d ошибок" % len(sob_bad))

    # op5 model index == SOB index of the hull name
    model_bad = []
    for lv in levels:
        for blk in lv["chain"]:
            for r in blk["rows"]:
                idx = r["model_index"]
                if idx >= len(lv["sob"]) or lv["sob"][idx] != r["hull"].upper():
                    model_bad.append((lv["name"], r["name"]))
    _check(results, "op5 указывает на SOB-индекс своего hull (все блоки)",
           not model_bad, "%d ошибок" % len(model_bad))

    # sounds: name constant, index < table or -1 for STEALTH
    snd_bad = []
    variants = {}
    for lv in levels:
        for blk in lv["chain"]:
            for r in blk["rows"]:
                s = r["sound_index"]
                if r["name"] == "STEALTH CHASSIS":
                    if s != 0xFFFFFFFF:
                        snd_bad.append((lv["name"], r["name"], s))
                    continue
                if not (isinstance(s, int) and 0 <= s < len(lv["sounds"])):
                    snd_bad.append((lv["name"], r["name"], s))
                    continue
                if lv["sounds"][s] != r["sound"]:
                    snd_bad.append((lv["name"], r["name"], r["sound"]))
                variants.setdefault(r["name"], set()).add(r["sound"])
    _check(results, "звук шасси: индекс -> имя, STEALTH -1", not snd_bad,
           "%d ошибок; имена %s" % (
               len(snd_bad), {k: sorted(v) for k, v in variants.items()}))
    _check(results, "имена сэмплов постоянны (thuml/thumm/thumh/thumoil)",
           all(len(v) == 1 for v in variants.values()) and
           {next(iter(v)) for v in variants.values()} ==
           {"thuml", "thumm", "thumh", "thumoil"},
           "%d вариантов" % sum(len(v) for v in variants.values()))

    # assets on disk
    chevy_sob = _find(os.path.join(game, "SOBS"), "Chevy.sob")
    live = all(_find(os.path.join(game, "SOBS"), n + ".sob")
               for n in ("Devsttr", "Tick", "Spartan", "Stealth"))
    wrecks = all(_find(os.path.join(game, "SOBS"), n + ".sob")
                 for n in ("Devsttrx", "Tickx", "Spartanx", "Stealthx"))
    _check(results, "файлы SOBS: 4 живых + 4 обломка", live and wrecks,
           "живые=%s обломки=%s" % (live, wrecks))
    _check(results, "SOBS/Chevy.sob есть, но в SOB_TABLE не адресуется",
           bool(chevy_sob) and not sob_bad,
           "%d байт" % (os.path.getsize(chevy_sob) if chevy_sob else 0))

    fdx = _find(os.path.join(game, "Film"), "tank.fdx")
    rows = 0
    if fdx:
        with open(fdx, "rb") as fh:
            rows = sum(1 for line in fh if line.strip())
    tank_sof = _files_like(os.path.join(game, "Film"), "tank", ".sof")
    _check(results, "HUD: tank.fdx 8 строк, TANK*.SOF 4 файла",
           rows == 8 and len(tank_sof) == 4, "строк=%d файлов=%d" % (
               rows, len(tank_sof)))

    # exe anchors
    if exelib is None:
        if not _QUIET:
            print("  skip проверка байтов exe (нет exelib/листинга)")
    else:
        bad = []
        for va, kind, what, anchor in SELECT_SITES:
            got_bytes = _bytes(va, len(anchor) // 2)
            if got_bytes is None or got_bytes.hex() != anchor:
                bad.append(hex(va))
        for va, what, anchor in GOD_SITES:
            got_bytes = _bytes(va, len(anchor) // 2)
            if got_bytes is None or got_bytes.hex() != anchor:
                bad.append(hex(va))
        _check(results, "байтовые якоря выбора/применения (24+3)",
               not bad, "не совпало: %s" % ",".join(bad) if bad else "27/27")

        def call_target(va):
            raw = _bytes(va, 5)
            if not raw or raw[0] != 0xE8:
                return None
            return va + 5 + struct.unpack_from("<i", raw, 1)[0]
        _check(results, "call 0x47bca5 -> 0x47a980, 0x443b47 -> 0x47b990",
               call_target(0x47BCA5) == 0x47A980 and
               call_target(0x443B47) == 0x47B990,
               "%s %s" % (hex(call_target(0x47BCA5) or 0),
                          hex(call_target(0x443B47) or 0)))

        refs = _record_refs()
        _check(results, "скан потребителей записи: >= 80 смещений",
               len(refs) >= 80, "%d смещений" % len(refs))
        named = {mem for _, _, _, mem, _, _, _, _ in FIELDS}
        missing = sorted(named - {off for off, _ in refs})
        _check(results, "все именованные поля найдены в потребителях",
               not missing,
               "нет: %s" % ",".join("+0x%03x" % x for x in missing))

    ok = all(r[1] for r in results)
    passed = sum(1 for r in results if r[1])
    if args.json:
        print(json.dumps({"game": game, "passed": passed,
                          "total": len(results), "ok": ok,
                          "seconds": round(time.time() - t0, 3),
                          "checks": [{"name": n, "ok": o, "detail": d}
                                     for n, o, d in results]},
                         indent=2, ensure_ascii=False))
    else:
        print("\n%d/%d проверок, %.2f с -> %s" % (
            passed, len(results), time.time() - t0,
            "OK" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Chassis selection, 57 CHEVY and chassis assets "
                    "(RE only, no Ghidra)")
    ap.add_argument("--self-test", action="store_true", help="проверки")
    ap.add_argument("--fields", action="store_true",
                    help="карта полей записи с VA")
    ap.add_argument("--levels", action="store_true",
                    help="значения DEFINE_PLAYER_TYPES по 36 уровням")
    ap.add_argument("--assets", action="store_true",
                    help="модели/звуки/HUD шасси")
    ap.add_argument("--select", action="store_true",
                    help="сайты записи выбранного шасси")
    ap.add_argument("--level", action="append", default=[],
                    help="ограничить --levels одним уровнем (повторяемо)")
    ap.add_argument("--game", help="каталог игры (по умолчанию кэш, затем "
                                   "установка)")
    ap.add_argument("--json", action="store_true", help="вывод в JSON")
    args = ap.parse_args(argv)

    if args.self_test:
        return cmd_self_test(args)
    if args.fields:
        return cmd_fields(args)
    if args.select:
        return cmd_select(args)
    if args.assets:
        return cmd_assets(args)
    if args.levels:
        return cmd_levels(args)
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
