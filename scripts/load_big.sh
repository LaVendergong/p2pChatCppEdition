#!/usr/bin/env bash
# 后台跑大规模压测（结果写 /tmp/load_big.log），用于避开交互超时。
# 用法: load_big.sh [conns] [msgs_per_conn] [segment_mb]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

CONNS="${1:-8}"
MSGS="${2:-25000}"
SEG="${3:-8}"

rm -f /tmp/load_big.log
setsid nohup bash scripts/load.sh "$CONNS" "$MSGS" batch "$SEG" > /tmp/load_big.log 2>&1 < /dev/null &
echo "started: conns=$CONNS msgs/conn=$MSGS total=$((CONNS * MSGS)) log=/tmp/load_big.log"
