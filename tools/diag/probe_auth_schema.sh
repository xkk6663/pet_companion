#!/bin/bash
echo "=====LOGIN ROUTE====="
docker exec astrbot sh -lc 'grep -nE "@router\.(post|get).*(login|token)|prefix=" /AstrBot/astrbot/dashboard/api/auth.py | head -20'
echo "=====ApiKeyCreateRequest====="
docker exec astrbot sh -lc 'grep -n -A 25 "class ApiKeyCreateRequest" /AstrBot/astrbot/dashboard/schemas.py | head -40'
echo "=====open_api chat====="
docker exec astrbot sh -lc 'grep -nE "@router|prefix=|/chat|class .*Chat" /AstrBot/astrbot/dashboard/api/open_api.py | head -25'
