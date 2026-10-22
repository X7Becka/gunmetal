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

Persistent corpus (save/replay)::

    python3 tools/fuzz_formats.py --game DIR --seed 7 --save /var/tmp/corpus
    python3 tools/fuzz_formats.py --replay /var/tmp/corpus
    python3 tools/qa/fuzz_corpus.py --dir /var/tmp/corpus --verify

``--seed`` drives every pseudo-random choice, so the same seed and the same
game installation always produce byte-identical inputs; ``--save`` writes them
to disk together with a ``manifest.json`` (target/kind/name/sha256/size/seed)
and ``--replay`` re-runs the *stored* bytes through the very same parsers
without generating anything anew.

Exit status is 0 when every malformed case raised a normal exception and 1 if
a case hung, raised ``MemoryError``/``RecursionError`` or otherwise escaped the
expected error set.  A corpus with a broken/mismatching file also exits 1.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import random
import signal
import struct
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Callable

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from gm import bigfile, sbc, sbm, sbt, sbv, sbw, sof, soo, sou1, soe, soa  # noqa: E402

DEFAULT_GAME = os.environ.get("GUNMETAL_DIR", "/mnt/d/games/Gunmetal 3dfx")

#: Exceptions a parser is allowed to raise for malformed input.  ``OSError``
#: covers a temp file disappearing, ``struct.error`` is normalised away in the
#: newer parsers but still allowed here.
EXPECTED = (ValueError, struct.error, IndexError, KeyError, OSError,
            UnicodeDecodeError, OverflowError)

#: Any of these means the parser tried to allocate/consume unbounded resources
#: from header fields -- a trust-boundary failure, not a clean rejection.
FATAL = (MemoryError, RecursionError, SystemError)

#: Corpus layout.  Bump :data:`CORPUS_VERSION` when the layout changes; a
#: replay refuses a manifest it does not understand instead of guessing.
CORPUS_FORMAT = "gunmetal-fuzz-corpus"
CORPUS_VERSION = 1
MANIFEST_NAME = "manifest.json"
CASES_SUBDIR = "cases"

#: Bumped whenever the *content* of a generated case changes (the layout of
#: the matrix, not the corpus format).  Recorded in the manifest meta.
TOOL_VERSION = 2


class Timeout(Exception):
    pass


class CorpusError(Exception):
    """A stored corpus is unusable (bad manifest, missing or altered file)."""


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


# ---------------------------------------------------------------------------
# case model
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Case:
    """One hostile input: the bytes to feed and how to label them."""

    target: str                      # parser under test ("sbv", "bigfile", ...)
    kind: str                        # "trunc" / "mutation" / "extreme" / ...
    name: str                        # label used in the failure report
    data: bytes = b""                # the bytes handed to the parser
    extra: dict = field(default_factory=dict)   # JSON-serialisable extras


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj) -> str:
    """Stable JSON: sorted keys, no whitespace, ASCII only."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def atomic_write_bytes(path: str, data: bytes) -> None:
    """Write ``data`` to ``path`` atomically (temp file + ``os.replace``)."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    tmp = os.path.join(directory, f".{os.path.basename(path)}.tmp-{os.getpid()}")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def rng_factory(seed: int, target: str):
    """Return ``make(kind) -> random.Random``.

    Every (target, kind) group gets its own stream derived from the seed, so
    the generated data depends on the seed, is independent of the order in
    which the groups run, and is stable across machines and Python versions
    (the seed is hashed, not taken from ``hash()``).
    """
    def make(kind: str) -> random.Random:
        material = f"{seed}|{target}|{kind}".encode()
        digest = hashlib.sha256(material).digest()
        return random.Random(int.from_bytes(digest[:8], "big"))
    return make


# ---------------------------------------------------------------------------
# generators
# ---------------------------------------------------------------------------


