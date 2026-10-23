#!/usr/bin/env python3
"""Game over, credits, restart and the level-end screen (M9.5).

Everything here is read-only and needs **no Ghidra**: the facts come from the
bytes of the installed ``gundx.exe`` (via :mod:`exelib`) and from the assets in
the installation directory.

Usage::

    python3 tools/re/gameover.py --self-test
    python3 tools/re/gameover.py --fields
    python3 tools/re/gameover.py --flows
    python3 tools/re/gameover.py --assets [--game DIR]
    python3 tools/re/gameover.py --json

``--self-test`` checks the hardcoded tables (byte signatures, string
literals, xref counts, negative scans) against the exe.  When the exe is
missing the byte checks are reported as skipped instead of failing.

Status words used below: ``код`` = proven by instruction bytes, ``данные`` =
proven by files, ``гипотеза`` = not proven.
"""
import argparse
import json
import os
import struct
import sys

import exelib

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
CACHE_EXE = os.path.join(exelib.cache_dir(), "gundx.exe")


# --------------------------------------------------------------------------
# 1. Globals / state fields
# --------------------------------------------------------------------------
# (name, va, width, meaning, writers, readers, status)
FIELDS = [
    ("frontend_state", 0x4BC440, 4,
     "фронтенд-состояние: 0 интро, 1 титул, 2 пагер SML, 5 ERROR, "
     "6 меню RESTART GAME, 9 уровень",
     "0x487709 (0->1), 0x48b54c (9->6), 0x48da31 (6->1), 0x485c53 (->0), "
     "0x48347e (credits->1)",
     "0x487e71 (диспетчер), 0x48b531", "код"),
    ("frame_submode", 0x83CB7C, 4,
     "подрежим кадра: 0 только ввод, 1 отдельная ветка, 4 сцена уровня, "
     "5 экран конца уровня",
     "0x45c066 (=5), 0x460549 (=5), 0x44b85a (=0), 0x44c2ed (=5)",
     "0x44c31e (ветвление кадра), 0x445445", "код"),
    ("action_state", 0x819040 + 0x3210, 4,
     "состояние игрока: 1..6 обычные, 7 = мёртв",
     "0x453763 (=7 при смерти), 0x4526d3 (=1 после респавна)",
     "0x44382d (switch 2..7), 0x493277 (панель)", "код"),
    ("flags2", 0x819040 + 0x3344, 4,
     "битовая маска ввода игрока; бит 0x8000 = действие/активация "
     "(одноразовый, сбрасывается потребителем)",
     "0x44d6b2/0x44d7c7 (копия из глобалов ввода 0x5a4a80/0x5a4a84)",
     "0x443ace (смерть), 0x445456 (экран конца), 0x4454bf", "код"),
    ("death_timer", 0x819040 + 0x33D4, 4,
     "тики с момента смерти; при >=0x88 (136) вызывается респавн",
     "0x45372f (=0 при смерти), 0x443853 (+=dt)",
     "0x443b0a (порог 0x88), 0x443849", "код"),
    ("mission_counter", 0x80F9CC, 4,
     "счётчик миссий кампании (M9.1); декремент только в "
     "'Restart level from HQ'",
     "0x44295e (dec), 0x442a0e/0x442a4c (=1), NEXT_LEVEL/NEXT_LEVEL_NUMBER",
     "0x48f0f4/0x48f118 (брифинг), 0x457b12", "код"),
    ("regen_cheat", 0x5A46D8, 4,
     "чит 'regen' из консоли; при 1 смерть не открывает меню (автореспавн)",
     "0x44aa0d (=1, команда 'regen')",
     "0x443af3", "код"),
    ("credits_gate", 0x83D940, 4,
     "1 = (тип игры F17 == 2); блокирует пункт HUD-панели 0x497264",
     "0x442e9e (= (0x83d63c==2))", "0x497264, 0x443aeb", "код"),
    ("frame_skip", 0x83D620, 4,
     "==4: кадр пропускается (0x44c2a2); !=0 блокирует открытие меню на смерти",
     "0x44a4c9 (=0), 0x44b85a", "0x44c2a2, 0x443ae0", "код"),
    ("level_current", 0x5A4728, 0x50,
     "имя текущего/загружаемого уровня (strncmp по 6/8 байтам)",
     "0x442ef0 (копия из 0x5a47c8)", "0x4435e3, 0x44c2ab", "код"),
    ("level_load", 0x5A47C8, 0x50,
     "имя уровня, который надо загрузить",
     "0x442992/0x4429bb (из снимков), 0x442e58", "0x442ef0", "код"),
    ("hq_name", 0x5A4868, 0x50,
     "имя уровня-штаба на момент последнего визита",
     "0x442e81 ('\\0'), 0x44363d (снимок)", "0x442979 (рестарт из HQ)", "код"),
    ("hq_player", 0x815440, 0x3510,
     "снимок структуры игрока в штабе (деньги, инвентарь, шасси)",
     "0x443622 (копия 0x819040), 0x4521ca", "0x4429a3 (рестарт из HQ)", "код"),
    ("mission_name", 0x5A48B8, 0x50,
     "имя уровня-миссии на момент её старта",
     "0x442e8d ('\\0'), 0x443671", "0x44294e (проверка 'LEVEL0'), 0x4429aa",
     "код"),
    ("mission_player", 0x8338C0, 0x3510,
     "снимок структуры игрока на старте миссии ('тот же R.P.V.')",
     "0x443656, 0x45220d", "0x4429d4", "код"),
    ("rpv_valid", 0x836AD0, 4,
     "запись R.P.V. (машины игрока) существует; гейт пункта меню",
     "0x442e93 (=1), 0x44a4c3 (=0)", "0x4888b0", "код"),
    ("hq_restart_ok", 0x818650, 4,
     "пункт 'Restart level from HQ' разрешён",
     "0x442e87 (=1), 0x44a4bd (=0)", "0x488927", "код"),
    ("pager_state", 0x4BC3A8, 4,
     "состояние SML-пагера: 1 = загрузить CREDITS.SML, 2 = страницы "
     "активны, 0 = закрыт; НАЧАЛЬНОЕ ЗНАЧЕНИЕ В ФАЙЛЕ = 1",
     "0x487d75/0x487db4 (=0), 0x4834ad/0x4834e7/0x48cf4a (=0), "
     "0x48352d/0x487e1d (=2)",
     "0x487d5e, 0x48cf22, 0x487b12", "код+данные"),
    ("pager_page", 0x4BC43C, 4,
     "номер страницы SML (0/1/2 -> prev/next/next)",
     "0x487cbf (=1), 0x487c57..0x487cb6",
     "0x487c4f", "код"),
    ("level_seconds", 0x83D638, 4,
     "время уровня в секундах: [0x5a46e4] / 91",
     "0x45c080", "0x483a?? (экран)", "код"),
    ("best_time", 0x83D2C0, 4,
     "лучшее время уровня (читается/обновляется в GUNMETAL.HI)",
     "0x45c08e", "0x483a?? (BEST TIME)", "код"),
    ("tick", 0x5A46E4, 4,
     "тикер 91 Гц (поток Sleep(11)); база времени уровня",
     "0x44bdda (INC)", "0x45c057 (деление на 91)", "код"),
]

