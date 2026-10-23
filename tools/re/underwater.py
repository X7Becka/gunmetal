#!/usr/bin/env python3
"""Underwater state, screen effect and sound of Gunmetal (``M7.7``).

Stdlib only.  The installation is only read; nothing is written.  Every
constant carries a virtual address (image base ``0x400000``); the reasoning,
the disproved hypotheses and the corpus numbers are in ``docs/UNDERWATER.md``.

Three independent things are modelled and must not be confused:

1. **Player water flag** -- bit ``0x8`` of ``player+0x3200`` (slot 0:
   ``0x81c240``).  It is *derived*, not saved: the physics tick sets it when
   the player's current sector (``player+0x327c``) has sector type 1 and
   clears it otherwise.  ``0x42d99f`` is the type getter (``kind/6``, and the
   record stores ``type*6`` in ``u16`` at ``+0x14``).  VM condition 47
   (``SOURCEPLAYER_UNDERWATER``) returns ``flags & 8``; condition 48
   (``SOURCE_UNDERWATER``) returns ``type(source sector) == 1``.

2. **Screen effect** -- a hard swap of the current palette pointer
   ``[0x83d2c4]`` from ``GAMEPAL`` (``0x83d320``) to ``WATERPAL``
   (``0x83d640``) and back, applied at the *end of the frame* through the
   brightness path (``0x449ec0`` -> ``0x4847b0`` -> ``0x40c7d0`` ->
   ``0x407220``).  No fade, no fog, no depth test.

3. **Sound** -- there is no low-pass/filter/muffling while underwater.  The
   only player-state reads in the audio range are the player index /
   ``action_state`` / sector index; the only volume shaping is the per-sector
   ``+0x50`` flags ``0x2``/``0x4`` (each adds ``1`` to a gain shift ``n``:
   ``atten *= 2^n``, ``gain >>= 2n``).  Shipped water sectors never have those
   bits.  Splashes are ordinary scripted samples, triggered by the chassis
   event ids that the *engine* posts on the water transition.

Usage::

    python3 tools/re/underwater.py --self-test   # anchors + logic + corpus
    python3 tools/re/underwater.py --fields      # field/reader/writer map
    python3 tools/re/underwater.py --scripts     # per-level water census
    python3 tools/re/underwater.py --anchors     # byte anchors with VAs
    python3 tools/re/underwater.py --json        # everything, one object

Paths honour ``GUNMETAL_EXE`` (via ``exelib``) and ``GUNMETAL_GAME``.
"""

import argparse
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)

import exelib  # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx")

# --------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------

PLAYER_BASE = 0x819040          # player[0], stride 0x3510, 8 slots
PLAYER_STRIDE = 0x3510
PLAYER_FLAGS = 0x3200           # u32; slot 0 = 0x81c240
PLAYER_SECTOR = 0x323C          # i32; slot 0 = 0x81c27c (docs say +0x327c: wrong)
PLAYER_STATE = 0x3210           # i32 action_state; slot 0 = 0x81c250
PLAYER_CHASSIS_ENTER = 0x340C   # u32 event id posted on water entry (P21)
PLAYER_CHASSIS_EXIT = 0x3410    # u32 event id posted on water exit  (P22)
FLAG_WATER = 0x8
FLAG_DAMAGE = 0x2               # sector flags +0x50, bit 1
FLAG_AMBIENT_OFF = 0x8          # sector flags +0x50, bit 3

WORLD_SECTORS = 0x5670D8        # sector record array, stride 0x18
WORLD_SECTOR_COUNT = 0x5670DC   # i32
SECTOR_KIND = 0x14              # u16 = type * 6
SECTOR_TYPE_WATER = 1

SECTOR_STUFF = 0x800B60         # 256 * 0x58
STUFF_STRIDE = 0x58
STUFF_FLAGS = 0x50
STUFF_SOUND = 0x48
STUFF_SOUND_VOL = 0x4C
STUFF_DAMAGE_A = 0x40
STUFF_DAMAGE_B = 0x44

GAMEPAL = 0x83D320              # 768 B, 6-bit RGB
WATERPAL = 0x83D640             # 768 B, 6-bit RGB
CUR_PAL = 0x83D2C4              # pointer to GAMEPAL or WATERPAL
HAZE_LEVEL = 0x80F9A8           # f64, SET_CAMERA_HAZE level

C_075 = 0x4A2330                # 0.75
C_05 = 0x4A2310                 # 0.5
C_ONE = 0x4A2628                # 1.0
C_ZERO = 0x4A22E0               # 0.0

S_UNDERWATER_ERR = 0x4ABB5C     # "Unable to load underwater palette ..."
S_WATERPAL = 0x4ABBAC           # "waterpal"
S_FRONTEND = 0x4ABBEC           # "frontend"
S_GAMEPAL = 0x4ABBF8            # "gamepal"
S_GAMEPAL_ERR = 0x4ABBB8        # "Unable to load game palette ..."

