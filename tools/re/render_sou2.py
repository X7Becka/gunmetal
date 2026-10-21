"""Render a SOU2 music track to a listenable WAV with the game's synthesis filter.

The bitstream decoder ``0x852d80`` does **not** produce PCM.  It writes 13
multiplexed sub-band streams into a 1024-frame block; the game's mixer
``0x409330`` then reloads a 0x19d-dword (413-sample) history from the previous
block, copies a 0x184-dword (388-sample) lookahead from the next block, and
calls the polyphase synthesis filter ``0x8534c0``.  Playing the raw decoder
output (the old ``clamp(raw >> 1)`` path in ``gm/sou2.py``) is what sounds like
crackle/noise.

This tool runs the shipped ``0x8534c0`` under :mod:`tools.re.x86mini`, so the
filter coefficients and fixed-point arithmetic are the original ones.  Only
the decoder front-end is the independent Python model from ``gm/sou2.py``.

The decoder/synthesis stream is always a **32 kHz** stream in the original
mixer (the DirectSound music buffer is created with 32000 Hz).  The ``rate``
field in a few SOU2 headers (``L01BONUS``, ``LOADSCRN``) says 16000, but the
loader's rate step is never read by the music mixer: it feeds 1024 decoded
frames into the 32 kHz ring every callback.  Therefore those tracks must be
written at 32000 Hz as well; writing them at the header rate makes them an
octave too low and twice as long.  The tool defaults to 32000 Hz; do not
"restore" the header rate.

Usage::

    python3 tools/re/render_sou2.py L01BONUS /tmp/L01BONUS.wav
    python3 tools/re/render_sou2.py L01A01 /tmp/L01A01.wav --seconds 2
    python3 tools/re/render_sou2.py L01BONUS /tmp/L01BONUS.wav \
        --out-rate 32000 --loop --seconds 3

``--loop`` wraps the decoded track at its loop point (including the 413-sample
history and 388-sample lookahead across the seam), matching the mixer's
playlist loop for a one-track ``.sog`` song.

``GUNMETAL_EXE`` selects the executable; ``GUNMETAL_GAME`` selects the
installation used for ``MUSIC/SOU.IDX``.
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
from gm import bigfile, sou2  # noqa: E402

FILTER_VA = 0x8534C0
HISTORY_LEN = 0x19D          # ints copied by the mixer (0x409755/0x409797)
LOOKAHEAD_LEN = 0x184        # ints copied by the mixer (0x409ae1/0x409b01)
LEFT_STATE_VA = 0x84A6C0
RIGHT_STATE_VA = 0x84C3E4
LEFT_DATA_VA = 0x84AD34
RIGHT_DATA_VA = 0x84CA58
LEFT_LOOKAHEAD_VA = 0x84BD34
RIGHT_LOOKAHEAD_VA = 0x84DA58
AUDIO_STATE_LO = 0x84A000
AUDIO_STATE_HI = 0x84E000


def load_cpu() -> x86mini.CPU:
    cpu = x86mini.CPU()
    image = exelib.data()
    for _name, va, rawsize, off in exelib.sections():
        cpu.load(va, image[off:off + rawsize])
    return cpu


def render(blob: bytes, out_rate: int = 32000,
           loop_seconds: float = 0.0) -> tuple[int, list[int], list[int]]:
    """Decode *blob* and synthesize both channels.

    Returns ``(rate, L, R)``.  The original music mixer always runs at 32000 Hz,
    so that is the default even for the two tracks whose SOU2 header says 16000
    (``L01BONUS``, ``LOADSCRN``); pass ``out_rate`` only for an experiment.
    With ``loop_seconds > 0`` the decoded track is treated as a circular loop:
    the first block uses the last 413 frames as history and the last block
    looks ahead into the first 388 frames, exactly like the mixer's
    single-track playlist loop.
    """
    rate, raw = sou2.decode_sou2_raw(blob, sou2.MAX_VOLUME)
    frames = len(raw) // 2
    if not out_rate:
        out_rate = 32000
    if frames <= 0:
        return out_rate, [], []
    loop = loop_seconds > 0.0
    total = frames
    if loop:
        total = max(frames, int(round(loop_seconds * out_rate)))
    cpu = load_cpu()
    out_l: list[int] = []
    out_r: list[int] = []
    prev_l = prev_r = None

    def src(sample: int, channel: int) -> int:
        idx = sample % frames
        return raw[2 * idx + channel]

    for start in range(0, total, 1024):
        rem = min(1024, total - start)
        if loop:
            inp_l = [src(start + i, 0) for i in range(1024)]
            inp_r = [src(start + i, 1) for i in range(1024)]
        else:
            inp_l = [src(start + i, 0) if i < rem else 0 for i in range(1024)]
            inp_r = [src(start + i, 1) if i < rem else 0 for i in range(1024)]

        # The mixer resets the filter scratch area, reloads the 413-sample
        # overlap history from the previous block, and copies the first 388
        # samples of the next block as lookahead.
        cpu.mem[AUDIO_STATE_LO:AUDIO_STATE_HI] = bytes(AUDIO_STATE_HI - AUDIO_STATE_LO)
        if prev_l is not None:
            for i in range(HISTORY_LEN):
                cpu.wr32(LEFT_STATE_VA + 4 * i, prev_l[-HISTORY_LEN + i])
                cpu.wr32(RIGHT_STATE_VA + 4 * i, prev_r[-HISTORY_LEN + i])
        elif loop:
            for i in range(HISTORY_LEN):
                cpu.wr32(LEFT_STATE_VA + 4 * i, src(frames - HISTORY_LEN + i, 0))
                cpu.wr32(RIGHT_STATE_VA + 4 * i, src(frames - HISTORY_LEN + i, 1))
        for i in range(1024):
            cpu.wr32(LEFT_DATA_VA + 4 * i, inp_l[i])
            cpu.wr32(RIGHT_DATA_VA + 4 * i, inp_r[i])

        nstart = start + 1024
        if loop:
            for i in range(LOOKAHEAD_LEN):
                cpu.wr32(LEFT_LOOKAHEAD_VA + 4 * i, src(nstart + i, 0))
                cpu.wr32(RIGHT_LOOKAHEAD_VA + 4 * i, src(nstart + i, 1))
        elif nstart < frames:
            nrem = min(LOOKAHEAD_LEN, frames - nstart)
            for i in range(LOOKAHEAD_LEN):
                cpu.wr32(LEFT_LOOKAHEAD_VA + 4 * i,
                         src(nstart + i, 0) if i < nrem else 0)
                cpu.wr32(RIGHT_LOOKAHEAD_VA + 4 * i,
                         src(nstart + i, 1) if i < nrem else 0)

        cpu.call(FILTER_VA, [], limit=10_000_000)
        for i in range(rem):
            out_l.append(struct.unpack("<i", cpu.read(LEFT_DATA_VA + 4 * i, 4))[0])
            out_r.append(struct.unpack("<i", cpu.read(RIGHT_DATA_VA + 4 * i, 4))[0])
        prev_l, prev_r = inp_l, inp_r

    return out_rate, out_l, out_r


def write_wav(path: str, rate: int, left: list[int], right: list[int],
              peak: float = 0.9) -> None:
    top = max(max(map(abs, left), default=0), max(map(abs, right), default=0))
    scale = (peak * 32767 / top) if top else 0.0
    pcm = bytearray()
    for l_val, r_val in zip(left, right):
        for value in (l_val, r_val):
            sample = int(round(value * scale))
            pcm += struct.pack("<h", max(-32768, min(32767, sample)))
    with wave.open(path, "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(pcm))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("name", help="track name inside MUSIC/SOU.IDX")
    ap.add_argument("out", help="output .wav")
    ap.add_argument("--game", default=os.environ.get(
        "GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="cut after N seconds (0 = whole track)")
    ap.add_argument("--out-rate", type=int, default=32000,
                    help="output rate (default: 32000, the game music mixer; "
                         "SOU2 header 16000 is not a playback rate)")
    ap.add_argument("--loop", action="store_true",
                    help="loop the track to --seconds (game single-song loop)")
    ap.add_argument("--peak", type=float, default=0.9,
                    help="normalization peak (default 0.9)")
    opts = ap.parse_args(argv)

    bf = bigfile.BigFile(os.path.join(opts.game, "MUSIC", "SOU.IDX"))
    data = bf.read(bf[opts.name])
    rate, left, right = render(data, out_rate=opts.out_rate,
                               loop_seconds=opts.seconds if opts.loop else 0.0)
    if opts.seconds > 0 and not opts.loop:
        n = min(len(left), int(round(opts.seconds * rate)))
        left, right = left[:n], right[:n]
    write_wav(opts.out, rate, left, right, opts.peak)
    print("%s: rate=%d frames=%d (%.3f s) -> %s"
          % (opts.name, rate, len(left), len(left) / rate, opts.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
