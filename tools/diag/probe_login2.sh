#!/bin/bash
echo "=====router defs====="
docker exec astrbot sh -lc 'sed -n "25,45p" /AstrBot/astrbot/dashboard/api/auth.py'
echo "=====_login====="
docker exec astrbot sh -lc 'grep -n -A 30 "async def _login" /AstrBot/astrbot/dashboard/api/auth.py | head -45'
echo "=====login service====="
docker exec astrbot sh -lc 'grep -n -B2 -A 25 "def login\|async def login" /AstrBot/astrbot/dashboard/services/auth_service.py 2>/dev/null | head -60'
echo "=====probe endpoints====="
for EP in /api/auth/auth/login /api/auth/login; do
  echo "--- $EP ---"
  curl -s -X POST "http://127.0.0.1:6185$EP" -H "Content-Type: application/json" -d '{"username":"astrbot","password":"J1w1wNv9ymmXppKmIWsvbTho","trust_device_flag":false}' | head -c 200
  echo ""
done
echo "=====setup-status====="
curl -s "http://127.0.0.1:6185/api/auth/setup-status" | head -c 200
