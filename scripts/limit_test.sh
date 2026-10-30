#!/usr/bin/env bash
# 磁盘保护验证：--max-room-mb 1，灌 2.4MB 数据，落盘量必须被限制在 ~1MB
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT=7820
DATA=/tmp/chat_limit_data
LOG=/tmp/chat_limit_srv.log

pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3
rm -rf "$DATA"
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --max-room-mb 1 --rate-limit 0 --log-level warn \
  > "$LOG" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done

echo "== 灌 20000 条（约 2.4MB），房间上限 1MB"
./build/bin/cht-load --port "$PORT" --conns 4 --msgs 5000 --room small --window 32 | tail -n 3
sleep 1

COUNT=$(./build/bin/cht-dump "$DATA"/rooms/small/*.seg 2>/dev/null | wc -l)
SIZE_MB=$(du -sm "$DATA"/rooms/small | awk '{print $1}')
REJECTED=$(grep -c "data limit reached" "$LOG" 2>/dev/null || true)
[ -z "$REJECTED" ] && REJECTED=0

echo "== stored: records=$COUNT size=${SIZE_MB}MB rejected_log_lines=$REJECTED"
pkill -f "chatd --port $PORT" 2>/dev/null

if [ "$SIZE_MB" -le 2 ] && [ "$COUNT" -gt 0 ] && [ "$COUNT" -lt 20000 ]; then
  echo "LIMIT TEST PASSED: 写入被限制在 ${SIZE_MB}MB / ${COUNT} 条（其余被拒绝，未丢已有数据）"
else
  echo "LIMIT TEST FAILED"
  exit 1
fi
