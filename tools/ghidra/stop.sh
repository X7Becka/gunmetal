#!/usr/bin/env bash
# Stop the project-local Ghidra MCP server.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="${GM_GHIDRA_PROJECT:-$ROOT/re/ghidra}"
PIDFILE="$PROJECT/server.pid"

if [[ ! -f "$PIDFILE" ]]; then
    echo "ghidra-mcp: no pid file"
    exit 0
fi
pid="$(cat "$PIDFILE")"
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
fi
rm -f "$PIDFILE"
echo "ghidra-mcp stopped: pid $pid"
