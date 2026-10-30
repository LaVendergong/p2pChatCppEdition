#!/usr/bin/env bash
# 语法检查 / 构建 / ASan 测试。用法：bash scripts/check.sh [syntax|build|asan]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

MODE="${1:-build}"
# 日志放在项目内：/tmp 在 WSL 重启后会被清空，排查问题时容易丢现场
mkdir -p build
LOG="$(pwd)/build/build.log"

if [ "$MODE" = "syntax" ]; then
  rc=0
  for f in src/common/*.cpp; do
    g++ -std=c++20 -Iinclude -Wall -Wextra -Wshadow -fsyntax-only "$f" || { echo "FAILED: $f"; rc=1; }
  done
  exit $rc
fi

if [ "$MODE" = "asan" ]; then
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHAT_ENABLE_ASAN=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null || exit 1
  # ASan 编译单元内存占用大，7 GB 的 WSL 上 -j8 会 OOM，默认降到 -j3
  JOBS="${CHAT_ASAN_JOBS:-3}"
  if ! cmake --build build-asan -j"$JOBS" > "$LOG" 2>&1; then
    grep -E "error:|Error " "$LOG" | head -60
    echo "--- build failed, full log: $LOG"
    exit 1
  fi
  grep -E "warning:" "$LOG" | head -20
  ./build-asan/chat_tests
  exit $?
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null || exit 1
if ! cmake --build build -j"$(nproc)" > "$LOG" 2>&1; then
  grep -E "error:|Error " "$LOG" | head -80
  echo "--- build failed, full log: $LOG"
  exit 1
fi
grep -cE "warning:" "$LOG" | xargs -I{} echo "warnings: {}"
echo "build ok: build/bin/chatd build/bin/chatt"
