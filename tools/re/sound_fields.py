#!/usr/bin/env python3
"""Maps of the sample and voice records of the Gunmetal sound subsystem (M2.3.7).

The engine keeps two fixed record arrays in ``.data``:

* sample records (``0x83d960``, stride ``0x1c`` = 28, **300** entries),
* voice slots    (``0x84e068``, stride ``0x50`` = 80, **9** slots: eight
  effect voices mixed by ``0x409330`` plus one music stream used when
  ``DAT_0084e300 != 0``).

The numbers in ``ROADMAP.md`` were read as "28 records" and "80-byte voices";
28 is the *stride* of the sample record and 80 the stride of a voice. The
counts are proven by the init/clear loops and the bounds checks listed in
``docs/SOUND_FIELDS.md``.

The tool is read-only: it parses container headers (never decodes audio) and
only uses the Python standard library.

Usage::

    python3 tools/re/sound_fields.py --map
    python3 tools/re/sound_fields.py --verify
    python3 tools/re/sound_fields.py --verify --json
    python3 tools/re/sound_fields.py --self-test
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

DEFAULT_DIR = "/mnt/d/games/Gunmetal 3dfx"
OUT_RATE = 32000              # [0x4a6810] = 0x7d00, docs/INIT.md §7
SAMPLE_BASE = 0x83D960
SAMPLE_STRIDE = 0x1C
SAMPLE_COUNT = 300
VOICE_BASE = 0x84E068
VOICE_STRIDE = 0x50
VOICE_SLOTS = 9
EFFECT_VOICES = 8

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3

# ---------------------------------------------------------------------------
# maps
# ---------------------------------------------------------------------------
# (offset, type, name, status, evidence)
#   status: "код"     — field is read/written by the named instructions
#           "значения" — proven by shipped data only
#           "гипотеза" — plausible, no direct reader/writer
SAMPLE_FIELDS = [
    (0x00, "u32", "size_bytes", "код",
     "W 0x40af30 (SOU0/1: length&~1, SOU2: frames); "
     "R 0x40a4d9 0x40a6b9 0x40a745 0x40a853 0x40b41c; clear 0x40b460"),
    (0x04, "u32", "slots_32k", "код",
     "W 0x40af30 (0x40b0e8/0x40b258..0x40b2b0): SOU0 len*32000/rate, "
     "SOU1 len<<{3,2,1} (>>1 stereo), SOU2 frames; "
     "R 0x40a578 0x40a766 0x40a489 (музыка) 0x40a9b0"),
    (0x08, "u32", "step_hi", "код",
     "W 0x40b10d = rate/32000; R 0x40a09d (SOU0-ветка микшера)"),
    (0x0c, "u32", "step_lo", "код",
     "W 0x40b11b = ((rate<<16)/32000)<<16 — младшие 32 бита шага 32.32; "
     "R 0x40a099 (SOU0-ветка)"),
    (0x10, "ptr", "data", "код",
     "W 0x40b168 (FUN_00402770); R 0x40a591 0x40a422 0x40a0a5 0x40a0d6; "
     "free 0x40b400 0x40a840"),
    (0x14, "u32", "right_offset", "код",
     "W 0x40b0e8 (stereo: len/2) / 0x40b0f0 (mono: 0); "
     "R только загрузчик (0x40b2d8 деинтерлив); в голос НЕ копируется"),
    (0x18, "u32", "format", "код",
     "W 0x40af30 (0=SOU0, 1/2/3=SOU1, 4=SOU2); R 0x40a556 0x40a43a → voice+0x20"),
]

VOICE_FIELDS = [
    (0x00, "u32", "remaining", "код",
     "W 0x40a67c (start), 0x40a0fa (-0x400), 0x40a37f (loop rewind); "
     "R 0x409dde (count=min(remaining,0x400))"),
    (0x04, "u32", "sample_id", "код",
     "W 0x40a600; R 0x40a867 (stop by sample), 0x40a8b3 (update)"),
    (0x08, "u32", "step_hi", "код",
     "W 0x40a60a (rate/32000); R 0x40a09d только для format 0"),
    (0x0c, "u32", "step_lo", "код",
     "W 0x40a614 (младшие 32 бита шага 32.32); R 0x40a099 format 0"),
    (0x10, "ptr", "cursor_l", "код",
     "W 0x40a61e, 0x40a0e9/0x40a36c (после декода); "
     "R 0x409ead 0x40a0a5 0x40a0d6"),
    (0x14, "u32", "right_offset", "гипотеза",
     "R 0x409ee5 0x40a0a1 0x40a2c8; writer не найден "
     "(0x40a5e0/0x40a8a0 не пишут) → 0; для моно это верно"),
    (0x18, "ptr", "loop_start", "код",
     "W 0x40a635 (arg6, обычный старт = 0); музыка 0x40a42e; "
     "R 0x40a101 0x40a2cc 0x40a322"),
    (0x1c, "u32", "loop_len", "код",
     "W 0x40a62f (arg5, старт = 0); музыка 0x40a42e (record+0x04); "
     "R 0x40a374 (remaining = loop_len - remainder)"),
    (0x20, "u32", "format", "код",
     "W 0x40a63f; R 0x409e64 (0..3 → 0x861000/0x852b40/c00/cc0)"),
    (0x24, "u32", "prev_volume_l", "код",
     "W 0x40a2f6 (после блока); R 0x409e70 (acc = acc*vol/prev)"),
    (0x28, "u32", "prev_volume_r", "код",
     "W 0x40a301; R 0x409e8a (acc = acc*vol/prev)"),
    (0x2c, "u32", "volume_l", "код",
     "W 0x40a665 (arg9, 1..32), 0x40a8?? (update); "
     "R 0x409e16 0x409e59 0x40a0bc → [0x84a6a8]=vol-1"),
    (0x30, "u32", "volume_r", "код",
     "W 0x40a66f (arg10, 1..32); R 0x409e1d 0x409ec6 0x40a2ad"),
    (0x34, "u32", "music_state_a", "гипотеза",
     "слот 8 (0x84e31c): состояние SOU2-синтеза, band 12"),
    (0x38, "u32", "music_state_b", "гипотеза", "слот 8 (0x84e320)"),
    (0x3c, "u32", "music_state_c", "гипотеза", "слот 8 (0x84e324)"),
    (0x40, "i32", "acc_l", "код",
     "W 0x40a653 (0), 0x409e81 (rescale); R 0x409e77, декодер 0x852b40"),
    (0x44, "i32", "level_l", "код", "W 0x40a64d (0); R/использует декодер"),
    (0x48, "i32", "acc_r", "код",
     "W 0x40a65f (0), 0x409e9b; R 0x409e94, декодер 0x852b40"),
    (0x4c, "i32", "level_r", "код", "W 0x40a659 (0); R/использует декодер"),
]

# auxiliary tables found while tracing the two main ones
REQUEST_FIELDS = [
    (0x00, "u32", "queued", "код", "0x40be?? =0, 0x40c080 ставит 1"),
    (0x04, "u32", "class", "код", "0x40c2c?; сравнение с voice+0x04 в 0x40c080"),
    (0x08, "u32", "sample_id", "код", "→ 0x40a4b0 param_1 через 0x40c2c7"),
    (0x0c, "u32", "volume", "код", "0x40be??; 0..0xff → 1..32 в 0x40a4b0"),
    (0x10, "u32", "pan", "код", "0x40be??; 0..0xff → 1..32 в 0x40a4b0"),
    (0x14, "u32", "rate", "код", "→ 0x40a4b0 param_2 (шаг и длина SOU0)"),
    (0x18, "u32", "priority", "код", "0x40c080 выбирает максимум, 0x40c2??"),
    (0x1c, "u32", "flags", "код", "0x80 = петля → 0x40ad20/0x40bec0"),
]

QUEUE3D_FIELDS = [
    (0x00, "f64", "atten_per_unit", "код", "0x46c243 FMUL дистанции; 0.03 у 0x46b840"),
    (0x08, "i32", "x", "код", "0x46b840 param_5"),
    (0x0c, "i32", "y", "код", "0x46b840 param_6"),
    (0x10, "i32", "z", "код", "0x46b840 param_7"),
    (0x14, "u32", "class", "код", "уникальный id источника (4e6+index и т.п.)"),
    (0x18, "u32", "sample_id", "код", "индекс 0x4ca720 → запись 0x83d960"),
    (0x1c, "u32", "volume", "код", "0x46c250: volume − atten*dist"),
    (0x20, "u32", "rate", "код", "16000/11025 → 0x40a4b0 param_2"),
    (0x24, "u32", "priority", "код", "→ 0x40be?? param_6"),
    (0x28, "u32", "loop", "код", "1 = зацикленный источник"),
]


def _fields_json(fields):
    return [{"offset": o, "type": t, "name": n, "status": s, "evidence": e}
            for o, t, n, s, e in fields]


MAPS = {
    "sample": {
        "title": "Запись сэмпла",
        "base": SAMPLE_BASE,
        "stride": SAMPLE_STRIDE,
        "count": SAMPLE_COUNT,
        "end": SAMPLE_BASE + SAMPLE_STRIDE * SAMPLE_COUNT,
        "fields": SAMPLE_FIELDS,
    },
    "voice": {
        "title": "Голос (слот)",
        "base": VOICE_BASE,
        "stride": VOICE_STRIDE,
        "count": VOICE_SLOTS,
        "end": VOICE_BASE + VOICE_STRIDE * VOICE_SLOTS,
        "fields": VOICE_FIELDS,
    },
    "request": {
        "title": "Запрос звука (aux)",
        "base": 0x4CABD8,
        "stride": 0x20,
        "count": 8,
        "end": 0x4CABD8 + 0x20 * 8,
        "fields": REQUEST_FIELDS,
    },
    "queue3d": {
        "title": "Очередь 3D-звуков (aux)",
        "base": 0x62CE18,
        "stride": 0x2C,
        "count": 0x100,
        "end": 0x62CE18 + 0x2C * 0x100,
        "fields": QUEUE3D_FIELDS,
    },
}


def print_map(key, out=sys.stdout):
    m = MAPS[key]
    out.write("== %s: 0x%08x, шаг 0x%x, записей %d, конец 0x%08x\n"
              % (m["title"], m["base"], m["stride"], m["count"], m["end"]))
    for off, typ, name, status, ev in m["fields"]:
        out.write("  +0x%02x %-9s %-15s %-8s %s\n"
                  % (off, typ, name, status, ev))
    out.write("\n")


# ---------------------------------------------------------------------------
# container parsing (never touches the payload codec)
# ---------------------------------------------------------------------------
class Fail(Exception):
    pass


def u32(b, o):
    if o + 4 > len(b):
        raise Fail("truncated u32 at %d" % o)
    return struct.unpack_from("<I", b, o)[0]


def fnv1a64(data, seed=FNV_OFFSET):
    h = seed
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_sou(blob, total=None):
    """Header only: what ``0x40af30`` reads before the codec.

    *total* is the entry size when only a header prefix was read; payload
    bounds are checked against it instead of ``len(blob)``.
    """
    if total is None:
        total = len(blob)
    if len(blob) < 16:
        raise Fail("short header (%d bytes)" % len(blob))
    magic = blob[:4]
    if magic not in (b"SOU0", b"SOU1", b"SOU2"):
        raise Fail("bad magic %r" % magic)
    rate = u32(blob, 4)
    if magic == b"SOU2":
        if len(blob) < 20:
            raise Fail("short SOU2 header (%d bytes)" % len(blob))
        frames = u32(blob, 8)
        meta2 = u32(blob, 12)
        units = u32(blob, 16)
        payload = units * 4
        if payload > total - 20:
            raise Fail("SOU2 units*4=%d past EOF" % payload)
        return {"magic": "SOU2", "rate": rate, "flags": 1,
                "channels": 2, "length_field": frames, "frames": frames,
                "units": units, "meta2": meta2, "payload": payload,
                "trailer": total - 20 - payload}
    flags = u32(blob, 8)
    length = u32(blob, 12)
    payload = length & ~1
    if payload > total - 16:
        raise Fail("payload %d past EOF" % payload)
    return {"magic": magic.decode(), "rate": rate, "flags": flags & 0xFF,
            "channels": 2 if flags & 1 else 1, "length_field": length,
            "payload": payload, "trailer": total - 16 - length}


def loadable(h):
    """Would ``0x40af30`` accept the header? (codec is not checked)"""
    if h["rate"] <= 0 or h["rate"] > 192000 or h["payload"] <= 0:
        return False
    if h["magic"] == "SOU2":
        return h["frames"] > 0 and h["frames"] % 32 == 0 and h["units"] > 0
    return h["flags"] <= 1


def record_fields(h):
    """The seven dwords written by ``0x40af30`` (see SAMPLE_FIELDS).

    A header the loader rejects still gets the format/step fields it would
    compute, but ``slots``/``loadable`` are zero/false (the shipped
    ``L04I01``/``L06A01`` anomalies).
    """
    rate = h["rate"]
    ch = h["channels"]
    payload = h["payload"]
    ok = loadable(h)
    if rate <= 0:
        raise Fail("zero rate")
    if h["magic"] == "SOU0":
        fmt = 0
        slots = payload * OUT_RATE // rate          # FUN_0040d130(len,32000,rate)
    elif h["magic"] == "SOU1":
        fmt = 3 if rate < 10000 else 2 if rate < 18000 else 1
        # fmt 3 -> <<3, fmt 2 -> <<2, fmt 1 -> <<1 (0x40b263..0x40b2b0)
        slots = payload << {3: 3, 2: 2, 1: 1}[fmt]
        if ch == 2:
            slots >>= 1
    else:
        fmt = 4
        slots = h["frames"]
    step = (rate << 32) // OUT_RATE                  # 32.32
    if not ok:
        slots = 0
    return {
        "format": fmt,
        "loadable": ok,
        "size_bytes": h["length_field"],
        "slots": slots,
        "step_hi": step >> 32,
        "step_lo": step & 0xFFFFFFFF,
        "step": step,
        "right_offset": payload // 2 if ch == 2 and ok else 0,
        "seconds": slots / OUT_RATE,
    }


def parse_index(idx_path):
    """BIGFILE index -> [(name, offset, size)] using only the .DAT size."""
    with open(idx_path, "rb") as f:
        head = f.read(12)
        if head[:7] != b"BIGFILE" or head[7] != 0x1A:
            raise Fail("bad BIGFILE index %s" % idx_path)
        count = u32(head, 8)
        if count > 1000000:
            raise Fail("implausible entry count %d" % count)
        raw = f.read(count * 12)
        if len(raw) != count * 12:
            raise Fail("truncated index")
        dat_path = os.path.splitext(idx_path)[0] + ".DAT"
        size = os.path.getsize(dat_path)
        out = []
        for i in range(count):
            name = raw[i * 12:i * 12 + 8].rstrip(b"\0").decode("latin1")
            off = struct.unpack_from("<I", raw, i * 12 + 8)[0]
            end = (struct.unpack_from("<I", raw, (i + 1) * 12 + 8)[0]
                   if i + 1 < count else size)
            if not (0 <= off <= end <= size):
                raise Fail("entry %r range %d..%d outside %d"
                           % (name, off, end, size))
            out.append((name, off, end - off))
        return dat_path, out


def read_header(dat_path, offset, size):
    need = min(size, 64)
    with open(dat_path, "rb") as f:
        f.seek(offset)
        head = f.read(need)
    if len(head) != need:
        raise Fail("cannot read %d header bytes at %d" % (need, offset))
    return head


# ---------------------------------------------------------------------------
# --verify
# ---------------------------------------------------------------------------
EXPECTED = {
    "sounds": {"entries": 999, "SOU0": 13, "SOU1": 983, "SOU2": 3},
    "music": {"entries": 3092, "SOU0": 0, "SOU1": 0, "SOU2": 3092},
}


def verify_archive(source, idx_path, users=(), check_expected=False):
    dat_path, entries = parse_index(idx_path)
    stats = {"source": source, "entries": len(entries), "formats": {},
             "rates": {}, "channels": {}, "slots": 0, "seconds": 0.0,
             "rejected": 0, "errors": [], "examples": {}}
    digest = FNV_OFFSET
    for name, off, size in entries:
        try:
            h = parse_sou(read_header(dat_path, off, size), size)
            r = record_fields(h)
        except Fail as exc:
            stats["errors"].append("%s: %s" % (name, exc))
            continue
        for key, val in (("formats", h["magic"]), ("rates", h["rate"]),
                         ("channels", h["channels"])):
            d = stats[key]
            d[val] = d.get(val, 0) + 1
        stats["slots"] += r["slots"]
        stats["seconds"] += r["seconds"]
        if not r["loadable"]:
            stats.setdefault("rejected", 0)
            stats["rejected"] += 1

        tag = "%s|%s|%d|%d|%d|%d|%08x|%08x|%d" % (
            name, h["magic"], h["rate"], h["channels"], h["payload"],
            r["slots"], r["step_hi"], r["step_lo"], r["right_offset"])
        digest = fnv1a64(tag.encode("ascii") + b"\n", digest)

        err = check_entry(name, h, r)
        if err:
            stats["errors"].append("%s: %s" % (name, err))
        if h["magic"] not in stats["examples"]:
            stats["examples"][h["magic"]] = (name, h, r)

    stats["fnv1a64"] = "0x%016x" % digest
    if check_expected:
        exp = EXPECTED[source]
        if stats["entries"] != exp["entries"]:
            stats["errors"].append("entries %d != %d"
                                   % (stats["entries"], exp["entries"]))
        for magic in ("SOU0", "SOU1", "SOU2"):
            got = stats["formats"].get(magic, 0)
            if got != exp[magic]:
                stats["errors"].append("%s %d != %d" % (magic, got, exp[magic]))
    return stats


def check_entry(name, h, r):
    rate = h["rate"]
    if not r["loadable"]:
        return None          # documented loader rejection, counted separately
    if not (0 < rate <= 192000):
        return "rate %d" % rate
    if r["slots"] <= 0:
        return "zero slots"
    step = (rate << 32) // OUT_RATE
    if r["step"] != step:
        return "step mismatch"
    if r["step_hi"] != rate // OUT_RATE:
        return "step_hi %d != %d" % (r["step_hi"], rate // OUT_RATE)
    if r["step_lo"] != (((rate << 16) // OUT_RATE) << 16) & 0xFFFFFFFF:
        return "step_lo mismatch"
    if h["channels"] == 2 and r["right_offset"] != h["payload"] // 2:
        return "right_offset"
    if h["channels"] == 1 and r["right_offset"] != 0:
        return "mono right_offset"
    if h["magic"] == "SOU0":
        last = ((r["slots"] - 1) * rate) // OUT_RATE
        if not (0 <= last < h["payload"] and last >= h["payload"] - 1):
            return "SOU0 step does not cover payload (%d/%d)" % (
                last, h["payload"])
    elif h["magic"] == "SOU1":
        if OUT_RATE % rate:
            return "SOU1 rate %d does not divide 32000" % rate
        frames = 2 * h["payload"] // h["channels"]
        if r["slots"] != frames * (OUT_RATE // rate):
            return "SOU1 slots %d != %d" % (
                r["slots"], frames * (OUT_RATE // rate))
        if h["flags"] > 1:
            return "flags %d" % h["flags"]
    else:
        if h["frames"] <= 0 or h["frames"] % 32:
            return "SOU2 frames %d" % h["frames"]
        if r["slots"] != h["frames"]:
            return "SOU2 slots"
        if h["channels"] != 2:
            return "SOU2 channels"
    return None


def print_verify(stats, out=sys.stdout):
    out.write("%s: %d записей; форматы %s; каналы %s; частоты %s\n" % (
        stats["source"], stats["entries"],
        ", ".join("%s=%d" % kv for kv in sorted(stats["formats"].items())),
        ", ".join("%s=%d" % kv for kv in sorted(stats["channels"].items())),
        ", ".join("%d=%d" % kv for kv in sorted(stats["rates"].items()))))
    out.write("  слотов 32 кГц: %d (%.3f с); отвергнуто загрузчиком: %d; "
              "FNV-1a64 карты полей: %s\n" % (
                  stats["slots"], stats["seconds"], stats["rejected"],
                  stats["fnv1a64"]))
    for magic, (name, h, r) in sorted(stats["examples"].items()):
        out.write("  %s: %-8s rate=%d ch=%d payload=%d slots=%d "
                  "step=0x%08x:%08x fmt=%d right_off=%d (%.3f с)\n" % (
                      magic, name, h["rate"], h["channels"], h["payload"],
                      r["slots"], r["step_hi"], r["step_lo"], r["format"],
                      r["right_offset"], r["seconds"]))
    if stats["errors"]:
        out.write("  ОШИБКИ (%d):\n" % len(stats["errors"]))
        for err in stats["errors"][:20]:
            out.write("    %s\n" % err)
        if len(stats["errors"]) > 20:
            out.write("    ... ещё %d\n" % (len(stats["errors"]) - 20))
    else:
        out.write("  OK\n")


def verify_all(game_dir):
    results = []
    for source, rel in (("sounds", "sounds/SOU.IDX"),
                        ("music", "MUSIC/SOU.IDX")):
        idx = os.path.join(game_dir, rel)
        if not os.path.isfile(idx):
            print("NOTE: %s нет, архив пропущен" % idx)
            continue
        results.append(verify_archive(source, idx))
    return results


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------
class _Check:
    def __init__(self):
        self.count = 0
        self.failed = []

    def __call__(self, cond, what):
        self.count += 1
        if not cond:
            self.failed.append(what)
            print("FAIL: %s" % what)


def _hdr(magic, rate, flags=0, length=0, frames=0, units=0):
    if magic == "SOU2":
        return (magic.encode() + struct.pack("<IIII", rate, frames, 0, units)
                + b"\0" * (units * 4))
    return (magic.encode() + struct.pack("<III", rate, flags, length)
            + b"\0" * (length & ~1))


def map_selftest(c):
    s, v = MAPS["sample"], MAPS["voice"]
    c(s["stride"] == 28 and s["count"] == 300, "sample: шаг/число")
    c(s["end"] == 0x83FA30, "sample: конец 0x83fa30")
    c(v["stride"] == 0x50 and v["count"] == 9, "voice: шаг/число")
    c(v["end"] == 0x84E338, "voice: конец 0x84e338")
    c(EFFECT_VOICES == 8, "voice: микшер обходит 8")
    for m in (s, v, MAPS["request"], MAPS["queue3d"]):
        offs = [f[0] for f in m["fields"]]
        c(offs == sorted(offs) and len(set(offs)) == len(offs),
          "%s: смещения возрастают и уникальны" % m["title"])
        c(all(o + 4 <= m["stride"] for o in offs),
          "%s: поле не выходит за запись" % m["title"])
    c(s["fields"][-1][0] == 0x18 and v["fields"][-1][0] == 0x4C,
      "последние поля на месте")


def formula_selftest(c):
    cases = [
        # (magic, rate, flags, length, frames, units, fmt, slots, hi, lo)
        ("SOU0", 8000, 0, 4, 0, 0, 0, 16, 0, 0x40000000),
        ("SOU0", 16000, 0, 4, 0, 0, 0, 8, 0, 0x80000000),
        ("SOU1", 8000, 0, 2, 0, 0, 3, 16, 0, 0x40000000),
        ("SOU1", 16000, 0, 2, 0, 0, 2, 8, 0, 0x80000000),
        ("SOU1", 32000, 0, 2, 0, 0, 1, 4, 1, 0),
        ("SOU1", 32000, 1, 4, 0, 0, 1, 4, 1, 0),   # stereo: len<<1>>1
        ("SOU2", 32000, 1, 64, 64, 8, 4, 64, 1, 0),
    ]
    for magic, rate, flags, length, frames, units, fmt, slots, hi, lo in cases:
        blob = _hdr(magic, rate, flags, length, frames=frames, units=units)
        h = parse_sou(blob)
        r = record_fields(h)
        tag = "%s %d" % (magic, rate)
        c(r["format"] == fmt, "%s: format %d != %d" % (tag, r["format"], fmt))
        c(r["slots"] == slots, "%s: slots %d != %d" % (tag, r["slots"], slots))
        c(r["step_hi"] == hi and r["step_lo"] == lo,
          "%s: step %08x:%08x" % (tag, r["step_hi"], r["step_lo"]))
        c((r["step_hi"] << 32 | r["step_lo"]) == (rate << 32) // OUT_RATE,
          "%s: 32.32 step" % tag)
        c(check_entry("synthetic", h, r) is None,
          "%s: check_entry отверг синтетику" % tag)
        if flags & 1 and magic != "SOU2":
            c(r["right_offset"] == h["payload"] // 2, "%s: right_offset" % tag)
    # negative / trust boundary
    bad = [b"", b"SOU", b"XXXX" + b"\0" * 12, b"SOU1" + b"\0" * 4,
           _hdr("SOU1", 8000, 0, 4)[:-2]]
    for blob in bad:
        try:
            parse_sou(blob)
        except Fail:
            pass
        else:
            c(False, "parse_sou принял битый заголовок %r" % blob[:8])
    c(parse_sou(_hdr("SOU1", 8000, 0, 5))["payload"] == 4,
      "odd length & ~1")
    rej = parse_sou(b"SOU2" + struct.pack("<IIII", 32000, 33, 0, 1) + b"\0" * 4)
    c(record_fields(rej)["loadable"] is False and
      check_entry("x", rej, record_fields(rej)) is None,
      "SOU2 frames % 32 должен отвергаться загрузчиком")


def real_selftest(c, game_dir):
    found = False
    for source, rel in (("sounds", "sounds/SOU.IDX"),
                        ("music", "MUSIC/SOU.IDX")):
        idx = os.path.join(game_dir, rel)
        if not os.path.isfile(idx):
            continue
        found = True
        st = verify_archive(source, idx, check_expected=True)
        c(not st["errors"], "%s: %s" % (source, st["errors"][:3]))
        exp = EXPECTED[source]
        c(st["entries"] == exp["entries"], "%s: entries" % source)
        if source == "sounds":
            c(st["formats"].get("SOU0") == 13, "sounds: 13 SOU0")
            c(st["formats"].get("SOU1") == 983, "sounds: 983 SOU1")
            c(st["formats"].get("SOU2") == 3, "sounds: 3 SOU2")
            c(st["channels"].get(2) == 9, "sounds: 9 stereo (6 SOU1 + 3 SOU2)")
            c(st["rates"].get(8000) == 364 and st["rates"].get(16000) == 617,
              "sounds: частоты 8000/16000")
        else:
            c(st["formats"].get("SOU2") == 3092, "music: 3092 SOU2")
            c(st["rejected"] == 2, "music: 2 записи отвергает загрузчик")
        print("  %s: %d записей, FNV %s" % (source, st["entries"],
                                            st["fnv1a64"]))
    if not found:
        print("NOTE: поставочных архивов нет (%s), реальные файлы пропущены"
              % game_dir)


def self_test(game_dir):
    c = _Check()
    map_selftest(c)
    formula_selftest(c)
    real_selftest(c, game_dir)
    print("\nself-test: %s (%d проверок)"
          % ("OK" if not c.failed else "FAIL", c.count))
    return 0 if not c.failed else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--map", action="store_true",
                    help="печать карт (по умолчанию)")
    ap.add_argument("--verify", action="store_true",
                    help="сверить шапки всех .sou с формулами загрузчика")
    ap.add_argument("--self-test", action="store_true", help="встроенные тесты")
    ap.add_argument("--json", action="store_true", help="вывод --verify в JSON")
    ap.add_argument("--dir", default=os.environ.get("GUNMETAL_GAME",
                                                    DEFAULT_DIR),
                    help="каталог игры")
    opts = ap.parse_args(argv)

    if opts.self_test:
        try:
            return self_test(opts.dir)
        except Exception as exc:                      # noqa: BLE001
            print("FAIL: непредвиденная ошибка self-test: %r" % (exc,))
            return 1

    status = 0
    if opts.verify:
        try:
            results = verify_all(opts.dir)
        except (Fail, OSError) as exc:
            print("FAIL: %s" % exc)
            return 1
        if opts.json:
            print(json.dumps({"archives": results,
                              "maps": {k: _fields_json(v["fields"])
                                       for k, v in MAPS.items()}},
                             ensure_ascii=False, indent=1))
        else:
            for st in results:
                print_verify(st)
        status = 1 if any(st["errors"] for st in results) or not results else 0
        if not results:
            print("FAIL: ни одного архива не найдено в %s" % opts.dir)

    if opts.map or (not opts.verify and not opts.self_test):
        for key in ("sample", "voice", "request", "queue3d"):
            print_map(key)
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
