#!/usr/bin/env bash
# 验证优雅退出是否把 room.meta 刷到最新
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT=7803
DATA=/tmp/chat_meta_test
pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.2
rm -rf "$DATA"
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --sync always --log-level info --rate-limit 0 \
  > /tmp/chat_meta_srv.log 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done

./build/bin/cht-load --port "$PORT" --conns 2 --msgs 50 --room metatest --window 8
echo "== meta while running:"
cat "$DATA"/rooms/metatest/room.meta

echo "== sending SIGTERM"
pkill -TERM -f "chatd --port $PORT"
for _ in $(seq 1 50); do
  pgrep -f "chatd --port $PORT" > /dev/null || break
  sleep 0.1
done
echo "== still running? $(pgrep -f "chatd --port $PORT" | wc -l) process(es)"

echo "== meta after shutdown:"
cat "$DATA"/rooms/metatest/room.meta
echo "== dumped records: $(./build/bin/cht-dump "$DATA"/rooms/metatest/*.seg 2>/dev/null | wc -l)"
echo "== server log tail:"
tail -n 15 /tmp/chat_meta_srv.log
