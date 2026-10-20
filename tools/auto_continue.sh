#!/usr/bin/env bash
# Unattended continuation loop for OpenCode.
#
# Usage:
#   cd ~/gunmetal-re
#   tmux new -s gm './tools/auto_continue.sh'
#   # or: nohup ./tools/auto_continue.sh > /tmp/opencode/gm-auto.log 2>&1 &
#
# The loop stops when the agent creates .opencode/STOP, when all work is
# blocked/finished, or after MAX_CYCLES (default 100).
set -u

cd "$(dirname "$0")/.." || exit 1
mkdir -p .opencode
rm -f .opencode/STOP

MAX_CYCLES="${MAX_CYCLES:-100}"
PROMPT='Продолжай автономно. Прочитай NEXT.md и ROADMAP.md, возьми верхний незаблокированный пункт и выполни его до проверяемого результата. Не спрашивай разрешения, не жди фоновых сабагентов, делай независимую задачу. В конце хода обнови NEXT.md. Если все пункты закрыты или заблокированы — создай .opencode/STOP и заверши работу. Явный STOP от пользователя также означает остановку.'

first=1
consecutive_failures=0
for ((i = 1; i <= MAX_CYCLES; i++)); do
    if [ -f .opencode/STOP ]; then
        echo "[auto_continue] STOP marker found, exiting after $((i - 1)) cycles"
        exit 0
    fi

    echo "[auto_continue] cycle $i/$MAX_CYCLES $(date -Is)"
    if [ "$first" -eq 1 ]; then
        opencode run --auto --agent build "$PROMPT"
    else
        opencode run --continue --auto --agent build "$PROMPT"
    fi
    status=$?
    first=0
    if [ "$status" -ne 0 ]; then
        consecutive_failures=$((consecutive_failures + 1))
        echo "[auto_continue] opencode exited with $status (failure $consecutive_failures/3)"
        if [ "$consecutive_failures" -ge 3 ]; then
            echo "[auto_continue] too many failures, stopping"
            exit 1
        fi
    else
        consecutive_failures=0
    fi
    sleep 2
done

echo "[auto_continue] MAX_CYCLES reached"
