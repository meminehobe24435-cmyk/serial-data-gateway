#!/usr/bin/env bash
# 冒烟测试：不依赖任何硬件，启动模拟源 -> 用 nc 查询 -> 校验 -> 优雅退出
# 用法: bash tests/smoke_test.sh
set -u

BIN=./serial-data-gateway
PORT=9100
LOG=$(mktemp)
FAIL=0

pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1"; FAIL=1; }

echo "== 1. 构建 =="
make -s || { echo "构建失败"; exit 1; }

echo "== 2. 启动（模拟源，端口 $PORT）=="
$BIN --simulate --listen "$PORT" --file /tmp/gw_test.bin --fsync 200 >"$LOG" 2>&1 &
PID=$!
sleep 1.5

if kill -0 "$PID" 2>/dev/null; then pass "进程存活"; else fail "进程已退出"; cat "$LOG"; exit 1; fi

echo "== 3. STATS 命令 =="
OUT=$(printf 'STATS\n' | timeout 3 nc 127.0.0.1 "$PORT")
echo "  <- $OUT"
echo "$OUT" | grep -q "source=simulated" && pass "source=simulated" || fail "source 字段不对"
echo "$OUT" | grep -q "records=" && pass "records 字段存在" || fail "缺 records 字段"

echo "== 4. LAST 命令 =="
OUT=$(printf 'LAST 3\n' | timeout 3 nc 127.0.0.1 "$PORT")
echo "$OUT" | head -3 | sed 's/^/  <- /'
echo "$OUT" | grep -q "count=3" && pass "返回 3 条" || fail "返回条数不对"

echo "== 5. 未知命令容错 =="
OUT=$(printf 'FOO\n' | timeout 3 nc 127.0.0.1 "$PORT")
echo "$OUT" | grep -q "ERR unknown command" && pass "未知命令被拒绝" || fail "未知命令未处理"

echo "== 6. 优雅退出（SIGTERM）=="
kill -TERM "$PID"
for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$PID" 2>/dev/null; then fail "未在 6 秒内退出"; kill -9 "$PID"; else pass "已优雅退出"; fi

echo "== 7. 退出后落盘完整性 =="
grep -q "落盘" "$LOG" && pass "退出时打印了落盘条数" || fail "未见落盘汇总"
if [ -s /tmp/gw_test.bin ]; then
  SIZE=$(stat -c%s /tmp/gw_test.bin)
  REC=$((SIZE / 88))   # sizeof(Record) = 88（8+8+1+1+64，按 8 字节对齐补到 88）
  echo "  <- 文件 $SIZE 字节，$REC 条记录"
  [ "$REC" -gt 0 ] && pass "记录文件非空" || fail "记录文件为空"
  [ $((REC * 88)) -eq "$SIZE" ] && pass "长度是 88 的整数倍（无半条写入）" || fail "存在半条记录"
else
  fail "记录文件不存在"
fi

echo
[ "$FAIL" -eq 0 ] && echo "全部通过 ✅" || echo "存在失败项 ❌"
rm -f /tmp/gw_test.bin "$LOG"
exit $FAIL
