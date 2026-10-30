#!/usr/bin/env bash
# 统计代码与文档规模
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

echo "== C++ 代码"
find include src tests tools -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 wc -l | tail -1
echo "  文件数: $(find include src tests tools -type f \( -name '*.cpp' -o -name '*.h' \) | wc -l)"

echo "== 文档"
wc -l docs/*.md README.md | tail -1
echo "  篇数: $(ls docs/*.md | wc -l) + README"