# --------------------------------------------------------------------------
# 2. Byte signatures (VA, expected hex, note)
# --------------------------------------------------------------------------
SITES = [
    (0x4428F0, "a1d4465a00", "game_restartlevel: mov eax,[0x5a46d4]"),
    (0x442946, "6a0668d4b64a0068b8485a00", "strnicmp(mission_name,'LEVEL0',6)"),
    (0x44295E, "ff0dccf98000", "dec dword [0x80f9cc] (счётчик миссий -1)"),
    (0x442978, "bf68485a00", "edi = 0x5a4868 (имя штаба)"),
    (0x4429A2, "be40548100", "esi = 0x815440 (снимок игрока в штабе)"),
    (0x4429A9, "bfb8485a00", "edi = 0x5a48b8 (имя миссии)"),
    (0x4429D3, "bec0388300", "esi = 0x8338c0 (снимок игрока миссии)"),
    (0x442A0E, "c705ccf9800001000000", "counter = 1 (кламп снизу)"),
    (0x442A2E, "f3ab", "rep stosd: очистка 0x40 dword от 0x81c1bc"),
    (0x4435E3, "6a0868b8b74a006828475a00", "strnicmp(level_current,'SAVEGAME',8)"),
    (0x443621, "bf40548100", "снимок игрока -> 0x815440 (уровень LEVEL0*)"),
    (0x443655, "bfc0388300", "снимок игрока -> 0x8338c0 (миссия)"),
    (0x443AC8, "8b8384c38100", "ветка смерти: mov eax,[ebx+0x81c384]"),
    (0x443ACE, "f6c480", "test ah,0x80 (бит действия 0x8000)"),
    (0x443AF9, "c70540c44b0006000000", "state = 6 (меню RESTART GAME)"),
    (0x443B0A, "81bb14c4810088000000", "cmp [0x81c414],0x88 (136 тиков)"),
    (0x443B1E, "e84deb0000", "call 0x452670 (респавн)"),
    (0x445450, "8b8584c38100", "экран конца: test ah,0x80"),
    (0x4454A2, "6af8", "push -8 (SYSTEM_LEVEL_FINISHED)"),
    (0x4454A4, "e807410300", "call 0x4795b0 (event_post)"),
    (0x45372F, "c78614c4810000000000", "death_timer = 0"),
    (0x453763, "c78650c2810007000000", "action_state = 7 (мёртв)"),
    (0x45C057, "8b0de4465a00", "opcode 91 SHOW_LEVEL_END_SCREEN"),
    (0x45C066, "c7057ccb830005000000", "frame_submode = 5"),
    (0x45C086, "e8f5070000", "call 0x45c880 (лучшее время из GUNMETAL.HI)"),
    (0x483470, "576808cb4b00", "push 'winner' -> 0x4913a0 (плеер .sbv)"),
    (0x483476, "e825df0000", "call 0x4913a0"),
    (0x48347E, "c70540c44b0001000000", "state = 1 (титул)"),
    (0x48348D, "68f0ca4b00", "push 'SML\\BRIEF\\LEVEL0A.SML'"),
    (0x483492, "e8793e0100", "call 0x497310 (загрузчик SML/SME)"),
    (0x487D94, "6828d24b00", "push 'SML\\CREDITS.SML'"),
    (0x487D99, "e872f50000", "call 0x497310"),
    (0x487E1B, "c705a8c34b0002000000", "pager_state = 2"),
    (0x48B4FD, "80fb0d", "cmp bl,0xd (Enter закрывает пагер)"),
    (0x48CF22, "a1a8c34b00", "state 1: mov eax,[0x4bc3a8]"),
    (0x48CF4A, "c705a8c34b0000000000", "pager_state = 0"),
    (0x48D994, "83fd06", "меню: cmp ebp,6 (state 6)"),
    (0x48D9FD, "6a00", "push 0 -> 0x4428f0 ('same R.P.V.')"),
    (0x48DA09, "6a01", "push 1 -> 0x4428f0 ('from HQ')"),
    (0x48DA0B, "e8e04efbff", "call 0x4428f0"),
    (0x488860, "6864d44b00", "push 'RESTART GAME' (заголовок меню)"),
    (0x493273, "8b8250c28100", "панель игрока: mov eax,[edx+0x81c250]"),
    (0x493279, "83f801", "панель: cmp action_state,1"),
    (0x49327C, "7423", "je: жив -> надпись не рисуется"),
    (0x49327E, "83f807", "cmp action_state,7 (мёртв)"),
    (0x493281, "751e", "jne: не мёртв -> не рисовать"),
    (0x4971ED, "ff2485cc724900", "jmp [eax*4+0x4972cc] (меню действий)"),
    (0x4972C1, "e8aac1feff", "call 0x483470 (пункт 3)"),
]

