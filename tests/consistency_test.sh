#!/usr/bin/env bash
# 一致性校验：进程报告的落盘条数 是否等于 文件字节数 / sizeof(Record)
set -u
cd "$(dirname "$0")/.."

cat > /tmp/sz.cpp <<'EOF'
#include <cstdio>
#include "gateway.h"
int main() { std::printf("%zu", sizeof(Record)); return 0; }
EOF
g++ -std=c++17 -Isrc /tmp/sz.cpp -o /tmp/sz || exit 1
RECSZ=$(/tmp/sz)
echo "sizeof(Record) = $RECSZ 字节"

rm -f /tmp/prec.bin /tmp/prec.log
./serial-data-gateway --simulate --listen 9200 --file /tmp/prec.bin --fsync 200 >/tmp/prec.log 2>&1 &
PID=$!
sleep 3
kill -TERM "$PID"
wait "$PID" 2>/dev/null

echo "--- 进程日志 ---"
grep -E "解析|分发|落盘" /tmp/prec.log

REPORTED=$(grep -oP '落盘 \K[0-9]+' /tmp/prec.log | tail -1)
DISPATCHED=$(grep -oP '分发 \K[0-9]+' /tmp/prec.log | tail -1)
PARSED=$(grep -oP '解析 \K[0-9]+' /tmp/prec.log | tail -1)
BYTES=$(stat -c%s /tmp/prec.bin)
BYREC=$((BYTES / RECSZ))

echo "--- 对账 ---"
echo "source 解析帧数 : $PARSED"
echo "dispatch 分发数 : $DISPATCHED"
echo "storage 落盘数  : $REPORTED"
echo "文件字节 / 记录 : $BYTES / $RECSZ = $BYREC"

FAIL=0
[ "$PARSED" = "$DISPATCHED" ] && echo "  [PASS] 解析 == 分发（无丢帧）" || { echo "  [FAIL] 解析($PARSED) != 分发($DISPATCHED)"; FAIL=1; }
[ "$DISPATCHED" = "$REPORTED" ] && echo "  [PASS] 分发 == 落盘（退出前 flush 生效）" || { echo "  [FAIL] 分发($DISPATCHED) != 落盘($REPORTED)"; FAIL=1; }
[ "$REPORTED" = "$BYREC" ] && echo "  [PASS] 落盘数 == 文件记录数（无半条写入）" || { echo "  [FAIL] 落盘($REPORTED) != 文件记录($BYREC)"; FAIL=1; }

rm -f /tmp/prec.bin /tmp/prec.log /tmp/sz /tmp/sz.cpp
echo
[ "$FAIL" -eq 0 ] && echo "一致性校验通过 ✅" || echo "存在不一致 ❌"
exit $FAIL
