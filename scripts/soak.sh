#!/usr/bin/env bash
# 浸泡测试：持续发消息，周期性采样 RSS / 连接数 / 消息数，输出 CSV。
# 用法: soak.sh [总时长秒=259200(72h)] [采样间隔秒=3600] [端口=7810]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

DURATION="${1:-259200}"
INTERVAL="${2:-3600}"
PORT="${3:-7810}"
DATA=/tmp/chat_soak_data
CSV="build/soak.csv"

mkdir -p build
pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3
rm -rf "$DATA"
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --log-level info --rate-limit 0 --sync batch \
  > build/soak_server.log 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done
PID=$(pgrep -f "chatd --port $PORT" | head -1)
if [ -z "$PID" ]; then
  echo "server failed to start"
  exit 1
fi

echo "elapsed_s,rss_kb,msgs,sent_total,load_runs,errors" > "$CSV"
echo "soak started: pid=$PID duration=${DURATION}s interval=${INTERVAL}s csv=$CSV"

START=$(date +%s)
SENT=0
RUNS=0
ERRORS=0
NEXT_SAMPLE=$START

while :; do
  NOW=$(date +%s)
  ELAPSED=$((NOW - START))
  if [ "$ELAPSED" -ge "$DURATION" ]; then break; fi

  OUT=$(./build/bin/cht-load --port "$PORT" --conns 8 --msgs 500 --room soak --window 64 2>&1)
  RUNS=$((RUNS + 1))
  N=$(echo "$OUT" | sed -n 's/.*msgs_acked=\([0-9]*\).*/\1/p')
  if [ -n "$N" ]; then SENT=$((SENT + N)); else ERRORS=$((ERRORS + 1)); fi

  NOW=$(date +%s)
  if [ "$NOW" -ge "$NEXT_SAMPLE" ]; then
    NEXT_SAMPLE=$((NOW + INTERVAL))
    RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
    MSGS=$(grep -m1 '^last ' "$DATA"/rooms/soak/room.meta 2>/dev/null | awk '{print $2}')
    [ -z "$RSS" ] && RSS=0
    [ -z "$MSGS" ] && MSGS=0
    echo "$((NOW - START)),$RSS,$MSGS,$SENT,$RUNS,$ERRORS" >> "$CSV"
    echo "[$((NOW - START))s] rss=${RSS}KB msgs=$MSGS sent=$SENT runs=$RUNS errors=$ERRORS"
  fi
done

RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
MSGS=$(grep -m1 '^last ' "$DATA"/rooms/soak/room.meta 2>/dev/null | awk '{print $2}')
echo "$(( $(date +%s) - START )),${RSS:-0},${MSGS:-0},$SENT,$RUNS,$ERRORS" >> "$CSV"

pkill -TERM -f "chatd --port $PORT" 2>/dev/null
sleep 1
echo "soak finished: sent=$SENT runs=$RUNS errors=$ERRORS csv=$CSV"