# VA -> expected literal
STRINGS = [
    (0x4BC2C8, "PRESS ACTIVATE TO RESTART"),
    (0x4BCB08, "winner"),
    (0x4BCB10, "PRESS %s TO CONTINUE"),
    (0x4BCB60, "TO CONTINUE"),
    (0x4BCB6C, "PRESS %s"),
    (0x4BCB84, "YOUR BEST:"),
    (0x4BCB90, "BEST TIME:"),
    (0x4BCB9C, "TIME:"),
    (0x4BCBA4, "SALVAGE:"),
    (0x4BCBB0, "SECRETS FOUND:"),
    (0x4BCBC0, "KILLS:"),
    (0x4BCBC8, "levelend"),
    (0x4BCC04, "SML\\PRELEVEL\\%s.SML"),
    (0x4BCAD4, "error loading cheats.sml\n"),
    (0x4BCAAC, "couldn't allocate credits scroll mem\n"),
    (0x4BCB54, "levelen2"),
    (0x4BD20C, "error loading credits.sml\n"),
    (0x4BD228, "SML\\CREDITS.SML"),
    (0x4BD3EC, "Quit to title"),
    (0x4BD3FC, "Restart level from HQ"),
    (0x4BD414, "SECRT2"),
    (0x4BD41C, "SECRT1"),
    (0x4BD424, "LEVEL20"),
    (0x4BD42C, "Restart level with same R.P.V."),
    (0x4BD44C, "Reload last saved game"),
    (0x4BD464, "RESTART GAME"),
    (0x4BE618, "%s HAS DIED"),
]

# Global whose value 1 is what selects the CREDITS.SML branch.  Every store to
# it in the shipped exe (scan below) writes 0 or 2; the *initial* dword is 1.
PAGER_VA = 0x4BC3A8
EXPECTED_PAGER_IMM_WRITES = {0x4834AB: 0, 0x48352B: 2, 0x487E1B: 2, 0x48CF4A: 0}
EXPECTED_PAGER_SITES = 18          # all byte-pattern hits in .text/.data

