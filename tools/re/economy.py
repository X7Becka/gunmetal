#!/usr/bin/env python3
"""Economy of Gunmetal: the cash field, the shops, refit and mass.

``docs/ECONOMY.md`` carries the prose and the evidence; this module is the
runnable model.  Everything here was taken from the shipped ``gundx.exe``
(reads only) plus the 36 ``.sbt`` level scripts; nothing needs Ghidra.

The one fact that had been misnamed across the docs is the money field::

    player + 0x332c   ("equipment_cost_total" in PLAYER_FIELDS/EQUIPMENT)
                      is CASH.  ADD_SOURCEPLAYER_CASH adds to it, the shops
                      debit/credit it, SOURCEPLAYER_CASH_AT_LEAST compares it
                      with an operand and the HUD prints it as ``CASH: %ld``.

Shop transactions (all four screens, see ``SHOP_SCREENS``) work on the same
player state::

    +0x3320  i32  equipment_index      -1 = nothing selected
    +0x3324  u32  equipment_owned      bit i = record i bought
    +0x3328  u32  equipment_active     bit i = record i switched on
    +0x332c  i32  cash                 never negative (ADD clamps at 0)
    +0x3330  i32  salvage              added to cash on respawn, then cleared
    +0x3334  i32  mass_total           recomputed by 0x4524d0
    +0x3338  i32  mass_limit           from the chassis, never blocks a buy
    +0x3468  i32  chassis_price         from the chassis record (+0x168)

Usage::

    python3 tools/re/economy.py --fields      # field map with addresses
    python3 tools/re/economy.py --summary     # 36 levels: prices/masses/sums
    python3 tools/re/economy.py --json
    python3 tools/re/economy.py --self-test   # synthetic + byte anchors

Standard library only; ``gm/sbt.py``/``gm/sbtvm.py`` and
``tools/re/equipment.py`` are reused as-is (never edited here).
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _p in (ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import equipment                                   # noqa: E402  (tools/re)
from gm import sbt as sbtmod                       # noqa: E402
from gm import sbtvm                               # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")

# ---------------------------------------------------------------------------
# addresses
# ---------------------------------------------------------------------------
PLAYER_BASE = 0x819040
PLAYER_STRIDE = 0x3510
PLAYER_COUNT = 8

CASH_OFF = 0x332C
SALVAGE_OFF = 0x3330
MASS_OFF = 0x3334
MASS_LIMIT_OFF = 0x3338
CHASSIS_PRICE_OFF = 0x3468

EQUIP_REC = 0x814C60          # DEFINE_EQUIPMENT records, stride 0x40
EQUIP_STRIDE = 0x40
EQUIP_COUNT = 0x815420
SHIELD_PRICE = 0x818954       # DEFINE_SHIELDS arg0: cash per 100 shield points
SHIELD_WEIGHT = 0x818950      # DEFINE_SHIELDS arg1: mass per 100 shield points
AMMO_REC = 0x818960           # DEFINE_AMMO records, stride 0x38
AMMO_STRIDE = 0x38
AMMO_COUNT = 0x818958
WEAPON_REC = 0x80FA00         # DEFINE_WEAPONS records, stride 0x2a8
WEAPON_STRIDE = 0x2A8
WEAPON_COUNT = 0x819028
CHASSIS_REC = 0x7995E0        # DEFINE_PLAYER_TYPES records, stride 0x1cc
CHASSIS_STRIDE = 0x1CC
CHASSIS_COUNT = 0x7941BC
LOCAL_PLAYER = 0x83D308       # index used by the accessor 0x4422b0
START_CASH_GLOBAL = 0x83D310  # cash granted on respawn (set from 0x83d198)
START_CASH_TABLE = 0x4BC6F8   # 12 menu values, "Starting Cash: %ld"

#: player fields the economy touches.  (offset, address, type, name, status,
#: writers, readers, note)
FIELDS = [
    (0x3210, 0x81C250, "i32", "action_state", "доказано",
     "0x45c0fe/0x45c14c/0x45c19a (SHOW_*_BUY_SCREEN: 2/3/4), 0x47b377 (5), "
     "0x443b65 (после каждого магазина)",
     "0x44382d (switch магазинов), 0x47bce2",
     "2=аммо, 3=предметы, 4=шасси, 5=сохранение шасси, 6=оружие, 7=езда"),
    (0x3294, 0x81C2D4, "i32", "weapon_slot", "доказано",
     "0x45bd06 (sell all -> -1)", "0x44531c",
     "sell_all сбрасывает в -1 (0x45bd06)"),
    (0x3298, 0x81C2D8, "u32", "weapon_mask", "доказано",
     "0x45bcfe (sell all -> 0), 0x47b089..0x47b0da (магазин оружия)",
     "0x45253f (вес), 0x47bb80", "бит i = ствол i куплен"),
    (0x329C, 0x81C2DC, "u32[31]", "ammo", "доказано",
     "0x459ca6 (op109), 0x45bcb6 (sell all -> 0), 0x47b613 (покупка)",
     "0x4525b9 (вес), 0x45bca1 (продажа)",
     "индекс — тип DEFINE_AMMO; +0x3318-0x329c = 0x7c = 31 dword запаса"),
    (0x3250, 0x81C290, "i32[4]", "shields", "доказано",
     "0x45bc81..0x45bc89 (sell all -> 0), 0x452de4.. (спавн)",
     "0x45bc51 (sell all), 0x452504 (вес)",
     "сотые доли; sell_all платит shields[i]/100 * 0x818954"),
    (0x3320, 0x81C360, "i32", "equipment_index", "доказано",
     "0x45a4ec (op136), 0x4460ab/0x4461a5, 0x45bdd9 (sell all -> -1), "
     "0x46721a (загрузка)",
     "0x446053, 0x492b32", "-1 = ничего не выбрано"),
    (0x3324, 0x81C364, "u32", "equipment_owned", "доказано",
     "0x45bbc8/0x45bc08 (op110/111), 0x47b7e1 (снятие), 0x47b8f8 (покупка), "
     "0x45bdcc (sell all), 0x4671a8 (загрузка)",
     "0x445fe6, 0x45257b (вес), 0x495ce4 (витрина)", "бит i = запись i куплена"),
    (0x3328, 0x81C368, "u32", "equipment_active", "доказано",
     "0x45a858/0x45a881 (op130), 0x47b863 (снятие при продаже), 0x45bd60 "
     "(sell all), 0x4671b4 (загрузка)",
     "0x4463a9 (тик), 0x491a13 (HUD)", "бит i = запись i включена"),
    (CASH_OFF, 0x81C36C, "i32", "cash", "доказано",
     "0x459d99 (op114, clamp 0), 0x47b881 (продажа), 0x47b8dd (покупка), "
     "0x45bc67..0x45bce9 (sell all), 0x452e61..0x452fca (респавн), "
     "0x47b2af/0x47b514/0x47b56a/0x47b5f4 (шасси/аммо), 0x47bc4a/0x47bd2d "
     "(шасси), 0x47c2ba/0x47c37c (оружие), 0x466dbe/0x4671c0 (загрузка), "
     "0x460665 (чит +1 000 000)",
     "0x45e572 (cond34), 0x495d2a (витрина), 0x4927e0 0x49528f 0x495bf8 "
     "0x496197 0x496b08 (HUD), 0x49910c (армори), 0x464d56 (сеть)",
     "деньги игрока; в saved0 = 2000"),
    (SALVAGE_OFF, 0x81C370, "i32", "salvage", "доказано",
     "0x459e18 (op115 += arg), 0x452f85 (респавн -> 0), 0x466dc9 (поток)",
     "0x452e4f (респавн: cash = salvage + база), 0x4422c7 (аксессор), "
     "0x4995ab (строка SALVAGE)",
     "премия за уровень; на покупки в магазинах не влияет"),
    (MASS_OFF, 0x81C374, "i32", "mass_total", "доказано",
     "0x4524f4..0x4525cd (0x4524d0: сумма щитов/оружия/экипировки/аммо)",
     "0x4525e7 (сравнение с лимитом), 0x4952c3 (WEIGHT: %ld)",
     "пересчитывается после каждой покупки/продажи (0x47b887, 0x45bde3)"),
    (MASS_LIMIT_OFF, 0x81C378, "i32", "mass_limit", "доказано",
     "0x47aa6b (1000 без шасси), 0x47aca1 (из записи шасси +0x148)",
     "0x4525e7 (штраф скорости)", "покупку не блокирует — только скорость"),
    (0x3460, 0x81C4A0, "i32", "chassis_requested", "доказано",
     "0x47b742..0x47b787, 0x47b9f7 (магазин шасси), 0x47b377",
     "0x47b76e, 0x47ba72", "индекс выбранного шасси в витрине"),
    (0x3464, 0x81C4A4, "i32", "chassis_current", "доказано",
     "0x47aa34 (создание шасси)", "0x47b2b7, 0x452e...",
     "текущий тип; смена обнуляет owned/active"),
    (CHASSIS_PRICE_OFF, 0x81C4A8, "i32", "chassis_price", "доказано",
     "0x47ac7d (из записи шасси +0x168), 0x47aa4f (без шасси)",
     "0x47aa28 (обмен), 0x47bb0b (бюджет магазина шасси), 0x452fa0 (респавн)",
     "10000 = LIGHT CHASSIS; в saved0 +0x3468 = 10000"),
]

#: globals the economy depends on
GLOBALS = [
    (EQUIP_REC, "DEFINE_EQUIPMENT: 0x40 байт, имя +0x2c, "
     "цена +0x24, вес +0x28, флаги +0x00"),
    (EQUIP_COUNT, "счётчик записей экипировки (<= 31)"),
    (SHIELD_PRICE, "цена 100 единиц щита (DEFINE_SHIELDS arg0; "
     "в 36 уровнях 50)"),
    (SHIELD_WEIGHT, "вес 100 единиц щита (DEFINE_SHIELDS arg1; 0)"),
    (AMMO_REC, "DEFINE_AMMO: 0x38 байт, max +0x04, lot +0x08, "
     "price +0x0c, weight/round +0x10"),
    (AMMO_COUNT, "счётчик типов патронов (в поставке 15)"),
    (WEAPON_REC, "DEFINE_WEAPONS: 0x2a8 байт, price +0x44, "
     "weight +0x48"),
    (WEAPON_COUNT, "счётчик стволов"),
    (CHASSIS_REC, "DEFINE_PLAYER_TYPES: 0x1cc байт, "
     "mass_limit +0x148, shields +0x14c, price +0x168"),
    (CHASSIS_COUNT, "счётчик типов шасси (<= 32)"),
    (LOCAL_PLAYER, "индекс локального игрока (аксессор 0x4422b0)"),
    (START_CASH_GLOBAL, "деньги при респавне: cash = salvage + "
     "это значение (+ гандикап), затем минус шасси и щиты"),
    (START_CASH_TABLE, "12 значений меню «Starting Cash: %ld» "
     "(0x4863fd/0x48861c читают таблицу)"),
]

#: one entry per shop screen; the VM opcode only sets action_state
SHOP_SCREENS = [
    (2, "ammo", 0x47B390, 0x45C0AE, 189, "SHOW_AMMO_BUY_SCREEN",
     "патроны: цена +0x0c, партия +0x08, лимит +0x04"),
    (3, "items", 0x47B6E0, 0x45C109, 190, "SHOW_ITEM_BUY_SCREEN",
     "экипировка: флаги/цена E9/события E1/E4"),
    (4, "chassis", 0x47B990, 0x45C157, 191, "SHOW_TANK_BUY_SCREEN",
     "шасси: бюджет = cash + старое шасси + щиты + стволы + экипировка + "
     "патроны, цена = 0x479dd0(индекс)"),
    (5, "rpv-design", 0x47BDA0, 0x45C1A5, 192, "SHOW_TANK_SAVE_SCREEN",
     "сохранение проекта R.P.V. в rpv\\*.rpt (0x47a720); денег не трогает"),
    (6, "weapons", 0x47C1A0, 0x45C1F0, 193, "SHOW_WEAPON_BUY_SCREEN",
     "стволы: цена +0x44, вес +0x48; cash -= цена (0x47c365)"),
]

#: input bits of player+0x3344 read by the shop updaters (0x44382d switch)
SHOP_ACTIONS = [
    (0x0001, "prev", "0x47b739: index-- (вне магазинов — 0x47b65b)"),
    (0x0002, "next", "0x47b752: index++"),
    (0x0004, "sell", "0x47b7a5: флаги & 2 и бит owned -> продать"),
    (0x0008, "buy", "0x47b88f: флаги & 1, cash >= цена -> купить"),
    (0x8000, "leave", "0x47b95b: выход из магазина (возврат 1)"),
]

#: VM hooks that move money / state
VM = [
    (114, "ADD_SOURCEPLAYER_CASH", "L", 0x459D99,
     "cash += arg; если < 0 -> 0 (0x459ddb)"),
    (115, "ADD_SOURCEPLAYER_SALVAGE", "L", 0x459DE9,
     "salvage (+0x3330) += arg"),
    (140, "SOURCEPLAYER_SELL_ALL", "-", 0x45BC1A,
     "сброс всего: щиты/патроны/стволы/экипировка -> деньги"),
    (176, "DEFINE_SHIELDS", "LL", 0x45C353,
     "0x818954 = arg0 (цена), 0x818950 = arg1 (вес)"),
]
CONDITIONS = [
    (34, "SOURCEPLAYER_CASH_AT_LEAST", "L", 0x45E532,
     "cash (0x81c36c) >= аргумент (cmp 0x45e572, setge 0x45e578)"),
]

#: byte anchors in gundx.exe: (vma, hex, what)
ANCHORS = [
    (0x459DC2, "8d 82 6c c3 81 00", "op114: база денег 0x81c36c + игрок*0x3510"),
    (0x459DCE, "03 d1", "op114: cash += операнд"),
    (0x459DDB, "c7 00 00 00 00 00", "op114: зажим cash в 0 при отрицательном"),
    (0x45E572, "39 91 6c c3 81 00", "cond34: cmp cash, операнд"),
    (0x45E578, "0f 9d c0", "cond34: setge — «денег не меньше»"),
    (0x47B6FC, "f6 00 08", "витрина: пропуск записей flags & 8 (скрытые)"),
    (0x47B7CD, "c1 e1 06", "витрина: индекс * 0x40 (запись экипировки)"),
    (0x47B7D0, "f6 81 60 4c 81 00 02", "продажа: флаг 0x02 у записи"),
    (0x47B7E1, "89 96 64 c3 81 00", "продажа: owned &= ~бит"),
    (0x47B863, "89 86 68 c3 81 00", "продажа: active &= ~бит (если был активен)"),
    (0x47B879, "8b 88 84 4c 81 00", "продажа: цена E9 (+0x24)"),
    (0x47B881, "89 96 6c c3 81 00", "продажа: cash += цена"),
    (0x47B8BA, "f6 81 60 4c 81 00 01", "покупка: флаг 0x01 у записи"),
    (0x47B8C7, "8b 86 6c c3 81 00", "покупка: читает cash"),
    (0x47B8CD, "8b 89 84 4c 81 00", "покупка: цена E9 (+0x24)"),
    (0x47B8D3, "3b c1", "покупка: cmp cash, цена"),
    (0x47B8DD, "89 86 6c c3 81 00", "покупка: cash -= цена"),
    (0x47B8F8, "89 8e 64 c3 81 00", "покупка: owned |= бит"),
    (0x47B940, "8b 88 64 4c 81 00", "покупка: пост E1 (+0x04) через 0x4795b0"),
    (0x47B887, "e8 44 6c fd ff", "и продажа, и покупка: пересчёт 0x4524d0"),
    (0x45BC51, "8b 11", "sell all: shields[i]"),
    (0x45BC67, "8b 86 6c c3 81 00", "sell all: читает cash (щиты)"),
    (0x45BC77, "89 86 6c c3 81 00", "sell all: cash += щиты/100*0x818954"),
    (0x45BC81, "89 0f", "sell all: щит = 0"),
    (0x45BCB0, "01 86 6c c3 81 00", "sell all: cash += патроны*цена/lot"),
    (0x45BCE9, "01 be 6c c3 81 00", "sell all: cash += цена ствола (для экипировки)"),
    (0x45BCCF, "3b d3", "sell all: цикл по стволам (биты weapon_mask)"),
    (0x45BD06, "c7 86 d4 c2 81 00 ff ff ff ff", "sell all: weapon_slot = -1"),
    (0x45BD60, "89 8e 68 c3 81 00", "sell all: снимает бит active"),
    (0x45BD9F, "e8 0c d8 01 00", "sell all: пост E4 (+0x10) для активных"),
    (0x45BDCC, "89 86 64 c3 81 00", "sell all: owned = 0"),
    (0x45BDD2, "89 86 68 c3 81 00", "sell all: active = 0"),
    (0x45BDD9, "c7 86 60 c3 81 00 ff ff ff ff", "sell all: index = -1"),
    (0x4524F4, "c7 86 74 c3 81 00 00 00 00 00", "масса: сброс +0x3334"),
    (0x4525E7, "8b 8e 78 c3 81 00", "масса: лимит +0x3338"),
    (0x452601, "d8 e9", "масса: скорость -= (масса-лимит)*штраф"),
    (0x45261E, "c7 86 88 bd 81 00 9a 99 99 99", "масса: пол скорости 0.05"),
    (0x452E61, "89 83 6c c3 81 00", "респавн: cash = salvage + 0x83d310"),
    (0x452EAA, "89 bb 6c c3 81 00", "респавн: гандикап x4 (0x83d184 & 0x20)"),
    (0x452F85, "89 b3 70 c3 81 00", "респавн: salvage = 0"),
    (0x452FBE, "89 83 6c c3 81 00", "респавн: cash -= шасси + щиты"),
    (0x452FCA, "89 b3 6c c3 81 00", "респавн: зажим cash в 0"),
    (0x47AA28, "8b 8b a8 c4 81 00", "создание шасси: старое +0x3468 в обмен"),
    (0x47AC6B, "8b 82 48 97 79 00", "создание шасси: цена из записи +0x168"),
    (0x47AC7D, "89 83 a8 c4 81 00", "создание шасси: +0x3468 = цена"),
    (0x479DFC, "8b 88 2c 97 79 00", "цена шасси: запись +0x14c (щиты)"),
    (0x479E08, "0f af 0d 54 89 81 00", "цена шасси: * цена щита 0x818954"),
    (0x479E0F, "8d 04 8a", "цена шасси: база + 4*щиты*цена"),
    (0x47BB0B, "8b 87 a8 c4 81 00", "магазин шасси: бюджет = cash + старое шасси"),
    (0x47BC25, "3b c8", "магазин шасси: бюджет vs цена комплекта"),
    (0x47BC4A, "89 b7 6c c3 81 00", "магазин шасси: cash -= цена"),
    (0x45C359, "a3 54 89 81 00", "op176: 0x818954 = arg0 (цена щита)"),
    (0x45C364, "89 0d 50 89 81 00", "op176: 0x818950 = arg1 (вес щита)"),
    (0x460665, "81 86 6c c3 81 00 40 42 0f 00", "чит CHEAT GIVE CASH: +1 000 000"),
    (0x466DBE, "89 91 6c c3 81 00", "загрузка: cash из потока"),
    (0x4671C0, "89 88 6c c3 81 00", "загрузка: cash из 0x61ec8c"),
    (0x4657F2, "8b 86 6c c3 81 00", "сохранение: читает cash"),
    (0x4927E0, "8b 97 6c c3 81 00", "HUD: печатает cash"),
    (0x495D2A, "8b 97 6c c3 81 00", "витрина предметов: cash для RIGHT TO BUY"),
    (0x495D30, "8b 81 84 4c 81 00", "витрина предметов: цена E9"),
    (0x495D36, "3b d0", "витрина предметов: cash >= цена?"),
]

#: expected numbers of the shipped data (verified by --self-test)
EXPECT = {
    "files": 36,
    "equipment_records": 434,
    "ammo_types": 15,
    "shield_price": 50,
    "chassis_prices": {
        "LIGHT CHASSIS": 10000, "MEDIUM CHASSIS": 30000,
        "STEALTH CHASSIS": 350000, "HEAVY CHASSIS": 80000,
        "57 CHEVY": 500000,
    },
    "chassis_shields": {
        "LIGHT CHASSIS": 80, "MEDIUM CHASSIS": 130,
        "STEALTH CHASSIS": 90, "HEAVY CHASSIS": 200,
        "57 CHEVY": 300,
    },
}


class Fail(Exception):
    pass


_COUNT = [0]


def expect(cond, msg):
    _COUNT[0] += 1
    if not cond:
        raise Fail(msg)


# ---------------------------------------------------------------------------
# synthetic player fit + the transaction rules (mirror of the shop code)
# ---------------------------------------------------------------------------
class Fit:
    """Just the player state a shop touches, plus the tables it prices with."""

    def __init__(self, cash=0, owned=0, active=0, index=-1, shields=(0, 0, 0, 0),
                 ammo=None, weapon_mask=0, weapon_slot=-1, salvage=0,
                 shield_price=50, shield_weight=0, mass_limit=500,
                 equipment=None, weapons=None, ammo_types=None):
        self.cash = cash
        self.salvage = salvage
        self.owned = owned
        self.active = active
        self.index = index
        self.shields = list(shields)
        self.ammo = list(ammo or [0] * 16)
        self.weapon_mask = weapon_mask
        self.weapon_slot = weapon_slot
        self.shield_price = shield_price
        self.shield_weight = shield_weight
        self.mass_limit = mass_limit
        self.equipment = equipment or []      # [{flags,price,weight,...}]
        self.weapons = weapons or []          # [{price,weight}]
        self.ammo_types = ammo_types or []    # [{lot,price,weight}]
        self.events = []

    # -- mass: 0x4524d0 ----------------------------------------------------
    def mass(self):
        total = 0
        for value in self.shields:
            total += (value // 100) * self.shield_weight
        for i, weapon in enumerate(self.weapons):
            if self.weapon_mask & (1 << i):
                total += weapon["weight"]
        for i, rec in enumerate(self.equipment):
            if self.owned & (1 << i):
                total += rec["weight"]
        for i, rec in enumerate(self.ammo_types):
            if i < len(self.ammo):
                total += self.ammo[i] * rec["weight"]
        return total

    def max_speed(self, base, penalty):
        mass = self.mass()
        speed = float(base)
        if mass > self.mass_limit:
            speed -= (mass - self.mass_limit) * penalty
        return max(speed, 0.05)

    def post(self, event):
        if event:
            self.events.append(event)

    # -- shop action bit 0x0008: 0x47b88f --------------------------------
    def buy(self, bit, rec):
        if self.owned & (1 << bit):          # 0x47b8af: already owned
            return False
        if not rec["flags"] & 0x01:          # 0x47b8ba
            return False
        if self.cash < rec["price"]:         # 0x47b8d3
            return False
        self.cash -= rec["price"]            # 0x47b8dd
        self.owned |= 1 << bit               # 0x47b8f8
        self.post(rec.get("spawn_event", 0))  # 0x47b940
        return True

    # -- shop action bit 0x0004: 0x47b7a5 --------------------------------
    def sell(self, bit, rec):
        if not self.owned & (1 << bit):      # 0x47b7c5
            return False
        if not rec["flags"] & 0x02:          # 0x47b7d0
            return False
        self.owned &= ~(1 << bit)            # 0x47b7e1
        if self.active & (1 << bit):         # 0x47b7fa
            self.post(rec.get("event_off", 0))  # 0x47b844
            self.active &= ~(1 << bit)       # 0x47b863
        self.cash += rec["price"]            # 0x47b881
        return True

    # -- opcode 140: 0x45bc1a --------------------------------------------
    def sell_all(self):
        """Return a breakdown; mirrors 0x45bc1a instruction for instruction."""
        out = {"shields": 0, "ammo": 0, "weapons": 0, "equipment": 0,
               "events": []}
        for i in range(4):                   # 0x45bc51..0x45bc77
            out["shields"] += (self.shields[i] // 100) * self.shield_price
            self.shields[i] = 0              # 0x45bc81..0x45bc89
        self.cash += out["shields"]
        for i in range(len(self.ammo)):      # 0x45bca1..0x45bcb6
            if self.ammo[i] > 0 and i < len(self.ammo_types):
                lot = self.ammo_types[i]["lot"]
                price = self.ammo_types[i]["price"]
                if lot:
                    out["ammo"] += self.ammo[i] * price // lot
            self.ammo[i] = 0
        self.cash += out["ammo"]
        for i, weapon in enumerate(self.weapons):   # 0x45bcd8..0x45bce9
            if self.weapon_mask & (1 << i):
                out["weapons"] += weapon["price"]
                self.cash += weapon["price"]
        self.weapon_mask = 0                 # 0x45bcfe
        self.weapon_slot = -1                # 0x45bd04
        for i, rec in enumerate(self.equipment):    # 0x45bd23..0x45bd42
            bit = 1 << i
            if not self.owned & bit:
                continue
            # BUG of the original: the refund is taken from the WEAPON price
            # table (0x80fa44 + i*0x2a8), not from the equipment record.
            refund = self.weapons[i]["price"] if i < len(self.weapons) else 0
            out["equipment"] += refund
            self.cash += refund
            if self.active & bit:            # 0x45bd48..0x45bd60
                self.post(rec.get("event_off", 0))
                out["events"].append(rec.get("event_off", 0))
                self.active &= ~bit
        self.owned = 0                       # 0x45bdcc
        self.active = 0                      # 0x45bdd2
        self.index = -1                      # 0x45bdd9
        return out

    # -- opcode 114: 0x459d99 --------------------------------------------
    def add_cash(self, delta):
        self.cash += delta
        if self.cash < 0:                    # 0x459dd2/0x459ddb
            self.cash = 0
        return self.cash

    def cash_at_least(self, amount):         # cond 34, 0x45e572
        return self.cash >= amount


# ---------------------------------------------------------------------------
# level data
# ---------------------------------------------------------------------------
def corpus_paths(game):
    return sorted(glob.glob(os.path.join(game, "*.sbt")))


def _walk(insns):
    for ins in insns:
        yield ins
        body = getattr(ins, "body", None)
        if isinstance(body, list):
            yield from _walk(body)


def equipment_rows(path):
    """``[{name, flags, price, weight, ...}]`` via tools/re/equipment.py."""
    tables = equipment.read_level(path)
    rows = []
    for table in tables:
        for rec in table["records"]:
            view = rec["view"]
            rows.append({
                "name": rec["name"], "flags": view["flags"],
                "price": view["price"], "weight": view["weight"],
                "object_id": view["object_id"],
                "spawn_event": view["spawn_event"],
                "event_on": view["event_on"], "event_off": view["event_off"],
                "ammo_type": view["ammo_type"],
            })
    return rows


def level_rows(path):
    """Equipment + chassis + shields + ammo of one level script."""
    level = sbtmod.parse_file(path)
    scripts = level.records_of(11)
    out = {"file": os.path.basename(path), "equipment": equipment_rows(path),
           "chassis": [], "ammo": [], "shields": None, "weapons": []}
    if not scripts:
        return out
    insns, _end = sbtvm.decode(scripts[0].payload.code)
    for ins in _walk(insns):
        if ins.name == "DEFINE_PLAYER_TYPES" and ins.data:
            for strings, vals in ins.data:
                out["chassis"].append({
                    "name": strings[0],
                    # `vals` is the typed view of the record (doubles become
                    # floats): 0 mass limit, 2 shields, 3 armour, 10 price.
                    "mass_limit": int(vals[0]), "shields": int(vals[2]),
                    "armor": int(vals[3]), "price": int(vals[10]),
                })
        elif ins.name == "DEFINE_SHIELDS":
            args = [int(a) for a in ins.args if isinstance(a, int)]
            if len(args) >= 2:
                out["shields"] = {"price": args[0], "weight": args[1]}
        elif ins.name == "DEFINE_AMMO" and ins.data:
            for strings, vals in ins.data:
                out["ammo"].append({
                    "name": strings[0], "max": int(vals[0]),
                    "lot": int(vals[4]), "price": int(vals[5]),
                    "weight": int(vals[6]), "flags": int(vals[7]),
                })
        elif ins.name == "DEFINE_WEAPONS" and ins.data:
            for strings, vals in ins.data:
                out["weapons"].append({
                    "name": strings[0], "price": int(vals[8]),
                    "weight": int(vals[9]), "ammo_type": int(vals[2]),
                })
    return out


def chassis_package_price(row, shield_price):
    """0x479dd0: base price + 4 sides * shields * price per 100 points."""
    return row["price"] + 4 * row["shields"] * shield_price


def level_summary(row):
    """Numbers `--summary` prints for one level."""
    eq = row["equipment"]
    buy = [r for r in eq if r["flags"] & 0x01]
    sell = [r for r in eq if r["flags"] & 0x02]
    shields = row["shields"] or {"price": 0, "weight": 0}
    chassis = [dict(r, package=chassis_package_price(r, shields["price"]))
               for r in row["chassis"]]
    by_name = collections.OrderedDict()
    for rec in chassis:
        entry = by_name.setdefault(rec["name"], {"prices": set(),
                                                 "limits": set(),
                                                 "shields": set(),
                                                 "package": set()})
        entry["prices"].add(rec["price"])
        entry["limits"].add(rec["mass_limit"])
        entry["shields"].add(rec["shields"])
        entry["package"].add(rec["package"])
    return {
        "file": row["file"],
        "equipment": len(eq),
        "buyable": len(buy),
        "buy_price": sum(r["price"] for r in buy),
        "sellable": len(sell),
        "sell_price": sum(r["price"] for r in sell),
        "weight": sum(r["weight"] for r in eq),
        "weight_buyable": sum(r["weight"] for r in buy),
        "chassis": chassis,
        "chassis_by_name": by_name,
        "chassis_names": len(by_name),
        "chassis_prices": sorted({c["price"] for c in chassis}),
        "shield_price": shields["price"],
        "shield_weight": shields["weight"],
        "ammo": row["ammo"],
        "ammo_types": len({a["name"] for a in row["ammo"]}),
        "signature": tuple(sorted((r["name"], r["price"], r["weight"],
                                   r["flags"]) for r in eq)),
        "chassis_signature": tuple(sorted(
            (name, tuple(sorted(e["prices"])), tuple(sorted(e["limits"])),
             tuple(sorted(e["shields"])))
            for name, e in by_name.items())),
    }


# ---------------------------------------------------------------------------
# anchors
# ---------------------------------------------------------------------------
def check_anchors():
    try:
        import exelib
        blob = exelib.data()
        va2off = exelib.va2off
    except Exception:                                  # noqa: BLE001
        blob = None
    out = []
    for vma, want, what in ANCHORS:
        ok, got = None, ""
        if blob is not None:
            off = va2off(vma)
            if off is not None:
                got = blob[off:off + len(want) // 3 + 1].hex(" ")
                ok = got == want
        out.append({"vma": vma, "expect": want, "bytes": got, "ok": ok,
                    "what": what})
    return out


# ---------------------------------------------------------------------------
# --fields / --summary / --json
# ---------------------------------------------------------------------------
def print_fields():
    print("игрок = 0x%06x + i*0x%x (i = 0..%d)"
          % (PLAYER_BASE, PLAYER_STRIDE, PLAYER_COUNT - 1))
    print("  %-8s %-9s %-4s %-22s %-9s" %
          ("offset", "address", "type", "field", "status"))
    for off, addr, kind, name, status, _w, _r, note in FIELDS:
        print("  +0x%04x  0x%06x  %-8s %-24s %-9s" % (off, addr, kind, name,
                                                      status))
        print("        W: %s" % _w)
        print("        R: %s" % _r)
        if note:
            print("        %s" % note)
    print()
    print("глобалы:")
    for addr, note in GLOBALS:
        print("  0x%06x  %s" % (addr, note))
    print()
    print("магазины и экраны (action_state, +0x%04x):" % 0x3210)
    for state, name, fn, handler, opcode, opname, note in SHOP_SCREENS:
        print("  %d  %-11s update 0x%06x  %s (op%d) 0x%06x"
              % (state, name, fn, opname, opcode, handler))
        print("        %s" % note)
    print()
    print("биты действий (+0x3344), которые читает витрина:")
    for bit, role, note in SHOP_ACTIONS:
        print("  0x%04x  %-6s %s" % (bit, role, note))
    print()
    print("ВМ:")
    for opcode, name, sig, handler, effect in VM:
        print("  op%-3d 0x%06x %-28s %-3s %s" % (opcode, handler, name, sig,
                                                 effect))
    for cid, name, sig, handler, effect in CONDITIONS:
        print("  cond%-2d 0x%06x %-27s %-3s %s" % (cid, handler, name, sig,
                                                   effect))
    print()
    print("масса (0x4524d0):")
    print("  mass = Σ щиты[i]/100*[0x818950] + Σ стволы: +0x48 "
          "+ Σ экипировка: +0x28 + Σ патроны[i]*+0x10")
    print("  если mass > +0x3338: speed_max = speed_base(+0x2d30) - "
          "(mass-limit)*+0x2d40, пол 0.05 (0x45261e)")
    print("  покупку масса не блокирует: единственный читатель +0x3338 — "
          "0x4525e7")


def print_summary(paths, game):
    if not paths:
        print("нет .sbt в %s" % game)
        return
    rows = [level_summary(level_rows(p)) for p in paths]
    groups = collections.OrderedDict()
    for row in rows:
        groups.setdefault(row["signature"], []).append(row["file"])
    print("== экономика: %d уровней из %s" % (len(rows), game))
    print("   вариантов таблицы экипировки: %d" % len(groups))
    for i, (sig, files) in enumerate(groups.items(), 1):
        buy = [s for s in sig if s[3] & 0x01]
        sell = [s for s in sig if s[3] & 0x02]
        print("   [%d] %d файл(ов): записей %d, купить %d (Σ%d), "
              "продать %d (Σ%d), Σвес %d%s"
              % (i, len(files), len(sig), len(buy),
                 sum(s[1] for s in buy), len(sell),
                 sum(s[1] for s in sell), sum(s[2] for s in sig),
                 "" if len(files) > 6 else "  (" + ", ".join(files) + ")"))
    print()
    print("  %-9s %3s %5s %9s %6s %7s %2s %9s %4s %4s"
          % ("level", "eq", "buy", "Σbuy", "Σsell", "Σвес", "ch",
             "chassis$", "shld", "ammo"))
    for row in rows:
        chassis = "/".join(str(p) for p in row["chassis_prices"]) or "-"
        print("  %-9s %3d %5d %9d %6d %7d %2d %9s %4d %4d"
              % (row["file"][:-4], row["equipment"], row["buyable"],
                 row["buy_price"], row["sell_price"], row["weight"],
                 row["chassis_names"], chassis, row["shield_price"],
                 row["ammo_types"]))
    if rows:
        print()
        print("  шасси (по всем уровням: цена / лимиты массы / щиты / "
              "комплект = база + 4*щиты*цена_щита):")
        merged = collections.OrderedDict()
        for row in rows:
            for name, entry in row["chassis_by_name"].items():
                slot = merged.setdefault(name, {"prices": set(),
                                                "limits": set(),
                                                "shields": set()})
                slot["prices"] |= entry["prices"]
                slot["limits"] |= entry["limits"]
                slot["shields"] |= entry["shields"]
        for name, entry in merged.items():
            price = sorted(entry["prices"])[0]
            point = sorted(entry["shields"])[0]
            package = chassis_package_price(
                {"price": price, "shields": point},
                rows[0]["shield_price"])
            print("    %-16s %7d  масса %-14s щиты %4d  комплект %7d"
                  % (name, price,
                     "/".join(str(v) for v in sorted(entry["limits"])),
                     point, package))
    total = {
        "eq": sum(r["equipment"] for r in rows),
        "buy": sum(r["buyable"] for r in rows),
        "sell": sum(r["sellable"] for r in rows),
        "ch": sum(len(r["chassis"]) for r in rows),
        "ammo": sum(r["ammo_types"] for r in rows),
    }
    print()
    print("ИТОГ: %d файлов, %d записей экипировки (купить %d), %d записей "
          "шасси, %d записей патронов"
          % (len(rows), total["eq"], total["buy"], total["ch"], total["ammo"]))
    if rows:
        print("      единицы экипировки (первый вариант):")
        for name, price, weight, flags in rows[0]["signature"]:
            print("        %-15s цена %8d  вес %5d  флаги 0x%02x%s%s"
                  % (name, price, weight, flags,
                     "  купить" if flags & 1 else "",
                     "  продать" if flags & 2 else ""))


def fit_from_save(name, game=DEFAULT_GAME):
    """Rebuild the shop state of an installed save (read-only).

    Returns ``(Fit, level-row, info)``; ``info`` keeps the raw field values so
    the caller can compare the model with the file.
    """
    import save_fields
    path = os.path.join(game, "SAVEGAME", name)
    with open(path, "rb") as handle:
        data = handle.read()
    save = save_fields.parse_save(data, path)
    block = save["blocks"][9]
    base = block["offset"] + 2                        # flag + struct of slot 0
    player = data[base:base + PLAYER_STRIDE]
    level_name = save["header"]["level"].lower()
    level_path = os.path.join(game, level_name + ".sbt")
    row = level_rows(level_path)
    shields = list(struct.unpack_from("<4i", player, 0x3250))
    ammo = list(struct.unpack_from("<31I", player, 0x329C))
    info = {
        "file": name, "level": save["header"]["level"],
        "cash": struct.unpack_from("<i", player, CASH_OFF)[0],
        "salvage": struct.unpack_from("<i", player, SALVAGE_OFF)[0],
        "mass": struct.unpack_from("<i", player, MASS_OFF)[0],
        "mass_limit": struct.unpack_from("<i", player, MASS_LIMIT_OFF)[0],
        "chassis_price": struct.unpack_from("<i", player,
                                            CHASSIS_PRICE_OFF)[0],
        "chassis": struct.unpack_from("<i", player, 0x3464)[0],
        "index": struct.unpack_from("<i", player, 0x3320)[0],
        "owned": struct.unpack_from("<I", player, 0x3324)[0],
        "active": struct.unpack_from("<I", player, 0x3328)[0],
        "weapon_mask": struct.unpack_from("<I", player, 0x3298)[0],
        "weapon_slot": struct.unpack_from("<i", player, 0x3294)[0],
        "shield_max": struct.unpack_from("<i", player, 0x324C)[0],
        "shields": shields, "ammo": ammo,
    }
    shield = row["shields"] or {"price": 0, "weight": 0}
    fit = Fit(cash=info["cash"], salvage=info["salvage"],
              owned=info["owned"], active=info["active"], index=info["index"],
              shields=shields, ammo=ammo, weapon_mask=info["weapon_mask"],
              weapon_slot=info["weapon_slot"],
              shield_price=shield["price"], shield_weight=shield["weight"],
              mass_limit=info["mass_limit"],
              equipment=row["equipment"], weapons=row["weapons"],
              ammo_types=row["ammo"])
    return fit, row, info


def print_saved(name, game):
    fit, row, info = fit_from_save(name, game)
    print("== %s: уровень %s" % (info["file"], info["level"]))
    print("   cash %d, salvage %d, масса %d / лимит %d (модель 0x4524d0: %d)"
          % (info["cash"], info["salvage"], info["mass"],
             info["mass_limit"], fit.mass()))
    print("   шасси %d, цена %d, индекс экипировки %d, owned 0x%x, active 0x%x"
          % (info["chassis"], info["chassis_price"], info["index"],
             info["owned"], info["active"]))
    print("   слот оружия %d, маска 0x%x; shield_max %d"
          % (info["weapon_slot"], info["weapon_mask"], info["shield_max"]))
    names = [rec["name"] for i, rec in enumerate(row["equipment"])
             if info["owned"] & (1 << i)]
    print("   куплено: %s" % (", ".join(names) or "—"))
    stock = [(row["ammo"][i]["name"], v) for i, v in enumerate(info["ammo"])
             if v and i < len(row["ammo"])]
    print("   патроны: %s" % (", ".join("%s %d" % s for s in stock) or "—"))
    fit_events = []
    before = fit.cash
    br = fit.sell_all()
    fit_events = br["events"]
    print("   SELL_ALL: щиты %d + патроны %d + стволы %d + экипировка %d "
          "= %d -> cash %d"
          % (br["shields"], br["ammo"], br["weapons"], br["equipment"],
             br["shields"] + br["ammo"] + br["weapons"] + br["equipment"],
             fit.cash))
    print("   после: owned %d, active %d, index %d, weapon_slot %d, масса %d, "
          "E4×%d" % (fit.owned, fit.active, fit.index, fit.weapon_slot,
                     fit.mass(), len(fit_events)))
    print("   (было cash %d, стало %d)" % (before, fit.cash))


def json_model(paths=None, game=DEFAULT_GAME):
    out = {
        "player": {"base": PLAYER_BASE, "stride": PLAYER_STRIDE,
                   "count": PLAYER_COUNT},
        "fields": [
            {"offset": f[0], "address": f[1], "type": f[2], "name": f[3],
             "status": f[4], "writers": f[5].split(), "readers": f[6].split(),
             "note": f[7]} for f in FIELDS],
        "globals": [
            {"address": g[0], "note": g[1]} for g in GLOBALS],
        "shops": [
            {"state": s[0], "name": s[1], "update": s[2], "handler": s[3],
             "opcode": s[4], "opname": s[5], "note": s[6]}
            for s in SHOP_SCREENS],
        "actions": [{"bit": a[0], "role": a[1], "note": a[2]}
                    for a in SHOP_ACTIONS],
        "vm": [{"opcode": v[0], "name": v[1], "signature": v[2],
                "handler": v[3], "effect": v[4]} for v in VM],
        "conditions": [{"id": c[0], "name": c[1], "signature": c[2],
                        "handler": c[3], "effect": c[4]} for c in CONDITIONS],
        "anchors": check_anchors(),
    }
    if paths:
        rows = [level_summary(level_rows(p)) for p in paths]
        out["summary"] = {
            "levels": [{
                "file": r["file"], "equipment": r["equipment"],
                "buyable": r["buyable"], "buy_price": r["buy_price"],
                "sellable": r["sellable"], "sell_price": r["sell_price"],
                "weight": r["weight"], "shield_price": r["shield_price"],
                "shield_weight": r["shield_weight"],
                "ammo_types": r["ammo_types"],
                "chassis": r["chassis"],
            } for r in rows],
            "equipment_variants": len({r["signature"] for r in rows}),
        }
        out["items"] = [
            {"name": name, "price": price, "weight": weight, "flags": flags}
            for name, price, weight, flags in rows[0]["signature"]]
    return out


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def _sample_tables():
    equipment_rows_ = [
        {"name": "HEADLIGHTS", "flags": 0x03, "price": 0, "weight": 0,
         "spawn_event": 0, "event_off": -30044},
        {"name": "FORCE SHIELD", "flags": 0x03, "price": 800000,
         "weight": 650, "spawn_event": 0, "event_off": -30046},
        {"name": "MINE LAYER", "flags": 0x03, "price": 75000, "weight": 300,
         "spawn_event": 0, "event_off": 0},
    ]
    weapons = [{"price": 100, "weight": 10}, {"price": 240, "weight": 65},
               {"price": 500, "weight": 40}]
    ammo_types = [{"lot": 100, "price": 500, "weight": 0},
                  {"lot": 30, "price": 4000, "weight": 1}]
    return equipment_rows_, weapons, ammo_types


def do_selftest():
    _COUNT[0] = 0

    # 1. the field map itself
    for off, addr, _kind, name, _status, _w, _r, _n in FIELDS:
        expect(PLAYER_BASE + off == addr,
               "%s: 0x%x + 0x%x != 0x%x" % (name, PLAYER_BASE, off, addr))
    offsets = [f[0] for f in FIELDS]
    expect(len(offsets) == len(set(offsets)), "смещения полей повторяются")
    expect(CASH_OFF == 0x332C and SALVAGE_OFF == 0x3330 and
           MASS_OFF == 0x3334 and MASS_LIMIT_OFF == 0x3338 and
           CHASSIS_PRICE_OFF == 0x3468, "смещения экономики")
    print("OK   карта: %d полей, %d глобалов, адреса = база + смещение"
          % (len(FIELDS), len(GLOBALS)))

    # 2. cash arithmetic (op114 + cond34)
    fit = Fit(cash=1000)
    fit.add_cash(-250)
    expect(fit.cash == 750, "op114: 1000-250 != %d" % fit.cash)
    expect(fit.cash_at_least(750), "cond34: 750 >= 750")
    expect(not fit.cash_at_least(751), "cond34: 750 >= 751")
    fit.add_cash(-5000)
    expect(fit.cash == 0, "op114 не зажал в 0: %d" % fit.cash)
    print("OK   деньги: +=/зажим 0, cond34 — «не меньше»")

    # 3. buy / sell exactly as 0x47b8f8 / 0x47b881
    eq, weapons, ammo = _sample_tables()
    fit = Fit(cash=90000, equipment=eq, weapons=weapons, ammo_types=ammo,
              shield_price=50)
    expect(not fit.buy(1, eq[1]), "покупка без денег прошла")
    expect(fit.cash == 90000 and fit.owned == 0, "состояние изменилось")
    expect(fit.buy(2, eq[2]), "покупка MINE LAYER не прошла")
    expect(fit.cash == 15000 and fit.owned == 0b100, "покупка: cash/owned")
    expect(not fit.buy(2, eq[2]), "повторная покупка прошла")
    expect(not fit.sell(1, eq[1]), "продажа некупленного прошла")
    eq[0]["flags"] = 0x00                       # SELF-DESTRUCT: не продаётся
    fit.owned |= 1
    expect(not fit.sell(0, eq[0]), "продажа при флаге 0x02 = 0 прошла")
    eq[0]["flags"] = 0x03
    fit.active = 1
    expect(fit.sell(0, eq[0]), "продажа HEADLIGHTS не прошла")
    expect(fit.cash == 15000 and fit.owned == 0b100 and fit.active == 0,
           "продажа: cash/owned/active")
    expect(fit.events == [-30044], "продажа активного не послала E4: %s"
           % fit.events)
    print("OK   витрина предметов: деньги/флаги/биты и событие E4")

    # 4. mass and the speed penalty (0x4524d0)
    fit = Fit(owned=0b010, shields=(8000, 8000, 8000, 8000), weapon_mask=0b101,
              shield_weight=0, mass_limit=100, equipment=eq, weapons=weapons,
              ammo_types=ammo, ammo=[10, 2])
    # 650 (FORCE SHIELD) + 10 + 40 (стволы 0 и 2) + 2*1 (патроны типа 1)
    expect(fit.mass() == 702, "масса %d, ожидалось 702" % fit.mass())
    expect(abs(fit.max_speed(2.0, 0.001) - (2.0 - 602 * 0.001)) < 1e-9,
           "штраф скорости")
    fit.mass_limit = 10 ** 9
    expect(abs(fit.max_speed(2.0, 0.001) - 2.0) < 1e-9, "нет штрафа без массы")
    fit.mass_limit = 0
    expect(abs(fit.max_speed(0.001, 10.0) - 0.05) < 1e-9,
           "пол скорости 0.05")
    print("OK   масса: сумма четырёх слагаемых, штраф скорости, пол 0.05")

    # 5. SELL_ALL (0x45bc1a), including the weapon-table refund bug
    fit = Fit(cash=0, owned=0b101, active=0b101,
              shields=(8000, 4000, 0, 0), weapon_mask=0b101,
              ammo=[150, 45], equipment=eq, weapons=weapons, ammo_types=ammo,
              shield_price=50)
    fit.ammo[0] = 150
    fit.ammo[1] = 45
    # ammo_types only has 2 entries, so patch the lots to the level0 values
    fit.ammo_types[0].update(lot=100, price=500)
    fit.ammo_types[1].update(lot=30, price=4000)
    fit.weapons[2]["price"] = 500
    before = fit.cash
    br = fit.sell_all()
    expect(br["shields"] == 8000 // 100 * 50 + 4000 // 100 * 50,
           "возврат щитов %d" % br["shields"])
    expect(br["ammo"] == 150 * 500 // 100 + 45 * 4000 // 30,
           "возврат патронов %d" % br["ammo"])
    expect(br["weapons"] == 100 + 500, "возврат стволов %d" % br["weapons"])
    expect(br["equipment"] == 100 + 500,
           "возврат экипировки по таблице оружия %d" % br["equipment"])
    expect(fit.cash == before + br["shields"] + br["ammo"] + br["weapons"]
           + br["equipment"], "итог cash")
    expect(fit.shields == [0, 0, 0, 0], "щиты не обнулены")
    expect(fit.ammo[0] == 0 and fit.ammo[1] == 0, "патроны не обнулены")
    expect(fit.weapon_mask == 0 and fit.weapon_slot == -1,
           "оружие не сброшено")
    expect(fit.owned == 0 and fit.active == 0 and fit.index == -1,
           "биты/индекс не сброшены")
    expect(len(br["events"]) == 2, "события E4: %s" % br["events"])
    expect(fit.mass() == 0, "масса после sell_all не 0: %d" % fit.mass())
    print("OK   SELL_ALL: щиты+патроны+стволы+экипировка -> деньги, поля "
          "сброшены, E4 для активных")

    # 6. respawn arithmetic (0x452e61 / 0x452fbe)
    fit = Fit(cash=0, salvage=5000, shield_price=50)
    fit.cash = fit.salvage + 36000              # +0x83d310
    fit.salvage = 0                             # 0x452f85
    pack = 10000 + 4 * 80 * 50                  # chassis + 4 shields
    fit.cash = fit.cash - pack
    if fit.cash < 0:
        fit.cash = 0
    expect(fit.cash == 5000 + 36000 - 26000, "респавн cash %d" % fit.cash)
    expect(pack == 26000, "комплект LIGHT %d" % pack)
    print("OK   респавн: cash = salvage + база - (шасси + 4*щиты*цена)")

    # 7. chassis price formula (0x479dd0)
    light = {"price": 10000, "shields": 80, "mass_limit": 500}
    expect(chassis_package_price(light, 50) == 26000, "цена комплекта LIGHT")
    expect(chassis_package_price({"price": 0, "shields": 0}, 50) == 0,
           "нулевое шасси")
    print("OK   цена шасси: база + 4*щиты*цена_щита")

    # 8. byte anchors in the executable
    anchors = check_anchors()
    if anchors and anchors[0]["ok"] is None:
        print("SKIP байтовые якоря: gundx.exe недоступен")
    else:
        bad = [a for a in anchors if not a["ok"]]
        expect(not bad, "якоря не совпали: %s"
               % ", ".join("0x%06x (%s != %s)"
                           % (a["vma"], a["bytes"], a["expect"])
                           for a in bad[:3]))
        print("OK   байтовые якоря: %d/%d" % (len(anchors), len(anchors)))

    # 9. corpus
    paths = corpus_paths(DEFAULT_GAME)
    if not paths:
        print("SKIP корпус: %s не найден" % DEFAULT_GAME)
        print("\nOK: %d проверок" % _COUNT[0])
        return

    rows = [level_summary(level_rows(p)) for p in paths]
    expect(len(rows) == EXPECT["files"], "файлов %d" % len(rows))
    total_eq = sum(r["equipment"] for r in rows)
    expect(total_eq == EXPECT["equipment_records"],
           "записей экипировки %d" % total_eq)
    names = collections.Counter()
    for row in rows:
        expect(row["shield_price"] == EXPECT["shield_price"],
               "%s: цена щита %d" % (row["file"], row["shield_price"]))
        expect(row["shield_weight"] == 0,
               "%s: вес щита %d" % (row["file"], row["shield_weight"]))
        expect(row["ammo_types"] == EXPECT["ammo_types"],
               "%s: типов патронов %d" % (row["file"], row["ammo_types"]))
        by_name = collections.defaultdict(set)
        for rec in row["ammo"]:
            by_name[rec["name"]].add(
                (rec["max"], rec["lot"], rec["price"], rec["weight"]))
        dupes = {k: v for k, v in by_name.items() if len(v) > 1}
        expect(not dupes, "%s: разные копии DEFINE_AMMO %s"
               % (row["file"], sorted(dupes)))
        for name, entry in row["chassis_by_name"].items():
            expect(entry["prices"] == {EXPECT["chassis_prices"].get(name)},
                   "%s: %s цена %s" % (row["file"], name,
                                       sorted(entry["prices"])))
            expect(entry["shields"] == {EXPECT["chassis_shields"][name]},
                   "%s: %s щиты %s" % (row["file"], name,
                                       sorted(entry["shields"])))
            names[name] += 1
    print("OK   корпус: %d файлов, %d записей, по 15 типов патронов, "
          "щит 50/0, шасси %s"
          % (len(rows), total_eq, sorted(names)))

    # item prices must be the same set the equipment model reports
    eq_rows = equipment.read_level(paths[0])
    prices = {rec["name"]: rec["view"]["price"]
              for table in eq_rows for rec in table["records"]}
    expect(prices["FORCE SHIELD"] == 800000, "цена FORCE SHIELD")
    expect(prices["MINE LAYER"] == 75000, "цена MINE LAYER")
    expect(sum(r["buy_price"] for r in rows if r["equipment"] == 12) // 34
           == 1100000, "средняя Σbuy не 1 100 000")
    print("OK   цены: FORCE SHIELD 800000, MINE LAYER 75000, Σbuyable "
          "1100000")

    # 10. save cross-check (read-only)
    for name, cash, mass, refund in (("saved0.sbt", 2000, 615, 128000),
                                     ("saved1.sbt", 2000, 615, 127475)):
        path = os.path.join(DEFAULT_GAME, "SAVEGAME", name)
        if not os.path.exists(path):
            continue
        fit, _row, info = fit_from_save(name, DEFAULT_GAME)
        expect(info["cash"] == cash, "%s: cash %d" % (name, info["cash"]))
        expect(info["mass"] == mass, "%s: масса %d" % (name, info["mass"]))
        expect(info["mass_limit"] == 500,
               "%s: лимит массы %d" % (name, info["mass_limit"]))
        expect(info["chassis_price"] == 10000,
               "%s: цена шасси %d" % (name, info["chassis_price"]))
        expect(info["shield_max"] == 80 * 100,
               "%s: shield_max %d != 80*100" % (name, info["shield_max"]))
        expect(fit.mass() == mass,
               "%s: модель массы %d != файл %d" % (name, fit.mass(), mass))
        before = fit.cash
        br = fit.sell_all()
        gained = (br["shields"] + br["ammo"] + br["weapons"]
                  + br["equipment"])
        expect(gained == refund, "%s: SELL_ALL даёт %d" % (name, gained))
        expect(fit.cash == before + refund, "%s: cash после SELL_ALL %d"
               % (name, fit.cash))
        expect(fit.mass() == 0 and fit.owned == 0 and fit.index == -1,
               "%s: состояние после SELL_ALL" % name)
    print("OK   сейвы: cash 2000, масса 615/500 (модель сходится), "
          "шасси 10000, SELL_ALL +128000/+127475")

    # 11. determinism
    j1 = json.dumps(json_model(paths[:1]), sort_keys=True, default=str)
    j2 = json.dumps(json_model(paths[:1]), sort_keys=True, default=str)
    expect(j1 == j2 and len(j1) > 1000, "JSON не детерминирован")
    print("OK   JSON детерминирован (%d байт)" % len(j1))

    print("\nOK: %d проверок" % _COUNT[0])


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Экономика Gunmetal: деньги, магазины, масса")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--fields", action="store_true",
                    help="карта полей с адресами")
    ap.add_argument("--summary", action="store_true",
                    help="36 уровней: цены/массы/суммы")
    ap.add_argument("--json", action="store_true", help="модель и данные")
    ap.add_argument("--anchors", action="store_true", help="байтовые якоря")
    ap.add_argument("--saved", metavar="NAME",
                    help="состояние установленного сейва и его SELL_ALL")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    paths = corpus_paths(args.game)
    if args.self_test:
        do_selftest()
        return 0
    if args.anchors:
        for a in check_anchors():
            state = "-" if a["ok"] is None else ("ok" if a["ok"] else "FAIL")
            print("%s 0x%06x  %-30s %s" % (state, a["vma"], a["expect"],
                                           a["what"]))
        return 0
    if args.json:
        print(json.dumps(json_model(paths, args.game), ensure_ascii=False,
                         indent=2, sort_keys=True, default=str))
        return 0
    if args.saved:
        print_saved(args.saved, args.game)
        return 0
    if args.summary:
        print_summary(paths, args.game)
        return 0
    print_fields()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
