#!/bin/bash
# S10 P5 部署：夜间记忆抽取（在服务器上以 root 执行）
# 用法：把 nightly.py / query.py / 两个 unit 放到 /tmp/mem/ 后运行本脚本
set -e

MEM_DIR=/opt/pet_memory
SRC_DIR=/tmp/mem
PY=/opt/pet_gateway/.venv/bin/python      # 复用网关 venv（nightly 要 import ws_gateway）

echo "== 1. 目录 =="
mkdir -p "$MEM_DIR/bin" "$MEM_DIR/raw" "$MEM_DIR/audio" "$MEM_DIR/derived" "$MEM_DIR/nightly"

echo "== 2. 拷贝脚本 =="
cp -f "$SRC_DIR/nightly.py" "$MEM_DIR/bin/nightly.py"
cp -f "$SRC_DIR/query.py" "$MEM_DIR/bin/query.py"

echo "== 3. 依赖检查（网关 venv 必须已有 fastapi/aiohttp）=="
"$PY" -c "import fastapi, aiohttp" || { echo "网关 venv 缺依赖，先跑 deploy_gateway.sh"; exit 1; }

echo "== 4. 语法检查 =="
"$PY" -m py_compile "$MEM_DIR/bin/nightly.py" "$MEM_DIR/bin/query.py"

echo "== 5. systemd timer =="
cp -f "$SRC_DIR/pet-memory-nightly.service" /etc/systemd/system/pet-memory-nightly.service
cp -f "$SRC_DIR/pet-memory-nightly.timer" /etc/systemd/system/pet-memory-nightly.timer
systemctl daemon-reload
systemctl enable --now pet-memory-nightly.timer

echo "== 6. 手动跑一次昨天（验收）=="
/bin/sh -c "$PY $MEM_DIR/bin/nightly.py --date \$(date -d yesterday +%Y%m%d)" || true

echo "== 7. 状态 =="
systemctl --no-pager list-timers pet-memory-nightly.timer
echo "部署完成：日报在 $MEM_DIR/nightly/，日志 tail -f $MEM_DIR/nightly/cron.log"