#: ``(va, hex, role)`` byte anchors: the instruction/constant at ``va`` must
#: be exactly ``hex``.  These are the claims the document makes.
ANCHORS = [
    # -- VM condition 47 (SOURCEPLAYER_UNDERWATER) -------------------------
    (0x45E8DC, "8b8424940000008b0025ffff00000f8c86040000",
     "cond47: arg9 slot ptr, low u16 -> player index, jl/jge 0..7"),
    (0x45E8F9, "8d0c408d0cc9d1e12bc8c1e10403c8c1e104",
     "cond47: index -> *0x3510 (player stride)"),
    (0x45E90B, "8b8140c2810083e008",
     "cond47: read player+0x3200 and test bit 0x8"),
    # -- VM condition 48 (SOURCE_UNDERWATER) -------------------------------
    (0x45E91C, "8b44247c85c00f8c4e04000050e871f0fc",
     "cond48: read source sector [esp+0x7c], call 0x42d99f"),
    (0x45E931, "48f7d81bc040",
     "cond48: ==1 test (dec/neg/sbb/inc -> 1 iff getter==1)"),
    # -- sector type getter/setter -----------------------------------------
    (0x42D99F, "558bec837d08007c0b8b45083b05dc7056",
     "0x42d99f: sector type getter, word(+0x14)/6"),
    (0x42D9F1, "6bc9068b55086bd218a1d870560066",
     "0x42d9d5: sector type setter, type*6 -> u16 +0x14"),
    # -- flag writers ------------------------------------------------------
    (0x444F4A, "f68340c2810008",
     "physics: test player flags bit 0x8"),
    (0x444F53, "8b937cc2810052e8408afeff",
     "physics: water branch reads player+0x323c, calls type getter"),
    (0x444F6B, "8b8b40c2810083e1f789",
     "physics: clear water bit (and flags,~0x8)"),
    (0x444F74, "898b40c28100",
     "physics: store cleared flags"),
    (0x444F7A, "8b8b50c48100",
     "physics: exit-water posts chassis id +0x3410"),
    (0x444FB8, "8b837cc2810050e8db89feff83c40483f8",
     "physics: not-in-water branch reads sector, compares type==1"),
    (0x444FCC, "8b9340c2810083ca0889",
     "physics: set water bit (or flags,0x8)"),
    (0x444FDB, "8b834cc48100",
     "physics: enter-water posts chassis id +0x340c"),
    # -- flag readers ------------------------------------------------------
    (0x444122, "f68340c2810008",
     "physics: test bit 0x8 (speed *0.75 path)"),
    (0x444807, "f68340c2810008",
     "physics: test bit 0x8 (water impact instead of ground)"),
    (0x444141, "dc0d30234a00",
     "physics: fmul [0x4a2330] = 0.75 while in water"),
    (0x444847, "dc0d10234a00",
     "physics: water impact threshold factor 0.5"),
    (0x4448C0, "6af5e8e94c0300",
     "physics: event_post(-11) SYSTEM_PLAYER_HIT_WATER"),
    (0x444EAE, "6af6e8fb460300",
     "physics: event_post(-10) SYSTEM_PLAYER_HIT_GROUND"),
    (0x464C25, "81e700d41100",
     "save load: player flags &= 0x11d400 (bit 0x8 never restored)"),
    # -- palette -----------------------------------------------------------
    (0x44AF78, "6820d383005568f8bb4a0068ecbb4a00",
     "0x44af78: load GAMEPAL from frontend/gamepal"),
    (0x44AFA3, "6840d683005568acbb4a0068ecbb4a00",
     "0x44afa3: load WATERPAL from frontend/waterpal"),
    (0x44AFB3, "c705c4d2830020d38300",
     "0x44afb3: [0x83d2c4] = GAMEPAL (start)"),
    (0x447962, "f68640c281000874118b0dc4d28300b840d68300",
     "frame: if flags&8 -> palette = 0x83d640"),
    (0x449F4D, "f68040c28100080f8491e5ffff8b0dc4d28300b840d68300",
     "world pass: if flags&8 -> palette = 0x83d640"),
    (0x449EBA, "397c24107405e8eba80300",
     "frame end: if palette changed -> 0x4847b0 (brightness)"),
    (0x47F0B4, "c0ea02",
     "0x47efe0 palette loader: shr dl,2 (6-bit)"),
    (0x41EDD3, "558bec51c745fc00000000eb098b45fc",
     "0x41edd3: WATERPAL auto-generator"),
    (0x41EE00, "c1f802",
     "autogen: out R = in R >> 2"),
    (0x41EE20, "83c210",
     "autogen: out G = in G/2 + 0x10"),
    (0x41EE41, "83c10a",
     "autogen: out B = in B/2 + 0x0a"),
    # -- audio -------------------------------------------------------------
    (0x46BCC6, "8bb67cc28100",
     "sound 3D: player sector for +0x50 flags"),
    (0x46BCE5, "a8027403ff4510a8047403ff",
     "sound 3D: flags bit 2 adds +1 to shift, bit 4 adds +1"),
    (0x46C174, "8bb67cc28100",
     "sound loop 2: same sector flags, bits 2/4"),
    (0x46C404, "8b877cc28100",
     "sound loop 3: same sector flags, bits 2/4"),
    (0x46BB27, "f60640",
     "sound 3D: flags test is 0x40, not 0x8"),
    (0x46BDA4, "dd46e48b1edc4c2420",
     "gain: atten * 2^n, volume - atten*dist"),
    (0x46BDC2, "8b4d108bf885c9740403",
     "gain: if n != 0 -> sar edi, 2n"),
    (0x46BDCB, "03c9d3ff",
     "gain: shift = 2*n (add ecx,ecx; sar)"),
    # -- do not confuse: this is not code ----------------------------------
    (0x4AAB6B, "ffffffffffffffffffffffffffffffff",
     "0x4aab6b is 0xff filler, not the WATERPAL string"),
]

