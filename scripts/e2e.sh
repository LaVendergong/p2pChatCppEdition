#!/usr/bin/env bash
# 端到端测试：起服务端，两个 plain 客户端互发消息，重启后校验历史仍在。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

BIN=./build/bin
PORT=7799
DATA=/tmp/chat_e2e_data
LOG=/tmp/chat_e2e.log
RECV1=/tmp/chat_e2e_recv1.txt
RECV2=/tmp/chat_e2e_recv2.txt
RECV3=/tmp/chat_e2e_recv3.txt

SRV=""

rm -rf "$DATA" "$LOG" "$RECV1" "$RECV2" "$RECV3"
mkdir -p "$DATA"

# 清理上次残留（否则端口占用会让新实例启动失败，测试跑在旧实例上）
pkill -f "chatd --port $PORT" 2>/dev/null
sleep 0.3

fail() {
  echo "E2E FAILED: $1"
  [ -f "$LOG" ] && tail -n 30 "$LOG"
  [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
  exit 1
}

start_server() {
  "$BIN/chatd" --port "$PORT" --data "$DATA" --log-level debug --sync always >> "$LOG" 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do
    sleep 0.1
    kill -0 "$SRV" 2>/dev/null || fail "server exited during startup"
    (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
  done
  fail "server did not start on port $PORT"
}

echo ">>> start server"
start_server

echo ">>> alice sends a message, holds connection 2s"
{ echo "hello-e2e-from-alice"; sleep 2; } | "$BIN/chatt" --plain --no-color --user alice --password pw --port "$PORT" --room testroom > "$RECV1" 2>&1 &
CLIENT1=$!
sleep 1

echo ">>> bob joins and should receive alice's message"
{ sleep 2; } | "$BIN/chatt" --plain --no-color --user bob --password pw --port "$PORT" --room testroom > "$RECV2" 2>&1 &
CLIENT2=$!
wait $CLIENT1 2>/dev/null
wait $CLIENT2 2>/dev/null

grep -q "hello-e2e-from-alice" "$RECV2" || fail "bob did not receive alice's message"
echo "    ok: bob received alice's message"

echo ">>> bob replies, alice's second session should see it plus history"
{ echo "reply-from-bob"; sleep 2; } | "$BIN/chatt" --plain --no-color --user bob --password pw --port "$PORT" --room testroom > "$RECV3" 2>&1 &
CLIENT3=$!
wait $CLIENT3 2>/dev/null

grep -q "hello-e2e-from-alice" "$RECV3" || fail "history missing in bob's second session"
grep -q "reply-from-bob" "$RECV3" || fail "bob did not see his own message"

echo ">>> restart server, history must survive"
kill -TERM "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
start_server

{ sleep 2; } | "$BIN/chatt" --plain --no-color --user carol --password pw --port "$PORT" --room testroom > "$RECV1" 2>&1 &
CLIENT4=$!
wait $CLIENT4 2>/dev/null
grep -q "hello-e2e-from-alice" "$RECV1" || fail "history lost after restart"
grep -q "reply-from-bob" "$RECV1" || fail "reply lost after restart"
echo "    ok: history survived restart"

echo ">>> verify data files with cht-dump"
SEG=$(ls "$DATA"/rooms/testroom/*.seg | head -1)
"$BIN/cht-dump" "$SEG" > /tmp/chat_e2e_dump.txt 2>/tmp/chat_e2e_dump.err || fail "cht-dump failed"
grep -q "hello-e2e-from-alice" /tmp/chat_e2e_dump.txt || fail "dump missing message"
echo "    ok: $(wc -l < /tmp/chat_e2e_dump.txt) records dumped"

kill -TERM "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null

echo "E2E PASSED"
