#!/bin/bash
# usage: swap_product.sh <snapshot-dir> before|after
# 只替换本包拥有的产品源（清单 = before/ 下的产品文件）；测试文件与新增头文件保持当前版本。
set -euo pipefail
SNAP=$1; MODE=$2
REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
EV="$REPO/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-exec-units"
cd "$EV/before"
find src -type f ! -path "src/TestUnits/*" | sort | while read -r f; do
  if [ "$MODE" = before ]; then cp "$EV/before/$f" "$SNAP/$f"; else cp "$REPO/$f" "$SNAP/$f"; fi  # 不保留 mtime，确保 ninja 重编
done
echo "swapped product sources to $MODE"