# Transition map (frontend state, sub-mode and death/level-end flows).
FLOWS = [
    # (from, to, site, what, status)
    ("state 0 (интро)", "state 1 (титул)", "0x487709",
     "интро закончилось/пропущено", "код"),
    ("state 1", "state 1 + pager 2 (CREDITS.SML)", "0x487D94",
     "pager_state==1 (начальное значение файла) -> загрузка титров", "код+данные"),
    ("credits overlay", "state 1 (меню)", "0x48CF2B..0x48CF4A",
     "выбор пункта меню освобождает SML-буфер и ставит pager=0", "код"),
    ("credits overlay", "credits overlay", "0x48B4E5..0x48B50C",
     "Enter (0xd/0xa4) освобождает SML-ресурсы (0x498d40), ключ не гасится",
     "код"),
    ("state 9 (уровень)", "state 6 (RESTART GAME)", "0x48B54C",
     "Esc в игре открывает меню", "код"),
    ("state 9 (уровень)", "state 0 (загрузка уровня)", "0x485C50",
     "рестарт/выбор пункта меню: state=0 + 0x485c90", "код"),
    ("state 6", "state 1", "0x48DA31",
     "пункт 'Quit to title'", "код"),
    ("state 6", "state 0", "0x48DA0B -> 0x4428F0 -> 0x485C50",
     "пункт 1/2 меню (перезапуск уровня)", "код"),
    ("state 9 (жив)", "state 9 (мёртв, action_state 7)", "0x453763",
     "player_killed: action_state=7, death_timer=0", "код"),
    ("мёртв", "мёртв", "0x443849",
     "death_timer += dt каждый кадр", "код"),
    ("мёртв", "state 6 (меню)", "0x443AF9",
     "Activate (бит 0x8000) и [0x83d620]==0, [0x83d940]==0, [0x5a46d8]==0",
     "код"),
    ("мёртв", "state 9 (респавн)", "0x443B0A -> 0x452670",
     "death_timer >= 136 тиков (~1.49 с при 91 Гц)", "код"),
    ("state 9 (уровень)", "state 9 + submode 5", "0x45C057/0x45C066",
     "опкод 91 SHOW_LEVEL_END_SCREEN", "код"),
    ("submode 5 (экран)", "submode 5", "0x4454A4",
     "Activate -> event_post(-8) SYSTEM_LEVEL_FINISHED", "код"),
    ("submode 5", "штаб", "скрипт -8: NEW_LEVEL \"level00\" и т.п.",
     "подскрипт -8 миссии грузит свой штаб (24/24, docs/CAMPAIGN_FLOW.md §3)",
     "данные"),
    ("штаб: entity 305", "следующая миссия", "NEXT_LEVEL [SOURCE_IS_PLAYER]",
     "обработчик 0x457b11: имя из счётчика + инкремент", "код"),
    ("level24 (submode 5)", "level24 (submode 5)", "level24.sbt -8",
     "в -8 нет кампанийного выхода (только IF DEATHMATCH_GAME)", "данные"),
    ("меню, пункт 3", "winner.sbv + LEVEL0A", "0x4972C1 -> 0x483470",
     "VM-опкод 54 CUSTOM_COMMAND_2; в 36 скриптах 0 сайтов", "код+данные"),
]

ASSETS = [
    ("SML/credits.sml", "обязателен", 1470,
     "текст титров (SML-пагер, ASCII)"),
    ("SML/brief/LEVEL0A.SML", "ожидается отсутствующим", None,
     "загрузчик сам пробует .SME (0x49736e дописывает 'E')"),
    ("SML/brief/LEVEL0A.SME", "обязателен", None,
     "экран 'CONGRATULATIONS! ... EMPLOYEE OF THE MONTH' + чит-коды"),
    ("Vid/winner.sbv", "обязателен", 15343896,
     "ролик финала, 1820 кадров (docs/SBV.md)"),
    ("Vid/Intro.sbv", "обязателен", 2811092, "интро (state 0)"),
    ("Vid/orient.sbv", "обязателен", 35005220,
     "ролик в поставке, но вызовов нет (docs/UI_SOUNDS.md §3.3)"),
    ("MUSIC/winner.sog", "обязателен", None,
     "музыка ролика winner (имя = имя ролика)"),
    ("sounds/SOU.IDX", "обязателен", 12000,
     "1000 записей name[8]+u32 offset; FINALE/OVERTURE/WINNER"),
    ("SAVEGAME/saved0.sbt", "обязателен", None,
     "после рестарта из HQ/загрузки мир берётся отсюда"),
    ("GUNMETAL.HI", "опционален", None,
     "таблица лучших времён; обновляется 0x45c880"),
]


# --------------------------------------------------------------------------
# 3. helpers
# --------------------------------------------------------------------------
def _exe_ok():
    if os.path.exists(exelib.EXE):
        return True
    if os.path.exists(CACHE_EXE):
        exelib.EXE = CACHE_EXE          # cached copy made by gmcache.py
        return True
    return False


def _va_section(va):
    for name, vma, vsize, _off in exelib.sections():
        if vma <= va < vma + vsize:
            return name
    for row in exelib.virtual_sections():
        name, vma, vsize = row[0], row[1], row[2]
        if vma <= va < vma + vsize:
            return name + " (bss)"
    return None


def _read(va, n):
    off = exelib.va2off(va)
    if off is None:
        return None
    return exelib.data()[off:off + n]


def _byte_hits(value):
    """VAs of every little-endian dword equal to *value* in the image."""
    d = exelib.data()
    needle = struct.pack("<I", value)
    out, i = [], d.find(needle)
    while i >= 0:
        va = exelib.off2va(i)
        if va is not None:
            out.append((va, i))
        i = d.find(needle, i + 1)
    return out


