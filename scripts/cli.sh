#!/usr/bin/env bash
# 以 plain 模式跑一个客户端：发送若干条消息，停留一会儿收消息，然后退出。
# 用法: cli.sh <port> <user> <room> <outfile> [msg ...]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT="${1:?port}"
USER="${2:?user}"
ROOM="${3:?room}"
OUT="${4:?outfile}"
shift 4

{
  for m in "$@"; do
    echo "$m"
    sleep 0.2
  done
  sleep 2
} | timeout 12 ./build/bin/chatt --plain --no-color --user "$USER" --password pw --port "$PORT" --room "$ROOM" > "$OUT" 2>&1
RC=$?

echo "--- client $USER exit=$RC ---"
cat "$OUT"
