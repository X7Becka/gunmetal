#!/usr/bin/env python3
"""Print the measured project snapshot without hand-counting.

Why: ``CURRENT_STATE.md`` drifted by 500 commits and its roadmap counters were
wrong because they were maintained by hand.  Counting is mechanical, so it
belongs to a script: git, roadmap statuses, test/category counts, doc sizes and
the decompiler cache.

Statuses are counted only on item lines (``- ✅ `M4.2.3` …``).  A plain
``grep -c '✅'`` also counts glyphs inside notes and comments and silently
inflates the number.

Examples::

    python3 tools/qa/state.py                 # table
    python3 tools/qa/state.py --json          # one JSON object
    python3 tools/qa/state.py --ghidra        # + project path and function count
    python3 tools/qa/state.py --check         # nonzero if the audit snapshot is behind HEAD
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATUS = {"✅": "done", "🟡": "partial", "⬜": "todo", "⛔": "blocked"}
ITEM_RE = re.compile(r"^\s*[-*]\s+(✅|🟡|⬜|⛔)\s")
STAGE_RE = re.compile(r"^##+\s+(M\d+)[.\s]")
DOC_FILES = ("docs/FORMATS.md", "ROADMAP.md", "NOTES.md", "NEXT.md",
             "CURRENT_STATE.md", "docs/RE_AUDIT.md")


def sh(args: list[str]) -> str:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True).stdout


def git_info() -> dict:
    head = sh(["git", "log", "-1", "--pretty=%H"]).strip()
    date = sh(["git", "log", "-1", "--date=short", "--pretty=%ad"]).strip()
    count = sh(["git", "rev-list", "--count", "HEAD"]).strip()
    return {"head": head[:7], "head_date": date,
            "commits": int(count) if count.isdigit() else None}


def roadmap_counts() -> dict:
    path = os.path.join(ROOT, "ROADMAP.md")
    stages: dict[str, dict[str, int]] = {}
    totals = {name: 0 for name in STATUS.values()}
    stage = None
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = STAGE_RE.match(line)
            if m:
                stage = m.group(1)
                stages.setdefault(stage, {name: 0 for name in STATUS.values()})
                continue
            m = ITEM_RE.match(line)
            if not m:
                continue
            name = STATUS[m.group(1)]
            totals[name] += 1
            if stage:
                stages[stage][name] += 1
    return {"totals": totals, "stages": stages}


def regress_categories() -> int | None:
    out = sh([sys.executable, "regress.py", "--list-categories"])
    lines = [ln for ln in out.splitlines()
             if ln.startswith(("  ", "\t")) and ln.strip()]
    return len(lines) or None


def ctest_count(build: str) -> int | None:
    cmake = os.path.expanduser("~/tools/cmake-4.4.4-linux-x86_64/bin")
    env_path = os.environ.get("PATH", "")
    os.environ["PATH"] = cmake + os.pathsep + env_path
    directory = os.path.join(ROOT, "build", build)
    if not os.path.isdir(directory):
        return None
    out = subprocess.run(["ctest", "--test-dir", directory, "-N"],
                         cwd=ROOT, capture_output=True, text=True).stdout
    m = re.search(r"Total Tests:\s*(\d+)", out)
    return int(m.group(1)) if m else None


def cache_stats() -> dict:
    root = os.path.expanduser(os.environ.get(
        "GM_GHIDRA_CACHE", "~/.cache/gunmetal-re/ghidra"))
    entries = size = 0
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            if not name.endswith(".json"):
                continue
            entries += 1
            try:
                with open(os.path.join(dirpath, name), encoding="utf-8") as fh:
                    size += len(json.load(fh).get("code") or "")
            except (OSError, json.JSONDecodeError):
                pass
    return {"entries": entries, "pseudo_c_kib": size // 1024}


def doc_sizes() -> dict:
    out = {}
    for rel in DOC_FILES:
        path = os.path.join(ROOT, rel)
        if os.path.isfile(path):
            out[rel] = os.path.getsize(path) // 1024
    return out


def ghidra_status() -> dict:
    server = sh(["bash", "-c",
                 "ss -ltnp 2>/dev/null | grep ':8765 ' | "
                 "grep -o 'pid=[0-9]*' | head -1"]).strip()
    pid = server.split("=")[-1] if server else ""
    info = {"pid": pid or None, "project": None, "functions": None}
    if pid and os.path.isdir(f"/proc/{pid}"):
        info["project"] = os.readlink(f"/proc/{pid}/cwd")
    mcp = os.path.join(ROOT, "tools", "ghidra", "mcp.py")
    out = sh([sys.executable, mcp, "metadata"])
    m = re.search(r'"# of Functions":\s*"(\d+)"', out)
    if m:
        info["functions"] = int(m.group(1))
    return info


def snapshot(with_ghidra: bool) -> dict:
    data = {
        "git": git_info(),
        "roadmap": roadmap_counts(),
        "regress_categories": regress_categories(),
        "ctest": {"sandbox": ctest_count("sandbox"),
                  "release": ctest_count("release")},
        "cache": cache_stats(),
        "docs_kib": doc_sizes(),
    }
    if with_ghidra:
        data["ghidra"] = ghidra_status()
    return data


def check(data: dict) -> int:
    """Fail when the audit snapshot records a different commit count.

    Commit count is used instead of the HEAD hash: an amended docs commit
    changes the hash but not the count, so the marker stays verifiable.
    """
    path = os.path.join(ROOT, "docs", "RE_AUDIT.md")
    with open(path, encoding="utf-8") as fh:
        head = fh.read(4000)
    m = re.search(r"(\d+)\s+коммит", head)
    if not m:
        print("check: docs/RE_AUDIT.md has no 'N коммитов' marker")
        return 2
    recorded = int(m.group(1))
    actual = data["git"]["commits"]
    if recorded != actual:
        print(f"check: audit snapshot is behind HEAD: {recorded} != {actual} commits")
        return 1
    print(f"check: audit snapshot matches HEAD ({actual} commits)")
    return 0


def print_table(data: dict) -> None:
    g = data["git"]
    r = data["roadmap"]["totals"]
    print(f"HEAD {g['head']} ({g['head_date']}), commits {g['commits']}")
    print(f"roadmap: {r['done']} done / {r['partial']} partial / "
          f"{r['todo']} todo / {r['blocked']} blocked")
    for stage, counts in data["roadmap"]["stages"].items():
        total = sum(counts.values())
        print(f"  {stage:4s} {counts['done']:2d}✅ {counts['partial']:2d}🟡 "
              f"{counts['todo']:2d}⬜ {counts['blocked']:2d}⛔  ({total})")
    print(f"regress categories: {data['regress_categories']}")
    print(f"ctest: sandbox={data['ctest']['sandbox']} "
          f"release={data['ctest']['release']}")
    print(f"decompile cache: {data['cache']['entries']} entries, "
          f"{data['cache']['pseudo_c_kib']} KiB")
    print("docs KiB: " + ", ".join(f"{k}={v}"
                                   for k, v in data["docs_kib"].items()))
    if "ghidra" in data:
        h = data["ghidra"]
        print(f"ghidra: pid={h['pid']} project={h['project']} "
              f"functions={h['functions']}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Measured project snapshot.")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--ghidra", action="store_true",
                    help="query the running MCP server (slower)")
    ap.add_argument("--check", action="store_true",
                    help="fail if CURRENT_STATE.md names another HEAD")
    opts = ap.parse_args(argv)
    data = snapshot(opts.ghidra)
    if opts.check:
        return check(data)
    if opts.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print_table(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
