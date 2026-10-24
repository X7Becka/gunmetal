#!/usr/bin/env python3
"""Apply names.csv and its evidence notes to the Ghidra project.

Why: the project is re-analysed from scratch whenever its path is lost (it was
living in a deleted worktree), and every fresh project shows only ``FUN_``
names.  ``tools/re/names.csv`` is the validated, versioned source of names
(``regress.py --category names``), so the project can be re-hydrated in one
batch instead of re-deriving names by hand in every session.

Rules:
  * only ``kind=code`` rows are renamed; tables/data are not functions;
  * the evidence column becomes a plate comment tagged with the status, so the
    project itself says whether a name is proven or a hypothesis;
  * ``--dry-run`` prints the plan and changes nothing (use it first);
  * ``--check`` compares the project with names.csv and exits 1 on drift;
  * renames are idempotent: a symbol already carrying the name is skipped;
  * after a successful ``save`` the decompiler cache entries for the renamed
    addresses are dropped: their old pseudo-C still says ``FUN_…`` and has no
    names.csv plate comment, so the next ``fncache get`` must refetch.

Examples::

    python3 tools/ghidra/apply_names.py --dry-run
    python3 tools/ghidra/apply_names.py --check --json
    python3 tools/ghidra/apply_names.py
    python3 tools/ghidra/apply_names.py --names tools/re/names.csv --limit 10
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import BINARY, URL, McpError, Session  # noqa: E402


def load_rows(path: str) -> list[dict]:
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def plan(rows: list[dict], limit: int) -> list[dict]:
    todo = []
    for row in rows:
        va = (row.get("va") or "").strip().lower()
        name = (row.get("name") or "").strip()
        if not va.startswith("0x") or not name:
            continue
        if (row.get("kind") or "").strip().lower() != "code":
            continue
        todo.append({
            "address": va,
            "name": name,
            "status": (row.get("status") or "").strip(),
            "evidence": (row.get("evidence") or "").strip(),
            "subsystem": (row.get("subsystem") or "").strip(),
        })
        if limit and len(todo) >= limit:
            break
    return todo


def comment_for(item: dict) -> str:
    parts = [f"names.csv [{item['status'] or '?'}] {item['name']}"
             f" ({item['subsystem']})"]
    if item["evidence"]:
        parts.append(item["evidence"])
    parts.append("pseudo-C is a hypothesis; proof lives in docs/tests")
    return "\n".join(parts)


def symbol_map(session: Session, binary: str) -> dict[int, set[str]]:
    """address -> names, in one MCP call instead of one call per name.

    The old per-name ``search_symbols_by_name`` loop cost one round trip per
    CSV row, so a 100-row check took tens of seconds and was skipped in
    practice.  Ghidra accepts the ``.*`` regex and returns all symbols at once.
    """
    try:
        docs = session.call_docs("search_symbols_by_name", {
            "binary_name": binary, "query": ".*", "functions_only": True,
            "limit": 10000})
    except McpError:
        return {}
    result: dict[int, set[str]] = {}
    for doc in docs:
        for symbol in doc.get("symbols") or []:
            raw = (symbol.get("address") or "").lower().removeprefix("0x")
            if not raw:
                continue
            try:
                va = int(raw, 16)
            except ValueError:
                continue
            result.setdefault(va, set()).add(symbol.get("name") or "")
    return result


def va_of(address: str) -> int:
    return int(address.lower().removeprefix("0x"), 16)


def missing_names(todo: list[dict], symbols: dict[int, set[str]]) -> list[dict]:
    return [item for item in todo
            if item["name"] not in symbols.get(va_of(item["address"]), set())]


def containing_function(session: Session, binary: str,
                        address: str) -> str | None:
    """Name of the function that contains *address*, if Ghidra can find one.

    Some names.csv rows point at an instruction inside a function (the map is
    an address map).  ``rename_function`` renames the containing function, so
    the name is present but not at the exact address — a check that only looks
    at entry points reports false drift.
    """
    try:
        docs = session.call_docs("decompile_function", {
            "binary_name": binary, "name_or_address": address})
    except McpError:
        return None
    for doc in docs:
        name = doc.get("name") or ""
        if "-" in name and not doc.get("error"):
            return name.rsplit("-", 1)[0]
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Rename Ghidra functions from tools/re/names.csv.")
    parser.add_argument("--url", default=URL)
    parser.add_argument("--binary", default=BINARY)
    parser.add_argument("--names", default="tools/re/names.csv")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--check", action="store_true",
                        help="compare the project with names.csv (exit 1 on drift)")
    parser.add_argument("--json", action="store_true",
                        help="machine output for --check")
    parser.add_argument("--no-comments", dest="comments", action="store_false")
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args(argv)

    todo = plan(load_rows(args.names), args.limit)
    print(f"names.csv: {len(todo)} code entries to apply")
    if args.dry_run:
        for item in todo:
            print(f"  {item['address']} -> {item['name']} "
                  f"[{item['status']}]")
        print("dry run: nothing changed")
        return 0

    try:
        session = Session(args.url, args.timeout)
    except McpError as exc:
        print(f"apply_names: {exc}", file=sys.stderr)
        return 2

    symbols = symbol_map(session, args.binary)
    if args.check:
        missing = missing_names(todo, symbols)
        contained = []
        really_missing = []
        for item in missing:
            if containing_function(session, args.binary,
                                   item["address"]) == item["name"]:
                contained.append(item)
            else:
                really_missing.append(item)
        if args.json:
            print(json.dumps({
                "planned": len(todo),
                "contained": len(contained),
                "missing": [{"address": item["address"], "name": item["name"]}
                            for item in really_missing]}, ensure_ascii=False))
        else:
            print(f"apply_names: {len(todo)} planned, {len(contained)} applied "
                  f"to containing functions, {len(really_missing)} missing")
            for item in really_missing[:20]:
                print(f"  missing {item['address']} {item['name']}")
            if len(really_missing) > 20:
                print(f"  ... {len(really_missing) - 20} more")
        return 0 if not really_missing else 1

    renamed = skipped = failed = 0
    renamed_addresses: list[str] = []
    for item in todo:
        va = va_of(item["address"])
        if item["name"] in symbols.get(va, set()):
            skipped += 1
            continue
        try:
            session.call("rename_function", {
                "binary_name": args.binary,
                "name_or_address": item["address"],
                "new_name": item["name"],
            })
        except McpError as exc:
            failed += 1
            print(f"  {item['address']} {item['name']}: {exc}")
            continue
        symbols.setdefault(va, set()).add(item["name"])
        if args.comments:
            try:
                session.call("set_comment", {
                    "binary_name": args.binary,
                    "target": item["name"],
                    "comment": comment_for(item),
                    "comment_type": "plate",
                })
            except McpError as exc:
                print(f"  {item['address']} comment: {exc}")
        renamed += 1
        renamed_addresses.append(item["address"])
    try:
        session.call("save", {})
    except McpError as exc:
        print(f"apply_names: save failed, cache kept: {exc}", file=sys.stderr)
        return 2
    invalidated = 0
    if renamed_addresses:
        import fncache
        sha = fncache.exe_sha(fncache.find_exe())
        directory = fncache.cache_dir(sha)
        invalidated = fncache.invalidate(directory, renamed_addresses)
        invalidated += fncache.invalidate_dependents(directory,
                                                     renamed_addresses)
    print(f"apply_names: {renamed} renamed, {skipped} already named, "
          f"{failed} failed; project saved; {invalidated} cache entries "
          f"invalidated")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
