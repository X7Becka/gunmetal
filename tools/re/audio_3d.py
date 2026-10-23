#!/usr/bin/env python3
"""3D positional audio of Gunmetal (M7.3): queue, listener, formulas.

The engine has **no** real-time 3D audio: script and level emitters only push
fixed records into a global queue, and once per frame (per view) a single
consumer walks that queue, computes distance / gain / pan and converts them
into the ordinary 2D sound requests.  This tool documents and reproduces that
arithmetic; it needs no Ghidra and only reads the shipped executable/levels.

Chain (all addresses are VA of ``gundx.exe`` v1.36):

* producers -- ``PLAY_SAMPLE_AT_LOCATION`` 0x457d7f and
  ``PLAY_SAMPLE_AT_SOURCE`` 0x457dbb -> ``0x46b840``;
  ``ADD_SOUND_SOURCE`` 0x459c6e -> ``0x46b210`` (columns) ->
  per-frame fill ``0x46b570``; ``SPEAK_SAMPLE`` 0x457f2e -> 0x40bd80 (2D).
* queue -- ``0x62ce18``, stride ``0x2c``, max 256, count ``0x62fa18``.
* consumer -- ``0x46baf0``: distance ``0x4344df``, gain ``0x46bda4``,
  pan ``0x437909`` + ``0x46be21``.
* request -- ``0x40be20`` (one-shot) / ``0x40bec0`` (loop), stride ``0x20`` at
  ``0x4cabd8``; scheduler ``0x40c080``; voice start ``0x40a4b0`` /``0x40a690``
  computes the 5-bit L/R gains.

Usage::

    python3 tools/re/audio_3d.py --self-test
    python3 tools/re/audio_3d.py --fields
    python3 tools/re/audio_3d.py --check
    python3 tools/re/audio_3d.py --check --json
    python3 tools/re/audio_3d.py --calc --call level0:0 --listener 24,-208,48,0
    python3 tools/re/audio_3d.py --calc --source level0:1 --listener 24,-208,48,90
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _p in (HERE, ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"

# --- data addresses ---------------------------------------------------------
SAMPLE_BASE = 0x83D960          # sample record, stride 0x1c, 300
SAMPLE_STRIDE = 0x1C
SAMPLE_COUNT = 300
VOICE_BASE = 0x84E068           # voice slot, stride 0x50, 8 (+1 music)
VOICE_STRIDE = 0x50
VOICE_COUNT = 8
REQ_BASE = 0x4CABD8             # request, stride 0x20
REQ_STRIDE = 0x20
REQ_MAX = 0x200
REQ_COUNT = 0x4CA71C
QUEUE_BASE = 0x62CE18           # 3D queue, stride 0x2c
QUEUE_STRIDE = 0x2C
QUEUE_COUNT = 0x62FA18
SOURCE_COUNT = 0x800740         # live SOUND_SOURCES count
SRC_X = 0x800340
SRC_Y = 0x800540
SRC_Z = 0x800140
SRC_SAMPLE = 0x7FFF40
SRC_VOLUME = 0x7FFD40
SRC_ATTEN = 0x800760            # f64 column

# --- constants (floats/doubles in .rdata, read by the formulas) -------------
K_MIN_DIST = 0x4A2628           # 1.0
K_PAN_BIAS = 0x4A2638           # -1.0
K_PAN_SCALE = 0x4A2640          # 128.0
K_LEN_K = 0x4A2248              # 0.25  (distance approximation)
K_PAN_REQ = 0x4A68BC            # 32    (request pan -> volume scale)
K_OUT_RATE = 0x4A6810           # 32000 output rate

#: script/level attenuation for the 0x46b840 / 0x46b960 producers
PRODUCER_ATTEN = 0.03           # 0x46b8b7/0x46b8be = 0x3f9eb851eb851eb8

#: layout of the queue entry produced by 0x46b840 / 0x46b960 / 0x46b570
QUEUE_FIELDS = [
    (0x00, "f64", "atten", "затухание на единицу расстояния",
     "W 0x46b8ad/0x46b8b7 (0.03), 0x46b945/0x46b94b (явное), "
     "0x46b5e4; R 0x46bda4"),
    (0x08, "i32", "x", "позиция источника (мировые единицы, int)",
     "W 0x46b860, 0x46b5e8; R 0x46bd63"),
    (0x0C, "i32", "y", "", "W 0x46b86a, 0x46b5f2; R 0x46bd4f"),
    (0x10, "i32", "z", "", "W 0x46b874, 0x46b5f6; R 0x46bd52"),
    (0x14, "u32", "class", "id группы: 0+base у скриптовых, 4000000+i / 10000000+i",
     "W 0x46b87c (0), 0x46b5f9 (0x3d0900+i), 0x46b6bf (0x989680+i); "
     "R 0x46bd36, 0x46be4c"),
    (0x18, "u32", "sample", "индекс записи 0x83d960 (через 0x4ca720)",
     "W 0x46b882, 0x46b609, 0x46b6b9; R → arg1 0x46be69/0x46be82"),
    (0x1C, "u32", "volume", "базовая громкость 0..255 (у скрипта — операнд V)",
     "W 0x46b88d, 0x46b613; R 0x46bd31, 0x46bda7 (= потолок гейна)"),
    (0x20, "u32", "rate", "частота запроса (влияет только на длину SOU0)",
     "W 0x46b893, 0x46b618; R 0x46be53/0x46be74"),
    (0x24, "u32", "priority", "приоритет голоса", "W 0x46b89d, 0x46b61f; "
     "R 0x46be56/0x46be77"),
    (0x28, "u32", "loop", "1 — петля (стоп/перезапуск по class), 0 — one-shot",
     "W 0x46b8a7, 0x46b626, 0x46b9c8; R 0x46bd36"),
]

REQUEST_FIELDS = [
    (0x00, "u32", "queued", "флаг «уже выбран планировщиком»",
     "W 0x40be71, 0x40c0e0; R 0x40c0ab"),
    (0x04, "u32", "class", "0 у one-shot; class у петли (перезапуск)",
     "W 0x40be7b, 0x40bf34; R 0x40c10d"),
    (0x08, "u32", "handle", "0x4ca720[sample]", "W 0x40be85, 0x40bf3a; "
     "R 0x40c3cd"),
    (0x0C, "u32", "volume", "гейн 3D, уже /2 и зажат 1..32",
     "W 0x40be6b, 0x40bf24 (arg2*32>>6); R 0x40c2ab"),
    (0x10, "u32", "pan", "панорама 0..255", "W 0x40be8f, 0x40bf44; R 0x40c2ab"),
    (0x14, "u32", "rate", "аргумент частоты", "W 0x40be99, 0x40bf4e; R 0x40c2ab"),
    (0x18, "u32", "priority", "priority + volume_index (тай-брейк по громкости)",
     "W 0x40bea6, 0x40bf5b; R 0x40c0b2"),
    (0x1C, "u32", "flags", "0x80 — петля", "W 0x40beac, 0x40bf61; R 0x40c26d"),
]

FIELDS = {
    "queue": QUEUE_FIELDS,
    "request": REQUEST_FIELDS,
    "globals": [
        (0x62FA18, "u32", "queue_count", "0..0x100; очистка 0x46bae0"),
        (0x4CA71C, "u32", "request_count", "0..0x200; очистка в 0x40c250"),
        (0x4CF088, "u32[8]", "request_list", "выбранные планировщиком индексы"),
        (0x4CF698, "u32[8]", "voice_priority", "приоритет, занявший голос"),
        (0x4CF658, "u32[8]", "voice_class", "class голоса"),
        (0x4CF678, "u32[8]", "voice_until", "оценка конца (0x12121212/0x7fffffff)"),
        (0x4C876C, "u32", "audio_ready", "0 — звук ещё не поднят, всё no-op"),
        (0x4AA384, "u32", "sound_enabled", "проверяется всеми обработчиками ВМ"),
    ],
    "listener": [
        (0x00, "f64[3]", "position", "x, y, z тела игрока (+0x00/+0x08/+0x10)"),
        (0x18, "f64[3]", "right", "e1, ось X; ею считается панорама (0x437909)"),
        (0x30, "f64[3]", "forward", "e2, ось взгляда"),
        (0x48, "f64[3]", "up", "e3"),
    ],
}

#: byte signatures used by --self-test to prove where a constant is read
SIGS = [
    ("min_dist", 0x46BD84, "dc1d28264a00", "fcomp [0x4a2628] — порог 1.0"),
    ("pan_bias", 0x46BE29, "dc2538264a00", "fsub [0x4a2638] — сдвиг -1"),
    ("pan_scale", 0x46BE2F, "dc0d40264a00", "fmul [0x4a2640] — *128"),
    ("dist_k", 0x434595, "dc0d48224a00", "fmul [0x4a2248] — *0.25"),
    ("req_pan", 0x40BE4E, "0faf0dbc684a00", "imul ecx,[0x4a68bc] — pan*32"),
    ("req_pan_shr", 0x40BE55, "c1f906", "sar ecx,6"),
    ("producer_atten", 0x46B8AD, "c78018ce6200b81e85eb", None),
    ("queue_stride", 0x46B85A, "8d0441", "lea eax,[ecx+eax*2] -> 0x2c"),
    ("pan_dot", 0x43793D, "dd45e8", "fld [ebp-0x18] — первая компонента d"),
    ("voice_l", 0x40A4F4, "b83fc52543", "magic 0x4325c53f — деление на 122"),
]


def _exelib():
    try:
        import exelib  # noqa: WPS433 - shipped beside this file

        return exelib
    except Exception:
        return None


def read_const(va: int):
    """Read one f64 from the executable image, or ``None`` without it."""
    ex = _exelib()
    if ex is None:
        return None
    try:
        off = ex.va2off(va)
        if off is None:
            return None
        return struct.unpack("<d", ex.data()[off:off + 8])[0]
    except Exception:
        return None


def read_code(va: int, n: int):
    ex = _exelib()
    if ex is None:
        return None
    try:
        off = ex.va2off(va)
        return ex.data()[off:off + n]
    except Exception:
        return None


# --- formulas ---------------------------------------------------------------

def length_approx(dx: float, dy: float, dz: float) -> float:
    """``0x4344df``: ``max(|.|) + 0.25*(sum of the other two)``.

    Not Euclid: the engine sorts the three absolute components (x87, see
    0x43452d..0x43458c) and returns ``s0 + (s1 + s2) * [0x4a2248]`` with
    ``s0 >= s1 >= s2`` and ``[0x4a2248] = 0.25``.
    """
    a = sorted((abs(dx), abs(dy), abs(dz)), reverse=True)
    return a[0] + (a[1] + a[2]) * 0.25


def clamped_distance(dist: float) -> float:
    """``0x46bd84``: ``dist < 1.0 -> 1.0`` (``[0x4a2628]``)."""
    return dist if dist >= 1.0 else 1.0


def gain_of(volume: int, atten: float, dist: float, muffle: int = 0) -> int:
    """Gain of one queue entry (``0x46bda4``..``0x46bdcd``).

    ``gain = trunc(volume - atten*2^n*dist_c)``, then ``>> 2n`` and finally
    ``min(gain, volume)``.  ``n`` is the count of sector-flag bits 2 and 4
    (``0x46bce5``/``0x46bcec``).  ``<= 0`` means "not audible".
    """
    scale = float(1 << muffle)
    t = atten * scale
    t = t * clamped_distance(dist)
    g = int(volume - t)          # 0x40e0e0 = fistp with RC=truncate
    if muffle:
        g >>= 2 * muffle         # sar edi,cl with cl = 2*n
    return min(g, volume)


def pan_of(dx: float, dy: float, dz: float, right, dist: float) -> int:
    """Panorama 0..255 (``0x46bddd``..``0x46be45``); 0 = left, 128 = centre.

    ``dot = d . right``; ``pan = (dot/dist_c + 1) * 128`` truncated and clamped.
    A source closer than one unit is forced to the centre (0x46be1a).
    """
    if dist <= 1.0:
        return 0x80
    dot = dx * right[0] + dy * right[1] + dz * right[2]
    p = (dot / clamped_distance(dist) + 1.0) * 128.0
    p = int(p)                    # ftol, truncate toward zero
    return max(0, min(255, p))


def channel_gains(vol_index: int, pan: int):
    """5-bit L/R of ``0x40a4b0``; ``None`` if the payload is silent.

    ``L = clamp(((255-pan)*v + 0x3d)/0x7a, 1, 32)``,
    ``R = clamp((pan*v + 0x3d)/0x7a, 1, 32)``.
    """
    v = vol_index
    if v <= 0:
        return None
    left = ((255 - pan) * v + 0x3D) // 0x7A
    right = (pan * v + 0x3D) // 0x7A
    return max(1, min(32, left)), max(1, min(32, right))


def request_volume(gain: int) -> int:
    """``0x40be4a``: ``v = gain*32 >> 6`` clamped to 32 (0 = no request)."""
    v = (gain * 32) >> 6
    return 0 if v <= 0 else min(v, 32)


def basis_from_angles(roll: float, pitch: float, yaw: float):
    """Columns e1(right), e2(forward), e3(up) of ``0x41e025``.

    ``R = Rz(-yaw) * Rx(-pitch) * Ry(roll)`` (``docs/CAMERA.md`` §3); the
    listener's own basis is filled with the player's roll/pitch/yaw at
    ``0x444c4d``..``0x444c9f`` (no view-pitch, no shake).
    """
    sA, cA = math.sin(roll), math.cos(roll)
    sB, cB = math.sin(pitch), math.cos(pitch)
    sC, cC = math.sin(yaw), math.cos(yaw)
    e1 = (cC * cA - sC * sB * sA, -sC * cA - cC * sB * sA, -cB * sA)
    e2 = (sC * cB, cC * cB, -sB)
    e3 = (cC * sA + sC * sB * cA, -sC * sA + cC * sB * cA, cB * cA)
    return e1, e2, e3


def basis_matrix(roll: float, pitch: float, yaw: float):
    """Same rotation as :func:`basis_from_angles` via explicit matrices."""
    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)]
                for i in range(3)]

    def rz(t):
        c, s = math.cos(t), math.sin(t)
        return [[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]]

    def rx(t):
        c, s = math.cos(t), math.sin(t)
        return [[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]]

    def ry(t):
        c, s = math.cos(t), math.sin(t)
        return [[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]

    m = mul(mul(rz(-yaw), rx(-pitch)), ry(roll))
    return ([m[0][0], m[1][0], m[2][0]],
            [m[0][1], m[1][1], m[2][1]],
            [m[0][2], m[1][2], m[2][2]])


def evaluate(source, listener, angles=(0.0, 0.0, 0.0), volume=64,
             atten=PRODUCER_ATTEN, rate=22050, priority=1000, muffle=0):
    """Full chain for one emitter; returns a dict with the intermediate steps."""
    roll, pitch, yaw = angles
    right, forward, up = basis_from_angles(roll, pitch, yaw)
    dx = float(source[0]) - float(listener[0])
    dy = float(source[1]) - float(listener[1])
    dz = float(source[2]) - float(listener[2])
    dist = length_approx(dx, dy, dz)
    dc = clamped_distance(dist)
    gain = gain_of(volume, atten, dist, muffle)
    pan = pan_of(dx, dy, dz, right, dist)
    vol_idx = request_volume(gain)
    ch = channel_gains(vol_idx, pan) if vol_idx else None
    dot = dx * right[0] + dy * right[1] + dz * right[2]
    return {
        "source": [source[0], source[1], source[2]],
        "listener": [listener[0], listener[1], listener[2]],
        "angles_deg": [math.degrees(a) for a in angles],
        "right": list(right),
        "delta": [dx, dy, dz],
        "dot_right": dot,
        "distance": dist,
        "distance_clamped": dc,
        "atten": atten,
        "muffle": muffle,
        "gain_trunc": gain,
        "gain": gain,
        "audible": gain > 0,
        "pan": pan,
        "volume_index": vol_idx,
        "channels": list(ch) if ch else None,
        "audible_range": (volume / atten) if atten > 0 else None,
        "request": {
            "volume": vol_idx,
            "pan": pan,
            "rate": rate,
            "priority": priority + vol_idx,
        },
    }


# --- corpus -----------------------------------------------------------------

def _walk(insns):
    for ins in insns:
        yield ins
        if ins.body:
            yield from _walk(ins.body)


def _levels(game_dir):
    return sorted(glob.glob(os.path.join(game_dir, "*.sbt")))


def _script_of(path):
    import gm.sbt as sbt

    doc = sbt.parse_file(path)
    rec = doc.records_of(11)
    return rec[0].payload.code if rec else None


def scan(game_dir=DEFAULT_GAME):
    """Walk every level script and collect the sound commands."""
    import gm.sbtvm as vm

    levels = _levels(game_dir)
    calls = {"location": [], "source": [], "speak": [], "emitter": []}
    per_level = {}
    for path in levels:
        name = os.path.splitext(os.path.basename(path))[0]
        try:
            code = _script_of(path)
            if code is None:
                continue
            insns, _ = vm.decode(code)
        except Exception as exc:              # pragma: no cover - broken file
            per_level[name] = {"error": str(exc)}
            continue
        counts = {"location": 0, "source": 0, "speak": 0, "emitter": 0}
        for ins in _walk(insns):
            if ins.name == "PLAY_SAMPLE_AT_LOCATION":
                a = ins.args
                calls["location"].append(
                    {"level": name, "sample": a[0], "volume": a[1], "rate": a[2],
                     "priority": a[3], "pos": [_s32(a[4]), _s32(a[5]), _s32(a[6])],
                     "atten": PRODUCER_ATTEN, "loop": False})
                counts["location"] += 1
            elif ins.name == "PLAY_SAMPLE_AT_SOURCE":
                a = ins.args
                calls["source"].append(
                    {"level": name, "sample": a[0], "volume": a[1], "rate": a[2],
                     "priority": a[3], "atten": PRODUCER_ATTEN, "loop": False})
                counts["source"] += 1
            elif ins.name == "PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE":
                # signature LENVL: (event_hi, event_lo, sample, volume, priority);
                # the handler 0x457e3c..0x457e54 passes 0x2b11 (=11025) as rate.
                a = ins.args
                calls["source"].append(
                    {"level": name, "sample": a[2], "volume": a[3],
                     "rate": 11025, "priority": a[4], "atten": PRODUCER_ATTEN,
                     "loop": True, "event": (a[0] << 16) | a[1]})
                counts["source"] += 1
            elif ins.name == "SPEAK_SAMPLE":
                a = ins.args
                calls["speak"].append(
                    {"level": name, "sample": a[0], "volume": a[1], "pan": a[2],
                     "rate": a[3]})
                counts["speak"] += 1
            elif ins.name == "ADD_SOUND_SOURCE":
                a = ins.args
                calls["emitter"].append(
                    {"level": name, "pos": [_s32(a[0]), _s32(a[1]), _s32(a[2])],
                     "sample": a[3], "volume": a[4], "atten": a[5], "loop": True,
                     "rate": 16000, "priority": 1000})
                counts["emitter"] += 1
        per_level[name] = counts
    return {"levels": [os.path.splitext(os.path.basename(p))[0] for p in levels],
            "calls": calls, "per_level": per_level}


def _s32(v):
    return v - (1 << 32) if v >= (1 << 31) else v


def _hist(values):
    out = {}
    for v in values:
        key = ("%g" % v) if isinstance(v, float) else str(v)
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items(), key=lambda kv: float(kv[0])))


def check(game_dir=DEFAULT_GAME):
    data = scan(game_dir)
    calls = data["calls"]
    loc = calls["location"]
    src = calls["source"]
    spk = calls["speak"]
    emi = calls["emitter"]
    bad = []
    for c in loc + src:
        if not 0 <= c["sample"] < SAMPLE_COUNT:
            bad.append(("sample", c))
        if not 0 <= c["volume"] <= 255:
            bad.append(("volume", c))
    for c in emi:
        if not 0 <= c["sample"] < SAMPLE_COUNT:
            bad.append(("sample", c))
        if not 0 <= c["volume"] <= 255:
            bad.append(("volume", c))
    result = {
        "game_dir": game_dir,
        "levels": len(data["levels"]),
        "totals": {
            "location": len(loc), "source": len(src),
            "speak": len(spk), "emitter": len(emi),
            "3d": len(loc) + len(src) + len(emi),
        },
        "location": {
            "samples": sorted({c["sample"] for c in loc}),
            "volumes": _hist([c["volume"] for c in loc]),
            "rates": _hist([c["rate"] for c in loc]),
            "priorities": _hist([c["priority"] for c in loc]),
            "atten": _hist([c["atten"] for c in loc]),
            "unique_positions": len({tuple(c["pos"]) for c in loc}),
            "audible_range": sorted({round(c["volume"] / c["atten"], 1)
                                     for c in loc}),
        },
        "source": {
            "samples": sorted({c["sample"] for c in src}),
            "volumes": _hist([c["volume"] for c in src]),
            "rates": _hist([c["rate"] for c in src]),
            "priorities": _hist([c["priority"] for c in src]),
            "atten": _hist([c["atten"] for c in src]),
            "loop_calls": sum(1 for c in src if c["loop"]),
            "audible_range": sorted({round(c["volume"] / c["atten"], 1)
                                     for c in src}),
        },
        "speak": {
            "samples": sorted({c["sample"] for c in spk}),
            "volumes": _hist([c["volume"] for c in spk]),
            "pans": _hist([c["pan"] for c in spk]),
            "rates": _hist([c["rate"] for c in spk]),
        },
        "emitter": {
            "samples": sorted({c["sample"] for c in emi}),
            "volumes": _hist([c["volume"] for c in emi]),
            "atten": _hist([c["atten"] for c in emi]),
            "audible_range": sorted({round(c["volume"] / c["atten"], 1)
                                     for c in emi}),
        },
        "invalid": bad,
        "per_level": data["per_level"],
    }
    return result


def _parse_vec(text):
    parts = [p.strip() for p in str(text).replace(";", ",").split(",") if p.strip()]
    return [float(p) for p in parts]


def _select_call(spec, kind, game_dir):
    level, _, idx = spec.partition(":")
    if not level:
        raise SystemExit("--call/--source expects LEVEL:INDEX")
    index = int(idx or 0)
    data = scan(game_dir)
    key = {"call": "location", "source": "emitter"}[kind]
    items = [c for c in data["calls"][key] if c["level"] == level]
    if not items:
        raise SystemExit("no %s calls in %s" % (key, level))
    if not 0 <= index < len(items):
        raise SystemExit("%s has %d %s calls (0..%d)"
                         % (level, len(items), key, len(items) - 1))
    return items[index], items


# --- rendering --------------------------------------------------------------

def cmd_fields(as_json=False):
    if as_json:
        out = {name: [dict(off=f[0], type=f[1], name=f[2], note=f[3],
                           evidence=(f[4] if len(f) > 4 else ""))
                      for f in fields]
               for name, fields in FIELDS.items()}
        out["constants"] = _constants_json()
        print(json.dumps(out, ensure_ascii=False, indent=1, sort_keys=True))
        return
    print("Gunmetal 3D audio: field map (VA, image base 0x400000)")
    print()
    print("sample record 0x%06x stride 0x%x x %d:" %
          (SAMPLE_BASE, SAMPLE_STRIDE, SAMPLE_COUNT))
    for off, typ, name, ev in (
            (0x00, "u32", "size_bytes", "0x40af30 / 0x40a4d9"),
            (0x04, "u32", "slots_32k", "0x40af30 / 0x40a578"),
            (0x08, "u32", "step_hi", "0x40b10d"),
            (0x0c, "u32", "step_lo", "0x40b11b"),
            (0x10, "ptr", "data", "0x40b168"),
            (0x18, "u32", "format", "0=SOU0 1..3=SOU1 4=SOU2")):
        print("  +0x%02x %-9s %-12s %s" % (off, typ, name, ev))
    print()
    print("voice 0x%06x stride 0x%x x %d (+1 music slot): vol_l +0x2c, vol_r +0x30"
          % (VOICE_BASE, VOICE_STRIDE, VOICE_COUNT))
    print()
    for group in ("queue", "request"):
        base = QUEUE_BASE if group == "queue" else REQ_BASE
        step = QUEUE_STRIDE if group == "queue" else REQ_STRIDE
        cap = 256 if group == "queue" else REQ_MAX
        print("%s 0x%06x stride 0x%x max %d:" % (group, base, step, cap))
        for f in FIELDS[group]:
            print("  +0x%02x %-6s %-10s %s" % (f[0], f[1], f[2], f[3]))
            print("        %s" % f[4])
        print()
    print("listener (player's body object, ptr at player+0x3178):")
    for f in FIELDS["listener"]:
        print("  +0x%02x %-8s %-9s %s" % (f[0], f[1], f[2], f[3]))
    print("  basis: 0x41e025(e1=+0x18, e2=+0x30, e3=+0x48, roll, pitch, yaw)")
    print("         call site 0x444c8e..0x444c9f; angles = player+0x20/+0x18/+0x28")
    print()
    print("constants:")
    for va, name, note in _CONST_TABLE:
        v = read_const(va)
        raw = read_code(va, 8)
        print("  0x%06x %-12s %-22s %s" % (
            va, name, ("%g" % v) if v is not None else "?",
            raw.hex() if raw else note))
    print()
    print("source columns (ADD_SOUND_SOURCE op 187 -> 0x46b210):")
    print("  x 0x%06x  y 0x%06x  z 0x%06x  sample 0x%06x  volume 0x%06x"
          % (SRC_X, SRC_Y, SRC_Z, SRC_SAMPLE, SRC_VOLUME))
    print("  atten 0x%06x (f64)  count 0x%06x  fill 0x46b570  clear 0x46bae0"
          % (SRC_ATTEN, SOURCE_COUNT))
    print("  NOTE: the f64 column is **attenuation per unit**, not pitch —")
    print("        it lands in queue+0x00 and is multiplied by the distance.")


_CONST_TABLE = [
    (K_MIN_DIST, "min_distance", "1.0"),
    (K_PAN_BIAS, "pan_bias", "-1.0"),
    (K_PAN_SCALE, "pan_scale", "128.0"),
    (K_LEN_K, "length_k", "0.25"),
    (K_PAN_REQ, "pan_req_scale", "32 (int)"),
    (K_OUT_RATE, "out_rate", "32000 (int)"),
]


def _constants_json():
    out = {}
    for va, name, note in _CONST_TABLE:
        v = read_const(va)
        raw = read_code(va, 8)
        out[name] = {"va": hex(va), "value": v if v is not None else note,
                     "bytes": raw.hex() if raw else None}
    return out


def cmd_calc(args):
    listener = _parse_vec(args.listener) if args.listener else [0.0, 0.0, 0.0]
    if len(listener) < 3:
        raise SystemExit("--listener needs X,Y,Z[,YAW[,PITCH[,ROLL]]]")
    pos = listener[:3]
    yaw, pitch, roll = (listener + [0.0, 0.0, 0.0])[3:6]
    angles = (roll, pitch, yaw)          # basis_from_angles takes (roll,pitch,yaw)

    source = None
    volume, atten, rate, priority = 64, PRODUCER_ATTEN, 22050, 1000
    label = "explicit"
    if args.call:
        item, items = _select_call(args.call, "call", args.game)
        source = item["pos"]
        volume, atten, rate, priority = (item["volume"], item["atten"],
                                         item["rate"], item["priority"])
        label = "PLAY_SAMPLE_AT_LOCATION %s (of %d)" % (args.call, len(items))
    elif args.source:
        item, items = _select_call(args.source, "source", args.game)
        source = item["pos"]
        volume, atten = item["volume"], item["atten"]
        rate, priority = item["rate"], item["priority"]
        label = "ADD_SOUND_SOURCE %s (of %d)" % (args.source, len(items))
    if args.pos:
        source = _parse_vec(args.pos)[:3]
        label = "explicit position"
    if source is None:
        raise SystemExit("--calc needs --pos, --call LEVEL:INDEX or --source LEVEL:INDEX")
    if args.volume is not None:
        volume = args.volume
    if args.atten is not None:
        atten = args.atten
    if args.rate is not None:
        rate = args.rate
    if args.priority is not None:
        priority = args.priority

    res = evaluate(source, pos, angles, volume=volume, atten=atten, rate=rate,
                   priority=priority, muffle=args.muffle)
    res["call"] = label
    if args.json:
        print(json.dumps(res, ensure_ascii=False, indent=1, sort_keys=True))
        return res
    r, d = res["right"], res["delta"]
    print("source: %s" % label)
    print("  pos=(%g, %g, %g)  volume=%d  atten=%g  rate=%d  priority=%d  muffle=%d"
          % (source[0], source[1], source[2], volume, atten, rate, priority,
             args.muffle))
    print("listener: pos=(%g, %g, %g)  roll=%.3f pitch=%.3f yaw=%.3f deg=(%g, %g, %g)"
          % (pos[0], pos[1], pos[2], angles[0], angles[1], angles[2],
             *res["angles_deg"]))
    print("  right e1 = (%.9f, %.9f, %.9f)" % tuple(r))
    print("d = source - listener = (%.3f, %.3f, %.3f)" % tuple(d))
    a = sorted((abs(d[0]), abs(d[1]), abs(d[2])), reverse=True)
    print("dist = max + 0.25*(mid+min) = %.3f + 0.25*(%.3f + %.3f) = %.6f"
          % (a[0], a[1], a[2], res["distance"]))
    print("dist_clamped = %.6f   audible range (volume/atten) = %s"
          % (res["distance_clamped"],
             "inf" if not res["audible_range"] else "%.1f" % res["audible_range"]))
    if args.muffle:
        pre = int(volume - atten * (1 << args.muffle) * res["distance_clamped"])
        print("gain = trunc(%d - %g*2^%d*%.6f) = %d, >> %d = %d  "
              "(clamped to <= volume) -> %d"
              % (volume, atten, args.muffle, res["distance_clamped"], pre,
                 2 * args.muffle, pre >> (2 * args.muffle), res["gain"]))
    else:
        print("gain = trunc(%d - %g*%.6f) = %d  (clamped to <= volume) -> %d"
              % (volume, atten, res["distance_clamped"],
                 int(volume - atten * res["distance_clamped"]), res["gain"]))
    print("dot = d.right = %.6f" % res["dot_right"])
    print("pan = clamp(trunc((%.6f/%.6f + 1)*128), 0, 255) = %d"
          % (res["dot_right"], res["distance_clamped"], res["pan"]))
    print("request: volume=%d (gain/2 clipped 1..32), pan=%d, rate=%d, priority=%d"
          % (res["volume_index"], res["pan"], rate,
             priority + res["volume_index"]))
    ch = res["channels"]
    if ch:
        print("L/R (5-bit, 0x40a4b0): L = ((255-%d)*%d+61)/122 = %d, "
              "R = (%d*%d+61)/122 = %d"
              % (res["pan"], res["volume_index"], ch[0],
                 res["pan"], res["volume_index"], ch[1]))
    else:
        print("L/R: no request (gain < 2 -> volume index 0)")
    return res


def cmd_self_test(args):
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        checks.append((name, bool(cond), detail))
        if not cond:
            ok = False

    # 1. constants from the executable
    consts = {va: read_const(va) for va, _, _ in _CONST_TABLE}
    have_exe = consts[K_MIN_DIST] is not None
    if have_exe:
        check("const 0x4a2628 == 1.0", consts[K_MIN_DIST] == 1.0,
              repr(consts[K_MIN_DIST]))
        check("const 0x4a2638 == -1.0", consts[K_PAN_BIAS] == -1.0,
              repr(consts[K_PAN_BIAS]))
        check("const 0x4a2640 == 128.0", consts[K_PAN_SCALE] == 128.0,
              repr(consts[K_PAN_SCALE]))
        check("const 0x4a2248 == 0.25", consts[K_LEN_K] == 0.25,
              repr(consts[K_LEN_K]))
        check("0x4a68bc == 32 (pan scale)",
              read_code(K_PAN_REQ, 4) == b"\x20\x00\x00\x00",
              (read_code(K_PAN_REQ, 4) or b"").hex())
        check("0x4a6810 == 32000 (output rate)",
              read_code(K_OUT_RATE, 4) == b"\x00\x7d\x00\x00",
              (read_code(K_OUT_RATE, 4) or b"").hex())
    else:
        checks.append(("executable present", False, "constants not verified"))

    # 2. instruction signatures
    for name, va, sig, note in SIGS:
        raw = read_code(va, len(sig) // 2)
        if raw is None:
            checks.append(("sig %s" % name, False, "no executable"))
            ok = False
            continue
        check("sig %s @0x%06x" % (name, va), raw.hex() == sig.replace(" ", ""),
              raw.hex())

    # 3. length approximation is *not* Euclid; pinned values
    check("dist(3,4,0) == 4.75 (max 4 + 0.25*3)",
          abs(length_approx(3, 4, 0) - 4.75) < 1e-12,
          "%r" % length_approx(3, 4, 0))
    check("dist(100,100,100) == 150",
          abs(length_approx(100, 100, 100) - 150.0) < 1e-12,
          "%r" % length_approx(100, 100, 100))
    check("dist(0,0,-7) == 7", length_approx(0, 0, -7) == 7.0,
          "%r" % length_approx(0, 0, -7))
    check("dist(24,3416,-184) == 3468",
          abs(length_approx(24, 3416, -184) - 3468.0) < 1e-12,
          "%r" % length_approx(24, 3416, -184))
    check("clamped_distance(0.5) == 1",
          clamped_distance(0.5) == 1.0, "1.0")
    check("clamped_distance(2.5) == 2.5", clamped_distance(2.5) == 2.5)

    # 4. gain: truncation, ceiling at the base volume, muffle shift
    check("gain(vol=64,atten=0.03,d=0) == 63 (d clamps to 1)",
          gain_of(64, 0.03, 0) == 63, str(gain_of(64, 0.03, 0)))
    check("gain(64,0.03,1000) == 34", gain_of(64, 0.03, 1000) == 34,
          str(gain_of(64, 0.03, 1000)))
    check("gain(64,0.03,3000) == -26 (silent)",
          gain_of(64, 0.03, 3000) == -26, str(gain_of(64, 0.03, 3000)))
    check("gain muffle=1: (64-0.03*2*d)>>2",
          gain_of(64, 0.03, 100, 1) == int(64 - 0.03 * 2 * 100) >> 2,
          str(gain_of(64, 0.03, 100, 1)))
    check("gain never exceeds volume",
          gain_of(100, 0.0, 0, 0) == 100, str(gain_of(100, 0.0, 0, 0)))

    # 5. pan: right/left/centre and the < 1 unit shortcut
    right, _, _ = basis_from_angles(0, 0, 0)
    check("pan purely right == 255", pan_of(10, 0, 0, right, 10) == 255,
          str(pan_of(10, 0, 0, right, 10)))
    check("pan purely left == 0", pan_of(-10, 0, 0, right, 10) == 0,
          str(pan_of(-10, 0, 0, right, 10)))
    check("pan forward == 128", pan_of(0, 10, 0, right, 10) == 128,
          str(pan_of(0, 10, 0, right, 10)))
    check("pan inside 1 unit is forced to 128",
          pan_of(0.5, 0, 0, right, 0.5) == 128, "128")
    diag = (math.cos(math.pi / 4), math.sin(math.pi / 4), 0.0)
    check("pan along e1 but diagonal source saturates at 255",
          pan_of(10, 10, 0, diag, length_approx(10, 10, 0)) == 255,
          str(pan_of(10, 10, 0, diag, length_approx(10, 10, 0))))
    check("pan with yaw=90 (right = -Y)",
          pan_of(0, -10, 0, basis_from_angles(0, 0, math.pi / 2)[0], 10) == 255,
          str(pan_of(0, -10, 0, basis_from_angles(0, 0, math.pi / 2)[0], 10)))

    # 6. request volume + 5-bit channels
    check("request_volume(64) == 32", request_volume(64) == 32,
          str(request_volume(64)))
    check("request_volume(3) == 1", request_volume(3) == 1,
          str(request_volume(3)))
    check("request_volume(1) == 0 (dropped)", request_volume(1) == 0, "0")
    check("channels centre=32/32", channel_gains(32, 128) == (32, 32),
          str(channel_gains(32, 128)))
    check("channels left=(32,1)", channel_gains(32, 0) == (32, 1),
          str(channel_gains(32, 0)))
    check("channels right=(1,32)", channel_gains(32, 255) == (1, 32),
          str(channel_gains(32, 255)))
    check("channels never zero", channel_gains(1, 128) == (1, 1),
          str(channel_gains(1, 128)))
    magic = 0x4325C53F
    check("magic 0x4325c53f == /122 (sar 5)",
          all(((x * magic) >> 37) == x // 0x7A for x in range(0, 5000)),
          "0..4999")

    # 7. basis == explicit matrix product, orthonormal, det = +1
    worst = 0.0
    for roll, pitch, yaw in ((0, 0, 0), (0.1, -0.2, 0.3), (-0.3, 0.25, 2.1),
                             (0.0, 0.5, -1.2), (0.2, 0.0, 3.0)):
        a = basis_from_angles(roll, pitch, yaw)
        b = basis_matrix(roll, pitch, yaw)
        for av, bv in zip(a, b):
            for x, y in zip(av, bv):
                worst = max(worst, abs(x - y))
    check("closed form == matrix product", worst < 1e-12, "%.3e" % worst)
    e1, e2, e3 = basis_from_angles(0.0, 0.0, 0.0)
    check("identity at zero angles",
          e1 == (1.0, 0.0, 0.0) and abs(e2[1] - 1.0) < 1e-15
          and abs(e3[2] - 1.0) < 1e-15, str((e1, e2, e3)))
    e1, e2, e3 = basis_from_angles(0.0, 0.0, math.pi / 2)
    check("yaw=+90 looks along +X", abs(e2[0] - 1.0) < 1e-15, str(e2))
    e1, e2, e3 = basis_from_angles(0.0, 0.3, 0.0)
    check("pitch>0 looks down (e2.z < 0)", e2[2] < 0, str(e2))

    # 8. full chain on pinned numbers (reproduced in docs/AUDIO_3D.md §6)
    r = evaluate([48, 3208, -136], [0, 3200, 48], (0, 0, 0),
                 volume=64, atten=0.03, rate=22050, priority=100)
    check("chain level0:0 dist", abs(r["distance"] - 198.0) < 1e-9,
          "%.4f" % r["distance"])
    check("chain level0:0 gain", r["gain"] == 58, str(r["gain"]))
    check("chain level0:0 pan", r["pan"] == 159, str(r["pan"]))
    check("chain level0:0 L/R", r["channels"] == [23, 32],
          str(r["channels"]))
    r = evaluate([48, 3208, -136], [0, 3200, 48], (0, 0, math.pi / 2),
                 volume=64, atten=0.03, rate=22050, priority=100)
    check("chain level0:0 yaw=90 mirrors pan", r["pan"] == 122,
          str(r["pan"]))
    check("chain level0:0 yaw=90 L/R", r["channels"] == [32, 29],
          str(r["channels"]))
    r = evaluate([-616, -168, 71], [-600, -150, 60], (0, 0, 0),
                 volume=19, atten=0.05, rate=16000, priority=1000)
    check("chain emitter level0:0 pan", r["pan"] == 45, str(r["pan"]))
    check("chain emitter level0:0 L/R", r["channels"] == [14, 3],
          str(r["channels"]))
    r = evaluate([48, 3208, -136], [0, 3200, 48], (0, 0, 0),
                 volume=64, atten=0.03, muffle=1)
    check("muffle=1: (64-2*0.03*198)>>2 == 13", r["gain"] == 13,
          str(r["gain"]))
    check("muffle=1 L/R", r["channels"] == [5, 8], str(r["channels"]))

    # 9. corpus (only with the shipped levels)
    if os.path.isdir(args.game):
        data = check_corpus(args.game)
        check("corpus: level0 location calls",
              data["per_level"]["level0"]["location"] == 8,
              str(data["per_level"]["level0"]["location"]))
        check("corpus: level0 emitter calls",
              data["per_level"]["level0"]["emitter"] == 6,
              str(data["per_level"]["level0"]["emitter"]))
        check("corpus: dm1 location calls",
              data["per_level"]["dm1"]["location"] == 17,
              str(data["per_level"]["dm1"]["location"]))
        check("corpus: no invalid sample/volume", not data["invalid"],
              str(len(data["invalid"])))
        check("corpus: every emitter attenuation > 0",
              all(float(k) > 0 for k in data["emitter"]["atten"]),
              str(data["emitter"]["atten"]))
        check("corpus: script 3D attenuation is exactly 0.03",
              data["location"]["atten"] == {"0.03": data["totals"]["location"]},
              str(data["location"]["atten"]))
    else:
        checks.append(("corpus", False, "game directory missing"))

    if args.json:
        print(json.dumps({"ok": ok,
                          "checks": [{"name": n, "ok": o, "detail": d}
                                     for n, o, d in checks]},
                         ensure_ascii=False, indent=1))
    else:
        for name, good, detail in checks:
            print("%s %-44s %s" % ("ok  " if good else "FAIL", name, detail))
        print("\n%d/%d checks passed" % (sum(1 for _, o, _ in checks if o),
                                         len(checks)))
    return 0 if ok else 1


def check_corpus(game_dir):
    return check(game_dir)


def cmd_check(args):
    data = check(args.game)
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=1, sort_keys=True))
        return 0 if not data["invalid"] else 1
    t = data["totals"]
    print("sound commands in %d level scripts (%s)" % (data["levels"], args.game))
    print("  PLAY_SAMPLE_AT_LOCATION         %4d  (atten 0.03, 7 args)"
          % t["location"])
    print("  PLAY_SAMPLE_AT_SOURCE (+loop)   %4d  (atten 0.03, pos = entity)"
          % t["source"])
    print("  ADD_SOUND_SOURCE                %4d  (per-source atten f64)"
          % t["emitter"])
    print("  SPEAK_SAMPLE                    %4d  (2D, no position)" % t["speak"])
    print("  => 3D calls                     %4d" % t["3d"])
    for name in ("location", "source", "emitter"):
        d = data[name]
        print()
        print("%s:" % name)
        print("  volumes:  %s" % d["volumes"])
        print("  atten:    %s" % d["atten"])
        print("  samples:  %s" % d["samples"])
        if "rates" in d:
            print("  rates:    %s" % d["rates"])
        if "priorities" in d:
            print("  priorities: %s" % d["priorities"])
        if "audible_range" in d:
            print("  audible range (volume/atten): %s" % d["audible_range"])
        if "unique_positions" in d:
            print("  unique positions: %d of %d calls"
                  % (d["unique_positions"], t["location"]))
        if "loop_calls" in d:
            print("  looping calls: %d" % d["loop_calls"])
    print()
    print("SPEAK_SAMPLE volumes %s  pans %s  rates %s"
          % (data["speak"]["volumes"], data["speak"]["pans"],
             data["speak"]["rates"]))
    print("invalid records: %d" % len(data["invalid"]))
    return 0 if not data["invalid"] else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Gunmetal 3D positional audio: map, formulas, corpus")
    ap.add_argument("--fields", action="store_true", help="field/address map")
    ap.add_argument("--check", action="store_true", help="scan the level corpus")
    ap.add_argument("--calc", action="store_true", help="compute one emitter")
    ap.add_argument("--self-test", action="store_true", help="run built-in checks")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game directory")
    ap.add_argument("--listener", help="X,Y,Z[,YAW[,PITCH[,ROLL]]] (radians)")
    ap.add_argument("--pos", help="explicit source X,Y,Z")
    ap.add_argument("--call", help="LEVEL:INDEX of PLAY_SAMPLE_AT_LOCATION")
    ap.add_argument("--source", help="LEVEL:INDEX of ADD_SOUND_SOURCE")
    ap.add_argument("--volume", type=int)
    ap.add_argument("--atten", type=float)
    ap.add_argument("--rate", type=int)
    ap.add_argument("--priority", type=int)
    ap.add_argument("--muffle", type=int, default=0,
                    help="sector flag bits 2/4 count (0..2)")
    args = ap.parse_args(argv)

    if args.self_test:
        return cmd_self_test(args)
    if args.fields:
        cmd_fields(args.json)
        return 0
    if args.check:
        return cmd_check(args)
    if args.calc:
        cmd_calc(args)
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
