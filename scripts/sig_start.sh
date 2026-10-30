#!/usr/bin/env bash
# 启动 chatsig（后台），日志 /tmp/chat_sig.log
# 用法: sig_start.sh [port] [data_dir] [keep]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT="${1:-7900}"
DATA="${2:-/tmp/chat_sig_data}"
KEEP="${3:-}"

pkill -f "chatsig --port $PORT" 2>/dev/null
sleep 0.3
if [ "$KEEP" != "keep" ]; then
  rm -rf "$DATA"
fi
mkdir -p "$DATA"

setsid nohup ./build/bin/chatsig --port "$PORT" --data "$DATA" --log-level debug \
  > /tmp/chat_sig.log 2>&1 < /dev/null &

for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    echo "chatsig up on $PORT, data=$DATA"
    exit 0
  fi
done
echo "chatsig failed to start"
tail -n 20 /tmp/chat_sig.log
exit 1