def _all_strings():
    out = []
    for name, vma, vsize, _off in exelib.sections():
        if name not in (".text", ".rdata", ".data"):
            continue
        for va, s in exelib.strings_in(vma, vma + vsize):
            out.append((name, va, s))
    return out


def _read_text(path):
    with open(path, "rb") as fh:
        return fh.read()


def decode_sme(raw):
    """Reverse the +(i % 10) Caesar shift used by .sme files (docs/UI.md §3)."""
    out = bytearray()
    for i, b in enumerate(raw):
        c = chr(b)
        if c.isalpha():
            base = ord("A") if c.isupper() else ord("a")
            out.append((b - base - (i % 10)) % 26 + base)
        else:
            out.append(b)
    return out.decode("latin1")


def _walk(insns, conds=()):
    """Yield ``(insn, conditions)`` recursing into IF/ELSE bodies."""
    for ins in insns:
        yield ins, conds
        body = getattr(ins, "body", None)
        if not isinstance(body, list):
            continue
        if ins.name == "IF":
            name = ins.args[0][0] if ins.args else "?"
            yield from _walk(body, conds + (name,))
        elif ins.name == "ELSE":
            yield from _walk(body, conds + ("else",))
        else:
            yield from _walk(body, conds)


def script_scan(game):
    """Parse every shipped ``.sbt``: CUSTOM_COMMAND sites and level24's -8.

    Keeps the negative results reproducible ("no script opens the finale").
    """
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    if root not in sys.path:
        sys.path.insert(0, root)
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    try:
        import campaign_flow as cf
        from gm import sbt as sbtmod
        from gm import sbtvm
    except Exception as exc:                        # pragma: no cover
        return {"error": "%s: %s" % (type(exc).__name__, exc)}

    files = sorted(__import__("glob").glob(os.path.join(game, "*.sbt")))
    custom = []
    end_screens = 0
    for path in files:
        doc = sbtmod.parse_file(path)
        for rec in doc.records_of(11):
            for ins, conds in _walk(sbtvm.decode(rec.payload.code)[0]):
                if ins.name.startswith("CUSTOM_COMMAND"):
                    custom.append(dict(file=os.path.basename(path),
                                       cmd=ins.name,
                                       args=[a[1] if isinstance(a, tuple) else a
                                             for a in (ins.args or [])],
                                       conditions=list(conds)))
                if ins.name == "SHOW_LEVEL_END_SCREEN":
                    end_screens += 1

    out = dict(files=len(files), custom_commands=custom,
               show_level_end_sites=end_screens, level24=None)
    try:
        flows = cf.flow_map(game)
        f24 = flows.get("level24")
        if f24:
            ex = cf.completion_campaign_exits(f24)
            out["level24"] = dict(
                offset=hex(f24["system_finished"]["offset"])
                if f24.get("system_finished") else None,
                all_completion_exits=[
                    dict(target=e.get("target"), conditions=e.get("conditions"),
                         deathmatch=e.get("deathmatch"))
                    for e in cf.completion_exits(f24)],
                campaign_exits=[
                    dict(target=e.get("target"), conditions=e.get("conditions"))
                    for e in ex],
            )
    except Exception as exc:                        # pragma: no cover
        out["level24_error"] = "%s: %s" % (type(exc).__name__, exc)
    return out


