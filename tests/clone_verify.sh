#!/usr/bin/env bash
# 全新克隆验证：证明仓库自包含、clone 下来就能构建并通过测试
set -u
WORK=$(mktemp -d)
echo "临时目录: $WORK"
cd "$WORK" || exit 1

echo "================ 1. 从 GitHub 克隆 ================"
git clone -q https://github.com/meminehobe24435-cmyk/serial-data-gateway.git 2>&1 | tail -3
cd serial-data-gateway || exit 1

echo "================ 2. 检查行尾（应为 LF）================"
for f in tests/smoke_test.sh Makefile src/main.cpp; do
  if file "$f" | grep -q CRLF; then
    echo "  [FAIL] $f 是 CRLF —— 脚本在 Linux 上会报 \$'\\r'"
  else
    echo "  [PASS] $f 为 LF"
  fi
done

echo "================ 3. 干净构建 ================"
make clean >/dev/null 2>&1
LOG=$(mktemp)
make >"$LOG" 2>&1
W=$(grep -cE 'warning:|error:' "$LOG")
echo "  告警/错误数: $W"
grep -E 'warning:|error:' "$LOG" | head -10
[ "$W" -eq 0 ] && echo "  [PASS] 零告警构建" || echo "  [FAIL] 存在告警"
rm -f "$LOG"

echo "================ 4. 文件清单 ================"
git ls-files | sed 's/^/  /'

echo "================ 5. 冒烟测试 ================"
bash tests/smoke_test.sh > /tmp/clone_smoke.log 2>&1
RC=$?
tail -12 /tmp/clone_smoke.log
[ "$RC" -eq 0 ] && echo "  [PASS] 冒烟测试通过" || echo "  [FAIL] 冒烟测试失败"

echo "================ 6. 一致性测试 ================"
bash tests/consistency_test.sh > /tmp/clone_cons.log 2>&1
RC2=$?
tail -12 /tmp/clone_cons.log
[ "$RC2" -eq 0 ] && echo "  [PASS] 一致性测试通过" || echo "  [FAIL] 一致性测试失败"

echo
echo "================ 汇总 ================"
if [ "$W" -eq 0 ] && [ "$RC" -eq 0 ] && [ "$RC2" -eq 0 ]; then
  echo "全新克隆可构建、可测试 ✅"
else
  echo "存在问题 ❌"
fi
cd / && rm -rf "$WORK"
