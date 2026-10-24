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
    python3 tools/qa/state.py --ev            # EV = plan midpoint x readiness
    python3 tools/qa/state.py --ghidra        # + project path and function count
    python3 tools/qa/state.py --check         # nonzero if CURRENT_STATE is behind HEAD
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from decimal import ROUND_HALF_UP, Decimal

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATUS = {"✅": "done", "🟡": "partial", "⬜": "todo", "⛔": "blocked"}
ITEM_RE = re.compile(r"^\s*[-*]\s+(✅|🟡|⬜|⛔)\s")
STAGE_RE = re.compile(r"^##+\s+(M\d+)[.\s]")
DOC_FILES = ("docs/FORMATS.md", "ROADMAP.md", "NOTES.md", "NEXT.md",
             "CURRENT_STATE.md", "docs/RE_AUDIT.md")

# Plan midpoints in agent-days from ``ROADMAP.md`` §9.2.  §6 of
# CURRENT_STATE is derived from them (EV = midpoint x readiness), so they
# belong here too: hand-copied EV went stale for four days while the roadmap
# moved on.
PLAN_DAYS = {
    "M0": (0.5, 1.0), "M1": (6.0, 12.0), "M2": (4.0, 10.0), "M3": (3.0, 6.0),
    "M4": (6.0, 14.0), "M5": (3.0, 6.0), "M6": (8.0, 18.0), "M7": (3.0, 6.0),
    "M8": (3.0, 6.0), "M9": (3.0, 7.0), "M11": (6.0, 15.0), "M12": (4.0, 10.0),
    "M10": (5.0, 15.0),
}
CORE = ("M0", "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8", "M9")
V1 = CORE + ("M11", "M12")
SCOPES = (("core M0..M9", CORE), ("v1.0 (no M10)", V1),
          ("full scope", tuple(PLAN_DAYS)))
SPEEDUP = (1.5, 2.3)  # factor still expected on the remainder (§7)


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
            # A level-two heading ends the current milestone.  Without this a
            # suffix section (test strategy, DoD, ...) keeps feeding its items
            # into the last milestone (M10) and inflates that row.
            if line.startswith("## "):
                m = STAGE_RE.match(line)
                stage = m.group(1) if m else None
                if stage:
                    stages.setdefault(stage,
                                      {name: 0 for name in STATUS.values()})
                continue
            m = ITEM_RE.match(line)
            if not m:
                continue
            name = STATUS[m.group(1)]
            totals[name] += 1
            if stage:
                stages[stage][name] += 1
    stage_totals = {name: sum(counts[name] for counts in stages.values())
                    for name in STATUS.values()}
    return {"totals": totals, "stage_totals": stage_totals, "stages": stages}


def r1(value: float) -> float:
    """Round to one decimal, half up, so a table row and its total agree."""
    return float(Decimal(str(value)).quantize(Decimal("0.1"),
                                              rounding=ROUND_HALF_UP))


def earned_value(roadmap: dict) -> dict:
    """EV per stage and per scope: plan midpoint x item readiness.

    Readiness is the same crude measure as CURRENT_STATE §3 (a 🟡 item counts
    as half), so EV overstates progress: "code exists" is not "verified
    against the original".
    """
    stages = {}
    for stage, (lo, hi) in PLAN_DAYS.items():
        counts = roadmap["stages"].get(stage)
        if not counts:
            continue
        mid = (lo + hi) / 2
        ready = (counts["done"] + 0.5 * counts["partial"]) / sum(counts.values())
        stages[stage] = {"lo": lo, "hi": hi, "mid": mid, "ready": ready,
                         "ev": r1(mid * ready)}
    scopes = {}
    for name, keys in SCOPES:
        plan = sum(stages[k]["mid"] for k in keys if k in stages)
        # Sum the rounded per-stage values so the total matches the table.
        ev = r1(sum(stages[k]["ev"] for k in keys if k in stages))
        left = plan - ev
        scopes[name] = {"plan": round(plan, 2), "ev": ev,
                        "share": ev / plan if plan else 0.0,
                        "left": r1(left),
                        "left_fast": r1(left / SPEEDUP[1]),
                        "left_slow": r1(left / SPEEDUP[0])}
    return {"stages": stages, "scopes": scopes}


