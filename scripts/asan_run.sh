#!/usr/bin/env bash
# 后台跑 ASan/UBSan 构建 + 单测（结果写 /tmp/asan.log），避开交互超时。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
mkdir -p build-asan
rm -f build-asan/asan.log
setsid nohup bash scripts/check.sh asan > build-asan/asan.log 2>&1 < /dev/null &
echo "started: log=$(pwd)/build-asan/asan.log"
