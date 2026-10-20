#!/usr/bin/env python3
"""Python ↔ C++ parity for the SOU2 music decoder.

The reference hashes come from ``engine/tests/test_sou2.cpp`` (which pins the
C++ decoder against the engine code under the x86 emulator) and from a full
3092-entry run of that test.

    python3 tools/parity_sou2.py --fast    # 6 records, <1 s (regress)
    python3 tools/parity_sou2.py --quick   # 24 pinned records, ~12 s
    python3 tools/parity_sou2.py           # all 3092, ~8 min

The full run hashes 448 million samples in pure Python; it is meant for a
final check, not the inner loop.  The quick set covers every decoder path
(clean/fractional level 2, clean/fractional level 3, silence, unknown level,
odd 16 kHz records, 0-frame and units=0 anomalies).
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from gm import bigfile, sou2  # noqa: E402

FNV_BASIS = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = 0xFFFFFFFFFFFFFFFF

#: Full-archive aggregate from the C++ `sou2_full` test.
FULL = {
    "raw": 0x01F9D974BAF293E4,
    "pcm": 0x2FD47D85665DC344,
    "nonzero": 309885750,
    "max_abs": 778304,
}

#: The subset used by the regression's quick pass.  Values are the C++
#: `RecordHash` entries in engine/tests/test_sou2.cpp.
FAST = {
    "DM1A01": (0x1FC8FED7652EF776, 0x57AE8E94B96636E0),
    "L01BONUS": (0xBD5A4EE701D61B27, 0x891A3DA1690A5280),
    "LOADSCRN": (0xC076E713AAD8453A, 0xAAE23106CCA4C23E),
    "L04I01": (0x14650FB0739D0383, 0x14650FB0739D0383),
    "L06A01": (0x47FE0D7EAF8E51E3, 0x315446A086A23133),
    "L20I06": (0x40AA142202D128FE, 0x3E547367CB77BFAA),
}


def fnv_u32(h: int, v: int) -> int:
    for i in range(4):
        h = ((h ^ ((v >> (8 * i)) & 0xFF)) * FNV_PRIME) & MASK
    return h


def fnv_u16(h: int, v: int) -> int:
    h = ((h ^ (v & 0xFF)) * FNV_PRIME) & MASK
    return ((h ^ (v >> 8)) * FNV_PRIME) & MASK


def cpp_hashes() -> dict[str, tuple[int, int]]:
    text = (ROOT / "engine" / "tests" / "test_sou2.cpp").read_text()
    return {name: (int(raw, 16), int(pcm, 16))
            for name, raw, pcm in re.findall(
                r'\{"([A-Z0-9_]+)",\s*0x([0-9a-f]+)ull,\s*0x([0-9a-f]+)ull\}', text)}


def hash_samples(raw, rawh: int = FNV_BASIS,
                 pcmh: int = FNV_BASIS) -> tuple[int, int, int, int]:
    nonzero = 0
    max_abs = 0
    p = FNV_PRIME
    m = MASK
    for v in raw:
        u = v & 0xFFFFFFFF
        rawh = ((rawh ^ (u & 0xFF)) * p) & m
        u >>= 8
        rawh = ((rawh ^ (u & 0xFF)) * p) & m
        u >>= 8
        rawh = ((rawh ^ (u & 0xFF)) * p) & m
        rawh = ((rawh ^ (u >> 8)) * p) & m
        a = -v if v < 0 else v
        if a > max_abs:
            max_abs = a
        x = v >> 1
        if x > 32767:
            x = 32767
        elif x < -32768:
            x = -32768
        pcmh = ((pcmh ^ (x & 0xFF)) * p) & m
        pcmh = ((pcmh ^ ((x >> 8) & 0xFF)) * p) & m
        if x:
            nonzero += 1
    return rawh, pcmh, nonzero, max_abs


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--idx", help="music .IDX (default: <game>/MUSIC/SOU.IDX)")
    ap.add_argument("--fast", action="store_true", help="6 records only")
    ap.add_argument("--quick", action="store_true", help="24 pinned records")
    ap.add_argument("--volume", type=int, default=sou2.MAX_VOLUME)
    opts = ap.parse_args(argv)
    idx = opts.idx or str(pathlib.Path(opts.game) / "MUSIC" / "SOU.IDX")
    if not pathlib.Path(idx).exists():
        print(f"parity_sou2: not found: {idx}", file=sys.stderr)
        return 2
    bf = bigfile.BigFile(idx)

    if opts.fast:
        refs = FAST
        selected = set(refs)
        aggregate = False
    elif opts.quick:
        refs = cpp_hashes()
        selected = set(refs)
        aggregate = False
    else:
        refs = cpp_hashes()
        selected = {e.name for e in bf.entries}
        aggregate = True

    t0 = time.time()
    rawh = pcmh = FNV_BASIS
    nonzero = max_abs = frames = 0
    decoded = 0
    failures: list[str] = []
    for e in bf.entries:
        if e.name not in selected:
            continue
        try:
            _rate, raw = sou2.decode_sou2_raw(bf.read(e), opts.volume)
        except ValueError as exc:
            failures.append(f"{e.name}: {exc}")
            continue
        decoded += 1
        frames += len(raw) // 2
        r, p, nz, mx = hash_samples(raw, rawh if aggregate else FNV_BASIS,
                                    pcmh if aggregate else FNV_BASIS)
        if not aggregate and e.name in refs:
            want_r, want_p = refs[e.name]
            if (r, p) != (want_r, want_p):
                failures.append(f"{e.name}: raw {r:016x} (want {want_r:016x}), "
                                f"pcm {p:016x} (want {want_p:016x})")
        if aggregate:
            rawh, pcmh = r, p
            nonzero += nz
            max_abs = max(max_abs, mx)

    if aggregate:
        if (rawh, pcmh, nonzero, max_abs) != (
                FULL["raw"], FULL["pcm"], FULL["nonzero"], FULL["max_abs"]):
            failures.append(
                f"aggregate: raw {rawh:016x}/{FULL['raw']:016x} "
                f"pcm {pcmh:016x}/{FULL['pcm']:016x} "
                f"nonzero {nonzero}/{FULL['nonzero']} max {max_abs}/{FULL['max_abs']}")
        expect = 3092
    else:
        expect = len(refs)
    if decoded != expect:
        failures.append(f"decoded {decoded} entries, expected {expect}")

    for line in failures:
        print(f"  {line}")
    mode = "full" if aggregate else ("quick" if opts.quick else "fast")
    print(f"parity_sou2[{mode}]: {decoded} records, {frames} frames, "
          f"{len(failures)} failure(s), {time.time() - t0:.1f}s")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
