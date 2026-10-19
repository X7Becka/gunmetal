#!/usr/bin/env python3
"""Validate and print ``tools/re/names.csv`` — the engine address map.

The map is deliberately small but *proven*: every row points at code or data we
have exercised (a checker in ``tools/re`` or a documented experiment), with the
evidence in the last column.  This is the machine-readable counterpart of
``docs/FORMATS.md`` / ``NOTES.md`` and the start of ``ROADMAP.md`` M2.1
(``names.csv``) without requiring Ghidra.

Checks:
  * every VA falls inside a section of the installed ``gundx.exe``;
  * kind is ``code`` for executable ranges and ``data`` for the rest;
  * names are unique and the CSV is well-formed.

Usage:  python3 tools/re/names.py [--game DIR] [--filter SUBSTR] [--json]
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import exelib  # noqa: E402

CSV_PATH = os.path.join(HERE, "names.csv")


def load(path: str = CSV_PATH) -> list[dict]:
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def section_of(sections: list[tuple], va: int):
    """``(name, vma, rawsize, fileoff)`` containing *va*, or ``None``."""
    for name, vma, size, _foff in sections:
        if vma <= va < vma + size:
            return name
    return None


def main(argv=None) -> int:
    global exelib  # noqa: PLW0603 - may be reloaded for another executable
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=exelib.DEFAULT_EXE,
                    help="gundx.exe (paths are validated against its sections)")
    ap.add_argument("--filter", help="only rows whose subsystem/name contains this")
    ap.add_argument("--json", action="store_true")
    opts = ap.parse_args(argv)

    if opts.game and os.path.abspath(opts.game) != os.path.abspath(exelib.EXE):
        os.environ["GUNMETAL_EXE"] = opts.game
        import importlib
        exelib = importlib.reload(exelib)

    rows = load()
    sections = exelib.sections()
    names = set()
    bad = []
    for r in rows:
        va = int(r["va"], 16)
        if r["name"] in names:
            bad.append("%s: duplicate name" % r["name"])
        names.add(r["name"])
        sec = section_of(sections, va)
        if sec is None:
            bad.append("%s: outside the image" % r["va"])
            continue
        executable = "text" in sec.lower()
        if r["kind"] == "code" and not executable:
            bad.append("%s: marked code but in %s" % (r["va"], sec))
        if r["kind"] == "data" and executable:
            bad.append("%s: marked data but in %s" % (r["va"], sec))
        if r["kind"] not in ("code", "data", "table"):
            bad.append("%s: unknown kind %r" % (r["va"], r["kind"]))
    if opts.filter:
        rows = [r for r in rows if opts.filter.lower() in
                (r["name"] + r["subsystem"]).lower()]
    if opts.json:
        print(json.dumps({"rows": len(rows), "errors": bad, "entries": rows},
                         indent=2, ensure_ascii=False))
        return 0 if not bad else 1
    subs = {}
    for r in rows:
        subs[r["subsystem"]] = subs.get(r["subsystem"], 0) + 1
    print("names.csv: %d entries in %d subsystems" % (len(rows), len(subs)))
    for k in sorted(subs):
        print("  %-10s %2d" % (k, subs[k]))
    for r in rows:
        print("  %-8s %-4s %-34s %-10s %s"
              % (r["va"], r["kind"], r["name"], r["subsystem"], r["evidence"]))
    for msg in bad:
        print("  FAIL %s" % msg, file=sys.stderr)
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main())
