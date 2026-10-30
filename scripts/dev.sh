#!/usr/bin/env bash
# 开发辅助：warn / test / e2e
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

case "${1:-all}" in
  warn)
    grep -E "warning:" /tmp/chat_build.log | sed 's|/mnt/d/projects/mall/chatTerminal/||'
    ;;
  test)
    ./build/chat_tests
    ;;
  all)
    echo "=== warnings ==="
    grep -E "warning:" /tmp/chat_build.log | sed 's|/mnt/d/projects/mall/chatTerminal/||' || true
    echo "=== unit tests ==="
    ./build/chat_tests
    ;;
  *)
    echo "usage: dev.sh [warn|test|all]"
    exit 2
    ;;
esac
