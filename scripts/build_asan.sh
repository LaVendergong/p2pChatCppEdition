#!/usr/bin/env bash
# 前台执行（由调用方决定是否放后台）：构建 ASan 版并跑单测，结果写 build/asan_build.log
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG="build/asan_build.log"
: > "$LOG"

echo "[1/3] configure" >> "$LOG"
if ! cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHAT_ENABLE_ASAN=ON >> "$LOG" 2>&1; then
  echo "configure FAILED" >> "$LOG"
  exit 1
fi

echo "[2/3] build -j3" >> "$LOG"
if ! cmake --build build-asan -j3 >> "$LOG" 2>&1; then
  echo "build FAILED" >> "$LOG"
  exit 1
fi

echo "[3/3] run unit tests" >> "$LOG"
./build-asan/chat_tests >> "$LOG" 2>&1
echo "tests exit=$?" >> "$LOG"
