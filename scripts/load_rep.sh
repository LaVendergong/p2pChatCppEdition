#!/usr/bin/env bash
# 重复压测，观察方差。用法同 load.sh
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
ROUNDS="${ROUNDS:-3}"
for i in $(seq 1 "$ROUNDS"); do
  echo "===== round $i/$ROUNDS ====="
  bash scripts/load.sh "$@"
done
