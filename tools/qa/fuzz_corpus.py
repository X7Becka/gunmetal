#!/usr/bin/env python3
"""Inspector and integrity checker of a persistent fuzz corpus.

A corpus is produced by ``tools/fuzz_formats.py --save DIR``: every hostile
input is stored as a file and described in ``DIR/manifest.json`` (target,
kind, case name, sha256, size, seed).  This tool answers three questions
about such a directory:

* is it intact?  -- every file is hashed and compared with the manifest, and
  the manifest digest is recomputed;
* what is inside? -- the per-target/per-kind histogram and the corpus digest;
* does it still pass? -- ``--replay`` runs the stored inputs through the very
  same parsers as the generator.

Usage::

    python3 tools/qa/fuzz_corpus.py --dir /var/tmp/corpus
    python3 tools/qa/fuzz_corpus.py --dir /var/tmp/corpus --json
    python3 tools/qa/fuzz_corpus.py --dir /var/tmp/corpus --replay

Exit status is 0 when the corpus is intact (and, with ``--replay``, when no
case fails) and 1 otherwise.  Stdlib only.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fuzz_formats  # noqa: E402  (needs the path tweak above)


def inspect(dirpath: str) -> tuple[dict, list[dict], list[str]]:
    """Load the manifest, verify every stored file, return the good entries."""
    manifest = fuzz_formats.load_manifest(dirpath)
    problems: list[str] = []
    good: list[dict] = []
    for entry in manifest.get("cases", []):
        rel = entry.get("file")
        path = os.path.join(dirpath, rel)
        try:
            data = fuzz_formats._read(path)
        except OSError as exc:
            problems.append(f"{rel}: cannot read: {exc}")
            continue
        digest = fuzz_formats.sha256_bytes(data)
        if digest != entry.get("sha256"):
            problems.append(f"{rel}: sha256 mismatch: expected "
                            f"{entry.get('sha256')}, got {digest}")
            continue
        if len(data) != entry.get("size"):
            problems.append(f"{rel}: size mismatch: expected "
                            f"{entry.get('size')}, got {len(data)}")
            continue
        good.append(entry)
    return manifest, good, problems


def histogram(entries: list[dict]) -> tuple[dict, dict, int]:
    targets = collections.Counter(e.get("target") for e in entries)
    kinds = collections.Counter(e.get("kind") for e in entries)
    total = sum(e.get("size", 0) for e in entries)
    return dict(targets), dict(kinds), total


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", required=True, metavar="DIR",
                    help="corpus directory (the one passed to --save)")
    ap.add_argument("--replay", action="store_true",
                    help="also run the stored inputs through the parsers")
    ap.add_argument("--json", action="store_true",
                    help="print a machine-readable report (one JSON line)")
    opts = ap.parse_args(argv)

    dirpath = opts.dir
    try:
        manifest, good, problems = inspect(dirpath)
    except fuzz_formats.CorpusError as exc:
        if opts.json:
            print(fuzz_formats.canonical_json({
                "corpus": os.path.abspath(dirpath), "ok": False,
                "cases": 0, "verified": 0, "problems": 1,
                "errors": [str(exc)],
            }))
        else:
            print(f"corpus: {exc}", file=sys.stderr)
            print(f"corpus: {dirpath}: BROKEN")
        return 1

    targets, kinds, total = histogram(good)
    meta = manifest.get("meta") or {}
    stored = manifest.get("cases", [])
    report = {
        "corpus": os.path.abspath(dirpath),
        "format": manifest.get("format"),
        "version": manifest.get("version"),
        "seed": meta.get("seed"),
        "mode": meta.get("mode"),
        "limit": meta.get("limit"),
        "game": meta.get("game"),
        "digest": manifest.get("digest"),
        "cases": len(stored),
        "verified": len(good),
        "bytes": total,
        "targets": targets,
        "kinds": kinds,
        "problems": len(problems),
        "errors": problems,
        "ok": not problems and len(good) == len(stored),
    }

    replay = None
    if opts.replay:
        cases = [fuzz_formats.Case(e.get("target"), e.get("kind", "unknown"),
                                    e.get("name", e.get("file", "?")),
                                    fuzz_formats._read(os.path.join(dirpath, e["file"])),
                                    e.get("extra", {}))
                 for e in good]
        fz, _ = fuzz_formats.execute(cases, float(meta.get("timeout", 2.0)))
        replay = {"cases": fz.cases, "failures": len(fz.failures),
                  "detail": fz.failures[:40]}
        report["replay"] = replay
        if fz.failures:
            report["ok"] = False

    if opts.json:
        print(fuzz_formats.canonical_json(report))
        return 0 if report["ok"] else 1

    print(f"corpus: {os.path.abspath(dirpath)}")
    print(f"format: {report['format']} v{report['version']}  "
          f"seed: {report['seed']}  mode: {report['mode']}  "
          f"limit: {report['limit']}")
    print(f"digest: {report['digest']}")
    print(f"cases: {report['cases']}  verified: {report['verified']}  "
          f"bytes: {total}")
    print("targets: " + " ".join(f"{k}={v}" for k, v in sorted(targets.items())))
    print("kinds: " + " ".join(f"{k}={v}" for k, v in sorted(kinds.items())))
    if replay is not None:
        print(f"replay: {replay['cases']} cases, {replay['failures']} failure(s)")
        for line in replay["detail"]:
            print(f"  {line}")
    for line in problems:
        print(f"  {line}")
    print(f"status: {'ok' if report['ok'] else 'BROKEN'}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
