#!/usr/bin/env bash
# 千万级存储验证（绕过网络直接打存储引擎），结果写 build/bench_big.log
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG="build/bench_big.log"
DIR="${1:-/home/baka/chat_bench}"
COUNT="${2:-10000000}"

rm -rf "$DIR"
setsid nohup ./build/bin/cht-bench --dir "$DIR" --count "$COUNT" --reads 100000 --segment-mb 64 --sync-every 5000 \
  > "$LOG" 2>&1 < /dev/null &
echo "started: count=$COUNT dir=$DIR log=$(pwd)/$LOG"