#: Player-state reads inside the audio range ``[0x409000, 0x46c600)``.
#: ``role`` documents why each is *not* a water test.
AUDIO_STATE_READS = [
    (0x46A64E, "player+0x323c sector -> ambient/sector sound selection"),
    (0x46A799, "player+0x323c sector -> ambient/sector sound selection"),
    (0x46AFB4, "player+0x323c sector -> 0x47e2d0 (sector sound helper)"),
    (0x46BB0E, "player+0x3200+0x10 = action_state (is the slot alive)"),
    (0x46BCC6, "player+0x323c sector -> +0x50 bits 2/4 gain shift (3D queue)"),
    (0x46BEF6, "player+0x3200+0x10 = action_state (second entry point)"),
    (0x46C174, "player+0x323c sector -> +0x50 bits 2/4 gain shift"),
    (0x46C404, "player+0x323c sector -> +0x50 bits 2/4 gain shift"),
    (0x46EE4D, "player+0x3200 & 0x1000 (bit ah,0x10; not water)"),
]

#: ``voice = 0x84e068 + i*0x50`` field map (``docs/SOUND_FIELDS.md`` §3).
#: 20 dwords, all named; there is no filter/cutoff/coefficient slot.
VOICE_FIELDS = [
    (0x00, "remaining", "slots left at 32 kHz"),
    (0x04, "sample_id", "index into 0x83d960"),
    (0x08, "step_hi", "rate/32000 (SOU0 only)"),
    (0x0C, "step_lo", "32.32 fraction (SOU0 only)"),
    (0x10, "cursor", "left channel pointer"),
    (0x14, "right_offset", "right = cursor + offset"),
    (0x18, "loop_start", "0 = one-shot"),
    (0x1C, "loop_len", "loop length in slots"),
    (0x20, "format", "0..4 decoder selector"),
    (0x24, "prev_vol_l", "for click-free scaling"),
    (0x28, "prev_vol_r", "for click-free scaling"),
    (0x2C, "vol_l", "1..32"),
    (0x30, "vol_r", "1..32"),
    (0x34, "music_a", "slot 8 only"),
    (0x38, "music_b", "slot 8 only"),
    (0x3C, "music_c", "slot 8 only"),
    (0x40, "acc_l", "ADPCM accumulator"),
    (0x44, "level_l", "ADPCM level"),
    (0x48, "acc_r", "ADPCM accumulator"),
    (0x4C, "level_r", "ADPCM level"),
]

VOICE_BASE = 0x84E068
VOICE_STRIDE = 0x50
MIXER = 0x409330

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


def read_bytes(va, n):
    """Raw bytes at ``va`` (``None`` when the RVA is unmapped)."""
    off = exelib.va2off(va)
    if off is None:
        return None
    return exelib.data()[off:off + n]


def u32(va):
    b = read_bytes(va, 4)
    return None if b is None or len(b) < 4 else struct.unpack("<I", b)[0]


def f64(va):
    b = read_bytes(va, 8)
    return None if b is None or len(b) < 8 else struct.unpack("<d", b)[0]


def sector_type(kind):
    """``0x42d99f``: ``u16 kind / 6`` (C ``idiv``; kind is always a multiple)."""
    return kind // 6


def _load_gm():
    from gm import sbt, sbtvm, sbw
    return sbt, sbtvm, sbw


def _walk(insns, conds=()):
    for ins in insns:
        name = ins.args[0][0] if ins.name in ("IF", "EVAL") and ins.args else None
        yield ins, conds
        if ins.body:
            yield from _walk(ins.body, conds + (name,) if name else conds)


SAMPLE_ARG = {
    "PLAY_SAMPLE": 0,
    "PLAY_SAMPLE_AT_LOCATION": 0,
    "PLAY_SAMPLE_AT_SOURCE": 0,
    "PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE": 2,
    "SPEAK_SAMPLE": 0,
    "PLAY_MUSIC": 0,
}