def doc_number_problems(data: dict) -> list[str]:
    """Compare the §3 and §6 tables of CURRENT_STATE with the roadmap.

    The commit-count marker alone let two failures through unnoticed: §3's
    header block multiplied while its rows aged, and §6 kept readiness from a
    four-day-old snapshot.  Both are mechanical, so the gate checks them.
    """
    path = os.path.join(ROOT, "CURRENT_STATE.md")
    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    def between(start: str, end: str) -> str:
        i = text.find(start)
        j = text.find(end, i + len(start)) if i >= 0 else -1
        return text[i:j] if i >= 0 and j > i else ""

    def cells(line: str) -> list[str]:
        return [c.strip() for c in line.strip().strip("|").split("|")]

    stages = data["roadmap"]["stages"]
    ev = data["earned"]["stages"]
    problems: list[str] = []
    seen = set()
    for line in between("## 3.", "## 4.").splitlines():
        m = re.match(r"^\|\s*(M\d+)\.", line)
        if not m or len(cells(line)) < 7:
            continue
        stage = m.group(1)
        counts = stages.get(stage)
        if not counts:
            problems.append(f"§3 {stage}: stage missing from ROADMAP")
            continue
        seen.add(stage)
        items = sum(counts.values())
        want = [counts["done"], counts["partial"], counts["todo"],
                counts["blocked"], items,
                round(100 * (counts["done"] + 0.5 * counts["partial"]) / items)]
        got = cells(line)[1:7]
        for name, value, cell in zip(("✅", "🟡", "⬜", "⛔", "пунктов",
                                      "готовность"), want, got):
            expect = f"{value}%" if name == "готовность" else str(value)
            if cell != expect:
                problems.append(f"§3 {stage}: {name} {cell} != {expect}")
    for stage in ev:
        if stage not in seen:
            problems.append(f"§3: no row for {stage}")
    seen_ev = set()
    for line in between("## 6.", "## 7.").splitlines():
        parts = cells(line)
        if len(parts) < 4:
            continue
        m = re.match(r"^`?(M\d+)`?(?:\s+\([^)]*\))?$", parts[0])
        if not m or m.group(1) not in ev:
            continue
        stage = m.group(1)
        seen_ev.add(stage)
        want_ev, want_pct = ev[stage]["ev"], round(100 * ev[stage]["ready"])
        try:
            got_ev = float(parts[2].replace(",", "."))
        except ValueError:
            problems.append(f"§6 {stage}: EV '{parts[2]}' is not a number")
        else:
            if abs(got_ev - want_ev) > 0.05:
                problems.append(f"§6 {stage}: EV {got_ev} != {want_ev}")
        if parts[3] != f"{want_pct}%":
            problems.append(f"§6 {stage}: readiness {parts[3]} != {want_pct}%")
    for stage in ev:
        if stage not in seen_ev:
            problems.append(f"§6: no row for {stage}")
    return problems


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
    """Cache size plus how much of it predates the current project names."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "ghidra"))
    import fncache
    root = os.path.expanduser(os.environ.get(
        "GM_GHIDRA_CACHE", "~/.cache/gunmetal-re/ghidra"))
    dirs = [os.path.join(root, name)
            for name in (os.listdir(root) if os.path.isdir(root) else [])
            if os.path.isdir(os.path.join(root, name))]
    latest = max(dirs, key=lambda path: len(
        [n for n in os.listdir(path) if n.endswith(".json")]), default=None)
    if latest is None:
        return {"entries": 0, "pseudo_c_kib": 0, "stale": 0}
    report = fncache.stale_report(
        latest, os.path.join(ROOT, "tools", "re", "names.csv"))
    return {
        "entries": report["entries"],
        "pseudo_c_kib": report["pseudo_c_kib"],
        "stale": report["stale"],
        "stale_self": report["stale_self"],
        "stale_callee": report["stale_callee"],
        "stale_comment": report["stale_comment"],
    }


def names_drift() -> dict:
    """Compare the live Ghidra project with names.csv (server must be up)."""
    server = sh(["bash", "-c",
                 "ss -ltn 2>/dev/null | grep ':8765 ' | head -1"]).strip()
    if not server:
        return {"skipped": "ghidra server down"}
    try:
        run = subprocess.run(
            [sys.executable, "tools/ghidra/apply_names.py", "--check",
             "--json"], cwd=ROOT, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"skipped": "apply_names --check timed out"}
    payload = None
    for line in run.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                payload = None
    if payload is None:
        return {"skipped": run.stderr.strip()[:200] or "no JSON from apply_names"}
    payload["exit"] = run.returncode
    return payload


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
    roadmap = roadmap_counts()
    data = {
        "git": git_info(),
        "roadmap": roadmap,
        "earned": earned_value(roadmap),
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
    """Fail when CURRENT_STATE.md is behind HEAD or its tables are stale.

    Commit count is used instead of the HEAD hash: an amended docs commit
    changes the hash but not the count, so the marker stays verifiable.
    The §3/§6 numbers are re-derived from ROADMAP.md, because both rotted
    while this gate only watched the commit count.
    """
    path = os.path.join(ROOT, "CURRENT_STATE.md")
    with open(path, encoding="utf-8") as fh:
        head = fh.read(4000)
    problems: list[str] = []
    m = re.search(r"(\d+)\s+коммит", head)
    actual = data["git"]["commits"]
    if not m:
        problems.append("no 'N коммитов' marker in CURRENT_STATE.md")
    elif int(m.group(1)) != actual:
        problems.append(f"behind HEAD: {int(m.group(1))} != {actual} commits")
    problems += doc_number_problems(data)

    drift = names_drift()
    if "missing" in drift and drift["missing"]:
        problems.append(
            f"Ghidra project has {len(drift['missing'])} names.csv names not "
            f"applied (run tools/ghidra/apply_names.py)")
    stale = data["cache"].get("stale", 0)
    if stale:
        problems.append(
            f"fncache has {stale} stale entries (run "
            f"tools/ghidra/fncache.py warm --cached --refresh)")

    if problems:
        print("check: repository state disagrees:")
        for problem in problems[:12]:
            print(f"  {problem}")
        if len(problems) > 12:
            print(f"  ... {len(problems) - 12} more")
        return 1
    detail = (f", names drift 0 ({drift['planned']} planned)"
              if "planned" in drift else f", names check {drift.get('skipped')}")
    print(f"check: CURRENT_STATE.md matches HEAD ({actual} commits), "
          f"§3/§6 match ROADMAP, cache stale 0{detail}")
    return 0


def print_ev(data: dict) -> None:
    ev = data["earned"]
    print("EV = plan midpoint (ROADMAP §9.2) x readiness (done + 0.5*partial):")
    for stage, s in ev["stages"].items():
        print(f"  {stage:4s} plan {s['lo']:g}-{s['hi']:g} mid {s['mid']:5.2f} "
              f"{100 * s['ready']:3.0f}%  EV {s['ev']:4.1f}")
    for name, sc in ev["scopes"].items():
        print(f"  {name:14s} EV {sc['ev']:5.1f} of {sc['plan']:6.2f} = "
              f"{100 * sc['share']:3.0f}%  left {sc['left']:5.1f} "
              f"({sc['left_fast']:4.1f}-{sc['left_slow']:4.1f} a.d. at "
              f"{SPEEDUP[0]}-{SPEEDUP[1]}x)")


def print_table(data: dict) -> None:
    g = data["git"]
    r = data["roadmap"]["totals"]
    s = data["roadmap"]["stage_totals"]
    print(f"HEAD {g['head']} ({g['head_date']}), commits {g['commits']}")
    print(f"roadmap: {r['done']} done / {r['partial']} partial / "
          f"{r['todo']} todo / {r['blocked']} blocked (all item lines)")
    print(f"stages:  {s['done']} done / {s['partial']} partial / "
          f"{s['todo']} todo / {s['blocked']} blocked "
          f"({sum(s.values())} items)")
    for stage, counts in data["roadmap"]["stages"].items():
        total = sum(counts.values())
        print(f"  {stage:4s} {counts['done']:2d}✅ {counts['partial']:2d}🟡 "
              f"{counts['todo']:2d}⬜ {counts['blocked']:2d}⛔  ({total})")
    print(f"regress categories: {data['regress_categories']}")
    print(f"ctest: sandbox={data['ctest']['sandbox']} "
          f"release={data['ctest']['release']}")
    print(f"decompile cache: {data['cache']['entries']} entries, "
          f"{data['cache']['pseudo_c_kib']} KiB, "
          f"{data['cache'].get('stale', 0)} stale")
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
    ap.add_argument("--ev", action="store_true",
                    help="print earned value (plan midpoint x readiness)")
    opts = ap.parse_args(argv)
    data = snapshot(opts.ghidra)
    if opts.check:
        return check(data)
    if opts.ev:
        print_ev(data)
    elif opts.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print_table(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
