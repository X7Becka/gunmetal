#!/usr/bin/env python3
"""Budgeted context extraction for a 2.5 MB documentation tree.

The point is token economy: an agent should never ``cat`` ``FORMATS.md``
(207 KB) or ``ROADMAP.md`` (126 KB) to answer "what is at ``0x4752d0``?".
These commands print only the matching lines/sections, capped by ``--max``
(default 40 lines) and trimmed to 200 columns, so a query costs a few hundred
tokens instead of tens of thousands.

    python3 tools/ctx.py outline FORMATS      # headings of one doc
    python3 tools/ctx.py doc FORMATS sbw      # just the .sbw section
    python3 tools/ctx.py addr 0x4752d0        # every place that documents it
    python3 tools/ctx.py find "target_history" --scope docs
    python3 tools/ctx.py find "apply_weapon_blast" --scope code --max 10
    python3 tools/ctx.py task                 # top of NEXT.md, compact
    python3 tools/ctx.py index                # regenerate docs/INDEX.md

Subagents should prefer ``addr``/``doc``/``find`` over ``read`` on any file
above ~20 KB; ``read`` is for source files you are about to edit.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
ROOT_DOCS = ["AGENTS.md", "README.md", "NOTES.md", "ROADMAP.md",
             "CURRENT_STATE.md", "NEXT.md"]
CODE_ROOTS = ["gm", "engine", "tools", "docs"]
CODE_FILES = ["gm.py", "regress.py"]
ADDR_RE = re.compile(r"0x[0-9a-fA-F]{4,8}\b")
HEAD_RE = re.compile(r"^(#{1,6})\s+(.*)$")
WIDTH = 200


def doc_paths() -> list[str]:
    out = []
    for name in ROOT_DOCS:
        path = os.path.join(ROOT, name)
        if os.path.exists(path):
            out.append(path)
    out.extend(sorted(glob.glob(os.path.join(DOCS, "*.md"))))
    out.extend(sorted(glob.glob(os.path.join(DOCS, "ADR", "*.md"))))
    return out


def code_paths() -> list[str]:
    out = [os.path.join(ROOT, f) for f in CODE_FILES if os.path.exists(os.path.join(ROOT, f))]
    for sub in CODE_ROOTS:
        base = os.path.join(ROOT, sub)
        if not os.path.isdir(base):
            continue
        for cur, dirs, files in os.walk(base):
            dirs[:] = sorted(d for d in dirs
                             if d not in ("__pycache__", "re", "node_modules"))
            for name in sorted(files):
                if name.endswith((".py", ".h", ".cpp", ".hpp", ".md", ".csv")):
                    out.append(os.path.join(cur, name))
    return out


def _rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def _read(path: str) -> list[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def _trim(text: str) -> str:
    text = text.strip()
    return text if len(text) <= WIDTH else text[:WIDTH - 3] + "..."


def resolve_doc(name: str) -> str:
    """``FORMATS``/``docs/FORMATS.md``/``formats`` -> absolute path."""
    want = name.lower().replace(os.sep, "/")
    if want.endswith(".md"):
        want = want[:-3]
    want = os.path.basename(want)
    for path in doc_paths():
        stem = os.path.splitext(os.path.basename(path))[0].lower()
        if stem == want:
            return path
    cands = [p for p in doc_paths() if want in os.path.basename(p).lower()]
    if len(cands) == 1:
        return cands[0]
    if not cands:
        sys.exit("ctx: no doc matches %r (try: ctx.py ls)" % name)
    sys.exit("ctx: %r is ambiguous: %s" % (name, ", ".join(_rel(p) for p in cands)))


def section(lines: list[str], heading: re.Pattern, max_lines: int) -> tuple[list[str], int]:
    """Heading block(s) matching *heading*, capped at *max_lines* lines."""
    out: list[str] = []
    total = 0
    i = 0
    while i < len(lines):
        m = HEAD_RE.match(lines[i])
        if m and heading.search(m.group(2)):
            level = len(m.group(1))
            start = i
            j = i + 1
            while j < len(lines):
                m2 = HEAD_RE.match(lines[j])
                if m2 and len(m2.group(1)) <= level:
                    break
                j += 1
            total += j - start
            out.extend(lines[start:j])
            out.append("")
            i = j
            continue
        i += 1
    return out, total


def cmd_ls(opts) -> int:
    rows = []
    for path in doc_paths():
        title = ""
        for line in _read(path)[:40]:
            if line.startswith("# "):
                title = line[2:].strip()
                break
        rows.append((_rel(path), title))
    width = max((len(r[0]) for r in rows), default=0)
    for rel, title in rows:
        print("%-*s  %s" % (width, rel, _trim(title)))
    return 0


def cmd_outline(opts) -> int:
    path = resolve_doc(opts.doc)
    for no, line in enumerate(_read(path), 1):
        m = HEAD_RE.match(line)
        if m:
            print("%5d  %s%s" % (no, "  " * (len(m.group(1)) - 1), m.group(2)))
    return 0


def cmd_doc(opts) -> int:
    path = resolve_doc(opts.doc)
    lines = _read(path)
    if opts.heading:
        pattern = re.compile(opts.heading, re.IGNORECASE)
    else:                                  # no heading: print the outline only
        return cmd_outline(opts)
    block, total = section(lines, pattern, opts.max)
    if not block:
        print("ctx: no section matches %r in %s" % (opts.heading, _rel(path)))
        print("     headings: %s" % ", ".join(
            m.group(2) for m in (HEAD_RE.match(l) for l in lines) if m)[:600])
        return 1
    shown = block[:opts.max]
    for line in shown:
        print(_trim(line))
    if total > len(shown):
        print("... (%d of %d section lines; --max to widen)"
              % (len(shown), total))
    return 0


def _files(scope: str) -> list[str]:
    if scope == "docs":
        return doc_paths()
    if scope == "code":
        return code_paths()
    return doc_paths() + code_paths()


def cmd_find(opts) -> int:
    try:
        pattern = re.compile(opts.pattern, re.IGNORECASE if opts.i else 0)
    except re.error as exc:
        sys.exit("ctx: bad regex: %s" % exc)
    per_file: dict[str, list[tuple[int, str]]] = {}
    total = 0
    for path in _files(opts.scope):
        try:
            if os.path.getsize(path) > 3 << 20:
                continue
        except OSError:
            continue
        hits = []
        for no, line in enumerate(_read(path), 1):
            if pattern.search(line):
                hits.append((no, _trim(line)))
                total += 1
                if total >= opts.max * 8:
                    break
        if hits:
            per_file[_rel(path)] = hits
    shown = 0
    for rel, hits in per_file.items():
        print("## %s (%d)" % (rel, len(hits)))
        for no, line in hits:
            if shown >= opts.max:
                break
            print("%5d: %s" % (no, line))
            shown += 1
        if shown >= opts.max:
            break
    print("-- %d matches in %d files, %d shown (--max %d)"
          % (total, len(per_file), shown, opts.max))
    return 0


def cmd_addr(opts) -> int:
    raw = opts.addr.lower()
    if raw.startswith("0x"):
        raw = raw[2:]
    if not re.fullmatch(r"[0-9a-f]{4,8}", raw):
        sys.exit("ctx: address must be 4-8 hex digits or 0x-prefixed")
    pattern = re.compile(r"(?<![0-9a-f])0x0*%s(?![0-9a-f])" % raw)
    opts.pattern = pattern.pattern
    opts.i = True
    return cmd_find(opts)


def cmd_task(opts) -> int:
    path = os.path.join(ROOT, "NEXT.md")
    lines = _read(path)
    start = next((i for i, l in enumerate(lines) if l.startswith("## ")), None)
    if start is None:
        print("ctx: NEXT.md has no '## ' section")
        return 1
    end = next((j for j in range(start + 1, len(lines)) if lines[j].startswith("## ")),
               len(lines))
    block = lines[start:end]
    for line in block[:opts.max]:
        print(_trim(line))
    if len(block) > opts.max:
        print("... (%d of %d lines; --max to widen)" % (opts.max, len(block)))
    return 0


def cmd_index(opts) -> int:
    rows = []
    for path in doc_paths():
        lines = _read(path)
        title = next((l[2:].strip() for l in lines if l.startswith("# ")), "")
        heads = [m.group(2) for m in (HEAD_RE.match(l) for l in lines)
                 if m and len(m.group(1)) == 2][:3]
        addrs = len(ADDR_RE.findall("\n".join(lines)))
        rel = _rel(path)
        if not rel.startswith("docs/") or rel == "docs/INDEX.md":
            continue
        rows.append((rel, os.path.getsize(path) // 1024, addrs, title, heads))
    out = ["# Индекс документации",
           "",
           "Сгенерировано `python3 tools/ctx.py index`; не редактировать вручную.",
           "Читать целиком большие файлы не нужно: `ctx.py doc <файл> <раздел>`,",
           "`ctx.py addr 0x...`, `ctx.py outline <файл>`. Полный список — `ctx.py ls`.",
           "",
           "| файл | КБ | адресов | о чём |",
           "|---|---:|---:|---|"]
    for rel, kb, addrs, title, heads in sorted(rows):
        desc = title or (heads[0] if heads else "")
        if len(desc) > 110:
            desc = desc[:107] + "..."
        out.append("| `%s` | %d | %d | %s |" % (rel, kb, addrs, desc.replace("|", "/")))
    out += ["", "## Быстрые команды", "",
            "```sh",
            "python3 tools/ctx.py ls                      # что вообще есть",
            "python3 tools/ctx.py outline FORMATS         # оглавление и номера строк",
            "python3 tools/ctx.py doc FORMATS sbw         # только раздел про .sbw",
            "python3 tools/ctx.py addr 0x4752d0           # где документирован адрес",
            "python3 tools/ctx.py find apply_weapon_blast --scope code",
            "python3 tools/ctx.py task                    # верх NEXT.md",
            "```", ""]
    text = "\n".join(out)
    if opts.stdout:
        print(text)
        return 0
    target = os.path.join(DOCS, "INDEX.md")
    tmp = target + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    os.replace(tmp, target)
    print("ctx: wrote %s (%d rows)" % (_rel(target), len(rows)))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("ls", help="list docs with their titles")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("outline", help="headings of one doc with line numbers")
    p.add_argument("doc")
    p.set_defaults(func=cmd_outline)

    p = sub.add_parser("doc", help="print only the matching section(s)")
    p.add_argument("doc")
    p.add_argument("heading", nargs="?", help="regex over heading text")
    p.add_argument("--max", type=int, default=40, help="line budget (default 40)")
    p.set_defaults(func=cmd_doc)

    p = sub.add_parser("find", help="budgeted grep over docs/code")
    p.add_argument("pattern")
    p.add_argument("--scope", choices=("docs", "code", "all"), default="docs")
    p.add_argument("--max", type=int, default=40)
    p.add_argument("-i", action="store_true", help="case-insensitive")
    p.set_defaults(func=cmd_find)

    p = sub.add_parser("addr", help="where a VA is documented (docs and code)")
    p.add_argument("addr")
    p.add_argument("--scope", choices=("docs", "code", "all"), default="all")
    p.add_argument("--max", type=int, default=30)
    p.set_defaults(func=cmd_addr)

    p = sub.add_parser("task", help="compact view of the current NEXT.md queue")
    p.add_argument("--max", type=int, default=40)
    p.set_defaults(func=cmd_task)

    p = sub.add_parser("index", help="regenerate docs/INDEX.md")
    p.add_argument("--stdout", action="store_true")
    p.set_defaults(func=cmd_index)

    opts = ap.parse_args(argv)
    if not opts.cmd:
        ap.print_help()
        return 2
    return opts.func(opts)


if __name__ == "__main__":
    raise SystemExit(main())
