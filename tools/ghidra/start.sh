#!/usr/bin/env bash
# Start the project-local Ghidra MCP server (pyghidra-mcp, headless).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="${GM_GHIDRA_PROJECT:-$ROOT/re/ghidra}"
BINARY="${GM_GHIDRA_BINARY:-/mnt/d/games/Gunmetal 3dfx/gundx.exe}"
PORT="${GM_GHIDRA_PORT:-8765}"
LOG="$PROJECT/server.log"
PIDFILE="$PROJECT/server.pid"

export JAVA_HOME="${JAVA_HOME:-$HOME/tools/jdk-21.0.12.1+1}"
export GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$HOME/tools/ghidra_12.1.4_PUBLIC}"
export PATH="$HOME/tools/bin:$HOME/.local/bin:$PATH"
export HF_HUB_DISABLE_PROGRESS_BARS=1
export TQDM_DISABLE=1

mkdir -p "$PROJECT"
if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "ghidra-mcp already running: pid $(cat "$PIDFILE")"
    exit 0
fi

nohup pyghidra-mcp \
    --transport streamable-http \
    --host 127.0.0.1 --port "$PORT" \
    --project-path "$PROJECT" --project-name gunmetal \
    --wait-for-analysis "$BINARY" >>"$LOG" 2>&1 &
echo $! >"$PIDFILE"
echo "ghidra-mcp started: pid $(cat "$PIDFILE"), http://127.0.0.1:$PORT/mcp"
echo "log: $LOG"