def find_case_insensitive(root, rel):
    """Return the real path for *rel* honouring the (Windows) case-insensitive
    expectation even on a case-sensitive mirror."""
    direct = os.path.join(root, rel)
    if os.path.exists(direct):
        return direct
    cur = root
    for part in rel.split("/"):
        if not os.path.isdir(cur):
            return None
        names = os.listdir(cur)
        hit = next((n for n in names if n.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


# --------------------------------------------------------------------------
# 4. self-test
# --------------------------------------------------------------------------
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


def self_test():
    c = Checker()
    # --- structural checks: no exe needed -------------------------------
    c.check(len(FIELDS) >= 20, "fields: >=20 записей")
    c.check(len({f[1] for f in FIELDS}) == len(FIELDS), "fields: адреса уникальны")
    c.check(all(f[2] in (4, 0x50, 0x3510) for f in FIELDS),
            "fields: ширина из {4,0x50,0x3510}")
    c.check(all(f[6] in ("код", "данные", "код+данные", "гипотеза") for f in FIELDS),
            "fields: статус из допустимых")
    c.check(len(STRINGS) >= 20 and len({v for v, _ in STRINGS}) == len(STRINGS),
            "strings: >=20 и адреса уникальны")
    c.check(all("0x" not in s for _, s in STRINGS), "strings: без префикса 0x")
    c.check(len(FLOWS) >= 15, "flows: >=15 переходов")
    c.check(all(len(f) == 5 for f in FLOWS), "flows: 5 полей в записи")
    c.check(all(f[1] for f in FLOWS), "flows: у каждого перехода есть цель")
    c.check(len({a[0] for a in ASSETS}) == len(ASSETS), "assets: имена уникальны")

    # deterministic JSON for the same input
    a = json.dumps(fields_json(), sort_keys=True)
    b = json.dumps(fields_json(), sort_keys=True)
    c.check(a == b, "json: детерминизм")

    if not _exe_ok():
        for name in ("bytes", "strings", "xrefs", "pager scan"):
            c.skip("%s (нет gundx.exe)" % name)
        return c, {"exe": None}

    exe_sha = exelib.exe_sha256()
    facts = {"exe_sha256": exe_sha, "exe": exelib.EXE}

    # --- every VA is inside a mapped section ---------------------------
    for name, va, _w, *_rest in FIELDS:
        c.check(_va_section(va) is not None, "va mapped: %s @0x%x" % (name, va))

    # --- byte signatures ------------------------------------------------
    for va, hexs, note in SITES:
        raw = _read(va, len(hexs) // 2)
        c.check(raw is not None and raw.hex() == hexs.lower(),
                "bytes 0x%06x: %s" % (va, note))

    # --- string literals ------------------------------------------------
    for va, want in STRINGS:
        got = exelib.cstr(va)
        c.check(got == want, "string 0x%06x == %r (got %r)" % (va, want, got))

    # --- negative: no "GAME OVER" screen text anywhere ------------------
    hits = [(sec, va, s) for sec, va, s in _all_strings()
            if "game over" in s.lower() or "gameover" in s.lower()]
    c.check(not hits, "нет строки GAME OVER в .text/.rdata/.data (%r)" % (hits[:3],))
    facts["game_over_strings"] = hits

    # --- negative: no lives counter string ------------------------------
    lives = [(va, s) for _sec, va, s in _all_strings()
             if s.strip().upper() in ("LIVES", "LIVES:", "PLAYER LIVES")]
    c.check(not lives, "нет строки LIVES (%r)" % (lives[:3],))

    # --- pager_state 0x4bc3a8: initial value and writers ----------------
    init = _read(PAGER_VA, 4)
    c.check(init == b"\x01\x00\x00\x00",
            "pager_state init == 1 (файл %s)" % (init.hex() if init else None))

    hits = _byte_hits(PAGER_VA)
    c.check(len(hits) == EXPECTED_PAGER_SITES,
            "pager_state: %d сайтов (ожидалось %d)" %
            (len(hits), EXPECTED_PAGER_SITES))
    imm = {}
    reg = []
    for va, off in hits:
        d = exelib.data()
        prev2 = d[off - 2:off]
        if prev2 == b"\xc7\x05":                   # mov dword [addr], imm32
            imm[va - 2] = struct.unpack("<I", d[off + 4:off + 8])[0]
        elif prev2[:1] == b"\x89":                 # mov [addr], r32
            reg.append((va - 2, prev2[1]))
    c.check(imm == EXPECTED_PAGER_IMM_WRITES,
            "pager_state: immediate-записи %s" %
            {"0x%06x" % k: v for k, v in sorted(imm.items())})
    c.check(all(v != 1 for v in imm.values()),
            "pager_state: ни одна immediate-запись не пишет 1")
    c.check(len(reg) == 4, "pager_state: 4 записи через регистр (%r)" % (reg,))
    facts["pager_writers"] = {"imm": {"0x%06x" % k: v for k, v in sorted(imm.items())},
                              "reg": [("0x%06x" % va, op) for va, op in reg],
                              "sites": len(hits), "init": 1}

    # --- CREDITS.SML has exactly one reference --------------------------
    refs = [va for va, _off in _byte_hits(0x4BD228)]
    c.check(len(refs) == 1, "SML\\CREDITS.SML: ровно 1 xref (%r)" % (refs,))
    facts["credits_refs"] = ["0x%06x" % v for v in refs]

    # --- winner: one string ref, one call of the handler ----------------
    refs = [va for va, _off in _byte_hits(0x4BCB08)]
    c.check(len(refs) == 1, "'winner': ровно 1 xref (%r)" % (refs,))
    d = exelib.data()
    calls = []
    for i in range(len(d) - 5):
        if d[i] == 0xE8:
            src = exelib.off2va(i)
            if src is None:
                continue
            rel = struct.unpack("<i", d[i + 1:i + 5])[0]
            if (src + 5 + rel) & 0xFFFFFFFF == 0x483470:
                calls.append(src)
    c.check(len(calls) == 1, "call 0x483470: ровно 1 сайт (%r)" %
            (["0x%06x" % v for v in calls],))
    facts["winner_callers"] = ["0x%06x" % v for v in calls]

    # --- mission counter 0x80f9cc: refs and no comparison with 24/25 -----
    hits = _byte_hits(0x80F9CC)
    c.check(len(hits) == 15, "mission_counter: 15 ссылок (%d)" % len(hits))
    d = exelib.data()
    bad = []
    for va, off in hits:
        win = d[max(0, off - 32):off + 32]
        for imm in (0x18, 0x19):            # 24 / 25
            pats = (b"\x83\xf8" + bytes([imm]), b"\x83\xf9" + bytes([imm]),
                    b"\x83\xfa" + bytes([imm]), b"\x3d" + struct.pack("<I", imm))
            if any(p in win for p in pats):
                bad.append("0x%06x" % va)
    c.check(not bad, "mission_counter: нет cmp с 24/25 рядом (%r)" % bad)
    facts["mission_counter_refs"] = ["0x%06x" % v for v, _o in hits]

    # --- level24 has no campaign exit (data, checked in --assets) -------
    return c, facts


# --------------------------------------------------------------------------
# 5. output helpers
# --------------------------------------------------------------------------
def fields_json():
    return [dict(name=n, va="0x%06x" % va, width=w, meaning=m, writers=wr,
                 readers=rd, status=st)
            for n, va, w, m, wr, rd, st in FIELDS]


def flows_json():
    return [dict(src=a, dst=b, site=c, what=d, status=e) for a, b, c, d, e in FLOWS]


def assets_json(game):
    out = []
    for rel, need, size, why in ASSETS:
        path = find_case_insensitive(game, rel)
        row = dict(path=rel, required=need, expected_size=size, why=why,
                   present=bool(path))
        if path:
            row["size"] = os.path.getsize(path)
            if size is not None:
                row["size_ok"] = row["size"] == size
        out.append(row)
    return out


def print_fields():
    print("Game over / титры / рестарт — карта адресов (M9.5)\n")
    for n, va, w, m, wr, rd, st in FIELDS:
        print("  %-15s 0x%06x  +%#-6x %s" % (n, va, w, m))
        print("      писатели: %s" % wr)
        print("      читатели: %s   [%s]" % (rd, st))
    print("\n  всего %d записей" % len(FIELDS))


def print_flows():
    print("""\
Машина состояний: смерть, меню, титры, экран конца уровня
=========================================================

  INTRO  state 0  (0x487709 -> 1)
    |
    v
  TITLE  state 1  --------- pager_state==1 (init в exe = 1) -------------+
    |                       0x487d94: load SML\\CREDITS.SML               |
    |                       0x487e1b: pager_state = 2                     |
    |                       Enter 0x48b4e5..0x48b50c -> free SML          |
    |                       пункт 0x48cf2b..0x48cf4a -> pager=0           |
    |<-------------------------------------------------------------------+
    |
    |  пункт "START GAME" (не в этой задаче)
    v
  LEVEL  state 9, submode 4
    |
    |  смерть: player_killed 0x453763 -> action_state = 7, timer = 0
    v
  DEAD  action_state 7        timer 0x81c414 += dt      (0x443849)
    |---- Activate (бит 0x8000) + !frame_skip + !credits + !regen
    |         0x443af9 -> state = 6  == МЕНЮ (это и есть "game over")
    |
    |---- timer >= 136 (0x88)  0x443b0a -> 0x452670: РЕСПАВН
    |         (кампания: SET_PLAYER_START, DM: самый далёкий слот)
    v
  LEVEL продолжается           <-- отдельного состояния GAME OVER нет
                               (ни жизней, ни счётчика смертей, ни сейва)

  МЕНЮ  state 6  (заголовок 'RESTART GAME', строитель 0x488850)
    |
    +-- 0 'Reload last saved game'      0x48d9e9 -> 0x442340
    +-- 1 'Restart level with same R.P.V.' 0x48d9fd push 0 -> 0x4428f0(0)
    +-- 2 'Restart level from HQ'       0x48da09 push 1 -> 0x4428f0(1)
    +-- 3 'Quit to title'               0x48da31 -> state 1

  Экран конца уровня
    |
    |  опкод 91 SHOW_LEVEL_END_SCREEN  0x45c057
    |    секунды = [0x5a46e4] / 91    0x45c080
    |    submode 5                    0x45c066
    |    лучшее время из GUNMETAL.HI  0x45c086 -> [0x83d2c0]
    v
  SUBMODE 5:  один игрок 0x483a70 | несколько 0x483550 (0x454110)
    |         'levelend'/'levelen2', TIME, BEST TIME, YOUR BEST,
    |         SECRETS FOUND: X / Y, KILLS, SALVAGE
    |
    |  Activate -> event_post(-8, ...)  0x4454a2/0x4454a4
    v
  -8 = SYSTEM_LEVEL_FINISHED (docs/EVENTS.md §4.1)
    |
    +-- подскрипт -8 миссии: NEW_LEVEL "<свой штаб>" (level1 -> level00)
    |     в штабе предмет entity 305: NEXT_LEVEL [SOURCE_IS_PLAYER]
    |     (0x457b11: имя из счётчика + инкремент) -> следующая миссия
    +-- level24: в -8 только IF DEATHMATCH_GAME { NEW_LEVEL "DM1" }
                и ELSE пустой => кампания остаётся на экране (конец игры)
""")
    print("Переходы (данные инструмента):")
    for a, b, site, what, st in FLOWS:
        print("  %-26s -> %-28s %-22s %-52s [%s]" % (a, b, site, what, st))


def print_assets(game):
    print("Ассеты в %s\n" % game)
    for rel, need, size, why in ASSETS:
        path = find_case_insensitive(game, rel)
        if path:
            got = os.path.getsize(path)
            mark = "OK "
            if size is not None and got != size:
                mark = "!! "
            print("  %s %-24s %9d  %s" % (mark, rel, got, why))
        else:
            print("  --  %-24s %9s  %s  [%s]" %
                  (rel, "-", why, need))
    # decode LEVEL0A.SME to prove it is the finale/cheat screen
    sme = find_case_insensitive(game, "SML/brief/LEVEL0A.SME")
    if sme:
        text = decode_sme(_read_text(sme))
        head = " ".join(text.replace("<", " <").split())[:120]
        print("\n  LEVEL0A.SME (декодировано, %d байт):" % len(text))
        print("    %s" % head)
        print("    -> это экран 'CONGRATULATIONS' + чит-коды, а не титры")
    # sounds
    idx = find_case_insensitive(game, "sounds/SOU.IDX")
    if idx:
        raw = _read_text(idx)
        names = {}
        for i in range(len(raw) // 12):
            nm = raw[i * 12:i * 12 + 8].split(b"\0")[0].decode("latin1")
            names[nm] = struct.unpack("<I", raw[i * 12 + 8:i * 12 + 12])[0]
        found = [n for n in ("FINALE", "OVERTURE", "WINNER") if n in names]
        print("  SOU.IDX: записей %d, из них %s" % (len(raw) // 12, ", ".join(found)))
    # level24 -8 tail: campaign exit?
    sbt = find_case_insensitive(game, "level24.sbt")
    if sbt:
        print("  level24.sbt: %d байт (авто-титров в -8 нет, см. docs/GAME_OVER.md §3)"
              % os.path.getsize(sbt))
    scan = script_scan(game)
    if "error" not in scan and "level24_error" not in scan:
        print("\n  Скрипты .sbt: %d файлов" % scan["files"])
        print("    CUSTOM_COMMAND_*: %d сайтов %s" %
              (len(scan["custom_commands"]),
               "(ни один не вызывает 0x483470)" if not scan["custom_commands"]
               else str(scan["custom_commands"][:3])))
        print("    SHOW_LEVEL_END_SCREEN: %d сайтов" % scan["show_level_end_sites"])
        l24 = scan["level24"]
        if l24:
            print("    level24: -8 на %s, кампанийных выходов %d %s" %
                  (l24["offset"], len(l24["campaign_exits"]),
                   [e["target"] for e in l24["campaign_exits"]]))
            print("      все выходы -8: %s" %
                  [(e["target"], e["conditions"]) for e in l24["all_completion_exits"]])
    else:
        print("\n  Скрипты: %s" % (scan.get("error") or scan.get("level24_error")))


def run_json(game):
    _, facts = self_test()
    return dict(
        tool="gameover",
        milestone="M9.5",
        exe=facts.get("exe_sha256"),
        exe_path=facts.get("exe"),
        game=game,
        fields=fields_json(),
        flows=flows_json(),
        assets=assets_json(game),
        scripts=script_scan(game),
        negatives=dict(
            game_over_strings=facts.get("game_over_strings", []),
            pager_writers=facts.get("pager_writers"),
            credits_refs=facts.get("credits_refs"),
            winner_callers=facts.get("winner_callers"),
        ),
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true", help="проверить таблицы по exe")
    ap.add_argument("--fields", action="store_true", help="карта полей/адресов")
    ap.add_argument("--flows", action="store_true", help="диаграмма переходов")
    ap.add_argument("--assets", action="store_true", help="ассеты установки")
    ap.add_argument("--json", action="store_true", help="всё в JSON")
    ap.add_argument("--game", default=DEFAULT_GAME, help="каталог установки")
    args = ap.parse_args(argv)

    if not any((args.self_test, args.fields, args.flows, args.assets, args.json)):
        ap.print_help()
        return 0

    if args.json:
        print(json.dumps(run_json(args.game), ensure_ascii=False, indent=2,
                         sort_keys=True))
        return 0

    rc = 0
    if args.self_test:
        c, facts = self_test()
        if facts.get("exe_sha256"):
            print("exe: %s  sha256 %s" % (facts["exe"], facts["exe_sha256"][:12]))
        for name in c.skipped:
            print("SKIP %s" % name)
        for name in c.failed:
            print("FAIL %s" % name)
        total = c.passed + len(c.failed)
        print("%d/%d self-tests passed%s" %
              (c.passed, total,
               ", %d skipped" % len(c.skipped) if c.skipped else ""))
        rc = 1 if c.failed else 0
    if args.fields:
        print()
        print_fields()
    if args.flows:
        print()
        print_flows()
    if args.assets:
        print()
        print_assets(args.game)
    return rc


if __name__ == "__main__":
    sys.exit(main())
