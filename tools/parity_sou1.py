#!/usr/bin/env python3
"""Check ``gm/sou1.py`` against the verified SOU1 FNV manifest.

The manifest (``docs/manifests/sou1-fnv.json``) was produced by the independent
model/emulator comparison in ``tools/re/sou1_model.py``: all 983 entries matched
the real decoder bytes under x86 emulation.  This script re-decodes the shipped
corpus through the package-facing decoder and pins the same hashes.

    python3 tools/parity_sou1.py --fast   # 6 records, ~1 s (regress)
    python3 tools/parity_sou1.py          # all 983, ~20-30 s
"""

from __future__ import annotations

import argparse
import array
import json
import pathlib
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from gm import bigfile, sou1  # noqa: E402

FAST = ("SILENCE", "INTRO", "LOADSCRN", "PRAM", "SOYBOY", "DRONE")


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def interleave(streams) -> array:
    if len(streams) == 1:
        return streams[0]
    out = array.array("I")
    left, right = streams
    for i in range(len(left)):
        out.append(left[i])
        out.append(right[i])
    return out


def pcm16_bytes(interleaved) -> bytes:
    out = bytearray()
    for s in interleaved:
        v = s - 0x100000000 if s & 0x80000000 else s
        v >>= sou1.PCM16_SHIFT
        v = 32767 if v > 32767 else (-32768 if v < -32768 else v)
        out += struct.pack("<h", v)
    return bytes(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--manifest", default=str(ROOT / "docs" / "manifests" /
                                             "sou1-fnv.json"))
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--limit", type=int)
    opts = ap.parse_args(argv)

    manifest = json.loads(pathlib.Path(opts.manifest).read_text())
    refs = {r["name"]: r for r in manifest["entries"]}
    idx = pathlib.Path(opts.game) / "sounds" / "SOU.IDX"
    if not idx.exists():
        print(f"parity_sou1: not found: {idx}", file=sys.stderr)
        return 2
    bf = bigfile.BigFile(str(idx))
    entries = [e for e in bf.entries if bf.read(e)[:4] == b"SOU1"]
    if opts.fast:
        wanted = set(FAST) & set(refs)
        entries = [e for e in entries if e.name in wanted]
    if opts.limit:
        entries = entries[:opts.limit]

    t0 = time.time()
    bad = 0
    slots = 0
    for e in entries:
        blob = bf.read(e)
        try:
            rate, channels, streams = sou1.decode_entry(blob)
        except ValueError as exc:
            print(f"FAIL {e.name}: {exc}")
            bad += 1
            continue
        inter = interleave(streams)
        slots += len(inter)
        ref = refs.get(e.name)
        if ref is None:
            print(f"FAIL {e.name}: missing from manifest")
            bad += 1
            continue
        raw_h = fnv1a64(inter.tobytes())
        pcm_h = fnv1a64(pcm16_bytes(inter))
        if (raw_h != int(ref["fnv1a64"], 16)
                or pcm_h != int(ref["fnv1a64_pcm16"], 16)
                or rate != ref["rate"] or channels != ref["channels"]):
            print(f"FAIL {e.name}: raw {raw_h:016x}/{ref['fnv1a64']} "
                  f"pcm {pcm_h:016x}/{ref['fnv1a64_pcm16']} "
                  f"{rate}Hz x{channels}")
            bad += 1

    if not opts.fast and not opts.limit:
        missing = set(refs) - {e.name for e in entries}
        if missing:
            print(f"FAIL: {len(missing)} manifest entries not in the archive")
            bad += 1

    mode = "fast" if opts.fast else "full"
    print(f"parity_sou1[{mode}]: {len(entries)} records, {slots} slots, "
          f"{bad} failure(s), {time.time() - t0:.1f}s")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
