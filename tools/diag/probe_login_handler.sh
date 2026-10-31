#!/bin/bash
echo "=====auth.py login handler====="
docker exec astrbot sh -lc 'sed -n "370,470p" /AstrBot/astrbot/dashboard/api/auth.py'
