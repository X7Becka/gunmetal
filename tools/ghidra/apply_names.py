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
  * renames are idempotent: a symbol already carrying the name is skipped.

Examples::

    python3 tools/ghidra/apply_names.py --dry-run
    python3 tools/ghidra/apply_names.py
    python3 tools/ghidra/apply_names.py --names tools/re/names.csv --limit 10
"""
from __future__ import annotations

import argparse
import csv
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


def already_named(session: Session, binary: str, address: str,
                  name: str) -> bool:
    try:
        docs = session.call_docs("search_symbols_by_name", {
            "binary_name": binary, "query": f"^{name}$", "functions_only": True})
    except McpError:
        return False
    for doc in docs:
        for symbol in doc.get("symbols") or []:
            if (symbol.get("address") or "").lower() == address:
                return True
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Rename Ghidra functions from tools/re/names.csv.")
    parser.add_argument("--url", default=URL)
    parser.add_argument("--binary", default=BINARY)
    parser.add_argument("--names", default="tools/re/names.csv")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")
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

    renamed = skipped = failed = 0
    for item in todo:
        if already_named(session, args.binary, item["address"], item["name"]):
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
    session.call("save", {})
    print(f"apply_names: {renamed} renamed, {skipped} already named, "
          f"{failed} failed; project saved")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
