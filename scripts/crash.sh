#!/usr/bin/env bash
# 崩溃一致性：写入 N 条 → kill -9 → 重启 → 校验已 ACK 的消息一条不少、CRC 全通过。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PORT=7802
DATA=/tmp/chat_crash_data
SEG_GLOB="$DATA/rooms/crashtest"

pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3
rm -rf "$DATA"
mkdir -p "$DATA"

setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --sync always --log-level warn --rate-limit 0 \
  > /tmp/chat_crash_srv.log 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done

echo "== writing 1000 messages"
LOAD_OUT=$(./build/bin/cht-load --port "$PORT" --conns 4 --msgs 250 --room crashtest --window 32) || true
echo "$LOAD_OUT"
# 基准 = 客户端收到 ACK 的条数（ACK 语义是"已 fdatasync"，所以崩溃后一条都不能少）
ACKED=$(echo "$LOAD_OUT" | sed -n 's/.*msgs_acked=\([0-9]*\).*/\1/p')
echo "== acked before crash: $ACKED"

echo "== kill -9"
pkill -9 -f "chatd --port $PORT"
sleep 0.5

echo "== restart and verify"
setsid nohup ./build/bin/chatd --port "$PORT" --data "$DATA" --sync always --log-level warn \
  > /tmp/chat_crash_srv2.log 2>&1 < /dev/null &
for _ in $(seq 1 40); do
  sleep 0.1
  if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
done
sleep 0.3

# 房间是懒加载的：必须有人 join 才会触发 open() → 扫描尾部 → 修复 meta
echo "== join the room to trigger recovery/replay"
{ sleep 1; } | timeout 6 ./build/bin/chatt --plain --no-color --user verify --password pw --port "$PORT" \
  --room crashtest > /tmp/chat_crash_verify.txt 2>&1 || true
grep -E "joined #crashtest" /tmp/chat_crash_verify.txt || true

AFTER=$(grep -m1 '^last ' "$SEG_GLOB/room.meta" | awk '{print $2}')
COUNT=$("$PWD/build/bin/cht-dump" "$SEG_GLOB"/*.seg 2>/dev/null | wc -l)
echo "== meta last after restart: $AFTER, dumped records: $COUNT"

pkill -f "chatd --port $PORT" 2>/dev/null

if [ -z "$ACKED" ] || [ -z "$AFTER" ]; then
  echo "CRASH TEST FAILED: cannot read msg ids"
  exit 1
fi
if [ "$COUNT" -lt "$ACKED" ]; then
  echo "CRASH TEST FAILED: lost ACKed messages ($COUNT < $ACKED)"
  exit 1
fi
if [ "$COUNT" -ne "$AFTER" ]; then
  echo "CRASH TEST FAILED: record count $COUNT != meta last $AFTER (meta not repaired)"
  exit 1
fi
echo "CRASH TEST PASSED: acked=$ACKED stored=$COUNT, no loss, meta repaired, crc ok"
