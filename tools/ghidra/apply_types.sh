#!/usr/bin/env bash
# Apply tools/ghidra/gm_types.h to the Ghidra project (project must be closed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${PYGHIDRA_PYTHON:-$HOME/.local/share/uv/tools/pyghidra-mcp/bin/python}"

if [[ ! -x "$PY" ]]; then
    echo "apply_types: python with pyghidra not found at $PY" >&2
    echo "set PYGHIDRA_PYTHON to the pyghidra-mcp venv python" >&2
    exit 2
fi

export JAVA_HOME="${JAVA_HOME:-$HOME/tools/jdk-21.0.12.1+1}"
export GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$HOME/tools/ghidra_12.1.4_PUBLIC}"

if ss -ltn 2>/dev/null | grep -q ':8765 '; then
    if [[ "${GM_GHIDRA_ALLOW_RUNNING:-0}" != "1" ]]; then
        echo "apply_types: MCP server is running (port 8765); stop it first:" >&2
        echo "  tools/ghidra/stop.sh" >&2
        echo "  tools/ghidra/apply_types.sh $*" >&2
        echo "  tools/ghidra/start.sh" >&2
        exit 2
    fi
fi

# The header must match the field maps before it reaches the project.
python3 "$ROOT/tools/ghidra/typegen.py" --check >/dev/null

exec "$PY" "$ROOT/tools/ghidra/apply_types.py" "$@"
