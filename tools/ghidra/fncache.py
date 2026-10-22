#!/usr/bin/env python3
"""Persistent cache of Ghidra decompiler output, keyed by exe hash + address.

Why: the decompiler is queried hundreds of times per session (745 requests in
the current project history, only 343 distinct addresses -- 53% repeats), and
the answers were never stored.  Re-decompiling the same function costs a
Ghidra round trip and a fresh read of the same pseudo-C, and the result dies
with the chat.  This cache keeps the *raw* output on disk, outside the Ghidra
project, so it survives a project restart or a lost worktree.

The cache is evidence-neutral: pseudo-C is a hypothesis source, not proof.
Store it, quote it, but verify against bytes/data/tests before documenting.

Examples::

    python3 tools/ghidra/fncache.py get 0x4752d0
    python3 tools/ghidra/fncache.py get 0x4752d0 0x470c70 --refresh
    python3 tools/ghidra/fncache.py warm --names tools/re/names.csv
    python3 tools/ghidra/fncache.py warm --names tools/re/names.csv --limit 20
    python3 tools/ghidra/fncache.py stats
    python3 tools/ghidra/fncache.py export docs/re/ghidra/fn

Cache location: ``$GM_GHIDRA_CACHE`` (default ``~/.cache/gunmetal-re/ghidra``),
one directory per exe SHA-256 prefix.  Network access happens only on a miss;
a hit works with the MCP server stopped.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import BINARY, URL, McpError, Session  # noqa: E402

CACHE_ROOT = os.environ.get(
    "GM_GHIDRA_CACHE", os.path.expanduser("~/.cache/gunmetal-re/ghidra"))
EXE_CANDIDATES = (
    os.environ.get("GUNMETAL_EXE", ""),
    os.path.expanduser("~/.cache/gunmetal-re/gundx.exe"),
    "/mnt/d/games/Gunmetal 3dfx/gundx.exe",
)


def find_exe() -> str:
    for path in EXE_CANDIDATES:
        if path and os.path.isfile(path):
            return path
    raise SystemExit("fncache: gundx.exe not found; set GUNMETAL_EXE")


def exe_sha(path: str) -> str:
    sha = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            sha.update(chunk)
    return sha.hexdigest()


def cache_dir(sha: str) -> str:
    path = os.path.join(CACHE_ROOT, sha[:12])
    os.makedirs(path, exist_ok=True)
    return path


def entry_path(directory: str, address: str) -> str:
    return os.path.join(directory, address.lower() + ".json")


def read_entry(directory: str, address: str) -> dict | None:
    path = entry_path(directory, address)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return None


def write_entry(directory: str, address: str, payload: dict) -> None:
    name = payload.get("name") or ""
    if name.startswith("FUN_"):
        name = ""
    entry = {
        "address": payload.get("address") or address,
        "name": name,
        "code": payload.get("code") or "",
        "error": payload.get("error") or "",
        "fetched_at": int(time.time()),
    }
    path = entry_path(directory, address)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(entry, fh, ensure_ascii=False)
    os.replace(tmp, path)


def load_names(path: str) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """Return (functions, non-functions) from names.csv.

    Data addresses (``kind`` table/data) are not decompilable and would only
    produce "Function or symbol not found" noise, so they are kept apart.
    """
    with open(path, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    functions: list[tuple[str, str]] = []
    others: list[tuple[str, str]] = []
    for row in rows:
        va = (row.get("va") or "").strip().lower()
        if not re.fullmatch(r"0x[0-9a-f]{6,8}", va):
            continue
        name = (row.get("name") or "").strip()
        kind = (row.get("kind") or "code").strip().lower()
        (functions if kind in ("code", "func", "") else others).append((va, name))
    return functions, others


def fetch(session: Session, addresses: list[str], binary: str) -> dict[str, dict]:
    """Decompile a batch, keeping one MCP session for all of them."""
    result: dict[str, dict] = {}
    for address in addresses:
        try:
            docs = session.call_docs("decompile_function", {
                "binary_name": binary,
                "name_or_address": address,
            })
        except McpError as exc:
            result[address] = {"address": address, "error": str(exc), "code": ""}
            continue
        if not docs:
            result[address] = {"address": address, "error": "empty reply",
                               "code": ""}
            continue
        payload = docs[0]
        if "text" in payload and "code" not in payload:
            payload = {"address": address, "code": payload["text"]}
        result[address] = payload
    return result


def require_session(args) -> Session:
    try:
        return Session(args.url, args.timeout)
    except McpError as exc:
        print(f"fncache: {exc}", file=sys.stderr)
        raise SystemExit(2)


def names_index(path: str = "tools/re/names.csv") -> dict[str, str]:
    """address -> name from names.csv (best effort, for readable headers)."""
    if not os.path.isfile(path):
        return {}
    functions, others = load_names(path)
    return {va: name for va, name in functions + others if name}


def cmd_get(args, directory: str) -> int:
    addresses = [a.lower() for a in args.address]
    missing = [a for a in addresses if args.refresh or
               read_entry(directory, a) is None]
    if missing:
        session = require_session(args)
        fetched = fetch(session, missing, args.binary)
        for address in missing:
            payload = fetched[address]
            write_entry(directory, address, payload)
            state = "error" if payload.get("error") else "fetched"
            print(f"# {address} {state}")
    known = names_index()
    for address in addresses:
        entry = read_entry(directory, address) or {}
        if not entry.get("name") or entry["name"].startswith("FUN_"):
            entry["name"] = known.get(address, entry.get("name") or "")
        name = entry.get("name") or ""
        if not args.no_headers:
            print(f"// {entry.get('address', address)} {name}".rstrip())
        if entry.get("error"):
            print(f"// error: {entry['error']}")
            continue
        code = entry.get("code") or ""
        if len(code) > 32768 and args.head is None:
            print(f"// large: {len(code)} chars, {code.count(chr(10)) + 1} lines; "
                  f"use --head N (or disrange.py for one block)", file=sys.stderr)
        if args.head is not None:
            lines = code.splitlines()
            print("\n".join(lines[:args.head]))
            if len(lines) > args.head:
                print(f"// ... {len(lines) - args.head} of {len(lines)} "
                      f"lines not shown")
        else:
            print(code)
    return 0


def cmd_warm(args, directory: str) -> int:
    pairs, others = load_names(args.names)
    if args.limit:
        pairs = pairs[:args.limit]
    if others:
        print(f"fncache: skipping {len(others)} data/table entries "
              f"(not decompilable): "
              + ", ".join(f"{va} {name}" for va, name in others[:4])
              + (" ..." if len(others) > 4 else ""))
    todo = [(va, name) for va, name in pairs
            if args.refresh or read_entry(directory, va) is None]
    print(f"fncache: {len(pairs)} names, {len(todo)} to fetch, "
          f"{len(pairs) - len(todo)} cached")
    if not todo:
        return 0
    session = require_session(args)
    done = errors = 0
    for start in range(0, len(todo), args.chunk):
        chunk = todo[start:start + args.chunk]
        fetched = fetch(session, [va for va, _ in chunk], args.binary)
        for address, name in chunk:
            payload = fetched[address]
            if not payload.get("name") or payload["name"].startswith("FUN_"):
                payload["name"] = name
            write_entry(directory, address, payload)
            if payload.get("error"):
                errors += 1
                print(f"  {address} {name}: {payload['error']}")
            else:
                done += 1
                print(f"  {address} {name}: {len(payload.get('code') or '')} B")
    print(f"fncache: {done} cached, {errors} errors")
    return 0 if not errors else 1


def cmd_stats(args, directory: str) -> int:
    entries = []
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(directory, name)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                entries.append(json.load(fh))
        except (OSError, json.JSONDecodeError):
            continue
    size = sum(len(e.get("code") or "") for e in entries)
    named = sum(1 for e in entries if e.get("name") and
                not (e.get("name") or "").startswith("FUN_"))
    errors = sum(1 for e in entries if e.get("error"))
    print(f"fncache: {len(entries)} entries, {size / 1024:.0f} KiB of pseudo-C, "
          f"{named} named, {errors} errors")
    print(f"cache: {directory}")
    if entries:
        newest = max(entries, key=lambda e: e.get("fetched_at") or 0)
        print(f"newest: {newest.get('address')} "
              f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(newest.get('fetched_at') or 0))}")
    return 0


def cmd_export(args, directory: str) -> int:
    out = args.directory
    os.makedirs(out, exist_ok=True)
    known = names_index()
    written = 0
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(directory, name), "r", encoding="utf-8") as fh:
            entry = json.load(fh)
        if entry.get("error") or not entry.get("code"):
            continue
        address = (entry.get("address") or name[:-5]).lower()
        label = entry.get("name") or known.get(address, "")
        slug = re.sub(r"[^a-z0-9_]+", "_", label.lower()).strip("_") or "func"
        path = os.path.join(out, f"{address}_{slug}.c")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"// {address} {label}\n")
            fh.write("// gh: pyghidra-mcp decompile_function (hypothesis, verify)\n")
            fh.write(entry["code"])
        written += 1
    print(f"fncache: exported {written} files to {out}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Persistent decompiler-output cache (exe hash + address).")
    parser.add_argument("--url", default=URL)
    parser.add_argument("--binary", default=BINARY)
    parser.add_argument("--timeout", type=float, default=600.0)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("get", help="print cached pseudo-C (fetch on miss)")
    p.add_argument("address", nargs="+")
    p.add_argument("--refresh", action="store_true")
    p.add_argument("--head", type=int, default=None,
                   help="print only the first N lines")
    p.add_argument("--no-headers", dest="no_headers", action="store_true")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("warm", help="fetch every address from names.csv")
    p.add_argument("--names", default="tools/re/names.csv")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--chunk", type=int, default=20)
    p.add_argument("--refresh", action="store_true")
    p.set_defaults(func=cmd_warm)

    sub.add_parser("stats", help="cache size and contents").set_defaults(
        func=cmd_stats)

    p = sub.add_parser("export", help="write .c files for git")
    p.add_argument("directory")
    p.set_defaults(func=cmd_export)

    args = parser.parse_args(argv)
    sha = exe_sha(find_exe())
    directory = cache_dir(sha)
    try:
        return args.func(args, directory)
    except McpError as exc:
        print(f"fncache: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
