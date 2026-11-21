#!/bin/bash
# S9 桌宠网关部署脚本（在服务器上以 root 执行）
# 用法：把 ws_gateway.py / config.json 放到 /tmp/gw/ 后运行本脚本
set -e

GW_DIR=/opt/pet_gateway
SRC_DIR=/tmp/gw

echo "== 1. 建目录 =="
mkdir -p "$GW_DIR/static"

echo "== 2. 拷贝文件 =="
cp -f "$SRC_DIR/ws_gateway.py" "$GW_DIR/ws_gateway.py"
# ★ config.json 里是**真实的 AstrBot key**，仓库里的模板是打码的。
#   已存在就绝不覆盖（否则一次部署就把线上 key 冲成 "abk_xxx"）。
if [ -f "$GW_DIR/config.json" ]; then
  echo "   config.json 已存在，保留线上版本（模板仅在首次部署时使用）"
else
  cp -f "$SRC_DIR/config.json" "$GW_DIR/config.json"
  chmod 600 "$GW_DIR/config.json"
fi

echo "== 3. venv + 依赖 =="
if [ ! -d "$GW_DIR/.venv" ]; then
  python3 -m venv "$GW_DIR/.venv"
fi
"$GW_DIR/.venv/bin/pip" install -q --upgrade pip
"$GW_DIR/.venv/bin/pip" install -q fastapi "uvicorn[standard]" aiohttp python-multipart websockets

echo "== 4. 语法检查 =="
"$GW_DIR/.venv/bin/python" -m py_compile "$GW_DIR/ws_gateway.py"

echo "== 5. systemd 服务 =="
cp -f "$SRC_DIR/pet-gateway.service" /etc/systemd/system/pet-gateway.service
systemctl daemon-reload
systemctl enable pet-gateway 2>/dev/null || true
systemctl restart pet-gateway
sleep 3

echo "== 5b. 只热更代码（不动 config）=="
echo "   scp ws_gateway.py root@<host>:/opt/pet_gateway/ws_gateway.py.new"
echo "   ssh root@<host> 'cd /opt/pet_gateway && mv ws_gateway.py.new ws_gateway.py && systemctl restart pet-gateway'"

echo "== 6. 状态 =="
systemctl --no-pager status pet-gateway | head -8
echo "-- /health --"
curl -s http://127.0.0.1:8081/health
echo ""
echo "部署完成：日志 tail -f $GW_DIR/gateway.log"
