#!/usr/bin/env bash
cd "$(dirname "$0")/.."
FILES="src/gateway.h src/frame.cpp src/source.cpp src/storage.cpp src/tcp_server.cpp src/main.cpp"
TOTAL=0
EFF=0
echo "文件                      总行  有效代码"
for f in $FILES; do
  t=$(wc -l < "$f")
  e=$(grep -vE '^[[:space:]]*$' "$f" | grep -vE '^[[:space:]]*//' | grep -vE '^[[:space:]]*/\*' | grep -vE '^[[:space:]]*\*' | wc -l)
  printf '%-24s %5s %8s\n' "$f" "$t" "$e"
  TOTAL=$((TOTAL + t))
  EFF=$((EFF + e))
done
echo "-----------------------------------------"
printf '%-24s %5s %8s\n' "合计" "$TOTAL" "$EFF"
