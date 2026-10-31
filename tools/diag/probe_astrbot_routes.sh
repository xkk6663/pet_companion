#!/bin/bash
# 探测 AstrBot v4.29 dashboard 的 API 密钥管理路由与 /api/v1/chat 入口
set -x
docker exec astrbot sh -lc 'grep -rn "api_keys" /AstrBot/astrbot/core/dashboard --include="*.py" 2>/dev/null | grep -iE "router|include|prefix|post|get|delete" | head -25'
echo "---chat route---"
docker exec astrbot sh -lc 'grep -rn "api/v1/chat\|/chat" /AstrBot/astrbot/core/plugins/handler /AstrBot/astrbot/core/dashboard --include="*.py" 2>/dev/null | grep -iE "route|@|path" | head -10'
echo "---api key files---"
docker exec astrbot sh -lc 'find /AstrBot -name "*.py" | xargs grep -ln "api_key" 2>/dev/null | grep -i dashboard | head -10'
