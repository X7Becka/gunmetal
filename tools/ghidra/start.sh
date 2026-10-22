#!/usr/bin/env bash
# Start the project-local Ghidra MCP server (pyghidra-mcp, headless).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="${GM_GHIDRA_PROJECT:-$ROOT/re/ghidra}"
BINARY="${GM_GHIDRA_BINARY:-/mnt/d/games/Gunmetal 3dfx/gundx.exe}"
PORT="${GM_GHIDRA_PORT:-8765}"
LOG="$PROJECT/server.log"
PIDFILE="$PROJECT/server.pid"

# The project is state (names, comments, types), not a scratch file: a project
# inside /tmp or /var/tmp dies with its worktree, renames become unsavable
# ("File error during save") and the next session starts from FUN_ names.
case "$PROJECT" in
    /tmp/*|/var/tmp/*)
        if [[ "${GM_GHIDRA_ALLOW_EPHEMERAL:-0}" != "1" ]]; then
            echo "refusing ephemeral project path: $PROJECT" >&2
            echo "use $ROOT/re/ghidra (override: GM_GHIDRA_ALLOW_EPHEMERAL=1)" >&2
            exit 2
        fi
        ;;
esac

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

# A stale pidfile must not start a second server on the same port: it would
# import a fresh copy, fail to bind, and confuse the next query.
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    OWNER="$(ss -ltnp 2>/dev/null | grep ":$PORT " |
             grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)"
    echo "port $PORT busy: pid ${OWNER:-?}" >&2
    if [[ -n "${OWNER:-}" && -d "/proc/$OWNER" ]]; then
        PROJECT_IN_USE="$(readlink /proc/"$OWNER"/cwd 2>/dev/null || true)"
        echo "  project in use: $PROJECT_IN_USE" >&2
        case "$PROJECT_IN_USE" in
            *"(deleted)"|/tmp/*|/var/tmp/*)
                echo "  warning: that project is ephemeral; renames cannot be saved" >&2
                ;;
        esac
    fi
    echo "stop it with tools/ghidra/stop.sh (or GM_GHIDRA_PORT for a second server)" >&2
    exit 0
fi

# Keep the previous session's annotations: one snapshot per day, last 3 kept.
if [[ -d "$PROJECT/gunmetal.rep" ]]; then
    BACKUPS="$ROOT/re/ghidra-backups"
    STAMP="$(date +%F)"
    if [[ ! -d "$BACKUPS/$STAMP" ]]; then
        mkdir -p "$BACKUPS"
        cp -a "$PROJECT" "$BACKUPS/$STAMP" 2>/dev/null &&
            echo "project backup: $BACKUPS/$STAMP"
        ls -1dt "$BACKUPS"/*/ 2>/dev/null | tail -n +4 | xargs -r rm -rf
    fi
fi


# setsid is essential: an agent/tool shell reaps its process group on exit,
# which otherwise kills the long-lived MCP server.
setsid nohup pyghidra-mcp \
    --transport streamable-http \
    --host 127.0.0.1 --port "$PORT" \
    --project-path "$PROJECT" --project-name gunmetal \
    --wait-for-analysis "$BINARY" >>"$LOG" 2>&1 </dev/null &
echo $! >"$PIDFILE"
echo "ghidra-mcp started: pid $(cat "$PIDFILE"), http://127.0.0.1:$PORT/mcp"
echo "log: $LOG"
