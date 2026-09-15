#!/usr/bin/env bash
# 一键校验：清理构建 -> 检查告警数 -> 冒烟测试 -> 一致性测试
set -u
cd "$(dirname "$0")/.."

echo "================ 1. 清理构建 ================"
make clean >/dev/null 2>&1
BUILD_LOG=$(mktemp)
make >"$BUILD_LOG" 2>&1
WARN=$(grep -cE 'warning:|error:' "$BUILD_LOG")
echo "编译器告警/错误数: $WARN"
grep -E 'warning:|error:' "$BUILD_LOG" | head -20
[ "$WARN" -eq 0 ] && echo "  [PASS] -Wall -Wextra -Wpedantic 零告警" || echo "  [FAIL] 存在告警"
rm -f "$BUILD_LOG"

echo
echo "================ 2. 冒烟测试 ================"
bash tests/smoke_test.sh
RC1=$?

echo
echo "================ 3. 一致性测试 =============="
bash tests/consistency_test.sh
RC2=$?

echo
echo "================ 汇总 ================"
if [ "$WARN" -eq 0 ] && [ "$RC1" -eq 0 ] && [ "$RC2" -eq 0 ]; then
  echo "全部通过 ✅"
  exit 0
fi
echo "存在失败项 ❌"
exit 1
