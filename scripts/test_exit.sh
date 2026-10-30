#!/usr/bin/env bash
# 验证 SIGTERM/SIGINT 的退出码与优雅关闭路径
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT=7804
DATA=/tmp/chat_exit_test
rm -rf "$DATA"; mkdir -p "$DATA"

./build/bin/chatd --port "$PORT" --data "$DATA" --log-level debug > /tmp/chat_exit.log 2>&1 &
PID=$!
sleep 1

echo "== sending SIGTERM to $PID"
kill -TERM "$PID"
wait "$PID"
echo "exit_code=$?  (0/1 = graceful, 143 = killed by SIGTERM)"

echo "== log:"
cat /tmp/chat_exit.log
