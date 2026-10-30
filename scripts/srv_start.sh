#!/usr/bin/env bash
# 启动测试用服务端（后台常驻，日志写 /tmp/chat_srv.log）
# 用法: srv_start.sh [port] [data_dir]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT="${1:-7799}"
DATA="${2:-/tmp/chat_e2e_data}"
KEEP="${3:-}"

pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3
if [ "$KEEP" != "keep" ]; then
  rm -rf "$DATA"
fi
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --log-level debug --sync always --rate-limit 0 \
  > /tmp/chat_srv.log 2>&1 < /dev/null &

for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    echo "server up on $PORT, data=$DATA"
    pgrep -a chatd
    exit 0
  fi
done

echo "server failed to start"
tail -n 20 /tmp/chat_srv.log
exit 1
