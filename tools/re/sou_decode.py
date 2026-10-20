"""Decode a ``SOU1`` sample by running the game's own decoder (research tool).

The codec lives in ``gundx.exe``'s ``adpcmseg`` section and is *self-modifying*
(``mov [0x852ba3], edi`` patches a table pointer into an instruction), so this
tool emulates the real bytes with :mod:`tools.re.x86mini` instead of guessing.
The independent reimplementation lives in :mod:`tools.re.sou1_model`; its
``--verify`` mode compares both on the whole corpus.

Three decoders exist, picked by the sample rate in the loader (0x40b246):

    rate < 10000  -> 0x852cc0, 4 copies per code, 8 output slots per byte
    rate < 18000  -> 0x852c00, 2 copies per code, 4 output slots per byte
    otherwise     -> 0x852b40, 1 copy  per code, 2 output slots per byte

``count`` passed to the decoder is the number of output slots; each iteration
consumes one payload byte, emits two nibble-coded samples and then
``count >> (3|2|1)`` iterations are run.  The decoder **adds** into the
destination buffer and keeps its 8-byte ``{acc, level}`` state in memory, so
the harness below zeroes the output chunk and keeps the state outside both the
source and the destination.

Historical note: the previous harness put the destination at ``0x300000`` and
the state at ``0x400000``.  For ``count > 0x40000`` slots the output reached
``0x400000`` and overwrote the state (first wrong sample at slot 262144), which
looked like "long tracks diverge".  It was a harness bug, not the codec.

Usage:
    python3 tools/re/sou_decode.py INTRO out.wav [--vol 31]
    python3 tools/re/sou_decode.py --list
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

import sou1_model  # noqa: E402
from gm import bigfile  # noqa: E402


def decode(payload: bytes, rate: int, volume: int = 31) -> list[int]:
    """Emulate the shipped decoder bytes; returns signed 32-bit slots."""
    raw = sou1_model.decode_stream_emu(payload, rate, volume)
    return list(struct.unpack("<%di" % (len(raw) // 4), raw))


def read_sou(name: str, game: str) -> tuple[int, int, bytes]:
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    data = bf.read(bf[name])
    if data[:4] == b"SOU2":
        raise SystemExit("%s: SOU2 music is not handled here" % name)
    rate, flags, payload = sou1_model.parse_entry(data)
    return rate, flags, payload


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("name", nargs="?", help="sample name inside sounds/SOU.IDX")
    ap.add_argument("out", nargs="?", help="output .wav")
    ap.add_argument("--game", default=os.environ.get(
        "GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
    ap.add_argument("--vol", type=int, default=31, help="decoder table 0..31")
    ap.add_argument("--list", action="store_true")
    opts = ap.parse_args(argv)

    if opts.list:
        for name in sou1_model.list_entries(opts.game):
            print(name)
        return 0

    if not opts.name or not opts.out:
        ap.error("NAME and OUT are required unless --list is used")

    data = bigfile.BigFile(os.path.join(opts.game, "sounds", "SOU.IDX"))
    blob = data.read(data[opts.name])
    rate, flags, payload = sou1_model.parse_entry(blob)
    if flags & 1:
        left, right = sou1_model.deinterleave_stereo(payload)
        payload_streams = [left, right]
        channels = 2
    else:
        payload_streams = [payload]
        channels = 1
    streams = [sou1_model.decode_stream(p, rate, opts.vol)
               for p in payload_streams]

    # The emulator path is the ground truth used by --verify; decode through it
    # as well so this CLI also exercises the real bytes.
    for ch, stream in enumerate(payload_streams):
        raw = sou1_model.decode_stream_emu(stream, rate, opts.vol)
        if raw != streams[ch].tobytes():
            raise SystemExit("%s: emulator/model mismatch on channel %d"
                             % (opts.name, ch))

    sou1_model.write_wav(opts.out, rate, channels, streams)
    slots = len(streams[0])
    print("%s: rate=%d channels=%d payload=%d slots/ch=%d (%.3f s) -> %s"
          % (opts.name, rate, channels, len(payload), slots,
             slots / sou1_model.RATE_OUT, opts.out))
    for ch, stream in enumerate(streams):
        lo, hi = sou1_model.signed_range(stream)
        print("  channel %d raw accumulator range %d..%d" % (ch, lo, hi))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
