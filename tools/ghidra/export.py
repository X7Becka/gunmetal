#!/usr/bin/env python3
"""Export a few annotated functions from the running Ghidra MCP project.

Run with the pyghidra-mcp virtualenv (it has the ``mcp`` client)::

    ~/.local/share/uv/tools/pyghidra-mcp/bin/python tools/ghidra/export.py

The Ghidra project itself stays in ``re/ghidra/`` (ignored); only the text
artifacts under ``docs/re/ghidra/`` are committed.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import pathlib

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

TARGETS = [
    ("weapon_target_scan", "0x477f90", "weapon target/aim scan (hypothesis)"),
    ("thing_target_scan", "0x473a00", "thing/AI target scan (hypothesis)"),
    ("camera_basis", "0x41e025", "camera basis Rz(-yaw)*Rx(-pitch)*Ry(roll) (proven)"),
    ("soo_player", "0x433e00", "SOO/thing animation player (proven)"),
]


async def export(url: str, binary: str, out: pathlib.Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    index = []
    async with streamablehttp_client(url) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()
            meta = await session.call_tool(
                "list_project_binary_metadata", {"binary_name": binary})
            (out / "metadata.json").write_text(
                meta.content[0].text + "\n", encoding="utf-8")
            for name, address, note in TARGETS:
                result = await session.call_tool(
                    "decompile_function",
                    {"binary_name": binary, "name_or_address": address,
                     "include_xrefs": True, "include_strings": True},
                )
                payload = json.loads(result.content[0].text)
                code = payload.get("code") or ""
                (out / f"{name}.c").write_text(
                    f"// {address} - {note}\n"
                    f"// binary: {binary}\n"
                    f"// ghidra: {url}\n\n{code}\n", encoding="utf-8")
                index.append({
                    "name": name,
                    "address": address,
                    "note": note,
                    "xrefs": payload.get("xrefs") or [],
                    "error": payload.get("error"),
                })
    (out / "index.json").write_text(
        json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8765/mcp")
    parser.add_argument("--binary", default="/gundx.exe-902b77")
    parser.add_argument("--out", default="docs/re/ghidra")
    args = parser.parse_args()
    asyncio.run(export(args.url, args.binary, pathlib.Path(args.out)))
    print(f"exported {len(TARGETS)} functions to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
