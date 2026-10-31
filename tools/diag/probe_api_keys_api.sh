#!/bin/bash
docker exec astrbot sh -lc 'cat /AstrBot/astrbot/dashboard/api/api_keys.py 2>/dev/null | head -120'
echo "=====AUTH====="
docker exec astrbot sh -lc 'grep -nE "@router|def |class |payload|username|password" /AstrBot/astrbot/dashboard/api/auth.py | head -40'
