#!/usr/bin/env python3
"""Карта загрузки/выгрузки уровня в ``gundx.exe`` (M2.2.3).

Печатает таблицу шагов ``load_level`` (``0x442ef0``), связи с выгрузкой
(``0x428f67``, ``0x454cf0``), рестартом (``0x4428f0``) и загрузкой сейва
(``0x442340``).  ``--verify`` сверяет карту с байтами exe и с реальными файлами
уровней (только чтение), ``--self-test`` проверяет сам верификатор на
синтетических данных.

    python3 tools/re/level_load.py                 # карта шагов
    python3 tools/re/level_load.py --globals       # runtime-массивы
    python3 tools/re/level_load.py --records       # SBT: тип -> обработчик
    python3 tools/re/level_load.py --verify        # exe + level0/dm1 + корпус
    python3 tools/re/level_load.py --self-test     # автотест (exit != 0)

Только stdlib.  Путь к игре — ``GUNMETAL_EXE`` / ``GUNMETAL_DIR``.
Адреса — виртуальные (ImageBase ``0x400000``).  Пометки: **код** — шаг
подтверждён байтами (адрес есть в :data:`CODE_EDGES`/`IMM`), **гипотеза** —
вывод декомпилятора без байтового подтверждения.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

DEFAULT_EXE = "/mnt/d/games/Gunmetal 3dfx/gundx.exe"
EXE = Path(os.environ.get("GUNMETAL_EXE", DEFAULT_EXE))
GAME_DIR = Path(os.environ.get("GUNMETAL_DIR", str(EXE.parent)))

IMAGE_BASE = 0x400000

#: ``COMMANDS``/``CONDITIONS`` — таблицы опкодов VM (``tools/re/cmdtable.py``).
TABLE_COMMANDS = (0x4B53E8, 192)
TABLE_CONDITIONS = (0x4B62E8, 59)
ENTRY_STRIDE = 20  # {name_ptr, sig_ptr, flag, -1, id}


# --------------------------------------------------------------------------- #
# PE: чтение образа (нужны только .text/.data/.rdata по VA)
# --------------------------------------------------------------------------- #
class Image:
    """Минимальный read-only маппинг VA -> offset для PE32."""

    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        machine, nsec, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
        if machine != 0x14C:
            raise ValueError("not i386 (machine 0x%x)" % machine)
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x10B:
            raise ValueError("not PE32 (magic 0x%x)" % magic)
        self.base = struct.unpack_from("<I", data, opt + 28)[0]
        self.entry = self.base + struct.unpack_from("<I", data, opt + 16)[0]
        self.sections = []
        sec = opt + opt_size
        for i in range(nsec):
            name = data[sec + 40 * i: sec + 40 * i + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
            self.sections.append((name, self.base + va, vsize, rawptr, rawsize))
        if not self.sections:
            raise ValueError("no sections")

    def off(self, va: int) -> int:
        for _, sva, vsize, rawptr, rawsize in self.sections:
            if sva <= va < sva + max(vsize, rawsize):
                delta = va - sva
                if delta >= rawsize:
                    raise ValueError("VA 0x%x is not backed by file data" % va)
                return rawptr + delta
        raise ValueError("VA 0x%x outside sections" % va)

    def u8(self, va: int) -> int:
        return self.data[self.off(va)]

    def u16(self, va: int) -> int:
        return struct.unpack_from("<H", self.data, self.off(va))[0]

    def u32(self, va: int) -> int:
        return struct.unpack_from("<I", self.data, self.off(va))[0]

    def i32(self, va: int) -> int:
        return struct.unpack_from("<i", self.data, self.off(va))[0]

    def raw(self, va: int, size: int) -> bytes:
        o = self.off(va)
        return self.data[o:o + size]

    def cstr(self, va: int, limit: int = 200) -> str:
        o = self.off(va)
        end = self.data.find(b"\0", o, o + limit)
        if end < 0:
            end = o + limit
        return self.data[o:end].decode("latin-1")

    def call_target(self, va: int) -> int:
        """Цель ``call rel32`` по адресу самой инструкции."""
        if self.u8(va) != 0xE8:
            raise ValueError("0x%x is not a rel32 call (opcode 0x%02x)" % (va, self.u8(va)))
        return va + 5 + self.i32(va + 1)


def load_exe(path: Path = EXE) -> Image:
    return Image(path.read_bytes())


# --------------------------------------------------------------------------- #
# Карта шагов: 0x442ef0 (загрузка уровня), 0x442340 (сейв), 0x4428f0 (рестарт)
# --------------------------------------------------------------------------- #
# (шаг, адрес, что делает, файл/буфер, заполняемые глобалы, ошибка (адрес строки), статус)
STEPS = [
    ("S0", "0x442ef0", "вход: 0x5a47c8 (отложенное имя) -> 0x5a4728 (текущий уровень); "
                       "два double 1.0 -> 0x4aa390/0x4aa398, 0x5a4700 = 0",
     "—", "0x5a4728, 0x5a4700, 0x4aa390..9c", "", "код"),
    ("S1", "0x442f5a", "сброс состояния игрока 0x452430; VM 0x463a90; "
                       "экран загрузки 0x484a20 (frontend/LOADPAGE + SML/PRELEVEL)", "—",
     "0x619aa8..0x619ab0", "0x4bcc04 SML/PRELEVEL", "код"),
    ("S2", "0x442f7f", "если прежнее имя непустое -> unload_world 0x428f67", "—",
     "0x5670d8/0x5670e0/0x5a45cc", "", "код"),
    ("S3", "0x442fa6", "если звук включён (0x4aa384): стоп/старт музыки 0x40c030/0x40bcf0", "—",
     "—", "", "код"),
    ("S4", "0x442fff", "SBC (лайтмапы): 0x429a2a(\"\", \"<level>\") через bigfile «sbc» 0x40d1a0",
     "<level>.sbc", "0x5a45c8/0x5a45cc (count/массив 8 байт)", "0x4ab62c warning (не фатально)", "код"),
    ("S5", "0x44304b", "SBW (мир): 0x42aa63(\"\", \"<level>\") через bigfile «sbw»", "<level>.sbw",
     "0x76ed40 (0x800×0x10), 0x5310c0, 0x5670cc, 0x5670d8/0x5670dc",
     "0x4ab98c/0x4ab5e4 (фатально)", "код"),
    ("S5b", "0x443078", "SBW текстовый фолбэк: 0x42a33d", "<level>.sbw (текст)", "как S5",
     "0x4ab5e4", "код"),
    ("S6", "0x4430cc", "world_finalize 0x42f183: слияние копланарных порталов, AABB полигонов",
     "—", "0x5670d8, 0x5670dc (объекты)", "0x4ab970/0x4ab948", "код"),
    ("S7", "0x44312c", "SBT (скомпилированные things): 0x454dc0(\"<level>.SBT\", 0) — "
                       "бинарный диспетчер записей", "<level>.sbt",
     "0x5a9450/0x5a945c/0x5a9454, 0x8381a0, 0x5a4af0, 0x836de0, 0x638128, 0x5b1df0, 0x80d87c",
     "0x4ab910/0x4ab56c", "код"),
    ("S7b", "0x443155", "SBT текстовый фолбэк: 0x454b00(\"<level>.SOT\", 0)", "<level>.sot",
     "как S7", "0x4ab910/0x4ab56c", "код"),
    ("S7c", "0x4431d9", "/sbm (0x5a46dc): пересобрать бинарные .sbw (0x42afbb) и .sbt (0x455760)",
     "<level>.sbw/.sbt (запись)", "—", "0x4ab8e0/0x4ab8ac", "код"),
    ("S8", "0x443253", "системные скрипты: generate_runner(-5), (-6); 0x4847f0; 0x44f260 "
                       "(привязка дверей/лифтов 0x44dfc0/0x44e6f0)",
     "SCRIPT (тип 11)", "0x83d2d0/0x83d2d4", "", "код"),
    ("S8b", "0x443299", "generate_runner(-4), (-2), (0), (-1); 0x5a46e4 = 0; 0x5a46e8 = тик",
     "SCRIPT", "0x5a46e4/0x5a46e8", "", "код"),
    ("S9", "0x4432fa", "текстуры/спрайты: 0x422f75(\"textures\", 0) и (\"sprites\", 1)",
     "PICS/*.sbm", "0x55a0c0 (имена), 0x5310c0", "0x4ab528", "код"),
    ("S10", "0x443387", "init текстурного пула 0x4232b7: слить имена секторов 0x76ed40 в 0x55a0c0",
     "—", "0x5330c0, 0x5350c0, 0x55a0c0", "0x4ab528", "код"),
    ("S11", "0x4433c3", "0x455f30: обновление спрайтов/анимаций", "—", "0x5a9728", "0x4ab50c", "код"),
    ("S12", "0x443449", "игроки: 0x453120(0x83d2d0) и (0x83d2d4), 1..2 по 0x83d180", "—",
     "0x83d2d0/0x83d2d4/0x83d180", "0x4ab87c/0x4ab844", "код"),
    ("S13", "0x4434fa", "контроллеры игроков: 0x453400 -> 0x83d308/0x83d30c", "—",
     "0x83d308/0x83d30c", "", "код"),
    ("S14", "0x44351a", "8 слотов статуса игрока 0x81c370 (шаг 0x3510) обнуляются", "—",
     "0x81c370..0x836bf0", "", "код"),
    ("S15", "0x443565", "0x44be50(0)/0x44be50(1) — сброс управления игроков 0/1", "—",
     "0x5a49f0..0x5a4a6c, 0x5a4958..0x5a49d4", "", "код"),
    ("S16", "0x4435c2", "0x83cb7c = 0x83d300; при 0x83d300 == 4 -> generate_runner(-20)", "SCRIPT",
     "0x83cb7c", "", "код"),
    ("S17", "0x4435ef", "strnicmp(имя,\"SAVEGAME\")==0 -> выход; иначе копия 0x3510 Б статуса "
                        "0x819040: при strnicmp(имя,\"LEVEL0\")==0 -> 0x815440/0x5a4868, иначе "
                        "-> 0x8338c0/0x5a48b8 (0x480d10)",
     "—", "0x819040 -> 0x815440 или 0x8338c0; 0x5a4868/0x5a48b8", "", "код"),
]

SAVE_STEPS = [
    ("V0", "0x442340", "game_load_save(id): имя = \"savegame\\saved%d\"; лог", "—", "0x5a4728", "", "код"),
    ("V1", "0x4423a7", "если прежний уровень был — unload_world 0x428f67", "—", "как S2", "", "код"),
    ("V2", "0x442436", "0x455e60: прочитать заголовок savegame\\saved%d.SBT и имя уровня", "saved%d.sbt",
     "0x5a4818", "0x4accfc", "код"),
    ("V3", "0x442476", "SBC лайтмапов сохранённого уровня 0x429a2a", "saved%d.SBW+SBC",
     "0x5a45c8/0x5a45cc", "0x4ab62c", "код"),
    ("V4", "0x4424d9", "SBW: 0x42aa63(\"savegame\", \"saved%d\")", "savegame\\saved%d.sbw",
     "как S5", "0x4ab5e4", "код"),
    ("V5", "0x44254d", "world_finalize 0x42f183", "—", "как S6", "0x4ab5a4", "код"),
    ("V6", "0x4425d2", "0x454dc0(\"savegame\\saved%d.SBT\", 1) — things-режим (param2=1: "
                       "разрешены SWITCH/DOOR/THING/PLAYER_STATUS/ITEM)", "saved%d.SBT",
     "как S7 + 0x7ac080, 0x81c23c", "0x4ab56c", "код"),
    ("V7", "0x442643..", "дальше как S8..S16, но статус игрока берётся из сейва (тип 9 -> 0x451c20)",
     "saved%d.SBT", "0x81c23c..", "", "код"),
]

RESTART_STEPS = [
    ("R0", "0x48da0b", "фронтенд -> 0x4428f0(mode): рестарт текущего уровня", "—", "—", "", "код"),
    ("R1", "0x442900", "имя: mode&1 ? 0x5a48b8 : 0x5a4868 -> 0x5a47c8", "—", "0x5a47c8", "", "код"),
    ("R2", "0x442a2b", "копия статуса игрока 0x815440/0x8338c0 -> 0x819040 (0x3510 байт)", "—",
     "0x819040", "No level loaded to restart from", "код"),
    ("R3", "0x442a5c", "0x83d300 = (mode&2) ? 2 : 4 (режим спавна)", "—", "0x83d300", "", "код"),
    ("R4", "0x442a69", "-> 0x442ef0 (полная перезагрузка уровня)", "—", "—", "", "код"),
]

#: Порядок перехода уровня из кадра ``0x44c230``.
FRAME_HOOK = ("0x44c2ab: если 0x5a47c8 (отложенное имя) непусто -> "
               "0x83d620 == 0 ? 0x442ef0 : перенос в 0x5a4778; "
               "0x44c2fd: если 0x5a4778 непусто -> 0x442cf0; "
               "ошибка -> 0x4bc440 = 5, 0x83cb7c = 0")

#: (глобал, размер/шаг/лимит, кто заполняет, смысл)
GLOBALS = [
    ("0x5a4728", "имя уровня, 64 Б", "0x442ef0 (из 0x5a47c8)", "текущий уровень"),
    ("0x5a47c8", "имя, 64 Б", "0x442cf0/0x4428f0; 0x44c230", "отложенная загрузка"),
    ("0x5a4778", "имя, 64 Б", "0x442a90 (CLI), 0x44c230", "запрос «начать уровень»"),
    ("0x5a4868", "имя, 64 Б", "0x442ef0/0x4428f0", "имя для рестарта (обычный уровень)"),
    ("0x5a48b8", "имя, 64 Б", "0x442ef0/0x4428f0", "имя для рестарта (savegame)"),
    ("0x5a45c8", "u32", "0x429a2a", "число лайтмапов"),
    ("0x5a45cc", "ptr, 8 Б/запись", "0x429a2a", "лайтмапы: указатель + (w-1)|((h-1)<<8)"),
    ("0x76ed40", "0x800 × 0x10", "0x42aa63", "таблица текстур мира: [0]=?, [4],[5],[6], +7 имя[9]"),
    ("0x5310c0", "0x800 × 4", "0x42aa63", "счётчики ссылок текстуры на уровне"),
    ("0x5350c0", "0x800 × 2", "0x4232b7", "индекс текстуры пула (или 0)"),
    ("0x5670cc", "u32", "0x42aa63; 0x42267a = 0", "число эффектов"),
    ("0x5670d8", "ptr, 0x18/сектор", "0x42aa63; 0x422e9a free", "таблица объектов мира"),
    ("0x5670dc", "u32", "0x42aa63", "число секторов"),
    ("0x5670e0", "ptr, 0x40/запись", "0x42aa63; 0x420bd3 free", "таблица материалов"),
    ("0x5670e4", "u32", "0x42aa63", "число материалов"),
    ("0x638128", "u32 ≤ 200", "0x47c7c0 (SBT 20)", "число записей SOB_TABLE"),
    ("0x637188", "200 × 0x14", "0x47c7c0", "SOB_TABLE: имя[12], amount, ptr SOB/SOO"),
    ("0x5b1df0", "u32 ≤ 0x400", "0x462a80 (SBT 11)", "число скриптов/сущностей"),
    ("0x5b0df0/0x5e4680/0x616658", "0x400 × 4 каждый", "0x462a80", "колонки flags/id/offset"),
    ("0x5e2dd0", "u32 ≤ 0x30000", "0x462a80", "длина скомпилированного кода"),
    ("0x4b6784", "буфер", "0x462a80", "скомпилированный байткод SCRIPT"),
    ("0x80d87c", "u32 ≤ 200", "0x462a80", "число location handle"),
    ("0x80d8e0", "200 × 0x28", "0x462a80", "location handle (позиция/orientation)"),
    ("0x5a9450", "u32 ≤ 100", "0x44e540 (SBT 2)", "число дверей"),
    ("0x8381a0", "100 × 0xB8", "0x44e540", "массив дверей"),
    ("0x5a945c", "u32 ≤ 512", "0x44f000 (SBT 1)", "число переключателей"),
    ("0x5a4af0", "512 × 0x20", "0x44f000", "массив переключателей"),
    ("0x5a9454", "u32 ≤ 30", "0x44ec20 (SBT 18)", "число лифтов"),
    ("0x836de0", "30 × 0xA8", "0x44ec20", "массив лифтов"),
    ("0x5a9458", "u32 ≤ 200", "0x44ec20", "число остановок лифтов"),
    ("0x5a9130/0x5a8e10/0x5a8af0", "200 × 4 каждый", "0x44ec20", "три колонки остановок"),
    ("0x7ac080", "260 × 0x528", "0x46fd90 (SBT 3); 0x46c7d0 reset", "экземпляры thing"),
    ("0x806398", "300 × 0x60, счётчик 0x62cad0", "0x468df0 (SBT 19); 0x468490 reset", "предметы"),
    ("0x638330", "1000 × 0x50, счётчик 0x64bbb4", "ADD_LIGHT_SOURCE (87)", "локальные источники света"),
    ("0x638130", "16 × 0x20, счётчик 0x64bbb0", "ADD_SKY_LIGHT_SOURCE (88)", "небесные источники"),
    ("0x800340/0x800540/0x800140", "128 × 4 каждый", "0x46b280 (SBT 7)", "источники звука"),
    ("0x7ffd20", "0x7ffd20 + 0x10", "0x46c7d0", "хвост массива thing (курсор/конец)"),
    ("0x81c370", "8 × 0x3510", "0x442ef0 S14", "слоты статуса игроков"),
    ("0x819040", "0x3510", "0x4428f0", "текущий статус игрока (для 0x451c20/0x452020)"),
]

#: SBT: тип записи -> (обработчик, нагрузка). Обработчик проверяется по
#: jump-таблице ``0x455604`` + байтовой карте ``0x455658``.
SBT_TYPES = {
    1: ("0x44f000", "SWITCH: u32 n ≤ 0x200 + n*0x20", "0x5a945c, 0x5a4af0"),
    2: ("0x44e540", "DOOR: u32 n < 0x65 + n*0xB8", "0x5a9450, 0x8381a0"),
    3: ("0x46fd90", "THING: u8 exists + 0x528", "0x7ac080"),
    4: ("0x47e000", "THING_TYPES: u32 n ≤ 0x80 + n*936", "типы вещей"),
    5: ("0x468ef0", "NAMED_ITEMS: u32 n ≤ 0x31 + n*52", "0x62cad0, 0x62c0a8"),
    6: ("0x4564b0", "SPRITES: u32 n ≤ 0x1f3 + n*44", "спрайты"),
    7: ("0x46b280", "SOUND_SOURCES: u32 n ≤ 0x7f + 28n", "0x800340.."),
    8: ("0x46b7b0", "SECTOR_STUFF: 0x5800", "—"),
    9: ("0x451c20", "PLAYER_STATUS: u8 + 0x3510", "0x81c23c.., 0x819040"),
    10: ("0x4971b0", "OTHER: заглушка, возвращает -1", "—"),
    11: ("0x462a80", "SCRIPT: things + 3 колонки + код + flag(9*0x400)", "0x5b1df0.."),
    12: ("0x40b670", "SOUND_SAMPLE_TABLE: 300*u32 + 300*9", "сэмплы"),
    13: ("0x4971b0", "OTHER: заглушка, возвращает -1", "—"),
    15: ("0x4633a0", "POLY_HANDLE: n < 0x1f4, 4 колонки", "0x5b21f8.."),
    16: ("0x463680", "OBJECT_HANDLE: n < 0x12c, 3 колонки", "—"),
    17: ("0x463900", "SECTOR_HANDLE: n < 0x100, 2 колонки", "—"),
    18: ("0x44ec20", "LIFT: u32 n ≤ 0x1e (n*0xA8) + u32 k ≤ 0xC8 (k*12)", "0x836de0.."),
    19: ("0x468df0", "ITEM: 0x7080 (только при [0x83d620] != 1)", "0x806360, 0x806398"),
    20: ("0x47c7c0", "SOB_TABLE: u32 n ≤ 0xC8 + n*0x14", "0x638128, 0x637188"),
    255: ("—", "END_OF_FILE", "—"),
}

#: ``call rel32``: (адрес инструкции, ожидаемая цель, что это).
CODE_EDGES = [
    (0x442F7F, 0x428F67, "load_level -> unload_world (старый уровень)"),
    (0x442FFF, 0x429A2A, "load_level -> SBC"),
    (0x44304B, 0x42AA63, "load_level -> SBW (binary)"),
    (0x443078, 0x42A33D, "load_level -> SBW (text fallback)"),
    (0x4430CC, 0x42F183, "load_level -> world_finalize"),
    (0x44312C, 0x454DC0, "load_level -> SBT (binary)"),
    (0x443155, 0x454B00, "load_level -> SBT (text fallback)"),
    (0x4431D9, 0x42AFBB, "load_level -> save binary SBW (/sbm)"),
    (0x443219, 0x455760, "load_level -> save binary SBT (/sbm)"),
    (0x443272, 0x4847F0, "load_level -> сброс сцены"),
    (0x44327A, 0x44F260, "load_level -> привязка дверей/лифтов"),
    (0x443387, 0x4232B7, "load_level -> init текстур"),
    (0x4433C3, 0x455F30, "load_level -> update sprites"),
    (0x443449, 0x453120, "load_level -> create player 1"),
    (0x4434A6, 0x453120, "load_level -> create player 2"),
    (0x4434FA, 0x453400, "load_level -> контроллер игрока 1"),
    (0x44350D, 0x453400, "load_level -> контроллер игрока 2"),
    (0x443565, 0x44BE50, "load_level -> сброс управления (игрок 0)"),
    (0x443591, 0x44BE50, "load_level -> сброс управления (игрок 1)"),
    (0x4435DB, 0x45FA00, "load_level -> generate_runner(-20)"),
    (0x4435EF, 0x480D10, "load_level -> сверка имени (SAVEGAME)"),
    (0x4423A7, 0x428F67, "save_load -> unload_world"),
    (0x442436, 0x455E60, "save_load -> чтение заголовка savegame"),
    (0x442476, 0x429A2A, "save_load -> SBC"),
    (0x4424D9, 0x42AA63, "save_load -> SBW"),
    (0x44254D, 0x42F183, "save_load -> world_finalize"),
    (0x4425D2, 0x454DC0, "save_load -> SBT (things-режим)"),
    (0x454DCF, 0x454CF0, "sbt_load -> reset (0x454cf0)"),
    (0x455194, 0x44EC20, "SBT 18 (LIFT) -> 0x44ec20"),
    (0x455202, 0x462A80, "SBT 11 (SCRIPT) -> 0x462a80"),
    (0x455244, 0x47C7C0, "SBT 20 (SOB_TABLE) -> 0x47c7c0"),
    (0x455286, 0x4971B0, "SBT 13 (OTHER) -> заглушка"),
    (0x454CF0, 0x47C7B0, "reset -> обнулить SOB_TABLE (count)"),
    (0x454CF5, 0x44DFA0, "reset -> обнулить счётчики doors/switches/lifts"),
    (0x454CFA, 0x455F00, "reset -> таблица спрайтов"),
    (0x454CFF, 0x46C7D0, "reset -> things (0x7ac080)"),
    (0x454D0E, 0x46B1B0, "reset -> источники звука"),
    (0x454D13, 0x456F50, "reset -> эффекты"),
]

#: Байтовые сигнатуры: (адрес, hex, что доказывает).
IMM = [
    (0x442EF0, "a028475a00", "load_level: mov al,[0x5a4728] (имя уровня)"),
    (0x44E581, "83f864", "door_loadbin: cmp eax,0x64 (лимит 100)"),
    (0x44F02C, "3d00020000", "switch_loadbin: cmp eax,0x200 (лимит 512)"),
    (0x44EC77, "83f91e", "lift_loadbin: cmp ecx,0x1e (лимит 30)"),
    (0x47C7F1, "3dc8000000", "sob_table_load: cmp eax,0xc8 (лимит 200)"),
    (0x462ABF, "3dc8000000", "script_load: cmp eax,0xc8 (location handle)"),
    (0x462B3A, "3d00040000", "script_load: cmp eax,0x400 (скриптов ≤ 1024)"),
    (0x429A79, "817df053424330", "sbc_load: cmp magic,'SBC0'"),
    (0x429A82, "817df053424331", "sbc_load: cmp magic,'SBC1'"),
    (0x45516F, "3dfe000000", "sbt dispatcher: тип 1..0xff"),
    (0x455182, "ff248d04564500", "sbt dispatcher: jmp [ecx*4+0x455604]"),
    (0x462A8B, "687cd88000", "script_load: push 0x80d87c (location handle)"),
    (0x44E557, "83f801", "door_loadbin: чтение count"),
    (0x44EC41, "83f801", "lift_loadbin: чтение count"),
]

#: Строки: (адрес, текст, роль).
STRINGS = [
    (0x4AB5E4, "Unable to load world file (%s).", "SBW не загрузился -> 0xfffffffe"),
    (0x4AB56C, "Unable to load world things file (%s).", "SBT не загрузился -> 0xfffffffd"),
    (0x4AB62C, "Warning, unable to load level lightmaps (SBC file missing).\n",
     "SBC отсутствует — предупреждение, не ошибка"),
    (0x4AB910, "Unable to load world things file (%s)\n", "лог SBT-ошибки"),
    (0x4AB98C, "Unable to load world (%s)\n", "лог SBW-ошибки"),
    (0x4AB970, "Unable to engine_initall()\n", "0x42f183 упал -> 0xfffffffe"),
    (0x4AB948, "Unable to initalize rendering engine.", "то же, текст для UI"),
    (0x4AB528, "Unable to load textures.", "0x4232b7 -> 0xfffffffa"),
    (0x4AB50C, "Unable to update sprites.", "0x455f30 -> 0xfffffff9"),
    (0x4AB87C, "Unable to create player 1\n", "0x453120 -> 0xfffffffd"),
    (0x4AB844, "Unable to create player 2\n", "0x453120 -> 0xfffffffd"),
    (0x4AB8E0, "Unable to save binary world file '%s'\n", "/sbm: запись .sbw"),
    (0x4AB8AC, "Unable to save binary thing file '%s'\n", "/sbm: запись .sbt"),
    (0x4AB940, "%s.SBT", "имя SBT"),
    (0x4AB938, "%s.SOT", "имя текстового things"),
    (0x4AB908, "%s.SBW", "имя SBW"),
    (0x4A98EC, "sbc", "расширение bigfile для 0x40d1a0"),
    (0x4A9994, "sbw", "расширение bigfile для 0x40d1a0"),
    (0x4AB6A4, "savegame\\saved%d", "база имён сейва"),
    (0x4AB66C, "savegame\\saved%d.SBT", "SBT сейва"),
    (0x4AB624, "saved%d", "имя .sbw сейва"),
    (0x4ACAC8, "World Things file (%s) corrupt. Last keyword=%ld.\n", "неизвестный тип записи"),
    (0x4ACAA4, "Unsupported SBT version number.\n", "version != 0"),
    (0x4BCC04, "SML\\PRELEVEL\\%s.SML", "экран загрузки 0x484a20"),
    (0x4AB6DC, "No level loaded to restart from.", "0x4428f0 без имени"),
]

#: Ожидаемые значения на данных игры (независимый источник — ``gm.py``/``docs``).
EXPECT_LEVELS = {
    "level0": {
        "sbw": dict(textures=381, effects=37, sectors=58, objects=192, polygons=1449,
                    bytes=254439),
        "sbt": dict(bytes=127338, records=8),
        "counts": dict(lights=29, spawns=8, doors=25, switches=24, lifts=0),
    },
    "dm1": {
        "sbw": dict(textures=497, effects=36, sectors=83, objects=802, polygons=5786,
                    bytes=1008875),
        "sbt": dict(bytes=151516, records=9),
        "counts": dict(lights=60, spawns=13, doors=61, switches=129, lifts=0),
    },
}

EXPECT_CORPUS = dict(doors=1779, switches=3964, lifts=45, levels=36)


# --------------------------------------------------------------------------- #
# FMT-парсеры (только stdlib; формат — docs/FORMATS.md)
# --------------------------------------------------------------------------- #
def _u32(buf: bytes, o: int) -> int:
    return struct.unpack_from("<I", buf, o)[0]


def _u16(buf: bytes, o: int) -> int:
    return struct.unpack_from("<H", buf, o)[0]


def _cstr(buf: bytes, o: int) -> tuple[str, int]:
    end = buf.find(b"\0", o)
    if end < 0:
        raise ValueError("unterminated string at 0x%x" % o)
    return buf[o:end].decode("latin-1"), end + 1


def parse_sbw(buf: bytes) -> dict:
    """Счётчики ``.sbw`` (стадия 1 + стадия 2), проверка «ровно до EOF»."""
    o = 0
    ntex = _u32(buf, o)
    o += 4
    for _ in range(ntex):
        _, o = _cstr(buf, o)
        o += 2 + 4                       # u16 flag + u32 value
    neff = _u32(buf, o)
    o += 4
    for _ in range(neff):
        _, o = _cstr(buf, o)
        _kind, _start, n = struct.unpack_from("<III", buf, o)
        o += 12 + 2 * n
    nsec = _u32(buf, o)
    o += 4
    objects = []
    for _ in range(nsec):
        nobj = _u32(buf, o)
        o += 4 + 2 + 1                   # u32 count + u16 kind + u8 sub
        for _ in range(nobj):
            o += 4 + 96                  # u32 flags + 96 байт заголовка
            objects.append(1)
    verts = polys = 0
    for _ in objects:
        nv, np_ = struct.unpack_from("<HH", buf, o)
        o += 4 + 12 * nv + 50
        for _ in range(np_):
            n_idx = buf[o + 11]
            o += 12 + 2 * n_idx + 112
            polys += 1
        verts += nv
    return dict(textures=ntex, effects=neff, sectors=nsec, objects=len(objects),
                vertices=verts, polygons=polys, bytes=o, file_bytes=len(buf),
                to_eof=(o == len(buf)))


def parse_sbc(buf: bytes) -> dict:
    magic = buf[:4]
    if magic not in (b"SBC0", b"SBC1"):
        raise ValueError("bad SBC magic %r" % magic)
    n = _u32(buf, 4)
    o = 8
    sizes = []
    for _ in range(n):
        desc = _u32(buf, o)
        o += 4
        w = desc & 0xFFFF
        h = desc >> 16
        if w > 8 or h > 8:
            raise ValueError("SBC record %d: w=%d h=%d out of range" % (len(sizes), w, h))
        o += (1 << w) * (1 << h)
        sizes.append((w, h))
    return dict(magic=magic.decode(), records=n, sizes=sizes, bytes=o,
                file_bytes=len(buf), to_eof=(o == len(buf)))


#: Нагрузка SBT по типу: функция получает (buf, offset) и возвращает (info, new_offset).
def _sbt_record(buf: bytes, o: int) -> tuple[dict, int]:
    if o >= len(buf):
        raise ValueError("record header past EOF")
    t = buf[o]
    o += 1
    info = {"type": t}
    if t == 1:
        n = _u32(buf, o); o += 4
        if n > 0x200:
            raise ValueError("SWITCH count %d > 512" % n)
        info.update(count=n)
        o += 32 * n
    elif t == 2:
        n = _u32(buf, o); o += 4
        if n > 100:
            raise ValueError("DOOR count %d > 100" % n)
        info.update(count=n)
        o += 184 * n
    elif t == 3:
        exists = buf[o]; o += 1
        info.update(exists=exists)
        if exists:
            o += 0x528
    elif t == 4:
        n = _u32(buf, o); o += 4
        if n > 0x80:
            raise ValueError("THING_TYPES count %d > 128" % n)
        info.update(count=n)
        o += 936 * n
    elif t == 5:
        n = _u32(buf, o); o += 4
        if n > 0x31:
            raise ValueError("NAMED_ITEMS count %d > 49" % n)
        info.update(count=n)
        o += 52 * n
    elif t == 6:
        n = _u32(buf, o); o += 4
        if n > 0x1F3:
            raise ValueError("SPRITES count %d > 499" % n)
        info.update(count=n)
        o += 44 * n
    elif t == 7:
        n = _u32(buf, o); o += 4
        if n > 0x7F:
            raise ValueError("SOUND_SOURCES count %d > 127" % n)
        info.update(count=n)
        o += 28 * n
    elif t == 8:
        o += 0x5800
    elif t == 9:
        exists = buf[o]; o += 1
        info.update(exists=exists)
        if exists:
            o += 0x3510
    elif t == 11:
        n = _u32(buf, o); o += 4
        things = n
        o += 40 * n
        m = _u32(buf, o); o += 4
        if m > 0x400:
            raise ValueError("SCRIPT handles %d > 1024" % m)
        o += 12 * m
        code = _u32(buf, o); o += 4
        if code > 0x30000:
            raise ValueError("SCRIPT code %d > 0x30000" % code)
        code_off = o
        o += code
        flag = buf[o]; o += 1
        if flag:
            o += 9 * 0x400
        info.update(things=things, handles=m, code=code, code_off=code_off, flag=flag)
    elif t == 12:
        o += 1200 + 2700
    elif t == 15:
        n = _u32(buf, o); o += 4
        if n >= 0x1F4:
            raise ValueError("POLY_HANDLE count %d >= 500" % n)
        info.update(count=n)
        o += 16 * n
    elif t == 16:
        n = _u32(buf, o); o += 4
        if n >= 0x12C:
            raise ValueError("OBJECT_HANDLE count %d >= 300" % n)
        info.update(count=n)
        o += 12 * n
    elif t == 17:
        n = _u32(buf, o); o += 4
        if n >= 0x100:
            raise ValueError("SECTOR_HANDLE count %d >= 256" % n)
        info.update(count=n)
        o += 8 * n
    elif t == 18:
        n = _u32(buf, o); o += 4
        if n > 0x1E:
            raise ValueError("LIFT count %d > 30" % n)
        k = _u32(buf, o + 168 * n)
        if k > 0xC8:
            raise ValueError("LIFT stops %d > 200" % k)
        info.update(count=n, stops=k)
        o += 4 + 168 * n + 4 + 12 * k
    elif t == 19:
        o += 0x7080
    elif t == 20:
        n = _u32(buf, o); o += 4
        if n > 0xC8:
            raise ValueError("SOB_TABLE count %d > 200" % n)
        info.update(count=n)
        o += 20 * n
    elif t in (10, 13):
        raise ValueError("record type %d is a stub returning -1" % t)
    elif t == 0xFF:
        info.update(end=True)
    else:
        raise ValueError("unknown record type %d at 0x%x" % (t, o - 1))
    if o > len(buf):
        raise ValueError("record type %d runs past EOF" % t)
    return info, o


def parse_sbt(buf: bytes) -> dict:
    if len(buf) < 84:
        raise ValueError("SBT shorter than 84-byte header")
    magic = buf[:80].split(b"\0")[0]
    version = _u32(buf, 80)
    if version != 0:
        raise ValueError("unsupported SBT version %d" % version)
    o = 84
    records = []
    while o < len(buf):
        info, o = _sbt_record(buf, o)
        records.append(info)
        if info.get("end"):
            break
    return dict(magic=magic.decode("latin-1"), version=version, records=records,
                bytes=o, file_bytes=len(buf), to_eof=(o == len(buf)))


# --------------------------------------------------------------------------- #
# Опкоды SCRIPT: таблицы берутся из самого exe (0x4b53e8 / 0x4b62e8)
# --------------------------------------------------------------------------- #
SIZE_OF = {"A": 8, "B": 8, "D": 8, "Y": 1}


def opcode_tables(img: Image) -> tuple[dict, dict]:
    def table(base: int, count: int) -> dict:
        out = {}
        for i in range(count):
            nptr, sptr, _flag, _neg1, cid = struct.unpack(
                "<5I", img.raw(base + ENTRY_STRIDE * i, ENTRY_STRIDE))
            out[cid] = (img.cstr(nptr, 64), img.cstr(sptr, 64))
        return out
    return table(*TABLE_COMMANDS), table(*TABLE_CONDITIONS)


def _arg_size(sig: str) -> int:
    total = 0
    for ch in sig:
        if ch == "S":
            raise ValueError("string operand in a numeric signature")
        total += SIZE_OF.get(ch, 4)
    return total


def _skip_args(buf: bytes, o: int, sig: str) -> int:
    for ch in sig:
        if ch == "S":
            _, o = _cstr(buf, o)
        else:
            o += SIZE_OF.get(ch, 4)
    return o


def walk_script(code: bytes, cmds: dict, conds: dict, start: int = 0,
                end: int | None = None, depth: int = 0, counts: dict | None = None,
                visit=None) -> int:
    """Пройти байткод SCRIPT; вернуть позицию после последней инструкции.

    Повторяет разбор ``gm/sbtvm.py``: обычные команды — сигнатура из таблицы
    exe, структурные (103/158/170/171/172/175/188/199/200/201) — вручную.
    """
    if counts is None:
        counts = {}
    if end is None:
        end = len(code)
    if depth > 32:
        raise ValueError("SCRIPT nesting deeper than 32")
    i = start
    while i < end:
        off = i
        b = code[i]
        if b == 0xFF:
            i += 1
            continue
        if b == 199:                      # IF <cond> <size> { body } [ELSE]
            cid = code[i + 1]
            sig = conds.get(cid, ("OP_%d" % cid, ""))[1] or ""
            j = _skip_args(code, i + 2, sig)
            size = _u32(code, j)
            body_end = j + size
            if body_end > end:
                raise ValueError("IF body at 0x%x past end" % off)
            walk_script(code, cmds, conds, j + 4, body_end, depth + 1, counts, visit)
            i = body_end
            if i < end and code[i] == 200:
                rel = _u32(code, i + 1)
                tgt = i + 1 + rel
                # ELSE может прыгнуть за границу текущего блока (к концу
                # объемлющего подскрипта) — gm/sbtvm.py это допускает.
                if tgt > len(code):
                    raise ValueError("ELSE target at 0x%x past code end" % off)
                walk_script(code, cmds, conds, i + 5, tgt, depth + 1, counts, visit)
                i = tgt
            continue
        if b == 200:                      # ELSE без IF — относительный переход
            rel = _u32(code, i + 1)
            i = i + 1 + rel
            continue
        if b == 201:                      # EVAL <cond> <operands>
            cid = code[i + 1]
            sig = conds.get(cid, ("OP_%d" % cid, ""))[1] or ""
            i = _skip_args(code, i + 2, sig)
            continue
        if b == 103:                      # DEFINE_LEVEL_OBJECTIVES
            n = _u32(code, i + 1)
            i += 5
            for _ in range(n):
                _, i = _cstr(code, i)
            continue
        if b == 158:                      # SOURCEPLAYER_PATH
            n = _u32(code, i + 1)
            i += 5 + 4 * n
            continue
        if b in (170, 171, 172, 175):     # DEFINE_WEAPONS/AMMO/PLAYER_TYPES/EQUIPMENT
            i = _skip_table(code, i + 1, b)
            continue
        if b == 188:                      # ADD_LIFT
            n = _u32(code, i + 1 + 0x94)
            if visit:
                visit("ADD_LIFT", off, n)
            counts["ADD_LIFT"] = counts.get("ADD_LIFT", 0) + 1
            i = i + 1 + 0x94 + 4 + 12 * n
            continue
        if b not in cmds:
            raise ValueError("unknown opcode %d at 0x%x" % (b, off))
        name, sig = cmds[b]
        if visit:
            visit(name, off, None)
        counts[name] = counts.get(name, 0) + 1
        i = _skip_args(code, i + 1, sig)
    return i


#: структурные DEFINE_* (сигнатура "строка + запись", см. gm/sbtvm.py)
TABLE_SIGS = {
    170: ("SSS", 0x54),               # DEFINE_WEAPONS
    171: ("S", 0x28),                 # DEFINE_AMMO
    172: ("SSS", 0x17C),              # DEFINE_PLAYER_TYPES
    175: ("S", 0x30),                 # DEFINE_EQUIPMENT
}


def _skip_table(code: bytes, o: int, op: int) -> int:
    sig, tail = TABLE_SIGS[op]
    n = _u32(code, o)
    o += 4
    for _ in range(n):
        for ch in sig:
            _, o = _cstr(code, o)
        o += tail
    return o


def script_counts(code: bytes, cmds: dict, conds: dict) -> dict:
    """Счётчики команд уровня + проверка «код прочитан целиком»."""
    counts: dict = {}
    end = walk_script(code, cmds, conds, 0, len(code), 0, counts)
    counts["_end"] = end
    return counts


def classify(counts: dict) -> dict:
    """Свернуть счётчики опкодов в «сцену» (как ``gm.py scene``)."""
    lights = counts.get("ADD_LIGHT_SOURCE", 0) + counts.get("ADD_SKY_LIGHT_SOURCE", 0)
    spawns = counts.get("SET_PLAYER_START", 0) + counts.get("SET_DEATHMATCH_START", 0)
    things = sum(v for k, v in counts.items()
                 if k.startswith("ADD_THING") or k.startswith("SOURCEPLAYER_PATH"))
    return dict(lights=lights, spawns=spawns,
                doors=counts.get("ADD_DOOR", 0),
                switches=counts.get("ADD_SWITCH", 0),
                lifts=counts.get("ADD_LIFT", 0),
                locations=counts.get("ADD_LOCATION", 0),
                items=counts.get("ADD_ITEM", 0) + counts.get("ADD_ITEM_FIXED", 0),
                sounds=counts.get("ADD_SOUND_SOURCE", 0),
                sprites=counts.get("ADD_SPRITE", 0),
                things=things)


# --------------------------------------------------------------------------- #
# Проверки
# --------------------------------------------------------------------------- #
def first_call(img: Image, va: int, window: int = 40) -> int:
    """Цель первого ``call rel32`` в окне *window* байт от *va* (0 — не найдено)."""
    for i in range(window):
        if img.u8(va + i) == 0xE8:
            return img.call_target(va + i)
        if img.u8(va + i) in (0xC3, 0xCB):
            break
    return 0


class Report:
    def __init__(self, quiet: bool = False):
        self.fails = []
        self.checks = 0
        self.quiet = quiet

    def check(self, ok: bool, label: str, detail: str = "") -> bool:
        self.checks += 1
        if not ok:
            self.fails.append(label + ((" — " + detail) if detail else ""))
            print("  FAIL %s%s" % (label, (" — " + detail) if detail else ""))
        elif not self.quiet:
            print("  ok   %s%s" % (label, (" — " + detail) if detail else ""))
        return ok

    def summary(self) -> int:
        if self.fails:
            print("\nИТОГ: %d проверок, %d провалов" % (self.checks, len(self.fails)))
            for f in self.fails:
                print("  ! " + f)
            return 1
        print("\nИТОГ: %d проверок, все прошли" % self.checks)
        return 0


def verify_exe(img: Image, rep: Report) -> None:
    print("== exe: строки, байты, call-рёбра ==")
    for va, text, note in STRINGS:
        got = img.cstr(va, 300)
        rep.check(got == text, "строка 0x%x %r" % (va, text[:40]), "получено %r" % got[:60])
    for va, hexs, note in IMM:
        got = img.raw(va, len(hexs) // 2).hex()
        rep.check(got == hexs, "байты 0x%x (%s)" % (va, note), "получено %s" % got)
    for va, target, note in CODE_EDGES:
        try:
            got = img.call_target(va)
        except ValueError as exc:
            rep.check(False, "call 0x%x -> 0x%x" % (va, target), str(exc))
            continue
        rep.check(got == target, "call 0x%x -> 0x%x (%s)" % (va, target, note),
                  "получено 0x%x" % got)
    print("\n== exe: диспетчер SBT (jump-таблица 0x455604) ==")
    jt = [img.u32(0x455604 + 4 * i) for i in range(0x100)]
    bmap = [img.u8(0x455658 + i) for i in range(255)]
    case = {t: jt[bmap[t - 1]] for t in range(1, 256)}
    for t, (handler, _payload, _glob) in SBT_TYPES.items():
        if handler == "—":
            rep.check(case[t] == 0x455189, "тип 255 -> конец файла", "0x%x" % case[t])
            continue
        call = first_call(img, case[t])
        rep.check(call == int(handler, 16),
                  "тип %d: case 0x%x -> %s" % (t, case[t], handler),
                  "получено 0x%x" % call)
    stubs = {t for t, c in case.items() if c == 0x4555AE}
    rep.check(stubs == set(range(14, 255)) - set(SBT_TYPES), "неизвестные типы -> 0x4555ae",
              "несовпадение: %s" % sorted(stubs ^ (set(range(14, 255)) - set(SBT_TYPES)))[:8])


def verify_level(img: Image, name: str, rep: Report, cmds: dict, conds: dict) -> None:
    exp = EXPECT_LEVELS.get(name)
    sbw_path = GAME_DIR / (name + ".sbw")
    sbt_path = GAME_DIR / (name + ".sbt")
    sbc_path = GAME_DIR / (name + ".sbc")
    print("\n== %s: файлы уровня ==" % name)
    if not sbw_path.exists():
        rep.check(False, "%s.sbw найден" % name, str(sbw_path))
        return
    sbw = parse_sbw(sbw_path.read_bytes())
    print("  sbw: %(textures)d текстур, %(effects)d эффектов, %(sectors)d секторов, "
          "%(objects)d объектов, %(vertices)d вершин, %(polygons)d полигонов, "
          "%(file_bytes)d Б" % sbw)
    for key in ("textures", "effects", "sectors", "objects", "polygons"):
        rep.check(sbw[key] == exp["sbw"][key], "sbw.%s == %d" % (key, exp["sbw"][key]),
                  "получено %d" % sbw[key])
    rep.check(sbw["file_bytes"] == exp["sbw"]["bytes"], "sbw размер == %d" % exp["sbw"]["bytes"],
              "получено %d" % sbw["file_bytes"])
    rep.check(sbw["to_eof"], "sbw разобран ровно до EOF", "%d/%d" % (sbw["bytes"], sbw["file_bytes"]))

    sbt = parse_sbt(sbt_path.read_bytes())
    seq = [r["type"] for r in sbt["records"]]
    print("  sbt: %(file_bytes)d Б, записи %(records)s" % dict(sbt, records=seq))
    rep.check(len(seq) == exp["sbt"]["records"], "sbt записей == %d" % exp["sbt"]["records"],
              "получено %d" % len(seq))
    rep.check(sbt["file_bytes"] == exp["sbt"]["bytes"], "sbt размер == %d" % exp["sbt"]["bytes"],
              "получено %d" % sbt["file_bytes"])
    rep.check(sbt["to_eof"], "sbt разобран ровно до EOF",
              "%d/%d" % (sbt["bytes"], sbt["file_bytes"]))
    by_type = {r["type"]: r for r in sbt["records"]}
    for t in (20, 11):
        rep.check(t in by_type, "sbt содержит запись типа %d" % t)
    if 11 in by_type:
        rec = by_type[11]
        code = sbt_path.read_bytes()[rec["code_off"]:rec["code_off"] + rec["code"]]
        counts = script_counts(code, cmds, conds)
        rep.check(counts["_end"] == len(code), "SCRIPT-код %s разобран целиком" % name,
                  "%d/%d" % (counts["_end"], len(code)))
        scene = classify(counts)
        print("  script: %(lights)d lights, %(spawns)d spawns, %(doors)d doors, "
              "%(switches)d switches, %(lifts)d lifts, %(locations)d locations" % scene)
        for key in ("lights", "spawns", "doors", "switches", "lifts"):
            rep.check(scene[key] == exp["counts"][key],
                      "script.%s == %d" % (key, exp["counts"][key]),
                      "получено %d" % scene[key])

    if sbc_path.exists():
        sbc = parse_sbc(sbc_path.read_bytes())
        print("  sbc: %(magic)s, %(records)d лайтмапов, %(file_bytes)d Б" % sbc)
        rep.check(sbc["to_eof"], "sbc разобран ровно до EOF",
                  "%d/%d" % (sbc["bytes"], sbc["file_bytes"]))
    else:
        print("  sbc: нет файла (движок печатает warning 0x4ab62c)")


def verify_corpus(img: Image, rep: Report, cmds: dict, conds: dict) -> None:
    print("\n== корпус: все .sbt ==")
    files = sorted(GAME_DIR.glob("[A-Za-z]*.sbt")) + sorted(GAME_DIR.glob("[A-Za-z]*.SBT"))
    files = sorted({p.resolve() for p in files})
    totals = dict(doors=0, switches=0, lifts=0)
    errors = []
    for path in files:
        try:
            sbt = parse_sbt(path.read_bytes())
            for rec in sbt["records"]:
                if rec["type"] == 1:
                    totals["switches"] += rec["count"]
                elif rec["type"] == 2:
                    totals["doors"] += rec["count"]
                elif rec["type"] == 18:
                    totals["lifts"] += rec["count"]
                elif rec["type"] == 11:
                    code = path.read_bytes()[rec["code_off"]:rec["code_off"] + rec["code"]]
                    scene = classify(script_counts(code, cmds, conds))
                    totals["doors"] += scene["doors"]
                    totals["switches"] += scene["switches"]
                    totals["lifts"] += scene["lifts"]
        except (ValueError, struct.error, OSError) as exc:
            errors.append("%s: %s" % (path.name, exc))
    print("  файлов %d, двери %d, переключатели %d, лифты %d"
          % (len(files), totals["doors"], totals["switches"], totals["lifts"]))
    rep.check(not errors, "все .sbt разобраны", "; ".join(errors[:3]))
    rep.check(len(files) == EXPECT_CORPUS["levels"],
              "уровней == %d" % EXPECT_CORPUS["levels"], "получено %d" % len(files))
    for key in ("doors", "switches", "lifts"):
        rep.check(totals[key] == EXPECT_CORPUS[key], "корпус.%s == %d" % (key, EXPECT_CORPUS[key]),
                  "получено %d" % totals[key])


def verify(img: Image, levels: list[str], rep: Report) -> int:
    cmds, conds = opcode_tables(img)
    rep.check(len(cmds) == 192, "таблица COMMANDS: 192 записи", "получено %d" % len(cmds))
    rep.check(len(conds) == 59, "таблица CONDITIONS: 59 записей", "получено %d" % len(conds))
    verify_exe(img, rep)
    for name in levels:
        verify_level(img, name, rep, cmds, conds)
    verify_corpus(img, rep, cmds, conds)
    return rep.summary()


# --------------------------------------------------------------------------- #
# Печать карты
# --------------------------------------------------------------------------- #
def print_steps() -> None:
    def table(title: str, rows, with_status: bool = True) -> None:
        print("\n## %s\n" % title)
        head = "| шаг | адрес | что делает | файл/буфер | глобалы | ошибка |"
        sep = "|---|---|---|---|---|---|"
        if with_status:
            head += " статус |"
            sep += "---|"
        print(head)
        print(sep)
        for row in rows:
            cells = [str(c) for c in row]
            print("| " + " | ".join(cells) + " |")

    print("# Загрузка/выгрузка уровня (M2.2.3)")
    print("\nВход: 0x442ef0 (уровень), 0x442340 (сейв), 0x4428f0 (рестарт).")
    print("\n## Переход из кадра\n\n" + FRAME_HOOK)
    table("0x442ef0 — load_level", STEPS)
    table("0x442340 — game_load_save", SAVE_STEPS)
    table("0x4428f0 — game_restartlevel", RESTART_STEPS)


def print_globals() -> None:
    print("| глобал | размер/шаг/лимит | кто заполняет | смысл |")
    print("|---|---|---|---|")
    for name, size, who, what in GLOBALS:
        print("| `%s` | %s | %s | %s |" % (name, size, who, what))


def print_records() -> None:
    print("| тип | обработчик | нагрузка | глобалы |")
    print("|---|---|---|---|")
    for t in sorted(SBT_TYPES):
        handler, payload, glob = SBT_TYPES[t]
        print("| %d | `%s` | %s | %s |" % (t, handler, payload, glob))


# --------------------------------------------------------------------------- #
# Самопроверка
# --------------------------------------------------------------------------- #
def _synthetic_pe() -> bytes:
    """Минимальный PE32: одна секция .text, строка и dword по известным VA."""
    payload = bytearray(0x200)
    msg = b"hello-level-load\0"
    payload[0x10:0x10 + len(msg)] = msg
    struct.pack_into("<I", payload, 0x40, 0xDEADBEEF)
    payload[0x50] = 0xE8                          # call rel32 -> 0x401065
    struct.pack_into("<i", payload, 0x51, 0x10)

    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)             # e_lfanew -> 0x40
    pe = bytearray(0x1C0)
    pe[0:4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", pe, 4, 0x14C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = 24
    struct.pack_into("<H", pe, opt, 0x10B)
    struct.pack_into("<I", pe, opt + 16, 0x1000)       # AddressOfEntryPoint
    struct.pack_into("<I", pe, opt + 28, 0x400000)     # ImageBase
    sec = opt + 0xE0
    pe[sec:sec + 8] = b".text\0\0\0"
    # VA 0x401000 -> raw 0x200 (= 0x40 + len(pe))
    struct.pack_into("<IIII", pe, sec + 8, 0x1000, 0x1000, 0x200, 0x40 + len(pe))
    return bytes(dos) + bytes(pe) + bytes(payload)


def _op_by_name(cmds: dict, name: str) -> int:
    for cid, (nm, _sig) in cmds.items():
        if nm == name:
            return cid
    raise KeyError(name)


def _emit(cmds: dict, name: str) -> bytes:
    """Инструкция с нулевыми операндами по сигнатуре из таблицы exe."""
    cid = _op_by_name(cmds, name)
    sig = cmds[cid][1]
    return bytes([cid]) + bytes(_arg_size(sig))


def _synthetic_files(cmds: dict) -> dict:
    # SBW: 1 текстура, 1 эффект, 1 сектор с 1 объектом, 2 вершины, 2 полигона
    sbw = bytearray()
    sbw += struct.pack("<I", 1) + b"tex\0" + struct.pack("<HI", 1, 1936)
    sbw += struct.pack("<I", 1) + b"fx\0" + struct.pack("<III", 1, 10, 2) + struct.pack("<HH", 1, 2)
    sbw += struct.pack("<I", 1) + struct.pack("<I", 1) + struct.pack("<H", 6) + b"\0"
    sbw += struct.pack("<I", 8) + bytes(96)
    sbw += struct.pack("<HH", 2, 2)
    sbw += b"".join(struct.pack("<3f", float(i), 0.0, 0.0) for i in range(2))
    sbw += struct.pack("<f", 1.0) + bytes(1) + bytes(1) + bytes(8) + struct.pack("<3f", 0, 0, 0)
    sbw += bytes(24)
    for n_idx in (3, 4):
        sbw += struct.pack("<5H", 1, 0, 0, 0, 0) + bytes([16, n_idx])
        sbw += struct.pack("<%dH" % n_idx, *range(n_idx))
        sbw += bytes(112)
    # SBC: 2 лайтмапа 2x2
    sbc = bytearray(b"SBC1") + struct.pack("<I", 2)
    for _ in range(2):
        sbc += struct.pack("<I", 2 | (2 << 16)) + bytes(16)

    # SBT: SOB_TABLE(20) + SCRIPT(11) + END
    code = bytearray()
    for name in ("ADD_LIGHT_SOURCE", "ADD_SKY_LIGHT_SOURCE", "SET_PLAYER_START"):
        code += _emit(cmds, name)
    code += bytes([0xFF])
    sbt = bytearray(b"BINARY SAVE".ljust(80, b"\0")) + struct.pack("<I", 0)
    sbt += bytes([20]) + struct.pack("<I", 1) + b"tank\0".ljust(12, b"\0") + struct.pack("<II", 1, 0)
    sbt += bytes([11]) + struct.pack("<I", 0) + struct.pack("<I", 0)
    sbt += struct.pack("<I", len(code)) + bytes(code) + b"\0"
    sbt += bytes([0xFF])
    return dict(sbw=bytes(sbw), sbc=bytes(sbc), sbt=bytes(sbt))


def _synthetic_script(cmds: dict, cond: int = 0) -> bytes:
    """Байткод с IF/ELSE вокруг ADD_LIGHT_SOURCE (проверяет рекурсию)."""
    body = _emit(cmds, "ADD_LIGHT_SOURCE") + bytes([0xFF])
    else_body = _emit(cmds, "ADD_SKY_LIGHT_SOURCE") + bytes([0xFF])
    out = bytearray()
    out += bytes([199, cond]) + struct.pack("<I", len(body) + 4)
    out += body
    out += bytes([200]) + struct.pack("<I", len(else_body) + 4)
    out += else_body
    out += bytes([0xFF])
    return bytes(out)


def self_test(img: Image) -> int:
    rep = Report(quiet=True)
    cmds, conds = opcode_tables(img)
    print("== self-test: PE ==")
    tiny = Image(_synthetic_pe())
    rep.check(tiny.base == 0x400000, "PE: ImageBase 0x400000")
    rep.check(tiny.cstr(0x401010) == "hello-level-load", "PE: cstr по VA",
              repr(tiny.cstr(0x401010)))
    rep.check(tiny.u32(0x401040) == 0xDEADBEEF, "PE: u32 по VA")
    rep.check(tiny.call_target(0x401050) == 0x401065, "PE: call rel32",
              hex(tiny.call_target(0x401050)))

    print("== self-test: форматы ==")
    syn = _synthetic_files(cmds)
    sbw = parse_sbw(syn["sbw"])
    rep.check((sbw["textures"], sbw["effects"], sbw["sectors"], sbw["objects"],
               sbw["vertices"], sbw["polygons"]) == (1, 1, 1, 1, 2, 2),
              "SBW: счётчики", str(sbw))
    rep.check(sbw["to_eof"], "SBW: ровно до EOF")
    sbc = parse_sbc(syn["sbc"])
    rep.check(sbc["records"] == 2 and sbc["to_eof"], "SBC: 2 записи, до EOF", str(sbc))
    sbt = parse_sbt(syn["sbt"])
    rep.check([r["type"] for r in sbt["records"]] == [20, 11, 255], "SBT: последовательность",
              str([r["type"] for r in sbt["records"]]))
    rep.check(sbt["records"][0]["count"] == 1, "SBT: SOB_TABLE count")
    rep.check(sbt["to_eof"], "SBT: ровно до EOF")

    print("== self-test: VM ==")
    code = _synthetic_script(cmds)
    counts = script_counts(code, cmds, conds)
    scene = classify(counts)
    rep.check(counts["_end"] == len(code), "VM: IF/ELSE разобран целиком",
              "%d/%d" % (counts["_end"], len(code)))
    rep.check(scene["lights"] == 2, "VM: 2 света (IF + ELSE)", str(scene))
    rep.check(len(cmds) >= 190 and len(conds) >= 50, "VM: таблицы опкодов из exe",
              "%d/%d" % (len(cmds), len(conds)))
    try:
        walk_script(bytes([250]) + bytes(8), cmds, conds)
        rep.check(False, "VM: неизвестный опкод отвергнут")
    except ValueError:
        rep.check(True, "VM: неизвестный опкод отвергнут")

    print("== self-test: реальные файлы ==")
    level = GAME_DIR / "level0.sbt"
    if not level.exists():
        rep.check(False, "игра установлена (level0.sbt)", str(GAME_DIR))
    else:
        target = sorted(GAME_DIR.glob("*.sbt"))[:3]
        rep.check(len(target) >= 3, "корпус доступен", str(GAME_DIR))
        for path in target:
            parsed = parse_sbt(path.read_bytes())
            rec = next(r for r in parsed["records"] if r["type"] == 11)
            code = path.read_bytes()[rec["code_off"]:rec["code_off"] + rec["code"]]
            counts = script_counts(code, cmds, conds)
            rep.check(counts["_end"] == len(code),
                      "%s: байткод разобран целиком" % path.name,
                      "%d/%d" % (counts["_end"], len(code)))
    return rep.summary()


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--steps", action="store_true", help="таблица шагов (по умолчанию)")
    ap.add_argument("--globals", action="store_true", help="таблица runtime-массивов")
    ap.add_argument("--records", action="store_true", help="SBT: тип -> обработчик")
    ap.add_argument("--verify", action="store_true", help="сверка с exe и файлами уровня")
    ap.add_argument("--self-test", action="store_true", help="автотест")
    ap.add_argument("--level", action="append", default=None,
                    help="уровень для --verify (по умолчанию level0 и dm1)")
    ap.add_argument("--exe", default=str(EXE), help="путь к gundx.exe")
    args = ap.parse_args(argv)

    any_mode = args.globals or args.records or args.verify or args.self_test
    if args.globals:
        print_globals()
    if args.records:
        print_records()
    if not any_mode or args.steps:
        if any_mode and args.steps:
            print()
        print_steps()

    try:
        img = load_exe(Path(args.exe))
    except (OSError, ValueError) as exc:
        if args.verify or args.self_test:
            print("не читается exe %s: %s" % (args.exe, exc), file=sys.stderr)
            return 2
        return 0

    if args.verify:
        levels = args.level or ["level0", "dm1"]
        return verify(img, levels, Report(quiet=False))
    if args.self_test:
        return self_test(img)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
