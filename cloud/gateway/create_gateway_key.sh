#!/bin/bash
# 创建 S9 桌宠网关专用 AstrBot API 密钥（chat 权限，可随时吊销）
set -e
BASE="http://127.0.0.1:6185"
USERNAME="astrbot"
PASSWORD="J1w1wNv9ymmXppKmIWsvbTho"
COOKIE_JAR=/tmp/astrbot_cookies.txt
rm -f "$COOKIE_JAR"

echo "== login =="
LOGIN_RESP=$(curl -s -c "$COOKIE_JAR" -X POST "$BASE/api/auth/auth/login" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\",\"trust_device_flag\":true}")
echo "$LOGIN_RESP" | head -c 300
echo ""

echo "== create key =="
KEY_RESP=$(curl -s -b "$COOKIE_JAR" -X POST "$BASE/api/api-keys" \
  -H "Content-Type: application/json" \
  -d '{"name":"s9-pet-gateway","scopes":["chat"],"expires_in_days":3650}')
echo "$KEY_RESP"
