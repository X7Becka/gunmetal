#!/usr/bin/env python3
"""Fuzz the C++ save/world CLI paths (``--dump``, ``--save-out``, ``--info``).

``tools/fuzz_formats.py`` covers the pure-Python parsers in ``gm/``; this is its
*out-of-process* twin for the C++ reader/writer that ships in the engine.  A
mutated save or world is handed to the real binary and the run is judged by
what the operating system saw: a normal exit (0 or 2), no fatal signal, no
timeout and no sanitizer report.  That is exactly the trust boundary the
in-process matrix cannot see: a heap overflow, an assert or an end-less loop in
``parse_save``/``write_save``/``parse_sbw``/``load_level``.

Corpus and mutations per seed file:

* truncation to 1, 2 and ``len/2 - 1`` bytes;
* single-byte substitution in the header (6 offsets x ``00/01/80/ff``);
* single-byte substitution in the block area (3 block offsets x ``00/ff``);
* a ``0xffffffff`` dword *inserted* where a block id/count is expected (3);
* a zeroed block count/size dword (3);
* 1..8 random byte replacements driven by a documented LCG (48 in full mode).

Save targets run ``--dump`` and ``--dump --save-out``; world targets run
``--dump`` and ``--info`` (the mutant is mounted as ``<name>.sbw`` in a private
temp game dir that symlinks the shipped ``.sbt``/``.sbc``); a level ``.sbt``
target runs ``--dump`` and ``--info`` (mutant as ``<name>.sbt``, shipped world
symlinked).  Every mutant lives in its own directory under ``--out`` (default
``/tmp/opencode/fuzz_saves``); the game installation is only ever read.

Usage::

    python3 tools/qa/fuzz_saves.py                     # full matrix (~500 cases)
    python3 tools/qa/fuzz_saves.py --quick             # ~48 cases
    python3 tools/qa/fuzz_saves.py --json --seed 7
    python3 tools/qa/fuzz_saves.py --bin build/asan/engine/gunmetal   # sanitizer pass

Exit status is 0 when every mutant exited normally with a non-crashing code
(0/2) and the sanitizer stayed silent, 1 on any crash, hang, sanitizer report
or unexpected exit code, and 2 on a usage error (missing binary/game/seed).

The seed drives every pseudo-random choice and is printed in the summary, so
the same command reproduces byte-identical inputs.  A failure keeps its case
directory (input, stdout, stderr, ``--save-out`` result) for replay; clean
cases are removed to keep the tmpfs small (``--keep`` retains them all).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.environ.get("GUNMETAL_DIR", "/mnt/d/games/Gunmetal 3dfx")
DEFAULT_BIN = os.path.join(ROOT, "build", "sandbox", "engine", "gunmetal")
DEFAULT_ASAN_BIN = os.path.join(ROOT, "build", "asan", "engine", "gunmetal")
DEFAULT_OUT = "/tmp/opencode/fuzz_saves"

#: Header length of a save container (``docs/SAVE.md`` §1).
SAVE_HEADER = 0x13C

#: Where a mutant may be written.  The game installation is read-only and
#: nothing else in this harness needs a writable path, so the check is blunt.
OUT_ROOTS = ("/tmp/opencode", "/var/tmp")

#: Bytes a sanitizer prints before it dies; any of them is a finding.
SANITIZER_MARKERS = (
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "LeakSanitizer",
    "runtime error:",
)

#: Exit codes the CLI documents for "handled" input: 0 = success, 2 = error
#: message on stderr (``main.cpp``).  Anything else is a finding.
OK_CODES = (0, 2)

MASKS = (1 << 64) - 1

_SAVE_BLOCK_RE = re.compile(
    r"^\s*id\s+(\d+)\s+count\s+(\d+)\s+size\s+(\d+)\s+@0x([0-9a-fA-F]+)\s*$")


# ---------------------------------------------------------------------------
# deterministic randomness
# ---------------------------------------------------------------------------


class Lcg:
    """64-bit LCG (Knuth MMIX constants), documented so cases reproduce.

    Python's ``random`` is stable in practice but not promised across
    versions; a ten-line LCG is, and it keeps this tool stdlib-only and
    independent of any global seeding.
    """

    A = 6364136223846793005
    C = 1442695040888963407

    def __init__(self, seed: int):
        self.state = seed & MASKS

    def u64(self) -> int:
        self.state = (self.A * self.state + self.C) & MASKS
        return self.state

    def below(self, n: int) -> int:
        return self.u64() % n if n > 0 else 0

    def byte(self) -> int:
        return self.u64() >> 56


def stream_seed(seed: int, label: str) -> int:
    """Seed for one ``(seed, target)`` stream, independent of run order."""
    digest = hashlib.sha256(f"{seed}|{label}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


# ---------------------------------------------------------------------------
# case model
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Target:
    """One seed file and how the CLI is pointed at it."""

    name: str          # label in reports ("save0", "world:dm1", ...)
    kind: str          # "save" | "world" | "level"
    rel: str           # path relative to the game directory
    path: str          # resolved absolute path
    blocks: tuple[int, ...] = ()   # save only: block offsets from --dump


@dataclass(frozen=True)
class Case:
    target: str
    name: str
    kind: str          # mutation family ("trunc"/"header"/"block"/...)
    data: bytes
    target_kind: str = "save"      # "save" | "world" | "level"
    seed_path: str = ""            # the shipped file the mutant came from
    note: str = ""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.@=+-]+", "_", text).strip("_") or "case"


# ---------------------------------------------------------------------------
# corpus discovery
# ---------------------------------------------------------------------------


def find_ci(root: str, rel: str) -> str | None:
    """Resolve ``rel`` case-insensitively below ``root`` (the install is DOS)."""
    candidate = os.path.join(root, rel)
    if os.path.isfile(candidate):
        return candidate
    directory = os.path.join(root, os.path.dirname(rel))
    wanted = os.path.basename(rel).lower()
    if not os.path.isdir(directory):
        return None
    for name in sorted(os.listdir(directory)):
        if name.lower() == wanted:
            path = os.path.join(directory, name)
            return path if os.path.isfile(path) else None
    return None


def probe_save_blocks(bin_path: str, path: str, timeout: float) -> tuple[int, ...]:
    """Ask the binary under test for the real block offsets (``--dump``).

    Using the tool's own report keeps this harness free of a second save
    parser.  A failed probe only means the mutation offsets fall back to the
    fixed ladder in :func:`block_offsets`; it is not a finding by itself.
    """
    try:
        proc = subprocess.run([bin_path, "--dump", path], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return ()
    if proc.returncode != 0:
        return ()
    offsets = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        match = _SAVE_BLOCK_RE.match(line)
        if match:
            offsets.append(int(match.group(4), 16))
    return tuple(offsets)


def collect_targets(game: str, bin_path: str, timeout: float,
                    log=print) -> list[Target]:
    specs = (
        ("save0", "save", "SAVEGAME/saved0.sbt"),
        ("save1", "save", "SAVEGAME/saved1.sbt"),
        ("world:level0", "world", "level0.sbw"),
        ("world:dm1", "world", "dm1.sbw"),
        ("world:level000", "world", "level000.sbw"),
        ("level:level0", "level", "level0.sbt"),
    )
    targets: list[Target] = []
    for name, kind, rel in specs:
        path = find_ci(game, rel)
        if path is None:
            log(f"fuzz_saves: seed not found, skipping {name} ({rel})",
                file=sys.stderr)
            continue
        blocks: tuple[int, ...] = ()
        if kind == "save":
            blocks = probe_save_blocks(bin_path, path, timeout)
        targets.append(Target(name, kind, rel, path, blocks))
    return targets


# ---------------------------------------------------------------------------
# mutations
# ---------------------------------------------------------------------------


HEADER_OFFSETS = {
    # title[12], version word, reserved, level name, first record byte
    "save": (0x00, 0x01, 0x0C, 0x50, 0x54, SAVE_HEADER),
    # count1, effects/name bytes, early table data
    "world": (0x00, 0x01, 0x02, 0x03, 0x04, 0x08),
    "level": (0x00, 0x01, 0x02, 0x03, 0x04, 0x08),
}

HEADER_VALUES = (0x00, 0x01, 0x80, 0xFF)


def block_offsets(kind: str, base: bytes, blocks: tuple[int, ...]) -> list[int]:
    """Offsets whose first payload dword is a count/size field."""
    n = len(base)
    if kind == "save":
        if blocks:
            return [off + 1 for off in blocks if off + 4 < n]
        return [off for off in (SAVE_HEADER + 1, 0x800, n // 2) if off + 4 < n]
    # A world/level stream has no length-prefixed blocks: the counts of the
    # texture/effect/sector tables sit at the very start, so probe there plus
    # the middle of the stream.
    return [off for off in (0, 4, n // 2) if off + 4 < n]


def _put(data: bytes, off: int, value: int) -> bytes | None:
    if off < 0 or off + 4 > len(data):
        return None
    buf = bytearray(data)
    struct.pack_into("<I", buf, off, value)
    return bytes(buf)


def build_cases(target: Target, base: bytes, seed: int,
                quick: bool) -> list[Case]:
    """The hostile matrix for one seed file (count depends only on mode)."""
    n = len(base)
    rng = Lcg(stream_seed(seed, target.name))
    cases: list[Case] = []

    # 1. Truncation: a 1-byte file, a 2-byte file, half minus one.
    for cut in sorted({1, 2, max(0, n // 2 - 1)}):
        if cut < n:
            cases.append(Case(target.name, f"trunc[{cut}]", "trunc", base[:cut],
                              target.kind, target.path,
                              f"prefix of {cut} bytes"))

    # 2. One byte in the header.
    offsets = HEADER_OFFSETS[target.kind]
    values = (0xFF,) if quick else HEADER_VALUES
    if quick:
        offsets = offsets[:1]
    for off in offsets:
        for value in values:
            if off >= n or base[off] == value:
                continue          # a no-op mutation is not a test case
            buf = bytearray(base)
            buf[off] = value
            cases.append(Case(target.name, f"head[0x{off:x}]=0x{value:02x}",
                              "header", bytes(buf), target.kind, target.path,
                              f"byte at 0x{off:x} -> 0x{value:02x}"))

    # 3. One byte in the block/table area.
    blocks = block_offsets(target.kind, base, target.blocks)
    picks = blocks[:1] if quick else blocks[:3]
    for off in picks:
        for value in ((0xFF,) if quick else (0x7F, 0xFF)):
            if off >= n or base[off] == value:
                continue
            buf = bytearray(base)
            buf[off] = value
            cases.append(Case(target.name, f"blk[0x{off:x}]=0x{value:02x}",
                              "block", bytes(buf), target.kind, target.path,
                              f"block byte at 0x{off:x} -> 0x{value:02x}"))

    # 4. Insert a 0xffffffff dword where an id/count/length is expected: a
    #    length-prefixed block claiming 4 GiB must be rejected, not allocated.
    insert_at = (SAVE_HEADER,) if target.kind == "save" else (0,)
    if not quick:
        insert_at = insert_at + ((SAVE_HEADER + 1, n // 2) if target.kind == "save"
                                 else (4, n // 2))
    for off in insert_at:
        if off > n:
            continue
        data = base[:off] + struct.pack("<I", 0xFFFFFFFF) + base[off:]
        cases.append(Case(target.name, f"ins_ffffffff[0x{off:x}]", "insert",
                          data, target.kind, target.path,
                          f"0xffffffff inserted at 0x{off:x}"))

    # 5. Zero a block count/size dword.
    zeros = blocks[:3] if target.kind == "save" else blocks[:2]
    if quick:
        zeros = zeros[:1]
    for off in zeros:
        data = _put(base, off, 0)
        if data is not None and data != base:
            cases.append(Case(target.name, f"zero_cnt[0x{off:x}]", "zero",
                              data, target.kind, target.path,
                              f"u32 at 0x{off:x} -> 0"))

    # 6. Random byte replacements, 1..8 per case, from the documented LCG.
    for index in range(1 if quick else 48):
        blob = bytearray(base)
        count = 1 + rng.below(8)
        touched = []
        for _ in range(count):
            pos = rng.below(n)
            value = rng.byte()
            if blob[pos] == value:
                value ^= 0x5A            # every mutant must differ from the seed
            blob[pos] = value
            touched.append((pos, value))
        note = ", ".join(f"0x{pos:x}=0x{value:02x}" for pos, value in touched)
        cases.append(Case(target.name, f"rnd[{index}]", "random", bytes(blob),
                          target.kind, target.path, note))

    # Two families can still land on the same bytes (a random case may rewrite
    # a header byte to the value another family picked).  Keep the first
    # occurrence so the run never spends a slot on an identical input.
    unique: list[Case] = []
    seen: set[str] = set()
    for case in cases:
        digest = sha256_bytes(case.data)
        if digest in seen:
            continue
        seen.add(digest)
        unique.append(case)
    return unique


# ---------------------------------------------------------------------------
# execution
# ---------------------------------------------------------------------------


@dataclass
class Phase:
    name: str
    command: list[str]
    rc: int | None = None
    signal: int | None = None
    timed_out: bool = False
    sanitizer: str | None = None
    stdout: str = ""
    stderr: str = ""
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return (not self.timed_out and self.signal is None
                and self.sanitizer is None and self.rc in OK_CODES)


def run_phase(name: str, command: list[str], timeout: float) -> Phase:
    phase = Phase(name, command)
    started = time.monotonic()
    try:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, start_new_session=True)
    except OSError as exc:
        phase.stderr = f"cannot start: {exc}"
        phase.seconds = time.monotonic() - started
        return phase
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        phase.timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            proc.kill()
        try:
            out, err = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            out, err = b"", b""
    phase.seconds = time.monotonic() - started
    phase.stdout = out.decode("utf-8", "replace")
    phase.stderr = err.decode("utf-8", "replace")
    rc = proc.returncode
    if rc is not None and rc < 0:
        phase.signal = -rc
    else:
        phase.rc = rc
    for marker in SANITIZER_MARKERS:
        if marker in phase.stderr or marker in phase.stdout:
            phase.sanitizer = marker
            break
    return phase


def _tail(text: str, limit: int = 800) -> str:
    text = text.strip()
    return text if len(text) <= limit else "…" + text[-limit:]


def execute(cases: list[Case], bin_path: str, game: str, out_root: str,
            timeout: float, keep: bool) -> list[dict]:
    """Run every case, return the per-case report records."""
    records: list[dict] = []
    total = len(cases)
    for index, case in enumerate(cases, 1):
        stem = f"{index:03d}-{_slug(case.target)}-{_slug(case.kind)}"
        case_dir = os.path.join(out_root, stem)
        if os.path.isdir(case_dir):
            shutil.rmtree(case_dir)
        os.makedirs(case_dir)

        ext = {"save": ".sbt", "world": ".sbw", "level": ".sbt"}[case.target_kind]
        input_path = os.path.join(case_dir, "input" + ext)
        with open(input_path, "wb") as handle:
            handle.write(case.data)

        phases: list[Phase] = []
        phases.append(run_phase("dump", [bin_path, "--dump", input_path],
                                timeout))

        if case.target_kind == "save":
            out_path = os.path.join(case_dir, "out.sbt")
            phases.append(run_phase("save-out",
                                    [bin_path, "--dump", input_path,
                                     "--save-out", out_path], timeout))
        else:
            level_dir = os.path.join(case_dir, "game")
            os.makedirs(level_dir)
            name = "fz"
            mutant_name = (name + ".sbw" if case.target_kind == "world"
                           else name + ".sbt")
            os.symlink(input_path, os.path.join(level_dir, mutant_name))
            shipped = os.path.dirname(case.seed_path)
            stem_name = os.path.splitext(os.path.basename(case.seed_path))[0]
            for other in (".sbw", ".sbt", ".sbc"):
                if other == os.path.splitext(mutant_name)[1]:
                    continue
                source = find_ci(shipped, stem_name + other)
                if source is not None:
                    os.symlink(source, os.path.join(level_dir, name + other))
            phases.append(run_phase("info",
                                    [bin_path, "--game", level_dir,
                                     "--level", name, "--info"], timeout))

        record = {
            "case": case.name,
            "target": case.target,
            "kind": case.kind,
            "size": len(case.data),
            "sha256": sha256_bytes(case.data),
            "note": case.note,
            "dir": case_dir,
            "phases": [],
            "ok": True,
        }
        for phase in phases:
            item = {
                "name": phase.name,
                "rc": phase.rc,
                "signal": phase.signal,
                "timed_out": phase.timed_out,
                "sanitizer": phase.sanitizer,
                "seconds": round(phase.seconds, 3),
                "ok": phase.ok,
            }
            if not phase.ok:
                item["command"] = phase.command
                item["stderr"] = _tail(phase.stderr)
                item["stdout"] = _tail(phase.stdout)
                record["ok"] = False
            record["phases"].append(item)
        if record["ok"] and not keep:
            shutil.rmtree(case_dir, ignore_errors=True)
            record["dir"] = None
        records.append(record)
        if index % 100 == 0 or index == total:
            print(f"fuzz_saves: {index}/{total} cases", file=sys.stderr)
    return records


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------


def summarise(records: list[dict]) -> dict:
    failures = [r for r in records if not r["ok"]]
    hangs = [r for r in records
             if any(p["timed_out"] for p in r["phases"])]
    asan = [r for r in records
            if any(p["sanitizer"] for p in r["phases"])]
    signals = [r for r in records
               if any(p["signal"] for p in r["phases"])]
    return {
        "cases": len(records),
        "failures": len(failures),
        "hangs": len(hangs),
        "asan": len(asan),
        "signals": len(signals),
        "failed": failures,
    }


def looks_like_sanitizer_binary(path: str) -> bool:
    """True when the binary lives in a ``build/asan*`` tree (informational)."""
    parts = os.path.realpath(path).split(os.sep)
    return any(part.startswith("asan") for part in parts)


def check_out_dir(out_dir: str, game: str) -> str | None:
    """Refuse an output directory outside the approved scratch roots.

    The game installation is read-only; a typo like ``--out "$GAME"`` must be
    an error, not a silent write into the shipped files.
    """
    resolved = os.path.realpath(out_dir)
    game_real = os.path.realpath(game)
    if resolved == game_real or resolved.startswith(game_real + os.sep):
        return f"--out {out_dir} is inside the game directory (read-only)"
    roots = tuple(os.path.realpath(root) for root in OUT_ROOTS)
    if not any(resolved == root or resolved.startswith(root + os.sep)
               for root in roots):
        return (f"--out {out_dir} is outside {', '.join(OUT_ROOTS)}; "
                "fuzz artefacts must not land in the repository or the game")
    return None


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------


def self_test() -> int:
    """Cheap invariant checks for the generator (no subprocess, no corpus)."""
    checks = 0

    def expect(condition: bool, label: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise SystemExit(f"self-test: FAILED {label}")

    # The LCG is a documented sequence, not Python's global RNG.
    first = Lcg(0)
    expect([first.u64() for _ in range(3)] ==
           [1442695040888963407, 1876011003808476466, 11166244414315200793],
           "lcg sequence")
    expect(Lcg(7).u64() == Lcg(7).u64(), "lcg determinism")
    expect(stream_seed(3, "save0") == stream_seed(3, "save0"), "stream seed")

    base = bytes(range(256)) * 32  # 8192 bytes
    target = Target("selftest", "save", "x.sbt", "x.sbt",
                    (SAVE_HEADER, 0x800, 0x1000))
    full_a = build_cases(target, base, 5, False)
    full_b = build_cases(target, base, 5, False)
    quick = build_cases(target, base, 5, True)
    expect([c.data for c in full_a] == [c.data for c in full_b],
           "same seed -> same bytes")
    expect(83 <= len(full_a) <= 87, f"full case count ({len(full_a)})")
    expect(len(quick) == 8, f"quick case count ({len(quick)})")
    expect(len({sha256_bytes(c.data) for c in full_a}) == len(full_a),
           "all mutants distinct")
    expect(sorted({c.kind for c in full_a}) ==
           ["block", "header", "insert", "random", "trunc", "zero"],
           "mutation families")
    expect(all(c.data for c in full_a), "no empty mutant")
    expect({len(c.data) for c in full_a if c.kind == "trunc"} ==
           {1, 2, len(base) // 2 - 1}, "truncation ladder")
    expect(all(c.kind != "trunc" or base.startswith(c.data) for c in full_a),
           "truncations stay prefixes")
    expect(any(c.kind == "insert" and c.data.count(b"\xff\xff\xff\xff")
               for c in full_a), "0xffffffff inserted")
    expect(any(c.kind == "zero" for c in full_a), "zeroed count")

    expect(check_out_dir("/mnt/d/games/Gunmetal 3dfx/x", DEFAULT_GAME) is not None,
           "game dir rejected as --out")
    expect(check_out_dir("/tmp/opencode/fuzz_saves", DEFAULT_GAME) is None,
           "/tmp/opencode accepted")
    expect(looks_like_sanitizer_binary("/x/build/asan/engine/gunmetal"), "asan path")
    expect(not looks_like_sanitizer_binary("/x/build/sandbox/engine/gunmetal"),
           "plain path")
    expect(check_out_dir(os.path.join(ROOT, "out"), DEFAULT_GAME) is not None,
           "repo path rejected as --out")
    print(f"fuzz_saves: self-test {checks} checks, ok")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--bin", default=DEFAULT_BIN,
                    help="gunmetal binary (default build/sandbox; pass "
                         "build/asan/engine/gunmetal for the sanitizer pass)")
    ap.add_argument("--seed", type=int, default=0,
                    help="generator seed, printed in the report")
    ap.add_argument("--quick", action="store_true",
                    help="8 mutants per seed instead of the full matrix")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="wall-clock budget per CLI invocation (default 20 s)")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="scratch directory (default /tmp/opencode/fuzz_saves)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the directory of every case, not just failures")
    ap.add_argument("--json", action="store_true",
                    help="print one machine-readable JSON line")
    ap.add_argument("--self-test", action="store_true",
                    help="run the generator invariants and exit")
    opts = ap.parse_args(argv)

    if opts.self_test:
        return self_test()

    if not os.path.isfile(opts.bin) or not os.access(opts.bin, os.X_OK):
        print(f"fuzz_saves: binary not found or not executable: {opts.bin}",
              file=sys.stderr)
        return 2
    if not os.path.isdir(opts.game):
        print(f"fuzz_saves: game directory not found: {opts.game}",
              file=sys.stderr)
        return 2
    problem = check_out_dir(opts.out, opts.game)
    if problem:
        print(f"fuzz_saves: {problem}", file=sys.stderr)
        return 2

    targets = collect_targets(opts.game, opts.bin, opts.timeout)
    if not targets:
        print("fuzz_saves: no seed files found in the game directory",
              file=sys.stderr)
        return 2

    cases: list[Case] = []
    per_target: list[dict] = []
    for target in targets:
        try:
            with open(target.path, "rb") as handle:
                base = handle.read()
        except OSError as exc:
            print(f"fuzz_saves: cannot read {target.path}: {exc}", file=sys.stderr)
            continue
        built = build_cases(target, base, opts.seed, opts.quick)
        cases.extend(built)
        per_target.append({
            "name": target.name,
            "kind": target.kind,
            "file": os.path.abspath(target.path),
            "size": len(base),
            "blocks": list(target.blocks),
            "cases": len(built),
        })

    os.makedirs(opts.out, exist_ok=True)
    mode = "quick" if opts.quick else "full"
    asan_bin = os.path.realpath(DEFAULT_ASAN_BIN) if os.path.isfile(DEFAULT_ASAN_BIN) else None
    started = time.monotonic()
    records = execute(cases, os.path.abspath(opts.bin), opts.game,
                      os.path.abspath(opts.out), opts.timeout, opts.keep)
    elapsed = time.monotonic() - started
    summary = summarise(records)
    exit_code = 1 if (summary["failures"] or summary["hangs"]
                      or summary["asan"]) else 0

    if opts.json:
        report = {
            "tool": "tools/qa/fuzz_saves.py",
            "mode": mode,
            "seed": opts.seed,
            "bin": os.path.abspath(opts.bin),
            "game": os.path.abspath(opts.game),
            "out": os.path.abspath(opts.out),
            "timeout": opts.timeout,
            "asan_binary": asan_bin,
            "asan_pass": (looks_like_sanitizer_binary(opts.bin)
                          or (bool(asan_bin)
                              and os.path.realpath(opts.bin) == asan_bin)),
            "cases": summary["cases"],
            "failures": summary["failures"],
            "hangs": summary["hangs"],
            "asan": summary["asan"],
            "signals": summary["signals"],
            "seconds": round(elapsed, 2),
            "targets": per_target,
            "inputs": records,
        }
        print(json.dumps(report, sort_keys=True, ensure_ascii=True,
                         separators=(",", ":")))
        return exit_code

    print(f"fuzz_saves: cases={summary['cases']} failures={summary['failures']} "
          f"hangs={summary['hangs']} asan={summary['asan']} seed={opts.seed} "
          f"mode={mode} bin={os.path.abspath(opts.bin)}")
    print(f"fuzz_saves: {len(per_target)} seed files, "
          f"{elapsed:.1f} s, scratch {os.path.abspath(opts.out)}")
    if asan_bin:
        state = ("current" if looks_like_sanitizer_binary(opts.bin)
                 else "available")
        print(f"fuzz_saves: sanitizer binary {state}: {asan_bin}")
    for record in summary["failed"][:20]:
        phase = next(p for p in record["phases"] if not p["ok"])
        detail = ("timeout" if phase["timed_out"]
                  else f"signal {phase['signal']}" if phase["signal"]
                  else phase["sanitizer"] if phase["sanitizer"]
                  else f"exit {phase['rc']}")
        print(f"  {record['case']}: {phase['name']}: {detail} "
              f"(case {record['dir']})")
    if len(summary["failed"]) > 20:
        print(f"  ... {len(summary['failed']) - 20} more")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
