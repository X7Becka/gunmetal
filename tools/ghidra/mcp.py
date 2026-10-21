#!/usr/bin/env python3
"""Dependency-free CLI for the running pyghidra MCP server.

The OpenCode/Hermes agents do not always have the ``mcp`` Python package, so
this script speaks the MCP streamable-HTTP JSON-RPC transport directly with
``urllib``.  It is the supported way for subagents to query the shared Ghidra
project on ``127.0.0.1:8765``.

Examples::

    python3 tools/ghidra/mcp.py tools
    python3 tools/ghidra/mcp.py binaries
    python3 tools/ghidra/mcp.py decompile 0x444476 --xrefs
    python3 tools/ghidra/mcp.py decompile 0x451c20 0x452020
    python3 tools/ghidra/mcp.py disasm 0x4444c0 --count 40 --bytes
    python3 tools/ghidra/mcp.py xrefs 0x81bd00
    python3 tools/ghidra/mcp.py strings PLAYER_ --limit 50
    python3 tools/ghidra/mcp.py search "player armor" --limit 3
    python3 tools/ghidra/mcp.py bytes 0x4b53e8 --size 64
    python3 tools/ghidra/mcp.py callgraph 0x451c20 --direction called
    python3 tools/ghidra/mcp.py raw decompile_function '{"name_or_address":"0x401000"}'

Batch mode (one JSON request per line, one MCP session for the whole file)::

    {"tool": "decompile_function", "args": {"name_or_address": "0x451c20"}}
    {"tool": "list_xrefs", "args": {"name_or_address": "0x81c290"}}

    python3 tools/ghidra/mcp.py batch reqs.jsonl > out.txt

Environment: ``GM_GHIDRA_URL`` (default ``http://127.0.0.1:8765/mcp``),
``GM_GHIDRA_BINARY`` (default ``/gundx.exe-902b77``).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

URL = os.environ.get("GM_GHIDRA_URL", "http://127.0.0.1:8765/mcp")
BINARY = os.environ.get("GM_GHIDRA_BINARY", "/gundx.exe-902b77")


class McpError(RuntimeError):
    pass


class Session:
    """One MCP streamable-HTTP session (initialize once, then call tools)."""

    def __init__(self, url: str = URL, timeout: float = 600.0) -> None:
        self.url = url
        self.timeout = timeout
        self.sid: str | None = None
        self._id = 0
        self._initialize()

    def _post(self, payload: dict) -> dict | None:
        data = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if self.sid:
            headers["mcp-session-id"] = self.sid
        req = urllib.request.Request(self.url, data=data, headers=headers)
        try:
            resp = urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.URLError as exc:  # pragma: no cover - env dependent
            raise McpError(
                f"cannot reach {self.url}: {exc}. Start it with "
                f"tools/ghidra/start.sh") from exc
        sid = resp.headers.get("mcp-session-id")
        if sid:
            self.sid = sid
        body = resp.read().decode("utf-8", "replace")
        events = [line[6:] for line in body.splitlines()
                  if line.startswith("data: ")]
        if not events:
            return None
        return json.loads(events[-1])

    def _initialize(self) -> None:
        self._post({
            "jsonrpc": "2.0",
            "id": 0,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "gunmetal-mcp-cli", "version": "1.0"},
            },
        })
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, tool: str, args: dict | None = None) -> dict:
        self._id += 1
        reply = self._post({
            "jsonrpc": "2.0",
            "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}},
        })
        if reply is None:
            raise McpError(f"{tool}: empty reply")
        if "error" in reply:
            raise McpError(f"{tool}: {reply['error']}")
        result = reply.get("result") or {}
        content = result.get("content") or []
        text = "\n".join(part.get("text", "") for part in content
                         if part.get("type") == "text")
        if result.get("isError"):
            raise McpError(f"{tool}: {text or 'tool error'}")
        return {"text": text, "result": result}

    def call_json(self, tool: str, args: dict | None = None) -> dict:
        """Parse the reply; list-valued tools reply with several documents."""
        docs = json_docs(self.call(tool, args)["text"])
        if not docs:
            return {}
        return docs[0]

    def call_docs(self, tool: str, args: dict | None = None) -> list[dict]:
        return json_docs(self.call(tool, args)["text"])

    def tools(self) -> list[dict]:
        self._id += 1
        reply = self._post({"jsonrpc": "2.0", "id": self._id,
                            "method": "tools/list"})
        return (reply or {}).get("result", {}).get("tools", [])


def json_docs(text: str) -> list[dict]:
    """Decode one or more concatenated JSON documents from a tool reply."""
    text = text.strip()
    if not text:
        return []
    decoder = json.JSONDecoder()
    docs: list[dict] = []
    pos = 0
    while pos < len(text):
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text):
            break
        try:
            doc, pos = decoder.raw_decode(text, pos)
        except json.JSONDecodeError:
            return [{"text": text}]
        docs.append(doc)
    return docs


def _addrs(values: list[str]) -> str | list[str]:
    return values[0] if len(values) == 1 else list(values)


def cmd_tools(s: Session, args) -> None:
    for tool in s.tools():
        desc = (tool.get("description") or "").strip().splitlines()
        print(f"{tool['name']}: {desc[0] if desc else ''}")


def cmd_binaries(s: Session, args) -> None:
    print(s.call_json("list_project_binaries"))


def cmd_metadata(s: Session, args) -> None:
    payload = s.call_json("list_project_binary_metadata",
                          {"binary_name": args.binary or BINARY})
    print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_decompile(s: Session, args) -> None:
    payloads = s.call_docs("decompile_function", {
        "binary_name": args.binary or BINARY,
        "name_or_address": _addrs(args.address),
        "include_callees": args.callees,
        "include_strings": args.strings,
        "include_xrefs": args.xrefs,
    })
    for payload in payloads:
        if "text" in payload and len(payloads) == 1:
            print(payload["text"])
            continue
        _print_decompile(payload, args)


def _print_decompile(payload: dict, args) -> None:
    head = payload.get("name") or payload.get("address") or "?"
    if args.headers:
        print(f"// {head} {payload.get('address', '')}".rstrip())
    error = payload.get("error")
    if error:
        print(f"// error: {error}")
        return
    print(payload.get("code") or "")
    if args.xrefs and payload.get("xrefs"):
        print("// xrefs:")
        for x in payload["xrefs"]:
            print(f"//   {json.dumps(x, ensure_ascii=False)}")
    if args.strings and payload.get("strings"):
        print("// strings:")
        for x in payload["strings"]:
            print(f"//   {json.dumps(x, ensure_ascii=False)}")


def cmd_disasm(s: Session, args) -> None:
    payload = s.call_json("disassemble", {
        "binary_name": args.binary or BINARY,
        "address": args.address,
        "count": args.count,
        "include_bytes": args.bytes,
    })
    print(payload.get("instructions") or payload.get("text") or
          json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_xrefs(s: Session, args) -> None:
    payload = s.call_json("list_xrefs", {
        "binary_name": args.binary or BINARY,
        "name_or_address": _addrs(args.target),
    })
    print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_strings(s: Session, args) -> None:
    payload = s.call_json("search_strings", {
        "binary_name": args.binary or BINARY,
        "query": args.pattern,
        "limit": args.limit,
    })
    for item in payload.get("strings", []):
        if isinstance(item, dict):
            print(f"{item.get('address', '?'):>10}  {item.get('value', '')}")
        else:
            print(item)
    if not payload.get("strings"):
        print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_symbols(s: Session, args) -> None:
    payload = s.call_json("search_symbols_by_name", {
        "binary_name": args.binary or BINARY,
        "query": args.pattern,
        "functions_only": args.functions,
        "limit": args.limit,
    })
    print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_search(s: Session, args) -> None:
    payload = s.call_json("search_code", {
        "binary_name": args.binary or BINARY,
        "query": args.text,
        "limit": args.limit,
        "search_mode": "literal" if args.literal else "semantic",
        "include_full_code": args.full,
    })
    print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_bytes(s: Session, args) -> None:
    payload = s.call_json("read_bytes", {
        "binary_name": args.binary or BINARY,
        "address": args.address,
        "size": args.size,
    })
    print(json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_callgraph(s: Session, args) -> None:
    payload = s.call_json("gen_callgraph", {
        "binary_name": args.binary or BINARY,
        "function_name": args.function,
        "direction": args.direction,
    })
    print(payload.get("graph") or payload.get("mermaid") or
          json.dumps(payload, indent=2, ensure_ascii=False))


def cmd_raw(s: Session, args) -> None:
    request = json.loads(args.json)
    tool = args.tool
    tool_args = request
    if isinstance(request, dict) and "tool" in request and "args" in request:
        tool, tool_args = request["tool"], request["args"]
    tool_args.setdefault("binary_name", args.binary or BINARY)
    text = s.call(tool, tool_args)["text"]
    docs = json_docs(text)
    if len(docs) == 1:
        print(json.dumps(docs[0], indent=2, ensure_ascii=False))
    else:
        for doc in docs:
            print(json.dumps(doc, indent=2, ensure_ascii=False))


def cmd_batch(s: Session, args) -> None:
    """Run one request per line in a single MCP session."""
    with open(args.path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                request = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"### line {lineno}: bad JSON: {exc}")
                continue
            tool = request.get("tool")
            tool_args = dict(request.get("args") or {})
            if not tool:
                print(f"### line {lineno}: missing 'tool'")
                continue
            tool_args.setdefault("binary_name", args.binary or BINARY)
            label = request.get("label") or json.dumps(tool_args,
                                                       ensure_ascii=False)
            print(f"### {lineno} {label}")
            try:
                text = s.call(tool, tool_args)["text"]
            except McpError as exc:
                print(f"// error: {exc}")
                continue
            for payload in json_docs(text) or [{"text": text}]:
                if "text" in payload and "code" not in payload:
                    print(payload["text"])
                elif payload.get("error"):
                    print(f"// {payload.get('name', '?')}: {payload['error']}")
                elif "code" in payload:
                    print(payload.get("code") or "")
                else:
                    print(json.dumps(payload, indent=2, ensure_ascii=False))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="CLI for the running pyghidra MCP server.")
    parser.add_argument("--url", default=URL, help="MCP endpoint")
    parser.add_argument("--binary", default=BINARY, help="binary_name in project")
    parser.add_argument("--timeout", type=float, default=600.0)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("tools", help="list MCP tools").set_defaults(func=cmd_tools)

    sub.add_parser("binaries", help="list project binaries").set_defaults(
        func=cmd_binaries)
    sub.add_parser("metadata", help="binary metadata").set_defaults(
        func=cmd_metadata)

    p = sub.add_parser("decompile", help="pseudo-C for addresses/names")
    p.add_argument("address", nargs="+")
    p.add_argument("--xrefs", action="store_true")
    p.add_argument("--strings", action="store_true")
    p.add_argument("--callees", action="store_true")
    p.add_argument("--no-headers", dest="headers", action="store_false")
    p.set_defaults(func=cmd_decompile, headers=True)

    p = sub.add_parser("disasm", help="disassemble at an address")
    p.add_argument("address")
    p.add_argument("--count", type=int, default=20)
    p.add_argument("--bytes", action="store_true")
    p.set_defaults(func=cmd_disasm)

    p = sub.add_parser("xrefs", help="cross references to addresses/symbols")
    p.add_argument("target", nargs="+")
    p.set_defaults(func=cmd_xrefs)

    p = sub.add_parser("strings", help="search strings")
    p.add_argument("pattern")
    p.add_argument("--limit", type=int, default=100)
    p.set_defaults(func=cmd_strings)

    p = sub.add_parser("symbols", help="search symbols by regex")
    p.add_argument("pattern")
    p.add_argument("--limit", type=int, default=25)
    p.add_argument("--functions", action="store_true")
    p.set_defaults(func=cmd_symbols)

    p = sub.add_parser("search", help="search decompiled code")
    p.add_argument("text")
    p.add_argument("--limit", type=int, default=5)
    p.add_argument("--literal", action="store_true")
    p.add_argument("--full", action="store_true")
    p.set_defaults(func=cmd_search)

    p = sub.add_parser("bytes", help="read raw bytes")
    p.add_argument("address")
    p.add_argument("--size", type=int, default=32)
    p.set_defaults(func=cmd_bytes)

    p = sub.add_parser("callgraph", help="Mermaid call graph")
    p.add_argument("function")
    p.add_argument("--direction", choices=["calling", "called"],
                   default="calling")
    p.set_defaults(func=cmd_callgraph)

    p = sub.add_parser("raw", help="call any tool with a JSON argument object")
    p.add_argument("tool")
    p.add_argument("json")
    p.set_defaults(func=cmd_raw)

    p = sub.add_parser("batch", help="JSONL batch in one session")
    p.add_argument("path")
    p.set_defaults(func=cmd_batch)

    args = parser.parse_args(argv)
    try:
        session = Session(args.url, args.timeout)
        args.func(session, args)
    except McpError as exc:
        print(f"ghidra mcp: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