def _sample_of(ins):
    i = SAMPLE_ARG.get(ins.name)
    if i is None or not ins.args or len(ins.args) <= i:
        return None
    return ins.args[i]


def _script_table(rec):
    """``[(event_id, code_offset)]`` from the three SCRIPT handle columns."""
    n = rec.n
    if n <= 0:
        return []
    cols = [struct.unpack_from("<%dI" % n, rec.handles, 4 * n * k) for k in range(3)]
    return list(zip(cols[1], cols[2]))


def _code_at_event(table, event):
    want = event & 0xFFFFFFFF
    for ev, off in table:
        if ev == want:
            return off
    return None


def _first_sample_at(insns, off):
    """First ``PLAY_*`` with a sample at or after ``off`` (same script block)."""
    for ins in insns:
        if ins.off < off:
            continue
        if ins.name == "END":
            return None
        s = _sample_of(ins)
        if s is not None:
            return ins, s
    return None


def scan_levels(game=DEFAULT_GAME):
    """Per-level water census.  Missing corpus -> ``{"available": False}``."""
    files = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if not files:
        return {"available": False, "game": game}
    sbt, sbtvm, sbw = _load_gm()
    levels = []
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        doc = sbt.parse_file(path)
        rec = doc.records_of(11)[0].payload
        top, _end = sbtvm.decode(rec.code)
        flat = [i for i in top]
        names = list(doc.records_of(12)[0].payload.names)

        wpath = os.path.join(game, stem + ".sbw")
        kinds = []
        if os.path.exists(wpath):
            kinds = [s.kind for s in sbw.parse_file(wpath).sectors]

        tag_map = {}
        try:
            handles = doc.records_of(17)
            if handles:
                for tag, sector in handles[0].payload.rows():
                    tag_map.setdefault(tag, []).append(sector)
        except Exception:                          # noqa: BLE001 - optional table
            tag_map = {}

        entry = {
            "level": stem,
            "sectors": len(kinds),
            "water_sectors": kinds.count(6),
            "kind_histogram": {str(k): kinds.count(k) for k in sorted(set(kinds))},
            "su_sites": 0,
            "spu_sites": 0,
            "haze_under_water": [],
            "sector_flags_2_4": [],
            "sector_flags_2_4_on_water": 0,
            "type_changes": [],
            "sounds_on_water": [],
            "enter_water": None,
            "exit_water": None,
        }

        for ins, conds in _walk(top):
            if ins.name in ("IF", "EVAL") and ins.args:
                cname = ins.args[0][0]
                if cname == "SOURCE_UNDERWATER":
                    entry["su_sites"] += 1
                elif cname == "SOURCEPLAYER_UNDERWATER":
                    entry["spu_sites"] += 1
            if ins.name == "SET_CAMERA_HAZE" and "SOURCE_UNDERWATER" in conds:
                entry["haze_under_water"].append(
                    {"off": ins.off, "args": list(ins.args), "conds": list(conds)})
            if ins.name == "SET_SECTOR_FLAGS" and len(ins.args) >= 2:
                sector, value = int(ins.args[0]), int(ins.args[1])
                if value & 0x6:
                    kind = kinds[sector] if 0 <= sector < len(kinds) else None
                    entry["sector_flags_2_4"].append(
                        {"sector": sector, "flags": value, "kind": kind})
                    if kind == 6:
                        entry["sector_flags_2_4_on_water"] += 1
            if "SECTOR_TYPE" in ins.name and ins.args:
                item = {"off": ins.off, "cmd": ins.name, "args": list(ins.args)}
                if ins.name == "SET_HANDLESECTOR_TYPE":
                    item["sectors"] = list(tag_map.get(int(ins.args[0]), []))
                entry["type_changes"].append(item)
            if ins.name == "SET_SECTOR_SOUND" and len(ins.args) >= 2:
                sector, sample = int(ins.args[0]), int(ins.args[1])
                if 0 <= sector < len(kinds) and kinds[sector] == 6:
                    name = names[sample] if 0 <= sample < len(names) else None
                    entry["sounds_on_water"].append(
                        {"off": ins.off, "sector": sector, "sample": sample,
                         "name": name, "args": list(ins.args)})

        table = _script_table(rec)
        for key, event in (("enter_water", -30062), ("exit_water", 901010)):
            off = _code_at_event(table, event)
            hit = _first_sample_at(flat, off) if off is not None else None
            if hit:
                ins, sample = hit
                name = names[sample] if 0 <= sample < len(names) else None
                entry[key] = {"event": event, "off": ins.off, "cmd": ins.name,
                              "sample": sample, "name": name, "args": list(ins.args)}
            else:
                entry[key] = {"event": event, "off": off, "cmd": None,
                              "sample": None, "name": None}
        levels.append(entry)

    return {"available": True, "game": game, "files": len(files), "levels": levels}