def truncations(data: bytes, limit: int, rng: random.Random) -> list[int]:
    """Cut points: a fixed ladder plus ``limit`` distinct random positions.

    The random positions are drawn *without* replacement and never collide
    with the ladder, so the number of cases does not depend on the seed (only
    their contents do).
    """
    n = len(data)
    fixed = [0, 1, 2, 3, 4, 5, 8, 12, 16, 20, 32, 48, 64, 96, 128, 192, 256,
             512, n // 2, max(0, n - 1)]
    fixed = [x for x in fixed if x <= n]
    seen = set(fixed)
    extra: list[int] = []
    want = min(limit, (n + 1) - len(seen))
    while len(extra) < want:
        pos = rng.randrange(n + 1)
        if pos in seen:
            continue
        seen.add(pos)
        extra.append(pos)
    return sorted(fixed + extra)


def mutations(data: bytes, limit: int, rng: random.Random) -> list[bytes]:
    """A mix of header, random-bit and extreme-dword mutations."""
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


def payload_mutations(data: bytes, start: int, limit: int,
                      rng: random.Random) -> list[bytes]:
    """Single-bit flips that leave the container header intact."""
    out: list[bytes] = []
    if start >= len(data):
        return out
    for _ in range(limit):
        blob = bytearray(data)
        pos = rng.randrange(start, len(data))
        blob[pos] ^= 1 << rng.randrange(8)
        out.append(bytes(blob))
    return out


def _trunc_cases(target: str, base: bytes, limit: int, make_rng) -> list[Case]:
    out = []
    for cut in truncations(base, limit, make_rng("trunc")):
        out.append(Case(target, "trunc", f"{target} trunc[{cut}]",
                        base[:cut], {"cut": cut}))
    return out


def _mut_cases(target: str, base: bytes, limit: int, make_rng) -> list[Case]:
    out = []
    for i, bad in enumerate(mutations(base, limit, make_rng("mutation"))):
        out.append(Case(target, "mutation", f"{target} mut[{i}]", bad,
                        {"index": i}))
    return out


# ---------------------------------------------------------------------------
# targets: what is parsed, how the cases are built and how they are replayed
# ---------------------------------------------------------------------------


@dataclass
class Target:
    """One parser under test: where the base file comes from, how the hostile
    inputs are built from it and how a stored input is fed back."""

    name: str
    source: str                                        # shipped file it comes from
    load: Callable[[str], bytes | None]                # (game) -> base bytes
    build: Callable[[bytes, int, Callable[[str], random.Random]], list[Case]]
    run: Callable[[Case, str], None]                   # (case, tmpdir) -> None


def _read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def _first_with_suffix(directory: str, suffix: str) -> str | None:
    if not os.path.isdir(directory):
        return None
    for name in sorted(os.listdir(directory)):
        if name.lower().endswith(suffix):
            return os.path.join(directory, name)
    return None


def _load_bigfile(game: str) -> bytes:
    return _read(os.path.join(game, "PICS", "SBM.IDX"))


def _build_bigfile(base: bytes, limit: int, make_rng) -> list[Case]:
    cases = _trunc_cases("bigfile", base, limit, make_rng)
    for i, bad in enumerate(mutations(base, limit, make_rng("mutation"))):
        cases.append(Case("bigfile", "mutation", f"bigfile mut[{i}]", bad,
                          {"index": i, "entries": True}))
    # A valid index against short DATs must reject offsets past EOF, and a
    # forged negative size must never turn into ``read(-n)``.
    for size in (0, 1, 11, 100):
        cases.append(Case("bigfile", "dat_size", f"bigfile dat_size={size}",
                          base, {"dat_size": size, "entries": True}))
    return cases


def _run_bigfile(case: Case, tmpdir: str) -> None:
    idx_path = os.path.join(tmpdir, "case.idx")
    dat_path = os.path.join(tmpdir, "case.dat")
    with open(idx_path, "wb") as fh:
        fh.write(case.data)
    size = int(case.extra.get("dat_size", 0))
    with open(dat_path, "wb") as fh:
        if size:
            fh.truncate(size)
    archive = bigfile.BigFile(idx_path, dat_path)
    if case.extra.get("entries"):
        # ``entries`` is built in ``__init__``; touching it mirrors the old
        # ``BigFile(...).entries`` call.
        _ = archive.entries


def _load_sbv(game: str) -> bytes:
    return _read(os.path.join(game, "Vid", "Intro.sbv"))


def _build_sbv(base: bytes, limit: int, make_rng) -> list[Case]:
    cases = _trunc_cases("sbv", base, limit, make_rng)
    cases += _mut_cases("sbv", base, limit, make_rng)
    # Header extremes: dimensions/count must be rejected before allocation.
    for width in (0, 1, 321, 0xFFFFFFFF):
        for height in (0, 15, 17, 1024, 1025, 0xFFFFFFFF):
            bad = bytearray(base)
            struct.pack_into("<II", bad, 4, width, height)
            cases.append(Case("sbv", "extreme", f"sbv geom {width}x{height}",
                              bytes(bad), {"width": width, "height": height}))
    for count in (0, 1, 200_000, 200_001, 0xFFFFFFFF):
        bad = bytearray(base)
        struct.pack_into("<I", bad, 12, count)
        cases.append(Case("sbv", "extreme", f"sbv count={count}", bytes(bad),
                          {"count": count}))
    # A mutated frame must decode or raise, never loop.  Only run when the
    # header itself still parses so we actually reach the pixel codec.
    try:
        parsed = sbv.parse(base)
    except ValueError:
        parsed = None
    if parsed is not None and parsed.frame_count:
        for i, bad in enumerate(payload_mutations(base, parsed.data_start,
                                                  max(4, limit // 8),
                                                  make_rng("payload"))):
            cases.append(Case("sbv", "payload", f"sbv decode[{i}]", bad,
                              {"index": i}))
    return cases


def _run_sbv(case: Case, tmpdir: str) -> None:
    parsed = sbv.parse(case.data)
    if case.kind == "payload" and parsed.frame_count:
        sbv.decode_frame_rgb(parsed.frame(case.data, 0).data,
                             parsed.width, parsed.height)


def _load_sbw(game: str) -> bytes | None:
    path = os.path.join(game, "LEVEL0.SBW")
    if not os.path.exists(path):
        path = _first_with_suffix(game, ".sbw")
    return None if path is None else _read(path)


def _load_sbt(game: str) -> bytes | None:
    path = os.path.join(game, "LEVEL0.SBT")
    if not os.path.exists(path):
        path = _first_with_suffix(game, ".sbt")
    return None if path is None else _read(path)


def _load_sbm(game: str) -> bytes:
    archive = bigfile.BigFile(os.path.join(game, "PICS", "SBM.IDX"))
    return archive.read(archive.entries[0])


def _load_sbc(game: str) -> bytes | None:
    path = _first_with_suffix(game, ".sbc")
    return None if path is None else _read(path)


def _load_sof(game: str) -> bytes | None:
    path = os.path.join(game, "Film", "AMMO4.SOF")
    return _read(path) if os.path.exists(path) else None


def _load_fdx(game: str) -> bytes | None:
    path = os.path.join(game, "Film", "ammo.fdx")
    return _read(path) if os.path.exists(path) else None


def _load_soo(game: str) -> bytes | None:
    path = os.path.join(game, "Sobanim", "Cultist.soo")
    return _read(path) if os.path.exists(path) else None


def _load_soe(game: str) -> bytes | None:
    path = os.path.join(game, "SOBS", "railrock.soe")
    return _read(path) if os.path.exists(path) else None


def _load_soa(game: str) -> bytes | None:
    path = os.path.join(game, "anims", "pulsar.soa")
    return _read(path) if os.path.exists(path) else None


def _load_sou1(game: str) -> bytes | None:
    idx = os.path.join(game, "sounds", "SOU.IDX")
    if not os.path.exists(idx):
        return None
    archive = bigfile.BigFile(idx)
    entry = next((e for e in archive.entries if e.name.upper() == "SILENCE"), None)
    return None if entry is None else archive.read(entry)


def _runner(parse):
    def run(case: Case, tmpdir: str) -> None:
        parse(case.data)
    return run


def _parse_sof(blob: bytes) -> None:
    film = sof.parse(blob)
    if len(film):
        film.frame(0)


def _parse_soo(blob: bytes) -> None:
    anim = soo.parse(blob)
    if anim.frames:
        anim.frame_at(0)


def _parse_soe(blob: bytes) -> None:
    soe.parse_text(blob.decode("latin1"))


TARGETS: list[Target] = [
    Target("bigfile", "PICS/SBM.IDX", _load_bigfile, _build_bigfile, _run_bigfile),
    Target("sbv", "Vid/Intro.sbv", _load_sbv, _build_sbv, _run_sbv),
    Target("sbw", "LEVEL0.SBW", _load_sbw,
           lambda b, l, r: _trunc_cases("sbw", b, l, r) + _mut_cases("sbw", b, l, r),
           _runner(sbw.parse)),
    Target("sbt", "LEVEL0.SBT", _load_sbt,
           lambda b, l, r: _trunc_cases("sbt", b, l, r) + _mut_cases("sbt", b, l, r),
           _runner(sbt.parse)),
    Target("sbm", "PICS/SBM.DAT[0]", _load_sbm,
           lambda b, l, r: _trunc_cases("sbm", b, l, r) + _mut_cases("sbm", b, l, r),
           _runner(sbm.parse)),
    Target("sbc", "*.sbc", _load_sbc,
           lambda b, l, r: _trunc_cases("sbc", b, l, r) + _mut_cases("sbc", b, l, r),
           _runner(sbc.parse)),
    Target("sof", "Film/AMMO4.SOF", _load_sof,
           lambda b, l, r: _trunc_cases("sof", b, l, r) + _mut_cases("sof", b, l, r),
           _runner(_parse_sof)),
    Target("fdx", "Film/ammo.fdx", _load_fdx,
           lambda b, l, r: _trunc_cases("fdx", b, l, r) + _mut_cases("fdx", b, l, r),
           _runner(lambda blob: sof.parse_fdx_bytes(blob))),
    Target("soo", "Sobanim/Cultist.soo", _load_soo,
           lambda b, l, r: _trunc_cases("soo", b, l, r) + _mut_cases("soo", b, l, r),
           _runner(_parse_soo)),
    Target("soe", "SOBS/railrock.soe", _load_soe,
           lambda b, l, r: _trunc_cases("soe", b, l, r) + _mut_cases("soe", b, l, r),
           _runner(_parse_soe)),
    Target("soa", "anims/pulsar.soa", _load_soa,
           lambda b, l, r: _trunc_cases("soa", b, l, r) + _mut_cases("soa", b, l, r),
           _runner(soa.parse)),
    Target("sou1", "sounds/SOU.IDX[SILENCE]", _load_sou1,
           lambda b, l, r: _trunc_cases("sou1", b, l, r) + _mut_cases("sou1", b, l, r),
           _runner(sou1.decode_entry)),
]

RUNNERS = {t.name: t.run for t in TARGETS}


def collect_cases(game: str, seed: int, limit: int) -> list[Case]:
    """Generate the whole hostile matrix for ``game``."""
    cases: list[Case] = []
    for target in TARGETS:
        try:
            base = target.load(game)
        except (OSError, ValueError) as exc:
            print(f"fuzz: target {target.name}: cannot load the base file: {exc}",
                  file=sys.stderr)
            base = None
        if base is None:
            continue
        cases.extend(target.build(base, limit, rng_factory(seed, target.name)))
    return cases


# ---------------------------------------------------------------------------
# corpus: save / load / verify
# ---------------------------------------------------------------------------


def corpus_digest(manifest: dict) -> str:
    """Path-independent digest of the corpus content.

    Only the format, the version and the case list take part: the meta block
    (seed, game path, timeout) is informational and must not change the
    identity of the stored inputs.
    """
    body = {
        "format": manifest.get("format"),
        "version": manifest.get("version"),
        "cases": manifest.get("cases"),
    }
    return sha256_bytes(canonical_json(body).encode())


def save_corpus(dirpath: str, cases: list[Case], seed: int, meta: dict) -> dict:
    """Write every generated input plus a ``manifest.json`` into ``dirpath``."""
    os.makedirs(os.path.join(dirpath, CASES_SUBDIR), exist_ok=True)
    counters: dict[tuple[str, str], int] = {}
    entries = []
    for seq, case in enumerate(cases):
        key = (case.target, case.kind)
        index = counters.get(key, 0)
        counters[key] = index + 1
        rel = f"{CASES_SUBDIR}/{seed}_{case.target}_{case.kind}_{index:05d}.bin"
        atomic_write_bytes(os.path.join(dirpath, rel), case.data)
        entries.append({
            "seq": seq,
            "target": case.target,
            "kind": case.kind,
            "name": case.name,
            "file": rel,
            "sha256": sha256_bytes(case.data),
            "size": len(case.data),
            "seed": seed,
            "extra": case.extra,
        })
    manifest = {
        "format": CORPUS_FORMAT,
        "version": CORPUS_VERSION,
        "meta": meta,
        "cases": entries,
    }
    manifest["digest"] = corpus_digest(manifest)
    atomic_write_bytes(
        os.path.join(dirpath, MANIFEST_NAME),
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
    return manifest


def load_manifest(dirpath: str) -> dict:
    path = os.path.join(dirpath, MANIFEST_NAME)
    try:
        raw = _read(path)
    except OSError as exc:
        raise CorpusError(f"cannot read {path}: {exc}") from exc
    try:
        manifest = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CorpusError(f"{path}: not a valid manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise CorpusError(f"{path}: manifest is not an object")
    if manifest.get("format") != CORPUS_FORMAT:
        raise CorpusError(f"{path}: unknown corpus format "
                          f"{manifest.get('format')!r}")
    if manifest.get("version") != CORPUS_VERSION:
        raise CorpusError(f"{path}: unsupported corpus version "
                          f"{manifest.get('version')!r} (expected {CORPUS_VERSION})")
    if not isinstance(manifest.get("cases"), list):
        raise CorpusError(f"{path}: manifest has no case list")
    stored = manifest.get("digest")
    if stored is not None:
        actual = corpus_digest(manifest)
        if actual != stored:
            raise CorpusError(f"{path}: manifest digest mismatch "
                              f"(stored {stored}, computed {actual})")
    return manifest


def verify_corpus(dirpath: str) -> tuple[dict, list[Case], list[str], list[dict]]:
    """Load the stored inputs and check every file against the manifest.

    Returns ``(manifest, cases, problems, entries)``.  ``problems`` holds one
    human readable line per integrity problem (missing file, size or sha256
    mismatch); the corresponding case is dropped from ``cases``.
    """
    manifest = load_manifest(dirpath)
    problems: list[str] = []
    cases: list[Case] = []
    entries: list[dict] = []
    for entry in manifest["cases"]:
        if not isinstance(entry, dict):
            problems.append("corpus: malformed case entry")
            continue
        rel = entry.get("file")
        if not isinstance(rel, str):
            problems.append("corpus: case entry without a file name")
            continue
        path = os.path.join(dirpath, rel)
        try:
            data = _read(path)
        except OSError as exc:
            problems.append(f"corpus: {rel}: cannot read: {exc}")
            continue
        digest = sha256_bytes(data)
        if digest != entry.get("sha256"):
            problems.append(f"corpus: {rel}: sha256 mismatch: expected "
                            f"{entry.get('sha256')}, got {digest}")
            continue
        if len(data) != entry.get("size"):
            problems.append(f"corpus: {rel}: size mismatch: expected "
                            f"{entry.get('size')}, got {len(data)}")
            continue
        target = entry.get("target")
        if target not in RUNNERS:
            problems.append(f"corpus: {rel}: unknown target {target!r}")
            continue
        entries.append(entry)
        cases.append(Case(target, entry.get("kind", "unknown"),
                          entry.get("name", rel), data, entry.get("extra", {})))
    return manifest, cases, problems, entries


# ---------------------------------------------------------------------------
# execution
# ---------------------------------------------------------------------------


def _tmp_root() -> str:
    """Directory for the transient BIGFILE scratch files."""
    for candidate in ("/tmp/opencode", tempfile.gettempdir()):
        if os.path.isdir(candidate):
            return candidate
    return tempfile.gettempdir()


def execute(cases: list[Case], timeout: float) -> tuple[Fuzzer, list[str]]:
    """Run every case through its parser; return the fuzzer and the corpus
    problems found while loading (replay only)."""
    fz = Fuzzer(timeout)
    problems: list[str] = []
    needs_tmp = any(c.target == "bigfile" for c in cases)
    if needs_tmp:
        with tempfile.TemporaryDirectory(dir=_tmp_root()) as tmp:
            for case in cases:
                fz.check(case.name, lambda c=case: RUNNERS[c.target](c, tmp))
    else:
        for case in cases:
            fz.check(case.name, lambda c=case: RUNNERS[c.target](c, ""))
    return fz, problems


def _inputs_report(cases: list[Case], entries: list[dict] | None = None) -> list[dict]:
    """Per-input sha256 report (from the manifest when replaying)."""
    out = []
    if entries is not None:
        for entry in entries:
            out.append({
                "target": entry.get("target"),
                "kind": entry.get("kind"),
                "name": entry.get("name"),
                "file": entry.get("file"),
                "sha256": entry.get("sha256"),
                "size": entry.get("size"),
            })
        return out
    for case in cases:
        out.append({
            "target": case.target,
            "kind": case.kind,
            "name": case.name,
            "file": None,
            "sha256": sha256_bytes(case.data),
            "size": len(case.data),
        })
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--seed", type=int, default=0,
                    help="seed of the generator (reproducible corpus)")
    ap.add_argument("--quick", action="store_true", help="fewer mutations")
    ap.add_argument("--timeout", type=float, default=2.0,
                    help="per-case wall-clock budget in seconds (default 2)")
    ap.add_argument("--save", nargs="?", const="", metavar="DIR",
                    help="store the generated inputs in DIR (persistent corpus)")
    ap.add_argument("--corpus", metavar="DIR",
                    help="corpus directory, used together with --save")
    ap.add_argument("--replay", metavar="DIR",
                    help="replay the stored corpus in DIR instead of generating")
    ap.add_argument("--json", action="store_true",
                    help="print a machine-readable report (one JSON line)")
    opts = ap.parse_args(argv)

    if opts.replay and (opts.save is not None or opts.corpus):
        print("fuzz: --replay cannot be combined with --save/--corpus",
              file=sys.stderr)
        return 2
    corpus_dir = opts.save if opts.save else opts.corpus
    if opts.save is not None and not corpus_dir:
        print("fuzz: --save needs a directory: --save DIR or --corpus DIR",
              file=sys.stderr)
        return 2
    if opts.save and opts.corpus and opts.save != opts.corpus:
        print(f"fuzz: --save {opts.save} overrides --corpus {opts.corpus}",
              file=sys.stderr)

    limit = 12 if opts.quick else 40
    mode = "plain"
    entries: list[dict] | None = None
    problems: list[str] = []
    manifest: dict | None = None
    seed = opts.seed

    if opts.replay:
        mode = "replay"
        corpus_dir = opts.replay
        try:
            manifest, cases, problems, entries = verify_corpus(corpus_dir)
        except CorpusError as exc:
            print(f"fuzz: {exc}", file=sys.stderr)
            if opts.json:
                print(canonical_json({
                    "cases": 0, "failures": 1, "fuzz_failures": 0,
                    "corpus_errors": 1, "corpus": os.path.abspath(corpus_dir),
                    "seed": opts.seed, "replay": True, "manifest_digest": None,
                    "manifest_sha256": None, "inputs": [],
                    "errors": [str(exc)],
                }))
            else:
                print("fuzz: 0 cases, 1 failure(s)")
            return 1
        meta = manifest.get("meta") or {}
        if isinstance(meta.get("seed"), int):
            seed = meta["seed"]
    else:
        if not os.path.isdir(opts.game):
            print(f"fuzz: game directory not found: {opts.game}", file=sys.stderr)
            return 2
        cases = collect_cases(opts.game, opts.seed, limit)
        if corpus_dir:
            mode = "save"
            meta = {
                "tool": "tools/fuzz_formats.py",
                "tool_version": TOOL_VERSION,
                "seed": opts.seed,
                "mode": "quick" if opts.quick else "full",
                "limit": limit,
                "timeout": opts.timeout,
                "game": os.path.abspath(opts.game),
                "case_count": len(cases),
            }
            manifest = save_corpus(corpus_dir, cases, opts.seed, meta)
            entries = manifest["cases"]
            print(f"fuzz: corpus saved to {os.path.abspath(corpus_dir)} "
                  f"({len(cases)} cases, digest {manifest['digest'][:16]})",
                  file=sys.stderr)

    fz, run_problems = execute(cases, opts.timeout)
    problems = list(problems) + list(run_problems)
    if not cases:
        # A broken installation must not look like a clean run.
        problems.append("no cases generated: the game directory has none of "
                        "the expected files (or the corpus is empty)")
    failures = list(fz.failures)
    total_failures = len(failures) + len(problems)

    if opts.json:
        report = {
            "cases": fz.cases,
            "failures": total_failures,
            "fuzz_failures": len(failures),
            "corpus_errors": len(problems),
            "corpus": os.path.abspath(corpus_dir) if corpus_dir else None,
            "seed": seed,
            "replay": mode == "replay",
            "mode": mode,
            "manifest_digest": manifest.get("digest") if manifest else None,
            "manifest_sha256": None,
            "inputs": _inputs_report(cases, entries),
            "errors": problems + failures,
        }
        if corpus_dir:
            manifest_path = os.path.join(corpus_dir, MANIFEST_NAME)
            if os.path.exists(manifest_path):
                report["manifest_sha256"] = sha256_bytes(_read(manifest_path))
        print(canonical_json(report))
        return 1 if total_failures else 0

    # The summary line stays the first line of stdout: regress.py parses it.
    print(f"fuzz: {fz.cases} cases, {total_failures} failure(s)")
    for line in (problems + failures)[:40]:
        print(f"  {line}")
    if len(problems) + len(failures) > 40:
        print(f"  ... {len(problems) + len(failures) - 40} more")
    return 1 if total_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
