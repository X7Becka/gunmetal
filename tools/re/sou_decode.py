"""Decode a ``SOU1`` sample by running the game's own decoder (research tool).

The codec lives in ``gundx.exe``'s ``adpcmseg`` section and is *self-modifying*
(``mov [0x852ba3], edi`` patches a table pointer into an instruction), so this
tool emulates the real bytes with :mod:`tools.re.x86mini` instead of guessing.

Three decoders exist, picked by the sample rate in the loader (0x40b246):

    rate < 10000  -> 0x852cc0, 1 bit/sample-ish,  4 output slots per code
    rate < 18000  -> 0x852c00, 2 bit/sample-ish,  2 output slots per code
    otherwise     -> 0x852b40, 4 bit/sample-ish,  1 output slot per code

``count`` passed to the decoder is ``payload_bytes * (2, 4, 8)``; the routine
consumes one byte per iteration and emits two DPCM-coded samples (nibbles)
with 1/2/4 copies each.

``SOU2`` (music) uses the bit-packed decoder at 0x852d80 and is NOT handled
here yet.

Usage:
    python3 tools/re/sou_decode.py INTRO out.wav [--vol 31]
    python3 tools/re/sou_decode.py --list
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

import exelib  # noqa: E402
import x86mini  # noqa: E402
from gm import bigfile  # noqa: E402

DECODERS = {
    10000: (0x852cc0, 8),   # 1-bit class
    18000: (0x852c00, 4),   # 2-bit class
    1 << 30: (0x852b40, 2),  # 4-bit class
}


def load_cpu() -> x86mini.CPU:
    cpu = x86mini.CPU()
    data = exelib.data()
    for _name, va, rawsize, off in exelib.sections():
        cpu.load(va, data[off:off + rawsize])
    return cpu


def decoder_for(rate: int) -> tuple[int, int]:
    for limit, spec in sorted(DECODERS.items()):
        if rate < limit:
            return spec
    return DECODERS[1 << 30]


def decode(payload: bytes, rate: int, volume: int = 31) -> list[int]:
    cpu = load_cpu()
    func, factor = decoder_for(rate)
    cpu.wr8(0x84A6A8, volume)
    cpu.load(0x200000, payload)
    cpu.wr32(0x400000, 0)
    cpu.wr32(0x400004, 0)
    count = len(payload) * factor
    # 4 bytes per output slot
    need = count * 4
    base = 0x300000
    if base + need >= x86mini.MEM_SIZE:
        raise MemoryError("sample too large for the flat test memory")
    cpu.call(func, [0x200000, base, count, 0x400000], limit=count * 40 + 100000)
    raw = cpu.read(base, need)
    return list(struct.unpack("<%di" % (need // 4), raw))


def read_sou(name: str, game: str) -> tuple[int, int, bytes]:
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    entry = bf[name]
    data = bf.read(entry)
    magic = data[:4]
    if magic not in (b"SOU0", b"SOU1", b"SOU2"):
        raise ValueError("%s: unknown magic %r" % (name, magic))
    rate, flags = struct.unpack_from("<II", data, 4)
    if magic == b"SOU2":
        raise SystemExit("%s: SOU2 music is not decoded yet" % name)
    length = struct.unpack_from("<I", data, 12)[0]
    return rate, flags, data[16:16 + length]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("name", nargs="?", help="sample name inside sounds/SOU.IDX")
    ap.add_argument("out", nargs="?", help="output .wav")
    ap.add_argument("--game", default="/mnt/d/games/Gunmetal 3dfx")
    ap.add_argument("--vol", type=int, default=31, help="decoder table 0..31")
    ap.add_argument("--list", action="store_true")
    opts = ap.parse_args(argv)

    if opts.list:
        bf = bigfile.BigFile(os.path.join(opts.game, "sounds", "SOU.IDX"))
        for e in bf.entries:
            data = bf.read(e)[:4]
            if data == b"SOU1":
                print(e.name)
        return 0

    if not opts.name or not opts.out:
        ap.error("NAME and OUT are required unless --list is used")

    rate, flags, payload = read_sou(opts.name, opts.game)
    samples = decode(payload, rate, opts.vol)
    clipped = [max(-32768, min(32767, s >> 1)) for s in samples]
    with wave.open(opts.out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate // (2 if flags else 1))
        wav.writeframes(struct.pack("<%dh" % len(clipped), *clipped))
    print("%s: rate=%d stereo=%d payload=%d samples=%d -> %s"
          % (opts.name, rate, flags, len(payload), len(samples), opts.out))
    print("  range %d..%d" % (min(samples), max(samples)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
