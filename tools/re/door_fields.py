#!/usr/bin/env python3
"""Field map for the runtime door/switch/lift tables (M2.3.4).

Three arrays are proven by the shipped ``gundx.exe``:

* doors   -- ``0x8381a0``, stride ``0xB8``, count ``0x5a9450``, max 100
  (writer ``ADD_DOOR`` -> ``0x44e250``, reader/tick ``0x44f2e0``/``0x44f3f0``);
* switches-- ``0x5a4af0``, stride ``0x20``, count ``0x5a945c``, max 512
  (writer ``ADD_SWITCH`` -> ``0x44e5f0``, consumer ``0x44f960``);
* lifts   -- ``0x836de0``, stride ``0xA8``, count ``0x5a9454``, max 30
  (writer ``ADD_LIFT`` -> ``0x44e800``, tick ``0x450180``/``0x4502a0``).

The level ``.sbt`` SCRIPT only contains ``ADD_DOOR``/``ADD_SWITCH``/
``ADD_LIFT``.  A save-like ``.sbt`` may instead carry whole records (types
2/1/18); this tool parses both record layouts directly, read-only.

Usage::

    python3 tools/re/door_fields.py                 # field maps
    python3 tools/re/door_fields.py --fields        # same
    python3 tools/re/door_fields.py --verify        # live .sbt/.sbw checks
    python3 tools/re/door_fields.py --self-test     # synthetic + live checks
    python3 tools/re/door_fields.py --json          # machine-readable maps

Only the standard library is used; the project's ``gm`` parsers are imported
from the repository root for the live checks.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# --- proven table geometry ---------------------------------------------------

DOOR_BASE = 0x8381A0
DOOR_STRIDE = 0xB8
DOOR_COUNT = 0x5A9450
DOOR_MAX = 100

SWITCH_BASE = 0x5A4AF0
SWITCH_STRIDE = 0x20
SWITCH_COUNT = 0x5A945C
SWITCH_MAX = 512

LIFT_BASE = 0x836DE0
LIFT_STRIDE = 0xA8
LIFT_COUNT = 0x5A9454
LIFT_MAX = 30
LIFT_STOPS_COUNT = 0x5A9458
LIFT_STOP_MAX = 200
LIFT_STOP_COLUMNS = (0x5A9130, 0x5A8E10, 0x5A8AF0)  # floor id, height, sound

#: Expected shipped-corpus aggregates (36 levels, 1779 doors).
EXPECT = {
    "doors": 1779,
    "switches": 3964,
    "lifts": 45,
    "lift_stops": 151,
    "bound": 1779,
    "distinct": 1779,
    "sliding": 1534,
    "linked": 200,
}

#: Per-level spot checks (name -> doors, switches, lifts, bound).
LEVELS = {
    "dm1": (61, 129, 0, 61),
    "level0": (25, 24, 0, 25),
}

# --- field maps --------------------------------------------------------------
# (offset, size, kind, name, cpp, tested, status, evidence)
# status: proven | hypothesis | unknown

DOOR_FIELDS = (
    (0x00, 0x18, "3 x f64", "closed corner (D1..D3)", "Door.closed[3]", "doors",
     "proven", "ADD_DOOR write 0x44e31c..0x44e342; fold +size 0x44e368"),
    (0x18, 0x18, "3 x f64", "open corner (D4..D6)", "Door.open[3]", "doors",
     "proven", "write 0x44e342..0x44e362; fold 0x44e3c6..0x44e3f6"),
    (0x30, 0x18, "3 x f64", "size (D7..D9)", "Door.size[3]", "doors",
     "proven", "write 0x44e356..0x44e362; read as box size 0x44dfc0"),
    (0x48, 0x18, "3 x f64", "box delta / 65536", "not modelled", "-",
     "proven", "write 0x44e40c..0x44e41a; read 0x44e014/0x44e03b/0x44e09a"),
    (0x60, 0x08, "f64", "constant 0.03 (sound level)", "not modelled", "-",
     "hypothesis", "write 0x44e460/0x44e46a (0x3f9eb851eb851eb8); pushed as qword 0x44f742/45 into 0x46b8d0"),
    (0x68, 0x04, "i32", "flags; low nibble kind, 0x40 usable", "Door.flags/Door.kind",
     "doors", "proven", "write 0x44e289; mask 0x44f6b6, 0x45068d, 0x4507c9"),
    (0x6C, 0x14, "5 x i32", "ids[0..4] (E1..E5)", "Door.ids[5]",
     "doors", "proven", "write 0x44e297..0x44e2cb; compare 0x44f6c3/6ee/706"),
    (0x80, 0x04, "i32", "state -1/0/+1", "Door.state", "doors",
     "proven", "write 0x44f6e0/6e5/701/710; read 0x44f31b, 0x44f41c"),
    (0x84, 0x04, "i32", "sector index", "Door.sector", "doors/doors_full",
     "proven", "write 0x44e2de; read 0x44f72c; resolve 0x434419"),
    (0x88, 0x04, "i32", "object index", "Door.object", "doors/doors_full",
     "proven", "write 0x44e2e8; read 0x44f72f; resolve 0x434419"),
    (0x8C, 0x04, "u32", "phase before tick", "Door.phase", "doors",
     "proven", "write 0x44e2fc; read 0x44f319; compare 0x44f6d7; commit 0x44f461"),
    (0x90, 0x04, "u32", "phase after tick", "not exposed (game current phase)",
     "-", "proven", "write 0x44f335, clamp 0x44f33b/0x44f349; read 0x44f461"),
    (0x94, 0x04, "i32", "phase step per tick", "Door.step", "doors",
     "proven", "write 0x44e2f2; read 0x44f326"),
    (0x98, 0x04, "u32", "open dwell (L6)", "not modelled", "-",
     "proven", "copy +0x98 -> +0x9c 0x44f54d/0x44f550; source L6"),
    (0x9C, 0x04, "i32", "auto-close countdown (L7)", "not modelled", "-",
     "proven", "reset 0x44f725; countdown 0x44f5ff; timeout -> state=-1"),
    (0xA0, 0x04, "i32", "v0: sound volume", "not modelled", "-",
     "hypothesis", "pushed last in sound call 0x44f763 (volume)"),
    (0xA4, 0x04, "i32", "v1", "not modelled", "-", "unknown", "no reader found"),
    (0xA8, 0x04, "i32", "v2: sound sample", "not modelled", "-",
     "hypothesis", "passed to 0x46b8d0 at 0x44f52b"),
    (0xAC, 0x04, "i32", "n0: sound sample", "not modelled", "-",
     "hypothesis", "first arg of 0x46b8d0 at 0x44f763"),
    (0xB0, 0x04, "i32", "n1", "not modelled", "-", "unknown", "no reader found"),
    (0xB4, 0x04, "i32", "n2: sound parameter", "not modelled", "-",
     "hypothesis", "passed to 0x46b8d0 at 0x44f52b"),
)

SWITCH_FIELDS = (
    (0x00, 0x04, "i32", "kind", "Switch.kind", "doors",
     "proven", "0x44f960 (<0x101 / 0x101), 0x44fc30 (0/1/0x101), 0x44fbd0 (0x104)"),
    (0x04, 0x04, "i32", "target / GENERATE id", "Switch.target", "doors",
     "proven", "0x44f960 -> generate_event 0x479960; 0x44fbd0 match"),
    (0x08, 0x04, "i32", "radius", "Switch.radius", "doors",
     "proven", "distance compare 0x44fc30 (kind 0/1), 0x44fb9e (0x102..0x104)"),
    (0x0C, 0x0C, "3 x i32", "position x/y/z", "Switch.pos[3]", "doors",
     "proven", "source context 0x44f960; distance 0x44fc30"),
    (0x18, 0x04, "i32", "aux (sound volume)", "not modelled", "-",
     "hypothesis", "2nd arg of 0x46b840 in 0x44f960"),
    (0x1C, 0x04, "i32", "sound sample, -1 = silent", "not modelled", "-",
     "proven", "tested -1 and passed to 0x46b840/0x46b8d0 in 0x44f960"),
)

LIFT_FIELDS = (
    (0x00, 0x30, "6 x f64", "basis / transform", "not modelled", "-",
     "hypothesis", "write 0x44e8a5..0x44e901; read 0x44e6f0"),
    (0x30, 0x04, "u32", "runtime object handle", "not modelled", "-",
     "proven", "write 0x44eb11; read 0x44e6f0, 0x4501ad, 0x44f85f"),
    (0x34, 0x04, "i32", "kind (tick requires 1)", "Lift.kind", "doors",
     "proven", "write 0x44e855; checks 0x44f7b6, 0x45019b, 0x4502ae"),
    (0x38, 0x04, "i32", "SOB model index", "Lift.id (misused as id)", "doors",
     "proven", "ADD_LIFT param 2 -> 0x44e85f; resolved 0x44e9d4/0x47c990"),
    (0x3C, 0x0C, "3 x i32", "base x/y/z", "Lift.x/y/z", "doors",
     "proven", "write 0x44e869..0x44e885; read 0x44e6f0, lift tick"),
    (0x48, 0x0C, "3 x i32", "basis deltas", "not modelled", "-",
     "proven", "write 0x44e891..0x44e89b; read 0x44e6f0"),
    (0x54, 0x04, "i32", "speed, height units/tick", "Lift.speed", "doors",
     "proven", "write 0x44e907; IMUL 0x4501c5"),
    (0x58, 0x04, "i32", "dwell at stop (ticks)", "not modelled", "-",
     "proven", "write 0x44e911; reload timer 0x4502a0 (piVar5[1]=piVar5[-2])"),
    (0x5C, 0x04, "i32", "committed height", "Lift.height", "doors",
     "proven", "write 0x44e91b; read 0x4501c2, 0x44f8ca, 0x4502a0"),
    (0x60, 0x04, "i32", "per-tick/display height", "Lift.target (misnamed)", "doors",
     "proven", "write 0x4501ce, clamp 0x4501ff; read 0x4502b4"),
    (0x64, 0x04, "i32", "wait timer at stop", "not modelled", "-",
     "proven", "reset 0x44f8bd; countdown/arm 0x4502a0"),
    (0x68, 0x04, "i32", "direction -1/0/+1", "Lift.moving (bool)", "doors",
     "proven", "write 0x44f8d8/+1, 0x44f8dd/-1, 0x44f855/0; read 0x4501a1"),
    (0x6C, 0x04, "i32", "stop index", "Lift.stop_index", "doors",
     "proven", "write 0x44f7d0/7d9/7f4/808; read 0x4501d7"),
    (0x70, 0x0C, "3 x i32", "call ids", "Lift.call_ids[3]", "doors",
     "proven", "compare 0x44f7e0, 0x44f7f7, 0x44f70b; index 0x44f88c"),
    (0x7C, 0x04, "i32", "call event id", "not modelled", "-",
     "proven", "event_post(piVar6[-1]) in 0x44fc30 (lift proximity)"),
    (0x80, 0x04, "i32", "call trigger radius", "not modelled", "-",
     "proven", "distance compare in 0x44fc30"),
    (0x84, 0x04, "i32", "sound/parameter", "not modelled", "-",
     "hypothesis", "passed as sound arg 0x44f8e4..0x44f92b"),
    (0x88, 0x04, "i32", "sound/parameter", "not modelled", "-",
     "hypothesis", "passed as sound arg in 0x4502a0"),
    (0x94, 0x04, "i32", "sound/parameter", "not modelled", "-",
     "hypothesis", "passed as sound arg in 0x4502a0"),
    (0x98, 0x04, "i32", "sound/parameter", "not modelled", "-",
     "hypothesis", "passed as sound arg in 0x4502a0"),
    (0x9C, 0x04, "i32", "stop count", "Lift.stops.size()", "doors",
     "proven", "increment 0x44ebc3; loop bound 0x44f7d3, 0x44f80f"),
    (0xA0, 0x04, "i32", "first global stop index", "not modelled", "-",
     "proven", "write 0x44e9c4; used 0x44f88c, 0x4501e? "),
    (0xA4, 0x04, "i32", "unknown/reserved", "not modelled", "-",
     "unknown", "no reader/writer in the analysed build"),
)

#: ``ADD_LIFT`` script block is 37 dwords; ``0x45c9a0`` forwards head[i] as
#: ``0x44e800`` parameter i+1.  Only fields used by the C++ layer are listed.
LIFT_HEAD_FIELDS = (
    (0, "kind", "Lift.kind", "proven", "param 1 -> +0x34 at 0x44e855"),
    (1, "SOB model index", "Lift.id (wrong: not an entity id)", "proven",
     "param 2 -> +0x38 at 0x44e85f; checked/resolved 0x44e9d4"),
    (2, "x", "Lift.x", "proven", "param 3 -> +0x3c at 0x44e869"),
    (3, "y", "Lift.y", "proven", "param 4 -> +0x40 at 0x44e873"),
    (4, "z", "Lift.z", "proven", "param 5 -> +0x44 at 0x44e87d"),
    (16, "speed", "Lift.speed (not parsed from script)", "proven",
     "param 17 -> +0x54 at 0x44e907"),
    (17, "dwell ticks", "not modelled", "proven",
     "param 18 -> +0x58 at 0x44e911"),
    (18, "initial height", "Lift.height (currently head[7]!)", "proven",
     "param 19 -> +0x5c at 0x44e91b"),
    (20, "direction", "not modelled", "hypothesis", "param 21 -> +0x68"),
    (21, "stop index", "Lift.stop_index (script value)", "proven",
     "param 22 -> +0x6c at 0x44e9b2"),
    (22, "call id 1", "Lift.call_ids[0]", "proven", "param 23 -> +0x70"),
    (23, "call id 2", "Lift.call_ids[1]", "proven", "param 24 -> +0x74"),
    (24, "call id 3", "Lift.call_ids[2]", "proven", "param 25 -> +0x78"),
)


def _need(payload, offset, need, what):
    if offset < 0 or need < 0 or offset + need > len(payload):
        raise ValueError("%s: truncated at +0x%x (need 0x%x, have 0x%x)"
                         % (what, offset, need, max(0, len(payload) - offset)))


def parse_door(payload, offset=0):
    """Decode one 0xB8 door record (``0x8381a0`` layout)."""
    _need(payload, offset, DOOR_STRIDE, "door")
    box = struct.unpack_from("<9d", payload, offset)
    delta = struct.unpack_from("<3d", payload, offset + 0x48)
    const = struct.unpack_from("<d", payload, offset + 0x60)[0]
    flags = struct.unpack_from("<i", payload, offset + 0x68)[0]
    ids = struct.unpack_from("<5i", payload, offset + 0x6C)
    state, sector, obj = struct.unpack_from("<3i", payload, offset + 0x80)
    phase_prev, phase_next, step = struct.unpack_from("<III", payload, offset + 0x8C)
    dwell, timer = struct.unpack_from("<Ii", payload, offset + 0x98)
    tail = struct.unpack_from("<6i", payload, offset + 0xA0)
    return {
        "closed": list(box[0:3]), "open": list(box[3:6]), "size": list(box[6:9]),
        "delta": list(delta), "const": const,
        "flags": flags, "kind": flags & 0xF, "usable": bool(flags & 0x40),
        "ids": list(ids), "state": state,
        "sector": sector, "object": obj,
        "phase_prev": phase_prev, "phase_next": phase_next, "step": step,
        "dwell": dwell, "timer": timer,
        "v": list(tail[0:3]), "n": list(tail[3:6]),
        "sliding": any(abs(box[i] - box[i + 3]) > 1e-6 for i in range(3)),
        "linked": ids[3] != 0,
    }


def parse_switch(payload, offset=0):
    """Decode one 0x20 switch record (``0x5a4af0`` layout)."""
    _need(payload, offset, SWITCH_STRIDE, "switch")
    kind, target, radius, x, y, z, aux, sound = struct.unpack_from(
        "<8i", payload, offset)
    return {"kind": kind, "target": target, "radius": radius,
            "pos": [x, y, z], "aux": aux, "sound": sound}


def parse_lift(payload, offset=0):
    """Decode one 0xA8 lift record (``0x836de0`` layout)."""
    _need(payload, offset, LIFT_STRIDE, "lift")
    basis = struct.unpack_from("<6d", payload, offset)
    handle, kind, sob = struct.unpack_from("<3I", payload, offset + 0x30)
    x, y, z, dx, dy, dz = struct.unpack_from("<6i", payload, offset + 0x3C)
    speed, dwell, height, target, timer, direction, stop_index = \
        struct.unpack_from("<7i", payload, offset + 0x54)
    calls = struct.unpack_from("<3i", payload, offset + 0x70)
    call_event, call_radius = struct.unpack_from("<2i", payload, offset + 0x7C)
    stop_count, stop_start = struct.unpack_from("<2i", payload, offset + 0x9C)
    return {
        "basis": list(basis), "handle": handle, "kind": kind, "sob": sob,
        "x": x, "y": y, "z": z, "basis_delta": [dx, dy, dz],
        "speed": speed, "dwell": dwell, "height": height, "target": target,
        "timer": timer, "direction": direction, "stop_index": stop_index,
        "call_ids": list(calls), "call_event": call_event,
        "call_radius": call_radius, "stop_count": stop_count,
        "stop_start": stop_start,
    }


def parse_lift_record(payload):
    """Decode a type-18 payload: ``n`` records + ``k`` stop columns."""
    if len(payload) < 4:
        raise ValueError("lift record: missing count")
    n = struct.unpack_from("<I", payload, 0)[0]
    if n > LIFT_MAX:
        raise ValueError("lift record: n=%d > %d" % (n, LIFT_MAX))
    need = 4 + n * LIFT_STRIDE + 4
    _need(payload, 0, need, "lift record")
    lifts = [parse_lift(payload, 4 + i * LIFT_STRIDE) for i in range(n)]
    k = struct.unpack_from("<I", payload, 4 + n * LIFT_STRIDE)[0]
    if k > LIFT_STOP_MAX:
        raise ValueError("lift record: k=%d > %d" % (k, LIFT_STOP_MAX))
    stop_off = 8 + n * LIFT_STRIDE
    _need(payload, stop_off, k * 12, "lift stops")
    cols = [struct.unpack_from("<%dI" % k, payload, stop_off + c * k * 4)
            for c in range(3)]
    for lift in lifts:
        if lift["stop_start"] + lift["stop_count"] > k:
            raise ValueError("lift %d: stops outside columns" % lift["stop_count"])
        lift["stops"] = [
            {"floor_id": cols[0][i], "height": cols[1][i], "sound": cols[2][i]}
            for i in range(lift["stop_start"],
                           lift["stop_start"] + lift["stop_count"])
        ]
    return {"lifts": lifts, "stops": k}


def script_head_map(head):
    """Map the 37 dwords of an ``ADD_LIFT`` block to runtime fields."""
    if len(head) < 25:
        raise ValueError("ADD_LIFT head: need 25 dwords, have %d" % len(head))
    return {
        "kind": head[0], "sob": head[1],
        "x": head[2], "y": head[3], "z": head[4],
        "speed": head[16], "dwell": head[17], "height": head[18],
        "direction": head[20], "stop_index": head[21],
        "call_ids": [head[22], head[23], head[24]],
    }


# --- presentation ------------------------------------------------------------

def _print_table(title, rows, headers):
    print("\n%s" % title)
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    print("  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)))
    print("  ".join("-" * w for w in widths))
    for row in rows:
        print("  ".join(str(c).ljust(widths[i]) for i, c in enumerate(row)))


def print_maps(as_json=False):
    if as_json:
        print(json.dumps({
            "geometry": {
                "door": [hex(DOOR_BASE), DOOR_STRIDE, hex(DOOR_COUNT), DOOR_MAX],
                "switch": [hex(SWITCH_BASE), SWITCH_STRIDE, hex(SWITCH_COUNT),
                           SWITCH_MAX],
                "lift": [hex(LIFT_BASE), LIFT_STRIDE, hex(LIFT_COUNT), LIFT_MAX],
                "lift_stops": [hex(c) for c in LIFT_STOP_COLUMNS],
            },
            "door": DOOR_FIELDS, "switch": SWITCH_FIELDS, "lift": LIFT_FIELDS,
            "lift_head": LIFT_HEAD_FIELDS,
        }, indent=2))
        return
    print("Runtime tables (proven by gundx.exe):")
    print("  doors    0x%06x  stride 0x%02x  count 0x%06x  max %d"
          % (DOOR_BASE, DOOR_STRIDE, DOOR_COUNT, DOOR_MAX))
    print("  switches 0x%06x  stride 0x%02x  count 0x%06x  max %d"
          % (SWITCH_BASE, SWITCH_STRIDE, SWITCH_COUNT, SWITCH_MAX))
    print("  lifts    0x%06x  stride 0x%02x  count 0x%06x  max %d"
          % (LIFT_BASE, LIFT_STRIDE, LIFT_COUNT, LIFT_MAX))
    print("  stops    0x%06x 0x%06x 0x%06x  count 0x%06x  max %d"
          % (LIFT_STOP_COLUMNS + (LIFT_STOPS_COUNT, LIFT_STOP_MAX)))
    for title, rows in (("DOOR record 0xB8", DOOR_FIELDS),
                        ("SWITCH record 0x20", SWITCH_FIELDS),
                        ("LIFT record 0xA8", LIFT_FIELDS)):
        _print_table(title, [(("+0x%02x" % r[0]), r[2], r[3], r[4], r[5],
                              r[6], r[7]) for r in rows],
                     ("off", "type", "meaning", "C++", "test", "status",
                      "evidence"))
    _print_table("ADD_LIFT script head (37 dwords)",
                 [(("head[%d]" % r[0]), r[1], r[2], r[3], r[4])
                  for r in LIFT_HEAD_FIELDS],
                 ("index", "meaning", "C++", "status", "evidence"))


# --- live corpus -------------------------------------------------------------

def _load_scene(stem, game):
    from gm import sbt as sbtmod
    from gm import scene as scenemod
    from gm import sbw as sbwmod

    sbt = sbtmod.parse_file(os.path.join(game, stem + ".sbt"))
    script = sbt.records_of(11)
    if not script:
        raise ValueError("%s: no SCRIPT record" % stem)
    scene = scenemod.extract(script[0].payload)
    world = sbwmod.parse_file(os.path.join(game, stem + ".sbw"))
    record_types = collections.Counter(r.type for r in sbt.records)
    return scene, world, record_types


def analyse(game=DEFAULT_GAME, only=None):
    """Read-only scan.  Returns (totals, per-level dict, errors)."""
    paths = sorted(glob.glob(os.path.join(game, "*.sbt")))
    if only:
        paths = [p for p in paths
                 if os.path.splitext(os.path.basename(p))[0] in only]
    totals = collections.Counter()
    door_kinds = collections.Counter()
    switch_kinds = collections.Counter()
    dwell = collections.Counter()
    steps = collections.Counter()
    levels = {}
    errors = []
    for path in paths:
        stem = os.path.splitext(os.path.basename(path))[0]
        try:
            scene, world, rtypes = _load_scene(stem, game)
        except Exception as exc:                     # noqa: BLE001 - report all
            errors.append("%s: %s" % (stem, exc))
            continue
        bound = distinct = 0
        ok_pairs = set()
        for door in scene.doors:
            totals["doors"] += 1
            door_kinds[door.flags & 0xF] += 1
            dwell[door.params[5]] += 1
            steps[door.params[2]] += 1
            if len(door.ids) > 3 and door.ids[3]:
                totals["linked"] += 1
            if door.moving:
                totals["sliding"] += 1
            if (0 <= door.sector < len(world.sectors)
                    and 0 <= door.object < len(world.sectors[door.sector].objects)):
                bound += 1
                ok_pairs.add((door.sector, door.object))
        distinct = len(ok_pairs)
        totals["bound"] += bound
        totals["distinct"] += distinct
        for sw in scene.switches:
            totals["switches"] += 1
            switch_kinds[sw.kind] += 1
        for lift in scene.lifts:
            totals["lifts"] += 1
            totals["lift_stops"] += len(lift.stops)
            if len(lift.head) >= 37:
                head = script_head_map(list(struct.unpack("<37i", lift.head)))
                if head["speed"] > 0:
                    totals["script_lifts_with_speed"] += 1
        totals["record_lift_18"] += rtypes.get(18, 0)
        levels[stem] = {
            "doors": len(scene.doors), "switches": len(scene.switches),
            "lifts": len(scene.lifts), "bound": bound, "distinct": distinct,
            "sliding": sum(1 for d in scene.doors if d.moving),
            "linked": sum(1 for d in scene.doors
                          if len(d.ids) > 3 and d.ids[3]),
            "door_kinds": dict(collections.Counter(d.flags & 0xF
                                                   for d in scene.doors)),
            "switch_kinds": dict(collections.Counter(s.kind
                                                     for s in scene.switches)),
            "dwell": dict(collections.Counter(d.params[5]
                                              for d in scene.doors)),
            "records": dict(rtypes),
        }
    return {
        "totals": totals, "levels": levels, "errors": errors,
        "door_kinds": dict(door_kinds), "switch_kinds": dict(switch_kinds),
        "dwell": dict(dwell), "steps": dict(steps),
    }


def verify(game=DEFAULT_GAME, only=None, quiet=False):
    """Check live levels against proven geometry and corpus aggregates."""
    if not os.path.isdir(game):
        print("verify: game directory not found: %s (skipped)" % game)
        return 0
    data = analyse(game, only)
    totals = data["totals"]
    bad = list(data["errors"])
    if only is None:
        for key, want in EXPECT.items():
            got = totals.get(key, 0)
            if got != want:
                bad.append("%s: %d != %d" % (key, got, want))
        if totals.get("record_lift_18", 0) != 0:
            bad.append("shipped levels contain LIFT record 18: %d"
                       % totals["record_lift_18"])
    for stem, want in LEVELS.items():
        if only is not None and stem not in only:
            continue
        row = data["levels"].get(stem)
        if row is None:
            if only is None:
                bad.append("%s: missing" % stem)
            continue
        got = (row["doors"], row["switches"], row["lifts"], row["bound"])
        if got != want:
            bad.append("%s: (doors,switches,lifts,bound)=%r != %r"
                       % (stem, got, want))
    if not quiet:
        print("verify: %d level(s), %d .sbt" % (len(data["levels"]),
                                                len(data["levels"])))
        if only is None:
            print("  doors=%d switches=%d lifts=%d stops=%d"
                  % (totals["doors"], totals["switches"], totals["lifts"],
                     totals["lift_stops"]))
            print("  bound=%d distinct=%d sliding=%d linked=%d"
                  % (totals["bound"], totals["distinct"], totals["sliding"],
                     totals["linked"]))
            print("  door kinds=%s" % dict(sorted(data["door_kinds"].items())))
            print("  switch kinds=%s"
                  % dict(sorted(data["switch_kinds"].items())))
            nz = {k: v for k, v in sorted(data["dwell"].items()) if k}
            print("  dwell (L6) nonzero=%s" % nz)
            print("  steps (top)=%s"
                  % collections.Counter(data["steps"]).most_common(8))
        for stem in sorted(data["levels"]):
            row = data["levels"][stem]
            print("  %-8s doors=%-4d switches=%-4d lifts=%-2d bound=%-4d "
                  "kinds=%s sw=%s"
                  % (stem, row["doors"], row["switches"], row["lifts"],
                     row["bound"], row["door_kinds"], row["switch_kinds"]))
    if bad:
        print("verify: FAIL")
        for item in bad:
            print("  - %s" % item)
        return 1
    print("verify: OK")
    return 0


# --- self test ---------------------------------------------------------------

def _synth_door():
    buf = bytearray(DOOR_STRIDE)
    struct.pack_into("<9d", buf, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9)
    struct.pack_into("<3d", buf, 0x48, 0.5, 0.25, 0.125)
    struct.pack_into("<d", buf, 0x60, 0.03)
    struct.pack_into("<i", buf, 0x68, 0x41)
    struct.pack_into("<5i", buf, 0x6C, 11, 22, 33, 44, 0)
    struct.pack_into("<3i", buf, 0x80, 1, 7, 9)
    struct.pack_into("<III", buf, 0x8C, 1000, 2000, 1500)
    struct.pack_into("<iI", buf, 0x98, 300, 250)
    struct.pack_into("<6i", buf, 0xA0, 1, 2, 3, 4, 5, 6)
    return bytes(buf), {
        "flags": 0x41, "kind": 1, "usable": True,
        "ids": [11, 22, 33, 44, 0], "state": 1, "sector": 7, "object": 9,
        "phase_prev": 1000, "phase_next": 2000, "step": 1500,
        "dwell": 300, "timer": 250, "linked": True, "sliding": True,
    }


def _synth_switch():
    buf = bytearray(SWITCH_STRIDE)
    struct.pack_into("<8i", buf, 0, 0x101, -30231, 128, 10, -20, 30, 19, 126)
    return bytes(buf), {"kind": 0x101, "target": -30231, "radius": 128,
                        "pos": [10, -20, 30], "aux": 19, "sound": 126}


def _synth_lift():
    buf = bytearray(LIFT_STRIDE)
    struct.pack_into("<6d", buf, 0x00, *[float(i) for i in range(6)])
    struct.pack_into("<3I", buf, 0x30, 0xDEADBEEF, 1, 41)
    struct.pack_into("<6i", buf, 0x3C, 100, 200, 300, 1, 2, 3)
    struct.pack_into("<7i", buf, 0x54, 10, 12, 50, 60, 3, 1, 1)
    struct.pack_into("<3i", buf, 0x70, 7001, 7002, 7003)
    struct.pack_into("<2i", buf, 0x7C, 7004, 400)
    struct.pack_into("<2i", buf, 0x9C, 2, 0)
    return bytes(buf), {"kind": 1, "sob": 41, "x": 100, "y": 200, "z": 300,
                        "speed": 10, "dwell": 12, "height": 50, "target": 60,
                        "timer": 3, "direction": 1, "stop_index": 1,
                        "call_ids": [7001, 7002, 7003], "stop_count": 2,
                        "stop_start": 0}


def _synth_lift_record():
    body = bytearray(4)
    struct.pack_into("<I", body, 0, 1)
    body += bytearray(LIFT_STRIDE)
    struct.pack_into("<3I", body, 4 + 0x30, 0, 1, 41)
    struct.pack_into("<7i", body, 4 + 0x54, 10, 12, 50, 60, 3, 1, 0)
    struct.pack_into("<2i", body, 4 + 0x9C, 2, 0)
    body += struct.pack("<I", 3)
    body += struct.pack("<3I", 9001, 9002, 0)      # floor ids
    body += struct.pack("<3I", 10, 60, 0)          # heights
    body += struct.pack("<3I", 1, 2, 0)            # sounds
    return bytes(body)


def self_test(game=DEFAULT_GAME):
    failures = []

    def check(name, cond, detail=""):
        print("  %-42s %s%s" % (name, "ok" if cond else "FAIL",
                                "" if cond else "  " + detail))
        if not cond:
            failures.append(name)

    print("self-test: synthetic records")
    raw, want = _synth_door()
    got = parse_door(raw)
    check("door all fields", all(got[k] == v for k, v in want.items()),
          repr({k: (got.get(k), v) for k, v in want.items()
                if got.get(k) != v}))
    check("door flags/kind/usable", (got["flags"], got["kind"], got["usable"])
          == (0x41, 1, True))
    check("door geometry", got["closed"] == [1, 2, 3]
          and got["open"] == [4, 5, 6] and got["size"] == [7, 8, 9])
    try:
        parse_door(raw[:-1])
        check("door truncation rejected", False)
    except ValueError:
        check("door truncation rejected", True)

    raw, want = _synth_switch()
    got = parse_switch(raw)
    check("switch all fields", all(got[k] == v for k, v in want.items()),
          repr(got))
    try:
        parse_switch(raw[:-1])
        check("switch truncation rejected", False)
    except ValueError:
        check("switch truncation rejected", True)

    raw, want = _synth_lift()
    got = parse_lift(raw)
    check("lift all fields", all(got[k] == v for k, v in want.items()),
          repr({k: (got.get(k), v) for k, v in want.items()
                if got.get(k) != v}))
    check("lift handle not sob", got["handle"] == 0xDEADBEEF
          and got["sob"] == 41)
    try:
        parse_lift(raw[:-1])
        check("lift truncation rejected", False)
    except ValueError:
        check("lift truncation rejected", True)

    rec = parse_lift_record(_synth_lift_record())
    check("type-18 count/stops", len(rec["lifts"]) == 1
          and rec["stops"] == 3
          and rec["lifts"][0]["stops"] == [
              {"floor_id": 9001, "height": 10, "sound": 1},
              {"floor_id": 9002, "height": 60, "sound": 2}],
          repr(rec))
    head = list(range(37))
    hm = script_head_map(head)
    check("ADD_LIFT speed=head[16]", hm["speed"] == 16)
    check("ADD_LIFT height=head[18] (not head[7])",
          hm["height"] == 18 and head[7] != hm["height"])
    check("ADD_LIFT stops are 0xc",
          struct.calcsize("<3I") == 0xC)

    print("self-test: geometry")
    check("door stride/count", (DOOR_STRIDE, DOOR_COUNT, DOOR_MAX)
          == (0xB8, 0x5A9450, 100))
    check("switch stride/count", (SWITCH_STRIDE, SWITCH_COUNT, SWITCH_MAX)
          == (0x20, 0x5A945C, 512))
    check("lift stride/count", (LIFT_STRIDE, LIFT_COUNT, LIFT_MAX)
          == (0xA8, 0x5A9454, 30))

    print("self-test: live corpus")
    if not os.path.isdir(game):
        print("  skipped (no game directory: %s)" % game)
    else:
        data = analyse(game)
        totals = data["totals"]
        for key, want in EXPECT.items():
            check("%s=%d" % (key, want), totals.get(key, 0) == want,
                  "got %d" % totals.get(key, 0))
        check("no shipped LIFT record 18", totals.get("record_lift_18", 0) == 0)
        check("all doors bound", totals["bound"] == totals["doors"])
        check("all pairs distinct", totals["distinct"] == totals["doors"])
        dm1 = data["levels"].get("dm1", {})
        check("dm1 doors/switches/lifts",
              (dm1.get("doors"), dm1.get("switches"), dm1.get("lifts"))
              == (61, 129, 0), repr(dm1))
        level0 = data["levels"].get("level0", {})
        check("level0 doors/switches/lifts",
              (level0.get("doors"), level0.get("switches"),
               level0.get("lifts")) == (25, 24, 0), repr(level0))
        # L7 is params[6] and is not part of the dwell counter; verify through
        # the raw scene once, for the two spot-check levels.
        from gm import sbt as sbtmod
        from gm import scene as scenemod
        for stem in ("dm1", "level0"):
            sc = scenemod.extract(
                sbtmod.parse_file(os.path.join(game, stem + ".sbt"))
                .records_of(11)[0].payload)
            check("%s L7 all zero" % stem,
                  all(d.params[6] == 0 for d in sc.doors))

    if failures:
        print("self-test: FAIL (%d)" % len(failures))
        return 1
    print("self-test: OK")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="door/switch/lift runtime field map (M2.3.4)")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--fields", action="store_true", help="print field maps")
    ap.add_argument("--verify", action="store_true", help="check live levels")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic + live checks, nonzero on failure")
    ap.add_argument("--level", action="append", metavar="STEM",
                    help="limit --verify/--self-test to a level (repeatable)")
    ap.add_argument("--json", action="store_true", help="JSON field maps")
    args = ap.parse_args(argv)

    if not (args.fields or args.verify or args.self_test):
        args.fields = True
    if args.fields:
        print_maps(args.json)
    if args.verify:
        return verify(args.game, set(args.level) if args.level else None)
    if args.self_test:
        return self_test(args.game)
    return 0


if __name__ == "__main__":
    sys.exit(main())
