#!/usr/bin/env bash
# 压测：起一个独立服务端（/tmp，避开 /mnt 的 9p 慢盘），跑 cht-load 并输出 QPS/延迟。
# 用法: load.sh [conns] [msgs] [sync_mode]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

CONNS="${1:-8}"
MSGS="${2:-2000}"
SYNC="${3:-batch}"
SEG_MB="${4:-64}"
WINDOW="${5:-64}"
PORT=7801
DATA=/tmp/chat_load_data

pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3
rm -rf "$DATA"
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --sync "$SYNC" --segment-mb "$SEG_MB" \
  --log-level warn --rate-limit 0 > /tmp/chat_load_srv.log 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done

echo "== cht-load: conns=$CONNS msgs/conn=$MSGS sync=$SYNC window=$WINDOW"
./build/bin/cht-load --port "$PORT" --conns "$CONNS" --msgs "$MSGS" --room loadtest --window "$WINDOW"
RC=$?

# 优雅关闭（会 flush 并写 room.meta），再离线核对落盘记录数
pkill -TERM -f "chatd --port $PORT" 2>/dev/null
for _ in $(seq 1 50); do
  pgrep -f "chatd --port $PORT" > /dev/null || break
  sleep 0.1
done

LAST=$(grep -m1 '^last ' "$DATA"/rooms/loadtest/room.meta | awk '{print $2}')
DUMPED=$(./build/bin/cht-dump "$DATA"/rooms/loadtest/*.seg 2>/dev/null | wc -l)
SEGS=$(ls "$DATA"/rooms/loadtest/*.seg 2>/dev/null | wc -l)
SIZE=$(du -sh "$DATA"/rooms/loadtest 2>/dev/null | awk '{print $1}')
echo "== stored: meta_last=$LAST dumped_records=$DUMPED segments=$SEGS size=$SIZE"
if [ "$DUMPED" -ne "$LAST" ]; then
  echo "WARN: dumped records ($DUMPED) != meta last ($LAST)"
  RC=1
fi

exit $RC
