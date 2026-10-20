#!/usr/bin/env python3
"""Dump, structurally rebuild and validate Gunmetal ``.sbt`` records.

``gm/sbt.py`` owns the container and the typed payloads; this tool adds the
per-slot field maps for the three state blocks (8/19/18), the save-chain
``THING`` block (3), and a corpus-wide ``parse -> rebuild`` check for every
record type.

Record types (binary dispatcher ``0x455150`` -> table ``0x455604``)::

    1 SWITCH            2 DOOR              3 THING
    4 THING_TYPES       5 NAMED_ITEMS       6 SPRITES
    7 SOUND_SOURCES     8 SECTOR_STUFF      9 PLAYER_STATUS
    11 SCRIPT           12 SOUND_SAMPLE_TABLE
    15 POLY_HANDLE      16 OBJECT_HANDLE     17 SECTOR_HANDLE
    18 LIFT             19 ITEM             20 SOB_TABLE
    255 END_OF_FILE     10/13 = stubs      14 = corrupt

``SECTOR_STUFF`` (8, fixed ``0x5800``)
    256 records x ``0x58`` bytes, one per world sector (runtime array
    ``0x800b60`` .. ``0x806360``).  The record is *gameplay* state, not a
    render/light block: movement deltas, enter/leave effects, damage, ambient
    sound and flags.  Layout and consumers are listed in ``docs/RECORDS.md``.

``ITEM`` (19, fixed ``0x7080``)
    300 records x ``0x60`` bytes, the pickup table (runtime ``0x806360`` ..
    ``0x80d418``; the ``+0x38`` field is the well-known ``0x806398`` base).
    Filled by ``ADD_ITEM`` / ``ADD_ITEM_FIXED`` and their ``AT_SOURCE``
    variants (handlers ``0x4598e7`` / ``0x45994c`` / ``0x4599c5`` /
    ``0x459a14`` -> ``0x4686a0``).

``LIFT`` (18, variable)
    ``u32 n (<=30)`` + ``n * 0xa8`` lift records + ``u32 k (<=200)`` +
    three parallel ``u32[k]`` stop columns (``k * 12``).  Runtime arrays:
    lifts ``0x836de0``, stops ``0x5a9130`` / ``0x5a8e10`` / ``0x5a8af0``.
    No shipped ``.sbt`` and no shipped save contains this record; the layout
    is taken from the reader/writer ``0x44eed0`` and the producer
    ``ADD_LIFT`` (``0x45c9a0`` -> ``0x44e800``).

Sources: a plain level ``.sbt`` (shipped levels have none of 8/18/19) or a
``SAVEGAME/savedN.sbt`` file, whose appended state stream carries blocks
``6, 19, 8, 7, 3, [10], 9``.  Save blocks are located with
``tools/re/savefile.py`` (the block chain is self-checking).  Note that save
block 3 is ``260 x (u8 present + optional 0x528)``, unlike the single
``exists + 0x528`` record of level type 3.

Each field in the output carries a provenance tag:

``code``       address/instruction proves the layout or the consumer;
``data``       value ranges/cross-checks over the shipped corpus;
``hypothesis`` plausible meaning, not proven.

Usage::

    python3 tools/re/records.py                      # all levels + saves
    python3 tools/re/records.py --json               # machine-readable dump
    python3 tools/re/records.py --check              # parse -> rebuild, 38 files
    python3 tools/re/records.py --check --json       # per-type N/N report
    python3 tools/re/records.py --rebuild DIR        # write rebuilt copies
    python3 tools/re/records.py "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt"
    python3 tools/re/records.py --type 8,19 --all --raw
    python3 tools/re/records.py --fields             # field map only
    python3 tools/re/records.py --self-test          # negative/framing cases

The tool is read-only unless ``--rebuild`` is given; it never writes to the
game directory.
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
sys.path.insert(0, os.path.join(ROOT, "tools", "re"))

from gm import sbt as sbtmod  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# --- fixed record geometry ---------------------------------------------------

ITEM_SLOTS = 300
ITEM_SIZE = 0x60
ITEM_BLOCK = ITEM_SLOTS * ITEM_SIZE          # 0x7080
SECTOR_SLOTS = 256
SECTOR_SIZE = 0x58
SECTOR_BLOCK = SECTOR_SLOTS * SECTOR_SIZE    # 0x5800
LIFT_MAX = 0x1E
LIFT_SIZE = 0xA8
STOP_MAX = 0xC8
STOP_COLUMNS = ("floor_id", "height", "sound")
THING_SLOTS = 260
THING_SIZE = 0x528

#: How far each record type is re-encoded by ``--check``.
#: ``fields`` = every byte is rebuilt from decoded fields; ``framed`` = the
#: count/framing is structural and each record is exact raw bytes; ``opaque`` =
#: a fixed or length-prefixed raw block.
TYPE_MODE = {
    1: "framed", 2: "framed", 3: "opaque", 4: "fields", 5: "fields",
    6: "framed", 7: "fields", 8: "fields", 9: "opaque", 11: "fields",
    12: "fields", 15: "fields", 16: "fields", 17: "fields",
    18: "fields", 19: "fields", 20: "fields",
}

# Item state values, from the per-frame updater 0x468e41:
#   -2 free (0x468490), -1 active, 0 (re)spawn, -3/-4 pending removal,
#   >0 deathmatch respawn timer (0x468840).
ITEM_FREE = -2
ITEM_ACTIVE = -1
ITEM_SPAWN = 0
ITEM_PENDING = (-3, -4)

#: Item record fields: (offset, name, kind, provenance, note)
ITEM_FIELDS = (
    (0x00, "basis0", "3d", "code",
     "first basis vector; 0x41e025 builds the object matrix (0x4688ad)"),
    (0x18, "position", "3d", "code",
     "copied to object+0x00 (0x468507) and used as x/y/z"),
    (0x30, "runtime_object", "u32", "code",
     "0x468619/0x4685cc store the created object/emitter; 0 in saves"),
    (0x34, "kind", "u32", "code",
     "1 = ADD_ITEM / ADD_ITEM_AT_SOURCE, 2 = *_FIXED (0x468715)"),
    (0x38, "state", "i32", "code",
     "-2 free, -1 active, 0 (re)spawn, -3/-4 pending, >0 DM timer (0x468e41); named items: f2"),
    (0x3c, "op2", "u32", "code", "second script operand (a2); named items: f3"),
    (0x40, "sob_index", "i32", "code",
     "index into SOB_TABLE; 0x47c990 -> 0x4277cd (0x4684b0, 0x468670)"),
    (0x44, "op3", "u32", "code", "script operand a3; named items: f5 (0x468d7f..0x468da2)"),
    (0x48, "op4", "u32", "code", "script operand a4; named items: f6"),
    (0x4c, "op5", "u32", "code", "script operand a5; named items: f7"),
    (0x50, "op6", "u32", "code", "script operand a6; named items: f8, duplicated at +0x54"),
    (0x54, "op6_hi", "u32", "code", "copy of +0x50 (0x4687be/0x4687c4)"),
    (0x58, "flags", "u8", "code", "copied to object+0x74 (0x468516)"),
    (0x5c, "sentinel", "u32", "code", "0xffff written by all four handlers"),
)

#: Sector record fields: (offset, name, kind, provenance, note)
SECTOR_FIELDS = (
    (0x00, "move", "3d", "code",
     "SET_SECTOR_MOVEMENT a1..a3; 0x4713a5 adds move*dt to object positions"),
    (0x18, "extra", "3d", "code",
     "SET_SECTOR_MOVEMENT a4..a6 (0x457846); written only, no reader found"),
    (0x30, "sound_param", "f64", "code",
     "SET_SECTOR_SOUND D operand, stored as +0x30/+0x34 (0x46b790)"),
    (0x38, "event_a", "u32", "code",
     "SET_SECTOR_EVENTS a1; 0x445095 passes it to 0x4795b0"),
    (0x3c, "event_b", "u32", "code",
     "SET_SECTOR_EVENTS a2; 0x44503f passes it to 0x4795b0"),
    (0x40, "damage_lo", "u16", "code",
     "low half of SET_SECTOR_DAMAGE operand (0x457997), used at 0x4450f6"),
    (0x44, "damage_hi", "u16", "code",
     "high half of SET_SECTOR_DAMAGE operand (0x45797c)"),
    (0x48, "sound", "i32", "code",
     "SET_SECTOR_SOUND V operand: SOUND_SAMPLE_TABLE index (0x46b6e3)"),
    (0x4c, "sound_count", "i32", "code",
     "SET_SECTOR_SOUND N operand; >0 means active (0x46c649)"),
    (0x50, "flags", "u32", "code",
     "SET_SECTOR_FLAGS L operand; bits 1/2/4/8/0x10 used (0x444166, "
     "0x46bce5, 0x46c652, 0x444767)"),
    (0x54, "unused", "u32", "hypothesis",
     "no reader/writer; zero in all shipped saves"),
)

#: Lift record fields: (offset, name, kind, provenance, note)
LIFT_FIELDS = (
    (0x00, "basis0", "3d", "code", "six doubles -> object matrix (0x44ea6a)"),
    (0x18, "basis1", "3d", "code", "second basis vector (0x41e025)"),
    (0x30, "sob_index", "i32", "code",
     "car model: 0x47c990 -> 0x4277cd (0x44e706)"),
    (0x34, "kind", "i32", "code", "must be 1 for the elevator logic (0x44f7b6)"),
    (0x38, "sob_index2", "i32", "code",
     "second model created on add (0x44e9d4) - button/panel"),
    (0x3c, "x", "i32", "code", "button position, fiadd at 0x44e743"),
    (0x40, "y", "i32", "code", "button position, fiadd at 0x44e755"),
    (0x44, "z", "i32", "code", "button position, fiadd at 0x44e767"),
    (0x54, "speed", "i32", "code", "height step per tick (0x4501c5)"),
    (0x5c, "height", "i32", "code", "current height (0x4501c2, 0x44f8cd)"),
    (0x60, "target", "i32", "code", "target height (0x4501ce)"),
    (0x68, "moving", "i32", "code", "non-zero while travelling (0x4501a1)"),
    (0x6c, "stop_index", "i32", "code",
     "current stop within this lift (0x44f7d0, 0x4501ad)"),
    (0x70, "call_1", "i32", "code", "call-button floor id (0x44f7e0)"),
    (0x74, "call_2", "i32", "code", "call-button floor id (0x44f7f7)"),
    (0x78, "call_3", "i32", "code", "call-button floor id (0x44f7f7)"),
    (0x9c, "stop_count", "i32", "code", "stops of this lift, 0x44ebc3"),
    (0xa0, "stop_start", "i32", "code",
     "first global stop index, 0x44e9c4; used as 0xa0+0x6c"),
)


def _i32(b, off):
    return struct.unpack_from("<i", b, off)[0]


def _u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def _f64(b, off):
    return struct.unpack_from("<d", b, off)[0]


# --- item table --------------------------------------------------------------


def decode_items(payload, sob_count=None, include_empty=False):
    if len(payload) != ITEM_BLOCK:
        raise ValueError("ITEM payload is %d bytes, expected 0x%x"
                         % (len(payload), ITEM_BLOCK))
    slots = []
    for i in range(ITEM_SLOTS):
        raw = payload[i * ITEM_SIZE:(i + 1) * ITEM_SIZE]
        state = _i32(raw, 0x38)
        item = {
            "index": i,
            "state": state,
            "active": state != ITEM_FREE,
            "kind": _u32(raw, 0x34),
            "sob_index": _i32(raw, 0x40),
            "ops": [_i32(raw, o) for o in (0x3C, 0x44, 0x48, 0x4C, 0x50)],
            "op6_hi": _u32(raw, 0x54),
            "basis": list(struct.unpack_from("<3d", raw, 0x00)),
            "position": list(struct.unpack_from("<3d", raw, 0x18)),
            "runtime_object": _u32(raw, 0x30),
            "flags": raw[0x58],
            "pad": raw[0x59:0x5C].hex(),
            "sentinel": _u32(raw, 0x5C),
            "raw": raw.hex(),
        }
        if item["active"] or include_empty:
            slots.append(item)
    checks = []
    for it in slots:
        if it["kind"] not in (1, 2):
            checks.append("item %d: unexpected kind %d" % (it["index"], it["kind"]))
        if it["state"] not in (ITEM_ACTIVE, ITEM_SPAWN) + ITEM_PENDING \
                and it["state"] <= 0:
            checks.append("item %d: unexpected negative state %d"
                          % (it["index"], it["state"]))
        if sob_count is not None and not 0 <= it["sob_index"] < sob_count:
            checks.append("item %d: SOB index %d out of range 0..%d"
                          % (it["index"], it["sob_index"], sob_count - 1))
        pad = bytes.fromhex(it["raw"])[0x59:0x5C]
        if pad != b"\0\0\0":
            checks.append("item %d: non-zero padding %r" % (it["index"], pad))
        if it["sentinel"] != 0xFFFF:
            checks.append("item %d: sentinel 0x%x != 0xffff"
                          % (it["index"], it["sentinel"]))
    return {"slots": ITEM_SLOTS, "active": len(slots), "items": slots,
            "checks": checks}


def encode_items(decoded):
    """Re-encode the 300-slot item table from decoded fields."""
    if len(decoded["items"]) != ITEM_SLOTS:
        raise ValueError("ITEM encoder needs all 300 slots (use include_empty)")
    out = bytearray(ITEM_BLOCK)
    for it in decoded["items"]:
        o = it["index"] * ITEM_SIZE
        struct.pack_into("<3d", out, o + 0x00, *it["basis"])
        struct.pack_into("<3d", out, o + 0x18, *it["position"])
        struct.pack_into("<I", out, o + 0x30, it["runtime_object"])
        struct.pack_into("<I", out, o + 0x34, it["kind"])
        struct.pack_into("<i", out, o + 0x38, it["state"])
        struct.pack_into("<i", out, o + 0x3C, it["ops"][0])
        struct.pack_into("<i", out, o + 0x40, it["sob_index"])
        struct.pack_into("<4i", out, o + 0x44, *it["ops"][1:])
        struct.pack_into("<I", out, o + 0x54, it["op6_hi"])
        out[o + 0x58] = it["flags"]
        out[o + 0x59:o + 0x5C] = bytes.fromhex(it.get("pad", "000000"))
        struct.pack_into("<I", out, o + 0x5C, it["sentinel"])
    return bytes(out)


# --- sector table ------------------------------------------------------------


def decode_sectors(payload, sample_count=None, include_empty=False):
    if len(payload) != SECTOR_BLOCK:
        raise ValueError("SECTOR_STUFF payload is %d bytes, expected 0x%x"
                         % (len(payload), SECTOR_BLOCK))
    sectors = []
    for i in range(SECTOR_SLOTS):
        raw = payload[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]
        if not any(raw) and not include_empty:
            continue
        sec = {
            "index": i,
            "move": list(struct.unpack_from("<3d", raw, 0x00)),
            "extra": list(struct.unpack_from("<3d", raw, 0x18)),
            "sound_param": _f64(raw, 0x30),
            "event_a": _i32(raw, 0x38),
            "event_b": _i32(raw, 0x3C),
            "damage_lo": struct.unpack_from("<H", raw, 0x40)[0],
            "damage_hi": struct.unpack_from("<H", raw, 0x44)[0],
            "pad_42": raw[0x42:0x44].hex(),
            "pad_46": raw[0x46:0x48].hex(),
            "sound": _i32(raw, 0x48),
            "sound_count": _i32(raw, 0x4C),
            "flags": _u32(raw, 0x50),
            "unused": _u32(raw, 0x54),
            "raw": raw.hex(),
        }
        sectors.append(sec)
    checks = []
    for sec in sectors:
        if sec["unused"]:
            checks.append("sector %d: unused +0x54 = 0x%x"
                          % (sec["index"], sec["unused"]))
        if sample_count is not None and sec["sound"] \
                and not 0 <= sec["sound"] < sample_count:
            checks.append("sector %d: sample %d out of range 0..%d"
                          % (sec["index"], sec["sound"], sample_count - 1))
        if sec["flags"] & ~0x1F:
            checks.append("sector %d: unknown flag bits 0x%x"
                          % (sec["index"], sec["flags"] & ~0x1F))
    return {"slots": SECTOR_SLOTS, "nonzero": len(sectors), "sectors": sectors,
            "checks": checks}


def encode_sectors(decoded):
    """Re-encode the decoded 256-sector table (fields cover all 0x58 bytes)."""
    out = bytearray(SECTOR_BLOCK)
    for sec in decoded["sectors"]:
        o = sec["index"] * SECTOR_SIZE
        struct.pack_into("<3d", out, o + 0x00, *sec["move"])
        struct.pack_into("<3d", out, o + 0x18, *sec["extra"])
        struct.pack_into("<d", out, o + 0x30, sec["sound_param"])
        struct.pack_into("<i", out, o + 0x38, sec["event_a"])
        struct.pack_into("<i", out, o + 0x3C, sec["event_b"])
        struct.pack_into("<H", out, o + 0x40, sec["damage_lo"])
        out[o + 0x42:o + 0x44] = bytes.fromhex(sec.get("pad_42", "0000"))
        struct.pack_into("<H", out, o + 0x44, sec["damage_hi"])
        out[o + 0x46:o + 0x48] = bytes.fromhex(sec.get("pad_46", "0000"))
        struct.pack_into("<i", out, o + 0x48, sec["sound"])
        struct.pack_into("<i", out, o + 0x4C, sec["sound_count"])
        struct.pack_into("<I", out, o + 0x50, sec["flags"])
        struct.pack_into("<I", out, o + 0x54, sec["unused"])
    return bytes(out)


# --- lift table --------------------------------------------------------------


def decode_lifts(payload):
    if len(payload) < 8:
        raise ValueError("LIFT payload too short")
    n = _u32(payload, 0)
    if n > LIFT_MAX:
        raise ValueError("LIFT count %d > %d" % (n, LIFT_MAX))
    need = 4 + n * LIFT_SIZE + 4
    if len(payload) < need:
        raise ValueError("LIFT payload truncated (need %d, have %d)"
                         % (need, len(payload)))
    body = payload[4:4 + n * LIFT_SIZE]
    k = _u32(payload, 4 + n * LIFT_SIZE)
    if k > STOP_MAX:
        raise ValueError("LIFT stop count %d > %d" % (k, STOP_MAX))
    if len(payload) != need + k * 12:
        raise ValueError("LIFT payload is %d bytes, expected %d"
                         % (len(payload), need + k * 12))
    p = 4 + n * LIFT_SIZE + 4
    cols = []
    for _ in STOP_COLUMNS:
        cols.append(struct.unpack_from("<%dI" % k, payload, p))
        p += 4 * k
    checks = []
    lifts = []
    for i in range(n):
        rec = body[i * LIFT_SIZE:(i + 1) * LIFT_SIZE]
        start = _i32(rec, 0xA0)
        count = _i32(rec, 0x9C)
        if start < 0 or count < 0 or start + count > k:
            checks.append("lift %d: stop window %d+%d outside 0..%d"
                          % (i, start, count, k))
        stops = []
        if 0 <= start and 0 <= count and start + count <= k:
            for j in range(count):
                stops.append({name: cols[c][start + j]
                              for c, name in enumerate(STOP_COLUMNS)})
        lifts.append({
            "index": i,
            "kind": _i32(rec, 0x34),
            "sob_index": _i32(rec, 0x30),
            "sob_index2": _i32(rec, 0x38),
            "position": [_i32(rec, o) for o in (0x3C, 0x40, 0x44)],
            "basis": list(struct.unpack_from("<6d", rec, 0x00)),
            "speed": _i32(rec, 0x54),
            "height": _i32(rec, 0x5C),
            "target": _i32(rec, 0x60),
            "moving": _i32(rec, 0x68),
            "stop_index": _i32(rec, 0x6C),
            "calls": [_i32(rec, o) for o in (0x70, 0x74, 0x78)],
            "stop_start": start,
            "stop_count": count,
            "stops": stops,
            "raw": rec.hex(),
        })
    return {"lifts": n, "stops": k, "records": lifts,
            "stop_columns": list(cols), "checks": checks}


# --- things block (save chain id 3) -----------------------------------------


def decode_things(payload):
    """Decode the save-chain things block: 260 × (u8 present + optional 0x528).

    This is the *save* layout; the level-record layout of type 3 is a single
    ``u8 exists`` + one 0x528 record (loader ``0x46fd90``), so the two must not
    be confused.  The 0x528 body is runtime state and is kept verbatim.
    """
    slots = []
    p = 0
    for i in range(THING_SLOTS):
        if p >= len(payload):
            raise ValueError("THING block truncated at slot %d" % i)
        present = payload[p]
        if present not in (0, 1):
            raise ValueError("THING slot %d: present=%d" % (i, present))
        p += 1
        raw = b""
        if present:
            if p + THING_SIZE > len(payload):
                raise ValueError("THING slot %d: truncated 0x528 record" % i)
            raw = payload[p:p + THING_SIZE]
            p += THING_SIZE
        slots.append({"index": i, "present": present, "raw": raw.hex()})
    if p != len(payload):
        raise ValueError("THING block has %d trailing bytes" % (len(payload) - p))
    return {"slots": THING_SLOTS,
            "present": sum(1 for s in slots if s["present"]),
            "things": slots, "checks": []}


def encode_things(decoded):
    out = bytearray()
    if len(decoded["things"]) != THING_SLOTS:
        raise ValueError("THING encoder needs all 260 slots")
    for s in decoded["things"]:
        out.append(s["present"])
        if s["present"]:
            raw = bytes.fromhex(s["raw"])
            if len(raw) != THING_SIZE:
                raise ValueError("THING slot %d: bad record length" % s["index"])
            out += raw
    return bytes(out)


# --- corpus-wide structural check -------------------------------------------


def _payload_writer(type_, payload, raw, level_tables=None):
    """Return the structural re-encoding of one record payload."""
    if type_ == 8:
        dec = decode_sectors(raw, level_tables[1] if level_tables else None, True)
        return encode_sectors(dec)
    if type_ == 19:
        dec = decode_items(raw, level_tables[0] if level_tables else None, True)
        return encode_items(dec)
    return sbtmod.encode_payload(type_, payload)


def _save_tables(game, level):
    try:
        return _level_tables(game, level)
    except Exception:
        return None


def check_sbt(path, rebuild_dir=None):
    data = open(path, "rb").read()
    f = sbtmod.parse_file(path)
    tables = None
    base = os.path.basename(path)
    recs = []
    for r in f.records:
        if r.type == sbtmod.END_OF_FILE:
            continue
        item = {"type": r.type, "name": r.name, "offset": r.offset,
                "size": r.size, "mode": TYPE_MODE.get(r.type, "raw"),
                "rebuilt": False, "error": None}
        try:
            enc = _payload_writer(r.type, r.payload, r.raw, tables)
            item["rebuilt"] = enc == r.raw
            if not item["rebuilt"]:
                item["error"] = ("structural encoding differs (%d vs %d bytes)"
                                 % (len(enc), len(r.raw)))
        except Exception as exc:
            item["error"] = str(exc)
        recs.append(item)
    # a second, whole-file pass catches header/count/EOF mistakes
    try:
        rebuilt = f.rebuild(verify=True)
        if rebuilt != data:
            raise ValueError("whole-file rebuild differs from the source")
        item = {"whole_file": True, "rebuilt": True, "error": None}
    except Exception as exc:
        item = {"whole_file": True, "rebuilt": False, "error": str(exc)}
    recs.append(item)
    doc = {"file": os.path.abspath(path), "kind": "level",
           "records": recs, "record_types": [r.type for r in f.records]}
    if rebuild_dir:
        out = os.path.join(rebuild_dir, base)
        with open(out, "wb") as fh:
            fh.write(f.rebuild())
        doc["rebuild_path"] = os.path.abspath(out)
        doc["rebuild_exact"] = open(out, "rb").read() == data
    return doc


def check_save(path, game, rebuild_dir=None):
    import savefile  # tools/re

    data = open(path, "rb").read()
    info = savefile.inspect(path, game)
    tail_abs, tail_len = info["state_tail"]
    level = info.get("source_level")
    tables = _save_tables(game, level) if level else None
    chain = info.get("chain", [])
    recs = []
    rebuilt_chain = bytearray()
    for block in chain:
        type_ = block["id"]
        off = tail_abs + block["offset"]
        payload = data[off + 1:off + block["size"]]
        item = {"type": type_, "name": sbtmod.TYPE_NAMES.get(type_, "?"),
                "offset": off, "size": block["size"],
                "mode": "framed" if type_ == 3 else TYPE_MODE.get(type_, "raw"),
                "rebuilt": False, "error": None}
        try:
            if type_ == 3:
                enc = encode_things(decode_things(payload))
            else:
                enc = _payload_writer(type_, sbtmod.parse_payload(type_, payload),
                                      payload, tables)
            item["rebuilt"] = enc == payload
            if not item["rebuilt"]:
                item["error"] = ("structural encoding differs (%d vs %d bytes)"
                                 % (len(enc), len(payload)))
            rebuilt_chain += bytes((type_,)) + enc
        except Exception as exc:
            item["error"] = str(exc)
            rebuilt_chain += data[off:off + block["size"]]
        recs.append(item)
    chain_end = tail_abs + chain[-1]["offset"] + chain[-1]["size"] if chain else tail_abs
    chain_start = tail_abs + chain[0]["offset"] if chain else tail_abs
    rebuilt = data[:chain_start] + bytes(rebuilt_chain) + data[chain_end:]
    doc = {"file": os.path.abspath(path), "kind": "save", "level": level,
           "save_name": info.get("level_name"),
           "records": recs,
           "chain": [{k: b.get(k) for k in ("id", "offset", "size")}
                     for b in chain],
           "whole_file": rebuilt == data}
    if rebuild_dir:
        out = os.path.join(rebuild_dir, os.path.basename(path))
        with open(out, "wb") as fh:
            fh.write(rebuilt)
        doc["rebuild_path"] = os.path.abspath(out)
        doc["rebuild_exact"] = open(out, "rb").read() == data
    return doc


# --- self-test ---------------------------------------------------------------


def _synthetic_lift(n=2, k=3):
    out = bytearray(struct.pack("<I", n))
    for i in range(n):
        rec = bytearray(LIFT_SIZE)
        struct.pack_into("<i", rec, 0x30, 36 + i)
        struct.pack_into("<i", rec, 0x34, 1)
        struct.pack_into("<i", rec, 0x9C, 1)
        struct.pack_into("<i", rec, 0xA0, i)
        out += rec
    out += struct.pack("<I", k)
    out += struct.pack("<%dI" % k, *range(100, 100 + k))      # floor ids
    out += struct.pack("<%dI" % k, *range(0, 10 * k, 10))     # heights
    out += struct.pack("<%dI" % k, *range(200, 200 + k))      # sounds
    return bytes(out)


def self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        print("%-46s %s" % (name, "ok" if cond else "FAIL"))
        ok = ok and bool(cond)

    # sector: full-size zero block, field round-trip through the decoder
    zero = bytes(SECTOR_BLOCK)
    dec = decode_sectors(zero)
    check("sector 0x5800 zero block", dec["slots"] == 256 and not dec["nonzero"])
    raw = bytearray(zero)
    struct.pack_into("<3d", raw, 9 * SECTOR_SIZE, 1.0, -2.0, 0.5)
    struct.pack_into("<i", raw, 9 * SECTOR_SIZE + 0x48, 130)
    struct.pack_into("<i", raw, 9 * SECTOR_SIZE + 0x4C, 10)
    dec = decode_sectors(bytes(raw))
    sec = dec["sectors"][0]
    check("sector movement/sound decode",
          sec["index"] == 9 and sec["move"] == [1.0, -2.0, 0.5]
          and sec["sound"] == 130 and sec["sound_count"] == 10)
    try:
        decode_sectors(bytes(SECTOR_BLOCK - 1))
        check("sector short payload rejected", False)
    except ValueError:
        check("sector short payload rejected", True)

    # item: 300 slots, one active slot
    raw = bytearray(ITEM_BLOCK)
    for i in range(ITEM_SLOTS):
        struct.pack_into("<i", raw, i * ITEM_SIZE + 0x38, ITEM_FREE)
    struct.pack_into("<i", raw, 0x34, 2)
    struct.pack_into("<i", raw, 0x38, ITEM_ACTIVE)
    struct.pack_into("<i", raw, 0x40, 36)
    struct.pack_into("<3d", raw, 0x18, 1.0, 2.0, 3.0)
    struct.pack_into("<I", raw, 0x5C, 0xFFFF)
    dec = decode_items(bytes(raw))
    it = dec["items"][0]
    check("item 300 slots / 1 active", dec["slots"] == 300 and dec["active"] == 1)
    check("item kind/position/sob",
          it["kind"] == 2 and it["position"] == [1.0, 2.0, 3.0]
          and it["sob_index"] == 36)
    try:
        decode_items(bytes(ITEM_BLOCK - 1))
        check("item short payload rejected", False)
    except ValueError:
        check("item short payload rejected", True)

    # lift: synthetic counts, stop windows, bounds
    dec = decode_lifts(_synthetic_lift())
    check("lift n/k and stop windows",
          dec["lifts"] == 2 and dec["stops"] == 3
          and dec["records"][0]["stops"][0]["floor_id"] == 100
          and dec["records"][1]["stops"][0]["floor_id"] == 101)
    dec = decode_lifts(struct.pack("<II", 0, 0))
    check("lift empty payload (n=0,k=0)", dec["lifts"] == 0 and dec["stops"] == 0)
    try:
        decode_lifts(struct.pack("<I", LIFT_MAX + 1))
        check("lift count > 30 rejected", False)
    except ValueError:
        check("lift count > 30 rejected", True)
    try:
        decode_lifts(_synthetic_lift()[:-4])
        check("lift truncated stops rejected", False)
    except ValueError:
        check("lift truncated stops rejected", True)
    bad = bytearray(_synthetic_lift())
    struct.pack_into("<i", bad, 4 + 0xA0, 2)     # lift 0 starts at 2
    struct.pack_into("<i", bad, 4 + 0x9C, 2)     # and claims 2 stops -> 4 > 3
    dec = decode_lifts(bytes(bad))
    check("lift stop window overflow flagged", bool(dec["checks"]))

    # --- .sbt structural writers / negative framing cases -------------------
    def sbt_file(payloads):
        hdr = sbtmod.MAGIC + b"\0" * (80 - len(sbtmod.MAGIC)) + b"\0" * 4
        body = b"".join(bytes((t,)) + p for t, p in payloads)
        return hdr + body + bytes((sbtmod.END_OF_FILE,))

    def parse_ok(name, data):
        try:
            f = sbtmod.parse(data)
            return f.rebuild(verify=True) == data
        except ValueError:
            return False

    switch = struct.pack("<I", 1) + bytes(range(32))
    door = struct.pack("<I", 1) + bytes(range(184))
    sprite = struct.pack("<I", 1) + bytes(44)
    sound = (struct.pack("<I", 1) + struct.pack("<5i", 1, 2, 3, 4, 5)
             + struct.pack("<d", 0.5))
    sample = struct.pack("<300I", *range(300)) + b"name\0\0\0\0\0" * 300
    handles = struct.pack("<I", 1) + struct.pack("<4I", 1, 2, 3, 4)
    sob = struct.pack("<I", 1) + b"model\0\0\0\0\0\0\0" + struct.pack("<II", 7, 0)
    good = [(1, switch), (2, door), (6, sprite), (7, sound),
            (12, sample), (15, handles), (20, sob)]
    data = sbt_file(good)
    check("all writer types parse->rebuild exact", parse_ok("writers", data))

    try:
        sbtmod.parse(sbt_file([(4, struct.pack("<I", 0x81))]))
        check("huge THING_TYPES count rejected", False)
    except ValueError:
        check("huge THING_TYPES count rejected", True)
    try:
        sbtmod.parse(sbt_file([(15, struct.pack("<I", 3) + struct.pack("<4I", 1, 2, 3, 4))]))
        check("handle table OOB/truncated rejected", False)
    except ValueError:
        check("handle table OOB/truncated rejected", True)
    try:
        sbtmod.parse(sbt_file([(6, struct.pack("<I", 0x1F4))]))
        check("huge SPRITES count rejected", False)
    except ValueError:
        check("huge SPRITES count rejected", True)
    try:
        sbtmod.parse(sbt_file([(3, b"\x01" + bytes(16))]))
        check("truncated THING record rejected", False)
    except ValueError:
        check("truncated THING record rejected", True)
    try:
        sbtmod.parse(sbt_file([(5, struct.pack("<I", 0x32))]))
        check("huge NAMED_ITEMS count rejected", False)
    except ValueError:
        check("huge NAMED_ITEMS count rejected", True)
    try:
        sbtmod.parse(sbt_file([(20, struct.pack("<I", 0xC9))]))
        check("huge SOB_TABLE count rejected", False)
    except ValueError:
        check("huge SOB_TABLE count rejected", True)
    try:
        sbtmod.parse(sbt_file([(12, struct.pack("<300I", *range(300)))]))
        check("truncated SOUND_SAMPLE_TABLE rejected", False)
    except ValueError:
        check("truncated SOUND_SAMPLE_TABLE rejected", True)
    try:
        sbtmod.parse(sbt_file([(7, struct.pack("<I", 2) + struct.pack("<5i", 1, 2, 3, 4, 5))]))
        check("truncated SOUND_SOURCES columns rejected", False)
    except ValueError:
        check("truncated SOUND_SOURCES columns rejected", True)
    try:
        sbtmod.parse(sbt_file([(9, b"\x01" + bytes(16))]))
        check("truncated PLAYER_STATUS rejected", False)
    except ValueError:
        check("truncated PLAYER_STATUS rejected", True)
    try:
        sbtmod.parse(sbt_file([(11, struct.pack("<I", 0))])[:-1])
        check("missing END_OF_FILE rejected", False)
    except ValueError:
        check("missing END_OF_FILE rejected", True)
    try:
        sbtmod.parse(sbt_file([(255, b"")]) + b"\0")
        check("trailing bytes after EOF rejected", False)
    except ValueError:
        check("trailing bytes after EOF rejected", True)
    try:
        sbtmod.parse(sbt_file([(14, b"")]))
        check("unknown record type 14 rejected", False)
    except ValueError:
        check("unknown record type 14 rejected", True)
    try:
        sbtmod.parse(sbt_file([(13, b"")]))
        check("unsupported record type 13 rejected", False)
    except ValueError:
        check("unsupported record type 13 rejected", True)

    # save-chain things block: 260 present flags, exact records
    things = bytes((1,)) + bytes(THING_SIZE) + bytes(THING_SLOTS - 1)
    dec = decode_things(things)
    check("save THING block 260 slots / 1 present",
          dec["slots"] == 260 and dec["present"] == 1
          and encode_things(dec) == things)
    try:
        decode_things(things[:-1])
        check("save THING block truncation rejected", False)
    except ValueError:
        check("save THING block truncation rejected", True)
    try:
        decode_things(things + b"\0")
        check("save THING block trailing byte rejected", False)
    except ValueError:
        check("save THING block trailing byte rejected", True)
    try:
        decode_things(bytes([2]) + bytes(THING_SLOTS - 1))
        check("save THING block bad present flag rejected", False)
    except ValueError:
        check("save THING block bad present flag rejected", True)

    # item/sector field encoders must reproduce a synthetic block
    raw = bytearray(ITEM_BLOCK)
    for i in range(ITEM_SLOTS):
        struct.pack_into("<i", raw, i * ITEM_SIZE + 0x38, ITEM_FREE)
        struct.pack_into("<I", raw, i * ITEM_SIZE + 0x5C, 0xFFFF)
    dec = decode_items(bytes(raw), include_empty=True)
    check("ITEM 300-slot field encoder exact",
          encode_items(dec) == bytes(raw))
    raw2 = bytearray(SECTOR_BLOCK)
    struct.pack_into("<3d", raw2, 7 * SECTOR_SIZE, 1.0, 2.0, 3.0)
    struct.pack_into("<i", raw2, 7 * SECTOR_SIZE + 0x48, 4)
    dec = decode_sectors(bytes(raw2), include_empty=True)
    check("SECTOR_STUFF 256-slot field encoder exact",
          encode_sectors(dec) == bytes(raw2))

    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


# --- containers --------------------------------------------------------------


def _level_tables(game, level):
    """(sob_count, sample_count, sob names) of the source level, or None."""
    if not level:
        return None
    path = os.path.join(game, level)
    if not os.path.isfile(path):
        return None
    f = sbtmod.parse_file(path)
    sob = f.records_of(20)
    smp = f.records_of(12)
    return (
        len(sob[0].payload.entries) if sob else None,
        len(smp[0].payload.counts) if smp else None,
        [e.name for e in sob[0].payload.entries] if sob else [],
    )


def _decode_payload(type_, payload, tables, all_slots):
    if type_ == 8:
        return "SECTOR_STUFF", decode_sectors(
            payload, tables[1] if tables else None, all_slots)
    if type_ == 19:
        return "ITEM", decode_items(payload, tables[0] if tables else None,
                                    all_slots)
    if type_ == 18:
        return "LIFT", decode_lifts(payload)
    raise ValueError("unsupported record type %d" % type_)


def scan_sbt(path, types, all_slots, raw):
    f = sbtmod.parse_file(path)
    base = os.path.basename(path)
    tables = None
    if any(f.records_of(t) for t in (8, 19)):
        tables = _level_tables(os.path.dirname(path), base)
    recs = []
    for r in f.records:
        if r.type not in types:
            continue
        name, decoded = _decode_payload(r.type, r.raw, tables, all_slots)
        item = {"type": r.type, "name": name, "offset": r.offset,
                "size": r.size, "payload_size": len(r.raw), "decoded": decoded}
        recs.append(item)
    return {"file": os.path.abspath(path), "kind": "level", "records": recs,
            "record_types": [r.type for r in f.records]}


def scan_save(path, types, all_slots, raw, game):
    import savefile  # tools/re

    data = open(path, "rb").read()
    info = savefile.inspect(path, game)
    tail_abs, tail_len = info["state_tail"]
    level = info.get("source_level")
    tables = _level_tables(game, level)
    chain = {b["id"]: b for b in info.get("chain", [])}
    recs = []
    for type_ in sorted(types):
        block = chain.get(type_)
        if block is None or block.get("size") is None:
            continue
        abs_off = tail_abs + block["offset"]
        payload = data[abs_off + 1:abs_off + block["size"]]
        name, decoded = _decode_payload(type_, payload, tables, all_slots)
        if type_ == 8:
            decoded["roundtrip"] = encode_sectors(decoded) == payload
        recs.append({"type": type_, "name": name, "offset": abs_off,
                     "size": block["size"], "payload_size": len(payload),
                     "decoded": decoded})
    return {"file": os.path.abspath(path), "kind": "save",
            "level": level, "save_name": info.get("level_name"),
            "chain": [{k: b.get(k) for k in ("id", "offset", "size")}
                      for b in info.get("chain", [])],
            "records": recs}


def is_save(path, game=DEFAULT_GAME):
    """A save is either the classic ``A SAVED GAME`` container or a
    ``savedN.sbt`` whose header got rewritten (saved1 starts with 0x32);
    ``savefile.state_start`` recognises both."""
    with open(path, "rb") as f:
        head = f.read(0x100)
    if head.startswith(b"A SAVED GAME"):
        return True
    if not head.startswith(b"BINARY SAVE"):
        import savefile  # tools/re
        try:
            return savefile.state_start(open(path, "rb").read(),
                                        game) is not None
        except Exception:
            return False
    return False


def discover(game, patterns):
    if patterns:
        out = []
        for p in patterns:
            out.extend(sorted(glob.glob(p)))
        return out
    out = sorted(glob.glob(os.path.join(game, "*.sbt")))
    out += sorted(glob.glob(os.path.join(game, "SAVEGAME", "saved*.sbt")))
    return out


def print_fields():
    for title, fields in (("ITEM (19)", ITEM_FIELDS),
                          ("SECTOR_STUFF (8)", SECTOR_FIELDS),
                          ("LIFT (18)", LIFT_FIELDS)):
        print(title)
        for off, name, kind, prov, note in fields:
            print("  +0x%02x  %-14s %-4s %-11s %s"
                  % (off, name, kind, prov, note))
        print()


def print_check(docs, types):
    """Human-readable per-type N/N table plus per-file failures."""
    print("%-14s %-6s %8s %8s  %s" % ("type", "mode", "rebuilt", "total", "status"))
    rows = {}
    for doc in docs:
        for rec in doc.get("records", []):
            if "type" not in rec:
                continue
            key = rec["type"]
            row = rows.setdefault(key, {"mode": rec.get("mode", "?"),
                                        "total": 0, "ok": 0, "files": []})
            row["total"] += 1
            if rec["rebuilt"]:
                row["ok"] += 1
            else:
                row["files"].append((os.path.basename(doc["file"]),
                                     rec.get("error")))
    for key in sorted(rows):
        row = rows[key]
        status = "OK" if row["ok"] == row["total"] else "MISMATCH"
        print("%-14s %-6s %8d %8d  %s"
              % (sbtmod.TYPE_NAMES.get(key, str(key)), row["mode"],
                 row["ok"], row["total"], status))
    for doc in docs:
        if doc.get("error"):
            print("%s: ERROR %s" % (os.path.basename(doc["file"]), doc["error"]))
        elif doc.get("kind") == "save" and not doc.get("whole_file"):
            print("%s: whole-file rebuild MISMATCH" % os.path.basename(doc["file"]))
        for rec in doc.get("records", []):
            if rec.get("error"):
                if "type" in rec:
                    print("  %s type %d @0x%x: %s"
                          % (os.path.basename(doc["file"]), rec["type"],
                             rec.get("offset", 0), rec["error"]))
                else:
                    print("  %s: %s" % (os.path.basename(doc["file"]),
                                        rec["error"]))
    files_ok = sum(1 for d in docs if not d.get("error")
                   and all(r.get("rebuilt") for r in d.get("records", []))
                   and (d.get("kind") != "save" or d.get("whole_file")))
    print("\n%d/%d file(s) byte-exact, %d record(s)"
          % (files_ok, len(docs),
             sum(1 for d in docs for r in d.get("records", []) if "type" in r)))


def run_check(paths, game, rebuild_dir, as_json):
    if rebuild_dir:
        os.makedirs(rebuild_dir, exist_ok=True)
    docs = []
    for path in paths:
        try:
            if is_save(path, game):
                docs.append(check_save(path, game, rebuild_dir))
            else:
                docs.append(check_sbt(path, rebuild_dir))
        except Exception as exc:
            docs.append({"file": os.path.abspath(path), "error": str(exc),
                         "records": []})
    ok = all(not d.get("error")
             and all(r.get("rebuilt") for r in d.get("records", []))
             and (d.get("kind") != "save" or d.get("whole_file"))
             for d in docs)
    if as_json:
        print(json.dumps({"tool": "records", "mode": "check",
                          "ok": ok, "files": docs}, indent=2))
    else:
        print_check(docs, None)
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", help="level .sbt / savedN.sbt / glob")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME))
    ap.add_argument("--type", default="8,18,19",
                    help="comma-separated record types (default 8,18,19)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--all", action="store_true",
                    help="keep empty sectors/items (default: only active)")
    ap.add_argument("--raw", action="store_true", help="include raw hex per slot")
    ap.add_argument("--fields", action="store_true", help="print the field map")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="structurally re-encode every record and compare bytes")
    ap.add_argument("--rebuild", metavar="DIR",
                    help="write structurally rebuilt files into DIR")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic boundary tests and exit")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return self_test()
    if opts.fields:
        print_fields()
        return 0
    paths = discover(opts.game, opts.paths)
    if not paths:
        print("no input files", file=sys.stderr)
        return 2
    if opts.check or opts.rebuild:
        return run_check(paths, opts.game, opts.rebuild, opts.json)
    types = {int(t) for t in opts.type.split(",") if t.strip()}

    out = []
    for path in paths:
        try:
            if is_save(path, opts.game):
                doc = scan_save(path, types, opts.all, opts.raw, opts.game)
            else:
                doc = scan_sbt(path, types, opts.all, opts.raw)
        except Exception as exc:                  # keep going over the corpus
            doc = {"file": os.path.abspath(path), "error": str(exc)}
        out.append(doc)

    if not opts.raw:
        for doc in out:
            for rec in doc.get("records", []):
                dec = rec["decoded"]
                for key in ("items", "sectors", "records"):
                    for row in dec.get(key, []):
                        row.pop("raw", None)

    if opts.json:
        print(json.dumps({"tool": "records", "files": out}, indent=2))
    elif not opts.quiet:
        for doc in out:
            if "error" in doc:
                print("%s: ERROR %s" % (os.path.basename(doc["file"]), doc["error"]))
                continue
            print("%s (%s)%s" % (
                os.path.basename(doc["file"]), doc["kind"],
                ", level " + str(doc.get("level")) if doc.get("kind") == "save" else ""))
            if not doc["records"]:
                print("  no records of type %s" % ",".join(map(str, sorted(types))))
            for rec in doc["records"]:
                dec = rec["decoded"]
                print("  type %d %-14s @0x%x  %d bytes"
                      % (rec["type"], rec["name"], rec["offset"], rec["size"]))
                if rec["type"] == 19:
                    print("    slots %d, active %d"
                          % (dec["slots"], dec["active"]))
                    for it in dec["items"]:
                        print("      [%3d] state %-2d kind %d sob %-3d pos (%g, %g, %g)"
                              % (it["index"], it["state"], it["kind"],
                                 it["sob_index"], *it["position"]))
                elif rec["type"] == 8:
                    print("    slots %d, nonzero %d"
                          % (dec["slots"], dec["nonzero"]))
                    for s in dec["sectors"]:
                        print("      [%3d] move (%g, %g, %g) sound %d x%d @%.3g "
                              "events %d/%d dmg %d/%d flags 0x%x"
                              % (s["index"], *s["move"], s["sound"],
                                 s["sound_count"], s["sound_param"],
                                 s["event_a"], s["event_b"], s["damage_lo"],
                                 s["damage_hi"], s["flags"]))
                else:
                    print("    lifts %d, stops %d"
                          % (dec["lifts"], dec["stops"]))
                    for l in dec["records"]:
                        print("      [%d] kind %d sob %d/%d pos %s stops %d+%d"
                              % (l["index"], l["kind"], l["sob_index"],
                                 l["sob_index2"], tuple(l["position"]),
                                 l["stop_start"], l["stop_count"]))
                for msg in dec.get("checks", []):
                    print("    CHECK %s" % msg)

    failed = [d for d in out if "error" in d
              or any(r["decoded"].get("checks") for r in d.get("records", []))]
    if failed:
        if not opts.json and not opts.quiet:
            print("\n%d file(s) with problems" % len(failed))
        return 1
    if not opts.json and not opts.quiet:
        print("\nOK: %d file(s), %d record(s)"
              % (len(out), sum(len(d.get("records", [])) for d in out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