# --------------------------------------------------------------------------
# Reports
# --------------------------------------------------------------------------


FIELD_ROWS = [
    ("player+0x3200", "u32", "flags; bit 0x8 = in water (derived, not saved)",
     "R 0x444122/0x444807/0x444F4A/0x45E911/0x447962/0x449F4D",
     "W 0x444FD2 (set), 0x444F74 (clear); loader mask 0x464C25"),
    ("player+0x323c", "i32", "current sector index (source of the water state)",
     "0x444F53/0x444FB8, 0x46BCC6 (sound), 0x45EC23 (cond 49)",
     "0x451160 collision, event_post sector slot"),
    ("player+0x3210", "i32", "action_state; 7 suppresses the water palette",
     "0x44795A, 0x44491B, 0x46BB13", "0x453303 / 0x444476"),
    ("player+0x340c", "u32", "chassis id posted as event on water ENTRY",
     "0x444FDB", "0x4530B9 (chassis select)"),
    ("player+0x3410", "u32", "chassis id posted as event on water EXIT",
     "0x444F7A", "0x4530D8 (chassis select)"),
    ("sector+0x14", "u16", "type*6; type 1 = water (getter 0x42d99f, setter 0x42d9d5)",
     "0x42D99F (VM 48, 0x444F53/0x444FB8)", "SET_*SECTOR_TYPE 0x42D9FF"),
    ("sector_stuff+0x50", "u32", "flags; 0x2 = damage, 0x4 = +1 gain shift, 0x8 = no ambient",
     "0x444166, 0x46BCE5, 0x46C193, 0x46C422, 0x46C652", "SET_SECTOR_FLAGS 0x457955"),
    ("[0x83d2c4]", "ptr", "current palette: 0x83d320 GAMEPAL / 0x83d640 WATERPAL",
     "0x44796B, 0x449F5A, 0x449FB0, 0x4847B0", "0x44798B, 0x449F69, 0x449FBF, 0x44AFB3"),
    ("0x83d640", "u8[768]", "WATERPAL, 6-bit RGB",
     "0x407220 via 0x4847B0", "0x44AFA3 (loader 0x47efe0), 0x41EDD3 (autogen)"),
    ("0x80f9a8", "f64", "camera haze level (SET_CAMERA_HAZE; not a water effect)",
     "0x44672C, 0x418C7F", "0x45B3C2"),
]


def print_fields():
    print("field                     type    role")
    for row in FIELD_ROWS:
        print("%-25s %-7s %s" % row[:3])
        print("%-25s %-7s read:  %s" % ("", "", row[3]))
        print("%-25s %-7s write: %s" % ("", "", row[4]))


def check_anchors():
    out = []
    for va, want, role in ANCHORS:
        got = read_bytes(va, len(want) // 2)
        ok = got is not None and got.hex() == want.lower()
        out.append({"va": "0x%06x" % va, "want": want.lower(),
                    "got": got.hex() if got else None, "ok": ok, "role": role})
    return out


def print_anchors(rows):
    bad = 0
    for r in rows:
        mark = "ok  " if r["ok"] else "FAIL"
        if not r["ok"]:
            bad += 1
        print("%s %s %s  %s" % (mark, r["va"], r["want"], r["role"]))
        if not r["ok"]:
            print("           got %s" % r["got"])
    print("%d anchors, %d failed" % (len(rows), bad))


def print_scripts(res):
    if not res.get("available"):
        print("no .sbt under %r" % res.get("game"))
        return
    print("%-8s %5s %5s %4s %4s %4s %4s  %s" %
          ("level", "sec", "water", "SU", "SPU", "haze", "f24", "enter/exit sample"))
    for lv in res["levels"]:
        ent = (lv["enter_water"] or {}).get("name")
        ex = (lv["exit_water"] or {}).get("name")
        print("%-8s %5d %5d %4d %4d %4d %4d  %s/%s" %
              (lv["level"], lv["sectors"], lv["water_sectors"], lv["su_sites"],
               lv["spu_sites"], len(lv["haze_under_water"]),
               len(lv["sector_flags_2_4"]), ent, ex))
    detail = [lv for lv in res["levels"]
              if lv["type_changes"] or lv["sounds_on_water"]
              or lv["sector_flags_2_4_on_water"]]
    if detail:
        print("\nwater-related script sites:")
        for lv in detail:
            print("  %s" % lv["level"])
            for t in lv["type_changes"]:
                extra = ""
                if t.get("sectors"):
                    extra = " -> sectors %s" % t["sectors"]
                print("    0x%-6x %-24s %s%s" % (t["off"], t["cmd"], t["args"], extra))
            for s in lv["sounds_on_water"]:
                print("    0x%-6x SET_SECTOR_SOUND sector=%d sample=%d (%s)" %
                      (s["off"], s["sector"], s["sample"], s["name"]))
            if lv["sector_flags_2_4"]:
                print("    flags 2/4: %d sites, on water: %d" %
                      (len(lv["sector_flags_2_4"]), lv["sector_flags_2_4_on_water"]))
    print("\nSET_SECTOR_FLAGS writes that touch bit 0x2/0x4 (sector kinds):")
    for lv in res["levels"]:
        for f in lv["sector_flags_2_4"]:
            print("  %-8s sector=%-4d flags=0x%02x kind=%s" %
                  (lv["level"], f["sector"], f["flags"], f["kind"]))


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

class _T:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, cond, name, detail=""):
        if cond:
            self.passed += 1
        else:
            self.failed.append("%s%s" % (name, (": " + detail) if detail else ""))


