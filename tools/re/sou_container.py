#!/usr/bin/env python3
"""Reader / writer / statistics for the Gunmetal ``.sou`` container.

The payload codecs live in ``gm/sou1.py`` (4-bit ADPCM) and ``gm/sou2.py``
(format 4); this tool never decodes audio.  It exercises the container layer
that ``gm/sou.py`` exports:

``--check``
    Parse every entry of ``sounds/SOU.IDX`` and ``MUSIC/SOU.IDX``, rebuild it
    from its parsed fields and compare the result with the shipped bytes
    (``parse -> rebuild`` byte-exact).  Also runs the negative ``--self-test``
    cases, so one command proves both the whole corpus and the trust boundary.

``--stats --json``
    Container statistics: formats, rates, channels, frames, seconds, payload
    and ignored bytes, loader-rejected headers and anomalies.  No codec is
    decoded, so this is fast even for the 75 MB music archive.

``--rebuild DIR``
    Write every entry through the writer to ``DIR/<source>/<name>.sou``
    (atomic, ``--force`` needed to replace existing files).  The tool never
    writes into the game directory.  ``--source sounds|music`` limits the set.

``--self-test``
    Negative tests only (no game data needed): empty blob, bad magic, short
    header, huge declared length/units, truncated payload, bad flags, the odd
    ``length`` rule and the SOU2 tail.

Container facts (loader ``0x40af30``, see ``docs/SOU_CONTAINER.md``)::

    SOU0/SOU1: magic[4], u32 rate, u32 flags, u32 length, payload[length & ~1]
    SOU2:      magic[4], u32 rate, u32 frames, u32 meta2, u32 units,
               payload[units * 4]

Flags bit 0 = stereo.  The loader drops the last byte of an odd ``length``
(``and al,0xfe``); 481 shipped SOU0/SOU1 entries have odd lengths, 478 of them
therefore lose one byte.  SOU2 requires ``frames > 0``, ``frames % 32 == 0``
and ``units > 0``; ``L04I01`` and ``L06A01`` violate that but still round-trip.

Only the Python standard library and the pure-stdlib ``gm`` package are used.
The game directory is only ever read.

Usage::

    python3 tools/re/sou_container.py --check
    python3 tools/re/sou_container.py --stats --json
    python3 tools/re/sou_container.py --rebuild /tmp/opencode/sou-out
    python3 tools/re/sou_container.py --rebuild /tmp/opencode/sou-out --force
    python3 tools/re/sou_container.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gm import bigfile, sou  # noqa: E402

DEFAULT_GAME = "/mnt/d/games/Gunmetal 3dfx"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
FNV_MASK = (1 << 64) - 1


def fnv1a64(data: bytes, seed: int = FNV_OFFSET) -> int:
    value = seed
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & FNV_MASK
    return value


# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------

def archive_specs(opts) -> list[tuple[str, str]]:
    return [("sounds", opts.sounds), ("music", opts.music)]


def selected_archives(opts) -> list[tuple[str, str]]:
    specs = archive_specs(opts)
    if opts.source == "sounds":
        specs = specs[:1]
    elif opts.source == "music":
        specs = specs[1:]
    return specs


def load_entries(opts) -> list[tuple[str, str, bytes]]:
    """``[(source, name, blob), ...]`` for the selected archives."""
    out: list[tuple[str, str, bytes]] = []
    for source, idx in selected_archives(opts):
        if not os.path.isfile(idx):
            raise FileNotFoundError(idx)
        bf = bigfile.BigFile(idx)
        for entry in bf:
            out.append((source, entry.name, bf.read(entry)))
    return out


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def cmd_check(opts) -> int:
    rows = load_entries(opts)
    exact = parsed = 0
    digest = FNV_OFFSET
    failures: list[str] = []
    per_source: dict[str, dict] = {}

    seen: dict[str, list[tuple[str, bytes]]] = {}
    for source, name, blob in rows:
        seen.setdefault(source, []).append((name, blob))
        try:
            snd = sou.parse(blob)
            rebuilt = snd.rebuild()
            again = sou.parse(rebuilt)
        except ValueError as exc:
            failures.append(f"{source}/{name}: {exc}")
            continue
        parsed += 1
        digest = fnv1a64(rebuilt, digest)
        if rebuilt != blob:
            failures.append(f"{source}/{name}: rebuild differs "
                            f"({len(rebuilt)} vs {len(blob)} bytes)")
        elif (again.magic, again.sample_rate, again.flags, again.payload,
              again.meta, again.trailer) != (
                  snd.magic, snd.sample_rate, snd.flags, snd.payload,
                  snd.meta, snd.trailer):
            failures.append(f"{source}/{name}: fields change across parse")
        else:
            exact += 1

    for source, blobs in seen.items():
        st = sou.stats(blobs)
        st["fnv1a64"] = "0x%016x" % fnv1a64(
            b"".join(data for _n, data in blobs))
        per_source[source] = st

    negatives = self_test(quiet=True)
    neg_ok = sum(1 for _name, ok, _detail in negatives if ok)
    total = len(rows)

    if opts.json:
        print(json.dumps({
            "entries": total,
            "parsed": parsed,
            "byte_exact": exact,
            "failures": failures,
            "fnv1a64": "0x%016x" % digest,
            "archives": per_source,
            "negative": {"passed": neg_ok, "total": len(negatives),
                         "cases": [{"name": n, "ok": ok, "detail": d}
                                   for n, ok, d in negatives]},
        }, indent=2, ensure_ascii=False))
    else:
        summary = ", ".join("%s=%d" % (source, st["parsed"])
                            for source, st in per_source.items())
        print(f"sou container: {total} entries ({summary})")
        print(f"  parse                    : {parsed}/{total}")
        print(f"  rebuild byte-exact       : {exact}/{parsed}")
        print(f"  FNV-1a64(rebuild)        : 0x{digest:016x}")
        for source, st in per_source.items():
            print(f"  {source:7s} formats           : {st['formats']}")
            print(f"  {source:7s} rates / channels  : {st['rates']} / "
                  f"{st['channels']}")
            print(f"  {source:7s} seconds           : {st['seconds']:.3f} "
                  f"({st['frames']} frames)")
            print(f"  {source:7s} loadable / ignored: {st['loadable']}/{st['parsed']}"
                  f" / {st['ignored_bytes']} B")
            if st["anomalies"]:
                print(f"  {source:7s} anomalies         : {st['anomalies']}")
            if st["errors"]:
                for err in st["errors"]:
                    print(f"    FAIL {err['name']}: {err['error']}",
                          file=sys.stderr)
        print(f"  negative tests           : {neg_ok}/{len(negatives)}")
        for line in failures[:30]:
            print(f"  FAIL {line}", file=sys.stderr)
        if len(failures) > 30:
            print(f"  … {len(failures) - 30} more failures", file=sys.stderr)

    return 0 if (parsed == total and exact == parsed
                 and neg_ok == len(negatives) and not failures) else 1


# ---------------------------------------------------------------------------
# --stats
# ---------------------------------------------------------------------------

def cmd_stats(opts) -> int:
    rows = load_entries(opts)
    grouped: dict[str, list[tuple[str, bytes]]] = {}
    for source, name, blob in rows:
        grouped.setdefault(source, []).append((name, blob))

    total = sou.stats((name, blob) for _s, name, blob in rows)
    report = {
        "archives": {source: sou.stats(blobs)
                     for source, blobs in grouped.items()},
        "total": total,
    }
    if opts.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return 0

    for source, st in list(report["archives"].items()) + [("total", total)]:
        print(f"{source}: {st['entries']} entries, {st['bytes']} bytes")
        print(f"  parsed / bad        : {st['parsed']} / {st['bad']}")
        print(f"  formats             : {st['formats']}")
        print(f"  rates               : {st['rates']}")
        print(f"  channels            : {st['channels']}")
        print(f"  payload bytes       : {st['payload_bytes']}")
        print(f"  frames / seconds    : {st['frames']} / {st['seconds']:.3f}")
        print(f"  loadable headers    : {st['loadable']}/{st['parsed']}")
        print(f"  ignored bytes       : {st['ignored_bytes']}")
        if st["anomalies"]:
            print(f"  anomalies           : {st['anomalies']}")
        for err in st["errors"]:
            print(f"  bad {err['name']}: {err['error']}", file=sys.stderr)
    return 0 if total["bad"] == 0 else 1


# ---------------------------------------------------------------------------
# --rebuild
# ---------------------------------------------------------------------------

def _inside(path: str, directory: str) -> bool:
    path = os.path.realpath(path)
    directory = os.path.realpath(directory)
    try:
        return os.path.commonpath([path, directory]) == directory
    except ValueError:
        return False


def write_atomic(path: str, data: bytes, force: bool) -> None:
    if os.path.exists(path) and not force:
        raise FileExistsError(f"refusing to overwrite {path} (use --force)")
    directory = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".sou-")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def cmd_rebuild(opts) -> int:
    game = opts.game
    if _inside(opts.rebuild, game):
        print(f"sou-container: refusing to write inside the game directory "
              f"({opts.rebuild})", file=sys.stderr)
        return 2
    rows = load_entries(opts)
    os.makedirs(opts.rebuild, exist_ok=True)

    written = exact = failed = 0
    used: set[str] = set()
    errors: list[str] = []
    for source, name, blob in rows:
        safe = bigfile.safe_name(name, "unnamed")
        key = (source, safe.lower())
        if key in used:
            safe = f"{safe}_{len(used):04d}"
            key = (source, safe.lower())
        used.add(key)
        target = os.path.join(opts.rebuild, source, safe + ".sou")
        try:
            snd = sou.parse(blob)
            data = snd.rebuild()
            write_atomic(target, data, opts.force)
            with open(target, "rb") as fh:
                on_disk = fh.read()
            if on_disk != blob:
                raise ValueError("written file differs from the source")
            written += 1
            if on_disk == blob:
                exact += 1
        except (OSError, ValueError) as exc:
            failed += 1
            errors.append(f"{source}/{name}: {exc}")

    for line in errors[:20]:
        print(f"  FAIL {line}", file=sys.stderr)
    if len(errors) > 20:
        print(f"  … {len(errors) - 20} more failures", file=sys.stderr)
    print(f"sou container: {written} written to {opts.rebuild}, "
          f"{exact} byte-identical to the source, {failed} failed")
    return 0 if failed == 0 else 1


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def _expect_error(name: str, blob: bytes) -> tuple[str, bool, str]:
    try:
        sou.parse(blob)
    except ValueError as exc:
        return name, True, str(exc)
    return name, False, "parsed, but should have been rejected"


def _expect_roundtrip(name: str, blob: bytes) -> tuple[str, bool, str]:
    try:
        snd = sou.parse(blob)
        rebuilt = snd.rebuild()
    except ValueError as exc:
        return name, False, str(exc)
    return name, rebuilt == blob, f"{len(blob)} bytes"


def self_test(quiet: bool = False) -> list[tuple[str, bool, str]]:
    results: list[tuple[str, bool, str]] = []

    results.append(_expect_error("empty blob", b""))
    results.append(_expect_error("bad magic", b"XXXX" + b"\0" * 12))
    results.append(_expect_error(
        "short SOU0 header",
        b"SOU0" + (8000).to_bytes(4, "little")))
    results.append(_expect_error("zero sample rate",
                                 b"SOU1" + b"\0" * 12))
    results.append(_expect_error("bad flags",
                                 b"SOU1" + (1000).to_bytes(4, "little")
                                 + (2).to_bytes(4, "little")
                                 + (4).to_bytes(4, "little") + b"\0" * 4))
    results.append(_expect_error(
        "huge length (0xffffffff)",
        b"SOU1" + (1000).to_bytes(4, "little")
        + (0).to_bytes(4, "little")
        + (0xFFFFFFFF).to_bytes(4, "little") + b"\0" * 8))
    results.append(_expect_error(
        "truncated SOU1 payload",
        b"SOU1" + (1000).to_bytes(4, "little")
        + (0).to_bytes(4, "little")
        + (100).to_bytes(4, "little") + b"\0" * 50))
    results.append(_expect_error(
        "short SOU2 header",
        b"SOU2" + (32000).to_bytes(4, "little") + b"\0" * 2))
    results.append(_expect_error(
        "huge SOU2 units",
        b"SOU2" + (32000).to_bytes(4, "little")
        + (32).to_bytes(4, "little") + (0).to_bytes(4, "little")
        + (sou.MAX_SOU2_UNITS + 1).to_bytes(4, "little")))
    results.append(_expect_error(
        "truncated SOU2 payload",
        b"SOU2" + (32000).to_bytes(4, "little")
        + (32).to_bytes(4, "little") + (0).to_bytes(4, "little")
        + (10).to_bytes(4, "little") + b"\0" * 8))

    # Odd length: loader reads ``length & ~1``; the last byte is preserved in
    # the tail, so parse -> rebuild must still be byte-exact.
    odd = (b"SOU1" + (8000).to_bytes(4, "little")
           + (0).to_bytes(4, "little") + (5).to_bytes(4, "little")
           + b"\x01\x02\x03\x04\x05")
    results.append(_expect_roundtrip("odd length round-trip", odd))

    # SOU2 anomaly shape: units == 0 with a tail (L06A01).
    anomal = (b"SOU2" + (32000).to_bytes(4, "little")
              + (1).to_bytes(4, "little") + (0).to_bytes(4, "little")
              + (0).to_bytes(4, "little") + b"\xaa" * 7)
    results.append(_expect_roundtrip("SOU2 units=0 + tail", anomal))

    # Edited payload: canonical length, still parseable.
    try:
        snd = sou.parse(b"SOU0" + (8000).to_bytes(4, "little")
                        + (0).to_bytes(4, "little")
                        + (4).to_bytes(4, "little") + b"\x01\x02\x03\x04")
        snd.payload = b"\x10\x20"
        edited = snd.to_bytes()
        again = sou.parse(edited)
        ok = edited[12:16] == (2).to_bytes(4, "little") and again.payload == snd.payload
        results.append(("edited payload uses canonical length", ok,
                        f"{len(edited)} bytes"))
    except ValueError as exc:
        results.append(("edited payload uses canonical length", False, str(exc)))

    # Overwrite policy.
    tmp = tempfile.mkdtemp(prefix="sou-self-test-")
    try:
        path = os.path.join(tmp, "x.sou")
        write_atomic(path, b"one", force=False)
        try:
            write_atomic(path, b"two", force=False)
            ok, detail = False, "overwrote without --force"
        except FileExistsError:
            ok, detail = True, "refused without --force"
        results.append(("overwrite policy", ok, detail))
        write_atomic(path, b"two", force=True)
        results.append(("force overwrites", open(path, "rb").read() == b"two",
                        "atomic replace"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if not quiet:
        for name, ok, detail in results:
            print(f"  {'ok  ' if ok else 'FAIL'} {name}: {detail}")
        passed = sum(1 for _n, ok, _d in results if ok)
        print(f"sou container self-test: {passed}/{len(results)} passed")
    return results


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Gunmetal .sou container: check, stats, rebuild")
    ap.add_argument("--game", default=os.environ.get("GUNMETAL_GAME", DEFAULT_GAME),
                    help="installation root (read-only)")
    ap.add_argument("--sounds", help="sounds/SOU.IDX (default: <game>/sounds/SOU.IDX)")
    ap.add_argument("--music", help="MUSIC/SOU.IDX (default: <game>/MUSIC/SOU.IDX)")
    ap.add_argument("--source", choices=("all", "sounds", "music"), default="all")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--force", action="store_true",
                    help="replace existing files in --rebuild")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true",
                       help="parse -> rebuild on the whole corpus + negative tests")
    group.add_argument("--stats", action="store_true",
                       help="container statistics (no decoding)")
    group.add_argument("--rebuild", metavar="DIR",
                       help="write every entry through the writer")
    group.add_argument("--self-test", action="store_true",
                       help="negative tests, no game data needed")
    opts = ap.parse_args(argv)

    opts.sounds = opts.sounds or os.path.join(opts.game, "sounds", "SOU.IDX")
    opts.music = opts.music or os.path.join(opts.game, "MUSIC", "SOU.IDX")

    if opts.self_test:
        return 0 if all(ok for _n, ok, _d in self_test()) else 1
    if opts.stats:
        try:
            return cmd_stats(opts)
        except (OSError, ValueError) as exc:
            print(f"sou-container: {exc}", file=sys.stderr)
            return 2
    if opts.rebuild:
        try:
            return cmd_rebuild(opts)
        except (OSError, ValueError) as exc:
            print(f"sou-container: {exc}", file=sys.stderr)
            return 2
    if opts.check:
        try:
            return cmd_check(opts)
        except (OSError, ValueError) as exc:
            print(f"sou-container: {exc}", file=sys.stderr)
            return 2
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
