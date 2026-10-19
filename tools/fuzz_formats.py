#!/usr/bin/env python3
"""Robustness harness for the binary format parsers in ``gm/``.

The toolkit consumes files of unknown provenance, so every parser must fail
*cleanly* (a Python exception, never a crash, hang or multi-gigabyte
allocation) on empty, truncated and mutated input.  This script feeds the
shipped corpus through that hostile matrix and reports any case that survives
with the wrong failure mode.

It is deliberately in-process and stdlib-only: a segfault in a pure-Python
parser would be a real portability bug, and ``signal.setitimer`` bounds the
wall time of every single case (2 s by default) so an accidental infinite
loop cannot wedge the run.

Usage::

    python3 tools/fuzz_formats.py                 # full matrix
    python3 tools/fuzz_formats.py --quick         # ~1/4 of the mutations
    python3 tools/fuzz_formats.py --game /path/to/"Gunmetal 3dfx" --seed 7

Exit status is 0 when every malformed case raised a normal exception and 1 if
a case hung, raised ``MemoryError``/``RecursionError`` or otherwise escaped the
expected error set.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import random
import signal
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from gm import bigfile, sbc, sbm, sbt, sbv, sbw  # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_DIR", "/mnt/d/games/Gunmetal 3dfx")

#: Exceptions a parser is allowed to raise for malformed input.  ``OSError``
#: covers a temp file disappearing, ``struct.error`` is normalised away in the
#: newer parsers but still allowed here.
EXPECTED = (ValueError, struct.error, IndexError, KeyError, OSError,
            UnicodeDecodeError, OverflowError)

#: Any of these means the parser tried to allocate/consume unbounded resources
#: from header fields -- a trust-boundary failure, not a clean rejection.
FATAL = (MemoryError, RecursionError, SystemError)


class Timeout(Exception):
    pass


@contextlib.contextmanager
def deadline(seconds: float):
    if not hasattr(signal, "setitimer"):
        yield
        return

    def _fire(_signum, _frame):
        raise Timeout()

    old = signal.signal(signal.SIGALRM, _fire)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


class Fuzzer:
    def __init__(self, seconds: float):
        self.seconds = seconds
        self.cases = 0
        self.failures: list[str] = []

    def check(self, label: str, fn) -> None:
        self.cases += 1
        try:
            with deadline(self.seconds):
                fn()
        except EXPECTED:
            return
        except Timeout:
            self.failures.append(f"{label}: TIMEOUT (>{self.seconds:g}s)")
        except FATAL as exc:
            self.failures.append(f"{label}: {type(exc).__name__}: {exc}")
        except Exception as exc:  # noqa: BLE001 - the whole point is to catch it
            # An unexpected exception type still means "no crash", but it would
            # surface as a traceback from the CLI, so report it for triage.
            self.failures.append(f"{label}: unexpected {type(exc).__name__}: {exc}")


def truncations(data: bytes, limit: int) -> list[int]:
    n = len(data)
    fixed = [0, 1, 2, 3, 4, 5, 8, 12, 16, 20, 32, 48, 64, 96, 128, 192, 256,
             512, n // 2, max(0, n - 1)]
    fixed = [x for x in fixed if x <= n]
    rng = random.Random(0xC0FFEE ^ n)
    fixed += [rng.randrange(n + 1) for _ in range(limit)]
    return sorted(set(fixed))


def mutations(data: bytes, limit: int) -> list[bytes]:
    """A mix of header, random-bit and extreme-dword mutations."""
    rng = random.Random(0xBADC0DE ^ len(data))
    n = len(data)
    header: list[bytes] = []
    for pos in range(min(64, n)):
        for value in (0x00, 0xFF, 0x7F, 0x80):
            blob = bytearray(data)
            blob[pos] = value
            header.append(bytes(blob))
    flips: list[bytes] = []
    for _ in range(limit):
        blob = bytearray(data)
        if n:
            pos = rng.randrange(n)
            blob[pos] ^= 1 << rng.randrange(8)
        flips.append(bytes(blob))
    extremes: list[bytes] = []
    for off in range(4, min(32, n), 4):
        for value in (0, 1, 0x7FFFFFFF, 0xFFFFFFFF):
            blob = bytearray(data)
            struct.pack_into("<I", blob, off, value)
            extremes.append(bytes(blob))
    pool = header[: limit // 2] + flips + extremes
    return pool[:limit]


def payload_mutations(data: bytes, start: int, limit: int) -> list[bytes]:
    """Single-bit flips that leave the container header intact."""
    rng = random.Random(0x5EED ^ len(data))
    out: list[bytes] = []
    if start >= len(data):
        return out
    for _ in range(limit):
        blob = bytearray(data)
        pos = rng.randrange(start, len(data))
        blob[pos] ^= 1 << rng.randrange(8)
        out.append(bytes(blob))
    return out


def fuzz_bigfile(fz: Fuzzer, game: str, limit: int) -> None:
    idx = os.path.join(game, "PICS", "SBM.IDX")
    blob = open(idx, "rb").read()
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
        empty = os.path.join(tmp, "empty.dat")
        open(empty, "wb").close()
        for i, cut in enumerate(truncations(blob, limit)):
            path = os.path.join(tmp, f"t{i}.idx")
            with open(path, "wb") as f:
                f.write(blob[:cut])
            fz.check(f"bigfile trunc[{cut}]", lambda p=path: bigfile.BigFile(p, empty))
        for i, bad in enumerate(mutations(blob, limit)):
            path = os.path.join(tmp, f"m{i}.idx")
            with open(path, "wb") as f:
                f.write(bad)
            fz.check(f"bigfile mut[{i}]", lambda p=path: bigfile.BigFile(p, empty).entries)

    # A valid index against short DATs must reject offsets past EOF, and a
    # forged negative size must never turn into ``read(-n)``.
    for size in (0, 1, 11, 100):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
            dat = os.path.join(tmp, "SBM.DAT")
            with open(dat, "wb") as f:
                f.truncate(size)
            fz.check(f"bigfile dat_size={size}", lambda d=dat: bigfile.BigFile(idx, d).entries)


def fuzz_sbv(fz: Fuzzer, game: str, limit: int) -> None:
    path = os.path.join(game, "Vid", "Intro.sbv")
    blob = open(path, "rb").read()
    for cut in truncations(blob, limit):
        fz.check(f"sbv trunc[{cut}]", lambda b=blob[:cut]: sbv.parse(b))
    for i, bad in enumerate(mutations(blob, limit)):
        fz.check(f"sbv mut[{i}]", lambda b=bad: sbv.parse(b))
    # Header extremes: dimensions/count must be rejected before allocation.
    base = bytearray(blob)
    for width in (0, 1, 321, 0xFFFFFFFF):
        for height in (0, 15, 17, 1024, 1025, 0xFFFFFFFF):
            bad = bytearray(base)
            struct.pack_into("<II", bad, 4, width, height)
            fz.check(f"sbv geom {width}x{height}", lambda b=bytes(bad): sbv.parse(b))
    for count in (0, 1, 200_000, 200_001, 0xFFFFFFFF):
        bad = bytearray(base)
        struct.pack_into("<I", bad, 12, count)
        fz.check(f"sbv count={count}", lambda b=bytes(bad): sbv.parse(b))
    # A mutated frame must decode or raise, never loop.  Only run when the
    # header itself still parses so we actually reach the pixel codec.
    parsed = sbv.parse(blob)
    for i, bad in enumerate(payload_mutations(blob, parsed.data_start,
                                              max(4, limit // 8))):
        def decode(b=bad):
            s = sbv.parse(b)
            if s.frame_count:
                sbv.decode_frame_rgb(s.frame(b, 0).data, s.width, s.height)
        fz.check(f"sbv decode[{i}]", decode)
    assert parsed.frame_count


def _fuzz_bytes(fz: Fuzzer, label: str, blob: bytes, parse, limit: int) -> None:
    for cut in truncations(blob, limit):
        fz.check(f"{label} trunc[{cut}]", lambda b=blob[:cut]: parse(b))
    for i, bad in enumerate(mutations(blob, limit)):
        fz.check(f"{label} mut[{i}]", lambda b=bad: parse(b))


def fuzz_sbw(fz: Fuzzer, game: str, limit: int) -> None:
    path = os.path.join(game, "LEVEL0.SBW")
    if not os.path.exists(path):
        path = next(os.path.join(game, f) for f in sorted(os.listdir(game))
                    if f.lower().endswith(".sbw"))
    _fuzz_bytes(fz, "sbw", open(path, "rb").read(), sbw.parse, limit)


def fuzz_sbt(fz: Fuzzer, game: str, limit: int) -> None:
    path = os.path.join(game, "LEVEL0.SBT")
    if not os.path.exists(path):
        path = next(os.path.join(game, f) for f in sorted(os.listdir(game))
                    if f.lower().endswith(".sbt"))
    _fuzz_bytes(fz, "sbt", open(path, "rb").read(), sbt.parse, limit)


def fuzz_sbm(fz: Fuzzer, game: str, limit: int) -> None:
    bf = bigfile.BigFile(os.path.join(game, "PICS", "SBM.IDX"))
    blob = bf.read(bf.entries[0])
    _fuzz_bytes(fz, "sbm", blob, sbm.parse, limit)


def fuzz_sbc(fz: Fuzzer, game: str, limit: int) -> None:
    matches = []
    for name in sorted(os.listdir(game)):
        if name.lower().endswith(".sbc"):
            matches.append(os.path.join(game, name))
    if not matches:
        return
    _fuzz_bytes(fz, "sbc", open(matches[0], "rb").read(), sbc.parse, limit)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--seed", type=int, default=0, help="reserved for reproducibility")
    ap.add_argument("--quick", action="store_true", help="fewer mutations")
    ap.add_argument("--timeout", type=float, default=2.0,
                    help="per-case wall-clock budget in seconds (default 2)")
    opts = ap.parse_args(argv)
    if not os.path.isdir(opts.game):
        print(f"fuzz: game directory not found: {opts.game}", file=sys.stderr)
        return 2
    random.seed(opts.seed)
    limit = 12 if opts.quick else 40
    fz = Fuzzer(opts.timeout)
    fuzz_bigfile(fz, opts.game, limit)
    fuzz_sbv(fz, opts.game, limit)
    fuzz_sbw(fz, opts.game, limit)
    fuzz_sbt(fz, opts.game, limit)
    fuzz_sbm(fz, opts.game, limit)
    fuzz_sbc(fz, opts.game, limit)
    bad = fz.failures
    print(f"fuzz: {fz.cases} cases, {len(bad)} failure(s)")
    for line in bad[:40]:
        print(f"  {line}")
    if len(bad) > 40:
        print(f"  ... {len(bad) - 40} more")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