def _water_flag_trace(kinds):
    """Emulate 0x444F4A..0x445014 for a sequence of sector kinds."""
    flag = False
    trace = []
    for k in kinds:
        kind = k
        if flag:
            if kind != 6:
                flag = False
                trace.append(("clear", 0x444F74, kind))
        else:
            if kind == 6:
                flag = True
                trace.append(("set", 0x444FD2, kind))
    return flag, trace


def self_test(game=DEFAULT_GAME):
    t = _T()

    # -- byte anchors ------------------------------------------------------
    rows = check_anchors()
    for r in rows:
        t.check(r["ok"], "anchor %s" % r["va"], "want %s got %s" % (r["want"], r["got"]))
    t.check(read_bytes(0x4AAB6B, 32) == b"\xff" * 32,
            "0x4aab6b is 0xff filler (the string is at 0x4abb5c)")
    t.check(exelib.cstr(S_UNDERWATER_ERR).startswith("Unable to load underwater palette"),
            "0x4abb5c WATERPAL warning string")
    t.check(exelib.cstr(S_WATERPAL) == "waterpal", "0x4abbac 'waterpal'")
    t.check(exelib.cstr(S_GAMEPAL) == "gamepal", "0x4abbf8 'gamepal'")
    t.check(exelib.cstr(S_FRONTEND) == "frontend", "0x4abbec 'frontend'")

    # -- condition 47 arithmetic ------------------------------------------
    for i in range(8):
        eax = i
        ecx = eax + eax * 2
        ecx = ecx + ecx * 8
        ecx = ecx << 1
        ecx = ecx - eax
        ecx = ecx << 4
        ecx = ecx + eax
        ecx = ecx << 4
        t.check(ecx == i * PLAYER_STRIDE, "cond47 index*0x3510 for player %d" % i,
                "%d" % ecx)
    t.check(PLAYER_BASE + 0 * PLAYER_STRIDE + PLAYER_FLAGS == 0x81C240,
            "slot 0 flags address == 0x81c240")
    t.check(PLAYER_BASE + 0 * PLAYER_STRIDE + PLAYER_SECTOR == 0x81C27C,
            "slot 0 sector address == 0x81c27c (+0x323c, not +0x327c)")
    t.check(read_bytes(0x45E90B, 9)[-2:] == b"\xe0\x08",
            "cond47 masks with 0x8")

    # -- condition 48 semantics -------------------------------------------
    for kind, want in ((0, False), (6, True), (12, False), (18, False), (24, False),
                       (30, False)):
        t.check((sector_type(kind) == SECTOR_TYPE_WATER) == want,
                "cond48 kind=%d -> %s" % (kind, want))
    for raw, want in ((0, False), (5, False), (6, True), (7, True), (11, True),
                      (12, False), (0xFFFF, False)):
        t.check((raw // 6 == 1) == want, "getter raw=%d -> %s" % (raw, want))
    t.check(sector_type(6) * 6 == 6, "setter writes type*6")

    # -- flag transitions --------------------------------------------------
    flag, trace = _water_flag_trace([0, 6, 6, 12, 0, 6])
    t.check(flag is True, "after 0,6,6,12,0,6 the flag is set")
    t.check([s for s, _, _ in trace] == ["set", "clear", "set"],
            "transition order set/clear/set", str(trace))
    t.check(trace[0][1] == 0x444FD2 and trace[1][1] == 0x444F74,
            "set at 0x444fd2, clear at 0x444f74")
    # save loader never restores bit 0x8
    mask = 0x11D400
    t.check((mask & FLAG_WATER) == 0, "save-load mask 0x11d400 drops bit 0x8")

    # -- constants ---------------------------------------------------------
    t.check(f64(C_075) == 0.75, "0x4a2330 == 0.75")
    t.check(f64(C_05) == 0.5, "0x4a2310 == 0.5")
    t.check(f64(C_ONE) == 1.0, "0x4a2628 == 1.0")
    t.check(f64(C_ZERO) == 0.0, "0x4a22e0 == 0.0")

    # -- gain shift formula (sector flags bits 2/4) ------------------------
    for n, want in ((0, (1, 0)), (1, (2, 2)), (2, (4, 4))):
        t.check((2 ** n, 2 * n) == want, "gain shift n=%d" % n)
    t.check(struct.unpack("<d", b"\x33\x33\x33\x33\x33\x33\x03\xc0")[0] < 0.0,
            "water vertical velocity constant is negative")
    t.check((FLAG_DAMAGE, FLAG_AMBIENT_OFF) == (0x2, 0x8),
            "sector flag bit values")

    # -- palette auto-generator -------------------------------------------
    def autogen(r, g, b):
        return ((r >> 2) & 0xFF, ((g >> 1) + 0x10) & 0xFF, ((b >> 1) + 0x0A) & 0xFF)

    t.check(autogen(63, 63, 63) == (15, 47, 41), "autogen(63,63,63)")
    t.check(autogen(0, 0, 0) == (0, 16, 10), "autogen(0,0,0)")

    # -- corpus ------------------------------------------------------------
    res = scan_levels(game)
    if not res.get("available"):
        t.check(False, "corpus present", "no .sbt under %r" % game)
        return t, rows, res
    lv = res["levels"]
    t.check(res["files"] == 36, "36 .sbt levels", str(res["files"]))
    su = sum(x["su_sites"] for x in lv)
    spu = sum(x["spu_sites"] for x in lv)
    haze = [h for x in lv for h in x["haze_under_water"]]
    t.check(su == 690, "SOURCE_UNDERWATER sites == 690", str(su))
    t.check(spu == 72, "SOURCEPLAYER_UNDERWATER sites == 72", str(spu))
    t.check(len(haze) == 36, "SET_CAMERA_HAZE under SOURCE_UNDERWATER == 36",
            str(len(haze)))
    t.check(all(h["args"] == [1.0, 0.005] for h in haze),
            "every water haze is (1.0, 0.005)")
    t.check(len([x for x in lv if len(x["haze_under_water"]) == 1]) == 36,
            "exactly one water haze block per level")
    water = [x for x in lv if x["water_sectors"] > 0]
    t.check(len(water) == 17, "levels with static water sectors == 17", str(len(water)))
    t.check(sum(x["water_sectors"] for x in lv) == 133,
            "static water sectors == 133",
            str(sum(x["water_sectors"] for x in lv)))
    t.check(all(x["kind_histogram"].get("6", 0) == x["water_sectors"] for x in lv),
            "water_sectors == histogram[6]")
    f24 = [f for x in lv for f in x["sector_flags_2_4"]]
    on_water = sum(x["sector_flags_2_4_on_water"] for x in lv)
    t.check(len(f24) == 95, "SET_SECTOR_FLAGS with bit 2/4 == 95", str(len(f24)))
    t.check(on_water == 0, "no shipped water sector has flags bit 2/4 (sound shift)",
            str(on_water))
    type1 = [c for x in lv for c in x["type_changes"]
             if c["cmd"] == "SET_HANDLESECTOR_TYPE" and c["args"][1] == 1]
    t.check(len(type1) == 2, "runtime SET_HANDLESECTOR_TYPE ... 1 == 2", str(len(type1)))
    t.check(sorted(c["args"][0] for c in type1) == [33, 44],
            "runtime water tags are 33 and 44")
    ent = [x["enter_water"]["name"] for x in lv]
    ex = [x["exit_water"]["name"] for x in lv]
    t.check(ent.count("splash") == 33,
            "water ENTRY maps to sample 'splash' in 33/36 levels", str(ent.count("splash")))
    t.check(ex.count("splash3") == 36,
            "water EXIT maps to sample 'splash3' in 36/36 levels",
            str(ex.count("splash3")))
    t.check(all(x["enter_water"]["off"] is not None for x in lv),
            "every level resolves the -30062 entry handler")
    snd = [s for x in lv for s in x["sounds_on_water"]]
    t.check(len(snd) == 8, "SET_SECTOR_SOUND on static water sectors == 8",
            str(len(snd)))

    # -- WATERPAL lives in FRONTEND/BMP.IDX -------------------------------
    ok_pal = False
    try:
        from gm import bigfile
        idx = os.path.join(game, "FRONTEND", "BMP.IDX")
        bf = bigfile.BigFile(idx)
        entry = next((e for e in bf.entries if e.name == "WATERPAL"), None)
        gp = next((e for e in bf.entries if e.name == "GAMEPAL"), None)
        if entry and gp:
            raw = bf.read(entry)
            rg = bf.read(gp)
            ok_pal = len(raw) == 1142 and len(rg) == 1142
            t.check(ok_pal, "FRONTEND/BMP.IDX has GAMEPAL+WATERPAL (1142 B)")
            pal = raw[54:54 + 1024]
            cols = [(pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4])
                    for i in range(256)]      # BMP stores B,G,R
            mr = sum(c[0] for c in cols) / 256.0
            mb = sum(c[2] for c in cols) / 256.0
            t.check(mb > mr, "WATERPAL is blue/teal (mean B %.1f > mean R %.1f)"
                    % (mb, mr))
            gcols = [(rg[54 + i * 4 + 2], rg[54 + i * 4 + 1], rg[54 + i * 4])
                     for i in range(256)]
            diff = sum(1 for a, b in zip(cols, gcols) if a != b)
            t.check(diff > 200, "WATERPAL differs from GAMEPAL in %d/256 slots" % diff)
    except Exception as exc:                      # noqa: BLE001 - report it
        t.check(False, "FRONTEND/BMP.IDX palette check", str(exc))

    # -- audio: no water bit ----------------------------------------------
    try:
        path = exelib.asm_path()
        lines = open(path, encoding="latin1").read().splitlines()
    except OSError:
        lines = []
    reads = []
    bit8 = []
    for line in lines:
        if "0x81c240" not in line and "0x81c27c" not in line:
            continue
        try:
            va = int(line.split(":", 1)[0].strip(), 16)
        except ValueError:
            continue
        if not (0x409000 <= va < 0x40F000 or 0x46A000 <= va < 0x46F000):
            continue
        reads.append(va)
        body = line.split("\t")[-1]
        if re.search(r"0x8\b", body.split(",")[-1]):
            bit8.append(va)
    t.check(len(reads) > 0, "audio range scanned for player-state reads", str(len(reads)))
    t.check(sorted(reads) == sorted(a for a, _ in AUDIO_STATE_READS),
            "audio player-state read set matches the documented list",
            "%s" % ["0x%06x" % v for v in sorted(reads)])
    t.check(bit8 == [], "no audio instruction tests player flags bit 0x8",
            str(["0x%06x" % v for v in bit8]))
    t.check(all(read_bytes(va, 6) is not None for va, _ in AUDIO_STATE_READS),
            "all documented audio reads are mapped")
    for va, role in AUDIO_STATE_READS:
        t.check(read_bytes(va, 6) is not None, "audio read 0x%06x (%s)" % (va, role))

    # -- voice record: fully mapped, no filter field -----------------------
    t.check(VOICE_STRIDE == 0x50 and len(VOICE_FIELDS) * 4 == VOICE_STRIDE,
            "voice record 0x50 B == 20 documented dwords")
    t.check(all(off == 4 * i for i, (off, _, _) in enumerate(VOICE_FIELDS)),
            "voice fields are contiguous 0x00..0x4c")
    t.check(not any(("filter" in n or "cutoff" in n or n.startswith("lp"))
                    for _, n, _ in VOICE_FIELDS),
            "voice record has no filter/cutoff slot")
    t.check(read_bytes(MIXER, 4) == bytes.fromhex("a1d4224c"),
            "mixer 0x409330 entry")

    return t, rows, res


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main(argv):
    ap = argparse.ArgumentParser(
        description="Underwater state / palette / sound census (M7.7)")
    ap.add_argument("--self-test", action="store_true", help="anchors + logic + corpus")
    ap.add_argument("--fields", action="store_true", help="field map with VAs")
    ap.add_argument("--scripts", action="store_true", help="per-level water census")
    ap.add_argument("--anchors", action="store_true", help="byte anchors")
    ap.add_argument("--json", action="store_true", help="everything as one object")
    ap.add_argument("--game", default=DEFAULT_GAME, help="installation directory")
    args = ap.parse_args(argv)
    if not (args.self_test or args.fields or args.scripts or args.anchors or args.json):
        ap.print_help()
        return 1

    if args.json:
        t, rows, res = self_test(args.game)
        out = {
            "exe": {"path": exelib.EXE, "sha256": exelib.exe_sha256()},
            "constants": {
                "player_base": "0x%06x" % PLAYER_BASE,
                "player_stride": "0x%x" % PLAYER_STRIDE,
                "water_flag": "0x%x" % FLAG_WATER,
                "sector_kind_water": 6,
                "sector_type_water": SECTOR_TYPE_WATER,
                "gamepal": "0x%06x" % GAMEPAL,
                "waterpal": "0x%06x" % WATERPAL,
            },
            "fields": [{"field": f, "type": ty, "role": role, "read": r, "write": w}
                       for f, ty, role, r, w in FIELD_ROWS],
            "anchors": rows,
            "scripts": res,
            "self_test": {"passed": t.passed, "failed": t.failed,
                          "total": t.passed + len(t.failed)},
        }
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0 if not t.failed else 1

    rc = 0
    if args.anchors:
        print_anchors(check_anchors())
    if args.fields:
        print_fields()
    if args.scripts:
        print_scripts(scan_levels(args.game))
    if args.self_test:
        t, _rows, _res = self_test(args.game)
        total = t.passed + len(t.failed)
        for f in t.failed:
            print("FAIL %s" % f)
        print("self-test: %d/%d passed" % (t.passed, total))
        if t.failed:
            rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
